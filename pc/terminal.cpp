#include "terminal.hpp"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004  // missing from older SDK headers
#endif
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace unlimited {
namespace pc {

namespace {

const char k_restore_sequence[] = "\x1b[0m\x1b[?25h\r\n";  // attributes off, cursor on, next line
const std::size_t k_restore_length = sizeof(k_restore_sequence) - 1;

typedef void (*SignalHandler)(int);

const int k_restore_signals[] = {SIGINT, SIGTERM};
const std::size_t k_restore_signal_count = sizeof(k_restore_signals) / sizeof(k_restore_signals[0]);

volatile std::sig_atomic_t g_restore_armed = 0;
bool g_exit_hook_installed = false;
bool g_hooked[k_restore_signal_count];

// StopOnSignals: SIGHUP too, where the system has it (a closed terminal must still release a keyed radio).
#ifdef SIGHUP
const int k_stop_signals[] = {SIGINT, SIGTERM, SIGHUP};
#else
const int k_stop_signals[] = {SIGINT, SIGTERM};
#endif
const std::size_t k_stop_signal_count = sizeof(k_stop_signals) / sizeof(k_stop_signals[0]);

static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "a signal handler may use lock-free atomics only");
std::atomic<StopOnSignals::StopFunction> g_stop_function(nullptr);
std::atomic<void*> g_stop_context(nullptr);
volatile std::sig_atomic_t g_stop_count = 0;
volatile std::sig_atomic_t g_stop_last = 0;
SignalHandler g_stop_previous[k_stop_signal_count];

#ifdef _WIN32
HANDLE stream_handle(std::FILE* stream) {
    return reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
}
#endif

// Async-signal-safe: a raw write on the stdout descriptor.
void write_restore_raw() {
#ifdef _WIN32
    _write(_fileno(stdout), k_restore_sequence, static_cast<unsigned>(k_restore_length));
#else
    const ssize_t written = write(STDOUT_FILENO, k_restore_sequence, k_restore_length);
    (void)written;
#endif
}

void on_restore_signal(int signal_number) {
    if (g_restore_armed) write_restore_raw();
    g_restore_armed = 0;
    std::signal(signal_number, SIG_DFL);
    std::raise(signal_number);
}

// Not in signal context: go through stdio so the sequence stays after any buffered output.
void on_process_exit() {
    if (!g_restore_armed) return;
    g_restore_armed = 0;
    std::fputs(k_restore_sequence, stdout);
    std::fflush(stdout);
}

// Async-signal-safe: lock-free atomics, sig_atomic_t, and the stop function (a live device's stop()).
void on_stop_signal(int signal_number) {
    g_stop_last = signal_number;
    g_stop_count = g_stop_count + 1;
    const StopOnSignals::StopFunction stop = g_stop_function.load();
    if (stop != nullptr) stop(g_stop_context.load());
}

}  // namespace

bool is_tty(std::FILE* stream) {
#ifdef _WIN32
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

bool terminal_size(int& columns, int& rows, std::FILE* stream) {
    columns = k_default_terminal_columns;
    rows = k_default_terminal_rows;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    const HANDLE handle = stream_handle(stream);
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(handle, &info)) return false;
    const int width = info.srWindow.Right - info.srWindow.Left + 1;
    const int height = info.srWindow.Bottom - info.srWindow.Top + 1;
#else
    struct winsize size;
    if (ioctl(fileno(stream), TIOCGWINSZ, &size) != 0) return false;
    const int width = size.ws_col;
    const int height = size.ws_row;
#endif
    if (width <= 0 || height <= 0) return false;
    columns = width;
    rows = height;
    return true;
}

bool enable_vt(std::FILE* stream) {
#ifdef _WIN32
    const HANDLE handle = stream_handle(stream);
    DWORD mode = 0;
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode)) return false;
    SetConsoleOutputCP(CP_UTF8);
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) return true;
    return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    (void)stream;
    return true;
#endif
}

// Only signals that would still kill the process are hooked: a program that handles or ignores
// SIGINT itself keeps doing so, and closes the TUI on its own path.
void restore_terminal_on_exit(bool enabled) {
    if (enabled == (g_restore_armed != 0)) return;
    if (enabled) {
        if (!g_exit_hook_installed) {
            std::atexit(on_process_exit);
            g_exit_hook_installed = true;
        }
        for (std::size_t i = 0; i < k_restore_signal_count; ++i) {
            const SignalHandler previous = std::signal(k_restore_signals[i], on_restore_signal);
            g_hooked[i] = previous == SIG_DFL;
            if (!g_hooked[i] && previous != SIG_ERR) std::signal(k_restore_signals[i], previous);
        }
        g_restore_armed = 1;
        return;
    }
    g_restore_armed = 0;
    for (std::size_t i = 0; i < k_restore_signal_count; ++i) {
        if (g_hooked[i]) std::signal(k_restore_signals[i], SIG_DFL);
        g_hooked[i] = false;
    }
}

// An ignored signal stays ignored (a program started with nohup, or in the background by a shell without job control).
StopOnSignals::StopOnSignals(StopFunction stop, void* context) {
    g_stop_count = 0;
    g_stop_last = 0;
    g_stop_context.store(context);
    g_stop_function.store(stop);
    for (std::size_t i = 0; i < k_stop_signal_count; ++i) {
        g_stop_previous[i] = std::signal(k_stop_signals[i], on_stop_signal);
        if (g_stop_previous[i] == SIG_IGN) std::signal(k_stop_signals[i], SIG_IGN);
    }
}

StopOnSignals::~StopOnSignals() {
    for (std::size_t i = 0; i < k_stop_signal_count; ++i)
        if (g_stop_previous[i] != SIG_ERR) std::signal(k_stop_signals[i], g_stop_previous[i]);
    g_stop_function.store(nullptr);
    g_stop_context.store(nullptr);
}

int StopOnSignals::count() const {
    return g_stop_count;
}

int StopOnSignals::last_signal() const {
    return g_stop_last;
}

}  // namespace pc
}  // namespace unlimited
