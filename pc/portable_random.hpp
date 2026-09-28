#pragma once

// Random draws that give the same numbers with every standard library (spec 12.8).
//
// std::mt19937 and std::seed_seq are exact in the C++ standard, so a seed gives the same raw numbers everywhere; the
// distributions are not: libc++ (macOS) and libstdc++ (Linux) turn the same raw numbers into different noise. These
// replace the standard distributions with libc++'s algorithms, written out, so every number measured on macOS repeats
// on Linux. Each keeps the shape of the standard class it replaces: constructed with its parameters, called with the
// engine.

#include <cmath>
#include <cstdint>
#include <random>

namespace unlimited {
namespace sim {

// A double in [0, 1) from two engine outputs, as std::generate_canonical<double, 53>.
inline double canonical(std::mt19937& engine) {
    const double outputs = static_cast<double>(std::mt19937::max()) + 1.0;  // 2^32 values per output
    double sum = static_cast<double>(engine());
    sum += static_cast<double>(engine()) * outputs;
    return sum / (outputs * outputs);
}

// Uniform in [low, high), as std::uniform_real_distribution.
class UniformReal {
public:
    explicit UniformReal(double low = 0.0, double high = 1.0) : low_(low), high_(high) {}

    double operator()(std::mt19937& engine) const { return (high_ - low_) * canonical(engine) + low_; }

private:
    double low_;
    double high_;
};

// Uniform whole numbers in [low, high], at most 2^32 of them, as libc++'s std::uniform_int_distribution: the fewest
// low bits of one engine output that can hold the range, drawn again while the value is too large. A range of one
// value draws nothing. Default: the single value 0, until assigned.
template <class Integer>
class UniformInteger {
public:
    UniformInteger() : UniformInteger(0, 0) {}
    UniformInteger(Integer low, Integer high)
        : low_(low),
          range_(static_cast<std::uint64_t>(static_cast<std::int64_t>(high) - static_cast<std::int64_t>(low)) + 1),
          mask_(0) {
        while (mask_ < range_ - 1) mask_ = (mask_ << 1) | 1;
    }

    Integer operator()(std::mt19937& engine) const {
        if (range_ == 1) return low_;
        std::uint64_t value;
        do {
            value = engine() & mask_;
        } while (value >= range_);
        return static_cast<Integer>(static_cast<std::int64_t>(low_) + static_cast<std::int64_t>(value));
    }

private:
    Integer low_;
    std::uint64_t range_;
    std::uint64_t mask_;
};

// Exponential with the given rate, as std::exponential_distribution.
class Exponential {
public:
    explicit Exponential(double rate = 1.0) : rate_(rate) {}

    double operator()(std::mt19937& engine) const { return -std::log(1.0 - canonical(engine)) / rate_; }

private:
    double rate_;
};

// Gamma with a shape above 1 and the given scale, as libc++'s std::gamma_distribution: Best's rejection method (1978),
// whose constants these are.
class Gamma {
public:
    Gamma(double shape, double scale) : shape_(shape), scale_(scale) {}

    double operator()(std::mt19937& engine) const {
        const UniformReal unit(0.0, 1.0);
        const double b = shape_ - 1;
        const double c = 3 * shape_ - 0.75;
        double x = 0.0;
        while (true) {
            const double u = unit(engine);
            const double v = unit(engine);
            const double w = u * (1 - u);
            if (w != 0) {
                const double y = std::sqrt(c / w) * (u - 0.5);
                x = b + y;
                if (x >= 0) {
                    const double z = 64 * w * w * w * v * v;
                    if (z <= 1 - 2 * y * y / x) break;
                    if (std::log(z) <= 2 * (b * std::log(x / b) - y)) break;
                }
            }
        }
        return x * scale_;
    }

private:
    double shape_;
    double scale_;
};

// Gaussian, as libc++'s std::normal_distribution: the polar method; of the two values it makes, the first is returned
// and the second kept for the next call (libstdc++ returns them the other way round).
class Normal {
public:
    explicit Normal(double mean = 0.0, double sigma = 1.0) : mean_(mean), sigma_(sigma), kept_(0.0), has_kept_(false) {}

    double operator()(std::mt19937& engine) {
        double value;
        if (has_kept_) {
            has_kept_ = false;
            value = kept_;
        } else {
            const UniformReal square(-1.0, 1.0);
            double u;
            double v;
            double s;
            do {
                u = square(engine);
                v = square(engine);
                s = u * u + v * v;
            } while (s > 1.0 || s == 0.0);
            const double factor = std::sqrt(-2.0 * std::log(s) / s);
            kept_ = v * factor;
            has_kept_ = true;
            value = u * factor;
        }
        return value * sigma_ + mean_;
    }

private:
    double mean_;
    double sigma_;
    double kept_;
    bool has_kept_;
};

}  // namespace sim
}  // namespace unlimited
