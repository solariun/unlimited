#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"
#include "unlimited/protocol.hpp"

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Prints docs/protocol_examples.md (make docs) as Markdown: the speeds, "Hi" slot by slot, a VOX lead, durations,
// the bandwidth in typical filters and the receiver's settings. Every value is computed by the library; every example
// spec.md also gives is checked against it, and a disagreement stops make docs (exit 1, the reason on stderr).
namespace {

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
using unlimited::SlotKind;

const uint32_t k_rate_hz = unlimited::k_decoder_rate_hz;
const double k_us_per_ms = 1e3;
const double k_ms_per_s = 1e3;
const double k_us_per_s = 1e6;
const size_t k_format_buffer = 2048;
const int k_exit_failure = 1;
const unsigned k_nibble_bits = 4;
const unsigned k_nibble_mask = 0xFu;
const uint8_t k_first_printable = 0x20;
const uint8_t k_last_printable = 0x7E;
const double k_bits_per_byte = 8.0;
const unsigned k_centi = unlimited::k_centi_per_unit;

const char* const k_hi_text = "Hi";
const char* const k_minus = "\xE2\x88\x92";  // U+2212, typographic minus

// ---------------------------------------------------------------------------
// The examples of spec.md the library must reproduce
// ---------------------------------------------------------------------------

const float k_spec_speeds[] = {1.0f, 3.0f, 6.0f, 12.0f, 25.0f};  // spec 1.3's table
const uint32_t k_spec_slots_us[] = {100000, 33333, 16667, 8333, 4000};
const char* const k_spec_uses[] = {"very weak HF paths", "weak HF", "HF default", "good HF, AM", "FM"};
// Spec 2.2: "Hi" at 6 bytes/s, the windows as slot strings, 20 slots, and the transmission with its 100 ms tail.
const char* const k_spec_hi_windows[] = {"T.T..T...T", "T.TT.T..TT"};
const uint32_t k_spec_hi_samples = 3467;
const uint32_t k_spec_hi_signal_ms = 333;
// Spec 2.4: 100 bytes at 6 bytes/s take 16.7 s of windows.
const size_t k_spec_duration_bytes = 100;
const double k_spec_duration_s = 16.7;
const size_t k_duration_sizes[] = {1, 10, 100, 1000};

// Typical receiver filters (spec 1.3).
struct Filter {
    const char* name;
    uint16_t low_hz;
    uint16_t high_hz;
};

const Filter k_filters[] = {{"1.8 kHz SSB", 300, 2100},
                            {"2.4 kHz SSB (default)", 300, 2700},
                            {"2.7 kHz SSB", 200, 2900},
                            {"AM", 100, 3000},
                            {"NBFM", 300, 3000}};

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

std::string binary(unsigned value, unsigned bits) {
    std::string text;
    for (unsigned b = bits; b-- > 0;) text += ((value >> b) & 1u) != 0 ? '1' : '0';
    return text;
}

std::string char_of(uint8_t byte) {
    return byte >= k_first_printable && byte <= k_last_printable ? format("'%c'", byte) : std::string("");
}

std::vector<uint8_t> bytes_of(const char* text) {
    const std::string s = text;
    return std::vector<uint8_t>(s.begin(), s.end());
}

double slot_ms_of(const EncoderConfig& config) {
    return config.slot_us / k_us_per_ms;
}

double samples_ms(uint32_t samples) {
    return samples * k_ms_per_s / k_rate_hz;
}

std::string speed_text(float speed) {
    const unsigned centi = unlimited::centi_bytes_per_second(speed);
    return format("%u.%02u", centi / k_centi, centi % k_centi);
}

// "±1068" or "−1068 / +468": how far the pitch may move down / up.
std::string room_text(const PassbandFit& fit) {
    if (!fit.fits) {
        const int outside = fit.margin_low_hz < 0 ? -fit.margin_low_hz : -fit.margin_high_hz;
        return format("does not fit (%d Hz %s)", outside, fit.margin_low_hz < 0 ? "below" : "above");
    }
    if (fit.margin_low_hz == fit.margin_high_hz) return format("±%d", fit.margin_low_hz);
    return std::string(k_minus) + format("%d / +%d", fit.margin_low_hz, fit.margin_high_hz);
}

EncoderConfig at_speed(float speed) {
    EncoderConfig config;
    config.sample_rate_hz = k_rate_hz;
    config.slot_us = unlimited::slot_us_for_speed(speed);
    return config;
}

// ---------------------------------------------------------------------------
// The Encoder's own account of a transmission: one record per slot, read from status() before its first sample.
// ---------------------------------------------------------------------------

struct SlotRecord {
    EncoderStatus status;
    uint32_t first_sample;
    uint32_t samples;
};

struct Transmission {
    EncoderConfig config;
    std::vector<uint8_t> data;
    std::vector<SlotRecord> slots;  // the tail is the last record
    uint32_t samples;
};

Transmission transmit(const EncoderConfig& config, const std::vector<uint8_t>& data) {
    Transmission t;
    t.config = config;
    t.data = data;
    Encoder encoder(config);
    size_t written = encoder.write(data.data(), data.size());
    expect(encoder.start(), "the encoder refused the configuration");
    uint32_t n = 0;
    while (encoder.busy()) {
        if (written < data.size()) written += encoder.write(&data[written], data.size() - written);
        const EncoderStatus status = encoder.status();
        if (t.slots.empty() || t.slots.back().status.slot_index != status.slot_index ||
            t.slots.back().status.segment != status.segment) {
            const SlotRecord record = {status, n, 0};
            t.slots.push_back(record);
        }
        encoder.next_sample();
        ++t.slots.back().samples;
        ++n;
    }
    t.samples = n;
    expect(n == encoder.duration_samples(data.size()), "duration_samples() differs from the rendered length");
    return t;
}

// The windows as slot strings (T a tone, . a silence).
std::vector<std::string> windows_of(const Transmission& t) {
    std::vector<std::string> windows;
    for (size_t s = 0; s < t.slots.size(); ++s) {
        const EncoderStatus& st = t.slots[s].status;
        if (st.segment != EncoderSegment::window) continue;
        if (st.slot == unlimited::k_start_slot) windows.push_back(std::string());
        windows.back() += st.kind == SlotKind::zero ? '.' : 'T';
    }
    return windows;
}

const char* segment_name(EncoderSegment segment) {
    switch (segment) {
        case EncoderSegment::idle:
            return "idle";
        case EncoderSegment::lead_in:
            return "lead-in";
        case EncoderSegment::vox_lead:
            return "VOX lead";
        case EncoderSegment::gap:
            return "gap";
        case EncoderSegment::window:
            return "window";
        case EncoderSegment::tail:
            return "tail";
    }
    return "?";
}

const char* kind_name(SlotKind kind) {
    switch (kind) {
        case SlotKind::silent:
            return "silent";
        case SlotKind::lead:
            return "lead (steady tone)";
        case SlotKind::start:
            return "START (beep)";
        case SlotKind::one:
            return "1 (beep)";
        case SlotKind::zero:
            return "0 (silence)";
        case SlotKind::stop:
            return "STOP (beep)";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// 1. Speeds
// ---------------------------------------------------------------------------

void print_speeds() {
    std::printf("## 1. Speeds (spec 1.3)\n\n");
    std::printf("The speed B is given in bytes per second, the same on both sides; T = 1 / (10 B) is "
                "`slot_us_for_speed(B)`, rounded to the microsecond. Every byte is one window of 10 slots, so the net "
                "rate is exactly B bytes/s (8 B bit/s). The occupied band (99 %% of the power) is "
                "`occupied_band(1500, T)`; the −26 dB and −40 dB widths are `width_26db_hz(T)` and `width_40db_hz(T)`; "
                "the shift tolerance is `passband_fit(config)` in the 2.4 kHz SSB filter (300–2700 Hz): how far the "
                "pitch may move down / up and still pass the filter and be found by the receiver of that speed. The "
                "look-ahead is `Decoder::lookahead_samples()`: the receiver's events come this much after the audio "
                "(spec 3.1).\n\n");
    std::printf("| Speed | T | Samples per slot at 8 kHz | Bit rate | Occupied band | −26 dB width | −40 dB width | "
                "Shift tolerance (300–2700 Hz) | Look-ahead | Typical use |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|\n");
    for (size_t i = 0; i < sizeof(k_spec_speeds) / sizeof(k_spec_speeds[0]); ++i) {
        const EncoderConfig config = at_speed(k_spec_speeds[i]);
        expect(config.slot_us == k_spec_slots_us[i],
               format("%.0f bytes/s: T %u us, not %u", static_cast<double>(k_spec_speeds[i]),
                      static_cast<unsigned>(config.slot_us), static_cast<unsigned>(k_spec_slots_us[i])));
        expect(config.valid(), format("%.0f bytes/s refused", static_cast<double>(k_spec_speeds[i])));
        const Band band = unlimited::occupied_band(config);
        DecoderConfig receiver;
        receiver.slot_us = config.slot_us;
        const unlimited::Decoder decoder(receiver, nullptr, nullptr);
        std::printf("| **%s bytes/s** | %.3f ms | %.2f | %.0f bit/s | %u Hz, %u–%u Hz | %u Hz | %u Hz | %s Hz | "
                    "%u samples = %.0f ms | %s |\n",
                    speed_text(k_spec_speeds[i]).c_str(), slot_ms_of(config),
                    config.slot_us * k_rate_hz / k_us_per_s, k_bits_per_byte * k_spec_speeds[i],
                    static_cast<unsigned>(band.width_hz), static_cast<unsigned>(band.low_hz),
                    static_cast<unsigned>(band.high_hz), static_cast<unsigned>(unlimited::width_26db_hz(config.slot_us)),
                    static_cast<unsigned>(unlimited::width_40db_hz(config.slot_us)),
                    room_text(unlimited::passband_fit(config)).c_str(),
                    static_cast<unsigned>(decoder.lookahead_samples()), samples_ms(decoder.lookahead_samples()),
                    k_spec_uses[i]);
    }
    std::printf("\nAny speed from %s to %s bytes/s in steps of 0.01 is valid (`slot_valid()`); "
                "`centi_bytes_per_second()` and `slot_us_for_centi_speed()` are the integer forms for the AVR.\n\n",
                speed_text(unlimited::k_min_bytes_per_second).c_str(),
                speed_text(unlimited::k_max_bytes_per_second).c_str());
}

// ---------------------------------------------------------------------------
// 2. "Hi" slot by slot
// ---------------------------------------------------------------------------

void print_hi() {
    const EncoderConfig config;  // 6 bytes/s, 1500 Hz, 8000 Hz, no lead-in, 100 ms tail
    const std::vector<uint8_t> hi = bytes_of(k_hi_text);
    const Transmission t = transmit(config, hi);
    const std::vector<std::string> windows = windows_of(t);
    expect(windows.size() == 2 && windows[0] == k_spec_hi_windows[0] && windows[1] == k_spec_hi_windows[1],
           "the \"Hi\" windows differ from spec 2.2");
    expect(t.samples == k_spec_hi_samples, format("\"Hi\" is %u samples, not %u", static_cast<unsigned>(t.samples),
                                                  static_cast<unsigned>(k_spec_hi_samples)));
    const uint32_t signal = t.slots.back().first_sample;
    expect(static_cast<uint32_t>(samples_ms(signal)) == k_spec_hi_signal_ms,
           format("\"Hi\" signal %.1f ms, not %u ms", samples_ms(signal), static_cast<unsigned>(k_spec_hi_signal_ms)));

    std::printf("## 2. \"Hi\" = 0x48 0x69 at 6 bytes/s, slot by slot (spec 2.2)\n\n");
    std::printf("`EncoderConfig()` (6 bytes/s: T %.3f ms, 1500 Hz, 8000 Hz, no lead-in, 100 ms tail). Each byte is one "
                "window: START, its 8 bits most significant first (a beep is 1, silence is 0), STOP. Windows follow "
                "each other with no gap. Read from `Encoder::status()` at the first sample of each slot; slot j starts "
                "at sample ceil(j × %.3f).\n\n",
                slot_ms_of(config), config.slot_us * k_rate_hz / k_us_per_s);
    std::printf("| Window | Slots 0..9 (T = tone, . = silence) | Byte |\n|---|---|---|\n");
    for (size_t w = 0; w < windows.size(); ++w) {
        std::string spaced;
        for (size_t i = 0; i < windows[w].size(); ++i) spaced += format("%s%c", i == 0 ? "" : " ", windows[w][i]);
        std::printf("| %u | `%s` | 0x%02X %s = %s %s |\n", static_cast<unsigned>(w), spaced.c_str(), hi[w],
                    char_of(hi[w]).c_str(), binary(hi[w] >> k_nibble_bits, k_nibble_bits).c_str(),
                    binary(hi[w] & k_nibble_mask, k_nibble_bits).c_str());
    }
    std::printf("\n| Slot | Start | Segment | Slot kind | Window | Position | Bit | `status()` byte, bit_index |\n");
    std::printf("|---|---|---|---|---|---|---|---|\n");
    for (size_t s = 0; s < t.slots.size(); ++s) {
        const EncoderStatus& st = t.slots[s].status;
        const double start_ms = samples_ms(t.slots[s].first_sample);
        if (st.segment == EncoderSegment::tail) {
            std::printf("| – | %.1f ms (sample %u) | tail | %u samples of silence (%.0f ms) | | | | |\n", start_ms,
                        static_cast<unsigned>(t.slots[s].first_sample), static_cast<unsigned>(t.slots[s].samples),
                        samples_ms(t.slots[s].samples));
            continue;
        }
        const bool data = st.slot >= unlimited::k_first_data_slot && st.slot < unlimited::k_stop_slot;
        std::printf("| %u | %.1f ms (sample %u) | %s | %s | %u | %u of 10 | %s | %s |\n",
                    static_cast<unsigned>(st.slot_index), start_ms, static_cast<unsigned>(t.slots[s].first_sample),
                    segment_name(st.segment), kind_name(st.kind), static_cast<unsigned>(st.byte_index),
                    static_cast<unsigned>(st.slot),
                    data ? (st.kind == SlotKind::one ? "1" : "0") : "",
                    data ? format("0x%02X %s, %u", st.byte, char_of(st.byte).c_str(),
                                  static_cast<unsigned>(st.bit_index)).c_str()
                         : "");
    }
    std::printf("\nThe transmission: 20 slots = %.0f ms of signal and the tail, %u samples = %.1f ms in all "
                "(`duration_samples(2)`). A key pressed alone is one window: %.0f ms at 6 bytes/s.\n\n",
                samples_ms(signal), static_cast<unsigned>(t.samples), samples_ms(t.samples),
                10.0 * slot_ms_of(config));
}

// ---------------------------------------------------------------------------
// 3. A VOX lead
// ---------------------------------------------------------------------------

void print_vox() {
    EncoderConfig config;
    config.vox_lead_ms = unlimited::k_default_vox_lead_ms;
    const Transmission t = transmit(config, bytes_of(k_hi_text));
    size_t lead = 0;
    size_t gap = 0;
    uint32_t first_start = 0;
    for (size_t s = 0; s < t.slots.size(); ++s) {
        if (t.slots[s].status.segment == EncoderSegment::vox_lead) ++lead;
        if (t.slots[s].status.segment == EncoderSegment::gap) ++gap;
        if (t.slots[s].status.segment == EncoderSegment::window && first_start == 0)
            first_start = t.slots[s].first_sample;
    }
    expect(gap == unlimited::k_vox_gap_slots, "the VOX gap is not 2 slots");
    std::printf("## 3. With a VOX lead (spec 2.1)\n\n");
    std::printf("`vox_lead_ms` %u at 6 bytes/s: a steady tone of max(ceil(%u ms / T), %u) = %u slots, ramped up in its "
                "first slot and down in its last, then %u silent slots; the START of byte 0 is the first tone after "
                "them, at %.1f ms. \"Hi\" then takes %u samples = %.1f ms. A lead shorter than %u slots is stretched: "
                "the receiver tells a steady tone from beeps by its first two slot boundaries, where beeps fall "
                "silent.\n\n",
                static_cast<unsigned>(config.vox_lead_ms), static_cast<unsigned>(config.vox_lead_ms),
                static_cast<unsigned>(unlimited::k_min_vox_lead_slots), static_cast<unsigned>(lead),
                static_cast<unsigned>(gap), samples_ms(first_start), static_cast<unsigned>(t.samples),
                samples_ms(t.samples), static_cast<unsigned>(unlimited::k_min_vox_lead_slots));
}

// ---------------------------------------------------------------------------
// 4. Durations
// ---------------------------------------------------------------------------

void print_durations() {
    std::printf("## 4. Airtime (spec 2.4)\n\n");
    std::printf("`duration_samples(n)` / 8000 for n bytes: lead-in + (VOX lead + gap + 10 n) T + max(tail, 2 T), "
                "with no lead-in and the 100 ms tail; the same with a 150 ms VOX lead in brackets.\n\n");
    std::printf("| Speed |");
    for (size_t n = 0; n < sizeof(k_duration_sizes) / sizeof(k_duration_sizes[0]); ++n)
        std::printf(" %u byte%s |", static_cast<unsigned>(k_duration_sizes[n]), k_duration_sizes[n] == 1 ? "" : "s");
    std::printf("\n|---|");
    for (size_t n = 0; n < sizeof(k_duration_sizes) / sizeof(k_duration_sizes[0]); ++n) std::printf("---|");
    std::printf("\n");
    for (size_t i = 0; i < sizeof(k_spec_speeds) / sizeof(k_spec_speeds[0]); ++i) {
        EncoderConfig config = at_speed(k_spec_speeds[i]);
        EncoderConfig vox = config;
        vox.vox_lead_ms = unlimited::k_default_vox_lead_ms;
        const Encoder plain(config);
        const Encoder led(vox);
        std::printf("| %s bytes/s |", speed_text(k_spec_speeds[i]).c_str());
        for (size_t n = 0; n < sizeof(k_duration_sizes) / sizeof(k_duration_sizes[0]); ++n) {
            const double seconds = plain.duration_samples(k_duration_sizes[n]) / static_cast<double>(k_rate_hz);
            const double vox_seconds = led.duration_samples(k_duration_sizes[n]) / static_cast<double>(k_rate_hz);
            std::printf(" %.2f s (%.2f s) |", seconds, vox_seconds);
        }
        std::printf("\n");
    }
    const Encoder six{EncoderConfig()};
    const double windows_s =
        (six.duration_samples(k_spec_duration_bytes) - six.duration_samples(1)) / static_cast<double>(k_rate_hz) +
        10.0 * slot_ms_of(EncoderConfig()) / k_ms_per_s;
    expect(std::fabs(windows_s - k_spec_duration_s) < 0.05,
           format("100 bytes at 6 bytes/s: %.2f s of windows, not %.1f s", windows_s, k_spec_duration_s));
    std::printf("\n100 bytes at 6 bytes/s are %.1f s of windows (spec 2.4).\n\n", windows_s);
}

// ---------------------------------------------------------------------------
// 5. Bandwidth in typical filters
// ---------------------------------------------------------------------------

void print_bandwidth() {
    std::printf("## 5. The signal in typical receiver filters (spec 1.3)\n\n");
    std::printf("Pitch 1500 Hz. Each cell is the shift tolerance `passband_fit(config)` with the filter as "
                "`config.passband`: how far the pitch may move down / up (mistuning) and still pass the filter and be "
                "found by the receiver, whose search never leaves 300..2700 Hz. A signal that does not fit is refused "
                "by `EncoderConfig::check()` (`ConfigError::outside_passband`).\n\n");
    std::printf("| Speed |");
    for (size_t f = 0; f < sizeof(k_filters) / sizeof(k_filters[0]); ++f)
        std::printf(" %s, %u–%u Hz |", k_filters[f].name, k_filters[f].low_hz, k_filters[f].high_hz);
    std::printf("\n|---|");
    for (size_t f = 0; f < sizeof(k_filters) / sizeof(k_filters[0]); ++f) std::printf("---|");
    std::printf("\n");
    for (size_t i = 0; i < sizeof(k_spec_speeds) / sizeof(k_spec_speeds[0]); ++i) {
        std::printf("| %s bytes/s |", speed_text(k_spec_speeds[i]).c_str());
        for (size_t f = 0; f < sizeof(k_filters) / sizeof(k_filters[0]); ++f) {
            EncoderConfig config = at_speed(k_spec_speeds[i]);
            config.passband.low_hz = k_filters[f].low_hz;
            config.passband.high_hz = k_filters[f].high_hz;
            std::printf(" %s Hz |", room_text(unlimited::passband_fit(config)).c_str());
        }
        std::printf("\n");
    }
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// 6. The receiver
// ---------------------------------------------------------------------------

void print_receiver() {
    const DecoderConfig defaults;
    const bool adaptive = defaults.decision_mode == unlimited::DecisionMode::adaptive;
    std::printf("## 6. The receiver's settings (spec 3, 5)\n\n");
    std::printf("`DecoderConfig()`: %s bytes/s, passband %u–%u Hz, %s; the fixed line sits at `threshold_percent` "
                "(%u %% by default) of each window's START–STOP reference line; impulse blanker %s; fade bridge %s "
                "(V16). The receiver searches the pitch in `search_range()`: the passband less half the occupied band "
                "at its speed, within 300..2700 Hz.\n\n",
                speed_text(unlimited::bytes_per_second(defaults.slot_us)).c_str(), defaults.passband.low_hz,
                defaults.passband.high_hz,
                adaptive ? "the adaptive decision line (`--threshold auto`, the default since 2026-09-28)"
                         : "the fixed decision line",
                static_cast<unsigned>(defaults.threshold_percent), defaults.impulse_blanker ? "on" : "off",
                defaults.fade_bridge ? "on" : "off");
    std::printf("One byte, one decision (V20): the first window after the anchor is decided alone, like every "
                "other, and its byte comes out with `locked`; `dcd()` is on while the receiver tracks (spec 3.10).\n\n");
    std::printf("| Speed |");
    for (size_t f = 0; f < sizeof(k_filters) / sizeof(k_filters[0]); ++f) std::printf(" %s |", k_filters[f].name);
    std::printf("\n|---|");
    for (size_t f = 0; f < sizeof(k_filters) / sizeof(k_filters[0]); ++f) std::printf("---|");
    std::printf("\n");
    for (size_t i = 0; i < sizeof(k_spec_speeds) / sizeof(k_spec_speeds[0]); ++i) {
        std::printf("| %s bytes/s |", speed_text(k_spec_speeds[i]).c_str());
        for (size_t f = 0; f < sizeof(k_filters) / sizeof(k_filters[0]); ++f) {
            DecoderConfig receiver;
            receiver.slot_us = unlimited::slot_us_for_speed(k_spec_speeds[i]);
            receiver.passband.low_hz = k_filters[f].low_hz;
            receiver.passband.high_hz = k_filters[f].high_hz;
            const Passband search = receiver.search_range();
            if (receiver.valid()) {
                std::printf(" %u–%u Hz |", search.low_hz, search.high_hz);
            } else {
                std::printf(" no pitch fits |");
            }
        }
        std::printf("\n");
    }
    std::printf("\n");
}

}  // namespace

int main() {
    std::printf("<!-- Generated by `make docs` (tools/doc_examples.cpp). Do not edit: every value is computed by the "
                "library. -->\n\n");
    std::printf("# Unlimited v1.0 — protocol examples\n\n");
    std::printf("Generated by `make docs` from the library code (`tools/doc_examples.cpp`); do not edit by hand. Every "
                "number below comes from the library function named with it, and every example that `spec.md` also "
                "gives is checked against the library: `make docs` stops when they disagree. Section numbers in "
                "parentheses refer to `spec.md`.\n\n");
    print_speeds();
    print_hi();
    print_vox();
    print_durations();
    print_bandwidth();
    print_receiver();
    return 0;
}
