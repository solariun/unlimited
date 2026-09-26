#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"
#include "unlimited/packet.hpp"
#include "unlimited/protocol.hpp"

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Prints bit-exact protocol examples as Markdown, every value computed by the library (make docs writes them to
// docs/protocol_examples.md): the presets, the mode header of each, the spec 1.5 mapping vector and a worked
// transmission of "CQ DE PY2" as the Encoder sends it.
namespace {

using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint8_t;
using unlimited::DecoderConfig;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSegment;
using unlimited::EncoderStatus;
using unlimited::GridSide;
using unlimited::HeaderFields;
using unlimited::Preset;
using unlimited::Profile;
using unlimited::SlotKind;
using unlimited::Spacing;

const uint32_t k_rate_hz = unlimited::k_decoder_rate_hz;
const double k_us_per_s = 1e6;
const double k_us_per_ms = 1e3;
const double k_ms_per_s = 1e3;
const double k_dense_spacing = 1.0;  // c = 1: dense spacing 1 / T
const size_t k_format_buffer = 1024;
const size_t k_packet_buffer = 256;
const int k_exit_failure = 1;

const char* const k_example_text = "CQ DE PY2";

// Spec 1.5 bit-exact vector: k = 5, N = 8, bytes 48 69 21 00 FF.
const uint8_t k_mapping_bytes[] = {0x48, 0x69, 0x21, 0x00, 0xFF};
const uint8_t k_mapping_bits_per_peak = 5;
const uint8_t k_mapping_data_slots = 8;

// Header word fields (spec 2.2): a = bits 0..2, b = bits 3..5, spacing bit 6, N code bits 7..8.
const unsigned k_field_bits = 3;
const unsigned k_field_mask = (1u << k_field_bits) - 1u;
const unsigned k_spacing_shift = 2 * k_field_bits;
const unsigned k_n_code_shift = k_spacing_shift + 1;
const unsigned k_n_code_mask = 3;

struct PresetName {
    Preset preset;
    const char* name;
};

const PresetName k_presets[] = {{Preset::fm_fast, "fm_fast"}, {Preset::fm, "fm"},
                                {Preset::hf_fast, "hf_fast"}, {Preset::hf, "hf"},
                                {Preset::hf_robust, "hf_robust"}, {Preset::hf_weak, "hf_weak"}};

struct ProfileName {
    Profile profile;
    const char* name;
};

const ProfileName k_profiles[] = {{Profile::ssb, "ssb"}, {Profile::am, "am"}, {Profile::fm, "fm"}};

// Spec 2.2 test vectors beyond the presets: N codes 1 and 2, and dense spacing.
struct ExtraMode {
    uint32_t slot_ms;
    uint8_t bits_per_peak;
    uint8_t data_slots;
    Spacing spacing;
};

const ExtraMode k_extra_modes[] = {{32, 5, 16, Spacing::standard},
                                   {16, 4, 32, Spacing::standard},
                                   {128, 8, 8, Spacing::dense}};

std::string format(const char* pattern, ...) __attribute__((format(printf, 1, 2)));

std::string format(const char* pattern, ...) {
    char buffer[k_format_buffer];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(buffer, sizeof(buffer), pattern, args);
    va_end(args);
    return buffer;
}

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "doc_examples: %s\n", message.c_str());
    std::exit(k_exit_failure);
}

const char* spacing_name(Spacing spacing) {
    return spacing == Spacing::dense ? "dense 1/T" : "standard 8/(7T)";
}

const char* side_name(GridSide side) {
    return side == GridSide::above ? "above" : "below";
}

double slot_ms_of(const EncoderConfig& config) {
    return config.slot_us / k_us_per_ms;
}

// Distance of grid tone n from f_ref (spec 1.3): (G + n c) / T.
double tone_offset_hz(unsigned tone, Spacing spacing, uint32_t slot_us) {
    const double standard = static_cast<double>(unlimited::k_standard_spacing_num) / unlimited::k_standard_spacing_den;
    const double c = spacing == Spacing::dense ? k_dense_spacing : standard;
    return (unlimited::k_grid_guard + tone * c) * k_us_per_s / slot_us;
}

// Audio frequency of data tone n (or header tone n: always the standard grid).
double tone_hz(const EncoderConfig& config, unsigned tone, bool header) {
    const double side = config.side == GridSide::above ? 1.0 : -1.0;
    const Spacing spacing = header ? Spacing::standard : config.spacing;
    return config.tone_hz + side * tone_offset_hz(tone, spacing, config.slot_us);
}

unsigned tone_count(const EncoderConfig& config) {
    return 1u << config.bits_per_peak;
}

// Lowest and highest data or header tone, f_ref included.
void band_hz(const EncoderConfig& config, double& low, double& high) {
    const double data = tone_offset_hz(tone_count(config) - 1u, config.spacing, config.slot_us);
    const double header = tone_offset_hz(unlimited::k_header_slots - 1u, Spacing::standard, config.slot_us);
    const double span = data > header ? data : header;
    low = config.side == GridSide::below ? config.tone_hz - span : config.tone_hz;
    high = config.side == GridSide::below ? config.tone_hz : config.tone_hz + span;
}

double raw_bit_rate(const EncoderConfig& config) {
    return config.bits_per_peak * k_ms_per_s / slot_ms_of(config);
}

double net_bit_rate(const EncoderConfig& config) {
    return config.data_slots * raw_bit_rate(config) / (config.data_slots + 1u);
}

std::string binary(unsigned value, unsigned bits) {
    std::string text;
    for (unsigned b = bits; b-- > 0;) text += ((value >> b) & 1u) != 0 ? '1' : '0';
    return text;
}

std::string hex_bytes(const std::vector<uint8_t>& bytes, size_t first, size_t count) {
    std::string text;
    for (size_t i = first; i < first + count; ++i) text += format("%s%02X", i == first ? "" : " ", bytes[i]);
    return text;
}

// The bit string of `count` bytes (MSB first) cut into k-bit groups; the last group is padded with 0.
std::string grouped_bits(const std::vector<uint8_t>& bytes, size_t first, size_t count, unsigned k) {
    std::string bits;
    for (size_t i = first; i < first + count; ++i) bits += binary(bytes[i], unlimited::k_bits_per_byte);
    while (bits.size() % k != 0) bits += '0';
    std::string text;
    for (size_t i = 0; i < bits.size(); i += k) text += (i == 0 ? "" : " ") + bits.substr(i, k);
    return text;
}

std::string printable(const std::vector<uint8_t>& bytes, size_t first, size_t count) {
    const uint8_t k_first_printable = 0x20;
    const uint8_t k_last_printable = 0x7E;
    std::string text;
    for (size_t i = first; i < first + count; ++i) {
        const uint8_t c = bytes[i];
        text += c >= k_first_printable && c <= k_last_printable ? static_cast<char>(c) : '.';
    }
    return text;
}

// ---------------------------------------------------------------------------
// The transmission as the Encoder sends it: one record per slot, read from status() before its first sample.
// ---------------------------------------------------------------------------

struct SlotRecord {
    EncoderStatus status;
    uint32_t first_sample;
    uint32_t samples;
};

std::vector<SlotRecord> render_slots(const EncoderConfig& config, const std::vector<uint8_t>& data, uint32_t& total) {
    Encoder encoder(config);
    size_t written = encoder.write(data.data(), data.size());
    std::vector<SlotRecord> slots;
    if (!encoder.start()) fail("the encoder did not start");
    total = 0;
    while (encoder.busy()) {
        if (written < data.size()) written += encoder.write(&data[written], data.size() - written);
        const EncoderStatus status = encoder.status();
        if (slots.empty() || status.slot_index != slots.back().status.slot_index ||
            status.segment != slots.back().status.segment) {
            const SlotRecord record = {status, total, 0};
            slots.push_back(record);
        }
        encoder.next_sample();
        ++slots.back().samples;
        ++total;
    }
    if (written != data.size()) fail("the encoder did not take every byte");
    return slots;
}

const char* segment_name(EncoderSegment segment) {
    switch (segment) {
        case EncoderSegment::idle:
            return "idle";
        case EncoderSegment::lead_in:
            return "lead-in";
        case EncoderSegment::tune:
            return "tune";
        case EncoderSegment::sync:
            return "sync";
        case EncoderSegment::header:
            return "header";
        case EncoderSegment::frame:
            return "frame";
        case EncoderSegment::eot:
            return "EOT";
        case EncoderSegment::tail:
            return "tail";
    }
    return "?";
}

double samples_ms(uint32_t samples) {
    return samples * k_ms_per_s / k_rate_hz;
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

void print_presets() {
    std::printf("## 1. Presets\n\n");
    std::printf("`EncoderConfig::from_preset(preset, %u)`. Raw = k / T; net = N k / ((N + 1) T) (spec 1.4). The band "
                "runs from the farthest data or header tone to f_ref (spec 1.3).\n\n",
                static_cast<unsigned>(k_rate_hz));
    std::printf("| Preset | T | k | M | N | Spacing | Grid | f_ref | Band | Span | Bytes/frame | Raw bit/s | "
                "Net bit/s | Tune / sync / lead-in |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (size_t p = 0; p < sizeof(k_presets) / sizeof(k_presets[0]); ++p) {
        const EncoderConfig c = EncoderConfig::from_preset(k_presets[p].preset, k_rate_hz);
        if (!c.valid()) fail(format("preset %s is invalid", k_presets[p].name));
        double low = 0.0;
        double high = 0.0;
        band_hz(c, low, high);
        std::printf("| `%s` | %.0f ms | %u | %u | %u | %s | %s | %u Hz | %.1f–%.1f Hz | %.1f Hz | %u | %.2f | %.1f | "
                    "%u ms / %u / %u ms |\n",
                    k_presets[p].name, slot_ms_of(c), static_cast<unsigned>(c.bits_per_peak), tone_count(c),
                    static_cast<unsigned>(c.data_slots), spacing_name(c.spacing), side_name(c.side),
                    static_cast<unsigned>(c.tone_hz), low, high, high - low, static_cast<unsigned>(c.frame_bytes()),
                    raw_bit_rate(c), net_bit_rate(c), static_cast<unsigned>(c.tune_ms),
                    static_cast<unsigned>(c.sync_markers), static_cast<unsigned>(c.lead_in_ms));
    }
    std::printf("\nThe `fm` and `fm_fast` presets are FM-only (spec G1): f_ref 2650 Hz sits on the upper edge of an "
                "SSB passband.\n\n");

    std::printf("### Decoder profiles\n\n");
    std::printf("`DecoderConfig::for_profile(profile)`: the receiver chooses only the T range it accepts; the mode "
                "comes from the header.\n\n");
    std::printf("| Profile | `min_slot_ms` (= block size, samples) | Accepted T | f_ref search | Impulse blanker |\n");
    std::printf("|---|---|---|---|---|\n");
    for (size_t p = 0; p < sizeof(k_profiles) / sizeof(k_profiles[0]); ++p) {
        const DecoderConfig d = DecoderConfig::for_profile(k_profiles[p].profile);
        std::printf("| `%s` | %u | %u–%u ms | %u–%u Hz | %s |\n", k_profiles[p].name,
                    static_cast<unsigned>(d.min_slot_ms), static_cast<unsigned>(d.min_slot_ms),
                    static_cast<unsigned>(d.max_slot_ms()), static_cast<unsigned>(d.min_tone_hz),
                    static_cast<unsigned>(d.max_tone_hz), d.impulse_blanker ? "on" : "off");
    }
    std::printf("\n");
}

void print_header_row(const std::string& name, const EncoderConfig& c) {
    const uint16_t word = unlimited::header_word(c.bits_per_peak, c.data_slots, c.slot_us, c.spacing);
    const HeaderFields fields = unlimited::header_fields(word);
    if (fields.bits_per_peak != c.bits_per_peak || fields.data_slots != c.data_slots || fields.spacing != c.spacing)
        fail("header_fields() does not invert header_word() for " + name);
    std::string tones;
    std::string freqs;
    for (uint8_t j = 0; j < unlimited::k_header_slots; ++j) {
        const uint8_t h = unlimited::header_symbol(word, j);
        tones += format("%s%u", j == 0 ? "" : " ", static_cast<unsigned>(h));
        freqs += format("%s%.1f", j == 0 ? "" : " ", tone_hz(c, h, true));
    }
    std::printf("| %s | T%.0f k%u N%u %s | `0x%03X` | %u | %u | %u | %u | %s | %s |\n", name.c_str(), slot_ms_of(c),
                static_cast<unsigned>(c.bits_per_peak), static_cast<unsigned>(c.data_slots),
                c.spacing == Spacing::dense ? "dense" : "std", static_cast<unsigned>(word), word & k_field_mask,
                (word >> k_field_bits) & k_field_mask, (word >> k_spacing_shift) & 1u,
                (word >> k_n_code_shift) & k_n_code_mask, tones.c_str(), freqs.c_str());
}

void print_headers() {
    std::printf("## 2. Mode header (spec 2.2)\n\n");
    std::printf("`header_word(k, N, T, spacing)` = a | b << 3 | spacing << 6 | N code << 7 with a = k − 1, "
                "b = T_ms mod 8, N code 0/1/2 = N 8/16/32. `header_symbol(word, j)` gives the tone h_j of header slot "
                "j (RS(8,3) + x³ coset over GF(8)); `header_fields(word)` inverts the word (checked for every row). "
                "Header tone h sits at f_ref ∓ (5 + 8h/7)/T in every mode (grid below: −).\n\n");
    std::printf("| Preset | Mode | Word | a | b | Spacing bit | N code | Tones h_0..h_7 | Tone frequencies (Hz) |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|\n");
    for (size_t p = 0; p < sizeof(k_presets) / sizeof(k_presets[0]); ++p) {
        print_header_row(format("`%s`", k_presets[p].name), EncoderConfig::from_preset(k_presets[p].preset, k_rate_hz));
    }
    std::printf("\nOther spec 2.2 vectors (f_ref centred on 1500 Hz, grid below):\n\n");
    std::printf("| Placement | Mode | Word | a | b | Spacing bit | N code | Tones h_0..h_7 | Tone frequencies (Hz) |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|\n");
    for (size_t m = 0; m < sizeof(k_extra_modes) / sizeof(k_extra_modes[0]); ++m) {
        EncoderConfig c = EncoderConfig::from_preset(Preset::hf, k_rate_hz);
        c.slot_us = static_cast<uint32_t>(k_extra_modes[m].slot_ms * k_us_per_ms);
        c.bits_per_peak = k_extra_modes[m].bits_per_peak;
        c.data_slots = k_extra_modes[m].data_slots;
        c.spacing = k_extra_modes[m].spacing;
        double low = 0.0;
        double high = 0.0;
        band_hz(c, low, high);
        // f_ref = ceil(1500 + W / 2), the placement of the HF presets (spec 1.3).
        c.tone_hz = static_cast<uint16_t>(std::ceil(unlimited::k_band_centre_hz + (high - low) / 2.0));
        print_header_row(format("f_ref %u Hz", static_cast<unsigned>(c.tone_hz)), c);
    }
    std::printf("\n");
}

void print_mapping() {
    const unsigned k = k_mapping_bits_per_peak;
    const unsigned tones = 1u << k;
    const unsigned r = unlimited::tone_rotation(k_mapping_bits_per_peak);
    const std::vector<uint8_t> bytes(k_mapping_bytes, k_mapping_bytes + sizeof(k_mapping_bytes));
    std::printf("## 3. Mapping vector (spec 1.5)\n\n");
    std::printf("k = %u (M = %u tones), N = %u, r = (M/8) | 1 = %u. Bytes `%s` → bit string (MSB first), cut into "
                "k-bit symbols:\n\n",
                k, tones, static_cast<unsigned>(k_mapping_data_slots), r,
                hex_bytes(bytes, 0, bytes.size()).c_str());
    std::printf("```\n%s\n```\n\n", grouped_bits(bytes, 0, bytes.size(), k).c_str());
    std::printf("n_i = (gray⁻¹(s_i) + (i − 1)·r) mod M = `peak_tone(s_i, i − 1, k)`; the receiver inverts it with "
                "`peak_symbol(n_i, i − 1, k)` = gray((n_i − (i − 1)·r) mod M). Frequencies for the hf preset "
                "(f_ref 2132 Hz, T 32 ms, grid below).\n\n");
    std::printf("| Slot i | Bits | s_i | gray⁻¹(s_i) | (i − 1)·r mod M | Tone n_i | peak_symbol(n_i) | Frequency (hf) |\n");
    std::printf("|---|---|---|---|---|---|---|---|\n");
    const EncoderConfig hf = EncoderConfig::from_preset(Preset::hf, k_rate_hz);
    std::string symbols;
    std::string tone_list;
    for (unsigned i = 0; i < k_mapping_data_slots; ++i) {
        const unsigned bit = i * k;
        unsigned symbol = 0;
        for (unsigned b = 0; b < k; ++b) {
            const unsigned position = bit + b;
            const unsigned value = (bytes[position / unlimited::k_bits_per_byte] >>
                                    (unlimited::k_bits_per_byte - 1u - position % unlimited::k_bits_per_byte)) & 1u;
            symbol = (symbol << 1) | value;
        }
        const unsigned inverse = unlimited::gray_decode(static_cast<uint8_t>(symbol));
        const unsigned rotation = (i * r) % tones;
        const unsigned tone = unlimited::peak_tone(static_cast<uint8_t>(symbol), static_cast<uint8_t>(i),
                                                   k_mapping_bits_per_peak);
        const unsigned back = unlimited::peak_symbol(static_cast<uint8_t>(tone), static_cast<uint8_t>(i),
                                                     k_mapping_bits_per_peak);
        if (tone != (inverse + rotation) % tones || back != symbol) fail("peak_tone/peak_symbol disagree with spec 1.5");
        symbols += format("%s%u", i == 0 ? "" : ", ", symbol);
        tone_list += format("%s%u", i == 0 ? "" : ", ", tone);
        std::printf("| %u | `%s` | %u | %u | %u | **%u** | %u | %.1f Hz |\n", i + 1, binary(symbol, k).c_str(), symbol,
                    inverse, rotation, tone, back, tone_hz(hf, tone, false));
    }
    std::printf("\nSymbols %s; **tones %s**.\n\n", symbols.c_str(), tone_list.c_str());
}

void print_slot_table(const EncoderConfig& c, const std::vector<SlotRecord>& slots) {
    std::printf("| Slots | Segment | Kind | Slot (header j, frame i) | Bits | Symbol | Tone | Frequency | Start |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|\n");
    const unsigned k = c.bits_per_peak;
    unsigned frame = 0;
    for (size_t s = 0; s < slots.size();) {
        const EncoderStatus& st = slots[s].status;
        // Tune, sync and EOT slots are alike: one row per run.
        size_t run = 1;
        if (st.segment == EncoderSegment::tune || st.segment == EncoderSegment::sync ||
            st.segment == EncoderSegment::eot || st.segment == EncoderSegment::lead_in) {
            while (s + run < slots.size() && slots[s + run].status.segment == st.segment) ++run;
        }
        const std::string index =
            run == 1 ? format("%u", static_cast<unsigned>(st.slot_index))
                     : format("%u–%u", static_cast<unsigned>(st.slot_index),
                              static_cast<unsigned>(slots[s + run - 1].status.slot_index));
        const std::string start = format("%.0f ms", samples_ms(slots[s].first_sample));
        switch (st.kind) {
            case SlotKind::tone:
                std::printf("| %s | tune | steady tone ×%u | | | | | %u Hz (f_ref) | %s |\n", index.c_str(),
                            static_cast<unsigned>(run), static_cast<unsigned>(c.tone_hz), start.c_str());
                break;
            case SlotKind::marker: {
                std::string what = format("marker ×%u", static_cast<unsigned>(run));
                if (st.segment == EncoderSegment::sync) what += ", the last one is the header START";
                if (st.segment == EncoderSegment::header) what = "header STOP = frame 0 START";
                if (st.segment == EncoderSegment::frame) {
                    const bool next = s + 1 < slots.size() && slots[s + 1].status.segment == EncoderSegment::frame;
                    what = format("frame %u STOP", frame);
                    ++frame;
                    if (next) what += format(" = frame %u START", frame);
                }
                const std::string segment = st.segment == EncoderSegment::frame ? format("frame %u", frame - 1u)
                                                                                : segment_name(st.segment);
                std::printf("| %s | %s | %s | | | | | %u Hz (f_ref), phase flip | %s |\n", index.c_str(),
                            segment.c_str(), what.c_str(), static_cast<unsigned>(c.tone_hz), start.c_str());
                break;
            }
            case SlotKind::peak: {
                const bool header = st.segment == EncoderSegment::header;
                const std::string position = header ? format("j = %u", static_cast<unsigned>(st.slot))
                                                    : format("i = %u", st.slot + 1u);
                const std::string bits = header ? "" : "`" + binary(st.symbol, k) + "`";
                std::printf("| %s | %s | peak | %s | %s | %u | %u | %.1f Hz | %s |\n", index.c_str(),
                            header ? "header" : format("frame %u", frame).c_str(), position.c_str(), bits.c_str(),
                            static_cast<unsigned>(st.symbol), static_cast<unsigned>(st.tone),
                            tone_hz(c, st.tone, header), start.c_str());
                if (!header && unlimited::peak_tone(st.symbol, st.slot, c.bits_per_peak) != st.tone)
                    fail("the encoder's tone differs from peak_tone()");
                break;
            }
            case SlotKind::silent:
                std::printf("| %s | %s | silence | | | | | | %s |\n", index.c_str(), segment_name(st.segment),
                            start.c_str());
                break;
        }
        s += run;
    }
    std::printf("\nHeader peaks carry the 3-bit tones h_j of section 2 (standard grid); frame peaks carry k = %u bits "
                "each. Frequencies are the exact grid f_ref − (5 + n·8/7)/T; the encoder's NCO is within 1.4e-3 Hz of "
                "it (spec 1.7).\n\n",
                k);
}

void print_frames(const EncoderConfig& c, const std::vector<uint8_t>& data) {
    const size_t bytes = c.frame_bytes();
    std::printf("| Frame | Bytes | Text | Peaks | Bit string in k = %u groups (last group padded with 0) |\n",
                static_cast<unsigned>(c.bits_per_peak));
    std::printf("|---|---|---|---|---|\n");
    for (size_t first = 0, f = 0; first < data.size(); first += bytes, ++f) {
        const size_t count = data.size() - first < bytes ? data.size() - first : bytes;
        const unsigned peaks =
            static_cast<unsigned>((count * unlimited::k_bits_per_byte + c.bits_per_peak - 1) / c.bits_per_peak);
        std::printf("| %u%s | `%s` | `%s` | %u | `%s` |\n", static_cast<unsigned>(f),
                    count < bytes ? " (short final)" : "", hex_bytes(data, first, count).c_str(),
                    printable(data, first, count).c_str(), peaks,
                    grouped_bits(data, first, count, c.bits_per_peak).c_str());
    }
    std::printf("\n");
}

void print_duration(const EncoderConfig& c, const std::vector<uint8_t>& data, const std::vector<SlotRecord>& slots,
                    uint32_t rendered) {
    const Encoder encoder(c);
    const uint32_t expected = encoder.duration_samples(data.size());
    if (rendered + 1 < expected || rendered > expected + 1)
        fail(format("duration_samples() %u against %u rendered", expected, rendered));
    std::printf("`Encoder::duration_samples(%u)` = **%u samples = %.3f s** at %u Hz (rendered: %u samples).\n\n",
                static_cast<unsigned>(data.size()), expected, expected / static_cast<double>(k_rate_hz),
                static_cast<unsigned>(k_rate_hz), rendered);
    std::printf("| Segment | Slots | Samples | Duration |\n");
    std::printf("|---|---|---|---|\n");
    const EncoderSegment order[] = {EncoderSegment::lead_in, EncoderSegment::tune,  EncoderSegment::sync,
                                    EncoderSegment::header,  EncoderSegment::frame, EncoderSegment::eot,
                                    EncoderSegment::tail};
    for (size_t o = 0; o < sizeof(order) / sizeof(order[0]); ++o) {
        unsigned count = 0;
        uint32_t samples = 0;
        for (size_t s = 0; s < slots.size(); ++s) {
            if (slots[s].status.segment != order[o]) continue;
            ++count;
            samples += slots[s].samples;
        }
        const std::string slot_text = order[o] == EncoderSegment::tail ? "–" : format("%u", count);
        std::printf("| %s | %s | %u | %.0f ms |\n", segment_name(order[o]), slot_text.c_str(),
                    static_cast<unsigned>(samples), samples_ms(samples));
    }
    const unsigned peaks = static_cast<unsigned>((data.size() * unlimited::k_bits_per_byte + c.bits_per_peak - 1) /
                                                 c.bits_per_peak);
    const unsigned frames = (peaks + c.data_slots - 1u) / c.data_slots;
    unsigned tune = 0;
    for (size_t s = 0; s < slots.size(); ++s) tune += slots[s].status.segment == EncoderSegment::tune ? 1u : 0u;
    std::printf("\nSpec 2.1: lead + (N_tune + N_sync + 9 + D + ⌈D/N⌉ + 2)·T + tail with D = ⌈8·%u/%u⌉ = %u peaks "
                "in %u frame%s: %u ms + (%u + %u + 9 + %u + %u + 2) × %.0f ms + %u ms = %.0f ms.\n\n",
                static_cast<unsigned>(data.size()), static_cast<unsigned>(c.bits_per_peak), peaks, frames,
                frames == 1 ? "" : "s", static_cast<unsigned>(c.lead_in_ms), tune,
                static_cast<unsigned>(c.sync_markers), peaks, frames, slot_ms_of(c), static_cast<unsigned>(c.tail_ms),
                c.lead_in_ms + (tune + c.sync_markers + unlimited::k_header_slots + 1 + peaks + frames +
                                unlimited::k_eot_markers) * slot_ms_of(c) + c.tail_ms);
}

void print_worked_example() {
    const EncoderConfig c = EncoderConfig::from_preset(Preset::hf, k_rate_hz);
    const std::string text = k_example_text;
    const std::vector<uint8_t> payload(text.begin(), text.end());
    std::vector<uint8_t> packet(k_packet_buffer);
    const size_t size =
        unlimited::packet_build(payload.data(), static_cast<uint16_t>(payload.size()), packet.data(), packet.size());
    if (size == 0) fail("packet_build() failed");
    packet.resize(size);
    const size_t crc_at = unlimited::k_packet_header + payload.size();

    std::printf("## 4. Worked example: \"%s\" with the hf preset and packet framing\n\n", k_example_text);
    std::printf("### 4.1 Packet (`packet_build`, spec 2.6)\n\n");
    std::printf("| Field | Bytes |\n|---|---|\n");
    std::printf("| sync | `%s` |\n", hex_bytes(packet, 0, 2).c_str());
    std::printf("| LEN (16-bit, big-endian) | `%s` = %u |\n", hex_bytes(packet, 2, 2).c_str(),
                static_cast<unsigned>(payload.size()));
    std::printf("| payload \"%s\" | `%s` |\n", k_example_text,
                hex_bytes(packet, unlimited::k_packet_header, payload.size()).c_str());
    std::printf("| CRC-16/CCITT-FALSE over LEN + payload | `%s` |\n", hex_bytes(packet, crc_at, 2).c_str());
    std::printf("\nPacket: %u bytes `%s`.\n\n", static_cast<unsigned>(size), hex_bytes(packet, 0, size).c_str());

    std::printf("### 4.2 Frames\n\n");
    std::printf("hf: T %.0f ms, k %u, N %u → B = N·k/8 = %u bytes per frame. %u bytes = %u full frame%s%s.\n\n",
                slot_ms_of(c), static_cast<unsigned>(c.bits_per_peak), static_cast<unsigned>(c.data_slots),
                static_cast<unsigned>(c.frame_bytes()), static_cast<unsigned>(size),
                static_cast<unsigned>(size / c.frame_bytes()), size / c.frame_bytes() == 1 ? "" : "s",
                size % c.frame_bytes() == 0 ? ", no short final frame"
                                            : format(" + a short final frame of %u bytes",
                                                     static_cast<unsigned>(size % c.frame_bytes())).c_str());
    print_frames(c, packet);

    uint32_t rendered = 0;
    const std::vector<SlotRecord> slots = render_slots(c, packet, rendered);
    std::printf("### 4.3 Slots, as the Encoder sends them\n\n");
    std::printf("Read from `Encoder::status()` at the first sample of each slot (f_ref %u Hz, grid below).\n\n",
                static_cast<unsigned>(c.tone_hz));
    print_slot_table(c, slots);

    std::printf("### 4.4 Duration\n\n");
    print_duration(c, packet, slots, rendered);

    std::printf("## 5. The same text without packet framing (the transmission drawn in `docs/images`)\n\n");
    std::printf("The figures send the 9 text bytes directly: frame 0 carries 5 bytes, a short final frame the other 4 "
                "(⌈32/5⌉ = 7 peaks, the last 3 bits padded with 0), then STOP and the 2 EOT markers (spec 2.4).\n\n");
    print_frames(c, payload);
    uint32_t text_rendered = 0;
    const std::vector<SlotRecord> text_slots = render_slots(c, payload, text_rendered);
    print_slot_table(c, text_slots);
    print_duration(c, payload, text_slots, text_rendered);
}

}  // namespace

int main() {
    std::printf("<!-- Generated by `make docs` (tools/doc_examples.cpp). Do not edit: every value is computed by the "
                "library. -->\n\n");
    std::printf("# Unlimited v0.2 — protocol examples\n\n");
    std::printf("Generated by `make docs` from the library code (`tools/doc_examples.cpp`); do not edit by hand. "
                "Every number below is computed by the library functions named with it; sections refer to `spec.md`."
                "\n\n");
    print_presets();
    print_headers();
    print_mapping();
    print_worked_example();
    return 0;
}
