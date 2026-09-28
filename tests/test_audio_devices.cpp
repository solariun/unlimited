#include "audio.hpp"
#include "audio_live.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Live audio devices (spec 12.5): the device specs, choosing a device, the --list-devices table, this system's list,
// and the real-time machinery of pc/audio_live.hpp driven by stand-in backends (no sound is played or recorded).

using unlimited::SampleSink;
using unlimited::SampleSource;
using unlimited::pc::CaptureBackend;
using unlimited::pc::DeviceInfo;
using unlimited::pc::DeviceKind;
using unlimited::pc::DeviceSpec;
using unlimited::pc::Direction;
using unlimited::pc::InputFeed;
using unlimited::pc::LiveInput;
using unlimited::pc::LiveOutput;
using unlimited::pc::MemorySource;
using unlimited::pc::OutputFeed;
using unlimited::pc::PlaybackBackend;
using unlimited::pc::SampleRing;
using unlimited::pc::Wakeup;
using unlimited::pc::k_channels_unknown;
using unlimited::pc::k_device_refused;
using unlimited::pc::k_device_unmatched;

namespace {

using std::int16_t;
using std::size_t;
using std::uint32_t;
using test::count_of;

typedef std::chrono::steady_clock Clock;

const uint32_t k_rate = 48000;
const std::chrono::seconds k_patience(10);  // a stand-in's work takes milliseconds; this only stops a hung test
const int16_t k_other_channel = 0x7ABC;     // written to every channel but the first: never delivered
const unsigned char k_utf8_continuation_mask = 0xC0;
const unsigned char k_utf8_continuation = 0x80;

std::vector<int16_t> ramp(size_t count) {
    std::vector<int16_t> samples(count);
    for (size_t i = 0; i < count; ++i) samples[i] = static_cast<int16_t>(i);
    return samples;
}

DeviceInfo device(unsigned number, const std::string& name, const std::string& uid, unsigned in, unsigned out) {
    DeviceInfo info;
    info.backend = "coreaudio";
    info.number = number;
    info.name = name;
    info.uid = uid;
    info.input_channels = in;
    info.output_channels = out;
    info.rate_hz = k_rate;
    return info;
}

const char* const k_icom_a = "AppleUSBAudioEngine:Burr-Brown from TI:USB Audio CODEC:14100000:2,1";
const char* const k_icom_b = "AppleUSBAudioEngine:Burr-Brown from TI:USB Audio CODEC:14200000:2,1";

// A Mac with a microphone, speakers, two Icoms (both "USB Audio CODEC") and a device whose name is part of theirs.
std::vector<DeviceInfo> bench() {
    std::vector<DeviceInfo> devices;
    devices.push_back(device(0, "MacBook Pro Microphone", "BuiltInMicrophoneDevice", 1, 0));
    devices.push_back(device(1, "MacBook Pro Speakers", "BuiltInSpeakerDevice", 0, 2));
    devices.push_back(device(2, "USB Audio CODEC", k_icom_a, 2, 2));
    devices.push_back(device(3, "USB Audio CODEC", k_icom_b, 2, 2));
    devices.push_back(device(4, "USB Audio", "usb-audio", 1, 1));
    devices[0].default_input = true;
    devices[1].default_output = true;
    return devices;
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    return lines;
}

size_t display_width(const std::string& text) {
    size_t width = 0;
    for (size_t i = 0; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & k_utf8_continuation_mask) != k_utf8_continuation) ++width;
    return width;
}

// ---------------------------------------------------------------------------
// Stand-in backends
// ---------------------------------------------------------------------------

// Delivers `samples` in blocks from a thread of its own, like a device callback, with `channels` interleaved
// channels (the first carries the samples); optionally fails at the end, or runs until stopped, or (resume) waits after
// its first block until the test lets it run on.
class FakeCapture final : public CaptureBackend {
public:
    FakeCapture(const std::vector<int16_t>& samples, size_t block, size_t channels)
        : samples_(samples), block_(block), channels_(channels), stopping_(false) {}

    ~FakeCapture() { stop(); }

    bool start(InputFeed& feed, std::string& error) override {
        if (refuse_start) {
            error = "the stand-in refused to start";
            return false;
        }
        feed_ = &feed;
        producer_ = std::thread(&FakeCapture::produce, this);
        return true;
    }

    void stop() override {
        stopping_.store(true);
        if (producer_.joinable()) producer_.join();
    }

    uint32_t sample_rate_hz() const override { return k_rate; }
    std::string description() const override { return "stand-in capture"; }

    bool refuse_start = false;
    bool endless = false;   // after the samples, silence until stopped
    std::string fail_at_end;
    std::thread::id producer_id;
    std::promise<void> produced;
    std::future<void> resume;  // valid: after the first block, wait for it (at most k_patience) before the rest

private:
    void produce() {
        producer_id = std::this_thread::get_id();
        std::vector<int16_t> frames(block_ * channels_, k_other_channel);
        for (size_t at = 0; at < samples_.size() && !stopping_.load(); at += block_) {
            const size_t count = std::min(block_, samples_.size() - at);
            for (size_t i = 0; i < count; ++i) frames[i * channels_] = samples_[at + i];
            feed_->deliver(&frames[0], count, channels_);
            if (at == 0 && resume.valid()) resume.wait_for(k_patience);
        }
        if (!fail_at_end.empty()) feed_->fail(fail_at_end);
        produced.set_value();
        while (endless && !stopping_.load()) {
            std::fill(frames.begin(), frames.end(), 0);
            feed_->deliver(&frames[0], block_, channels_);
            std::this_thread::yield();
        }
    }

    std::vector<int16_t> samples_;
    size_t block_;
    size_t channels_;
    InputFeed* feed_ = nullptr;
    std::atomic<bool> stopping_;
    std::thread producer_;
};

// Pulls `blocks` blocks from a thread of its own, like a render callback, records them, then waits to be stopped.
template <typename T>
class FakePlayback final : public PlaybackBackend {
public:
    FakePlayback(size_t blocks, size_t block, size_t channels, uint32_t latency_ms)
        : blocks_(blocks), block_(block), channels_(channels), latency_ms_(latency_ms), stopping_(false) {}

    ~FakePlayback() { stop(); }

    bool start(OutputFeed& feed, std::string& error) override {
        if (refuse_start) {
            error = "the stand-in refused to start";
            return false;
        }
        feed_ = &feed;
        consumer_ = std::thread(&FakePlayback::consume, this);
        return true;
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        stopped_.notify_all();
        if (consumer_.joinable()) consumer_.join();
    }

    uint32_t sample_rate_hz() const override { return k_rate; }
    uint32_t latency_ms() const override { return latency_ms_; }
    std::string description() const override { return "stand-in playback"; }

    bool refuse_start = false;
    std::string fail_after_blocks;
    std::vector<T> played;  // read after stop()

private:
    void consume() {
        std::vector<T> frames(block_ * channels_);
        for (size_t b = 0; b < blocks_; ++b) {
            feed_->fill(&frames[0], block_, channels_);
            played.insert(played.end(), frames.begin(), frames.end());
        }
        if (!fail_after_blocks.empty()) feed_->fail(fail_after_blocks);
        std::unique_lock<std::mutex> lock(mutex_);
        stopped_.wait(lock, [this] { return stopping_; });
    }

    size_t blocks_;
    size_t block_;
    size_t channels_;
    uint32_t latency_ms_;
    OutputFeed* feed_ = nullptr;
    std::mutex mutex_;
    std::condition_variable stopped_;
    bool stopping_;
    std::thread consumer_;
};

// Records what a live input delivers, and on which threads.
class RecordingSink final : public SampleSink {
public:
    void write(const int16_t* in, size_t count) override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            samples.insert(samples.end(), in, in + count);
            threads.insert(std::this_thread::get_id());
            ++writes;
            if (hold) {
                stalled_ = true;
                changed_.notify_all();
                gate_.wait(lock, [this] { return !hold; });
            }
        }
        changed_.notify_all();
        if (device != nullptr && writes == stop_at_write) device->stop();
    }

    bool wait_for(size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, k_patience, [&] { return samples.size() >= count; });
    }

    // Blocks until a write() is held (hold): the worker has taken audio from the ring and is stuck in the sink.
    bool wait_stalled() {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, k_patience, [this] { return stalled_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hold = false;
        }
        gate_.notify_all();
    }

    size_t received() {
        std::lock_guard<std::mutex> lock(mutex_);
        return samples.size();
    }

    std::vector<int16_t> samples;
    std::set<std::thread::id> threads;
    size_t writes = 0;
    bool hold = false;  // the first write blocks until release(): a sink that falls behind
    unlimited::AudioInput* device = nullptr;
    size_t stop_at_write = 0;

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::condition_variable gate_;
    bool stalled_ = false;
};

// A source that never ends.
class EndlessSource final : public SampleSource {
public:
    size_t read(int16_t* out, size_t count) override {
        std::fill(out, out + count, static_cast<int16_t>(1));
        return count;
    }
};

LiveInput* g_signalled_input = nullptr;

void stop_on_signal(int) {
    g_signalled_input->stop();
}

}  // namespace

// ---------------------------------------------------------------------------
// Specs, choice, table
// ---------------------------------------------------------------------------

TEST(audio_device_spec_forms) {
    struct Case {
        const char* spec;
        DeviceKind kind;
        const char* backend;
        const char* target;
    };
    const std::string here = unlimited::pc::live_backend_name();
    const Case cases[] = {{"null", DeviceKind::null, "", ""},
                          {"wav:out.wav", DeviceKind::wav, "", "out.wav"},
                          {"wav:", DeviceKind::wav, "", ""},
                          {"take.WAV", DeviceKind::wav, "", "take.WAV"},
                          {"coreaudio:3", DeviceKind::live, "coreaudio", "3"},
                          {"coreaudio:USB Audio CODEC", DeviceKind::live, "coreaudio", "USB Audio CODEC"},
                          {"coreaudio:", DeviceKind::live, "coreaudio", ""},
                          {"alsa:plughw:CARD=CODEC,DEV=0", DeviceKind::live, "alsa", "plughw:CARD=CODEC,DEV=0"},
                          {"alsa:hw:1,0", DeviceKind::live, "alsa", "hw:1,0"},
                          {"default", DeviceKind::live, here.c_str(), "default"},
                          {"3", DeviceKind::unknown, "", ""},
                          {"CODEC", DeviceKind::unknown, "", ""},
                          {"pulse:x", DeviceKind::unknown, "", ""},
                          {"coreaudio", DeviceKind::unknown, "", ""},
                          {"", DeviceKind::unknown, "", ""}};
    for (size_t i = 0; i < count_of(cases); ++i) {
        const DeviceSpec spec = unlimited::pc::parse_device_spec(cases[i].spec);
        if (!CHECK(spec.kind == cases[i].kind)) NOTE("'%s'", cases[i].spec);
        CHECK_EQ(spec.backend, std::string(cases[i].backend));
        CHECK_EQ(spec.target, std::string(cases[i].target));
    }
}

TEST(audio_device_choice_by_number_uid_and_name) {
    const std::vector<DeviceInfo> devices = bench();
    std::string error;
    CHECK_EQ(unlimited::pc::select_device(devices, "default", Direction::input, error), 0);
    CHECK_EQ(unlimited::pc::select_device(devices, "default", Direction::output, error), 1);
    CHECK_EQ(unlimited::pc::select_device(devices, "3", Direction::input, error), 3);
    CHECK_EQ(unlimited::pc::select_device(devices, k_icom_a, Direction::output, error), 2);
    CHECK_EQ(unlimited::pc::select_device(devices, k_icom_b, Direction::input, error), 3);
    // A part of the name picks among the devices of that direction: "MacBook" is the microphone for an input and the
    // speakers for an output; the case does not matter.
    CHECK_EQ(unlimited::pc::select_device(devices, "MacBook", Direction::input, error), 0);
    CHECK_EQ(unlimited::pc::select_device(devices, "macbook", Direction::output, error), 1);
    CHECK_EQ(unlimited::pc::select_device(devices, "speak", Direction::output, error), 1);
    // A whole name wins over the longer names it is part of.
    CHECK_EQ(unlimited::pc::select_device(devices, "usb audio", Direction::input, error), 4);
}

TEST(audio_device_choice_refuses_ambiguous_names) {
    const std::vector<DeviceInfo> devices = bench();
    const char* const targets[] = {"USB Audio CODEC", "CODEC", "codec"};
    for (size_t i = 0; i < count_of(targets); ++i) {
        std::string error;
        CHECK_EQ(unlimited::pc::select_device(devices, targets[i], Direction::input, error), k_device_refused);
        NOTE("%s", error.c_str());
        CHECK(error.find("matches 2 input devices") != std::string::npos);
        CHECK(error.find("\n  2  USB Audio CODEC  " + std::string(k_icom_a)) != std::string::npos);
        CHECK(error.find("\n  3  USB Audio CODEC  " + std::string(k_icom_b)) != std::string::npos);
        CHECK(error.find("coreaudio:2") != std::string::npos);
    }
}

TEST(audio_device_choice_errors) {
    const std::vector<DeviceInfo> devices = bench();
    std::string error;
    CHECK_EQ(unlimited::pc::select_device(devices, "1", Direction::input, error), k_device_refused);
    CHECK(error.find("MacBook Pro Speakers") != std::string::npos && error.find("no input") != std::string::npos);
    CHECK_EQ(unlimited::pc::select_device(devices, "BuiltInMicrophoneDevice", Direction::output, error),
             k_device_refused);
    CHECK(error.find("no output") != std::string::npos);
    CHECK_EQ(unlimited::pc::select_device(devices, "5", Direction::input, error), k_device_refused);
    CHECK(error.find("0..4") != std::string::npos);
    CHECK_EQ(unlimited::pc::select_device(devices, "99999999999999999999", Direction::input, error),
             k_device_refused);
    CHECK_EQ(unlimited::pc::select_device(devices, "Teams", Direction::input, error), k_device_unmatched);
    CHECK(error.find("no input device matches 'Teams'") != std::string::npos);
    // A name only the other direction has: said so, with the devices it names.
    CHECK_EQ(unlimited::pc::select_device(devices, "Speakers", Direction::input, error), k_device_unmatched);
    CHECK(error.find("without input") != std::string::npos && error.find("1  MacBook Pro Speakers") != std::string::npos);
    std::vector<DeviceInfo> no_defaults = devices;
    no_defaults[0].default_input = false;
    CHECK_EQ(unlimited::pc::select_device(no_defaults, "default", Direction::input, error), k_device_refused);
    CHECK(error.find("no default input") != std::string::npos);
    CHECK_EQ(unlimited::pc::select_device(std::vector<DeviceInfo>(), "0", Direction::output, error), k_device_refused);
}

TEST(audio_device_table_layout) {
    std::vector<DeviceInfo> devices;
    devices.push_back(device(0, "MacBook Pro Microphone", "BuiltInMicrophoneDevice", 1, 0));
    devices.push_back(device(1, "USB Audio CODEC", "icom-705", 2, 2));
    devices[0].default_input = true;
    devices[1].default_output = true;
    const uint32_t mic_rates[] = {44100, 48000, 96000};
    const uint32_t codec_rates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000};
    devices[0].rates.assign(mic_rates, mic_rates + count_of(mic_rates));
    devices[1].rates.assign(codec_rates, codec_rates + count_of(codec_rates));
    // Two spaces before each column; numbers right-aligned; the UID last and unpadded.
    const std::string expected =
        "Audio devices (coreaudio):\n"
        "  #  Name" + std::string(20, ' ') + "In  Out  Default  Rate Hz  Rates kHz" + std::string(21, ' ') + "UID\n"
        "  0  MacBook Pro Microphone   1    -  in         48000  44.1 48 96" + std::string(20, ' ') +
        "BuiltInMicrophoneDevice\n"
        "  1  USB Audio CODEC" + std::string(10, ' ') + "2    2  out        48000  8 11.025 16 22.05 32 44.1 48  icom-705\n"
        "\n"
        "Choose with --input and --output, or -d for both: coreaudio:<#>, coreaudio:<part of the name>, "
        "coreaudio:<UID>, or default.\n"
        "A part of the name that matches several devices is refused: use the # or the UID.\n"
        "For example: --input coreaudio:0 --output coreaudio:1\n";
    const std::string table = unlimited::pc::device_table(devices);
    if (!CHECK_EQ(table, expected)) NOTE("\n%s", table.c_str());
}

TEST(audio_device_table_marks_and_widths) {
    std::vector<DeviceInfo> devices = bench();
    devices.push_back(device(5, "Gustavo\xE2\x80\x99s radio", "gr", k_channels_unknown, 0));  // U+2019, 3 bytes
    devices[5].rate_hz = 0;
    devices[5].default_input = true;
    devices[5].default_output = false;
    const std::vector<std::string> lines = lines_of(unlimited::pc::device_table(devices));
    REQUIRE(lines.size() > devices.size() + 1);
    const std::string& header = lines[1];
    const size_t uid_column = display_width(header.substr(0, header.find("UID")));
    for (size_t i = 0; i < devices.size(); ++i) {
        const std::string& row = lines[2 + i];
        const size_t at = row.rfind(devices[i].uid);
        REQUIRE(at != std::string::npos);
        if (!CHECK_EQ(display_width(row.substr(0, at)), uid_column)) NOTE("%s", row.c_str());
    }
    const std::string& radio = lines[2 + 5];
    CHECK(radio.find("  yes    -  in") != std::string::npos);  // unknown count, no output, default input
    CHECK(radio.find("  -  any  ") != std::string::npos);      // no fixed rate, any rate
    CHECK(unlimited::pc::device_table(std::vector<DeviceInfo>()).find("No audio devices found") != std::string::npos ||
          std::string(unlimited::pc::live_backend_name()).empty());
}

// ---------------------------------------------------------------------------
// This system
// ---------------------------------------------------------------------------

TEST(audio_list_devices_on_this_system) {
    const std::vector<DeviceInfo> devices = unlimited::pc::list_devices();
    NOTE("%zu device(s) (%s)", devices.size(), unlimited::pc::live_backend_name());
    const std::vector<std::string> lines = lines_of(unlimited::pc::device_table(devices));
    for (size_t i = 0; i < lines.size(); ++i) NOTE("| %s", lines[i].c_str());
    size_t default_inputs = 0;
    size_t default_outputs = 0;
    for (size_t i = 0; i < devices.size(); ++i) {
        CHECK_EQ(devices[i].number, static_cast<unsigned>(i));
        CHECK_EQ(devices[i].backend, std::string(unlimited::pc::live_backend_name()));
        CHECK(!devices[i].name.empty());
        CHECK(!devices[i].uid.empty());
        CHECK(devices[i].input_channels != 0 || devices[i].output_channels != 0);
        if (devices[i].default_input) ++default_inputs;
        if (devices[i].default_output) ++default_outputs;
        for (size_t r = 1; r < devices[i].rates.size(); ++r) CHECK(devices[i].rates[r - 1] < devices[i].rates[r]);
    }
    CHECK(default_inputs <= 1);
    CHECK(default_outputs <= 1);
}

TEST(audio_open_refuses_what_this_system_cannot_open) {
    const std::string here = unlimited::pc::live_backend_name();
    std::string error;
    const char* const elsewhere = here == "alsa" ? "coreaudio:0" : "alsa:default";
    CHECK(unlimited::pc::open_output(elsewhere, error) == nullptr);
    NOTE("%s", error.c_str());
    CHECK(error.find(here.empty() ? "no live audio" : "only") != std::string::npos);
    error.clear();
    CHECK(unlimited::pc::open_input(elsewhere, error) == nullptr);
    CHECK(!error.empty());
    if (here.empty()) return;
    error.clear();
    CHECK(unlimited::pc::open_output(here + ":", error) == nullptr);
    CHECK(error.find("needs a device") != std::string::npos);
    error.clear();
    CHECK(unlimited::pc::open_input(here + ":999", error) == nullptr);
    CHECK(error.find("999") != std::string::npos);
    error.clear();
    CHECK(unlimited::pc::open_output("3", error) == nullptr);
    CHECK(error.find(here + ":<number") != std::string::npos);
}

// Opens the default output without starting it (nothing is played): its rate, latency and description.
TEST(audio_open_default_output_without_playing) {
    std::string error;
    std::unique_ptr<unlimited::pc::OutputDevice> output = unlimited::pc::open_output("default", error);
    if (output == nullptr) {
        NOTE("no default output here: %s", error.c_str());
        return;
    }
    NOTE("%s", output->description().c_str());
    CHECK(output->sample_rate_hz() >= unlimited::pc::k_standard_rates_hz[0]);
    CHECK(!output->description().empty());
    CHECK(output->latency_ms() > 0);
    CHECK(!output->drain());  // never started
    CHECK(!output->wait());
    if (std::string(unlimited::pc::live_backend_name()) != "coreaudio") return;
    // CoreAudio runs at the nominal rate only.
    const uint32_t other = output->sample_rate_hz() == k_rate ? 44100 : k_rate;
    error.clear();
    CHECK(unlimited::pc::open_output("default", error, other) == nullptr);
    CHECK(error.find("nominal rate") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Real-time building blocks
// ---------------------------------------------------------------------------

TEST(audio_sample_conversions) {
    using unlimited::pc::to_sample;
    CHECK_EQ(to_sample(0.0f), 0);
    CHECK_EQ(to_sample(0.5f), 16384);
    CHECK_EQ(to_sample(-0.5f), -16384);
    CHECK_EQ(to_sample(-1.0f), -32768);
    CHECK_EQ(to_sample(1.0f), 32767);  // clamped: +1.0 is one step past full scale
    CHECK_EQ(to_sample(3.0f), 32767);
    CHECK_EQ(to_sample(-3.0f), -32768);
    CHECK_EQ(to_sample(100.4f / 32768.0f), 100);
    CHECK_EQ(to_sample(-100.6f / 32768.0f), -101);
    CHECK_EQ(to_sample(static_cast<int16_t>(-1234)), -1234);
    float out = 0.0f;
    unlimited::pc::store(out, static_cast<int16_t>(16384));
    CHECK_EQ(out, 0.5f);
    unlimited::pc::store(out, static_cast<int16_t>(-32768));
    CHECK_EQ(out, -1.0f);
    for (int s = -32768; s <= 32767; s += 7) {
        unlimited::pc::store(out, static_cast<int16_t>(s));
        if (to_sample(out) != s) {
            CHECK_EQ(to_sample(out), s);
            break;
        }
    }
}

TEST(audio_sample_ring_order_wrap_and_room) {
    SampleRing ring(1000);
    CHECK_EQ(ring.capacity(), static_cast<size_t>(1024));
    const std::vector<int16_t> samples = ramp(3000);
    CHECK_EQ(ring.push(&samples[0], 1000, 1), static_cast<size_t>(1000));
    CHECK_EQ(ring.push(&samples[1000], 100, 1), static_cast<size_t>(24));  // full
    std::vector<int16_t> out(2000);
    CHECK_EQ(ring.pop(&out[0], 600), static_cast<size_t>(600));
    for (size_t i = 0; i < 600; ++i) CHECK_EQ(out[i], samples[i]);
    // Across the end of the storage; only the first of two channels.
    std::vector<int16_t> stereo(2 * 500);
    for (size_t i = 0; i < 500; ++i) {
        stereo[2 * i] = samples[1024 + i];
        stereo[2 * i + 1] = k_other_channel;
    }
    CHECK_EQ(ring.push(&stereo[0], 500, 2), static_cast<size_t>(500));
    CHECK_EQ(ring.pop(&out[0], out.size()), static_cast<size_t>(924));
    for (size_t i = 0; i < 924; ++i) CHECK_EQ(out[i], samples[600 + i]);
    CHECK_EQ(ring.pop(&out[0], out.size()), static_cast<size_t>(0));
}

TEST(audio_wakeup_is_never_lost_and_never_blocks) {
    Wakeup wakeup;
    REQUIRE(wakeup.valid());
    wakeup.post();
    wakeup.wait();  // a post before the wait: returns at once
    std::atomic<bool> woken(false);
    std::thread waiter([&] {
        wakeup.wait();
        woken.store(true);
    });
    wakeup.post();
    waiter.join();
    CHECK(woken.load());
    // Far more posts than a pipe holds: none blocks.
    const size_t posts = 200000;
    const Clock::time_point start = Clock::now();
    for (size_t i = 0; i < posts; ++i) wakeup.post();
    CHECK(Clock::now() - start < k_patience);
    wakeup.wait();
}

// ---------------------------------------------------------------------------
// Live input: the device context hands off to the worker
// ---------------------------------------------------------------------------

TEST(audio_live_input_hands_audio_to_its_worker) {
    const std::vector<int16_t> samples = ramp(30000);
    FakeCapture* capture = new FakeCapture(samples, 480, 2);
    std::future<void> produced = capture->produced.get_future();
    LiveInput input((std::unique_ptr<CaptureBackend>(capture)));
    CHECK_EQ(input.sample_rate_hz(), k_rate);
    CHECK_EQ(input.description(), std::string("stand-in capture"));
    RecordingSink sink;
    REQUIRE(input.start(sink, k_rate));
    CHECK(!input.start(sink, k_rate));  // once
    REQUIRE(produced.wait_for(k_patience) == std::future_status::ready);
    REQUIRE(sink.wait_for(samples.size()));
    input.stop();
    CHECK(input.wait());
    CHECK(sink.samples == samples);  // in order, the first channel only
    CHECK_EQ(input.xruns(), 0u);
    CHECK(input.error().empty());
    // Every write came from the device's worker: neither the device context nor the caller.
    REQUIRE(sink.threads.size() == 1);
    CHECK(*sink.threads.begin() != capture->producer_id);
    CHECK(*sink.threads.begin() != std::this_thread::get_id());
}

TEST(audio_live_input_stops_from_its_sink) {
    FakeCapture* capture = new FakeCapture(ramp(1000), 100, 1);
    capture->endless = true;
    LiveInput input((std::unique_ptr<CaptureBackend>(capture)));
    RecordingSink sink;
    sink.device = &input;
    sink.stop_at_write = 3;
    REQUIRE(input.start(sink, k_rate));
    CHECK(input.wait());
    CHECK_EQ(sink.writes, static_cast<size_t>(3));
}

TEST(audio_live_input_stops_from_a_signal_handler) {
    FakeCapture* capture = new FakeCapture(ramp(1000), 100, 1);
    capture->endless = true;
    LiveInput input((std::unique_ptr<CaptureBackend>(capture)));
    RecordingSink sink;
    REQUIRE(input.start(sink, k_rate));
    g_signalled_input = &input;
    void (*previous)(int) = std::signal(SIGUSR1, stop_on_signal);
    std::raise(SIGUSR1);
    std::signal(SIGUSR1, previous);
    CHECK(input.wait());
    g_signalled_input = nullptr;
}

// A sink that falls behind: the device's first block reaches the sink, whose write() stalls; only then does the device
// run on (its stand-in waits for that: a device may otherwise fill the ring before the worker's first read), filling the
// ring behind the stalled write and losing the rest. After the release exactly the stalled block and one full ring
// arrive, the samples in order with none missing between; every block that found no room is one xrun.
TEST(audio_live_input_counts_what_a_slow_sink_loses) {
    const size_t ring = 2048;
    const size_t block = 256;
    const std::vector<int16_t> samples = ramp(20000);
    FakeCapture* capture = new FakeCapture(samples, block, 1);
    std::future<void> produced = capture->produced.get_future();
    LiveInput input(std::unique_ptr<CaptureBackend>(capture), ring);
    RecordingSink sink;
    sink.hold = true;  // the first write stalls: the ring fills behind it, the rest is dropped
    std::promise<void> resume;  // after the input: destroyed first, it never leaves the stand-in waiting
    capture->resume = resume.get_future();
    REQUIRE(input.start(sink, k_rate));
    CHECK(sink.wait_stalled());  // the worker took the first block and its write() holds it
    resume.set_value();          // now the device runs ahead of the stalled sink
    CHECK(produced.wait_for(k_patience) == std::future_status::ready);
    const size_t blocks = (samples.size() + block - 1) / block;
    CHECK_EQ(input.xruns(), static_cast<uint32_t>(blocks - 1 - ring / block));
    CHECK_EQ(sink.received(), block);  // the stalled write holds the first block
    sink.release();
    CHECK(sink.wait_for(block + ring));  // then the full ring, and nothing else
    input.stop();
    CHECK(input.wait());
    NOTE("%zu of %zu samples delivered (a stalled block of %zu and a full ring of %zu), %u xruns", sink.samples.size(),
         samples.size(), block, ring, static_cast<unsigned>(input.xruns()));
    CHECK(sink.samples == std::vector<int16_t>(samples.begin(), samples.begin() + block + ring));
}

TEST(audio_live_input_reports_a_device_error) {
    FakeCapture* capture = new FakeCapture(ramp(500), 100, 1);
    capture->fail_at_end = "the device was disconnected";
    LiveInput input((std::unique_ptr<CaptureBackend>(capture)));
    RecordingSink sink;
    REQUIRE(input.start(sink, k_rate));
    CHECK(!input.wait());  // ends by itself, without stop()
    CHECK_EQ(input.error(), std::string("the device was disconnected"));
    CHECK(sink.samples == ramp(500));  // what came before the error is delivered
}

TEST(audio_live_input_refuses_a_wrong_rate_or_a_failed_start) {
    LiveInput wrong_rate(std::unique_ptr<CaptureBackend>(new FakeCapture(ramp(10), 10, 1)));
    RecordingSink sink;
    CHECK(!wrong_rate.start(sink, k_rate / 2));
    CHECK(!wrong_rate.wait());
    CHECK(wrong_rate.error().find("48000 Hz, not 24000 Hz") != std::string::npos);

    FakeCapture* refusing = new FakeCapture(ramp(10), 10, 1);
    refusing->refuse_start = true;
    LiveInput failed((std::unique_ptr<CaptureBackend>(refusing)));
    CHECK(!failed.start(sink, k_rate));
    CHECK(!failed.wait());
    CHECK_EQ(failed.error(), std::string("the stand-in refused to start"));
}

// ---------------------------------------------------------------------------
// Live output: the device context pulls the source
// ---------------------------------------------------------------------------

TEST(audio_live_output_pulls_and_drains) {
    const std::vector<int16_t> samples = ramp(1000);
    const size_t block = 256;
    const size_t blocks = 8;
    const size_t channels = 2;
    const uint32_t latency_ms = 60;
    FakePlayback<int16_t>* playback = new FakePlayback<int16_t>(blocks, block, channels, latency_ms);
    LiveOutput output((std::unique_ptr<PlaybackBackend>(playback)));
    CHECK_EQ(output.sample_rate_hz(), k_rate);
    CHECK_EQ(output.latency_ms(), latency_ms);
    MemorySource source(samples);
    const Clock::time_point started = Clock::now();
    REQUIRE(output.start(source, k_rate));
    CHECK(output.drain());
    const long long waited_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    NOTE("drain() returned after %lld ms (latency %u ms)", waited_ms, latency_ms);
    CHECK(waited_ms >= latency_ms);
    output.stop();
    CHECK(output.wait());
    CHECK(!output.drain());  // stopped
    REQUIRE(playback->played.size() == blocks * block * channels);
    for (size_t frame = 0; frame < blocks * block; ++frame) {
        const int16_t expected = frame < samples.size() ? samples[frame] : 0;  // silence after the end
        for (size_t c = 0; c < channels; ++c)
            if (!CHECK_EQ(playback->played[frame * channels + c], expected)) return;
    }
}

TEST(audio_live_output_float_frames) {
    std::vector<int16_t> samples(300, static_cast<int16_t>(-16384));
    FakePlayback<float>* playback = new FakePlayback<float>(2, 256, 1, 1);
    LiveOutput output((std::unique_ptr<PlaybackBackend>(playback)));
    MemorySource source(samples);
    REQUIRE(output.start(source, k_rate));
    CHECK(output.drain());
    output.stop();
    CHECK(output.wait());
    REQUIRE(playback->played.size() == static_cast<size_t>(512));
    CHECK_EQ(playback->played[0], -0.5f);
    CHECK_EQ(playback->played[299], -0.5f);
    CHECK_EQ(playback->played[300], 0.0f);
}

TEST(audio_live_output_drain_ends_on_stop) {
    FakePlayback<int16_t>* playback = new FakePlayback<int16_t>(4, 128, 1, 1);
    LiveOutput output((std::unique_ptr<PlaybackBackend>(playback)));
    EndlessSource source;  // never runs out: drain() waits until stop()
    REQUIRE(output.start(source, k_rate));
    std::future<bool> drained = std::async(std::launch::async, [&] { return output.drain(); });
    CHECK(drained.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    output.stop();
    REQUIRE(drained.wait_for(k_patience) == std::future_status::ready);
    CHECK(!drained.get());
    CHECK(output.wait());
}

TEST(audio_live_output_reports_errors) {
    FakePlayback<int16_t>* failing = new FakePlayback<int16_t>(2, 128, 1, 1);
    failing->fail_after_blocks = "the device was disconnected";
    LiveOutput output((std::unique_ptr<PlaybackBackend>(failing)));
    EndlessSource source;
    REQUIRE(output.start(source, k_rate));
    CHECK(!output.drain());
    CHECK(!output.wait());
    CHECK_EQ(output.error(), std::string("the device was disconnected"));

    LiveOutput wrong_rate(std::unique_ptr<PlaybackBackend>(new FakePlayback<int16_t>(1, 128, 1, 1)));
    CHECK(!wrong_rate.start(source, 8000));
    CHECK(wrong_rate.error().find("plays at 48000 Hz, not 8000 Hz") != std::string::npos);
    CHECK(!wrong_rate.wait());

    FakePlayback<int16_t>* refusing = new FakePlayback<int16_t>(1, 128, 1, 1);
    refusing->refuse_start = true;
    LiveOutput refused((std::unique_ptr<PlaybackBackend>(refusing)));
    CHECK(!refused.start(source, k_rate));
    CHECK(!refused.drain());
    CHECK_EQ(refused.error(), std::string("the stand-in refused to start"));
}
