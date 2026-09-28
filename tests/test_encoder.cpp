#include "test_harness.hpp"
#include "unlimited/encoder.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using unlimited::ConfigError;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSegment;
using unlimited::EncoderStatus;
using unlimited::Passband;
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
const unsigned window_slots = 10;
const unsigned bits_per_byte = 8;
const unsigned vox_gap_slots = 2;
const unsigned min_vox_slots = 3;
const unsigned min_tail_slots = 2;
const size_t queue_capacity = Encoder::k_queue_size;
const uint16_t vox_lead_ms = 150;

const float speeds[] = {1.0f, 2.5f, 3.0f, 3.33f, 6.0f, 7.77f, 12.0f, 25.0f};
const uint32_t test_rates[] = {8000, 11025, 44100, 48000};

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
    unsigned slot;        // window: 0..9
    unsigned byte_index;  // window: the byte's position
};

// A config at `speed` bytes/s, pitch 1500 Hz, the AM passband so that every speed fits.
EncoderConfig at_speed(float speed, uint32_t rate = base_rate) {
    EncoderConfig config;
    config.sample_rate_hz = rate;
    config.slot_us = unlimited::slot_us_for_speed(speed);
    config.passband.low_hz = unlimited::k_am_passband_low_hz;
    config.passband.high_hz = unlimited::k_am_passband_high_hz;
    return config;
}

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

uint32_t ceil_div(uint64_t a, uint64_t b) {
    return static_cast<uint32_t>((a + b - 1) / b);
}

uint32_t lead_in_slots(const EncoderConfig& config) {
    return ceil_div(static_cast<uint64_t>(config.lead_in_ms) * us_per_ms, config.slot_us);
}

uint32_t vox_slots(const EncoderConfig& config) {
    if (config.vox_lead_ms == 0) return 0;
    return std::max<uint32_t>(ceil_div(static_cast<uint64_t>(config.vox_lead_ms) * us_per_ms, config.slot_us),
                              min_vox_slots);
}

std::vector<Expected> expected_slots(const EncoderConfig& config, const std::vector<uint8_t>& data) {
    std::vector<Expected> slots;
    const Expected silent = {EncoderSegment::lead_in, SlotKind::silent, 0, 0};
    slots.insert(slots.end(), lead_in_slots(config), silent);
    const uint32_t vox = vox_slots(config);
    for (uint32_t i = 0; i < vox; ++i) {
        const Expected lead = {EncoderSegment::vox_lead, SlotKind::lead, i == 0 ? 0u : (i + 1 == vox ? 2u : 1u), 0};
        slots.push_back(lead);
    }
    if (vox > 0) {
        const Expected gap = {EncoderSegment::gap, SlotKind::silent, 0, 0};
        slots.insert(slots.end(), vox_gap_slots, gap);
    }
    for (size_t b = 0; b < data.size(); ++b) {
        const Expected start = {EncoderSegment::window, SlotKind::start, 0, static_cast<unsigned>(b)};
        slots.push_back(start);
        for (unsigned j = 1; j <= bits_per_byte; ++j) {
            const bool one = ((data[b] >> (bits_per_byte - j)) & 1u) != 0;
            const Expected bit = {EncoderSegment::window, one ? SlotKind::one : SlotKind::zero, j,
                                  static_cast<unsigned>(b)};
            slots.push_back(bit);
        }
        const Expected stop = {EncoderSegment::window, SlotKind::stop, window_slots - 1, static_cast<unsigned>(b)};
        slots.push_back(stop);
    }
    const Expected tail = {EncoderSegment::tail, SlotKind::silent, 0, 0};
    slots.push_back(tail);
    return slots;
}

// Spec 2.4: lead + (N_vox + gap + 10 n) T + max(tail, 2 T), in samples.
double formula_samples(const EncoderConfig& config, size_t bytes) {
    const double rate = config.sample_rate_hz;
    const uint32_t vox = vox_slots(config);
    const double slots = (vox > 0 ? vox + vox_gap_slots : 0) + static_cast<double>(bytes) * window_slots;
    const double tail = std::max(config.tail_ms * rate / ms_per_s, min_tail_slots * config.slot_us * rate / us_per_s);
    return config.lead_in_ms * rate / ms_per_s + slots * config.slot_us * rate / us_per_s + tail;
}

// The same in whole samples as the encoder renders it: the lead-in and the slots end on the first sample at or after
// their exact end (ceil), then the tail: tail_ms rounded down, or 2 slots rounded up when that is longer.
uint64_t exact_samples(const EncoderConfig& config, size_t bytes) {
    const uint64_t rate = config.sample_rate_hz;
    const uint32_t vox = vox_slots(config);
    const uint64_t slots = (vox > 0 ? vox + vox_gap_slots : 0) + static_cast<uint64_t>(bytes) * window_slots;
    const uint64_t slotted_us = static_cast<uint64_t>(config.lead_in_ms) * us_per_ms + slots * config.slot_us;
    const uint64_t tail = std::max<uint64_t>(config.tail_ms * rate / ms_per_s,
                                             (min_tail_slots * config.slot_us * rate + us_per_s - 1) / us_per_s);
    return (slotted_us * rate + us_per_s - 1) / us_per_s + tail;
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

// The encoder's NCO phase at sample n (phase 0 at start(), running through silent slots too).
double tone_phase(const EncoderConfig& config, size_t n) {
    const uint64_t step = ((static_cast<uint64_t>(config.tone_hz) << 32) + config.sample_rate_hz / 2) /
                          config.sample_rate_hz;
    return two_pi * static_cast<double>(static_cast<uint32_t>(step * n)) / turn;
}

// Correlation with the carrier over [begin, end): positive for a beep of the one phase-continuous carrier.
double project_on_tone(const EncoderConfig& config, const Capture& capture, size_t begin, size_t end) {
    double sum = 0.0;
    for (size_t n = begin; n < end; ++n) sum += capture.samples[n] * std::sin(tone_phase(config, n));
    return sum;
}

bool is_tone(SlotKind kind) {
    return kind == SlotKind::start || kind == SlotKind::one || kind == SlotKind::stop || kind == SlotKind::lead;
}

char kind_char(SlotKind kind) {
    switch (kind) {
    case SlotKind::silent: return '_';
    case SlotKind::lead: return '~';
    case SlotKind::start: return 'S';
    case SlotKind::one: return '1';
    case SlotKind::zero: return '0';
    case SlotKind::stop: return 'E';
    }
    return '?';
}

}  // namespace

// ---------------------------------------------------------------------------
// U1-U7: the encoder (spec 1, 2)
// ---------------------------------------------------------------------------

TEST(encoder_config_defaults) {
    const EncoderConfig config;
    CHECK_EQ(config.sample_rate_hz, base_rate);
    CHECK_EQ(config.slot_us, 16667u);  // 6 bytes/s
    CHECK_EQ(config.slot_us, unlimited::slot_us_for_speed(unlimited::k_default_bytes_per_second));
    CHECK_EQ(config.tone_hz, 1500);
    CHECK_EQ(config.amplitude, default_amplitude);
    CHECK_EQ(config.passband.low_hz, 300);
    CHECK_EQ(config.passband.high_hz, 2700);
    CHECK_EQ(config.lead_in_ms, 0);
    CHECK_EQ(config.vox_lead_ms, 0);
    CHECK_EQ(config.tail_ms, 100);
    CHECK(config.valid());
    CHECK(config.check() == ConfigError::none);
    // Every speed at every rate is valid, in the AM passband and in the default 2.4 kHz SSB one (25 bytes/s: 950..2050
    // Hz).
    for (size_t v = 0; v < count_of(speeds); ++v) {
        for (size_t r = 0; r < count_of(test_rates); ++r) CHECK(at_speed(speeds[v], test_rates[r]).valid());
        EncoderConfig ssb;
        ssb.slot_us = unlimited::slot_us_for_speed(speeds[v]);
        CHECK(ssb.valid());
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
        {"T 3999 us (25.01 bytes/s)", ConfigError::slot, [](EncoderConfig& c) { c.slot_us = 3999; }},
        {"T 100001 us (0.99 bytes/s)", ConfigError::slot, [](EncoderConfig& c) { c.slot_us = 100001; }},
        {"T 0", ConfigError::slot, [](EncoderConfig& c) { c.slot_us = 0; }},
        {"passband inverted", ConfigError::passband,
         [](EncoderConfig& c) {
             c.passband.low_hz = 2700;
             c.passband.high_hz = 300;
         }},
        {"passband above 4000", ConfigError::passband, [](EncoderConfig& c) { c.passband.high_hz = 4001; }},
        {"tone 400 Hz at 6 bytes/s", ConfigError::outside_passband, [](EncoderConfig& c) { c.tone_hz = 400; }},
        {"25 bytes/s in 1250-1750 Hz", ConfigError::outside_passband,
         [](EncoderConfig& c) {
             c.slot_us = 4000;
             c.passband.low_hz = 1250;
             c.passband.high_hz = 1750;
         }},
        {"amplitude 0", ConfigError::amplitude, [](EncoderConfig& c) { c.amplitude = 0; }},
        {"amplitude -1", ConfigError::amplitude, [](EncoderConfig& c) { c.amplitude = -1; }},
    };
    for (size_t i = 0; i < count_of(cases); ++i) {
        EncoderConfig config;
        cases[i].change(config);
        if (!CHECK(config.check() == cases[i].expected)) NOTE("case %s", cases[i].name);
        CHECK(!config.valid());
    }
    // The limits themselves are valid: 1.00 and 25.00 bytes/s.
    EncoderConfig limit = at_speed(1.0f);
    CHECK_EQ(limit.slot_us, unlimited::k_max_slot_us);
    CHECK(limit.check() == ConfigError::none);
    limit = at_speed(25.0f);
    CHECK_EQ(limit.slot_us, unlimited::k_min_slot_us);
    CHECK(limit.check() == ConfigError::none);
    // Earlier rules win: a bad rate and a bad tone report the rate.
    EncoderConfig both;
    both.sample_rate_hz = 1;
    both.tone_hz = 1;
    CHECK(both.check() == ConfigError::sample_rate);
}

TEST(encoder_occupied_band_of_config) {
    const EncoderConfig hf;
    const unlimited::Band band = unlimited::occupied_band(hf);
    CHECK_EQ(band.low_hz, 1368);
    CHECK_EQ(band.high_hz, 1632);
    CHECK_EQ(band.width_hz, 264);
    const unlimited::PassbandFit fit = unlimited::passband_fit(hf);
    CHECK(fit.fits);
    CHECK_EQ(fit.margin_low_hz, 1068);
    CHECK_EQ(fit.margin_high_hz, 1068);
    CHECK_EQ(fit.tolerance_hz, 1068);
    // 25 bytes/s in the FM passband 300..3000 Hz: 650 Hz of filter below, 950 Hz above; the receiver searches
    // 850..2450 Hz (the passband less half the band), so the shift is the same.
    EncoderConfig fm;
    fm.slot_us = unlimited::slot_us_for_speed(25.0f);
    fm.passband.low_hz = unlimited::k_fm_passband_low_hz;
    fm.passband.high_hz = unlimited::k_fm_passband_high_hz;
    const unlimited::PassbandFit fm_room = unlimited::passband_fit(unlimited::occupied_band(fm), fm.passband);
    CHECK_EQ(fm_room.margin_low_hz, 650);
    CHECK_EQ(fm_room.margin_high_hz, 950);
    const Passband fm_search = unlimited::search_range(fm);
    CHECK_EQ(fm_search.low_hz, 850);
    CHECK_EQ(fm_search.high_hz, 2450);
    const unlimited::PassbandFit fm_fit = unlimited::passband_fit(fm);
    CHECK(fm_fit.fits);
    CHECK_EQ(fm_fit.margin_low_hz, 650);
    CHECK_EQ(fm_fit.margin_high_hz, 950);
    CHECK_EQ(fm_fit.tolerance_hz, 650);
}

TEST(encoder_start_rules) {
    Encoder encoder{EncoderConfig()};
    CHECK(!encoder.busy());
    CHECK_EQ(encoder.next_sample(), 0);
    CHECK(encoder.status().segment == EncoderSegment::idle);
    CHECK(!encoder.start());  // empty queue
    CHECK(encoder.write(0x42));
    CHECK(encoder.start());
    CHECK(encoder.busy());
    CHECK(!encoder.start());  // busy

    EncoderConfig bad;
    bad.slot_us = 0;
    Encoder invalid(bad);
    CHECK(invalid.write(0x42));
    CHECK(!invalid.start());
    CHECK_EQ(invalid.duration_samples(1), 0u);
    CHECK_EQ(encoder.duration_samples(0), 0u);
    CHECK(encoder.config().slot_us == EncoderConfig().slot_us);
}

// The queue holds exactly k_queue_size bytes (free-running indices, spec 2.5); a byte stays queued until its STOP ends.
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
    Encoder streaming(at_speed(25.0f));
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
        if (status.segment == EncoderSegment::window && status.slot_index != last_slot &&
            status.slot == unlimited::k_start_slot) {
            if (!CHECK_EQ(unsigned(status.byte), unsigned(stream[bytes]))) break;
            CHECK_EQ(status.byte_index, uint32_t(bytes));
            CHECK(streaming.queued() >= 1u);  // the byte being sent is still queued
            ++bytes;
        }
        last_slot = status.slot_index;
        streaming.next_sample();
    }
    CHECK_EQ(bytes, stream.size());
    CHECK_EQ(streaming.queued(), 0u);
}

// U4: total length = the spec 2.4 formula (within 2 samples: the slots and the tail's 2-slot minimum each end on the
// next whole sample) for every speed, lead-in, VOX lead and tail at four rates; duration_samples() gives the rendered
// length exactly.
TEST(encoder_total_length_all_speeds_and_rates) {
    const size_t sizes[] = {1, 3, 17};
    const uint16_t leads_ms[] = {0, 7, 100};
    const uint16_t voxes_ms[] = {0, 10, vox_lead_ms};
    const uint16_t tails_ms[] = {0, 100};
    size_t checked = 0;
    double worst = 0.0;
    for (size_t r = 0; r < count_of(test_rates); ++r) {
        for (size_t v = 0; v < count_of(speeds); ++v) {
            for (size_t l = 0; l < count_of(leads_ms); ++l) {
                for (size_t x = 0; x < count_of(voxes_ms); ++x) {
                    for (size_t t = 0; t < count_of(tails_ms); ++t) {
                        EncoderConfig config = at_speed(speeds[v], test_rates[r]);
                        config.lead_in_ms = leads_ms[l];
                        config.vox_lead_ms = voxes_ms[x];
                        config.tail_ms = tails_ms[t];
                        const size_t size = sizes[(v + l + x + t) % count_of(sizes)];
                        const std::vector<uint8_t> data = test_bytes(size, uint32_t(v * 100 + l * 10 + x));
                        const double length = static_cast<double>(rendered_length(config, data, 4096));
                        const double error = std::fabs(length - formula_samples(config, data.size()));
                        worst = std::max(worst, error);
                        if (!CHECK(error < 2.0) || !CHECK_EQ(uint64_t(length), exact_samples(config, data.size())) ||
                            !CHECK_EQ(duration_of(config, data.size()), size_t(length))) {
                            NOTE("%.2f bytes/s rate %u lead %u vox %u tail %u: %.0f vs %.2f",
                                 static_cast<double>(speeds[v]), test_rates[r], leads_ms[l], voxes_ms[x], tails_ms[t],
                                 length, formula_samples(config, data.size()));
                            return;
                        }
                        ++checked;
                    }
                }
            }
        }
    }
    NOTE("%zu transmissions, largest |length - formula| %.3f samples", checked, worst);
    // Spec 2.2: "Hi" at 6 bytes/s is 20 slots (333 ms, 2667 samples) and the 100 ms tail: 3467 samples.
    CHECK_EQ(duration_of(EncoderConfig(), 2), 3467u);
    // A tail shorter than 2 slots is stretched to 2 slots (spec 2.1).
    EncoderConfig short_tail = at_speed(1.0f);
    short_tail.tail_ms = 50;
    CHECK_EQ(duration_of(short_tail, 1), 8000u + 1600u);
}

TEST(encoder_slot_drift_over_ten_thousand_slots) {
    struct Case {
        uint32_t rate;
        float speed;
        uint16_t lead_ms;
    };
    const Case cases[] = {{44100, 3.0f, 300}, {11025, 3.33f, 300}, {48000, 12.0f, 0}, {8000, 25.0f, 7}};
    const uint32_t slots_wanted = 10000;
    for (size_t c = 0; c < count_of(cases); ++c) {
        EncoderConfig config = at_speed(cases[c].speed, cases[c].rate);
        config.lead_in_ms = cases[c].lead_ms;
        REQUIRE(config.valid());
        const std::vector<uint8_t> data = test_bytes(slots_wanted / window_slots + 1);
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
        NOTE("rate %u, %.2f bytes/s: %u slots, max |start - exact| %.4f samples", cases[c].rate,
             static_cast<double>(cases[c].speed), slot, worst);
    }
}

// U21, U23: every slot of a transmission as spec 2.1 says (lead-in, VOX lead, gap, windows, tail), with the status
// fields of spec 5.
TEST(encoder_segments_and_status) {
    struct Case {
        float speed;
        size_t bytes;
        uint16_t lead_ms;
        uint16_t vox_ms;
    };
    const Case cases[] = {
        {6.0f, 5, 0, 0}, {3.0f, 4, 100, vox_lead_ms}, {25.0f, 3, 300, vox_lead_ms}, {1.0f, 2, 0, 0}, {12.0f, 5, 0, 20}};
    for (size_t c = 0; c < count_of(cases); ++c) {
        EncoderConfig config = at_speed(cases[c].speed);
        config.lead_in_ms = cases[c].lead_ms;
        config.vox_lead_ms = cases[c].vox_ms;
        REQUIRE(config.valid());
        const std::vector<uint8_t> data = test_bytes(cases[c].bytes, uint32_t(c + 1));
        const Capture capture = transmit(config, data);
        const std::vector<Slot> slots = slots_of(capture);
        const std::vector<Expected> expected = expected_slots(config, data);
        REQUIRE(slots.size() == expected.size());
        for (size_t i = 0; i < slots.size(); ++i) {
            const EncoderStatus& s = slots[i].status;
            const Expected& e = expected[i];
            bool ok = CHECK(s.segment == e.segment) && CHECK(s.kind == e.kind);
            if (e.segment == EncoderSegment::window) {
                const unsigned bit = e.slot >= 1 && e.slot <= bits_per_byte ? e.slot - 1 : 0;
                ok = ok && CHECK_EQ(unsigned(s.slot), e.slot) && CHECK_EQ(unsigned(s.byte), unsigned(data[e.byte_index])) &&
                     CHECK_EQ(s.byte_index, uint32_t(e.byte_index)) && CHECK_EQ(unsigned(s.bit_index), bit);
            } else {
                ok = ok && CHECK_EQ(unsigned(s.slot), 0u) && CHECK_EQ(unsigned(s.byte), 0u) &&
                     CHECK_EQ(s.byte_index, 0u) && CHECK_EQ(unsigned(s.bit_index), 0u);
            }
            ok = ok && CHECK_EQ(s.slot_index, uint32_t(i));
            if (!ok) {
                NOTE("case %zu slot %zu", c, i);
                break;
            }
        }
        CHECK_EQ(capture.status.back().samples_rendered, uint32_t(capture.samples.size() - 1));
        // The lead-in, the gap and the tail are digital silence.
        for (size_t i = 0; i < slots.size(); ++i) {
            const EncoderSegment segment = slots[i].status.segment;
            if (segment != EncoderSegment::lead_in && segment != EncoderSegment::gap && segment != EncoderSegment::tail)
                continue;
            for (size_t n = slots[i].begin; n < slots[i].end; ++n) {
                if (!CHECK_EQ(capture.samples[n], 0)) {
                    NOTE("case %zu slot %zu", c, i);
                    break;
                }
            }
        }
    }
}

// U5: spec 2.2's "Hi" at 6 bytes/s, bit-exact: START, the bits MSB first, STOP, twice; every beep on the one
// phase-continuous carrier (phase 0 at start()); then the tail.
TEST(encoder_hi_slot_sequence) {
    const EncoderConfig config;
    const std::vector<uint8_t> hi = {0x48, 0x69};
    const Capture capture = transmit(config, hi);
    const std::vector<Slot> slots = slots_of(capture);
    REQUIRE(slots.size() == 21u);  // 20 slots and the tail
    std::string kinds;
    for (size_t i = 0; i < slots.size(); ++i) kinds += kind_char(slots[i].status.kind);
    CHECK_EQ(kinds, std::string("S01001000ES01101001E_"));
    // Slot j starts at sample ceil(j * 133.336) (T = 16667 us at 8000 Hz).
    for (size_t j = 0; j < 20; ++j) CHECK_EQ(slots[j].begin, size_t(ceil_div(uint64_t(j) * 16667 * 8, 1000)));
    for (size_t i = 0; i < 20; ++i) {
        if (!is_tone(slots[i].status.kind)) continue;
        const Slot& s = slots[i];
        const size_t middle = s.begin + (s.end - s.begin) / 2;
        const double first = project_on_tone(config, capture, s.begin, middle);
        const double second = project_on_tone(config, capture, middle, s.end);
        if (!CHECK(first > 0.0 && second > 0.0)) NOTE("slot %zu", i);
    }
    CHECK(slots[20].status.segment == EncoderSegment::tail);
    CHECK_EQ(capture.samples.size(), 3467u);
    CHECK_EQ(slots[20].end - slots[20].begin, 800u);
}

// U5: |y| <= A, quiet beep edges, no phase reversal anywhere (every tone slot on the one carrier), at several rates and
// speeds, with a VOX lead.
TEST(encoder_waveform_bounds_edges_and_phase) {
    EncoderConfig configs[] = {EncoderConfig(), at_speed(25.0f, 48000), at_speed(3.33f, 11025), at_speed(12.0f),
                               at_speed(1.0f, 44100)};
    configs[1].vox_lead_ms = vox_lead_ms;
    configs[3].vox_lead_ms = 20;
    for (size_t c = 0; c < count_of(configs); ++c) {
        const EncoderConfig& config = configs[c];
        const std::vector<uint8_t> data = test_bytes(9, uint32_t(c + 3));
        const Capture capture = transmit(config, data);
        const std::vector<Slot> slots = slots_of(capture);
        const int limit = config.amplitude;
        // A beep's first and last samples lie within the first two samples of its ramp: A sin^2(2 pi 2 / L).
        const double slot_samples = config.slot_us * double(config.sample_rate_hz) / us_per_s;
        const double ramp = std::sin(two_pi * 2.0 / slot_samples);
        const double edge_limit = config.amplitude * ramp * ramp;
        size_t beeps = 0;
        bool ok = true;
        for (size_t i = 0; i < slots.size() && ok; ++i) {
            const Slot& s = slots[i];
            for (size_t n = s.begin; n < s.end; ++n) ok = ok && CHECK(std::abs(int(capture.samples[n])) <= limit);
            const SlotKind kind = s.status.kind;
            if (!is_tone(kind)) continue;
            if (kind != SlotKind::lead) {
                ok = ok && CHECK(std::abs(double(capture.samples[s.begin])) <= edge_limit) &&
                     CHECK(std::abs(double(capture.samples[s.end - 1])) <= edge_limit);
                ++beeps;
            }
            const size_t middle = s.begin + (s.end - s.begin) / 2;
            ok = ok && CHECK(project_on_tone(config, capture, s.begin, middle) > 0.0) &&
                 CHECK(project_on_tone(config, capture, middle, s.end) > 0.0);
            if (!ok) NOTE("config %zu slot %zu", c, i);
        }
        CHECK(beeps >= 2 * data.size());
    }
}

// Spec 2.1: the VOX lead is one steady tone, ramped at both ends (a quarter slot each), then 2 silent slots, then
// the START.
TEST(encoder_vox_lead_ramps_and_flat_top) {
    EncoderConfig config;
    config.vox_lead_ms = vox_lead_ms;
    const Capture capture = transmit(config, test_bytes(1));
    const std::vector<Slot> slots = slots_of(capture);
    const uint32_t vox = vox_slots(config);
    CHECK_EQ(vox, 9u);  // 150 ms / 16.667 ms
    const Slot& first = slots[0];
    const Slot& last = slots[vox - 1];
    CHECK(first.status.segment == EncoderSegment::vox_lead && last.status.segment == EncoderSegment::vox_lead);
    CHECK(std::abs(int(capture.samples[first.begin])) <= config.amplitude / 256);
    double peak = 0.0;
    for (size_t n = first.begin + (first.end - first.begin) / 2; n < last.begin; ++n)
        peak = std::max(peak, std::fabs(double(capture.samples[n])));
    CHECK(peak >= 0.999 * config.amplitude);
    CHECK(std::abs(int(capture.samples[last.end - 1])) <= config.amplitude / 256);
    // The flat top holds the crest between the ramps: no dip at the slot edges inside the lead.
    for (uint32_t i = 1; i + 1 < vox; ++i) {
        const Slot& s = slots[i];
        double slot_peak = 0.0;
        for (size_t n = s.begin; n < s.begin + 8; ++n) slot_peak = std::max(slot_peak, std::fabs(double(capture.samples[n])));
        CHECK(slot_peak >= 0.9 * config.amplitude);
    }
    for (uint32_t g = 0; g < vox_gap_slots; ++g) {
        const Slot& s = slots[vox + g];
        CHECK(s.status.segment == EncoderSegment::gap);
        for (size_t n = s.begin; n < s.end; ++n) CHECK_EQ(capture.samples[n], 0);
    }
    CHECK(slots[vox + vox_gap_slots].status.kind == SlotKind::start);
    // A lead shorter than 3 slots is stretched to 3 (the receiver tells it from a beep by its slot edges).
    EncoderConfig brief = at_speed(1.0f);
    brief.vox_lead_ms = 10;
    CHECK_EQ(vox_slots(brief), min_vox_slots);
    const std::vector<Slot> brief_slots = slots_of(transmit(brief, test_bytes(1)));
    size_t leads = 0;
    for (size_t i = 0; i < brief_slots.size(); ++i) leads += brief_slots[i].status.segment == EncoderSegment::vox_lead;
    CHECK_EQ(leads, size_t(min_vox_slots));
}

// U7: a beep's energy 0.6875 (T A^2 / 2 units) and its central 75 % envelope mean 0.9394 (+-0.5 %), the START, a 1
// and the STOP alike.
TEST(encoder_energies_and_window_gains) {
    const EncoderConfig config = at_speed(1.0f);  // 800 samples per slot
    const Capture capture = transmit(config, std::vector<uint8_t>(1, 0x10));  // START 0001 0000 STOP
    const std::vector<Slot> slots = slots_of(capture);
    const double a = config.amplitude;
    const auto energy = [&](const Slot& s) {
        double sum = 0.0;
        for (size_t n = s.begin; n < s.end; ++n) sum += double(capture.samples[n]) * capture.samples[n];
        return sum / (0.5 * a * a * double(s.end - s.begin));
    };
    // Envelope magnitude by coherent demodulation with the NCO.
    const auto envelope_mean = [&](const Slot& s, double from, double to) {
        const size_t length = s.end - s.begin;
        const size_t b = s.begin + size_t(from * length);
        const size_t e = s.begin + size_t(to * length);
        Complex sum(0.0, 0.0);
        for (size_t n = b; n < e; ++n) sum += double(capture.samples[n]) * std::polar(1.0, -tone_phase(config, n));
        return 2.0 * std::abs(sum) / (a * double(e - b));
    };
    size_t beeps = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (!is_tone(slots[i].status.kind)) continue;
        CHECK_NEAR(energy(slots[i]), unlimited::k_beep_energy, unlimited::k_beep_energy * 0.005);
        CHECK_NEAR(envelope_mean(slots[i], 0.125, 0.875), 0.9394, 0.9394 * 0.005);
        ++beeps;
    }
    CHECK_EQ(beeps, 3u);
}

// Spec 2.3: bytes written while the transmission runs follow without a gap, up to the last window's STOP; an empty
// queue at a window boundary sends the tail and ends the transmission.
TEST(encoder_streaming_and_end_at_empty_queue) {
    const EncoderConfig config;
    Encoder encoder(config);
    const std::vector<uint8_t> data = test_bytes(4);
    CHECK_EQ(encoder.write(data.data(), 1), 1u);
    REQUIRE(encoder.start());
    size_t written = 1;
    std::vector<EncoderSegment> order;
    std::vector<uint8_t> sent;
    EncoderSegment previous = EncoderSegment::idle;
    uint32_t last_slot = 0xFFFFFFFFu;
    while (encoder.busy()) {
        const EncoderStatus status = encoder.status();
        const bool new_slot = status.slot_index != last_slot || status.segment != previous;
        // One more byte during each window's STOP: in time for the next window.
        if (new_slot && status.segment == EncoderSegment::window && status.slot == unlimited::k_stop_slot &&
            written < data.size()) {
            written += encoder.write(&data[written], 1);
        }
        if (new_slot && status.segment == EncoderSegment::window && status.slot == unlimited::k_start_slot)
            sent.push_back(status.byte);
        if (status.segment != previous) order.push_back(status.segment);
        previous = status.segment;
        last_slot = status.slot_index;
        encoder.next_sample();
    }
    CHECK(sent == data);
    const EncoderSegment expected[] = {EncoderSegment::window, EncoderSegment::tail};
    REQUIRE(order.size() == count_of(expected));
    for (size_t i = 0; i < order.size(); ++i) CHECK(order[i] == expected[i]);
    CHECK_EQ(encoder.next_sample(), 0);
    CHECK(!encoder.start());  // queue drained

    // A byte written once the tail has begun waits for the next start().
    Encoder late(config);
    late.write(0x41);
    REQUIRE(late.start());
    bool wrote = false;
    size_t windows = 0;
    last_slot = 0xFFFFFFFFu;
    while (late.busy()) {
        const EncoderStatus status = late.status();
        if (!wrote && status.segment == EncoderSegment::tail) wrote = late.write(0x42);
        if (status.segment == EncoderSegment::window && status.slot == unlimited::k_start_slot &&
            status.slot_index != last_slot)
            ++windows;
        last_slot = status.slot_index;
        late.next_sample();
    }
    CHECK(wrote);
    CHECK_EQ(windows, 1u);
    CHECK_EQ(late.queued(), 1u);
    CHECK(late.start());
    CHECK_EQ(unsigned(late.status().byte), 0x42u);
}

TEST(encoder_abort_and_restart) {
    const EncoderConfig config;
    Encoder encoder(config);
    const std::vector<uint8_t> data = test_bytes(10);
    encoder.write(data.data(), data.size());
    REQUIRE(encoder.start());
    std::vector<int16_t> buffer(2000);  // inside window 1
    CHECK_EQ(encoder.render(buffer.data(), buffer.size()), buffer.size());
    CHECK(encoder.status().segment == EncoderSegment::window);
    CHECK_EQ(encoder.status().byte_index, 1u);
    encoder.abort();
    CHECK(!encoder.busy());
    CHECK_EQ(encoder.next_sample(), 0);
    CHECK_EQ(encoder.render(buffer.data(), buffer.size()), 0u);
    CHECK(encoder.status().segment == EncoderSegment::idle);
    CHECK_EQ(encoder.queue_free(), queue_capacity);
    CHECK(!encoder.start());

    // A new transmission after an abort is identical to one from a fresh encoder (the carrier restarts at phase 0).
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
    EncoderConfig config = at_speed(25.0f, 11025);
    config.vox_lead_ms = vox_lead_ms;
    config.lead_in_ms = 13;
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
    const Encoder encoder(at_speed(1.0f, 192000));
    CHECK_EQ(encoder.duration_samples(static_cast<size_t>(-1)), 0xFFFFFFFFu);
    CHECK_EQ(encoder.duration_samples(0xFFFFFFFFu / 8), 0xFFFFFFFFu);
    CHECK_EQ(encoder.duration_samples(0), 0u);
    // Far from saturation the formula holds: 2000 bytes at 1 byte/s and 192 kHz (33 min).
    const EncoderConfig config = encoder.config();
    const size_t bytes = 2000;
    CHECK(std::fabs(encoder.duration_samples(bytes) - formula_samples(config, bytes)) < 2.0);
    CHECK_EQ(uint64_t(encoder.duration_samples(bytes)), exact_samples(config, bytes));
}
