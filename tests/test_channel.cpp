#include "channel.hpp"
#include "portable_random.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using unlimited::sim::Channel;
using unlimited::sim::ChannelConfig;
using unlimited::sim::Exponential;
using unlimited::sim::FadingPreset;
using unlimited::sim::Gamma;
using unlimited::sim::Mode;
using unlimited::sim::Normal;
using unlimited::sim::UniformInteger;
using unlimited::sim::UniformReal;
using unlimited::sim::apply_preset;
using unlimited::sim::fm_cnr_db;

namespace {

using std::size_t;
using test::count_of;

typedef std::complex<double> Complex;

const double pi = 3.14159265358979323846;
const double two_pi = 2.0 * pi;
const double rate = 8000.0;
const double settle_s = 0.5;     // longer than any filter / resampler latency
const double measure_s = 1.0;    // whole cycles for every integer-Hz tone
const double tone_s = settle_s + measure_s;
const double block_s = 0.01;     // fading blocks: whole cycles at 1000, 1500, 2000 Hz
const double fading_skip_s = 1.0;  // fading statistics start once every filter has settled
const double ms_per_s = 1000.0;
const double us_per_s = 1e6;
const double reference_bandwidth_hz = 2500.0;
const double impulse_reference_rate_hz = 48000.0;  // impulses have the area of a 1/48000 s spike
const double emphasis_reference_hz = 1000.0;       // fm_deviation_hz is defined at this tone frequency
const double not_a_number = std::numeric_limits<double>::quiet_NaN();

const double click_threshold_fraction = 0.6;  // of signal_level
const double click_dead_time_s = 0.002;

const double deep_fade = 0.1;  // -10 dB below the average power
const double rayleigh_deep_fraction = 1.0 - std::exp(-deep_fade);
const double mean_gain_tolerance_db = 1.0;
const double deep_fraction_tolerance = 0.04;

const Mode all_modes[] = {Mode::clean, Mode::usb, Mode::lsb, Mode::am, Mode::fm};

const char* mode_name(Mode mode) {
    switch (mode) {
    case Mode::clean:
        return "clean";
    case Mode::usb:
        return "usb";
    case Mode::lsb:
        return "lsb";
    case Mode::am:
        return "am";
    case Mode::fm:
        return "fm";
    }
    return "?";
}

size_t samples(double seconds, double sample_rate = rate) {
    return static_cast<size_t>(std::lround(seconds * sample_rate));
}

double power_db(double ratio) {
    return 10.0 * std::log10(ratio);
}

double amplitude_db(double ratio) {
    return 20.0 * std::log10(ratio);
}

double db_to_power(double db) {
    return std::pow(10.0, db / 10.0);
}

double db_to_amplitude(double db) {
    return std::pow(10.0, db / 20.0);
}

std::vector<float> tone(double freq_hz, double amplitude, double seconds, double sample_rate = rate,
                        double start_phase = 0.0) {
    std::vector<float> x(samples(seconds, sample_rate));
    for (size_t n = 0; n < x.size(); ++n)
        x[n] = static_cast<float>(amplitude * std::sin(two_pi * freq_hz * n / sample_rate + start_phase));
    return x;
}

std::vector<float> silence(double seconds) {
    return std::vector<float>(samples(seconds), 0.0f);
}

std::vector<float> sum(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> out(a);
    for (size_t n = 0; n < out.size(); ++n) out[n] += b[n];
    return out;
}

std::vector<float> scaled(const std::vector<float>& x, float gain) {
    std::vector<float> out(x);
    for (size_t n = 0; n < out.size(); ++n) out[n] *= gain;
    return out;
}

std::vector<float> run(const ChannelConfig& config, const std::vector<float>& input) {
    Channel channel(config);
    return channel.process(input);
}

ChannelConfig quiet(Mode mode) {
    ChannelConfig config;
    config.mode = mode;
    config.noise = false;
    return config;
}

bool rejected(const ChannelConfig& config) {
    try {
        Channel channel(config);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

// Goertzel: amplitude of the component at freq_hz in x[start, start + count).
double tone_amplitude(const std::vector<float>& x, size_t start, size_t count, double freq_hz,
                      double sample_rate = rate) {
    const double coefficient = 2.0 * std::cos(two_pi * freq_hz / sample_rate);
    double s1 = 0.0;
    double s2 = 0.0;
    for (size_t n = 0; n < count; ++n) {
        const double s = x[start + n] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return 2.0 * std::sqrt(std::max(0.0, power)) / count;
}

// Amplitude at freq_hz in the measurement window after settling.
double settled_amplitude(const std::vector<float>& x, double freq_hz, double sample_rate = rate) {
    return tone_amplitude(x, samples(settle_s, sample_rate), samples(measure_s, sample_rate), freq_hz, sample_rate);
}

// Channel gain for a tone of the given level.
double gain_db(const ChannelConfig& config, double freq_hz, double level) {
    const std::vector<float> out = run(config, tone(freq_hz, level, tone_s, config.sample_rate));
    return amplitude_db(settled_amplitude(out, freq_hz, config.sample_rate) / level);
}

// Strongest frequency (1 Hz steps) in the measurement window.
double peak_frequency(const std::vector<float>& x, double low_hz, double high_hz) {
    const double step_hz = 1.0;
    double best_hz = low_hz;
    double best = -1.0;
    for (double f = low_hz; f <= high_hz; f += step_hz) {
        const double a = settled_amplitude(x, f);
        if (a > best) {
            best = a;
            best_hz = f;
        }
    }
    return best_hz;
}

double mean_square(const std::vector<float>& x, size_t start, size_t count) {
    double total = 0.0;
    for (size_t n = start; n < start + count; ++n) total += static_cast<double>(x[n]) * x[n];
    return total / count;
}

double rms(const std::vector<float>& x, double from_s, double length_s) {
    return std::sqrt(mean_square(x, samples(from_s), samples(length_s)));
}

double max_abs(const std::vector<float>& x, size_t start) {
    double peak = 0.0;
    for (size_t n = start; n < x.size(); ++n) peak = std::max(peak, static_cast<double>(std::fabs(x[n])));
    return peak;
}

// One-sided power spectral density (power per Hz) averaged over low_hz..high_hz,
// from the settled part of x. Welch: Hann window, non-overlapping 512-point segments.
double noise_density(const std::vector<float>& x, double low_hz, double high_hz) {
    const size_t segment = 512;
    std::vector<double> window(segment);
    std::vector<double> cos_table(segment);
    std::vector<double> sin_table(segment);
    double window_power = 0.0;
    for (size_t n = 0; n < segment; ++n) {
        cos_table[n] = std::cos(two_pi * n / segment);
        sin_table[n] = std::sin(two_pi * n / segment);
        window[n] = 0.5 - 0.5 * cos_table[n];
        window_power += window[n] * window[n];
    }
    const size_t first_bin = static_cast<size_t>(std::ceil(low_hz * segment / rate));
    const size_t last_bin = static_cast<size_t>(std::floor(high_hz * segment / rate));
    double total = 0.0;
    size_t segments = 0;
    for (size_t begin = samples(settle_s); begin + segment <= x.size(); begin += segment, ++segments) {
        for (size_t k = first_bin; k <= last_bin; ++k) {
            double re = 0.0;
            double im = 0.0;
            for (size_t n = 0; n < segment; ++n) {
                const double v = window[n] * x[begin + n];
                re += v * cos_table[(k * n) % segment];
                im -= v * sin_table[(k * n) % segment];
            }
            total += re * re + im * im;
        }
    }
    return 2.0 * total / (segments * (last_bin - first_bin + 1) * rate * window_power);
}

// Power gain of the tone at freq_hz (input amplitude 'level') per fading block, after fading_skip_s.
std::vector<double> block_gains(const std::vector<float>& x, double freq_hz, double level) {
    const size_t block = samples(block_s);
    std::vector<double> gains;
    for (size_t begin = samples(fading_skip_s); begin + block <= x.size(); begin += block) {
        const double a = tone_amplitude(x, begin, block, freq_hz) / level;
        gains.push_back(a * a);
    }
    return gains;
}

// Complex amplitude of the tone at freq_hz per fading block (blocks hold whole
// cycles, so a steady tone gives the same phasor in every block).
std::vector<Complex> block_phasors(const std::vector<float>& x, double freq_hz) {
    const size_t block = samples(block_s);
    std::vector<Complex> kernel(block);
    for (size_t n = 0; n < block; ++n) kernel[n] = std::polar(2.0 / block, -two_pi * freq_hz * n / rate);
    std::vector<Complex> phasors;
    for (size_t begin = samples(fading_skip_s); begin + block <= x.size(); begin += block) {
        Complex acc;
        for (size_t n = 0; n < block; ++n) acc += kernel[n] * static_cast<double>(x[begin + n]);
        phasors.push_back(acc);
    }
    return phasors;
}

double mean(const std::vector<double>& v) {
    double total = 0.0;
    for (size_t i = 0; i < v.size(); ++i) total += v[i];
    return total / v.size();
}

double median(std::vector<double> v) {
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    const double mean_a = mean(a);
    const double mean_b = mean(b);
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        ab += (a[i] - mean_a) * (b[i] - mean_b);
        aa += (a[i] - mean_a) * (a[i] - mean_a);
        bb += (b[i] - mean_b) * (b[i] - mean_b);
    }
    return ab / std::sqrt(aa * bb);
}

// Fraction of blocks whose gain is below deep_fade times the average.
double fraction_below(const std::vector<double>& gain, double deep_fade) {
    const double average = mean(gain);
    size_t below = 0;
    for (size_t i = 0; i < gain.size(); ++i)
        if (gain[i] < deep_fade * average) ++below;
    return static_cast<double>(below) / gain.size();
}

// Peak |x| per window.
std::vector<double> window_peaks(const std::vector<float>& x, size_t window) {
    std::vector<double> peaks;
    for (size_t begin = 0; begin + window <= x.size(); begin += window) {
        double peak = 0.0;
        for (size_t n = begin; n < begin + window; ++n) peak = std::max(peak, static_cast<double>(std::fabs(x[n])));
        peaks.push_back(peak);
    }
    return peaks;
}

// Sample indices where |x| first exceeds threshold, at least dead_time + 1 samples apart.
std::vector<size_t> detect_events(const std::vector<float>& x, size_t start, double threshold, size_t dead_time) {
    std::vector<size_t> events;
    for (size_t n = start; n < x.size(); ++n) {
        if (std::fabs(x[n]) > threshold) {
            events.push_back(n);
            n += dead_time;
        }
    }
    return events;
}

// FM clicks after settling: excursions far above the hiss of a receiver working above threshold.
size_t count_clicks(const std::vector<float>& out, double signal_level) {
    const double threshold = click_threshold_fraction * signal_level;
    return detect_events(out, samples(settle_s), threshold, samples(click_dead_time_s)).size();
}

// A filtered impulse is E sinc(B t) cos(2 pi fc t + phase) on the sample grid
// (E: envelope peak, B: passband width, fc: its centre). Its largest |sample|
// depends on the random phase; this is the median over the phase, re E.
double median_sampled_peak(double bandwidth_hz, double center_hz) {
    const size_t phase_steps = 3600;
    const int span_samples = 6;  // sinc main lobe and first sidelobes
    std::vector<double> peaks;
    for (size_t p = 0; p < phase_steps; ++p) {
        const double phase = two_pi * p / phase_steps;
        double best = 0.0;
        for (int k = -span_samples; k <= span_samples; ++k) {
            const double x = pi * bandwidth_hz * k / rate;
            const double envelope = k == 0 ? 1.0 : std::sin(x) / x;
            best = std::max(best, std::fabs(envelope * std::cos(phase + two_pi * center_hz * k / rate)));
        }
        peaks.push_back(best);
    }
    return median(peaks);
}

// First-order pre-emphasis gain at freq_hz relative to the 1 kHz reference.
double emphasis_gain(double freq_hz, double emphasis_us) {
    const double tau = emphasis_us / us_per_s;
    return std::hypot(1.0, two_pi * freq_hz * tau) / std::hypot(1.0, two_pi * emphasis_reference_hz * tau);
}

}  // namespace

// ---------------------------------------------------------------------------
// clean / usb / lsb
// ---------------------------------------------------------------------------

TEST(channel_clean_is_passthrough) {
    const double freq = 1234.0;
    const double level = 0.7;
    const double duration_s = 0.5;
    const float half = 0.5f;
    ChannelConfig config;
    config.mode = Mode::clean;
    config.fading = true;
    config.impulse_rate_hz = 10.0;
    config.carrier_hz = 1000.0;
    config.carrier_db = 0.0;
    config.cw_hz = 700.0;
    config.cw_db = 0.0;
    config.qsb_depth_db = 20.0;
    config.clock_ppm = 500.0;
    const std::vector<float> input = tone(freq, level, duration_s);
    CHECK(run(config, input) == input);

    config.output_gain = half;
    CHECK(run(config, input) == scaled(input, half));
}

TEST(channel_output_gain_every_mode) {
    // Scaling by a power of two is exact in floating point, so the match is bit for bit.
    const float half = 0.5f;
    const double freq = 1000.0;
    const double duration_s = 0.5;
    const std::vector<float> input = tone(freq, ChannelConfig().signal_level, duration_s);
    for (size_t m = 0; m < count_of(all_modes); ++m) {
        ChannelConfig config;
        config.mode = all_modes[m];
        const std::vector<float> full = run(config, input);
        config.output_gain = half;
        if (!CHECK(run(config, input) == scaled(full, half))) NOTE("mode %s", mode_name(all_modes[m]));
    }
}

TEST(channel_usb_unity_gain_and_selectivity) {
    const ChannelConfig config = quiet(Mode::usb);
    const double level = config.signal_level;
    const double in_band_hz = 1500.0;
    const double gain_tolerance_db = 0.1;
    const double in_band = gain_db(config, in_band_hz, level);
    NOTE("%.0f Hz gain %.4f dB", in_band_hz, in_band);
    CHECK_NEAR(in_band, 0.0, gain_tolerance_db);

    const double rejection_db = -40.0;
    const double stop_frequencies[] = {150.0, 3800.0};
    for (size_t i = 0; i < count_of(stop_frequencies); ++i) {
        const double f = stop_frequencies[i];
        const double gain = gain_db(config, f, level);
        NOTE("%.0f Hz gain %.1f dB", f, gain);
        CHECK(gain < rejection_db);
    }
}

TEST(channel_usb_snr_calibration) {
    const double snrs[] = {0.0, 10.0, 20.0};
    const double freq = 1500.0;
    const double noise_s = 20.0;
    const double band_low_hz = 600.0;
    const double band_high_hz = 2400.0;
    const double snr_tolerance_db = 0.5;
    const double tone_tolerance_db = 0.1;
    for (size_t i = 0; i < count_of(snrs); ++i) {
        ChannelConfig config;
        config.mode = Mode::usb;
        config.snr_db = snrs[i];

        ChannelConfig tone_only = config;
        tone_only.noise = false;
        const std::vector<float> clean = run(tone_only, tone(freq, config.signal_level, tone_s));
        const double tone_power = mean_square(clean, samples(settle_s), samples(measure_s));

        const double density = noise_density(run(config, silence(noise_s)), band_low_hz, band_high_hz);
        const double measured = power_db(tone_power / (density * reference_bandwidth_hz));
        NOTE("snr %.1f dB -> measured %.3f dB (tone power %.5f)", snrs[i], measured, tone_power);
        CHECK_NEAR(measured, snrs[i], snr_tolerance_db);
        CHECK_NEAR(power_db(tone_power / (config.signal_level * config.signal_level / 2.0)), 0.0, tone_tolerance_db);
    }
}

TEST(channel_usb_frequency_offset) {
    const double freq = 1500.0;
    const double offsets[] = {120.0, -200.0};
    const double frequency_tolerance_hz = 1.0;
    const double gain_tolerance_db = 0.2;
    for (size_t i = 0; i < count_of(offsets); ++i) {
        ChannelConfig config = quiet(Mode::usb);
        config.freq_offset_hz = offsets[i];
        const std::vector<float> out = run(config, tone(freq, config.signal_level, tone_s));
        const double expected = freq + config.freq_offset_hz;
        const double measured = peak_frequency(out, config.rx_low_hz, config.rx_high_hz);
        const double gain = amplitude_db(settled_amplitude(out, measured) / config.signal_level);
        NOTE("%.0f Hz %+.0f Hz -> %.0f Hz, gain %.3f dB", freq, offsets[i], measured, gain);
        CHECK_NEAR(measured, expected, frequency_tolerance_hz);
        CHECK_NEAR(gain, 0.0, gain_tolerance_db);
    }
}

TEST(channel_lsb_inversion) {
    struct Case {
        double pivot_hz;
        double offset_hz;
        double freq_hz;
    };
    const Case cases[] = {{3000.0, 0.0, 1250.0}, {3000.0, 0.0, 700.0}, {3000.0, 100.0, 1250.0}, {2800.0, 0.0, 1250.0}};
    const double frequency_tolerance_hz = 1.0;
    const double gain_tolerance_db = 0.2;
    for (size_t i = 0; i < count_of(cases); ++i) {
        ChannelConfig config = quiet(Mode::lsb);
        config.lsb_pivot_hz = cases[i].pivot_hz;
        config.freq_offset_hz = cases[i].offset_hz;
        const std::vector<float> out = run(config, tone(cases[i].freq_hz, config.signal_level, tone_s));
        const double expected = config.lsb_pivot_hz - cases[i].freq_hz + config.freq_offset_hz;
        const double measured = peak_frequency(out, config.rx_low_hz, config.rx_high_hz);
        const double gain = amplitude_db(settled_amplitude(out, measured) / config.signal_level);
        NOTE("lsb pivot %.0f, offset %+.0f: %.0f Hz -> %.0f Hz (expected %.0f), gain %.3f dB", config.lsb_pivot_hz,
             config.freq_offset_hz, cases[i].freq_hz, measured, expected, gain);
        CHECK_NEAR(measured, expected, frequency_tolerance_hz);
        CHECK_NEAR(gain, 0.0, gain_tolerance_db);
    }
}

// ---------------------------------------------------------------------------
// am / fm
// ---------------------------------------------------------------------------

TEST(channel_am_fm_tone_fidelity) {
    const Mode modes[] = {Mode::am, Mode::fm};
    const double offsets[] = {0.0, 300.0};
    const double freq = 1000.0;
    const double harmonic_limit_db = -30.0;
    const double frequency_tolerance_hz = 1.0;
    const double gain_tolerance_db = 1.0;
    const double search_hz = 100.0;
    for (size_t m = 0; m < count_of(modes); ++m) {
        for (size_t i = 0; i < count_of(offsets); ++i) {
            ChannelConfig config = quiet(modes[m]);
            config.freq_offset_hz = offsets[i];
            const std::vector<float> out = run(config, tone(freq, config.signal_level, tone_s));
            const double gain = amplitude_db(settled_amplitude(out, freq) / config.signal_level);
            const double second = amplitude_db(settled_amplitude(out, 2.0 * freq) / config.signal_level);
            const double peak = peak_frequency(out, freq - search_hz, freq + search_hz);
            NOTE("%s, carrier offset %.0f Hz: %.0f Hz, gain %.3f dB, 2nd harmonic %.1f dB", mode_name(modes[m]),
                 offsets[i], peak, gain, second);
            CHECK_NEAR(peak, freq, frequency_tolerance_hz);
            CHECK_NEAR(gain, 0.0, gain_tolerance_db);
            CHECK(second < harmonic_limit_db);
        }
    }
}

TEST(channel_fm_emphasis_response) {
    // Half level keeps the pre-emphasised 2 kHz tone under the limiter and inside the 12.5 kHz IF.
    const double level = 0.5 * ChannelConfig().signal_level;
    const double low_hz = 1000.0;
    const double high_hz = 2000.0;  // one octave up
    const double octave_db = 6.0;
    const double tolerance_db = 1.0;

    ChannelConfig config = quiet(Mode::fm);
    double at_low = gain_db(config, low_hz, level);
    double at_high = gain_db(config, high_hz, level);
    NOTE("matched emphasis: 1 kHz %.3f dB, 2 kHz %.3f dB", at_low, at_high);
    CHECK_NEAR(at_low, 0.0, tolerance_db);
    CHECK_NEAR(at_high - at_low, 0.0, tolerance_db);

    config.fm_rx_deemphasis = false;
    at_low = gain_db(config, low_hz, level);
    at_high = gain_db(config, high_hz, level);
    NOTE("flat rx port: 1 kHz %.3f dB, tilt %.3f dB/octave", at_low, at_high - at_low);
    CHECK_NEAR(at_low, 0.0, tolerance_db);
    CHECK_NEAR(at_high - at_low, octave_db, tolerance_db);

    config.fm_rx_deemphasis = true;
    config.fm_tx_preemphasis = false;
    at_low = gain_db(config, low_hz, level);
    at_high = gain_db(config, high_hz, level);
    NOTE("flat tx, de-emphasised rx: tilt %.3f dB/octave", at_high - at_low);
    CHECK_NEAR(at_high - at_low, -octave_db, tolerance_db);

    config.fm_emphasis_us = 0.0;
    at_low = gain_db(config, low_hz, level);
    at_high = gain_db(config, high_hz, level);
    NOTE("no emphasis: 1 kHz %.3f dB, 2 kHz %.3f dB", at_low, at_high);
    CHECK_NEAR(at_low, 0.0, tolerance_db);
    CHECK_NEAR(at_high, 0.0, tolerance_db);
}

TEST(channel_fm_deviation_limiter) {
    // Flat receiver port and an IF wide enough for any of these deviations, so
    // the output is the transmitted deviation (fm_deviation_hz reads signal_level).
    ChannelConfig config = quiet(Mode::fm);
    config.fm_rx_deemphasis = false;
    config.fm_if_bandwidth_hz = 25000.0;
    const double freq = 2500.0;
    const double level = config.signal_level;
    const double linear_hz = config.fm_deviation_hz * emphasis_gain(freq, config.fm_emphasis_us);
    const double unlimited_tolerance_db = 0.5;
    const double limited_tolerance_db = 0.3;

    ChannelConfig unlimited = config;
    unlimited.fm_max_deviation_hz = 0.0;
    const double unlimited_hz = config.fm_deviation_hz * db_to_amplitude(gain_db(unlimited, freq, level));

    // Fundamental of a sine clipped at a fraction c of its peak: (2 / pi) (asin c + c sqrt(1 - c^2)).
    // The splatter filter removes the harmonics, so the fundamental is what is transmitted.
    const double c = config.fm_max_deviation_hz / linear_hz;
    const double clipped_hz = linear_hz * 2.0 / pi * (std::asin(c) + c * std::sqrt(1.0 - c * c));
    const double limited_hz = config.fm_deviation_hz * db_to_amplitude(gain_db(config, freq, level));
    NOTE("%.0f Hz key-down tone: linear deviation %.0f Hz, unlimited %.0f Hz, limited %.0f Hz (clipped sine %.0f Hz)",
         freq, linear_hz, unlimited_hz, limited_hz, clipped_hz);
    CHECK_NEAR(amplitude_db(unlimited_hz / linear_hz), 0.0, unlimited_tolerance_db);
    CHECK_NEAR(amplitude_db(limited_hz / clipped_hz), 0.0, limited_tolerance_db);
}

TEST(channel_fm_two_tone_intermodulation) {
    // Two equal tones with the key-down peak (each at half level, or backed
    // off). Pre-emphasis lifts high tones, so at full level a high pair hits the
    // deviation limiter and spreads past the 12.5 kHz IF: third-order products
    // appear, as with an overdriven transmitter. Backed off, it stays linear.
    struct Case {
        double f1_hz;
        double f2_hz;
        double level_fraction;
        bool linear;
    };
    const Case cases[] = {{700.0, 1000.0, 0.5, true}, {2000.0, 2300.0, 0.25, true}, {2000.0, 2300.0, 0.5, false}};
    const double linear_limit_dbc = -40.0;
    const double overdriven_floor_dbc = -30.0;
    for (size_t i = 0; i < count_of(cases); ++i) {
        const ChannelConfig config = quiet(Mode::fm);
        const double f1 = cases[i].f1_hz;
        const double f2 = cases[i].f2_hz;
        const double level = cases[i].level_fraction * config.signal_level;
        const std::vector<float> out = run(config, sum(tone(f1, level, tone_s), tone(f2, level, tone_s)));
        const double carrier = settled_amplitude(out, f1);
        const double im3 = std::max(settled_amplitude(out, 2.0 * f1 - f2), settled_amplitude(out, 2.0 * f2 - f1));
        const double im3_dbc = amplitude_db(im3 / carrier);
        NOTE("%.0f + %.0f Hz at %.2f x signal_level each: IM3 %.1f dBc", f1, f2, cases[i].level_fraction, im3_dbc);
        if (cases[i].linear)
            CHECK(im3_dbc < linear_limit_dbc);
        else
            CHECK(im3_dbc > overdriven_floor_dbc);
    }
}

TEST(channel_am_noise_density) {
    // Above threshold the envelope detector passes the in-phase noise:
    // one-sided density N0 * (signal_level / (C m))^2 with N0 = C^2 / (snr * 2500).
    ChannelConfig config;
    config.mode = Mode::am;
    config.snr_db = 30.0;
    const double noise_s = 20.0;
    const double band_low_hz = 600.0;
    const double band_high_hz = 2400.0;
    const double tolerance_db = 0.5;
    const double measured = noise_density(run(config, silence(noise_s)), band_low_hz, band_high_hz);
    const double m = config.am_modulation_index;
    const double expected =
        config.signal_level * config.signal_level / (m * m * db_to_power(config.snr_db) * reference_bandwidth_hz);
    NOTE("am audio noise density %.4g, expected %.4g (%.3f dB)", measured, expected, power_db(measured / expected));
    CHECK_NEAR(power_db(measured / expected), 0.0, tolerance_db);
}

TEST(channel_fm_noise_density) {
    // Above threshold the discriminator noise is parabolic: one-sided density
    // (signal_level / deviation)^2 * f^2 * N0 / C^2 with N0 = C^2 / (snr * 2500).
    ChannelConfig config;
    config.mode = Mode::fm;
    config.snr_db = 40.0;
    config.fm_rx_deemphasis = false;
    const double noise_s = 20.0;
    const double half_band_hz = 50.0;
    const double tolerance_db = 0.5;
    const std::vector<float> out = run(config, silence(noise_s));
    const double frequencies[] = {1000.0, 2000.0};
    const double scale = config.signal_level / config.fm_deviation_hz;
    double densities[count_of(frequencies)];
    for (size_t i = 0; i < count_of(frequencies); ++i) {
        const double f = frequencies[i];
        densities[i] = noise_density(out, f - half_band_hz, f + half_band_hz);
        const double expected = scale * scale * f * f / (db_to_power(config.snr_db) * reference_bandwidth_hz);
        NOTE("fm noise density at %.0f Hz: %.4g, expected %.4g (%.3f dB)", f, densities[i], expected,
             power_db(densities[i] / expected));
        CHECK_NEAR(power_db(densities[i] / expected), 0.0, tolerance_db);
    }
    const double ratio = frequencies[1] / frequencies[0];
    CHECK_NEAR(power_db(densities[1] / densities[0]), power_db(ratio * ratio), tolerance_db);
}

TEST(channel_fm_cnr_helper) {
    const double snr_db = 20.0;
    const double exact = 1e-9;
    ChannelConfig config;
    config.snr_db = snr_db;
    CHECK_NEAR(fm_cnr_db(config), snr_db + power_db(reference_bandwidth_hz / config.fm_if_bandwidth_hz), exact);
    config.fm_if_bandwidth_hz = reference_bandwidth_hz;
    CHECK_NEAR(fm_cnr_db(config), snr_db, exact);
}

TEST(channel_fm_threshold_clicks) {
    // Unmodulated carrier: above threshold the output is faint hiss; below it
    // 2 pi phase slips give large clicks (Rice). A click (one cycle of
    // frequency area) through de-emphasis and the audio filter peaks at about
    // 1-1.5 x signal_level; the threshold sits well above the hiss.
    const double cnrs[] = {4.0, 8.0, 12.0, 25.0};
    const size_t cases = count_of(cnrs);
    const double duration_s = 10.0;
    const size_t min_clicks_far_below_threshold = 100;
    size_t clicks[cases];
    for (size_t i = 0; i < cases; ++i) {
        ChannelConfig config;
        config.mode = Mode::fm;
        config.snr_db = cnrs[i] - power_db(reference_bandwidth_hz / config.fm_if_bandwidth_hz);
        const std::vector<float> out = run(config, silence(duration_s));
        clicks[i] = count_clicks(out, config.signal_level);
        const double noise_rms = rms(out, settle_s, duration_s - settle_s);
        NOTE("cnr %.0f dB: %zu clicks in %.0f s, max |y| %.3f, rms %.4f", fm_cnr_db(config), clicks[i], duration_s,
             max_abs(out, samples(settle_s)), noise_rms);
    }
    CHECK(clicks[0] > min_clicks_far_below_threshold);
    CHECK(clicks[0] > clicks[1]);
    CHECK(clicks[1] > 0);
    CHECK_EQ(clicks[cases - 1], static_cast<size_t>(0));

    // Modulation raises the click rate (Rice: + mean |deviation| * exp(-cnr)).
    const double freq = 1000.0;
    ChannelConfig config;
    config.mode = Mode::fm;
    config.snr_db = cnrs[1] - power_db(reference_bandwidth_hz / config.fm_if_bandwidth_hz);
    const std::vector<float> input = tone(freq, config.signal_level, duration_s);
    const std::vector<float> noisy = run(config, input);
    config.noise = false;
    const std::vector<float> clean = run(config, input);
    std::vector<float> residual(noisy.size());
    for (size_t n = 0; n < noisy.size(); ++n) residual[n] = noisy[n] - clean[n];
    const size_t modulated = count_clicks(residual, config.signal_level);
    NOTE("cnr %.0f dB with a 1 kHz key-down tone: %zu clicks (unmodulated %zu)", cnrs[1], modulated, clicks[1]);
    CHECK(modulated > clicks[1]);
}

// ---------------------------------------------------------------------------
// fading, impulses
// ---------------------------------------------------------------------------

TEST(channel_fading_rayleigh_statistics) {
    ChannelConfig config = quiet(Mode::usb);
    config.fading = true;
    config.doppler_spread_hz = 2.0;
    config.path_delay_ms = 0.0;
    const double freq = 1500.0;
    const double duration_s = 200.0;
    const double crossing_rate_tolerance = 0.25;  // relative
    const std::vector<float> out = run(config, tone(freq, config.signal_level, duration_s));
    const std::vector<double> gain = block_gains(out, freq, config.signal_level);
    const double average = mean(gain);

    size_t crossings = 0;
    for (size_t i = 1; i < gain.size(); ++i)
        if (gain[i - 1] < average && gain[i] >= average) ++crossings;
    const double fraction = fraction_below(gain, deep_fade);
    // Rice: upward crossings of the rms level = 2 sqrt(pi) sigma_f / e for a
    // Gaussian Doppler spectrum with sigma_f = spread / 2.
    const double sigma_f = config.doppler_spread_hz / 2.0;
    const double expected_rate = 2.0 * std::sqrt(pi) * sigma_f * std::exp(-1.0);
    const double measured_rate = crossings / (gain.size() * block_s);
    NOTE("mean gain %.3f dB, below -10 dB %.3f (Rayleigh %.3f), crossings %.3f/s (theory %.3f/s)", power_db(average),
         fraction, rayleigh_deep_fraction, measured_rate, expected_rate);
    CHECK_NEAR(power_db(average), 0.0, mean_gain_tolerance_db);
    CHECK_NEAR(fraction, rayleigh_deep_fraction, deep_fraction_tolerance);
    CHECK_NEAR(measured_rate / expected_rate, 1.0, crossing_rate_tolerance);
}

TEST(channel_fading_two_path) {
    // Paths 1 ms apart: H(f) = g1 + g2 exp(-j 2 pi f 1 ms), so H(1000) =
    // H(2000) = g1 + g2 and H(1500) = g1 - g2. With path powers P1 + P2 = 1:
    //   corr(|H(1000)|^2, |H(1500)|^2) = (P1 - P2)^2 (zero for equal paths),
    //   E[H(1000) conj(H(1500))] = P1 - P2 (its sign tells which path is delayed),
    //   H(1000) and H(2000) fade together.
    const double path2_gains_db[] = {0.0, -6.0};
    const double duration_s = 200.0;
    const double frequencies[] = {1000.0, 1500.0, 2000.0};
    const size_t tones = count_of(frequencies);
    const double correlation_tolerance = 0.12;
    const double cross_tolerance = 0.12;
    const double together = 0.9;

    ChannelConfig config = quiet(Mode::usb);
    config.doppler_spread_hz = 2.0;
    config.path_delay_ms = 1.0;
    const double level = config.signal_level / tones;
    std::vector<float> input(samples(duration_s), 0.0f);
    for (size_t i = 0; i < tones; ++i) input = sum(input, tone(frequencies[i], level, duration_s));

    // Without fading every block has the same phasor: the receiver's own response.
    const std::vector<float> steady = run(config, input);
    Complex reference[tones];
    for (size_t i = 0; i < tones; ++i) reference[i] = block_phasors(steady, frequencies[i])[0];
    const Complex reference_cross = reference[0] * std::conj(reference[1]);

    config.fading = true;
    for (size_t p = 0; p < count_of(path2_gains_db); ++p) {
        config.path2_gain_db = path2_gains_db[p];
        const double ratio = db_to_power(config.path2_gain_db);
        const double p1 = 1.0 / (1.0 + ratio);
        const double p2 = ratio / (1.0 + ratio);
        const std::vector<float> out = run(config, input);

        std::vector<double> gains[tones];
        for (size_t i = 0; i < tones; ++i) {
            gains[i] = block_gains(out, frequencies[i], level);
            NOTE("path 2 at %.0f dB: %.0f Hz mean gain %.3f dB", config.path2_gain_db, frequencies[i],
                 power_db(mean(gains[i])));
            CHECK_NEAR(power_db(mean(gains[i])), 0.0, mean_gain_tolerance_db);
        }
        const double half_period = correlation(gains[0], gains[1]);
        const double full_period = correlation(gains[0], gains[2]);

        const std::vector<Complex> h0 = block_phasors(out, frequencies[0]);
        const std::vector<Complex> h1 = block_phasors(out, frequencies[1]);
        Complex cross;
        for (size_t b = 0; b < h0.size(); ++b) cross += h0[b] * std::conj(h1[b]);
        cross /= static_cast<double>(h0.size()) * reference_cross;

        NOTE("power correlation 1000/1500 Hz %.3f (theory %.3f), 1000/2000 Hz %.3f", half_period,
             (p1 - p2) * (p1 - p2), full_period);
        NOTE("E[H(1000) H*(1500)] = %.3f %+.3fj (theory %.3f)", cross.real(), cross.imag(), p1 - p2);
        CHECK_NEAR(half_period, (p1 - p2) * (p1 - p2), correlation_tolerance);
        CHECK(full_period > together);
        CHECK_NEAR(cross.real(), p1 - p2, cross_tolerance);
        CHECK_NEAR(cross.imag(), 0.0, cross_tolerance);
    }
}

TEST(channel_am_fading) {
    // Flat Rayleigh fading scales the AM carrier and sidebands together: the
    // detected audio follows |g| with unit mean power gain.
    ChannelConfig config = quiet(Mode::am);
    config.fading = true;
    config.doppler_spread_hz = 5.0;
    config.path_delay_ms = 0.0;
    const double freq = 1000.0;
    const double duration_s = 30.0;
    const std::vector<double> gain = block_gains(run(config, tone(freq, config.signal_level, duration_s)), freq,
                                                 config.signal_level);
    const double fraction = fraction_below(gain, deep_fade);
    NOTE("am mean gain %.3f dB, below -10 dB %.3f (Rayleigh %.3f)", power_db(mean(gain)), fraction,
         rayleigh_deep_fraction);
    CHECK_NEAR(power_db(mean(gain)), 0.0, mean_gain_tolerance_db);
    CHECK_NEAR(fraction, rayleigh_deep_fraction, deep_fraction_tolerance);
}

TEST(channel_fm_fading) {
    // The FM limiter hides fading: without noise the tone keeps its level.
    // With noise, fades drop the instantaneous CNR below threshold and clicks
    // appear at an average CNR that is click-free without fading.
    ChannelConfig config = quiet(Mode::fm);
    config.fading = true;
    config.doppler_spread_hz = 5.0;
    config.path_delay_ms = 0.0;
    const double freq = 1000.0;
    const double tone_gain_tolerance_db = 0.5;
    const double tone_gain = gain_db(config, freq, config.signal_level);

    const double cnr_db = 20.0;
    const double duration_s = 10.0;
    config.noise = true;
    config.snr_db = cnr_db - power_db(reference_bandwidth_hz / config.fm_if_bandwidth_hz);
    const size_t faded = count_clicks(run(config, silence(duration_s)), config.signal_level);
    config.fading = false;
    const size_t steady = count_clicks(run(config, silence(duration_s)), config.signal_level);
    NOTE("fm tone gain under fading %.3f dB; cnr %.0f dB: %zu clicks faded, %zu steady", tone_gain, cnr_db, faded,
         steady);
    CHECK_NEAR(tone_gain, 0.0, tone_gain_tolerance_db);
    CHECK_EQ(steady, static_cast<size_t>(0));
    CHECK(faded > 0);
}

TEST(channel_impulse_poisson) {
    ChannelConfig config = quiet(Mode::usb);
    config.impulse_rate_hz = 3.0;
    config.impulse_level_db = 20.0;
    const double duration_s = 200.0;
    const std::vector<float> out = run(config, silence(duration_s));

    // Area-preserving spike at 8 kHz, then the receiver passband: envelope peak = spike * bandwidth / rate.
    const double spike =
        config.signal_level * db_to_amplitude(config.impulse_level_db) * rate / impulse_reference_rate_hz;
    const double bandwidth_hz = config.rx_high_hz - config.rx_low_hz;
    const double envelope_peak = spike * bandwidth_hz / rate;
    const double expected_peak_db =
        amplitude_db(median_sampled_peak(bandwidth_hz, (config.rx_high_hz + config.rx_low_hz) / 2.0));
    const double detection_fraction = 0.35;  // of envelope_peak
    const double dead_time_s = 0.005;  // longer than one filtered impulse
    const size_t dead_time = samples(dead_time_s);
    const std::vector<size_t> events = detect_events(out, 0, detection_fraction * envelope_peak, dead_time);

    const double count_sigmas = 4.0;
    const double interval_tolerance = 0.06;
    const double level_tolerance_db = 0.5;
    const double random_sign_fraction = 0.5;  // RF phase uniform: the peak is as often negative as positive
    const double sign_tolerance = 0.1;

    const double expected = config.impulse_rate_hz * duration_s;
    const double mean_interval = rate / config.impulse_rate_hz;
    size_t long_intervals = 0;
    for (size_t i = 1; i < events.size(); ++i)
        if (events[i] - events[i - 1] > mean_interval) ++long_intervals;
    const double long_fraction = static_cast<double>(long_intervals) / (events.size() - 1);

    // Each event's largest sample: its size checks impulse_level_db, its sign the random RF phase.
    std::vector<double> event_peaks;
    size_t positive = 0;
    for (size_t i = 0; i < events.size(); ++i) {
        size_t best = events[i];
        for (size_t n = events[i]; n < std::min(out.size(), events[i] + dead_time); ++n)
            if (std::fabs(out[n]) > std::fabs(out[best])) best = n;
        event_peaks.push_back(std::fabs(out[best]));
        if (out[best] > 0.0f) ++positive;
    }
    const double level_db = amplitude_db(median(event_peaks) / envelope_peak);
    const double positive_fraction = static_cast<double>(positive) / events.size();
    NOTE("impulses %zu (expected %.0f), median peak %.3f dB re envelope %.3f (sampled-sinc model %.3f dB), "
         "positive %.3f",
         events.size(), expected, level_db, envelope_peak, expected_peak_db, positive_fraction);
    NOTE("P(interval > mean interval) %.3f (exponential: %.3f)", long_fraction, std::exp(-1.0));
    CHECK_NEAR(static_cast<double>(events.size()), expected, count_sigmas * std::sqrt(expected));
    CHECK_NEAR(long_fraction, std::exp(-1.0), interval_tolerance);
    CHECK_NEAR(level_db, expected_peak_db, level_tolerance_db);
    CHECK_NEAR(positive_fraction, random_sign_fraction, sign_tolerance);
}

// ---------------------------------------------------------------------------
// AGC
// ---------------------------------------------------------------------------

TEST(channel_agc_levels_and_time_constants) {
    ChannelConfig config = quiet(Mode::usb);
    config.agc = true;
    const double target = config.agc_target;
    const double quiet_level = 0.05;
    const double loud_level = 0.5;
    const double step_up_s = 2.0;
    const double step_down_s = 3.5;
    const double end_s = 6.5;
    const double freq = 1000.0;
    const double window_s = 0.001;        // one cycle
    const double steady_span_s = 0.15;    // steady level measured over this span...
    const double steady_margin_s = 0.05;  // ...ending this long before the next step
    const double level_tolerance_db = 0.5;

    std::vector<float> input(samples(end_s));
    for (size_t n = 0; n < input.size(); ++n) {
        const double t = n / rate;
        const double level = (t >= step_up_s && t < step_down_s) ? loud_level : quiet_level;
        input[n] = static_cast<float>(level * std::sin(two_pi * freq * t));
    }
    const std::vector<double> peaks = window_peaks(run(config, input), samples(window_s));
    const auto window_at = [&](double t) -> size_t { return static_cast<size_t>(std::lround(t / window_s)); };
    const auto steady_db = [&](double before_s) -> double {
        const size_t end = window_at(before_s - steady_margin_s);
        double peak = 0.0;
        for (size_t w = end - window_at(steady_span_s); w < end; ++w) peak = std::max(peak, peaks[w]);
        return amplitude_db(peak / target);
    };
    const double quiet_db = steady_db(step_up_s);
    const double loud_db = steady_db(step_down_s);
    const double end_db = steady_db(end_s);
    NOTE("output peak vs target: %.3f dB (input %.2f), %.3f dB (input %.2f), %.3f dB (after decay)", quiet_db,
         quiet_level, loud_db, loud_level, end_db);
    CHECK_NEAR(quiet_db, 0.0, level_tolerance_db);
    CHECK_NEAR(loud_db, 0.0, level_tolerance_db);
    CHECK_NEAR(end_db, 0.0, level_tolerance_db);

    // Attack: the step up overshoots, then the level detector charges
    // exponentially from quiet to loud; the output is within settle_db of the
    // target once the detector reaches loud / 10^(settle_db / 20).
    const double overshoot_threshold = 2.0;  // x target: marks the step's arrival at the output
    const double min_overshoot = 3.0;
    const double settle_db = 1.0;
    const double attack_tolerance_s = 2.0 * window_s;
    size_t up = window_at(step_up_s);
    while (up < peaks.size() && peaks[up] < overshoot_threshold * target) ++up;
    REQUIRE(up + 1 < peaks.size());
    const double overshoot = std::max(peaks[up], peaks[up + 1]) / target;
    size_t settled = up;
    while (settled < peaks.size() && amplitude_db(peaks[settled] / target) > settle_db) ++settled;
    REQUIRE(settled < peaks.size());
    const double attack_s = (settled - up) * window_s;
    const double expected_attack_s = config.agc_attack_ms / ms_per_s *
                                     std::log((loud_level - quiet_level) /
                                              (loud_level - loud_level / db_to_amplitude(settle_db)));
    NOTE("attack: overshoot x%.2f, within %.0f dB after %.1f ms (expected %.1f ms)", overshoot, settle_db,
         attack_s * ms_per_s, expected_attack_s * ms_per_s);
    CHECK(overshoot > min_overshoot);
    CHECK_NEAR(attack_s, expected_attack_s, attack_tolerance_s);

    // Decay: after the step down the output is quiet * target / level(t) with
    // level(t) = loud * exp(-t / decay), so it is back to a fraction f of the
    // target after decay * ln(loud * f / quiet).
    const double dropped_fraction = 0.3;    // x target: marks the step's arrival at the output
    const double recovered_fraction = 0.5;  // x target
    const double decay_tolerance = 0.15;    // relative
    size_t down = window_at(step_down_s);
    while (down < peaks.size() && peaks[down] > dropped_fraction * target) ++down;
    REQUIRE(down < peaks.size());
    size_t recovered = down;
    while (recovered < peaks.size() && peaks[recovered] < recovered_fraction * target) ++recovered;
    REQUIRE(recovered < peaks.size());
    const double decay_s = (recovered - down) * window_s;
    const double expected_decay_s =
        config.agc_decay_ms / ms_per_s * std::log(loud_level * recovered_fraction / quiet_level);
    NOTE("decay: half target after %.3f s, expected %.3f s (decay %.0f ms)", decay_s, expected_decay_s,
         config.agc_decay_ms);
    CHECK_NEAR(decay_s / expected_decay_s, 1.0, decay_tolerance);
}

TEST(channel_agc_level_does_not_depend_on_sample_phase) {
    // At a quarter of the sample rate every sample falls on the same four points
    // of the waveform, which hit the crest only at some start phases. The AGC
    // detector follows the envelope, so the output level is the same for all.
    ChannelConfig config = quiet(Mode::usb);
    config.agc = true;
    const double freq = rate / 4.0;
    const double level = 0.2;
    const size_t phases = 16;
    const double tolerance_db = 0.2;
    double worst_db = 0.0;
    for (size_t p = 0; p < phases; ++p) {
        const std::vector<float> out = run(config, tone(freq, level, tone_s, rate, two_pi * p / phases));
        const double output_db = amplitude_db(settled_amplitude(out, freq) / config.agc_target);
        if (std::fabs(output_db) > std::fabs(worst_db)) worst_db = output_db;
    }
    NOTE("%.0f Hz tone: worst output level vs target over %zu start phases %.3f dB", freq, phases, worst_db);
    CHECK_NEAR(worst_db, 0.0, tolerance_db);
}

TEST(channel_agc_amplifies_noise_between_bursts) {
    ChannelConfig config;
    config.mode = Mode::usb;
    config.snr_db = 20.0;
    config.agc = true;
    const double freq = 1500.0;
    const double burst_s = 1.0;
    const double total_s = 5.0;
    const double after_s = 1.1;
    const double after_span_s = 0.2;
    const double late_s = 4.5;
    const double late_span_s = 0.5;
    const double min_rise = 2.0;                // late noise at least this much louder than just after the burst
    const double min_late_fraction = 0.1;       // of agc_target
    std::vector<float> input = tone(freq, config.signal_level, burst_s);
    input.resize(samples(total_s), 0.0f);
    const std::vector<float> out = run(config, input);
    const double just_after = rms(out, after_s, after_span_s);
    const double late = rms(out, late_s, late_span_s);
    NOTE("noise rms after the burst %.4f, %.1f s later %.4f (target %.2f)", just_after, late_s - after_s, late,
         config.agc_target);
    CHECK(late > min_rise * just_after);
    CHECK(late > min_late_fraction * config.agc_target);
}

TEST(channel_am_agc_follows_the_carrier) {
    // The AM AGC works on the carrier: it keeps the modulation depth (a tone at
    // half level comes out at half the target) and compensates carrier fading.
    ChannelConfig config = quiet(Mode::am);
    config.agc = true;
    config.agc_target = 0.25;  // differs from signal_level so the AGC has to act
    const double freq = 1000.0;
    const double level = config.signal_level;
    const double half = 0.5;
    const double tolerance_db = 0.5;
    const double key_down_db =
        amplitude_db(settled_amplitude(run(config, tone(freq, level, tone_s)), freq) / config.agc_target);
    const double half_db = amplitude_db(settled_amplitude(run(config, tone(freq, half * level, tone_s)), freq) /
                                        (half * config.agc_target));
    NOTE("key-down tone %.3f dB re target, half-level tone %.3f dB re half target", key_down_db, half_db);
    CHECK_NEAR(key_down_db, 0.0, tolerance_db);
    CHECK_NEAR(half_db, 0.0, tolerance_db);

    // A frozen flat fade (one random draw of the tap) scales the received
    // carrier; these seeds draw one fade down and one fade up.
    const std::uint32_t faded_seeds[] = {10, 11};
    const double min_fade_db = 3.0;
    config.fading = true;
    config.doppler_spread_hz = 0.0;
    config.path_delay_ms = 0.0;
    for (size_t i = 0; i < count_of(faded_seeds); ++i) {
        config.seed = faded_seeds[i];
        ChannelConfig without_agc = config;
        without_agc.agc = false;
        const double faded_db = gain_db(without_agc, freq, level);
        const double compensated_db =
            amplitude_db(settled_amplitude(run(config, tone(freq, level, tone_s)), freq) / config.agc_target);
        NOTE("frozen fade: %.3f dB without AGC, %.3f dB re target with AGC", faded_db, compensated_db);
        REQUIRE(std::fabs(faded_db) > min_fade_db);
        CHECK_NEAR(compensated_db, 0.0, tolerance_db);
    }
}

TEST(channel_am_agc_does_not_pump_noise) {
    // The carrier stays on between bursts, so the AM AGC holds its gain and the
    // noise stays at N0 (signal_level / (C m))^2 over the audio passband.
    ChannelConfig config;
    config.mode = Mode::am;
    config.snr_db = 20.0;
    config.agc = true;
    const double freq = 1500.0;
    const double burst_s = 1.0;
    const double total_s = 5.0;
    const double after_s = 1.1;
    const double after_span_s = 0.2;
    const double late_s = 4.5;
    const double late_span_s = 0.5;
    const double tolerance_db = 1.0;
    std::vector<float> input = tone(freq, config.signal_level, burst_s);
    input.resize(samples(total_s), 0.0f);
    const std::vector<float> out = run(config, input);
    const double just_after = rms(out, after_s, after_span_s);
    const double late = rms(out, late_s, late_span_s);

    const double m = config.am_modulation_index;
    const double density =
        config.signal_level * config.signal_level / (m * m * db_to_power(config.snr_db) * reference_bandwidth_hz);
    const double expected = std::sqrt(density * (config.rx_high_hz - config.rx_low_hz)) * config.agc_target /
                            config.signal_level;
    NOTE("noise rms after the burst %.4f, %.1f s later %.4f, expected %.4f", just_after, late_s - after_s, late,
         expected);
    CHECK_NEAR(amplitude_db(late / just_after), 0.0, tolerance_db);
    CHECK_NEAR(amplitude_db(late / expected), 0.0, tolerance_db);
}

// ---------------------------------------------------------------------------
// configuration, streaming, determinism, other rates
// ---------------------------------------------------------------------------

TEST(channel_rejects_invalid_config) {
    struct Change {
        const char* name;
        Mode mode;
        void (*apply)(ChannelConfig&);
    };
    const Change invalid[] = {
        {"sample_rate 0", Mode::clean, [](ChannelConfig& c) { c.sample_rate = 0.0; }},
        {"sample_rate 0", Mode::usb, [](ChannelConfig& c) { c.sample_rate = 0.0; }},
        {"sample_rate NaN", Mode::am, [](ChannelConfig& c) { c.sample_rate = not_a_number; }},
        {"signal_level 0", Mode::fm, [](ChannelConfig& c) { c.signal_level = 0.0; }},
        {"snr_db NaN", Mode::usb, [](ChannelConfig& c) { c.snr_db = not_a_number; }},
        {"rx_low_hz = rx_high_hz", Mode::usb, [](ChannelConfig& c) { c.rx_low_hz = c.rx_high_hz; }},
        {"rx_low_hz < 0", Mode::lsb, [](ChannelConfig& c) { c.rx_low_hz = -1.0; }},
        {"rx_low_hz above Nyquist", Mode::am,
         [](ChannelConfig& c) {
             c.rx_low_hz = c.sample_rate / 2.0;
             c.rx_high_hz = c.sample_rate;
         }},
        {"rx_low_hz = fm_audio_high_hz", Mode::fm, [](ChannelConfig& c) { c.rx_low_hz = c.fm_audio_high_hz; }},
        {"doppler_spread_hz < 0", Mode::usb,
         [](ChannelConfig& c) {
             c.fading = true;
             c.doppler_spread_hz = -1.0;
         }},
        {"path_delay_ms NaN", Mode::am,
         [](ChannelConfig& c) {
             c.fading = true;
             c.path_delay_ms = not_a_number;
         }},
        {"impulse_rate_hz < 0", Mode::usb, [](ChannelConfig& c) { c.impulse_rate_hz = -1.0; }},
        {"agc_target 0", Mode::lsb,
         [](ChannelConfig& c) {
             c.agc = true;
             c.agc_target = 0.0;
         }},
        {"agc_attack_ms 0", Mode::am,
         [](ChannelConfig& c) {
             c.agc = true;
             c.agc_attack_ms = 0.0;
         }},
        {"am_modulation_index 0", Mode::am, [](ChannelConfig& c) { c.am_modulation_index = 0.0; }},
        {"am_if_bandwidth_hz 0", Mode::am, [](ChannelConfig& c) { c.am_if_bandwidth_hz = 0.0; }},
        {"fm_deviation_hz 0", Mode::fm, [](ChannelConfig& c) { c.fm_deviation_hz = 0.0; }},
        {"fm_max_deviation_hz < 0", Mode::fm, [](ChannelConfig& c) { c.fm_max_deviation_hz = -1.0; }},
        {"fm_if_bandwidth_hz 0", Mode::fm, [](ChannelConfig& c) { c.fm_if_bandwidth_hz = 0.0; }},
        {"fm_emphasis_us < 0", Mode::fm, [](ChannelConfig& c) { c.fm_emphasis_us = -1.0; }},
        {"carrier_hz < 0", Mode::usb, [](ChannelConfig& c) { c.carrier_hz = -1.0; }},
        {"carrier_hz at Nyquist", Mode::lsb, [](ChannelConfig& c) { c.carrier_hz = c.sample_rate / 2.0; }},
        {"carrier_db NaN", Mode::am,
         [](ChannelConfig& c) {
             c.carrier_hz = 1000.0;
             c.carrier_db = not_a_number;
         }},
        {"cw_hz above Nyquist", Mode::fm, [](ChannelConfig& c) { c.cw_hz = c.sample_rate; }},
        {"cw_wpm 0", Mode::usb,
         [](ChannelConfig& c) {
             c.cw_hz = 1000.0;
             c.cw_wpm = 0.0;
         }},
        {"qsb_depth_db < 0", Mode::usb, [](ChannelConfig& c) { c.qsb_depth_db = -1.0; }},
        {"qsb_rate_hz NaN", Mode::am, [](ChannelConfig& c) { c.qsb_rate_hz = not_a_number; }},
        {"clock_ppm NaN", Mode::usb, [](ChannelConfig& c) { c.clock_ppm = not_a_number; }},
        {"clock_ppm beyond 10 %", Mode::fm, [](ChannelConfig& c) { c.clock_ppm = 2e5; }},
    };
    // Parameters a mode does not use are not checked.
    const Change valid[] = {
        {"defaults", Mode::clean, [](ChannelConfig&) {}},
        {"defaults", Mode::usb, [](ChannelConfig&) {}},
        {"defaults", Mode::lsb, [](ChannelConfig&) {}},
        {"defaults", Mode::am, [](ChannelConfig&) {}},
        {"defaults", Mode::fm, [](ChannelConfig&) {}},
        {"unused signal_level 0", Mode::clean, [](ChannelConfig& c) { c.signal_level = 0.0; }},
        {"unused am_if_bandwidth_hz 0", Mode::usb, [](ChannelConfig& c) { c.am_if_bandwidth_hz = 0.0; }},
        {"unused agc_target 0", Mode::fm,
         [](ChannelConfig& c) {
             c.agc = true;
             c.agc_target = 0.0;
         }},
        {"fm limiter off", Mode::fm, [](ChannelConfig& c) { c.fm_max_deviation_hz = 0.0; }},
        {"unused cw_wpm 0", Mode::usb, [](ChannelConfig& c) { c.cw_wpm = 0.0; }},
        {"unused clock_ppm NaN", Mode::clean, [](ChannelConfig& c) { c.clock_ppm = not_a_number; }},
        {"clock_ppm at 10 %", Mode::lsb, [](ChannelConfig& c) { c.clock_ppm = -1e5; }},
    };
    for (size_t i = 0; i < count_of(invalid); ++i) {
        ChannelConfig config;
        config.mode = invalid[i].mode;
        invalid[i].apply(config);
        if (!CHECK(rejected(config))) NOTE("%s: %s accepted", mode_name(invalid[i].mode), invalid[i].name);
    }
    for (size_t i = 0; i < count_of(valid); ++i) {
        ChannelConfig config;
        config.mode = valid[i].mode;
        valid[i].apply(config);
        if (!CHECK(!rejected(config))) NOTE("%s: %s rejected", mode_name(valid[i].mode), valid[i].name);
    }
}

TEST(channel_chunking_invariance) {
    const size_t max_chunk = 700;
    const double freq = 1000.0;
    const double level = 0.4;
    const double noise_sigma = 0.1;
    const double duration_s = 2.0;
    const std::uint32_t input_seed = 123;
    std::mt19937 rng(input_seed);
    Normal normal(0.0, noise_sigma);
    std::vector<float> input = tone(freq, level, duration_s);
    for (size_t n = 0; n < input.size(); ++n) input[n] += static_cast<float>(normal(rng));

    for (size_t m = 0; m < count_of(all_modes); ++m) {
        ChannelConfig config;
        config.mode = all_modes[m];
        config.snr_db = 10.0;
        config.fading = true;
        config.doppler_spread_hz = 1.0;
        config.impulse_rate_hz = 5.0;
        config.agc = true;
        config.freq_offset_hz = 37.0;
        config.seed = 7;
        const std::vector<float> one_shot = run(config, input);

        Channel channel(config);
        std::vector<float> chunked(input.size());
        UniformInteger<size_t> chunk_size(0, max_chunk);
        size_t pos = 0;
        while (pos < input.size()) {
            const size_t count = std::min(chunk_size(rng), input.size() - pos);
            channel.process(&input[pos], &chunked[pos], count);
            pos += count;
        }
        const bool identical = std::memcmp(one_shot.data(), chunked.data(), one_shot.size() * sizeof(float)) == 0;
        if (!CHECK(identical)) NOTE("mode %s differs", mode_name(all_modes[m]));
    }
}

TEST(channel_reset_and_seed) {
    ChannelConfig config;
    config.mode = Mode::usb;
    config.fading = true;
    config.impulse_rate_hz = 2.0;
    const double freq = 1500.0;
    const double duration_s = 1.0;
    const std::uint32_t other_seed = 2;
    const std::vector<float> input = tone(freq, config.signal_level, duration_s);
    Channel channel(config);
    const std::vector<float> first = channel.process(input);
    const std::vector<float> continued = channel.process(input);
    channel.reset();
    CHECK(channel.process(input) == first);
    CHECK(continued != first);
    Channel moved(std::move(channel));
    CHECK(moved.process(input) == continued);
    CHECK(run(config, input) == first);
    config.seed = other_seed;
    CHECK(run(config, input) != first);
}

// Spec 12.8: a seed gives the same draws on every system. These values were drawn on macOS, where the draws equal
// libc++'s standard distributions bit for bit; Linux (libstdc++) must draw the same. A real value may differ in its
// last bits where a processor fuses a multiply and an add; whole numbers and the engine's next output, never.
TEST(channel_draws_repeat_on_every_system) {
    const std::uint32_t seed = 2026;
    const double same_value = 1e-12;
    {
        std::mt19937 engine(seed);
        Normal normal;
        const double expected[] = {-0.18422739243031372, 0.046741668197978994, 0.93427996117398993,
                                   -0.67169979055848195};
        for (size_t i = 0; i < count_of(expected); ++i) CHECK_NEAR(normal(engine), expected[i], same_value);
        CHECK_EQ(engine(), 2397665997u);
    }
    {
        std::mt19937 engine(seed);
        const UniformReal uniform(-40.0, 40.0);
        const double expected[] = {21.328723546477882, 36.895083047691948, 37.029539607030067};
        for (size_t i = 0; i < count_of(expected); ++i) CHECK_NEAR(uniform(engine), expected[i], same_value);
        CHECK_EQ(engine(), 381818397u);
    }
    {
        std::mt19937 engine(seed);
        const Exponential exponential(0.5);
        const double expected[] = {2.9100806204548517, 6.4980793321695245, 6.5866193588095685};
        for (size_t i = 0; i < count_of(expected); ++i) CHECK_NEAR(exponential(engine), expected[i], same_value);
        CHECK_EQ(engine(), 381818397u);
    }
    {
        std::mt19937 engine(seed);
        const Gamma gamma(8.0, 1.0 / 8.0);
        const double expected[] = {1.254898334222482, 1.307641473030954, 1.0398713399249493};
        for (size_t i = 0; i < count_of(expected); ++i) CHECK_NEAR(gamma(engine), expected[i], same_value);
        CHECK_EQ(engine(), 1271169940u);
    }
    {
        std::mt19937 engine(seed);
        const UniformInteger<int> die(1, 6);
        const int expected[] = {2, 3, 1, 6, 6, 6, 5, 5, 4, 6, 5, 5};
        for (size_t i = 0; i < count_of(expected); ++i) CHECK_EQ(die(engine), expected[i]);
        CHECK_EQ(engine(), 3915786882u);
    }
    {
        std::mt19937 engine(seed);
        const UniformInteger<int> wide(-20000, 20000);
        const int expected[] = {-17695, 12134, -11014, 9624};
        for (size_t i = 0; i < count_of(expected); ++i) CHECK_EQ(wide(engine), expected[i]);
        CHECK_EQ(engine(), 4194617421u);
    }
}

TEST(channel_other_sample_rates) {
    struct Case {
        Mode mode;
        double sample_rate;
        double freq;
    };
    const Case cases[] = {{Mode::usb, 48000.0, 1500.0}, {Mode::am, 11025.0, 1000.0}, {Mode::fm, 44100.0, 1000.0}};
    const double tolerance_db = 1.0;
    for (size_t i = 0; i < count_of(cases); ++i) {
        ChannelConfig config = quiet(cases[i].mode);
        config.sample_rate = cases[i].sample_rate;
        const double gain = gain_db(config, cases[i].freq, config.signal_level);
        NOTE("%s at %.0f Hz: gain %.3f dB", mode_name(cases[i].mode), cases[i].sample_rate, gain);
        CHECK_NEAR(gain, 0.0, tolerance_db);
    }
}

// ---------------------------------------------------------------------------
// interferers, QSB, transmitter clock error, fading presets (D26)
// ---------------------------------------------------------------------------

namespace {

const double ppm = 1e-6;

struct Run {
    bool high;
    size_t length;
};

// Envelope of a tone whose period is a whole number of samples: 2 |mean of x e^(-j w n) over one period|.
std::vector<double> tone_envelope(const std::vector<float>& x, double freq_hz) {
    const size_t period = static_cast<size_t>(std::lround(rate / freq_hz));
    std::vector<double> envelope(x.size(), 0.0);
    Complex sum;
    std::vector<Complex> terms(x.size());
    for (size_t n = 0; n < x.size(); ++n) {
        terms[n] = static_cast<double>(x[n]) * std::polar(1.0, -two_pi * freq_hz * n / rate);
        sum += terms[n];
        if (n >= period) sum -= terms[n - period];
        if (n + 1 >= period) envelope[n] = 2.0 * std::abs(sum) / period;
    }
    return envelope;
}

// Median 10 % to 90 % rise time of the envelope's crossings of half the level, after start.
double median_rise_s(const std::vector<double>& envelope, double level, size_t start) {
    const double half = 0.5;
    const double low_fraction = 0.1;
    const double high_fraction = 0.9;
    std::vector<double> rises;
    for (size_t n = start + 1; n < envelope.size(); ++n) {
        if (!(envelope[n - 1] <= half * level && envelope[n] > half * level)) continue;
        size_t low = n;
        while (low > start && envelope[low] > low_fraction * level) --low;
        size_t high = n;
        while (high < envelope.size() && envelope[high] < high_fraction * level) ++high;
        if (low > start && high < envelope.size()) rises.push_back(static_cast<double>(high - low) / rate);
    }
    return rises.empty() ? not_a_number : median(rises);
}

// Lengths of the runs above and below threshold, without the first and the last (cut) run.
std::vector<Run> runs(const std::vector<double>& envelope, double threshold, size_t start) {
    std::vector<Run> all;
    for (size_t n = start; n < envelope.size(); ++n) {
        const bool high = envelope[n] > threshold;
        if (all.empty() || all.back().high != high) all.push_back(Run{high, 0});
        ++all.back().length;
    }
    if (all.size() < 2) return std::vector<Run>();
    return std::vector<Run>(all.begin() + 1, all.end() - 1);
}

std::vector<float> run_chunked(const ChannelConfig& config, const std::vector<float>& input, bool pointer_api,
                               std::mt19937& rng) {
    const size_t max_chunk = 700;
    UniformInteger<size_t> chunk_size(0, max_chunk);
    Channel channel(config);
    std::vector<float> out;
    size_t pos = 0;
    while (pos < input.size()) {
        const size_t count = std::min(chunk_size(rng), input.size() - pos);
        if (pointer_api) {
            out.resize(pos + count);
            channel.process(&input[pos], &out[pos], count);
        } else {
            const std::vector<float> part =
                channel.process(std::vector<float>(input.begin() + pos, input.begin() + pos + count));
            out.insert(out.end(), part.begin(), part.end());
        }
        pos += count;
    }
    return out;
}

}  // namespace

TEST(channel_additions_are_off_by_default) {
    // Levels and rates alone change nothing: an interferer needs a frequency, QSB a depth.
    const double freq = 1234.0;
    const double duration_s = 0.5;
    for (size_t m = 0; m < count_of(all_modes); ++m) {
        ChannelConfig config;
        config.mode = all_modes[m];
        const std::vector<float> input = tone(freq, config.signal_level, duration_s);
        ChannelConfig loud = config;
        loud.carrier_db = 20.0;
        loud.cw_db = 20.0;
        loud.cw_wpm = 40.0;
        loud.qsb_rate_hz = 3.0;
        if (!CHECK(run(loud, input) == run(config, input))) NOTE("mode %s", mode_name(all_modes[m]));
    }
}

TEST(channel_carrier_interferer_lands_in_the_audio) {
    struct Case {
        Mode mode;
        double offset_hz;
    };
    // The frequency is where the carrier lands after the receiver, whatever the sideband and mistuning.
    const Case cases[] = {{Mode::usb, 0.0}, {Mode::lsb, 100.0}, {Mode::am, 300.0}, {Mode::fm, 0.0}};
    const double carrier_hz = 1000.0;
    const double carrier_db = 6.0;
    const double search_hz = 100.0;
    const double level_tolerance_db = 0.2;
    const double frequency_tolerance_hz = 1.0;
    for (size_t i = 0; i < count_of(cases); ++i) {
        ChannelConfig config = quiet(cases[i].mode);
        config.freq_offset_hz = cases[i].offset_hz;
        config.carrier_hz = carrier_hz;
        config.carrier_db = carrier_db;
        const std::vector<float> out = run(config, silence(tone_s));
        const double level_db = amplitude_db(settled_amplitude(out, carrier_hz) / config.signal_level);
        const double peak = peak_frequency(out, carrier_hz - search_hz, carrier_hz + search_hz);
        NOTE("%s: carrier at %.0f Hz, %.3f dB re key-down tone", mode_name(cases[i].mode), peak, level_db);
        CHECK_NEAR(level_db, carrier_db, level_tolerance_db);
        CHECK_NEAR(peak, carrier_hz, frequency_tolerance_hz);
    }
}

TEST(channel_cw_interferer_keys_random_morse) {
    ChannelConfig config = quiet(Mode::usb);
    config.cw_hz = 1000.0;  // a whole number of samples per cycle, for tone_envelope
    config.cw_db = -3.0;
    config.cw_wpm = 20.0;
    const double duration_s = 20.0;
    const double paris_dot_s = 1.2;
    const double edge_s = 0.005;
    const double half = 0.5;
    const double level_tolerance_db = 0.2;
    const double length_tolerance_s = 0.002;
    const double rise_tolerance_s = 0.0003;  // the receiver filter's share
    const double key_units[] = {1.0, 3.0};       // dot, dash
    const double gap_units[] = {1.0, 3.0, 7.0};  // symbol, character, word
    const double level = config.signal_level * db_to_amplitude(config.cw_db);
    const double dot = paris_dot_s / config.cw_wpm * rate;

    const std::vector<float> out = run(config, silence(duration_s));
    const std::vector<double> envelope = tone_envelope(out, config.cw_hz);
    const size_t start = samples(settle_s);
    const double peak_db = amplitude_db(*std::max_element(envelope.begin() + start, envelope.end()) / level);
    NOTE("key-down level %.3f dB re cw_db", peak_db);
    CHECK_NEAR(peak_db, 0.0, level_tolerance_db);

    // Every key-down and key-up run, cut at half level, lasts a whole number of dots of a legal length.
    const std::vector<Run> all = runs(envelope, half * level, start);
    size_t key_counts[count_of(key_units)] = {};
    size_t gap_counts[count_of(gap_units)] = {};
    size_t illegal = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        const double* units = all[i].high ? key_units : gap_units;
        size_t* counts = all[i].high ? key_counts : gap_counts;
        const size_t kinds = all[i].high ? count_of(key_units) : count_of(gap_units);
        bool legal = false;
        for (size_t k = 0; k < kinds; ++k) {
            if (std::fabs(all[i].length - units[k] * dot) <= length_tolerance_s * rate) {
                ++counts[k];
                legal = true;
            }
        }
        if (!legal) ++illegal;
    }
    NOTE("dots %zu, dashes %zu; gaps: symbol %zu, character %zu, word %zu; other %zu", key_counts[0], key_counts[1],
         gap_counts[0], gap_counts[1], gap_counts[2], illegal);
    CHECK_EQ(illegal, static_cast<size_t>(0));
    for (size_t k = 0; k < count_of(key_counts); ++k) CHECK(key_counts[k] > 0);
    for (size_t k = 0; k < count_of(gap_counts); ++k) CHECK(gap_counts[k] > 0);

    // Raised-cosine edges: the 10 % to 90 % rise of every key-down, against a synthetic raised-cosine key-down
    // measured the same way (the one-period average of tone_envelope slows both).
    std::vector<float> reference = silence(settle_s + dot / rate);
    const size_t key_down = samples(settle_s);
    for (size_t n = key_down; n < reference.size(); ++n) {
        const double edge = std::min(1.0, (n - key_down + 1) / (edge_s * rate));
        const double key = (1.0 - std::cos(pi * edge)) / 2.0;
        reference[n] = static_cast<float>(level * key * std::cos(two_pi * config.cw_hz * n / rate));
    }
    const double expected_rise_s = median_rise_s(tone_envelope(reference, config.cw_hz), level, 0);
    const double rise_s = median_rise_s(envelope, level, start);
    NOTE("median 10-90 %% rise %.2f ms (raised cosine %.2f ms)", rise_s * ms_per_s, expected_rise_s * ms_per_s);
    CHECK_NEAR(rise_s, expected_rise_s, rise_tolerance_s);

    // The text is random per seed.
    ChannelConfig other = config;
    other.seed = config.seed + 1;
    CHECK(run(other, silence(duration_s)) != out);
}

TEST(channel_qsb_follows_a_raised_cosine) {
    const Mode modes[] = {Mode::usb, Mode::am};
    const double freq = 1500.0;  // whole cycles in every fading block
    const double depth_db = 20.0;
    const double qsb_rate_hz = 0.5;
    const double duration_s = 5.0;
    const double max_delay_s = 0.1;  // receiver filters delay the output by a few tens of ms
    const double delay_step_s = 0.001;
    const double fit_tolerance_db = 0.3;
    const double crest_tolerance_db = 0.3;
    const double trough_tolerance_db = 0.5;
    for (size_t m = 0; m < count_of(modes); ++m) {
        ChannelConfig config = quiet(modes[m]);
        config.qsb_depth_db = depth_db;
        config.qsb_rate_hz = qsb_rate_hz;
        const std::vector<double> gains =
            block_gains(run(config, tone(freq, config.signal_level, duration_s)), freq, config.signal_level);
        std::vector<double> gains_db(gains.size());
        for (size_t b = 0; b < gains.size(); ++b) gains_db[b] = power_db(gains[b]);

        double best_rms = std::numeric_limits<double>::infinity();
        double best_delay_s = 0.0;
        for (double delay_s = 0.0; delay_s <= max_delay_s; delay_s += delay_step_s) {
            double squares = 0.0;
            for (size_t b = 0; b < gains_db.size(); ++b) {
                const double t = fading_skip_s + (b + 0.5) * block_s - delay_s;
                const double expected = -depth_db * (1.0 - std::cos(two_pi * qsb_rate_hz * t)) / 2.0;
                squares += (gains_db[b] - expected) * (gains_db[b] - expected);
            }
            const double rms_db = std::sqrt(squares / gains_db.size());
            if (rms_db < best_rms) {
                best_rms = rms_db;
                best_delay_s = delay_s;
            }
        }
        const double crest_db = *std::max_element(gains_db.begin(), gains_db.end());
        const double trough_db = *std::min_element(gains_db.begin(), gains_db.end());
        NOTE("%s: crest %.3f dB, trough %.3f dB, fit rms %.3f dB at a delay of %.0f ms", mode_name(modes[m]), crest_db,
             trough_db, best_rms, best_delay_s * ms_per_s);
        CHECK(best_rms < fit_tolerance_db);
        CHECK(best_delay_s < max_delay_s);
        CHECK_NEAR(crest_db, 0.0, crest_tolerance_db);
        CHECK_NEAR(trough_db, -depth_db, trough_tolerance_db);
    }
}

TEST(channel_clock_ppm_scales_time_and_frequency) {
    const double freq = 1000.0;
    const double duration_s = 4.0;
    const double ppms[] = {1000.0, 10000.0, -5000.0};  // tones land on whole Hz
    const double search_hz = 60.0;
    const double frequency_tolerance_hz = 0.5;
    const double gain_tolerance_db = 0.2;
    const double max_held_samples = 64.0;  // resampler look-ahead not yet delivered
    for (size_t i = 0; i < count_of(ppms); ++i) {
        ChannelConfig config = quiet(Mode::usb);
        config.clock_ppm = ppms[i];
        const std::vector<float> input = tone(freq, config.signal_level, duration_s);
        const std::vector<float> out = run(config, input);
        const double expected_length = input.size() / (1.0 + config.clock_ppm * ppm);
        const double expected_hz = freq * (1.0 + config.clock_ppm * ppm);
        const double peak = peak_frequency(out, freq - search_hz, freq + search_hz);
        const double gain = amplitude_db(settled_amplitude(out, expected_hz) / config.signal_level);
        NOTE("%+.0f ppm: %zu -> %zu samples (%.1f expected), %.0f Hz -> %.0f Hz, gain %.3f dB", config.clock_ppm,
             input.size(), out.size(), expected_length, freq, peak, gain);
        CHECK(out.size() <= expected_length + 1.0);
        CHECK(out.size() + max_held_samples >= expected_length);
        CHECK_NEAR(peak, expected_hz, frequency_tolerance_hz);
        CHECK_NEAR(gain, 0.0, gain_tolerance_db);
    }

    // The pointer process() delivers one output per input, which a clock error cannot.
    ChannelConfig config = quiet(Mode::usb);
    config.clock_ppm = ppms[0];
    const std::vector<float> input = tone(freq, config.signal_level, duration_s);
    std::vector<float> out(input.size());
    Channel channel(config);
    bool thrown = false;
    try {
        channel.process(&input[0], &out[0], input.size());
    } catch (const std::logic_error&) {
        thrown = true;
    }
    CHECK(thrown);
}

TEST(channel_fading_presets) {
    struct Case {
        FadingPreset preset;
        double delay_ms;
        double doppler_hz;
    };
    const Case cases[] = {{FadingPreset::flat, 0.0, 1.0},
                          {FadingPreset::ccir_good, 0.5, 0.1},
                          {FadingPreset::ccir_moderate, 1.0, 0.5},
                          {FadingPreset::ccir_poor, 2.0, 1.0},
                          {FadingPreset::flutter, 0.5, 10.0}};
    const double unequal_paths_db = -6.0;
    for (size_t i = 0; i < count_of(cases); ++i) {
        ChannelConfig config;
        config.path2_gain_db = unequal_paths_db;
        apply_preset(config, cases[i].preset);
        CHECK(config.fading);
        CHECK_EQ(config.path_delay_ms, cases[i].delay_ms);
        CHECK_EQ(config.doppler_spread_hz, cases[i].doppler_hz);
        CHECK_EQ(config.path2_gain_db, 0.0);
        CHECK(!rejected(config));
    }
    ChannelConfig config;
    apply_preset(config, FadingPreset::ccir_poor);
    apply_preset(config, FadingPreset::none);
    CHECK(!config.fading);
}

TEST(channel_chunking_invariance_with_additions) {
    const double freq = 1000.0;
    const double level = 0.4;
    const double noise_sigma = 0.1;
    const double duration_s = 1.5;
    const double clock_ppms[] = {0.0, 250.0};  // 0: pointer process(); otherwise the vector one
    const std::uint32_t input_seed = 321;
    std::mt19937 rng(input_seed);
    Normal normal(0.0, noise_sigma);
    std::vector<float> input = tone(freq, level, duration_s);
    for (size_t n = 0; n < input.size(); ++n) input[n] += static_cast<float>(normal(rng));

    for (size_t c = 0; c < count_of(clock_ppms); ++c) {
        for (size_t m = 0; m < count_of(all_modes); ++m) {
            ChannelConfig config;
            config.mode = all_modes[m];
            config.snr_db = 10.0;
            apply_preset(config, FadingPreset::ccir_moderate);
            config.impulse_rate_hz = 5.0;
            config.agc = true;
            config.freq_offset_hz = 37.0;
            config.carrier_hz = 1700.0;
            config.carrier_db = 0.0;
            config.cw_hz = 800.0;
            config.cw_db = 0.0;
            config.cw_wpm = 40.0;
            config.qsb_depth_db = 10.0;
            config.qsb_rate_hz = 1.0;
            config.clock_ppm = clock_ppms[c];
            config.seed = 9;
            const std::vector<float> one_shot = run(config, input);
            const std::vector<float> chunked = run_chunked(config, input, clock_ppms[c] == 0.0, rng);
            const bool identical = chunked.size() == one_shot.size() &&
                                   std::memcmp(one_shot.data(), chunked.data(), one_shot.size() * sizeof(float)) == 0;
            if (!CHECK(identical)) NOTE("mode %s, clock %.0f ppm differs", mode_name(all_modes[m]), clock_ppms[c]);
        }
    }
}
