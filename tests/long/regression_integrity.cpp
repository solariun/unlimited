#include "regression.hpp"

#include "test_harness.hpp"

#include <cmath>
#include <memory>
#include <random>

// F1-F4 (spec 4 F): 30 minutes of audio per speed class with no transmission: noise alone, a steady carrier, keyed
// CW and speech-shaped bursts. The audio is generated and decoded in one-second blocks, so memory stays small.
namespace unlimited {
namespace regression {

namespace {

const std::uint32_t k_test_f = 200;
const double k_minutes = 30.0;
const double k_s_per_min = 60.0;
const std::size_t k_block = k_decoder_rate_hz;  // 1 s
const double k_rate = static_cast<double>(k_decoder_rate_hz);
const double k_pi = 3.141592653589793;
const double k_two_pi = 2.0 * k_pi;
const double k_full_scale = 32768.0;
const double k_reference = 0.05;       // key-down reference amplitude the levels are quoted against
const double k_segment_s = 300.0;      // F1/F2: 5-minute segments
const double k_edge_s = 0.005;         // 5 ms raised-cosine edges
const double k_band_margin_hz = 50.0;  // interferer tones stay this far inside the profile's search range
const double k_db_per_decade = 20.0;
const std::size_t k_lock_details = 4;
const std::size_t k_lost_reasons = static_cast<std::size_t>(LostReason::reset) + 1;

// F1: noise level (output gain) and, for fm, carrier SNR in 2500 Hz per segment (the channel's snr_db; the fm CNR in
// its 12.5 kHz IF is 7 dB lower, so segments 0, 1, 3 and 5 are below the FM threshold).
const double k_f1_gains[] = {1.0, 0.03, 0.3, 3.0, 0.1, 1.0};
const double k_f1_rf_snr_db[] = {10.0, 3.0, 20.0, 0.0, 30.0, 6.0};
const std::size_t k_f1_segments = sizeof(k_f1_gains) / sizeof(k_f1_gains[0]);

// F2: steady carrier +20 dB over the noise in 2500 Hz, slow drift.
const double k_f2_carrier_db = 20.0;
const double k_f2_drift_min = 0.02;  // Hz/s
const double k_f2_drift_max = 0.3;
const double k_f2_wobble_hz = 2.0;
const double k_f2_wobble_period_s = 60.0;
const double k_f2_quiet_s = 1.0;

// F3: keyed CW.
const double k_cw_wpm_min = 12.0;
const double k_cw_wpm_max = 30.0;
const double k_cw_level_min_db = -6.0;
const double k_cw_level_max_db = 30.0;
const double k_cw_station_min_s = 20.0;
const double k_cw_station_max_s = 90.0;
const double k_cw_gap_min_s = 0.5;
const double k_cw_gap_max_s = 5.0;
const double k_cw_second_gap_min_s = 20.0;  // a second, sparser station overlaps about a third of the time
const double k_cw_second_gap_max_s = 120.0;
const double k_paris_units_s = 1.2;          // dot length = 1.2 / wpm seconds
const int k_dot_units = 1;
const int k_dash_units = 3;
const int k_element_gap_units = 1;
const int k_letter_gap_units = 3;
const int k_word_gap_units = 7;
const int k_word_min_letters = 2;
const int k_word_max_letters = 6;
const std::uint32_t k_second_track_seed = 0x9E3779B9u;
const double k_coin = 0.5;

// F4: speech-shaped bursts.
const double k_f0_min_hz = 90.0;
const double k_f0_max_hz = 250.0;
const double k_f0_decline = 0.2;  // over a phrase
const double k_accent_min = 0.9;
const double k_accent_max = 1.15;
const double k_syllable_min_s = 0.15;
const double k_syllable_max_s = 0.35;
const double k_syllable_gap_min_s = 0.02;
const double k_syllable_gap_max_s = 0.08;
const double k_fricative_probability = 0.3;
const double k_fricative_min_s = 0.04;
const double k_fricative_max_s = 0.1;
const double k_fricative_gain = 0.3;
const int k_phrase_min_syllables = 4;
const int k_phrase_max_syllables = 14;
const double k_pause_min_s = 0.3;
const double k_pause_max_s = 2.0;
const double k_attack_s = 0.03;
const double k_release_s = 0.06;
const double k_speech_level_min_db = 10.0;
const double k_speech_level_max_db = 25.0;
const double k_speech_top_hz = 3400.0;
const int k_formants = 3;
const double k_formant_low[k_formants] = {300.0, 850.0, 2400.0};
const double k_formant_high[k_formants] = {850.0, 2400.0, 3000.0};
const double k_formant_width[k_formants] = {80.0, 120.0, 160.0};
const double k_formant_gain[k_formants] = {1.0, 0.6, 0.3};
const double k_formant_floor = 0.02;
const double k_tilt_hz = 800.0;
const int k_max_harmonics = 40;

double db_amplitude(double db) {
    return std::pow(10.0, db / k_db_per_decade);
}

double raised(double position) {  // 0..1 -> 0..1
    return 0.5 - 0.5 * std::cos(k_pi * std::max(0.0, std::min(1.0, position)));
}

class Uniform {
public:
    explicit Uniform(std::uint32_t seed) : generator_(seed) {}
    double operator()(double low, double high) {
        return std::uniform_real_distribution<double>(low, high)(generator_);
    }
    int integer(int low, int high) {
        return std::uniform_int_distribution<int>(low, high)(generator_);
    }

private:
    std::mt19937 generator_;
};

class Interferer {
public:
    virtual ~Interferer() {}
    virtual double next() = 0;
};

class NoInterferer : public Interferer {
public:
    double next() override {
        return 0.0;
    }
};

// Steady carrier with slow drift, new frequency every segment, 1 s off between segments.
class DriftingCarrier : public Interferer {
public:
    DriftingCarrier(std::uint32_t seed, double low_hz, double high_hz)
        : uniform_(seed), low_(low_hz), high_(high_hz), amplitude_(k_reference * db_amplitude(k_f2_carrier_db)) {
        start_segment();
    }

    double next() override {
        const double t = static_cast<double>(sample_) / k_rate;
        ++sample_;
        if (t >= k_segment_s) {
            start_segment();
            return 0.0;
        }
        if (t < k_f2_quiet_s) return 0.0;
        const double wobble = k_f2_wobble_hz * std::sin(k_two_pi * t / k_f2_wobble_period_s);
        const double hz = std::max(low_, std::min(high_, base_ + drift_ * t + wobble));
        phase_ = std::fmod(phase_ + k_two_pi * hz / k_rate, k_two_pi);
        const double envelope = raised((t - k_f2_quiet_s) / k_edge_s) * raised((k_segment_s - t) / k_edge_s);
        return amplitude_ * envelope * std::sin(phase_);
    }

private:
    void start_segment() {
        sample_ = 0;
        base_ = uniform_(low_, high_);
        drift_ = uniform_(k_f2_drift_min, k_f2_drift_max) * (uniform_(0.0, 1.0) < k_coin ? -1.0 : 1.0);
    }

    Uniform uniform_;
    double low_;
    double high_;
    double amplitude_;
    double base_ = 0.0;
    double drift_ = 0.0;
    double phase_ = 0.0;
    std::size_t sample_ = 0;
};

// Keyed CW stations one after the other: random text, PARIS timing, 5 ms raised-cosine edges.
class CwTrack : public Interferer {
public:
    CwTrack(std::uint32_t seed, double low_hz, double high_hz, double gap_min_s, double gap_max_s)
        : uniform_(seed), low_(low_hz), high_(high_hz), gap_min_(gap_min_s), gap_max_(gap_max_s) {
        gap_left_ = static_cast<std::size_t>(uniform_(gap_min_, gap_max_) * k_rate);
    }

    double next() override {
        if (gap_left_ > 0) {
            --gap_left_;
            if (gap_left_ == 0) start_station();
            return 0.0;
        }
        const std::size_t unit = sample_ / unit_samples_;
        ++sample_;
        const bool key = unit < keying_.size() && keying_[unit] != 0;
        ramp_ = key ? std::min(ramp_ + 1, edge_samples_) : std::max(ramp_ - 1, 0);
        phase_ = std::fmod(phase_ + k_two_pi * tone_ / k_rate, k_two_pi);
        if (unit >= keying_.size() && ramp_ == 0) {
            gap_left_ = std::max<std::size_t>(1, static_cast<std::size_t>(uniform_(gap_min_, gap_max_) * k_rate));
        }
        return amplitude_ * raised(static_cast<double>(ramp_) / edge_samples_) * std::sin(phase_);
    }

private:
    void start_station() {
        static const char* const k_morse[] = {".-",    "-...",  "-.-.",  "-..",   ".",     "..-.",  "--.",   "....",
                                              "..",    ".---",  "-.-",   ".-..",  "--",    "-.",    "---",   ".--.",
                                              "--.-",  ".-.",   "...",   "-",     "..-",   "...-",  ".--",   "-..-",
                                              "-.--",  "--..",  "-----", ".----", "..---", "...--", "....-", ".....",
                                              "-....", "--...", "---..", "----."};
        const int symbols = static_cast<int>(sizeof(k_morse) / sizeof(k_morse[0]));
        const double wpm = uniform_(k_cw_wpm_min, k_cw_wpm_max);
        unit_samples_ = static_cast<std::size_t>(k_paris_units_s / wpm * k_rate);
        tone_ = uniform_(low_, high_);
        amplitude_ = k_reference * db_amplitude(uniform_(k_cw_level_min_db, k_cw_level_max_db));
        const double seconds = uniform_(k_cw_station_min_s, k_cw_station_max_s);
        const std::size_t units = static_cast<std::size_t>(seconds * k_rate / unit_samples_);
        keying_.clear();
        while (keying_.size() < units) {
            const int letters = uniform_.integer(k_word_min_letters, k_word_max_letters);
            for (int c = 0; c < letters; ++c) {
                const char* code = k_morse[uniform_.integer(0, symbols - 1)];
                for (const char* e = code; *e != 0; ++e) {
                    keying_.insert(keying_.end(), *e == '.' ? k_dot_units : k_dash_units, 1);
                    keying_.insert(keying_.end(), k_element_gap_units, 0);
                }
                keying_.insert(keying_.end(), k_letter_gap_units - k_element_gap_units, 0);
            }
            keying_.insert(keying_.end(), k_word_gap_units - k_letter_gap_units, 0);
        }
        sample_ = 0;
        ramp_ = 0;
    }

    Uniform uniform_;
    double low_;
    double high_;
    double gap_min_;
    double gap_max_;
    std::vector<std::uint8_t> keying_;
    std::size_t unit_samples_ = 1;
    std::size_t sample_ = 0;
    std::size_t gap_left_ = 0;
    int ramp_ = 0;
    const int edge_samples_ = static_cast<int>(k_edge_s * k_rate);
    double tone_ = 0.0;
    double amplitude_ = 0.0;
    double phase_ = 0.0;
};

class TwoCwTracks : public Interferer {
public:
    TwoCwTracks(std::uint32_t seed, double low_hz, double high_hz)
        : first_(seed, low_hz, high_hz, k_cw_gap_min_s, k_cw_gap_max_s),
          second_(seed ^ k_second_track_seed, low_hz, high_hz, k_cw_second_gap_min_s, k_cw_second_gap_max_s) {}

    double next() override {
        return first_.next() + second_.next();
    }

private:
    CwTrack first_;
    CwTrack second_;
};

// Voiced syllables (harmonics of a gliding f0 shaped by three formants) at about 4 per second, optional
// fricatives, phrases separated by pauses.
class Speech : public Interferer {
public:
    explicit Speech(std::uint32_t seed) : uniform_(seed) {
        start_pause();
    }

    double next() override {
        if (left_ == 0) advance();
        --left_;
        const double t = static_cast<double>(length_ - left_) / k_rate;
        const double duration = static_cast<double>(length_) / k_rate;
        switch (part_) {
        case Part::pause:
        case Part::gap:
            return 0.0;
        case Part::fricative: {
            const double white = uniform_(-1.0, 1.0);
            const double sample = white - previous_noise_;
            previous_noise_ = white;
            return level_ * k_fricative_gain * raised(t / k_attack_s) * raised((duration - t) / k_attack_s) * sample;
        }
        case Part::voiced:
            break;
        }
        const double f0 = f0_start_ + (f0_end_ - f0_start_) * t / duration;
        phase_ = std::fmod(phase_ + k_two_pi * f0 / k_rate, k_two_pi);
        double sum = 0.0;
        for (int k = 0; k < harmonics_; ++k) sum += gains_[k] * std::sin((k + 1) * phase_);
        return level_ * raised(t / k_attack_s) * raised((duration - t) / k_release_s) * sum;
    }

private:
    enum class Part { pause, fricative, voiced, gap };

    void set_length(double seconds) {
        length_ = std::max<std::size_t>(1, static_cast<std::size_t>(seconds * k_rate));
        left_ = length_;
    }

    void start_pause() {
        part_ = Part::pause;
        set_length(uniform_(k_pause_min_s, k_pause_max_s));
    }

    void start_phrase() {
        syllables_left_ = uniform_.integer(k_phrase_min_syllables, k_phrase_max_syllables);
        syllables_ = syllables_left_;
        phrase_f0_ = uniform_(k_f0_min_hz, k_f0_max_hz);
        level_ = k_reference * db_amplitude(uniform_(k_speech_level_min_db, k_speech_level_max_db));
    }

    void start_syllable() {
        const double progress = 1.0 - static_cast<double>(syllables_left_) / syllables_;
        const double f0 = phrase_f0_ * (1.0 - k_f0_decline * progress) * uniform_(k_accent_min, k_accent_max);
        f0_start_ = f0;
        f0_end_ = f0 * uniform_(k_accent_min, 1.0);
        double formant[k_formants];
        for (int f = 0; f < k_formants; ++f) formant[f] = uniform_(k_formant_low[f], k_formant_high[f]);
        harmonics_ = std::min(k_max_harmonics, static_cast<int>(k_speech_top_hz / f0));
        double power = 0.0;
        for (int k = 0; k < harmonics_; ++k) {
            const double hz = (k + 1) * f0;
            double shape = k_formant_floor;
            for (int f = 0; f < k_formants; ++f) {
                const double x = (hz - formant[f]) / k_formant_width[f];
                shape += k_formant_gain[f] / (1.0 + x * x);
            }
            gains_[k] = shape / std::sqrt(1.0 + (hz / k_tilt_hz) * (hz / k_tilt_hz));
            power += 0.5 * gains_[k] * gains_[k];
        }
        for (int k = 0; k < harmonics_; ++k) gains_[k] /= std::sqrt(2.0 * power);  // unit key-down-equivalent power
        part_ = Part::voiced;
        set_length(uniform_(k_syllable_min_s, k_syllable_max_s));
        --syllables_left_;
    }

    void advance() {
        if (part_ == Part::voiced) {
            part_ = Part::gap;
            set_length(uniform_(k_syllable_gap_min_s, k_syllable_gap_max_s));
            return;
        }
        if (part_ == Part::fricative) {
            start_syllable();
            return;
        }
        if (part_ == Part::pause) {
            start_phrase();
        } else if (syllables_left_ == 0) {
            start_pause();
            return;
        }
        if (uniform_(0.0, 1.0) < k_fricative_probability) {
            part_ = Part::fricative;
            set_length(uniform_(k_fricative_min_s, k_fricative_max_s));
            return;
        }
        start_syllable();
    }

    Uniform uniform_;
    Part part_ = Part::pause;
    std::size_t length_ = 1;
    std::size_t left_ = 1;
    int syllables_left_ = 0;
    int syllables_ = 1;
    double phrase_f0_ = 0.0;
    double f0_start_ = 0.0;
    double f0_end_ = 0.0;
    double level_ = 0.0;
    double phase_ = 0.0;
    double previous_noise_ = 0.0;
    int harmonics_ = 0;
    double gains_[k_max_harmonics] = {};
};

enum class Scene { noise, carrier, cw, speech };

struct FalseLock {
    std::size_t locks = 0;
    std::size_t bytes = 0;
    std::size_t acquires = 0;       // SEARCH -> ACQUIRE transitions (tone grabs)
    std::size_t lost = 0;
    std::size_t lost_reasons[k_lost_reasons] = {};
    double busy_seconds = 0.0;      // time outside SEARCH (DCD on)
    double seconds = 0.0;
    std::vector<std::string> lock_details;  // the first k_lock_details locks: when, pitch, T, SNR
};

struct EventLog {
    FalseLock* result;
    std::size_t* sample;
    std::size_t busy_from;
    bool busy;
};

void on_event(const Event& event, void* context) {
    EventLog* log = static_cast<EventLog*>(context);
    switch (event.type) {
    case EventType::locked:
        ++log->result->locks;
        if (log->result->lock_details.size() < k_lock_details) {
            log->result->lock_details.push_back(format("%.1f s: %.0f Hz, T %.2f ms, %.1f dB",
                                                       static_cast<double>(*log->sample) / k_rate, event.tone_hz,
                                                       event.slot_ms, event.snr_db));
        }
        break;
    case EventType::byte:
        ++log->result->bytes;
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

// The speed classes of F (spec 4): slow HF, the HF default and FM, each with its receiver.
struct SpeedClass {
    float speed;
    Passband passband;
    sim::Mode noise;  // the receiver noise of F1
};

const SpeedClass k_classes[] = {{1.0f, {k_ssb_passband_low_hz, k_ssb_passband_high_hz}, sim::Mode::usb},
                                {6.0f, {k_ssb_passband_low_hz, k_ssb_passband_high_hz}, sim::Mode::usb},
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
    EventLog log = {&result, &sample, 0, false};
    Decoder decoder(decoder_config, &on_event, &log);

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
        }
    }
    if (log.busy) result.busy_seconds += static_cast<double>(sample - log.busy_from) / k_rate;
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
    return format("%zu tone grabs (ACQUIRE), %zu lost; %.1f s outside SEARCH (DCD on)", r.acquires, r.lost,
                  r.busy_seconds);
}

void run_f(Scene scene, const char* id, const char* what) {
    const std::vector<FalseLock>& results = all_scenes();
    for (std::size_t i = 0; i < k_class_count; ++i) {
        const FalseLock& r = results[static_cast<std::size_t>(scene) * k_class_count + i];
        result(id, format("%s, %g min, %s", what, k_minutes, class_name(k_classes[i]).c_str()),
               format("locked %zu, bytes %zu; ", r.locks, r.bytes) + activity_text(r) + locks_text(r),
               "0 bytes released", r.bytes == 0);
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
