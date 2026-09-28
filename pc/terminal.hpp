#pragma once

#include <cstdio>

namespace unlimited {
namespace pc {

const int k_default_terminal_columns = 80;
const int k_default_terminal_rows = 24;

bool is_tty(std::FILE* stream = stdout);

// Size of the terminal behind the stream. When it is not a terminal (or the size is unknown) the
// result is 80 x 24 and the function returns false.
bool terminal_size(int& columns, int& rows, std::FILE* stream = stdout);

// Windows: turns on ENABLE_VIRTUAL_TERMINAL_PROCESSING and UTF-8 output for the console.
// POSIX terminals already understand VT sequences: nothing to do, returns true.
bool enable_vt(std::FILE* stream = stdout);

// While enabled, SIGINT, SIGTERM and exit() reset the text attributes and show the cursor on stdout
// before the process ends, so an interrupted TUI never leaves the terminal without a cursor.
void restore_terminal_on_exit(bool enabled);

// While it exists, Ctrl-C (SIGINT), SIGTERM and SIGHUP call stop(context) instead of ending the program (spec 12.6).
// A live device's stop() is async-signal-safe (an atomic store and a byte on a pipe, spec 12.5), so the program then
// ends on its own path: the device stopped, the final frame drawn, the cursor and colours back, the PTT released, the
// summary printed. Every signal calls stop() again (it must be idempotent); the handlers in place before come back
// when it is destroyed, so it may be created before or after Tui::open(). One at a time.
class StopOnSignals {
public:
    typedef void (*StopFunction)(void* context);

    StopOnSignals(StopFunction stop, void* context);
    ~StopOnSignals();
    StopOnSignals(const StopOnSignals&) = delete;
    StopOnSignals& operator=(const StopOnSignals&) = delete;

    int count() const;        // signals received
    int last_signal() const;  // the number of the last one; 0 before any
};

}  // namespace pc
}  // namespace unlimited
