#include "audio.hpp"
#include "resampler.hpp"
#include "test_harness.hpp"
#include "unlimited/encoder.hpp"
#include "wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using unlimited::SampleSink;
using unlimited::SampleSource;
using unlimited::pc::InputDevice;
using unlimited::pc::MemoryInput;
using unlimited::pc::MemoryOutput;
using unlimited::pc::MemorySource;
using unlimited::pc::OutputDevice;
using unlimited::pc::ResamplingSink;
using unlimited::pc::open_input;
using unlimited::pc::open_output;
using unlimited::wav::FileByteSink;
using unlimited::wav::FileByteSource;
using unlimited::pc::resample;

namespace {

using std::int16_t;
using std::size_t;
using std::uint32_t;
using std::uint8_t;
using test::count_of;

const double k_pi = 3.14159265358979323846;
const double k_two_pi = 2.0 * k_pi;
const uint32_t k_decoder_rate = 8000;
const size_t k_wav_header_bytes = 44;
const size_t k_bytes_per_sample = 2;
const size_t k_riff_size_offset = 4;
const size_t k_data_size_offset = 40;
const size_t k_riff_size_excluded = 8;  // "RIFF" and the size field itself
const unsigned k_bits_per_byte = 8;

std::string temp_path(const std::string& name) {
    const char* dir = std::getenv("TMPDIR");
    std::string base = (dir != nullptr && *dir != '\0') ? dir : "/tmp";
    if (base[base.size() - 1] != '/') base += '/';
    return base + "unlimited_pc_audio_" + name;
}

std::vector<uint8_t> file_bytes(const std::string& path) {
    std::ifstream file(path.c_str(), std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

uint32_t read_u32(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < sizeof(uint32_t); ++i)
        value |= static_cast<uint32_t>(bytes[offset + i]) << (k_bits_per_byte * i);
    return value;
}

std::vector<int16_t> tone(double freq_hz, double amplitude, size_t count, double rate) {
    std::vector<int16_t> x(count);
    for (size_t n = 0; n < count; ++n)
        x[n] = static_cast<int16_t>(std::lround(amplitude * std::sin(k_two_pi * freq_hz * n / rate)));
    return x;
}

// Collects what an input pushes; optionally stops the device after a number of writes.
class CollectSink final : public SampleSink {
public:
    void write(const int16_t* in, size_t count) override {
        samples.insert(samples.end(), in, in + count);
        chunks.push_back(count);
        if (device != nullptr && chunks.size() == stop_after_writes) device->stop();
    }

    std::vector<int16_t> samples;
    std::vector<size_t> chunks;
    unlimited::AudioInput* device = nullptr;
    size_t stop_after_writes = 0;
};

// Produces total samples of a ramp; optionally stops an output device after a number of reads.
class RampSource final : public SampleSource {
public:
    explicit RampSource(size_t total) : total_(total) {}

    size_t read(int16_t* out, size_t count) override {
        ++reads;
        if (device != nullptr && reads == stop_after_reads) device->stop();
        const size_t n = std::min(count, total_ - produced);
        for (size_t i = 0; i < n; ++i) out[i] = static_cast<int16_t>(produced + i);
        produced += n;
        return n;
    }

    size_t produced = 0;
    size_t reads = 0;
    unlimited::AudioOutput* device = nullptr;
    size_t stop_after_reads = 0;

private:
    size_t total_;
};

}  // namespace

TEST(pc_audio_open_parses_specs) {
    std::string error;
    CHECK(open_output("null", error) != nullptr);
    std::unique_ptr<InputDevice> null_input = open_input("null", error);
    REQUIRE(null_input != nullptr);
    CHECK_EQ(null_input->sample_rate_hz(), k_decoder_rate);

    const std::string prefixed = temp_path("prefixed.out");
    const std::string bare = temp_path("bare.wav");
    const std::string upper = temp_path("upper.WAV");
    CHECK(open_output("wav:" + prefixed, error) != nullptr);
    CHECK(open_output(bare, error) != nullptr);
    CHECK(open_output(upper, error) != nullptr);

    // Live devices (coreaudio:, alsa:, default) are tests/test_audio_devices.cpp's.
    const char* unknown[] = {"", "nul", "wav", "pulse:default", "coreaudio", "tone.txt", ".wav", "wav:"};
    for (size_t i = 0; i < count_of(unknown); ++i) {
        error.clear();
        if (!CHECK(open_output(unknown[i], error) == nullptr)) NOTE("output '%s' accepted", unknown[i]);
        CHECK(!error.empty());
        error.clear();
        if (!CHECK(open_input(unknown[i], error) == nullptr)) NOTE("input '%s' accepted", unknown[i]);
        CHECK(!error.empty());
    }

    error.clear();
    CHECK(open_output("wav:" + temp_path("no_such_dir/x.wav"), error) == nullptr);
    CHECK(!error.empty());
    error.clear();
    CHECK(open_input(temp_path("missing.wav"), error) == nullptr);
    CHECK(error.find("missing.wav") != std::string::npos);

    const std::string not_wav = temp_path("not_wav.wav");
    {
        std::ofstream file(not_wav.c_str(), std::ios::binary);
        file << "this is not a RIFF file at all, but it is long enough to hold a header";
    }
    error.clear();
    CHECK(open_input("wav:" + not_wav, error) == nullptr);
    CHECK(!error.empty());

    std::remove(prefixed.c_str());
    std::remove(bare.c_str());
    std::remove(upper.c_str());
    std::remove(not_wav.c_str());
}

TEST(pc_audio_wav_file_round_trip) {
    const uint32_t rate = 11025;
    const size_t count = 1234;
    std::vector<int16_t> samples = tone(440.0, 20000.0, count, rate);
    samples[0] = std::numeric_limits<int16_t>::min();
    samples[1] = std::numeric_limits<int16_t>::max();
    const std::string path = temp_path("round_trip.wav");

    std::string error;
    std::unique_ptr<OutputDevice> output = open_output("wav:" + path, error);
    REQUIRE(output != nullptr);
    MemorySource source(samples);
    CHECK(output->start(source, rate));
    CHECK(output->wait());

    // Sizes patched in place on the seekable file.
    const std::vector<uint8_t> bytes = file_bytes(path);
    const size_t data_bytes = count * k_bytes_per_sample;
    CHECK_EQ(bytes.size(), k_wav_header_bytes + data_bytes);
    REQUIRE(bytes.size() >= k_wav_header_bytes);
    CHECK_EQ(static_cast<size_t>(read_u32(bytes, k_riff_size_offset)), bytes.size() - k_riff_size_excluded);
    CHECK_EQ(static_cast<size_t>(read_u32(bytes, k_data_size_offset)), data_bytes);

    std::unique_ptr<InputDevice> input = open_input(path, error);
    REQUIRE(input != nullptr);
    CHECK_EQ(input->sample_rate_hz(), rate);
    CollectSink sink;
    CHECK(!input->start(sink, rate + 1));  // no implicit rate conversion
    CHECK(input->start(sink, rate));
    CHECK(input->wait());
    CHECK(sink.samples == samples);
    for (size_t i = 0; i + 1 < sink.chunks.size(); ++i) CHECK_EQ(sink.chunks[i], unlimited::pc::k_device_chunk_samples);
    std::remove(path.c_str());
}

TEST(pc_audio_null_devices) {
    std::string error;
    const size_t total = 5000;
    std::unique_ptr<OutputDevice> output = open_output("null", error);
    REQUIRE(output != nullptr);
    RampSource source(total);
    CHECK(!output->start(source, 0));
    CHECK(output->start(source, k_decoder_rate));
    CHECK(output->wait());
    CHECK_EQ(source.produced, total);

    std::unique_ptr<InputDevice> input = open_input("null", error);
    REQUIRE(input != nullptr);
    CollectSink sink;
    CHECK(input->start(sink, input->sample_rate_hz()));
    CHECK(input->wait());
    CHECK(sink.samples.empty());
}

TEST(pc_audio_memory_devices) {
    const uint32_t rate = 48000;
    const size_t total = 1000;
    const size_t chunk = 300;
    RampSource source(total);
    MemoryOutput output;
    CHECK(!output.start(source, 0));
    CHECK(output.start(source, rate));
    CHECK(output.wait());
    CHECK_EQ(output.sample_rate_hz(), rate);
    REQUIRE(output.samples().size() == total);
    for (size_t i = 0; i < total; ++i) CHECK_EQ(output.samples()[i], static_cast<int16_t>(i));

    MemoryInput input(output.samples(), rate, chunk);
    CHECK_EQ(input.sample_rate_hz(), rate);
    CollectSink sink;
    CHECK(!input.start(sink, k_decoder_rate));
    CHECK(input.start(sink, rate));
    CHECK(input.wait());
    CHECK(sink.samples == output.samples());
    const size_t expected_chunks[] = {chunk, chunk, chunk, total - 3 * chunk};
    CHECK(sink.chunks == std::vector<size_t>(expected_chunks, expected_chunks + count_of(expected_chunks)));
}

TEST(pc_audio_stop_from_a_callback) {
    const size_t total = 10000;
    const size_t stop_after = 3;

    // Output: stop() inside the source's read(); the samples up to that read are kept.
    MemoryOutput memory;
    RampSource source(total);
    source.device = &memory;
    source.stop_after_reads = stop_after;
    CHECK(memory.start(source, k_decoder_rate));
    CHECK(!memory.wait());
    CHECK_EQ(memory.samples().size(), stop_after * unlimited::pc::k_device_chunk_samples);

    // Input: stop() inside the sink's write().
    std::vector<int16_t> samples(total, 1);
    MemoryInput input(samples, k_decoder_rate);
    CollectSink sink;
    sink.device = &input;
    sink.stop_after_writes = stop_after;
    CHECK(input.start(sink, k_decoder_rate));
    CHECK_EQ(sink.chunks.size(), stop_after);

    // A stopped WAV recording is still a valid file holding what was delivered.
    const std::string path = temp_path("stopped.wav");
    std::string error;
    std::unique_ptr<OutputDevice> output = open_output(path, error);
    REQUIRE(output != nullptr);
    RampSource wav_source(total);
    wav_source.device = output.get();
    wav_source.stop_after_reads = stop_after;
    CHECK(output->start(wav_source, k_decoder_rate));
    CHECK(!output->wait());
    std::unique_ptr<InputDevice> reader = open_input(path, error);
    REQUIRE(reader != nullptr);
    CollectSink written;
    CHECK(reader->start(written, k_decoder_rate));
    CHECK(!written.samples.empty());
    CHECK(written.samples.size() < total);
    for (size_t i = 0; i < written.samples.size(); ++i) CHECK_EQ(written.samples[i], static_cast<int16_t>(i));
    std::remove(path.c_str());
}

TEST(pc_audio_resampling_sink) {
    // Equal rates: an exact copy, nothing held back.
    const std::vector<int16_t> narrow = tone(1000.0, 10000.0, 4000, k_decoder_rate);
    CollectSink copy;
    ResamplingSink same(copy, k_decoder_rate);
    same.write(&narrow[0], narrow.size());
    CHECK(copy.samples == narrow);
    same.flush();
    CHECK(copy.samples == narrow);

    // 48 kHz -> 8 kHz: the resampler's output, rounded, whatever the chunking.
    const double from_hz = 48000.0;
    const std::vector<int16_t> wide = tone(1000.0, 10000.0, 24000, from_hz);
    const std::vector<float> wide_float(wide.begin(), wide.end());
    const std::vector<float> reference = resample(wide_float, from_hz, k_decoder_rate);
    std::vector<int16_t> expected(reference.size());
    for (size_t i = 0; i < reference.size(); ++i) expected[i] = static_cast<int16_t>(std::lround(reference[i]));
    const size_t chunks[] = {1, 7, 256, 4096};
    for (size_t c = 0; c < count_of(chunks); ++c) {
        CollectSink sink;
        ResamplingSink resampling(sink, from_hz);
        MemoryInput input(wide, static_cast<uint32_t>(from_hz), chunks[c]);
        CHECK(input.start(resampling, input.sample_rate_hz()));
        resampling.flush();
        if (!CHECK(sink.samples == expected)) NOTE("chunks of %zu differ", chunks[c]);
    }

    // Overshoot past full scale clamps instead of wrapping.
    const double square_hz = 1000.0;
    std::vector<int16_t> square(24000);
    for (size_t n = 0; n < square.size(); ++n)
        square[n] = std::sin(k_two_pi * square_hz * (n + 0.5) / from_hz) > 0.0 ? std::numeric_limits<int16_t>::max()
                                                                                : std::numeric_limits<int16_t>::min();
    const std::vector<float> square_float(square.begin(), square.end());
    const std::vector<float> overshoot = resample(square_float, from_hz, k_decoder_rate);
    const float peak = *std::max_element(overshoot.begin(), overshoot.end());
    NOTE("square wave overshoot %.0f", peak);
    CHECK(peak > std::numeric_limits<int16_t>::max());
    CollectSink clamped;
    ResamplingSink resampling(clamped, from_hz);
    resampling.write(&square[0], square.size());
    resampling.flush();
    REQUIRE(clamped.samples.size() == overshoot.size());
    size_t mismatches = 0;
    for (size_t i = 0; i < overshoot.size(); ++i) {
        const double limited = std::max(static_cast<double>(std::numeric_limits<int16_t>::min()),
                                        std::min(static_cast<double>(std::numeric_limits<int16_t>::max()),
                                                 static_cast<double>(overshoot[i])));
        if (clamped.samples[i] != static_cast<int16_t>(std::lround(limited))) ++mismatches;
    }
    CHECK_EQ(mismatches, static_cast<size_t>(0));
}

TEST(pc_audio_encoder_through_wav_files) {
    // Encoder -> EncoderSource -> "<path>.wav" -> open_input -> ResamplingSink: nothing lost or added.
    const uint32_t rate = 44100;
    const uint8_t text[] = {'P', 'C', ' ', 'A', 'U', 'D', 'I', 'O'};
    unlimited::EncoderConfig config;
    config.sample_rate_hz = rate;
    config.slot_us = unlimited::slot_us_for_speed(12.0f);
    unlimited::Encoder encoder(config);
    REQUIRE(encoder.write(text, sizeof(text)) == sizeof(text));
    const uint32_t duration = encoder.duration_samples(sizeof(text));
    REQUIRE(encoder.start());
    unlimited::EncoderSource source(encoder);
    const std::string path = temp_path("encoder.wav");
    std::string error;
    std::unique_ptr<OutputDevice> output = open_output(path, error);
    REQUIRE(output != nullptr);
    CHECK(output->start(source, rate));
    CHECK(output->wait());

    std::unique_ptr<InputDevice> input = open_input(path, error);
    REQUIRE(input != nullptr);
    CHECK_EQ(input->sample_rate_hz(), rate);
    CollectSink native;
    CHECK(input->start(native, input->sample_rate_hz()));
    CHECK_EQ(native.samples.size(), static_cast<size_t>(duration));

    input = open_input(path, error);
    REQUIRE(input != nullptr);
    CollectSink decoder_rate;
    ResamplingSink resampling(decoder_rate, input->sample_rate_hz());
    CHECK(input->start(resampling, input->sample_rate_hz()));
    resampling.flush();
    const size_t expected = static_cast<size_t>(std::ceil(static_cast<double>(duration) * k_decoder_rate / rate));
    CHECK_EQ(decoder_rate.samples.size(), expected);
    std::remove(path.c_str());
}

TEST(pc_audio_file_byte_adapters) {
    const std::string path = temp_path("bytes.bin");
    const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t patch[] = {0xAA, 0xBB};
    const uint32_t patch_at = 3;

    FileByteSink sink;
    CHECK(!sink.write(data, sizeof(data)));  // not open
    CHECK(!sink.open(temp_path("no_such_dir/bytes.bin")));
    REQUIRE(sink.open(path));
    CHECK(sink.write(data, sizeof(data)));
    CHECK(sink.seek(patch_at));
    CHECK(sink.write(patch, sizeof(patch)));
    CHECK(sink.close());

    std::vector<uint8_t> expected(data, data + sizeof(data));
    std::copy(patch, patch + sizeof(patch), expected.begin() + patch_at);
    CHECK(file_bytes(path) == expected);

    FileByteSource source;
    CHECK_EQ(source.read(&expected[0], expected.size()), static_cast<size_t>(0));  // not open
    REQUIRE(source.open(path));
    std::vector<uint8_t> back(expected.size() + 1);
    CHECK_EQ(source.read(&back[0], back.size()), expected.size());
    back.resize(expected.size());
    CHECK(back == expected);
    CHECK_EQ(source.read(&back[0], back.size()), static_cast<size_t>(0));
    CHECK(source.seek(patch_at));
    uint8_t patched[sizeof(patch)];
    CHECK_EQ(source.read(patched, sizeof(patched)), sizeof(patched));
    CHECK(std::equal(patched, patched + sizeof(patched), patch));
    CHECK(source.close());
    CHECK(!source.seek(0));  // not open
    CHECK(!source.open(temp_path("missing.bin")));
    std::remove(path.c_str());
}
