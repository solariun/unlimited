#include "channel.hpp"

#include "resampler.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <random>
#include <stdexcept>
#include <string>

namespace unlimited {
namespace sim {

using std::size_t;
using std::uint32_t;

namespace {

typedef std::complex<double> Complex;

// ===========================================================================
// Constants
// ===========================================================================

const double k_pi = 3.14159265358979323846;
const double k_two_pi = 2.0 * k_pi;
const double k_ms_per_s = 1e3;
const double k_us_per_s = 1e6;

const double k_reference_bandwidth_hz = 2500.0;  // SNR reference bandwidth
const double k_carrier_amplitude = 1.0;          // am/fm unmodulated carrier envelope
const double k_min_rf_rate_hz = 48000.0;         // am/fm run at the first integer multiple of the audio rate >= this

// Blackman window: a windowed-sinc FIR of length N has a transition band of
// about 5.5 * rate / N and ~74 dB of stopband attenuation.
const double k_blackman_a0 = 0.42;
const double k_blackman_a1 = 0.5;
const double k_blackman_a2 = 0.08;
const double k_blackman_transition_factor = 5.5;

const double k_hilbert_transition_hz = 100.0;        // analytic signal is accurate above ~100 Hz
const double k_audio_transition_hz = 200.0;          // receiver audio passband skirts
const double k_if_transition_fraction = 0.15;        // IF filter skirts, fraction of the IF bandwidth
const double k_resampler_cutoff_fraction = 0.45;     // -6 dB point, fraction of the audio rate
const double k_resampler_transition_fraction = 0.1;  // flat to 0.4 * rate, stopband from 0.5 * rate

const double k_am_dc_block_hz = 20.0;
const double k_am_carrier_smoothing_hz = 20.0;   // AM AGC carrier detector, far below the audio band
const double k_emphasis_reference_hz = 1000.0;   // pre-emphasis has unity gain here
const double k_emphasis_shelf_hz = 12000.0;      // pre-emphasis stops rising here, far above the audio band
const double k_splatter_transition_hz = 1000.0;  // FM splatter filter: flat to fm_audio_high_hz, stop 1 kHz above

const double k_fading_updates_per_spread = 32.0;  // tap update rate relative to the Doppler spread
const double k_fading_span_sigmas = 4.0;          // Gaussian shaping filter half-length in sigmas
const double k_sigmas_per_spread = 2.0;           // Doppler spread is defined as 2 sigma
const double k_complex_component_variance = 0.5;  // unit-power complex Gaussian

const double k_impulse_reference_rate_hz = 48000.0;  // an impulse has the area of one sample at this rate
const double k_impulse_duration_s = 1.0 / k_impulse_reference_rate_hz;
const double k_agc_max_gain_db = 40.0;

const double k_ppm = 1e-6;
const double k_max_clock_ppm = 1e5;  // 10 %

// Morse, PARIS timing: a dot lasts 1.2 / wpm seconds; element and gap lengths in dots.
const double k_paris_dot_s = 1.2;
const double k_dot_units = 1.0;
const double k_dash_units = 3.0;
const double k_symbol_gap_units = 1.0;
const double k_character_gap_units = 3.0;
const double k_word_gap_units = 7.0;
const int k_max_word_characters = 6;
const double k_cw_edge_s = 0.005;  // raised-cosine key edges
const char* const k_morse_codes[] = {".-",    "-...",  "-.-.",  "-..",   ".",     "..-.",  "--.",   "....",  "..",
                                     ".---",  "-.-",   ".-..",  "--",    "-.",    "---",   ".--.",  "--.-",  ".-.",
                                     "...",   "-",     "..-",   "...-",  ".--",   "-..-",  "-.--",  "--..",  "-----",
                                     ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};
const size_t k_morse_code_count = sizeof(k_morse_codes) / sizeof(k_morse_codes[0]);

struct FadingProfile {
    FadingPreset preset;
    double path_delay_ms;
    double doppler_spread_hz;
};

const FadingProfile k_fading_profiles[] = {{FadingPreset::flat, 0.0, 1.0},
                                           {FadingPreset::ccir_good, 0.5, 0.1},
                                           {FadingPreset::ccir_moderate, 1.0, 0.5},
                                           {FadingPreset::ccir_poor, 2.0, 1.0},
                                           {FadingPreset::flutter, 0.5, 10.0}};

enum RandomStream : uint32_t { noise_stream = 1, impulse_stream, fading_path1_stream, fading_path2_stream, cw_stream };

double db_to_power(double db) {
    return std::pow(10.0, db / 10.0);
}

double db_to_amplitude(double db) {
    return std::pow(10.0, db / 20.0);
}

double wrap_phase(double phase) {
    return std::remainder(phase, k_two_pi);
}

// Every random consumer gets its own engine so that, e.g., enabling fading
// does not change the noise realisation.
std::mt19937 make_engine(uint32_t seed, uint32_t stream) {
    std::seed_seq sequence{seed, stream};
    return std::mt19937(sequence);
}

// ===========================================================================
// FIR design (Blackman-windowed sinc, linear phase, odd length)
// ===========================================================================

size_t fir_length(double rate, double transition_hz) {
    const size_t half = static_cast<size_t>(std::ceil(k_blackman_transition_factor * rate / transition_hz / 2.0));
    return 2 * half + 1;
}

double blackman(size_t n, size_t length) {
    const double x = k_two_pi * n / (length - 1);
    return k_blackman_a0 - k_blackman_a1 * std::cos(x) + k_blackman_a2 * std::cos(2.0 * x);
}

// Keeps a band edge far enough below Nyquist for the filter skirt to fit.
double below_nyquist(double edge_hz, double rate, double transition_hz) {
    return std::min(edge_hz, rate / 2.0 - transition_hz / 2.0);
}

// Lowpass with unity DC gain; cutoff_hz is the -6 dB point.
std::vector<double> design_lowpass(double cutoff_hz, double rate, double transition_hz) {
    const size_t length = fir_length(rate, transition_hz);
    const double center = (length - 1) / 2.0;
    const double fc = cutoff_hz / rate;
    std::vector<double> taps(length);
    double sum = 0.0;
    for (size_t n = 0; n < length; ++n) {
        const double t = n - center;
        const double sinc = t == 0.0 ? 2.0 * fc : std::sin(k_two_pi * fc * t) / (k_pi * t);
        taps[n] = sinc * blackman(n, length);
        sum += taps[n];
    }
    for (size_t n = 0; n < length; ++n) taps[n] /= sum;
    return taps;
}

// Real bandpass: the lowpass prototype moved to +center and -center (2 cos).
std::vector<double> design_bandpass(double low_hz, double high_hz, double rate, double transition_hz) {
    std::vector<double> taps = design_lowpass((high_hz - low_hz) / 2.0, rate, transition_hz);
    const double center_hz = (low_hz + high_hz) / 2.0;
    const double center = (taps.size() - 1) / 2.0;
    for (size_t n = 0; n < taps.size(); ++n) taps[n] *= 2.0 * std::cos(k_two_pi * center_hz * (n - center) / rate);
    return taps;
}

// Hilbert transformer: ideal response 2 / (pi k) at odd k, zero at even k.
std::vector<double> design_hilbert(double rate) {
    const size_t length = fir_length(rate, k_hilbert_transition_hz);
    const long center = static_cast<long>(length - 1) / 2;
    std::vector<double> taps(length, 0.0);
    for (size_t n = 0; n < length; ++n) {
        const long k = static_cast<long>(n) - center;
        if (k % 2 != 0) taps[n] = 2.0 / (k_pi * k) * blackman(n, length);
    }
    return taps;
}

// ===========================================================================
// Streaming filters
// ===========================================================================

// Doubled buffer so the newest 'length' samples are always contiguous.
template <typename T>
class DelayLine {
public:
    explicit DelayLine(size_t length = 1) : length_(length), buffer_(2 * length, T()), pos_(0) {}

    void push(const T& x) {
        pos_ = (pos_ == 0 ? length_ : pos_) - 1;
        buffer_[pos_] = x;
        buffer_[pos_ + length_] = x;
    }

    // [0] is the newest sample, [k] the one pushed k samples earlier.
    const T* newest_first() const { return &buffer_[pos_]; }

private:
    size_t length_;
    std::vector<T> buffer_;
    size_t pos_;
};

template <typename T>
class FirFilter {
public:
    FirFilter() {}
    explicit FirFilter(const std::vector<double>& taps) : taps_(taps), line_(taps.size()) {}

    void push(const T& x) { line_.push(x); }

    T output() const {
        const T* x = line_.newest_first();
        T acc = T();
        for (size_t k = 0; k < taps_.size(); ++k) acc += x[k] * taps_[k];
        return acc;
    }

    T filter(const T& x) {
        push(x);
        return output();
    }

    const T& delayed(size_t k) const { return line_.newest_first()[k]; }
    size_t length() const { return taps_.size(); }

private:
    std::vector<double> taps_;
    DelayLine<T> line_;
};

// SSB receiver selectivity: a one-sided (complex) bandpass that passes
// low..high and rejects the mirror band. The product detector output is the
// real part; the magnitude is the envelope the IF AGC sees.
class SidebandDetector {
public:
    SidebandDetector() {}
    SidebandDetector(double low_hz, double high_hz, double rate) {
        const std::vector<double> prototype = design_lowpass((high_hz - low_hz) / 2.0, rate, k_audio_transition_hz);
        const double center_hz = (low_hz + high_hz) / 2.0;
        const double center = (prototype.size() - 1) / 2.0;
        taps_.resize(prototype.size());
        for (size_t n = 0; n < prototype.size(); ++n)
            taps_[n] = prototype[n] * std::polar(1.0, k_two_pi * center_hz * (n - center) / rate);
        line_ = DelayLine<Complex>(taps_.size());
    }

    Complex process(const Complex& z) {
        line_.push(z);
        const Complex* x = line_.newest_first();
        double re = 0.0;
        double im = 0.0;
        for (size_t k = 0; k < taps_.size(); ++k) {
            re += taps_[k].real() * x[k].real() - taps_[k].imag() * x[k].imag();
            im += taps_[k].real() * x[k].imag() + taps_[k].imag() * x[k].real();
        }
        return Complex(re, im);
    }

private:
    std::vector<Complex> taps_;
    DelayLine<Complex> line_;
};

// Polyphase interpolator: each input sample yields 'factor' output phases.
// y[n * L + p] = sum_i h[p + i * L] * x[n - i]
class Interpolator {
public:
    Interpolator() : phase_length_(1) {}
    Interpolator(size_t factor, const std::vector<double>& prototype)
        : phase_length_((prototype.size() + factor - 1) / factor),
          phases_(factor, std::vector<double>(phase_length_, 0.0)),
          line_(phase_length_) {
        // zero stuffing divides the amplitude by the factor; the prototype has unity DC gain
        for (size_t m = 0; m < prototype.size(); ++m)
            phases_[m % factor][m / factor] = prototype[m] * static_cast<double>(factor);
    }

    void push(double x) { line_.push(x); }

    double output(size_t phase) const {
        const double* x = line_.newest_first();
        const std::vector<double>& taps = phases_[phase];
        double acc = 0.0;
        for (size_t i = 0; i < phase_length_; ++i) acc += taps[i] * x[i];
        return acc;
    }

private:
    size_t phase_length_;
    std::vector<std::vector<double> > phases_;
    DelayLine<double> line_;
};

class DcBlocker {
public:
    DcBlocker() {}
    DcBlocker(double cutoff_hz, double rate) : pole_(std::exp(-k_two_pi * cutoff_hz / rate)) {}

    double process(double x) {
        const double y = x - previous_input_ + pole_ * previous_output_;
        previous_input_ = x;
        previous_output_ = y;
        return y;
    }

private:
    double pole_ = 0.0;
    double previous_input_ = 0.0;
    double previous_output_ = 0.0;
};

// y[n] = b0 x[n] + b1 x[n-1] - a1 y[n-1]
struct FirstOrderSection {
    double b0 = 1.0;
    double b1 = 0.0;
    double a1 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;

    double process(double x) {
        const double y = b0 * x + b1 * x1 - a1 * y1;
        x1 = x;
        y1 = y;
        return y;
    }

    double gain(double freq_hz, double rate) const {
        const Complex z_inv = std::polar(1.0, -k_two_pi * freq_hz / rate);
        return std::abs((b0 + b1 * z_inv) / (1.0 + a1 * z_inv));
    }
};

// FM pre-emphasis (1 + s tau) / (1 + s tau_shelf) by the bilinear transform,
// normalised to unity gain at 1 kHz. The shelf pole makes it realisable; it
// sits far above the audio band.
FirstOrderSection design_preemphasis(double tau_s, double rate) {
    const double k = 2.0 * rate;
    const double tau_shelf = 1.0 / (k_two_pi * k_emphasis_shelf_hz);
    const double norm = 1.0 + tau_shelf * k;
    FirstOrderSection section;
    section.b0 = (1.0 + tau_s * k) / norm;
    section.b1 = (1.0 - tau_s * k) / norm;
    section.a1 = (1.0 - tau_shelf * k) / norm;
    const double reference_gain = section.gain(k_emphasis_reference_hz, rate);
    section.b0 /= reference_gain;
    section.b1 /= reference_gain;
    return section;
}

// Exact inverse, so matched pre/de-emphasis is flat.
FirstOrderSection inverse(const FirstOrderSection& s) {
    FirstOrderSection r;
    r.b0 = 1.0 / s.b0;
    r.b1 = s.a1 / s.b0;
    r.a1 = s.b1 / s.b0;
    return r;
}

// AGC level detector: fast attack towards a rising envelope, slow exponential
// decay otherwise. It is fed an envelope, never the instantaneous audio, so the
// gain does not depend on where the samples fall on the waveform. It starts at
// unity gain (level = target) rather than at full gain, so the start of a
// simulation is not a power-on transient.
class Agc {
public:
    Agc() {}
    Agc(double rate, double attack_ms, double decay_ms, double target)
        : attack_(1.0 - std::exp(-k_ms_per_s / (attack_ms * rate))),
          decay_(std::exp(-k_ms_per_s / (decay_ms * rate))),
          target_(target),
          floor_(target / db_to_amplitude(k_agc_max_gain_db)),
          level_(target) {}

    double gain(double envelope) {
        if (envelope > level_)
            level_ += attack_ * (envelope - level_);
        else
            level_ *= decay_;
        return target_ / std::max(level_, floor_);
    }

private:
    double attack_ = 1.0;
    double decay_ = 0.0;
    double target_ = 1.0;
    double floor_ = 1.0;
    double level_ = 0.0;
};

// Two cascaded one-pole lowpasses with unity DC gain.
class LevelSmoother {
public:
    LevelSmoother() {}
    LevelSmoother(double cutoff_hz, double rate) : coefficient_(1.0 - std::exp(-k_two_pi * cutoff_hz / rate)) {}

    double process(double x) {
        stage1_ += coefficient_ * (x - stage1_);
        stage2_ += coefficient_ * (stage1_ - stage2_);
        return stage2_;
    }

private:
    double coefficient_ = 1.0;
    double stage1_ = 0.0;
    double stage2_ = 0.0;
};

// ===========================================================================
// Random processes
// ===========================================================================

// Complex AWGN with the given standard deviation per component.
class NoiseSource {
public:
    NoiseSource() {}
    NoiseSource(double sigma, uint32_t seed) : sigma_(sigma), rng_(make_engine(seed, noise_stream)) {}

    Complex next() {
        if (sigma_ == 0.0) return Complex();
        const double re = normal_(rng_);
        const double im = normal_(rng_);
        return Complex(re, im) * sigma_;
    }

private:
    double sigma_ = 0.0;
    std::mt19937 rng_;
    std::normal_distribution<double> normal_;
};

// Poisson impulses: exponential inter-arrival times, uniform random phase.
class ImpulseSource {
public:
    ImpulseSource() {}
    ImpulseSource(double rate_hz, double amplitude, double sample_rate, uint32_t seed)
        : enabled_(rate_hz > 0.0),
          amplitude_(amplitude),
          rng_(make_engine(seed, impulse_stream)),
          interval_(enabled_ ? rate_hz / sample_rate : 1.0),
          phase_(0.0, k_two_pi) {
        if (enabled_) countdown_ = interval_(rng_);
    }

    Complex next() {
        Complex sum;
        if (!enabled_) return sum;
        countdown_ -= 1.0;
        while (countdown_ <= 0.0) {
            sum += std::polar(amplitude_, phase_(rng_));
            countdown_ += interval_(rng_);
        }
        return sum;
    }

private:
    bool enabled_ = false;
    double amplitude_ = 0.0;
    std::mt19937 rng_;
    std::exponential_distribution<double> interval_;  // in samples
    std::uniform_real_distribution<double> phase_;
    double countdown_ = 0.0;
};

// One Rayleigh fading tap with a Gaussian Doppler spectrum. Complex white
// noise is shaped by a Gaussian FIR at a low update rate (32x the spread) and
// linearly interpolated up to the sample rate.
class DopplerTap {
public:
    DopplerTap() {}
    DopplerTap(double rate, double spread_hz, double power, uint32_t seed, uint32_t stream)
        : rng_(make_engine(seed, stream)),
          normal_(0.0, std::sqrt(k_complex_component_variance)),
          scale_(std::sqrt(power)) {
        if (spread_hz <= 0.0) {
            frozen_ = true;
            current_ = white() * scale_;
            return;
        }
        const double target_update_rate = k_fading_updates_per_spread * spread_hz;
        samples_per_update_ = std::max<size_t>(1, static_cast<size_t>(std::lround(rate / target_update_rate)));
        const double update_rate = rate / samples_per_update_;
        // Power spectrum exp(-f^2 / (2 sigma_f^2)) <=> amplitude response exp(-f^2 / (4 sigma_f^2))
        // <=> impulse response exp(-t^2 / (2 sigma_t^2)) with sigma_t = 1 / (2 sqrt(2) pi sigma_f).
        const double sigma_f = spread_hz / k_sigmas_per_spread;
        const double sigma_t = 1.0 / (2.0 * std::sqrt(2.0) * k_pi * sigma_f);
        const size_t half = static_cast<size_t>(std::ceil(k_fading_span_sigmas * sigma_t * update_rate));
        std::vector<double> taps(2 * half + 1);
        double energy = 0.0;
        for (size_t n = 0; n < taps.size(); ++n) {
            const double t = (static_cast<double>(n) - static_cast<double>(half)) / update_rate;
            taps[n] = std::exp(-t * t / (2.0 * sigma_t * sigma_t));
            energy += taps[n] * taps[n];
        }
        for (size_t n = 0; n < taps.size(); ++n) taps[n] /= std::sqrt(energy);  // unit power gain
        shaping_ = FirFilter<Complex>(taps);
        for (size_t n = 0; n < taps.size(); ++n) shaping_.push(white());  // start in steady state
        current_ = generate();
        next_ = generate();
    }

    Complex next() {
        if (frozen_) return current_;
        const double fraction = static_cast<double>(counter_) / samples_per_update_;
        const Complex gain = current_ + (next_ - current_) * fraction;
        if (++counter_ == samples_per_update_) {
            counter_ = 0;
            current_ = next_;
            next_ = generate();
        }
        return gain;
    }

private:
    Complex white() {
        const double re = normal_(rng_);
        const double im = normal_(rng_);
        return Complex(re, im);
    }

    Complex generate() { return shaping_.filter(white()) * scale_; }

    std::mt19937 rng_;
    std::normal_distribution<double> normal_;
    double scale_ = 0.0;
    bool frozen_ = false;
    FirFilter<Complex> shaping_;
    size_t samples_per_update_ = 1;
    size_t counter_ = 0;
    Complex current_;
    Complex next_;
};

// Watterson channel: path 1 undelayed, path 2 delayed, independent taps,
// powers split so the total average gain is 1.
class Watterson {
public:
    Watterson() {}
    Watterson(const ChannelConfig& config, double rate) : two_path_(config.path_delay_ms > 0.0) {
        const double ratio = two_path_ ? db_to_power(config.path2_gain_db) : 0.0;
        const double path1_power = 1.0 / (1.0 + ratio);
        path1_ = DopplerTap(rate, config.doppler_spread_hz, path1_power, config.seed, fading_path1_stream);
        if (!two_path_) return;
        path2_ = DopplerTap(rate, config.doppler_spread_hz, ratio * path1_power, config.seed, fading_path2_stream);
        delay_samples_ = static_cast<size_t>(std::lround(config.path_delay_ms * rate / k_ms_per_s));
        delay_ = DelayLine<Complex>(delay_samples_ + 1);
    }

    Complex process(const Complex& x) {
        Complex y = path1_.next() * x;
        if (two_path_) {
            delay_.push(x);
            y += path2_.next() * delay_.newest_first()[delay_samples_];
        }
        return y;
    }

private:
    bool two_path_ = false;
    DopplerTap path1_;
    DopplerTap path2_;
    size_t delay_samples_ = 0;
    DelayLine<Complex> delay_;
};

class Oscillator {
public:
    Oscillator() {}
    Oscillator(double freq_hz, double rate) : step_(k_two_pi * freq_hz / rate) {}

    Complex next() {
        const Complex value = std::polar(1.0, phase_);
        phase_ = wrap_phase(phase_ + step_);
        return value;
    }

private:
    double step_ = 0.0;
    double phase_ = 0.0;
};

// Slow sinusoidal fading, starting at the crest: gain dB = -depth (1 - cos(2 pi rate t)) / 2.
class Qsb {
public:
    Qsb() {}
    Qsb(double depth_db, double rate_hz, double sample_rate)
        : depth_db_(depth_db), step_(k_two_pi * rate_hz / sample_rate) {}

    double next() {
        const double gain = db_to_amplitude(-depth_db_ * (1.0 - std::cos(phase_)) / 2.0);
        phase_ = wrap_phase(phase_ + step_);
        return gain;
    }

private:
    double depth_db_ = 0.0;
    double step_ = 0.0;
    double phase_ = 0.0;
};

// Key envelope of random Morse: letters and digits in words of 1..k_max_word_characters characters, PARIS
// timing, raised-cosine edges. The envelope is 0.5 - 0.5 cos(pi e), e ramping between 0 and 1 in one edge time.
class MorseKeyer {
public:
    MorseKeyer() {}
    MorseKeyer(double wpm, double rate, uint32_t seed)
        : rng_(make_engine(seed, cw_stream)),
          character_(0, k_morse_code_count - 1),
          word_length_(1, k_max_word_characters),
          dot_samples_(k_paris_dot_s / wpm * rate),
          edge_step_(1.0 / (k_cw_edge_s * rate)) {}

    double next() {
        remaining_ -= 1.0;
        while (remaining_ <= 0.0) {
            if (elements_.empty()) queue_character();
            key_down_ = elements_.front().key_down;
            remaining_ += elements_.front().dots * dot_samples_;
            elements_.pop_front();
        }
        edge_ = key_down_ ? std::min(1.0, edge_ + edge_step_) : std::max(0.0, edge_ - edge_step_);
        return (1.0 - std::cos(k_pi * edge_)) / 2.0;
    }

private:
    struct Element {
        bool key_down;
        double dots;
    };

    void queue_character() {
        if (characters_left_ == 0) characters_left_ = word_length_(rng_);
        --characters_left_;
        for (const char* symbol = k_morse_codes[character_(rng_)]; *symbol != '\0'; ++symbol) {
            elements_.push_back(Element{true, *symbol == '-' ? k_dash_units : k_dot_units});
            double gap = k_symbol_gap_units;
            if (symbol[1] == '\0') gap = characters_left_ == 0 ? k_word_gap_units : k_character_gap_units;
            elements_.push_back(Element{false, gap});
        }
    }

    std::mt19937 rng_;
    std::uniform_int_distribution<size_t> character_;
    std::uniform_int_distribution<int> word_length_;
    double dot_samples_ = 1.0;
    double edge_step_ = 1.0;
    std::deque<Element> elements_;
    int characters_left_ = 0;
    bool key_down_ = false;
    double remaining_ = 0.0;
    double edge_ = 0.0;
};

// Other stations as heard in the receiver audio: the analytic signal of a steady carrier plus keyed CW,
// key-down amplitudes relative to the key-down tone (signal_level at the receiver output).
class Interference {
public:
    Interference() {}
    Interference(const ChannelConfig& config, double rate)
        : carrier_enabled_(config.carrier_hz > 0.0),
          cw_enabled_(config.cw_hz > 0.0),
          carrier_amplitude_(config.signal_level * db_to_amplitude(config.carrier_db)),
          cw_amplitude_(config.signal_level * db_to_amplitude(config.cw_db)),
          carrier_(config.carrier_hz, rate),
          cw_(config.cw_hz, rate) {
        if (cw_enabled_) keyer_ = MorseKeyer(config.cw_wpm, rate, config.seed);
    }

    bool enabled() const { return carrier_enabled_ || cw_enabled_; }

    Complex next() {
        Complex sum;
        if (carrier_enabled_) sum += carrier_.next() * carrier_amplitude_;
        if (cw_enabled_) sum += cw_.next() * (cw_amplitude_ * keyer_.next());
        return sum;
    }

private:
    bool carrier_enabled_ = false;
    bool cw_enabled_ = false;
    double carrier_amplitude_ = 0.0;
    double cw_amplitude_ = 0.0;
    Oscillator carrier_;
    Oscillator cw_;
    MorseKeyer keyer_;
};

bool positive(double x) {
    return std::isfinite(x) && x > 0.0;
}

bool non_negative(double x) {
    return std::isfinite(x) && x >= 0.0;
}

void require(bool condition, const char* rule) {
    if (!condition) throw std::invalid_argument(std::string("ChannelConfig: ") + rule);
}

// Only the parameters the mode uses are checked.
void validate(const ChannelConfig& c) {
    require(positive(c.sample_rate), "sample_rate > 0");
    if (c.mode == Mode::clean) return;

    const bool am = c.mode == Mode::am;
    const bool fm = c.mode == Mode::fm;
    const double audio_high =
        below_nyquist(fm ? c.fm_audio_high_hz : c.rx_high_hz, c.sample_rate, k_audio_transition_hz);
    require(positive(c.signal_level), "signal_level > 0");
    require(std::isfinite(c.snr_db) && std::isfinite(c.freq_offset_hz) && std::isfinite(c.lsb_pivot_hz),
            "finite snr_db, freq_offset_hz and lsb_pivot_hz");
    require(non_negative(c.rx_low_hz) && c.rx_low_hz < audio_high,
            "0 <= rx_low_hz < audio high edge (rx_high_hz, fm: fm_audio_high_hz) < sample_rate / 2");
    if (c.fading)
        require(non_negative(c.doppler_spread_hz) && non_negative(c.path_delay_ms) && std::isfinite(c.path2_gain_db),
                "doppler_spread_hz >= 0, path_delay_ms >= 0, finite path2_gain_db");
    require(non_negative(c.impulse_rate_hz) && std::isfinite(c.impulse_level_db),
            "impulse_rate_hz >= 0, finite impulse_level_db");
    const double nyquist = c.sample_rate / 2.0;
    require(non_negative(c.carrier_hz) && c.carrier_hz < nyquist && std::isfinite(c.carrier_db),
            "0 <= carrier_hz < sample_rate / 2, finite carrier_db");
    require(non_negative(c.cw_hz) && c.cw_hz < nyquist && std::isfinite(c.cw_db),
            "0 <= cw_hz < sample_rate / 2, finite cw_db");
    if (c.cw_hz > 0.0) require(positive(c.cw_wpm), "cw_wpm > 0");
    require(non_negative(c.qsb_depth_db) && non_negative(c.qsb_rate_hz), "qsb_depth_db >= 0, qsb_rate_hz >= 0");
    require(std::isfinite(c.clock_ppm) && std::fabs(c.clock_ppm) <= k_max_clock_ppm, "|clock_ppm| <= 1e5");
    if (c.agc && !fm)
        require(positive(c.agc_attack_ms) && positive(c.agc_decay_ms) && positive(c.agc_target),
                "agc_attack_ms, agc_decay_ms, agc_target > 0");
    if (am)
        require(positive(c.am_modulation_index) && positive(c.am_if_bandwidth_hz),
                "am_modulation_index, am_if_bandwidth_hz > 0");
    if (fm)
        require(positive(c.fm_deviation_hz) && non_negative(c.fm_max_deviation_hz) &&
                    positive(c.fm_if_bandwidth_hz) && non_negative(c.fm_emphasis_us),
                "fm_deviation_hz > 0, fm_max_deviation_hz >= 0, fm_if_bandwidth_hz > 0, fm_emphasis_us >= 0");
}

}  // namespace

double fm_cnr_db(const ChannelConfig& config) {
    return config.snr_db + 10.0 * std::log10(k_reference_bandwidth_hz / config.fm_if_bandwidth_hz);
}

void apply_preset(ChannelConfig& config, FadingPreset preset) {
    config.fading = preset != FadingPreset::none;
    for (size_t i = 0; i < sizeof(k_fading_profiles) / sizeof(k_fading_profiles[0]); ++i) {
        if (k_fading_profiles[i].preset != preset) continue;
        config.path_delay_ms = k_fading_profiles[i].path_delay_ms;
        config.doppler_spread_hz = k_fading_profiles[i].doppler_spread_hz;
        config.path2_gain_db = 0.0;
    }
}

// ===========================================================================
// Channel implementation
// ===========================================================================

class Channel::Impl {
public:
    explicit Impl(const ChannelConfig& config);
    double process(double x);
    bool clock_error() const { return clock_ != nullptr; }
    void apply_clock_error(const float* in, size_t count, std::vector<float>& out) { clock_->process(in, count, out); }

private:
    void init_ssb();
    void init_am_fm();
    Complex propagate(Complex z);
    double process_ssb(double x);
    double process_am(double x);
    double process_fm(double x);

    ChannelConfig config_;
    size_t factor_ = 1;     // rf rate / audio rate
    double rf_rate_ = 0.0;  // rate of the complex-baseband simulation

    std::unique_ptr<pc::Resampler> clock_;  // transmitter clock error

    // RF path, shared by all modes
    bool fading_enabled_ = false;
    Watterson fading_;
    bool qsb_enabled_ = false;
    Qsb qsb_;
    bool invert_spectrum_ = false;
    Oscillator oscillator_;
    NoiseSource noise_;
    ImpulseSource impulses_;
    bool interference_enabled_ = false;
    Interference interference_;  // at the audio rate

    // usb / lsb
    FirFilter<double> hilbert_;
    size_t hilbert_delay_ = 0;
    SidebandDetector detector_;

    // am / fm
    Interpolator interpolator_;
    FirFilter<double> decimator_;
    FirFilter<Complex> if_filter_;
    FirFilter<double> audio_filter_;
    DcBlocker dc_blocker_;
    LevelSmoother carrier_detector_;
    double am_index_ = 0.0;
    double am_output_scale_ = 0.0;
    bool preemphasis_enabled_ = false;
    bool deemphasis_enabled_ = false;
    FirstOrderSection preemphasis_;
    FirstOrderSection deemphasis_;
    bool limiter_enabled_ = false;
    double deviation_limit_ = 0.0;  // audio units
    FirFilter<double> splatter_filter_;
    double fm_phase_ = 0.0;
    double fm_phase_step_ = 0.0;     // radians per sample per unit of audio
    double fm_output_scale_ = 0.0;   // discriminator radians -> audio units
    Complex previous_;

    bool agc_enabled_ = false;
    Agc agc_;
};

Channel::Impl::Impl(const ChannelConfig& config) : config_(config) {
    validate(config);
    if (config.mode == Mode::clean) return;

    const bool ssb = config.mode == Mode::usb || config.mode == Mode::lsb;
    factor_ = ssb ? 1 : static_cast<size_t>(std::ceil(k_min_rf_rate_hz / config.sample_rate));
    rf_rate_ = config.sample_rate * factor_;

    // Noise and impulses are referenced to the key-down complex envelope at RF
    // (power P_ref = amplitude^2). Complex noise with N0 * rate / 2 per
    // component has the density N0 across the simulated band.
    const double reference_amplitude = ssb ? config.signal_level : k_carrier_amplitude;
    const double n0 =
        reference_amplitude * reference_amplitude / (db_to_power(config.snr_db) * k_reference_bandwidth_hz);
    noise_ = NoiseSource(config.noise ? std::sqrt(n0 * rf_rate_ / 2.0) : 0.0, config.seed);
    const double peak =
        config.mode == Mode::am ? k_carrier_amplitude * (1.0 + config.am_modulation_index) : reference_amplitude;
    const double impulse_sample = peak * db_to_amplitude(config.impulse_level_db) * k_impulse_duration_s * rf_rate_;
    impulses_ = ImpulseSource(config.impulse_rate_hz, impulse_sample, rf_rate_, config.seed);

    if (config.clock_ppm != 0.0)
        clock_.reset(new pc::Resampler(config.sample_rate * (1.0 + config.clock_ppm * k_ppm), config.sample_rate));

    fading_enabled_ = config.fading;
    if (fading_enabled_) fading_ = Watterson(config, rf_rate_);
    qsb_enabled_ = config.qsb_depth_db > 0.0;
    if (qsb_enabled_) qsb_ = Qsb(config.qsb_depth_db, config.qsb_rate_hz, rf_rate_);
    interference_ = Interference(config, config.sample_rate);
    interference_enabled_ = interference_.enabled();
    invert_spectrum_ = config.mode == Mode::lsb;
    const double shift_hz = invert_spectrum_ ? config.lsb_pivot_hz + config.freq_offset_hz : config.freq_offset_hz;
    oscillator_ = Oscillator(shift_hz, rf_rate_);

    agc_enabled_ = config.agc && config.mode != Mode::fm;
    if (agc_enabled_) agc_ = Agc(config.sample_rate, config.agc_attack_ms, config.agc_decay_ms, config.agc_target);

    if (ssb)
        init_ssb();
    else
        init_am_fm();
}

void Channel::Impl::init_ssb() {
    const double rate = config_.sample_rate;
    hilbert_ = FirFilter<double>(design_hilbert(rate));
    hilbert_delay_ = (hilbert_.length() - 1) / 2;
    detector_ =
        SidebandDetector(config_.rx_low_hz, below_nyquist(config_.rx_high_hz, rate, k_audio_transition_hz), rate);
}

void Channel::Impl::init_am_fm() {
    const double rate = config_.sample_rate;
    const bool am = config_.mode == Mode::am;

    const std::vector<double> resampler =
        design_lowpass(k_resampler_cutoff_fraction * rate, rf_rate_, k_resampler_transition_fraction * rate);
    interpolator_ = Interpolator(factor_, resampler);
    decimator_ = FirFilter<double>(resampler);

    const double if_bandwidth = am ? config_.am_if_bandwidth_hz : config_.fm_if_bandwidth_hz;
    const double if_transition = k_if_transition_fraction * if_bandwidth;
    const double if_cutoff = below_nyquist(if_bandwidth / 2.0, rf_rate_, if_transition);
    if_filter_ = FirFilter<Complex>(design_lowpass(if_cutoff, rf_rate_, if_transition));

    const double audio_edge = am ? config_.rx_high_hz : config_.fm_audio_high_hz;
    const double audio_high = below_nyquist(audio_edge, rate, k_audio_transition_hz);
    audio_filter_ = FirFilter<double>(design_bandpass(config_.rx_low_hz, audio_high, rate, k_audio_transition_hz));

    if (am) {
        am_index_ = config_.am_modulation_index;
        am_output_scale_ = config_.signal_level / (k_carrier_amplitude * am_index_);
        dc_blocker_ = DcBlocker(k_am_dc_block_hz, rf_rate_);
        carrier_detector_ = LevelSmoother(k_am_carrier_smoothing_hz, rf_rate_);
        return;
    }

    const bool emphasis = config_.fm_emphasis_us > 0.0;
    preemphasis_enabled_ = emphasis && config_.fm_tx_preemphasis;
    deemphasis_enabled_ = emphasis && config_.fm_rx_deemphasis;
    if (emphasis) {
        preemphasis_ = design_preemphasis(config_.fm_emphasis_us / k_us_per_s, rf_rate_);
        deemphasis_ = inverse(preemphasis_);
    }
    limiter_enabled_ = config_.fm_max_deviation_hz > 0.0;
    if (limiter_enabled_) {
        deviation_limit_ = config_.signal_level * config_.fm_max_deviation_hz / config_.fm_deviation_hz;
        const double splatter_edge = config_.fm_audio_high_hz + k_splatter_transition_hz / 2.0;
        const double splatter_cutoff = below_nyquist(splatter_edge, rf_rate_, k_splatter_transition_hz);
        splatter_filter_ = FirFilter<double>(design_lowpass(splatter_cutoff, rf_rate_, k_splatter_transition_hz));
    }
    fm_phase_step_ = k_two_pi * config_.fm_deviation_hz / (config_.signal_level * rf_rate_);
    fm_output_scale_ = rf_rate_ / k_two_pi / config_.fm_deviation_hz * config_.signal_level;
}

// Fading and QSB, receiver spectrum inversion (lsb), frequency shift, then additive noise and impulses.
Complex Channel::Impl::propagate(Complex z) {
    if (fading_enabled_) z = fading_.process(z);
    if (qsb_enabled_) z *= qsb_.next();
    if (invert_spectrum_) z = std::conj(z);
    z *= oscillator_.next();
    return z + noise_.next() + impulses_.next();
}

double Channel::Impl::process(double x) {
    switch (config_.mode) {
    case Mode::usb:
    case Mode::lsb:
        return process_ssb(x);
    case Mode::am:
        return process_am(x);
    case Mode::fm:
        return process_fm(x);
    case Mode::clean:
        break;
    }
    return x;
}

// ---------------------------------------------------------------------------
// usb / lsb: analytic signal at the audio rate. An SSB transmitter moves the
// audio spectrum up by the carrier; in complex baseband that is exactly the
// analytic signal. The receiver's crystal filter is the one-sided bandpass.
// ---------------------------------------------------------------------------
double Channel::Impl::process_ssb(double x) {
    hilbert_.push(x);
    const Complex analytic(hilbert_.delayed(hilbert_delay_), hilbert_.output());
    Complex rf = propagate(analytic);
    if (interference_enabled_) rf += interference_.next();
    const Complex received = detector_.process(rf);
    double y = received.real();
    if (agc_enabled_) y *= agc_.gain(std::abs(received));
    return y;
}

// ---------------------------------------------------------------------------
// am: envelope C (1 + m x / signal_level) clipped at 0, complex baseband at the
// rf rate, IF filter, envelope detector, DC removal, decimation, audio filter.
// The AGC follows the detected carrier: signal_level * carrier / C is the peak
// a key-down tone has at the output for that carrier level.
// ---------------------------------------------------------------------------
double Channel::Impl::process_am(double x) {
    interpolator_.push(x);
    double carrier = 0.0;
    for (size_t phase = 0; phase < factor_; ++phase) {
        const double audio = interpolator_.output(phase);
        const double envelope = std::max(0.0, k_carrier_amplitude * (1.0 + am_index_ * audio / config_.signal_level));
        const double detected = std::abs(if_filter_.filter(propagate(Complex(envelope, 0.0))));
        carrier = carrier_detector_.process(detected);
        decimator_.push(dc_blocker_.process(detected));
    }
    double audio = decimator_.output() * am_output_scale_;
    if (interference_enabled_) audio += interference_.next().real();
    double y = audio_filter_.filter(audio);
    if (agc_enabled_) y *= agc_.gain(config_.signal_level * carrier / k_carrier_amplitude);
    return y;
}

// ---------------------------------------------------------------------------
// fm: pre-emphasis, deviation limiter and splatter filter, phase integration,
// complex baseband at the rf rate, IF filter, limiter-discriminator
// arg(z[n] conj(z[n-1])), de-emphasis, decimation, audio filter. Below
// threshold the discriminator produces clicks (2 pi phase slips) by itself.
// ---------------------------------------------------------------------------
double Channel::Impl::process_fm(double x) {
    interpolator_.push(x);
    for (size_t phase = 0; phase < factor_; ++phase) {
        double audio = interpolator_.output(phase);
        if (preemphasis_enabled_) audio = preemphasis_.process(audio);
        if (limiter_enabled_)
            audio = splatter_filter_.filter(std::max(-deviation_limit_, std::min(deviation_limit_, audio)));
        fm_phase_ = wrap_phase(fm_phase_ + fm_phase_step_ * audio);
        const Complex received = if_filter_.filter(propagate(std::polar(k_carrier_amplitude, fm_phase_)));
        double demodulated = std::arg(received * std::conj(previous_)) * fm_output_scale_;
        previous_ = received;
        if (deemphasis_enabled_) demodulated = deemphasis_.process(demodulated);
        decimator_.push(demodulated);
    }
    double audio = decimator_.output();
    if (interference_enabled_) audio += interference_.next().real();
    return audio_filter_.filter(audio);
}

// ===========================================================================
// Channel
// ===========================================================================

Channel::Channel(const ChannelConfig& config) : config_(config), impl_(new Impl(config)) {}

Channel::~Channel() {}

Channel::Channel(Channel&& other) = default;

Channel& Channel::operator=(Channel&& other) = default;

void Channel::process(const float* in, float* out, size_t count) {
    if (impl_->clock_error())
        throw std::logic_error("Channel: with clock_ppm the output length differs, use the vector process()");
    for (size_t i = 0; i < count; ++i) out[i] = static_cast<float>(impl_->process(in[i]) * config_.output_gain);
}

std::vector<float> Channel::process(const std::vector<float>& in) {
    std::vector<float> out;
    if (!impl_->clock_error()) {
        out.resize(in.size());
        if (!in.empty()) process(&in[0], &out[0], in.size());
        return out;
    }
    if (!in.empty()) impl_->apply_clock_error(&in[0], in.size(), out);
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<float>(impl_->process(out[i]) * config_.output_gain);
    return out;
}

void Channel::reset() {
    impl_.reset(new Impl(config_));
}

const ChannelConfig& Channel::config() const {
    return config_;
}

}  // namespace sim
}  // namespace unlimited
