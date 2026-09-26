#pragma once

#include "unlimited/audio_io.hpp"

namespace unlimited {

class ByteSink {
public:
    virtual bool write(const uint8_t* data, size_t size) = 0;
    virtual bool seek(uint32_t position) {  // optional; false when the sink cannot seek
        (void)position;
        return false;
    }

protected:
    ~ByteSink() {}
};

class ByteSource {
public:
    virtual size_t read(uint8_t* data, size_t size) = 0;  // 0 at the end
    virtual bool seek(uint32_t position) {                 // optional; needed only for a data chunk before fmt
        (void)position;
        return false;
    }

protected:
    ~ByteSource() {}
};

enum class WavEncoding : uint8_t { pcm, ieee_float };

struct WavFormat {
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t bits_per_sample;  // pcm 8/16/24/32, float 32
    WavEncoding encoding;
    uint32_t data_bytes;       // k_wav_unknown_size when streaming
};

static const uint16_t k_wav_header_bytes = 44;
static const uint32_t k_wav_unknown_size = 0xFFFFFFFFu;

bool wav_build_header(uint32_t sample_rate_hz, uint32_t total_samples, uint8_t (&header)[k_wav_header_bytes]);

class WavWriter : public SampleSink {  // 16-bit PCM mono
public:
    WavWriter();
    bool begin(ByteSink& sink, uint32_t sample_rate_hz, uint32_t total_samples = k_wav_unknown_size);
    void write(const int16_t* in, size_t count) override;
    bool finish();  // patches the sizes when the sink can seek
    bool ok() const;
    uint32_t samples_written() const;

private:
    ByteSink* sink_;
    uint32_t sample_rate_hz_;
    uint32_t declared_samples_;
    uint32_t written_;
    bool ok_;
};

class WavReader : public SampleSource {  // any supported format, downmixed to mono
public:
    static const uint8_t k_max_frame_bytes = 128;  // channels * container bytes

    WavReader();
    bool open(ByteSource& source);  // parses RIFF and fmt (incl. EXTENSIBLE), skips other chunks
    const WavFormat& format() const;
    size_t read(int16_t* out, size_t count) override;
    size_t read(float* out, size_t count);  // full precision, [-1, 1]

private:
    uint32_t read_bytes(uint8_t* data, uint32_t size);
    bool read_exact(uint8_t* data, uint32_t size);
    bool skip(uint32_t size);
    size_t read_frames(uint8_t* frames, size_t count);

    ByteSource* source_;
    WavFormat format_;
    uint32_t remaining_bytes_;
    uint32_t position_;
    uint16_t block_align_;
    uint8_t container_bytes_;
};

class WavOutput : public AudioOutput {  // "wav:" driver; runs synchronously inside start()
public:
    explicit WavOutput(ByteSink& sink, uint32_t total_samples = k_wav_unknown_size);
    bool start(SampleSource& source, uint32_t sample_rate_hz) override;
    bool wait() override;
    void stop() override;

private:
    ByteSink& sink_;
    uint32_t total_samples_;
    bool ok_;
};

}  // namespace unlimited
