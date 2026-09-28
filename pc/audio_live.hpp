#pragma once

#include "audio.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The real-time side of the live devices (spec 12.5), shared by the CoreAudio and ALSA backends and by the tests'
// stand-in backends. A device context (a real-time callback, or an ALSA thread blocked in snd_pcm_readi or
// snd_pcm_writei) only copies samples and posts a wake-up; a normal worker thread of the device calls the sink and
// turns the wake-ups into condition-variable notifications, so wait() and drain() are plain waits. No polling.
//
//   capture:  device context --deliver()--> SampleRing --> worker thread --> sink.write()
//                             `--post()--> Wakeup ------^
//   playback: device context --fill()--> source.read() (in place, silence after its end)
//                             `--post() on the end, a failure or stop()--> service thread --> drain(), wait()
namespace unlimited {
namespace pc {

// Rates a device offering a continuous range is listed with.
const std::uint32_t k_standard_rates_hz[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};

const std::size_t k_live_chunk_samples = 1024;  // samples per sink.write() of a live input, per source.read() of an output
const std::uint32_t k_input_ring_seconds = 2;   // how far a sink may fall behind before audio is dropped

// A wake-up carried by a byte on a pipe. post() never blocks and takes no lock, so a real-time callback or a signal
// handler may call it; one thread waits. A post made while nobody waits makes the next wait() return at once.
class Wakeup {
public:
    Wakeup();
    ~Wakeup();
    Wakeup(const Wakeup&) = delete;
    Wakeup& operator=(const Wakeup&) = delete;

    bool valid() const;
    void post();
    void wait();  // returns once a post came since the last wait()

private:
    int read_fd_;
    int write_fd_;
};

inline std::int16_t to_sample(std::int16_t x) {
    return x;
}

// Full scale 1.0 = 32768; rounded and clamped.
inline std::int16_t to_sample(float x) {
    const float scaled = x * -static_cast<float>(std::numeric_limits<std::int16_t>::min());
    const float low = std::numeric_limits<std::int16_t>::min();
    const float high = std::numeric_limits<std::int16_t>::max();
    if (!(scaled > low)) return std::numeric_limits<std::int16_t>::min();
    if (!(scaled < high)) return std::numeric_limits<std::int16_t>::max();
    return static_cast<std::int16_t>(std::lrint(scaled));
}

inline void store(std::int16_t& out, std::int16_t sample) {
    out = sample;
}

inline void store(float& out, std::int16_t sample) {
    out = static_cast<float>(sample) / -static_cast<float>(std::numeric_limits<std::int16_t>::min());
}

// Lock-free ring of samples between one producer and one consumer: release/acquire on the two counters.
class SampleRing {
public:
    explicit SampleRing(std::size_t min_capacity);  // rounded up to a power of two
    SampleRing(const SampleRing&) = delete;
    SampleRing& operator=(const SampleRing&) = delete;

    std::size_t capacity() const;

    // Producer: the first channel of `count` interleaved frames; returns the frames that fitted.
    template <typename T>
    std::size_t push(const T* frames, std::size_t count, std::size_t channels) {
        const std::size_t written = written_.load(std::memory_order_relaxed);
        const std::size_t read = read_.load(std::memory_order_acquire);
        const std::size_t taken = std::min(count, data_.size() - (written - read));
        for (std::size_t i = 0; i < taken; ++i) data_[(written + i) & mask_] = to_sample(frames[i * channels]);
        written_.store(written + taken, std::memory_order_release);
        return taken;
    }

    std::size_t pop(std::int16_t* out, std::size_t count);  // consumer

private:
    std::vector<std::int16_t> data_;
    std::size_t mask_;
    std::atomic<std::size_t> written_;
    std::atomic<std::size_t> read_;
};

// What a device context reports besides audio: gaps, and the reason it failed.
class DeviceStatus {
public:
    explicit DeviceStatus(Wakeup& wakeup);
    DeviceStatus(const DeviceStatus&) = delete;
    DeviceStatus& operator=(const DeviceStatus&) = delete;

    void xrun();                        // real-time safe
    void fail(const std::string& why);  // not from a real-time callback; the first reason is kept
    std::uint32_t xruns() const;
    bool failed() const;
    std::string failure() const;

protected:
    Wakeup& wakeup_;

private:
    std::atomic<std::uint32_t> xruns_;
    std::atomic<bool> failed_;
    mutable std::mutex mutex_;
    std::string failure_;
};

// What a capture backend's device context delivers to: the first channel of each block goes into the ring (what does
// not fit is dropped and counted as an xrun), then the worker is woken.
class InputFeed : public DeviceStatus {
public:
    InputFeed(std::size_t ring_samples, Wakeup& wakeup);

    template <typename T>
    void deliver(const T* frames, std::size_t count, std::size_t channels) {
        if (ring_.push(frames, count, channels) < count) xrun();
        wakeup_.post();
    }

    std::size_t take(std::int16_t* out, std::size_t count);  // the worker

private:
    SampleRing ring_;
};

// What a playback backend's device context pulls from: fill() reads the source in place and writes each sample to
// every channel, silence where the source returned less than asked. The first short read after audio marks the end
// of the source (counted in ends()) and wakes the service thread; a full read clears it.
class OutputFeed : public DeviceStatus {
public:
    explicit OutputFeed(Wakeup& wakeup);

    void attach(SampleSource& source);  // before the backend starts

    template <typename T>
    void fill(T* frames, std::size_t count, std::size_t channels) {
        for (std::size_t done = 0; done < count;) {
            const std::size_t wanted = std::min(count - done, k_live_chunk_samples);
            const std::size_t got = source_ != nullptr ? std::min(wanted, source_->read(scratch_, wanted)) : 0;
            for (std::size_t i = 0; i < wanted; ++i) {
                const std::int16_t sample = i < got ? scratch_[i] : 0;
                T* frame = frames + (done + i) * channels;
                for (std::size_t c = 0; c < channels; ++c) store(frame[c], sample);
            }
            note(got < wanted);
            done += wanted;
        }
    }

    bool ended() const;
    std::uint32_t ends() const;

private:
    void note(bool short_read);

    SampleSource* source_;
    std::int16_t scratch_[k_live_chunk_samples];
    std::atomic<bool> ended_;
    std::atomic<std::uint32_t> ends_;
};

// A backend delivers or pulls audio from its own device context between start() and stop(); after stop() returns it
// no longer touches the feed. stop() must be safe after a failed or missing start().
class CaptureBackend {
public:
    virtual ~CaptureBackend() {}
    virtual bool start(InputFeed& feed, std::string& error) = 0;
    virtual void stop() = 0;
    virtual std::uint32_t sample_rate_hz() const = 0;
    virtual std::string description() const = 0;
};

class PlaybackBackend {
public:
    virtual ~PlaybackBackend() {}
    virtual bool start(OutputFeed& feed, std::string& error) = 0;
    virtual void stop() = 0;
    virtual std::uint32_t sample_rate_hz() const = 0;
    virtual std::uint32_t latency_ms() const = 0;
    virtual std::string description() const = 0;
};

// A live input: the backend's device context fills the ring; the worker thread empties it into the sink. The worker
// also stops the backend, so stop() stays a flag and a post. ring_samples 0: k_input_ring_seconds at the device rate.
class LiveInput final : public InputDevice {
public:
    explicit LiveInput(std::unique_ptr<CaptureBackend> backend, std::size_t ring_samples = 0);
    ~LiveInput();

    bool start(SampleSink& sink, std::uint32_t sample_rate_hz) override;
    bool wait() override;
    void stop() override;
    std::uint32_t sample_rate_hz() const override;
    std::uint32_t xruns() const override;
    std::string error() const override;
    std::string description() const override;

private:
    void run();

    std::unique_ptr<CaptureBackend> backend_;
    Wakeup wakeup_;
    InputFeed feed_;
    SampleSink* sink_;
    std::vector<std::int16_t> chunk_;
    std::atomic<bool> stop_requested_;
    bool attempted_;
    std::mutex mutex_;
    std::condition_variable finished_changed_;
    bool started_;
    bool finished_;
    std::thread worker_;
};

// A live output: the backend's device context pulls the source through the feed; the service thread wakes drain()
// and wait() and stops the backend.
class LiveOutput final : public OutputDevice {
public:
    explicit LiveOutput(std::unique_ptr<PlaybackBackend> backend);
    ~LiveOutput();

    bool start(SampleSource& source, std::uint32_t sample_rate_hz) override;
    bool wait() override;
    void stop() override;
    bool drain() override;
    std::uint32_t sample_rate_hz() const override;
    std::uint32_t latency_ms() const override;
    std::uint32_t xruns() const override;
    std::string error() const override;
    std::string description() const override;

private:
    void serve();

    std::unique_ptr<PlaybackBackend> backend_;
    Wakeup wakeup_;
    OutputFeed feed_;
    std::atomic<bool> stop_requested_;
    bool attempted_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool started_;
    bool finished_;
    std::uint32_t end_count_;  // the feed's ends() when end_time_ was taken
    std::chrono::steady_clock::time_point end_time_;
    std::thread service_;
};

// This system's live backend: audio_coreaudio.cpp (macOS), audio_alsa.cpp (Linux) or audio_live.cpp (none).
const char* live_backend_name();  // "coreaudio", "alsa", or "" when there is none
std::vector<DeviceInfo> live_devices();
std::unique_ptr<InputDevice> open_live_input(const std::string& target, std::uint32_t rate_hz, std::string& error);
std::unique_ptr<OutputDevice> open_live_output(const std::string& target, std::uint32_t rate_hz, std::string& error);

// "the device plays at 48000 Hz, not 8000 Hz ..." for start() with another rate.
std::string rate_mismatch(const char* verb, std::uint32_t device_hz, std::uint32_t given_hz);

// Sorted, without duplicates: the rates of k_standard_rates_hz between low and high.
std::vector<std::uint32_t> standard_rates_between(double low_hz, double high_hz);

}  // namespace pc
}  // namespace unlimited
