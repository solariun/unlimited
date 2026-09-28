// The real-time output resampler (spec 12.5, open issue 1 of the radio I/O layer): the modem's 8 kHz audio at a
// device's rate, inside the device callback: the Resampler's output exactly, no allocation in read().
#include "audio.hpp"
#include "resampling_source.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <random>
#include <stdexcept>
#include <vector>

using unlimited::pc::ResamplingSource;
using unlimited::pc::resample;

// Allocation counter for the whole test binary: operator new counts while this thread asks it to. Both stay out of
// line: g++ inlines one of them into its caller and then sees operator new paired with free (or malloc with operator
// delete), a false -Wmismatched-new-delete that -Werror turns into a build failure (g++ 13 on Linux, -O2).
namespace {

thread_local bool t_counting = false;
thread_local std::size_t t_allocations = 0;

}  // namespace

[[gnu::noinline]] void* operator new(std::size_t size) {
    if (t_counting) ++t_allocations;
    void* memory = std::malloc(size != 0 ? size : 1);
    if (memory == nullptr) throw std::bad_alloc();
    return memory;
}

[[gnu::noinline]] void operator delete(void* memory) noexcept {
    std::free(memory);
}

namespace {

using std::int16_t;
using std::size_t;

const double k_modem_rate_hz = 8000.0;
const size_t k_input_samples = 24000;  // 3 s at 8 kHz
const int16_t k_peak = 20000;
const uint32_t k_seed = 99;
const size_t k_read_sizes[] = {1, 7, 64, 255, 256, 1000, 1024, 4096, 3};

// A source that plays a buffer, then nothing (read() returns less: silence after).
class BufferSource final : public unlimited::SampleSource {
public:
    explicit BufferSource(const std::vector<int16_t>& samples) : samples_(samples), position_(0), reads_(0) {}

    size_t read(int16_t* out, size_t count) override {
        ++reads_;
        const size_t n = std::min(count, samples_.size() - position_);
        std::copy(samples_.begin() + static_cast<std::ptrdiff_t>(position_),
                  samples_.begin() + static_cast<std::ptrdiff_t>(position_ + n), out);
        position_ += n;
        return n;
    }

    size_t position() const { return position_; }
    size_t reads() const { return reads_; }

private:
    const std::vector<int16_t>& samples_;
    size_t position_;
    size_t reads_;
};

std::vector<int16_t> noise(size_t count) {
    std::mt19937 generator(k_seed);
    std::uniform_int_distribution<int> value(-k_peak, k_peak);
    std::vector<int16_t> samples(count);
    for (size_t i = 0; i < count; ++i) samples[i] = static_cast<int16_t>(value(generator));
    return samples;
}

int16_t rounded(float x) {
    return static_cast<int16_t>(std::lround(std::max(-32768.0f, std::min(32767.0f, x))));
}

// Reads `total` outputs in the sizes of k_read_sizes, in turn.
std::vector<int16_t> read_all(ResamplingSource& source, size_t total) {
    std::vector<int16_t> out(total);
    size_t done = 0;
    for (size_t turn = 0; done < total; ++turn) {
        const size_t size = std::min(k_read_sizes[turn % test::count_of(k_read_sizes)], total - done);
        CHECK_EQ(source.read(&out[done], size), size);
        done += size;
    }
    return out;
}

}  // namespace

// Every rate a device may run at, rational (exact rows) and not (interpolated rows), and equal rates (a copy): the
// output is pc::resample() of the same input, sample for sample, whatever the sizes the device asks for.
TEST(resampling_source_equals_the_resampler) {
    const std::vector<int16_t> input = noise(k_input_samples);
    const std::vector<float> as_float(input.begin(), input.end());
    const double rates[] = {48000.0, 44100.0, 22050.0, 16000.0, 11025.0, 96000.0, 192000.0, 8001.0, 8000.0};
    for (size_t r = 0; r < test::count_of(rates); ++r) {
        const std::vector<float> expected = resample(as_float, k_modem_rate_hz, rates[r]);
        BufferSource upstream(input);
        ResamplingSource source(upstream, k_modem_rate_hz, rates[r]);
        const std::vector<int16_t> got = read_all(source, expected.size());
        size_t differences = 0;
        for (size_t i = 0; i < expected.size(); ++i) differences += got[i] != rounded(expected[i]) ? 1 : 0;
        CHECK_EQ(differences, 0u);
        NOTE("8000 -> %.0f Hz: %zu outputs, %zu differ from pc::resample(), lead %.3f ms", rates[r], expected.size(),
             differences, source.lead_ms());
    }
}

// read() in the device callback: no allocation, whatever the sizes; the history is sized at construction.
TEST(resampling_source_never_allocates_in_read) {
    t_allocations = 0;
    t_counting = true;
    // The counter's own check: one allocation. Called directly: a compiler may drop a container's unused allocation
    // (clang with libc++ does), never a direct call.
    ::operator delete(::operator new(sizeof(int16_t)));
    t_counting = false;
    CHECK_EQ(t_allocations, 1u);
    const std::vector<int16_t> input = noise(k_input_samples);
    const double rates[] = {48000.0, 44100.0, 8001.0, 8000.0};
    for (size_t r = 0; r < test::count_of(rates); ++r) {
        BufferSource upstream(input);
        ResamplingSource source(upstream, k_modem_rate_hz, rates[r]);
        std::vector<int16_t> out(4096);
        t_allocations = 0;
        t_counting = true;
        for (size_t turn = 0; turn < 200; ++turn) source.read(&out[0], k_read_sizes[turn % test::count_of(k_read_sizes)]);
        t_counting = false;
        CHECK_EQ(t_allocations, 0u);
    }
}

// The source is read ahead of the output by the filter's half-length, and not more than a pull beyond it; an impulse
// comes out where the input put it (no delay).
TEST(resampling_source_reads_ahead_by_its_lead) {
    std::vector<int16_t> input(4000, 0);
    const size_t impulse = 1000;
    input[impulse] = k_peak;
    BufferSource upstream(input);
    const double rate = 48000.0;
    ResamplingSource source(upstream, k_modem_rate_hz, rate);
    CHECK_NEAR(source.lead_ms(), 3.0, 1e-9);
    const size_t ratio = 6;
    std::vector<int16_t> out(impulse * ratio);
    CHECK_EQ(source.read(&out[0], out.size()), out.size());
    // Outputs up to the impulse's time need inputs up to 24 samples past it: 3 ms at 8 kHz.
    const size_t lead_samples = 24;
    CHECK(upstream.position() >= impulse + lead_samples - 1);
    CHECK(upstream.position() <= impulse + lead_samples + ResamplingSource::k_pull_samples);
    std::vector<int16_t> more(ratio * 100);
    source.read(&more[0], more.size());
    CHECK(more[0] > k_peak * 9 / 10);  // the peak at output impulse * 6
    ResamplingSource copy(upstream, k_modem_rate_hz, k_modem_rate_hz);
    CHECK_NEAR(copy.lead_ms(), 0.0, 1e-12);
}

TEST(resampling_source_plays_silence_after_its_source) {
    const std::vector<int16_t> input(100, 1000);
    BufferSource upstream(input);
    ResamplingSource source(upstream, k_modem_rate_hz, 48000.0);
    std::vector<int16_t> out(48000);
    CHECK_EQ(source.read(&out[0], out.size()), out.size());
    CHECK_EQ(out.back(), 0);
    CHECK(out[300] != 0);
    bool threw = false;
    try {
        ResamplingSource bad(upstream, 0.0, 48000.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}
