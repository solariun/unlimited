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

// Prints docs/protocol_examples.md (make docs) as Markdown: presets, profiles, "Hi" slot by slot for N = 8, 4 and 3,
// a packet, durations and the bandwidth tables. Every value is computed by the library; every example spec.md also
// gives is checked against it, and a disagreement stops make docs (exit 1, the reason on stderr).
namespace {

using std::int16_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using unlimited::Band;
using unlimited::DecoderConfig;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSegment;
using unlimited::EncoderStatus;
using unlimited::Passband;
using unlimited::PassbandFit;
using unlimited::Preset;
using unlimited::Profile;
using unlimited::SlotKind;

const uint32_t k_rate_hz = unlimited::k_decoder_rate_hz;
const double k_us_per_ms = 1e3;
const double k_ms_per_s = 1e3;
const double k_us_per_s = 1e6;
const double k_two_pi = 6.28318530717958647692;
const double k_phase_turn = 4294967296.0;  // 2^32: one turn of the encoder's NCO
const unsigned k_phase_bits = 32;
const size_t k_format_buffer = 2048;
const size_t k_packet_buffer = 64;
const int k_exit_failure = 1;
const unsigned k_byte_bits = unlimited::k_bits_per_byte;
const unsigned k_msb = k_byte_bits - 1;
const uint8_t k_first_printable = 0x20;
const uint8_t k_last_printable = 0x7E;

const char* const k_hi_text = "Hi";
const char* const k_packet_text = "CQ DE PY2";
const char* const k_crc_check_text = "123456789";
const char* const k_minus = "\xE2\x88\x92";  // U+2212, typographic minus
const char* const k_no_carrier = "\xE2\x80\x93";  // U+2013

// ---------------------------------------------------------------------------
// The examples of spec.md the library must reproduce
// ---------------------------------------------------------------------------

// Spec 1.7, sender presets: T, N, net bit/s, passband, lead-in, tune slots, overhead, occupied band and the room
// below / above it in the preset's own passband, the receiver profiles that hear it.
struct PresetSpec {
    Preset preset;
    const char* name;
    uint32_t slot_us;
    uint8_t bits;
    const char* net_bit_s;
    uint16_t passband_low_hz;
    uint16_t passband_high_hz;
    uint16_t lead_in_ms;
    uint16_t tune_slots;
    uint16_t overhead_ms;
    uint16_t band_low_hz;
    uint16_t band_high_hz;
    int16_t room_down_hz;
    int16_t room_up_hz;
    const char* heard_by;
};

const PresetSpec k_spec_presets[] = {
    {Preset::hf_slow, "hf_slow", 32000, 8, "27.78", 300, 2700, 0, 8, 676, 1431, 1569, 1131, 1131, "ssb, am, fm"},
    {Preset::hf, "hf", 16000, 8, "55.56", 300, 2700, 0, 16, 516, 1362, 1638, 1062, 1062, "ssb, am, fm"},
    {Preset::hf_fast, "hf_fast", 8000, 8, "111.11", 300, 2700, 0, 32, 436, 1225, 1775, 925, 925, "ssb, am, fm"},
    {Preset::am, "am", 8000, 16, "117.65", 100, 3000, 0, 32, 436, 1225, 1775, 1125, 1200, "ssb, am, fm"},
    {Preset::fm, "fm", 4000, 16, "235.29", 300, 3000, 300, 63, 692, 950, 2050, 500, 950, "fm"}};

// Spec 1.7, receiver profiles: window, passband and tone search range.
struct ProfileSpec {
    Profile profile;
    const char* name;
    uint8_t min_slot_ms;
    uint16_t max_slot_ms;
    uint16_t passband_low_hz;
    uint16_t passband_high_hz;
    uint16_t search_low_hz;
    uint16_t search_high_hz;
    const char* radio;
};

const ProfileSpec k_spec_profiles[] = {{Profile::ssb, "ssb", 8, 64, 300, 2700, 335, 2665, "HF SSB, USB or LSB"},
                                       {Profile::am, "am", 8, 64, 100, 3000, 300, 2700, "AM receivers"},
                                       {Profile::fm, "fm", 4, 32, 300, 3000, 1000, 2700, "VHF/UHF NBFM"}};

// Spec 1.5, widths per slot length: occupied band (99 %), -26 dB and -40 dB widths.
struct WidthSpec {
    uint32_t slot_us;
    uint16_t occupied_hz;
    uint16_t width_26db_hz;
    uint16_t width_40db_hz;
};

const WidthSpec k_spec_widths[] = {{4000, 1100, 1750, 2475}, {5000, 880, 1400, 1980},  {8000, 550, 875, 1238},
                                   {12000, 368, 584, 825},   {16000, 276, 438, 619},   {20000, 220, 350, 495},
                                   {32000, 138, 219, 310},   {64000, 70, 110, 155},    {128000, 36, 55, 78}};

// Spec 1.5, typical receiver passbands (-6 dB points).
struct Filter {
    const char* name;
    uint16_t low_hz;
    uint16_t high_hz;
};

const Filter k_filters[] = {{"SSB 1.8 kHz", 300, 2100},
                            {"SSB 2.4 kHz", unlimited::k_ssb_passband_low_hz, unlimited::k_ssb_passband_high_hz},
                            {"SSB 2.7 kHz", 200, 2900},
                            {"SSB 3.0 kHz", 100, 3000},
                            {"AM", unlimited::k_am_passband_low_hz, unlimited::k_am_passband_high_hz},
                            {"NBFM", unlimited::k_fm_passband_low_hz, unlimited::k_fm_passband_high_hz}};
const size_t k_filter_count = sizeof(k_filters) / sizeof(k_filters[0]);
const size_t k_narrow_filter = 0;

// Spec 1.5, "Presets in typical filters" at 1500 Hz: the shift tolerance down / up in each filter above: the room
// the filter leaves, each side also ending where the pitch leaves the search of the receiver that hears it.
struct FitSpec {
    uint32_t slot_us;
    int16_t room[k_filter_count][2];
};

const FitSpec k_spec_fits[] = {
    {32000, {{1131, 531}, {1131, 1131}, {1200, 1200}, {1200, 1200}, {1200, 1200}, {1131, 1200}}},
    {16000, {{1062, 462}, {1062, 1062}, {1162, 1200}, {1200, 1200}, {1200, 1200}, {1062, 1200}}},
    {8000, {{925, 325}, {925, 925}, {1025, 1125}, {1125, 1200}, {1125, 1200}, {925, 1200}}},
    {4000, {{500, 50}, {500, 650}, {500, 850}, {500, 950}, {500, 950}, {500, 950}}}};

// Spec 1.5: the pitch at the centre of a 1.8 kHz filter.
const uint16_t k_centred_tone_hz = 1200;
struct CentredSpec {
    Preset preset;
    const char* name;
    int16_t room_down_hz;
    int16_t room_up_hz;
};
const CentredSpec k_spec_centred[] = {{Preset::hf, "hf", 762, 762}, {Preset::fm, "fm", 200, 350}};

// Spec 2.2, "Hi" packed with N = 8, 4 and 3: the packages and the data slots + STOPs.
const size_t k_max_hi_packages = 6;
struct PackingSpec {
    uint8_t bits;
    const char* packages[k_max_hi_packages];
    unsigned data_and_stops;
    const char* note;
};

const PackingSpec k_spec_packing[] = {
    {8, {"01001000", "01101001", 0, 0, 0, 0}, 18, "the HF presets: one byte per package"},
    {4, {"0100", "1000", "0110", "1001", 0, 0}, 20, "Gustavo's first picture: two packages per byte"},
    {3, {"010", "010", "000", "110", "100", "1"}, 22, "a byte spans packages; a short final package"}};

// Spec 2.2, the "Hi" transmission with the hf preset at 8000 Hz, slot by slot: kind (t tune, m marker, 0/1 data) and
// the carrier sign during the slot (before a marker's twist; '.' = no carrier).
const char* const k_spec_hi_kinds = "tttttttttttttttt" "mmmmmmmm" "01001000" "m" "01101001" "m" "mm";
const char* const k_spec_hi_signs = "++++++++++++++++" "+-+-+-+-" ".+..+..." "+" ".--.-..-" "-" "+-";
const uint32_t k_spec_hi_samples = 6432;
const uint32_t k_spec_hi_status_slot = 28;
const uint8_t k_spec_hi_status_position = 5;
const uint8_t k_spec_hi_status_byte = 0x48;
const uint8_t k_spec_hi_status_bit_index = 4;

// Spec 2.2 and 2.6, the "Hi" packet: its bytes, its N = 16 packages and its duration with the hf preset.
const uint8_t k_spec_hi_packet[] = {0x2D, 0xD4, 0x00, 0x02, 0x48, 0x69, 0x93, 0x4A};
const uint16_t k_spec_hi_crc = 0x934A;
const char* const k_spec_hi_packet_n16[] = {"0010110111010100", "0000000000000010", "0100100001101001",
                                            "1001001101001010"};
const uint32_t k_spec_hi_packet_slots = 98;
const uint32_t k_spec_hi_packet_samples = 13344;
const uint16_t k_spec_crc_check = 0x29B1;

// Spec 2.4, airtime and effective rate without packet framing, printed as the spec prints them.
const size_t k_airtime_sizes[] = {1, 16, 100, 256, 1024};
const size_t k_airtime_columns = sizeof(k_airtime_sizes) / sizeof(k_airtime_sizes[0]);
struct AirtimeSpec {
    Preset preset;
    const char* seconds[k_airtime_columns];
    const char* bit_s[k_airtime_columns];
};

const AirtimeSpec k_spec_airtime[] = {
    {Preset::hf_slow, {"0.96", "5.28", "29.48", "74.40", "295.6"}, {"8.3", "24.2", "27.1", "27.5", "27.7"}},
    {Preset::hf, {"0.66", "2.82", "14.92", "37.38", "147.97"}, {"12.1", "45.4", "53.6", "54.8", "55.4"}},
    {Preset::hf_fast, {"0.51", "1.59", "7.64", "18.87", "74.16"}, {"15.7", "80.6", "104.8", "108.5", "110.5"}},
    {Preset::am, {"0.51", "1.52", "7.24", "17.84", "70.07"}, {"15.7", "84.0", "110.6", "114.8", "116.9"}},
    {Preset::fm, {"0.73", "1.24", "4.09", "9.40", "35.51"}, {"11.0", "103.6", "195.5", "218.0", "230.7"}}};

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

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
    std::fprintf(stderr, "doc_examples: the library disagrees with spec.md: %s\n", message.c_str());
    std::exit(k_exit_failure);
}

void expect(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

// An integer with a typographic minus.
std::string signed_hz(int value) {
    return value < 0 ? std::string(k_minus) + format("%d", -value) : format("%d", value);
}

std::string binary(unsigned value, unsigned bits) {
    std::string text;
    for (unsigned b = bits; b-- > 0;) text += ((value >> b) & 1u) != 0 ? '1' : '0';
    return text;
}

std::string hex_bytes(const std::vector<uint8_t>& bytes) {
    std::string text;
    for (size_t i = 0; i < bytes.size(); ++i) text += format("%s%02X", i == 0 ? "" : " ", bytes[i]);
    return text;
}

std::string char_of(uint8_t byte) {
    return byte >= k_first_printable && byte <= k_last_printable ? format("'%c'", byte) : std::string("");
}

std::string stream_bits(const std::vector<uint8_t>& bytes) {
    std::string bits;
    for (size_t i = 0; i < bytes.size(); ++i) bits += binary(bytes[i], k_byte_bits);
    return bits;
}

std::vector<uint8_t> bytes_of(const char* text) {
    const std::string s = text;
    return std::vector<uint8_t>(s.begin(), s.end());
}

double slot_ms_of(const EncoderConfig& config) {
    return config.slot_us / k_us_per_ms;
}

double net_bit_s(const EncoderConfig& config) {
    const double n = config.bits_per_package;
    return n * k_us_per_s / ((n + 1.0) * config.slot_us);
}

double samples_ms(uint32_t samples) {
    return samples * k_ms_per_s / k_rate_hz;
}

const char* preset_name(Preset preset) {
    for (size_t i = 0; i < sizeof(k_spec_presets) / sizeof(k_spec_presets[0]); ++i) {
        if (k_spec_presets[i].preset == preset) return k_spec_presets[i].name;
    }
    return "?";
}

// "fits, −1062 / +462" style room of a band in a passband; the sign says which way the pitch may move.
std::string room_text(const PassbandFit& fit) {
    if (!fit.fits) {
        const int outside = fit.margin_low_hz < 0 ? -fit.margin_low_hz : -fit.margin_high_hz;
        return format("does not fit (%d Hz %s)", outside, fit.margin_low_hz < 0 ? "below" : "above");
    }
    if (fit.margin_low_hz == fit.margin_high_hz) return format("±%d", fit.margin_low_hz);
    return signed_hz(-fit.margin_low_hz) + " / +" + format("%d", fit.margin_high_hz);
}

// ---------------------------------------------------------------------------
// The Encoder's own account of a transmission: one record per slot, read from status() before its first sample,
// and the carrier sign measured on the audio against the encoder's NCO (spec 1.8: tone_step = round(tone 2^32 / rate)).
// ---------------------------------------------------------------------------

struct SlotRecord {
    EncoderStatus status;
    uint32_t first_sample;
    uint32_t samples;
    int sign;  // +1 or -1; 0 without a carrier
};

struct Transmission {
    EncoderConfig config;
    std::vector<uint8_t> data;
    std::vector<SlotRecord> slots;  // the tail is the last record
    uint32_t samples;
};

bool carries_tone(SlotKind kind) {
    return kind == SlotKind::tone || kind == SlotKind::one || kind == SlotKind::marker;
}

Transmission transmit(const EncoderConfig& config, const std::vector<uint8_t>& data) {
    Transmission t;
    t.config = config;
    t.data = data;
    Encoder encoder(config);
    size_t written = encoder.write(data.data(), data.size());
    if (!encoder.start()) fail("the encoder did not start");
    std::vector<int16_t> audio;
    while (encoder.busy()) {
        if (written < data.size()) written += encoder.write(&data[written], data.size() - written);
        const EncoderStatus status = encoder.status();
        if (t.slots.empty() || status.slot_index != t.slots.back().status.slot_index ||
            status.segment != t.slots.back().status.segment) {
            const SlotRecord record = {status, static_cast<uint32_t>(audio.size()), 0, 0};
            t.slots.push_back(record);
        }
        audio.push_back(encoder.next_sample());
        ++t.slots.back().samples;
    }
    expect(written == data.size(), "the encoder did not take every byte");
    t.samples = static_cast<uint32_t>(audio.size());

    const uint32_t step = static_cast<uint32_t>(
        ((static_cast<uint64_t>(config.tone_hz) << k_phase_bits) + config.sample_rate_hz / 2) / config.sample_rate_hz);
    for (size_t s = 0; s < t.slots.size(); ++s) {
        SlotRecord& slot = t.slots[s];
        if (!carries_tone(slot.status.kind)) continue;
        const double part = slot.status.kind == SlotKind::marker ? unlimited::k_reversal_start : 1.0;
        const uint32_t last = slot.first_sample + static_cast<uint32_t>(part * slot.samples);
        double correlation = 0.0;
        for (uint32_t n = slot.first_sample; n < last; ++n) {
            const uint32_t phase = static_cast<uint32_t>(n * step);
            correlation += audio[n] * std::sin(k_two_pi * phase / k_phase_turn);
        }
        slot.sign = correlation > 0.0 ? 1 : -1;
    }
    return t;
}

size_t count_segment(const Transmission& t, EncoderSegment segment) {
    size_t count = 0;
    for (size_t s = 0; s < t.slots.size(); ++s) count += t.slots[s].status.segment == segment ? 1u : 0u;
    return count;
}

// The bits of each package, as the Encoder sent them.
std::vector<std::string> packages_of(const Transmission& t) {
    std::vector<std::string> packages;
    for (size_t s = 0; s < t.slots.size(); ++s) {
        const EncoderStatus& st = t.slots[s].status;
        if (st.segment != EncoderSegment::package || st.slot > st.package_bits) continue;
        if (packages.size() <= st.package_index) packages.resize(st.package_index + 1);
        packages[st.package_index] += st.kind == SlotKind::one ? '1' : '0';
    }
    return packages;
}

// Spec 2.2: data slot i of package k carries stream bit k N + i - 1 = bit (7 - bit_index) of byte byte_index.
void check_bits(const Transmission& t) {
    for (size_t s = 0; s < t.slots.size(); ++s) {
        const EncoderStatus& st = t.slots[s].status;
        if (st.segment != EncoderSegment::package || st.slot > st.package_bits) continue;
        const uint32_t stream_bit = st.package_index * t.config.bits_per_package + st.slot - 1u;
        expect(stream_bit == st.byte_index * k_byte_bits + st.bit_index,
               format("slot %u: stream bit %u is not byte %u bit %u", static_cast<unsigned>(st.slot_index),
                      static_cast<unsigned>(stream_bit), static_cast<unsigned>(st.byte_index),
                      static_cast<unsigned>(st.bit_index)));
        expect(st.byte == t.data[st.byte_index], "status().byte is not the byte being sent");
        const bool one = ((st.byte >> (k_msb - st.bit_index)) & 1u) != 0;
        expect(one == (st.kind == SlotKind::one), "a data slot's kind is not its bit");
    }
}

// ---------------------------------------------------------------------------
// Section 1: presets and profiles
// ---------------------------------------------------------------------------

bool heard_by(const EncoderConfig& sender, const DecoderConfig& receiver) {
    const double slot_ms = slot_ms_of(sender);
    const Passband range = receiver.search_range();
    return slot_ms >= receiver.min_slot_ms && slot_ms <= receiver.max_slot_ms() && sender.tone_hz >= range.low_hz &&
           sender.tone_hz <= range.high_hz && unlimited::passband_fit(occupied_band(sender), receiver.passband).fits;
}

std::string hearing_profiles(const EncoderConfig& sender) {
    std::string list;
    for (size_t p = 0; p < sizeof(k_spec_profiles) / sizeof(k_spec_profiles[0]); ++p) {
        if (!heard_by(sender, DecoderConfig::for_profile(k_spec_profiles[p].profile))) continue;
        list += (list.empty() ? "" : ", ") + std::string(k_spec_profiles[p].name);
    }
    return list;
}

void print_presets() {
    std::printf("## 1. Presets and receiver profiles\n\n");
    std::printf("### 1.1 Sender presets (spec 1.7)\n\n");
    std::printf("`EncoderConfig::from_preset(preset, %u)`, every preset on %u Hz with %u sync markers, a %u ms tune "
                "tone and a %u ms tail. Net rate = N / ((N + 1) T); overhead = lead-in + tune + sync + END + tail. The "
                "band is `occupied_band(config)`; the fit and the shift tolerance are `passband_fit(config)` in the "
                "preset's own passband: how far the pitch may move down / up and still pass the filter and be found by "
                "the receiver that hears it.\n\n",
                static_cast<unsigned>(k_rate_hz), static_cast<unsigned>(unlimited::k_default_tone_hz),
                static_cast<unsigned>(unlimited::k_default_sync_markers),
                static_cast<unsigned>(unlimited::k_default_tune_ms),
                static_cast<unsigned>(unlimited::k_default_tail_ms));
    std::printf("| Preset | T | N | Pitch | Slots/s | Net bit/s | Passband | Lead-in | Tune | Overhead | Occupied band "
                "| Shift tolerance | −26 dB width | Heard by |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (size_t p = 0; p < sizeof(k_spec_presets) / sizeof(k_spec_presets[0]); ++p) {
        const PresetSpec& spec = k_spec_presets[p];
        const EncoderConfig c = EncoderConfig::from_preset(spec.preset, k_rate_hz);
        expect(c.valid(), format("preset %s is invalid", spec.name));
        const Transmission t = transmit(c, bytes_of(k_hi_text));
        const size_t tune = count_segment(t, EncoderSegment::tune);
        const size_t sync = count_segment(t, EncoderSegment::sync);
        const size_t end = count_segment(t, EncoderSegment::end);
        const double overhead_ms = c.lead_in_ms + (tune + sync + end) * slot_ms_of(c) + c.tail_ms;
        const Band band = occupied_band(c);
        const PassbandFit fit = passband_fit(c);
        const std::string rate = format("%.2f", net_bit_s(c));
        const std::string heard = hearing_profiles(c);
        expect(c.slot_us == spec.slot_us && c.bits_per_package == spec.bits, format("%s: T or N", spec.name));
        expect(rate == spec.net_bit_s, format("%s: net rate %s, spec %s", spec.name, rate.c_str(), spec.net_bit_s));
        expect(c.passband.low_hz == spec.passband_low_hz && c.passband.high_hz == spec.passband_high_hz,
               format("%s: passband", spec.name));
        expect(c.lead_in_ms == spec.lead_in_ms && tune == spec.tune_slots, format("%s: lead-in or tune", spec.name));
        expect(std::lround(overhead_ms) == spec.overhead_ms,
               format("%s: overhead %.0f ms, spec %u ms", spec.name, overhead_ms,
                      static_cast<unsigned>(spec.overhead_ms)));
        expect(band.low_hz == spec.band_low_hz && band.high_hz == spec.band_high_hz,
               format("%s: occupied band %u-%u Hz", spec.name, static_cast<unsigned>(band.low_hz),
                      static_cast<unsigned>(band.high_hz)));
        expect(fit.fits && fit.margin_low_hz == spec.room_down_hz && fit.margin_high_hz == spec.room_up_hz,
               format("%s: room %d / %d Hz", spec.name, fit.margin_low_hz, fit.margin_high_hz));
        expect(heard == spec.heard_by, format("%s: heard by %s, spec %s", spec.name, heard.c_str(), spec.heard_by));
        std::printf("| `%s` | %.0f ms | %u | %u Hz | %.2f | **%s** | %u–%u Hz | %u ms | %u slots, %.0f ms | %.0f ms | "
                    "%u Hz, %u–%u Hz | %s Hz | %u Hz | %s |\n",
                    spec.name, slot_ms_of(c), static_cast<unsigned>(c.bits_per_package),
                    static_cast<unsigned>(c.tone_hz), k_us_per_s / c.slot_us, rate.c_str(),
                    static_cast<unsigned>(c.passband.low_hz), static_cast<unsigned>(c.passband.high_hz),
                    static_cast<unsigned>(c.lead_in_ms), static_cast<unsigned>(tune), tune * slot_ms_of(c),
                    overhead_ms, static_cast<unsigned>(band.width_hz), static_cast<unsigned>(band.low_hz),
                    static_cast<unsigned>(band.high_hz), room_text(fit).c_str(),
                    static_cast<unsigned>(unlimited::width_26db_hz(c.slot_us)), heard.c_str());
    }
    std::printf("\n\"Heard by\": the profiles whose window holds T, whose tone search holds the pitch and whose "
                "passband holds the occupied band (`DecoderConfig::search_range()`, `passband_fit()`).\n\n");

    std::printf("### 1.2 Receiver profiles (spec 1.7)\n\n");
    std::printf("`DecoderConfig::for_profile(profile)` only fills the fields; the receiver learns the pitch, T and N "
                "from the signal.\n\n");
    std::printf("| Profile | `min_slot_ms` | Window (T accepted) | Passband | Tone search (`search_range()`) | Impulse "
                "blanker | Decision | Radio |\n");
    std::printf("|---|---|---|---|---|---|---|---|\n");
    for (size_t p = 0; p < sizeof(k_spec_profiles) / sizeof(k_spec_profiles[0]); ++p) {
        const ProfileSpec& spec = k_spec_profiles[p];
        const DecoderConfig d = DecoderConfig::for_profile(spec.profile);
        const Passband range = d.search_range();
        expect(d.valid(), format("profile %s is invalid", spec.name));
        expect(d.min_slot_ms == spec.min_slot_ms && d.max_slot_ms() == spec.max_slot_ms,
               format("profile %s: window", spec.name));
        expect(d.passband.low_hz == spec.passband_low_hz && d.passband.high_hz == spec.passband_high_hz,
               format("profile %s: passband", spec.name));
        expect(range.low_hz == spec.search_low_hz && range.high_hz == spec.search_high_hz,
               format("profile %s: search %u-%u Hz", spec.name, static_cast<unsigned>(range.low_hz),
                      static_cast<unsigned>(range.high_hz)));
        std::printf("| `%s` | %u | %u–%u ms | %u–%u Hz | %u–%u Hz | %s | %s | %s |\n", spec.name,
                    static_cast<unsigned>(d.min_slot_ms), static_cast<unsigned>(d.min_slot_ms),
                    static_cast<unsigned>(d.max_slot_ms()), static_cast<unsigned>(d.passband.low_hz),
                    static_cast<unsigned>(d.passband.high_hz), static_cast<unsigned>(range.low_hz),
                    static_cast<unsigned>(range.high_hz), d.impulse_blanker ? "on" : "off",
                    d.decision_mode == unlimited::DecisionMode::adaptive
                        ? "smart line"
                        : format("fixed %.0f %%", d.fixed_ratio * 100.0f).c_str(),
                    spec.radio);
    }
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// Section 2: "Hi" slot by slot
// ---------------------------------------------------------------------------

// '+' or '−'; '–' for a slot without a carrier.
std::string sign_text(int sign) {
    return sign > 0 ? "+" : (sign < 0 ? k_minus : k_no_carrier);
}

char kind_letter(const SlotRecord& slot) {
    switch (slot.status.kind) {
        case SlotKind::tone:
            return 't';
        case SlotKind::marker:
            return 'm';
        case SlotKind::one:
            return '1';
        case SlotKind::zero:
            return '0';
        case SlotKind::silent:
            break;
    }
    return 's';
}

// One row per slot of the packages; the tune, the sync train and END in one row each.
void print_slot_table(const Transmission& t) {
    const unsigned n = t.config.bits_per_package;
    std::printf("| Slot | Start | Segment | Slot kind | Package | Position | Bit | Byte, bit (MSB = 0) | "
                "Carrier sign |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|\n");
    for (size_t s = 0; s < t.slots.size();) {
        const SlotRecord& slot = t.slots[s];
        const EncoderStatus& st = slot.status;
        size_t run = 1;
        if (st.segment != EncoderSegment::package) {
            while (s + run < t.slots.size() && t.slots[s + run].status.segment == st.segment) ++run;
        }
        const unsigned first = static_cast<unsigned>(st.slot_index);
        const unsigned last = static_cast<unsigned>(t.slots[s + run - 1].status.slot_index);
        const std::string index = run == 1 ? format("%u", first) : format("%u–%u", first, last);
        const std::string start = format("%.0f ms", samples_ms(slot.first_sample));
        std::string signs;
        for (size_t r = 0; r < run; ++r) signs += (r == 0 ? "" : " ") + sign_text(t.slots[s + r].sign);
        switch (st.segment) {
            case EncoderSegment::lead_in:
                std::printf("| %s | %s | lead-in | silence ×%u | | | | | |\n", index.c_str(), start.c_str(),
                            static_cast<unsigned>(run));
                break;
            case EncoderSegment::tune:
                std::printf("| %s | %s | tune | steady tone ×%u, ramped up in slot %u and down in slot %u | | | | | "
                            "%s |\n",
                            index.c_str(), start.c_str(), static_cast<unsigned>(run), first, last,
                            sign_text(slot.sign).c_str());
                break;
            case EncoderSegment::sync:
                std::printf("| %s | %s | sync | marker ×%u; slot %u is the START of package 0 | | | | | %s |\n",
                            index.c_str(), start.c_str(), static_cast<unsigned>(run), last, signs.c_str());
                break;
            case EncoderSegment::package: {
                if (st.slot > st.package_bits) {
                    const bool next =
                        s + 1 < t.slots.size() && t.slots[s + 1].status.segment == EncoderSegment::package;
                    const std::string what =
                        next ? format("marker: STOP of package %u = START of package %u",
                                      static_cast<unsigned>(st.package_index),
                                      static_cast<unsigned>(st.package_index + 1))
                             : format("marker: STOP of package %u", static_cast<unsigned>(st.package_index));
                    std::printf("| %s | %s | package | %s | %u | STOP | | | %s (then %s) |\n", index.c_str(),
                                start.c_str(), what.c_str(), static_cast<unsigned>(st.package_index),
                                sign_text(slot.sign).c_str(), sign_text(-slot.sign).c_str());
                    break;
                }
                const bool short_package = st.package_bits < n;
                std::printf("| %s | %s | package | %s | %u | %u of %u%s | %u | 0x%02X %s, bit %u (byte %u) | %s |\n",
                            index.c_str(), start.c_str(), st.kind == SlotKind::one ? "one" : "zero",
                            static_cast<unsigned>(st.package_index), static_cast<unsigned>(st.slot),
                            static_cast<unsigned>(st.package_bits), short_package ? " (short)" : "",
                            st.kind == SlotKind::one ? 1u : 0u, st.byte, char_of(st.byte).c_str(),
                            static_cast<unsigned>(st.bit_index), static_cast<unsigned>(st.byte_index),
                            sign_text(slot.sign).c_str());
                break;
            }
            case EncoderSegment::end:
                std::printf("| %s | %s | END | marker ×%u | | | | | %s |\n", index.c_str(), start.c_str(),
                            static_cast<unsigned>(run), signs.c_str());
                break;
            case EncoderSegment::tail:
                std::printf("| – | %s | tail | silence, %u samples (%u ms) | | | | | |\n", start.c_str(),
                            static_cast<unsigned>(slot.samples), static_cast<unsigned>(t.config.tail_ms));
                break;
            case EncoderSegment::idle:
                break;
        }
        s += run;
    }
    std::printf("\n");
}

EncoderConfig hf_with_bits(uint8_t bits) {
    EncoderConfig c = EncoderConfig::from_preset(Preset::hf, k_rate_hz);
    c.bits_per_package = bits;
    expect(c.valid(), format("hf with N = %u is invalid", static_cast<unsigned>(bits)));
    return c;
}

void check_hi_hf(const Transmission& t) {
    const std::string kinds_expected = k_spec_hi_kinds;
    const std::string signs_expected = k_spec_hi_signs;
    std::string kinds;
    std::string signs;
    for (size_t s = 0; s + 1 < t.slots.size(); ++s) {
        kinds += kind_letter(t.slots[s]);
        signs += t.slots[s].sign > 0 ? '+' : (t.slots[s].sign < 0 ? '-' : '.');
    }
    expect(kinds == kinds_expected, "hf \"Hi\" slot kinds " + kinds + ", spec " + kinds_expected);
    expect(signs == signs_expected, "hf \"Hi\" carrier signs " + signs + ", spec " + signs_expected);
    expect(t.samples == k_spec_hi_samples,
           format("hf \"Hi\" lasts %u samples, spec %u", static_cast<unsigned>(t.samples),
                  static_cast<unsigned>(k_spec_hi_samples)));
    const EncoderStatus& st = t.slots[k_spec_hi_status_slot].status;
    expect(st.segment == EncoderSegment::package && st.kind == SlotKind::one && st.slot == k_spec_hi_status_position &&
               st.package_bits == unlimited::k_hf_bits_per_package && st.byte == k_spec_hi_status_byte &&
               st.bit_index == k_spec_hi_status_bit_index && st.package_index == 0 && st.byte_index == 0 &&
               st.slot_index == k_spec_hi_status_slot,
           "Encoder::status() during slot 28 of hf \"Hi\"");
}

void print_hi() {
    const std::vector<uint8_t> hi = bytes_of(k_hi_text);
    const std::string stream = stream_bits(hi);
    std::printf("## 2. \"Hi\" = 0x48 0x69, bit by bit (spec 2.2)\n\n");
    std::printf("The bytes are one stream of bits, most significant bit first: `%s %s`. The stream is cut into "
                "packages of N bits; each package is START, its bits, STOP, and each STOP is the START of the next "
                "package. The last sync marker is the first START; END is two markers after the last STOP. Timing "
                "below: the hf preset (T %.0f ms, %u Hz) at %u Hz, with N changed where stated.\n\n",
                stream.substr(0, k_byte_bits).c_str(), stream.substr(k_byte_bits).c_str(),
                slot_ms_of(EncoderConfig::from_preset(Preset::hf, k_rate_hz)),
                static_cast<unsigned>(unlimited::k_default_tone_hz), static_cast<unsigned>(k_rate_hz));

    std::printf("### 2.1 Packages for N = 8, 4 and 3\n\n");
    std::printf("| N | Packages (START … STOP each) | Data slots + STOPs | Slots in the transmission | "
                "`duration_samples(2)` |\n");
    std::printf("|---|---|---|---|---|\n");
    std::vector<Transmission> transmissions;
    for (size_t i = 0; i < sizeof(k_spec_packing) / sizeof(k_spec_packing[0]); ++i) {
        const PackingSpec& spec = k_spec_packing[i];
        const Transmission t = transmit(hf_with_bits(spec.bits), hi);
        check_bits(t);
        const std::vector<std::string> packages = packages_of(t);
        std::string list;
        size_t data_and_stops = 0;
        for (size_t p = 0; p < packages.size(); ++p) {
            expect(p < k_max_hi_packages && spec.packages[p] != 0 && packages[p] == spec.packages[p],
                   format("N = %u: package %u is %s", static_cast<unsigned>(spec.bits), static_cast<unsigned>(p),
                          packages[p].c_str()));
            list += (p == 0 ? "`" : " · `") + packages[p] + "`";
            data_and_stops += packages[p].size() + 1;
        }
        expect(packages.size() == k_max_hi_packages || spec.packages[packages.size()] == 0,
               format("N = %u: %u packages", static_cast<unsigned>(spec.bits), static_cast<unsigned>(packages.size())));
        expect(data_and_stops == spec.data_and_stops, format("N = %u: %u data slots + STOPs",
                                                             static_cast<unsigned>(spec.bits),
                                                             static_cast<unsigned>(data_and_stops)));
        const uint32_t duration = Encoder(t.config).duration_samples(hi.size());
        expect(duration == t.samples,
               format("N = %u: duration_samples %u, rendered %u", static_cast<unsigned>(spec.bits),
                      static_cast<unsigned>(duration), static_cast<unsigned>(t.samples)));
        std::printf("| %u (%s) | %s | %u | %u | %u samples = %.0f ms |\n", static_cast<unsigned>(spec.bits), spec.note,
                    list.c_str(), static_cast<unsigned>(data_and_stops), static_cast<unsigned>(t.slots.size() - 1),
                    static_cast<unsigned>(duration), samples_ms(duration));
        transmissions.push_back(t);
    }
    std::printf("\n");
    check_hi_hf(transmissions[0]);

    const char* const titles[] = {"N = 8, the hf preset", "N = 4", "N = 3"};
    const char* const notes[] = {
        ("Read from `Encoder::status()` at the first sample of each slot; the carrier sign is measured on the audio "
         "(a marker's before its twist; the sign flips after every marker). `status()` during slot 28: segment "
         "`package`, kind `one`, slot 5, package_bits 8, byte 0x48, bit_index 4, package_index 0, byte_index 0."),
        "Two packages per byte: every byte starts a package.",
        ("A byte spans packages (package 2 holds the last two bits of 'H' and the first of 'i'); the last package "
         "carries the one bit left, its STOP comes early and END follows at once (spec 2.3).")};
    for (size_t i = 0; i < transmissions.size(); ++i) {
        std::printf("### 2.%u %s: slot by slot\n\n", static_cast<unsigned>(i + 2), titles[i]);
        std::printf("%s\n\n", notes[i]);
        print_slot_table(transmissions[i]);
    }
}

// ---------------------------------------------------------------------------
// Section 3: packets
// ---------------------------------------------------------------------------

std::vector<uint8_t> packet_of(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet(k_packet_buffer);
    const size_t size =
        unlimited::packet_build(payload.data(), static_cast<uint16_t>(payload.size()), packet.data(), packet.size());
    expect(size == payload.size() + unlimited::k_packet_overhead, "packet_build() failed");
    packet.resize(size);
    return packet;
}

std::string package_list(const std::vector<std::string>& packages) {
    std::string list;
    for (size_t p = 0; p < packages.size(); ++p) list += (p == 0 ? "`" : " · `") + packages[p] + "`";
    return list;
}

void print_packets() {
    const std::vector<uint8_t> check = bytes_of(k_crc_check_text);
    const uint16_t crc_check = unlimited::crc16_ccitt(check.data(), check.size());
    expect(crc_check == k_spec_crc_check && crc_check == unlimited::k_crc16_check, "CRC-16 of \"123456789\"");

    const std::vector<uint8_t> hi_packet = packet_of(bytes_of(k_hi_text));
    expect(hi_packet == std::vector<uint8_t>(k_spec_hi_packet, k_spec_hi_packet + sizeof(k_spec_hi_packet)),
           "the \"Hi\" packet is " + hex_bytes(hi_packet));
    const size_t hi_crc_at = hi_packet.size() - unlimited::k_packet_crc;
    expect(((hi_packet[hi_crc_at] << k_byte_bits) | hi_packet[hi_crc_at + 1]) == k_spec_hi_crc, "the \"Hi\" CRC");
    const Transmission hi_n16 = transmit(hf_with_bits(unlimited::k_wide_bits_per_package), hi_packet);
    const std::vector<std::string> hi_packages = packages_of(hi_n16);
    for (size_t p = 0; p < hi_packages.size(); ++p) {
        expect(p < sizeof(k_spec_hi_packet_n16) / sizeof(k_spec_hi_packet_n16[0]) &&
                   hi_packages[p] == k_spec_hi_packet_n16[p],
               "the \"Hi\" packet's N = 16 packages");
    }
    const EncoderConfig hf = EncoderConfig::from_preset(Preset::hf, k_rate_hz);
    const Transmission hi_hf = transmit(hf, hi_packet);
    expect(hi_hf.slots.size() - 1 == k_spec_hi_packet_slots && hi_hf.samples == k_spec_hi_packet_samples &&
               Encoder(hf).duration_samples(hi_packet.size()) == k_spec_hi_packet_samples,
           format("the \"Hi\" packet with hf: %u slots, %u samples", static_cast<unsigned>(hi_hf.slots.size() - 1),
                  static_cast<unsigned>(hi_hf.samples)));

    const std::vector<uint8_t> payload = bytes_of(k_packet_text);
    const std::vector<uint8_t> packet = packet_of(payload);
    const size_t crc_at = unlimited::k_packet_header + payload.size();
    std::printf("## 3. Packets (spec 2.6)\n\n");
    std::printf("`packet_build()` wraps a payload as `[2D D4][LEN hi][LEN lo][payload][CRC hi][CRC lo]`; the CRC is "
                "CRC-16/CCITT-FALSE (poly 0x%04X, init 0x%04X) over the two LEN bytes and the payload; "
                "`crc16_ccitt(\"%s\")` = 0x%04X.\n\n",
                static_cast<unsigned>(unlimited::k_crc16_poly), static_cast<unsigned>(unlimited::k_crc16_init),
                k_crc_check_text, static_cast<unsigned>(crc_check));
    std::printf("### 3.1 \"%s\"\n\n", k_packet_text);
    std::printf("| Field | Bytes |\n|---|---|\n");
    std::printf("| sync | `%02X %02X` |\n", packet[0], packet[1]);
    std::printf("| LEN (16 bits, big-endian) | `%02X %02X` = %u |\n", packet[2], packet[3],
                static_cast<unsigned>(payload.size()));
    std::printf("| payload \"%s\" | `%s` |\n", k_packet_text,
                hex_bytes(std::vector<uint8_t>(packet.begin() + unlimited::k_packet_header, packet.begin() + crc_at))
                    .c_str());
    std::printf("| CRC over LEN + payload | `%02X %02X` = 0x%04X |\n", packet[crc_at], packet[crc_at + 1],
                static_cast<unsigned>((packet[crc_at] << k_byte_bits) | packet[crc_at + 1]));
    std::printf("\nPacket: %u bytes `%s`.\n\n", static_cast<unsigned>(packet.size()), hex_bytes(packet).c_str());

    std::printf("As packages (the bytes of the packet, MSB first; one row per preset's N):\n\n");
    std::printf("| N | Presets | Packages |\n|---|---|---|\n");
    const uint8_t sizes[] = {unlimited::k_hf_bits_per_package, unlimited::k_wide_bits_per_package};
    const char* const presets[] = {"`hf_slow`, `hf`, `hf_fast`", "`am`, `fm`"};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        const Transmission t = transmit(hf_with_bits(sizes[i]), packet);
        check_bits(t);
        const std::vector<std::string> packages = packages_of(t);
        const size_t last_bits = packages.back().size();
        const std::string short_text =
            last_bits < sizes[i] ? format(" (the last one short, %u bits)", static_cast<unsigned>(last_bits)) : "";
        std::printf("| %u | %s | %u packages%s: %s |\n", static_cast<unsigned>(sizes[i]), presets[i],
                    static_cast<unsigned>(packages.size()), short_text.c_str(), package_list(packages).c_str());
    }
    std::printf("\n### 3.2 \"%s\" (the spec's example)\n\n", k_hi_text);
    std::printf("`%s` (CRC 0x%04X): with N = 16 the packages %s; with `hf` %u slots + tail = %u samples = %.3f s.\n\n",
                hex_bytes(hi_packet).c_str(), static_cast<unsigned>(k_spec_hi_crc), package_list(hi_packages).c_str(),
                static_cast<unsigned>(hi_hf.slots.size() - 1), static_cast<unsigned>(hi_hf.samples),
                hi_hf.samples / static_cast<double>(k_rate_hz));
}

// ---------------------------------------------------------------------------
// Section 4: durations
// ---------------------------------------------------------------------------

void print_duration_row(const char* name, const Transmission& t) {
    const EncoderConfig& c = t.config;
    const uint32_t expected = Encoder(c).duration_samples(t.data.size());
    expect(t.samples + 1 >= expected && t.samples <= expected + 1,
           format("%s: duration_samples %u against %u rendered", name, static_cast<unsigned>(expected),
                  static_cast<unsigned>(t.samples)));
    const size_t bits = t.data.size() * k_byte_bits;
    const size_t packages = (bits + c.bits_per_package - 1) / c.bits_per_package;
    size_t data_slots = 0;
    size_t stops = 0;
    for (size_t s = 0; s < t.slots.size(); ++s) {
        const EncoderStatus& st = t.slots[s].status;
        if (st.segment != EncoderSegment::package) continue;
        if (st.slot > st.package_bits) {
            ++stops;
        } else {
            ++data_slots;
        }
    }
    const size_t tune = count_segment(t, EncoderSegment::tune);
    const size_t sync = count_segment(t, EncoderSegment::sync);
    const size_t end = count_segment(t, EncoderSegment::end);
    expect(data_slots == bits && stops == packages && end == unlimited::k_end_markers,
           format("%s: %u data slots, %u STOPs", name, static_cast<unsigned>(data_slots),
                  static_cast<unsigned>(stops)));
    const size_t slots = tune + sync + data_slots + stops + end;
    std::printf("| `%s` | %u | %u ms | %u | %u | %u | %u | %u | %u ms | %u slots × %.0f ms | **%u** (%.3f s) | %u |\n",
                name, static_cast<unsigned>(t.data.size()), static_cast<unsigned>(c.lead_in_ms),
                static_cast<unsigned>(tune), static_cast<unsigned>(sync), static_cast<unsigned>(data_slots),
                static_cast<unsigned>(stops), static_cast<unsigned>(end), static_cast<unsigned>(c.tail_ms),
                static_cast<unsigned>(slots), slot_ms_of(c), static_cast<unsigned>(expected),
                expected / static_cast<double>(k_rate_hz), static_cast<unsigned>(t.samples));
}

void print_durations() {
    std::printf("## 4. Durations (spec 2.4)\n\n");
    std::printf("`Encoder::duration_samples(n)` = lead + (N_tune + N_sync + B + P + 2)·T·rate + tail samples for "
                "n bytes, B = 8n data slots and P = ⌈B/N⌉ STOPs; the last column is what the Encoder rendered.\n\n");
    std::printf("### 4.1 Segment breakdown\n\n");
    std::printf("| Preset | Bytes | Lead-in | Tune | Sync | Data slots | STOPs | END | Tail | Slots | "
                "`duration_samples` | Rendered |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    const std::vector<uint8_t> packet = packet_of(bytes_of(k_packet_text));
    print_duration_row("hf", transmit(EncoderConfig::from_preset(Preset::hf, k_rate_hz), bytes_of(k_hi_text)));
    for (size_t p = 0; p < sizeof(k_spec_presets) / sizeof(k_spec_presets[0]); ++p) {
        print_duration_row(k_spec_presets[p].name,
                           transmit(EncoderConfig::from_preset(k_spec_presets[p].preset, k_rate_hz), packet));
    }
    std::printf("\nFirst row: \"%s\" (2 bytes, no packet); the others: the %u-byte \"%s\" packet of section 3.\n\n",
                k_hi_text, static_cast<unsigned>(packet.size()), k_packet_text);

    std::printf("### 4.2 Airtime and effective rate (no packet framing)\n\n");
    std::printf("| Preset (net bit/s) |");
    for (size_t i = 0; i < k_airtime_columns; ++i) {
        std::printf(" %u byte%s |", static_cast<unsigned>(k_airtime_sizes[i]), k_airtime_sizes[i] == 1 ? "" : "s");
    }
    std::printf("\n|---|");
    for (size_t i = 0; i < k_airtime_columns; ++i) std::printf("---|");
    std::printf("\n");
    for (size_t p = 0; p < sizeof(k_spec_airtime) / sizeof(k_spec_airtime[0]); ++p) {
        const AirtimeSpec& spec = k_spec_airtime[p];
        const EncoderConfig c = EncoderConfig::from_preset(spec.preset, k_rate_hz);
        const Encoder encoder(c);
        std::printf("| `%s` (%.1f) |", preset_name(spec.preset), net_bit_s(c));
        for (size_t i = 0; i < k_airtime_columns; ++i) {
            const double seconds = encoder.duration_samples(k_airtime_sizes[i]) / static_cast<double>(k_rate_hz);
            const double rate = k_airtime_sizes[i] * k_byte_bits / seconds;
            const std::string expected = spec.seconds[i];
            const size_t point = expected.find('.');
            const int decimals = static_cast<int>(point == std::string::npos ? 0 : expected.size() - point - 1);
            const std::string text = format("%.*f", decimals, seconds);
            const std::string rate_text = format("%.1f", rate);
            expect(text == expected && rate_text == spec.bit_s[i],
                   format("%s, %u bytes: %s s, %s bit/s; spec %s s, %s bit/s", preset_name(spec.preset),
                          static_cast<unsigned>(k_airtime_sizes[i]), text.c_str(), rate_text.c_str(), expected.c_str(),
                          spec.bit_s[i]));
            std::printf(" %.2f s, %s bit/s |", seconds, rate_text.c_str());
        }
        std::printf("\n");
    }
    std::printf("\nAdd %u bytes per packet with the packet framing of section 3.\n\n",
                static_cast<unsigned>(unlimited::k_packet_overhead));
}

// ---------------------------------------------------------------------------
// Section 5: bandwidth
// ---------------------------------------------------------------------------

void print_bandwidth() {
    const uint16_t tone = unlimited::k_default_tone_hz;
    std::printf("## 5. Bandwidth (spec 1.5)\n\n");
    std::printf("### 5.1 Widths per slot length\n\n");
    std::printf("`occupied_band(tone, T)`: tone ± ⌈%u·500/T_µs⌉ Hz, 99 %% of a data slot's energy (4.4/T); "
                "`width_26db_hz(T)` = ⌈%u·1000/T_µs⌉ and `width_40db_hz(T)` = ⌈%u·1000/T_µs⌉: outside them a data "
                "slot's spectrum stays 26 dB and 40 dB below its centre.\n\n",
                static_cast<unsigned>(unlimited::k_band_99_milli), static_cast<unsigned>(unlimited::k_band_26db_milli),
                static_cast<unsigned>(unlimited::k_band_40db_milli));
    std::printf("| T | Occupied band (99 %%) | At %u Hz | −26 dB width | −40 dB width | Presets |\n",
                static_cast<unsigned>(tone));
    std::printf("|---|---|---|---|---|---|\n");
    for (size_t i = 0; i < sizeof(k_spec_widths) / sizeof(k_spec_widths[0]); ++i) {
        const WidthSpec& spec = k_spec_widths[i];
        const Band band = unlimited::occupied_band(tone, spec.slot_us);
        const uint16_t w26 = unlimited::width_26db_hz(spec.slot_us);
        const uint16_t w40 = unlimited::width_40db_hz(spec.slot_us);
        expect(band.width_hz == spec.occupied_hz && w26 == spec.width_26db_hz && w40 == spec.width_40db_hz,
               format("widths at T = %u us: %u, %u, %u Hz", static_cast<unsigned>(spec.slot_us),
                      static_cast<unsigned>(band.width_hz), static_cast<unsigned>(w26), static_cast<unsigned>(w40)));
        std::string presets;
        for (size_t p = 0; p < sizeof(k_spec_presets) / sizeof(k_spec_presets[0]); ++p) {
            if (k_spec_presets[p].slot_us != spec.slot_us) continue;
            presets += (presets.empty() ? "`" : ", `") + std::string(k_spec_presets[p].name) + "`";
        }
        std::printf("| %.0f ms | %u Hz | %u–%u Hz | %u Hz | %u Hz | %s |\n", spec.slot_us / k_us_per_ms,
                    static_cast<unsigned>(band.width_hz), static_cast<unsigned>(band.low_hz),
                    static_cast<unsigned>(band.high_hz), static_cast<unsigned>(w26), static_cast<unsigned>(w40),
                    presets.c_str());
    }

    std::printf("\n### 5.2 Fit and shift tolerance at %u Hz in typical receivers\n\n", static_cast<unsigned>(tone));
    std::printf("`passband_fit(config)` of a %u Hz sender with slot T in each passband: how far the pitch may move "
                "down / up before the band leaves the passband or the pitch leaves the search of the receiver that "
                "hears it (%u..%u Hz; from %u Hz below %u ms). ±: the same both ways. Mistuning moves every audio "
                "frequency by the same amount; USB tuned too low moves the audio up, LSB the opposite.\n\n",
                static_cast<unsigned>(tone), static_cast<unsigned>(unlimited::k_min_tone_hz),
                static_cast<unsigned>(unlimited::k_max_tone_hz), static_cast<unsigned>(unlimited::k_min_fast_tone_hz),
                static_cast<unsigned>(unlimited::k_fast_slot_us / k_us_per_ms));
    std::printf("| T |");
    for (size_t f = 0; f < k_filter_count; ++f) {
        std::printf(" %s (%u–%u Hz) |", k_filters[f].name, static_cast<unsigned>(k_filters[f].low_hz),
                    static_cast<unsigned>(k_filters[f].high_hz));
    }
    std::printf("\n|---|");
    for (size_t f = 0; f < k_filter_count; ++f) std::printf("---|");
    std::printf("\n");
    const uint32_t rows[] = {128000, 64000, 32000, 16000, 8000, 4000};
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); ++r) {
        std::printf("| %.0f ms |", rows[r] / k_us_per_ms);
        for (size_t f = 0; f < k_filter_count; ++f) {
            EncoderConfig c = EncoderConfig::from_preset(Preset::hf, k_rate_hz);
            c.slot_us = rows[r];
            c.passband.low_hz = k_filters[f].low_hz;
            c.passband.high_hz = k_filters[f].high_hz;
            expect(c.valid(), format("T = %u us in %s: invalid sender", static_cast<unsigned>(rows[r]),
                                     k_filters[f].name));
            const PassbandFit fit = passband_fit(c);
            for (size_t s = 0; s < sizeof(k_spec_fits) / sizeof(k_spec_fits[0]); ++s) {
                if (k_spec_fits[s].slot_us != rows[r]) continue;
                expect(fit.fits && fit.margin_low_hz == k_spec_fits[s].room[f][0] &&
                           fit.margin_high_hz == k_spec_fits[s].room[f][1],
                       format("T = %u us in %s: room %d / %d Hz", static_cast<unsigned>(rows[r]), k_filters[f].name,
                              fit.margin_low_hz, fit.margin_high_hz));
            }
            std::printf(" %s |", room_text(fit).c_str());
        }
        std::printf("\n");
    }
    std::printf("\n### 5.3 The pitch centred in a 1.8 kHz filter\n\n");
    const Passband narrow = {k_filters[k_narrow_filter].low_hz, k_filters[k_narrow_filter].high_hz};
    std::printf("In %s (%u–%u Hz) a pitch of %u Hz centres the band:", k_filters[k_narrow_filter].name,
                static_cast<unsigned>(narrow.low_hz), static_cast<unsigned>(narrow.high_hz),
                static_cast<unsigned>(k_centred_tone_hz));
    for (size_t i = 0; i < sizeof(k_spec_centred) / sizeof(k_spec_centred[0]); ++i) {
        EncoderConfig c = EncoderConfig::from_preset(k_spec_centred[i].preset, k_rate_hz);
        c.tone_hz = k_centred_tone_hz;
        c.passband = narrow;
        const PassbandFit fit = passband_fit(c);
        expect(c.valid() && fit.margin_low_hz == k_spec_centred[i].room_down_hz &&
                   fit.margin_high_hz == k_spec_centred[i].room_up_hz,
               format("%s at %u Hz in 1.8 kHz: -%d/+%d Hz", k_spec_centred[i].name,
                      static_cast<unsigned>(k_centred_tone_hz), fit.margin_low_hz, fit.margin_high_hz));
        const Band band = occupied_band(c);
        std::printf("%s `%s` %u–%u Hz, %s Hz", i == 0 ? "" : ";", k_spec_centred[i].name,
                    static_cast<unsigned>(band.low_hz), static_cast<unsigned>(band.high_hz), room_text(fit).c_str());
    }
    std::printf(" (the sender checks it with `EncoderConfig::passband`, `ConfigError::outside_passband`).\n");
}

}  // namespace

int main() {
    std::printf("<!-- Generated by `make docs` (tools/doc_examples.cpp). Do not edit: every value is computed by the "
                "library. -->\n\n");
    std::printf("# Unlimited v0.3 — protocol examples\n\n");
    std::printf("Generated by `make docs` from the library code (`tools/doc_examples.cpp`); do not edit by hand. Every "
                "number below comes from the library function named with it, and every example that `spec.md` also "
                "gives is checked against the library: `make docs` stops when they disagree. Section numbers in "
                "parentheses refer to `spec.md`.\n\n");
    print_presets();
    print_hi();
    print_packets();
    print_durations();
    print_bandwidth();
    return 0;
}
