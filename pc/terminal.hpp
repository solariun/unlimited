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

}  // namespace pc
}  // namespace unlimited
