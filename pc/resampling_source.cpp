#include "resampling_source.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace unlimited {
namespace pc {

using std::int16_t;
using std::int64_t;
using std::size_t;
using std::uint64_t;

const size_t ResamplingSource::k_pull_samples;

namespace {

const double k_ms_per_s = 1000.0;

// Rounded and clamped, as ResamplingSink converts.
int16_t to_int16(float x) {
    const float low = std::numeric_limits<int16_t>::min();
    const float high = std::numeric_limits<int16_t>::max();
    return static_cast<int16_t>(std::lround(std::max(low, std::min(high, x))));
}

size_t power_of_two_at_least(size_t value) {
    size_t power = 1;
    while (power < value) power <<= 1;
    return power;
}

}  // namespace

ResamplingSource::ResamplingSource(SampleSource& source, double from_hz, double to_hz)
    : source_(source),
      copy_(from_hz == to_hz),
      from_hz_(from_hz),
      kernel_(from_hz, to_hz),
      span_(power_of_two_at_least(kernel_.row_length() + k_pull_samples)),
      history_(2 * span_, 0.0f),
      pulled_(0),
      output_count_(0),
      scratch_() {}

// Output m needs the inputs up to position(m).index + taps_after; they are pulled at most k_pull_samples at a time,
// so the oldest input a row still needs (index - taps_before) is never overwritten: span_ >= row + pull.
size_t ResamplingSource::read(int16_t* out, size_t count) {
    if (copy_) {
        const size_t got = std::min(count, source_.read(out, count));
        std::fill(out + got, out + count, static_cast<int16_t>(0));
        return count;
    }
    if (count == 0) return 0;
    const int64_t last_needed = kernel_.position(output_count_ + count - 1).index + kernel_.taps_after() + 1;
    const size_t mask = span_ - 1;
    for (size_t i = 0; i < count; ++i, ++output_count_) {
        const ResamplerKernel::Position p = kernel_.position(output_count_);
        const int64_t needed = p.index + kernel_.taps_after() + 1;
        while (pulled_ < needed) pull(std::min<int64_t>(static_cast<int64_t>(k_pull_samples), last_needed - pulled_));
        out[i] = to_int16(kernel_.apply(p, &history_[static_cast<size_t>(p.index) & mask]));
    }
    return count;
}

// Input i goes to slot (i + taps_before) mod span_ and its mirror: the zeros before the stream are the first
// taps_before slots, and a row starting at input p.index - taps_before starts at slot p.index mod span_.
void ResamplingSource::pull(int64_t count) {
    const size_t wanted = static_cast<size_t>(count);
    const size_t got = std::min(wanted, source_.read(scratch_, wanted));
    std::fill(scratch_ + got, scratch_ + wanted, static_cast<int16_t>(0));
    const size_t mask = span_ - 1;
    for (size_t i = 0; i < wanted; ++i) {
        const size_t slot = static_cast<size_t>(pulled_ + kernel_.taps_before()) & mask;
        const float sample = static_cast<float>(scratch_[i]);
        history_[slot] = sample;
        history_[slot + span_] = sample;
        ++pulled_;
    }
}

double ResamplingSource::lead_ms() const {
    return copy_ ? 0.0 : static_cast<double>(kernel_.taps_after()) * k_ms_per_s / from_hz_;
}

}  // namespace pc
}  // namespace unlimited
