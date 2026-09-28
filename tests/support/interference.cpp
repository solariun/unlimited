#include "interference.hpp"

#include "portable_random.hpp"
#include "unlimited/dsp.hpp"

#include <algorithm>
#include <cmath>

namespace unlimited {
namespace interference {

const int Speech::k_max_harmonics;

namespace {

const double k_rate = static_cast<double>(k_decoder_rate_hz);
const double k_pi = 3.141592653589793;
const double k_two_pi = 2.0 * k_pi;
const double k_edge_s = 0.005;  // 5 ms raised-cosine edges
const double k_db_per_decade = 20.0;

// The carrier: +20 dB over the noise in 2500 Hz, slow drift, a new tone every 5 minutes.
const double k_carrier_db = 20.0;
const double k_carrier_segment_s = 300.0;
const double k_drift_min = 0.02;  // Hz/s
const double k_drift_max = 0.3;
const double k_wobble_hz = 2.0;
const double k_wobble_period_s = 60.0;
const double k_carrier_quiet_s = 1.0;

// Keyed CW.
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

// Speech-shaped bursts.
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

double db_amplitude(double db) {
    return std::pow(10.0, db / k_db_per_decade);
}

double raised(double position) {  // 0..1 -> 0..1
    return 0.5 - 0.5 * std::cos(k_pi * std::max(0.0, std::min(1.0, position)));
}

}  // namespace

Uniform::Uniform(std::uint32_t seed) : generator_(seed) {}

double Uniform::operator()(double low, double high) {
    return sim::UniformReal(low, high)(generator_);
}

int Uniform::integer(int low, int high) {
    return sim::UniformInteger<int>(low, high)(generator_);
}

double NoInterferer::next() {
    return 0.0;
}

DriftingCarrier::DriftingCarrier(std::uint32_t seed, double low_hz, double high_hz)
    : uniform_(seed), low_(low_hz), high_(high_hz), amplitude_(k_reference * db_amplitude(k_carrier_db)) {
    start_segment();
}

double DriftingCarrier::next() {
    const double t = static_cast<double>(sample_) / k_rate;
    ++sample_;
    if (t >= k_carrier_segment_s) {
        start_segment();
        return 0.0;
    }
    if (t < k_carrier_quiet_s) return 0.0;
    const double wobble = k_wobble_hz * std::sin(k_two_pi * t / k_wobble_period_s);
    const double hz = std::max(low_, std::min(high_, base_ + drift_ * t + wobble));
    phase_ = std::fmod(phase_ + k_two_pi * hz / k_rate, k_two_pi);
    const double envelope =
        raised((t - k_carrier_quiet_s) / k_edge_s) * raised((k_carrier_segment_s - t) / k_edge_s);
    return amplitude_ * envelope * std::sin(phase_);
}

void DriftingCarrier::start_segment() {
    sample_ = 0;
    base_ = uniform_(low_, high_);
    drift_ = uniform_(k_drift_min, k_drift_max) * (uniform_(0.0, 1.0) < k_coin ? -1.0 : 1.0);
}

CwTrack::CwTrack(std::uint32_t seed, double low_hz, double high_hz, double gap_min_s, double gap_max_s)
    : uniform_(seed),
      low_(low_hz),
      high_(high_hz),
      gap_min_(gap_min_s),
      gap_max_(gap_max_s),
      edge_samples_(static_cast<int>(k_edge_s * k_rate)) {
    gap_left_ = static_cast<std::size_t>(uniform_(gap_min_, gap_max_) * k_rate);
}

double CwTrack::next() {
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

void CwTrack::start_station() {
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

TwoCwTracks::TwoCwTracks(std::uint32_t seed, double low_hz, double high_hz)
    : first_(seed, low_hz, high_hz, k_cw_gap_min_s, k_cw_gap_max_s),
      second_(seed ^ k_second_track_seed, low_hz, high_hz, k_cw_second_gap_min_s, k_cw_second_gap_max_s) {}

double TwoCwTracks::next() {
    return first_.next() + second_.next();
}

Speech::Speech(std::uint32_t seed) : uniform_(seed) {
    start_pause();
}

double Speech::next() {
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

void Speech::set_length(double seconds) {
    length_ = std::max<std::size_t>(1, static_cast<std::size_t>(seconds * k_rate));
    left_ = length_;
}

void Speech::start_pause() {
    part_ = Part::pause;
    set_length(uniform_(k_pause_min_s, k_pause_max_s));
}

void Speech::start_phrase() {
    syllables_left_ = uniform_.integer(k_phrase_min_syllables, k_phrase_max_syllables);
    syllables_ = syllables_left_;
    phrase_f0_ = uniform_(k_f0_min_hz, k_f0_max_hz);
    level_ = k_reference * db_amplitude(uniform_(k_speech_level_min_db, k_speech_level_max_db));
}

void Speech::start_syllable() {
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

void Speech::advance() {
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

}  // namespace interference
}  // namespace unlimited
