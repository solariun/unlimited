#include "regression.hpp"

#include "test_harness.hpp"
#include "unlimited/packet.hpp"

#include <cmath>
#include <memory>
#include <random>

// F1-F4 and F7 (spec 8.5): 30 minutes of audio per profile with no transmission. The audio is generated and
// decoded in one-second blocks, so memory stays small.
namespace unlimited {
namespace regression {

namespace {

const std::uint32_t k_test_f = 200;
const double k_minutes = 30.0;
const double k_s_per_min = 60.0;
const double k_s_per_hour = 3600.0;
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

// F1: noise level (output gain) and, for am/fm, carrier SNR in 2500 Hz per segment (the channel's snr_db;
// the fm CNR in its 12.5 kHz IF is 7 dB lower, so segments 0, 1, 3 and 5 are below the FM threshold).
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
    std::size_t preambles = 0;      // sync trains accepted (ACQUIRE -> PREAMBLE)
    std::size_t confirmations = 0;  // PREAMBLE -> TRACK: N confirmed by the package learning (F7)
    std::size_t late_tracks = 0;    // ACQUIRE -> TRACK: a relock or a cold join started
    std::size_t packages = 0;       // `package` events (telemetry of decided packages, before the guard)
    std::size_t lost = 0;
    std::size_t lost_reasons[k_lost_reasons] = {};
    double busy_seconds = 0.0;      // time outside SEARCH
    double seconds = 0.0;
    std::size_t packets = 0;        // CRC-valid packets delivered (nothing was sent: all wrong)
    std::vector<std::string> lock_details;  // the first k_lock_details locks: when, T, N, pitch, SNR
};

struct EventLog {
    FalseLock* result;
    PacketReader* reader;
    std::size_t* sample;
    std::size_t busy_from;
    bool busy;
    DecoderState previous;
};

void on_event(const Event& event, void* context) {
    EventLog* log = static_cast<EventLog*>(context);
    log->reader->on_event(event);
    switch (event.type) {
    case EventType::locked:
        ++log->result->locks;
        if (log->result->lock_details.size() < k_lock_details) {
            log->result->lock_details.push_back(format("%.1f s: T %.2f ms N %u at %.0f Hz, %.1f dB",
                                                       static_cast<double>(*log->sample) / k_rate, event.slot_ms,
                                                       event.bits_per_package, event.tone_hz, event.snr_db));
        }
        break;
    case EventType::byte:
        ++log->result->bytes;
        break;
    case EventType::package:
        ++log->result->packages;
        break;
    case EventType::lost:
        ++log->result->lost;
        ++log->result->lost_reasons[static_cast<std::size_t>(event.reason)];
        break;
    case EventType::state:
        if (event.state == DecoderState::acquire) ++log->result->acquires;
        if (event.state == DecoderState::preamble) ++log->result->preambles;
        if (event.state == DecoderState::track && log->previous == DecoderState::preamble) {
            ++log->result->confirmations;
        }
        if (event.state == DecoderState::track && log->previous == DecoderState::acquire) ++log->result->late_tracks;
        if (event.state == DecoderState::search && log->busy) {
            log->result->busy_seconds += static_cast<double>(*log->sample - log->busy_from) / k_rate;
            log->busy = false;
        } else if (event.state != DecoderState::search && !log->busy) {
            log->busy_from = *log->sample;
            log->busy = true;
        }
        log->previous = event.state;
        break;
    case EventType::slot:
    case EventType::end:
        break;
    }
}

void on_packet(const std::uint8_t* payload, std::uint16_t size, std::uint8_t flags, void* context) {
    (void)payload;
    (void)size;
    (void)flags;
    ++static_cast<FalseLock*>(context)->packets;
}

sim::Mode noise_mode(Scene scene, Profile profile) {
    if (scene != Scene::noise) return sim::Mode::usb;
    if (profile == Profile::am) return sim::Mode::am;
    if (profile == Profile::fm) return sim::Mode::fm;
    return sim::Mode::usb;
}

FalseLock run_scene(Scene scene, Profile profile, std::uint32_t seed) {
    const DecoderConfig decoder_config = DecoderConfig::for_profile(profile);
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
    PacketReader reader(&on_packet, &result);
    std::size_t sample = 0;
    EventLog log = {&result, &reader, &sample, 0, false, DecoderState::search};
    Decoder decoder(decoder_config, &on_event, &log);

    const std::size_t total = static_cast<std::size_t>(k_minutes * k_s_per_min * k_rate);
    const std::size_t per_segment = static_cast<std::size_t>(k_segment_s * k_rate);
    const sim::Mode mode = noise_mode(scene, profile);
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

const Profile k_profiles[] = {Profile::ssb, Profile::am, Profile::fm};
const std::size_t k_profile_count = sizeof(k_profiles) / sizeof(k_profiles[0]);
const std::size_t k_scene_count = 4;  // Scene values
// F3/F4 also run on 5 more seeds (report only, spec 8.5): one 30-minute run per profile is a thin sample of a rate.
const std::size_t k_extra_seeds = 5;
const Scene k_rate_scenes[] = {Scene::cw, Scene::speech};
const std::size_t k_rate_scene_count = sizeof(k_rate_scenes) / sizeof(k_rate_scenes[0]);
const std::size_t k_main_runs = k_scene_count * k_profile_count;
const char* const k_scene_names[k_scene_count] = {"F1 noise", "F2 carrier", "F3 keyed CW", "F4 speech"};

// All scenes, profiles and extra seeds run once, in parallel, on the first F test; the others read the
// results. Runs [0, k_main_runs) are the gated ones (seed 0), the rest the extra seeds of k_rate_scenes.
const std::vector<FalseLock>& all_scenes() {
    static const std::vector<FalseLock> results = parallel_map<FalseLock>(
        k_main_runs + k_rate_scene_count * k_profile_count * k_extra_seeds, [](std::size_t i) {
            Scene scene = static_cast<Scene>(i / k_profile_count);
            std::size_t profile = i % k_profile_count;
            std::uint32_t job = 0;
            if (i >= k_main_runs) {
                const std::size_t extra = i - k_main_runs;
                scene = k_rate_scenes[extra / (k_profile_count * k_extra_seeds)];
                profile = (extra / k_extra_seeds) % k_profile_count;
                job = static_cast<std::uint32_t>(1 + extra % k_extra_seeds);
            }
            const std::uint32_t seed =
                seed_of(k_test_f + static_cast<std::uint32_t>(scene), static_cast<std::uint32_t>(profile), job);
            return run_scene(scene, k_profiles[profile], seed);
        });
    return results;
}

void add(FalseLock& into, const FalseLock& from) {
    into.locks += from.locks;
    into.bytes += from.bytes;
    into.acquires += from.acquires;
    into.preambles += from.preambles;
    into.confirmations += from.confirmations;
    into.late_tracks += from.late_tracks;
    into.packages += from.packages;
    into.lost += from.lost;
    for (std::size_t r = 0; r < k_lost_reasons; ++r) into.lost_reasons[r] += from.lost_reasons[r];
    into.busy_seconds += from.busy_seconds;
    into.seconds += from.seconds;
    into.packets += from.packets;
    for (std::size_t i = 0; i < from.lock_details.size() && into.lock_details.size() < k_lock_details; ++i) {
        into.lock_details.push_back(from.lock_details[i]);
    }
}

std::string locks_text(const FalseLock& r) {
    std::string text;
    for (std::size_t i = 0; i < r.lock_details.size(); ++i) text += (i == 0 ? "; locks at " : ", ") + r.lock_details[i];
    return text;
}

std::string learning_text(const FalseLock& r) {
    const std::size_t* reason = r.lost_reasons;
    return format("%zu tone grabs, %zu sync trains (PREAMBLE), %zu N confirmations, %zu late-join TRACKs, %zu package "
                  "events; lost: gone %zu, alias %zu, preamble %zu, unsupported %zu; %.1f s outside SEARCH",
                  r.acquires, r.preambles, r.confirmations, r.late_tracks, r.packages,
                  reason[static_cast<std::size_t>(LostReason::signal_gone)],
                  reason[static_cast<std::size_t>(LostReason::alias)],
                  reason[static_cast<std::size_t>(LostReason::preamble_timeout)],
                  reason[static_cast<std::size_t>(LostReason::unsupported)], r.busy_seconds);
}

Outcome packet_outcome(const FalseLock& r) {
    Outcome outcome;
    outcome.packets_bad = r.packets;
    return outcome;
}

void run_f(Scene scene, const char* id, const char* what, bool bytes_gate) {
    const std::vector<FalseLock>& results = all_scenes();
    for (std::size_t i = 0; i < k_profile_count; ++i) {
        const FalseLock& r = results[static_cast<std::size_t>(scene) * k_profile_count + i];
        const bool pass = r.locks == 0 && (!bytes_gate || r.bytes == 0);
        result(id, format("%s, %g min, %s profile", what, k_minutes, profile_name(k_profiles[i]).c_str()),
               format("locked %zu, bytes %zu, packets %zu; ", r.locks, r.bytes, r.packets) + learning_text(r) +
                   locks_text(r),
               bytes_gate ? "0 locked, 0 bytes" : "0 locked", pass);
        ledger_packets(format("%s %s", id, profile_name(k_profiles[i]).c_str()), packet_outcome(r));
    }
    for (std::size_t k = 0; k < k_rate_scene_count; ++k) {
        if (k_rate_scenes[k] != scene) continue;
        for (std::size_t i = 0; i < k_profile_count; ++i) {
            FalseLock total;
            for (std::size_t j = 0; j < k_extra_seeds; ++j) {
                const FalseLock& r = results[k_main_runs + (k * k_profile_count + i) * k_extra_seeds + j];
                add(total, r);
                ledger_packets(format("%s %s seed %zu", id, profile_name(k_profiles[i]).c_str(), j + 1),
                               packet_outcome(r));
            }
            result(id, format("%s, %zu more seeds x %g min, %s profile", what, k_extra_seeds, k_minutes,
                              profile_name(k_profiles[i]).c_str()),
                   format("locked %zu, bytes %zu, packets %zu; ", total.locks, total.bytes, total.packets) +
                       learning_text(total) + locks_text(total),
                   "report (rate)", true, Kind::report);
        }
    }
}

}  // namespace

void test_f1_noise() {
    run_f(Scene::noise, "F1",
          "receiver noise only (ssb: usb noise; am/fm: unmodulated carrier, SNR 0-30 dB in 2500 Hz, fm CNR -7..23 dB), "
          "6 levels",
          true);
}

void test_f2_carrier() {
    run_f(Scene::carrier, "F2", "noise + steady carrier +20 dB, drift 0.02-0.3 Hz/s, new tone every 5 min", false);
}

void test_f3_cw() {
    run_f(Scene::cw, "F3", "noise + keyed CW 12-30 WPM, random tones, -6..+30 dB, up to 2 stations", false);
}

void test_f4_speech() {
    run_f(Scene::speech, "F4", "noise + speech-shaped bursts (~4 syllables/s, f0 90-250 Hz, 3 formants), +10..+25 dB",
          false);
}

// F7: package learning on non-signals. The learner's candidates are internal; what the events show is each sync
// train accepted (PREAMBLE entries, the upper bound of candidates formed) and each N confirmed (PREAMBLE -> TRACK).
// Per hour, every seed; a confirmation must still fail the guard: 0 `locked` in the gated runs.
void test_f7_package_learning() {
    const std::vector<FalseLock>& results = all_scenes();
    FalseLock gated;
    for (std::size_t scene = 0; scene < k_scene_count; ++scene) {
        for (std::size_t i = 0; i < k_profile_count; ++i) {
            const FalseLock& main = results[scene * k_profile_count + i];
            add(gated, main);
            FalseLock total = main;
            std::size_t runs = 1;
            for (std::size_t k = 0; k < k_rate_scene_count; ++k) {
                if (static_cast<std::size_t>(k_rate_scenes[k]) != scene) continue;
                for (std::size_t j = 0; j < k_extra_seeds; ++j) {
                    add(total, results[k_main_runs + (k * k_profile_count + i) * k_extra_seeds + j]);
                    ++runs;
                }
            }
            const double hours = total.seconds / k_s_per_hour;
            result("F7", format("%s, %zu x %g min, %s profile", k_scene_names[scene], runs, k_minutes,
                                profile_name(k_profiles[i]).c_str()),
                   format("sync trains %.1f/h, N confirmations %.2f/h (%zu), late-join TRACKs %.2f/h; locked %zu, "
                          "bytes %zu",
                          total.preambles / hours, total.confirmations / hours, total.confirmations,
                          total.late_tracks / hours, total.locks, total.bytes),
                   "report (candidates are internal: sync trains bound them)", true, Kind::report);
        }
    }
    result("F7", "every confirmation of the gated F1-F4 runs (12 x 30 min) fails the guard",
           format("%zu N confirmations, %zu late-join TRACKs -> %zu locked, %zu bytes", gated.confirmations,
                  gated.late_tracks, gated.locks, gated.bytes),
           "0 locked", gated.locks == 0);
}

}  // namespace regression
}  // namespace unlimited
