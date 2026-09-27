#include "resampler.hpp"
#include "support/loopback.hpp"
#include "test_harness.hpp"
#include "unlimited/packet.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace unlimited;
using namespace unlimited::loopback;

namespace {

const double k_attenuation_30_db = 0.0316227766;
const double k_gap_ms = 500.0;
const double k_slot_tolerance = 0.005;  // L1: measured T within 0.5 %
const double k_gate_margin_db = 3.0;

const Preset k_presets[] = {Preset::hf_slow, Preset::hf, Preset::hf_fast, Preset::am, Preset::fm};

// Spec 4.1 release gates (key-down SNR in 2500 Hz); other T: +3 dB per halving of T.
double gate_db(double slot_ms) {
    if (std::fabs(slot_ms - 4.0) < 1e-6) return 8.0;
    if (std::fabs(slot_ms - 128.0) < 1e-6) return -6.5;
    return 1.5 - 10.0 * std::log10(slot_ms / 16.0);
}

// The receiver whose window holds T: fm below 8 ms, ssb to 64 ms, a 16..128 ms window above.
DecoderConfig receiver_for(double slot_ms) {
    if (slot_ms < 8.0) return DecoderConfig::for_profile(Profile::fm);
    DecoderConfig config = DecoderConfig::for_profile(Profile::ssb);
    if (slot_ms > 64.0) config.min_slot_ms = 16;
    return config;
}

Profile profile_of(Preset preset) {
    if (preset == Preset::fm) return Profile::fm;
    if (preset == Preset::am) return Profile::am;
    return Profile::ssb;
}

double preset_slot_ms(Preset preset) {
    return preset_config(preset).slot_us / 1000.0;
}

void report(const char* label, const Score& s) {
    NOTE("%s: sent %zu released %zu wrong %zu bits %zu lost %zu extra %zu locks %zu late %zu lost-events %zu "
         "ends %zu flywheel %zu",
         label, s.bytes_sent, s.bytes_released, s.wrong_bytes, s.bit_errors, s.lost_bytes, s.extra_bytes, s.locks,
         s.late_joins, s.lost_events, s.ends, s.flywheel_bytes);
}

std::size_t find_event(const Capture& capture, EventType type, std::size_t from = 0) {
    for (std::size_t i = from; i < capture.events.size(); ++i) {
        if (capture.events[i].type == type) return i;
    }
    return capture.events.size();
}

// The first package event's T and the N of the locked event (0 when none).
struct Learnt {
    float slot_ms;
    std::uint8_t bits;
};

Learnt learnt(const Capture& capture) {
    Learnt l = {0.0f, 0};
    const std::size_t locked = find_event(capture, EventType::locked);
    if (locked < capture.events.size()) {
        l.slot_ms = capture.events[locked].slot_ms;
        l.bits = capture.events[locked].bits_per_package;
    }
    return l;
}

// A transmission of `bytes` random bytes alone in silence, through the usb channel when snr_db < 99.
Capture run(const EncoderConfig& config, const std::vector<std::uint8_t>& data, const DecoderConfig& receiver,
            double snr_db, std::uint32_t seed, Recording& recording) {
    recording = single(data, config);
    const std::vector<std::int16_t> samples =
        snr_db < 99.0 ? usb(recording.samples, snr_db, seed, config.amplitude) : recording.samples;
    return run_decoder(samples, receiver, 4096);
}

// The input driven `gain` times too hot into a 16-bit converter.
std::vector<std::int16_t> clipped(const std::vector<std::int16_t>& samples, double gain) {
    std::vector<std::int16_t> out(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double value = gain * samples[i];
        out[i] = static_cast<std::int16_t>(std::lround(std::max(-32768.0, std::min(32767.0, value))));
    }
    return out;
}

// The receiver retuned from sample `from` on: a step of step_hz plus a drift of ramp_hz_per_s, applied to the analytic
// signal (a 255-tap Hilbert transformer; the output is delayed by its 127 samples).
std::vector<std::int16_t> retuned(const std::vector<std::int16_t>& x, std::size_t from, double step_hz,
                                  double ramp_hz_per_s) {
    const double pi = 3.14159265358979323846;
    const int taps = 255;
    const int delay = (taps - 1) / 2;
    const double hamming_a = 0.54;
    const double hamming_b = 0.46;
    std::vector<double> h(taps, 0.0);
    for (int n = 0; n < taps; ++n) {
        const int k = n - delay;
        if (k % 2 != 0) h[n] = 2.0 / (pi * k) * (hamming_a - hamming_b * std::cos(2.0 * pi * n / (taps - 1)));
    }
    std::vector<std::int16_t> y(x.size(), 0);
    double phase = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        double quadrature = 0.0;
        for (int j = 0; j < taps && j <= static_cast<int>(n); ++j) quadrature += h[j] * x[n - j];
        const double in_phase = static_cast<long>(n) - delay >= 0 ? x[n - delay] : 0.0;
        const double value = in_phase * std::cos(phase) - quadrature * std::sin(phase);
        y[n] = static_cast<std::int16_t>(std::lround(std::max(-32768.0, std::min(32767.0, value))));
        if (n < from) continue;
        const double hz = step_hz + ramp_hz_per_s * static_cast<double>(n - from) / k_decoder_rate_hz;
        phase = std::fmod(phase + 2.0 * pi * hz / k_decoder_rate_hz, 2.0 * pi);
    }
    return y;
}

// No byte outside locked .. end/lost.
bool bytes_inside_locks(const Capture& capture) {
    bool locked = false;
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        switch (capture.events[i].type) {
        case EventType::locked:
            locked = true;
            break;
        case EventType::lost:
        case EventType::end:
            locked = false;
            break;
        case EventType::byte:
            if (!locked) return false;
            break;
        case EventType::state:
        case EventType::slot:
        case EventType::package:
            break;
        }
    }
    return true;
}

// The mean snr_db of the byte events, and how many there were.
double reported_snr(const Capture& capture, std::size_t& count) {
    double sum = 0.0;
    count = 0;
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        if (capture.events[i].type != EventType::byte) continue;
        sum += capture.events[i].snr_db;
        ++count;
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

bool same_event(const Event& a, const Event& b) {
    return a.type == b.type && a.reason == b.reason && a.state == b.state && a.flags == b.flags && a.value == b.value &&
           a.slot == b.slot && a.bits_per_package == b.bits_per_package && a.level_pct == b.level_pct &&
           a.threshold_pct == b.threshold_pct && a.start_pct == b.start_pct && a.stop_pct == b.stop_pct &&
           std::memcmp(a.soft, b.soft, sizeof(a.soft)) == 0 && a.package_index == b.package_index &&
           a.byte_index == b.byte_index && a.tone_hz == b.tone_hz && a.slot_ms == b.slot_ms && a.snr_db == b.snr_db;
}

}  // namespace

// U15: the memory gates (also static_asserts in decoder.cpp and encoder.cpp, built by check_embedded).
TEST(decoder_sizes) {
    NOTE("cap %u: sizeof(Decoder) %zu B (gate %u), sizeof(Event) %zu B", unsigned(k_max_bits_per_package),
         sizeof(Decoder), unsigned(7168 + 576 * k_max_bits_per_package), sizeof(Event));
    CHECK(sizeof(Event) <= 40u);
    CHECK(sizeof(Decoder) <= 7168u + 576u * k_max_bits_per_package);
}

// U18: profile values, check() in order, search_range(); an invalid configuration emits nothing.
TEST(decoder_config_profiles_and_check) {
    const DecoderConfig ssb;
    CHECK_EQ(unsigned(ssb.min_slot_ms), 8u);
    CHECK_EQ(ssb.passband.low_hz, 300);
    CHECK_EQ(ssb.passband.high_hz, 2700);
    CHECK(ssb.decision_mode == DecisionMode::adaptive);
    CHECK_NEAR(ssb.fixed_ratio, 0.70, 1e-6);
    CHECK(ssb.impulse_blanker);
    CHECK_EQ(ssb.max_slot_ms(), 64);
    const DecoderConfig am = DecoderConfig::for_profile(Profile::am);
    CHECK_EQ(unsigned(am.min_slot_ms), 8u);
    CHECK_EQ(am.passband.low_hz, 100);
    CHECK_EQ(am.passband.high_hz, 3000);
    const DecoderConfig fm = DecoderConfig::for_profile(Profile::fm);
    CHECK_EQ(unsigned(fm.min_slot_ms), 4u);
    CHECK_EQ(fm.passband.low_hz, 300);
    CHECK_EQ(fm.passband.high_hz, 3000);
    CHECK_EQ(fm.max_slot_ms(), 32);
    const DecoderConfig configs[] = {ssb, am, fm, DecoderConfig::for_profile(Profile::ssb)};
    const std::uint16_t lows[] = {335, 300, 1000, 335};
    const std::uint16_t highs[] = {2665, 2700, 2700, 2665};
    for (std::size_t i = 0; i < test::count_of(configs); ++i) {
        CHECK(configs[i].valid());
        CHECK_EQ(configs[i].search_range().low_hz, lows[i]);
        CHECK_EQ(configs[i].search_range().high_hz, highs[i]);
    }
    struct Case {
        const char* name;
        ConfigError expected;
        void (*change)(DecoderConfig&);
    };
    const Case cases[] = {
        {"min_slot 3", ConfigError::min_slot, [](DecoderConfig& c) { c.min_slot_ms = 3; }},
        {"min_slot 33", ConfigError::min_slot, [](DecoderConfig& c) { c.min_slot_ms = 33; }},
        {"inverted passband", ConfigError::passband,
         [](DecoderConfig& c) {
             c.passband.low_hz = 2700;
             c.passband.high_hz = 300;
         }},
        {"passband above 4000", ConfigError::passband, [](DecoderConfig& c) { c.passband.high_hz = 4001; }},
        {"narrower than the margins", ConfigError::passband,
         [](DecoderConfig& c) {
             c.passband.low_hz = 1000;
             c.passband.high_hz = 1060;
         }},
        {"decision mode 7", ConfigError::decision_mode,
         [](DecoderConfig& c) { c.decision_mode = static_cast<DecisionMode>(7); }},
        {"fixed ratio 0", ConfigError::fixed_ratio, [](DecoderConfig& c) { c.fixed_ratio = 0.0f; }},
        {"fixed ratio 1", ConfigError::fixed_ratio, [](DecoderConfig& c) { c.fixed_ratio = 1.0f; }},
    };
    for (std::size_t i = 0; i < test::count_of(cases); ++i) {
        DecoderConfig config;
        cases[i].change(config);
        if (!CHECK(config.check() == cases[i].expected)) NOTE("case %s", cases[i].name);
        CHECK(!config.valid());
    }
    // The public window limits (frozen v0.3 API): 3 and 33 above are just outside them, the limits themselves pass.
    CHECK_EQ(unsigned(unlimited::k_min_window_slot_ms), 4u);
    CHECK_EQ(unsigned(unlimited::k_max_window_slot_ms), 32u);
    DecoderConfig edge;
    edge.min_slot_ms = unlimited::k_min_window_slot_ms;
    CHECK(edge.valid());
    edge.min_slot_ms = unlimited::k_max_window_slot_ms;
    CHECK(edge.valid());
    DecoderConfig wide;
    wide.min_slot_ms = 16;
    CHECK(wide.valid());
    CHECK_EQ(wide.search_range().low_hz, 318);  // 300 + ceil(2200000 / 128000)
    // An invalid configuration leaves the decoder idle: no event at all, even for a clean transmission.
    DecoderConfig bad;
    bad.min_slot_ms = 2;
    const Recording recording = single(random_bytes(4, 1), preset_config(Preset::hf));
    const Capture capture = run_decoder(recording.samples, bad);
    CHECK(capture.events.empty());
}

// L18: every event type filled as spec 5.1; slot and package events precede the bytes they complete.
TEST(decoder_event_fields) {
    const EncoderConfig config = preset_config(Preset::hf);
    const std::vector<std::uint8_t> data = random_bytes(12, 31);
    const Recording recording = single(data, config);
    const std::vector<std::int16_t> samples = usb(recording.samples, 15.0, 32, config.amplitude);
    struct Context {
        Decoder* decoder;
        std::vector<Event> events;
        std::vector<bool> dcd;
        std::vector<std::uint8_t> bits;
    } context = {nullptr, {}, {}, {}};
    Decoder decoder(DecoderConfig(), [](const Event& e, void* c) {
        Context* self = static_cast<Context*>(c);
        self->events.push_back(e);
        self->dcd.push_back(self->decoder->dcd());
        self->bits.push_back(self->decoder->bits_per_package());
    }, &context);
    context.decoder = &decoder;
    CHECK(!decoder.dcd());
    CHECK_EQ(decoder.tone_hz(), 0.0f);
    decoder.process(samples.data(), samples.size());
    const std::vector<Event>& events = context.events;
    REQUIRE(!events.empty());
    std::size_t locked = 0, slots = 0, packages = 0, bytes = 0, ends = 0;
    std::uint32_t last_package = 0;
    std::uint8_t package_bits_seen = 0;
    int bits_in_package = 0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const Event& e = events[i];
        switch (e.type) {
        case EventType::state:
            CHECK_EQ(context.dcd[i], e.state != DecoderState::search);
            break;
        case EventType::locked:
            ++locked;
            CHECK_EQ(unsigned(e.bits_per_package), 8u);
            CHECK_EQ(e.package_index, 0u);
            CHECK_NEAR(e.tone_hz, 1500.0, 2.0);
            CHECK_NEAR(e.slot_ms, 16.0, 0.2);
            CHECK(e.snr_db > 5.0 && e.snr_db < 25.0);
            CHECK_EQ(unsigned(e.flags), 0u);
            break;
        case EventType::slot:
            ++slots;
            ++bits_in_package;
            CHECK(e.value == 0 || e.value == 1);
            CHECK_EQ(int(e.slot), bits_in_package);
            CHECK((e.soft[0] > 0) == (e.value == 1));
            CHECK(e.threshold_pct >= 50 && e.threshold_pct <= 100);
            CHECK(e.start_pct > 50 && e.stop_pct > 50);
            CHECK_EQ(unsigned(e.bits_per_package), 8u);
            break;
        case EventType::package:
            ++packages;
            CHECK_EQ(int(e.value), bits_in_package);
            bits_in_package = 0;
            CHECK_NEAR(e.slot_ms, 16.0, 0.3);
            CHECK(e.package_index == last_package || e.package_index == last_package + 1);
            last_package = e.package_index;
            package_bits_seen = e.value;
            break;
        case EventType::byte: {
            CHECK_EQ(e.byte_index, std::uint32_t(bytes));
            CHECK_EQ(unsigned(e.value), unsigned(data[bytes]));
            // N = 8: each byte is a package, completed by it; its package event came first.
            CHECK_EQ(e.package_index, std::uint32_t(bytes));
            CHECK(packages > bytes);
            unsigned value = 0;
            for (int b = 0; b < 8; ++b) value = (value << 1) | (e.soft[b] > 0 ? 1u : 0u);
            CHECK_EQ(value, unsigned(e.value));
            CHECK(locked == 1);
            ++bytes;
            break;
        }
        case EventType::end:
            ++ends;
            CHECK_EQ(e.package_index, std::uint32_t(data.size() - 1));
            break;
        case EventType::lost:
            CHECK(false);
            break;
        }
    }
    CHECK_EQ(locked, 1u);
    CHECK_EQ(bytes, data.size());
    CHECK_EQ(ends, 1u);
    CHECK_EQ(packages, data.size());
    CHECK_EQ(slots, 8 * data.size());
    CHECK_EQ(unsigned(package_bits_seen), 8u);
    CHECK(decoder.state() == DecoderState::search);
    CHECK_EQ(unsigned(decoder.bits_per_package()), 0u);
    CHECK_EQ(decoder.slot_ms(), 0.0f);
    CHECK_EQ(decoder.snr_db(), 0.0f);
    CHECK(decoder.config().min_slot_ms == 8);
    // bits_per_package() is N from TRACK entry on.
    bool seen_n = false;
    for (std::size_t i = 0; i < events.size(); ++i) seen_n = seen_n || context.bits[i] == 8;
    CHECK(seen_n);
    // reset() in TRACK: lost(reset), then the state event; nothing after it.
    std::vector<Event> reset_events;
    Decoder second(DecoderConfig(), [](const Event& e, void* c) { static_cast<std::vector<Event>*>(c)->push_back(e); },
                   &reset_events);
    const std::size_t half = recording.transmissions[0].start_sample + recording.transmissions[0].length / 2;
    second.process(samples.data(), half);
    REQUIRE(second.state() == DecoderState::track);
    CHECK(second.dcd());
    CHECK_EQ(unsigned(second.bits_per_package()), 8u);
    CHECK_NEAR(second.slot_ms(), 16.0, 0.3);
    CHECK_NEAR(second.tone_hz(), 1500.0, 2.0);
    const std::size_t before = reset_events.size();
    second.reset();
    REQUIRE(reset_events.size() == before + 2);
    CHECK(reset_events[before].type == EventType::lost && reset_events[before].reason == LostReason::reset);
    CHECK(reset_events[before + 1].type == EventType::state && reset_events[before + 1].state == DecoderState::search);
    CHECK(second.state() == DecoderState::search);
}

// L1: every preset (1000 bytes), and T x N over the grid (within (N + 1) T <= 1152 ms, the cap and a window holding T;
// 200 bytes here): 0 errors, the exact byte count, end, measured T within 0.5 %, N learnt exactly.
TEST(decoder_l1_clean_loopback) {
    for (std::size_t p = 0; p < test::count_of(k_presets); ++p) {
        const EncoderConfig config = preset_config(k_presets[p]);
        const std::vector<std::uint8_t> data = random_bytes(1000, std::uint32_t(10 + p));
        Recording recording;
        const Capture capture = run(config, data, DecoderConfig::for_profile(profile_of(k_presets[p])), 99.0, 0, recording);
        const Score s = score(recording, capture);
        const Learnt l = learnt(capture);
        const bool ok = CHECK_EQ(s.matched, data.size()) && CHECK_EQ(s.wrong_bytes, 0u) && CHECK_EQ(s.extra_bytes, 0u) &&
                        CHECK_EQ(s.ends, 1u) && CHECK_EQ(s.locks, 1u) &&
                        CHECK_EQ(unsigned(l.bits), unsigned(config.bits_per_package)) &&
                        CHECK(std::fabs(l.slot_ms / preset_slot_ms(k_presets[p]) - 1.0) <= k_slot_tolerance);
        if (!ok) report("preset", s);
    }
    const double slots_ms[] = {4, 5, 8, 12.5, 16, 20, 32, 37, 64, 100, 128};
    const std::uint8_t bits[] = {1, 2, 3, 4, 7, 8, 9, 16, 31, 32};
    std::size_t runs = 0;
    for (std::size_t t = 0; t < test::count_of(slots_ms); ++t) {
        for (std::size_t b = 0; b < test::count_of(bits); ++b) {
            if (bits[b] > k_max_bits_per_package) continue;
            const EncoderConfig config = slot_config(slots_ms[t], bits[b]);
            if (!config.valid()) continue;
            const std::vector<std::uint8_t> data = random_bytes(1000, std::uint32_t(100 * t + b));
            Recording recording;
            const Capture capture = run(config, data, receiver_for(slots_ms[t]), 99.0, 0, recording);
            const Score s = score(recording, capture);
            const Learnt l = learnt(capture);
            const bool ok = CHECK_EQ(s.matched, data.size()) && CHECK_EQ(s.wrong_bytes, 0u) &&
                            CHECK_EQ(s.extra_bytes, 0u) && CHECK_EQ(s.ends, 1u) &&
                            CHECK_EQ(unsigned(l.bits), unsigned(bits[b])) &&
                            CHECK(std::fabs(l.slot_ms / slots_ms[t] - 1.0) <= k_slot_tolerance);
            if (!ok) {
                NOTE("T %.1f ms N %u: learnt T %.3f N %u", slots_ms[t], unsigned(bits[b]), l.slot_ms, unsigned(l.bits));
                report("grid", s);
            }
            ++runs;
        }
    }
    NOTE("%zu grid transmissions", runs);
}

// L2: chunks of 1, 7, 160 and 4096 samples give identical event streams, slot and package events included.
TEST(decoder_l2_chunk_invariance) {
    const EncoderConfig config = preset_config(Preset::hf_fast);
    const Recording recording = single(random_bytes(30, 3), config);
    const std::vector<std::int16_t> samples = usb(recording.samples, 8.0, 4, config.amplitude);
    const std::size_t chunks[] = {1, 7, 160, 4096};
    const Capture reference = run_decoder(samples, DecoderConfig(), 0);
    REQUIRE(count_events(reference, EventType::byte) == 30u);
    for (std::size_t c = 0; c < test::count_of(chunks); ++c) {
        const Capture capture = run_decoder(samples, DecoderConfig(), chunks[c]);
        REQUIRE(capture.events.size() == reference.events.size());
        for (std::size_t i = 0; i < capture.events.size(); ++i) {
            if (!CHECK(same_event(capture.events[i], reference.events[i]))) {
                NOTE("chunk %zu, event %zu", chunks[c], i);
                break;
            }
        }
    }
}

namespace {

// Slot kinds of a synthesized transmission (spec 1.2).
enum SlotCode { slot_silent, slot_tone, slot_one, slot_zero, slot_marker };

// Lead-in, tune, 8 sync markers, packages of N bits (the last one short), 2 END markers; `first_data` receives the index
// of package 0's first data slot.
std::vector<int> transmission_kinds(double slot_ms, unsigned bits, const std::vector<std::uint8_t>& data,
                                    std::size_t& first_data, double lead_ms = 0.0) {
    std::vector<int> kinds;
    const int lead_slots = static_cast<int>(std::ceil(lead_ms / slot_ms));
    kinds.insert(kinds.end(), lead_slots, slot_silent);
    const int tune = std::max(6, static_cast<int>(std::ceil(250.0 / slot_ms - 1e-9)));
    kinds.insert(kinds.end(), tune, slot_tone);
    kinds.insert(kinds.end(), 8, slot_marker);
    first_data = kinds.size();
    const std::size_t total = data.size() * 8;
    for (std::size_t first = 0; first < total; first += bits) {
        const std::size_t d = std::min<std::size_t>(bits, total - first);
        for (std::size_t i = 0; i < d; ++i) {
            const std::size_t bit = first + i;
            kinds.push_back(((data[bit / 8] >> (7 - bit % 8)) & 1u) != 0 ? slot_one : slot_zero);
        }
        kinds.push_back(slot_marker);
    }
    kinds.insert(kinds.end(), 2, slot_marker);
    return kinds;
}

// The slots in double precision (spec 1.1): the tune ramps up in its first slot and down in its last; the carrier sign
// turns after every marker. A 100 ms tail of silence follows.
std::vector<std::int16_t> render_kinds(double slot_ms, const std::vector<int>& kinds, double tone_hz = 1500.0) {
    const double rate = k_decoder_rate_hz;
    const double two_pi = 6.28318530717958647692;
    const double amplitude = 23197.0;
    const double length = slot_ms * rate / 1000.0;
    std::vector<std::int16_t> out;
    double sign = 1.0;
    const double pi = two_pi / 2.0;
    for (std::size_t j = 0; j < kinds.size(); ++j) {
        const std::size_t begin = static_cast<std::size_t>(std::ceil(j * length));
        const std::size_t end = static_cast<std::size_t>(std::ceil((j + 1) * length));
        const bool first_tune = kinds[j] == slot_tone && (j == 0 || kinds[j - 1] != slot_tone);
        const bool last_tune = kinds[j] == slot_tone && (j + 1 == kinds.size() || kinds[j + 1] != slot_tone);
        for (std::size_t n = begin; n < end; ++n) {
            const double u = (n - j * length) / length;
            double w = u < 0.25 ? std::pow(std::sin(two_pi * u), 2) : (u > 0.75 ? std::pow(std::sin(two_pi * (1 - u)), 2) : 1.0);
            double e = 0.0;
            if (kinds[j] == slot_tone) {
                e = (first_tune && u < 0.25) || (last_tune && u > 0.75) ? w : 1.0;
            } else if (kinds[j] == slot_one) {
                e = w;
            } else if (kinds[j] == slot_marker) {
                const double r = u <= 0.375 ? 1.0 : (u >= 0.625 ? -1.0 : std::cos(4.0 * pi * (u - 0.375)));
                e = w * r;
            }
            out.push_back(static_cast<std::int16_t>(std::lround(amplitude * sign * e * std::sin(two_pi * tone_hz * n / rate))));
        }
        if (kinds[j] == slot_marker) sign = -sign;
    }
    out.insert(out.end(), static_cast<std::size_t>(0.1 * rate), 0);
    return out;
}

// A reference transmission in double precision (spec 1.1, 2.1): tune, sync, packages of any N (also above this build's
// cap, which the encoder refuses), END, tail. For senders the encoder cannot make.
std::vector<std::int16_t> synthesize(double slot_ms, unsigned bits, const std::vector<std::uint8_t>& data,
                                     double tone_hz = 1500.0, double lead_ms = 0.0) {
    std::size_t first_data = 0;
    return render_kinds(slot_ms, transmission_kinds(slot_ms, bits, data, first_data, lead_ms), tone_hz);
}

std::vector<std::int16_t> silence_ms(double ms) {
    return std::vector<std::int16_t>(static_cast<std::size_t>(ms * k_decoder_rate_hz / 1000.0), 0);
}

void append(std::vector<std::int16_t>& a, const std::vector<std::int16_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
}

// Bytes of package range [first, last] (N bits each): those with at least one bit in it.
bool byte_touches(std::size_t byte, unsigned bits, std::size_t first, std::size_t last) {
    const std::size_t low = byte * 8 / bits;
    const std::size_t high = (byte * 8 + 7) / bits;
    return high >= first && low <= last;
}

// Channel with the transmission's pitch shifted, for usb (offset) or lsb (inverted about 3000 Hz, then offset).
std::vector<std::int16_t> sideband(const std::vector<std::int16_t>& samples, sim::Mode mode, double snr_db,
                                   double offset_hz, std::uint32_t seed, std::int16_t amplitude,
                                   double rx_low = 300.0, double rx_high = 2700.0) {
    sim::ChannelConfig config;
    config.mode = mode;
    config.snr_db = snr_db;
    config.freq_offset_hz = offset_hz;
    config.seed = seed;
    config.rx_low_hz = rx_low;
    config.rx_high_hz = rx_high;
    return through_channel(samples, config, amplitude);
}

}  // namespace

// L3: one decoder per profile hears back-to-back transmissions of every T of its window, N = 1, 4, 8, 16, 32 and pitches
// low, centre and high inside its search range, without reconfiguration.
TEST(decoder_l3_no_configuration) {
    const Profile profiles[] = {Profile::ssb, Profile::am, Profile::fm};
    const std::uint8_t bits[] = {1, 4, 8, 16, 32};
    for (std::size_t p = 0; p < test::count_of(profiles); ++p) {
        const DecoderConfig receiver = DecoderConfig::for_profile(profiles[p]);
        const Passband range = receiver.search_range();
        Recording recording;
        append_silence(recording, k_gap_ms);
        std::size_t pitch = 0;
        for (unsigned t = receiver.min_slot_ms; t <= receiver.max_slot_ms(); t *= 2) {
            for (std::size_t b = 0; b < test::count_of(bits); ++b) {
                if (bits[b] > k_max_bits_per_package) continue;
                EncoderConfig config = slot_config(t, bits[b]);
                config.passband = receiver.passband;
                const unsigned half = occupied_band(1500, config.slot_us).high_hz - 1500u;
                const unsigned low = std::max<unsigned>(range.low_hz, receiver.passband.low_hz + half + 5);
                const unsigned high = std::min<unsigned>(range.high_hz, receiver.passband.high_hz - half - 5);
                const unsigned pitches[] = {low, (low + high) / 2, high};
                config.tone_hz = static_cast<std::uint16_t>(pitches[pitch++ % 3]);
                if (!config.valid()) continue;
                append_transmission(recording, random_bytes(20, std::uint32_t(t * 100 + b)), config);
                append_silence(recording, k_gap_ms);
            }
        }
        const Capture capture = run_decoder(recording.samples, receiver, 4096);
        const Score s = score(recording, capture);
        const bool ok = CHECK_EQ(s.matched, s.bytes_sent) && CHECK_EQ(s.wrong_bytes, 0u) && CHECK_EQ(s.extra_bytes, 0u) &&
                        CHECK_EQ(s.ends, recording.transmissions.size());
        NOTE("profile %zu: %zu transmissions", p, recording.transmissions.size());
        if (!ok) report("L3", s);
    }
}

// L4: pitch sweep 300..2700 Hz in 37 Hz steps where the band fits; USB and LSB with the mistuning up to the shift
// tolerance - 10 Hz at gate + 3 dB, the HF presets.
TEST(decoder_l4_pitch_sideband_shift) {
    std::size_t pitches = 0;
    for (unsigned tone = 300; tone <= 2700; tone += 37) {
        EncoderConfig config = preset_config(Preset::hf);
        config.tone_hz = static_cast<std::uint16_t>(tone);
        if (!config.valid() || tone < DecoderConfig().search_range().low_hz) continue;
        const std::vector<std::uint8_t> data = random_bytes(6, tone);
        Recording recording;
        const Capture capture = run(config, data, DecoderConfig(), 99.0, 0, recording);
        const Score s = score(recording, capture);
        if (!CHECK(s.matched == data.size() && s.wrong_bytes == 0 && s.extra_bytes == 0)) {
            NOTE("pitch %u Hz", tone);
            report("sweep", s);
        }
        ++pitches;
    }
    NOTE("%zu pitches", pitches);
    const Preset presets[] = {Preset::hf_slow, Preset::hf, Preset::hf_fast};
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        const EncoderConfig config = preset_config(presets[p]);
        const double tolerance = passband_fit(config).tolerance_hz - 10.0;
        const double offsets[] = {-tolerance, 0.0, tolerance};
        const sim::Mode modes[] = {sim::Mode::usb, sim::Mode::lsb};
        for (std::size_t m = 0; m < test::count_of(modes); ++m) {
            for (std::size_t o = 0; o < test::count_of(offsets); ++o) {
                const std::vector<std::uint8_t> data = random_bytes(24, std::uint32_t(p * 10 + m * 3 + o));
                const Recording recording = single(data, config);
                const std::vector<std::int16_t> samples =
                    sideband(recording.samples, modes[m], gate_db(preset_slot_ms(presets[p])) + k_gate_margin_db,
                             offsets[o], std::uint32_t(40 + p * 10 + m * 3 + o), config.amplitude);
                const Score s = score(recording, run_decoder(samples, DecoderConfig(), 4096));
                if (!CHECK(s.matched == data.size() && s.wrong_bytes == 0 && s.extra_bytes == 0)) {
                    NOTE("preset %zu %s offset %+.0f Hz", p, m == 0 ? "usb" : "lsb", offsets[o]);
                    report("shift", s);
                }
            }
        }
    }
}

// L5: clock error of +-1000 ppm on TX, RX and both at T = 16 ms, N = 8 and 32, gate + 3 dB, 30 s: no slip, no error.
TEST(decoder_l5_clock_error) {
    struct Case {
        double tx_ppm;
        double rx_ppm;
    };
    const Case cases[] = {{1000, 0}, {-1000, 0}, {0, 1000}, {0, -1000}, {1000, 1000}, {-1000, 1000}};
    const std::uint8_t bits[] = {8, 32};
    for (std::size_t b = 0; b < test::count_of(bits); ++b) {
        if (bits[b] > k_max_bits_per_package) continue;
        EncoderConfig config = preset_config(Preset::hf);
        config.bits_per_package = bits[b];
        const std::size_t bytes = static_cast<std::size_t>(30.0 / (0.016 * (bits[b] + 1)) * bits[b] / 8);
        const std::vector<std::uint8_t> data = random_bytes(bytes, std::uint32_t(bits[b]));
        const Recording recording = single(data, config);
        for (std::size_t c = 0; c < test::count_of(cases); ++c) {
            sim::ChannelConfig channel;
            channel.mode = sim::Mode::usb;
            channel.snr_db = gate_db(16.0) + k_gate_margin_db;
            channel.clock_ppm = cases[c].tx_ppm;
            channel.seed = std::uint32_t(50 + c);
            std::vector<std::int16_t> samples = through_channel(recording.samples, channel, config.amplitude);
            if (cases[c].rx_ppm != 0.0) {
                std::vector<float> in(samples.begin(), samples.end());
                const std::vector<float> out =
                    pc::resample(in, k_decoder_rate_hz, k_decoder_rate_hz * (1.0 + cases[c].rx_ppm * 1e-6));
                samples.assign(out.size(), 0);
                for (std::size_t i = 0; i < out.size(); ++i) {
                    samples[i] = static_cast<std::int16_t>(std::lround(std::max(-32768.0f, std::min(32767.0f, out[i]))));
                }
            }
            const Capture capture = run_decoder(samples, DecoderConfig(), 4096);
            const Score s = score(recording, capture);
            const Learnt l = learnt(capture);
            const double expected_ms = 16.0 * (1.0 - cases[c].tx_ppm * 1e-6) * (1.0 + cases[c].rx_ppm * 1e-6);
            if (!CHECK(s.matched == data.size() && s.wrong_bytes == 0 && s.extra_bytes == 0 && s.lost_events == 0 &&
                       s.locks == 1 && std::fabs(l.slot_ms / expected_ms - 1.0) <= 0.002)) {
                NOTE("N %u tx %+.0f ppm rx %+.0f ppm: T %.4f ms", unsigned(bits[b]), cases[c].tx_ppm, cases[c].rx_ppm,
                     l.slot_ms);
                report("clock", s);
            }
        }
    }
}

// L6: faded preamble markers at gate + 3 dB, N = 1, 2, 4, 8, 16. (a) any 3 of the 8 sync markers (all 56 triples) and
// (b) the last one: every byte; (c) the first STOP, (d) the START and the first STOP, (e) the last two sync markers:
// the bytes of packages 0 and 1 may be missing, every other byte exact. N = 1 in (b): every byte or none. Never a
// shifted byte, no byte off the grid.
TEST(decoder_l6_preamble_fades) {
    const std::uint8_t bits[] = {1, 2, 4, 8, 16};
    enum Case { triple, last_sync, first_stop, start_and_stop, last_two };
    const char* names[] = {"sync triple", "last sync", "first STOP", "START and STOP", "last two"};
    const int sync_markers = 8;
    const std::size_t k_l6_bit_errors = 2;
    const std::size_t k_l6_total_bit_errors = 8;
    std::size_t runs = 0;
    std::size_t bit_errors = 0;
    for (std::size_t b = 0; b < test::count_of(bits); ++b) {
        EncoderConfig config = preset_config(Preset::hf);
        config.bits_per_package = bits[b];
        const std::vector<std::uint8_t> data = random_bytes(24, std::uint32_t(60 + b));
        // Every triple of sync markers (i < j < k), then the four single cases.
        int zeroed[3] = {0, 1, 1};
        for (int c = triple; c <= last_two; ++c) {
            while (true) {
                if (c == triple) {
                    // Next triple in lexicographic order; the first call yields {0, 1, 2}.
                    if (++zeroed[2] >= sync_markers) {
                        if (++zeroed[1] >= sync_markers - 1) {
                            if (++zeroed[0] >= sync_markers - 2) break;
                            zeroed[1] = zeroed[0] + 1;
                        }
                        zeroed[2] = zeroed[1] + 1;
                    }
                }
                Recording recording = single(data, config);
                const Transmission& t = recording.transmissions[0];
                const std::size_t start = first_start_slot(config);  // the last sync marker
                const std::size_t first_sync = start + 1 - config.sync_markers;
                if (c == triple) {
                    for (int k = 0; k < 3; ++k) scale_slot(recording, 0, first_sync + std::size_t(zeroed[k]), 0.0);
                }
                if (c == last_sync || c == start_and_stop || c == last_two) scale_slot(recording, 0, start, 0.0);
                if (c == last_two) scale_slot(recording, 0, start - 1, 0.0);
                if (c == first_stop || c == start_and_stop) scale_slot(recording, 0, stop_slot(t, 0), 0.0);
                const int triple_seed = c == triple ? 100 * zeroed[0] + 10 * zeroed[1] + zeroed[2] : 0;
                const std::uint32_t seed = std::uint32_t(600 + 10 * b + c + triple_seed);
                const std::vector<std::int16_t> samples =
                    usb(recording.samples, gate_db(16.0) + k_gate_margin_db, seed, config.amplitude);
                const Mapping m = map_events(recording, run_decoder(samples, DecoderConfig(), 4096));
                ++runs;
                // A byte read off the grid is wrong in half its bits; a few noise bit errors are the channel's (a
                // package whose START faded reads its first bits against a low reference line).
                bit_errors += m.score.bit_errors;
                bool ok = CHECK(m.score.bit_errors <= k_l6_bit_errors) && CHECK_EQ(m.score.extra_bytes, 0u) &&
                          CHECK_EQ(m.score.shifted_segments, 0u);
                const bool every = c <= last_sync;
                std::size_t missing = 0;
                for (std::size_t k = 0; k < data.size(); ++k) {
                    const bool may_miss = !every && byte_touches(k, bits[b], 0, 1);
                    if (m.received[0][k] < 0 && !may_miss) ++missing;
                }
                // N = 1 with the START faded: every byte or none. With the first STOP faded too, the carrier across
                // the gap places package 0 (a deviation from spec L6, which refuses g1 - L = 4): its bytes may be
                // missing.
                if (bits[b] == 1 && c == last_sync) {
                    ok = ok && CHECK(m.score.matched == 0 || m.score.matched == data.size());
                } else {
                    ok = ok && CHECK_EQ(missing, 0u);
                }
                if (!ok) {
                    if (c == triple) {
                        NOTE("N %u, sync markers %d %d %d zeroed", unsigned(bits[b]), zeroed[0], zeroed[1], zeroed[2]);
                    } else {
                        NOTE("N %u, %s", unsigned(bits[b]), names[c]);
                    }
                    report("L6", m.score);
                }
                if (c != triple) break;
            }
        }
    }
    NOTE("%zu preamble runs, %zu bit errors", runs, bit_errors);
    CHECK(bit_errors <= k_l6_total_bit_errors);
}

// L7: the STOPs of packages 0, 2, 4 and 6 at -30 dB (a chain that reads as one of twice the length): 0 wrong, 0 extra;
// every byte from the settled part (byte 20 of 40) on.
TEST(decoder_l7_alias) {
    const EncoderConfig config = preset_config(Preset::hf);
    const std::vector<std::uint8_t> data = random_bytes(40, 70);
    for (std::uint32_t seed = 71; seed < 75; ++seed) {
        Recording recording = single(data, config);
        for (std::size_t p = 0; p <= 6; p += 2) {
            scale_slot(recording, 0, stop_slot(recording.transmissions[0], p), k_attenuation_30_db);
        }
        const std::vector<std::int16_t> samples = usb(recording.samples, 10.0, seed, config.amplitude);
        const Mapping m = map_events(recording, run_decoder(samples, DecoderConfig(), 4096));
        std::size_t missing = 0;
        for (std::size_t k = 20; k < data.size(); ++k) missing += m.received[0][k] < 0 ? 1 : 0;
        if (!CHECK(m.score.wrong_bytes == 0 && m.score.extra_bytes == 0 && missing == 0)) report("L7", m.score);
    }
}

// L8: one STOP in five zeroed: every byte, the flywheeled ones flagged flywheel_stop; then the signal cut: lost(signal_gone)
// within the presence window and no byte after.
TEST(decoder_l8_flywheel_and_loss) {
    const EncoderConfig config = preset_config(Preset::hf);
    const std::vector<std::uint8_t> data = random_bytes(60, 80);
    Recording recording = single(data, config);
    const Transmission& t = recording.transmissions[0];
    const std::size_t cut_package = 40;
    // The last package before the cut keeps its STOP: a STOP missing right before the signal goes is a loss, whose
    // held packages are dropped.
    for (std::size_t p = 4; p + 1 < cut_package; p += 5) scale_slot(recording, 0, stop_slot(t, p), 0.0);
    const std::size_t cut = static_cast<std::size_t>(slot_start_sample(t, package_start_slot(t, cut_package) + 1));
    std::fill(recording.samples.begin() + cut, recording.samples.end(), 0);
    const std::vector<std::int16_t> samples = usb(recording.samples, 15.0, 81, config.amplitude);
    const Capture capture = run_decoder(samples, DecoderConfig(), 0);
    const Mapping m = map_events(recording, capture);
    CHECK_EQ(m.score.wrong_bytes, 0u);
    CHECK_EQ(m.score.extra_bytes, 0u);
    CHECK_EQ(m.score.ends, 0u);
    for (std::size_t k = 0; k < data.size(); ++k) {
        if (k < cut_package) {
            if (!CHECK(m.received[0][k] >= 0)) NOTE("byte %zu missing", k);
            const bool flywheel = (m.byte_events[0][k].flags & event_flag_flywheel_stop) != 0;
            CHECK_EQ(flywheel, k % 5 == 4 && k + 1 < cut_package);
        } else {
            CHECK(m.received[0][k] < 0);
        }
    }
    const std::size_t lost = find_event(capture, EventType::lost);
    REQUIRE(lost < capture.events.size());
    CHECK(capture.events[lost].reason == LostReason::signal_gone);
    const double packages_after = (capture.event_sample[lost] - slot_start_sample(t, stop_slot(t, cut_package))) /
                                  (9.0 * slot_samples(config));
    NOTE("lost %.2f packages after the cut", packages_after);
    CHECK(packages_after <= 5.0);
}

// L9: full and short final packages, N = 1, one-package transmissions and two packages with a short second: END within
// 3 T of the final STOP (a lock confirmed by its END alone waits one slot more for its silence) and exact bytes; a PTT
// cut with no END: lost, no garbage, no end.
TEST(decoder_l9_end) {
    struct Case {
        std::uint8_t bits;
        std::size_t bytes;
        bool short_transmission;  // confirmed by its END alone
    };
    const Case cases[] = {{3, 20, false}, {5, 20, false}, {7, 21, false}, {16, 21, false}, {32, 21, false}, {1, 12, false},
                          {8, 1, true},   {16, 2, true},  {32, 1, true},  {12, 2, true},  {16, 3, true},  {8, 20, false}};
    const double channel_delay = channel_delay_samples();
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        if (cases[c].bits > k_max_bits_per_package) continue;
        EncoderConfig config = preset_config(Preset::hf);
        config.bits_per_package = cases[c].bits;
        const std::vector<std::uint8_t> data = random_bytes(cases[c].bytes, std::uint32_t(90 + c));
        const Recording recording = single(data, config, 400.0);
        const Transmission& t = recording.transmissions[0];
        const std::vector<std::int16_t> samples = usb(recording.samples, 12.0, std::uint32_t(91 + c), config.amplitude);
        const Capture capture = run_decoder(samples, DecoderConfig(), 0);
        const Score s = score(recording, capture);
        const std::size_t end = find_event(capture, EventType::end);
        bool ok = CHECK_EQ(s.matched, data.size()) && CHECK_EQ(s.wrong_bytes, 0u) && CHECK_EQ(s.extra_bytes, 0u) &&
                  CHECK_EQ(s.ends, 1u) && CHECK(end < capture.events.size());
        if (ok) {
            const double final_stop = slot_start_sample(t, stop_slot(t, package_count(t) - 1)) + 0.5 * slot_samples(config);
            const double delay = (capture.event_sample[end] - final_stop - channel_delay) / slot_samples(config);
            // A short transmission's END also checks the clean slot at +3 T; N = 1 (V16) looks where the chain's next
            // STOPs would be, +4 T and +6 T.
            double limit = cases[c].short_transmission ? 4.0 : 3.0;
            if (cases[c].bits == 1) limit = 7.0;
            ok = CHECK(delay <= limit);
            if (!ok || c == 0 || c == 6) NOTE("N %u, %zu bytes: end %.2f T after the final STOP", unsigned(cases[c].bits),
                                              data.size(), delay);
        }
        if (!ok) report("L9", s);
    }
    // PTT cut after package 10, no END.
    const EncoderConfig config = preset_config(Preset::hf);
    const std::vector<std::uint8_t> data = random_bytes(30, 99);
    Recording recording = single(data, config);
    const Transmission& t = recording.transmissions[0];
    const std::size_t cut = static_cast<std::size_t>(slot_start_sample(t, package_start_slot(t, 10) + 1));
    std::fill(recording.samples.begin() + cut, recording.samples.end(), 0);
    const Capture capture = run_decoder(usb(recording.samples, 12.0, 98, config.amplitude), DecoderConfig(), 0);
    const Score s = score(recording, capture);
    CHECK_EQ(s.wrong_bytes, 0u);
    CHECK_EQ(s.extra_bytes, 0u);
    CHECK_EQ(s.ends, 0u);
    CHECK(s.lost_events >= 1u);
    CHECK(s.matched <= 10u);
}

// L10, U28: 5 packages faded out at 20 dB, every preset and N = 1, 3, 4, 8, 16: relock with late_join within 10 packages,
// the bytes after the relock at their byte_index, none wrong, extra or mixed across the gap; a fade of more than 64
// packages: no byte until the next transmission, which decodes.
TEST(decoder_l10_relock_after_fade) {
    struct Case {
        Preset preset;
        std::uint8_t bits;
    };
    const Case cases[] = {{Preset::hf_slow, 8}, {Preset::hf, 8},  {Preset::hf_fast, 8}, {Preset::am, 16}, {Preset::fm, 16},
                          {Preset::hf, 1},      {Preset::hf, 3},  {Preset::hf, 4},      {Preset::hf, 16}};
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        EncoderConfig config = preset_config(cases[c].preset);
        config.bits_per_package = cases[c].bits;
        const std::size_t packages = 50;
        const std::vector<std::uint8_t> data = random_bytes(packages * cases[c].bits / 8, std::uint32_t(100 + c));
        Recording recording = single(data, config);
        const Transmission& t = recording.transmissions[0];
        const std::size_t fade_first = 12;
        const std::size_t fade_last = fade_first + 4;
        for (std::size_t slot = package_start_slot(t, fade_first) + 1; slot <= stop_slot(t, fade_last); ++slot) {
            scale_slot(recording, 0, slot, 0.0);
        }
        const std::vector<std::int16_t> samples = usb(recording.samples, 20.0, std::uint32_t(110 + c), config.amplitude);
        const Capture capture = run_decoder(samples, DecoderConfig::for_profile(profile_of(cases[c].preset)), 0);
        const Mapping m = map_events(recording, capture);
        std::size_t late = capture.events.size();
        for (std::size_t i = 0; i < capture.events.size(); ++i) {
            if (capture.events[i].type == EventType::locked && (capture.events[i].flags & event_flag_late_join) != 0) {
                late = i;
                break;
            }
        }
        // A fade shorter than 3/4 of the presence window (N <= 4: 5 packages are at most 25 slots of 27) is flywheeled
        // through without a loss; its packages read on noise are erasures.
        const bool held = find_event(capture, EventType::lost) == capture.events.size();
        bool ok = CHECK_EQ(m.score.wrong_bytes, 0u) && CHECK_EQ(m.score.extra_bytes, 0u) &&
                  CHECK_EQ(m.score.shifted_segments, 0u) && CHECK(held || late < capture.events.size());
        if (ok) {
            double after = 0.0;
            if (!held) {
                const double fade_end = slot_start_sample(t, stop_slot(t, fade_last));
                after = (capture.event_sample[late] - fade_end) / ((cases[c].bits + 1) * slot_samples(config));
                ok = CHECK(after <= 10.0);
            }
            // Every byte from 10 packages after the fade on is there.
            const std::size_t from = (fade_last + 11) * cases[c].bits / 8 + 1;
            for (std::size_t k = from; k < data.size() && ok; ++k) ok = CHECK(m.received[0][k] >= 0);
            if (held) {
                NOTE("preset %d N %u: held through the fade", int(cases[c].preset), unsigned(cases[c].bits));
            } else {
                NOTE("preset %d N %u: relock %.1f packages after the fade", int(cases[c].preset), unsigned(cases[c].bits),
                     after);
            }
        }
        if (!ok) report("L10", m.score);
    }
    // More than 64 packages faded: nothing until the next transmission.
    const EncoderConfig config = preset_config(Preset::hf_fast);
    const std::vector<std::uint8_t> data = random_bytes(100, 120);
    Recording recording = single(data, config);
    const Transmission& t = recording.transmissions[0];
    for (std::size_t slot = package_start_slot(t, 10) + 1; slot <= stop_slot(t, 80); ++slot) scale_slot(recording, 0, slot, 0.0);
    append_transmission(recording, random_bytes(10, 121), config);
    append_silence(recording, 300.0);
    const std::vector<std::int16_t> received = usb(recording.samples, 20.0, 122, config.amplitude);
    const Mapping m = map_events(recording, run_decoder(received, DecoderConfig(), 4096));
    CHECK_EQ(m.score.wrong_bytes, 0u);
    CHECK_EQ(m.score.extra_bytes, 0u);
    for (std::size_t k = 12; k < 80; ++k) CHECK(m.received[0][k] < 0);
    for (std::size_t k = 0; k < 10; ++k) CHECK(m.received[1][k] >= 0);
}

// L11: back-to-back transmissions with different T, N and pitch, 0.5 s apart.
TEST(decoder_l11_back_to_back) {
    struct Case {
        double slot_ms;
        std::uint8_t bits;
        std::uint16_t tone;
    };
    const Case cases[] = {{16, 8, 1500}, {8, 16, 1200}, {32, 3, 1800}, {16, 1, 900}, {64, 8, 2100}, {12.5, 5, 1400}};
    Recording recording;
    append_silence(recording, k_gap_ms);
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        EncoderConfig config = slot_config(cases[c].slot_ms, cases[c].bits, cases[c].tone);
        config.passband.low_hz = 300;
        config.passband.high_hz = 2700;
        REQUIRE(config.valid());
        append_transmission(recording, random_bytes(12, std::uint32_t(130 + c)), config);
        append_silence(recording, k_gap_ms);
    }
    const Score s = score(recording, run_decoder(usb(recording.samples, 15.0, 131, 23197), DecoderConfig(), 4096));
    if (!CHECK(s.matched == s.bytes_sent && s.wrong_bytes == 0 && s.extra_bytes == 0 && s.ends == test::count_of(cases))) {
        report("L11", s);
    }
}

// L12: each profile decodes every preset of its window, clean and at gate + 3 dB; senders outside it: nothing.
TEST(decoder_l12_window_coverage) {
    const Profile profiles[] = {Profile::ssb, Profile::am, Profile::fm};
    for (std::size_t p = 0; p < test::count_of(profiles); ++p) {
        const DecoderConfig receiver = DecoderConfig::for_profile(profiles[p]);
        for (std::size_t s = 0; s < test::count_of(k_presets); ++s) {
            const EncoderConfig config = preset_config(k_presets[s]);
            const double slot_ms = preset_slot_ms(k_presets[s]);
            const bool heard = slot_ms >= receiver.min_slot_ms && slot_ms <= receiver.max_slot_ms();
            const std::vector<std::uint8_t> data = random_bytes(16, std::uint32_t(140 + s));
            for (int noisy = 0; noisy < 2; ++noisy) {
                Recording recording;
                const double snr = noisy ? gate_db(slot_ms) + k_gate_margin_db : 99.0;
                const Capture capture = run(config, data, receiver, snr, std::uint32_t(141 + s), recording);
                const Score sc = score(recording, capture);
                bool ok = true;
                if (heard) {
                    ok = CHECK(sc.matched == data.size() && sc.wrong_bytes == 0 && sc.extra_bytes == 0);
                } else {
                    ok = CHECK(sc.locks == 0 && sc.bytes_released == 0);
                }
                if (!ok) {
                    NOTE("profile %zu preset %zu %s", p, s, noisy ? "gate + 3 dB" : "clean");
                    report("L12", sc);
                }
            }
        }
        // Slower than the window: T = 8 T_max... ssb and am miss 128 ms, fm misses 64 ms.
        const double slow = receiver.max_slot_ms() * 2.0;
        const EncoderConfig config = slot_config(slow, 8);
        Recording recording;
        const Score sc = score(recording, run(config, random_bytes(10, 150), receiver, 99.0, 0, recording));
        if (!CHECK(sc.locks == 0 && sc.bytes_released == 0)) report("slow sender", sc);
    }
}

// L13: the encoder at 8000, 11025, 22050, 44100 and 48000 Hz, resampled to 8 kHz: 0 errors, the sent T and N.
TEST(decoder_l13_sample_rates) {
    const std::uint32_t rates[] = {8000, 11025, 22050, 44100, 48000};
    for (std::size_t r = 0; r < test::count_of(rates); ++r) {
        EncoderConfig config = EncoderConfig::from_preset(Preset::hf_fast, rates[r]);
        config.bits_per_package = 16;
        const std::vector<std::uint8_t> data = random_bytes(40, std::uint32_t(160 + r));
        const std::vector<std::int16_t> audio = encode(data, config);
        std::vector<float> in(audio.begin(), audio.end());
        in.insert(in.end(), rates[r] / 2, 0.0f);
        const std::vector<float> out = pc::resample(in, rates[r], k_decoder_rate_hz);
        std::vector<std::int16_t> samples(out.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            samples[i] = static_cast<std::int16_t>(std::lround(std::max(-32768.0f, std::min(32767.0f, out[i]))));
        }
        Recording recording;
        Transmission t;
        t.config = config;
        t.data = data;
        t.start_sample = 0;
        t.length = samples.size();
        recording.transmissions.push_back(t);
        const Capture capture = run_decoder(samples, DecoderConfig(), 4096);
        const Score s = score(recording, capture);
        const Learnt l = learnt(capture);
        if (!CHECK(s.matched == data.size() && s.wrong_bytes == 0 && l.bits == 16 && std::fabs(l.slot_ms - 8.0) < 0.04)) {
            NOTE("rate %u: T %.3f N %u", rates[r], l.slot_ms, unsigned(l.bits));
            report("L13", s);
        }
    }
}

// L15: short final packages, N = 3, 5, 7, 9, 16, 32 and n = 1..3N bytes: exactly n bytes, then end.
TEST(decoder_l15_short_final_packages) {
    const std::uint8_t bits[] = {3, 5, 7, 9, 16, 32};
    std::size_t runs = 0;
    for (std::size_t b = 0; b < test::count_of(bits); ++b) {
        if (bits[b] > k_max_bits_per_package) continue;
        EncoderConfig config = preset_config(Preset::hf_fast);
        config.bits_per_package = bits[b];
        for (std::size_t n = 1; n <= 3u * bits[b]; ++n) {
            const std::vector<std::uint8_t> data = random_bytes(n, std::uint32_t(170 + 100 * b + n));
            Recording recording;
            const Score s = score(recording, run(config, data, DecoderConfig(), 99.0, 0, recording));
            if (!CHECK(s.matched == n && s.bytes_released == n && s.wrong_bytes == 0 && s.ends == 1)) {
                NOTE("N %u, %zu bytes", unsigned(bits[b]), n);
                report("L15", s);
            }
            ++runs;
        }
    }
    NOTE("%zu transmissions", runs);
}

// L16: senders above this build's cap: lost(unsupported) up to twice the cap, lost(preamble_timeout) above; no byte.
TEST(decoder_l16_above_the_cap) {
    const unsigned cap = k_max_bits_per_package;
    const unsigned senders[] = {cap + 1, cap + cap / 2, 2 * cap, 2 * cap + 5};
    for (std::size_t i = 0; i < test::count_of(senders); ++i) {
        const double slot_ms = 8.0;
        const std::vector<std::uint8_t> data = random_bytes(3 * senders[i] / 8 + 4, std::uint32_t(180 + i));
        std::vector<std::int16_t> samples = silence_ms(300.0);
        append(samples, synthesize(slot_ms, senders[i], data));
        append(samples, silence_ms(300.0));
        const Capture capture = run_decoder(samples, DecoderConfig(), 4096);
        bool unsupported = false;
        bool timeout = false;
        for (std::size_t e = 0; e < capture.events.size(); ++e) {
            if (capture.events[e].type != EventType::lost) continue;
            unsupported = unsupported || capture.events[e].reason == LostReason::unsupported;
            timeout = timeout || capture.events[e].reason == LostReason::preamble_timeout;
        }
        CHECK_EQ(count_events(capture, EventType::locked), 0u);
        CHECK_EQ(count_events(capture, EventType::byte), 0u);
        if (senders[i] <= 2 * cap) {
            if (!CHECK(unsupported)) NOTE("N %u", senders[i]);
        } else {
            if (!CHECK(timeout)) NOTE("N %u", senders[i]);
        }
    }
    // The synthesized waveform is the encoder's: a sender within the cap decodes.
    const std::vector<std::uint8_t> data = random_bytes(10, 189);
    std::vector<std::int16_t> samples = silence_ms(300.0);
    append(samples, synthesize(16.0, 8, data));
    const Capture capture = run_decoder(samples, DecoderConfig(), 4096);
    CHECK_EQ(count_events(capture, EventType::byte), data.size());
}

// L17: package chains that look like a train of the window (N = 1, 2, 4, 7 at T = T_min, (N + 1) T in the window),
// heard without their preamble: no lock, no byte (60 s each here; 5 min in the spec).
TEST(decoder_l17_chain_read_as_train) {
    const std::uint8_t bits[] = {1, 2, 4, 7};
    for (std::size_t b = 0; b < test::count_of(bits); ++b) {
        EncoderConfig config = preset_config(Preset::hf_fast);  // T = 8 ms = T_min of ssb
        config.bits_per_package = bits[b];
        const std::size_t bytes = static_cast<std::size_t>(60.0 / (0.008 * (bits[b] + 1)) * bits[b] / 8);
        const Recording recording = single(random_bytes(bytes, std::uint32_t(190 + b)), config);
        const Transmission& t = recording.transmissions[0];
        const std::size_t from = static_cast<std::size_t>(slot_start_sample(t, package_start_slot(t, 3)));
        const std::vector<std::int16_t> clipped(recording.samples.begin() + from, recording.samples.end());
        for (int noisy = 0; noisy < 2; ++noisy) {
            const std::vector<std::int16_t> samples =
                noisy ? usb(clipped, 15.0, std::uint32_t(191 + b), config.amplitude) : clipped;
            const Capture capture = run_decoder(samples, DecoderConfig(), 4096);
            if (!CHECK(count_events(capture, EventType::locked) == 0 && count_events(capture, EventType::byte) == 0)) {
                NOTE("N %u %s", unsigned(bits[b]), noisy ? "15 dB" : "clean");
            }
        }
    }
}

// L19: the receiver filter at each spec 1.5 filter: the presets that fit decode at gate + 3 dB with the pitch shifted to
// the tolerance - 10 Hz; a station outside the decoder's search range is ignored.
TEST(decoder_l19_passband) {
    struct Case {
        std::uint16_t low;
        std::uint16_t high;
        Preset preset;
        std::uint16_t tone;
    };
    const Case cases[] = {{300, 2100, Preset::hf, 1200}, {300, 2100, Preset::hf_fast, 1200}, {300, 2700, Preset::hf_fast, 1500},
                          {200, 2900, Preset::hf_slow, 1500}, {100, 3000, Preset::hf, 1500}};
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        EncoderConfig config = preset_config(cases[c].preset);
        config.tone_hz = cases[c].tone;
        config.passband.low_hz = cases[c].low;
        config.passband.high_hz = cases[c].high;
        REQUIRE(config.valid());
        // Each side's shift tolerance - 10 Hz (spec 1.5): passband_fit(config) already stops where the pitch would
        // leave the search of the receiver that hears it, so every shift stays inside that search.
        const PassbandFit fit = passband_fit(config);
        DecoderConfig receiver;
        receiver.passband = config.passband;
        const Passband range = receiver.search_range();
        const double shifts[] = {-(fit.margin_low_hz - 10.0), fit.margin_high_hz - 10.0};
        for (std::size_t s = 0; s < 2; ++s) {
            const double pitch = cases[c].tone + shifts[s];
            CHECK(pitch >= range.low_hz && pitch <= range.high_hz);
            const std::vector<std::uint8_t> data = random_bytes(20, std::uint32_t(200 + 2 * c + s));
            const Recording recording = single(data, config);
            const std::vector<std::int16_t> samples =
                sideband(recording.samples, sim::Mode::usb, gate_db(preset_slot_ms(cases[c].preset)) + k_gate_margin_db,
                         shifts[s], std::uint32_t(210 + 2 * c + s), config.amplitude, cases[c].low, cases[c].high);
            const Score sc = score(recording, run_decoder(samples, receiver, 4096));
            if (!CHECK(sc.matched == data.size() && sc.wrong_bytes == 0 && sc.extra_bytes == 0)) {
                NOTE("filter %u..%u, preset %d at %u Hz shifted %+.0f Hz", cases[c].low, cases[c].high,
                     int(cases[c].preset), cases[c].tone, shifts[s]);
                report("L19", sc);
            }
        }
    }
    // A station at 2200 Hz and a receiver whose passband ends at 1800 Hz: nothing.
    EncoderConfig config = preset_config(Preset::hf);
    config.tone_hz = 2200;
    DecoderConfig receiver;
    receiver.passband.high_hz = 1800;
    Recording recording;
    const Score sc = score(recording, run(config, random_bytes(10, 220), receiver, 99.0, 0, recording));
    CHECK(sc.locks == 0 && sc.bytes_released == 0);
}

// L20: a receiver started inside a transmission joins it when N is a multiple of 8 (spec 3.12 cold late join), bytes
// aligned; for other N it waits. Fast version: 6 starts per (N, T) at gate + 3 dB and at 20 dB.
TEST(decoder_l20_cold_late_join) {
    const double k_join_packages = 6.0;
    const std::uint8_t joining[] = {8, 16, 24, 32};
    const double slots_ms[] = {8, 16, 32};
    std::size_t starts = 0;
    std::size_t joined = 0;
    std::size_t in_time = 0;
    double worst = 0.0;
    std::mt19937 generator(230);
    for (std::size_t b = 0; b < test::count_of(joining); ++b) {
        if (joining[b] > k_max_bits_per_package) continue;
        for (std::size_t t = 0; t < test::count_of(slots_ms); ++t) {
            EncoderConfig config = slot_config(slots_ms[t], joining[b]);
            if (!config.valid()) continue;
            const std::vector<std::uint8_t> data = random_bytes(200, std::uint32_t(231 + 10 * b + t));
            const Recording recording = single(data, config);
            const Transmission& tx = recording.transmissions[0];
            const double package = (joining[b] + 1) * slot_samples(config);
            for (int noisy = 0; noisy < 2; ++noisy) {
                const double snr = noisy ? gate_db(slots_ms[t]) + k_gate_margin_db : 20.0;
                const std::vector<std::int16_t> samples =
                    usb(recording.samples, snr, std::uint32_t(232 + b * 10 + t), config.amplitude);
                const std::size_t first = static_cast<std::size_t>(slot_start_sample(tx, package_start_slot(tx, 2)));
                const std::size_t last =
                    static_cast<std::size_t>(slot_start_sample(tx, package_start_slot(tx, package_count(tx) - 12)));
                for (int k = 0; k < 3; ++k) {
                    const std::size_t from = first + generator() % (last - first);
                    Recording tail;
                    tail.samples.assign(samples.begin() + from, samples.end());
                    Transmission shifted = tx;
                    shifted.start_sample = 0;
                    tail.transmissions.push_back(shifted);
                    const Profile profile = slots_ms[t] < 8.0 ? Profile::fm : Profile::ssb;
                    const Capture capture = run_decoder(tail.samples, DecoderConfig::for_profile(profile), 0);
                    const Mapping m = map_events(tail, capture);
                    ++starts;
                    const std::size_t locked = find_event(capture, EventType::locked);
                    CHECK_EQ(m.score.wrong_bytes, 0u);
                    CHECK_EQ(m.score.extra_bytes, 0u);
                    if (locked >= capture.events.size()) {
                        NOTE("N %u T %.0f ms %s start %zu: no join", unsigned(joining[b]), slots_ms[t],
                             noisy ? "gate+3" : "20 dB", from);
                        continue;
                    }
                    ++joined;
                    CHECK((capture.events[locked].flags & event_flag_late_join) != 0);
                    const double packages = capture.event_sample[locked] / package;
                    worst = std::max(worst, packages);
                    if (packages <= k_join_packages) ++in_time;
                }
            }
        }
    }
    // Spec L20 asks for the lock within 6 packages; the tone lock (20 ms blocks), the fine AFC's first look (up to 1 s
    // on data) and the chain of 3 intervals plus the full guard take longer on short packages: reported, not gated.
    NOTE("cold joins: %zu of %zu starts joined, %zu within %.0f packages, slowest %.1f packages", joined, starts, in_time,
         k_join_packages, worst);
    CHECK(joined * 100 >= starts * 95);
    // Other N: no lock, no byte.
    const std::uint8_t waiting[] = {3, 4, 5, 7, 12};
    for (std::size_t b = 0; b < test::count_of(waiting); ++b) {
        EncoderConfig config = preset_config(Preset::hf);
        config.bits_per_package = waiting[b];
        const Recording recording = single(random_bytes(120, std::uint32_t(250 + b)), config);
        const Transmission& tx = recording.transmissions[0];
        const std::size_t from = static_cast<std::size_t>(slot_start_sample(tx, package_start_slot(tx, 5)) + 37);
        const std::vector<std::int16_t> tail(recording.samples.begin() + from, recording.samples.end());
        for (int noisy = 0; noisy < 2; ++noisy) {
            const std::vector<std::int16_t> samples = noisy ? usb(tail, 20.0, std::uint32_t(251 + b), config.amplitude) : tail;
            const Capture capture = run_decoder(samples, DecoderConfig(), 4096);
            if (!CHECK(count_events(capture, EventType::locked) == 0 && count_events(capture, EventType::byte) == 0)) {
                NOTE("N %u joined", unsigned(waiting[b]));
            }
        }
    }
}

// A receiver started inside a transmission (spec 3.12, cold late join) at T = 8 ms and N = 8, the release's slowest
// case (here the release joined 22 of 24 starts, none within 6 packages, median 9.7): it keeps what it hears from its
// first search block through the tone lock, re-mixed to the tone (§0.7 I29, I30), and joins at a median of about 5.4
// packages; on a search bin and between two (the history re-mixed across the lock and the provisional tune's moves).
// Every start joins with no wrong byte; a few start late for reasons of chance (no energy on the pitch in the first
// blocks, a marker's twist leading the search to a sideband, the rival package lengths of rule I28 deciding a package
// later): the 95 % gate is the long L20's.
TEST(decoder_cold_join_from_the_first_block) {
    const double k_join_packages = 6.0;
    const double k_median_packages = 5.6;
    const std::size_t k_min_in_time = 20;  // of 24
    const std::uint16_t pitches[] = {1500, 1522, 1544};
    const int starts = 8;
    std::size_t joined = 0;
    std::size_t in_time = 0;
    double worst = 0.0;
    std::vector<double> times;
    for (std::size_t p = 0; p < test::count_of(pitches); ++p) {
        const EncoderConfig config = slot_config(8.0, 8, pitches[p]);
        const Recording recording = single(random_bytes(200, std::uint32_t(840 + p)), config);
        const Transmission& tx = recording.transmissions[0];
        const std::vector<std::int16_t> samples =
            usb(recording.samples, 20.0, std::uint32_t(841 + p), config.amplitude);
        const double package = 9.0 * slot_samples(config);
        const std::size_t first = static_cast<std::size_t>(slot_start_sample(tx, package_start_slot(tx, 2)));
        const std::size_t last =
            static_cast<std::size_t>(slot_start_sample(tx, package_start_slot(tx, package_count(tx) - 12)));
        std::mt19937 generator(std::uint32_t(842 + p));
        for (int k = 0; k < starts; ++k) {
            const std::size_t from = first + generator() % (last - first);
            Recording tail;
            tail.samples.assign(samples.begin() + static_cast<long>(from), samples.end());
            Transmission shifted = tx;
            shifted.start_sample = 0;
            tail.transmissions.push_back(shifted);
            const Capture capture = run_decoder(tail.samples, DecoderConfig(), 0);
            const Mapping m = map_events(tail, capture);
            CHECK_EQ(m.score.wrong_bytes, 0u);
            CHECK_EQ(m.score.extra_bytes, 0u);
            const std::size_t locked = find_event(capture, EventType::locked);
            if (locked >= capture.events.size()) {
                NOTE("pitch %u start %zu: no join", unsigned(pitches[p]), from);
                worst = 1e9;
                continue;
            }
            ++joined;
            CHECK((capture.events[locked].flags & event_flag_late_join) != 0);
            const double packages = capture.event_sample[locked] / package;
            worst = std::max(worst, packages);
            times.push_back(packages);
            if (packages <= k_join_packages) ++in_time;
        }
    }
    std::sort(times.begin(), times.end());
    const double median = times.empty() ? 1e9 : times[times.size() / 2];
    NOTE("T = 8 ms, N = 8, 20 dB: %zu of %zu starts joined, %zu within %.0f packages, median %.2f, slowest %.2f "
         "packages",
         joined, test::count_of(pitches) * starts, in_time, k_join_packages, median, worst);
    CHECK_EQ(joined, test::count_of(pitches) * starts);
    CHECK(median <= k_median_packages);
    CHECK(in_time >= k_min_in_time);
}

// A lock taken on keyed data between two search bins (a receiver started inside a transmission) stays within the fine
// AFC's +-30 Hz of the pitch and is on it within 2 Hz 300 ms after the lock (§0.7 I31): the AFC measures the tone
// against the provisional tune, the leading bin kept at most half a bin from it. The release tuned to the tone
// search's estimate, which on keyed data can be half a bin or a whole one off (here up to 76 Hz, still 76 Hz at
// 300 ms): beyond the AFC's reach it walked away and never joined.
TEST(decoder_lock_on_data_is_tuned) {
    const std::uint16_t pitches[] = {1512, 1525, 1531, 1538};
    const double slots_ms[] = {8.0, 16.0};
    const int starts = 5;
    const double reach_hz = 30.0;
    const double tolerance_hz = 2.0;
    const double settle_ms = 300.0;
    double widest = 0.0;
    double worst = 0.0;
    std::size_t locks = 0;
    for (std::size_t p = 0; p < test::count_of(pitches); ++p) {
        for (std::size_t t = 0; t < test::count_of(slots_ms); ++t) {
            const EncoderConfig config = slot_config(slots_ms[t], 8, pitches[p]);
            const Recording recording = single(random_bytes(120, std::uint32_t(850 + 10 * p + t)), config);
            const Transmission& tx = recording.transmissions[0];
            const std::vector<std::int16_t> samples =
                usb(recording.samples, 20.0, std::uint32_t(851 + 10 * p + t), config.amplitude);
            const std::size_t first = static_cast<std::size_t>(slot_start_sample(tx, package_start_slot(tx, 2)));
            const std::size_t last =
                static_cast<std::size_t>(slot_start_sample(tx, package_start_slot(tx, package_count(tx) - 8)));
            std::mt19937 generator(std::uint32_t(852 + 10 * p + t));
            for (int k = 0; k < starts; ++k) {
                const std::size_t from = first + generator() % (last - first);
                Decoder decoder(DecoderConfig(), 0, 0);
                const std::size_t settle = static_cast<std::size_t>(settle_ms * k_decoder_rate_hz / 1000.0);
                std::size_t after = 0;
                for (std::size_t i = from; i < samples.size() && after <= settle; ++i) {
                    decoder.process_sample(samples[i]);
                    if (decoder.state() == DecoderState::search) continue;
                    ++after;
                    widest = std::max(widest, static_cast<double>(std::fabs(decoder.tone_hz() - pitches[p])));
                }
                if (after <= settle) continue;
                ++locks;
                worst = std::max(worst, static_cast<double>(std::fabs(decoder.tone_hz() - pitches[p])));
            }
        }
    }
    NOTE("%zu locks on keyed data between bins: at most %.2f Hz off after the lock, %.2f Hz at %.0f ms", locks, widest,
         worst, settle_ms);
    CHECK_EQ(locks, test::count_of(pitches) * test::count_of(slots_ms) * starts);
    CHECK(widest <= reach_hz);
    CHECK(worst <= tolerance_hz);
}

// Keyed data ACQUIRE holds has spectral lines k / T from its tone (250 Hz at T = 8 ms) that stay steady within a
// package and break as its markers turn its carrier over: the watch must not take them for a new transmission's train
// (§0.7 I32). The release left the held data for them in 4 of these 24 starts (and forgot its history), joining 4 to
// 14 packages late.
TEST(decoder_watch_ignores_keying_lines) {
    const std::uint8_t bits[] = {24, 32};
    const int starts = 12;
    std::size_t jumps = 0;
    std::size_t runs = 0;
    for (std::size_t b = 0; b < test::count_of(bits); ++b) {
        const EncoderConfig config = slot_config(8.0, bits[b], 1500);
        const Recording recording = single(random_bytes(200, std::uint32_t(860 + b)), config);
        const Transmission& tx = recording.transmissions[0];
        const std::vector<std::int16_t> samples =
            usb(recording.samples, 20.0, std::uint32_t(861 + b), config.amplitude);
        const double package = (bits[b] + 1.0) * slot_samples(config);
        const std::size_t first = static_cast<std::size_t>(slot_start_sample(tx, package_start_slot(tx, 2)));
        const std::size_t last =
            static_cast<std::size_t>(slot_start_sample(tx, package_start_slot(tx, package_count(tx) - 8)));
        std::mt19937 generator(std::uint32_t(862 + b));
        for (int k = 0; k < starts; ++k) {
            const std::size_t from = first + generator() % (last - first);
            Decoder decoder(DecoderConfig(), 0, 0);
            const std::size_t span = static_cast<std::size_t>(6.0 * package);  // the join's time
            bool jumped = false;
            for (std::size_t i = from; i < samples.size() && i < from + span; ++i) {
                decoder.process_sample(samples[i]);
                const bool away = std::fabs(decoder.tone_hz() - 1500.0f) > 100.0f;
                if (decoder.state() != DecoderState::search && away) jumped = true;
            }
            ++runs;
            if (jumped) {
                ++jumps;
                NOTE("N %u start %zu: the watch left the held data", unsigned(bits[b]), from);
            }
        }
    }
    NOTE("%zu of %zu starts left the held data for a line of its keying", jumps, runs);
    CHECK_EQ(jumps, 0u);
}

// R1: a train heard without its tune at every sample alignment of a tone-search block: the watch never leaves a valid
// preamble for the train's own spectral lines.
TEST(decoder_r1_watch_ignores_train_lines) {
    struct Case {
        double slot_ms;
        std::uint16_t tone_hz;
        std::uint8_t sync_markers;
        Profile profile;
    };
    const Case cases[] = {{20, 2000, 12, Profile::ssb}, {8, 2313, 32, Profile::am}};
    const std::size_t alignments = 160;  // one tone-search block
    const std::size_t lead_samples = 8000;
    const std::vector<std::uint8_t> data = random_bytes(30, 1);
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        EncoderConfig config = slot_config(cases[c].slot_ms, 8, cases[c].tone_hz);
        config.tune_ms = 0;
        config.sync_markers = cases[c].sync_markers;
        REQUIRE(config.valid());
        const DecoderConfig receiver = DecoderConfig::for_profile(cases[c].profile);
        std::size_t failures = 0;
        for (std::size_t offset = 0; offset < alignments; ++offset) {
            Recording recording;
            recording.samples.assign(lead_samples + offset, 0);
            append_transmission(recording, data, config);
            append_silence(recording, 1000.0);
            const Score s = score(recording, run_decoder(recording.samples, receiver, 4096));
            if (s.lost_bytes + s.wrong_bytes + s.extra_bytes != 0) ++failures;
        }
        NOTE("T %.0f ms, %u sync markers at %u Hz: %zu of %zu alignments lose bytes", cases[c].slot_ms,
             unsigned(cases[c].sync_markers), unsigned(cases[c].tone_hz), failures, alignments);
        CHECK_EQ(failures, 0u);
    }
}

// R2: an input driven 1.5..4 times too hot (clipped odd harmonics fold into the band and reverse with the markers).
TEST(decoder_r2_saturated_input) {
    struct Case {
        Preset preset;
        double gain;
    };
    const Case cases[] = {{Preset::hf_slow, 1.5}, {Preset::hf_slow, 2.0}, {Preset::hf_slow, 4.0},
                          {Preset::hf, 1.8},      {Preset::hf, 3.0},      {Preset::hf_fast, 4.0}};
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        const EncoderConfig config = preset_config(cases[c].preset);
        const Recording recording = single(random_bytes(30, 94), config, 1000.0);
        const Score s = score(recording, run_decoder(clipped(recording.samples, cases[c].gain), DecoderConfig(), 4096));
        if (!CHECK(s.lost_bytes == 0 && s.wrong_bytes == 0 && s.extra_bytes == 0)) {
            NOTE("preset %d, gain x%.1f", int(cases[c].preset), cases[c].gain);
            report("R2", s);
        }
    }
}

// R3: a tune heard alone (its train and packages faded) and the station's retry 5..12 s later on the same pitch: the
// tone is banned at the ACQUIRE timeout only while still present, so the retry decodes.
TEST(decoder_r3_retry_after_a_lone_tune) {
    const EncoderConfig config = preset_config(Preset::hf);
    const double starts_s[] = {5.3, 8.0, 12.0};
    const std::vector<std::uint8_t> first = random_bytes(40, 101);
    const std::vector<std::uint8_t> retry = random_bytes(40, 102);
    for (std::size_t g = 0; g < test::count_of(starts_s); ++g) {
        Recording recording;
        append_silence(recording, k_gap_ms);
        append_transmission(recording, first, config);
        const std::size_t first_sync = first_start_slot(config) + 1 - config.sync_markers;
        const std::size_t slots = static_cast<std::size_t>(recording.transmissions[0].length / slot_samples(config)) + 2;
        for (std::size_t slot = first_sync; slot < slots; ++slot) scale_slot(recording, 0, slot, 0.0);
        recording.samples.resize(recording.transmissions[0].start_sample +
                                     static_cast<std::size_t>(starts_s[g] * k_decoder_rate_hz), 0);
        append_transmission(recording, retry, config);
        append_silence(recording, 3000.0);
        const Capture capture =
            run_decoder(usb(recording.samples, 15.0, std::uint32_t(55 + g), config.amplitude), DecoderConfig(), 4096);
        const Mapping m = map_events(recording, capture);
        std::size_t received = 0;
        for (std::size_t i = 0; i < retry.size(); ++i) received += m.received[1][i] == retry[i] ? 1 : 0;
        NOTE("retry %.1f s after the lone tune: %zu of %zu bytes", starts_s[g], received, retry.size());
        CHECK_EQ(received, retry.size());
        CHECK_EQ(m.score.wrong_bytes + m.score.extra_bytes, 0u);
    }
}

// R4: a same-pitch transmission (or a carrier) keying up 125..200 ms before the previous one ends masks its END
// markers (at 200 ms its last STOP too): 0 wrong or extra bytes, at most the last package dropped.
TEST(decoder_r4_next_tune) {
    const EncoderConfig config = preset_config(Preset::hf);
    const double overlaps_ms[] = {125.0, 150.0, 175.0, 200.0};
    const int seeds = 3;
    const std::size_t lead_samples = 4000;
    const std::size_t trailing_samples = 16000;
    const std::size_t bytes = 43;
    const double pi = 3.14159265358979323846;
    std::size_t extra = 0;
    std::size_t wrong = 0;
    std::size_t released = 0;
    std::size_t runs = 0;
    for (std::size_t o = 0; o < test::count_of(overlaps_ms); ++o) {
        for (int seed = 0; seed < seeds; ++seed) {
            for (int carrier = 0; carrier < 2; ++carrier) {
                const std::vector<std::uint8_t> first = random_bytes(bytes, std::uint32_t(401 + 2 * seed));
                const std::vector<std::int16_t> a = encode(first, config);
                const std::vector<std::int16_t> b = encode(random_bytes(bytes, std::uint32_t(402 + 2 * seed)), config);
                const std::size_t overlap = static_cast<std::size_t>(overlaps_ms[o] * k_decoder_rate_hz / 1000.0);
                const std::size_t b0 = lead_samples + a.size() - overlap;
                std::vector<double> mix(b0 + b.size() + trailing_samples, 0.0);
                for (std::size_t i = 0; i < a.size(); ++i) mix[lead_samples + i] += a[i];
                for (std::size_t i = 0; i < b.size(); ++i) {
                    const double tone = config.amplitude * std::sin(2.0 * pi * config.tone_hz * i / k_decoder_rate_hz);
                    mix[b0 + i] += carrier != 0 ? tone : b[i];
                }
                std::vector<std::int16_t> samples(mix.size());
                for (std::size_t i = 0; i < mix.size(); ++i) {
                    samples[i] = static_cast<std::int16_t>(std::lround(std::max(-32768.0, std::min(32767.0, 0.6 * mix[i]))));
                }
                const Capture capture =
                    run_decoder(usb(samples, 20.0, std::uint32_t(403 + seed), config.amplitude), DecoderConfig(), 4096);
                int lock = 0;
                for (std::size_t i = 0; i < capture.events.size() && lock < 2; ++i) {
                    const Event& e = capture.events[i];
                    if (e.type == EventType::locked) ++lock;
                    if ((e.type == EventType::end || e.type == EventType::lost) && lock == 1) lock = 2;
                    if (e.type != EventType::byte || lock != 1) continue;
                    ++released;
                    if (e.byte_index >= first.size()) {
                        ++extra;
                    } else if (first[e.byte_index] != e.value) {
                        ++wrong;
                    }
                }
                ++runs;
            }
        }
    }
    NOTE("%zu runs: the first transmission released %zu bytes, %zu wrong, %zu past its end", runs, released, wrong, extra);
    CHECK_EQ(extra, 0u);
    CHECK_EQ(wrong, 0u);
    CHECK(released + runs >= runs * bytes);  // at most the last package (one byte at N = 8) dropped
}

// R5: the receiver's VFO steps or drifts after the lock (20 dB): the rotation AFC of the missed STOPs pulls the NCO back
// (T >= 32 ms): wrong bytes within 3 packages' worth, 0 extra, the END found, no loss.
TEST(decoder_r5_frequency_step) {
    struct Case {
        double slot_ms;
        double step_hz;
        double ramp_hz_per_s;
        std::size_t bytes;
    };
    const Case cases[] = {{64, 10.0, 0.0, 60}, {128, 5.0, 0.0, 40}, {128, -5.0, 0.0, 40},
                          {32, -20.0, 0.0, 100}, {64, 0.0, 0.8, 60}, {128, 0.0, 0.5, 40}};
    const double step_after_s = 3.0;
    const std::size_t transient_packages = 3;
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        const EncoderConfig config = slot_config(cases[c].slot_ms, 8);
        const Recording recording = single(random_bytes(cases[c].bytes, std::uint32_t(501 + c)), config, 500.0);
        const Transmission& t = recording.transmissions[0];
        std::vector<std::int16_t> samples = recording.samples;
        samples.resize(samples.size() + static_cast<std::size_t>(40.0 * slot_samples(config)), 0);
        const std::size_t from =
            static_cast<std::size_t>(slot_start_sample(t, package_start_slot(t, 0)) + step_after_s * k_decoder_rate_hz);
        const std::vector<std::int16_t> shifted = retuned(samples, from, cases[c].step_hz, cases[c].ramp_hz_per_s);
        Recording scored = recording;
        scored.samples = usb(shifted, 20.0, std::uint32_t(502 + c), config.amplitude);
        const Mapping m = map_events(scored, run_decoder(scored.samples, receiver_for(cases[c].slot_ms), 4096));
        const std::size_t limit = transient_packages * config.bits_per_package / 8;
        NOTE("T %.0f ms, step %+.1f Hz, drift %.1f Hz/s: wrong %zu (limit %zu), lost %zu, extra %zu, ends %zu",
             cases[c].slot_ms, cases[c].step_hz, cases[c].ramp_hz_per_s, m.score.wrong_bytes, limit, m.score.lost_bytes,
             m.score.extra_bytes, m.score.ends);
        CHECK(m.score.wrong_bytes <= limit);
        CHECK_EQ(m.score.extra_bytes, 0u);
        CHECK_EQ(m.score.ends, 1u);
        CHECK_EQ(m.score.lost_events, 0u);
    }
}

// R6: hf_fast one dB below its gate, mistuned by up to +-50 Hz: at least 38 of 40 transmissions lock at the sender's
// T and N.
TEST(decoder_r6_weak_markers) {
    const EncoderConfig config = preset_config(Preset::hf_fast);
    const int trials = 40;
    const int required = 38;
    const double snr_db = gate_db(8.0) - 1.0;
    const double offset_span_hz = 100.0;
    std::mt19937 generator(190);
    std::uniform_real_distribution<double> offset(-0.5 * offset_span_hz, 0.5 * offset_span_hz);
    int locked = 0;
    for (int trial = 0; trial < trials; ++trial) {
        Recording recording;
        append_silence(recording, 1000.0);
        append_transmission(recording, random_bytes(8, std::uint32_t(191 + trial)), config);
        append_silence(recording, 200.0);
        const Capture capture = run_decoder(
            usb(recording.samples, snr_db, std::uint32_t(240 + trial), config.amplitude, offset(generator)),
            DecoderConfig(), 4096);
        bool ok = false;
        for (std::size_t i = 0; i < capture.events.size(); ++i) {
            const Event& e = capture.events[i];
            ok = ok || (e.type == EventType::locked && e.bits_per_package == config.bits_per_package &&
                        std::fabs(e.slot_ms / (config.slot_us / 1000.0) - 1.0) <= k_slot_tolerance);
        }
        locked += ok ? 1 : 0;
    }
    NOTE("hf_fast at %+.1f dB: %d of %d transmissions locked", snr_db, locked, trials);
    CHECK(locked >= required);
}

// R7: a receiver AGC (1 ms attack, 300 ms decay) at 30 dB: between transmissions it raises the noise by the SNR; the
// search must not chase it, and the train is read against the noise under the tune.
TEST(decoder_r7_agc_back_to_back) {
    const EncoderConfig config = preset_config(Preset::hf);
    const std::size_t transmissions = 4;
    const double gap_ms = 1500.0;
    Recording recording;
    append_silence(recording, gap_ms);
    for (std::size_t i = 0; i < transmissions; ++i) {
        append_transmission(recording, random_bytes(24, std::uint32_t(180 + i)), config);
        append_silence(recording, gap_ms);
    }
    sim::ChannelConfig channel;
    channel.mode = sim::Mode::usb;
    channel.snr_db = 30.0;
    channel.seed = 181;
    channel.agc = true;
    channel.agc_attack_ms = 1.0;
    channel.agc_decay_ms = 300.0;
    const Score s = score(recording, run_decoder(through_channel(recording.samples, channel, config.amplitude),
                                                 DecoderConfig(), 4096));
    if (!CHECK(s.locks == transmissions && s.lost_bytes == 0 && s.wrong_bytes == 0 && s.extra_bytes == 0)) {
        report("R7", s);
    }
}

// R8: a +6 dB steady carrier or 0 dB keyed CW 300 Hz from the pitch, present from the first sample: one lock, no loss;
// the interferer alone: no lock, no byte. hf_fast (8 ms): 300 Hz is inside its band (+-275 Hz) and inside the
// passband of its 2.8 ms marker halves, so its interferer sits 600 Hz away; the search holds the interferer when the
// tune starts and the watch leaves it only at the 64 ms train, too late to read it, and the transmission is joined
// cold (N = 8) with its first bytes missing: one lock, nothing wrong (reported).
TEST(decoder_r8_qrm_from_start) {
    const Preset presets[] = {Preset::hf_slow, Preset::hf, Preset::hf_fast};
    const double offsets_hz[] = {300.0, 300.0, 600.0};
    const double snr_db = 10.0;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        const double offset_hz = offsets_hz[p];
        const bool joined_cold = presets[p] == Preset::hf_fast;
        for (int carrier = 0; carrier < 2; ++carrier) {
            const EncoderConfig config = preset_config(presets[p]);
            sim::ChannelConfig channel;
            channel.mode = sim::Mode::usb;
            channel.snr_db = snr_db;
            channel.seed = std::uint32_t(160 + p);
            if (carrier != 0) {
                channel.carrier_hz = config.tone_hz - offset_hz;
                channel.carrier_db = 6.0;
            } else {
                channel.cw_hz = config.tone_hz + offset_hz;
                channel.cw_db = 0.0;
            }
            Recording recording;
            append_transmission(recording, random_bytes(20, std::uint32_t(161 + p)), config);
            append_silence(recording, 1000.0);
            const Score s = score(recording, run_decoder(through_channel(recording.samples, channel, config.amplitude),
                                                         DecoderConfig(), 4096));
            const bool ok = s.wrong_bytes == 0 && s.extra_bytes == 0 && s.locks == 1 && (joined_cold || s.lost_bytes == 0);
            if (!CHECK(ok) || joined_cold) {
                NOTE("preset %d, %s: %zu of %zu bytes", int(presets[p]), carrier ? "carrier +6 dB" : "CW 0 dB",
                     s.matched, s.bytes_sent);
                if (!ok) report("R8", s);
            }
        }
    }
    for (int carrier = 0; carrier < 2; ++carrier) {
        sim::ChannelConfig channel;
        channel.mode = sim::Mode::usb;
        channel.snr_db = snr_db;
        channel.seed = 77;
        channel.carrier_hz = carrier ? 1200.0 : 0.0;
        channel.carrier_db = 6.0;
        channel.cw_hz = carrier ? 0.0 : 1800.0;
        channel.cw_db = 0.0;
        const std::vector<std::int16_t> quiet(static_cast<std::size_t>(20 * k_decoder_rate_hz), 0);
        const Capture capture = run_decoder(through_channel(quiet, channel, preset_config(Preset::hf).amplitude),
                                            DecoderConfig(), 4096);
        CHECK_EQ(count_events(capture, EventType::locked), 0u);
        CHECK_EQ(count_events(capture, EventType::byte), 0u);
    }
}

// R9: fm near and below the FM threshold: no byte outside a confirmed lock, every lock at the sender's T and N.
TEST(decoder_r9_fm_threshold_integrity) {
    const EncoderConfig config = preset_config(Preset::fm);
    const double snrs[] = {7.0, 8.0, 9.0};  // carrier SNR in 2500 Hz
    std::size_t extra = 0;
    for (std::size_t n = 0; n < test::count_of(snrs); ++n) {
        for (std::uint32_t trial = 0; trial < 3; ++trial) {
            const Recording recording = single(random_bytes(40, 300 + trial), config, 400.0);
            sim::ChannelConfig channel;
            channel.mode = sim::Mode::fm;
            channel.snr_db = snrs[n];
            channel.seed = std::uint32_t(500 + trial + 10 * n);
            const Capture capture = run_decoder(through_channel(recording.samples, channel, config.amplitude),
                                                DecoderConfig::for_profile(Profile::fm), 4096);
            CHECK(bytes_inside_locks(capture));
            for (std::size_t i = 0; i < capture.events.size(); ++i) {
                const Event& e = capture.events[i];
                if (e.type != EventType::locked) continue;
                CHECK(std::fabs(e.slot_ms / (config.slot_us / 1000.0) - 1.0) <= k_slot_tolerance);
                CHECK_EQ(unsigned(e.bits_per_package), unsigned(config.bits_per_package));
            }
            extra += score(recording, capture).extra_bytes;
        }
    }
    NOTE("extra bytes near the FM threshold: %zu", extra);
    CHECK_EQ(extra, 0u);
}

// R10: the SNR report at hf within +-1.5 dB from the gate to gate + 20 dB.
TEST(decoder_r10_snr_report) {
    const EncoderConfig config = preset_config(Preset::hf);
    const double tolerance_db = 1.5;
    for (double extra = 0.0; extra <= 20.0; extra += 10.0) {
        const double snr = gate_db(16.0) + extra;
        double reported = 0.0;
        std::size_t counted = 0;
        for (int r = 0; r < 3; ++r) {
            const std::uint32_t seed = std::uint32_t(200 + 10 * r + extra);
            const Recording recording = single(random_bytes(20, seed), config);
            std::size_t count = 0;
            const double mean = reported_snr(run_decoder(usb(recording.samples, snr, seed, config.amplitude),
                                                         DecoderConfig(), 4096), count);
            reported += mean * count;
            counted += count;
        }
        REQUIRE(counted > 0);
        reported /= counted;
        NOTE("hf at %+.1f dB: reported %+.1f dB", snr, reported);
        CHECK_NEAR(reported, snr, tolerance_db);
    }
}

// U27: the TRACK noise estimate (seen through the SNR report, whose crest is exact at these levels) within +-1 dB from
// the gate to gate + 10 dB for N = 1, 2 and 8; with only half-quiet gaps (N = 1) at 30 dB its bias stays within 3 dB
// and the bits are all right.
TEST(decoder_u27_noise_estimate) {
    const std::uint8_t bits[] = {1, 2, 8};
    for (std::size_t b = 0; b < test::count_of(bits); ++b) {
        EncoderConfig config = preset_config(Preset::hf);
        config.bits_per_package = bits[b];
        for (double extra = 0.0; extra <= 10.0; extra += 5.0) {
            const double snr = gate_db(16.0) + extra;
            double reported = 0.0;
            std::size_t counted = 0;
            for (int r = 0; r < 3; ++r) {
                const std::uint32_t seed = std::uint32_t(270 + 10 * b + r + extra);
                const Recording recording = single(random_bytes(20, seed), config);
                std::size_t count = 0;
                const double mean = reported_snr(run_decoder(usb(recording.samples, snr, seed, config.amplitude),
                                                             DecoderConfig(), 4096), count);
                reported += mean * count;
                counted += count;
            }
            REQUIRE(counted > 0);
            reported /= counted;
            if (!CHECK_NEAR(reported, snr, 1.0)) NOTE("N %u at %+.1f dB: reported %+.1f dB", unsigned(bits[b]), snr, reported);
        }
    }
    EncoderConfig config = preset_config(Preset::hf);
    config.bits_per_package = 1;
    const double snr = 30.0;
    const Recording recording = single(random_bytes(20, 299), config);
    const Capture capture = run_decoder(usb(recording.samples, snr, 299, config.amplitude), DecoderConfig(), 4096);
    std::size_t count = 0;
    const double reported = reported_snr(capture, count);
    const Score s = score(recording, capture);
    NOTE("N 1 at %.0f dB: reported %+.1f dB", snr, reported);
    CHECK(count > 0 && reported >= snr - 3.0);
    CHECK(s.matched == recording.transmissions[0].data.size() && s.bit_errors == 0);
}

namespace {

// A transmission rendered from its slot kinds (spec 1.2) between silences, as a recording the scorers understand.
Recording kinds_recording(double slot_ms, unsigned bits, const std::vector<std::uint8_t>& data,
                          const std::vector<int>& kinds) {
    Recording recording;
    append_silence(recording, k_gap_ms);
    Transmission transmission;
    transmission.config = slot_config(slot_ms, static_cast<std::uint8_t>(bits));
    transmission.data = data;
    transmission.start_sample = recording.samples.size();
    const std::vector<std::int16_t> samples = render_kinds(slot_ms, kinds);
    transmission.length = samples.size();
    recording.samples.insert(recording.samples.end(), samples.begin(), samples.end());
    recording.transmissions.push_back(transmission);
    append_silence(recording, 2.0 * k_gap_ms);
    return recording;
}

// Every released byte where it belongs (spec 3.13, V5): a shifted byte scores as a wrong one.
bool bytes_in_place(const Recording& recording, const Capture& capture, const char* label) {
    const Mapping m = map_events(recording, capture);
    const bool ok = CHECK_EQ(m.score.wrong_bytes, 0u) && CHECK_EQ(m.score.extra_bytes, 0u);
    if (!ok) report(label, m.score);
    return ok;
}

const double k_integrity_snr_db = 20.0;
const std::int16_t k_render_amplitude = 23197;

}  // namespace

// Spec 3.8 step 7: packages that end before N is confirmed are the whole transmission only when the first of them is
// package 0. N = 32, 16 bytes, the STOP of package 1 faded: the preamble meets the last two packages alone, then END
// (long suite A3: they were released as bytes 0-3).
TEST(decoder_integrity_short_end_is_package_0) {
    const unsigned bits = 32;
    for (std::uint32_t seed = 1; seed <= 3; ++seed) {
        const std::vector<std::uint8_t> data = random_bytes(16, seed);
        std::size_t first_data = 0;
        std::vector<int> kinds = transmission_kinds(16.0, bits, data, first_data);
        kinds[first_data + 2 * (bits + 1) - 1] = slot_zero;  // the STOP of package 1
        const Recording recording = kinds_recording(16.0, bits, data, kinds);
        bytes_in_place(recording,
                       run_decoder(usb(recording.samples, k_integrity_snr_db, seed, k_render_amplitude),
                                   receiver_for(16.0), 4096),
                       "short END");
    }
}

// Spec 3.8 step 5: at most 3 train markers fade at the train's end, so markers in package 0's data never extend the
// train: markers in its data slots 5..7 used to move the train's end into the data, and every package index was one
// off (long suite C8 at -300 Hz: 40 bytes shifted).
TEST(decoder_integrity_train_ends_at_its_gap) {
    const unsigned bits = 8;
    for (std::uint32_t seed = 1; seed <= 3; ++seed) {
        const std::vector<std::uint8_t> data = random_bytes(24, seed);
        std::size_t first_data = 0;
        std::vector<int> kinds = transmission_kinds(16.0, bits, data, first_data);
        for (std::size_t slot = 4; slot <= 6; ++slot) kinds[first_data + slot] = slot_marker;
        const Recording recording = kinds_recording(16.0, bits, data, kinds);
        bytes_in_place(recording,
                       run_decoder(usb(recording.samples, k_integrity_snr_db, seed, k_render_amplitude),
                                   receiver_for(16.0), 4096),
                       "train in data");
    }
}

// Spec 3.7, 3.12: a sync train inside a transmission whose lock was lost is data beeps (fading and filter edges twist
// them), never the start of a new one: packages 20..25 faded, then package 27's ones twisted like a train. The
// stream relocks from the station memory (long suite C14 hf_fast at -915 Hz: 292 bytes shifted); after a fade too long
// for the memory the sync is refused without a tune before it and a cold join takes over (C2 hf_fast: 179 bytes).
TEST(decoder_integrity_no_sync_inside_a_transmission) {
    const unsigned bits = 8;
    struct Case {
        std::size_t bytes;
        std::size_t fade_from;
        std::size_t fade_to;
        std::size_t twisted;
        std::uint32_t seed;
    };
    const Case cases[] = {{60, 20, 25, 27, 1}, {60, 20, 25, 27, 2}, {160, 20, 95, 100, 2}};
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        std::vector<std::uint8_t> data = random_bytes(cases[c].bytes, cases[c].seed);
        data[cases[c].twisted] = 0xFF;  // N = 8: package k is byte k, a run of ones
        std::size_t first_data = 0;
        std::vector<int> kinds = transmission_kinds(16.0, bits, data, first_data);
        const std::size_t span = bits + 1;
        for (std::size_t p = cases[c].fade_from; p <= cases[c].fade_to; ++p) {
            for (std::size_t k = 0; k < span; ++k) kinds[first_data + p * span + k] = slot_silent;
        }
        for (std::size_t k = 0; k < bits; ++k) kinds[first_data + cases[c].twisted * span + k] = slot_marker;
        const Recording recording = kinds_recording(16.0, bits, data, kinds);
        const Capture capture = run_decoder(
            usb(recording.samples, k_integrity_snr_db, cases[c].seed, k_render_amplitude), receiver_for(16.0), 4096);
        if (!bytes_in_place(recording, capture, "sync in data")) NOTE("case %zu", c);
    }
}

// Spec 3.12 (V7): a cold join takes N only when no other package length explains the chain's slot edges better. N = 12
// is no multiple of 8; its grid read as N = 8 at T' = 13 T / 9 once passed the fold (long suite L20: 14 wrong bytes).
TEST(decoder_cold_join_rival_package_length) {
    const std::uint32_t seed = 301501254;  // long suite L20 row N = 12, T = 16 ms, gate + 3 dB
    const std::size_t start = 60671;
    const EncoderConfig config = slot_config(16.0, 12);
    Recording recording;
    append_silence(recording, 1500.0);
    append_transmission(recording, random_bytes(200, seed), config);
    append_silence(recording, 1500.0);
    const std::vector<std::int16_t> samples = usb(recording.samples, gate_db(16.0) + k_gate_margin_db, seed, config.amplitude);
    const std::vector<std::int16_t> tail(samples.begin() + static_cast<long>(start), samples.end());
    const Capture capture = run_decoder(tail, receiver_for(16.0), 4096);
    CHECK_EQ(count_events(capture, EventType::locked), 0u);
    CHECK_EQ(count_events(capture, EventType::byte), 0u);
}

// Spec 3.11: the full guard's inner flips scale with the audit positions of a package (2 N + 1): noise next to data
// beeps reaches the inner level at random positions. N = 32 short messages at the A3 gate were refused as aliases at
// their last package (long suite A3: 5 of 400; these are those 5). Other standard libraries draw other noise
// (std::normal_distribution): none may be refused as an alias there either, but a few may miss for other reasons.
TEST(decoder_guard_long_packages) {
    struct Case {
        std::uint32_t seed;
        std::uint32_t data_seed;
        double offset_hz;
    };
    const Case cases[] = {{3200241, 99207562, 35.514392753027202},
                          {3200243, 99207626, 43.316360774431942},
                          {3200244, 99207658, 1.7790880039608714},
                          {3200267, 99208394, 11.618942382934394},
                          {3200457, 99214474, -45.969811851562767}};
    const EncoderConfig config = slot_config(16.0, 32);
    std::size_t delivered = 0;
    std::size_t aliases = 0;
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        Recording recording;
        append_silence(recording, 1500.0);
        append_transmission(recording, random_bytes(16, cases[c].data_seed), config);
        append_silence(recording, 1500.0);
        const Capture capture = run_decoder(
            usb(recording.samples, gate_db(16.0), cases[c].seed, config.amplitude, cases[c].offset_hz), receiver_for(16.0),
            4096);
        for (std::size_t e = 0; e < capture.events.size(); ++e) {
            if (capture.events[e].type == EventType::lost && capture.events[e].reason == LostReason::alias) ++aliases;
        }
        const Mapping m = map_events(recording, capture);
        CHECK_EQ(m.score.extra_bytes, 0u);
        if (m.score.locks == 1 && m.score.matched == recording.transmissions[0].data.size()) ++delivered;
    }
    NOTE("%zu of %zu short N = 32 messages delivered, %zu refused as aliases", delivered, test::count_of(cases),
         aliases);
    CHECK_EQ(aliases, 0u);
}

// Spec 2.1, 3.7: a tune locked late, in its last slots: the history holds less than two of them, and the train still
// follows the tune when the tone was locked steady at most the longest train of the sync's T before the sync (that
// bound is taken at the sync's T, not the T of an earlier lock). T = 128 ms, the tune cut to 2 slots, 16 bytes: the
// preamble locks (a cold join would take the transmission too, but only a preamble places package 0 for any N).
TEST(decoder_late_tune_lock) {
    const double slot_ms = 128.0;
    const unsigned bits = 8;
    const std::size_t kept_tune_slots = 2;
    for (std::uint32_t seed = 1; seed <= 3; ++seed) {
        const std::vector<std::uint8_t> data = random_bytes(16, seed);
        std::size_t first_data = 0;
        std::vector<int> kinds = transmission_kinds(slot_ms, bits, data, first_data);
        const std::size_t tune_slots = first_data - k_min_sync_markers;
        kinds.erase(kinds.begin(), kinds.begin() + static_cast<long>(tune_slots - kept_tune_slots));
        const Recording recording = kinds_recording(slot_ms, bits, data, kinds);
        const Capture capture = run_decoder(usb(recording.samples, k_integrity_snr_db, seed, k_render_amplitude),
                                            receiver_for(slot_ms), 4096);
        const Score s = score(recording, capture);
        const bool placed = s.locks == 1 && s.late_joins == 0 && s.lost_bytes == 0;
        if (!CHECK(placed && s.wrong_bytes == 0 && s.extra_bytes == 0)) report("late tune", s);
    }
}

// Spec 3.9 step 9, 3.11: a lock the guard confirms after the transmission ended (its END faded) holds packages the
// flywheel measured past the end: they are no packages, and only those up to the newest detected STOP are released.
// N = 16, 6 bytes (3 packages), a train of 5 markers (not a clean lock: the full guard needs 4 packages), no END.
TEST(decoder_no_phantom_package_past_a_faded_end) {
    const double slot_ms = 16.0;
    const unsigned bits = 16;
    const std::size_t faded_train_markers = 3;
    const std::size_t end_markers = 2;
    for (std::uint32_t seed = 1; seed <= 3; ++seed) {
        const std::vector<std::uint8_t> data = random_bytes(6, seed);
        std::size_t first_data = 0;
        std::vector<int> kinds = transmission_kinds(slot_ms, bits, data, first_data);
        const std::size_t train_first = first_data - k_min_sync_markers;
        for (std::size_t k = 0; k < faded_train_markers; ++k) kinds[train_first + k] = slot_silent;
        for (std::size_t k = 0; k < end_markers; ++k) kinds[kinds.size() - 1 - k] = slot_silent;
        const Recording recording = kinds_recording(slot_ms, bits, data, kinds);
        const Capture capture = run_decoder(usb(recording.samples, k_integrity_snr_db, seed, k_render_amplitude),
                                            receiver_for(slot_ms), 4096);
        bytes_in_place(recording, capture, "faded END");
    }
}
