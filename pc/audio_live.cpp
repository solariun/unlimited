#include "audio_live.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace unlimited {
namespace pc {

using std::int16_t;
using std::size_t;
using std::uint32_t;

namespace {

const size_t k_wakeup_bytes = 64;  // posts consumed per wait()

size_t ring_samples_for(uint32_t rate_hz) {
    return static_cast<size_t>(std::max<uint32_t>(rate_hz, 1)) * k_input_ring_seconds;
}

}  // namespace

// ---------------------------------------------------------------------------
// Wakeup
// ---------------------------------------------------------------------------

Wakeup::Wakeup() : read_fd_(-1), write_fd_(-1) {
    int fds[2];
    if (::pipe(fds) != 0) return;
    read_fd_ = fds[0];
    write_fd_ = fds[1];
    ::fcntl(read_fd_, F_SETFD, FD_CLOEXEC);
    ::fcntl(write_fd_, F_SETFD, FD_CLOEXEC);
    ::fcntl(write_fd_, F_SETFL, ::fcntl(write_fd_, F_GETFL) | O_NONBLOCK);  // post() never blocks
}

Wakeup::~Wakeup() {
    if (read_fd_ >= 0) ::close(read_fd_);
    if (write_fd_ >= 0) ::close(write_fd_);
}

bool Wakeup::valid() const {
    return read_fd_ >= 0;
}

// write() is async-signal-safe; a full pipe already holds a pending wake-up, so a failed write loses nothing.
void Wakeup::post() {
    if (write_fd_ < 0) return;
    const char byte = 1;
    const ssize_t written = ::write(write_fd_, &byte, sizeof(byte));
    (void)written;
}

void Wakeup::wait() {
    char bytes[k_wakeup_bytes];
    while (::read(read_fd_, bytes, sizeof(bytes)) < 0 && errno == EINTR) {
    }
}

// ---------------------------------------------------------------------------
// SampleRing
// ---------------------------------------------------------------------------

SampleRing::SampleRing(size_t min_capacity) : mask_(0), written_(0), read_(0) {
    size_t capacity = 1;
    while (capacity < min_capacity) capacity <<= 1;
    data_.assign(capacity, 0);
    mask_ = capacity - 1;
}

size_t SampleRing::capacity() const {
    return data_.size();
}

size_t SampleRing::pop(int16_t* out, size_t count) {
    const size_t read = read_.load(std::memory_order_relaxed);
    const size_t written = written_.load(std::memory_order_acquire);
    const size_t taken = std::min(count, written - read);
    for (size_t i = 0; i < taken; ++i) out[i] = data_[(read + i) & mask_];
    read_.store(read + taken, std::memory_order_release);
    return taken;
}

// ---------------------------------------------------------------------------
// DeviceStatus, InputFeed, OutputFeed
// ---------------------------------------------------------------------------

DeviceStatus::DeviceStatus(Wakeup& wakeup) : wakeup_(wakeup), xruns_(0), failed_(false) {}

void DeviceStatus::xrun() {
    xruns_.fetch_add(1, std::memory_order_relaxed);
}

void DeviceStatus::fail(const std::string& why) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!failed_.load()) failure_ = why;
        failed_.store(true);
    }
    wakeup_.post();
}

uint32_t DeviceStatus::xruns() const {
    return xruns_.load(std::memory_order_relaxed);
}

bool DeviceStatus::failed() const {
    return failed_.load();
}

std::string DeviceStatus::failure() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failure_;
}

InputFeed::InputFeed(size_t ring_samples, Wakeup& wakeup) : DeviceStatus(wakeup), ring_(ring_samples) {}

size_t InputFeed::take(int16_t* out, size_t count) {
    return ring_.pop(out, count);
}

OutputFeed::OutputFeed(Wakeup& wakeup) : DeviceStatus(wakeup), source_(nullptr), scratch_(), ended_(false), ends_(0) {}

void OutputFeed::attach(SampleSource& source) {
    source_ = &source;
}

bool OutputFeed::ended() const {
    return ended_.load();
}

uint32_t OutputFeed::ends() const {
    return ends_.load();
}

void OutputFeed::note(bool short_read) {
    if (!short_read) {
        ended_.store(false);
        return;
    }
    if (ended_.exchange(true)) return;
    ends_.fetch_add(1);
    wakeup_.post();
}

std::string rate_mismatch(const char* verb, uint32_t device_hz, uint32_t given_hz) {
    return "the device " + std::string(verb) + " at " + std::to_string(device_hz) + " Hz, not " +
           std::to_string(given_hz) + " Hz: use its sample_rate_hz() and resample";
}

std::vector<uint32_t> standard_rates_between(double low_hz, double high_hz) {
    std::vector<uint32_t> rates;
    for (size_t i = 0; i < sizeof(k_standard_rates_hz) / sizeof(k_standard_rates_hz[0]); ++i)
        if (k_standard_rates_hz[i] >= low_hz && k_standard_rates_hz[i] <= high_hz) rates.push_back(k_standard_rates_hz[i]);
    return rates;
}

// ---------------------------------------------------------------------------
// LiveInput
// ---------------------------------------------------------------------------

LiveInput::LiveInput(std::unique_ptr<CaptureBackend> backend, size_t ring_samples)
    : backend_(std::move(backend)),
      feed_(ring_samples != 0 ? ring_samples : ring_samples_for(backend_->sample_rate_hz()), wakeup_),
      sink_(nullptr),
      chunk_(k_live_chunk_samples),
      stop_requested_(false),
      attempted_(false),
      started_(false),
      finished_(false) {}

LiveInput::~LiveInput() {
    stop();
    if (worker_.joinable()) worker_.join();
}

bool LiveInput::start(SampleSink& sink, uint32_t sample_rate_hz) {
    if (attempted_) return false;
    attempted_ = true;
    if (!wakeup_.valid()) {
        feed_.fail("cannot create the wake-up pipe");
        return false;
    }
    if (sample_rate_hz != backend_->sample_rate_hz()) {
        feed_.fail(rate_mismatch("delivers", backend_->sample_rate_hz(), sample_rate_hz));
        return false;
    }
    sink_ = &sink;
    // The backend starts first: the worker is what stops it, so the worker must not run before backend_->start() has
    // returned. What the device delivers meanwhile waits in the ring, its wake-ups in the pipe.
    std::string why;
    if (!backend_->start(feed_, why)) {
        feed_.fail(why);
        return false;
    }
    worker_ = std::thread(&LiveInput::run, this);
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = true;
    return true;
}

// Delivers everything the ring holds, then sleeps until the device context posts again.
void LiveInput::run() {
    for (;;) {
        size_t count = 0;
        while (!stop_requested_.load() && (count = feed_.take(&chunk_[0], chunk_.size())) > 0)
            sink_->write(&chunk_[0], count);
        if (stop_requested_.load() || feed_.failed()) break;
        wakeup_.wait();
    }
    backend_->stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    finished_changed_.notify_all();
}

bool LiveInput::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_) return false;
    finished_changed_.wait(lock, [this] { return finished_; });
    return !feed_.failed();
}

void LiveInput::stop() {
    stop_requested_.store(true);
    wakeup_.post();
}

uint32_t LiveInput::sample_rate_hz() const {
    return backend_->sample_rate_hz();
}

uint32_t LiveInput::xruns() const {
    return feed_.xruns();
}

std::string LiveInput::error() const {
    return feed_.failure();
}

std::string LiveInput::description() const {
    return backend_->description();
}

// ---------------------------------------------------------------------------
// LiveOutput
// ---------------------------------------------------------------------------

LiveOutput::LiveOutput(std::unique_ptr<PlaybackBackend> backend)
    : backend_(std::move(backend)),
      feed_(wakeup_),
      stop_requested_(false),
      attempted_(false),
      started_(false),
      finished_(false),
      end_count_(0) {}

LiveOutput::~LiveOutput() {
    stop();
    if (service_.joinable()) service_.join();
}

bool LiveOutput::start(SampleSource& source, uint32_t sample_rate_hz) {
    if (attempted_) return false;
    attempted_ = true;
    if (!wakeup_.valid()) {
        feed_.fail("cannot create the wake-up pipe");
        return false;
    }
    if (sample_rate_hz != backend_->sample_rate_hz()) {
        feed_.fail(rate_mismatch("plays", backend_->sample_rate_hz(), sample_rate_hz));
        return false;
    }
    feed_.attach(source);
    std::string why;
    if (!backend_->start(feed_, why)) {  // before the service thread, which stops it (see LiveInput::start)
        feed_.fail(why);
        return false;
    }
    service_ = std::thread(&LiveOutput::serve, this);
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = true;
    return true;
}

// Takes the time of each end of the source, wakes drain() and wait(); on stop() or a failure, stops the backend.
void LiveOutput::serve() {
    for (;;) {
        wakeup_.wait();
        const bool ending = stop_requested_.load() || feed_.failed();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const uint32_t ends = feed_.ends();
            if (ends != end_count_) {
                end_count_ = ends;
                end_time_ = std::chrono::steady_clock::now();
            }
        }
        changed_.notify_all();
        if (ending) break;
    }
    backend_->stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    changed_.notify_all();
}

bool LiveOutput::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_) return false;
    changed_.wait(lock, [this] { return finished_; });
    return !feed_.failed();
}

bool LiveOutput::drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_) return false;
    for (;;) {
        if (finished_ || stop_requested_.load() || feed_.failed()) return false;
        if (feed_.ended() && feed_.ends() == end_count_) {
            const std::chrono::steady_clock::time_point left = end_time_ + std::chrono::milliseconds(latency_ms());
            if (std::chrono::steady_clock::now() >= left) return true;
            changed_.wait_until(lock, left);
        } else {
            changed_.wait(lock);
        }
    }
}

void LiveOutput::stop() {
    stop_requested_.store(true);
    wakeup_.post();
}

uint32_t LiveOutput::sample_rate_hz() const {
    return backend_->sample_rate_hz();
}

uint32_t LiveOutput::latency_ms() const {
    return backend_->latency_ms();
}

uint32_t LiveOutput::xruns() const {
    return feed_.xruns();
}

std::string LiveOutput::error() const {
    return feed_.failure();
}

std::string LiveOutput::description() const {
    return backend_->description();
}

// ---------------------------------------------------------------------------
// No live backend (neither macOS nor Linux): files and null only.
// ---------------------------------------------------------------------------

#if !defined(__APPLE__) && !defined(__linux__)

const char* live_backend_name() {
    return "";
}

std::vector<DeviceInfo> live_devices() {
    return std::vector<DeviceInfo>();
}

std::unique_ptr<InputDevice> open_live_input(const std::string&, uint32_t, std::string& error) {
    error = "this system has no live audio backend (wav:<path> and null only)";
    return std::unique_ptr<InputDevice>();
}

std::unique_ptr<OutputDevice> open_live_output(const std::string&, uint32_t, std::string& error) {
    error = "this system has no live audio backend (wav:<path> and null only)";
    return std::unique_ptr<OutputDevice>();
}

#endif

}  // namespace pc
}  // namespace unlimited
