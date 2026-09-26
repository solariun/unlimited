#include "test_harness.hpp"
#include "unlimited/wav_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

using unlimited::ByteSink;
using unlimited::ByteSource;
using unlimited::WavEncoding;
using unlimited::WavReader;
using unlimited::WavWriter;
using unlimited::k_wav_header_bytes;
using unlimited::k_wav_unknown_size;
using unlimited::wav_build_header;

namespace {

using std::int16_t;
using std::int64_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using test::count_of;

typedef std::vector<uint8_t> Bytes;

const uint32_t rate = 8000;
const unsigned bits_per_byte = 8;
const uint16_t tag_pcm = 1;
const uint16_t tag_float = 3;
const uint16_t tag_extensible = 0xFFFE;

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

// Hands out at most `slice` bytes per read, like a device or a slow stream.
class MemorySource : public ByteSource {
public:
    MemorySource(const Bytes& bytes, bool seekable, size_t slice = 1000000)
        : bytes_(bytes), seekable_(seekable), slice_(slice), position_(0) {}

    size_t read(uint8_t* data, size_t size) override {
        size_t count = std::min(std::min(size, slice_), bytes_.size() - position_);
        std::memcpy(data, bytes_.data() + position_, count);
        position_ += count;
        return count;
    }

    bool seek(uint32_t position) override {
        if (!seekable_ || position > bytes_.size()) return false;
        position_ = position;
        return true;
    }

private:
    Bytes bytes_;
    bool seekable_;
    size_t slice_;
    size_t position_;
};

class Builder {
public:
    Builder& u8(uint8_t value) {
        bytes.push_back(value);
        return *this;
    }
    Builder& le(int64_t value, size_t count) {
        for (size_t i = 0; i < count; ++i) bytes.push_back(static_cast<uint8_t>(static_cast<uint64_t>(value) >> (bits_per_byte * i)));
        return *this;
    }
    Builder& u16(uint16_t value) { return le(value, 2); }
    Builder& u32(uint32_t value) { return le(value, 4); }
    Builder& f32(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return u32(bits);
    }
    Builder& id(const char* fourcc) {  // up to 4 characters: a shorter one builds a truncated file
        bytes.insert(bytes.end(), fourcc, fourcc + std::strlen(fourcc));
        return *this;
    }
    Builder& append(const Bytes& other) {
        bytes.insert(bytes.end(), other.begin(), other.end());
        return *this;
    }
    Bytes bytes;
};

Bytes chunk(const char* id, const Bytes& body) {
    Builder out;
    out.id(id).u32(static_cast<uint32_t>(body.size())).append(body);
    if (body.size() % 2 != 0) out.u8(0);
    return out.bytes;
}

Bytes fmt(uint16_t tag, uint16_t channels, uint16_t bits, uint16_t block_align = 0) {
    if (block_align == 0) block_align = static_cast<uint16_t>(channels * ((bits + 7) / 8));
    Builder body;
    body.u16(tag).u16(channels).u32(rate).u32(rate * block_align).u16(block_align).u16(bits);
    return body.bytes;
}

Bytes extensible(uint16_t subformat, uint16_t channels, uint16_t container_bits, uint16_t valid_bits) {
    const uint8_t guid_tail[] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    Builder body;
    body.append(fmt(tag_extensible, channels, container_bits)).u16(22).u16(valid_bits).u32(0).u16(subformat);
    for (size_t i = 0; i < count_of(guid_tail); ++i) body.u8(guid_tail[i]);
    return body.bytes;
}

Bytes riff(const Bytes& chunks) {
    Builder out;
    out.id("RIFF").u32(static_cast<uint32_t>(chunks.size() + 4)).id("WAVE").append(chunks);
    return out.bytes;
}

Bytes wav(const Bytes& fmt_body, const Bytes& data) {
    Builder chunks;
    chunks.append(chunk("fmt ", fmt_body)).append(chunk("data", data));
    return riff(chunks.bytes);
}

std::vector<float> read_all_float(const Bytes& file, bool seekable = true, size_t chunk_size = 1000) {
    MemorySource source(file, seekable);
    WavReader reader;
    std::vector<float> out;
    if (!reader.open(source)) return out;
    std::vector<float> buffer(chunk_size);
    for (;;) {
        const size_t got = reader.read(buffer.data(), buffer.size());
        if (got == 0) break;
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return out;
}

std::vector<int16_t> read_all_int16(const Bytes& file, size_t chunk_size = 1000, size_t slice = 1000000) {
    MemorySource source(file, true, slice);
    WavReader reader;
    std::vector<int16_t> out;
    if (!reader.open(source)) return out;
    std::vector<int16_t> buffer(chunk_size);
    for (;;) {
        const size_t got = reader.read(buffer.data(), buffer.size());
        if (got == 0) break;
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return out;
}

bool opens(const Bytes& file, bool seekable = true) {
    MemorySource source(file, seekable);
    WavReader reader;
    return reader.open(source);
}

uint32_t u32_at(const Bytes& bytes, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes[offset + i]) << (bits_per_byte * i);
    return value;
}

}  // namespace

TEST(wav_codec_header_bytes_match_riff_layout) {
    uint8_t header[k_wav_header_bytes];
    REQUIRE(wav_build_header(rate, 1000, header));
    Builder expected;
    expected.id("RIFF").u32(36 + 2000).id("WAVE");
    expected.id("fmt ").u32(16).u16(tag_pcm).u16(1).u32(rate).u32(rate * 2).u16(2).u16(16);
    expected.id("data").u32(2000);
    REQUIRE(expected.bytes.size() == k_wav_header_bytes);
    CHECK(Bytes(header, header + k_wav_header_bytes) == expected.bytes);

    REQUIRE(wav_build_header(rate, k_wav_unknown_size, header));
    CHECK_EQ(u32_at(Bytes(header, header + k_wav_header_bytes), 4), k_wav_unknown_size);
    CHECK_EQ(u32_at(Bytes(header, header + k_wav_header_bytes), 40), k_wav_unknown_size);

    CHECK(!wav_build_header(0, 10, header));
    CHECK(!wav_build_header(std::numeric_limits<uint32_t>::max(), 10, header));
    CHECK(!wav_build_header(rate, (0xFFFFFFFFu - 36) / 2 + 1, header));
    CHECK(wav_build_header(rate, (0xFFFFFFFFu - 36) / 2, header));
}

TEST(wav_codec_writer_exact_sizes_up_front) {
    const int16_t samples[] = {0, 1, -1, 32767, -32768, 1234};
    MemorySink sink(false);
    WavWriter writer;
    REQUIRE(writer.begin(sink, rate, count_of(samples)));
    writer.write(samples, 2);
    writer.write(samples + 2, count_of(samples) - 2);
    CHECK(writer.finish());
    CHECK(writer.ok());
    CHECK_EQ(writer.samples_written(), count_of(samples));
    REQUIRE(sink.bytes.size() == k_wav_header_bytes + 2 * count_of(samples));
    CHECK_EQ(u32_at(sink.bytes, 4), 36u + 2 * count_of(samples));
    CHECK_EQ(u32_at(sink.bytes, 40), 2u * count_of(samples));
    for (size_t i = 0; i < count_of(samples); ++i) {
        const uint16_t raw = static_cast<uint16_t>(sink.bytes[44 + 2 * i] | (sink.bytes[45 + 2 * i] << bits_per_byte));
        CHECK_EQ(static_cast<int16_t>(raw), samples[i]);
    }
    CHECK(read_all_int16(sink.bytes) == std::vector<int16_t>(samples, samples + count_of(samples)));
}

TEST(wav_codec_writer_patches_sizes_on_seekable_sink) {
    std::vector<int16_t> samples(1001);
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<int16_t>(i * 37);
    MemorySink sink(true);
    WavWriter writer;
    REQUIRE(writer.begin(sink, rate));
    writer.write(samples.data(), samples.size());
    CHECK(writer.finish());
    REQUIRE(sink.bytes.size() == k_wav_header_bytes + 2 * samples.size());
    CHECK_EQ(u32_at(sink.bytes, 4), 36u + 2 * samples.size());
    CHECK_EQ(u32_at(sink.bytes, 40), 2u * samples.size());
    CHECK(read_all_int16(sink.bytes) == samples);

    // A wrong declared length is corrected too.
    MemorySink wrong(true);
    WavWriter second;
    REQUIRE(second.begin(wrong, rate, 5));
    second.write(samples.data(), 3);
    CHECK(second.finish());
    CHECK_EQ(u32_at(wrong.bytes, 40), 6u);
    CHECK_EQ(wrong.bytes.size(), k_wav_header_bytes + 6u);
}

TEST(wav_codec_writer_streaming_sizes_without_seek) {
    const int16_t samples[] = {100, -100, 200};
    MemorySink sink(false);
    WavWriter writer;
    REQUIRE(writer.begin(sink, rate));
    writer.write(samples, count_of(samples));
    CHECK(writer.finish());
    CHECK_EQ(u32_at(sink.bytes, 4), k_wav_unknown_size);
    CHECK_EQ(u32_at(sink.bytes, 40), k_wav_unknown_size);
    // The reader streams an unknown-size data chunk to the end of the source.
    CHECK(read_all_int16(sink.bytes) == std::vector<int16_t>(samples, samples + count_of(samples)));

    // A declared length that turns out wrong cannot be fixed without seeking.
    MemorySink fixed(false);
    WavWriter third;
    REQUIRE(third.begin(fixed, rate, 10));
    third.write(samples, count_of(samples));
    CHECK(!third.finish());
    CHECK(!third.ok());

    WavWriter unused;
    CHECK(!unused.ok());
    CHECK(!unused.finish());
    MemorySink bad(false);
    CHECK(!unused.begin(bad, 0));
}

TEST(wav_codec_reader_formats) {
    struct Case {
        const char* name;
        Bytes file;
        std::vector<float> expected;
        WavEncoding encoding;
        uint16_t bits;
    };
    std::vector<Case> cases;
    {
        Builder data;
        data.u8(0).u8(128).u8(255).u8(64);
        cases.push_back(Case{"pcm8", wav(fmt(tag_pcm, 1, 8), data.bytes), {-1.0f, 0.0f, 127.0f / 128, -0.5f},
                             WavEncoding::pcm, 8});
    }
    {
        Builder data;
        data.le(16384, 2).le(-32768, 2).le(32767, 2);
        cases.push_back(Case{"pcm16", wav(fmt(tag_pcm, 1, 16), data.bytes), {0.5f, -1.0f, 32767.0f / 32768},
                             WavEncoding::pcm, 16});
    }
    {
        Builder data;
        data.le(0x7FFFFF, 3).le(-0x800000, 3).le(1, 3).le(0x400000, 3);
        cases.push_back(Case{"pcm24", wav(fmt(tag_pcm, 1, 24), data.bytes),
                             {8388607.0f / 8388608, -1.0f, 1.0f / 8388608, 0.5f}, WavEncoding::pcm, 24});
    }
    {
        Builder data;
        data.le(INT32_MIN, 4).le(0x40000000, 4).le(INT32_MAX, 4);
        cases.push_back(Case{"pcm32", wav(fmt(tag_pcm, 1, 32), data.bytes), {-1.0f, 0.5f, 1.0f}, WavEncoding::pcm, 32});
    }
    {
        Builder data;
        data.f32(0.25f).f32(-2.0f).f32(std::numeric_limits<float>::quiet_NaN()).f32(std::numeric_limits<float>::infinity());
        cases.push_back(Case{"float", wav(fmt(tag_float, 1, 32), data.bytes), {0.25f, -1.0f, 0.0f, 1.0f},
                             WavEncoding::ieee_float, 32});
    }
    {
        Builder data;
        data.le(int64_t(0x400000) << 8, 4).le(0, 4);  // 24 valid bits in 32, stereo
        cases.push_back(Case{"extensible_pcm", wav(extensible(tag_pcm, 2, 32, 24), data.bytes), {0.25f},
                             WavEncoding::pcm, 32});
    }
    {
        Builder data;
        data.f32(-0.5f).f32(0.25f).f32(0.75f);  // three channels
        cases.push_back(Case{"extensible_float", wav(extensible(tag_float, 3, 32, 32), data.bytes), {1.0f / 6},
                             WavEncoding::ieee_float, 32});
    }
    {
        Builder data;
        data.le(16384, 2).le(-8192, 2).le(-32768, 2).le(-32768, 2);
        cases.push_back(Case{"stereo16", wav(fmt(tag_pcm, 2, 16), data.bytes), {0.125f, -1.0f}, WavEncoding::pcm, 16});
    }
    {
        Builder data;
        data.le(16384, 2).le(-1, 2).le(-16384, 2).le(-1, 2);  // 16-bit in 4-byte frames
        cases.push_back(Case{"padded", wav(fmt(tag_pcm, 1, 16, 4), data.bytes), {0.5f, -0.5f}, WavEncoding::pcm, 16});
    }
    for (size_t i = 0; i < cases.size(); ++i) {
        MemorySource source(cases[i].file, false, 3);  // 3-byte reads: frames straddle source reads
        WavReader reader;
        if (!CHECK(reader.open(source))) {
            NOTE("case %s rejected", cases[i].name);
            continue;
        }
        CHECK_EQ(reader.format().sample_rate_hz, rate);
        CHECK(reader.format().encoding == cases[i].encoding);
        CHECK_EQ(reader.format().bits_per_sample, cases[i].bits);
        std::vector<float> got(cases[i].expected.size() + 4);
        const size_t count = reader.read(got.data(), got.size());
        if (!CHECK_EQ(count, cases[i].expected.size())) NOTE("case %s", cases[i].name);
        for (size_t k = 0; k < std::min(count, cases[i].expected.size()); ++k)
            if (!CHECK_NEAR(got[k], cases[i].expected[k], 1e-7)) NOTE("case %s sample %zu", cases[i].name, k);
        CHECK_EQ(reader.read(got.data(), got.size()), 0u);
    }
}

TEST(wav_codec_reader_int16_rounds_and_clamps) {
    Builder pcm24;
    pcm24.le(0x7FFFFF, 3).le(0x7FFF7F, 3).le(0x000080, 3).le(0x00007F, 3).le(-0x800000, 3).le(-0x81, 3);
    const int16_t expected24[] = {32767, 32767, 1, 0, -32768, -1};
    CHECK(read_all_int16(wav(fmt(tag_pcm, 1, 24), pcm24.bytes)) ==
          std::vector<int16_t>(expected24, expected24 + count_of(expected24)));

    Builder pcm32;
    pcm32.le(INT32_MAX, 4).le(INT32_MIN, 4).le(0x8000, 4).le(0x7FFF, 4).le(-0x8001, 4);
    const int16_t expected32[] = {32767, -32768, 1, 0, -1};
    CHECK(read_all_int16(wav(fmt(tag_pcm, 1, 32), pcm32.bytes)) ==
          std::vector<int16_t>(expected32, expected32 + count_of(expected32)));

    Builder floats;
    floats.f32(1.0f).f32(-1.0f).f32(0.5f).f32(-0.25f).f32(std::numeric_limits<float>::quiet_NaN()).f32(3.0f);
    const int16_t expected_float[] = {32767, -32768, 16384, -8192, 0, 32767};
    CHECK(read_all_int16(wav(fmt(tag_float, 1, 32), floats.bytes)) ==
          std::vector<int16_t>(expected_float, expected_float + count_of(expected_float)));

    Builder pcm8;
    pcm8.u8(0).u8(128).u8(255);
    const int16_t expected8[] = {-32768, 0, 32512};
    CHECK(read_all_int16(wav(fmt(tag_pcm, 1, 8), pcm8.bytes)) ==
          std::vector<int16_t>(expected8, expected8 + count_of(expected8)));

    Builder stereo;
    stereo.le(3, 2).le(0, 2).le(-3, 2).le(0, 2).le(32767, 2).le(32767, 2);
    const int16_t expected_stereo[] = {2, -2, 32767};  // mean rounded half away from zero
    CHECK(read_all_int16(wav(fmt(tag_pcm, 2, 16), stereo.bytes)) ==
          std::vector<int16_t>(expected_stereo, expected_stereo + count_of(expected_stereo)));
}

TEST(wav_codec_reader_chunks_and_order) {
    Builder odd;
    odd.u8('a').u8('b').u8('c');
    Builder data;
    data.le(16384, 2).le(-16384, 2).le(0, 2);
    Builder chunks;
    chunks.append(chunk("LIST", odd.bytes))
        .append(chunk("fmt ", fmt(tag_pcm, 1, 16)))
        .append(chunk("junk", odd.bytes))
        .append(chunk("data", data.bytes))
        .append(chunk("cue ", odd.bytes));
    const std::vector<float> expected = {0.5f, -0.5f, 0.0f};
    CHECK(read_all_float(riff(chunks.bytes), false) == expected);

    // data before fmt needs a seekable source to come back to the samples.
    Builder late;
    late.append(chunk("data", data.bytes)).append(chunk("LIST", odd.bytes)).append(chunk("fmt ", fmt(tag_pcm, 1, 16)));
    CHECK(read_all_float(riff(late.bytes), true) == expected);
    CHECK(!opens(riff(late.bytes), false));

    // A data chunk that claims more than exists is read up to what exists; a partial frame is dropped.
    Builder truncated;
    truncated.append(chunk("fmt ", fmt(tag_pcm, 1, 16))).id("data").u32(1000).le(8192, 2).le(8192, 2).u8(0);
    CHECK(read_all_float(riff(truncated.bytes), false) == std::vector<float>(2, 0.25f));

    // Chunked reads give the same samples as one big read.
    std::vector<int16_t> ramp;
    Builder long_data;
    for (int i = 0; i < 500; ++i) {
        ramp.push_back(static_cast<int16_t>(i * 64 - 16000));
        long_data.le(ramp.back(), 2).le(0, 2);
    }
    const Bytes stereo = wav(fmt(tag_pcm, 2, 16), long_data.bytes);
    const std::vector<int16_t> whole = read_all_int16(stereo, 1000);
    CHECK(read_all_int16(stereo, 1, 1) == whole);
    CHECK(read_all_int16(stereo, 7, 5) == whole);
    REQUIRE(whole.size() == ramp.size());
    for (size_t i = 0; i < ramp.size(); ++i) CHECK_EQ(whole[i], ramp[i] / 2);
}

TEST(wav_codec_reader_rejects_malformed) {
    Builder data;
    data.le(0, 2).le(0, 2);
    const Bytes good_fmt = fmt(tag_pcm, 1, 16);
    std::vector<std::pair<const char*, Bytes> > cases;
    cases.push_back(std::make_pair("empty", Bytes()));
    {
        Builder b;
        b.id("RIFX").u32(4).id("WAVE");
        cases.push_back(std::make_pair("not_riff", b.bytes));
    }
    {
        Builder b;
        b.id("RIFF").u32(4).id("AVI ").append(chunk("fmt ", good_fmt)).append(chunk("data", data.bytes));
        cases.push_back(std::make_pair("not_wave", b.bytes));
    }
    {
        Builder b;
        b.id("RIFF").u32(4).id("WA");
        cases.push_back(std::make_pair("short_riff", b.bytes));
    }
    cases.push_back(std::make_pair("missing_fmt", riff(chunk("data", data.bytes))));
    cases.push_back(std::make_pair("missing_data", riff(chunk("fmt ", good_fmt))));
    {
        Builder b;
        b.id("fmt ").u32(16).u16(tag_pcm).u16(1);
        cases.push_back(std::make_pair("truncated_fmt", riff(b.bytes)));
    }
    {
        Builder b;
        b.id("LIST").u32(0xFFFFFFFFu).id("INFO").append(chunk("fmt ", good_fmt)).append(chunk("data", data.bytes));
        cases.push_back(std::make_pair("unbounded_chunk_before_fmt", riff(b.bytes)));
    }
    {
        Builder short_fmt;
        short_fmt.u16(tag_pcm).u16(1).u32(rate);
        cases.push_back(std::make_pair("short_fmt", wav(short_fmt.bytes, data.bytes)));
    }
    cases.push_back(std::make_pair("adpcm", wav(fmt(2, 1, 4), data.bytes)));
    cases.push_back(std::make_pair("float16", wav(fmt(tag_float, 1, 16), data.bytes)));
    cases.push_back(std::make_pair("pcm40", wav(fmt(tag_pcm, 1, 40), data.bytes)));
    cases.push_back(std::make_pair("pcm0", wav(fmt(tag_pcm, 1, 0), data.bytes)));
    // bits_per_sample whose container size wraps modulo 256 to 1, 2 or 4 bytes (was read as 8-, 16- or 32-bit).
    cases.push_back(std::make_pair("pcm2049", wav(fmt(tag_pcm, 1, 2049, 1), data.bytes)));
    cases.push_back(std::make_pair("pcm2056", wav(fmt(tag_pcm, 1, 2056, 4), data.bytes)));
    cases.push_back(std::make_pair("pcm2057", wav(fmt(tag_pcm, 1, 2057, 4), data.bytes)));
    cases.push_back(std::make_pair("pcm4097", wav(fmt(tag_pcm, 1, 4097, 1), data.bytes)));
    cases.push_back(std::make_pair("pcm2080", wav(fmt(tag_pcm, 1, 2080, 4), data.bytes)));
    cases.push_back(std::make_pair("float2080", wav(fmt(tag_float, 1, 2080, 4), data.bytes)));
    cases.push_back(std::make_pair("pcm33", wav(fmt(tag_pcm, 1, 33, 8), data.bytes)));
    cases.push_back(std::make_pair("zero_channels", wav(fmt(tag_pcm, 0, 16), data.bytes)));
    {
        Builder zero_rate;
        zero_rate.u16(tag_pcm).u16(1).u32(0).u32(0).u16(2).u16(16);
        cases.push_back(std::make_pair("zero_rate", wav(zero_rate.bytes, data.bytes)));
    }
    cases.push_back(std::make_pair("block_align", wav(fmt(tag_pcm, 2, 16, 2), data.bytes)));
    cases.push_back(std::make_pair("frame_too_wide", wav(fmt(tag_pcm, 33, 32), data.bytes)));
    {
        Bytes ext = fmt(tag_extensible, 1, 16);
        ext.push_back(0);
        ext.push_back(0);
        cases.push_back(std::make_pair("short_extensible", wav(ext, data.bytes)));
    }
    for (size_t i = 0; i < cases.size(); ++i)
        if (!CHECK(!opens(cases[i].second))) NOTE("case %s accepted", cases[i].first);

    // A frame of exactly the supported width is accepted.
    CHECK(opens(wav(fmt(tag_pcm, 32, 32), Bytes(128, 0))));
    CHECK_EQ(WavReader::k_max_frame_bytes, 128);

    WavReader unopened;
    int16_t sample = 0;
    float value = 0.0f;
    CHECK_EQ(unopened.read(&sample, 1), 0u);
    CHECK_EQ(unopened.read(&value, 1), 0u);
}
