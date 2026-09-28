#include "regression.hpp"

#include "support/interference.hpp"
#include "test_harness.hpp"
#include "unlimited/kiss.hpp"
#include "unlimited/modem.hpp"

#include <cmath>
#include <memory>

// F1-F4 (spec 4 F): 30 minutes of audio per speed class with no transmission: noise alone, a steady carrier, keyed
// CW and speech-shaped bursts; the stray bytes they give per hour are reported (V20: the upper protocol's business),
// and what of them reaches a computer through the KISS modem's core with its defaults (V23: receptions shorter than
// 15 bytes are dropped). The audio is generated and decoded in one-second blocks, so memory stays small.
namespace unlimited {
namespace regression {

namespace {

using interference::CwTrack;
using interference::DriftingCarrier;
using interference::Interferer;
using interference::NoInterferer;
using interference::Speech;
using interference::TwoCwTracks;
using interference::k_reference;

const std::uint32_t k_test_f = 200;
const double k_minutes = 30.0;
const double k_s_per_min = 60.0;
const double k_s_per_hour = 3600.0;
const std::size_t k_block = k_decoder_rate_hz;  // 1 s
const double k_rate = static_cast<double>(k_decoder_rate_hz);
const double k_full_scale = 32768.0;
const double k_segment_s = 300.0;      // F1: 5-minute segments
const double k_band_margin_hz = 50.0;  // interferer tones stay this far inside the profile's search range
const std::size_t k_lock_details = 4;
const std::size_t k_lost_reasons = static_cast<std::size_t>(LostReason::reset) + 1;

// F1: noise level (output gain) and, for fm, carrier SNR in 2500 Hz per segment (the channel's snr_db; the fm CNR in
// its 12.5 kHz IF is 7 dB lower, so segments 0, 1, 3 and 5 are below the FM threshold).
const double k_f1_gains[] = {1.0, 0.03, 0.3, 3.0, 0.1, 1.0};
const double k_f1_rf_snr_db[] = {10.0, 3.0, 20.0, 0.0, 30.0, 6.0};
const std::size_t k_f1_segments = sizeof(k_f1_gains) / sizeof(k_f1_gains[0]);

enum class Scene { noise, carrier, cw, speech };

struct FalseLock {
    std::size_t locks = 0;
    std::size_t bytes = 0;
    std::size_t acquires = 0;       // SEARCH -> ACQUIRE transitions (tone grabs)
    std::size_t lost = 0;
    std::size_t lost_reasons[k_lost_reasons] = {};
    double busy_seconds = 0.0;      // time outside SEARCH (DCD's meaning before 2026-09-28)
    double dcd_seconds = 0.0;       // time with Decoder::dcd() on: a transmission being decoded (spec 3.10)
    double seconds = 0.0;
    std::vector<std::string> lock_details;  // the first k_lock_details locks: when, pitch, T, SNR
    std::size_t longest = 0;        // bytes of the longest stray reception (lock to end or lost)
    std::size_t modem_frames = 0;   // KISS frames the modem's core handed its computer (V23: at least 15 bytes each)
    std::size_t modem_bytes = 0;
    std::size_t modem_short = 0;    // receptions it dropped as shorter than the minimum frame
};

// The KISS the modem's core hands its computer: data frames and their bytes.
struct HostCount {
    KissDecoder kiss;
    std::size_t frames = 0;
    std::size_t bytes = 0;
    static void on_host(const std::uint8_t* data, std::size_t size, void* context) {
        HostCount& count = *static_cast<HostCount*>(context);
        for (std::size_t i = 0; i < size; ++i) {
            std::uint8_t value = 0;
            const KissStep step = count.kiss.feed(data[i], value);
            if (step == KissStep::data) ++count.bytes;
            if (step == KissStep::end) ++count.frames;
        }
    }
};

struct EventLog {
    FalseLock* result;
    std::size_t* sample;
    std::size_t busy_from;
    bool busy;
    std::size_t reception;  // bytes of the reception open now
};

void on_event(const Event& event, void* context) {
    EventLog* log = static_cast<EventLog*>(context);
    switch (event.type) {
    case EventType::locked:
        ++log->result->locks;
        log->reception = 0;
        if (log->result->lock_details.size() < k_lock_details) {
            log->result->lock_details.push_back(format("%.1f s: %.0f Hz, T %.2f ms, %.1f dB",
                                                       static_cast<double>(*log->sample) / k_rate, event.tone_hz,
                                                       event.slot_ms, event.snr_db));
        }
        break;
    case EventType::byte:
        ++log->result->bytes;
        log->result->longest = std::max(log->result->longest, ++log->reception);
        break;
    case EventType::lost:
        ++log->result->lost;
        ++log->result->lost_reasons[static_cast<std::size_t>(event.reason)];
        break;
    case EventType::state:
        if (event.state == DecoderState::acquire && !log->busy) ++log->result->acquires;
        if (event.state == DecoderState::search && log->busy) {
            log->result->busy_seconds += static_cast<double>(*log->sample - log->busy_from) / k_rate;
            log->busy = false;
        } else if (event.state != DecoderState::search && !log->busy) {
            log->busy_from = *log->sample;
            log->busy = true;
        }
        break;
    case EventType::slot:
    case EventType::end:
        break;
    }
}

// The speed classes of F (spec 4): slow HF, the HF default, fast HF (12 bytes/s: T = 8.3 ms, the glottal period of a
// 120 Hz voice, speech's worst case) and FM, each with its receiver.
struct SpeedClass {
    float speed;
    Passband passband;
    sim::Mode noise;  // the receiver noise of F1
};

const SpeedClass k_classes[] = {{1.0f, {k_ssb_passband_low_hz, k_ssb_passband_high_hz}, sim::Mode::usb},
                                {6.0f, {k_ssb_passband_low_hz, k_ssb_passband_high_hz}, sim::Mode::usb},
                                {12.0f, {k_ssb_passband_low_hz, k_ssb_passband_high_hz}, sim::Mode::usb},
                                {25.0f, {k_fm_passband_low_hz, k_fm_passband_high_hz}, sim::Mode::fm}};
const std::size_t k_class_count = sizeof(k_classes) / sizeof(k_classes[0]);

DecoderConfig class_receiver(const SpeedClass& c) {
    DecoderConfig receiver;
    receiver.slot_us = slot_us_for_speed(c.speed);
    receiver.passband = c.passband;
    return receiver;
}

std::string class_name(const SpeedClass& c) {
    return format("%s in %u-%u Hz", speed_text(c.speed).c_str(), c.passband.low_hz, c.passband.high_hz);
}

FalseLock run_scene(Scene scene, const SpeedClass& speed_class, std::uint32_t seed) {
    const DecoderConfig decoder_config = class_receiver(speed_class);
    const Passband range = decoder_config.search_range();
    const double low = range.low_hz + k_band_margin_hz;
    const double high = range.high_hz - k_band_margin_hz;
    NoInterferer none;
    DriftingCarrier carrier(seed, low, high);
    TwoCwTracks cw(seed, low, high);
    Speech speech(seed);
    Interferer* interferer = &none;
    if (scene == Scene::carrier) interferer = &carrier;
    if (scene == Scene::cw) interferer = &cw;
    if (scene == Scene::speech) interferer = &speech;

    FalseLock result;
    std::size_t sample = 0;
    std::size_t dcd_samples = 0;
    EventLog log = {&result, &sample, 0, false, 0};
    Decoder decoder(decoder_config, &on_event, &log);
    // The same audio through the KISS modem's core with its defaults (the minimum frame of V23), speed and passband as
    // the class: what reaches the computer.
    ModemConfig modem_config;
    modem_config.signal.slot_us = decoder_config.slot_us;
    modem_config.signal.passband = decoder_config.passband;
    modem_config.receiver = decoder_config;
    HostCount host;
    Modem modem(modem_config, &HostCount::on_host, nullptr, &host);
    REQUIRE(modem.valid());

    const std::size_t total = static_cast<std::size_t>(k_minutes * k_s_per_min * k_rate);
    const std::size_t per_segment = static_cast<std::size_t>(k_segment_s * k_rate);
    const sim::Mode mode = scene == Scene::noise ? speed_class.noise : sim::Mode::usb;
    std::size_t segment = k_f1_segments;
    sim::ChannelConfig channel_config;
    std::unique_ptr<sim::Channel> channel;
    std::vector<float> zeros(k_block, 0.0f);
    std::vector<float> noise(k_block);
    std::vector<std::int16_t> block(k_block);
    double gain = 1.0;
    for (std::size_t first = 0; first < total; first += k_block) {
        const std::size_t current = scene == Scene::noise ? (first / per_segment) % k_f1_segments : 0;
        if (current != segment) {
            segment = current;
            channel_config = sim::ChannelConfig();
            channel_config.mode = mode;
            channel_config.signal_level = k_reference;
            channel_config.snr_db = mode == sim::Mode::usb ? 0.0 : k_f1_rf_snr_db[segment];
            channel_config.seed = seed + static_cast<std::uint32_t>(segment);
            if (mode == sim::Mode::fm) channel_config.fm_audio_high_hz = speed_class.passband.high_hz;
            channel.reset(new sim::Channel(channel_config));
            gain = scene == Scene::noise ? k_f1_gains[segment] : 1.0;
        }
        const std::size_t count = std::min(k_block, total - first);
        channel->process(zeros.data(), noise.data(), count);
        for (std::size_t i = 0; i < count; ++i) {
            const double value = std::round((noise[i] + interferer->next()) * gain * k_full_scale);
            block[i] = static_cast<std::int16_t>(std::max(-k_full_scale, std::min(k_full_scale - 1.0, value)));
        }
        for (std::size_t i = 0; i < count; ++i) {
            sample = first + i + 1;
            decoder.process_sample(block[i]);
            if (decoder.dcd()) ++dcd_samples;
        }
        modem.audio_input(block.data(), count);
    }
    result.modem_frames = host.frames;
    result.modem_bytes = host.bytes;
    result.modem_short = modem.counters().short_frames;
    if (log.busy) result.busy_seconds += static_cast<double>(sample - log.busy_from) / k_rate;
    result.dcd_seconds = static_cast<double>(dcd_samples) / k_rate;
    result.seconds = static_cast<double>(total) / k_rate;
    return result;
}

const std::size_t k_scene_count = 4;  // Scene values
const std::size_t k_main_runs = k_scene_count * k_class_count;

// All scenes and speed classes run once, in parallel, on the first F test; the others read the results.
const std::vector<FalseLock>& all_scenes() {
    static const std::vector<FalseLock> results = parallel_map<FalseLock>(k_main_runs, [](std::size_t i) {
        const Scene scene = static_cast<Scene>(i / k_class_count);
        const std::size_t speed_class = i % k_class_count;
        const std::uint32_t seed =
            seed_of(k_test_f + static_cast<std::uint32_t>(scene), static_cast<std::uint32_t>(speed_class), 0);
        return run_scene(scene, k_classes[speed_class], seed);
    });
    return results;
}

std::string locks_text(const FalseLock& r) {
    std::string text;
    for (std::size_t i = 0; i < r.lock_details.size(); ++i) text += (i == 0 ? "; locks at " : ", ") + r.lock_details[i];
    return text;
}

std::string activity_text(const FalseLock& r) {
    return format("%zu tone grabs (ACQUIRE), %zu lost; DCD on %.1f s (a transmission being decoded), %.1f s outside "
                  "SEARCH",
                  r.acquires, r.lost, r.dcd_seconds, r.busy_seconds);
}

// Pure one byte, one decision (V20): bytes a scene without a transmission makes readable are stray bytes for the upper
// protocol; the rows report them per hour (and the locks, the DCD time) and gate nothing.
void run_f(Scene scene, const char* id, const char* what) {
    const std::vector<FalseLock>& results = all_scenes();
    for (std::size_t i = 0; i < k_class_count; ++i) {
        const FalseLock& r = results[static_cast<std::size_t>(scene) * k_class_count + i];
        const double hours = r.seconds / k_s_per_hour;
        result(id, format("%s, %g min, %s", what, k_minutes, class_name(k_classes[i]).c_str()),
               format("stray bytes %.0f per hour (%zu in %g min), locks %.0f per hour, the longest %zu bytes; to the "
                      "computer through the modem (--min-frame %u): %.0f frames, %.0f bytes per hour (receptions "
                      "dropped: %zu); ",
                      r.bytes / hours, r.bytes, k_minutes, r.locks / hours, r.longest,
                      static_cast<unsigned>(k_default_min_frame_bytes), r.modem_frames / hours, r.modem_bytes / hours,
                      r.modem_short) +
                   activity_text(r) + locks_text(r),
               "report: stray bytes go to the upper protocol (V20)", true, Kind::report);
    }
}

}  // namespace

void test_f1_noise() {
    run_f(Scene::noise, "F1", "receiver noise only (HF: usb noise at 6 levels; FM: an unmodulated carrier, CNR -7..23 dB)");
}

void test_f2_carrier() {
    run_f(Scene::carrier, "F2", "noise + steady carrier +20 dB, drift 0.02-0.3 Hz/s, new tone every 5 min");
}

void test_f3_cw() {
    run_f(Scene::cw, "F3", "noise + keyed CW 12-30 WPM, random tones, -6..+30 dB, up to 2 stations");
}

void test_f4_speech() {
    run_f(Scene::speech, "F4", "noise + speech-shaped bursts (~4 syllables/s, f0 90-250 Hz, 3 formants), +10..+25 dB");
}

}  // namespace regression
}  // namespace unlimited
