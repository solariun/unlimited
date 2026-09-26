#include "test_harness.hpp"
#include "unlimited/protocol.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>

using unlimited::cosine_q15;
using unlimited::k_quarter_sine;
using unlimited::k_quarter_table_size;
using unlimited::sine_q15;

namespace {

const double two_pi = 6.28318530717958647692;
const double turn = 4294967296.0;
const double q15_full_scale = 32767.0;
const double max_error_lsb = 1.0;
const unsigned grid_bits = 16;
const std::uint32_t grid_points = 1u << grid_bits;
const std::uint32_t grid_shift = 32 - grid_bits;
const std::uint32_t random_points = 1u << 20;
const std::uint32_t half_turn = 0x80000000u;
const std::uint32_t quarter_turn = 0x40000000u;

// Numerical Recipes LCG: deterministic phases between the grid points.
std::uint32_t next_random(std::uint32_t& state) {
    const std::uint32_t multiplier = 1664525u;
    const std::uint32_t increment = 1013904223u;
    state = state * multiplier + increment;
    return state;
}

double error_lsb(std::uint32_t phase) {
    const double reference = q15_full_scale * std::sin(two_pi * phase / turn);
    return std::fabs(sine_q15(phase) - reference);
}

}  // namespace

TEST(tables_quarter_sine_endpoints) {
    CHECK_EQ(k_quarter_sine[0], 0);
    CHECK_EQ(k_quarter_sine[k_quarter_table_size - 1], 2 * 32767);  // twice the Q15 full scale
    for (unsigned i = 1; i < k_quarter_table_size; ++i) CHECK(k_quarter_sine[i] > k_quarter_sine[i - 1]);
}

TEST(tables_sine_within_one_lsb_on_grid) {
    double worst = 0.0;
    for (std::uint32_t k = 0; k < grid_points; ++k) {
        const double error = error_lsb(k << grid_shift);
        if (error > worst) worst = error;
    }
    NOTE("max |sine_q15 - 32767 sin| over 2^16 phases: %.4f LSB", worst);
    CHECK(worst <= max_error_lsb);
}

TEST(tables_sine_within_one_lsb_between_grid_points) {
    std::uint32_t state = 1;
    double worst = 0.0;
    for (std::uint32_t k = 0; k < random_points; ++k) {
        const double error = error_lsb(next_random(state));
        if (error > worst) worst = error;
    }
    NOTE("max error over 2^20 pseudo-random phases: %.4f LSB", worst);
    CHECK(worst <= max_error_lsb);
}

TEST(tables_symmetry_and_cosine) {
    std::uint32_t state = 7;
    for (std::uint32_t k = 0; k < grid_points; ++k) {
        const std::uint32_t phase = next_random(state);
        if (!CHECK_EQ(sine_q15(phase + half_turn), -sine_q15(phase))) break;
        if (!CHECK_EQ(cosine_q15(phase), sine_q15(phase + quarter_turn))) break;
    }
    CHECK_EQ(sine_q15(0), 0);
    CHECK_EQ(sine_q15(quarter_turn), 32767);
    CHECK_EQ(sine_q15(half_turn), 0);
    CHECK_EQ(sine_q15(3 * quarter_turn), -32767);
    CHECK_EQ(cosine_q15(0), 32767);
}
