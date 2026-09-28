#pragma once

#include "resampler.hpp"
#include "unlimited/audio_io.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace unlimited {
namespace pc {

// Plays an 8 kHz source (the modem core's audio_output()) at a device's rate from inside the device's real-time
// callback (spec 12.5, open issue 1 of the radio I/O layer): the Resampler's filter (ResamplerKernel), with every
// buffer sized here, so read() never allocates and never locks. The output equals pc::resample() of the same input,
// whatever the read() sizes.
//
// read() always fills `count`: a short read of the source is taken as silence (the modem's source never ends). The
// source is read ahead of the output by the filter's half-length: lead_ms() (3 ms from 8000 Hz), which adds to the
// device's latency before the PTT release.
class ResamplingSource final : public SampleSource {
public:
    static const std::size_t k_pull_samples = 256;  // source samples read per call of the source's read()

    ResamplingSource(SampleSource& source, double from_hz, double to_hz);  // throws std::invalid_argument (rates)

    std::size_t read(std::int16_t* out, std::size_t count) override;
    double lead_ms() const;

private:
    void pull(std::int64_t count);

    SampleSource& source_;
    bool copy_;             // equal rates: the source is read as it is
    double from_hz_;
    ResamplerKernel kernel_;
    std::size_t span_;      // a power of two >= a row and one pull: the history holds this many inputs
    std::vector<float> history_;  // 2 * span_: input i at (i + taps_before) mod span_ and again span_ later, so a
                                  // row's inputs are always contiguous
    std::int64_t pulled_;         // inputs in the history
    std::uint64_t output_count_;
    std::int16_t scratch_[k_pull_samples];
};

}  // namespace pc
}  // namespace unlimited
