#include "test_harness.hpp"
#include "unlimited/encoder.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSegment;
using unlimited::EncoderStatus;
using unlimited::GridSide;
using unlimited::Preset;
using unlimited::SlotKind;
using unlimited::Spacing;

namespace {

using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using test::count_of;

typedef std::complex<double> Complex;

const double two_pi = 6.28318530717958647692;
const double turn = 4294967296.0;
const uint64_t us_per_s = 1000000;
const uint32_t us_per_ms = 1000;
const uint32_t ms_per_s = 1000;
const uint32_t base_rate = 8000;
const int16_t default_amplitude = 23197;
const unsigned min_tune_slots = 6;
const unsigned eot_slots = 2;
const unsigned header_peaks = 8;
const unsigned bits_per_byte = 8;
const unsigned grid_guard = 5;
const double standard_spacing = 8.0 / 7.0;
const double band_low_hz = 300.0;
const double band_high_hz = 2700.0;
const double band_centre_hz = 1500.0;
const uint16_t fm_tone_hz = 2650;
const size_t queue_capacity = Encoder::k_queue_size;

const Preset all_presets[] = {Preset::fm_fast, Preset::fm, Preset::hf_fast, Preset::hf, Preset::hf_robust,
                              Preset::hf_weak};
const uint32_t test_rates[] = {8000, 11025, 44100, 48000};

// Spec 1.4, in Preset order.
struct PresetMode {
    uint32_t slot_ms;
    uint8_t bits_per_peak;
    uint16_t tone_hz;
    uint16_t tune_ms;
    uint8_t sync_markers;
    uint16_t lead_in_ms;
};
const PresetMode preset_modes[] = {
    {6, 3, 2650, 250, 8, 300}, {8, 3, 2650, 250, 8, 300},  {16, 4, 2192, 250, 8, 0},
    {32, 5, 2132, 250, 8, 0},  {64, 6, 2102, 500, 16, 0}, {128, 7, 2087, 1500, 16, 0},
};

struct Capture {
    std::vector<int16_t> samples;
    std::vector<EncoderStatus> status;  // status before each sample: describes the slot it belongs to
    std::vector<size_t> queue_free;     // queue_free() before each sample
};

struct Slot {
    size_t begin;
    size_t end;
    EncoderStatus status;
};

// What spec 2.1 says each slot of a transmission is.
struct Expected {
    EncoderSegment segment;
    SlotKind kind;
    unsigned slot;
    unsigned symbol;
    unsigned tone;
    unsigned byte;
};

// Renders a whole transmission, refilling the queue as the encoder drains it.
Capture transmit(const EncoderConfig& config, const std::vector<uint8_t>& data) {
    Encoder encoder(config);
    size_t fed = encoder.write(data.data(), data.size());
    REQUIRE(encoder.start());
    Capture capture;
    while (encoder.busy()) {
        if (fed < data.size()) fed += encoder.write(&data[fed], data.size() - fed);
        capture.status.push_back(encoder.status());
        capture.queue_free.push_back(encoder.queue_free());
        capture.samples.push_back(encoder.next_sample());
    }
    return capture;
}

std::vector<Slot> slots_of(const Capture& capture) {
    std::vector<Slot> slots;
    for (size_t n = 0; n < capture.status.size(); ++n) {
        const EncoderStatus& s = capture.status[n];
        if (slots.empty() || s.segment != capture.status[n - 1].segment ||
            s.slot_index != capture.status[n - 1].slot_index) {
            const Slot slot = {n, n, s};
            slots.push_back(slot);
        }
        slots.back().end = n + 1;
    }
    return slots;
}

// The n whose Gray code is `code` (spec 1.5: symbol s goes on the de-rotated tone gray^-1(s)).
unsigned reference_gray_inverse(unsigned code) {
    unsigned value = 0;
    for (unsigned bits = code; bits != 0; bits >>= 1) value ^= bits;
    return value;
}

uint32_t expected_tune_slots(const EncoderConfig& config) {
    const uint32_t tune_us = config.tune_ms * us_per_ms;
    return std::max<uint32_t>((tune_us + config.slot_us - 1) / config.slot_us, min_tune_slots);
}

unsigned frame_bytes_of(const EncoderConfig& config) {
    return unsigned(config.data_slots) * config.bits_per_peak / bits_per_byte;
}

unsigned peaks_for(size_t bytes, unsigned bits_per_peak) {
    return static_cast<unsigned>((bytes * bits_per_byte + bits_per_peak - 1) / bits_per_peak);
}

// k-bit symbol `slot` of a frame, MSB first, zero bits past the frame's bytes.
unsigned frame_symbol(const std::vector<uint8_t>& bytes, unsigned slot, unsigned k) {
    unsigned value = 0;
    for (unsigned b = 0; b < k; ++b) {
        const unsigned bit = slot * k + b;
        const unsigned byte = bit / bits_per_byte;
        const unsigned set = byte < bytes.size() ? (bytes[byte] >> (bits_per_byte - 1 - bit % bits_per_byte)) & 1u : 0u;
        value = (value << 1) | set;
    }
    return value;
}

std::vector<Expected> expected_slots(const EncoderConfig& config, const std::vector<uint8_t>& data) {
    std::vector<Expected> slots;
    const Expected silent = {EncoderSegment::lead_in, SlotKind::silent, 0, 0, 0, 0};
    const uint32_t lead_us = config.lead_in_ms * us_per_ms;
    slots.insert(slots.end(), (lead_us + config.slot_us - 1) / config.slot_us, silent);
    const Expected tone = {EncoderSegment::tune, SlotKind::tone, 0, 0, 0, 0};
    slots.insert(slots.end(), expected_tune_slots(config), tone);
    const Expected sync = {EncoderSegment::sync, SlotKind::marker, 0, 0, 0, 0};
    slots.insert(slots.end(), config.sync_markers, sync);
    const uint16_t word =
        unlimited::header_word(config.bits_per_peak, config.data_slots, config.slot_us, config.spacing);
    for (unsigned j = 0; j < header_peaks; ++j) {
        const unsigned h = unlimited::header_symbol(word, static_cast<uint8_t>(j));
        const Expected peak = {EncoderSegment::header, SlotKind::peak, j, h, h, 0};
        slots.push_back(peak);
    }
    const Expected header_stop = {EncoderSegment::header, SlotKind::marker, header_peaks, 0, 0, 0};
    slots.push_back(header_stop);
    const unsigned k = config.bits_per_peak;
    const unsigned tones = 1u << k;
    const unsigned rotation = (tones / 8) | 1u;
    const unsigned full = frame_bytes_of(config);
    for (size_t position = 0; position < data.size();) {
        const size_t queued = data.size() - position;
        const size_t take = std::min<size_t>(queued, full);
        const unsigned peaks = queued >= full ? config.data_slots : peaks_for(queued, k);
        const std::vector<uint8_t> frame(data.begin() + position, data.begin() + position + take);
        for (unsigned i = 0; i < peaks; ++i) {
            const unsigned symbol = frame_symbol(frame, i, k);
            const Expected peak = {EncoderSegment::frame, SlotKind::peak, i, symbol,
                                   (reference_gray_inverse(symbol) + i * rotation) % tones, frame[0]};
            slots.push_back(peak);
        }
        const Expected stop = {EncoderSegment::frame, SlotKind::marker, peaks, 0, 0, frame[0]};
        slots.push_back(stop);
        position += take;
        if (peaks < config.data_slots) break;
    }
    const Expected eot = {EncoderSegment::eot, SlotKind::marker, 0, 0, 0, 0};
    slots.insert(slots.end(), eot_slots, eot);
    if (config.tail_ms != 0) {
        const Expected tail = {EncoderSegment::tail, SlotKind::silent, 0, 0, 0, 0};
        slots.push_back(tail);
    }
    return slots;
}

// Spec 2.1: lead + (N_tune + N_sync + 9 + D + ceil(D / N) + 2) T + tail, D = ceil(8 n / k).
double formula_samples(const EncoderConfig& config, size_t bytes) {
    const double rate = config.sample_rate_hz;
    const unsigned peaks = peaks_for(bytes, config.bits_per_peak);
    const unsigned frames = (peaks + config.data_slots - 1) / config.data_slots;
    const double slots =
        expected_tune_slots(config) + config.sync_markers + header_peaks + 1 + peaks + frames + eot_slots;
    return config.lead_in_ms * rate / ms_per_s + slots * config.slot_us * rate / us_per_s +
           config.tail_ms * rate / ms_per_s;
}

size_t duration_of(const EncoderConfig& config, size_t bytes) {
    return Encoder(config).duration_samples(bytes);
}

size_t rendered_length(const EncoderConfig& config, const std::vector<uint8_t>& data, size_t chunk) {
    Encoder encoder(config);
    size_t fed = encoder.write(data.data(), data.size());
    REQUIRE(encoder.start());
    std::vector<int16_t> buffer(chunk);
    size_t total = 0;
    for (;;) {
        if (fed < data.size()) fed += encoder.write(&data[fed], data.size() - fed);
        const size_t got = encoder.render(buffer.data(), buffer.size());
        if (got == 0) break;
        total += got;
    }
    CHECK(!encoder.busy());
    return total;
}

std::vector<uint8_t> test_bytes(size_t count, uint32_t seed = 12345) {
    std::vector<uint8_t> bytes;
    uint32_t state = seed;
    for (size_t i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        bytes.push_back(static_cast<uint8_t>(state >> 24));
    }
    return bytes;
}

double slot_seconds(const EncoderConfig& config) {
    return config.slot_us / double(us_per_s);
}

// Exact grid frequency of a peak: f_ref -+ (5 + n c) / T; the header always uses c = 8/7.
double peak_frequency(const EncoderConfig& config, const EncoderStatus& status) {
    const bool dense = status.segment == EncoderSegment::frame && config.spacing == Spacing::dense;
    const double offset = (grid_guard + status.tone * (dense ? 1.0 : standard_spacing)) / slot_seconds(config);
    return config.side == GridSide::above ? config.tone_hz + offset : config.tone_hz - offset;
}

// Span W = max(G + (M - 1) c, G + 8) / T (spec 1.3).
double span_hz(const EncoderConfig& config) {
    const double c = config.spacing == Spacing::dense ? 1.0 : standard_spacing;
    const double data = grid_guard + ((1u << config.bits_per_peak) - 1) * c;
    const double header = grid_guard + (header_peaks - 1) * standard_spacing;
    return std::max(data, header) / slot_seconds(config);
}

// The encoder's f_ref NCO without the marker sign flips.
double reference_phase(const EncoderConfig& config, size_t n) {
    const uint64_t step = ((static_cast<uint64_t>(config.tone_hz) << 32) + config.sample_rate_hz / 2) /
                          config.sample_rate_hz;
    return two_pi * static_cast<double>(static_cast<uint32_t>(step * n)) / turn;
}

double project_on_tone(const EncoderConfig& config, const Capture& capture, size_t begin, size_t end) {
    double sum = 0.0;
    for (size_t n = begin; n < end; ++n) sum += capture.samples[n] * std::sin(reference_phase(config, n));
    return sum;
}

Complex mix_down(const EncoderConfig& config, const Capture& capture, size_t begin, size_t end) {
    Complex sum(0.0, 0.0);
    for (size_t n = begin; n < end; ++n)
        sum += double(capture.samples[n]) * std::polar(1.0, -reference_phase(config, n));
    return sum;
}

Complex mix_at(const Capture& capture, size_t begin, size_t end, double frequency, double rate) {
    Complex sum(0.0, 0.0);
    for (size_t n = begin; n < end; ++n)
        sum += double(capture.samples[n]) * std::polar(1.0, -two_pi * frequency * double(n - begin) / rate);
    return sum;
}

// Energy of the least-squares fit a cos + b sin at f: maximal at the tone's true frequency, without the
// bias a one-sided (complex) correlation gets from the negative-frequency image.
double fitted_energy(const Capture& capture, size_t begin, size_t end, double frequency, double rate) {
    double cc = 0.0, ss = 0.0, cs = 0.0, yc = 0.0, ys = 0.0;
    for (size_t n = begin; n < end; ++n) {
        const double phase = two_pi * frequency * double(n - begin) / rate;
        const double c = std::cos(phase);
        const double s = std::sin(phase);
        const double y = capture.samples[n];
        cc += c * c;
        ss += s * s;
        cs += c * s;
        yc += y * c;
        ys += y * s;
    }
    return (ss * yc * yc - 2.0 * cs * yc * ys + cc * ys * ys) / (cc * ss - cs * cs);
}

// Golden-section search of the fitted energy over f0 +- half_width.
double estimate_frequency(const Capture& capture, size_t begin, size_t end, double f0, double rate) {
    const double half_width = 0.5;
    const double resolution = 1e-6;
    const double ratio = 0.61803398874989484820;
    double low = f0 - half_width;
    double high = f0 + half_width;
    double a = high - ratio * (high - low);
    double b = low + ratio * (high - low);
    double fa = fitted_energy(capture, begin, end, a, rate);
    double fb = fitted_energy(capture, begin, end, b, rate);
    while (high - low > resolution) {
        if (fa > fb) {
            high = b;
            b = a;
            fb = fa;
            a = high - ratio * (high - low);
            fa = fitted_energy(capture, begin, end, a, rate);
        } else {
            low = a;
            a = b;
            fa = fb;
            b = low + ratio * (high - low);
            fb = fitted_energy(capture, begin, end, b, rate);
        }
    }
    return 0.5 * (low + high);
}

// -40 dB width of a spectrum sampled on a uniform grid of step_hz.
double width_40db(const std::vector<double>& power, double step_hz) {
    const double level = 1e-4;
    const double peak = *std::max_element(power.begin(), power.end());
    size_t low = 0;
    while (power[low] < peak * level) ++low;
    size_t high = power.size() - 1;
    while (power[high] < peak * level) --high;
    CHECK(low > 0 && high + 1 < power.size());
    return (high - low) * step_hz;
}

std::vector<double> spectrum(const std::vector<double>& x, double rate, double from_hz, double to_hz, double step_hz) {
    std::vector<double> power;
    for (double f = from_hz; f <= to_hz; f += step_hz) {
        Complex sum(0.0, 0.0);
        const Complex rotation = std::polar(1.0, -two_pi * f / rate);
        Complex phasor(1.0, 0.0);
        for (size_t n = 0; n < x.size(); ++n) {
            sum += x[n] * phasor;
            phasor *= rotation;
        }
        power.push_back(std::norm(sum));
    }
    return power;
}

std::vector<double> audio_of(const Capture& capture, const Slot& slot) {
    return std::vector<double>(capture.samples.begin() + slot.begin, capture.samples.begin() + slot.end);
}

const Slot* find_slot(const std::vector<Slot>& slots, EncoderSegment segment, SlotKind kind, size_t skip) {
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].status.segment != segment || slots[i].status.kind != kind) continue;
        if (skip == 0) return &slots[i];
        --skip;
    }
    return nullptr;
}

// f_ref 2650 Hz, grid below, N = 8: the largest k whose band fits at this T.
EncoderConfig largest_mode(uint32_t slot_ms, uint32_t rate) {
    EncoderConfig config = EncoderConfig::from_preset(Preset::fm, rate);
    config.slot_us = slot_ms * us_per_ms;
    config.lead_in_ms = 0;
    for (uint8_t k = unlimited::k_max_bits_per_peak; k >= 1; --k) {
        config.bits_per_peak = k;
        if (config.valid()) break;
    }
    return config;
}

EncoderConfig dense_k8_mode(uint32_t rate) {
    EncoderConfig config = EncoderConfig::from_preset(Preset::hf_weak, rate);
    config.bits_per_peak = 8;
    config.spacing = Spacing::dense;
    config.tone_hz = 2516;  // ceil(1500 + 2031.25 / 2)
    return config;
}

EncoderConfig above_mode(uint32_t rate) {
    EncoderConfig config = EncoderConfig::from_preset(Preset::hf, rate);
    config.side = GridSide::above;
    config.tone_hz = 868;  // floor(1500 - 1263.4 / 2)
    return config;
}

// Extra modes beyond the presets: N = 16 and 32, dense spacing, grid above.
std::vector<EncoderConfig> extra_modes(uint32_t rate) {
    std::vector<EncoderConfig> modes;
    EncoderConfig c = EncoderConfig::from_preset(Preset::hf, rate);
    c.data_slots = 16;
    modes.push_back(c);
    c = EncoderConfig::from_preset(Preset::hf_fast, rate);
    c.data_slots = 32;
    modes.push_back(c);
    c = EncoderConfig::from_preset(Preset::hf, rate);
    c.bits_per_peak = 6;
    c.spacing = Spacing::dense;
    c.tone_hz = 2563;  // ceil(1500 + 2125 / 2); dense needs T >= 32 ms (G2)
    modes.push_back(c);
    modes.push_back(dense_k8_mode(rate));
    modes.push_back(above_mode(rate));
    return modes;
}

}  // namespace

TEST(encoder_config_defaults_and_presets) {
    const EncoderConfig config;
    CHECK_EQ(config.sample_rate_hz, base_rate);
    CHECK_EQ(config.tone_hz, 2132);
    CHECK_EQ(config.slot_us, 32000u);
    CHECK_EQ(unsigned(config.bits_per_peak), 5u);
    CHECK_EQ(unsigned(config.data_slots), 8u);
    CHECK(config.spacing == Spacing::standard);
    CHECK(config.side == GridSide::below);
    CHECK_EQ(config.amplitude, default_amplitude);
    CHECK_EQ(config.lead_in_ms, 0);
    CHECK_EQ(config.tune_ms, 250);
    CHECK_EQ(config.sync_markers, 8);
    CHECK_EQ(config.tail_ms, 100);
    CHECK_EQ(unsigned(config.frame_bytes()), 5u);
    CHECK(config.valid());

    for (size_t p = 0; p < count_of(all_presets); ++p) {
        const PresetMode& mode = preset_modes[p];
        for (size_t r = 0; r < count_of(test_rates); ++r) {
            const EncoderConfig preset = EncoderConfig::from_preset(all_presets[p], test_rates[r]);
            CHECK_EQ(preset.sample_rate_hz, test_rates[r]);
            CHECK_EQ(preset.slot_us, mode.slot_ms * us_per_ms);
            CHECK_EQ(unsigned(preset.bits_per_peak), unsigned(mode.bits_per_peak));
            CHECK_EQ(unsigned(preset.data_slots), 8u);
            CHECK(preset.spacing == Spacing::standard);
            CHECK(preset.side == GridSide::below);
            CHECK_EQ(preset.tone_hz, mode.tone_hz);
            CHECK_EQ(preset.tune_ms, mode.tune_ms);
            CHECK_EQ(unsigned(preset.sync_markers), unsigned(mode.sync_markers));
            CHECK_EQ(preset.lead_in_ms, mode.lead_in_ms);
            CHECK_EQ(preset.amplitude, default_amplitude);
            CHECK_EQ(preset.tail_ms, 100);
            CHECK_EQ(unsigned(preset.frame_bytes()), unsigned(mode.bits_per_peak));
            CHECK(preset.valid());
        }
        // HF: f_ref = ceil(1500 + W / 2), so the band is centred on 1500 Hz; FM: f_ref 2650 Hz.
        const EncoderConfig preset = EncoderConfig::from_preset(all_presets[p], base_rate);
        const double span = span_hz(preset);
        const bool fm = all_presets[p] == Preset::fm_fast || all_presets[p] == Preset::fm;
        const double expected = fm ? fm_tone_hz : std::ceil(band_centre_hz + span / 2.0);
        CHECK_EQ(double(preset.tone_hz), expected);
        CHECK(preset.tone_hz - span >= band_low_hz);
        NOTE("preset %u: f_ref %u Hz, band %.1f..%u Hz (span %.0f Hz)", unsigned(p), preset.tone_hz,
             preset.tone_hz - span, preset.tone_hz, span);
    }
}

TEST(encoder_config_valid_rejects_out_of_range) {
    struct Case {
        const char* name;
        EncoderConfig config;
        bool valid;
    };
    std::vector<Case> cases;
    const EncoderConfig hf;
    EncoderConfig c = hf;
    c.sample_rate_hz = 7999;
    cases.push_back(Case{"rate_low", c, false});
    c = hf;
    c.sample_rate_hz = 192001;
    cases.push_back(Case{"rate_high", c, false});
    c = hf;
    c.tone_hz = 299;
    cases.push_back(Case{"tone_low", c, false});
    c = hf;
    c.tone_hz = 2701;
    cases.push_back(Case{"tone_high", c, false});
    c = EncoderConfig::from_preset(Preset::fm_fast, base_rate);
    c.slot_us = 5000;
    cases.push_back(Case{"slot_5ms", c, false});
    c.slot_us = 5999;
    cases.push_back(Case{"slot_short", c, false});
    c = EncoderConfig::from_preset(Preset::hf_weak, base_rate);
    c.slot_us = 128001;
    cases.push_back(Case{"slot_long", c, false});
    c = hf;
    c.slot_us = 32500;
    cases.push_back(Case{"slot_not_whole_ms", c, false});
    c.slot_us = 32001;
    cases.push_back(Case{"slot_not_whole_ms_2", c, false});
    c = hf;
    c.bits_per_peak = 0;
    cases.push_back(Case{"k_zero", c, false});
    c.bits_per_peak = 9;
    cases.push_back(Case{"k_nine", c, false});
    const uint8_t bad_slots[] = {0, 7, 9, 12, 24, 31, 33, 64, 255};
    for (size_t i = 0; i < count_of(bad_slots); ++i) {
        c = hf;
        c.data_slots = bad_slots[i];
        cases.push_back(Case{"bad_n", c, false});
    }
    c = hf;
    c.spacing = static_cast<Spacing>(2);
    cases.push_back(Case{"bad_spacing", c, false});
    c = hf;
    c.side = static_cast<GridSide>(2);
    cases.push_back(Case{"bad_side", c, false});
    // (N + 1) T <= 1152 ms.
    c = EncoderConfig::from_preset(Preset::hf_robust, base_rate);
    c.data_slots = 16;
    c.slot_us = 67000;
    cases.push_back(Case{"n16_t67", c, true});
    c.slot_us = 68000;
    cases.push_back(Case{"n16_t68", c, false});
    c = EncoderConfig::from_preset(Preset::hf, base_rate);
    c.data_slots = 32;
    c.bits_per_peak = 4;
    c.slot_us = 34000;
    cases.push_back(Case{"n32_t34", c, true});
    c.slot_us = 35000;
    cases.push_back(Case{"n32_t35", c, false});
    // Band: at T 13 ms, k 3, the farthest tone is exactly 1000 Hz from f_ref (data and header alike).
    c = EncoderConfig::from_preset(Preset::fm, base_rate);
    c.slot_us = 13000;
    c.tone_hz = 1300;
    cases.push_back(Case{"band_below_edge", c, true});
    c.tone_hz = 1299;
    cases.push_back(Case{"band_below_out", c, false});
    c.side = GridSide::above;
    c.tone_hz = 1700;
    cases.push_back(Case{"band_above_edge", c, true});
    c.tone_hz = 1701;
    cases.push_back(Case{"band_above_out", c, false});
    // Header-limited: k 1 at T 6 ms spans G + 8 = 13 / T = 2166.7 Hz.
    c = EncoderConfig::from_preset(Preset::fm_fast, base_rate);
    c.bits_per_peak = 1;
    c.tone_hz = 2467;
    cases.push_back(Case{"header_band_edge", c, true});
    c.tone_hz = 2466;
    cases.push_back(Case{"header_band_out", c, false});
    // Dense k 8 at T 128 ms spans (5 + 255) / T = 2031.25 Hz.
    c = dense_k8_mode(base_rate);
    c.tone_hz = 2332;
    cases.push_back(Case{"dense_band_edge", c, true});
    c.tone_hz = 2331;
    cases.push_back(Case{"dense_band_out", c, false});
    c.spacing = Spacing::standard;
    c.tone_hz = 2700;
    cases.push_back(Case{"standard_k8_t128", c, true});
    c.slot_us = 123000;
    cases.push_back(Case{"standard_k8_t123", c, false});
    // G2: dense spacing needs T >= 32 ms, even where the band fits (k 5 dense at T 31 ms spans 1161 Hz).
    c = hf;
    c.spacing = Spacing::dense;
    cases.push_back(Case{"dense_t32", c, true});
    c.slot_us = 31000;
    cases.push_back(Case{"dense_t31", c, false});
    c.spacing = Spacing::standard;
    cases.push_back(Case{"standard_t31", c, true});
    c = EncoderConfig::from_preset(Preset::fm_fast, base_rate);
    c.bits_per_peak = 1;
    c.spacing = Spacing::dense;
    c.tone_hz = 2467;
    cases.push_back(Case{"dense_t6_band_fits", c, false});
    c = hf;
    c.side = GridSide::above;
    cases.push_back(Case{"hf_preset_above_out", c, false});
    c = hf;
    c.sync_markers = 7;
    cases.push_back(Case{"sync_few", c, false});
    c.sync_markers = 33;
    cases.push_back(Case{"sync_many", c, false});
    c = hf;
    c.amplitude = 0;
    cases.push_back(Case{"amplitude_zero", c, false});
    c.amplitude = -1;
    cases.push_back(Case{"amplitude_negative", c, false});
    c = EncoderConfig::from_preset(Preset::fm_fast, 192000);
    c.sync_markers = 32;
    c.tone_hz = 2700;
    cases.push_back(Case{"limits_accepted", c, true});
    for (size_t i = 0; i < cases.size(); ++i) {
        if (!CHECK_EQ(cases[i].config.valid(), cases[i].valid)) NOTE("case %s", cases[i].name);
        // Whatever valid() says, the frame never outgrows half the queue for any valid k and N.
        if (cases[i].valid) CHECK(cases[i].config.frame_bytes() <= Encoder::k_queue_size / 2);
    }
    // With the 64-byte queue the queue rule never binds: the band and frame rules already cap frame_bytes().
    unsigned largest = 0;
    const uint8_t slot_counts[] = {8, 16, 32};
    for (size_t n = 0; n < count_of(slot_counts); ++n) {
        for (uint8_t k = 1; k <= unlimited::k_max_bits_per_peak; ++k) {
            EncoderConfig config = hf;
            config.tone_hz = static_cast<uint16_t>(band_high_hz);
            config.bits_per_peak = k;
            config.data_slots = slot_counts[n];
            for (uint32_t ms = 6; ms <= 128; ++ms) {
                config.slot_us = ms * us_per_ms;
                if (config.valid()) largest = std::max<unsigned>(largest, config.frame_bytes());
            }
        }
    }
    NOTE("largest frame of any valid mode: %u bytes (queue rule: <= %u)", largest, Encoder::k_queue_size / 2u);
    CHECK(largest <= Encoder::k_queue_size / 2u);
}

// check() names the first rule broken; valid() is check() == none.
TEST(encoder_config_check_names_the_rule) {
    using unlimited::ConfigError;
    struct Case {
        EncoderConfig config;
        ConfigError error;
    };
    std::vector<Case> cases;
    const EncoderConfig hf;
    cases.push_back(Case{hf, ConfigError::none});
    EncoderConfig c = hf;
    c.sample_rate_hz = unlimited::k_max_sample_rate_hz + 1;
    cases.push_back(Case{c, ConfigError::sample_rate});
    c = hf;
    c.tone_hz = unlimited::k_max_tone_hz + 1;
    cases.push_back(Case{c, ConfigError::tone});
    c = hf;
    c.slot_us = 32500;
    cases.push_back(Case{c, ConfigError::slot});
    c = hf;
    c.bits_per_peak = 0;
    cases.push_back(Case{c, ConfigError::bits_per_peak});
    c = hf;
    c.data_slots = 12;
    cases.push_back(Case{c, ConfigError::data_slots});
    c = hf;
    c.data_slots = 32;
    c.slot_us = 35000;
    cases.push_back(Case{c, ConfigError::frame_length});
    c = hf;
    c.spacing = static_cast<Spacing>(2);
    cases.push_back(Case{c, ConfigError::spacing});
    c = hf;
    c.spacing = Spacing::dense;
    c.slot_us = unlimited::k_min_dense_slot_us - us_per_ms;
    cases.push_back(Case{c, ConfigError::dense_slot});
    c = hf;
    c.side = static_cast<GridSide>(2);
    cases.push_back(Case{c, ConfigError::side});
    c = hf;
    c.side = GridSide::above;
    cases.push_back(Case{c, ConfigError::band});
    c = hf;
    c.sync_markers = unlimited::k_min_sync_markers - 1;
    cases.push_back(Case{c, ConfigError::sync_markers});
    c.sync_markers = unlimited::k_max_sync_markers + 1;
    cases.push_back(Case{c, ConfigError::sync_markers});
    c = hf;
    c.amplitude = 0;
    cases.push_back(Case{c, ConfigError::amplitude});
    for (size_t i = 0; i < cases.size(); ++i) {
        if (!CHECK(cases[i].config.check() == cases[i].error)) NOTE("case %zu", i);
        CHECK_EQ(cases[i].config.valid(), cases[i].error == ConfigError::none);
    }
}

TEST(encoder_start_rules) {
    Encoder encoder{EncoderConfig()};
    CHECK(!encoder.busy());
    CHECK_EQ(encoder.next_sample(), 0);
    CHECK(!encoder.start());  // empty queue
    CHECK(encoder.write(0x42));
    CHECK(encoder.start());
    CHECK(encoder.busy());
    CHECK(!encoder.start());  // busy

    EncoderConfig bad;
    bad.sync_markers = 0;
    Encoder invalid(bad);
    CHECK(invalid.write(0x42));
    CHECK(!invalid.start());
    CHECK_EQ(invalid.duration_samples(1), 0u);
}

// The queue holds exactly k_queue_size bytes (free-running indices): with frame_bytes() = k_queue_size / 2 a producer
// still keeps one whole frame queued behind the one being sent (it held one byte less, and a 16-byte queue with
// 8-byte frames split a stream into a transmission per 15 bytes).
TEST(encoder_queue_capacity) {
    // U15': the AVR size gate (96 B plus the queue) is a static_assert in encoder.cpp, compiled by check_embedded.
    NOTE("sizeof(Encoder) = %zu B on this host, queue %u B", sizeof(Encoder), unsigned(Encoder::k_queue_size));
    Encoder encoder{EncoderConfig()};
    CHECK_EQ(queue_capacity, size_t(Encoder::k_queue_size));
    CHECK_EQ(encoder.queue_free(), queue_capacity);
    CHECK_EQ(encoder.queued(), 0u);
    const std::vector<uint8_t> data = test_bytes(Encoder::k_queue_size * 2);
    CHECK_EQ(encoder.write(data.data(), data.size()), queue_capacity);
    CHECK_EQ(encoder.queue_free(), 0u);
    CHECK_EQ(encoder.queued(), queue_capacity);
    CHECK(!encoder.write(0x00));
    encoder.abort();
    CHECK_EQ(encoder.queue_free(), queue_capacity);
    CHECK_EQ(encoder.queued(), 0u);

    // The indices wrap (mod 256) many times over a long stream: the bytes come out in order and the count holds.
    const EncoderConfig config;
    Encoder streaming(config);
    const std::vector<uint8_t> stream = test_bytes(20 * Encoder::k_queue_size, 7);
    size_t fed = streaming.write(stream.data(), stream.size());
    CHECK_EQ(fed, queue_capacity);
    REQUIRE(streaming.start());
    size_t frames = 0;
    uint32_t last_slot = 0xFFFFFFFFu;
    while (streaming.busy()) {
        if (fed < stream.size()) fed += streaming.write(&stream[fed], stream.size() - fed);
        CHECK(streaming.queued() + streaming.queue_free() == queue_capacity);
        const EncoderStatus status = streaming.status();
        if (status.segment == EncoderSegment::frame && status.slot_index != last_slot && status.slot == 0) {
            if (!CHECK_EQ(unsigned(status.byte), unsigned(stream[frames * frame_bytes_of(config)]))) break;
            ++frames;
        }
        last_slot = status.slot_index;
        streaming.next_sample();
    }
    CHECK_EQ(frames, stream.size() / frame_bytes_of(config));
}

// U4', U25: presets and N = 16 / 32, dense, above, short final frames, at four rates.
TEST(encoder_total_length_modes_frames_and_rates) {
    double worst = 0.0;
    size_t runs = 0;
    for (size_t r = 0; r < count_of(test_rates); ++r) {
        std::vector<EncoderConfig> modes;
        for (size_t p = 0; p < count_of(all_presets); ++p)
            modes.push_back(EncoderConfig::from_preset(all_presets[p], test_rates[r]));
        const std::vector<EncoderConfig> extra = extra_modes(test_rates[r]);
        modes.insert(modes.end(), extra.begin(), extra.end());
        for (size_t m = 0; m < modes.size(); ++m) {
            const EncoderConfig& config = modes[m];
            REQUIRE(config.valid());
            const size_t full = frame_bytes_of(config);
            const size_t counts[] = {1, full - 1, full, full + 1, 3 * full - 1, 3 * full};
            for (size_t c = 0; c < count_of(counts); ++c) {
                if (counts[c] == 0) continue;
                const std::vector<uint8_t> data = test_bytes(counts[c]);
                const size_t length = rendered_length(config, data, 4096);
                CHECK_EQ(length, (duration_of(config, data.size())));
                const double error = std::fabs(length - formula_samples(config, data.size()));
                worst = std::max(worst, error);
                ++runs;
                if (!CHECK(error <= 1.0))
                    NOTE("rate %u mode %u, %zu bytes: %zu samples", test_rates[r], unsigned(m), counts[c], length);
            }
        }
    }
    NOTE("%zu transmissions: max |length - formula| %.3f samples", runs, worst);
}

TEST(encoder_total_length_odd_speeds_and_lead_in) {
    const uint32_t slot_ms[] = {6, 7, 12, 20, 37, 100, 127, 128};
    const uint16_t lead_ms[] = {0, 1, 300, 1234};
    const uint16_t tail_ms[] = {0, 1, 100};
    double worst = 0.0;
    for (size_t r = 0; r < count_of(test_rates); ++r) {
        for (size_t s = 0; s < count_of(slot_ms); ++s) {
            for (size_t l = 0; l < count_of(lead_ms); ++l) {
                EncoderConfig config = largest_mode(slot_ms[s], test_rates[r]);
                REQUIRE(config.valid());
                config.lead_in_ms = lead_ms[l];
                config.tail_ms = tail_ms[(s + l) % count_of(tail_ms)];
                config.tune_ms = static_cast<uint16_t>(100 * s);
                const std::vector<uint8_t> data = test_bytes(2 + s);
                const size_t length = rendered_length(config, data, 1000);
                CHECK_EQ(length, (duration_of(config, data.size())));
                const double error = std::fabs(length - formula_samples(config, data.size()));
                worst = std::max(worst, error);
                CHECK(error <= 1.0);
            }
        }
    }
    NOTE("max |length - formula|: %.3f samples", worst);
}

TEST(encoder_slot_drift_over_ten_thousand_slots) {
    struct Case {
        Preset preset;
        uint32_t rate;
        uint32_t slot_ms;
        uint16_t lead_ms;
    };
    const Case cases[] = {{Preset::hf, 44100, 32, 300},
                          {Preset::hf, 11025, 37, 300},
                          {Preset::hf_weak, 48000, 128, 0},
                          {Preset::fm_fast, 8000, 6, 7}};
    const uint32_t slots_wanted = 10000;
    for (size_t c = 0; c < count_of(cases); ++c) {
        EncoderConfig config = EncoderConfig::from_preset(cases[c].preset, cases[c].rate);
        config.slot_us = cases[c].slot_ms * us_per_ms;
        config.lead_in_ms = cases[c].lead_ms;
        REQUIRE(config.valid());
        const std::vector<uint8_t> data = test_bytes(slots_wanted * config.bits_per_peak / bits_per_byte + 1);
        Encoder encoder(config);
        size_t fed = encoder.write(data.data(), data.size());
        REQUIRE(encoder.start());
        // Slot j starts at exact sample (j * slot_us - offset_us) * rate / 10^6, offset = lead padding to a slot.
        const uint32_t lead_us = config.lead_in_ms * us_per_ms;
        const uint32_t lead_slots = (lead_us + config.slot_us - 1) / config.slot_us;
        const uint64_t offset_us = static_cast<uint64_t>(lead_slots) * config.slot_us - lead_us;
        uint32_t slot = 0;
        double worst = 0.0;
        size_t mismatches = 0;
        for (size_t n = 0; slot < slots_wanted && encoder.busy(); ++n) {
            if (fed < data.size()) fed += encoder.write(&data[fed], data.size() - fed);
            const uint32_t index = encoder.status().slot_index;
            if (index != slot) {
                slot = index;
                const uint64_t numerator =
                    (static_cast<uint64_t>(slot) * config.slot_us - offset_us) * config.sample_rate_hz;
                const double exact = static_cast<double>(numerator) / us_per_s;
                worst = std::max(worst, std::fabs(n - exact));
                if (n != (numerator + us_per_s - 1) / us_per_s) ++mismatches;
            }
            encoder.next_sample();
        }
        CHECK_EQ(slot, slots_wanted);
        CHECK_EQ(mismatches, 0u);
        CHECK(worst < 1.0);
        NOTE("rate %u, T %u ms: %u slots, max |start - exact| %.4f samples", cases[c].rate, cases[c].slot_ms, slot,
             worst);
    }
}

TEST(encoder_segments_and_status) {
    EncoderConfig config;
    config.lead_in_ms = 100;  // 3.125 slots: a partial slot, then 3 whole ones
    const std::vector<uint8_t> data = test_bytes(7);  // one full frame (5 bytes) and a short one (2 bytes)
    const Capture capture = transmit(config, data);
    const std::vector<Slot> slots = slots_of(capture);
    const std::vector<Expected> expected = expected_slots(config, data);
    REQUIRE(slots.size() == expected.size());
    unsigned peaks = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        const EncoderStatus& s = slots[i].status;
        const Expected& e = expected[i];
        const bool ok = CHECK(s.segment == e.segment) && CHECK(s.kind == e.kind) &&
                        CHECK_EQ(unsigned(s.slot), e.slot) && CHECK_EQ(unsigned(s.symbol), e.symbol) &&
                        CHECK_EQ(unsigned(s.tone), e.tone) && CHECK_EQ(unsigned(s.byte), e.byte);
        if (!ok) NOTE("slot %zu", i);
        if (s.kind == SlotKind::peak) ++peaks;
        if (i > 0 && s.segment != EncoderSegment::tail) CHECK_EQ(capture.status[slots[i].begin].slot_index, i);
        // Status is constant over a slot.
        for (size_t n = slots[i].begin; n < slots[i].end; ++n) {
            if (capture.status[n].slot != s.slot || capture.status[n].symbol != s.symbol ||
                capture.status[n].tone != s.tone || capture.status[n].kind != s.kind) {
                CHECK(false);
                break;
            }
        }
    }
    CHECK_EQ(peaks, header_peaks + 8 + 4);  // header, 8 peaks, ceil(16 / 5) = 4 peaks
    for (size_t n = 0; n < capture.status.size(); ++n)
        if (!CHECK_EQ(capture.status[n].samples_rendered, n)) break;
    // The partial first lead-in slot lasts (1 - 0.125) of a slot: 100 ms = 3.125 slots of 256 samples.
    const unsigned lead_slots = 4;
    CHECK_EQ(slots[0].end - slots[0].begin, 32u);
    CHECK_EQ(slots[lead_slots].begin, 800u);
    CHECK_EQ(capture.samples.size(), (duration_of(config, data.size())));
    for (size_t n = slots[0].begin; n < slots[lead_slots].begin; ++n) CHECK_EQ(capture.samples[n], 0);

    // The bytes of a frame stay queued until its STOP ends.
    const Slot* first_stop = find_slot(slots, EncoderSegment::frame, SlotKind::marker, 0);
    REQUIRE(first_stop != nullptr);
    CHECK_EQ(capture.queue_free[first_stop->end - 1], queue_capacity - data.size());
    CHECK_EQ(capture.queue_free[first_stop->end], queue_capacity - 2);
    CHECK_EQ(capture.queue_free.back(), queue_capacity);
}

// U24 and the encoder half of L15: every (k, N) with a valid T, full and short frames.
TEST(encoder_packing_all_modes) {
    const uint8_t slot_counts[] = {8, 16, 32};
    size_t modes = 0;
    size_t transmissions = 0;
    for (size_t n = 0; n < count_of(slot_counts); ++n) {
        for (uint8_t k = 1; k <= unlimited::k_max_bits_per_peak; ++k) {
            EncoderConfig config = EncoderConfig::from_preset(Preset::hf, base_rate);
            config.bits_per_peak = k;
            config.data_slots = slot_counts[n];
            config.tone_hz = static_cast<uint16_t>(band_high_hz);
            config.tune_ms = 0;
            config.tail_ms = 0;
            bool found = false;
            for (uint32_t ms = 6; ms <= 128 && !found; ++ms) {
                config.slot_us = ms * us_per_ms;
                found = config.valid();
            }
            if (!found) {
                NOTE("k %u, N %u: no valid T (band or frame rule)", unsigned(k), unsigned(slot_counts[n]));
                continue;
            }
            ++modes;
            const size_t full = frame_bytes_of(config);
            std::vector<size_t> counts;
            for (size_t q = 1; q < full; ++q) counts.push_back(q);
            counts.push_back(full);
            counts.push_back(2 * full + 1);
            counts.push_back(3 * full - 1);
            for (size_t c = 0; c < counts.size(); ++c) {
                const std::vector<uint8_t> data = test_bytes(counts[c], static_cast<uint32_t>(97 * c + k));
                const Capture capture = transmit(config, data);
                const std::vector<Slot> slots = slots_of(capture);
                const std::vector<Expected> expected = expected_slots(config, data);
                ++transmissions;
                if (!CHECK_EQ(slots.size(), expected.size())) continue;
                // Rebuild the bytes from the frame symbols: the decoder's view.
                std::vector<uint8_t> received;
                size_t mismatches = 0;
                unsigned bits = 0;
                unsigned accumulator = 0;
                for (size_t i = 0; i < slots.size(); ++i) {
                    const EncoderStatus& s = slots[i].status;
                    if (s.segment != expected[i].segment || s.kind != expected[i].kind ||
                        s.symbol != expected[i].symbol || s.tone != expected[i].tone || s.slot != expected[i].slot)
                        ++mismatches;
                    if (s.segment == EncoderSegment::frame && s.kind == SlotKind::marker) {
                        // STOP: whole bytes of this frame; the remaining bits are the zero padding.
                        CHECK_EQ(accumulator & ((1u << bits) - 1u), 0u);
                        bits = 0;
                        accumulator = 0;
                    }
                    if (s.segment != EncoderSegment::frame || s.kind != SlotKind::peak) continue;
                    CHECK_EQ(unsigned(unlimited::peak_symbol(s.tone, s.slot, k)), unsigned(s.symbol));
                    accumulator = (accumulator << k) | s.symbol;
                    bits += k;
                    if (bits >= bits_per_byte) {
                        bits -= bits_per_byte;
                        received.push_back(static_cast<uint8_t>(accumulator >> bits));
                    }
                }
                CHECK_EQ(mismatches, 0u);
                if (!CHECK(received == data))
                    NOTE("k %u N %u: %zu bytes", unsigned(k), unsigned(slot_counts[n]), data.size());
            }
        }
    }
    NOTE("%zu (k, N) modes, %zu transmissions", modes, transmissions);
    CHECK_EQ(modes, 8u + 7u + 6u);
}

// U25: every peak on its exact grid frequency (header on the standard grid in dense modes).
TEST(encoder_peak_frequencies) {
    std::vector<EncoderConfig> configs;
    configs.push_back(EncoderConfig::from_preset(Preset::hf, 8000));
    configs.push_back(EncoderConfig::from_preset(Preset::fm_fast, 8000));
    configs.push_back(EncoderConfig::from_preset(Preset::hf_fast, 48000));
    configs.push_back(EncoderConfig::from_preset(Preset::hf_weak, 11025));
    const std::vector<EncoderConfig> extra = extra_modes(44100);
    configs.insert(configs.end(), extra.begin(), extra.end());
    configs.push_back(dense_k8_mode(8000));
    const double tolerance_hz = 0.01;
    double worst = 0.0;
    size_t checked = 0;
    for (size_t c = 0; c < configs.size(); ++c) {
        const EncoderConfig& config = configs[c];
        REQUIRE(config.valid());
        const std::vector<uint8_t> data = test_bytes(2 * frame_bytes_of(config), static_cast<uint32_t>(c + 1));
        const Capture capture = transmit(config, data);
        const std::vector<Slot> slots = slots_of(capture);
        size_t header = 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            const Slot& slot = slots[i];
            if (slot.status.kind != SlotKind::peak) continue;
            if (slot.status.segment == EncoderSegment::header) ++header;
            // Flat top: 1/8 <= u <= 7/8, one sample of margin.
            const size_t length = slot.end - slot.begin;
            const size_t begin = slot.begin + length / 8 + 1;
            const size_t end = slot.end - length / 8 - 1;
            const double expected = peak_frequency(config, slot.status);
            const double measured = estimate_frequency(capture, begin, end, expected, config.sample_rate_hz);
            const double error = std::fabs(measured - expected);
            worst = std::max(worst, error);
            ++checked;
            if (!CHECK(error <= tolerance_hz))
                NOTE("config %zu slot %zu tone %u: %.5f Hz, expected %.5f Hz", c, i, slot.status.tone, measured,
                     expected);
        }
        CHECK_EQ(header, header_peaks);
    }
    NOTE("%zu peaks: max |f - f_grid| %.2e Hz", checked, worst);
}

// U5': bounds, quiet slot edges, marker reversals on f_ref with a persistent sign, peaks without reversal.
TEST(encoder_waveform_bounds_edges_and_signs) {
    std::vector<EncoderConfig> configs;
    for (size_t p = 0; p < count_of(all_presets); ++p)
        configs.push_back(EncoderConfig::from_preset(all_presets[p], base_rate));
    const std::vector<EncoderConfig> extra = extra_modes(base_rate);
    configs.insert(configs.end(), extra.begin(), extra.end());
    for (size_t c = 0; c < configs.size(); ++c) {
        const EncoderConfig& config = configs[c];
        const Capture capture = transmit(config, test_bytes(frame_bytes_of(config) + 3, static_cast<uint32_t>(c)));
        const std::vector<Slot> slots = slots_of(capture);
        const double amplitude = config.amplitude;
        const double slot_samples = static_cast<double>(config.slot_us) * base_rate / us_per_s;
        // |y| at the sample next to an edge is at most A sin^2(2 pi / L) (markers) or A sin^2(4 pi / L)
        // (peaks); below A/256 once L >= 256 (T >= 32 ms at 8 kHz).
        const double marker_edge = amplitude * std::pow(std::sin(two_pi / slot_samples), 2) + 1;
        const double peak_edge = amplitude * std::pow(std::sin(2 * two_pi / slot_samples), 2) + 1;
        const double quiet_edge = config.slot_us >= 32000 ? amplitude / 256.0 : 1e9;
        int peak = 0;
        for (size_t n = 0; n < capture.samples.size(); ++n) peak = std::max(peak, std::abs(int(capture.samples[n])));
        CHECK(peak <= config.amplitude);

        int sign = 1;
        size_t markers = 0;
        size_t peaks = 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            const Slot& slot = slots[i];
            const SlotKind kind = slot.status.kind;
            if (kind == SlotKind::silent) continue;
            const size_t length = slot.end - slot.begin;
            const size_t middle = slot.begin + length / 2;
            const size_t from = slot.begin + length / 10;
            const size_t to = slot.end - length / 10;
            if (kind != SlotKind::tone) {
                const double edge = std::min(kind == SlotKind::marker ? marker_edge : peak_edge, quiet_edge);
                CHECK(std::abs(capture.samples[slot.begin]) <= edge);
                CHECK(std::abs(capture.samples[slot.end - 1]) <= edge);
            }
            if (kind == SlotKind::peak) {
                // No reversal: both halves have the same phase at the peak's own grid tone.
                const double frequency = peak_frequency(config, slot.status);
                const Complex before = mix_at(capture, from, middle, frequency, config.sample_rate_hz);
                const Complex after =
                    mix_at(capture, middle, to, frequency, config.sample_rate_hz) *
                    std::polar(1.0, -two_pi * frequency * double(middle - from) / config.sample_rate_hz);
                CHECK((before * std::conj(after)).real() > 0.95 * std::abs(before) * std::abs(after));
                ++peaks;
                continue;
            }
            // Markers and tune on the f_ref NCO, which runs through the data slots: in phase with the reference.
            const double before = project_on_tone(config, capture, from, middle);
            const double after = project_on_tone(config, capture, middle, to);
            CHECK(std::fabs(before) >= 0.9 * std::abs(mix_down(config, capture, from, middle)));
            if (kind == SlotKind::marker) {
                CHECK(before * after < 0.0);
                CHECK(before * sign > 0.0);
                CHECK(after * sign < 0.0);
                sign = -sign;
                ++markers;
            } else {
                CHECK(before * after > 0.0);
                CHECK(before * sign > 0.0);
            }
        }
        const unsigned frames = 2;  // one full frame and a short one
        CHECK_EQ(markers, config.sync_markers + 1u + frames + eot_slots);
        CHECK(peaks > header_peaks);
    }
}

TEST(encoder_tune_ramps_and_flat_top) {
    const EncoderConfig config;
    const Capture capture = transmit(config, std::vector<uint8_t>(1, 0x00));
    const std::vector<Slot> slots = slots_of(capture);
    const Slot* first = find_slot(slots, EncoderSegment::tune, SlotKind::tone, 0);
    REQUIRE(first != nullptr);
    const size_t length = first->end - first->begin;
    // Starts from silence, flat (|y| reaches A - 1 LSB) from u = 0.25 until the last tune slot's u = 0.75.
    CHECK(std::abs(capture.samples[first->begin]) <= config.amplitude / 256);
    int peak = 0;
    const size_t flat_begin = first->begin + length / 4 + 1;
    const size_t tune_end = first->begin + expected_tune_slots(config) * length;
    for (size_t n = flat_begin; n < tune_end - length / 4 - 1; ++n)
        peak = std::max(peak, std::abs(int(capture.samples[n])));
    CHECK(peak >= config.amplitude - 2);
    CHECK(std::abs(capture.samples[tune_end - 1]) <= config.amplitude / 256);
    const double energy_first = std::norm(mix_down(config, capture, first->begin, first->end));
    const double energy_second = std::norm(mix_down(config, capture, first->end, first->end + length));
    CHECK(energy_first < energy_second);
}

// U6: at 48 kHz the negative-frequency image of a tone near 2 kHz stays >= 20 / T away from the scan.
TEST(encoder_spectrum_marker_not_wider_than_data) {
    const uint32_t slot_ms[] = {8, 32, 128};
    const uint32_t rate = 48000;
    const double span_slots = 16.0;       // scan +-16/T around the tone
    const double resolution_slots = 0.01;  // 1% of 1/T
    for (size_t i = 0; i < count_of(slot_ms); ++i) {
        const EncoderConfig config = largest_mode(slot_ms[i], rate);
        REQUIRE(config.valid());
        const double slot_s = slot_seconds(config);
        const double span = span_slots / slot_s;
        const double step = resolution_slots / slot_s;
        const Capture capture = transmit(config, test_bytes(1));
        const std::vector<Slot> slots = slots_of(capture);
        const Slot* marker = find_slot(slots, EncoderSegment::sync, SlotKind::marker, 3);
        const Slot* peak = find_slot(slots, EncoderSegment::header, SlotKind::peak, 0);
        REQUIRE(marker != nullptr && peak != nullptr);
        const double tone = config.tone_hz;
        const double peak_tone = peak_frequency(config, peak->status);
        const double marker_width =
            width_40db(spectrum(audio_of(capture, *marker), rate, tone - span, tone + span, step), step);
        const double peak_width =
            width_40db(spectrum(audio_of(capture, *peak), rate, peak_tone - span, peak_tone + span, step), step);
        NOTE("T %u ms: -40 dB width marker %.1f Hz (%.2f/T), data peak %.1f Hz (%.2f/T), ratio %.3f", slot_ms[i],
             marker_width, marker_width * slot_s, peak_width, peak_width * slot_s, marker_width / peak_width);
        CHECK(marker_width <= 1.1 * peak_width);
    }
}

// U7': E_peak 0.84375, E_m 0.5625, g_m 0.8355 (T A^2 / 2 units), each within 0.5%.
TEST(encoder_energies_and_window_gains) {
    EncoderConfig config;
    config.slot_us = 50000;  // 400 samples: the 0.35 T half windows are whole samples
    REQUIRE(config.valid());
    const Capture capture = transmit(config, test_bytes(5));
    const std::vector<Slot> slots = slots_of(capture);
    const Slot* marker = find_slot(slots, EncoderSegment::sync, SlotKind::marker, 3);
    REQUIRE(marker != nullptr);
    const double amplitude = config.amplitude;
    const size_t length = marker->end - marker->begin;
    REQUIRE(length == 400u);
    const double unit = length * amplitude * amplitude / 2.0;
    const double tolerance = 0.005;

    const double peak_energy = 0.84375;
    double worst_peak = peak_energy;
    size_t peaks = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].status.kind != SlotKind::peak) continue;
        double energy = 0.0;
        for (size_t n = slots[i].begin; n < slots[i].end; ++n)
            energy += double(capture.samples[n]) * capture.samples[n];
        const double relative = energy / unit;
        if (std::fabs(relative - peak_energy) > std::fabs(worst_peak - peak_energy)) worst_peak = relative;
        CHECK_NEAR(relative, peak_energy, peak_energy * tolerance);
        ++peaks;
    }
    double marker_energy = 0.0;
    for (size_t n = marker->begin; n < marker->end; ++n)
        marker_energy += double(capture.samples[n]) * capture.samples[n];
    const size_t half = length * 35 / 100;
    const size_t centre = marker->begin + length / 2;
    const Complex before = mix_down(config, capture, centre - half, centre);
    const Complex after = mix_down(config, capture, centre, centre + half);
    const double g_marker = 2.0 * std::abs(before - after) / (amplitude * 2 * half);
    NOTE("%zu peaks: E_peak worst %.5f; E_m %.5f; g_m %.5f", peaks, worst_peak, marker_energy / unit, g_marker);
    CHECK_EQ(peaks, header_peaks + 8);
    CHECK_NEAR(marker_energy / unit, 0.5625, 0.5625 * tolerance);
    CHECK_NEAR(g_marker, 0.8355, 0.8355 * tolerance);
}

TEST(encoder_streaming_and_underrun_eot) {
    const EncoderConfig config;
    const size_t full = frame_bytes_of(config);
    Encoder encoder(config);
    const std::vector<uint8_t> data = test_bytes(2 * full);
    CHECK_EQ(encoder.write(data.data(), full), full);
    REQUIRE(encoder.start());
    // A second frame written during the header follows without a gap; then the queue runs dry: EOT.
    bool wrote = false;
    size_t frame_peaks = 0;
    uint32_t last_slot = 0xFFFFFFFFu;
    EncoderSegment previous = EncoderSegment::idle;
    std::vector<EncoderSegment> order;
    while (encoder.busy()) {
        const EncoderStatus status = encoder.status();
        if (!wrote && status.segment == EncoderSegment::header) wrote = encoder.write(&data[full], full) == full;
        if (status.segment == EncoderSegment::frame && status.kind == SlotKind::peak && status.slot_index != last_slot)
            ++frame_peaks;
        if (status.segment != previous) order.push_back(status.segment);
        previous = status.segment;
        last_slot = status.slot_index;
        encoder.next_sample();
    }
    CHECK(wrote);
    CHECK_EQ(frame_peaks, 2u * config.data_slots);
    const EncoderSegment expected[] = {EncoderSegment::tune, EncoderSegment::sync, EncoderSegment::header,
                                       EncoderSegment::frame, EncoderSegment::eot, EncoderSegment::tail};
    REQUIRE(order.size() == count_of(expected));
    for (size_t i = 0; i < order.size(); ++i) CHECK(order[i] == expected[i]);
    CHECK_EQ(encoder.next_sample(), 0);
    CHECK(!encoder.start());  // queue drained
}

// A short final frame always ends the transmission: its STOP and the two EOT markers are the flip triple.
TEST(encoder_short_final_frame_ends_transmission) {
    const EncoderConfig config;
    const size_t full = frame_bytes_of(config);
    const std::vector<uint8_t> data = test_bytes(full + 2);
    const std::vector<uint8_t> late = test_bytes(3, 99);
    Encoder encoder(config);
    CHECK_EQ(encoder.write(data.data(), data.size()), data.size());
    REQUIRE(encoder.start());
    bool wrote = false;
    size_t stops = 0;
    size_t short_peaks = 0;
    uint32_t last_slot = 0xFFFFFFFFu;
    std::vector<EncoderSegment> after_short;
    while (encoder.busy()) {
        const EncoderStatus status = encoder.status();
        const bool new_slot = status.slot_index != last_slot;
        last_slot = status.slot_index;
        const bool frame = status.segment == EncoderSegment::frame;
        if (new_slot && frame && status.kind == SlotKind::marker) ++stops;
        const bool in_short = stops >= 1 && !(stops == 1 && frame && status.kind == SlotKind::marker && new_slot);
        if (stops >= 1 && frame && status.kind == SlotKind::peak && !wrote)
            wrote = encoder.write(late.data(), late.size()) == late.size();
        if (in_short && new_slot) {
            if (frame && status.kind == SlotKind::peak) ++short_peaks;
            if (!frame) after_short.push_back(status.segment);
        }
        encoder.next_sample();
    }
    CHECK_EQ(stops, 2u);
    CHECK(wrote);
    CHECK_EQ(short_peaks, size_t(peaks_for(2, config.bits_per_peak)));
    const EncoderSegment expected[] = {EncoderSegment::eot, EncoderSegment::eot, EncoderSegment::tail};
    REQUIRE(after_short.size() == count_of(expected));
    for (size_t i = 0; i < after_short.size(); ++i) CHECK(after_short[i] == expected[i]);
    // The late bytes stay queued for the next transmission.
    CHECK_EQ(encoder.queue_free(), queue_capacity - late.size());
    CHECK(encoder.start());
}

TEST(encoder_abort_and_restart) {
    const EncoderConfig config;
    Encoder encoder(config);
    const std::vector<uint8_t> data = test_bytes(10);
    encoder.write(data.data(), data.size());
    REQUIRE(encoder.start());
    std::vector<int16_t> buffer(8000);  // tune 8 + sync 8 + header 9 slots = 6400 samples: inside frame 0
    CHECK_EQ(encoder.render(buffer.data(), buffer.size()), buffer.size());
    CHECK(encoder.status().segment == EncoderSegment::frame);
    encoder.abort();
    CHECK(!encoder.busy());
    CHECK_EQ(encoder.next_sample(), 0);
    CHECK_EQ(encoder.render(buffer.data(), buffer.size()), 0u);
    CHECK(encoder.status().segment == EncoderSegment::idle);
    CHECK_EQ(encoder.queue_free(), queue_capacity);
    CHECK(!encoder.start());

    // A new transmission after an abort is identical to one from a fresh encoder (both NCOs restart).
    encoder.write(data.data(), 2);
    REQUIRE(encoder.start());
    Encoder fresh(config);
    fresh.write(data.data(), 2);
    REQUIRE(fresh.start());
    size_t n = 0;
    while (encoder.busy() || fresh.busy()) {
        if (!CHECK_EQ(encoder.next_sample(), fresh.next_sample())) break;
        ++n;
    }
    CHECK_EQ(n, (duration_of(config, 2)));
}

TEST(encoder_render_chunk_invariance) {
    const EncoderConfig config = EncoderConfig::from_preset(Preset::fm, 11025);
    const std::vector<uint8_t> data = test_bytes(20);
    const size_t chunks[] = {1, 7, 160, 4096};
    std::vector<int16_t> reference;
    for (size_t c = 0; c < count_of(chunks); ++c) {
        Encoder encoder(config);
        encoder.write(data.data(), data.size());
        REQUIRE(encoder.start());
        std::vector<int16_t> out;
        std::vector<int16_t> buffer(chunks[c]);
        for (;;) {
            const size_t got = encoder.render(buffer.data(), buffer.size());
            if (got == 0) break;
            out.insert(out.end(), buffer.begin(), buffer.begin() + got);
        }
        if (c == 0) {
            reference = out;
            CHECK_EQ(out.size(), (duration_of(config, data.size())));
        } else {
            CHECK(out == reference);
        }
    }
}

TEST(encoder_duration_saturates) {
    const Encoder encoder(EncoderConfig::from_preset(Preset::hf_weak, 192000));
    CHECK_EQ(encoder.duration_samples(static_cast<size_t>(-1)), 0xFFFFFFFFu);
    CHECK_EQ(encoder.duration_samples(0xFFFFFFFFu / 8), 0xFFFFFFFFu);
    CHECK(encoder.duration_samples(0) > 0);
    // Far from saturation the formula holds: 10000 bytes of hf_weak at 192 kHz (27 min).
    const EncoderConfig config = encoder.config();
    const size_t bytes = 10000;
    CHECK(std::fabs(encoder.duration_samples(bytes) - formula_samples(config, bytes)) <= 1.0);
}
