#include "unlimited/wav_codec.hpp"

#include <string.h>

namespace unlimited {

const uint8_t WavReader::k_max_frame_bytes;

namespace {

static_assert(sizeof(float) == sizeof(uint32_t), "32-bit IEEE float required");

const uint16_t k_format_pcm = 0x0001;
const uint16_t k_format_ieee_float = 0x0003;
const uint16_t k_format_extensible = 0xFFFE;

// RIFF layout: "RIFF" <u32 size> "WAVE", then chunks of <id> <u32 size> <body> [pad to even].
const uint8_t k_fourcc_bytes = 4;
const uint8_t k_riff_header_bytes = 12;
const uint8_t k_riff_form_offset = 8;
const uint8_t k_chunk_header_bytes = 8;
const uint8_t k_chunk_size_offset = 4;
const uint32_t k_riff_size_offset = 4;
const uint32_t k_data_size_offset = 40;
const uint32_t k_riff_size_base = k_wav_header_bytes - k_chunk_header_bytes;  // RIFF size minus data bytes

// "fmt " chunk body.
const uint8_t k_fmt_tag_offset = 0;
const uint8_t k_fmt_channels_offset = 2;
const uint8_t k_fmt_rate_offset = 4;
const uint8_t k_fmt_byte_rate_offset = 8;
const uint8_t k_fmt_block_align_offset = 12;
const uint8_t k_fmt_bits_offset = 14;
const uint8_t k_fmt_basic_bytes = 16;
const uint8_t k_fmt_extensible_bytes = 40;
const uint8_t k_fmt_subformat_offset = 24;  // GUID whose first two bytes are the real format tag

const uint8_t k_max_container_bytes = 4;
const uint8_t k_float_bytes = 4;
const uint16_t k_output_channels = 1;
const uint16_t k_output_bits = 16;
const uint8_t k_output_sample_bytes = 2;
const uint32_t k_max_output_samples = (0xFFFFFFFFu - k_riff_size_base) / k_output_sample_bytes;

const uint8_t k_skip_buffer_bytes = 64;
const uint8_t k_write_buffer_samples = 32;
const uint8_t k_output_chunk_samples = 64;

// Container widths in bytes; 8-bit PCM is unsigned, wider PCM is two's complement.
const uint8_t k_pcm8_bytes = 1;
const uint8_t k_pcm16_bytes = 2;
const uint8_t k_pcm24_bytes = 3;
const uint8_t k_pcm16_bits = 16;
const uint8_t k_pcm24_bits = 24;
const int32_t k_pcm8_offset = 128;
const int32_t k_int16_max = 32767;
const int32_t k_int16_min = -32768;
const uint8_t k_int24_to_16_shift = 8;
const uint8_t k_int32_to_16_shift = 16;
const uint32_t k_int32_sign = 0x80000000u;
const float k_int16_scale = 32768.0f;
const float k_int8_scale = 128.0f;
const float k_int24_scale = 8388608.0f;
const float k_int32_scale = 2147483648.0f;
const float k_round_half = 0.5f;

uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << k_bits_per_byte));
}

uint32_t read_u32(const uint8_t* p) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < sizeof(uint32_t); ++i) value |= static_cast<uint32_t>(p[i]) << (k_bits_per_byte * i);
    return value;
}

void write_u16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> k_bits_per_byte);
}

void write_u32(uint8_t* p, uint32_t value) {
    for (uint8_t i = 0; i < sizeof(uint32_t); ++i) p[i] = static_cast<uint8_t>(value >> (k_bits_per_byte * i));
}

bool has_id(const uint8_t* p, const char* id) {
    return memcmp(p, id, k_fourcc_bytes) == 0;
}

int32_t to_int32(uint32_t raw) {
    return raw < k_int32_sign ? static_cast<int32_t>(raw) : -static_cast<int32_t>(~raw) - 1;
}

// Two's complement value of the low `bits` bits (bits < 32).
int32_t sign_extend(uint32_t raw, uint8_t bits) {
    const uint32_t sign = static_cast<uint32_t>(1) << (bits - 1);
    const uint32_t mask = (sign << 1) - 1;
    raw &= mask;
    return (raw & sign) != 0 ? -static_cast<int32_t>(mask - raw) - 1 : static_cast<int32_t>(raw);
}

int32_t clamp_int16(int32_t value) {
    return value > k_int16_max ? k_int16_max : (value < k_int16_min ? k_int16_min : value);
}

// NaN carries no level: silence. Everything else clips to full scale.
float decode_float(const uint8_t* p) {
    const uint32_t bits = read_u32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    if (value != value) return 0.0f;
    return value > 1.0f ? 1.0f : (value < -1.0f ? -1.0f : value);
}

uint32_t read_container(const uint8_t* p, uint8_t bytes) {
    uint32_t raw = 0;
    for (uint8_t i = 0; i < bytes; ++i) raw |= static_cast<uint32_t>(p[i]) << (k_bits_per_byte * i);
    return raw;
}

// One channel as int16, rounded half up and clamped.
int32_t channel_int16(const uint8_t* p, uint8_t bytes, WavEncoding encoding) {
    if (encoding == WavEncoding::ieee_float) {
        const float scaled = decode_float(p) * k_int16_scale;
        return clamp_int16(scaled >= 0.0f ? static_cast<int32_t>(scaled + k_round_half)
                                          : -static_cast<int32_t>(k_round_half - scaled));
    }
    const uint32_t raw = read_container(p, bytes);
    switch (bytes) {
        case k_pcm8_bytes:
            return (static_cast<int32_t>(raw) - k_pcm8_offset) * (1 << k_bits_per_byte);
        case k_pcm16_bytes:
            return sign_extend(raw, k_pcm16_bits);
        case k_pcm24_bytes:
            return clamp_int16((sign_extend(raw, k_pcm24_bits) + (1 << (k_int24_to_16_shift - 1))) >>
                               k_int24_to_16_shift);
        default: {
            const int32_t value = to_int32(raw);
            return clamp_int16((value >> k_int32_to_16_shift) + ((value >> (k_int32_to_16_shift - 1)) & 1));
        }
    }
}

// One channel in [-1, 1]; exact for 8, 16 and 24 bits.
float channel_float(const uint8_t* p, uint8_t bytes, WavEncoding encoding) {
    if (encoding == WavEncoding::ieee_float) return decode_float(p);
    const uint32_t raw = read_container(p, bytes);
    switch (bytes) {
        case k_pcm8_bytes:
            return (static_cast<float>(raw) - k_int8_scale) / k_int8_scale;
        case k_pcm16_bytes:
            return static_cast<float>(sign_extend(raw, k_pcm16_bits)) / k_int16_scale;
        case k_pcm24_bytes:
            return static_cast<float>(sign_extend(raw, k_pcm24_bits)) / k_int24_scale;
        default:
            return static_cast<float>(to_int32(raw)) / k_int32_scale;
    }
}

}  // namespace

bool wav_build_header(uint32_t sample_rate_hz, uint32_t total_samples, uint8_t (&header)[k_wav_header_bytes]) {
    const uint16_t block_align = k_output_channels * k_output_sample_bytes;
    if (sample_rate_hz == 0 || sample_rate_hz > 0xFFFFFFFFu / block_align) return false;
    uint32_t data_bytes = k_wav_unknown_size;
    uint32_t riff_size = k_wav_unknown_size;
    if (total_samples != k_wav_unknown_size) {
        if (total_samples > k_max_output_samples) return false;
        data_bytes = total_samples * k_output_sample_bytes;
        riff_size = k_riff_size_base + data_bytes;
    }
    uint8_t* p = header;
    memcpy(p, "RIFF", k_fourcc_bytes);
    write_u32(p + k_riff_size_offset, riff_size);
    memcpy(p + k_riff_form_offset, "WAVE", k_fourcc_bytes);
    p += k_riff_header_bytes;
    memcpy(p, "fmt ", k_fourcc_bytes);
    write_u32(p + k_chunk_size_offset, k_fmt_basic_bytes);
    p += k_chunk_header_bytes;
    write_u16(p + k_fmt_tag_offset, k_format_pcm);
    write_u16(p + k_fmt_channels_offset, k_output_channels);
    write_u32(p + k_fmt_rate_offset, sample_rate_hz);
    write_u32(p + k_fmt_byte_rate_offset, sample_rate_hz * block_align);
    write_u16(p + k_fmt_block_align_offset, block_align);
    write_u16(p + k_fmt_bits_offset, k_output_bits);
    p += k_fmt_basic_bytes;
    memcpy(p, "data", k_fourcc_bytes);
    write_u32(p + k_chunk_size_offset, data_bytes);
    return true;
}

WavWriter::WavWriter() : sink_(nullptr), sample_rate_hz_(0), declared_samples_(0), written_(0), ok_(false) {}

bool WavWriter::begin(ByteSink& sink, uint32_t sample_rate_hz, uint32_t total_samples) {
    sink_ = &sink;
    sample_rate_hz_ = sample_rate_hz;
    declared_samples_ = total_samples;
    written_ = 0;
    uint8_t header[k_wav_header_bytes];
    ok_ = wav_build_header(sample_rate_hz, total_samples, header) && sink.write(header, sizeof(header));
    return ok_;
}

void WavWriter::write(const int16_t* in, size_t count) {
    uint8_t buffer[k_write_buffer_samples * k_output_sample_bytes];
    size_t done = 0;
    while (ok_ && done < count) {
        size_t batch = count - done;
        if (batch > k_write_buffer_samples) batch = k_write_buffer_samples;
        if (batch > k_max_output_samples - written_) {
            ok_ = false;
            break;
        }
        for (size_t i = 0; i < batch; ++i)
            write_u16(buffer + i * k_output_sample_bytes, static_cast<uint16_t>(in[done + i]));
        ok_ = sink_->write(buffer, batch * k_output_sample_bytes);
        written_ += static_cast<uint32_t>(batch);
        done += batch;
    }
}

bool WavWriter::finish() {
    if (!ok_) return false;
    if (written_ == declared_samples_) return true;
    const uint32_t data_bytes = written_ * k_output_sample_bytes;
    uint8_t size[sizeof(uint32_t)];
    if (sink_->seek(k_riff_size_offset)) {
        write_u32(size, k_riff_size_base + data_bytes);
        ok_ = sink_->write(size, sizeof(size)) && sink_->seek(k_data_size_offset);
        write_u32(size, data_bytes);
        ok_ = ok_ && sink_->write(size, sizeof(size)) && sink_->seek(k_wav_header_bytes + data_bytes);
        return ok_;
    }
    // A sink that cannot seek keeps the header as written: valid only for streaming sizes.
    ok_ = declared_samples_ == k_wav_unknown_size;
    return ok_;
}

bool WavWriter::ok() const {
    return ok_;
}

uint32_t WavWriter::samples_written() const {
    return written_;
}

WavReader::WavReader()
    : source_(nullptr), format_(), remaining_bytes_(0), position_(0), block_align_(0), container_bytes_(0) {}

bool WavReader::open(ByteSource& source) {
    source_ = &source;
    format_ = WavFormat();
    remaining_bytes_ = 0;
    position_ = 0;
    block_align_ = 0;
    container_bytes_ = 0;

    uint8_t header[k_riff_header_bytes];
    if (!read_exact(header, sizeof(header)) || !has_id(header, "RIFF") ||
        !has_id(header + k_riff_form_offset, "WAVE"))
        return false;

    uint16_t tag = 0;
    bool have_fmt = false;
    bool have_data = false;
    uint32_t data_position = 0;
    uint32_t data_bytes = 0;
    // Chunk sizes are trusted only as far as the source goes: the walk stops at the first chunk that
    // runs past the end, and a data chunk longer than the file is read up to what exists.
    for (;;) {
        uint8_t chunk[k_chunk_header_bytes];
        if (!read_exact(chunk, sizeof(chunk))) break;
        const uint32_t size = read_u32(chunk + k_chunk_size_offset);
        const uint32_t pad = size & 1u;
        if (has_id(chunk, "fmt ")) {
            uint8_t body[k_fmt_extensible_bytes];
            if (size < k_fmt_basic_bytes) return false;
            const uint32_t kept = size < sizeof(body) ? size : sizeof(body);
            if (!read_exact(body, kept) || !skip(size - kept)) return false;
            tag = read_u16(body + k_fmt_tag_offset);
            format_.channels = read_u16(body + k_fmt_channels_offset);
            format_.sample_rate_hz = read_u32(body + k_fmt_rate_offset);
            block_align_ = read_u16(body + k_fmt_block_align_offset);
            format_.bits_per_sample = read_u16(body + k_fmt_bits_offset);
            if (tag == k_format_extensible) {
                if (size < k_fmt_extensible_bytes) return false;
                tag = read_u16(body + k_fmt_subformat_offset);
            }
            have_fmt = true;
            if (have_data || !skip(pad)) break;
        } else if (has_id(chunk, "data")) {
            have_data = true;
            data_position = position_;
            data_bytes = size;
            if (have_fmt) break;
            if (!skip(size) || !skip(pad)) break;
        } else if (!skip(size) || !skip(pad)) {
            break;
        }
    }
    if (!have_fmt || !have_data) return false;

    if (format_.channels == 0 || format_.sample_rate_hz == 0) return false;
    // Checked before narrowing: a uint8_t container would wrap bits_per_sample 2049 to 1 byte.
    if (format_.bits_per_sample == 0 || format_.bits_per_sample > k_max_container_bytes * k_bits_per_byte) return false;
    container_bytes_ = static_cast<uint8_t>((format_.bits_per_sample + k_bits_per_byte - 1) / k_bits_per_byte);
    if (tag == k_format_pcm) {
        format_.encoding = WavEncoding::pcm;
    } else if (tag == k_format_ieee_float) {
        if (container_bytes_ != k_float_bytes) return false;
        format_.encoding = WavEncoding::ieee_float;
    } else {
        return false;
    }
    const uint32_t frame_bytes = static_cast<uint32_t>(format_.channels) * container_bytes_;
    if (block_align_ < frame_bytes || block_align_ > k_max_frame_bytes) return false;
    if (position_ != data_position) {
        if (!source.seek(data_position)) return false;
        position_ = data_position;
    }
    format_.data_bytes = data_bytes;
    remaining_bytes_ = data_bytes;
    return true;
}

const WavFormat& WavReader::format() const {
    return format_;
}

size_t WavReader::read(int16_t* out, size_t count) {
    if (block_align_ == 0) return 0;
    uint8_t frames[k_max_frame_bytes];
    const size_t batch = k_max_frame_bytes / block_align_;
    const int32_t channels = format_.channels;
    const int32_t half = channels / 2;
    size_t produced = 0;
    while (produced < count) {
        const size_t got = read_frames(frames, count - produced < batch ? count - produced : batch);
        for (size_t f = 0; f < got; ++f) {
            const uint8_t* frame = frames + f * block_align_;
            int32_t sum = 0;
            for (uint16_t c = 0; c < format_.channels; ++c)
                sum += channel_int16(frame + c * container_bytes_, container_bytes_, format_.encoding);
            out[produced++] = static_cast<int16_t>(sum >= 0 ? (sum + half) / channels : -((half - sum) / channels));
        }
        if (got == 0) break;
    }
    return produced;
}

size_t WavReader::read(float* out, size_t count) {
    if (block_align_ == 0) return 0;
    uint8_t frames[k_max_frame_bytes];
    const size_t batch = k_max_frame_bytes / block_align_;
    size_t produced = 0;
    while (produced < count) {
        const size_t got = read_frames(frames, count - produced < batch ? count - produced : batch);
        for (size_t f = 0; f < got; ++f) {
            const uint8_t* frame = frames + f * block_align_;
            float sum = 0.0f;
            for (uint16_t c = 0; c < format_.channels; ++c)
                sum += channel_float(frame + c * container_bytes_, container_bytes_, format_.encoding);
            out[produced++] = sum / format_.channels;
        }
        if (got == 0) break;
    }
    return produced;
}

uint32_t WavReader::read_bytes(uint8_t* data, uint32_t size) {
    uint32_t done = 0;
    while (done < size) {
        const size_t got = source_->read(data + done, size - done);
        if (got == 0) break;
        done += static_cast<uint32_t>(got);
    }
    position_ += done;
    return done;
}

bool WavReader::read_exact(uint8_t* data, uint32_t size) {
    return read_bytes(data, size) == size;
}

bool WavReader::skip(uint32_t size) {
    uint8_t scratch[k_skip_buffer_bytes];
    while (size > 0) {
        const uint32_t step = size < sizeof(scratch) ? size : sizeof(scratch);
        if (!read_exact(scratch, step)) return false;
        size -= step;
    }
    return true;
}

// Whole frames only: at the end of the source a partial frame is dropped.
size_t WavReader::read_frames(uint8_t* frames, size_t count) {
    if (remaining_bytes_ != k_wav_unknown_size && count > remaining_bytes_ / block_align_)
        count = remaining_bytes_ / block_align_;
    if (count == 0) return 0;
    const uint32_t wanted = static_cast<uint32_t>(count * block_align_);
    const uint32_t got = read_bytes(frames, wanted);
    if (got < wanted) {
        remaining_bytes_ = 0;
    } else if (remaining_bytes_ != k_wav_unknown_size) {
        remaining_bytes_ -= got;
    }
    return got / block_align_;
}

WavOutput::WavOutput(ByteSink& sink, uint32_t total_samples) : sink_(sink), total_samples_(total_samples), ok_(false) {}

bool WavOutput::start(SampleSource& source, uint32_t sample_rate_hz) {
    WavWriter writer;
    ok_ = writer.begin(sink_, sample_rate_hz, total_samples_);
    int16_t buffer[k_output_chunk_samples];
    while (ok_) {
        const size_t got = source.read(buffer, k_output_chunk_samples);
        if (got == 0) break;
        writer.write(buffer, got);
        ok_ = writer.ok();
    }
    ok_ = writer.finish() && ok_;
    return ok_;
}

bool WavOutput::wait() {
    return ok_;
}

void WavOutput::stop() {}

}  // namespace unlimited
