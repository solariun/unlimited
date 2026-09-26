#include "resampler.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

using unlimited::pc::Resampler;
using unlimited::pc::resample;

namespace {

using std::size_t;
using test::count_of;

typedef std::complex<double> Complex;

const double k_pi = 3.14159265358979323846;
const double k_two_pi = 2.0 * k_pi;
const double k_amplitude = 0.5;
const double k_lead_s = 0.25;     // far longer than the filter half-length at any tested ratio
const double k_measure_s = 1.0;   // whole cycles of every integer-Hz tone: other tones do not leak in
const double k_signal_s = 2.0 * k_lead_s + k_measure_s;
const double k_passband_fraction = 0.45;  // of the lower rate
const double k_stopband_fraction = 0.55;
const double k_max_ripple_db = 0.1;
const double k_max_phase_error_rad = 1e-3;  // output is not delayed
const double k_min_rejection_db = 60.0;

struct RatePair {
    double from_hz;
    double to_hz;
};

// The speed gate holds for an optimized, uninstrumented build (make test); other builds only report the speed.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define UNLIMITED_TEST_SANITIZED
#endif
#endif
#if defined(__OPTIMIZE__) && !defined(__SANITIZE_ADDRESS__) && !defined(UNLIMITED_TEST_SANITIZED)
const bool k_speed_gated = true;
#else
const bool k_speed_gated = false;
#endif

// Whole rates with a short ratio use exact rows; 48001 and 8001 have none to 8000 and 48000, so they use the
// interpolated rows, as would any other ratio.
const RatePair k_pairs[] = {{8000.0, 48000.0}, {48000.0, 8000.0}, {44100.0, 8000.0}, {22050.0, 8000.0},
                            {16000.0, 8000.0}, {11025.0, 8000.0}, {48001.0, 8000.0}, {8001.0, 48000.0}};

size_t samples(double seconds, double rate) {
    return static_cast<size_t>(std::lround(seconds * rate));
}

double amplitude_db(double ratio) {
    return 20.0 * std::log10(ratio);
}

std::vector<float> sine(double freq_hz, double rate, double seconds) {
    std::vector<float> x(samples(seconds, rate));
    for (size_t n = 0; n < x.size(); ++n)
        x[n] = static_cast<float>(k_amplitude * std::sin(k_two_pi * freq_hz * n / rate));
    return x;
}

// Complex amplitude of freq_hz over the measurement window, phase referred to sample 0.
// A sine of amplitude A reads -jA.
Complex phasor(const std::vector<float>& x, double freq_hz, double rate) {
    const size_t start = samples(k_lead_s, rate);
    const size_t count = samples(k_measure_s, rate);
    Complex acc;
    for (size_t n = start; n < start + count; ++n)
        acc += static_cast<double>(x[n]) * std::polar(1.0, -k_two_pi * freq_hz * n / rate);
    return acc * (2.0 / static_cast<double>(count));
}

double level_db(const std::vector<float>& x, double freq_hz, double rate) {
    return amplitude_db(std::abs(phasor(x, freq_hz, rate)) / k_amplitude);
}

// Frequency at which a tone at freq_hz appears after sampling at rate.
double alias_of(double freq_hz, double rate) {
    return std::fabs(freq_hz - rate * std::round(freq_hz / rate));
}

}  // namespace

TEST(resampler_passband_gain_and_zero_delay) {
    const double tones_hz[] = {100.0, 1000.0, 2400.0, 3600.0};
    for (size_t p = 0; p < count_of(k_pairs); ++p) {
        const RatePair pair = k_pairs[p];
        const double low_rate = std::min(pair.from_hz, pair.to_hz);
        double worst_db = 0.0;
        double worst_phase = 0.0;
        for (size_t t = 0; t < count_of(tones_hz); ++t) {
            const double f = tones_hz[t];
            REQUIRE(f <= k_passband_fraction * low_rate);
            const std::vector<float> out = resample(sine(f, pair.from_hz, k_signal_s), pair.from_hz, pair.to_hz);
            const Complex z = phasor(out, f, pair.to_hz);
            const double gain_db = amplitude_db(std::abs(z) / k_amplitude);
            const double phase_error = std::arg(z * Complex(0.0, 1.0));  // -jA rotated to the real axis
            if (std::fabs(gain_db) > std::fabs(worst_db)) worst_db = gain_db;
            if (std::fabs(phase_error) > std::fabs(worst_phase)) worst_phase = phase_error;
            CHECK_NEAR(gain_db, 0.0, k_max_ripple_db);
            CHECK_NEAR(phase_error, 0.0, k_max_phase_error_rad);
        }
        NOTE("%.0f -> %.0f Hz: worst passband gain %+.5f dB, worst phase error %+.2e rad", pair.from_hz, pair.to_hz,
             worst_db, worst_phase);
    }
}

TEST(resampler_alias_rejection) {
    struct Case {
        double from_hz;
        double to_hz;
        double tones_hz[5];
    };
    // Every tone is at or above 0.55 of the output rate and below the input Nyquist.
    const Case cases[] = {{48000.0, 8000.0, {4400.0, 5000.0, 7000.0, 12345.0, 21000.0}},
                          {44100.0, 8000.0, {4400.0, 5000.0, 7000.0, 12345.0, 21000.0}},
                          {22050.0, 8000.0, {4400.0, 5000.0, 7000.0, 9000.0, 11000.0}},
                          {16000.0, 8000.0, {4400.0, 5000.0, 6000.0, 7000.0, 7900.0}},
                          {11025.0, 8000.0, {4400.0, 5000.0, 5500.0, 4700.0, 5200.0}},
                          {48001.0, 8000.0, {4400.0, 5000.0, 7000.0, 12345.0, 21000.0}}};
    for (size_t c = 0; c < count_of(cases); ++c) {
        double worst_db = -std::numeric_limits<double>::infinity();
        for (size_t t = 0; t < count_of(cases[c].tones_hz); ++t) {
            const double f = cases[c].tones_hz[t];
            REQUIRE(f >= k_stopband_fraction * cases[c].to_hz && f < cases[c].from_hz / 2.0);
            const std::vector<float> out = resample(sine(f, cases[c].from_hz, k_signal_s), cases[c].from_hz,
                                                    cases[c].to_hz);
            const double alias_db = level_db(out, alias_of(f, cases[c].to_hz), cases[c].to_hz);
            worst_db = std::max(worst_db, alias_db);
            if (!CHECK(alias_db < -k_min_rejection_db)) NOTE("%.0f Hz aliases at %.1f dB", f, alias_db);
        }
        NOTE("%.0f -> %.0f Hz: worst alias %.1f dB", cases[c].from_hz, cases[c].to_hz, worst_db);
    }
}

TEST(resampler_image_rejection) {
    const double from_rates_hz[] = {8000.0, 8001.0};  // exact and interpolated rows
    const double to_hz = 48000.0;
    const double tones_hz[] = {1000.0, 2400.0, 3600.0};
    const int max_image_order = 3;
    for (size_t r = 0; r < count_of(from_rates_hz); ++r) {
        const double from_hz = from_rates_hz[r];
        double worst_db = -std::numeric_limits<double>::infinity();
        for (size_t t = 0; t < count_of(tones_hz); ++t) {
            const double f = tones_hz[t];
            const std::vector<float> out = resample(sine(f, from_hz, k_signal_s), from_hz, to_hz);
            for (int k = 1; k <= max_image_order; ++k) {
                const double images_hz[] = {k * from_hz - f, k * from_hz + f};
                for (size_t i = 0; i < count_of(images_hz); ++i) {
                    if (images_hz[i] >= to_hz / 2.0) continue;
                    const double image_db = level_db(out, images_hz[i], to_hz);
                    worst_db = std::max(worst_db, image_db);
                    if (!CHECK(image_db < -k_min_rejection_db))
                        NOTE("%.0f Hz image at %.0f Hz: %.1f dB", f, images_hz[i], image_db);
                }
            }
        }
        NOTE("%.0f -> %.0f Hz: worst image %.1f dB", from_hz, to_hz, worst_db);
    }
}

TEST(resampler_chunking_invariance) {
    const size_t max_chunk = 700;
    const double freq = 1234.0;
    const double noise_sigma = 0.1;
    const double duration_s = 1.0;
    const std::uint32_t seed = 99;
    const RatePair clock_pairs[] = {{8000.0 * (1.0 + 1e-3), 8000.0}, {8000.37, 8000.0}, {8000.0, 8000.0}};
    std::vector<RatePair> pairs(k_pairs, k_pairs + count_of(k_pairs));
    pairs.insert(pairs.end(), clock_pairs, clock_pairs + count_of(clock_pairs));
    std::mt19937 rng(seed);
    std::normal_distribution<double> normal(0.0, noise_sigma);
    std::uniform_int_distribution<size_t> chunk_size(0, max_chunk);
    for (size_t p = 0; p < pairs.size(); ++p) {
        std::vector<float> input = sine(freq, pairs[p].from_hz, duration_s);
        for (size_t n = 0; n < input.size(); ++n) input[n] += static_cast<float>(normal(rng));
        const std::vector<float> one_shot = resample(input, pairs[p].from_hz, pairs[p].to_hz);

        Resampler resampler(pairs[p].from_hz, pairs[p].to_hz);
        std::vector<float> chunked;
        size_t pos = 0;
        while (pos < input.size()) {
            const size_t count = std::min(chunk_size(rng), input.size() - pos);
            resampler.process(&input[pos], count, chunked);
            pos += count;
        }
        resampler.flush(chunked);
        const bool identical = chunked.size() == one_shot.size() &&
                               std::memcmp(chunked.data(), one_shot.data(), one_shot.size() * sizeof(float)) == 0;
        if (!CHECK(identical)) NOTE("%.3f -> %.0f Hz differs", pairs[p].from_hz, pairs[p].to_hz);
    }
}

TEST(resampler_length_identity_and_restart) {
    const double freq = 700.0;
    const double duration_s = 0.3;
    for (size_t p = 0; p < count_of(k_pairs); ++p) {
        const std::vector<float> input = sine(freq, k_pairs[p].from_hz, duration_s);
        const size_t expected =
            static_cast<size_t>(std::ceil(static_cast<double>(input.size()) * k_pairs[p].to_hz / k_pairs[p].from_hz));
        const std::vector<float> out = resample(input, k_pairs[p].from_hz, k_pairs[p].to_hz);
        CHECK_EQ(out.size(), expected);

        // flush() ends the stream and starts over: the same input gives the same output again.
        Resampler resampler(k_pairs[p].from_hz, k_pairs[p].to_hz);
        std::vector<float> first;
        std::vector<float> second;
        resampler.process(&input[0], input.size(), first);
        resampler.flush(first);
        resampler.process(&input[0], input.size(), second);
        resampler.flush(second);
        CHECK(first == out);
        CHECK(second == out);
    }

    const double rate = 8000.0;
    const std::vector<float> input = sine(freq, rate, duration_s);
    CHECK(resample(input, rate, rate) == input);
    CHECK(resample(std::vector<float>(), rate, rate).empty());

    // Before flush() the output holds back the filter half-length of input.
    Resampler held(rate, rate);
    std::vector<float> partial;
    held.process(&input[0], input.size(), partial);
    CHECK(partial.size() < input.size());
    CHECK(std::equal(partial.begin(), partial.end(), input.begin()));
}

TEST(resampler_rejects_invalid_rates) {
    const double invalid[][2] = {{0.0, 8000.0},
                                 {8000.0, 0.0},
                                 {-8000.0, 8000.0},
                                 {std::numeric_limits<double>::quiet_NaN(), 8000.0},
                                 {8000.0, std::numeric_limits<double>::infinity()}};
    for (size_t i = 0; i < count_of(invalid); ++i) {
        bool thrown = false;
        try {
            Resampler resampler(invalid[i][0], invalid[i][1]);
        } catch (const std::invalid_argument&) {
            thrown = true;
        }
        if (!CHECK(thrown)) NOTE("%g -> %g accepted", invalid[i][0], invalid[i][1]);
    }
}

TEST(resampler_speed) {
    // Streams in device-sized chunks and keeps the best of a few runs, so a busy machine does not fail the gate.
    // The kernel rows are built by the constructor, outside the timed loop.
    const RatePair pairs[] = {{48000.0, 8000.0}, {44100.0, 8000.0}, {22050.0, 8000.0}, {16000.0, 8000.0},
                              {11025.0, 8000.0}, {8000.0, 48000.0}, {48001.0, 8000.0}};
    const double duration_s = 30.0;
    const double freq = 1000.0;
    const size_t chunk = 256;
    const int runs = 3;
    const double min_speed = 500.0;  // times real time; about 7000 for 48000 -> 8000 on an Apple M4
    typedef std::chrono::steady_clock Clock;
    for (size_t p = 0; p < count_of(pairs); ++p) {
        const std::vector<float> input = sine(freq, pairs[p].from_hz, duration_s);
        double best_s = std::numeric_limits<double>::infinity();
        for (int run = 0; run < runs; ++run) {
            Resampler resampler(pairs[p].from_hz, pairs[p].to_hz);
            std::vector<float> out;
            out.reserve(samples(duration_s, pairs[p].to_hz) + 1);
            const Clock::time_point start = Clock::now();
            for (size_t pos = 0; pos < input.size(); pos += chunk)
                resampler.process(&input[pos], std::min(chunk, input.size() - pos), out);
            resampler.flush(out);
            best_s = std::min(best_s, std::chrono::duration<double>(Clock::now() - start).count());
        }
        const double speed = duration_s / best_s;
        NOTE("%.0f -> %.0f Hz: %.0fx real time%s", pairs[p].from_hz, pairs[p].to_hz, speed,
             k_speed_gated ? "" : " (not gated in this build)");
        if (k_speed_gated) CHECK(speed >= min_speed);
    }
}
