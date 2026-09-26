#include "audio.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace unlimited {
namespace pc {

using std::int16_t;
using std::size_t;
using std::uint32_t;
using std::uint8_t;

namespace {

const std::string k_null_spec = "null";
const std::string k_wav_prefix = "wav:";
const std::string k_wav_extension = ".wav";

bool has_wav_extension(const std::string& path) {
    if (path.size() <= k_wav_extension.size()) return false;
    const size_t offset = path.size() - k_wav_extension.size();
    for (size_t i = 0; i < k_wav_extension.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(path[offset + i])) != k_wav_extension[i]) return false;
    return true;
}

// True when the spec names a WAV file; path is then its file name (possibly empty).
bool wav_path(const std::string& spec, std::string& path) {
    if (spec.compare(0, k_wav_prefix.size(), k_wav_prefix) == 0) {
        path = spec.substr(k_wav_prefix.size());
        return true;
    }
    if (!has_wav_extension(spec)) return false;
    path = spec;
    return true;
}

std::string unknown_spec(const std::string& spec) {
    return "unknown audio device '" + spec + "' (expected wav:<path>, <path>.wav or null)";
}

int16_t to_int16(float x) {
    const float low = std::numeric_limits<int16_t>::min();
    const float high = std::numeric_limits<int16_t>::max();
    return static_cast<int16_t>(std::lround(std::max(low, std::min(high, x))));
}

// Ends the stream early once stop was requested.
class StoppableSource final : public SampleSource {
public:
    StoppableSource(SampleSource& source, const bool& stop_requested)
        : source_(source), stop_requested_(stop_requested) {}

    size_t read(int16_t* out, size_t count) override { return stop_requested_ ? 0 : source_.read(out, count); }

private:
    SampleSource& source_;
    const bool& stop_requested_;
};

class NullOutput final : public OutputDevice {
public:
    bool start(SampleSource& source, uint32_t sample_rate_hz) override {
        if (sample_rate_hz == 0) return false;
        int16_t buffer[k_device_chunk_samples];
        while (!stop_requested_ && source.read(buffer, k_device_chunk_samples) > 0) {
        }
        delivered_ = !stop_requested_;
        return true;
    }

    bool wait() override { return delivered_; }
    void stop() override { stop_requested_ = true; }

private:
    bool stop_requested_ = false;
    bool delivered_ = false;
};

class NullInput final : public InputDevice {
public:
    bool start(SampleSink&, uint32_t) override { return true; }
    bool wait() override { return true; }
    void stop() override {}
    uint32_t sample_rate_hz() const override { return k_decoder_rate_hz; }
};

class WavFileOutput final : public OutputDevice {
public:
    WavFileOutput() : wav_(file_) {}

    bool open(const std::string& path) { return file_.open(path); }

    bool start(SampleSource& source, uint32_t sample_rate_hz) override {
        if (started_) return false;
        started_ = true;
        StoppableSource stoppable(source, stop_requested_);
        const bool started = wav_.start(stoppable, sample_rate_hz);
        const bool written = started && wav_.wait();
        const bool closed = file_.close();
        delivered_ = written && closed && !stop_requested_;
        return started;
    }

    bool wait() override { return delivered_; }
    void stop() override { stop_requested_ = true; }

private:
    wav::FileByteSink file_;
    WavOutput wav_;
    bool started_ = false;
    bool stop_requested_ = false;
    bool delivered_ = false;
};

class WavFileInput final : public InputDevice {
public:
    bool open(const std::string& path, std::string& error) {
        if (!file_.open(path)) {
            error = "cannot open " + path;
            return false;
        }
        if (!reader_.open(file_)) {
            error = path + ": not a supported WAV file";
            return false;
        }
        return true;
    }

    bool start(SampleSink& sink, uint32_t sample_rate_hz) override {
        if (started_ || sample_rate_hz != this->sample_rate_hz()) return false;
        started_ = true;
        int16_t buffer[k_device_chunk_samples];
        size_t count = 0;
        while (!stop_requested_ && (count = reader_.read(buffer, k_device_chunk_samples)) > 0)
            sink.write(buffer, count);
        file_.close();
        return true;
    }

    bool wait() override { return started_; }
    void stop() override { stop_requested_ = true; }
    uint32_t sample_rate_hz() const override { return reader_.format().sample_rate_hz; }

private:
    wav::FileByteSource file_;
    WavReader reader_;
    bool started_ = false;
    bool stop_requested_ = false;
};

}  // namespace

std::unique_ptr<OutputDevice> open_output(const std::string& spec, std::string& error) {
    if (spec == k_null_spec) return std::unique_ptr<OutputDevice>(new NullOutput());
    std::string path;
    if (!wav_path(spec, path)) {
        error = unknown_spec(spec);
        return std::unique_ptr<OutputDevice>();
    }
    if (path.empty()) {
        error = "wav: needs a file name";
        return std::unique_ptr<OutputDevice>();
    }
    std::unique_ptr<WavFileOutput> device(new WavFileOutput());
    if (!device->open(path)) {
        error = "cannot create " + path;
        return std::unique_ptr<OutputDevice>();
    }
    return std::unique_ptr<OutputDevice>(device.release());
}

std::unique_ptr<InputDevice> open_input(const std::string& spec, std::string& error) {
    if (spec == k_null_spec) return std::unique_ptr<InputDevice>(new NullInput());
    std::string path;
    if (!wav_path(spec, path)) {
        error = unknown_spec(spec);
        return std::unique_ptr<InputDevice>();
    }
    if (path.empty()) {
        error = "wav: needs a file name";
        return std::unique_ptr<InputDevice>();
    }
    std::unique_ptr<WavFileInput> device(new WavFileInput());
    if (!device->open(path, error)) return std::unique_ptr<InputDevice>();
    return std::unique_ptr<InputDevice>(device.release());
}

// ---------------------------------------------------------------------------
// Memory devices
// ---------------------------------------------------------------------------

MemoryOutput::MemoryOutput() : sample_rate_hz_(0), stop_requested_(false), delivered_(false) {}

bool MemoryOutput::start(SampleSource& source, uint32_t sample_rate_hz) {
    if (sample_rate_hz == 0) return false;
    sample_rate_hz_ = sample_rate_hz;
    samples_.clear();
    int16_t buffer[k_device_chunk_samples];
    size_t count = 0;
    while (!stop_requested_ && (count = source.read(buffer, k_device_chunk_samples)) > 0)
        samples_.insert(samples_.end(), buffer, buffer + count);
    delivered_ = !stop_requested_;
    return true;
}

bool MemoryOutput::wait() {
    return delivered_;
}

void MemoryOutput::stop() {
    stop_requested_ = true;
}

const std::vector<int16_t>& MemoryOutput::samples() const {
    return samples_;
}

uint32_t MemoryOutput::sample_rate_hz() const {
    return sample_rate_hz_;
}

MemoryInput::MemoryInput(const std::vector<int16_t>& samples, uint32_t sample_rate_hz, size_t chunk_samples)
    : samples_(samples),
      sample_rate_hz_(sample_rate_hz),
      chunk_samples_(std::max<size_t>(1, chunk_samples)),
      stop_requested_(false),
      delivered_(false) {}

bool MemoryInput::start(SampleSink& sink, uint32_t sample_rate_hz) {
    if (sample_rate_hz != sample_rate_hz_) return false;
    for (size_t pos = 0; pos < samples_.size() && !stop_requested_; pos += chunk_samples_)
        sink.write(&samples_[pos], std::min(chunk_samples_, samples_.size() - pos));
    delivered_ = true;
    return true;
}

bool MemoryInput::wait() {
    return delivered_;
}

void MemoryInput::stop() {
    stop_requested_ = true;
}

uint32_t MemoryInput::sample_rate_hz() const {
    return sample_rate_hz_;
}

MemorySource::MemorySource(const std::vector<int16_t>& samples) : samples_(samples), position_(0) {}

size_t MemorySource::read(int16_t* out, size_t count) {
    const size_t n = std::min(count, samples_.size() - position_);
    std::copy(samples_.begin() + position_, samples_.begin() + position_ + n, out);
    position_ += n;
    return n;
}

// ---------------------------------------------------------------------------
// ResamplingSink
// ---------------------------------------------------------------------------

ResamplingSink::ResamplingSink(SampleSink& sink, double from_hz, double to_hz)
    : sink_(sink), copy_(from_hz == to_hz), resampler_(from_hz, to_hz) {}

void ResamplingSink::write(const int16_t* in, size_t count) {
    if (copy_) {
        sink_.write(in, count);
        return;
    }
    input_.assign(in, in + count);
    output_.clear();
    resampler_.process(input_.data(), input_.size(), output_);
    deliver();
}

void ResamplingSink::flush() {
    if (copy_) return;
    output_.clear();
    resampler_.flush(output_);
    deliver();
}

void ResamplingSink::deliver() {
    converted_.resize(output_.size());
    for (size_t i = 0; i < output_.size(); ++i) converted_[i] = to_int16(output_[i]);
    if (!converted_.empty()) sink_.write(&converted_[0], converted_.size());
}

}  // namespace pc
}  // namespace unlimited
