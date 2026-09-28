#include "support/loopback.hpp"
#include "test_harness.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using unlimited::ConfigError;
using unlimited::DecisionMode;
using unlimited::Decoder;
using unlimited::DecoderConfig;
using unlimited::DecoderState;
using unlimited::EncoderConfig;
using unlimited::Event;
using unlimited::EventType;
using unlimited::LostReason;
using unlimited::Passband;
using namespace unlimited::loopback;

namespace sim = unlimited::sim;

namespace {

using std::int16_t;
using std::size_t;
using std::uint32_t;
using std::uint8_t;
using test::count_of;

const float k_speeds[] = {1.0f, 3.0f, 6.0f, 12.0f, 25.0f};
// v0.3's gates at the same T (spec 4, provisional), key-down tone in 2500 Hz; gate + 3 dB is where A3 and S1 are run.
const double k_gate_db[] = {-6.5, -1.7, 1.3, 4.3, 8.0};
const double k_gate_margin_db = 3.0;
const double k_strong_db = 20.0;
const double k_silence_ms = 700.0;
const size_t k_chunk = 64;
const double k_slot_tolerance = 0.005;  // slot_ms within 0.5 % of T
const float k_tone_tolerance_hz = 1.0f;
const uint8_t k_full_pct = 100;
const uint8_t k_pct_tolerance = 10;
const size_t k_bits_per_byte = 8;
const size_t k_window_slots = unlimited::k_window_slots;
const double k_margin_slots = 0.5;     // the history a window needs past its STOP before it is read
const double k_pipeline_blocks = 5.0;  // 2 blocks of the CIC-2, 2 of the impulse blanker's delay, 1 of rounding
const double k_grid_slots = 0.1;       // the tracked grid against the sent one

double slot_of(const EncoderConfig& config) {
    return slot_samples(config);
}

double block_of(const EncoderConfig& config) {
    const double blocks = std::round(slot_samples(config) / 8.0);
    return std::min(std::max(blocks, 4.0), 32.0);
}

std::vector<int16_t> noisy(const Recording& recording, const EncoderConfig& config, double snr_db, uint32_t seed,
                           double offset_hz = 0.0) {
    return usb(recording.samples, snr_db, seed, config.amplitude, offset_hz);
}

Mapping decode(const Recording& recording, const std::vector<int16_t>& samples, const DecoderConfig& receiver,
               double delay, Capture* keep = nullptr, size_t chunk = k_chunk) {
    const Capture capture = run_decoder(samples, receiver, chunk);
    if (keep != nullptr) *keep = capture;
    return map_events(recording, capture, delay);
}

// Every byte of every transmission, each transmission locked once and ended once, nothing extra or lost.
bool exact(const Score& s, size_t transmissions) {
    return s.bytes_released == s.bytes_sent && s.matched == s.bytes_sent && s.wrong_bytes == 0 && s.lost_bytes == 0 &&
           s.extra_bytes == 0 && s.locks == transmissions && s.ends == transmissions && s.lost_events == 0 &&
           s.locked_transmissions == transmissions;
}

void note_score(const char* what, const Score& s) {
    NOTE("%s: sent %zu released %zu matched %zu wrong %zu (bits %zu) lost %zu extra %zu locks %zu ends %zu lost %zu "
         "framing %zu",
         what, s.bytes_sent, s.bytes_released, s.matched, s.wrong_bytes, s.bit_errors, s.lost_bytes, s.extra_bytes,
         s.locks, s.ends, s.lost_events, s.framing_windows);
}

size_t count_type(const Capture& capture, EventType type) {
    return count_events(capture, type);
}

}  // namespace

// ---------------------------------------------------------------------------
// U30: configuration and construction (spec 3, 5)
// ---------------------------------------------------------------------------

TEST(decoder_config_defaults_and_check) {
    const DecoderConfig config;
    CHECK_EQ(config.slot_us, unlimited::slot_us_for_speed(6.0f));
    CHECK_EQ(config.passband.low_hz, 300);
    CHECK_EQ(config.passband.high_hz, 2700);
    CHECK_EQ(unsigned(config.threshold_percent), 70u);
    CHECK(config.decision_mode == DecisionMode::fixed);
    CHECK(config.impulse_blanker);
    CHECK(config.valid());
    const Passband search = config.search_range();
    CHECK_EQ(search.low_hz, 432);
    CHECK_EQ(search.high_hz, 2568);

    struct Case {
        const char* name;
        ConfigError expected;
        void (*change)(DecoderConfig&);
    };
    const Case cases[] = {
        {"T 0", ConfigError::slot, [](DecoderConfig& c) { c.slot_us = 0; }},
        {"T 3999 us", ConfigError::slot, [](DecoderConfig& c) { c.slot_us = 3999; }},
        {"T 100001 us", ConfigError::slot, [](DecoderConfig& c) { c.slot_us = 100001; }},
        {"passband inverted", ConfigError::passband,
         [](DecoderConfig& c) {
             c.passband.low_hz = 2700;
             c.passband.high_hz = 300;
         }},
        {"passband above 4000", ConfigError::passband, [](DecoderConfig& c) { c.passband.high_hz = 4001; }},
        {"no pitch left at 25 bytes/s", ConfigError::passband,
         [](DecoderConfig& c) {
             c.slot_us = 4000;
             c.passband.low_hz = 1000;
             c.passband.high_hz = 1500;
         }},
        {"threshold 49", ConfigError::threshold, [](DecoderConfig& c) { c.threshold_percent = 49; }},
        {"threshold 91", ConfigError::threshold, [](DecoderConfig& c) { c.threshold_percent = 91; }},
        {"decision mode 7", ConfigError::decision_mode,
         [](DecoderConfig& c) { c.decision_mode = static_cast<DecisionMode>(7); }},
    };
    for (size_t i = 0; i < count_of(cases); ++i) {
        DecoderConfig bad;
        cases[i].change(bad);
        if (!CHECK(bad.check() == cases[i].expected)) NOTE("case %s", cases[i].name);
        CHECK(!bad.valid());
    }
    DecoderConfig limits;
    limits.threshold_percent = 50;
    CHECK(limits.valid());
    limits.threshold_percent = 90;
    CHECK(limits.valid());
    limits.decision_mode = DecisionMode::adaptive;
    CHECK(limits.valid());
}

// An invalid configuration leaves the decoder idle: no events, whatever it hears.
TEST(decoder_invalid_config_stays_idle) {
    DecoderConfig bad;
    bad.slot_us = 0;
    const EncoderConfig config = speed_config(6.0f);
    const Recording recording = single(random_bytes(4, 1), config, k_silence_ms);
    const Capture capture = run_decoder(recording.samples, bad, k_chunk);
    CHECK(capture.events.empty());
    Decoder decoder(bad, nullptr, nullptr);
    decoder.process(recording.samples.data(), recording.samples.size());
    CHECK(decoder.state() == DecoderState::search);
    CHECK(!decoder.dcd());
}

// Spec 3.1, 3.9: the look-ahead is 2 T + 200 ms, at most 400 ms; the size stays within its budget.
TEST(decoder_size_and_lookahead) {
    NOTE("sizeof(Decoder) = %zu B on this host (budget 18432 B)", sizeof(Decoder));
    CHECK(sizeof(Decoder) <= 18432u);
    const uint32_t expected[] = {3200, 2133, 1867, 1733, 1664};
    for (size_t v = 0; v < count_of(k_speeds); ++v) {
        DecoderConfig config;
        config.slot_us = unlimited::slot_us_for_speed(k_speeds[v]);
        config.passband.low_hz = 100;
        config.passband.high_hz = 3000;
        Decoder decoder(config, nullptr, nullptr);
        if (!CHECK(std::abs(int(decoder.lookahead_samples()) - int(expected[v])) <= 1)) {
            NOTE("%.0f bytes/s: %u samples", static_cast<double>(k_speeds[v]), unsigned(decoder.lookahead_samples()));
        }
        CHECK_NEAR(decoder.slot_ms(), config.slot_us / 1000.0, 1e-3);
        CHECK_EQ(decoder.tone_hz(), 0.0f);
        CHECK_EQ(decoder.snr_db(), 0.0f);
    }
}

// ---------------------------------------------------------------------------
// U31: loopbacks (spec 8)
// ---------------------------------------------------------------------------

// Clean audio at the five speeds: every byte at its byte_index, one lock, one end, T and the pitch as sent, each
// byte's 8 slot events with the window's levels.
TEST(decoder_clean_loopback_all_speeds) {
    for (size_t v = 0; v < count_of(k_speeds); ++v) {
        const EncoderConfig config = speed_config(k_speeds[v]);
        const std::vector<uint8_t> data = random_bytes(24, uint32_t(100 + v));
        const Recording recording = single(data, config, k_silence_ms);
        Capture capture;
        const Mapping mapping = decode(recording, recording.samples, receiver_for(config), 0.0, &capture);
        if (!CHECK(exact(mapping.score, 1))) {
            NOTE("%.0f bytes/s", static_cast<double>(k_speeds[v]));
            note_score("clean", mapping.score);
            continue;
        }
        size_t slots = 0;
        for (size_t i = 0; i < capture.events.size(); ++i) {
            const Event& e = capture.events[i];
            if (e.type == EventType::slot) {
                ++slots;
                CHECK(e.slot >= unlimited::k_first_data_slot && e.slot < unlimited::k_stop_slot);
                CHECK(std::abs(int(e.start_pct) - int(k_full_pct)) <= k_pct_tolerance);
                CHECK(std::abs(int(e.stop_pct) - int(k_full_pct)) <= k_pct_tolerance);
                CHECK_EQ(unsigned(e.threshold_pct), 70u);
                CHECK(e.value == 1 ? e.level_pct >= 90 && e.soft[0] > 0 : e.level_pct <= 10 && e.soft[0] < 0);
            }
            if (e.type != EventType::byte) continue;
            CHECK_NEAR(e.slot_ms / (config.slot_us / 1000.0), 1.0, k_slot_tolerance);
            CHECK_NEAR(e.tone_hz, 1500.0f, k_tone_tolerance_hz);
            CHECK_EQ(e.flags, 0);
            for (size_t b = 0; b < k_bits_per_byte; ++b) {
                const bool one = ((e.value >> (k_bits_per_byte - 1 - b)) & 1u) != 0;
                CHECK(one ? e.soft[b] > 0 : e.soft[b] < 0);
            }
        }
        CHECK_EQ(slots, data.size() * k_bits_per_byte);
    }
}

// Spec 3.6: each byte comes out as soon as its window is in the history: half a slot and a few blocks after its STOP
// (plus the look-ahead); the end comes a silent window after the last STOP (spec 3.7).
TEST(decoder_streams_each_byte_at_its_stop) {
    const float speeds[] = {1.0f, 6.0f, 25.0f};
    for (size_t v = 0; v < count_of(speeds); ++v) {
        const EncoderConfig config = speed_config(speeds[v]);
        const std::vector<uint8_t> data = random_bytes(12, uint32_t(7 + v));
        const Recording recording = single(data, config, k_silence_ms);
        const Capture capture = run_decoder(recording.samples, receiver_for(config), 0);
        const Decoder probe(receiver_for(config), nullptr, nullptr);
        const Transmission& tx = recording.transmissions[0];
        const double slot = slot_of(config);
        const double block = block_of(config);
        double worst = 0.0;
        size_t bytes = 0;
        for (size_t i = 0; i < capture.events.size(); ++i) {
            const Event& e = capture.events[i];
            const double heard = static_cast<double>(capture.event_sample[i]) - capture.lookahead;
            // The windows of the check before lock (and the one being read) come out with the lock.
            if (e.type == EventType::byte && e.byte_index > probe.acquire_windows()) {
                const double stop_end = slot_start_sample(tx, e.byte_index + 1, 0);
                const double latency = heard - stop_end;
                worst = std::max(worst, latency / slot);
                CHECK(latency >= k_margin_slots * slot + (k_pipeline_blocks - 1.0) * block - k_grid_slots * slot);
                CHECK(latency <= k_margin_slots * slot + k_pipeline_blocks * block + k_grid_slots * slot);
                ++bytes;
            }
            if (e.type == EventType::end) {
                const double after = heard - end_sample(tx);
                CHECK(after >= (k_window_slots + k_margin_slots) * slot + (k_pipeline_blocks - 1.0) * block -
                                   k_grid_slots * slot);
                CHECK(after <= (k_window_slots + k_margin_slots) * slot + k_pipeline_blocks * block +
                                   k_grid_slots * slot);
                NOTE("%.0f bytes/s: end %.2f slots after the last STOP (look-ahead %zu samples)",
                     static_cast<double>(speeds[v]), after / slot, capture.lookahead);
            }
        }
        CHECK_EQ(bytes, data.size() - probe.acquire_windows() - 1u);
        NOTE("%.0f bytes/s: bytes %.2f slots after their STOP at most", static_cast<double>(speeds[v]), worst);
    }
}

// Spec 4 A1: AWGN at the gate and 3 dB above it (the key-down tone in 2500 Hz), the fixed 70 % line.
TEST(decoder_awgn_per_speed) {
    const size_t transmissions = 8;
    const size_t bytes = 16;
    for (size_t v = 0; v < count_of(k_speeds); ++v) {
        const EncoderConfig config = speed_config(k_speeds[v]);
        const double delay = channel_delay_samples();
        Score strong;
        Score gate;
        for (size_t t = 0; t < transmissions; ++t) {
            const Recording recording = single(random_bytes(bytes, uint32_t(1000 + 10 * v + t)), config, k_silence_ms);
            const double snrs[] = {k_gate_db[v] + k_gate_margin_db, k_gate_db[v]};
            Score* scores[] = {&strong, &gate};
            for (size_t s = 0; s < 2; ++s) {
                const std::vector<int16_t> samples = noisy(recording, config, snrs[s], uint32_t(500 + 17 * t + v));
                const Score sc = decode(recording, samples, receiver_for(config), delay).score;
                scores[s]->bytes_sent += sc.bytes_sent;
                scores[s]->matched += sc.matched;
                scores[s]->bit_errors += sc.bit_errors;
                scores[s]->lost_bytes += sc.lost_bytes;
                scores[s]->extra_bytes += sc.extra_bytes;
                scores[s]->locked_transmissions += sc.locked_transmissions;
            }
        }
        NOTE("%2.0f bytes/s: gate+3 dB (%.1f dB) BER %.2e loss %.1f %% locked %zu/%zu; gate (%.1f dB) BER %.2e loss "
             "%.1f %% locked %zu/%zu",
             static_cast<double>(k_speeds[v]), k_gate_db[v] + k_gate_margin_db, strong.ber(), 100.0 * strong.loss(),
             strong.locked_transmissions, transmissions, k_gate_db[v], gate.ber(), 100.0 * gate.loss(),
             gate.locked_transmissions, transmissions);
        CHECK_EQ(strong.locked_transmissions, transmissions);
        CHECK(strong.ber() <= 5e-3);
        CHECK_EQ(strong.extra_bytes, 0u);
        CHECK_EQ(gate.extra_bytes, 0u);
    }
}

// Spec 3.2, 4 L19: the receiver finds the pitch anywhere in its search: mistuned +-50 Hz, and the whole signal
// shifted across the passband (USB mistuning), at 20 dB.
TEST(decoder_mistuned_and_shifted) {
    const double delay = channel_delay_samples();
    const double offsets[] = {-50.0, -17.0, 0.0, 33.0, 50.0};
    for (size_t v = 0; v < count_of(k_speeds); ++v) {
        const EncoderConfig config = speed_config(k_speeds[v]);
        for (size_t o = 0; o < count_of(offsets); ++o) {
            const Recording recording = single(random_bytes(8, uint32_t(40 + o)), config, k_silence_ms);
            const Score s = decode(recording, noisy(recording, config, k_strong_db, uint32_t(9 + o), offsets[o]),
                                   receiver_for(config), delay)
                                .score;
            if (!CHECK(exact(s, 1))) {
                NOTE("%.0f bytes/s, offset %+.0f Hz", static_cast<double>(k_speeds[v]), offsets[o]);
                note_score("mistuned", s);
            }
        }
    }
    // Shifted: the tone anywhere in the search range of 6 bytes/s (432..2568 Hz) and of 25 bytes/s in 300..3000 Hz
    // (850..2450 Hz), the edges included.
    struct Shift {
        float speed;
        uint16_t tone_hz;
        uint16_t low_hz;
        uint16_t high_hz;
    };
    const Shift shifts[] = {{6.0f, 440, 300, 2700},  {6.0f, 800, 300, 2700},   {6.0f, 2200, 300, 2700},
                            {6.0f, 2560, 300, 2700}, {25.0f, 860, 300, 3000},  {25.0f, 2440, 300, 3000},
                            {1.0f, 330, 100, 3000},  {1.0f, 2690, 100, 3000}};
    for (size_t i = 0; i < count_of(shifts); ++i) {
        EncoderConfig config = speed_config(shifts[i].speed, shifts[i].tone_hz);
        config.passband.low_hz = shifts[i].low_hz;
        config.passband.high_hz = shifts[i].high_hz;
        REQUIRE(config.valid());
        const Recording recording = single(random_bytes(8, uint32_t(60 + i)), config, k_silence_ms);
        sim::ChannelConfig channel;
        channel.mode = sim::Mode::usb;
        channel.snr_db = k_strong_db;
        channel.seed = uint32_t(70 + i);
        channel.rx_low_hz = shifts[i].low_hz;
        channel.rx_high_hz = shifts[i].high_hz;
        const std::vector<int16_t> samples = through_channel(recording.samples, channel, config.amplitude);
        Capture capture;
        const Score s = decode(recording, samples, receiver_for(config), delay, &capture).score;
        if (!CHECK(exact(s, 1))) {
            NOTE("%.0f bytes/s at %u Hz in %u-%u Hz", static_cast<double>(shifts[i].speed), shifts[i].tone_hz,
                 shifts[i].low_hz, shifts[i].high_hz);
            note_score("shifted", s);
        }
        for (size_t e = 0; e < capture.events.size(); ++e) {
            if (capture.events[e].type == EventType::byte)
                CHECK_NEAR(capture.events[e].tone_hz, shifts[i].tone_hz, 2.0);
        }
    }
}

// Spec 1.1, 4: USB and LSB (the spectrum inverted about the pivot) at the five speeds.
TEST(decoder_usb_and_lsb) {
    const sim::Mode modes[] = {sim::Mode::usb, sim::Mode::lsb};
    for (size_t m = 0; m < count_of(modes); ++m) {
        const double delay = channel_delay_samples(modes[m]);
        for (size_t v = 0; v < count_of(k_speeds); ++v) {
            const EncoderConfig config = speed_config(k_speeds[v]);
            const Recording recording = single(random_bytes(12, uint32_t(80 + v)), config, k_silence_ms);
            sim::ChannelConfig channel;
            channel.mode = modes[m];
            channel.snr_db = k_gate_db[v] + k_gate_margin_db + 6.0;
            channel.seed = uint32_t(90 + v);
            channel.freq_offset_hz = 20.0;
            const Score s = decode(recording, through_channel(recording.samples, channel, config.amplitude),
                                   receiver_for(config), delay)
                                .score;
            if (!CHECK(s.locked_transmissions == 1 && s.lost_bytes == 0 && s.extra_bytes == 0 && s.wrong_bytes <= 1)) {
                NOTE("%s %.0f bytes/s", m == 0 ? "usb" : "lsb", static_cast<double>(k_speeds[v]));
                note_score("sideband", s);
            }
        }
    }
}

// Spec 3.4, 4 L5: sender clocks 1000 ppm fast and slow: 64 bytes without a slip; the measured T follows.
TEST(decoder_clock_error) {
    const double ppms[] = {1000.0, -1000.0};
    const float speeds[] = {1.0f, 6.0f, 25.0f};
    for (size_t v = 0; v < count_of(speeds); ++v) {
        const EncoderConfig config = speed_config(speeds[v]);
        const size_t bytes = speeds[v] < 2.0f ? 24 : 64;
        for (size_t p = 0; p < count_of(ppms); ++p) {
            const Recording recording = single(random_bytes(bytes, uint32_t(110 + p)), config, k_silence_ms);
            sim::ChannelConfig channel;
            channel.mode = sim::Mode::usb;
            channel.snr_db = k_strong_db;
            channel.seed = uint32_t(120 + p);
            channel.clock_ppm = ppms[p];
            Capture capture;
            const Score s = decode(recording, through_channel(recording.samples, channel, config.amplitude),
                                   receiver_for(config), channel_delay_samples(), &capture)
                                .score;
            if (!CHECK(exact(s, 1))) {
                NOTE("%.0f bytes/s at %+.0f ppm", static_cast<double>(speeds[v]), ppms[p]);
                note_score("clock", s);
            }
            // The sender's slot as heard: its clock runs (1 + ppm) fast, so its slots are T / (1 + ppm) here.
            const double heard_ms = config.slot_us / 1000.0 / (1.0 + ppms[p] * 1e-6);
            for (size_t e = 0; e < capture.events.size(); ++e) {
                if (capture.events[e].type != EventType::byte || capture.events[e].byte_index < bytes / 2) continue;
                CHECK_NEAR(capture.events[e].slot_ms / heard_ms, 1.0, 5e-4);
            }
        }
    }
}

// Spec 2.1, V8: a VOX lead (a steady tone, then 2 silent slots) is never taken for data: the lock is on byte 0.
TEST(decoder_vox_lead) {
    const double delay = channel_delay_samples();
    const uint16_t leads_ms[] = {unlimited::k_default_vox_lead_ms, 10, 500};
    for (size_t v = 0; v < count_of(k_speeds); ++v) {
        for (size_t l = 0; l < count_of(leads_ms); ++l) {
            EncoderConfig config = speed_config(k_speeds[v]);
            config.vox_lead_ms = leads_ms[l];
            const Recording recording = single(random_bytes(8, uint32_t(130 + l)), config, k_silence_ms);
            const Score s =
                decode(recording, noisy(recording, config, k_strong_db, uint32_t(140 + v + l)), receiver_for(config),
                       delay)
                    .score;
            if (!CHECK(exact(s, 1))) {
                NOTE("%.0f bytes/s, VOX lead %u ms", static_cast<double>(k_speeds[v]), leads_ms[l]);
                note_score("vox", s);
            }
        }
    }
}

// Spec 2.3, 3.7: transmissions back to back, the next START 2 slots after the last STOP (the shortest tail) or more:
// each ends and the next locks on its byte 0.
TEST(decoder_back_to_back) {
    const double gaps_slots[] = {2.0, 3.5, 7.3, 12.0};
    const double delay = channel_delay_samples();
    for (size_t v = 0; v < count_of(k_speeds); ++v) {
        for (size_t g = 0; g < count_of(gaps_slots); ++g) {
            const EncoderConfig base = speed_config(k_speeds[v]);
            Recording recording;
            append_silence(recording, leading_silence_ms(base, k_silence_ms));
            for (size_t t = 0; t < 3; ++t) {
                EncoderConfig config = base;
                config.tail_ms = 0;  // the 2-slot minimum
                config.lead_in_ms = static_cast<uint16_t>(
                    t == 0 ? 0 : std::lround((gaps_slots[g] - 2.0) * base.slot_us / 1000.0));
                append_transmission(recording, random_bytes(6, uint32_t(150 + 3 * g + t)), config);
            }
            append_silence(recording, 2.0 * k_window_slots * base.slot_us / 1000.0 + k_silence_ms);
            const Score s =
                decode(recording, noisy(recording, base, k_strong_db, uint32_t(160 + g + v)), receiver_for(base), delay)
                    .score;
            if (!CHECK(exact(s, 3))) {
                NOTE("%.0f bytes/s, gap %.1f slots", static_cast<double>(k_speeds[v]), gaps_slots[g]);
                note_score("back to back", s);
            }
        }
    }
}

// Spec 3.6: a window whose START or STOP is missing is dropped (its slot events flagged, no byte) and the counting
// goes on; two in a row with signal present end the transmission with `lost`.
TEST(decoder_framing_error_drops_the_window) {
    const float speeds[] = {1.0f, 6.0f, 25.0f};
    for (size_t v = 0; v < count_of(speeds); ++v) {
        const EncoderConfig config = speed_config(speeds[v]);
        const std::vector<uint8_t> data = random_bytes(16, uint32_t(170 + v));
        Recording recording = single(data, config, k_silence_ms);
        scale_slot(recording, 0, 5, unlimited::k_start_slot, 0.0);  // window 5 without its START
        scale_slot(recording, 0, 9, unlimited::k_stop_slot, 0.0);   // window 9 without its STOP
        Capture capture;
        const Mapping mapping = decode(recording, noisy(recording, config, k_strong_db, uint32_t(180 + v)),
                                       receiver_for(config), channel_delay_samples(), &capture);
        const Score& s = mapping.score;
        if (!CHECK(s.locks == 1 && s.ends == 1 && s.lost_events == 0 && s.extra_bytes == 0 && s.wrong_bytes == 0 &&
                   s.lost_bytes == 2 && s.framing_windows == 2)) {
            NOTE("%.0f bytes/s", static_cast<double>(speeds[v]));
            note_score("framing", s);
            continue;
        }
        CHECK_EQ(mapping.received[0][5], -1);
        CHECK_EQ(mapping.received[0][9], -1);
        size_t flagged = 0;
        for (size_t i = 0; i < capture.events.size(); ++i) {
            const Event& e = capture.events[i];
            if (e.type == EventType::slot && (e.flags & unlimited::event_flag_framing) != 0) {
                CHECK(e.byte_index == 5 || e.byte_index == 9);
                ++flagged;
            }
            if (e.type == EventType::byte) CHECK((e.flags & unlimited::event_flag_framing) == 0);
        }
        CHECK_EQ(flagged, 2 * k_bits_per_byte);
    }
    // Two STARTs missing in a row: `lost` (framing), nothing wrong delivered; the next transmission locks again.
    const EncoderConfig config = speed_config(6.0f);
    Recording recording;
    append_silence(recording, leading_silence_ms(config, k_silence_ms));
    append_transmission(recording, random_bytes(16, 190), config);
    append_silence(recording, 3.0 * k_window_slots * config.slot_us / 1000.0);
    append_transmission(recording, random_bytes(8, 191), config);
    append_silence(recording, k_silence_ms + 2.0 * k_window_slots * config.slot_us / 1000.0);
    scale_slot(recording, 0, 6, unlimited::k_start_slot, 0.0);
    scale_slot(recording, 0, 7, unlimited::k_start_slot, 0.0);
    Capture capture;
    const Mapping mapping =
        decode(recording, noisy(recording, config, k_strong_db, 192), receiver_for(config), channel_delay_samples(),
               &capture);
    const Score& s = mapping.score;
    note_score("two in a row", s);
    CHECK_EQ(s.lost_events, 1u);
    CHECK_EQ(s.wrong_bytes, 0u);
    CHECK_EQ(s.extra_bytes, 0u);
    for (size_t k = 0; k < 6; ++k) CHECK(mapping.received[0][k] >= 0);
    for (size_t k = 0; k < 8; ++k) CHECK(mapping.received[1][k] >= 0);
    bool framing = false;
    for (size_t i = 0; i < capture.events.size(); ++i)
        if (capture.events[i].type == EventType::lost) framing = capture.events[i].reason == LostReason::framing;
    CHECK(framing);
}

// Spec 3.7: the end comes on a silent window; the decoder is back in SEARCH with DCD off.
TEST(decoder_end_detection) {
    for (size_t v = 0; v < count_of(k_speeds); ++v) {
        const EncoderConfig config = speed_config(k_speeds[v]);
        const Recording recording = single(random_bytes(4, uint32_t(200 + v)), config, k_silence_ms);
        const std::vector<int16_t> samples = noisy(recording, config, k_strong_db, uint32_t(210 + v));
        std::vector<Event> events;
        struct Sink {
            static void on_event(const Event& event, void* context) {
                static_cast<std::vector<Event>*>(context)->push_back(event);
            }
        };
        Decoder decoder(receiver_for(config), &Sink::on_event, &events);
        decoder.process(samples.data(), samples.size());
        CHECK(decoder.state() == DecoderState::search);
        CHECK(!decoder.dcd());
        size_t ends = 0;
        size_t bytes = 0;
        bool back_to_search = false;
        for (size_t i = 0; i < events.size(); ++i) {
            if (events[i].type == EventType::byte) ++bytes;
            if (events[i].type == EventType::end) ++ends;
            if (ends > 0 && events[i].type == EventType::state) back_to_search = events[i].state == DecoderState::search;
        }
        CHECK_EQ(bytes, 4u);
        CHECK_EQ(ends, 1u);
        CHECK(back_to_search);
    }
}

// V14: the fixed line at 50, 70 and 90 % of the reference and the adaptive line: on a clean signal all decode; at the
// gate the adaptive line and 70 % are compared (reported); the slot events carry the line used.
TEST(decoder_threshold_settings) {
    const EncoderConfig config = speed_config(6.0f);
    const uint8_t fixed_lines[] = {50, 70, 90};
    const double delay = channel_delay_samples();
    for (size_t f = 0; f <= count_of(fixed_lines); ++f) {
        DecoderConfig receiver = receiver_for(config);
        if (f < count_of(fixed_lines)) {
            receiver.threshold_percent = fixed_lines[f];
        } else {
            receiver.decision_mode = DecisionMode::adaptive;
        }
        const Recording clean = single(random_bytes(16, 220), config, k_silence_ms);
        Capture capture;
        const Score s = decode(clean, noisy(clean, config, k_strong_db, 221), receiver, delay, &capture).score;
        CHECK(exact(s, 1));
        for (size_t i = 0; i < capture.events.size(); ++i) {
            const Event& e = capture.events[i];
            if (e.type != EventType::slot) continue;
            if (f < count_of(fixed_lines)) {
                CHECK(std::abs(int(e.threshold_pct) - int(fixed_lines[f])) <= 1);
            } else {
                CHECK(e.threshold_pct >= 49 && e.threshold_pct <= 76);
            }
        }
        Score gate;
        for (size_t t = 0; t < 6; ++t) {
            const Recording recording = single(random_bytes(16, uint32_t(230 + t)), config, k_silence_ms);
            const Score g = decode(recording, noisy(recording, config, k_gate_db[2], uint32_t(240 + t)), receiver,
                                   delay)
                                .score;
            gate.matched += g.matched;
            gate.bit_errors += g.bit_errors;
            gate.lost_bytes += g.lost_bytes;
            gate.bytes_sent += g.bytes_sent;
            gate.extra_bytes += g.extra_bytes;
        }
        NOTE("%s: at the gate (%.1f dB) BER %.2e, loss %.1f %%",
             f < count_of(fixed_lines) ? (fixed_lines[f] == 50 ? "fixed 50 %" : fixed_lines[f] == 70 ? "fixed 70 %"
                                                                                                      : "fixed 90 %")
                                       : "adaptive",
             k_gate_db[2], gate.ber(), 100.0 * gate.loss());
        CHECK_EQ(gate.extra_bytes, 0u);
    }
}

// Spec 4 S1: 1, 2 and 4 bytes, at every speed, at 20 dB and at gate + 3 dB.
TEST(decoder_short_transmissions) {
    const size_t sizes[] = {1, 2, 4};
    const double delay = channel_delay_samples();
    for (size_t v = 0; v < count_of(k_speeds); ++v) {
        const EncoderConfig config = speed_config(k_speeds[v]);
        size_t strong = 0;
        size_t near_gate = 0;
        size_t runs = 0;
        for (size_t n = 0; n < count_of(sizes); ++n) {
            for (size_t t = 0; t < 3; ++t) {
                const Recording recording = single(random_bytes(sizes[n], uint32_t(250 + 10 * n + t)), config,
                                                   k_silence_ms);
                const Score a = decode(recording, noisy(recording, config, k_strong_db, uint32_t(260 + t)),
                                       receiver_for(config), delay)
                                    .score;
                const Score b = decode(recording,
                                       noisy(recording, config, k_gate_db[v] + k_gate_margin_db, uint32_t(270 + t)),
                                       receiver_for(config), delay)
                                    .score;
                if (exact(a, 1)) ++strong;
                if (b.locked_transmissions == 1 && b.lost_bytes == 0 && b.extra_bytes == 0) ++near_gate;
                CHECK_EQ(b.extra_bytes, 0u);
                ++runs;
            }
        }
        NOTE("%2.0f bytes/s: 1, 2 and 4 bytes decoded %zu/%zu at 20 dB, %zu/%zu at gate + 3 dB",
             static_cast<double>(k_speeds[v]), strong, runs, near_gate, runs);
        CHECK_EQ(strong, runs);
    }
}

// Spec 4 F (short form): noise alone, a steady carrier and keyed CW release nothing.
TEST(decoder_no_lock_on_noise_carrier_or_cw) {
    const EncoderConfig config = speed_config(6.0f);
    const double seconds = 30.0;
    for (size_t kind = 0; kind < 3; ++kind) {
        Recording recording;
        append_silence(recording, seconds * 1000.0);
        sim::ChannelConfig channel;
        channel.mode = sim::Mode::usb;
        channel.snr_db = 10.0;
        channel.seed = uint32_t(280 + kind);
        if (kind == 1) {
            channel.carrier_hz = 1234.0;
            channel.carrier_db = -10.0;
        }
        if (kind == 2) {
            channel.cw_hz = 1500.0;
            channel.cw_db = -10.0;
            channel.cw_wpm = 18.0;
        }
        const Capture capture = run_decoder(through_channel(recording.samples, channel, config.amplitude),
                                            receiver_for(config), k_chunk);
        const char* names[] = {"noise", "carrier", "CW"};
        NOTE("%s, %.0f s: %zu locks, %zu bytes, %zu ACQUIRE entries", names[kind], seconds,
             count_type(capture, EventType::locked), count_type(capture, EventType::byte),
             count_type(capture, EventType::state) / 2);
        CHECK_EQ(count_type(capture, EventType::locked), 0u);
        CHECK_EQ(count_type(capture, EventType::byte), 0u);
    }
}

// V6: a receiver that starts in the middle of a transmission releases nothing of it and decodes the next one from
// byte 0. The start falls anywhere from a slot after the first START to 30 slots before the end (at 1 byte/s, a tone
// at the very start of the history with no clear onset once let the untrusted scan take a later STOP for a START).
TEST(decoder_mid_transmission_start_waits_for_the_next) {
    const float speeds[] = {1.0f, 6.0f, 25.0f};
    const uint32_t seeds[] = {35, 64, 75, 3};
    for (size_t v = 0; v < count_of(speeds); ++v) {
        for (size_t r = 0; r < count_of(seeds); ++r) {
            const EncoderConfig config = speed_config(speeds[v]);
            Recording recording = single(random_bytes(64, 900 + seeds[r]), config, k_silence_ms);
            append_transmission(recording, random_bytes(16, 950 + seeds[r]), config);
            append_silence(recording, 1500.0);
            const std::vector<int16_t> heard = noisy(recording, config, k_strong_db, 1000 + seeds[r]);
            const Transmission& first = recording.transmissions[0];
            std::mt19937 generator(seeds[r]);
            std::uniform_real_distribution<double> at(first_start_sample(first) + slot_of(config),
                                                      end_sample(first) - 30.0 * slot_of(config));
            const size_t cut = static_cast<size_t>(at(generator));
            Recording tail;
            tail.samples.assign(heard.begin() + static_cast<std::ptrdiff_t>(cut), heard.end());
            Transmission next = recording.transmissions[1];
            next.start_sample -= cut;
            tail.transmissions.push_back(next);
            const Score s = decode(tail, tail.samples, receiver_for(config), channel_delay_samples()).score;
            if (!CHECK(exact(s, 1))) {
                NOTE("%.0f bytes/s, seed %u, start %.2f windows into the first transmission",
                     static_cast<double>(speeds[v]), seeds[r],
                     (cut - first_start_sample(first)) / (k_window_slots * slot_of(config)));
                note_score("mid-transmission start", s);
            }
        }
    }
}

// Spec 3.2: transmissions beside an interferer. Six transmissions of 16 bytes, 1.5 s apart, USB 10 dB: beside a steady
// carrier 300 Hz below at -6 dB every one is decoded from byte 0 (the carrier stays masked across each `end`; the
// first transmission, before the carrier is known to be steady, is taken over as it starts); beside keyed CW 250 Hz
// above at -6 dB most are (a transmission starting while the CW is held takes the pitch over), and nothing wrong is
// ever released (a candidate's pitch correction stays within its occupied band).
TEST(decoder_transmissions_beside_a_carrier_or_cw) {
    const size_t transmissions = 6;
    const size_t bytes = 16;
    const double gap_ms = 1500.0;
    const float speeds[] = {1.0f, 6.0f, 12.0f};
    for (size_t v = 0; v < count_of(speeds); ++v) {
        for (size_t kind = 0; kind < 2; ++kind) {
            const EncoderConfig config = speed_config(speeds[v]);
            Recording recording;
            append_silence(recording, gap_ms);
            for (size_t t = 0; t < transmissions; ++t) {
                append_transmission(recording, random_bytes(bytes, uint32_t(330 + 10 * v + t)), config);
                append_silence(recording, gap_ms);
            }
            sim::ChannelConfig channel;
            channel.mode = sim::Mode::usb;
            channel.snr_db = 10.0;
            channel.seed = uint32_t(340 + 2 * v + kind);
            if (kind == 0) {
                channel.carrier_hz = 1200.0;
                channel.carrier_db = -6.0;
            } else {
                channel.cw_hz = 1750.0;
                channel.cw_db = -6.0;
                channel.cw_wpm = 20.0;
            }
            const Score s = decode(recording, through_channel(recording.samples, channel, config.amplitude),
                                   receiver_for(config), channel_delay_samples())
                                .score;
            NOTE("%.0f bytes/s beside %s: %zu of %zu transmissions from byte 0", static_cast<double>(speeds[v]),
                 kind == 0 ? "a carrier" : "CW", s.locked_transmissions, transmissions);
            CHECK_EQ(s.extra_bytes, 0u);
            CHECK_EQ(s.wrong_bytes, 0u);
            CHECK(kind == 0 ? s.locked_transmissions == transmissions : 2 * s.locked_transmissions >= transmissions);
        }
    }
}

// Spec 3.2: pitches 5 Hz inside the edges of the search range (the passband less half the occupied band) are found
// and decoded like any other (L19), in the SSB filters 300-2700 and 200-2900 Hz.
TEST(decoder_pitch_at_search_edges) {
    const float speeds[] = {3.0f, 12.0f};
    const Passband filters[] = {{300, 2700}, {200, 2900}};
    const double inset_hz = 5.0;
    for (size_t v = 0; v < count_of(speeds); ++v) {
        for (size_t f = 0; f < count_of(filters); ++f) {
            DecoderConfig receiver = receiver_for(speed_config(speeds[v]));
            receiver.passband = filters[f];
            const Passband range = receiver.search_range();
            const double pitches[] = {range.low_hz + inset_hz, range.high_hz - inset_hz};
            for (size_t p = 0; p < count_of(pitches); ++p) {
                EncoderConfig config = speed_config(speeds[v], static_cast<uint16_t>(std::lround(pitches[p])));
                config.passband = filters[f];
                REQUIRE(config.valid());
                const Recording recording = single(random_bytes(16, uint32_t(350 + 4 * v + 2 * f + p)), config,
                                                   k_silence_ms);
                sim::ChannelConfig channel;
                channel.mode = sim::Mode::usb;
                channel.snr_db = k_strong_db;
                channel.seed = uint32_t(360 + 4 * v + 2 * f + p);
                channel.rx_low_hz = filters[f].low_hz;
                channel.rx_high_hz = filters[f].high_hz;
                const Score s = decode(recording, through_channel(recording.samples, channel, config.amplitude),
                                       receiver, channel_delay_samples())
                                    .score;
                if (!CHECK(exact(s, 1))) {
                    NOTE("%.0f bytes/s, %u-%u Hz, pitch %.0f Hz", static_cast<double>(speeds[v]), filters[f].low_hz,
                         filters[f].high_hz, pitches[p]);
                    note_score("edge pitch", s);
                }
            }
        }
    }
}

// Spec 3.3: a weak precursor right before the START (an FM receiver's linear-phase filters ring ahead of the signal)
// is not the START: silence is judged against the START's own level.
TEST(decoder_ignores_a_weak_precursor) {
    const float speeds[] = {6.0f, 25.0f};
    for (size_t v = 0; v < count_of(speeds); ++v) {
        const EncoderConfig config = speed_config(speeds[v]);
        Recording recording = single(random_bytes(12, uint32_t(290 + v)), config, k_silence_ms);
        // A tone 21 dB below the signal over the 2 slots before the START.
        const Transmission& tx = recording.transmissions[0];
        const double start = first_start_sample(tx);
        const double slot = slot_of(config);
        const double level = config.amplitude * std::pow(10.0, -21.0 / 20.0);
        for (size_t i = static_cast<size_t>(start - 2.0 * slot); i < static_cast<size_t>(start); ++i) {
            recording.samples[i] = static_cast<int16_t>(
                std::lround(level * std::sin(2.0 * 3.14159265358979 * config.tone_hz * i / 8000.0)));
        }
        const Score s = decode(recording, noisy(recording, config, 22.0, uint32_t(300 + v)), receiver_for(config),
                               channel_delay_samples())
                            .score;
        if (!CHECK(exact(s, 1))) {
            NOTE("%.0f bytes/s", static_cast<double>(speeds[v]));
            note_score("precursor", s);
        }
    }
}

// reset() while tracking reports `lost` (reset) and goes back to SEARCH.
TEST(decoder_reset_while_tracking) {
    const EncoderConfig config = speed_config(6.0f);
    const Recording recording = single(random_bytes(30, 310), config, k_silence_ms);
    std::vector<Event> events;
    struct Sink {
        static void on_event(const Event& event, void* context) {
            static_cast<std::vector<Event>*>(context)->push_back(event);
        }
    };
    Decoder decoder(receiver_for(config), &Sink::on_event, &events);
    const size_t half = recording.samples.size() / 2;
    decoder.process(recording.samples.data(), half);
    REQUIRE(decoder.state() == DecoderState::track);
    CHECK(decoder.dcd());
    CHECK_NEAR(decoder.tone_hz(), 1500.0f, 1.0f);
    CHECK(decoder.snr_db() > 30.0f);
    events.clear();
    decoder.reset();
    REQUIRE(events.size() == 2u);
    CHECK(events[0].type == EventType::lost && events[0].reason == LostReason::reset);
    CHECK(events[1].type == EventType::state && events[1].state == DecoderState::search);
    CHECK(decoder.state() == DecoderState::search);
    CHECK_EQ(decoder.tone_hz(), 0.0f);
    // The rest of the transmission is not decodable from its middle (spec V6): nothing is released.
    events.clear();
    decoder.process(recording.samples.data() + half, recording.samples.size() - half);
    size_t bytes = 0;
    for (size_t i = 0; i < events.size(); ++i) bytes += events[i].type == EventType::byte;
    CHECK_EQ(bytes, 0u);
}

// The impulse blanker off decodes the same clean signal; the DecoderSink-like chunking does not change the events.
TEST(decoder_chunking_and_blanker_do_not_change_clean_decoding) {
    const EncoderConfig config = speed_config(12.0f);
    const Recording recording = single(random_bytes(10, 320), config, k_silence_ms);
    DecoderConfig off = receiver_for(config);
    off.impulse_blanker = false;
    const size_t chunks[] = {1, 37, 64, 4096};
    std::vector<uint8_t> reference;
    for (size_t c = 0; c < count_of(chunks); ++c) {
        for (size_t b = 0; b < 2; ++b) {
            const Capture capture = run_decoder(recording.samples, b == 0 ? receiver_for(config) : off, chunks[c]);
            std::vector<uint8_t> bytes;
            for (size_t i = 0; i < capture.events.size(); ++i)
                if (capture.events[i].type == EventType::byte) bytes.push_back(capture.events[i].value);
            if (reference.empty()) reference = bytes;
            CHECK(bytes == recording.transmissions[0].data);
            CHECK(bytes == reference);
        }
    }
}

// Spec 3.9: the work per 8 kHz block is bounded (no replay bursts). Each block's time is the least of several passes
// over the same audio (preemption only adds), and the worst block is reported.
TEST(decoder_work_per_block_is_bounded) {
    typedef std::chrono::steady_clock Clock;
    const size_t passes = 5;
    for (size_t v = 0; v < count_of(k_speeds); ++v) {
        const EncoderConfig config = speed_config(k_speeds[v]);
        const Recording recording = single(random_bytes(6, uint32_t(330 + v)), config, k_silence_ms);
        const std::vector<int16_t> samples = noisy(recording, config, 10.0, uint32_t(340 + v));
        const size_t block = static_cast<size_t>(block_of(config));
        std::vector<double> least(samples.size() / block, 1e30);
        for (size_t p = 0; p < passes; ++p) {
            Decoder decoder(receiver_for(config), nullptr, nullptr);
            for (size_t b = 0; b < least.size(); ++b) {
                const Clock::time_point t0 = Clock::now();
                decoder.process(samples.data() + b * block, block);
                const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
                least[b] = std::min(least[b], us);
            }
        }
        const double worst = *std::max_element(least.begin(), least.end());
        const double block_us = 1e6 * block / 8000.0;
        NOTE("%2.0f bytes/s: blocks of %zu samples (%.0f us of audio), worst %.2f us of work", static_cast<double>(k_speeds[v]),
             block, block_us, worst);
        CHECK(worst < 0.25 * block_us);  // a PC is ~50-200 times an ESP32: this bound is far from tight
    }
}
