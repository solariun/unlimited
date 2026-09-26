#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace unlimited {
namespace pc {

// Streaming resampler for any rate ratio: a Kaiser-windowed sinc whose cutoff is half the lower of the two
// rates, so it anti-aliases when decimating and removes the images when interpolating. Flat within 0.01 dB
// up to 0.45 * min(rate), at least 70 dB down from 0.55 * min(rate).
//
// Polyphase: the kernel is precomputed as rows, one per fraction of an input sample an output can fall on.
// When both rates are whole numbers with a short ratio (48000, 44100, 22050, 16000, 11025 to 8000 and back)
// the fractions repeat and every row is exact; otherwise rows are spaced finely and the two nearest are
// interpolated.
//
// Output sample m is the band-limited input at time m / to_hz: no delay. It is emitted once every input
// sample it needs has arrived, so process() holds back the last filter half-length of input until flush().
// The output depends only on the input stream, never on how it is chunked.
class Resampler {
public:
    Resampler(double from_hz, double to_hz);  // throws std::invalid_argument unless both rates are finite and > 0

    void process(const float* in, std::size_t count, std::vector<float>& out);  // appends
    void flush(std::vector<float>& out);  // appends the rest, up to the end of the input, then starts over

private:
    struct Position {
        std::int64_t index;  // input sample at or just before the output time
        std::size_t row;
        float weight;  // of the next row; 0 for exact rows
    };

    Position position(std::uint64_t output) const;
    void emit(std::vector<float>& out, bool at_end);
    void restart();

    bool exact_;
    double step_;               // input samples per output sample (interpolated rows)
    std::uint64_t step_whole_;  // exact rows: step = step_whole_ + step_phase_ / phase_count_
    std::uint64_t step_phase_;
    std::uint64_t phase_count_;  // rows per input sample
    std::size_t row_length_;     // taps per row, padded to the dot-product width
    std::int64_t taps_before_;   // a row spans index - taps_before_ .. index + taps_after_
    std::int64_t taps_after_;
    std::vector<float> rows_;
    std::vector<float> history_;   // zeros stand in for the input before the start
    std::int64_t history_start_;   // input index of history_[0]
    std::int64_t input_count_;
    std::uint64_t output_count_;
};

std::vector<float> resample(const std::vector<float>& in, double from_hz, double to_hz);

}  // namespace pc
}  // namespace unlimited
