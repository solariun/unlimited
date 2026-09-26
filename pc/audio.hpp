#pragma once

#include "resampler.hpp"
#include "unlimited/audio_io.hpp"
#include "unlimited/dsp.hpp"
#include "wav.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace unlimited {
namespace pc {

static const std::size_t k_device_chunk_samples = 256;  // samples per read()/write() call of the drivers below

// Owned devices. The core interfaces have protected non-virtual destructors (the core never deletes through
// them), so PC drivers derive from these, which add the virtual destructor.
class OutputDevice : public AudioOutput {
public:
    virtual ~OutputDevice() {}
};

class InputDevice : public AudioInput {
public:
    virtual ~InputDevice() {}
    virtual std::uint32_t sample_rate_hz() const = 0;  // the rate the device delivers; pass it to start()
};

// Device specs: "wav:<path>" (a bare path ending in .wav means the same) and "null" (output: discard, input:
// no audio, nominally at 8 kHz). Anything else gives nullptr and an error. The file and null drivers run
// synchronously inside start(): an output pulls until read() returns 0, an input pushes its audio in chunks,
// and stop() called from inside those calls ends the transfer after the current chunk. A WAV output is 16-bit
// PCM mono at the rate given to start(); a WAV input delivers the file's rate, downmixed to mono.
std::unique_ptr<OutputDevice> open_output(const std::string& spec, std::string& error);
std::unique_ptr<InputDevice> open_input(const std::string& spec, std::string& error);

class MemoryOutput final : public OutputDevice {  // collects the audio
public:
    MemoryOutput();
    bool start(SampleSource& source, std::uint32_t sample_rate_hz) override;
    bool wait() override;
    void stop() override;
    const std::vector<std::int16_t>& samples() const;
    std::uint32_t sample_rate_hz() const;

private:
    std::vector<std::int16_t> samples_;
    std::uint32_t sample_rate_hz_;
    bool stop_requested_;
    bool delivered_;
};

class MemoryInput final : public InputDevice {  // plays a buffer at its rate, chunk_samples per write()
public:
    MemoryInput(const std::vector<std::int16_t>& samples, std::uint32_t sample_rate_hz,
                std::size_t chunk_samples = k_device_chunk_samples);
    bool start(SampleSink& sink, std::uint32_t sample_rate_hz) override;
    bool wait() override;
    void stop() override;
    std::uint32_t sample_rate_hz() const override;

private:
    std::vector<std::int16_t> samples_;
    std::uint32_t sample_rate_hz_;
    std::size_t chunk_samples_;
    bool stop_requested_;
    bool delivered_;
};

class MemorySource final : public SampleSource {  // plays a buffer (which must outlive it) into an AudioOutput
public:
    explicit MemorySource(const std::vector<std::int16_t>& samples);
    std::size_t read(std::int16_t* out, std::size_t count) override;

private:
    const std::vector<std::int16_t>& samples_;
    std::size_t position_;
};

// Resamples audio from from_hz to to_hz (default: the decoder's 8 kHz) and passes it on; a copy when the
// rates are equal. Chunking-invariant. flush() at the end of the input passes on what the resampler still holds.
class ResamplingSink final : public SampleSink {
public:
    ResamplingSink(SampleSink& sink, double from_hz, double to_hz = k_decoder_rate_hz);
    void write(const std::int16_t* in, std::size_t count) override;
    void flush();

private:
    void deliver();

    SampleSink& sink_;
    bool copy_;
    Resampler resampler_;
    std::vector<float> input_;
    std::vector<float> output_;
    std::vector<std::int16_t> converted_;
};

}  // namespace pc
}  // namespace unlimited
