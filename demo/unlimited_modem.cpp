// unlimited_modem: a KISS modem for radio audio (spec 12.1-12.3). The portable core (unlimited/modem.hpp) does the
// work; this program gives it a sound card, a PTT, and a pseudo-terminal (or a serial port) for the computer.
//
//   computer side (main thread): select() on the PTY or serial port and a wake pipe, no timeout
//   control side (a thread):     tick() and PTT, asleep until next_tick_ms() or a wake-up
//   receiving side:              the input device's worker thread -> resampled to 8 kHz -> audio_input()
//   playing side:                the output device's real-time callback -> audio_output() -> resampled to the device
//
// Nothing polls: each side sleeps until an event (a byte, a wake-up posted by another side, the core's next timer, the
// audio device) and a signal (SIGINT, SIGTERM, SIGHUP) posts the main thread's wake pipe.
#include "audio.hpp"
#include "channel.hpp"
#include "cli.hpp"
#include "kiss_port.hpp"
#include "modem_cli.hpp"
#include "modem_link.hpp"
#include "ptt.hpp"
#include "radio_options.hpp"
#include "resampling_source.hpp"
#include "terminal.hpp"
#include "tui.hpp"
#include "unlimited/modem.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/select.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using std::int16_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint8_t;
using unlimited::ChannelState;
using unlimited::Event;
using unlimited::EventType;
using unlimited::KissDecoder;
using unlimited::KissStep;
using unlimited::Modem;
using unlimited::ModemConfig;
using unlimited::ModemCounters;
using unlimited::ModemWake;
using unlimited::SampleSink;
using unlimited::SampleSource;

namespace cli = unlimited::cli;
namespace mc = unlimited::modem_cli;
namespace pc = unlimited::pc;
namespace sim = unlimited::sim;

typedef std::vector<uint8_t> Bytes;

const size_t k_read_bytes = 4096;             // read from the computer at a time
const size_t k_to_host_bytes = 65536;         // held for a computer that does not read; beyond, dropped and counted
const uint32_t k_silence_warning_s = 3;       // a live input of only zeros this long: say why it may be
const unsigned k_test_ptt_times = 3;
const int k_test_ptt_ms = 1000;               // on, then off
const size_t k_paced_samples = 160;           // file and null devices run in real time, 20 ms at a time
const size_t k_wake_bytes = 64;
const size_t k_debug_hex_bytes = 32;
const double k_ms_per_s_f = 1000.0;
const uint64_t k_us_per_s = 1000000;
// --debug levels (spec 12.3).
const unsigned k_debug_ptt = 1;      // PTT, the channel's state, DCD, receptions, each frame's layout
const unsigned k_debug_traffic = 2;  // the computer's bytes, the send queue, the draws
const unsigned k_debug_events = 3;   // every receiver event
const int k_no_timeout = -1;
const double k_loopback_offset_hz = 40.0;     // --loopback with an SNR: the receivers mistuned by 40 Hz
const uint32_t k_loopback_seed = 1;
const uint32_t k_loopback_max_ms = 3600000;
const char* const k_loopback_text = "Unlimited loopback test 1234567890";
// The loopback's frames: an AX.25 UI frame, one with KISS's special bytes, a key alone (1 byte: dropped by the default
// --min-frame, V23), and the answer; all but the key are at least the shortest AX.25 frame (15 bytes).
const char* const k_loopback_reply = "reply from B, over";
const uint8_t k_loopback_escapes[] = {0xC0, 0xDB, 0xDC, 0xDD, 0x00, 0xFF, 'K', 'I', 'S', 'S', ' ',
                                      'e',  's',  'c',  'a',  'p',  'e', 's'};
const char* const k_default_loopback_call = "N0CALL";
const char* const k_rule = "======================================================================";
const char* const k_thin_rule = "----------------------------------------------------------------------";
const int k_db_decimals = 1;
const int k_hz_decimals = 1;
const size_t k_monitor_indent = 19;       // under the text after the time stamp
const size_t k_held_lines = 1000;         // lines kept while the --tui view owns the screen (the latest ones)
const int k_whole = 0;
const size_t k_continuation_indent = 4;   // a frame's second and later lines, after the time stamp
const double k_us_per_s_f = 1e6;

volatile sig_atomic_t g_stop = 0;
int g_signal_fd = -1;  // the main thread's wake pipe

void on_signal(int) {
    g_stop = 1;
    if (g_signal_fd < 0) return;
    const char byte = 1;
    const ssize_t written = ::write(g_signal_fd, &byte, sizeof(byte));
    (void)written;
}

// A wake-up carried by a byte on a pipe: post() is one non-blocking write(), so a real-time callback or a signal
// handler may call it (a full pipe already holds a wake-up); wait() sleeps in poll() until a post or a timeout.
class WakePipe {
public:
    WakePipe() : read_fd_(-1), write_fd_(-1) {
        int fds[2];
        if (::pipe(fds) != 0) return;
        read_fd_ = fds[0];
        write_fd_ = fds[1];
        for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); ++i) {
            ::fcntl(fds[i], F_SETFL, ::fcntl(fds[i], F_GETFL) | O_NONBLOCK);
            ::fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        }
    }

    ~WakePipe() {
        if (read_fd_ >= 0) ::close(read_fd_);
        if (write_fd_ >= 0) ::close(write_fd_);
    }

    WakePipe(const WakePipe&) = delete;
    WakePipe& operator=(const WakePipe&) = delete;

    bool valid() const { return read_fd_ >= 0; }
    int fd() const { return read_fd_; }
    int write_fd() const { return write_fd_; }

    void post() const {
        const char byte = 1;
        const ssize_t written = ::write(write_fd_, &byte, sizeof(byte));
        (void)written;
    }

    void drain() const {
        char bytes[k_wake_bytes];
        while (::read(read_fd_, bytes, sizeof(bytes)) > 0) {
        }
    }

    void wait(int timeout_ms) const {
        pollfd waiting = {read_fd_, POLLIN, 0};
        ::poll(&waiting, 1, timeout_ms);  // EINTR: a signal, whose handler posted the main thread; returning is right
        drain();
    }

private:
    int read_fd_;
    int write_fd_;
};

// Lines to stdout (the banner, the monitor) and stderr (errors, --debug), whole and in order from any thread. While the
// --tui view owns the screen they wait (the latest k_held_lines of them) and come out when it closes.
class Console {
public:
    explicit Console(unsigned debug) : debug_(debug), held_(false), dropped_(0) {}

    void out(const std::string& line) { print(stdout, line); }
    void err(const std::string& line) { print(stderr, line); }
    bool debugging(unsigned level) const { return debug_ >= level; }

    void trace(unsigned level, const std::string& text) {
        if (debug_ >= level) err("[" + mc::clock_text() + "] " + text);
    }

    void hold(bool held) {
        std::lock_guard<std::mutex> lock(mutex_);
        held_ = held;
        if (held_) return;
        if (dropped_ > 0) std::fprintf(stdout, "(%zu earlier lines were not kept while the view was open)\n", dropped_);
        for (size_t i = 0; i < lines_.size(); ++i) std::fputs((lines_[i].second + "\n").c_str(), lines_[i].first);
        lines_.clear();
        dropped_ = 0;
        std::fflush(stdout);
        std::fflush(stderr);
    }

private:
    void print(std::FILE* stream, const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (held_) {
            if (lines_.size() == k_held_lines) {
                lines_.pop_front();
                ++dropped_;
            }
            lines_.push_back(std::make_pair(stream, line));
            return;
        }
        std::fputs((line + "\n").c_str(), stream);
        std::fflush(stream);
    }

    std::mutex mutex_;
    unsigned debug_;
    bool held_;
    std::deque<std::pair<std::FILE*, std::string> > lines_;
    size_t dropped_;
};

// Bytes for the computer, from the receiving side to the computer side: one producer, one consumer, no lock.
class ByteRing {
public:
    explicit ByteRing(size_t capacity) : data_(capacity), written_(0), read_(0) {}

    size_t push(const uint8_t* data, size_t size) {
        const size_t written = written_.load(std::memory_order_relaxed);
        const size_t read = read_.load(std::memory_order_acquire);
        const size_t count = std::min(size, data_.size() - (written - read));
        for (size_t i = 0; i < count; ++i) data_[(written + i) % data_.size()] = data[i];
        written_.store(written + count, std::memory_order_release);
        return count;
    }

    // The bytes waiting, as one contiguous piece (up to the end of the buffer).
    size_t peek(const uint8_t*& data) const {
        const size_t read = read_.load(std::memory_order_relaxed);
        const size_t written = written_.load(std::memory_order_acquire);
        const size_t start = read % data_.size();
        data = &data_[start];
        return std::min(written - read, data_.size() - start);
    }

    void pop(size_t count) { read_.store(read_.load(std::memory_order_relaxed) + count, std::memory_order_release); }

    bool empty() const { return written_.load(std::memory_order_acquire) == read_.load(std::memory_order_relaxed); }

private:
    std::vector<uint8_t> data_;
    std::atomic<size_t> written_;
    std::atomic<size_t> read_;
};

std::string hex_head(const uint8_t* data, size_t size) {
    std::string text;
    for (size_t i = 0; i < size && i < k_debug_hex_bytes; ++i) {
        char byte[4];
        std::snprintf(byte, sizeof(byte), " %02X", static_cast<unsigned>(data[i]));
        text += byte;
    }
    return text + (size > k_debug_hex_bytes ? " ..." : "");
}

std::string count_text(size_t count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// --monitor: each frame sent (when its PTT is keyed) and received (when its transmission ends). With --debug 1 each
// frame sent also shows its layout on the air: the lead (TX delay or VOX lead), the windows, the tail.
class Monitor {
public:
    Monitor(Console& console, const std::string& ptt, const std::string& lead, const std::string& tail,
            const unlimited::EncoderConfig& signal, uint32_t latency_ms, unsigned min_frame)
        : console_(console), ptt_(ptt), lead_(lead), tail_(tail), signal_(signal), latency_ms_(latency_ms), owed_(0),
          open_(false), min_frame_(min_frame) {}

    // Computer side: the bytes the modem took, decoded as it decodes them, so each frame here is one transmission.
    void on_computer(const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            uint8_t value = 0;
            const KissStep step = kiss_.feed(data[i], value);
            if (step == KissStep::data) frame_.push_back(value);
            if (step == KissStep::end) {
                std::unique_lock<std::mutex> lock(mutex_);
                if (owed_ > 0) {
                    --owed_;
                    lock.unlock();
                    print_sent(frame_);
                } else {
                    queued_.push_back(frame_);
                }
                frame_.clear();
            }
            if (!kiss_.in_data()) frame_.clear();
        }
    }

    // Control side: PTT keyed for the next frame (printed when the computer side has all of it).
    void on_key() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queued_.empty()) {
            ++owed_;
            return;
        }
        const Bytes frame = queued_.front();
        queued_.pop_front();
        lock.unlock();
        print_sent(frame);
    }

    // Receiving side.
    void on_event(const Event& event) {
        switch (event.type) {
            case EventType::locked:
                open_ = true;
                received_.clear();
                dropped_ = 0;
                last_ = event;
                break;
            case EventType::byte:
                if (!open_) break;
                received_.push_back(event.value);
                last_ = event;
                break;
            case EventType::slot:
                if (open_ && (event.flags & unlimited::event_flag_framing) != 0 &&
                    event.slot == unlimited::k_stop_slot - 1)
                    ++dropped_;
                break;
            case EventType::end:
            case EventType::lost:
                if (!open_) break;
                open_ = false;
                print_received(event.type == EventType::lost);
                break;
            case EventType::state:
                break;
        }
    }

private:
    void print(const std::string& head, const Bytes& frame) {
        const std::vector<std::string> lines = mc::frame_lines(frame);
        const std::string stamp = "[" + mc::clock_text() + "] ";
        console_.out(stamp + head + ": " + lines[0]);
        for (size_t i = 1; i < lines.size(); ++i)
            console_.out(std::string(stamp.size() + k_continuation_indent, ' ') + lines[i]);
    }

    void print_sent(const Bytes& frame) {
        print("TX " + count_text(frame.size(), "byte", "bytes") + ", PTT " + ptt_, frame);
        if (!console_.debugging(k_debug_ptt)) return;
        const double windows_s = frame.size() * unlimited::k_window_slots * (signal_.slot_us / k_us_per_s_f);
        const double keyed_s = unlimited::Encoder(signal_).duration_samples(frame.size()) /
                                   static_cast<double>(unlimited::k_modem_rate_hz) +
                               latency_ms_ / k_ms_per_s_f;
        console_.out(std::string(k_monitor_indent, ' ') + lead_ + "; " + count_text(frame.size(), "window", "windows") +
                     " " + cli::trimmed(windows_s, 3) + " s; " + tail_ + "; keyed " + cli::trimmed(keyed_s, 3) +
                     " s with the " + std::to_string(latency_ms_) + " ms output latency");
    }

    void print_received(bool lost) {
        const double speed = last_.slot_ms > 0.0f ? k_ms_per_s_f / (unlimited::k_window_slots * last_.slot_ms) : 0.0;
        std::string head = "RX " + count_text(received_.size(), "byte", "bytes") + ", " + cli::fixed(speed, 2) +
                           " bytes/s, pitch " + cli::fixed(last_.tone_hz, k_hz_decimals) + " Hz, SNR " +
                           cli::fixed(last_.snr_db, k_db_decimals) + " dB";
        if (dropped_ > 0) head += ", " + count_text(dropped_, "window", "windows") + " dropped";
        if (lost) head += ", lost";
        if (received_.size() < min_frame_)
            head += ", shorter than --min-frame " + std::to_string(min_frame_) + ": not passed to the computer";
        print(head, received_);
    }

    Console& console_;
    std::string ptt_;
    std::string lead_;
    std::string tail_;
    unlimited::EncoderConfig signal_;
    uint32_t latency_ms_;
    std::mutex mutex_;
    std::deque<Bytes> queued_;  // complete frames not keyed yet
    unsigned owed_;             // keys whose frame the computer side had not completed yet
    KissDecoder kiss_;          // computer side only
    Bytes frame_;
    bool open_;                 // receiving side only
    Bytes received_;
    unsigned dropped_ = 0;
    Event last_ = Event();
    unsigned min_frame_;        // receptions shorter than this never reach the computer (V23)
};

// ---------------------------------------------------------------------------
// The running modem
// ---------------------------------------------------------------------------

struct Runtime;

class ModemSource final : public SampleSource {  // the playing side, in the device's real-time callback
public:
    explicit ModemSource(Modem& modem) : modem_(modem) {}
    size_t read(int16_t* out, size_t count) override {
        modem_.audio_output(out, count);
        return count;
    }

private:
    Modem& modem_;
};

class ModemSink final : public SampleSink {  // the receiving side, on the input device's worker thread (8 kHz)
public:
    explicit ModemSink(Modem& modem) : modem_(modem) {}
    void write(const int16_t* in, size_t count) override { modem_.audio_input(in, count); }

private:
    Modem& modem_;
};

// A file or null output played in real time: the modem's 8 kHz audio, 20 ms at a time, until the stop.
class PacedSource final : public SampleSource {
public:
    PacedSource(Modem& modem, const std::atomic<bool>& stopping)
        : modem_(modem), stopping_(stopping), played_(0), start_(std::chrono::steady_clock::now()) {}

    size_t read(int16_t* out, size_t count) override {
        if (stopping_.load()) return 0;
        count = std::min(count, k_paced_samples);
        modem_.audio_output(out, count);
        played_ += count;
        std::this_thread::sleep_until(start_ + std::chrono::microseconds(played_ * k_us_per_s / unlimited::k_modem_rate_hz));
        return count;
    }

private:
    Modem& modem_;
    const std::atomic<bool>& stopping_;
    uint64_t played_;
    std::chrono::steady_clock::time_point start_;
};

// Watches the input at its own rate on the receiving side (the device's worker thread, never its real-time callback):
// measures its level, feeds and draws the --tui view (20 frames a second), and warns once when a live input stays at
// digital silence for 3 s (a microphone the system does not let the program hear gives zeros and no error: macOS
// privacy) or clips.
class InputWatch final : public SampleSink {
public:
    InputWatch(SampleSink& next, uint32_t rate_hz, Console& console, const std::string& spec, bool live, pc::Tui* tui,
               const Modem& modem, const std::string& method)
        : next_(next), rate_hz_(rate_hz), console_(console), spec_(spec), live_(live), tui_(tui), modem_(modem),
          method_(method), silence_warned_(false), clip_warned_(false) {}

    void write(const int16_t* in, size_t count) override {
        meter_.push(in, count, rate_hz_);
        if (tui_ != nullptr) tui_->push_audio(in, count, rate_hz_);
        next_.write(in, count);
        if (live_) watch();
        if (tui_ != nullptr && refresh_.due()) draw();
    }

    // The last frame, once the input has stopped (the caller's thread then owns the view).
    void last_frame() {
        if (tui_ != nullptr) draw();
    }

    const pc::LevelMeter& meter() const { return meter_; }

private:
    void draw() {
        tui_->set_level(meter_.recent());
        mc::show_modem(*tui_, modem_, method_);
        tui_->draw();
    }

    void watch() {
        const double silent = meter_.silent_seconds();
        if (silent >= k_silence_warning_s) {
            if (!silence_warned_)
                console_.err(std::string(mc::k_program) + ": warning: the input " + spec_ + " has been digital silence "
                             "(every sample 0) for " + std::to_string(k_silence_warning_s) + " s: nothing plays into it, "
                             "or the system gives this program silence (macOS: allow the terminal in System Settings > "
                             "Privacy & Security > Microphone); a radio's receiver always gives some noise");
            silence_warned_ = true;
            if (tui_ != nullptr)
                tui_->set_warning("digital silence " + cli::fixed(silent, k_whole) + " s: microphone permission?");
        } else if (tui_ != nullptr && silence_warned_) {
            tui_->set_warning("");
        }
        if (clip_warned_ || meter_.total().clips == 0) return;
        clip_warned_ = true;
        console_.err(std::string(mc::k_program) + ": warning: the input " + spec_ + " clips (samples at full scale): "
                     "turn the radio's audio output or the sound card's input level down");
    }

    SampleSink& next_;
    uint32_t rate_hz_;
    Console& console_;
    std::string spec_;
    bool live_;
    pc::Tui* tui_;
    const Modem& modem_;
    std::string method_;
    pc::LevelMeter meter_;
    pc::RefreshPacer refresh_;
    bool silence_warned_;
    bool clip_warned_;
};

struct Runtime {
    Runtime(const mc::Options& o, Console& c, bool test)
        : options(o), console(c), test_tx(test), stopping(false), to_host(k_to_host_bytes), to_host_dropped(0),
          start(std::chrono::steady_clock::now()), key_ms(0) {}

    uint32_t now_ms() const {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    }

    void fail(const std::string& why) {
        {
            std::lock_guard<std::mutex> lock(failure_mutex);
            if (failure.empty()) failure = why;
        }
        main_wake.post();
    }

    std::string failed() {
        std::lock_guard<std::mutex> lock(failure_mutex);
        return failure;
    }

    const mc::Options& options;
    Console& console;
    bool test_tx;
    WakePipe main_wake;
    WakePipe host_wake;
    WakePipe control_wake;
    std::atomic<bool> stopping;
    std::mutex failure_mutex;
    std::string failure;
    ByteRing to_host;
    std::atomic<uint32_t> to_host_dropped;
    std::chrono::steady_clock::time_point start;
    uint32_t key_ms;  // control side: when PTT was keyed

    // In the order of destruction's needs: what the device threads call is declared before the devices, so it goes
    // after them.
    std::unique_ptr<Modem> modem;
    std::unique_ptr<pc::Tui> tui;  // --tui: the receiving side's while the input runs
    std::unique_ptr<Monitor> monitor;
    std::unique_ptr<ModemSource> source;
    std::unique_ptr<pc::ResamplingSource> resampling_source;
    std::unique_ptr<PacedSource> paced_source;
    std::unique_ptr<ModemSink> sink;
    std::unique_ptr<pc::ResamplingSink> resampling_sink;
    std::unique_ptr<InputWatch> watch;
    std::unique_ptr<pc::Ptt> ptt;
    std::unique_ptr<pc::KissPort> port;
    std::unique_ptr<pc::InputDevice> input;
    std::unique_ptr<pc::OutputDevice> output;
    std::vector<std::thread> threads;
};

// Receiving side: bytes for the computer.
void on_host(const uint8_t* data, size_t size, void* context) {
    Runtime& r = *static_cast<Runtime*>(context);
    const size_t stored = r.to_host.push(data, size);
    if (stored < size) r.to_host_dropped += static_cast<uint32_t>(size - stored);
    r.host_wake.post();
}

// Control side: PTT around each transmission.
void on_ptt(bool on, void* context) {
    Runtime& r = *static_cast<Runtime*>(context);
    if (!r.ptt->key(on))
        r.fail(std::string("PTT: ") + (on ? "keying" : "releasing") + " failed (" + r.ptt->description() + ")");
    if (on) {
        r.key_ms = r.now_ms();
        r.console.trace(k_debug_ptt, "PTT on");
        if (r.monitor != nullptr) r.monitor->on_key();
    } else {
        r.console.trace(k_debug_ptt, "PTT off after " + std::to_string(r.now_ms() - r.key_ms) + " ms");
        if (r.test_tx) r.main_wake.post();
    }
}

// Any side, the audio callback included: a wake-up for the side that has work.
void on_wake(ModemWake what, void* context) {
    Runtime& r = *static_cast<Runtime*>(context);
    if (what == ModemWake::control) {
        r.control_wake.post();
    } else {
        r.host_wake.post();
    }
}

const char* event_name(EventType type) {
    switch (type) {
        case EventType::state:
            return "state";
        case EventType::locked:
            return "locked";
        case EventType::slot:
            return "slot";
        case EventType::byte:
            return "byte";
        case EventType::end:
            return "end";
        case EventType::lost:
            return "lost";
    }
    return "?";
}

// Receiving side: every receiver event, for the monitor and --debug.
void on_event(const Event& event, void* context) {
    Runtime& r = *static_cast<Runtime*>(context);
    if (r.monitor != nullptr) r.monitor->on_event(event);
    if (r.tui != nullptr) r.tui->on_event(event);
    if (!r.console.debugging(k_debug_ptt)) return;
    if (event.type == EventType::locked) {
        r.console.trace(k_debug_ptt, "receiver locked: pitch " + cli::fixed(event.tone_hz, k_hz_decimals) + " Hz, T " +
                               cli::fixed(event.slot_ms, 3) + " ms, SNR " + cli::fixed(event.snr_db, k_db_decimals) +
                               " dB");
    } else if (event.type == EventType::end || event.type == EventType::lost) {
        r.console.trace(k_debug_ptt, std::string("receiver: ") + event_name(event.type));
    } else if (event.type == EventType::byte) {
        char text[64];
        std::snprintf(text, sizeof(text), "receiver byte %u: %02X", static_cast<unsigned>(event.byte_index),
                      static_cast<unsigned>(event.value));
        r.console.trace(k_debug_events, text);
    } else if (event.type == EventType::slot) {
        r.console.trace(k_debug_events, "receiver slot " + std::to_string(event.slot) + " of window " +
                               std::to_string(event.byte_index) + ": bit " + std::to_string(event.value) + ", level " +
                               std::to_string(event.level_pct) + " %, line " + std::to_string(event.threshold_pct) +
                               " %");
    }
}

const char* state_name(ChannelState state) {
    switch (state) {
        case ChannelState::idle:
            return "idle";
        case ChannelState::waiting:
            return "waiting for the channel";
        case ChannelState::keyed:
            return "keyed";
        case ChannelState::releasing:
            return "releasing";
    }
    return "?";
}

// The control side: tick() when a wake-up comes or the core's next timer is due; nothing else wakes it.
void control_loop(Runtime& r) {
    ChannelState state = ChannelState::idle;
    bool dcd = false;
    uint32_t draws = 0;
    while (!r.stopping.load()) {
        const uint32_t now = r.now_ms();
        r.modem->tick(now);
        if (r.console.debugging(k_debug_ptt)) {
            const ChannelState now_state = r.modem->channel_state();
            if (now_state != state) r.console.trace(k_debug_ptt, std::string("channel: ") + state_name(now_state));
            state = now_state;
            if (r.modem->dcd() != dcd) r.console.trace(k_debug_ptt, r.modem->dcd() ? "DCD on" : "DCD off");
            dcd = r.modem->dcd();
            const ModemCounters counters = r.modem->counters();
            if (counters.draws != draws)
                r.console.trace(k_debug_traffic, "p-persistence: " + std::to_string(counters.draws - draws) + " draw(s), " +
                                       std::to_string(counters.deferrals) + " deferred so far");
            draws = counters.draws;
        }
        const uint32_t next = r.modem->next_tick_ms();
        int timeout = k_no_timeout;
        if (next != unlimited::k_no_tick) timeout = std::max(0, static_cast<int32_t>(next - r.now_ms()));
        r.control_wake.wait(timeout);
    }
}

// The computer side: KISS from the PTY or serial port into the modem (what it cannot take yet waits here, and the
// port is not read until the modem says there is room), and the receiver's bytes out to it.
void host_loop(Runtime& r) {
    const int port = r.port->fd();
    std::vector<uint8_t> pending(k_read_bytes);
    size_t pending_from = 0;
    size_t pending_to = 0;
    for (;;) {
        if (pending_from < pending_to) {
            const size_t taken = r.modem->host_input(&pending[pending_from], pending_to - pending_from);
            if (r.monitor != nullptr) r.monitor->on_computer(&pending[pending_from], taken);
            pending_from += taken;
            if (taken > 0 && r.console.debugging(k_debug_traffic)) {
                const ModemCounters counters = r.modem->counters();
                r.console.trace(k_debug_traffic, "send queue: " + std::to_string(counters.queued_bytes) + " bytes, " +
                                       std::to_string(counters.queued_frames) + " complete frame(s)");
            }
        }
        const bool room_awaited = pending_from < pending_to;
        fd_set reading;
        fd_set writing;
        FD_ZERO(&reading);
        FD_ZERO(&writing);
        FD_SET(r.main_wake.fd(), &reading);
        FD_SET(r.host_wake.fd(), &reading);
        if (!room_awaited) FD_SET(port, &reading);
        if (!r.to_host.empty()) FD_SET(port, &writing);
        const int highest = std::max(port, std::max(r.main_wake.fd(), r.host_wake.fd()));
        if (::select(highest + 1, &reading, &writing, nullptr, nullptr) < 0 && errno != EINTR) {
            r.fail(std::string("select: ") + std::strerror(errno));
            return;
        }
        if (g_stop != 0 || !r.failed().empty()) return;
        if (FD_ISSET(r.host_wake.fd(), &reading)) r.host_wake.drain();
        if (FD_ISSET(r.main_wake.fd(), &reading)) r.main_wake.drain();
        if (!room_awaited && FD_ISSET(port, &reading)) {
            const ssize_t got = ::read(port, &pending[0], pending.size());
            if (got > 0) {
                pending_from = 0;
                pending_to = static_cast<size_t>(got);
                r.console.trace(k_debug_traffic, "computer -> modem: " + std::to_string(got) + " bytes:" +
                                       hex_head(&pending[0], pending_to));
            } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
                r.fail(r.port->name() + (got == 0 ? ": closed" : std::string(": ") + std::strerror(errno)));
                return;
            }
        }
        if (FD_ISSET(port, &writing)) {
            const uint8_t* data = nullptr;
            const size_t size = r.to_host.peek(data);
            const ssize_t wrote = ::write(port, data, size);
            if (wrote > 0) {
                r.to_host.pop(static_cast<size_t>(wrote));
                r.console.trace(k_debug_traffic, "modem -> computer: " + std::to_string(wrote) + " bytes:" +
                                       hex_head(data, static_cast<size_t>(wrote)));
            } else if (wrote < 0 && errno != EAGAIN && errno != EINTR) {
                r.fail(r.port->name() + ": " + std::strerror(errno));
                return;
            }
        }
    }
}

// Blocks SIGINT, SIGTERM and SIGHUP in the threads started while it lives; the main thread takes them.
class SignalsBlocked {
public:
    SignalsBlocked() {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGINT);
        sigaddset(&set, SIGTERM);
        sigaddset(&set, SIGHUP);
        pthread_sigmask(SIG_BLOCK, &set, &old_);
    }
    ~SignalsBlocked() { pthread_sigmask(SIG_SETMASK, &old_, nullptr); }

private:
    sigset_t old_;
};

// A seed of its own for each run: the clock and the process number.
uint32_t run_seed() {
    const uint64_t ticks = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const uint32_t seed = static_cast<uint32_t>(ticks ^ (ticks >> 32)) ^ (static_cast<uint32_t>(::getpid()) << 16);
    return seed != 0 ? seed : 1u;
}

bool is_live(const std::string& spec) {
    return pc::parse_device_spec(spec).kind == pc::DeviceKind::live;
}

std::string device_text(const std::string& spec, const std::string& description) {
    return description.empty() || description == spec ? spec : spec + " (" + description + ")";
}

void print_banner(Runtime& r, const std::string& input_spec, const std::string& output_spec, uint32_t output_rate) {
    const mc::Options& o = r.options;
    const Modem& modem = *r.modem;
    const unlimited::EncoderConfig& signal = modem.signal();
    const unlimited::DecoderConfig& receiver = modem.config().receiver;
    Console& c = r.console;
    const std::string speed = cli::speed_number(signal.slot_us);
    c.out(k_rule);
    c.out("  unlimited_modem - KISS modem for radio audio (Unlimited v1.0)");
    c.out(k_rule);
    c.out("  Speed      : " + cli::speed_text(signal.slot_us));
    c.out("               BOTH STATIONS MUST USE --bps " + speed);
    c.out("  Signal     : pitch " + std::to_string(signal.tone_hz) + " Hz, crest " +
          cli::fixed(mc::k_amplitude_db * std::log10(signal.amplitude / mc::k_dbfs_reference), k_db_decimals) + " dBFS");
    c.out("  Bandwidth  : " + cli::bandwidth_line(signal));
    c.out("  Airtime    : " + mc::airtime_text(signal) + " (key to release)");
    c.out("  Receiver   : " + cli::threshold_text(receiver) + ", impulse blanker " +
          (receiver.impulse_blanker ? "on" : "off") + ", fade bridge " + (modem.config().fade_bridge ? "on" : "off"));
    c.out("  Min frame  : " + mc::min_frame_text(o));
    if (!r.test_tx) {
        const uint32_t input_rate = r.input->sample_rate_hz();
        c.out("  Audio in   : " + device_text(input_spec, r.input->description()) + ", " + std::to_string(input_rate) +
              " Hz" + (input_rate != unlimited::k_modem_rate_hz ? " (resampled to 8000 Hz)" : ""));
    }
    std::string out_line = "  Audio out  : " + device_text(output_spec, r.output->description()) + ", " +
                           std::to_string(output_rate) + " Hz";
    if (is_live(output_spec))
        out_line += ", PTT released " + std::to_string(modem.config().access.output_latency_ms) + " ms after the audio";
    c.out(out_line);
    c.out("  PTT        : " + r.ptt->description());
    c.out("  Lead, tail : " + mc::lead_text(o, signal) + "; " + mc::tail_text(o, signal));
    const unlimited::AccessConfig& access = modem.config().access;
    c.out("  Channel    : " + std::string(access.full_duplex ? "full duplex, no channel check"
                                                             : "half duplex, dwait " + std::to_string(access.dwait_ms) +
                                                                   " ms, persist " + std::to_string(access.persist) +
                                                                   ", slottime " + std::to_string(access.slot_time_ms) +
                                                                   " ms"));
    if (r.port != nullptr) {
        c.out(k_thin_rule);
        if (r.port->link().empty()) {
            c.out("  Serial     : " + r.port->name() + " at " + std::to_string(o.serial_baud) + " baud, 8N1");
        } else {
            c.out("  PTY device : " + r.port->name());
            c.out("  Symlink    : " + r.port->link() + " -> " + r.port->name());
            if (!r.port->replaced().empty()) c.out("               (replaced an old link to " + r.port->replaced() + ")");
            c.out("  Example    : ax25tnc -c N0CALL -r N0CALL-1 " + r.port->link());
        }
    }
    c.out(k_thin_rule);
    c.out(std::string(o.monitor ? "  Monitor on. " : "  ") + "Ctrl-C stops.");
}

// --tui: the decoder's view of what the radio hears, with the modem's items (spec 12.6). It is drawn on the receiving
// side only (the input's worker thread: never the audio callback), so it opens before the input starts and closes after
// it stopped; lines for the terminal wait meanwhile.
void open_view(Runtime& r, const std::string& input_spec, uint32_t latency_ms) {
    std::unique_ptr<pc::Tui> tui(new pc::Tui(pc::TuiMode::decoder));
    if (!tui->open()) {
        r.console.err(std::string(mc::k_program) + ": --tui needs a terminal on stdout; plain output");
        return;
    }
    const Modem& modem = *r.modem;
    const unlimited::DecoderConfig& receiver = modem.receiver();
    tui->set_label(input_spec);
    tui->set_speed(unlimited::bytes_per_second(receiver.slot_us));
    tui->set_tone_hz(modem.signal().tone_hz);
    tui->set_slot_ms(static_cast<float>(cli::slot_ms_of(receiver.slot_us)));
    tui->set_passband(receiver.passband);
    tui->set_search_range(receiver.search_range());
    mc::show_modem(*tui, modem, pc::ptt_method_name(r.options.radio.ptt.method));
    tui->set_field("threshold", receiver.decision_mode == unlimited::DecisionMode::adaptive
                                    ? std::string("auto")
                                    : std::to_string(receiver.threshold_percent) + "%");
    if (receiver.fade_bridge) tui->set_field("fade bridge", "on");
    tui->set_field("lead", mc::lead_text(r.options, modem.signal()));
    tui->set_field("latency", std::to_string(latency_ms) + " ms");
    pc::restore_terminal_on_exit(true);
    r.console.hold(true);
    r.tui.reset(tui.release());
}

// Opens everything, runs until a signal (or, --test-tx, until the frame is sent), stops cleanly. Exit code.
int run_modem(const mc::Options& o, const Bytes* test_frame) {
    Console console(o.debug);
    Runtime r(o, console, test_frame != nullptr);
    if (!r.main_wake.valid() || !r.host_wake.valid() || !r.control_wake.valid()) {
        console.err(std::string(mc::k_program) + ": cannot create the wake-up pipes");
        return mc::k_exit_failure;
    }
    g_signal_fd = r.main_wake.write_fd();
    const std::string input_spec = o.radio.input.empty() ? "default" : o.radio.input;
    const std::string output_spec = o.radio.output.empty() ? "default" : o.radio.output;
    std::string error;
    const std::string program = std::string(mc::k_program) + ": ";

    // Every device first; the computer's port and its link last (spec 12.3).
    if (!r.test_tx) {
        r.input = pc::open_input(input_spec, error, o.radio.rate_hz);
        if (r.input == nullptr) {
            console.err(program + "input " + input_spec + ": " + error);
            return mc::k_exit_failure;
        }
    }
    r.output = pc::open_output(output_spec, error, o.radio.rate_hz);
    if (r.output == nullptr) {
        console.err(program + "output " + output_spec + ": " + error);
        return mc::k_exit_failure;
    }
    r.ptt = pc::open_ptt(o.radio.ptt, error);
    if (r.ptt == nullptr) {
        console.err(program + "PTT: " + error);
        return mc::k_exit_failure;
    }
    const bool live_output = is_live(output_spec);
    const uint32_t output_rate = live_output ? r.output->sample_rate_hz() : unlimited::k_modem_rate_hz;
    uint16_t latency_ms = 0;
    if (live_output) {
        const pc::ResamplerKernel kernel(unlimited::k_modem_rate_hz, output_rate);
        const double lead_ms = output_rate == unlimited::k_modem_rate_hz
                                   ? 0.0
                                   : kernel.taps_after() * k_ms_per_s_f / unlimited::k_modem_rate_hz;
        latency_ms = static_cast<uint16_t>(r.output->latency_ms() + static_cast<uint32_t>(std::ceil(lead_ms)));
    }
    ModemConfig config = mc::modem_config(o, latency_ms);
    if (r.test_tx) config.access.full_duplex = true;  // --test-tx sends at once, as kiss_modem does
    config.access.seed = run_seed();  // two stations never draw the same p-persistence numbers
    r.modem.reset(new Modem(config, &on_host, &on_ptt, &r, &on_wake));
    r.modem->set_event_tap(&on_event, &r);
    if (o.monitor)
        r.monitor.reset(new Monitor(console, r.ptt->description(), mc::lead_text(o, r.modem->signal()),
                                    mc::tail_text(o, r.modem->signal()), r.modem->signal(), latency_ms, o.min_frame));
    if (!r.test_tx) {
        r.port = o.serial.empty() ? pc::KissPort::open_pty(o.link, error)
                                  : pc::KissPort::open_serial(o.serial, o.serial_baud, error);
        if (r.port == nullptr) {
            console.err(program + error);
            return mc::k_exit_failure;
        }
    }
    print_banner(r, input_spec, output_spec, output_rate);
    if (o.tui) open_view(r, input_spec, latency_ms);

    {
        const SignalsBlocked blocked;  // the threads started here leave the signals to the main thread
        r.source.reset(new ModemSource(*r.modem));
        if (live_output) {
            r.resampling_source.reset(
                new pc::ResamplingSource(*r.source, unlimited::k_modem_rate_hz, static_cast<double>(output_rate)));
            if (!r.output->start(*r.resampling_source, output_rate)) {
                console.err(program + "output " + output_spec + ": " + r.output->error());
                return mc::k_exit_failure;
            }
            r.threads.push_back(std::thread([&r, output_spec] {
                r.output->wait();
                if (!r.stopping.load()) r.fail("the output " + output_spec + " stopped: " + r.output->error());
            }));
        } else {
            r.paced_source.reset(new PacedSource(*r.modem, r.stopping));
            r.threads.push_back(std::thread([&r] { r.output->start(*r.paced_source, unlimited::k_modem_rate_hz); }));
        }
        if (!r.test_tx) {
            const uint32_t input_rate = r.input->sample_rate_hz();
            r.sink.reset(new ModemSink(*r.modem));
            r.resampling_sink.reset(new pc::ResamplingSink(*r.sink, input_rate));
            r.watch.reset(new InputWatch(*r.resampling_sink, input_rate, console, input_spec, is_live(input_spec),
                                         r.tui.get(), *r.modem, pc::ptt_method_name(o.radio.ptt.method)));
            const pc::DeviceKind kind = pc::parse_device_spec(input_spec).kind;
            if (kind == pc::DeviceKind::live) {
                if (!r.input->start(*r.watch, input_rate)) {
                    console.err(program + "input " + input_spec + ": " + r.input->error());
                    r.stopping = true;
                    r.output->stop();
                    for (size_t i = 0; i < r.threads.size(); ++i) r.threads[i].join();
                    return mc::k_exit_failure;
                }
                r.threads.push_back(std::thread([&r, input_spec] {
                    r.input->wait();
                    if (!r.stopping.load()) r.fail("the input " + input_spec + " stopped: " + r.input->error());
                }));
            } else if (kind == pc::DeviceKind::wav) {
                // A recording plays into the modem in real time, then silence until the stop.
                r.threads.push_back(std::thread([&r, input_rate] {
                    struct Paced final : SampleSink {
                        Paced(Runtime& runtime, uint32_t rate)
                            : r(runtime), rate_hz(rate), heard(0), start(std::chrono::steady_clock::now()) {}
                        void write(const int16_t* in, size_t count) override {
                            if (r.stopping.load()) {
                                r.input->stop();
                                return;
                            }
                            r.watch->write(in, count);
                            heard += count;
                            std::this_thread::sleep_until(start + std::chrono::microseconds(heard * k_us_per_s / rate_hz));
                        }
                        Runtime& r;
                        uint32_t rate_hz;
                        uint64_t heard;
                        std::chrono::steady_clock::time_point start;
                    } paced(r, input_rate);
                    r.input->start(paced, input_rate);
                    const std::vector<int16_t> silence(k_paced_samples * input_rate / unlimited::k_modem_rate_hz, 0);
                    while (!r.stopping.load()) paced.write(silence.data(), silence.size());
                }));
            }
        }
        r.threads.push_back(std::thread(control_loop, std::ref(r)));
    }

    if (r.test_tx) {
        const Bytes kiss = mc::kiss_frame(*test_frame);
        if (r.monitor != nullptr) r.monitor->on_computer(kiss.data(), kiss.size());
        r.modem->host_input(kiss.data(), kiss.size());
        console.out("  Sending " + count_text(test_frame->size(), "byte", "bytes") + " at once, " +
                    mc::seconds_text(unlimited::Encoder(r.modem->signal()).duration_samples(test_frame->size())) +
                    " from the key to the release");
        while (g_stop == 0 && r.failed().empty() &&
               !(r.modem->counters().transmissions > 0 && !r.modem->transmitting()))
            r.main_wake.wait(k_no_timeout);
    } else {
        host_loop(r);
    }

    // Clean stop: the control side ends, PTT is released, the devices stop, the link goes (KissPort).
    r.stopping = true;
    r.control_wake.post();
    r.output->stop();
    if (r.input != nullptr) r.input->stop();
    for (size_t i = 0; i < r.threads.size(); ++i) r.threads[i].join();
    const bool cut = r.modem->transmitting();  // a frame was on the air: its PTT goes now
    r.ptt->key(false);
    g_signal_fd = -1;
    if (r.tui != nullptr) {  // the input has stopped: this thread owns the view now
        if (r.watch != nullptr) r.watch->last_frame();
        r.tui->close();
        pc::restore_terminal_on_exit(false);
        console.hold(false);
    }

    const ModemCounters counters = r.modem->counters();
    const std::string failure = r.failed();
    if (!failure.empty()) console.err(program + failure);
    std::string summary = "  Stopped: " + count_text(counters.transmissions, "frame", "frames") + " sent, " +
                          count_text(counters.frames_received, "transmission", "transmissions") + " received";
    if (counters.short_frames > 0)
        summary += " (" + count_text(counters.short_frames, "shorter reception", "shorter receptions") +
                   " dropped: --min-frame " + std::to_string(o.min_frame) + ")";
    if (cut) summary += "; the frame on the air was cut short (PTT released)";
    if (!r.test_tx) {
        const uint32_t waiting = counters.queued_frames - (cut && counters.queued_frames > 0 ? 1u : 0u);
        if (waiting > 0)
            summary += "; " + count_text(waiting, "frame", "frames") + " waiting, not sent";
        else
            summary += "; nothing waiting";
        if (r.to_host_dropped.load() > 0)
            summary += "; " + std::to_string(r.to_host_dropped.load()) + " bytes for the computer dropped (it did not read)";
        if (counters.kiss.unknown > 0 || counters.kiss.bad_escapes > 0)
            summary += "; KISS: " + std::to_string(counters.kiss.unknown) + " unknown command(s), " +
                       std::to_string(counters.kiss.bad_escapes) + " bad escape(s)";
    }
    console.out(summary);
    return failure.empty() ? mc::k_exit_ok : mc::k_exit_failure;
}

// --test-ptt: three keyings of 1 s, 1 s apart; a signal ends it with PTT released.
int run_test_ptt(const mc::Options& o) {
    Console console(o.debug);
    WakePipe wake;
    g_signal_fd = wake.write_fd();
    std::string error;
    std::unique_ptr<pc::Ptt> ptt = pc::open_ptt(o.radio.ptt, error);
    if (ptt == nullptr) {
        console.err(std::string(mc::k_program) + ": PTT: " + error);
        return mc::k_exit_failure;
    }
    console.out("PTT test: " + ptt->description());
    if (o.vox()) console.out("  (VOX keys nothing here: the lead tone of each transmission keys the radio)");
    for (unsigned i = 1; i <= k_test_ptt_times && g_stop == 0; ++i) {
        if (!ptt->key(true)) {
            console.err(std::string(mc::k_program) + ": PTT: keying failed");
            return mc::k_exit_failure;
        }
        console.out("  [" + std::to_string(i) + "/" + std::to_string(k_test_ptt_times) + "] PTT on");
        wake.wait(k_test_ptt_ms);
        if (!ptt->key(false)) {
            console.err(std::string(mc::k_program) + ": PTT: releasing failed");
            return mc::k_exit_failure;
        }
        console.out("  [" + std::to_string(i) + "/" + std::to_string(k_test_ptt_times) + "] PTT off");
        if (i < k_test_ptt_times && g_stop == 0) wake.wait(k_test_ptt_ms);
    }
    g_signal_fd = -1;
    console.out(g_stop != 0 ? "PTT test stopped (PTT released)." : "PTT test done.");
    return mc::k_exit_ok;
}

// "A -> B frame 2, 18 bytes: byte for byte"; a frame shorter than --min-frame must not reach the computer (V23).
// `next`: the index in `got` of the next frame expected there.
std::string link_frame_line(const char* direction, size_t number, const Bytes& sent, const std::vector<Bytes>& got,
                            size_t& next, unsigned min_frame) {
    std::string line = std::string("  ") + direction + " frame " + std::to_string(number) + ", " +
                       count_text(sent.size(), "byte", "bytes") + ": ";
    if (sent.size() < min_frame) return line + "shorter than --min-frame " + std::to_string(min_frame) + ", not passed on";
    const size_t index = next++;
    const bool ok = index < got.size() && got[index] == sent;
    return line + (ok ? "byte for byte" : index < got.size() ? "DIFFERENT" : "NOT RECEIVED");
}

// The frames a computer gets of those sent: the ones at least --min-frame long (V23).
std::vector<Bytes> passed_on(const std::vector<Bytes>& sent, unsigned min_frame) {
    std::vector<Bytes> passed;
    for (size_t i = 0; i < sent.size(); ++i)
        if (sent[i].size() >= min_frame) passed.push_back(sent[i]);
    return passed;
}

// --loopback [SNR]: two modems in memory through the channel simulator (clean without an SNR).
int run_loopback(const mc::Options& o) {
    Console console(o.debug);
    const ModemConfig station = mc::modem_config(o);
    pc::LinkConfig config;
    config.stations[0] = station;
    config.stations[1] = station;
    config.stations[0].access.seed = k_loopback_seed;
    config.stations[1].access.seed = k_loopback_seed + 1;
    config.noisy = o.has_loopback_snr;
    config.channel.mode = sim::Mode::usb;
    config.channel.snr_db = o.loopback_snr_db;
    config.channel.freq_offset_hz = k_loopback_offset_hz;
    config.channel.rx_low_hz = station.receiver.passband.low_hz;
    config.channel.rx_high_hz = station.receiver.passband.high_hz;
    config.channel.seed = k_loopback_seed;
    std::unique_ptr<pc::ModemLink> link;
    try {
        link.reset(new pc::ModemLink(config));
    } catch (const std::exception& refused) {
        console.err(std::string(mc::k_program) + ": --loopback: " + refused.what());
        return mc::k_exit_failure;
    }
    const std::string call = o.callsign.empty() ? k_default_loopback_call : o.callsign;
    std::vector<Bytes> from_a;
    from_a.push_back(mc::ui_frame(call, k_loopback_text));
    from_a.push_back(Bytes(k_loopback_escapes, k_loopback_escapes + sizeof(k_loopback_escapes)));
    from_a.push_back(Bytes(1, 'X'));
    const std::vector<Bytes> from_b(1, Bytes(k_loopback_reply, k_loopback_reply + std::strlen(k_loopback_reply)));

    console.out("unlimited_modem loopback: two modems in memory, " + cli::speed_text(station.signal.slot_us));
    console.out("  channel: " + std::string(o.has_loopback_snr ? "usb, SNR " + cli::fixed(o.loopback_snr_db, 1) +
                                                                     " dB (key-down, 2500 Hz), mistuned " +
                                                                     cli::fixed(k_loopback_offset_hz, 0) + " Hz"
                                                               : "clean (--loopback SNR adds noise)"));
    console.out("  bandwidth: " + cli::bandwidth_line(station.sent()));
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < from_a.size(); ++i) link->send(0, from_a[i]);
    bool settled = link->run_until_idle(k_loopback_max_ms);
    for (size_t i = 0; i < from_b.size(); ++i) link->send(1, from_b[i]);
    settled = link->run_until_idle(k_loopback_max_ms) && settled;
    const double wall_s =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() /
        k_ms_per_s_f;

    const std::vector<Bytes>& at_b = link->frames(1);
    const std::vector<Bytes>& at_a = link->frames(0);
    size_t next_b = 0;
    size_t next_a = 0;
    for (size_t i = 0; i < from_a.size(); ++i) {
        console.out(link_frame_line("A -> B", i + 1, from_a[i], at_b, next_b, o.min_frame));
        if (o.monitor) {
            const std::vector<std::string> lines = mc::frame_lines(from_a[i]);
            for (size_t l = 0; l < lines.size(); ++l) console.out("      " + lines[l]);
        }
    }
    for (size_t i = 0; i < from_b.size(); ++i)
        console.out(link_frame_line("B -> A", i + 1, from_b[i], at_a, next_a, o.min_frame));
    const std::vector<pc::LinkReception>& heard = link->receptions(1);
    if (!heard.empty())
        console.out("  B heard A at pitch " + cli::fixed(heard[0].tone_hz, k_hz_decimals) + " Hz, SNR " +
                    cli::fixed(heard[0].snr_db, k_db_decimals) + " dB, T " + cli::fixed(heard[0].slot_ms, 3) + " ms");
    const bool pass = settled && at_b == passed_on(from_a, o.min_frame) && at_a == passed_on(from_b, o.min_frame) &&
                      link->keys(0).size() == from_a.size() && link->keys(1).size() == from_b.size();
    console.out("  " + count_text(link->keys(0).size(), "transmission", "transmissions") + " from A, " +
                count_text(link->keys(1).size(), "transmission", "transmissions") + " from B; " +
                cli::fixed(link->now_ms() / k_ms_per_s_f, 1) + " s simulated in " + cli::fixed(wall_s, 2) + " s");
    console.out(std::string("Result: ") + (pass ? "PASS" : "FAIL"));
    return pass ? mc::k_exit_ok : mc::k_exit_failure;
}

void install_signals() {
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = &on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGHUP, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
}

}  // namespace

int main(int argc, char** argv) {
    install_signals();
    mc::Options options;
    try {
        options = mc::parse_options(argc, argv);
    } catch (const cli::UsageError& error) {
        std::fprintf(stderr, "%s: %s (see --help)\n", mc::k_program, error.what());
        return mc::k_exit_usage;
    }
    if (options.help) {
        std::fputs(mc::help_text(options).c_str(), stdout);
        return mc::k_exit_ok;
    }
    if (options.radio.list_devices) {
        pc::print_devices(stdout);
        return mc::k_exit_ok;
    }
    if (options.test_ptt) return run_test_ptt(options);

    const std::string problem = mc::config_problem(mc::modem_config(options));
    if (!problem.empty()) {
        std::fprintf(stderr, "%s: refused: %s\n", mc::k_program, problem.c_str());
        return mc::k_exit_failure;
    }
    if (options.loopback) return run_loopback(options);
    if (options.has_test_tx) {
        const Bytes frame = options.callsign.empty() ? Bytes(options.test_tx.begin(), options.test_tx.end())
                                                     : mc::ui_frame(options.callsign, options.test_tx);
        return run_modem(options, &frame);
    }
    return run_modem(options, nullptr);
}
