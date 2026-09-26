#include "test_harness.hpp"
#include "wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using unlimited::wav::read_wav;
using unlimited::wav::write_wav;

namespace {

using std::int64_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using test::count_of;

const double pi = 3.14159265358979323846;
const uint32_t test_rate = 8000;
const uint16_t tag_pcm = 0x0001;
const uint16_t tag_adpcm = 0x0002;
const uint16_t tag_float = 0x0003;
const uint16_t tag_extensible = 0xFFFE;
const unsigned bits_per_byte = 8;
const uint16_t pcm8_bits = 8;
const uint16_t pcm16_bits = 16;
const uint16_t pcm24_bits = 24;
const uint16_t pcm32_bits = 32;
const uint16_t float32_bits = 32;
const uint16_t adpcm_bits = 4;
const uint16_t float16_bits = 16;
const uint16_t pcm40_bits = 40;
const uint16_t mono = 1;
const uint16_t stereo = 2;
const uint16_t three_channels = 3;
const uint16_t pcm16_bytes = pcm16_bits / bits_per_byte;
const uint16_t pcm24_bytes = pcm24_bits / bits_per_byte;
const uint16_t pcm32_bytes = pcm32_bits / bits_per_byte;
const double pcm16_step = 1.0 / 32768.0;
const double pcm24_full_scale = 8388608.0;
const double exact = 1e-9;
const double float_tolerance = 1e-7;  // float32 resolution near full scale
const uint32_t unbounded_chunk_size = 0xFFFFFFFF;

std::string temp_path(const std::string& name) {
    const char* dir = std::getenv("TMPDIR");
    std::string base = (dir != nullptr && *dir != '\0') ? dir : "/tmp";
    if (base[base.size() - 1] != '/') base += '/';
    return base + "unlimited_test_" + name;
}

class Bytes {
public:
    Bytes& u8(uint8_t value) {
        data_.push_back(value);
        return *this;
    }
    // Little-endian integer of 'count' bytes (two's complement for negatives).
    Bytes& le(int64_t value, size_t count) {
        const uint64_t bits = static_cast<uint64_t>(value);
        for (size_t i = 0; i < count; ++i) data_.push_back(static_cast<uint8_t>(bits >> (bits_per_byte * i)));
        return *this;
    }
    Bytes& u16(uint16_t value) { return le(value, sizeof(uint16_t)); }
    Bytes& u32(uint32_t value) { return le(value, sizeof(uint32_t)); }
    Bytes& f32(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return u32(bits);
    }
    Bytes& id(const char* fourcc) {
        data_.insert(data_.end(), fourcc, fourcc + std::strlen(fourcc));
        return *this;
    }
    Bytes& append(const Bytes& other) {
        data_.insert(data_.end(), other.data_.begin(), other.data_.end());
        return *this;
    }
    const std::vector<uint8_t>& data() const { return data_; }
    size_t size() const { return data_.size(); }

private:
    std::vector<uint8_t> data_;
};

// A chunk with its header and the pad byte required after an odd-sized body.
Bytes chunk(const char* id, const Bytes& body) {
    Bytes out;
    out.id(id).u32(static_cast<uint32_t>(body.size())).append(body);
    if (body.size() % 2 != 0) out.u8(0);
    return out;
}

Bytes fmt_body(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits, uint16_t block_align) {
    Bytes body;
    body.u16(tag).u16(channels).u32(rate).u32(rate * block_align).u16(block_align).u16(bits);
    return body;
}

Bytes fmt_body(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits) {
    const uint16_t block_align = static_cast<uint16_t>(channels * ((bits + bits_per_byte - 1) / bits_per_byte));
    return fmt_body(tag, channels, rate, bits, block_align);
}

Bytes fmt_extensible_body(uint16_t subformat, uint16_t channels, uint32_t rate, uint16_t container_bits,
                          uint16_t valid_bits) {
    const uint16_t extension_size = 22;
    const uint32_t channel_mask = 0;
    // KSDATAFORMAT_SUBTYPE_* GUID tail shared by PCM and IEEE float.
    const uint8_t guid_tail[] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    Bytes body = fmt_body(tag_extensible, channels, rate, container_bits);
    body.u16(extension_size).u16(valid_bits).u32(channel_mask).u16(subformat);
    for (size_t i = 0; i < count_of(guid_tail); ++i) body.u8(guid_tail[i]);
    return body;
}

Bytes riff(const Bytes& chunks, const char* form = "WAVE") {
    Bytes out;
    out.id("RIFF").u32(static_cast<uint32_t>(chunks.size() + std::strlen(form))).id(form).append(chunks);
    return out;
}

Bytes simple_wav(const Bytes& fmt, const Bytes& data) {
    Bytes chunks;
    chunks.append(chunk("fmt ", fmt)).append(chunk("data", data));
    return riff(chunks);
}

void write_file(const std::string& path, const Bytes& bytes) {
    std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data().data()), static_cast<std::streamsize>(bytes.size()));
}

bool read_bytes(const std::string& name, const Bytes& bytes, std::vector<float>& samples, uint32_t& rate,
                std::string& error) {
    const std::string path = temp_path(name);
    write_file(path, bytes);
    const bool ok = read_wav(path, samples, rate, error);
    std::remove(path.c_str());
    return ok;
}

void check_samples(const std::vector<float>& actual, const std::vector<double>& expected, double tolerance) {
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) CHECK_NEAR(actual[i], expected[i], tolerance);
}

}  // namespace

TEST(wav_round_trip_16bit) {
    const double tone_hz = 440.0;
    const double tone_amplitude = 0.8;
    const size_t tone_samples = 1000;
    std::vector<float> samples;
    for (size_t n = 0; n < tone_samples; ++n)
        samples.push_back(static_cast<float>(tone_amplitude * std::sin(2.0 * pi * tone_hz * n / test_rate)));
    const float extremes[] = {1.0f, -1.0f, 1.5f, -1.5f, 0.0f, 1e-5f};
    samples.insert(samples.end(), extremes, extremes + count_of(extremes));

    const std::string path = temp_path("round_trip.wav");
    std::string error;
    REQUIRE(write_wav(path, samples, test_rate, error));

    std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
    const size_t canonical_header = 44;
    CHECK_EQ(static_cast<size_t>(file.tellg()), canonical_header + pcm16_bytes * samples.size());

    std::vector<float> back;
    uint32_t rate = 0;
    REQUIRE(read_wav(path, back, rate, error));
    std::remove(path.c_str());
    CHECK_EQ(rate, test_rate);
    REQUIRE(back.size() == samples.size());
    // write scales by 32767 and rounds, read divides by 32768: error <= 1.5 LSB
    const double tolerance = 1.5 * pcm16_step;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double clipped = std::max(-1.0, std::min(1.0, static_cast<double>(samples[i])));
        CHECK_NEAR(back[i], clipped, tolerance);
    }
}

TEST(wav_write_rejects_bad_sample_rate) {
    const std::vector<float> samples(4, 0.0f);
    const uint32_t bad_rates[] = {0, std::numeric_limits<uint32_t>::max()};
    for (size_t i = 0; i < count_of(bad_rates); ++i) {
        std::string error;
        CHECK(!write_wav(temp_path("bad_rate.wav"), samples, bad_rates[i], error));
        CHECK(!error.empty());
    }
}

TEST(wav_read_8bit) {
    Bytes data;
    data.u8(0).u8(128).u8(255).u8(64);
    std::vector<float> samples;
    uint32_t rate = 0;
    std::string error;
    REQUIRE(read_bytes("8bit.wav", simple_wav(fmt_body(tag_pcm, mono, test_rate, pcm8_bits), data), samples, rate,
                       error));
    CHECK_EQ(rate, test_rate);
    check_samples(samples, {-1.0, 0.0, 127.0 / 128.0, -0.5}, exact);
}

TEST(wav_read_24bit) {
    Bytes data;
    data.le(0x7FFFFF, pcm24_bytes).le(-0x800000, pcm24_bytes).le(1, pcm24_bytes).le(-1, pcm24_bytes);
    data.le(0x400000, pcm24_bytes);
    std::vector<float> samples;
    uint32_t rate = 0;
    std::string error;
    REQUIRE(read_bytes("24bit.wav", simple_wav(fmt_body(tag_pcm, mono, test_rate, pcm24_bits), data), samples, rate,
                       error));
    check_samples(samples, {0x7FFFFF / pcm24_full_scale, -1.0, 1.0 / pcm24_full_scale, -1.0 / pcm24_full_scale, 0.5},
                  exact);
}

TEST(wav_read_32bit_integer) {
    Bytes data;
    data.le(INT32_MIN, pcm32_bytes).le(0x40000000, pcm32_bytes).le(-0x40000000, pcm32_bytes);
    data.le(INT32_MAX, pcm32_bytes);
    std::vector<float> samples;
    uint32_t rate = 0;
    std::string error;
    REQUIRE(read_bytes("32bit.wav", simple_wav(fmt_body(tag_pcm, mono, test_rate, pcm32_bits), data), samples, rate,
                       error));
    check_samples(samples, {-1.0, 0.5, -0.5, 1.0}, float_tolerance);
}

TEST(wav_read_float32) {
    Bytes data;
    data.f32(0.25f).f32(-0.75f).f32(1.5f).f32(-2.0f);
    std::vector<float> samples;
    uint32_t rate = 0;
    std::string error;
    REQUIRE(read_bytes("float.wav", simple_wav(fmt_body(tag_float, mono, test_rate, float32_bits), data), samples,
                       rate, error));
    check_samples(samples, {0.25, -0.75, 1.0, -1.0}, exact);
}

TEST(wav_read_float32_non_finite) {
    // NaN (either sign) carries no level and reads as silence; infinities clip to full scale.
    Bytes data;
    data.f32(std::numeric_limits<float>::quiet_NaN()).f32(-std::numeric_limits<float>::quiet_NaN());
    data.f32(std::numeric_limits<float>::infinity()).f32(-std::numeric_limits<float>::infinity());
    std::vector<float> samples;
    uint32_t rate = 0;
    std::string error;
    REQUIRE(read_bytes("float_non_finite.wav", simple_wav(fmt_body(tag_float, mono, test_rate, float32_bits), data),
                       samples, rate, error));
    check_samples(samples, {0.0, 0.0, 1.0, -1.0}, exact);
}

TEST(wav_read_stereo_downmix) {
    Bytes data;
    data.le(16384, pcm16_bytes).le(-8192, pcm16_bytes);   // frame 0: 0.5, -0.25
    data.le(-32768, pcm16_bytes).le(-32768, pcm16_bytes);  // frame 1: -1, -1
    std::vector<float> samples;
    uint32_t rate = 0;
    std::string error;
    REQUIRE(read_bytes("stereo.wav", simple_wav(fmt_body(tag_pcm, stereo, test_rate, pcm16_bits), data), samples,
                       rate, error));
    check_samples(samples, {0.125, -1.0}, exact);

    Bytes eight;
    eight.u8(255).u8(0).u8(192).u8(128);  // frame 0: 127/128, -1; frame 1: 0.5, 0
    REQUIRE(read_bytes("stereo8.wav", simple_wav(fmt_body(tag_pcm, stereo, test_rate, pcm8_bits), eight), samples,
                       rate, error));
    check_samples(samples, {-1.0 / 256.0, 0.25}, exact);

    Bytes three;
    three.f32(0.3f).f32(0.6f).f32(-0.3f);
    REQUIRE(read_bytes("three.wav", simple_wav(fmt_body(tag_float, three_channels, test_rate, float32_bits), three),
                       samples, rate, error));
    check_samples(samples, {0.2}, float_tolerance);
}

TEST(wav_read_padded_block_align) {
    // 16-bit mono in 4-byte frames: the reader must step by block_align, not by the sample size.
    const uint16_t padded_align = 4;
    const size_t pad_bytes = padded_align - pcm16_bytes;
    Bytes data;
    data.le(16384, pcm16_bytes).le(-1, pad_bytes).le(-16384, pcm16_bytes).le(-1, pad_bytes);
    std::vector<float> samples;
    uint32_t rate = 0;
    std::string error;
    REQUIRE(read_bytes("padded.wav", simple_wav(fmt_body(tag_pcm, mono, test_rate, pcm16_bits, padded_align), data),
                       samples, rate, error));
    check_samples(samples, {0.5, -0.5}, exact);
}

TEST(wav_read_extensible) {
    // 24 valid bits left-justified in a 32-bit container, stereo
    const uint16_t container_bits = pcm32_bits;
    const uint16_t valid_bits = pcm24_bits;
    const int64_t half_scale_24_in_32 = int64_t(0x400000) << (container_bits - valid_bits);
    Bytes data;
    data.le(half_scale_24_in_32, pcm32_bytes).le(0, pcm32_bytes);
    std::vector<float> samples;
    uint32_t rate = 0;
    std::string error;
    REQUIRE(read_bytes("ext_pcm.wav",
                       simple_wav(fmt_extensible_body(tag_pcm, stereo, test_rate, container_bits, valid_bits), data),
                       samples, rate, error));
    check_samples(samples, {0.25}, exact);

    Bytes float_data;
    float_data.f32(-0.5f);
    REQUIRE(read_bytes("ext_float.wav",
                       simple_wav(fmt_extensible_body(tag_float, mono, test_rate, float32_bits, float32_bits),
                                  float_data),
                       samples, rate, error));
    check_samples(samples, {-0.5}, exact);
}

TEST(wav_skips_unknown_and_odd_chunks) {
    Bytes odd;
    odd.u8('a').u8('b').u8('c');
    Bytes data;
    data.le(16384, pcm16_bytes).le(-16384, pcm16_bytes).le(0, pcm16_bytes);
    Bytes chunks;
    chunks.append(chunk("LIST", odd))
        .append(chunk("fmt ", fmt_body(tag_pcm, mono, test_rate, pcm16_bits)))
        .append(chunk("junk", odd))
        .append(chunk("data", data))
        .append(chunk("cue ", odd));
    std::vector<float> samples;
    uint32_t rate = 0;
    std::string error;
    REQUIRE(read_bytes("chunks.wav", riff(chunks), samples, rate, error));
    check_samples(samples, {0.5, -0.5, 0.0}, exact);
}

TEST(wav_data_before_fmt_and_odd_data) {
    // data first, with an odd size (three 8-bit samples + pad byte), then LIST, then fmt.
    Bytes data;
    data.u8(0).u8(128).u8(192);
    Bytes info;
    info.id("INFO");
    Bytes chunks;
    chunks.append(chunk("data", data))
        .append(chunk("LIST", info))
        .append(chunk("fmt ", fmt_body(tag_pcm, mono, test_rate, pcm8_bits)));
    std::vector<float> samples;
    uint32_t rate = 0;
    std::string error;
    REQUIRE(read_bytes("data_first.wav", riff(chunks), samples, rate, error));
    CHECK_EQ(rate, test_rate);
    check_samples(samples, {-1.0, 0.0, 0.5}, exact);
}

TEST(wav_clamps_truncated_data_chunk) {
    // data chunk claims more bytes than the file holds: 3 frames and a half
    const uint32_t claimed_size = 1000;
    Bytes data;
    data.le(8192, pcm16_bytes).le(8192, pcm16_bytes).le(8192, pcm16_bytes).u8(0);
    Bytes chunks;
    chunks.append(chunk("fmt ", fmt_body(tag_pcm, mono, test_rate, pcm16_bits)));
    chunks.id("data").u32(claimed_size).append(data);
    std::vector<float> samples;
    uint32_t rate = 0;
    std::string error;
    REQUIRE(read_bytes("truncated_data.wav", riff(chunks), samples, rate, error));
    check_samples(samples, {0.25, 0.25, 0.25}, exact);
}

TEST(wav_rejects_malformed) {
    Bytes data;
    data.le(0, pcm16_bytes).le(0, pcm16_bytes);
    const Bytes good_fmt = fmt_body(tag_pcm, mono, test_rate, pcm16_bits);

    struct Case {
        const char* name;
        Bytes bytes;
    };
    std::vector<Case> cases;
    cases.push_back(Case{"empty", Bytes()});
    {
        Bytes b;
        b.id("RIFX").u32(sizeof(uint32_t)).id("WAVE");
        cases.push_back(Case{"not_riff", b});
    }
    cases.push_back(Case{"not_wave", riff(chunk("fmt ", good_fmt).append(chunk("data", data)), "AVI ")});
    cases.push_back(Case{"missing_fmt", riff(chunk("data", data))});
    cases.push_back(Case{"missing_data", riff(chunk("fmt ", good_fmt))});
    {
        Bytes b;
        b.id("fmt ").u32(static_cast<uint32_t>(good_fmt.size())).u16(tag_pcm).u16(mono);  // body cut short
        cases.push_back(Case{"truncated_fmt", riff(b)});
    }
    {
        // A chunk claiming 4 GiB before fmt: the walk stops there, so fmt is never seen.
        Bytes b;
        b.id("LIST").u32(unbounded_chunk_size).id("INFO");
        b.append(chunk("fmt ", good_fmt)).append(chunk("data", data));
        cases.push_back(Case{"unbounded_chunk_before_fmt", riff(b)});
    }
    {
        Bytes short_fmt;
        short_fmt.u16(tag_pcm).u16(mono).u32(test_rate);
        cases.push_back(Case{"short_fmt", simple_wav(short_fmt, data)});
    }
    cases.push_back(Case{"adpcm", simple_wav(fmt_body(tag_adpcm, mono, test_rate, adpcm_bits), data)});
    cases.push_back(Case{"float16", simple_wav(fmt_body(tag_float, mono, test_rate, float16_bits), data)});
    cases.push_back(Case{"pcm40", simple_wav(fmt_body(tag_pcm, mono, test_rate, pcm40_bits), data)});
    cases.push_back(Case{"pcm0", simple_wav(fmt_body(tag_pcm, mono, test_rate, 0), data)});
    cases.push_back(Case{"zero_channels", simple_wav(fmt_body(tag_pcm, 0, test_rate, pcm16_bits), data)});
    cases.push_back(Case{"zero_rate", simple_wav(fmt_body(tag_pcm, mono, 0, pcm16_bits), data)});
    cases.push_back(
        Case{"block_align", simple_wav(fmt_body(tag_pcm, stereo, test_rate, pcm16_bits, pcm16_bytes), data)});
    {
        Bytes ext = fmt_body(tag_extensible, mono, test_rate, pcm16_bits);
        ext.u16(0);  // cbSize 0: no subformat
        cases.push_back(Case{"short_extensible", simple_wav(ext, data)});
    }

    for (size_t i = 0; i < cases.size(); ++i) {
        std::vector<float> samples;
        uint32_t rate = 0;
        std::string error;
        const bool ok = read_bytes(std::string("bad_") + cases[i].name + ".wav", cases[i].bytes, samples, rate, error);
        if (!CHECK(!ok)) NOTE("case %s was accepted", cases[i].name);
        if (!ok && error.empty()) NOTE("case %s: empty error", cases[i].name);
        CHECK(ok || !error.empty());
    }

    std::vector<float> samples;
    uint32_t rate = 0;
    std::string error;
    CHECK(!read_wav(temp_path("does_not_exist.wav"), samples, rate, error));
    CHECK(!error.empty());
}

TEST(wav_write_reports_bad_path) {
    std::string error;
    CHECK(!write_wav("/nonexistent_directory_for_test/out.wav", std::vector<float>(1), test_rate, error));
    CHECK(!error.empty());
}
