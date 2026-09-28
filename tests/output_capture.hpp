#pragma once

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

// Helpers for the tests of the programs' live paths (spec 12.6).
namespace test {

// Captures what a descriptor (stdout, stderr) receives while it lives: the descriptor is pointed at a pipe that a
// reader thread empties, so a test can wait for a line a program prints from another thread (wait_for() is woken by
// each read: no polling) and take everything once finish() has put the descriptor back. What the test harness prints
// meanwhile is captured too: check after finish().
class OutputCapture {
public:
    explicit OutputCapture(int fd) : fd_(fd), saved_(-1), read_end_(-1), closed_(false) {
        std::fflush(stdout);
        std::fflush(stderr);
        int ends[2];
        if (::pipe(ends) != 0) return;
        saved_ = ::dup(fd_);
        ::dup2(ends[1], fd_);
        ::close(ends[1]);
        read_end_ = ends[0];
        reader_ = std::thread(&OutputCapture::read_all, this);
    }

    ~OutputCapture() { finish(); }

    OutputCapture(const OutputCapture&) = delete;
    OutputCapture& operator=(const OutputCapture&) = delete;

    // Puts the descriptor back and returns everything it received.
    std::string finish() {
        if (saved_ >= 0) {
            std::fflush(stdout);
            std::fflush(stderr);
            ::dup2(saved_, fd_);  // the pipe's last write end closes: the reader sees its end
            ::close(saved_);
            saved_ = -1;
        }
        if (reader_.joinable()) reader_.join();
        if (read_end_ >= 0) {
            ::close(read_end_);
            read_end_ = -1;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return text_;
    }

    // Blocks until the text holds `part` (true), or the capture ends or `patience` passes (false).
    bool wait_for(const std::string& part, std::chrono::milliseconds patience) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, patience, [&] { return closed_ || text_.find(part) != std::string::npos; });
        return text_.find(part) != std::string::npos;
    }

private:
    static const std::size_t k_buffer = 4096;

    void read_all() {
        char buffer[k_buffer];
        for (;;) {
            const ssize_t count = ::read(read_end_, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (count > 0)
                    text_.append(buffer, static_cast<std::size_t>(count));
                else
                    closed_ = true;
            }
            changed_.notify_all();
            if (count <= 0) return;
        }
    }

    int fd_;
    int saved_;
    int read_end_;
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string text_;
    bool closed_;
};

// A signal's default action while it lives, the previous disposition after: a test that raises it to stop a live
// device must not depend on how it was started (a shell starts background jobs with SIGINT ignored, and
// pc::StopOnSignals keeps an ignored signal ignored).
class DefaultSignal {
public:
    explicit DefaultSignal(int signal_number)
        : signal_number_(signal_number), previous_(std::signal(signal_number, SIG_DFL)) {}
    ~DefaultSignal() { std::signal(signal_number_, previous_); }

    DefaultSignal(const DefaultSignal&) = delete;
    DefaultSignal& operator=(const DefaultSignal&) = delete;

private:
    int signal_number_;
    void (*previous_)(int);
};

inline std::size_t occurrences(const std::string& text, const std::string& part) {
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++count;
    return count;
}

}  // namespace test
