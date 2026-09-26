#include "resampler.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace unlimited {
namespace pc {

using std::int64_t;
using std::size_t;
using std::uint64_t;

namespace {

const double k_pi = 3.14159265358979323846;

// Kernel sinc(z) * kaiser(z / k_zero_crossings), z in sample periods of the lower rate. With 24 zero
// crossings per side and beta 7.5: ripple 0.0012 dB up to 0.45, -77 dB from 0.55 of the lower rate.
const double k_zero_crossings = 24.0;
const double k_kaiser_beta = 7.5;
const double k_bessel_tolerance = 1e-17;
const size_t k_lanes = 16;                         // independent dot-product sums; rows are padded to a multiple
const double k_max_whole_rate = 4294967296.0;      // 2^32: rates that may get exact rows
const uint64_t k_max_exact_coefficients = 1 << 18;  // beyond this table size, rows are interpolated
const double k_rows_per_zero_crossing = 256.0;     // interpolated rows: linear interpolation error below -90 dB
const size_t k_compact_samples = 4096;             // spent history is dropped in blocks of at least this size

double bessel_i0(double x) {
    const double half = x / 2.0;
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; term > k_bessel_tolerance * sum; ++k) {
        term *= (half / k) * (half / k);
        sum += term;
    }
    return sum;
}

double kernel(double z) {
    static const double norm = bessel_i0(k_kaiser_beta);
    const double a = std::fabs(z);
    if (a == 0.0) return 1.0;
    if (a >= k_zero_crossings || a == std::floor(a)) return 0.0;  // exact zeros make equal rates an exact copy
    const double r = z / k_zero_crossings;
    return std::sin(k_pi * z) / (k_pi * z) * bessel_i0(k_kaiser_beta * std::sqrt(1.0 - r * r)) / norm;
}

uint64_t gcd(uint64_t a, uint64_t b) {
    while (b != 0) {
        const uint64_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

bool whole(double hz) {
    return hz == std::floor(hz) && hz <= k_max_whole_rate;
}

// k_lanes independent sums run in parallel (SIMD) while the result stays fixed by the source order alone:
// it never depends on the alignment of the data, so chunking cannot change it.
float dot(const float* a, const float* b, size_t count) {
    float sums[k_lanes] = {};
    for (size_t i = 0; i < count; i += k_lanes)
        for (size_t lane = 0; lane < k_lanes; ++lane) sums[lane] += a[i + lane] * b[i + lane];
    float total = 0.0f;
    for (size_t lane = 0; lane < k_lanes; ++lane) total += sums[lane];
    return total;
}

}  // namespace

Resampler::Resampler(double from_hz, double to_hz)
    : exact_(false),
      step_(from_hz / to_hz),
      step_whole_(0),
      step_phase_(0),
      phase_count_(0),
      row_length_(0),
      taps_before_(0),
      taps_after_(0),
      history_start_(0),
      input_count_(0),
      output_count_(0) {
    if (!(std::isfinite(from_hz) && from_hz > 0.0 && std::isfinite(to_hz) && to_hz > 0.0))
        throw std::invalid_argument("Resampler: rates must be finite and > 0");

    // Input samples per sample period of the lower rate: the kernel is this much wider, and lower by as much.
    const double stretch = std::max(1.0, step_);
    taps_after_ = static_cast<int64_t>(std::ceil(k_zero_crossings * stretch));
    row_length_ = (2 * static_cast<size_t>(taps_after_) + k_lanes - 1) / k_lanes * k_lanes;
    taps_before_ = static_cast<int64_t>(row_length_) - taps_after_ - 1;

    // Output m falls at m * from / to input samples: with from / to = p / q in lowest terms, on q fractions.
    if (whole(from_hz) && whole(to_hz)) {
        const uint64_t divisor = gcd(static_cast<uint64_t>(from_hz), static_cast<uint64_t>(to_hz));
        const uint64_t p = static_cast<uint64_t>(from_hz) / divisor;
        const uint64_t q = static_cast<uint64_t>(to_hz) / divisor;
        exact_ = q * row_length_ <= k_max_exact_coefficients;
        if (exact_) {
            phase_count_ = q;
            step_whole_ = p / q;
            step_phase_ = p % q;
        }
    }
    if (!exact_) phase_count_ = static_cast<uint64_t>(std::ceil(k_rows_per_zero_crossing / stretch));

    const size_t rows = static_cast<size_t>(phase_count_) + (exact_ ? 0 : 1);  // fraction 1 closes the last span
    rows_.resize(rows * row_length_);
    for (size_t r = 0; r < rows; ++r) {
        const double fraction = static_cast<double>(r) / static_cast<double>(phase_count_);
        for (size_t i = 0; i < row_length_; ++i) {
            // Output time minus the input time of tap i, in input samples.
            const double offset = fraction - (static_cast<double>(i) - static_cast<double>(taps_before_));
            rows_[r * row_length_ + i] = static_cast<float>(kernel(offset / stretch) / stretch);
        }
    }
    restart();
}

void Resampler::process(const float* in, size_t count, std::vector<float>& out) {
    history_.insert(history_.end(), in, in + count);
    input_count_ += static_cast<int64_t>(count);
    emit(out, false);

    const int64_t keep_from = position(output_count_).index - taps_before_;
    if (keep_from - history_start_ < static_cast<int64_t>(k_compact_samples)) return;
    history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(keep_from - history_start_));
    history_start_ = keep_from;
}

void Resampler::flush(std::vector<float>& out) {
    history_.resize(history_.size() + static_cast<size_t>(taps_after_), 0.0f);
    emit(out, true);
    restart();
}

void Resampler::restart() {
    history_.assign(static_cast<size_t>(taps_before_), 0.0f);
    history_start_ = -taps_before_;
    input_count_ = 0;
    output_count_ = 0;
}

// Exact rows split m * step into whole samples and a phase in integers, so the time never drifts.
Resampler::Position Resampler::position(uint64_t output) const {
    Position p;
    if (exact_) {
        const uint64_t phase = output * step_phase_;
        p.index = static_cast<int64_t>(output * step_whole_ + phase / phase_count_);
        p.row = static_cast<size_t>(phase % phase_count_);
        p.weight = 0.0f;
    } else {
        const double time = static_cast<double>(output) * step_;
        const double whole_part = std::floor(time);
        const double row = (time - whole_part) * static_cast<double>(phase_count_);
        p.index = static_cast<int64_t>(whole_part);
        p.row = static_cast<size_t>(row);
        p.weight = static_cast<float>(row - std::floor(row));
    }
    return p;
}

// Before the end, an output waits for its last tap; at the end, zeros stand in for the input past the last
// sample and outputs continue up to the end of the input.
void Resampler::emit(std::vector<float>& out, bool at_end) {
    for (;; ++output_count_) {
        const Position p = position(output_count_);
        if (p.index + (at_end ? 0 : taps_after_) >= input_count_) return;
        const float* x = &history_[static_cast<size_t>(p.index - taps_before_ - history_start_)];
        const float* row = &rows_[p.row * row_length_];
        float y = dot(row, x, row_length_);
        if (p.weight != 0.0f) y += p.weight * (dot(row + row_length_, x, row_length_) - y);
        out.push_back(y);
    }
}

std::vector<float> resample(const std::vector<float>& in, double from_hz, double to_hz) {
    Resampler resampler(from_hz, to_hz);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(std::ceil(static_cast<double>(in.size()) * to_hz / from_hz)) + 1);
    if (!in.empty()) resampler.process(&in[0], in.size(), out);
    resampler.flush(out);
    return out;
}

}  // namespace pc
}  // namespace unlimited
