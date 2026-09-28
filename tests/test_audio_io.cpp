#include "test_harness.hpp"
#include "unlimited/audio_io.hpp"
#include "unlimited/wav_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using unlimited::AudioOutput;
using unlimited::ByteSink;
using unlimited::ByteSource;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSource;
using unlimited::SampleSink;
using unlimited::SampleSource;
using unlimited::WavOutput;
using unlimited::WavReader;
using unlimited::WavWriter;
using unlimited::k_wav_header_bytes;
using unlimited::k_wav_unknown_size;

namespace {

// A sender at `speed` bytes/s and `rate` Hz (1500 Hz in the default 300..2700 Hz passband).
unlimited::EncoderConfig at_speed(float speed, uint32_t rate) {
    unlimited::EncoderConfig config;
    config.sample_rate_hz = rate;
    config.slot_us = unlimited::slot_us_for_speed(speed);
    return config;
}

using std::int16_t;
using std::size_t;
using std::uint32_t;
using std::uint8_t;
using test::count_of;

typedef std::vector<uint8_t> Bytes;

const unsigned bits_per_byte = 8;

class MemorySink : public ByteSink {
public:
    explicit MemorySink(bool seekable) : seekable_(seekable), position_(0) {}

    bool write(const uint8_t* data, size_t size) override {
        if (position_ + size > bytes.size()) bytes.resize(position_ + size);
        std::memcpy(bytes.data() + position_, data, size);
        position_ += size;
        return true;
    }

    bool seek(uint32_t position) override {
        if (!seekable_ || position > bytes.size()) return false;
        position_ = position;
        return true;
    }

    Bytes bytes;

private:
    bool seekable_;
    size_t position_;
};

class FailingSink : public ByteSink {
public:
    bool write(const uint8_t*, size_t) override { return false; }
};

class MemorySource : public ByteSource {
public:
    explicit MemorySource(const Bytes& bytes) : bytes_(bytes), position_(0) {}

    size_t read(uint8_t* data, size_t size) override {
        const size_t count = std::min(size, bytes_.size() - position_);
        std::memcpy(data, bytes_.data() + position_, count);
        position_ += count;
        return count;
    }

private:
    Bytes bytes_;
    size_t position_;
};

class VectorSink : public SampleSink {
public:
    void write(const int16_t* in, size_t count) override { samples.insert(samples.end(), in, in + count); }
    std::vector<int16_t> samples;
};

std::vector<uint8_t> message() {
    const char* text = "CQ DE UNLIMITED";
    return std::vector<uint8_t>(text, text + std::strlen(text));
}

std::vector<int16_t> render_direct(const EncoderConfig& config, const std::vector<uint8_t>& data) {
    Encoder encoder(config);
    encoder.write(data.data(), data.size());
    std::vector<int16_t> out;
    if (!encoder.start()) return out;
    while (encoder.busy()) out.push_back(encoder.next_sample());
    return out;
}

std::vector<int16_t> drain(SampleSource& source, size_t chunk) {
    std::vector<int16_t> out;
    std::vector<int16_t> buffer(chunk);
    for (;;) {
        const size_t got = source.read(buffer.data(), buffer.size());
        if (got == 0) break;
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return out;
}

uint32_t u32_at(const Bytes& bytes, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes[offset + i]) << (bits_per_byte * i);
    return value;
}

}  // namespace

TEST(audio_io_encoder_source_returns_duration_then_zero) {
    const float speeds[] = {25.0f, 6.0f};
    const uint32_t rates[] = {8000, 44100};
    const size_t chunks[] = {1, 100, 4096};
    const std::vector<uint8_t> data = message();
    for (size_t p = 0; p < count_of(speeds); ++p) {
        for (size_t r = 0; r < count_of(rates); ++r) {
            const EncoderConfig config = at_speed(speeds[p], rates[r]);
            const std::vector<int16_t> direct = render_direct(config, data);
            for (size_t c = 0; c < count_of(chunks); ++c) {
                Encoder encoder(config);
                encoder.write(data.data(), data.size());
                REQUIRE(encoder.start());
                EncoderSource source(encoder);
                const std::vector<int16_t> out = drain(source, chunks[c]);
                CHECK_EQ(out.size(), static_cast<size_t>(encoder.duration_samples(data.size())));
                CHECK(out == direct);
                int16_t more[4];
                CHECK_EQ(source.read(more, count_of(more)), 0u);
                CHECK_EQ(source.read(more, count_of(more)), 0u);
            }
        }
    }
}

TEST(audio_io_encoder_source_idle_encoder_reads_zero) {
    Encoder encoder{EncoderConfig()};
    EncoderSource source(encoder);
    int16_t buffer[8];
    CHECK_EQ(source.read(buffer, count_of(buffer)), 0u);
}

TEST(audio_io_wav_output_writes_readable_wav) {
    const EncoderConfig config = at_speed(12.0f, 11025);
    const std::vector<uint8_t> data = message();
    const std::vector<int16_t> direct = render_direct(config, data);

    struct Case {
        const char* name;
        bool seekable;
        bool known_length;
    };
    const Case cases[] = {{"known, not seekable", false, true},
                          {"unknown, seekable", true, false},
                          {"unknown, not seekable", false, false}};
    for (size_t i = 0; i < count_of(cases); ++i) {
        Encoder encoder(config);
        encoder.write(data.data(), data.size());
        REQUIRE(encoder.start());
        EncoderSource source(encoder);
        MemorySink sink(cases[i].seekable);
        WavOutput output(sink, cases[i].known_length ? encoder.duration_samples(data.size()) : k_wav_unknown_size);
        AudioOutput& device = output;
        if (!CHECK(device.start(source, config.sample_rate_hz))) NOTE("case %s", cases[i].name);
        CHECK(device.wait());
        device.stop();

        const bool exact = cases[i].known_length || cases[i].seekable;
        const uint32_t data_bytes = static_cast<uint32_t>(2 * direct.size());
        CHECK_EQ(sink.bytes.size(), k_wav_header_bytes + static_cast<size_t>(data_bytes));
        CHECK_EQ(u32_at(sink.bytes, 40), exact ? data_bytes : k_wav_unknown_size);

        MemorySource file(sink.bytes);
        WavReader reader;
        REQUIRE(reader.open(file));
        CHECK_EQ(reader.format().sample_rate_hz, config.sample_rate_hz);
        CHECK_EQ(reader.format().channels, 1);
        CHECK_EQ(reader.format().bits_per_sample, 16);
        SampleSource& samples = reader;
        CHECK(drain(samples, 333) == direct);
    }
}

TEST(audio_io_wav_output_reports_failures) {
    const EncoderConfig config;
    Encoder encoder(config);
    encoder.write(0x55);
    REQUIRE(encoder.start());
    EncoderSource source(encoder);
    FailingSink failing;
    WavOutput output(failing);
    CHECK(!output.start(source, config.sample_rate_hz));
    CHECK(!output.wait());

    // A declared length that does not match what the source produced, on a sink that cannot seek.
    Encoder short_encoder(config);
    short_encoder.write(0x55);
    REQUIRE(short_encoder.start());
    EncoderSource short_source(short_encoder);
    MemorySink sink(false);
    WavOutput wrong(sink, short_encoder.duration_samples(1) + 1);
    CHECK(!wrong.start(short_source, config.sample_rate_hz));
    CHECK(!wrong.wait());

    MemorySink rate_sink(true);
    WavOutput bad_rate(rate_sink);
    Encoder idle{EncoderConfig()};
    EncoderSource idle_source(idle);
    CHECK(!bad_rate.start(idle_source, 0));
}

TEST(audio_io_wav_writer_is_a_sample_sink) {
    MemorySink sink(true);
    WavWriter writer;
    REQUIRE(writer.begin(sink, 8000));
    SampleSink& as_sink = writer;
    const int16_t samples[] = {1, -2, 3, -4, 5};
    as_sink.write(samples, 2);
    as_sink.write(samples + 2, 3);
    CHECK(writer.finish());
    CHECK_EQ(writer.samples_written(), 5u);

    MemorySource file(sink.bytes);
    WavReader reader;
    REQUIRE(reader.open(file));
    VectorSink collected;
    int16_t buffer[2];
    for (;;) {
        const size_t got = reader.read(buffer, count_of(buffer));
        if (got == 0) break;
        collected.write(buffer, got);
    }
    CHECK(collected.samples == std::vector<int16_t>(samples, samples + count_of(samples)));
}
