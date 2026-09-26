#include "test_harness.hpp"
#include "unlimited/encoder.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

using unlimited::ConfigError;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSegment;
using unlimited::EncoderStatus;
using unlimited::Passband;
using unlimited::Preset;
using unlimited::SlotKind;

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
const unsigned end_slots = 2;
const unsigned bits_per_byte = 8;
const size_t queue_capacity = Encoder::k_queue_size;

const Preset all_presets[] = {Preset::hf_slow, Preset::hf, Preset::hf_fast, Preset::am, Preset::fm};
const uint32_t test_rates[] = {8000, 11025, 44100, 48000};

// Spec 1.7, in Preset order.
struct PresetMode {
    uint32_t slot_ms;
    uint8_t bits;
    uint16_t passband_low_hz;
    uint16_t passband_high_hz;
    uint16_t lead_in_ms;
};
const PresetMode preset_modes[] = {
    {32, 8, 300, 2700, 0}, {16, 8, 300, 2700, 0}, {8, 8, 300, 2700, 0}, {8, 16, 100, 3000, 0}, {4, 16, 300, 3000, 300},
};

struct Capture {
    std::vector<int16_t> samples;
    std::vector<EncoderStatus> status;  // status before each sample: describes the slot it belongs to
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
    unsigned slot;           // package: 1..d, d + 1 the STOP
    unsigned package_bits;
    unsigned byte_index;     // package data slots: the byte of the bit
    unsigned bit_index;
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

uint32_t expected_tune_slots(const EncoderConfig& config) {
    const uint32_t tune_us = config.tune_ms * us_per_ms;
    return std::max<uint32_t>((tune_us + config.slot_us - 1) / config.slot_us, min_tune_slots);
}

size_t packages_for(size_t bytes, unsigned bits) {
    return (bytes * bits_per_byte + bits - 1) / bits;
}

unsigned stream_bit(const std::vector<uint8_t>& data, size_t bit) {
    return (data[bit / bits_per_byte] >> (bits_per_byte - 1 - bit % bits_per_byte)) & 1u;
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
    const size_t bits = data.size() * bits_per_byte;
    const unsigned n = config.bits_per_package;
    for (size_t first = 0; first < bits; first += n) {
        const unsigned d = static_cast<unsigned>(std::min<size_t>(n, bits - first));
        for (unsigned i = 1; i <= d; ++i) {
            const size_t bit = first + i - 1;
            const Expected slot = {EncoderSegment::package, stream_bit(data, bit) != 0 ? SlotKind::one : SlotKind::zero,
                                   i, d, static_cast<unsigned>(bit / bits_per_byte),
                                   static_cast<unsigned>(bit % bits_per_byte)};
            slots.push_back(slot);
        }
        const Expected stop = {EncoderSegment::package, SlotKind::marker, d + 1, d, 0, 0};
        slots.push_back(stop);
    }
    const Expected end = {EncoderSegment::end, SlotKind::marker, 0, 0, 0, 0};
    slots.insert(slots.end(), end_slots, end);
    if (config.tail_ms != 0) {
        const Expected tail = {EncoderSegment::tail, SlotKind::silent, 0, 0, 0, 0};
        slots.push_back(tail);
    }
    return slots;
}

// Spec 2.4: lead + (N_tune + N_sync + B + P + 2) T + tail.
double formula_samples(const EncoderConfig& config, size_t bytes) {
    const double rate = config.sample_rate_hz;
    const size_t bits = bytes * bits_per_byte;
    const double slots = expected_tune_slots(config) + config.sync_markers + bits +
                         packages_for(bytes, config.bits_per_package) + end_slots;
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

// The encoder's NCO phase without the marker sign flips.
double tone_phase(const EncoderConfig& config, size_t n) {
    const uint64_t step = ((static_cast<uint64_t>(config.tone_hz) << 32) + config.sample_rate_hz / 2) /
                          config.sample_rate_hz;
    return two_pi * static_cast<double>(static_cast<uint32_t>(step * n)) / turn;
}

// Correlation with the unflipped carrier over [begin, end): its sign is the carrier sign s there.
double project_on_tone(const EncoderConfig& config, const Capture& capture, size_t begin, size_t end) {
    double sum = 0.0;
    for (size_t n = begin; n < end; ++n) sum += capture.samples[n] * std::sin(tone_phase(config, n));
    return sum;
}

// A config with T in microseconds and N, pitch 1500 Hz, the widest passband so any T fits.
EncoderConfig mode(uint32_t slot_us, uint8_t bits, uint32_t rate) {
    EncoderConfig config = EncoderConfig::from_preset(Preset::hf, rate);
    config.slot_us = slot_us;
    config.bits_per_package = bits;
    config.passband.low_hz = unlimited::k_am_passband_low_hz;
    config.passband.high_hz = unlimited::k_am_passband_high_hz;
    return config;
}

}  // namespace

TEST(encoder_config_defaults_and_presets) {
    const EncoderConfig config;
    CHECK_EQ(config.sample_rate_hz, base_rate);
    CHECK_EQ(config.slot_us, 16000u);
    CHECK_EQ(config.tone_hz, 1500);
    CHECK_EQ(config.amplitude, default_amplitude);
    CHECK_EQ(config.passband.low_hz, 300);
    CHECK_EQ(config.passband.high_hz, 2700);
    CHECK_EQ(config.lead_in_ms, 0);
    CHECK_EQ(config.tune_ms, 250);
    CHECK_EQ(config.tail_ms, 100);
    CHECK_EQ(unsigned(config.bits_per_package), 8u);
    CHECK_EQ(unsigned(config.sync_markers), 8u);
    CHECK(config.valid());
    CHECK(config.check() == ConfigError::none);

    for (size_t p = 0; p < count_of(all_presets); ++p) {
        const PresetMode& expected = preset_modes[p];
        for (size_t r = 0; r < count_of(test_rates); ++r) {
            const EncoderConfig preset = EncoderConfig::from_preset(all_presets[p], test_rates[r]);
            CHECK_EQ(preset.sample_rate_hz, test_rates[r]);
            CHECK_EQ(preset.slot_us, expected.slot_ms * us_per_ms);
            CHECK_EQ(unsigned(preset.bits_per_package), unsigned(expected.bits));
            CHECK_EQ(preset.tone_hz, 1500);
            CHECK_EQ(preset.passband.low_hz, expected.passband_low_hz);
            CHECK_EQ(preset.passband.high_hz, expected.passband_high_hz);
            CHECK_EQ(preset.lead_in_ms, expected.lead_in_ms);
            CHECK_EQ(preset.tune_ms, 250);
            CHECK_EQ(preset.tail_ms, 100);
            CHECK_EQ(unsigned(preset.sync_markers), 8u);
            CHECK_EQ(preset.amplitude, default_amplitude);
            CHECK(preset.valid());
        }
    }
}

// U22: check() names the first rule broken, in ConfigError order.
TEST(encoder_config_check_names_the_rule) {
    struct Case {
        const char* name;
        ConfigError expected;
        void (*change)(EncoderConfig&);
    };
    const Case cases[] = {
        {"rate 7999", ConfigError::sample_rate, [](EncoderConfig& c) { c.sample_rate_hz = 7999; }},
        {"rate 192001", ConfigError::sample_rate, [](EncoderConfig& c) { c.sample_rate_hz = 192001; }},
        {"tone 299", ConfigError::tone, [](EncoderConfig& c) { c.tone_hz = 299; }},
        {"tone 2701", ConfigError::tone, [](EncoderConfig& c) { c.tone_hz = 2701; }},
        {"T 3999", ConfigError::slot, [](EncoderConfig& c) { c.slot_us = 3999; }},
        {"T 128001", ConfigError::slot, [](EncoderConfig& c) { c.slot_us = 128001; }},
        {"T 4 ms at 999 Hz", ConfigError::fast_tone,
         [](EncoderConfig& c) {
             c.slot_us = 4000;
             c.tone_hz = 999;
         }},
        {"N 0", ConfigError::bits_per_package, [](EncoderConfig& c) { c.bits_per_package = 0; }},
        {"N cap + 1", ConfigError::bits_per_package,
         [](EncoderConfig& c) { c.bits_per_package = unlimited::k_max_bits_per_package + 1; }},
        {"(N + 1) T = 1152018", ConfigError::package_length,
         [](EncoderConfig& c) {
             c.bits_per_package = 17;
             c.slot_us = 64001;
         }},
        {"passband inverted", ConfigError::passband,
         [](EncoderConfig& c) {
             c.passband.low_hz = 2700;
             c.passband.high_hz = 300;
         }},
        {"passband above 4000", ConfigError::passband, [](EncoderConfig& c) { c.passband.high_hz = 4001; }},
        {"tone 400 Hz at 16 ms", ConfigError::outside_passband, [](EncoderConfig& c) { c.tone_hz = 400; }},
        {"sync 7", ConfigError::sync_markers, [](EncoderConfig& c) { c.sync_markers = 7; }},
        {"sync 33", ConfigError::sync_markers, [](EncoderConfig& c) { c.sync_markers = 33; }},
        {"amplitude 0", ConfigError::amplitude, [](EncoderConfig& c) { c.amplitude = 0; }},
        {"amplitude -1", ConfigError::amplitude, [](EncoderConfig& c) { c.amplitude = -1; }},
    };
    for (size_t i = 0; i < count_of(cases); ++i) {
        EncoderConfig config;
        cases[i].change(config);
        if (!CHECK(config.check() == cases[i].expected)) NOTE("case %s", cases[i].name);
        CHECK(!config.valid());
    }
    // (N + 1) T exactly 1152 ms is allowed; one more microsecond is not.
    EncoderConfig limit;
    limit.bits_per_package = 8;
    limit.slot_us = 128000;
    CHECK(limit.check() == ConfigError::none);
    limit.bits_per_package = 17;
    limit.slot_us = 64000;
    CHECK(limit.check() == ConfigError::none);
    limit.slot_us = 64001;
    CHECK(limit.check() == ConfigError::package_length);
    // Earlier rules win: a bad rate and a bad tone report the rate.
    EncoderConfig both;
    both.sample_rate_hz = 1;
    both.tone_hz = 1;
    CHECK(both.check() == ConfigError::sample_rate);
    // T below 8 ms with a tone of 1000 Hz or more is fine.
    EncoderConfig fast;
    fast.slot_us = 4000;
    fast.tone_hz = 1000;
    fast.passband.high_hz = 3000;
    CHECK(fast.check() == ConfigError::none);
}

TEST(encoder_occupied_band_of_config) {
    const EncoderConfig hf;
    const unlimited::Band band = unlimited::occupied_band(hf);
    CHECK_EQ(band.low_hz, 1362);
    CHECK_EQ(band.high_hz, 1638);
    CHECK_EQ(band.width_hz, 276);
    const unlimited::PassbandFit fit = unlimited::passband_fit(hf);
    CHECK(fit.fits);
    CHECK_EQ(fit.margin_low_hz, 1062);
    CHECK_EQ(fit.margin_high_hz, 1062);
    CHECK_EQ(fit.tolerance_hz, 1062);
    // The fm preset in its own 300..3000 Hz passband (spec 1.7): the filter leaves 650 Hz below and 950 Hz above, but
    // its receiver (the fm profile) searches from 1000 Hz, so the pitch may move 500 Hz down (spec 1.5).
    const EncoderConfig fm = EncoderConfig::from_preset(Preset::fm, base_rate);
    const unlimited::PassbandFit fm_room = unlimited::passband_fit(unlimited::occupied_band(fm), fm.passband);
    CHECK_EQ(fm_room.margin_low_hz, 650);
    CHECK_EQ(fm_room.margin_high_hz, 950);
    const unlimited::Passband fm_search = unlimited::search_range(fm);
    CHECK_EQ(fm_search.low_hz, 1000);
    CHECK_EQ(fm_search.high_hz, 2700);
    const unlimited::PassbandFit fm_fit = unlimited::passband_fit(fm);
    CHECK(fm_fit.fits);
    CHECK_EQ(fm_fit.margin_low_hz, 500);
    CHECK_EQ(fm_fit.margin_high_hz, 950);
    CHECK_EQ(fm_fit.tolerance_hz, 500);
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
    CHECK_EQ(encoder.duration_samples(0), 0u);
    CHECK(encoder.config().slot_us == EncoderConfig().slot_us);
}

// The queue holds exactly k_queue_size bytes (free-running indices, spec 2.5).
TEST(encoder_queue_capacity) {
    NOTE("sizeof(Encoder) = %zu B on this host, queue %u B", sizeof(Encoder), unsigned(Encoder::k_queue_size));
    Encoder encoder{EncoderConfig()};
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

    // The indices wrap (mod 256) many times over a long stream: every byte comes out once, in order, at its
    // byte_index, and the count holds throughout.
    EncoderConfig config = mode(4000, 8, base_rate);
    config.tone_hz = 1500;
    Encoder streaming(config);
    const std::vector<uint8_t> stream = test_bytes(20 * Encoder::k_queue_size, 7);
    size_t fed = streaming.write(stream.data(), stream.size());
    CHECK_EQ(fed, queue_capacity);
    REQUIRE(streaming.start());
    size_t bytes = 0;
    uint32_t last_slot = 0xFFFFFFFFu;
    while (streaming.busy()) {
        if (fed < stream.size()) fed += streaming.write(&stream[fed], stream.size() - fed);
        CHECK(streaming.queued() + streaming.queue_free() == queue_capacity);
        const EncoderStatus status = streaming.status();
        if (status.segment == EncoderSegment::package && status.slot_index != last_slot && status.bit_index == 0 &&
            status.slot <= status.package_bits) {
            if (!CHECK_EQ(unsigned(status.byte), unsigned(stream[bytes]))) break;
            CHECK_EQ(status.byte_index, uint32_t(bytes));
            ++bytes;
        }
        last_slot = status.slot_index;
        streaming.next_sample();
    }
    CHECK_EQ(bytes, stream.size());
}

// U4: total length = the spec 2.4 formula +-1 sample for every preset, N and T (short final packages included), at four
// rates; duration_samples() gives the same.
TEST(encoder_total_length_all_modes_and_rates) {
    const uint8_t bits[] = {1, 2, 3, 5, 7, 8, 9, 16, 31, 32};
    const uint32_t slots_us[] = {4000, 5000, 8000, 12500, 16000, 20000, 32000, 37000, 64000, 100000, 128000};
    const size_t sizes[] = {1, 3};
    size_t checked = 0;
    double worst = 0.0;
    for (size_t r = 0; r < count_of(test_rates); ++r) {
        for (size_t p = 0; p < count_of(all_presets); ++p) {
            const EncoderConfig config = EncoderConfig::from_preset(all_presets[p], test_rates[r]);
            const std::vector<uint8_t> data = test_bytes(5);
            const double length = static_cast<double>(rendered_length(config, data, 509));
            CHECK(std::fabs(length - formula_samples(config, data.size())) <= 1.0);
            CHECK_EQ(duration_of(config, data.size()), size_t(length));
        }
        for (size_t b = 0; b < count_of(bits); ++b) {
            if (bits[b] > unlimited::k_max_bits_per_package) continue;
            for (size_t t = 0; t < count_of(slots_us); ++t) {
                const EncoderConfig config = mode(slots_us[t], bits[b], test_rates[r]);
                if (!config.valid()) continue;  // (N + 1) T above 1152 ms
                for (size_t s = 0; s < count_of(sizes); ++s) {
                    const std::vector<uint8_t> data = test_bytes(sizes[s], uint32_t(b * 100 + t));
                    const double length = static_cast<double>(rendered_length(config, data, 4096));
                    const double error = std::fabs(length - formula_samples(config, data.size()));
                    worst = std::max(worst, error);
                    if (!CHECK(error <= 1.0)) NOTE("N %u T %u us rate %u: %.0f vs %.2f", bits[b], slots_us[t],
                                                   test_rates[r], length, formula_samples(config, data.size()));
                    CHECK_EQ(duration_of(config, data.size()), size_t(length));
                    ++checked;
                }
            }
        }
    }
    NOTE("%zu transmissions, largest |length - formula| %.3f samples", checked, worst);
    // The spec 2.2 examples: "Hi" with hf is 6432 samples (804 ms); as a packet (8 bytes) 13344 samples.
    CHECK_EQ(duration_of(EncoderConfig(), 2), 6432u);
    CHECK_EQ(duration_of(EncoderConfig(), 8), 13344u);
}

TEST(encoder_slot_drift_over_ten_thousand_slots) {
    struct Case {
        uint32_t rate;
        uint32_t slot_us;
        uint16_t lead_ms;
    };
    const Case cases[] = {{44100, 32000, 300}, {11025, 37000, 300}, {48000, 12500, 0}, {8000, 4000, 7}};
    const uint32_t slots_wanted = 10000;
    for (size_t c = 0; c < count_of(cases); ++c) {
        EncoderConfig config = mode(cases[c].slot_us, 8, cases[c].rate);
        config.lead_in_ms = cases[c].lead_ms;
        REQUIRE(config.valid());
        const std::vector<uint8_t> data = test_bytes(slots_wanted / bits_per_byte + 1);
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
                const uint64_t numerator = (static_cast<uint64_t>(slot) * config.slot_us - offset_us) * config.sample_rate_hz;
                const double exact = static_cast<double>(numerator) / us_per_s;
                worst = std::max(worst, std::fabs(n - exact));
                if (n != (numerator + us_per_s - 1) / us_per_s) ++mismatches;
            }
            encoder.next_sample();
        }
        CHECK_EQ(slot, slots_wanted);
        CHECK_EQ(mismatches, 0u);
        CHECK(worst < 1.0);
        NOTE("rate %u, T %u us: %u slots, max |start - exact| %.4f samples", cases[c].rate, cases[c].slot_us, slot,
             worst);
    }
}

// U21, U23: every slot of a transmission as spec 2.1 and 2.2 say, with the status fields of spec 5.1.
TEST(encoder_segments_and_status) {
    struct Case {
        uint32_t slot_us;
        uint8_t bits;
        size_t bytes;
        uint16_t lead_ms;
    };
    const Case cases[] = {{16000, 8, 5, 0}, {8000, 3, 4, 100}, {4000, 16, 3, 300}, {32000, 1, 2, 0}, {12500, 31, 5, 0}};
    for (size_t c = 0; c < count_of(cases); ++c) {
        EncoderConfig config = mode(cases[c].slot_us, cases[c].bits, base_rate);
        config.lead_in_ms = cases[c].lead_ms;
        REQUIRE(config.valid());
        const std::vector<uint8_t> data = test_bytes(cases[c].bytes, uint32_t(c + 1));
        const Capture capture = transmit(config, data);
        const std::vector<Slot> slots = slots_of(capture);
        const std::vector<Expected> expected = expected_slots(config, data);
        REQUIRE(slots.size() == expected.size());
        uint32_t package = 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            const EncoderStatus& s = slots[i].status;
            const Expected& e = expected[i];
            bool ok = CHECK(s.segment == e.segment) && CHECK(s.kind == e.kind);
            ok = ok && CHECK_EQ(unsigned(s.slot), e.slot) && CHECK_EQ(unsigned(s.package_bits), e.package_bits);
            if (e.segment == EncoderSegment::package) {
                ok = ok && CHECK_EQ(s.package_index, package);
                if (e.kind != SlotKind::marker) {
                    ok = ok && CHECK_EQ(unsigned(s.byte), unsigned(data[e.byte_index])) &&
                         CHECK_EQ(unsigned(s.bit_index), e.bit_index) && CHECK_EQ(s.byte_index, uint32_t(e.byte_index));
                } else {
                    ok = ok && CHECK_EQ(unsigned(s.byte), 0u) && CHECK_EQ(unsigned(s.bit_index), 0u) &&
                         CHECK_EQ(s.byte_index, 0u);
                    ++package;
                }
            } else {
                ok = ok && CHECK_EQ(unsigned(s.byte), 0u) && CHECK_EQ(s.package_index, 0u) && CHECK_EQ(s.byte_index, 0u);
            }
            if (e.segment != EncoderSegment::tail) ok = ok && CHECK_EQ(s.slot_index, uint32_t(i));
            if (!ok) {
                NOTE("case %zu slot %zu", c, i);
                break;
            }
        }
        CHECK_EQ(capture.status.back().samples_rendered, uint32_t(capture.samples.size() - 1));
    }
    // Spec 2.2: "Hi" with hf, slot 28 (the fifth data slot of package 0).
    const std::vector<uint8_t> hi = {0x48, 0x69};
    const Capture capture = transmit(EncoderConfig(), hi);
    const std::vector<Slot> slots = slots_of(capture);
    REQUIRE(slots.size() > 28);
    const EncoderStatus& s = slots[28].status;
    CHECK(s.segment == EncoderSegment::package);
    CHECK(s.kind == SlotKind::one);
    CHECK_EQ(unsigned(s.slot), 5u);
    CHECK_EQ(unsigned(s.package_bits), 8u);
    CHECK_EQ(unsigned(s.byte), 0x48u);
    CHECK_EQ(unsigned(s.bit_index), 4u);
    CHECK_EQ(s.package_index, 0u);
    CHECK_EQ(s.byte_index, 0u);
    CHECK_EQ(s.slot_index, 28u);
    CHECK(slots[23].status.segment == EncoderSegment::sync);  // the first START belongs to the sync segment
    CHECK(slots[32].status.kind == SlotKind::marker && slots[32].status.slot == 9);
}

// U21: for every N in 1..cap and n = 1..64 bytes, P = ceil(8 n / N) packages, the last with 8 n - N (P - 1) bits, and
// the data slots carry the stream bits MSB first.
TEST(encoder_packing_all_n) {
    size_t transmissions = 0;
    for (unsigned n = 1; n <= unlimited::k_max_bits_per_package; ++n) {
        EncoderConfig config = mode(4000, static_cast<uint8_t>(n), base_rate);
        config.tune_ms = 0;
        config.tail_ms = 0;
        if (!config.valid()) continue;
        for (size_t bytes = 1; bytes <= 64; ++bytes) {
            const std::vector<uint8_t> data = test_bytes(bytes, uint32_t(n * 1000 + bytes));
            Encoder encoder(config);
            REQUIRE(encoder.write(data.data(), data.size()) == data.size());
            REQUIRE(encoder.start());
            size_t packages = 0;
            size_t last_bits = 0;
            size_t bit = 0;
            bool ok = true;
            uint32_t last_slot = 0xFFFFFFFFu;
            while (encoder.busy() && ok) {
                const EncoderStatus s = encoder.status();
                if (s.slot_index != last_slot && s.segment == EncoderSegment::package) {
                    if (s.slot <= s.package_bits) {
                        const unsigned expected = stream_bit(data, bit++);
                        ok = CHECK((s.kind == SlotKind::one) == (expected != 0));
                    } else {
                        ++packages;
                        last_bits = s.package_bits;
                    }
                }
                last_slot = s.slot_index;
                encoder.next_sample();
            }
            const size_t expected_packages = packages_for(bytes, n);
            ok = ok && CHECK_EQ(packages, expected_packages) && CHECK_EQ(bit, bytes * bits_per_byte) &&
                 CHECK_EQ(last_bits, bytes * bits_per_byte - n * (expected_packages - 1));
            if (!ok) {
                NOTE("N %u, %zu bytes", n, bytes);
                return;
            }
            ++transmissions;
        }
    }
    NOTE("%zu transmissions", transmissions);
}

// U5: the "Hi" slot sequence of spec 2.2, kinds and carrier signs.
TEST(encoder_hi_slot_sequence) {
    const EncoderConfig config;
    const std::vector<uint8_t> hi = {0x48, 0x69};
    const Capture capture = transmit(config, hi);
    const std::vector<Slot> slots = slots_of(capture);
    REQUIRE(slots.size() == 45u);  // 44 slots and the tail
    const char* kinds = "TTTTTTTTTTTTTTTTMMMMMMMM01001000M01101001MMM";
    // Carrier sign during each slot (markers: during their first half), spec 2.2.
    const char* signs = "++++++++++++++++ +-+-+-+- ++++++++ + -------- - +-";
    const char* sign = signs;
    for (size_t i = 0; i < 44; ++i) {
        const SlotKind kind = slots[i].status.kind;
        const char k = kind == SlotKind::tone ? 'T' : (kind == SlotKind::marker ? 'M' : (kind == SlotKind::one ? '1' : '0'));
        if (!CHECK_EQ(k, kinds[i])) NOTE("slot %zu", i);
        while (*sign == ' ') ++sign;
        const char expected = *sign++;
        if (kind == SlotKind::zero) continue;
        const Slot& s = slots[i];
        const size_t middle = s.begin + (s.end - s.begin) / 2;
        const double first = project_on_tone(config, capture, s.begin, middle);
        const double second = project_on_tone(config, capture, middle, s.end);
        if (!CHECK_EQ(first > 0.0 ? '+' : '-', expected)) NOTE("slot %zu", i);
        // A marker reverses in its middle; nothing else does.
        if (kind == SlotKind::marker) {
            CHECK(first * second < 0.0);
        } else {
            CHECK(first * second > 0.0);
        }
    }
    CHECK(slots[44].status.segment == EncoderSegment::tail);
    CHECK_EQ(capture.samples.size(), 6432u);
}

// U5: |y| <= A, silent slot edges, markers reverse and nothing else does, the carrier sign persists across packages.
TEST(encoder_waveform_bounds_edges_and_signs) {
    const EncoderConfig configs[] = {EncoderConfig(), mode(4000, 16, 48000), mode(37000, 3, 11025), mode(12500, 1, 8000)};
    for (size_t c = 0; c < count_of(configs); ++c) {
        const EncoderConfig& config = configs[c];
        const std::vector<uint8_t> data = test_bytes(9, uint32_t(c + 3));
        const Capture capture = transmit(config, data);
        const std::vector<Slot> slots = slots_of(capture);
        const int limit = config.amplitude;
        const double edge_limit = config.amplitude / 256.0;
        double sign = 1.0;
        size_t edges = 0;
        size_t reversals = 0;
        bool ok = true;
        for (size_t i = 0; i < slots.size() && ok; ++i) {
            const Slot& s = slots[i];
            for (size_t n = s.begin; n < s.end; ++n) ok = ok && CHECK(std::abs(int(capture.samples[n])) <= limit);
            const SlotKind kind = s.status.kind;
            if (kind == SlotKind::one || kind == SlotKind::marker) {
                ok = ok && CHECK(std::abs(double(capture.samples[s.begin])) <= edge_limit) &&
                     CHECK(std::abs(double(capture.samples[s.end - 1])) <= edge_limit);
                edges += 2;
            }
            if (kind == SlotKind::silent || kind == SlotKind::zero) continue;
            if (kind == SlotKind::tone) continue;
            const size_t middle = s.begin + (s.end - s.begin) / 2;
            const double first = project_on_tone(config, capture, s.begin, middle);
            const double second = project_on_tone(config, capture, middle, s.end);
            ok = ok && CHECK(first * sign > 0.0);
            if (kind == SlotKind::marker) {
                ok = ok && CHECK(second * sign < 0.0);
                sign = -sign;  // s <- -s after every marker
                ++reversals;
            } else {
                ok = ok && CHECK(second * sign > 0.0);
            }
            if (!ok) NOTE("config %zu slot %zu", c, i);
        }
        CHECK(edges > 0 && reversals > 0);
    }
}

TEST(encoder_tune_ramps_and_flat_top) {
    const EncoderConfig config;
    const Capture capture = transmit(config, test_bytes(1));
    const std::vector<Slot> slots = slots_of(capture);
    const Slot& first = slots[0];
    const Slot& last = slots[expected_tune_slots(config) - 1];
    // Ramp-up: silent at the start, full crest from a quarter slot on; ramp-down: the mirror image.
    CHECK(std::abs(int(capture.samples[first.begin])) <= config.amplitude / 256);
    double peak = 0.0;
    for (size_t n = first.begin + (first.end - first.begin) / 2; n < last.begin; ++n)
        peak = std::max(peak, std::fabs(double(capture.samples[n])));
    CHECK(peak >= 0.999 * config.amplitude);
    CHECK(std::abs(int(capture.samples[last.end - 1])) <= config.amplitude / 256);
    for (size_t i = 1; i + 1 < expected_tune_slots(config); ++i) {
        CHECK(slots[i].status.kind == SlotKind::tone);
        CHECK(slots[i].end - slots[i].begin == 128u);
    }
}

// U7: E1 0.6875, Em 0.5625, g_s 0.9394, g_m 0.8355 (+-0.5 %) from the encoder output.
TEST(encoder_energies_and_window_gains) {
    const EncoderConfig config = mode(128000, 8, base_rate);  // 1024 samples per slot
    const Capture capture = transmit(config, std::vector<uint8_t>(1, 0x10));  // one lone 1
    const std::vector<Slot> slots = slots_of(capture);
    const Slot* one = nullptr;
    const Slot* marker = nullptr;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].status.kind == SlotKind::one && one == nullptr) one = &slots[i];
        if (slots[i].status.kind == SlotKind::marker && marker == nullptr) marker = &slots[i];
    }
    REQUIRE(one != nullptr && marker != nullptr);
    const double a = config.amplitude;
    const auto energy = [&](const Slot& s) {
        double sum = 0.0;
        for (size_t n = s.begin; n < s.end; ++n) sum += double(capture.samples[n]) * capture.samples[n];
        return sum / (0.5 * a * a * double(s.end - s.begin));
    };
    // Envelope magnitude by coherent demodulation with the NCO (sign removed).
    const auto envelope_mean = [&](const Slot& s, double from, double to) {
        const size_t length = s.end - s.begin;
        const size_t b = s.begin + size_t(from * length);
        const size_t e = s.begin + size_t(to * length);
        Complex sum(0.0, 0.0);
        for (size_t n = b; n < e; ++n) sum += double(capture.samples[n]) * std::polar(1.0, -tone_phase(config, n));
        return 2.0 * std::abs(sum) / (a * double(e - b));
    };
    CHECK_NEAR(energy(*one), 0.6875, 0.6875 * 0.005);
    CHECK_NEAR(energy(*marker), 0.5625, 0.5625 * 0.005);
    CHECK_NEAR(envelope_mean(*one, 0.125, 0.875), 0.9394, 0.9394 * 0.005);
    CHECK_NEAR(envelope_mean(*marker, 0.15, 0.5), 0.8355, 0.8355 * 0.005);
    CHECK_NEAR(envelope_mean(*marker, 0.5, 0.85), 0.8355, 0.8355 * 0.005);
}

// Streaming: bytes written during the sync follow without a gap; an underrun at a START ends the transmission.
TEST(encoder_streaming_and_underrun_end) {
    const EncoderConfig config;
    Encoder encoder(config);
    const std::vector<uint8_t> data = test_bytes(4);
    CHECK_EQ(encoder.write(data.data(), 2), 2u);
    REQUIRE(encoder.start());
    bool wrote = false;
    size_t data_slots = 0;
    uint32_t last_slot = 0xFFFFFFFFu;
    EncoderSegment previous = EncoderSegment::idle;
    std::vector<EncoderSegment> order;
    while (encoder.busy()) {
        const EncoderStatus status = encoder.status();
        if (!wrote && status.segment == EncoderSegment::sync) wrote = encoder.write(&data[2], 2) == 2;
        if (status.segment == EncoderSegment::package && status.kind != SlotKind::marker && status.slot_index != last_slot)
            ++data_slots;
        if (status.segment != previous) order.push_back(status.segment);
        previous = status.segment;
        last_slot = status.slot_index;
        encoder.next_sample();
    }
    CHECK(wrote);
    CHECK_EQ(data_slots, 32u);
    const EncoderSegment expected[] = {EncoderSegment::tune, EncoderSegment::sync, EncoderSegment::package,
                                       EncoderSegment::end, EncoderSegment::tail};
    REQUIRE(order.size() == count_of(expected));
    for (size_t i = 0; i < order.size(); ++i) CHECK(order[i] == expected[i]);
    CHECK_EQ(encoder.next_sample(), 0);
    CHECK(!encoder.start());  // queue drained
}

// U23: a short final package ends the transmission; bytes written while it is sent wait for the next start().
TEST(encoder_short_final_package_ends_transmission) {
    EncoderConfig config;
    config.bits_per_package = 12;
    const std::vector<uint8_t> data = test_bytes(2);  // 16 bits: 12 + 4
    const std::vector<uint8_t> late = test_bytes(3, 99);
    Encoder encoder(config);
    CHECK_EQ(encoder.write(data.data(), data.size()), data.size());
    REQUIRE(encoder.start());
    bool wrote = false;
    std::vector<unsigned> package_bits;
    std::vector<EncoderSegment> after;
    uint32_t last_slot = 0xFFFFFFFFu;
    while (encoder.busy()) {
        const EncoderStatus status = encoder.status();
        const bool new_slot = status.slot_index != last_slot;
        last_slot = status.slot_index;
        if (new_slot && status.segment == EncoderSegment::package && status.slot == 1) {
            package_bits.push_back(status.package_bits);
            if (package_bits.size() == 2 && !wrote) wrote = encoder.write(late.data(), late.size()) == late.size();
        }
        if (new_slot && package_bits.size() == 2 && status.segment != EncoderSegment::package) after.push_back(status.segment);
        encoder.next_sample();
    }
    REQUIRE(package_bits.size() == 2u);
    CHECK_EQ(package_bits[0], 12u);
    CHECK_EQ(package_bits[1], 4u);
    CHECK(wrote);
    const EncoderSegment expected[] = {EncoderSegment::end, EncoderSegment::end, EncoderSegment::tail};
    REQUIRE(after.size() == count_of(expected));
    for (size_t i = 0; i < after.size(); ++i) CHECK(after[i] == expected[i]);
    // The late bytes stay queued for the next transmission.
    CHECK_EQ(encoder.queued(), late.size());
    CHECK(encoder.start());
}

TEST(encoder_abort_and_restart) {
    const EncoderConfig config;
    Encoder encoder(config);
    const std::vector<uint8_t> data = test_bytes(10);
    encoder.write(data.data(), data.size());
    REQUIRE(encoder.start());
    std::vector<int16_t> buffer(3400);  // tune 16 + sync 8 slots = 3072 samples: inside package 0
    CHECK_EQ(encoder.render(buffer.data(), buffer.size()), buffer.size());
    CHECK(encoder.status().segment == EncoderSegment::package);
    encoder.abort();
    CHECK(!encoder.busy());
    CHECK_EQ(encoder.next_sample(), 0);
    CHECK_EQ(encoder.render(buffer.data(), buffer.size()), 0u);
    CHECK(encoder.status().segment == EncoderSegment::idle);
    CHECK_EQ(encoder.queue_free(), queue_capacity);
    CHECK(!encoder.start());

    // A new transmission after an abort is identical to one from a fresh encoder (NCO and sign restart).
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
    const Encoder encoder(EncoderConfig::from_preset(Preset::hf_slow, 192000));
    CHECK_EQ(encoder.duration_samples(static_cast<size_t>(-1)), 0xFFFFFFFFu);
    CHECK_EQ(encoder.duration_samples(0xFFFFFFFFu / 8), 0xFFFFFFFFu);
    CHECK_EQ(encoder.duration_samples(0), 0u);
    // Far from saturation the formula holds: 10000 bytes of hf_slow at 192 kHz (48 min).
    const EncoderConfig config = encoder.config();
    const size_t bytes = 10000;
    CHECK(std::fabs(encoder.duration_samples(bytes) - formula_samples(config, bytes)) <= 1.0);
}
