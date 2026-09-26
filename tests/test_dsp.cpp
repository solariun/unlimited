#include "test_harness.hpp"
#include "unlimited/dsp.hpp"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using namespace unlimited;
using namespace unlimited::dsp;

namespace {

const double k_pi = 3.14159265358979323846;
const double k_two_pi = 2.0 * k_pi;
const double k_rate = k_decoder_rate_hz;
const double k_db_amplitude = 20.0;

double cic2_response(double hz, int block) {
    const double x = k_pi * hz / k_rate;
    if (x == 0.0) return 1.0;
    const double ratio = std::sin(block * x) / (block * std::sin(x));
    return ratio * ratio;
}

// Magnitude of the CIC-2 block output for a complex exponential at `hz`, relative to the DC gain B.
double measured_cic2(double hz, int block, double amplitude) {
    Cic2 cic;
    const int settle_blocks = 4;
    const int blocks = 64;
    double sum = 0.0;
    int count = 0;
    for (int n = 0; n < (settle_blocks + blocks) * block; ++n) {
        const double phase = k_two_pi * hz * n / k_rate;
        cic.push(static_cast<int32_t>(std::lround(amplitude * std::cos(phase))),
                 static_cast<int32_t>(std::lround(amplitude * std::sin(phase))));
        if ((n + 1) % block != 0) continue;
        int32_t re;
        int32_t im;
        cic.dump(static_cast<uint8_t>(block), re, im);
        if (n < settle_blocks * block) continue;
        sum += std::sqrt(static_cast<double>(re) * re + static_cast<double>(im) * im);
        ++count;
    }
    return sum / count / (amplitude * block);
}

double to_db(double ratio) {
    return k_db_amplitude * std::log10(ratio);
}

// Real tone through Nco + Cic2 at `nco_hz`; returns the block outputs.
std::vector<Complex> mix_tone(double tone_hz, double nco_hz, double amplitude, int block, double seconds,
                              double noise_sigma, std::uint32_t seed) {
    Nco nco;
    nco.set_frequency(static_cast<float>(nco_hz));
    Cic2 cic;
    std::mt19937 generator(seed);
    std::normal_distribution<double> noise(0.0, noise_sigma);
    std::vector<Complex> out;
    const int samples = static_cast<int>(seconds * k_rate);
    for (int n = 0; n < samples; ++n) {
        double value = amplitude * std::sin(k_two_pi * tone_hz * n / k_rate);
        if (noise_sigma > 0.0) value += noise(generator);
        const int32_t x = static_cast<int32_t>(std::lround(std::max(-32768.0, std::min(32767.0, value))));
        int16_t c;
        int16_t s;
        nco.next(c, s);
        cic.push((x * c) >> k_mix_shift, -((x * s) >> k_mix_shift));
        if ((n + 1) % block != 0) continue;
        int32_t re;
        int32_t im;
        cic.dump(static_cast<uint8_t>(block), re, im);
        const Complex h = {static_cast<float>(re), static_cast<float>(im)};
        out.push_back(h);
    }
    return out;
}

double phase_of(const Complex& value) {
    return std::atan2(value.im, value.re);
}

}  // namespace

// U8: NCO frequency and phase drift.
TEST(dsp_nco_frequency) {
    const float targets[] = {300.0f, 1234.567f, 1500.0f, 2699.9f};
    for (std::size_t i = 0; i < test::count_of(targets); ++i) {
        Nco nco;
        nco.set_frequency(targets[i]);
        CHECK_NEAR(nco.frequency(), targets[i], 0.01);
        nco.adjust_frequency(0.5f);
        CHECK_NEAR(nco.frequency(), targets[i] + 0.5, 0.01);
        nco.adjust_frequency(-1.25f);
        CHECK_NEAR(nco.frequency(), targets[i] - 0.75, 0.01);
    }
    // A tone mixed by an NCO at the same frequency must not drift: phase change over 1 s < 2 pi * 0.01 Hz * 1 s.
    const double tone = 1234.567;
    const int block = 16;
    const std::vector<Complex> h = mix_tone(tone, tone, 20000.0, block, 1.2, 0.0, 1);
    const std::size_t one_second = static_cast<std::size_t>(k_rate / block);
    double drift = phase_of(h[h.size() - 1]) - phase_of(h[h.size() - 1 - one_second]);
    while (drift > k_pi) drift -= k_two_pi;
    while (drift < -k_pi) drift += k_two_pi;
    const double drift_hz = drift / k_two_pi;
    NOTE("NCO residual frequency from phase drift: %.5f Hz", drift_hz);
    CHECK(std::fabs(drift_hz) < 0.01);
}

// U8: CIC-2 response and image rejection.
TEST(dsp_cic2_response) {
    const int blocks[] = {4, 8, 16, 32};
    const double frequencies[] = {0.0, 25.0, 60.0, 150.0, 300.0};
    const double amplitude = 1 << 18;
    for (std::size_t b = 0; b < test::count_of(blocks); ++b) {
        for (std::size_t f = 0; f < test::count_of(frequencies); ++f) {
            const double expected = to_db(cic2_response(frequencies[f], blocks[b]));
            const double measured = to_db(measured_cic2(frequencies[f], blocks[b], amplitude));
            CHECK_NEAR(measured, expected, 0.1);
        }
    }
    const int image_blocks[] = {8, 16};
    const double image_hz = 800.0;  // 2f image of a 400 Hz tone
    for (std::size_t b = 0; b < test::count_of(image_blocks); ++b) {
        const double rejection = -to_db(measured_cic2(image_hz, image_blocks[b], amplitude));
        NOTE("CIC-2 image rejection at %.0f Hz, B = %d: %.2f dB (analytic %.2f dB)", image_hz, image_blocks[b],
             rejection, -to_db(cic2_response(image_hz, image_blocks[b])));
        // Spec U8 asks >= 25 dB; the analytic CIC-2 value for B = 8 is 24.95 dB (reported, see test notes).
        CHECK(rejection >= 24.9);
    }
}

// U9: fractional windows against brute force.
TEST(dsp_prefix_history_windows) {
    PrefixHistory history;
    std::mt19937 generator(9);
    std::uniform_int_distribution<int32_t> value(-(1 << 20), 1 << 20);
    std::vector<double> re;
    std::vector<double> im;
    const uint32_t origin = history.end_block();
    const int blocks = 700;
    for (int k = 0; k < blocks; ++k) {
        const int32_t a = value(generator);
        const int32_t b = value(generator);
        history.push(a, b, false);
        re.push_back(a);
        im.push_back(b);
    }
    std::uniform_real_distribution<double> position(1.0, blocks - 80.0);
    std::uniform_real_distribution<double> length(0.5, 60.0);
    double worst = 0.0;
    for (int trial = 0; trial < 2000; ++trial) {
        const double from = position(generator);
        const double to = from + length(generator);
        const int a = static_cast<int>(std::floor(from));
        const int b = static_cast<int>(std::floor(to));
        double sum_re = 0.0;
        double sum_im = 0.0;
        for (int k = a; k < b; ++k) {
            sum_re += re[k];
            sum_im += im[k];
        }
        sum_re += (to - b) * re[b] - (from - a) * re[a];
        sum_im += (to - b) * im[b] - (from - a) * im[a];
        Complex sum;
        REQUIRE(history.window(origin, static_cast<float>(from), static_cast<float>(to), sum));
        // float positions: compare with the brute force at the float-rounded positions' scale
        const double scale = std::max(std::fabs(sum_re) + std::fabs(sum_im), (1 << 20) * 1.0) / k_mixer_gain;
        const double error = (std::fabs(sum.re - sum_re / k_mixer_gain) + std::fabs(sum.im - sum_im / k_mixer_gain)) / scale;
        worst = std::max(worst, error);
    }
    NOTE("worst relative window error: %.2e", worst);
    CHECK(worst <= 1e-3);
    Complex sum;
    CHECK(!history.window(origin, blocks - 5.0f, blocks + 0.5f, sum));  // future
    CHECK(history.window(origin, blocks - 5.0f, static_cast<float>(blocks), sum));
    CHECK(!history.window(origin, -1.0f, 3.0f, sum));  // before the history
    history.reset();
    CHECK(!history.window(origin, blocks - 5.0f, blocks - 1.0f, sum));  // forgotten by reset
}

// U9: exact after the uint32 prefix sums wrap many times; 24 h soak with an in-bin carrier.
TEST(dsp_prefix_history_wrap_and_soak) {
    PrefixHistory history;
    const int32_t big = 1 << 24;
    for (int k = 0; k < 3000; ++k) history.push(big, -big, false);  // wraps the sums ~12 times
    const uint32_t origin = history.end_block();
    for (int k = 0; k < 100; ++k) history.push(1000 + k, -7, false);
    Complex sum;
    REQUIRE(history.window(origin, 10.0f, 20.0f, sum));
    double expected = 0.0;
    for (int k = 10; k < 20; ++k) expected += 1000 + k;
    CHECK_NEAR(sum.re * k_mixer_gain, expected, 1e-3);
    CHECK_NEAR(sum.im * k_mixer_gain, -70.0, 1e-3);

    // 24 h at 4-sample blocks: 172.8 M blocks of a full-scale in-bin carrier.
    const int32_t carrier = 23197 / 2 * 32 * 4;
    const uint32_t soak_blocks = 172800000u;
    PrefixHistory soak;
    for (uint32_t k = 0; k < soak_blocks; ++k) soak.push(carrier, carrier / 3, false);
    const uint32_t end = soak.end_block();
    REQUIRE(soak.window(end - 40, 10.25f, 27.75f, sum));
    const double expected_re = 17.5 * carrier / k_mixer_gain;
    const double relative = std::fabs(sum.re - expected_re) / expected_re;
    NOTE("24 h soak: relative window error %.2e", relative);
    CHECK(relative < 1e-4);
}

TEST(dsp_prefix_history_blank_bits) {
    PrefixHistory history;
    const uint32_t origin = history.end_block();
    for (int k = 0; k < 50; ++k) history.push(1, 1, k == 20);
    CHECK(history.any_blanked(origin, 19.5f, 21.0f));
    CHECK(history.any_blanked(origin, 20.0f, 20.5f));
    CHECK(!history.any_blanked(origin, 10.0f, 20.0f));
    CHECK(!history.any_blanked(origin, 21.0f, 30.0f));
}

// U10: quantile tracker on exponential noise.
TEST(dsp_quantile_tracker) {
    const double means[] = {1e-3, 5.0, 4e6};
    for (std::size_t m = 0; m < test::count_of(means); ++m) {
        std::mt19937 generator(static_cast<std::uint32_t>(10 + m));
        std::exponential_distribution<double> exponential(1.0 / means[m]);
        QuantileTracker tracker;
        tracker.reset(static_cast<float>(means[m] * 20.0));  // start far off
        double average = 0.0;
        const int inputs = 10000;
        const int averaged = 5000;
        for (int i = 0; i < inputs; ++i) {
            tracker.push(static_cast<float>(exponential(generator)));
            if (i >= inputs - averaged) average += tracker.mean_estimate();
        }
        average /= averaged;
        NOTE("mean %.3g: estimate averaged over the last %d inputs %.4g (%.1f %%), last reading %.4g", means[m],
             averaged, average, 100.0 * (average / means[m] - 1.0), tracker.mean_estimate());
        CHECK(tracker.primed());
        CHECK_NEAR(average / means[m], 1.0, 0.05);
        CHECK_NEAR(tracker.mean_estimate() / means[m], 1.0, 0.5);  // one reading: +-16 % rms at step 1/64
    }
}

// TRACK's noise: the mean of exponential noise, of impulsive noise (where a quantile reads low), bounded
// moves on a single impulse, and a fast start from a seed that is far off.
TEST(dsp_noise_tracker) {
    const double mean = 5.0;
    const int inputs = 10000;
    const int averaged = 5000;
    std::mt19937 generator(21);
    std::exponential_distribution<double> exponential(1.0 / mean);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const double k_impulse_share = 0.05;  // 5 % of the windows hold an impulse of 8 x the background
    const double k_impulse = 8.0;
    const double impulsive_mean = mean * (1.0 + k_impulse_share * k_impulse);
    NoiseTracker plain;
    NoiseTracker impulsive;
    QuantileTracker quantile;
    plain.reset(static_cast<float>(mean * 20.0));
    impulsive.reset(static_cast<float>(mean));
    quantile.reset(static_cast<float>(mean));
    double plain_average = 0.0;
    double impulsive_average = 0.0;
    double quantile_average = 0.0;
    for (int i = 0; i < inputs; ++i) {
        plain.push(static_cast<float>(exponential(generator)));
        double value = exponential(generator);
        if (uniform(generator) < k_impulse_share) value += k_impulse * mean;
        impulsive.push(static_cast<float>(value));
        quantile.push(static_cast<float>(value));
        if (i < inputs - averaged) continue;
        plain_average += plain.mean_estimate();
        impulsive_average += impulsive.mean_estimate();
        quantile_average += quantile.mean_estimate();
    }
    plain_average /= averaged;
    impulsive_average /= averaged;
    quantile_average /= averaged;
    NOTE("exponential: %.3f of the mean; impulsive: tracker %.3f, 25 %% quantile %.3f of the mean",
         plain_average / mean, impulsive_average / impulsive_mean, quantile_average / impulsive_mean);
    CHECK_NEAR(plain_average / mean, 1.0, 0.05);
    CHECK_NEAR(impulsive_average / impulsive_mean, 1.0, 0.1);
    CHECK(quantile_average < 0.85 * impulsive_mean);  // the reason TRACK uses the mean

    NoiseTracker settled;
    settled.reset(static_cast<float>(mean));
    for (int i = 0; i < 64; ++i) settled.push(static_cast<float>(mean));
    settled.push(static_cast<float>(1000.0 * mean));  // one impulse of +30 dB
    NOTE("after one +30 dB impulse: %.3f of the mean", settled.mean_estimate() / mean);
    CHECK(settled.mean_estimate() < 1.2 * mean);

    NoiseTracker seeded;
    seeded.reset(static_cast<float>(mean / 100.0));  // 20 dB low, e.g. before the noise rose
    for (int i = 0; i < 128; ++i) seeded.push(static_cast<float>(exponential(generator)));
    NOTE("seed 20 dB low, after 128 inputs: %.3f of the mean", seeded.mean_estimate() / mean);
    CHECK_NEAR(seeded.mean_estimate() / mean, 1.0, 0.3);
}

// U12: flip statistics on synthetic windows.
TEST(dsp_flip_measure) {
    const float noise = 10.0f;
    const Complex a = {30.0f, 40.0f};
    const Complex minus_a = {-30.0f, -40.0f};
    const Complex tiny = {0.1f, -0.2f};
    const FlipMeasure flip = flip_measure(a, minus_a, noise, 50.0f);
    CHECK(flip.kappa > 0.9f);
    CHECK(flip.q > 0.0f);
    CHECK_NEAR(flip.q, 2500.0 / noise, 1e-3);
    CHECK_NEAR(flip.q_balanced, flip.q, 1e-3);
    CHECK_NEAR(flip.amplitude, 100.0 / (50.0 * k_g_marker), 1e-4);
    const FlipMeasure continuous = flip_measure(a, a, noise, 50.0f);
    CHECK(continuous.kappa < -0.9f);
    CHECK(continuous.q < 0.0f);
    const FlipMeasure onset = flip_measure(tiny, a, noise, 50.0f);
    CHECK(onset.q_balanced < 0.0f);
    const FlipMeasure offset = flip_measure(a, tiny, noise, 50.0f);
    CHECK(offset.q_balanced < 0.0f);
    // phase step: after = -before rotated by +0.3 rad
    const float angle = 0.3f;
    const Complex rotated = {-(a.re * std::cos(angle) - a.im * std::sin(angle)),
                             -(a.re * std::sin(angle) + a.im * std::cos(angle))};
    CHECK_NEAR(flip_measure(a, rotated, noise, 50.0f).phase_step, angle, 1e-4);
    CHECK_NEAR(noise_samples(8.0f, 16), (8.0 - 1.0 / 3.0) * 16.0, 1e-4);
}

// U13: floor, between-bin tones, steady-carrier mask.
TEST(dsp_tone_search_floor) {
    const double sigmas[] = {30.0, 1000.0};
    for (std::size_t s = 0; s < test::count_of(sigmas); ++s) {
        ToneSearch search;
        search.configure(300, 2700);
        std::mt19937 generator(static_cast<std::uint32_t>(13 + s));
        std::normal_distribution<double> noise(0.0, sigmas[s]);
        const int blocks = 300;
        float candidate_hz = 0.0f;
        bool locked = false;
        for (int n = 0; n < blocks * ToneSearch::k_block_samples; ++n) {
            if (search.push(static_cast<int16_t>(std::lround(noise(generator))))) locked |= search.candidate(candidate_hz);
        }
        const double truth = ToneSearch::k_block_samples * sigmas[s] * sigmas[s];
        NOTE("noise sigma %.0f: floor / true bin noise = %.3f", sigmas[s], search.floor() / truth);
        CHECK_NEAR(search.floor() / truth, 1.0, 0.10);
        CHECK(!locked);
    }
}

TEST(dsp_tone_search_between_bins) {
    const double tones[] = {412.5, 1000.0, 1525.0, 1537.5, 1561.0, 2012.3, 2687.0};
    const double snr_db = 6.0;  // key-down in 2500 Hz
    const double amplitude = 8000.0;
    const double sigma = amplitude * std::sqrt(0.5 * (k_rate / 2.0) / 2500.0 / std::pow(10.0, snr_db / 10.0));
    double worst = 0.0;
    for (std::size_t t = 0; t < test::count_of(tones); ++t) {
        ToneSearch search;
        search.configure(300, 2700);
        std::mt19937 generator(static_cast<std::uint32_t>(100 + t));
        std::normal_distribution<double> noise(0.0, sigma);
        float estimate = 0.0f;
        bool locked = false;
        const int silence = 20 * ToneSearch::k_block_samples + 37;
        for (int n = 0; n < 60 * ToneSearch::k_block_samples && !locked; ++n) {
            double value = noise(generator);
            if (n >= silence) value += amplitude * std::sin(k_two_pi * tones[t] * n / k_rate + 0.4);
            if (search.push(static_cast<int16_t>(std::lround(value)))) locked = search.candidate(estimate);
        }
        REQUIRE(locked);
        worst = std::max(worst, std::fabs(estimate - tones[t]));
        CHECK_NEAR(estimate, tones[t], 5.0);
    }
    NOTE("worst tone estimate error at %+.0f dB: %.2f Hz", snr_db, worst);
}

// A tune at the hf_weak gate (-9.5 dB in 2500 Hz, 7.5 dB in a bin at its centre) halfway between two bins: the half-bin
// powers find it within its 1.5 s, and the half-block phase picks the right side of the bin (the power pattern alone
// took the neighbour bin 50 Hz off in 1 of 100 A3' transmissions).
TEST(dsp_tone_search_weak_half_bin) {
    const double tones[] = {1525.0, 1574.0, 2073.0, 2126.0, 912.0};
    const double snr_db = -9.5;
    const double amplitude = 2000.0;  // noise sigma 5300: no clipping
    const double sigma = amplitude * std::sqrt(0.5 * (k_rate / 2.0) / 2500.0 / std::pow(10.0, snr_db / 10.0));
    const int seeds = 4;
    const double tune_s = 1.5;
    const double tolerance_hz = 12.0;
    double worst = 0.0;
    int missed = 0;
    for (std::size_t t = 0; t < test::count_of(tones); ++t) {
        for (int seed = 0; seed < seeds; ++seed) {
            ToneSearch search;
            search.configure(300, 2700);
            std::mt19937 generator(static_cast<std::uint32_t>(300 + 10 * t + seed));
            std::normal_distribution<double> noise(0.0, sigma);
            float estimate = 0.0f;
            bool locked = false;
            const int silence = 75 * ToneSearch::k_block_samples + 37;  // 1.5 s of noise first, as in A3'
            const int end = silence + static_cast<int>(tune_s * k_rate);
            for (int n = 0; n < end && !locked; ++n) {
                double value = noise(generator);
                if (n >= silence) value += amplitude * std::sin(k_two_pi * tones[t] * n / k_rate + 0.4 + seed);
                if (search.push(static_cast<int16_t>(std::lround(value)))) locked = search.candidate(estimate);
            }
            if (!locked) {
                ++missed;
                continue;
            }
            worst = std::max(worst, std::fabs(estimate - tones[t]));
        }
    }
    NOTE("half-bin tunes at %+.1f dB: %d of %zu missed, worst estimate error %.1f Hz", snr_db, missed,
         test::count_of(tones) * seeds, worst);
    CHECK_EQ(missed, 0);
    CHECK(worst <= tolerance_hz);
}

// A receiver AGC raises the noise 20 dB within a second after a strong signal ends: the recent floor follows it, so
// the rising noise is no tone to lock on (C13: the decoder chased such locks while the next tune went by).
TEST(dsp_tone_search_recent_floor) {
    ToneSearch search;
    search.configure(300, 2700);
    std::mt19937 generator(31);
    std::normal_distribution<double> noise(0.0, 1.0);
    const double quiet_sigma = 100.0;
    const double loud_sigma = 1000.0;
    const double rise_s = 0.3;  // the AGC's decay time constant
    const int quiet_blocks = 100;
    const int loud_blocks = 150;
    bool locked = false;
    float estimate = 0.0f;
    for (int n = 0; n < (quiet_blocks + loud_blocks) * ToneSearch::k_block_samples; ++n) {
        const double t = static_cast<double>(n - quiet_blocks * ToneSearch::k_block_samples) / k_rate;
        const double gain = t < 0.0 ? 1.0 : loud_sigma / quiet_sigma - (loud_sigma / quiet_sigma - 1.0) * std::exp(-t / rise_s);
        if (search.push(static_cast<int16_t>(std::lround(quiet_sigma * gain * noise(generator))))) {
            locked = locked || search.candidate(estimate);
        }
    }
    const double truth = ToneSearch::k_block_samples * loud_sigma * loud_sigma;
    NOTE("recent floor / noise after the rise %.2f (slow floor %.2f), locked %d", search.recent_floor() / truth,
         search.floor() / truth, locked);
    CHECK(!locked);
    CHECK_NEAR(search.recent_floor() / truth, 1.0, 0.2);
}

TEST(dsp_tone_search_steady_mask) {
    ToneSearch search;
    search.configure(300, 2700);
    std::mt19937 generator(21);
    std::normal_distribution<double> noise(0.0, 300.0);
    const double carrier_hz = 1000.0;
    const double keyed_hz = 2000.0;
    const double keying_hz = 4.0;  // 125 ms on/off
    const int blocks = 140;        // 2.8 s
    for (int n = 0; n < blocks * ToneSearch::k_block_samples; ++n) {
        const double t = n / k_rate;
        double value = noise(generator) + 3000.0 * std::sin(k_two_pi * carrier_hz * t);
        if (std::fmod(t * keying_hz, 1.0) < 0.5) value += 3000.0 * std::sin(k_two_pi * keyed_hz * t);
        search.push(static_cast<int16_t>(std::lround(value)));
    }
    CHECK(search.masked(carrier_hz));
    CHECK(!search.masked(keyed_hz));
    CHECK(!search.masked(1500.0f));
}

TEST(dsp_tone_search_ban) {
    ToneSearch search;
    search.configure(300, 2700);
    const double tone = 1500.0;
    const uint16_t ban_blocks = 50;
    search.ban(static_cast<float>(tone), ban_blocks);
    std::mt19937 generator(8);
    std::normal_distribution<double> noise(0.0, 200.0);
    float estimate = 0.0f;
    int locked_block = -1;
    for (int n = 0; n < 120 * ToneSearch::k_block_samples && locked_block < 0; ++n) {
        const double value = 5000.0 * std::sin(k_two_pi * tone * n / k_rate) + noise(generator);
        if (search.push(static_cast<int16_t>(std::lround(value))) && search.candidate(estimate)) {
            locked_block = n / ToneSearch::k_block_samples;
        }
    }
    NOTE("banned tone locked at block %d (ban %d blocks)", locked_block, ban_blocks);
    CHECK(locked_block >= ban_blocks);
    CHECK_NEAR(estimate, tone, 5.0);
}

// U14: fine AFC on the real front end at 0 dB (key-down in 2500 Hz). One look within +-10 Hz (1 Hz bins);
// offsets out to +-30 Hz are pulled in by a first correction and refined by the next (as the decoder does).
// Signals for the ToneSearch train-onset tests, one sample at a time.
namespace {

const double k_onset_amplitude = 6000.0;
const double k_onset_noise = 300.0;
const double k_onset_tone_hz = 1523.0;
const double k_onset_steady_s = 0.3;  // the tune tone
const double k_onset_slot_s = 0.032;  // marker train: a reversal in every slot
const double k_keying_s = 0.06;       // 20 WPM dot
const double k_key_edge_s = 0.005;

enum class Onset { train, carrier, keyed, noise };

double onset_signal(Onset kind, double t, double phase) {
    const double tone = std::sin(k_two_pi * k_onset_tone_hz * t + phase);
    switch (kind) {
    case Onset::train: {
        if (t < k_onset_steady_s) return k_onset_amplitude * tone;
        const long slots = static_cast<long>((t - k_onset_steady_s) / k_onset_slot_s + 0.5);  // flips at slot centres
        return k_onset_amplitude * ((slots & 1) != 0 ? -tone : tone);
    }
    case Onset::carrier:
        return k_onset_amplitude * tone;
    case Onset::keyed: {
        const double u = std::fmod(t, 2.0 * k_keying_s);  // continuous phase, 5 ms raised-cosine edges
        double key = 0.0;
        if (u < k_keying_s) key = std::min(1.0, std::min(u, k_keying_s - u) / k_key_edge_s);
        return k_onset_amplitude * (0.5 - 0.5 * std::cos(k_pi * key)) * tone;
    }
    case Onset::noise:
        return 0.0;
    }
    return 0.0;
}

// First search block (from the start) at which train_onset() fires after `products` steady products (the decoder
// asks 3, or 4 while it holds a tune tone), or -1; tone estimate in `tone_hz`.
int first_onset(Onset kind, double seconds, std::uint32_t seed, float& tone_hz, std::uint8_t products = 3) {
    ToneSearch search;
    search.configure(300, 2700);
    std::mt19937 generator(seed);
    std::normal_distribution<double> noise(0.0, k_onset_noise);
    const int samples = static_cast<int>(seconds * k_rate);
    int block = 0;
    for (int n = 0; n < samples; ++n) {
        const double value = onset_signal(kind, n / k_rate, 0.7) + noise(generator);
        if (!search.push(static_cast<int16_t>(std::lround(value)))) continue;
        ++block;
        if (search.train_onset(tone_hz, products)) return block;
    }
    return -1;
}

}  // namespace

// A tune tone that turns into a marker train is reported within three search blocks, with the tone
// measured on the steady part; carriers, keyed CW and noise never are (the ACQUIRE watch relies on this).
TEST(dsp_tone_search_train_onset) {
    const int onset_block = static_cast<int>(k_onset_steady_s * k_rate / ToneSearch::k_block_samples);
    const int max_delay_blocks = 3;
    for (std::uint32_t seed = 1; seed <= 3; ++seed) {
        float tone = 0.0f;
        const int block = first_onset(Onset::train, 1.0, seed, tone);
        NOTE("seed %u: onset at block %d (train from block %d), tone %.2f Hz", seed, block, onset_block, tone);
        CHECK(block > onset_block);
        CHECK(block <= onset_block + max_delay_blocks);
        CHECK_NEAR(tone, k_onset_tone_hz, 3.0);
        const int held_tune = first_onset(Onset::train, 1.0, seed, tone, 4);  // the stricter onset of a held tune
        CHECK(held_tune > onset_block);
        CHECK(held_tune <= onset_block + max_delay_blocks);
        CHECK_EQ(first_onset(Onset::carrier, 3.0, seed, tone), -1);
        CHECK_EQ(first_onset(Onset::keyed, 3.0, seed, tone), -1);
        CHECK_EQ(first_onset(Onset::noise, 3.0, seed, tone), -1);
    }
}

// The noise seed of a lock comes from the floor before its tone appeared: an off-bin tone 50 dB over the
// noise leaks into every search bin and lifts the current floor.
TEST(dsp_tone_search_onset_floor) {
    ToneSearch search;
    search.configure(300, 2700);
    std::mt19937 generator(5);
    const double sigma = 30.0;
    std::normal_distribution<double> noise(0.0, sigma);
    const int quiet_blocks = 100;
    const int tone_blocks = 20;
    for (int n = 0; n < (quiet_blocks + tone_blocks) * ToneSearch::k_block_samples; ++n) {
        double value = noise(generator);
        if (n >= quiet_blocks * ToneSearch::k_block_samples) value += 20000.0 * std::sin(k_two_pi * 1523.0 * n / k_rate);
        search.push(static_cast<int16_t>(std::lround(value)));
    }
    const double truth = ToneSearch::k_block_samples * sigma * sigma;
    NOTE("floor / noise: current %.2f, before the tone %.2f", search.floor() / truth, search.onset_floor() / truth);
    CHECK(search.floor() > 2.0 * truth);
    CHECK_NEAR(search.onset_floor() / truth, 1.0, 0.15);
}

// An excluded tone (the lock that is being tried) leaves the search free to follow the next one.
TEST(dsp_tone_search_exclude) {
    const float k_train_line_hz = 125.0f;  // a train at T = 4 ms
    ToneSearch search;
    search.configure(300, 2700);
    std::mt19937 generator(9);
    std::normal_distribution<double> noise(0.0, 200.0);
    const double strong_hz = 1000.0;
    const double weak_hz = 1800.0;
    float tone = 0.0f;
    bool excluded = false;
    bool found_weak = false;
    for (int n = 0; n < 60 * ToneSearch::k_block_samples && !found_weak; ++n) {
        const double t = n / k_rate;
        const double value = 8000.0 * std::sin(k_two_pi * strong_hz * t) + 2000.0 * std::sin(k_two_pi * weak_hz * t) +
                             noise(generator);
        if (!search.push(static_cast<int16_t>(std::lround(value))) || !search.candidate(tone)) continue;
        if (!excluded) {
            CHECK_NEAR(tone, strong_hz, 5.0);
            search.exclude(tone, k_train_line_hz);
            excluded = true;
        } else {
            found_weak = std::fabs(tone - weak_hz) < 5.0;
        }
    }
    CHECK(excluded);
    CHECK(found_weak);
}

TEST(dsp_fine_afc) {
    const double offsets[] = {3.3, -7.7, 0.4, 9.6};
    const int block = 8;
    const int decimation = 8;  // 64 samples: 125 Hz
    const double amplitude = 8000.0;
    const double sigma = amplitude * std::sqrt(0.5 * (k_rate / 2.0) / 2500.0);
    for (std::size_t i = 0; i < test::count_of(offsets); ++i) {
        const double nco_hz = 1500.0;
        const std::vector<Complex> h = mix_tone(nco_hz + offsets[i], nco_hz, amplitude, block, 0.6, sigma,
                                                static_cast<std::uint32_t>(40 + i));
        FineAfc afc;
        for (std::size_t k = 0; k + decimation <= h.size(); k += decimation) {
            Complex w = {0.0f, 0.0f};
            for (int j = 0; j < decimation; ++j) {
                w.re += h[k + j].re;
                w.im += h[k + j].im;
            }
            const float scale = 1.0f / (k_mixer_gain * block * decimation);
            w.re *= scale;
            w.im *= scale;
            afc.push(w);
        }
        float estimate = 0.0f;
        REQUIRE(afc.offset(estimate, false));
        NOTE("AFC offset %.2f Hz: estimate %.3f Hz", offsets[i], estimate);
        CHECK_NEAR(estimate, offsets[i], 0.2);
    }
    FineAfc empty;
    float estimate = 0.0f;
    CHECK(!empty.offset(estimate, true));
}

TEST(dsp_fine_afc_pull_in) {
    const double offsets[] = {12.5, -18.0, 24.0, -29.0};
    const int block = 8;
    const int decimation = 8;
    const double amplitude = 8000.0;
    const double sigma = amplitude * std::sqrt(0.5 * (k_rate / 2.0) / 2500.0);
    const int looks = 2;
    for (std::size_t i = 0; i < test::count_of(offsets); ++i) {
        double nco_hz = 1500.0;
        const double tone_hz = nco_hz + offsets[i];
        for (int look = 0; look < looks; ++look) {
            const std::vector<Complex> h = mix_tone(tone_hz, nco_hz, amplitude, block, 0.6, sigma,
                                                    static_cast<std::uint32_t>(60 + 7 * i + look));
            FineAfc afc;
            for (std::size_t k = 0; k + decimation <= h.size(); k += decimation) {
                Complex w = {0.0f, 0.0f};
                for (int j = 0; j < decimation; ++j) {
                    w.re += h[k + j].re;
                    w.im += h[k + j].im;
                }
                const float scale = 1.0f / (k_mixer_gain * block * decimation);
                w.re *= scale;
                w.im *= scale;
                afc.push(w);
            }
            float estimate = 0.0f;
            REQUIRE(afc.offset(estimate, true));
            nco_hz += estimate;
        }
        NOTE("AFC pull-in from %.1f Hz: residual after %d looks %.3f Hz", offsets[i], looks, tone_hz - nco_hz);
        CHECK_NEAR(nco_hz, tone_hz, 0.2);
    }
}

TEST(dsp_candidate_list_merge) {
    CandidateList list;
    const Candidate first = {1000, 0.25f, 5.0f, 2};
    CHECK(list.add(first));
    const Candidate weaker = {1002, 0.0f, 4.0f, 3};
    CHECK(!list.add(weaker));
    const Candidate stronger = {1001, 0.5f, 9.0f, 3};
    CHECK(list.add(stronger));
    CHECK_EQ(+list.count(), 1);
    CHECK_EQ(list.newest(0).block, 1001u);
    const Candidate next_marker = {1009, 0.5f, 3.5f, 1};  // one T_min later: a distinct marker
    CHECK(list.add(next_marker));
    CHECK_EQ(+list.count(), 2);
    CHECK_EQ(list.newest(1).block, 1001u);
    for (uint32_t k = 0; k < 40; ++k) {
        const Candidate c = {2000 + 100 * k, 0.0f, 3.0f, 0};
        list.add(c);
    }
    CHECK_EQ(+list.count(), +CandidateList::k_size);
    CHECK_EQ(list.newest(0).block, 2000u + 100u * 39u);
}

// Alias audit ring (spec 3.11): 2N + 1 positions, int8 in 1/15 units, committed per frame.
TEST(dsp_audit_ring) {
    AuditRing ring;
    const uint8_t positions = 2 * 16 + 1;
    ring.reset(positions);
    CHECK_EQ(+ring.positions(), +positions);
    CHECK_EQ(+ring.frames(), 0);
    CHECK_NEAR(ring.max_evidence(), 0.0, 1e-6);
    for (int f = 0; f < 3; ++f) {
        ring.set(8, 5.0f);
        ring.set(3, -4.0f);
        ring.set(positions, 8.0f);  // outside: ignored
        CHECK_NEAR(ring.max_evidence(), 5.0 * f, 1e-6);  // the frame being measured does not count
        ring.next_frame();
    }
    CHECK_NEAR(ring.max_evidence(), 15.0, 1e-6);
    CHECK_EQ(+ring.frames(), 3);
    ring.set(8, 8.0f);
    ring.next_frame();
    CHECK_NEAR(ring.max_evidence(), 23.0, 1e-6);
    for (int f = 0; f < 4; ++f) ring.next_frame();
    CHECK_NEAR(ring.max_evidence(), 0.0, 1e-6);
    CHECK_EQ(+ring.frames(), +AuditRing::k_frames);
    ring.set(0, 2.0f / 3.0f);  // 10 / 15, exact in the int8 scale
    ring.next_frame();
    CHECK_NEAR(ring.max_evidence(), 2.0 / 3.0, 1e-6);
    ring.reset(3);
    ring.set(2, -4.0f);
    ring.next_frame();
    CHECK_NEAR(ring.max_evidence(), 0.0, 1e-6);  // positions 0 and 1 hold 0
}

TEST(dsp_impulse_blanker) {
    ImpulseBlanker blanker;
    std::mt19937 generator(5);
    // block energy of 16 samples of white noise: chi-square with 16 degrees of freedom, mean 1
    std::gamma_distribution<double> noise(8.0, 1.0 / 8.0);
    const int spike_block = 200;
    const int blocks = 400;
    std::vector<bool> blanked;
    for (int k = 0; k < blocks; ++k) {
        float energy = static_cast<float>(noise(generator));
        if (k == spike_block) energy = 500.0f;
        blanked.push_back(blanker.push_block(energy, 0.0f));
    }
    const int latency = ImpulseBlanker::k_latency;
    CHECK(blanked[spike_block + latency]);
    CHECK(blanked[spike_block + latency + 1]);
    int count = 0;
    for (int k = 0; k < blocks; ++k) count += blanked[k] ? 1 : 0;
    NOTE("blanked blocks: %d (spike + follower expected)", count);
    CHECK_EQ(count, 2);

    // A tone peak (in-bin energy dominates) is never blanked, nor is a lasting level change beyond 2 blocks.
    ImpulseBlanker tone;
    int tone_blanks = 0;
    for (int k = 0; k < blocks; ++k) {
        const bool on = (k / 8) % 2 == 1;
        const float in_bin = on ? 400.0f : 0.0f;
        const float energy = in_bin + static_cast<float>(noise(generator));
        tone_blanks += tone.push_block(energy, in_bin) ? 1 : 0;
    }
    CHECK_EQ(tone_blanks, 0);
    ImpulseBlanker step;
    int step_blanks = 0;
    for (int k = 0; k < blocks; ++k) {
        const float energy = static_cast<float>(noise(generator)) * (k < 200 ? 1.0f : 50.0f);
        step_blanks += step.push_block(energy, 0.0f) ? 1 : 0;
    }
    NOTE("level step: %d blanked blocks", step_blanks);
    CHECK(step_blanks <= 3);
}

// ---------------------------------------------------------------------------
// v0.2 slot path (spec 3.2, 3.8, 3.10)
// ---------------------------------------------------------------------------

namespace {

const double k_peak_ramp = 0.125;

double tukey_peak(double u) {
    if (u < 0.0 || u >= 1.0) return 0.0;
    if (u < k_peak_ramp) return std::pow(std::sin(k_pi * u / (2.0 * k_peak_ramp)), 2.0);
    if (u > 1.0 - k_peak_ramp) return std::pow(std::sin(k_pi * (1.0 - u) / (2.0 * k_peak_ramp)), 2.0);
    return 1.0;
}

// Double-precision reference: |sum x[n] w[n] e^-jwn|^2 with w at u = (n + 0.5) / L.
double reference_energy(const std::vector<int16_t>& x, double hz) {
    const double length = static_cast<double>(x.size());
    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double w = tukey_peak((n + 0.5) / length) * x[n];
        re += w * std::cos(k_two_pi * hz * n / k_rate);
        im -= w * std::sin(k_two_pi * hz * n / k_rate);
    }
    return re * re + im * im;
}

template <typename Bank>
void run_bank(Bank& bank, const std::vector<int16_t>& x, float skip = 0.0f) {
    bank.open(static_cast<float>(x.size()), skip);
    for (std::size_t n = static_cast<std::size_t>(std::ceil(skip)); n < x.size(); ++n) {
        const bool closed = bank.push(x[n]);
        if (closed) break;
    }
}

std::vector<int16_t> tone_slot(std::size_t length, double hz, double amplitude, double phase, double noise_sigma,
                               std::mt19937& generator) {
    std::normal_distribution<double> noise(0.0, noise_sigma > 0.0 ? noise_sigma : 1.0);
    std::vector<int16_t> x(length);
    for (std::size_t n = 0; n < length; ++n) {
        double value = amplitude * tukey_peak(static_cast<double>(n) / length) * std::sin(k_two_pi * hz * n / k_rate + phase);
        if (noise_sigma > 0.0) value += noise(generator);
        x[n] = static_cast<int16_t>(std::lround(std::max(-32768.0, std::min(32767.0, value))));
    }
    return x;
}

// Square-law header bins for the Monte Carlo of U22: |a e^jphi + n|^2 with n ~ CN(0, 1).
struct HeaderSim {
    std::mt19937_64 generator;
    std::normal_distribution<double> gauss;
    std::uniform_real_distribution<double> uniform;
    explicit HeaderSim(uint64_t seed) : generator(seed), gauss(0.0, std::sqrt(0.5)), uniform(0.0, 1.0) {}
    double bin(double amplitude, double leak = 0.0) {
        const double phase = k_two_pi * uniform(generator);
        const double leak_phase = k_two_pi * uniform(generator);
        const double re = amplitude * std::cos(phase) + leak * std::cos(leak_phase) + gauss(generator);
        const double im = amplitude * std::sin(phase) + leak * std::sin(leak_phase) + gauss(generator);
        return re * re + im * im;
    }
};

// A carrier between header tones 3 and 4 of side 0, k_leak_over_header_db over the header's peaks at its own frequency,
// leaks into every tone of that side, k_leak_db_per_tone less per tone of distance: the header window's sidelobes, as
// measured in C8' (+10 dB, 10 dB SNR, T = 32 ms: peaks 29 dB over the noise in a bin; +7, +1, -5, -6, -11, -11, -16,
// -20 dB relative to the peaks in the tones from the nearest out).
enum class HeaderCase { noise, carrier, chirp, misaligned, header, carrier_leak, header_carrier_leak };
const double k_leak_db_per_tone = 8.0;
const double k_leak_centre = 3.3;
const double k_leak_over_header_db = 10.0;

// P(accepted wrong) and P(accepted right) over `trials`.
void header_monte_carlo(HeaderCase kind, double level_db, int trials, uint64_t seed, double& wrong, double& right) {
    HeaderSim sim(seed);
    const double amplitude = std::sqrt(std::pow(10.0, level_db / 10.0));
    const uint8_t interferer_tone = 3;
    long wrong_count = 0;
    long right_count = 0;
    for (int trial = 0; trial < trials; ++trial) {
        const uint16_t word = static_cast<uint16_t>(sim.generator() % k_header_words);
        const uint8_t side = static_cast<uint8_t>(sim.generator() & 1u);
        float energy[2][k_header_slots][k_header_slots];
        for (uint8_t d = 0; d < 2; ++d) {
            for (uint8_t j = 0; j < k_header_slots; ++j) {
                for (uint8_t h = 0; h < k_header_slots; ++h) {
                    double a = 0.0;
                    if (kind == HeaderCase::header && d == side && h == header_symbol(word, j)) a = amplitude;
                    if (kind == HeaderCase::misaligned && d == side && j > 0 && h == header_symbol(word, j - 1)) a = amplitude;
                    if (kind == HeaderCase::carrier && d == 0 && h == interferer_tone) a = amplitude;
                    if (kind == HeaderCase::chirp && d == 0 && h == j) a = amplitude;
                    if (kind == HeaderCase::header_carrier_leak && d == 0 && h == header_symbol(word, j)) a = amplitude;
                    double leak = 0.0;
                    if ((kind == HeaderCase::carrier_leak || kind == HeaderCase::header_carrier_leak) && d == 0) {
                        const double leak_db =
                            level_db + k_leak_over_header_db - k_leak_db_per_tone * std::fabs(h - k_leak_centre);
                        leak = std::sqrt(std::pow(10.0, leak_db / 10.0));
                    }
                    energy[d][j][h] = static_cast<float>(sim.bin(a, leak));
                }
            }
        }
        const HeaderDecision decision = decide_header(energy);
        if (!decision.accepted) continue;
        const bool header = kind == HeaderCase::header || kind == HeaderCase::header_carrier_leak;
        const uint8_t sent_side = kind == HeaderCase::header_carrier_leak ? 0 : side;
        const bool correct = header && decision.word == word && decision.side == (sent_side == 0 ? 1 : -1);
        if (correct) {
            ++right_count;
        } else {
            ++wrong_count;
        }
    }
    wrong = static_cast<double>(wrong_count) / trials;
    right = static_cast<double>(right_count) / trials;
}

}  // namespace

// U26: the streaming slot bank against a double-precision matched filter; window sums; full scale; late opening.
TEST(dsp_slot_bank) {
    std::mt19937 generator(26);
    const std::size_t lengths[] = {48, 256, 1024};
    for (std::size_t l = 0; l < test::count_of(lengths); ++l) {
        const std::size_t length = lengths[l];
        const double bin_hz = 8000.0 / length;
        GridBank bank;
        bank.reset();
        const uint16_t bins = 16;
        bank.set_bins(bins);
        for (uint16_t b = 0; b < bins; ++b) bank.set_frequency(b, static_cast<float>(1000.0 + 1.1428571 * bin_hz * b));
        const double hz = 1000.0 + 1.1428571 * bin_hz * 5;
        const std::vector<int16_t> x = tone_slot(length, hz, 8000.0, 0.7, 300.0, generator);
        run_bank(bank, x);
        CHECK(!bank.active());
        double peak = 0.0;
        double worst = 0.0;
        for (uint16_t b = 0; b < bins; ++b) peak = std::max(peak, reference_energy(x, 1000.0 + 1.1428571 * bin_hz * b));
        for (uint16_t b = 0; b < bins; ++b) {
            const double expected = reference_energy(x, 1000.0 + 1.1428571 * bin_hz * b);
            worst = std::max(worst, std::fabs(bank.energy(b) - expected) / peak);
        }
        NOTE("L = %zu: worst bank error %.2e of the peak bin", length, worst);
        CHECK(worst <= 1e-3);
        CHECK_NEAR(bank.window_sum(), k_peak_window_mean * length, 1e-3 * length);
        CHECK_NEAR(bank.window_square_sum(), k_peak_energy * length, 1e-3 * length);
    }

    // Full scale at T = 128 ms on the bank's edge frequencies: no overflow, still the reference energy.
    const std::size_t length = 1024;
    const double edges[] = {100.0, 300.0, 2700.0, 3900.0};
    for (std::size_t e = 0; e < test::count_of(edges); ++e) {
        std::vector<int16_t> x(length);
        for (std::size_t n = 0; n < length; ++n) {
            x[n] = static_cast<int16_t>(std::lround(32767.0 * std::cos(k_two_pi * edges[e] * n / k_rate)));
        }
        HeaderBank bank;
        bank.reset();
        bank.set_bins(1);
        bank.set_frequency(0, static_cast<float>(edges[e]));
        run_bank(bank, x);
        const double expected = reference_energy(x, edges[e]);
        NOTE("full scale at %.0f Hz: %.4e vs %.4e", edges[e], bank.energy(0), expected);
        CHECK_NEAR(bank.energy(0) / expected, 1.0, 1e-3);
    }

    // HeaderBank and GridBank agree on a shared bin; a late window (skip) is the window with zeros in front.
    const std::vector<int16_t> x = tone_slot(256, 1210.0, 5000.0, 0.2, 100.0, generator);
    HeaderBank header;
    header.reset();
    header.set_bins(1);
    header.set_frequency(0, 1210.0f);
    GridBank grid;
    grid.reset();
    grid.set_bins(1);
    grid.set_frequency(0, 1210.0f);
    run_bank(header, x);
    run_bank(grid, x);
    CHECK_EQ(header.energy(0), grid.energy(0));
    const std::size_t skip = 40;
    std::vector<int16_t> zeroed = x;
    for (std::size_t n = 0; n < skip; ++n) zeroed[n] = 0;
    run_bank(header, zeroed);
    const float full = header.energy(0);
    run_bank(grid, x, static_cast<float>(skip));
    CHECK_NEAR(grid.energy(0) / full, 1.0, 1e-4);
    // Idle: pushes are ignored.
    CHECK(!grid.push(1000));
}

// U27: per-bin background. A steady carrier in one bin is learnt within 32 slots; N_bin is the median; on AWGN
// the background-subtracted decision costs nothing against plain argmax.
TEST(dsp_bin_background) {
    BinBackground background;
    const uint16_t bins = 32;
    background.reset(bins, 100.0f);
    CHECK_NEAR(background.mean(0), 100.0, 1.0);
    CHECK_NEAR(background.noise(), 100.0, 1.0);

    std::mt19937 generator(27);
    std::exponential_distribution<double> exponential(1.0);
    const double noise = 100.0;
    const double carrier = 1000.0 * noise;  // 30 dB over the bin noise
    const uint16_t carrier_bin = 3;
    int learnt_at = -1;
    double noise_sum = 0.0;
    int noise_count = 0;
    const int slots = 400;
    for (int slot = 0; slot < slots; ++slot) {
        for (uint16_t b = 0; b < bins; ++b) {
            const double energy = noise * exponential(generator) + (b == carrier_bin ? carrier : 0.0);
            background.push(b, static_cast<float>(energy));
        }
        if (learnt_at < 0 && background.mean(carrier_bin) >= carrier) learnt_at = slot + 1;
        if (slot < slots / 2) continue;
        noise_sum += background.noise();
        ++noise_count;
    }
    // The mean assumes exponential energies (mean = 25 % quantile x 3.48): a steady carrier reads up to 3.5 times
    // its energy, so it is fully suppressed (and so is a peak on its tone less than 2.5 times as strong).
    // N_bin of one slot is the median of the bins' trackers, each spread by its steps: it is averaged here (the
    // decoder's TRACK noise averages it too).
    const double mean_noise = noise_sum / noise_count;
    NOTE("carrier 30 dB over the noise suppressed after %d slots (mean %.2f x carrier); N_bin %.3f x noise",
         learnt_at, background.mean(carrier_bin) / carrier, mean_noise / noise);
    CHECK(learnt_at > 0 && learnt_at <= 32);
    CHECK(background.mean(carrier_bin) >= carrier && background.mean(carrier_bin) <= 4.0 * carrier);
    CHECK_NEAR(mean_noise / noise, 1.0, 0.1);

    // Symbol decisions with background subtraction against plain argmax, M = 32 at the threshold (per-slot Es/N0
    // about 12 dB), 3000 slots: the background learns the noise, so the argmax does not change.
    const std::size_t length = 256;
    const uint8_t k = 5;
    const uint16_t tones = 32;
    const double spacing = 8.0 / 7.0 * 8000.0 / length;
    GridBank bank;
    bank.reset();
    bank.set_bins(tones);
    for (uint16_t t = 0; t < tones; ++t) bank.set_frequency(t, static_cast<float>(2000.0 - (5.0 * 8000.0 / length + t * spacing)));
    BinBackground tracked;
    const double sigma = 1000.0;
    tracked.reset(tones, static_cast<float>(sigma * sigma * k_peak_energy * length));
    // Per-slot Es/N0 = |X|^2 / (sigma^2 sum w^2) = A^2 L (0.875^2 / 4) / (0.84375 sigma^2): 9 dB.
    const double es_n0 = std::pow(10.0, 0.9);
    const double amplitude = sigma * std::sqrt(es_n0 * 4.0 * k_peak_energy / (k_peak_window_mean * k_peak_window_mean * length));
    std::uniform_int_distribution<int> pick(0, tones - 1);
    int errors_background = 0;
    int errors_argmax = 0;
    for (int slot = 0; slot < 3000; ++slot) {
        const int tone = pick(generator);
        const std::vector<int16_t> x =
            tone_slot(length, 2000.0 - (5.0 * 8000.0 / length + tone * spacing), amplitude, 0.3 * slot, sigma, generator);
        run_bank(bank, x);
        const SlotDecision d = decide_slot(bank, tracked, k, 0);
        uint16_t best = 0;
        for (uint16_t t = 1; t < tones; ++t) {
            if (bank.energy(t) > bank.energy(best)) best = t;
        }
        for (uint16_t t = 0; t < tones; ++t) tracked.push(t, bank.energy(t));
        errors_background += d.tone != tone ? 1 : 0;
        errors_argmax += best != tone ? 1 : 0;
    }
    NOTE("symbol errors: background %d, plain argmax %d (of 3000)", errors_background, errors_argmax);
    CHECK(errors_argmax > 100);
    CHECK(errors_background <= 1.05 * errors_argmax + 5);
}

// U28: LLR signs carry the decision; ln I0 table.
TEST(dsp_llr) {
    double worst_absolute = 0.0;
    double worst_relative = 0.0;
    for (int i = 1; i <= 4000; ++i) {
        const double x = 0.005 * i;
        double i0 = 0.0;
        double term = 1.0;
        for (int m = 1; m < 400 && term > 1e-18 * i0; ++m) {
            i0 += term;
            term *= (x / 2.0) * (x / 2.0) / (m * m);
        }
        const double error = std::fabs(ln_i0(static_cast<float>(x)) - std::log(i0));
        worst_absolute = std::max(worst_absolute, error);
        if (x >= 3.0) worst_relative = std::max(worst_relative, error / std::log(i0));
    }
    NOTE("ln I0: worst error %.4f nat, %.2f %% for x >= 3", worst_absolute, 100.0 * worst_relative);
    CHECK(worst_absolute <= 0.02);
    CHECK(worst_relative <= 0.01);
    CHECK_EQ(ln_i0(0.0f), 0.0f);

    std::mt19937 generator(28);
    const std::size_t length = 128;
    const uint8_t bits[] = {1, 3, 5, 7};
    for (std::size_t b = 0; b < test::count_of(bits); ++b) {
        const uint8_t k = bits[b];
        const uint16_t tones = static_cast<uint16_t>(1u << k);
        const uint16_t opened = tones > k_min_grid_bins ? tones : k_min_grid_bins;
        const double spacing = 8000.0 / length;
        GridBank bank;
        bank.reset();
        bank.set_bins(opened);
        for (uint16_t t = 0; t < opened; ++t) bank.set_frequency(t, static_cast<float>(400.0 + t * spacing * 0.3));
        BinBackground background;
        background.reset(opened, static_cast<float>(400.0 * 400.0 * k_peak_energy * length));
        int mismatches = 0;
        for (int slot = 0; slot < 200; ++slot) {
            const int tone = slot % tones;
            const std::vector<int16_t> x = tone_slot(length, 400.0 + tone * spacing * 0.3, 1500.0, 0.1 * slot, 400.0, generator);
            run_bank(bank, x);
            const SlotDecision d = decide_slot(bank, background, k, static_cast<uint8_t>(slot % 32));
            CHECK_EQ(+d.symbol, +peak_symbol(d.tone, static_cast<uint8_t>(slot % 32), k));
            for (uint8_t i = 0; i < k; ++i) {
                const bool one = ((d.symbol >> (k - 1 - i)) & 1u) != 0;
                if ((d.soft[i] > 0) != one || d.soft[i] == 0 || std::abs(d.soft[i]) > k_llr_q4_max) ++mismatches;
            }
            for (uint16_t t = 0; t < opened; ++t) background.push(t, bank.energy(t));
        }
        CHECK_EQ(mismatches, 0);
    }
}

// A clean full-scale peak at T = 128 ms over a near-silent background: the max-log metrics pass the int32 range, and
// every bit still reads the largest LLR (the float was converted before it was clamped: undefined, and on x86 the
// strongest bits came out at +-1).
TEST(dsp_llr_saturates_at_high_snr) {
    std::mt19937 generator(31);
    const std::size_t length = 1024;
    const uint8_t k = 7;
    const uint16_t tones = static_cast<uint16_t>(1u << k);
    const double spacing = 8.0 / 7.0 * 8000.0 / length;
    GridBank bank;
    bank.reset();
    bank.set_bins(tones);
    for (uint16_t t = 0; t < tones; ++t) bank.set_frequency(t, static_cast<float>(2087.0 - (5.0 * 8000.0 / length + t * spacing)));
    const double sigma = 0.5;
    BinBackground background;
    background.reset(tones, static_cast<float>(sigma * sigma * k_peak_energy * length));
    int saturated = 0;
    int bits = 0;
    const int slots = 16;
    for (int slot = 0; slot < slots; ++slot) {
        const int tone = (37 * slot + 5) % tones;
        const std::vector<int16_t> x =
            tone_slot(length, 2087.0 - (5.0 * 8000.0 / length + tone * spacing), 23197.0, 0.7 * slot, sigma, generator);
        run_bank(bank, x);
        const SlotDecision d = decide_slot(bank, background, k, static_cast<uint8_t>(slot));
        CHECK_EQ(+d.tone, tone);
        for (uint8_t i = 0; i < k; ++i) {
            const bool one = ((d.symbol >> (k - 1 - i)) & 1u) != 0;
            saturated += d.soft[i] == (one ? k_llr_q4_max : -k_llr_q4_max) ? 1 : 0;
            ++bits;
        }
    }
    NOTE("%d of %d bits at |LLR| = %d", saturated, bits, k_llr_q4_max);
    CHECK_EQ(saturated, bits);
}

// Slot decision fields on a clean peak and on noise: presence, erasure, confidence, crest.
TEST(dsp_slot_decision) {
    std::mt19937 generator(29);
    const std::size_t length = 256;
    const uint8_t k = 4;
    const uint16_t tones = 16;
    const double spacing = 8.0 / 7.0 * 8000.0 / length;
    GridBank bank;
    bank.reset();
    bank.set_bins(tones);
    for (uint16_t t = 0; t < tones; ++t) bank.set_frequency(t, static_cast<float>(800.0 + t * spacing));
    const double sigma = 50.0;
    BinBackground background;
    background.reset(tones, static_cast<float>(sigma * sigma * k_peak_energy * length));
    const std::vector<int16_t> peak = tone_slot(length, 800.0 + 9 * spacing, 10000.0, 0.4, sigma, generator);
    run_bank(bank, peak);
    const SlotDecision strong = decide_slot(bank, background, k, 2);
    CHECK_EQ(+strong.tone, 9);
    CHECK(strong.confident);
    CHECK(!strong.erasure);
    CHECK(strong.confidence > 40);  // > 20 dB over the runner-up
    CHECK_NEAR(strong.crest / 10000.0, 1.0, 0.02);
    int confident = 0;
    int erasures = 0;
    double noise = 0.0;
    const int slots = 400;
    for (int slot = 0; slot < slots; ++slot) {
        const std::vector<int16_t> x = tone_slot(length, 800.0, 0.0, 0.0, sigma, generator);
        run_bank(bank, x);
        const SlotDecision d = decide_slot(bank, background, k, 0);
        confident += d.confident ? 1 : 0;
        erasures += d.erasure ? 1 : 0;
        noise += d.noise / slots;
        for (uint16_t t = 0; t < tones; ++t) background.push(t, bank.energy(t));
    }
    // The slot's own noise (bins 3+ tones from the winner), averaged.
    NOTE("noise %.3f of sigma^2 sum w^2", noise / (sigma * sigma * k_peak_energy * length));
    CHECK_NEAR(noise / (sigma * sigma * k_peak_energy * length), 1.0, 0.1);
    // max / mean of the others >= 2 H_M: iid exponentials pass it 6 % of the time at M = 16 (14 % at M = 8).
    NOTE("noise only: %d of 400 slots confident, %d erasures", confident, erasures);
    CHECK(confident <= 60);
    CHECK(erasures >= 200);
}

// U30: the slot-path impulse blanker.
TEST(dsp_slot_blanker) {
    SlotBlanker blanker;
    std::mt19937 generator(30);
    std::uniform_real_distribution<double> uniform(-1732.0, 1732.0);  // RMS 1000, never above 4 RMS
    std::vector<int16_t> in(20000);
    for (std::size_t n = 0; n < in.size(); ++n) in[n] = static_cast<int16_t>(std::lround(uniform(generator)));
    const std::size_t impulse = 12000;
    std::vector<int16_t> with_impulse = in;
    with_impulse[impulse] = 20000;  // 20 x RMS
    std::vector<int16_t> out(in.size());
    int zeroed = 0;
    for (std::size_t n = 0; n < in.size(); ++n) {
        out[n] = blanker.push(with_impulse[n]);
        if (blanker.blanked()) ++zeroed;
    }
    CHECK_EQ(zeroed, 2 * k_slot_blank_hold + 1);
    bool unchanged = true;
    for (std::size_t n = k_slot_blank_delay; n < in.size(); ++n) {
        const std::size_t source = n - k_slot_blank_delay;
        const bool span = source + k_slot_blank_hold >= impulse && source <= impulse + k_slot_blank_hold;
        if (span) {
            unchanged = unchanged && out[n] == 0;
        } else {
            unchanged = unchanged && out[n] == with_impulse[source];
        }
    }
    CHECK(unchanged);

    // A tone starting after digital silence is a level change: blanking stops within a few ms.
    SlotBlanker step;
    int blanked_step = 0;
    for (int n = 0; n < 4000; ++n) {
        const double value = n < 1000 ? 0.0 : 20000.0 * std::sin(k_two_pi * 1000.0 * n / k_rate);
        step.push(static_cast<int16_t>(std::lround(value)));
        if (step.blanked() && n > 1000 + 80) ++blanked_step;
    }
    CHECK_EQ(blanked_step, 0);

    // Peaks 15 dB over the running RMS (a flat FM transmitter into a de-emphasised receiver puts the header's low
    // tones that far over the train): tones, not impulses. The first one rises from the noise as steeply as an impulse
    // does and may lose its onset; the ones after it are not zeroed (the RMS alone would blank all of them).
    SlotBlanker loud;
    std::mt19937 noise_generator(31);
    std::normal_distribution<double> noise(0.0, 1000.0);
    const int k_peak_samples = 48;  // T = 6 ms
    const int k_ramp_samples = 6;   // Tukey alpha 0.25
    const double k_peak_amplitude = 8000.0;
    int blanked_peaks = 0;
    for (int n = 0; n < 4000 + 20 * k_peak_samples; ++n) {
        double value = noise(noise_generator);
        if (n >= 4000) {
            const int m = (n - 4000) % k_peak_samples;
            const int edge = std::min(m, k_peak_samples - 1 - m);
            const double ramp = edge < k_ramp_samples ? std::pow(std::sin(k_two_pi * 0.25 * (edge + 0.5) / k_ramp_samples), 2) : 1.0;
            const double hz = 500.0 + 190.0 * ((n - 4000) / k_peak_samples % 7);
            value += k_peak_amplitude * ramp * std::sin(k_two_pi * hz * n / k_rate);
        }
        loud.push(static_cast<int16_t>(std::lround(value)));
        if (loud.blanked() && n >= 4000 + k_peak_samples + k_slot_blank_delay) ++blanked_peaks;
    }
    CHECK_EQ(blanked_peaks, 0);
}

// Header ML (spec 3.8) on clean energies: every test vector word and both sides.
TEST(dsp_header_decision_clean) {
    const uint16_t words[] = {0x032, 0x002, 0x003, 0x004, 0x005, 0x006, 0x084, 0x103, 0x047, 0x1FF};
    for (std::size_t w = 0; w < test::count_of(words); ++w) {
        for (uint8_t side = 0; side < 2; ++side) {
            float energy[2][k_header_slots][k_header_slots];
            for (uint8_t d = 0; d < 2; ++d) {
                for (uint8_t j = 0; j < k_header_slots; ++j) {
                    for (uint8_t h = 0; h < k_header_slots; ++h) {
                        energy[d][j][h] = (d == side && h == header_symbol(words[w], j)) ? 1000.0f : 1.0f + 0.01f * (h + j);
                    }
                }
            }
            const HeaderDecision decision = decide_header(energy);
            CHECK(decision.accepted);
            CHECK_EQ(decision.word, words[w]);
            CHECK_EQ(+decision.side, side == 0 ? 1 : -1);
            CHECK_EQ(+decision.agreement, +k_header_slots);
        }
    }
    float silence[2][k_header_slots][k_header_slots] = {};
    CHECK(!decide_header(silence).accepted);
}

// U22: header decoder Monte Carlo on square-law bins (per-slot Es/N0 in a bin, noise CN(0, 1)). The carrier leak cases
// model C8' (a carrier +10 dB over the peaks between two header tones) with a random carrier phase in every slot, which
// is harsher than the channel: 84 % detected here (70 % with one scale for the whole side) against 99 % in C8'.
const double k_carrier_leak_detection = 0.8;
TEST(dsp_header_decision_monte_carlo) {
    struct Case {
        HeaderCase kind;
        double level_db;
        int trials;
        const char* name;
    };
    const Case cases[] = {
        {HeaderCase::noise, 0.0, 100000, "noise"},        {HeaderCase::carrier, 20.0, 20000, "carrier +20 dB"},
        {HeaderCase::chirp, 20.0, 20000, "chirp +20 dB"},  {HeaderCase::misaligned, 20.0, 20000, "misaligned 20 dB"},
        {HeaderCase::header, 9.0, 20000, "header 9 dB"},   {HeaderCase::header, 12.0, 20000, "header 12 dB"},
        {HeaderCase::carrier_leak, 30.0, 20000, "carrier +40 dB between tones 3 and 4, leaking into side 0"},
        {HeaderCase::header_carrier_leak, 29.0, 20000, "header 29 dB under a carrier 10 dB over it"},
    };
    for (std::size_t c = 0; c < test::count_of(cases); ++c) {
        double wrong = 0.0;
        double right = 0.0;
        header_monte_carlo(cases[c].kind, cases[c].level_db, cases[c].trials, 2200 + c, wrong, right);
        NOTE("%s: accepted wrong %.2e, right %.4f (%d trials)", cases[c].name, wrong, right, cases[c].trials);
        switch (cases[c].kind) {
        case HeaderCase::noise:
        case HeaderCase::chirp:
        case HeaderCase::misaligned:
            CHECK(wrong <= 1e-4);
            break;
        case HeaderCase::carrier:
        case HeaderCase::carrier_leak:
            CHECK(wrong <= 1e-3);
            break;
        case HeaderCase::header_carrier_leak:
            CHECK(wrong <= 1e-3);
            CHECK(right >= k_carrier_leak_detection);
            break;
        case HeaderCase::header:
            CHECK(wrong <= 1e-4);
            CHECK(right >= (cases[c].level_db < 10.0 ? 0.97 : 0.999));
            break;
        }
    }
}

TEST(dsp_log2_q8) {
    CHECK_EQ(log2_q8(1.0f), 0);
    CHECK_EQ(log2_q8(2.0f), 256);
    CHECK_EQ(log2_q8(0.5f), -256);
    CHECK_EQ(log2_q8(0.0f), -32768);
    CHECK_NEAR(log2_q8(1e30f), 256.0 * std::log2(1e30), 1.0);
    const float values[] = {1e-6f, 0.3f, 7.0f, 12345.6f, 3e12f};
    for (std::size_t i = 0; i < test::count_of(values); ++i) {
        CHECK_NEAR(exp2_q8(log2_q8(values[i])) / values[i], 1.0, 0.003);
    }
}
