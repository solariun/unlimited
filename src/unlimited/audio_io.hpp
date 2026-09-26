#pragma once

#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

namespace unlimited {

// Driver boundary (D22). Protected non-virtual destructors: the core never deletes through
// these interfaces, so no operator delete is pulled in on MCUs.

class SampleSource {  // produces audio; read() returns 0 at the end
public:
    virtual size_t read(int16_t* out, size_t count) = 0;

protected:
    ~SampleSource() {}
};

class SampleSink {  // consumes audio
public:
    virtual void write(const int16_t* in, size_t count) = 0;

protected:
    ~SampleSink() {}
};

class AudioOutput {  // a device or file that plays a source
public:
    virtual bool start(SampleSource& source, uint32_t sample_rate_hz) = 0;  // driver pulls until read() == 0
    virtual bool wait() = 0;  // blocks until drained; true when all audio was delivered
    virtual void stop() = 0;

protected:
    ~AudioOutput() {}
};

class AudioInput {  // a device or file that captures into a sink
public:
    virtual bool start(SampleSink& sink, uint32_t sample_rate_hz) = 0;  // driver pushes as audio arrives
    virtual bool wait() = 0;  // blocks until the input ends (file) or stop() is called
    virtual void stop() = 0;

protected:
    ~AudioInput() {}
};

// The adapters are inline so that a program using only one of them links only that side of the core.

class EncoderSource : public SampleSource {  // adapts an Encoder; returns 0 once it is idle
public:
    explicit EncoderSource(Encoder& encoder) : encoder_(encoder) {}

    size_t read(int16_t* out, size_t count) override {
        return encoder_.render(out, count);
    }

private:
    Encoder& encoder_;
};

class DecoderSink : public SampleSink {  // adapts a Decoder (input must already be 8 kHz)
public:
    explicit DecoderSink(Decoder& decoder) : decoder_(decoder) {}

    void write(const int16_t* in, size_t count) override {
        decoder_.process(in, count);
    }

private:
    Decoder& decoder_;
};

}  // namespace unlimited
