#include "support/loopback.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace unlimited;
using namespace unlimited::loopback;

namespace {

const double k_pi = 3.14159265358979323846;
const double k_attenuation_30_db = 0.0316227766;
const double k_gap_ms = 500.0;
const double k_gate_margin_db = 3.0;
const double k_usb_offset_hz = 37.0;
const std::int16_t k_amplitude = 23197;
// A clean peak's level against the marker crest; the crest of a marker measured through the CIC-2 blocks reads up
// to 14 % low at T = 8 blocks.
const double k_level_low_pct = 85.0;
const double k_level_high_pct = 125.0;

struct PresetCase {
    Preset preset;
    const char* name;
    double slot_ms;
    double gate_db;       // spec 8.3 A1' gate (AWGN key-down SNR in 2500 Hz, SSB channel)
    double tolerance_hz;  // spec 1.4 tuning tolerance (0: FM presets)
};

const PresetCase k_presets[] = {
    {Preset::fm_fast, "fm_fast", 6.0, 7.0, 0.0},         {Preset::fm, "fm", 8.0, 1.5, 0.0},
    {Preset::hf_fast, "hf_fast", 16.0, -1.5, 380.0},     {Preset::hf, "hf", 32.0, -4.5, 500.0},
    {Preset::hf_robust, "hf_robust", 64.0, -7.0, 560.0}, {Preset::hf_weak, "hf_weak", 128.0, -9.5, 590.0},
};

const char* profile_name(Profile profile) {
    switch (profile) {
    case Profile::ssb:
        return "ssb";
    case Profile::am:
        return "am";
    case Profile::fm:
        return "fm";
    }
    return "?";
}

// A profile decodes a mode when its T is in range and f_ref is inside its tone search.
bool covers(Profile profile, const EncoderConfig& config) {
    const DecoderConfig decoder = DecoderConfig::for_profile(profile);
    const double slot_ms = config.slot_us / 1000.0;
    return slot_ms >= decoder.min_slot_ms && slot_ms <= decoder.max_slot_ms() && config.tone_hz >= decoder.min_tone_hz &&
           config.tone_hz <= decoder.max_tone_hz;
}

Profile profile_for(double slot_ms) {
    if (slot_ms < 8.0) return Profile::fm;
    if (slot_ms < 16.0) return Profile::am;
    return Profile::ssb;
}

std::vector<std::int16_t> usb(const std::vector<std::int16_t>& samples, double snr_db, std::uint32_t seed,
                              std::int16_t amplitude, double offset_hz = k_usb_offset_hz) {
    sim::ChannelConfig config;
    config.mode = sim::Mode::usb;
    config.snr_db = snr_db;
    config.seed = seed;
    config.freq_offset_hz = offset_hz;
    return through_channel(samples, config, amplitude);
}

Capture decode(const std::vector<std::int16_t>& samples, Profile profile, std::size_t chunk = 0) {
    return run_decoder(samples, DecoderConfig::for_profile(profile), chunk);
}

// White noise for a key-down SNR (dB in 2500 Hz, 0 samples: none) added to `samples` times `gain`, then saturated to
// int16 as an overdriven sound card or ADC does.
const double k_noise_bandwidth_hz = 2500.0;
const double k_nyquist_hz = 4000.0;
const double k_no_noise_db = 999.0;

std::vector<std::int16_t> scaled_with_noise(const std::vector<double>& samples, double gain, double snr_db,
                                            std::uint32_t seed) {
    const double power = 0.5 * k_amplitude * k_amplitude / std::pow(10.0, snr_db / 10.0);
    std::normal_distribution<double> noise(0.0, std::sqrt(power / k_noise_bandwidth_hz * k_nyquist_hz));
    std::mt19937 generator(seed);
    std::vector<std::int16_t> out(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double value = gain * samples[i] + (snr_db < k_no_noise_db ? noise(generator) : 0.0);
        out[i] = static_cast<std::int16_t>(std::lround(std::max(-32768.0, std::min(32767.0, value))));
    }
    return out;
}

std::vector<double> as_double(const std::vector<std::int16_t>& samples) {
    return std::vector<double>(samples.begin(), samples.end());
}

// The receiver retuned from sample `from` on: a step of step_hz plus a drift of ramp_hz_per_s, applied to the analytic
// signal (a Hamming-windowed Hilbert FIR), phase continuous: what an SSB receiver's VFO or RIT does to the audio.
std::vector<double> retuned(const std::vector<double>& x, std::size_t from, double step_hz, double ramp_hz_per_s) {
    const int taps = 255;
    const int delay = (taps - 1) / 2;
    const double hamming_a = 0.54;
    const double hamming_b = 0.46;
    std::vector<double> h(taps, 0.0);
    for (int n = 0; n < taps; ++n) {
        const int k = n - delay;
        if (k % 2 != 0) h[n] = 2.0 / (k_pi * k) * (hamming_a - hamming_b * std::cos(2.0 * k_pi * n / (taps - 1)));
    }
    std::vector<double> y(x.size(), 0.0);
    double phase = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        double quadrature = 0.0;
        for (int j = 0; j < taps && j <= static_cast<int>(n); ++j) quadrature += h[j] * x[n - j];
        const double in_phase = static_cast<long>(n) - delay >= 0 ? x[n - delay] : 0.0;
        y[n] = in_phase * std::cos(phase) - quadrature * std::sin(phase);
        if (n < from) continue;
        const double hz = step_hz + ramp_hz_per_s * static_cast<double>(n - from) / k_decoder_rate_hz;
        phase = std::fmod(phase + 2.0 * k_pi * hz / k_decoder_rate_hz, 2.0 * k_pi);
    }
    return y;
}

void report(const char* label, const Score& s) {
    NOTE("%s: sent %zu released %zu wrong %zu bits %zu lost %zu extra %zu frames %zu/%zu locks %zu late %zu "
         "lost-events %zu ends %zu flywheel %zu",
         label, s.bytes_sent, s.bytes_released, s.wrong_bytes, s.bit_errors, s.lost_bytes, s.extra_bytes,
         s.frames_delivered, s.frames_sent, s.locks, s.late_joins, s.lost_events, s.ends, s.flywheel_bytes);
}

// Index of the first event of `type` at or after `from`, or events.size().
std::size_t find_event(const Capture& capture, EventType type, std::size_t from = 0) {
    for (std::size_t i = from; i < capture.events.size(); ++i) {
        if (capture.events[i].type == type) return i;
    }
    return capture.events.size();
}

bool same_event(const Event& a, const Event& b) {
    if (a.type != b.type || a.reason != b.reason || a.state != b.state || a.flags != b.flags) return false;
    if (a.value != b.value || a.index != b.index || a.tone != b.tone || a.level_pct != b.level_pct) return false;
    if (a.confidence != b.confidence || a.bits_per_peak != b.bits_per_peak || a.data_slots != b.data_slots) return false;
    if (a.spacing != b.spacing || a.side != b.side || a.frame_index != b.frame_index) return false;
    if (a.tone_hz != b.tone_hz || a.slot_ms != b.slot_ms || a.snr_db != b.snr_db) return false;
    for (int i = 0; i < k_bits_per_byte; ++i) {
        if (a.soft[i] != b.soft[i]) return false;
    }
    return true;
}

// Bytes come only from a confirmed lock: each byte follows a locked event with no lost or end in between.
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
            break;
        }
    }
    return true;
}

// Peaks of a frame's payload: symbols of k bits, MSB first (spec 1.5).
std::vector<std::uint8_t> frame_symbols(const std::vector<std::uint8_t>& data, std::size_t first, std::size_t bytes,
                                        std::uint8_t bits_per_peak, std::size_t peaks) {
    std::vector<std::uint8_t> symbols;
    std::size_t bit = 0;
    for (std::size_t i = 0; i < peaks; ++i) {
        unsigned symbol = 0;
        for (std::uint8_t b = 0; b < bits_per_peak; ++b, ++bit) {
            const std::size_t byte = bit / k_bits_per_byte;
            const unsigned value = byte < bytes ? data[first + byte] : 0u;
            symbol = (symbol << 1) | ((value >> (k_bits_per_byte - 1 - bit % k_bits_per_byte)) & 1u);
        }
        symbols.push_back(static_cast<std::uint8_t>(symbol));
    }
    return symbols;
}

// Overwrites the 8 header peaks of a transmission with the tones of `word` (spec 2.2), as the encoder shapes them.
void write_header(Recording& recording, std::size_t transmission, std::uint16_t word) {
    const Transmission& t = recording.transmissions[transmission];
    const EncoderConfig& config = t.config;
    const double length = slot_samples(config);
    const double side = config.side == GridSide::above ? 1.0 : -1.0;
    const double slot_s = config.slot_us / 1e6;
    for (std::uint8_t j = 0; j < k_header_slots; ++j) {
        const double begin = slot_start_sample(t, header_start_slot(config) + 1 + j);
        const double hz = config.tone_hz +
                          side * (k_grid_guard + header_symbol(word, j) * static_cast<double>(k_standard_spacing_num) /
                                                     k_standard_spacing_den) / slot_s;
        for (std::size_t n = static_cast<std::size_t>(std::ceil(begin)); n < begin + length; ++n) {
            const double u = (n - begin) / length;
            double envelope = 1.0;
            if (u < k_data_ramp) envelope = std::pow(std::sin(k_pi * u / (2.0 * k_data_ramp)), 2.0);
            if (u > 1.0 - k_data_ramp) envelope = std::pow(std::sin(k_pi * (1.0 - u) / (2.0 * k_data_ramp)), 2.0);
            recording.samples[n] = static_cast<std::int16_t>(
                std::lround(config.amplitude * envelope * std::sin(2.0 * k_pi * hz * n / config.sample_rate_hz)));
        }
    }
}

// Every valid mode (both spacings, both sides) at T, centred on 1500 Hz, that `profile` covers.
std::vector<EncoderConfig> modes_at(std::uint32_t slot_ms, Profile profile) {
    std::vector<EncoderConfig> modes;
    const std::uint8_t slots[] = {8, 16, 32};
    for (std::uint8_t k = 1; k <= k_max_bits_per_peak; ++k) {
        for (std::size_t n = 0; n < test::count_of(slots); ++n) {
            for (int dense = 0; dense < 2; ++dense) {
                for (int above = 0; above < 2; ++above) {
                    const EncoderConfig config = mode_config(slot_ms, k, slots[n], dense ? Spacing::dense : Spacing::standard,
                                                             above ? GridSide::above : GridSide::below);
                    if (config.valid() && covers(profile, config)) modes.push_back(config);
                }
            }
        }
    }
    return modes;
}

}  // namespace

// U15': memory. The (7, 16) MCU budget is asserted at compile time in decoder.cpp.
TEST(decoder_memory_budget) {
    NOTE("sizeof(Decoder) = %zu B (caps %d, %d), sizeof(Event) = %zu B", sizeof(Decoder), UNLIMITED_MAX_BITS_PER_PEAK,
         UNLIMITED_MAX_FRAME_BYTES, sizeof(Event));
    CHECK(sizeof(Event) <= 40);
}

// U18': profiles. A profile only fills the fields (the accepted T range and the tone search); DecoderConfig() is ssb.
TEST(decoder_profiles) {
    const DecoderConfig standard;
    const DecoderConfig ssb = DecoderConfig::for_profile(Profile::ssb);
    CHECK_EQ(+standard.min_slot_ms, +ssb.min_slot_ms);
    CHECK_EQ(+standard.min_tone_hz, +ssb.min_tone_hz);
    CHECK_EQ(+standard.max_tone_hz, +ssb.max_tone_hz);
    const Profile profiles[] = {Profile::ssb, Profile::am, Profile::fm};
    const int min_slot[] = {16, 8, 4};
    const int min_tone[] = {300, 300, 1000};
    for (std::size_t i = 0; i < test::count_of(profiles); ++i) {
        const DecoderConfig c = DecoderConfig::for_profile(profiles[i]);
        CHECK_EQ(+c.min_slot_ms, min_slot[i]);
        CHECK_EQ(+c.min_tone_hz, min_tone[i]);
        CHECK_EQ(+c.max_tone_hz, 2700);
        CHECK(c.impulse_blanker);
        CHECK(c.valid());
        CHECK_EQ(+c.max_slot_ms(), 8 * min_slot[i]);
    }
    DecoderConfig c = DecoderConfig::for_profile(Profile::am);
    c.min_slot_ms = 3;
    CHECK(!c.valid());
    c.min_slot_ms = 33;
    CHECK(!c.valid());
    c.min_slot_ms = 5;  // below 8: needs tones >= 1000 Hz
    CHECK(!c.valid());
    c.min_tone_hz = 1000;
    CHECK(c.valid());
    c = DecoderConfig();
    c.min_tone_hz = 299;
    CHECK(!c.valid());
    c = DecoderConfig();
    c.max_tone_hz = 2701;
    CHECK(!c.valid());
    c = DecoderConfig();
    c.min_tone_hz = 1500;
    c.max_tone_hz = 1500;
    CHECK(!c.valid());
    c = DecoderConfig();
    c.min_slot_ms = 12;  // any integer 4..32
    CHECK(c.valid());
}

TEST(decoder_invalid_config_is_idle) {
    DecoderConfig config;
    config.min_slot_ms = 2;
    const Recording recording = single(random_bytes(5, 1), preset_config(Preset::hf));
    const Capture capture = run_decoder(recording.samples, config);
    CHECK(capture.events.empty());
}

// Event fields, state sequence and mode reporting on a clean transmission (spec 5.1).
TEST(decoder_event_fields) {
    const EncoderConfig config = preset_config(Preset::hf_fast);
    const std::size_t bytes_per_frame = config.frame_bytes();
    const std::vector<std::uint8_t> data = random_bytes(3 * bytes_per_frame, 3);
    const Recording recording = single(data, config);
    const Capture capture = decode(recording.samples, Profile::ssb);
    std::vector<DecoderState> states;
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        if (capture.events[i].type == EventType::state) states.push_back(capture.events[i].state);
    }
    const DecoderState expected[] = {DecoderState::acquire, DecoderState::preamble, DecoderState::track,
                                     DecoderState::search};
    REQUIRE(states.size() == test::count_of(expected));
    for (std::size_t i = 0; i < states.size(); ++i) CHECK(states[i] == expected[i]);

    const std::size_t locked = find_event(capture, EventType::locked);
    REQUIRE(locked < capture.events.size());
    const Event& lock = capture.events[locked];
    CHECK_NEAR(lock.tone_hz, config.tone_hz, 2.0);
    CHECK_EQ(lock.slot_ms, 16.0f);
    CHECK(lock.snr_db > 20.0f);
    CHECK_EQ(+lock.flags, 0);
    CHECK_EQ(+lock.bits_per_peak, 4);
    CHECK_EQ(+lock.data_slots, 8);
    CHECK(lock.spacing == Spacing::standard);
    CHECK_EQ(+lock.side, -1);
    CHECK_EQ(lock.frame_index, 0u);

    std::size_t bytes = 0;
    std::size_t slots = 0;
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        const Event& e = capture.events[i];
        if (e.type == EventType::slot && e.frame_index < 3) {
            const std::vector<std::uint8_t> symbols =
                frame_symbols(data, e.frame_index * bytes_per_frame, bytes_per_frame, 4, config.data_slots);
            REQUIRE(e.index >= 1 && e.index <= config.data_slots);
            const std::uint8_t symbol = symbols[e.index - 1];
            CHECK_EQ(+e.value, +symbol);
            CHECK_EQ(+e.tone, +peak_tone(symbol, static_cast<std::uint8_t>(e.index - 1), 4));
            CHECK(e.level_pct >= k_level_low_pct && e.level_pct <= k_level_high_pct);
            CHECK(e.confidence >= 30);  // 15 dB over the runner-up
            CHECK_EQ(+e.flags, 0);
            CHECK_EQ(+e.bits_per_peak, 4);
            for (int b = 0; b < 4; ++b) CHECK_EQ(e.soft[b] > 0, ((symbol >> (3 - b)) & 1u) != 0);
            for (int b = 4; b < k_bits_per_byte; ++b) CHECK_EQ(+e.soft[b], 0);
            ++slots;
        }
        if (e.type != EventType::byte) continue;
        CHECK(i > locked);
        CHECK_EQ(+e.value, +data[bytes]);
        CHECK_EQ(e.frame_index, static_cast<std::uint32_t>(bytes / bytes_per_frame));
        CHECK_EQ(+e.index, static_cast<int>(bytes % bytes_per_frame));
        CHECK(e.state == DecoderState::track);
        CHECK_EQ(e.slot_ms, 16.0f);
        CHECK_EQ(+e.flags, 0);
        for (int b = 0; b < k_bits_per_byte; ++b) {
            const bool one = ((data[bytes] >> (k_bits_per_byte - 1 - b)) & 1u) != 0;
            CHECK_EQ(e.soft[b] > 0, one);
            CHECK(std::abs(e.soft[b]) <= 112);
        }
        ++bytes;
    }
    CHECK_EQ(bytes, data.size());
    CHECK_EQ(slots, 3u * config.data_slots);
    const std::size_t end = find_event(capture, EventType::end);
    REQUIRE(end < capture.events.size());
    CHECK_EQ(+capture.events[end].bits_per_peak, 4);
    CHECK_EQ(count_events(capture, EventType::lost), 0u);
    CHECK_EQ(capture.events.back().bits_per_peak, 0);
}

// L1': clean loopback at every preset and at T in {6, 12, 20, 37, 100, 128} ms over the valid modes (k, N, both
// spacings, both sides; a short final frame in each).
TEST(decoder_l1_clean_loopback) {
    std::vector<EncoderConfig> cases;
    for (std::size_t p = 0; p < test::count_of(k_presets); ++p) cases.push_back(preset_config(k_presets[p].preset));
    const std::uint32_t speeds[] = {6, 12, 20, 37, 100, 128};
    std::size_t tried = 0;
    for (std::size_t s = 0; s < test::count_of(speeds); ++s) {
        const std::vector<EncoderConfig> modes = modes_at(speeds[s], profile_for(speeds[s]));
        tried += modes.size();
        // Every third mode keeps the run short; the first and last (lowest and highest k) are always in.
        for (std::size_t m = 0; m < modes.size(); ++m) {
            if (m % 3 == 0 || m + 1 == modes.size()) cases.push_back(modes[m]);
        }
    }
    std::size_t failures = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const EncoderConfig& config = cases[i];
        const double slot_ms = config.slot_us / 1000.0;
        const std::size_t bytes = 2 * config.frame_bytes() + std::max<std::size_t>(1, config.frame_bytes() / 2);
        const Recording recording = single(random_bytes(bytes, static_cast<std::uint32_t>(i)), config);
        const Capture capture = decode(recording.samples, profile_for(slot_ms));
        const Score s = score(recording, capture);
        if (config.bits_per_peak > UNLIMITED_MAX_BITS_PER_PEAK || config.frame_bytes() > UNLIMITED_MAX_FRAME_BYTES) {
            // Over this build's caps: refused (spec 8.2 L16).
            const std::size_t lost = find_event(capture, EventType::lost);
            const bool refused = s.bytes_released == 0 && lost < capture.events.size() &&
                                 capture.events[lost].reason == LostReason::unsupported_mode;
            if (!refused) ++failures;
            continue;
        }
        const std::size_t locked = find_event(capture, EventType::locked);
        const bool mode_ok = locked < capture.events.size() && capture.events[locked].slot_ms == static_cast<float>(slot_ms) &&
                             capture.events[locked].bits_per_peak == config.bits_per_peak &&
                             capture.events[locked].data_slots == config.data_slots &&
                             capture.events[locked].spacing == config.spacing &&
                             capture.events[locked].side == (config.side == GridSide::above ? 1 : -1);
        const bool ok = s.wrong_bytes == 0 && s.lost_bytes == 0 && s.extra_bytes == 0 && s.locks == 1 && s.ends == 1 &&
                        mode_ok;
        if (!ok) {
            ++failures;
            NOTE("T=%.0f k=%u N=%u %s %s f_ref=%u (%zu bytes): mode %d", slot_ms, config.bits_per_peak,
                 config.data_slots, config.spacing == Spacing::standard ? "std" : "dense",
                 config.side == GridSide::above ? "above" : "below", config.tone_hz, bytes, mode_ok);
            report("  ", s);
        }
    }
    NOTE("%zu transmissions (%zu valid modes at the listed T), %zu failed", cases.size(), tried, failures);
    CHECK_EQ(failures, 0u);
}

// L2: bit-exact event streams whatever the chunking (slot events included).
TEST(decoder_l2_chunk_invariance) {
    const EncoderConfig config = preset_config(Preset::hf_fast);
    const Recording recording = single(random_bytes(60, 22), config);
    const std::vector<std::int16_t> samples = usb(recording.samples, 0.0, 5, config.amplitude, 23.0);
    const Capture reference = decode(samples, Profile::ssb, 0);
    REQUIRE(count_events(reference, EventType::byte) > 0);
    REQUIRE(count_events(reference, EventType::slot) > 0);
    const std::size_t chunks[] = {1, 7, 160, 4096};
    for (std::size_t c = 0; c < test::count_of(chunks); ++c) {
        const Capture other = decode(samples, Profile::ssb, chunks[c]);
        REQUIRE(other.events.size() == reference.events.size());
        bool same = true;
        for (std::size_t i = 0; i < other.events.size(); ++i) same = same && same_event(other.events[i], reference.events[i]);
        CHECK(same);
    }
}

// L3': one decoder per profile hears back-to-back modes over its whole range and f_ref placements low, centre and
// high without reconfiguration.
TEST(decoder_l3_mode_agnostic) {
    const Profile profiles[] = {Profile::ssb, Profile::am, Profile::fm};
    const double multiples[] = {1.0, 1.5, 2.0, 3.0, 4.5, 6.0, 8.0};
    for (std::size_t p = 0; p < test::count_of(profiles); ++p) {
        const DecoderConfig decoder = DecoderConfig::for_profile(profiles[p]);
        Recording recording;
        append_silence(recording, k_gap_ms);
        std::size_t sent = 0;
        for (std::size_t m = 0; m < test::count_of(multiples); ++m) {
            const std::uint32_t slot_ms = static_cast<std::uint32_t>(std::lround(std::max(6.0, decoder.min_slot_ms * multiples[m])));
            EncoderConfig config = slot_config(slot_ms, 1500);
            // f_ref low (grid above), centre, high (grid below), in turn; inside the profile's search.
            const int placement = static_cast<int>(m % 3);
            if (placement == 0) config = mode_config(slot_ms, config.bits_per_peak, 8, Spacing::standard, GridSide::above);
            if (placement == 2) config = mode_config(slot_ms, config.bits_per_peak, 8, Spacing::standard, GridSide::below);
            if (!config.valid() || !covers(profiles[p], config)) config = slot_config(slot_ms, 2600);
            if (!config.valid() || !covers(profiles[p], config)) continue;
            append_transmission(recording, random_bytes(2 * config.frame_bytes() + 1, static_cast<std::uint32_t>(p * 10 + m)), config);
            append_silence(recording, k_gap_ms + 3.0 * slot_ms);
            ++sent;
        }
        const Capture capture = run_decoder(recording.samples, decoder);
        const Score s = score(recording, capture);
        report(profile_name(profiles[p]), s);
        CHECK_EQ(s.wrong_bytes, 0u);
        CHECK_EQ(s.lost_bytes, 0u);
        CHECK_EQ(s.extra_bytes, 0u);
        CHECK_EQ(s.locks, sent);
    }
}

// L4': USB and LSB, grid above and below f_ref, receiver offsets up to each HF preset's tuning tolerance, at the
// A1' gate + 3 dB: no wrong byte, nothing lost, orientation as received.
TEST(decoder_l4_sideband_and_offset) {
    const double pivot_hz = sim::ChannelConfig().lsb_pivot_hz;
    for (std::size_t p = 2; p < 5; ++p) {
        const PresetCase& preset = k_presets[p];
        const EncoderConfig base = preset_config(preset.preset);
        for (int above = 0; above < 2; ++above) {
            const EncoderConfig config = mode_config(static_cast<std::uint32_t>(preset.slot_ms), base.bits_per_peak, 8,
                                                     Spacing::standard, above ? GridSide::above : GridSide::below);
            REQUIRE(config.valid());
            const double offsets[] = {-0.9 * preset.tolerance_hz, 0.0, 0.9 * preset.tolerance_hz};
            for (int lsb = 0; lsb < 2; ++lsb) {
                for (std::size_t o = 0; o < test::count_of(offsets); ++o) {
                    if (o == 1 && lsb == 0) continue;
                    const std::size_t bytes = 2 * config.frame_bytes() + 1;
                    const Recording recording = single(random_bytes(bytes, static_cast<std::uint32_t>(p * 100 + o)), config);
                    sim::ChannelConfig channel;
                    channel.mode = lsb ? sim::Mode::lsb : sim::Mode::usb;
                    channel.snr_db = preset.gate_db + k_gate_margin_db;
                    channel.seed = static_cast<std::uint32_t>(40 + p + 10 * o + 100 * lsb + 1000 * above);
                    channel.freq_offset_hz = offsets[o];
                    const Capture capture = decode(through_channel(recording.samples, channel, config.amplitude), Profile::ssb);
                    const Score s = score(recording, capture);
                    const int side_sent = above ? 1 : -1;
                    const int side_expected = lsb ? -side_sent : side_sent;
                    const std::size_t locked = find_event(capture, EventType::locked);
                    const double tone = (lsb ? pivot_hz - config.tone_hz : config.tone_hz) + offsets[o];
                    if (s.wrong_bytes != 0 || s.lost_bytes != 0 || locked >= capture.events.size()) {
                        NOTE("%s grid %s %s offset %+.0f Hz (f_ref received %.0f Hz)", preset.name, above ? "above" : "below",
                             lsb ? "lsb" : "usb", offsets[o], tone);
                        report("  ", s);
                    }
                    CHECK_EQ(s.wrong_bytes, 0u);
                    CHECK_EQ(s.lost_bytes, 0u);
                    CHECK_EQ(s.extra_bytes, 0u);
                    REQUIRE(locked < capture.events.size());
                    CHECK_EQ(+capture.events[locked].side, side_expected);
                    CHECK_NEAR(capture.events[locked].tone_hz, tone, 2.0);
                }
            }
        }
    }
}

// L5: sender clock errors (the channel resamples the transmitter's audio), 30 s at T = 16 ms; the drift loop
// holds the grid: nothing lost at gate + 3 dB.
TEST(decoder_l5_clock_error) {
    const EncoderConfig config = preset_config(Preset::hf_fast);
    const double ppm[] = {1000.0, -1000.0, 2000.0, -2000.0};  // TX; RX alike; both in opposite directions
    const std::size_t bytes = 800;                            // about 30 s
    for (std::size_t i = 0; i < test::count_of(ppm); ++i) {
        const Recording recording = single(random_bytes(bytes, static_cast<std::uint32_t>(50 + i)), config);
        sim::ChannelConfig channel;
        channel.mode = sim::Mode::usb;
        channel.snr_db = k_presets[2].gate_db + k_gate_margin_db;
        channel.seed = static_cast<std::uint32_t>(51 + i);
        channel.freq_offset_hz = k_usb_offset_hz;
        channel.clock_ppm = ppm[i];
        const std::vector<std::int16_t> samples = through_channel(recording.samples, channel, config.amplitude);
        const Capture capture = decode(samples, Profile::ssb);
        Recording scaled = recording;  // the recording's time line at the receiver
        const double scale = 1.0 / (1.0 + ppm[i] * 1e-6);
        scaled.transmissions[0].start_sample = static_cast<std::size_t>(recording.transmissions[0].start_sample * scale);
        scaled.transmissions[0].config.slot_us = static_cast<std::uint32_t>(std::lround(config.slot_us * scale));
        const Mapping m = map_events(scaled, capture);
        report(("clock " + std::to_string(static_cast<int>(ppm[i])) + " ppm").c_str(), m.score);
        CHECK_EQ(m.score.wrong_bytes, 0u);
        CHECK_EQ(m.score.lost_bytes, 0u);
        CHECK_EQ(m.score.locks, 1u);
        CHECK_EQ(m.score.ends, 1u);
    }
}

// L6': faded sync markers, erased header peaks, and a whole header erased (with and without mode memory).
TEST(decoder_l6_sync_and_header_fades) {
    const EncoderConfig config = preset_config(Preset::hf);
    const std::vector<std::uint8_t> data = random_bytes(40, 60);
    const std::size_t header = header_start_slot(config);
    const std::size_t first_sync = header + 1 - config.sync_markers;
    const int triples[][3] = {{0, 1, 2}, {2, 4, 6}, {1, 3, 7}, {5, 6, 7}, {0, 4, 7}};
    for (std::size_t t = 0; t < test::count_of(triples); ++t) {
        Recording recording = single(data, config);
        for (int k = 0; k < 3; ++k) scale_slot(recording, 0, first_sync + triples[t][k], 0.0);
        const Mapping m = map_events(recording, decode(usb(recording.samples, 10.0, 60 + t, config.amplitude), Profile::ssb));
        if (m.score.lost_bytes != 0 || m.score.wrong_bytes != 0) {
            NOTE("sync markers %d %d %d zeroed", triples[t][0], triples[t][1], triples[t][2]);
            report("  ", m.score);
        }
        CHECK_EQ(m.score.lost_bytes, 0u);
        CHECK_EQ(m.score.wrong_bytes, 0u);
    }
    const int pairs[][2] = {{0, 1}, {3, 6}, {6, 7}, {2, 5}};
    for (std::size_t p = 0; p < test::count_of(pairs); ++p) {
        Recording recording = single(data, config);
        for (int k = 0; k < 2; ++k) scale_slot(recording, 0, header + 1 + pairs[p][k], 0.0);
        const Score s = score(recording, decode(usb(recording.samples, 10.0, 70 + p, config.amplitude), Profile::ssb));
        if (s.lost_bytes != 0 || s.wrong_bytes != 0) {
            NOTE("header peaks %d %d erased", pairs[p][0], pairs[p][1]);
            report("  ", s);
        }
        CHECK_EQ(s.lost_bytes, 0u);
        CHECK_EQ(s.wrong_bytes, 0u);
    }

    // The whole header erased: no bytes (no mode); lost(no_header).
    Recording erased = single(data, config);
    for (std::uint8_t j = 1; j <= k_header_slots; ++j) scale_slot(erased, 0, header + j, 0.0);
    const Capture alone = decode(usb(erased.samples, 10.0, 80, config.amplitude), Profile::ssb);
    const std::size_t lost = find_event(alone, EventType::lost);
    CHECK_EQ(count_events(alone, EventType::byte), 0u);
    CHECK_EQ(count_events(alone, EventType::locked), 0u);
    REQUIRE(lost < alone.events.size());
    CHECK(alone.events[lost].reason == LostReason::no_header);

    // The same after a complete transmission of that station: the mode memory gives the mode, bytes are flagged.
    Recording twice;
    append_silence(twice, k_gap_ms);
    append_transmission(twice, random_bytes(15, 61), config);
    append_silence(twice, 2000.0);
    append_transmission(twice, data, config);
    append_silence(twice, 1000.0);
    const std::size_t second_header = header;
    for (std::uint8_t j = 1; j <= k_header_slots; ++j) scale_slot(twice, 1, second_header + j, 0.0);
    const Capture capture = decode(usb(twice.samples, 10.0, 81, config.amplitude), Profile::ssb);
    const Mapping m = map_events(twice, capture);
    report("header erased, memory", m.score);
    CHECK_EQ(m.score.wrong_bytes, 0u);
    CHECK_EQ(m.score.lost_bytes, 0u);
    CHECK_EQ(m.score.locks, 2u);
    bool flagged = true;
    for (std::size_t k = 0; k < data.size(); ++k) {
        flagged = flagged && m.received[1][k] >= 0 && (m.byte_events[1][k].flags & event_flag_mode_memory) != 0;
    }
    CHECK(flagged);
}

// L7: the STOP of every even frame (markers 1, 3, 5, 7) attenuated 30 dB for frames 0-7: no wrong byte.
TEST(decoder_l7_alias_markers) {
    const EncoderConfig config = preset_config(Preset::hf);
    const std::vector<std::uint8_t> data = random_bytes(20 * config.frame_bytes(), 70);
    Recording recording = single(data, config);
    const Transmission& t = recording.transmissions[0];
    for (std::size_t frame = 0; frame < 8; frame += 2) {
        scale_slot(recording, 0, frame_start_slot(t, frame) + config.data_slots + 1, k_attenuation_30_db);
    }
    const Mapping m = map_events(recording, decode(usb(recording.samples, 10.0, 71, config.amplitude), Profile::ssb));
    report("even STOPs -30 dB", m.score);
    CHECK_EQ(m.score.wrong_bytes, 0u);
    CHECK_EQ(m.score.extra_bytes, 0u);
    CHECK_EQ(m.score.lost_bytes, 0u);
}

// L8': flywheel over single missing STOPs (flagged); then 3 of 4 frames fully zeroed: lost(signal_gone), no byte
// after it.
TEST(decoder_l8_flywheel) {
    const EncoderConfig config = preset_config(Preset::hf_fast);
    const std::size_t frames = 40;
    const std::vector<std::uint8_t> data = random_bytes(frames * config.frame_bytes(), 80);
    Recording recording = single(data, config);
    const Transmission& t = recording.transmissions[0];
    const std::size_t flywheel_until = 20;
    for (std::size_t frame = 4; frame < flywheel_until; frame += 5) {
        scale_slot(recording, 0, frame_start_slot(t, frame) + config.data_slots + 1, 0.0);
    }
    const std::size_t silent_from = 26;
    for (std::size_t frame = silent_from; frame < frames; ++frame) {
        if ((frame - silent_from) % 4 == 3) continue;
        for (std::size_t slot = 1; slot <= config.data_slots + 1u; ++slot) scale_slot(recording, 0, frame_start_slot(t, frame) + slot, 0.0);
    }
    const Capture capture = decode(usb(recording.samples, 10.0, 81, config.amplitude), Profile::ssb);
    const Mapping m = map_events(recording, capture);
    report("flywheel", m.score);
    std::size_t decoded = 0;
    for (std::size_t k = 0; k < flywheel_until * config.frame_bytes(); ++k) {
        decoded += m.received[0][k] == data[k] ? 1 : 0;
        const std::size_t frame = k / config.frame_bytes();
        if (frame % 5 == 4 && m.received[0][k] >= 0) CHECK((m.byte_events[0][k].flags & event_flag_flywheel_stop) != 0);
    }
    CHECK_EQ(decoded, flywheel_until * config.frame_bytes());
    std::size_t wrong_before = 0;
    std::size_t wrong_after = 0;
    for (std::size_t k = 0; k < data.size(); ++k) {
        if (m.received[0][k] < 0 || m.received[0][k] == data[k]) continue;
        if (k < silent_from * config.frame_bytes()) {
            ++wrong_before;
        } else {
            ++wrong_after;
        }
    }
    // A zeroed frame whose STOP reads q >= 1 on noise (7 %) is held as weak and released with the next frame.
    NOTE("wrong bytes: %zu before the zeroed frames, %zu among them", wrong_before, wrong_after);
    CHECK_EQ(wrong_before, 0u);
    CHECK_EQ(m.score.extra_bytes, 0u);
    const std::size_t lost = find_event(capture, EventType::lost);
    REQUIRE(lost < capture.events.size());
    CHECK(capture.events[lost].reason == LostReason::signal_gone);
    for (std::size_t i = lost; i < capture.events.size(); ++i) CHECK(capture.events[i].type != EventType::byte);
}

// L9: END within 3 T of the final STOP (full and short last frames); a PTT cut after a STOP gives lost within 4
// frames, no garbage bytes and no end.
TEST(decoder_l9_end) {
    const Preset presets[] = {Preset::hf_fast, Preset::hf, Preset::hf_weak};
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        const EncoderConfig config = preset_config(presets[p]);
        const std::size_t bytes_per_frame = config.frame_bytes();
        const std::size_t sizes[] = {2 * bytes_per_frame, 2 * bytes_per_frame + 1};
        for (std::size_t z = 0; z < test::count_of(sizes); ++z) {
            const std::vector<std::uint8_t> data = random_bytes(sizes[z], static_cast<std::uint32_t>(90 + z));
            const Recording recording = single(data, config, 1000.0);
            const Transmission& t = recording.transmissions[0];
            sim::ChannelConfig channel;
            channel.mode = sim::Mode::usb;
            channel.snr_db = 10.0;
            channel.seed = static_cast<std::uint32_t>(90 + p);
            channel.freq_offset_hz = k_usb_offset_hz;
            const long delay = channel_delay(recording.samples, channel, config.amplitude);  // SSB filters: 41 ms
            const Capture capture = decode(through_channel(recording.samples, channel, config.amplitude), Profile::ssb);
            const std::size_t end = find_event(capture, EventType::end);
            REQUIRE(end < capture.events.size());
            const double final_stop = stop_centre_sample(t, frame_count(t) - 1) + delay;
            const double latency = (capture.event_sample[end] - final_stop) / slot_samples(config);
            NOTE("T=%.0f ms, %zu bytes: end %.2f T after the final STOP", config.slot_us / 1000.0, data.size(), latency);
            CHECK(latency <= 3.0);
            for (std::size_t i = end; i < capture.events.size(); ++i) CHECK(capture.events[i].type != EventType::byte);
            const Score score_end = score(recording, capture);
            CHECK_EQ(score_end.wrong_bytes, 0u);
            CHECK_EQ(score_end.lost_bytes, 0u);
            CHECK_EQ(score_end.extra_bytes, 0u);
        }

        // PTT cut right after a full frame's STOP: no EOT markers, then noise only.
        const std::vector<std::uint8_t> data = random_bytes(4 * config.frame_bytes(), 95);
        Recording cut = single(data, config, 1000.0);
        const double stop = stop_centre_sample(cut.transmissions[0], 1);
        const std::size_t cut_at = static_cast<std::size_t>(stop + 0.5 * slot_samples(config));
        std::fill(cut.samples.begin() + static_cast<long>(cut_at), cut.samples.end(), 0);
        cut.samples.resize(cut_at + static_cast<std::size_t>(6 * (config.data_slots + 1) * slot_samples(config)), 0);
        const Capture cut_capture = decode(usb(cut.samples, 10.0, 91 + p, config.amplitude), Profile::ssb);
        const std::size_t lost = find_event(cut_capture, EventType::lost);
        REQUIRE(lost < cut_capture.events.size());
        CHECK(cut_capture.events[lost].reason == LostReason::signal_gone);
        const double frames = (cut_capture.event_sample[lost] - stop) / ((config.data_slots + 1) * slot_samples(config));
        // LOST needs 3 of 4 frames absent (STOP q < 1 and peaks not present): noise alone reaches q >= 1 at 7 % of
        // STOPs, so one frame more than the three after the cut is common.
        NOTE("T=%.0f ms, PTT cut: lost %.2f frames after the last STOP", config.slot_us / 1000.0, frames);
        CHECK(frames <= 5.0);
        const Mapping m = map_events(cut, cut_capture);
        CHECK_EQ(m.score.wrong_bytes, 0u);
        CHECK_EQ(m.score.extra_bytes, 0u);
        CHECK_EQ(m.score.matched, static_cast<std::size_t>(2 * config.frame_bytes()));
        CHECK_EQ(count_events(cut_capture, EventType::end), 0u);
    }
}

// L10': a mid-stream start of a station heard less than 60 s before: late join with the mode memory, no wrong byte,
// bytes flagged late_join + mode_memory. Without the memory no byte is released. Every HF preset at 10 and 30 dB: at
// high SNR the stream's peaks leaked into f_ref and filled the candidate list with flips in less than a frame, and
// they passed for trains of other T; at T = 128 ms the marker halves passed for a tune and the fine AFC walked the lock
// to the peaks 39 Hz away (hf 1/8 and hf_robust 0/8 joins at 30 dB, hf_weak 0/8 at every SNR).
TEST(decoder_l10_mid_stream_with_memory) {
    const Preset presets[] = {Preset::hf_fast, Preset::hf, Preset::hf_robust, Preset::hf_weak};
    const double snrs_db[] = {10.0, 30.0};
    const std::size_t frames = 40;
    const int trials = 8;
    // hf_fast (spec 8.2 L10' asks 5 frames; 4.4..6.4 are measured): the search locks on a peak (1..2.5 frames at T = 16
    // ms), then the remembered f_ref needs four markers (3 frame periods). A peak that looks steady to the search takes
    // a PREAMBLE without header first (about 12 frames more). The slower presets are reported.
    const double prompt_frames = 7.0;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        const EncoderConfig config = preset_config(presets[p]);
        const std::vector<std::uint8_t> first = random_bytes(3 * config.frame_bytes(), 99);
        const std::vector<std::uint8_t> data = random_bytes(frames * config.frame_bytes(), 100);
        for (std::size_t n = 0; n < test::count_of(snrs_db); ++n) {
            std::mt19937 generator(101);
            std::size_t joined = 0;
            std::size_t prompt = 0;
            std::size_t wrong = 0;
            double worst_frames = 0.0;
            for (int trial = 0; trial < trials; ++trial) {
                Recording recording;
                append_silence(recording, k_gap_ms);
                append_transmission(recording, first, config);
                append_silence(recording, 3000.0);
                append_transmission(recording, data, config);
                append_silence(recording, 1000.0);
                // Cut the second transmission's beginning: its first 2..12 frames are silent.
                const Transmission& t = recording.transmissions[1];
                std::uniform_int_distribution<int> pick(2, 12);
                const std::size_t join = static_cast<std::size_t>(pick(generator));
                const std::size_t from = t.start_sample;
                const std::size_t to = static_cast<std::size_t>(slot_start_sample(t, frame_start_slot(t, join)) +
                                                                (trial % 5) * 0.2 * slot_samples(config));
                std::fill(recording.samples.begin() + static_cast<long>(from),
                          recording.samples.begin() + static_cast<long>(to), 0);
                const Capture capture =
                    decode(usb(recording.samples, snrs_db[n], 102 + trial, config.amplitude), Profile::ssb);
                const Mapping m = map_events(recording, capture);
                std::size_t late_lock = capture.events.size();
                for (std::size_t i = 0; i < capture.events.size(); ++i) {
                    if (capture.events[i].type == EventType::locked &&
                        (capture.events[i].flags & event_flag_late_join) != 0) {
                        late_lock = i;
                        break;
                    }
                }
                const bool late = late_lock < capture.events.size() &&
                                  (capture.events[late_lock].flags & event_flag_mode_memory) != 0;
                joined += late ? 1 : 0;
                wrong += m.score.wrong_bytes + m.score.extra_bytes;
                if (late) {
                    // TRACK entry of the late join (its guard then confirms 4 frames).
                    std::size_t track = late_lock;
                    while (track > 0 && !(capture.events[track].type == EventType::state &&
                                          capture.events[track].state == DecoderState::track)) {
                        --track;
                    }
                    const double after = (capture.event_sample[track] - static_cast<double>(to)) /
                                         ((config.data_slots + 1) * slot_samples(config));
                    worst_frames = std::max(worst_frames, after);
                    prompt += after <= prompt_frames ? 1 : 0;
                } else {
                    NOTE("T %u ms, %.0f dB, trial %d (join at frame %zu): no late join", unsigned(config.slot_us / 1000),
                         snrs_db[n], trial, join);
                    report("  ", m.score);
                }
            }
            NOTE("T %u ms, %.0f dB: late joins with memory %zu / %d, %zu within %.0f frames, TRACK at worst %.1f frames "
                 "after the cut, wrong or extra bytes %zu", unsigned(config.slot_us / 1000), snrs_db[n], joined, trials,
                 prompt, prompt_frames, worst_frames, wrong);
            CHECK_EQ(joined, static_cast<std::size_t>(trials));
            if (presets[p] == Preset::hf_fast) CHECK(prompt + 1 >= static_cast<std::size_t>(trials));
            CHECK_EQ(wrong, 0u);
        }
    }

    const EncoderConfig config = preset_config(Preset::hf_fast);
    const std::vector<std::uint8_t> data = random_bytes(frames * config.frame_bytes(), 100);
    // No memory (a fresh decoder): no byte.
    Recording alone;
    append_transmission(alone, data, config);
    const Transmission& t = alone.transmissions[0];
    const std::size_t to = static_cast<std::size_t>(slot_start_sample(t, frame_start_slot(t, 5)));
    std::fill(alone.samples.begin(), alone.samples.begin() + static_cast<long>(to), 0);
    const Capture capture = decode(usb(alone.samples, 10.0, 120, config.amplitude), Profile::ssb);
    CHECK_EQ(count_events(capture, EventType::byte), 0u);
    CHECK_EQ(count_events(capture, EventType::locked), 0u);
}

// D44: fade -> LOST -> relock with the mode memory. Five frames of a transmission fade to nothing (20 dB): the decoder
// loses the lock, holds the remembered f_ref and joins the stream again when it is back. hf and hf_robust never relocked
// at high SNR, hf_weak never at all (the stream's peak leaks passed for trains, the ACQUIRE timeout ran out during
// the fade, and the fine AFC left f_ref).
TEST(decoder_fade_relock_with_memory) {
    const Preset presets[] = {Preset::hf_fast, Preset::hf, Preset::hf_robust, Preset::hf_weak};
    const std::size_t frames = 30;
    const std::size_t fade_first = 10;
    const std::size_t fade_frames = 5;
    const double fade_phase = 0.3;  // slots into the frame
    const double relock_frames = 10.0;
    const int trials = 2;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        const EncoderConfig config = preset_config(presets[p]);
        for (int trial = 0; trial < trials; ++trial) {
            const std::vector<std::uint8_t> data =
                random_bytes(frames * config.frame_bytes(), static_cast<std::uint32_t>(200 + trial));
            Recording recording;
            append_silence(recording, k_gap_ms);
            append_transmission(recording, data, config);
            append_silence(recording, 1000.0);
            const Transmission& t = recording.transmissions[0];
            const std::size_t from = static_cast<std::size_t>(slot_start_sample(t, frame_start_slot(t, fade_first)) +
                                                              fade_phase * slot_samples(config));
            const std::size_t to = static_cast<std::size_t>(
                slot_start_sample(t, frame_start_slot(t, fade_first + fade_frames)) + fade_phase * slot_samples(config));
            std::fill(recording.samples.begin() + static_cast<long>(from), recording.samples.begin() + static_cast<long>(to), 0);
            const Capture capture = decode(usb(recording.samples, 20.0, static_cast<std::uint32_t>(300 + trial),
                                               config.amplitude), Profile::ssb);
            const Mapping m = map_events(recording, capture);
            double relock = -1.0;
            for (std::size_t i = 0; i < capture.events.size() && relock < 0.0; ++i) {
                const Event& e = capture.events[i];
                if (e.type == EventType::locked && (e.flags & event_flag_late_join) != 0 &&
                    (e.flags & event_flag_mode_memory) != 0) {
                    relock = (capture.event_sample[i] - static_cast<double>(to)) /
                             ((config.data_slots + 1) * slot_samples(config));
                }
            }
            NOTE("T %u ms, trial %d: relocked %.1f frames after the fade (locked event), %zu of %zu bytes, wrong %zu, "
                 "extra %zu", unsigned(config.slot_us / 1000), trial, relock, m.score.matched, data.size(),
                 m.score.wrong_bytes, m.score.extra_bytes);
            CHECK(relock >= 0.0 && relock <= relock_frames);
            CHECK_EQ(m.score.wrong_bytes, 0u);
            CHECK_EQ(m.score.extra_bytes, 0u);
        }
    }
}

// The PREAMBLE watch and the preamble's own train: a train of period 2 T has lines at odd multiples of 1 / (2 T) from
// f_ref. The one at 3 / (2 T) (75 Hz at T = 20 ms) lies outside the lock's exclusion (1 / (2 T_min)), is steady while the
// train runs and breaks at the header: the watch took it for a new transmission and left a valid preamble, on a clean
// channel, at 10 of 160 sample alignments (T 20 ms, 12 sync markers) and 16 (T 8 ms, 32 markers, am).
TEST(decoder_preamble_watch_ignores_train_lines) {
    struct Case {
        std::uint32_t slot_ms;
        std::uint16_t tone_hz;
        std::uint8_t sync_markers;
        Profile profile;
    };
    const Case cases[] = {{20, 2000, 12, Profile::ssb}, {20, 2000, 32, Profile::ssb}, {8, 2313, 32, Profile::am}};
    const std::size_t alignments = 160;  // one tone-search block
    const std::size_t lead_samples = 8000;
    const std::vector<std::uint8_t> data = random_bytes(45, 1);
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        EncoderConfig config = mode_config(cases[c].slot_ms, 3, 8, Spacing::standard, GridSide::below, cases[c].tone_hz);
        config.tune_ms = 0;
        config.sync_markers = cases[c].sync_markers;
        REQUIRE(config.valid());
        std::size_t failures = 0;
        for (std::size_t offset = 0; offset < alignments; ++offset) {
            Recording recording;
            recording.samples.assign(lead_samples + offset, 0);
            append_transmission(recording, data, config);
            append_silence(recording, 1000.0);
            const Score s = score(recording, decode(recording.samples, cases[c].profile));
            if (s.lost_bytes + s.wrong_bytes + s.extra_bytes != 0) ++failures;
        }
        NOTE("T %u ms, %u sync markers, f_ref %u: %zu of %zu alignments lose bytes", cases[c].slot_ms,
             unsigned(cases[c].sync_markers), unsigned(cases[c].tone_hz), failures, alignments);
        CHECK_EQ(failures, 0u);
    }
}

// Saturated input (a sound card or ADC driven 0.5..3 dB too hot, no noise): the odd harmonics of the tune alias into the
// band (8000 - 3 f_ref) and reverse with it. The ACQUIRE watch took the image's train for a new transmission and every
// byte was lost (hf_robust from x1.5, hf from x1.8, hf_fast from x4).
TEST(decoder_saturated_input) {
    struct Case {
        Preset preset;
        double gain;
    };
    const Case cases[] = {{Preset::hf_robust, 1.5}, {Preset::hf_robust, 2.0}, {Preset::hf_robust, 4.0},
                          {Preset::hf, 1.8},        {Preset::hf, 3.0},        {Preset::hf_fast, 4.0}};
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        const EncoderConfig config = preset_config(cases[c].preset);
        const Recording recording = single(random_bytes(30, 94), config, 1000.0);
        const std::vector<std::int16_t> clipped =
            scaled_with_noise(as_double(recording.samples), cases[c].gain, k_no_noise_db, 0);
        const Score s = score(recording, decode(clipped, Profile::ssb));
        if (s.lost_bytes + s.wrong_bytes + s.extra_bytes != 0) {
            NOTE("f_ref %u Hz, gain x%.1f:", unsigned(config.tone_hz), cases[c].gain);
            report("  saturated", s);
        }
        CHECK_EQ(s.lost_bytes, 0u);
        CHECK_EQ(s.wrong_bytes, 0u);
        CHECK_EQ(s.extra_bytes, 0u);
    }
}

// A tune heard alone (the train and the rest faded) and the station's complete retry 5..12 s later on the same f_ref,
// 15 dB: the tune's tone was banned for 10 s at the ACQUIRE timeout, and the retry was lost. Only a tone still there
// is banned now.
TEST(decoder_retry_after_a_lone_tune) {
    const EncoderConfig config = preset_config(Preset::hf);
    const double starts_s[] = {5.3, 8.0, 12.0};
    const std::vector<std::uint8_t> first = random_bytes(40, 101);
    const std::vector<std::uint8_t> retry = random_bytes(40, 102);
    for (std::size_t g = 0; g < test::count_of(starts_s); ++g) {
        Recording recording;
        append_silence(recording, k_gap_ms);
        append_transmission(recording, first, config);
        const std::size_t first_sync = header_start_slot(config) - config.sync_markers + 1;
        const std::size_t slots = static_cast<std::size_t>(recording.transmissions[0].length / slot_samples(config)) + 2;
        for (std::size_t slot = first_sync; slot < slots; ++slot) scale_slot(recording, 0, slot, 0.0);
        recording.samples.resize(recording.transmissions[0].start_sample +
                                     static_cast<std::size_t>(starts_s[g] * k_decoder_rate_hz), 0);
        append_transmission(recording, retry, config);
        append_silence(recording, 3000.0);
        const Capture capture =
            decode(scaled_with_noise(as_double(recording.samples), 0.6, 15.0, static_cast<std::uint32_t>(55 + g)),
                   Profile::ssb);
        const Mapping m = map_events(recording, capture);
        std::size_t received = 0;
        for (std::size_t i = 0; i < retry.size(); ++i) received += m.received[1][i] == retry[i] ? 1 : 0;
        NOTE("retry %.1f s after the lone tune: %zu of %zu bytes", starts_s[g], received, retry.size());
        CHECK_EQ(received, retry.size());
        CHECK_EQ(m.score.wrong_bytes + m.score.extra_bytes, 0u);
    }
}

// A same-f_ref transmission (or a carrier) keying up 125..200 ms before the previous one ends masks the previous one's
// EOT markers (at 200 ms its STOP too): its short final frame (3 of hf's 5 bytes) was released at full length, with 2
// garbage bytes. The frame now ends before its first slot centre with a marker or a carrier on f_ref.
TEST(decoder_short_frame_under_the_next_tune) {
    const EncoderConfig config = preset_config(Preset::hf);
    const double overlaps_ms[] = {125.0, 150.0, 175.0, 200.0};
    const int seeds = 3;
    const std::size_t lead_samples = 4000;
    const std::size_t trailing_samples = 16000;
    std::size_t extra = 0;
    std::size_t wrong = 0;
    std::size_t released = 0;
    std::size_t runs = 0;
    for (std::size_t o = 0; o < test::count_of(overlaps_ms); ++o) {
        for (int seed = 0; seed < seeds; ++seed) {
            for (int carrier = 0; carrier < 2; ++carrier) {
                const std::vector<std::uint8_t> first = random_bytes(43, static_cast<std::uint32_t>(401 + 2 * seed));
                const std::vector<std::int16_t> a = encode(first, config);
                const std::vector<std::int16_t> b = encode(random_bytes(43, static_cast<std::uint32_t>(402 + 2 * seed)), config);
                const std::size_t overlap = static_cast<std::size_t>(overlaps_ms[o] * k_decoder_rate_hz / 1000.0);
                const std::size_t b0 = lead_samples + a.size() - overlap;
                std::vector<double> mix(b0 + b.size() + trailing_samples, 0.0);
                for (std::size_t i = 0; i < a.size(); ++i) mix[lead_samples + i] += a[i];
                for (std::size_t i = 0; i < b.size(); ++i) {
                    const double tone = config.amplitude * std::sin(2.0 * k_pi * config.tone_hz * i / k_decoder_rate_hz);
                    mix[b0 + i] += carrier != 0 ? tone : b[i];
                }
                const Capture capture = decode(scaled_with_noise(mix, 0.6, 20.0, static_cast<std::uint32_t>(403 + seed)),
                                               Profile::ssb);
                int lock = 0;
                for (std::size_t i = 0; i < capture.events.size() && lock < 2; ++i) {
                    const Event& e = capture.events[i];
                    if (e.type == EventType::locked) ++lock;
                    if ((e.type == EventType::end || e.type == EventType::lost) && lock == 1) lock = 2;
                    if (e.type != EventType::byte || lock != 1) continue;
                    const std::size_t position = e.frame_index * config.frame_bytes() + e.index;
                    ++released;
                    if (position >= first.size()) {
                        ++extra;
                    } else if (first[position] != e.value) {
                        ++wrong;
                    }
                }
                ++runs;
            }
        }
    }
    NOTE("%zu runs: first transmission released %zu bytes, %zu wrong, %zu past its end", runs, released, wrong, extra);
    CHECK_EQ(extra, 0u);
    CHECK_EQ(wrong, 0u);
    CHECK(released + 3 * runs >= runs * 43);  // at most the short frame is dropped
}

// A frequency step after the lock (the receiver's VFO or RIT moved by half a tone spacing or more, 20 dB): the rotated
// STOPs failed kappa, none was detected again, so the START/STOP AFC never ran, every later byte was wrong and the EOT
// was missed (hf_robust +10 Hz: 89 of 120 wrong, lost). A drift did the same (hf_weak from 0.3 Hz/s). The rotation of
// the missed STOPs now pulls the NCO back within two frames or so, at T >= 32 ms: at T = 16 ms a two-path fade mimics
// the rotation, so hf_fast keeps the limit of about 0.4 spacing (28 Hz).
TEST(decoder_frequency_step_in_track) {
    struct Case {
        Preset preset;
        double step_hz;
        double ramp_hz_per_s;
        std::size_t bytes;
    };
    const Case cases[] = {{Preset::hf_robust, 10.0, 0.0, 120}, {Preset::hf_weak, 5.0, 0.0, 80},
                          {Preset::hf_weak, -8.93, 0.0, 80},   {Preset::hf, -20.0, 0.0, 200},
                          {Preset::hf_robust, 0.0, 0.8, 120},  {Preset::hf_weak, 0.0, 0.5, 120}};
    const double step_after_s = 3.0;
    const std::size_t transient_frames = 3;
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        const EncoderConfig config = preset_config(cases[c].preset);
        const Recording recording = single(random_bytes(cases[c].bytes, 501), config, 500.0);
        const Transmission& transmission = recording.transmissions[0];
        std::vector<double> samples = as_double(recording.samples);
        samples.resize(samples.size() + static_cast<std::size_t>(40.0 * slot_samples(config)), 0.0);
        const std::size_t from = static_cast<std::size_t>(slot_start_sample(transmission, first_frame_slot(config)) +
                                                          step_after_s * k_decoder_rate_hz);
        const std::vector<std::int16_t> received = scaled_with_noise(
            retuned(samples, from, cases[c].step_hz, cases[c].ramp_hz_per_s), 0.6, 20.0, static_cast<std::uint32_t>(502 + c));
        Recording scored = recording;
        scored.samples = received;
        const Capture capture = decode(received, Profile::ssb);
        const Mapping m = map_events(scored, capture);
        const std::size_t limit = transient_frames * config.frame_bytes();
        NOTE("f_ref %u Hz, step %+.2f Hz, drift %.1f Hz/s: wrong %zu (limit %zu), lost %zu, extra %zu, ends %zu", unsigned(config.tone_hz),
             cases[c].step_hz, cases[c].ramp_hz_per_s, m.score.wrong_bytes, limit, m.score.lost_bytes, m.score.extra_bytes,
             m.score.ends);
        CHECK(m.score.wrong_bytes <= limit);
        CHECK_EQ(m.score.extra_bytes, 0u);
        CHECK_EQ(m.score.ends, 1u);
        CHECK_EQ(m.score.lost_events, 0u);
    }
}

// L11: back-to-back transmissions with different modes and f_ref, 0.5 s gaps.
TEST(decoder_l11_back_to_back) {
    const std::uint32_t slots[] = {16, 64, 32, 128, 20, 50, 16};
    const std::uint16_t tones[] = {1000, 2200, 700, 1500, 2500, 400, 1800};
    Recording recording;
    append_silence(recording, k_gap_ms);
    for (std::size_t i = 0; i < test::count_of(slots); ++i) {
        const EncoderConfig config = slot_config(slots[i], tones[i]);
        REQUIRE(config.valid());
        append_transmission(recording, random_bytes(config.frame_bytes() + 2, static_cast<std::uint32_t>(110 + i)), config);
        append_silence(recording, k_gap_ms + 2.0 * slots[i]);
    }
    const Score s = score(recording, decode(usb(recording.samples, 10.0, 111, k_amplitude), Profile::ssb));
    report("back to back", s);
    CHECK_EQ(s.wrong_bytes, 0u);
    CHECK_EQ(s.lost_bytes, 0u);
    CHECK_EQ(s.extra_bytes, 0u);
    CHECK_EQ(s.locks, test::count_of(slots));
    CHECK_EQ(s.ends, test::count_of(slots));
}

// A3' at T = 8 ms near the gate (+0.5 dB, gate - 1): a third of the 8 sync markers flip at q 2..4 only. The weak
// markers complete the train (3 hits and 2 weak markers are enough), which the 5-hit rule alone missed in 1 of 10.
TEST(decoder_weak_marker_sync_t8) {
    const EncoderConfig config = mode_config(8, 3);
    REQUIRE(config.valid());
    const int trials = 40;
    const int required = 38;
    const double snr_db = 0.5;
    const double offset_span_hz = 100.0;
    std::mt19937 generator(190);
    std::uniform_real_distribution<double> offset(-0.5 * offset_span_hz, 0.5 * offset_span_hz);
    int locked = 0;
    for (int trial = 0; trial < trials; ++trial) {
        Recording recording;
        append_silence(recording, 1000.0);
        append_transmission(recording, random_bytes(2 * config.frame_bytes(), static_cast<std::uint32_t>(191 + trial)), config);
        append_silence(recording, 200.0);
        const Capture capture = decode(usb(recording.samples, snr_db, static_cast<std::uint32_t>(240 + trial),
                                           config.amplitude, offset(generator)),
                                       Profile::am);
        bool ok = false;
        for (std::size_t i = 0; i < capture.events.size(); ++i) {
            const Event& e = capture.events[i];
            ok = ok || (e.type == EventType::locked && e.bits_per_peak == config.bits_per_peak &&
                        e.slot_ms == config.slot_us / 1000.0f);
        }
        locked += ok ? 1 : 0;
    }
    NOTE("T8 k3 at %+.1f dB: %d of %d transmissions locked", snr_db, locked, trials);
    CHECK(locked >= required);
}

// C13 (spec 8.4): a receiver AGC (1 ms attack, 300 ms decay) at high SNR. After each transmission it raises the noise
// by the SNR within about a second; under the next tune it drops it again. The search must not take the rising noise
// for tones (the next tune would come while the decoder chases one), and the train is read against the noise under
// the tune, not the AGC-raised noise before it (without either, 2 of these 4 transmissions got no header).
TEST(decoder_agc_back_to_back_high_snr) {
    const EncoderConfig config = preset_config(Preset::hf);
    const std::size_t transmissions = 4;
    const double gap_ms = 1500.0;
    Recording recording;
    append_silence(recording, gap_ms);
    for (std::size_t i = 0; i < transmissions; ++i) {
        append_transmission(recording, random_bytes(3 * config.frame_bytes(), static_cast<std::uint32_t>(180 + i)), config);
        append_silence(recording, gap_ms);
    }
    sim::ChannelConfig channel;
    channel.mode = sim::Mode::usb;
    channel.snr_db = 30.0;
    channel.seed = 181;
    channel.agc = true;
    channel.agc_attack_ms = 1.0;
    channel.agc_decay_ms = 300.0;
    const Score s = score(recording, decode(through_channel(recording.samples, channel, config.amplitude), Profile::ssb));
    report("AGC 1/300 ms, 30 dB", s);
    CHECK_EQ(s.locks, transmissions);
    CHECK_EQ(s.lost_bytes, 0u);
    CHECK_EQ(s.wrong_bytes, 0u);
    CHECK_EQ(s.extra_bytes, 0u);
}

// L12 / L17: each default profile decodes the presets in its range (clean and at gate + 3 dB) and ignores the
// others; the fm profile decodes hf_fast and hf (f_ref >= 1000 Hz).
TEST(decoder_l12_profile_coverage) {
    const Profile profiles[] = {Profile::ssb, Profile::am, Profile::fm};
    for (std::size_t p = 0; p < test::count_of(profiles); ++p) {
        for (std::size_t i = 0; i < test::count_of(k_presets); ++i) {
            const PresetCase& preset = k_presets[i];
            const EncoderConfig config = preset_config(preset.preset);
            const Recording recording = single(random_bytes(2 * config.frame_bytes() + 1, static_cast<std::uint32_t>(120 + i)), config);
            const bool inside = covers(profiles[p], config);
            // FM presets sit at the top of an SSB receiver's passband: judged clean only.
            const bool fm_preset = preset.tolerance_hz == 0.0;
            const double snrs[] = {99.0, fm_preset ? 99.0 : preset.gate_db + k_gate_margin_db};
            for (std::size_t n = 0; n < test::count_of(snrs); ++n) {
                const std::vector<std::int16_t> samples =
                    snrs[n] > 90.0 ? recording.samples : usb(recording.samples, snrs[n], 121 + i, config.amplitude);
                const Score s = score(recording, decode(samples, profiles[p]));
                if (inside) {
                    if (s.wrong_bytes != 0 || s.lost_bytes != 0 || s.locks != 1) {
                        NOTE("profile %s preset %s snr %.1f", profile_name(profiles[p]), preset.name, snrs[n]);
                        report("in range", s);
                    }
                    CHECK_EQ(s.wrong_bytes, 0u);
                    CHECK_EQ(s.lost_bytes, 0u);
                    CHECK_EQ(s.locks, 1u);
                } else {
                    if (s.locks != 0 || s.bytes_released != 0) {
                        NOTE("profile %s preset %s snr %.1f", profile_name(profiles[p]), preset.name, snrs[n]);
                        report("out of range", s);
                    }
                    CHECK_EQ(s.locks, 0u);
                    CHECK_EQ(s.bytes_released, 0u);
                }
            }
        }
    }
}

// L15: short final frames, n = 1..B - 1 and n = B + 1..2B - 1: exactly n bytes, then end.
TEST(decoder_l15_short_final_frame) {
    const EncoderConfig configs[] = {preset_config(Preset::hf_fast), mode_config(16, 3, 32), mode_config(64, 6, 16),
                                     mode_config(32, 6, 8, Spacing::dense)};
    std::size_t failures = 0;
    std::size_t runs = 0;
    for (std::size_t c = 0; c < test::count_of(configs); ++c) {
        const EncoderConfig& config = configs[c];
        REQUIRE(config.valid());
        const std::size_t bytes_per_frame = config.frame_bytes();
        for (std::size_t n = 1; n < 2 * bytes_per_frame; ++n) {
            if (n == bytes_per_frame) continue;
            const Recording recording = single(random_bytes(n, static_cast<std::uint32_t>(130 + n)), config);
            const Capture capture = decode(recording.samples, Profile::ssb);
            const Score s = score(recording, capture);
            ++runs;
            if (s.matched == n && s.wrong_bytes == 0 && s.extra_bytes == 0 && s.ends == 1 && s.locks == 1) continue;
            ++failures;
            NOTE("T=%u k=%u N=%u, %zu bytes", config.slot_us / 1000, config.bits_per_peak, config.data_slots, n);
            report("  ", s);
        }
    }
    NOTE("%zu transmissions, %zu failed", runs, failures);
    CHECK_EQ(failures, 0u);
}

// L16: a header with the reserved N code 3: lost(unsupported_mode), no byte.
TEST(decoder_l16_unsupported_mode) {
    const EncoderConfig config = preset_config(Preset::hf);
    Recording recording = single(random_bytes(20, 140), config);
    const std::uint16_t reserved = static_cast<std::uint16_t>(header_word(5, 8, config.slot_us, Spacing::standard) | (3u << 7));
    CHECK_EQ(+header_fields(reserved).data_slots, 0);
    write_header(recording, 0, reserved);
    const Capture capture = decode(recording.samples, Profile::ssb);
    CHECK_EQ(count_events(capture, EventType::byte), 0u);
    CHECK_EQ(count_events(capture, EventType::locked), 0u);
    const std::size_t lost = find_event(capture, EventType::lost);
    REQUIRE(lost < capture.events.size());
    CHECK(capture.events[lost].reason == LostReason::unsupported_mode);

    // The same transmission with its own header written back decodes: the synthetic peaks match the encoder's.
    Recording rewritten = single(random_bytes(20, 140), config);
    write_header(rewritten, 0, header_word(5, 8, config.slot_us, Spacing::standard));
    const Score s = score(rewritten, decode(rewritten.samples, Profile::ssb));
    CHECK_EQ(s.matched, 20u);
    CHECK_EQ(s.wrong_bytes, 0u);
}

// A4: the SNR report within 1.5 dB from the gate to gate + 20 dB (mean over three transmissions).
TEST(decoder_a4_snr_report) {
    const std::size_t presets[] = {2, 3, 4};
    const double span_db = 20.0;
    const double step_db = 10.0;
    const int repeats = 3;
    const double tolerance_db = 1.5;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        const PresetCase& preset = k_presets[presets[p]];
        const EncoderConfig config = preset_config(preset.preset);
        for (double extra = 0.0; extra <= span_db; extra += step_db) {
            const double snr = preset.gate_db + extra;
            double reported = 0.0;
            int counted = 0;
            for (int r = 0; r < repeats; ++r) {
                const std::uint32_t seed = static_cast<std::uint32_t>(200 + 100 * p + 10 * r + extra);
                const Recording recording = single(random_bytes(4 * config.frame_bytes(), seed), config);
                const Capture capture = decode(usb(recording.samples, snr, seed, config.amplitude), Profile::ssb);
                for (std::size_t i = 0; i < capture.events.size(); ++i) {
                    if (capture.events[i].type != EventType::byte) continue;
                    reported += capture.events[i].snr_db;
                    ++counted;
                }
            }
            REQUIRE(counted > 0);
            reported /= counted;
            NOTE("%s, SNR %+.1f dB: reported %+.1f dB", preset.name, snr, reported);
            CHECK_NEAR(reported, snr, tolerance_db);
        }
    }
}

// A5 (short): the integrated slot decisions against the genie (known grid, same audio) at the hf gate.
TEST(decoder_a5_genie_short) {
    const EncoderConfig config = preset_config(Preset::hf);
    const std::size_t frames = 30;
    const double snr = k_presets[3].gate_db - 1.0;
    std::size_t bits = 0;
    std::size_t errors = 0;
    std::size_t genie_errors = 0;
    for (int tx = 0; tx < 6; ++tx) {
        const Recording recording = single(random_bytes(frames * config.frame_bytes(), static_cast<std::uint32_t>(150 + tx)), config);
        sim::ChannelConfig channel;
        channel.mode = sim::Mode::usb;
        channel.snr_db = snr;
        channel.seed = static_cast<std::uint32_t>(151 + tx);
        channel.freq_offset_hz = k_usb_offset_hz;
        const std::vector<std::int16_t> samples = through_channel(recording.samples, channel, config.amplitude);
        const long delay = channel_delay(recording.samples, channel, config.amplitude);
        const Mapping m = map_events(recording, decode(samples, Profile::ssb));
        const std::vector<std::uint8_t> genie =
            genie_bytes(samples, recording.transmissions[0], config.tone_hz + k_usb_offset_hz, -1, delay);
        const std::vector<std::uint8_t>& data = recording.transmissions[0].data;
        for (std::size_t k = 0; k < data.size(); ++k) {
            if (m.received[0][k] < 0) continue;  // compare on the bytes the decoder released
            bits += k_bits_per_byte;
            for (int b = 0; b < k_bits_per_byte; ++b) {
                errors += ((m.received[0][k] ^ data[k]) >> b) & 1;
                genie_errors += ((genie[k] ^ data[k]) >> b) & 1;
            }
        }
    }
    NOTE("hf at %.1f dB: integrated %zu, genie %zu bit errors in %zu bits", snr, errors, genie_errors, bits);
    REQUIRE(bits > 0);
    CHECK(static_cast<double>(errors) <= 1.5 * genie_errors + 10.0);
}

// A steady carrier (+6 dB) or keyed CW (0 dB) present from the first sample, outside the grid: no loss. The
// interferers alone produce no lock.
TEST(decoder_qrm_from_start) {
    struct Case {
        Preset preset;
        bool carrier;  // otherwise keyed CW
        std::uint32_t seed;
    };
    const Case cases[] = {{Preset::hf, true, 1},       {Preset::hf, false, 1},       {Preset::hf_fast, true, 2},
                          {Preset::hf_fast, false, 2}, {Preset::hf_robust, true, 1}, {Preset::hf_robust, false, 2}};
    const double snr_db = 10.0;
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        sim::ChannelConfig channel;
        channel.mode = sim::Mode::usb;
        channel.snr_db = snr_db;
        channel.seed = cases[c].seed;
        if (cases[c].carrier) {
            channel.carrier_hz = 600.0;
            channel.carrier_db = 6.0;
        } else {
            channel.cw_hz = 2500.0;
            channel.cw_db = 0.0;
        }
        const EncoderConfig config = preset_config(cases[c].preset);
        Recording recording;
        append_transmission(recording, random_bytes(2 * config.frame_bytes() + 3, 160 + c), config);
        append_silence(recording, 1000.0);
        const Capture capture = decode(through_channel(recording.samples, channel, config.amplitude), Profile::ssb);
        const Score s = score(recording, capture);
        NOTE("%s, %s: lost %zu wrong %zu locks %zu", k_presets[static_cast<int>(cases[c].preset)].name,
             cases[c].carrier ? "carrier +6 dB at 600 Hz" : "CW 0 dB at 2500 Hz", s.lost_bytes, s.wrong_bytes, s.locks);
        CHECK_EQ(s.lost_bytes, 0u);
        CHECK_EQ(s.wrong_bytes, 0u);
        CHECK_EQ(s.locks, 1u);
    }
    for (int carrier = 0; carrier < 2; ++carrier) {
        sim::ChannelConfig channel;
        channel.mode = sim::Mode::usb;
        channel.snr_db = snr_db;
        channel.seed = 77;
        channel.carrier_hz = carrier ? 1000.0 : 0.0;
        channel.carrier_db = 6.0;
        channel.cw_hz = carrier ? 0.0 : 1800.0;
        channel.cw_db = 0.0;
        const std::vector<std::int16_t> quiet(static_cast<std::size_t>(20 * k_decoder_rate_hz), 0);
        const Capture capture = decode(through_channel(quiet, channel, k_amplitude), Profile::ssb);
        CHECK_EQ(count_events(capture, EventType::locked), 0u);
        CHECK_EQ(count_events(capture, EventType::byte), 0u);
    }
}

// C8' (short): a steady carrier on grid tone 3 at 0 dB relative to the key-down tone, hf at 10 dB. The per-bin
// background suppresses it; peaks on its tone are erased (1 slot in 32).
TEST(decoder_carrier_in_grid) {
    const EncoderConfig config = preset_config(Preset::hf);
    const std::vector<std::uint8_t> data = random_bytes(40 * config.frame_bytes(), 170);
    const Recording recording = single(data, config);
    sim::ChannelConfig channel;
    channel.mode = sim::Mode::usb;
    channel.snr_db = 10.0;
    channel.seed = 171;
    const double slot_s = config.slot_us / 1e6;
    channel.carrier_hz = config.tone_hz - (k_grid_guard + 3.0 * k_standard_spacing_num / k_standard_spacing_den) / slot_s;
    channel.carrier_db = 0.0;
    const Mapping m = map_events(recording, decode(through_channel(recording.samples, channel, config.amplitude), Profile::ssb));
    report("carrier on grid tone 3", m.score);
    NOTE("BER %.2e", m.score.ber());
    CHECK_EQ(m.score.locks, 1u);
    CHECK(m.score.matched >= data.size() * 9 / 10);
    CHECK(m.score.ber() <= 2e-2);
}

// Near and below the FM threshold no byte comes from outside a confirmed lock, and every lock has the sender's T.
TEST(decoder_fm_threshold_integrity) {
    const Preset presets[] = {Preset::fm, Preset::fm_fast};
    const double snrs[] = {7.0, 8.0, 9.0};  // carrier SNR in 2500 Hz; CNR = snr - 7 dB
    std::size_t wrong = 0;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        const EncoderConfig config = preset_config(presets[p]);
        for (std::size_t n = 0; n < test::count_of(snrs); ++n) {
            for (std::uint32_t trial = 0; trial < 3; ++trial) {
                const Recording recording = single(random_bytes(20 * config.frame_bytes(), 300 + trial), config, 400.0);
                sim::ChannelConfig channel;
                channel.mode = sim::Mode::fm;
                channel.snr_db = snrs[n];
                channel.seed = 500 + trial + 10 * n;
                const Capture capture = decode(through_channel(recording.samples, channel, config.amplitude), Profile::fm);
                CHECK(bytes_inside_locks(capture));
                for (std::size_t i = 0; i < capture.events.size(); ++i) {
                    if (capture.events[i].type == EventType::locked) CHECK_EQ(capture.events[i].slot_ms, config.slot_us / 1000.0f);
                }
                wrong += score(recording, capture).extra_bytes;
            }
        }
    }
    NOTE("extra bytes near the FM threshold: %zu", wrong);
}

// Senders outside a profile's range produce no locked event and no byte.
TEST(decoder_out_of_range_speeds) {
    struct Case {
        Profile profile;
        std::uint32_t slot_ms;
        double snr_db;  // 99: clean
    };
    const Case cases[] = {{Profile::ssb, 14, 99.0}, {Profile::ssb, 14, 10.0}, {Profile::ssb, 6, 10.0},
                          {Profile::am, 7, 20.0},   {Profile::am, 90, 20.0},  {Profile::am, 110, 20.0},
                          {Profile::fm, 80, 20.0}};
    const int seeds = 2;
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        std::size_t locks = 0;
        std::size_t bytes = 0;
        for (int s = 0; s < seeds; ++s) {
            // Centred band, the grid below f_ref and then above it.
            const EncoderConfig base = slot_config(cases[c].slot_ms, 1500);
            const EncoderConfig config = mode_config(cases[c].slot_ms, base.bits_per_peak, 8, Spacing::standard,
                                                     s == 0 ? GridSide::below : GridSide::above);
            REQUIRE(config.valid());
            const Recording recording = single(random_bytes(2 * config.frame_bytes() + 1, 7 + s), config, 500.0);
            std::vector<std::int16_t> samples = recording.samples;
            if (cases[c].snr_db < 90.0) samples = usb(recording.samples, cases[c].snr_db, 3 + s, config.amplitude, 13.0 * s);
            const Capture capture = decode(samples, cases[c].profile);
            locks += count_events(capture, EventType::locked);
            bytes += count_events(capture, EventType::byte);
        }
        if (locks != 0 || bytes != 0) {
            NOTE("profile %s, T=%u ms, snr %.0f: locks %zu bytes %zu", profile_name(cases[c].profile), cases[c].slot_ms,
                 cases[c].snr_db, locks, bytes);
        }
        CHECK_EQ(locks, 0u);
        CHECK_EQ(bytes, 0u);
    }
}
