#include "tui.hpp"

#include "terminal.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace unlimited {
namespace pc {

using std::size_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

namespace {

// ===========================================================================
// Constants
// ===========================================================================

const double k_pi = 3.14159265358979323846;
const double k_two_pi = 2.0 * k_pi;
const double k_ms_per_s = 1000.0;
const double k_full_scale = 32768.0;  // int16 full scale
const double k_power_to_db = 10.0;
const double k_amplitude_to_db = 20.0;
const double k_half = 0.5;
const double k_hann_coherent_gain = 0.5;
const double k_goertzel_coefficient_scale = 2.0;
const double k_standard_spacing = static_cast<double>(k_standard_spacing_num) / k_standard_spacing_den;
const int k_format_buffer = 64;

// Display precision (decimals)
const int k_hz_decimals = 1;
const int k_ms_decimals = 2;
const int k_baud_decimals = 2;
const int k_db_decimals = 1;
const int k_seconds_decimals = 1;
const int k_rate_decimals = 1;
const int k_whole = 0;  // percent, window length, bin frequency, confidence under a slot
const int k_khz_decimals = 1;

// Terminal control (ANSI / VT100)
const char k_cursor_home[] = "\x1b[H";
const char k_clear_screen[] = "\x1b[2J";
const char k_clear_line[] = "\x1b[K";
const char k_hide_cursor[] = "\x1b[?25l";
const char k_show_cursor[] = "\x1b[?25h";
const char k_reset_attributes[] = "\x1b[0m";
const char k_line_break[] = "\r\n";
const char k_separator[] = " \xe2\x94\x82 ";  // " │ " between status items
const uint8_t k_escape = 0x1B;
const char k_csi_introducer = '[';
const uint8_t k_csi_final_first = 0x40;
const uint8_t k_csi_final_last = 0x7E;

// Unicode code points
const uint32_t k_glyph_space = 0x20;
const uint32_t k_glyph_delete = 0x7F;
const uint32_t k_glyph_c1_first = 0x80;
const uint32_t k_glyph_c1_last = 0x9F;
const uint32_t k_glyph_middle_dot = 0x00B7;      // unprintable byte, idle slot, grid tone on the spectrum axis
const uint32_t k_glyph_return = 0x21B5;          // newline byte
const uint32_t k_glyph_rule = 0x2500;            // panel title rule
const uint32_t k_glyph_axis = 0x2502;
const uint32_t k_glyph_axis_tick = 0x2524;
const uint32_t k_glyph_reference = 0x2508;       // 100 % START/STOP crest line
const uint32_t k_glyph_reference_line = 0x254C;  // 70 % of the START/STOP crest
const uint32_t k_glyph_weak_block = 0x2592;      // lit tone of a slot below the 70 % line
const uint32_t k_glyph_caret = 0x25B2;           // current slot, tone marker
const uint32_t k_glyph_lower_eighth = 0x2581;    // lower 1/8 block; 2/8 .. 7/8 follow
const uint32_t k_glyph_full_block = 0x2588;
const uint32_t k_glyph_braille = 0x2800;         // empty braille cell; the dots are bits 0..7
const uint32_t k_glyph_replacement = 0xFFFD;
const uint8_t k_ascii_first_printable = 0x20;
const uint8_t k_ascii_delete = 0x7F;
const uint8_t k_ascii_newline = '\n';

// UTF-8
const uint32_t k_utf8_limit_1 = 0x80;
const uint32_t k_utf8_limit_2 = 0x800;
const uint32_t k_utf8_limit_3 = 0x10000;
const unsigned k_utf8_payload_bits = 6;
const uint32_t k_utf8_payload_mask = 0x3F;
const uint8_t k_utf8_continuation = 0x80;
const uint8_t k_utf8_continuation_mask = 0xC0;
const uint8_t k_utf8_lead_2 = 0xC0;
const uint8_t k_utf8_lead_2_mask = 0xE0;
const uint8_t k_utf8_lead_3 = 0xE0;
const uint8_t k_utf8_lead_3_mask = 0xF0;
const uint8_t k_utf8_lead_4 = 0xF0;
const uint8_t k_utf8_lead_4_mask = 0xF8;

// Braille: a cell is 2 x 4 dots; bit of each dot by [dot column][dot row] (Unicode braille order).
const int k_braille_dot_columns = 2;
const int k_braille_dot_rows = 4;
const uint8_t k_braille_bits[k_braille_dot_columns][k_braille_dot_rows] = {{0x01, 0x02, 0x04, 0x40},
                                                                            {0x08, 0x10, 0x20, 0x80}};
const int k_eighths = 8;  // block elements resolve a text row in eighths

// Layout: rows of each panel; every panel but the status has one extra title row.
const int k_title_rows = 1;
const int k_status_rows_min = 1;
const int k_status_rows = 2;
const int k_text_rows_min = 1;
const int k_text_rows_preferred = 2;
const int k_text_rows_max = 4;
const int k_bar_rows_min = 3;
const int k_bar_rows_preferred = 9;
const int k_bar_rows_max = 14;
const int k_decoder_footer_rows = 3;  // slot labels, tone indices, confidence
const int k_encoder_footer_rows = 4;  // slot labels, tone indices, symbols, current-slot caret
const int k_spectrum_rows_min = 1;
const int k_spectrum_rows_preferred = 2;
const int k_spectrum_axis_rows = 1;
const int k_scope_rows_min = 2;
const int k_scope_rows_preferred = 4;
const int k_scope_rows_max = 10;
const int k_spectrum_min_columns = 8;
const int k_scope_min_columns = 4;
const int k_title_lead = 2;     // rule cells before a panel title
const int k_status_margin = 1;  // blank cell before the status text

// Peaks panel: START, the peak slots, STOP; a cell is its bar plus one gap column. The grid rows sit above
// the decoder's level rows, which take a third of the panel.
const int k_frame_markers = 2;
const int k_fallback_data_slots = k_default_data_slots;  // until a mode is known
const int k_fallback_tones = 8;
const int k_header_tones = 8;  // GF(8) symbols
const int k_axis_label_columns = 4;  // "100%"
const int k_axis_columns = k_axis_label_columns + 1;
const int k_min_cell_columns = 2;
const int k_max_cell_columns = 6;
const int k_gap_columns = 1;
const int k_level_row_share = 3;
const int k_level_rows_max = 4;
const float k_level_top_pct = 150.0f;
const float k_reference_pct = 100.0f;      // START/STOP crest
const float k_reference_line_pct = 70.0f;  // visual reference only: argmax decides (spec 3.10.1)
const float k_full_pct = 100.0f;
const double k_confidence_step_db = 0.5;
const int k_no_cell = -1;

// Scope and spectrum
const double k_audio_history_ms = 320.0;
const double k_scope_slots = 2.0;
const double k_default_scope_ms = 64.0;
const double k_scope_min_peak = 64.0;  // about -54 dBFS: silence stays a flat line instead of noise at full height
const double k_spectrum_low_hz = 300.0;
const double k_spectrum_high_hz = 3000.0;
const double k_spectrum_window_ms = 128.0;
const size_t k_spectrum_max_samples = 4096;  // bounds the Goertzel cost at high rates (85 ms at 48 kHz)
const double k_spectrum_top_dbfs = 0.0;
const double k_spectrum_floor_dbfs = -60.0;
const int k_spectrum_axis_hz[] = {500, 1000, 1500, 2000, 2500, 3000};
const int k_hz_per_khz = 1000;

const size_t k_text_capacity = 1 << 16;

// ===========================================================================
// Text helpers
// ===========================================================================

enum class Color : uint8_t { plain, dim, green, yellow, cyan, reverse };

const char* sgr(Color color) {
    switch (color) {
    case Color::plain: return "\x1b[0m";
    case Color::dim: return "\x1b[0;90m";
    case Color::green: return "\x1b[0;32m";
    case Color::yellow: return "\x1b[0;33m";
    case Color::cyan: return "\x1b[0;36m";
    case Color::reverse: return "\x1b[0;7m";
    }
    return "\x1b[0m";
}

void append_utf8(uint32_t code_point, std::string& out) {
    if (code_point < k_utf8_limit_1) {
        out += static_cast<char>(code_point);
    } else if (code_point < k_utf8_limit_2) {
        out += static_cast<char>(k_utf8_lead_2 | (code_point >> k_utf8_payload_bits));
        out += static_cast<char>(k_utf8_continuation | (code_point & k_utf8_payload_mask));
    } else if (code_point < k_utf8_limit_3) {
        out += static_cast<char>(k_utf8_lead_3 | (code_point >> (2 * k_utf8_payload_bits)));
        out += static_cast<char>(k_utf8_continuation | ((code_point >> k_utf8_payload_bits) & k_utf8_payload_mask));
        out += static_cast<char>(k_utf8_continuation | (code_point & k_utf8_payload_mask));
    } else {
        out += static_cast<char>(k_utf8_lead_4 | (code_point >> (3 * k_utf8_payload_bits)));
        out += static_cast<char>(k_utf8_continuation |
                                 ((code_point >> (2 * k_utf8_payload_bits)) & k_utf8_payload_mask));
        out += static_cast<char>(k_utf8_continuation | ((code_point >> k_utf8_payload_bits) & k_utf8_payload_mask));
        out += static_cast<char>(k_utf8_continuation | (code_point & k_utf8_payload_mask));
    }
}

// Invalid sequences become U+FFFD, one per offending byte.
void decode_utf8(const std::string& text, std::vector<uint32_t>& out) {
    out.clear();
    size_t index = 0;
    while (index < text.size()) {
        const uint8_t lead = static_cast<uint8_t>(text[index]);
        uint32_t code_point = lead;
        size_t extra = 0;
        if (lead < k_utf8_limit_1) {
            extra = 0;
        } else if ((lead & k_utf8_lead_2_mask) == k_utf8_lead_2) {
            code_point = lead & static_cast<uint8_t>(~k_utf8_lead_2_mask);
            extra = 1;
        } else if ((lead & k_utf8_lead_3_mask) == k_utf8_lead_3) {
            code_point = lead & static_cast<uint8_t>(~k_utf8_lead_3_mask);
            extra = 2;
        } else if ((lead & k_utf8_lead_4_mask) == k_utf8_lead_4) {
            code_point = lead & static_cast<uint8_t>(~k_utf8_lead_4_mask);
            extra = 3;
        } else {
            out.push_back(k_glyph_replacement);
            ++index;
            continue;
        }
        bool valid = index + extra < text.size();
        for (size_t k = 1; valid && k <= extra; ++k) {
            const uint8_t byte = static_cast<uint8_t>(text[index + k]);
            valid = (byte & k_utf8_continuation_mask) == k_utf8_continuation;
            code_point = (code_point << k_utf8_payload_bits) | (byte & k_utf8_payload_mask);
        }
        if (!valid) {
            out.push_back(k_glyph_replacement);
            ++index;
            continue;
        }
        out.push_back(code_point);
        index += 1 + extra;
    }
}

uint32_t byte_glyph(uint8_t byte) {
    if (byte == k_ascii_newline) return k_glyph_return;
    if (byte >= k_ascii_first_printable && byte < k_ascii_delete) return byte;
    return k_glyph_middle_dot;
}

std::string fixed(double value, int decimals) {
    char buffer[k_format_buffer];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

std::string byte_text(uint8_t value) {
    char buffer[k_format_buffer];
    if (value >= k_ascii_first_printable && value < k_ascii_delete)
        std::snprintf(buffer, sizeof(buffer), "0x%02X '%c'", value, static_cast<char>(value));
    else
        std::snprintf(buffer, sizeof(buffer), "0x%02X", value);
    return buffer;
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += k_separator;
        out += items[i];
    }
    return out;
}

const char* state_name(DecoderState state) {
    switch (state) {
    case DecoderState::search: return "SEARCH";
    case DecoderState::acquire: return "ACQUIRE";
    case DecoderState::preamble: return "PREAMBLE";
    case DecoderState::track: return "TRACK";
    }
    return "?";
}

const char* reason_name(LostReason reason) {
    switch (reason) {
    case LostReason::none: return "none";
    case LostReason::signal_gone: return "signal gone";
    case LostReason::alias: return "alias";
    case LostReason::preamble_timeout: return "preamble timeout";
    case LostReason::reset: return "reset";
    case LostReason::no_header: return "no header";
    case LostReason::unsupported_mode: return "unsupported mode";
    }
    return "?";
}

const char* segment_name(EncoderSegment segment) {
    switch (segment) {
    case EncoderSegment::idle: return "idle";
    case EncoderSegment::lead_in: return "lead-in";
    case EncoderSegment::tune: return "tune";
    case EncoderSegment::sync: return "sync";
    case EncoderSegment::header: return "header";
    case EncoderSegment::frame: return "frame";
    case EncoderSegment::eot: return "eot";
    case EncoderSegment::tail: return "tail";
    }
    return "?";
}

std::string upper(const std::string& text) {
    std::string out = text;
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i] >= 'a' && out[i] <= 'z') out[i] = static_cast<char>(out[i] - 'a' + 'A');
    return out;
}

// ===========================================================================
// Canvas: a grid of single-width cells; nothing drawn can leave it
// ===========================================================================

struct Cell {
    uint32_t glyph;
    Color color;
};

Cell make_cell(uint32_t glyph, Color color) {
    const Cell cell = {glyph, color};
    return cell;
}

class Canvas {
public:
    Canvas(int columns, int rows)
        : columns_(std::max(columns, 0)),
          rows_(std::max(rows, 0)),
          cells_(static_cast<size_t>(columns_) * static_cast<size_t>(rows_), make_cell(k_glyph_space, Color::plain)) {}

    int columns() const {
        return columns_;
    }

    int rows() const {
        return rows_;
    }

    // Control characters never reach the terminal: they would move the cursor.
    void put(int row, int column, uint32_t glyph, Color color) {
        if (row < 0 || row >= rows_ || column < 0 || column >= columns_) return;
        const bool control = glyph < k_glyph_space || glyph == k_glyph_delete ||
                             (glyph >= k_glyph_c1_first && glyph <= k_glyph_c1_last);
        cells_[index(row, column)] = make_cell(control ? k_glyph_middle_dot : glyph, color);
    }

    int text(int row, int column, const std::string& utf8, Color color) {
        std::vector<uint32_t> glyphs;
        decode_utf8(utf8, glyphs);
        for (size_t i = 0; i < glyphs.size(); ++i) put(row, column++, glyphs[i], color);
        return column;
    }

    void fill(int row, int from, int to, uint32_t glyph, Color color) {
        for (int column = from; column < to; ++column) put(row, column, glyph, color);
    }

    std::string str(bool color) const {
        std::string out;
        for (int row = 0; row < rows_; ++row) {
            int end = columns_;
            while (end > 0 && blank(cells_[index(row, end - 1)], color)) --end;
            Color current = Color::plain;
            for (int column = 0; column < end; ++column) {
                const Cell& cell = cells_[index(row, column)];
                if (color && cell.color != current) {
                    current = cell.color;
                    out += sgr(current);
                }
                append_utf8(cell.glyph, out);
            }
            if (color && current != Color::plain) out += sgr(Color::plain);
            out += '\n';
        }
        return out;
    }

private:
    size_t index(int row, int column) const {
        return static_cast<size_t>(row) * static_cast<size_t>(columns_) + static_cast<size_t>(column);
    }

    static bool blank(const Cell& cell, bool color) {
        return cell.glyph == k_glyph_space && !(color && cell.color == Color::reverse);
    }

    int columns_;
    int rows_;
    std::vector<Cell> cells_;
};

void draw_title(Canvas& canvas, int row, const std::string& title) {
    canvas.fill(row, 0, k_title_lead, k_glyph_rule, Color::dim);
    const int end = canvas.text(row, k_title_lead + 1, title, Color::cyan);
    canvas.fill(row, end + 1, canvas.columns(), k_glyph_rule, Color::dim);
}

// Block element for a bar that fills `filled` eighths of this row (0: none).
uint32_t block_glyph(int filled) {
    if (filled >= k_eighths) return k_glyph_full_block;
    if (filled > 0) return k_glyph_lower_eighth + static_cast<uint32_t>(filled - 1);
    return 0;
}

int clamp_int(int value, int low, int high) {
    return std::min(std::max(value, low), high);
}

int level_eighths(double pct, double top_pct, int rows) {
    const int total = rows * k_eighths;
    return clamp_int(static_cast<int>(std::lround(pct / top_pct * total)), 0, total);
}

int level_row(double pct, double top_pct, int rows) {
    return clamp_int(static_cast<int>(std::floor(pct / top_pct * rows)), 0, rows - 1);
}

// ===========================================================================
// Layout: panels are dropped from the bottom of the priority list when rows run out
// ===========================================================================

struct Layout {
    int status;    // rows
    int scope;     // braille rows, 0 = hidden
    int bars;      // bar rows, 0 = hidden
    int spectrum;  // bar rows, 0 = hidden
    int text;      // text rows, 0 = hidden
};

void grow(int& value, int target, int& free_rows) {
    if (value <= 0 || value >= target) return;
    const int added = std::min(target - value, free_rows);
    value += added;
    free_rows -= added;
}

void reserve(int& value, int minimum, int fixed_rows, bool fits_width, int& free_rows) {
    if (!fits_width || free_rows < fixed_rows + minimum) return;
    value = minimum;
    free_rows -= fixed_rows + minimum;
}

Layout plan_layout(int columns, int rows, int footer_rows, int peak_cells) {
    Layout layout = {0, 0, 0, 0, 0};
    if (columns <= 0 || rows <= 0) return layout;
    int free_rows = rows - k_status_rows_min;
    layout.status = k_status_rows_min;
    // Priority: status, text, peaks, spectrum, scope.
    reserve(layout.text, k_text_rows_min, k_title_rows, true, free_rows);
    reserve(layout.bars, k_bar_rows_min, k_title_rows + footer_rows, columns >= peak_cells * k_min_cell_columns,
            free_rows);
    reserve(layout.spectrum, k_spectrum_rows_min, k_title_rows + k_spectrum_axis_rows,
            columns >= k_spectrum_min_columns, free_rows);
    reserve(layout.scope, k_scope_rows_min, k_title_rows, columns >= k_scope_min_columns, free_rows);
    grow(layout.status, k_status_rows, free_rows);
    grow(layout.bars, k_bar_rows_preferred, free_rows);
    grow(layout.scope, k_scope_rows_preferred, free_rows);
    grow(layout.text, k_text_rows_preferred, free_rows);
    grow(layout.spectrum, k_spectrum_rows_preferred, free_rows);
    grow(layout.scope, k_scope_rows_max, free_rows);
    grow(layout.bars, k_bar_rows_max, free_rows);
    grow(layout.text, k_text_rows_max, free_rows);
    return layout;
}

// ===========================================================================
// Panels
// ===========================================================================

void draw_status(Canvas& canvas, int top, int rows, const std::string& first, const std::string& second) {
    canvas.fill(top, 0, canvas.columns(), k_glyph_space, Color::reverse);
    if (rows < k_status_rows) {
        canvas.text(top, k_status_margin, first + k_separator + second, Color::reverse);
        return;
    }
    canvas.text(top, k_status_margin, first, Color::reverse);
    canvas.text(top + 1, k_status_margin, second, Color::plain);
}

void draw_text(Canvas& canvas, int top, int rows, const std::string& title, const std::string& text) {
    draw_title(canvas, top, title);
    const size_t width = static_cast<size_t>(canvas.columns());
    const size_t lines = std::max<size_t>(1, (text.size() + width - 1) / width);
    const size_t first = lines > static_cast<size_t>(rows) ? lines - static_cast<size_t>(rows) : 0;
    for (int line = 0; line < rows; ++line) {
        const size_t begin = (first + static_cast<size_t>(line)) * width;
        for (size_t column = 0; column < width && begin + column < text.size(); ++column)
            canvas.put(top + k_title_rows + line, static_cast<int>(column),
                       byte_glyph(static_cast<uint8_t>(text[begin + column])), Color::plain);
    }
}

int dot_row(float value, double scale, int dot_rows) {
    const double position = (1.0 - value / scale) * k_half * (dot_rows - 1);
    return clamp_int(static_cast<int>(std::lround(position)), 0, dot_rows - 1);
}

// Min/max of the newest `window` samples per dot column, right-aligned (samples may be fewer than
// window). Each column spans at least min_span samples: one carrier period shows the envelope instead
// of aliased cycles.
void draw_scope(Canvas& canvas, int top, int rows, const std::vector<float>& samples, size_t window,
                size_t min_span, double window_ms) {
    double peak = 0.0;
    for (size_t i = 0; i < samples.size(); ++i) peak = std::max(peak, static_cast<double>(std::fabs(samples[i])));
    std::string title = "scope " + fixed(window_ms, k_whole) + " ms";
    if (samples.empty())
        title += "  no audio";
    else if (peak <= 0.0)
        title += "  silence";
    else
        title += "  peak " + fixed(k_amplitude_to_db * std::log10(peak / k_full_scale), k_db_decimals) + " dBFS";
    draw_title(canvas, top, title);
    if (samples.empty() || window == 0) return;

    const double scale = std::max(peak, k_scope_min_peak);
    const int columns = canvas.columns();
    const size_t dot_columns = static_cast<size_t>(columns) * k_braille_dot_columns;
    const int dot_rows = rows * k_braille_dot_rows;
    const size_t missing = window - samples.size();
    std::vector<uint8_t> dots(static_cast<size_t>(columns) * static_cast<size_t>(rows), 0);
    for (size_t x = 0; x < dot_columns; ++x) {
        size_t from = x * window / dot_columns;
        size_t to = std::max((x + 1) * window / dot_columns, from + 1);
        if (to - from < min_span) {
            const size_t centre = (from + to) / 2;
            from = centre > min_span / 2 ? centre - min_span / 2 : 0;
            to = from + min_span;
        }
        if (to <= missing) continue;
        from = from > missing ? from - missing : 0;
        to = std::min(to - missing, samples.size());
        if (from >= to) continue;
        float low = samples[from];
        float high = low;
        for (size_t i = from + 1; i < to; ++i) {
            low = std::min(low, samples[i]);
            high = std::max(high, samples[i]);
        }
        const int top_dot = dot_row(high, scale, dot_rows);
        const int bottom_dot = dot_row(low, scale, dot_rows);
        for (int y = top_dot; y <= bottom_dot; ++y)
            dots[static_cast<size_t>(y / k_braille_dot_rows) * static_cast<size_t>(columns) +
                 x / k_braille_dot_columns] |= k_braille_bits[x % k_braille_dot_columns][y % k_braille_dot_rows];
    }
    for (int row = 0; row < rows; ++row)
        for (int column = 0; column < columns; ++column) {
            const uint8_t bits = dots[static_cast<size_t>(row) * static_cast<size_t>(columns) +
                                      static_cast<size_t>(column)];
            if (bits != 0) canvas.put(top + k_title_rows + row, column, k_glyph_braille + bits, Color::green);
        }
}

double column_hz(int column, int columns) {
    return k_spectrum_low_hz +
           (k_spectrum_high_hz - k_spectrum_low_hz) * column / std::max(columns - 1, 1);
}

int hz_column(double hz, int columns) {
    return static_cast<int>(
        std::lround((hz - k_spectrum_low_hz) / (k_spectrum_high_hz - k_spectrum_low_hz) * (columns - 1)));
}

std::string hz_label(int hz) {
    if (hz < k_hz_per_khz) return std::to_string(hz);
    if (hz % k_hz_per_khz == 0) return std::to_string(hz / k_hz_per_khz) + "k";
    return fixed(static_cast<double>(hz) / k_hz_per_khz, k_khz_decimals) + "k";
}

// Goertzel bank, one bin per column, Hann window; levels in dBFS (a full-scale sine reads 0). Columns of the
// tone grid (grid_hz) draw in cyan, with a dot on the floor where they show no level.
void draw_spectrum(Canvas& canvas, int top, int bar_rows, const std::vector<float>& samples, uint32_t rate_hz,
                   float tone_hz, const std::vector<double>& grid_hz) {
    const int columns = canvas.columns();
    std::vector<double> dbfs(static_cast<size_t>(columns), k_spectrum_floor_dbfs);
    int peak_column = -1;
    const size_t count = samples.size();
    if (count > 0 && rate_hz > 0) {
        std::vector<double> windowed(count);
        for (size_t i = 0; i < count; ++i)
            windowed[i] = samples[i] * k_half * (1.0 - std::cos(k_two_pi * static_cast<double>(i) / count));
        const double reference = k_full_scale * static_cast<double>(count) * k_hann_coherent_gain * k_half;
        for (int column = 0; column < columns; ++column) {
            const double hz = column_hz(column, columns);
            if (hz >= rate_hz * k_half) continue;
            const double coefficient = k_goertzel_coefficient_scale * std::cos(k_two_pi * hz / rate_hz);
            double s1 = 0.0;
            double s2 = 0.0;
            for (size_t i = 0; i < count; ++i) {
                const double s0 = windowed[i] + coefficient * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
            if (power <= 0.0) continue;
            const double level = k_power_to_db * std::log10(power / (reference * reference));
            dbfs[static_cast<size_t>(column)] = std::max(level, k_spectrum_floor_dbfs);
            if (peak_column < 0 || level > dbfs[static_cast<size_t>(peak_column)]) peak_column = column;
        }
    }

    std::vector<bool> grid_column(static_cast<size_t>(columns), false);
    double grid_low = 0.0;
    double grid_high = 0.0;
    for (size_t i = 0; i < grid_hz.size(); ++i) {
        grid_low = i == 0 ? grid_hz[i] : std::min(grid_low, grid_hz[i]);
        grid_high = i == 0 ? grid_hz[i] : std::max(grid_high, grid_hz[i]);
        const int column = hz_column(grid_hz[i], columns);
        if (column >= 0 && column < columns) grid_column[static_cast<size_t>(column)] = true;
    }

    std::string title =
        "spectrum " + fixed(k_spectrum_low_hz, k_whole) + "-" + fixed(k_spectrum_high_hz, k_whole) + " Hz";
    if (!grid_hz.empty())
        title += "  grid " + fixed(grid_low, k_whole) + "-" + fixed(grid_high, k_whole) + " Hz";
    if (peak_column >= 0)
        title += "  peak " + fixed(column_hz(peak_column, columns), k_whole) + " Hz " +
                 fixed(dbfs[static_cast<size_t>(peak_column)], k_db_decimals) + " dBFS";
    draw_title(canvas, top, title);

    const int bottom_row = top + k_title_rows + bar_rows - 1;
    const double range = k_spectrum_top_dbfs - k_spectrum_floor_dbfs;
    for (int column = 0; column < columns; ++column) {
        const double fraction = (dbfs[static_cast<size_t>(column)] - k_spectrum_floor_dbfs) / range;
        const int eighths = level_eighths(fraction, 1.0, bar_rows);
        const bool grid = grid_column[static_cast<size_t>(column)];
        for (int level = 0; level < bar_rows; ++level) {
            const uint32_t block = block_glyph(eighths - level * k_eighths);
            if (block != 0) canvas.put(bottom_row - level, column, block, grid ? Color::cyan : Color::green);
        }
        if (grid && eighths == 0) canvas.put(bottom_row, column, k_glyph_middle_dot, Color::cyan);
    }

    const int axis_row = bottom_row + 1;
    const bool marked = tone_hz >= k_spectrum_low_hz && tone_hz <= k_spectrum_high_hz;
    const int marker = marked ? hz_column(tone_hz, columns) : -columns;
    int next_free = 0;
    for (size_t i = 0; i < sizeof(k_spectrum_axis_hz) / sizeof(k_spectrum_axis_hz[0]); ++i) {
        const std::string label = hz_label(k_spectrum_axis_hz[i]);
        const int width = static_cast<int>(label.size());
        const int column = std::min(hz_column(k_spectrum_axis_hz[i], columns), columns - width);
        if (column < next_free || (marker >= column - 1 && marker <= column + width)) continue;
        canvas.text(axis_row, column, label, Color::dim);
        next_free = column + width + 1;
    }
    if (marked) canvas.put(axis_row, marker, k_glyph_caret, Color::yellow);
}

// Mode of the transmission shown, for tone frequencies and the status line.
struct GridView {
    uint8_t bits_per_peak;  // 0 = unknown
    uint8_t data_slots;
    Spacing spacing;
    int8_t side;            // +1 grid above f_ref, -1 below
    float tone_hz;          // f_ref, 0 = unknown
    float slot_ms;          // 0 = unknown
};

int grid_tones(const GridView& grid) {
    return grid.bits_per_peak > 0 ? 1 << grid.bits_per_peak : k_fallback_tones;
}

int grid_data_slots(const GridView& grid) {
    return grid.data_slots > 0 ? grid.data_slots : k_fallback_data_slots;
}

// f_ref + side (G + n c) / T (spec 1.3); header tones always use the standard spacing. 0 when unknown.
double tone_frequency_hz(const GridView& grid, int tone, Spacing spacing) {
    if (grid.tone_hz <= 0.0f || grid.slot_ms <= 0.0f) return 0.0;
    const double units = spacing == Spacing::dense ? 1.0 : k_standard_spacing;
    return grid.tone_hz + grid.side * (k_grid_guard + tone * units) * k_ms_per_s / grid.slot_ms;
}

// "k5 N8 standard below 138.9 bit/s": net rate N k / ((N + 1) T).
std::string mode_item(const GridView& grid) {
    if (grid.bits_per_peak == 0) return "mode --";
    std::string item = "k" + std::to_string(grid.bits_per_peak) + " N" + std::to_string(grid.data_slots) + " " +
                       (grid.spacing == Spacing::dense ? "dense" : "standard") + " " +
                       (grid.side > 0 ? "above" : "below");
    if (grid.slot_ms > 0.0f)
        item += " " + fixed(grid.data_slots * grid.bits_per_peak * k_ms_per_s / ((grid.data_slots + 1) * grid.slot_ms),
                            k_rate_decimals) + " bit/s";
    return item;
}

// Peaks panel (spec 6.5): the START marker, the peak slots of the header or frame, the STOP marker. Each
// column lights the tone of its peak on a tone-by-slot grid whose bottom row is f_ref (markers, tune tone).
// The decoder adds level bars against the START/STOP crest with the 100 % crest and 70 % reference lines.
enum class CellKind : uint8_t { empty, tune, marker, peak };

struct PeakCell {
    CellKind kind;
    int tone;            // peak: grid tone index
    float level_pct;     // decoder: bar height, % of the START/STOP crest
    Color color;
    std::string label;   // slot: "M", "T", "h0".."h7", "1".."N"
    std::string value;   // tone index
    std::string detail;  // decoder: confidence in whole dB; encoder: symbol
};

struct PeaksPanel {
    std::string title;
    std::vector<PeakCell> cells;
    int tones;                // grid rows cover tones 0..tones-1
    bool levels;              // decoder: level bars and reference lines
    int current;              // cell under the caret (encoder), k_no_cell for none
    std::string detail_name;  // footer row of `detail`
};

PeakCell peak_cell(CellKind kind, Color color, const std::string& label, float level_pct) {
    PeakCell cell;
    cell.kind = kind;
    cell.tone = 0;
    cell.level_pct = level_pct;
    cell.color = color;
    cell.label = label;
    return cell;
}

std::string confidence_db(uint8_t confidence, int decimals) {
    return fixed(confidence * k_confidence_step_db, decimals);
}

std::string slot_flags(uint8_t flags) {
    std::string text;
    if (flags & event_flag_erasure) text += "  erasure";
    if (flags & event_flag_blanked) text += "  blanked";
    return text;
}

// Flags of the last byte, for the text panel title.
std::string byte_flags(uint8_t flags) {
    std::string text;
    if (flags & event_flag_late_join) text += "  late-join";
    if (flags & event_flag_mode_memory) text += "  mode-memory";
    if (flags & event_flag_blind_mode) text += "  blind-mode";
    if (flags & event_flag_flywheel_start) text += "  flywheel-start";
    if (flags & event_flag_flywheel_stop) text += "  flywheel-stop";
    return text + slot_flags(flags);
}

PeaksPanel decoder_panel(const PeakSlot* peaks, bool has_peaks, const Event& last_slot, const GridView& grid) {
    PeaksPanel panel;
    panel.tones = grid_tones(grid);
    panel.levels = true;
    panel.current = k_no_cell;
    panel.detail_name = "conf";
    const int slots = grid_data_slots(grid);
    const PeakCell marker = peak_cell(has_peaks ? CellKind::marker : CellKind::empty, Color::cyan, "M",
                                      has_peaks ? k_reference_pct : 0.0f);
    panel.cells.push_back(marker);
    for (int i = 0; i < slots; ++i) {
        PeakCell cell = peak_cell(CellKind::empty, Color::green, std::to_string(i + 1), 0.0f);
        const PeakSlot& slot = peaks[i];
        if (has_peaks && slot.present) {
            cell.kind = CellKind::peak;
            cell.tone = slot.tone;
            cell.level_pct = slot.level_pct;
            cell.color = (slot.flags & event_flag_erasure) ? Color::yellow : Color::green;
            cell.value = std::to_string(slot.tone);
            cell.detail = confidence_db(slot.confidence, k_whole);
        }
        panel.cells.push_back(cell);
    }
    panel.cells.push_back(marker);
    if (!has_peaks) {
        panel.title = "peaks  waiting for a frame";
        return panel;
    }
    panel.title = "frame " + std::to_string(last_slot.frame_index) + "  slot " + std::to_string(last_slot.index) +
                  "/" + std::to_string(slots) + "  tone " + std::to_string(last_slot.tone) + "  symbol " +
                  std::to_string(last_slot.value) + "  level " + std::to_string(last_slot.level_pct) + "%  conf " +
                  confidence_db(last_slot.confidence, k_db_decimals) + " dB" + slot_flags(last_slot.flags);
    return panel;
}

// Tone being sent: "tone 13 1666.9 Hz".
std::string sent_tone(const GridView& grid, int tone, Spacing spacing) {
    const double hz = tone_frequency_hz(grid, tone, spacing);
    return "tone " + std::to_string(tone) + (hz > 0.0 ? " " + fixed(hz, k_hz_decimals) + " Hz" : "");
}

PeaksPanel encoder_panel(const EncoderStatus& status, bool has_status, const PeakSlot* peaks, bool has_peaks,
                         EncoderSegment peaks_segment, const GridView& grid) {
    PeaksPanel panel;
    panel.levels = false;
    panel.detail_name = "sym";
    const EncoderSegment segment = has_status ? status.segment : EncoderSegment::idle;
    const bool header = segment == EncoderSegment::header;
    panel.tones = header ? k_header_tones : grid_tones(grid);
    if (header || segment == EncoderSegment::frame) {
        const int slots = header ? k_header_slots : grid_data_slots(grid);
        const bool shown = has_peaks && peaks_segment == segment;
        const PeakCell marker = peak_cell(CellKind::marker, Color::cyan, "M", k_full_pct);
        panel.cells.push_back(marker);
        for (int i = 0; i < slots; ++i) {
            PeakCell cell = peak_cell(CellKind::empty, Color::green,
                                      header ? "h" + std::to_string(i) : std::to_string(i + 1), 0.0f);
            if (shown && peaks[i].present) {
                cell.kind = CellKind::peak;
                cell.tone = peaks[i].tone;
                cell.level_pct = k_full_pct;
                cell.value = std::to_string(peaks[i].tone);
                cell.detail = std::to_string(peaks[i].symbol);
            }
            panel.cells.push_back(cell);
        }
        panel.cells.push_back(marker);
        const bool peak = status.kind == SlotKind::peak;
        panel.current = peak ? 1 + std::min<int>(status.slot, slots - 1) : static_cast<int>(panel.cells.size()) - 1;
        panel.title = std::string(header ? "header" : "frame");
        if (peak)
            panel.title += "  slot " + panel.cells[static_cast<size_t>(panel.current)].label + "/" +
                           std::to_string(slots) + "  symbol " + std::to_string(status.symbol) + "  " +
                           sent_tone(grid, status.tone, header ? Spacing::standard : grid.spacing);
        else
            panel.title += "  STOP marker";
        if (!header) panel.title += "  first byte " + byte_text(status.byte);
        return panel;
    }
    PeakCell cell = peak_cell(CellKind::empty, Color::dim, "\xc2\xb7", 0.0f);  // "·"
    if (segment == EncoderSegment::tune)
        cell = peak_cell(CellKind::tune, Color::green, "T", k_full_pct);
    else if (segment == EncoderSegment::sync || segment == EncoderSegment::eot)
        cell = peak_cell(CellKind::marker, Color::cyan, "M", k_full_pct);
    const int count = grid_data_slots(grid) + k_frame_markers;
    panel.cells.assign(static_cast<size_t>(count), cell);
    panel.current = segment == EncoderSegment::idle ? k_no_cell : static_cast<int>(status.slot_index % count);
    panel.title = std::string(segment_name(segment));
    if (segment != EncoderSegment::idle) panel.title += "  slot " + std::to_string(status.slot_index);
    return panel;
}

// `text` in `width` cells: the last cells for a slot label (a ruler), nothing for a value that does not fit.
std::string fit(const std::string& text, int width, bool keep_tail) {
    const int length = static_cast<int>(display_width(text));
    if (length <= width) return text;
    if (!keep_tail || width <= 0) return "";
    return text.substr(text.size() - static_cast<size_t>(width));
}

// Title, the tone grid over its f_ref row, the decoder level bars, then the slot labels, tone indices,
// details (confidence or symbol) and, for the encoder, the current-slot caret.
void draw_peaks(Canvas& canvas, int top, int rows, const PeaksPanel& panel) {
    draw_title(canvas, top, panel.title);
    const int count = static_cast<int>(panel.cells.size());
    const int columns = canvas.columns();
    const int axis = columns >= count * k_min_cell_columns + k_axis_columns ? k_axis_columns : 0;
    const int cell_width = std::min(k_max_cell_columns, (columns - axis) / std::max(count, 1));
    if (cell_width < 1) return;
    const int bar_width = std::max(cell_width - k_gap_columns, 1);
    const int level_rows = panel.levels ? clamp_int(rows / k_level_row_share, 1, k_level_rows_max) : 0;
    const int grid_rows = rows - level_rows;
    const int tone_rows = std::max(std::min(grid_rows - 1, panel.tones), 1);
    const int tones_per_row = (panel.tones + tone_rows - 1) / tone_rows;
    const int ref_row = top + k_title_rows + grid_rows - 1;
    const int level_bottom = ref_row + level_rows;
    const int labels_row = level_bottom + 1;
    const int values_row = labels_row + 1;
    const int details_row = values_row + 1;
    const int caret_row = details_row + 1;
    const int crest_level = level_row(k_reference_pct, k_level_top_pct, std::max(level_rows, 1));
    const int line_level = level_row(k_reference_line_pct, k_level_top_pct, std::max(level_rows, 1));
    const int right = axis + count * cell_width;

    if (axis > 0) {
        const int top_tone_row = ref_row - tone_rows;
        for (int row = top_tone_row; row <= level_bottom; ++row) {
            std::string label;
            if (row == ref_row)
                label = "ref";
            else if (row == ref_row - 1)
                label = "0";
            else if (row == top_tone_row)
                label = std::to_string(panel.tones - 1);
            else if (row > ref_row && level_bottom - row == crest_level)
                label = fixed(k_reference_pct, k_whole) + "%";
            else if (row > ref_row && level_bottom - row == line_level)
                label = fixed(k_reference_line_pct, k_whole) + "%";
            canvas.text(row, k_axis_label_columns - static_cast<int>(label.size()), label, Color::dim);
            canvas.put(row, k_axis_label_columns, label.empty() ? k_glyph_axis : k_glyph_axis_tick, Color::dim);
        }
        canvas.text(labels_row, 0, "slot", Color::dim);
        canvas.text(values_row, 0, "tone", Color::dim);
        canvas.text(details_row, 0, panel.detail_name, Color::dim);
    }
    if (level_rows > 0) {
        canvas.fill(level_bottom - crest_level, axis, right, k_glyph_reference, Color::dim);
        canvas.fill(level_bottom - line_level, axis, right, k_glyph_reference_line, Color::yellow);
    }

    for (int i = 0; i < count; ++i) {
        const PeakCell& cell = panel.cells[static_cast<size_t>(i)];
        const int x0 = axis + i * cell_width;
        if (cell.kind == CellKind::peak) {
            const int row = ref_row - 1 - clamp_int(cell.tone / tones_per_row, 0, tone_rows - 1);
            const bool strong = !panel.levels || cell.level_pct >= k_reference_line_pct;
            canvas.fill(row, x0, x0 + bar_width, strong ? k_glyph_full_block : k_glyph_weak_block, cell.color);
        } else if (cell.kind != CellKind::empty) {
            canvas.fill(ref_row, x0, x0 + bar_width, k_glyph_full_block, cell.color);
        }
        const int eighths = level_eighths(cell.level_pct, k_level_top_pct, level_rows);
        for (int level = 0; level < level_rows; ++level) {
            const uint32_t block = block_glyph(eighths - level * k_eighths);
            if (block != 0) canvas.fill(level_bottom - level, x0, x0 + bar_width, block, cell.color);
        }
        canvas.text(labels_row, x0, fit(cell.label, bar_width, true), Color::plain);
        canvas.text(values_row, x0, fit(cell.value, bar_width, false), cell.color);
        canvas.text(details_row, x0, fit(cell.detail, bar_width, false), Color::plain);
        if (i == panel.current) canvas.put(caret_row, x0, k_glyph_caret, Color::yellow);
    }
}

}  // namespace

// ===========================================================================
// Public
// ===========================================================================

size_t display_width(const std::string& line) {
    size_t width = 0;
    size_t index = 0;
    while (index < line.size()) {
        const uint8_t byte = static_cast<uint8_t>(line[index]);
        if (byte == k_escape) {
            ++index;
            if (index < line.size() && line[index] == k_csi_introducer) {
                ++index;
                while (index < line.size()) {
                    const uint8_t final_byte = static_cast<uint8_t>(line[index]);
                    if (final_byte >= k_csi_final_first && final_byte <= k_csi_final_last) break;
                    ++index;
                }
            }
            ++index;  // CSI final byte, or the single character of a short escape
            continue;
        }
        if ((byte & k_utf8_continuation_mask) != k_utf8_continuation) ++width;
        ++index;
    }
    return width;
}

Tui::Tui(TuiMode mode)
    : mode_(mode),
      color_(true),
      tone_hz_(0.0f),
      slot_ms_(0.0f),
      snr_db_(0.0f),
      has_snr_(false),
      bits_per_peak_(0),
      data_slots_(0),
      spacing_(Spacing::standard),
      side_(-1),
      mode_memory_(false),
      audio_head_(0),
      audio_fill_(0),
      audio_rate_hz_(0),
      audio_seconds_(0.0),
      peaks_(),
      peaks_frame_(0),
      status_(),
      has_status_(false),
      peaks_segment_(EncoderSegment::idle),
      has_peaks_(false),
      sent_flushed_(false),
      state_(DecoderState::search),
      last_byte_(),
      last_slot_(),
      has_byte_(false),
      has_slot_(false),
      bytes_(0),
      locks_(0),
      losses_(0),
      ends_(0),
      out_(nullptr),
      open_(false),
      drawn_columns_(0),
      drawn_rows_(0) {}

Tui::~Tui() {
    close();
}

void Tui::set_color(bool enabled) {
    color_ = enabled;
}

void Tui::set_profile(const std::string& profile) {
    profile_ = profile;
}

void Tui::set_tone_hz(float tone_hz) {
    tone_hz_ = tone_hz;
}

void Tui::set_slot_ms(float slot_ms) {
    slot_ms_ = slot_ms;
}

void Tui::set_mode(uint8_t bits_per_peak, uint8_t data_slots, Spacing spacing, GridSide side) {
    bits_per_peak_ = bits_per_peak;
    data_slots_ = std::min<uint8_t>(data_slots, k_max_data_slots);
    spacing_ = spacing;
    side_ = side == GridSide::above ? 1 : -1;
}

void Tui::set_field(const std::string& key, const std::string& value) {
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].first != key) continue;
        if (value.empty())
            fields_.erase(fields_.begin() + static_cast<std::ptrdiff_t>(i));
        else
            fields_[i].second = value;
        return;
    }
    if (!value.empty()) fields_.push_back(std::make_pair(key, value));
}

void Tui::push_audio(const int16_t* samples, size_t count, uint32_t sample_rate_hz) {
    if (sample_rate_hz == 0) return;
    if (sample_rate_hz != audio_rate_hz_) {
        audio_rate_hz_ = sample_rate_hz;
        const long capacity = std::lround(sample_rate_hz * k_audio_history_ms / k_ms_per_s);
        audio_.assign(static_cast<size_t>(std::max(capacity, 1L)), 0);
        audio_head_ = 0;
        audio_fill_ = 0;
    }
    const size_t capacity = audio_.size();
    for (size_t i = 0; i < count; ++i) {
        audio_[audio_head_] = samples[i];
        if (++audio_head_ == capacity) audio_head_ = 0;
    }
    audio_fill_ = std::min(capacity, audio_fill_ + count);
    audio_seconds_ += static_cast<double>(count) / sample_rate_hz;
}

void Tui::clear_peaks() {
    for (size_t i = 0; i < k_max_data_slots; ++i) peaks_[i] = PeakSlot();
    has_peaks_ = false;
    sent_flushed_ = false;
}

void Tui::append_text(uint8_t byte) {
    ++bytes_;
    text_ += static_cast<char>(byte);
    if (text_.size() > k_text_capacity) text_.erase(0, text_.size() - k_text_capacity / 2);
}

// The bytes of a sent frame from its symbols, k bits each, MSB first (spec 1.5); a short final frame of d peaks
// gives d k / 8 bytes, rounded down.
void Tui::flush_sent_frame() {
    if (!has_peaks_ || sent_flushed_ || peaks_segment_ != EncoderSegment::frame || bits_per_peak_ == 0) return;
    sent_flushed_ = true;
    size_t peaks = 0;
    for (size_t i = 0; i < k_max_data_slots; ++i)
        if (peaks_[i].present) peaks = i + 1;
    uint32_t bits = 0;
    unsigned count = 0;
    for (size_t i = 0; i < peaks; ++i) {
        bits = (bits << bits_per_peak_) | peaks_[i].symbol;
        count += bits_per_peak_;
        if (count < k_bits_per_byte) continue;
        count -= k_bits_per_byte;
        append_text(static_cast<uint8_t>(bits >> count));
        bits &= (1u << count) - 1u;
    }
}

void Tui::on_encoder_status(const EncoderStatus& status) {
    // A new transmission restarts the slot count.
    if (status.segment == EncoderSegment::idle || (has_status_ && status.slot_index < status_.slot_index)) {
        flush_sent_frame();
        clear_peaks();
    }
    status_ = status;
    has_status_ = true;
    const bool slots = status.segment == EncoderSegment::header || status.segment == EncoderSegment::frame;
    if (!slots || status.kind != SlotKind::peak) {  // a STOP ends its header or frame
        flush_sent_frame();
        return;
    }
    const uint32_t first_peak = status.slot_index - status.slot;
    if (!has_peaks_ || first_peak != peaks_frame_ || status.segment != peaks_segment_) {
        flush_sent_frame();
        clear_peaks();
        has_peaks_ = true;
        peaks_frame_ = first_peak;
        peaks_segment_ = status.segment;
    }
    if (status.slot >= k_max_data_slots) return;
    PeakSlot& peak = peaks_[status.slot];
    peak.tone = status.tone;
    peak.symbol = status.symbol;
    peak.present = true;
}

void Tui::set_mode_from(const Event& event) {
    if (event.bits_per_peak == 0) return;
    bits_per_peak_ = event.bits_per_peak;
    data_slots_ = std::min<uint8_t>(event.data_slots, k_max_data_slots);
    spacing_ = event.spacing;
    side_ = event.side;
}

void Tui::on_event(const Event& event) {
    state_ = event.state;
    const bool measured =
        event.type == EventType::locked || event.type == EventType::slot || event.type == EventType::byte;
    if (measured) {
        if (event.tone_hz > 0.0f) tone_hz_ = event.tone_hz;
        if (event.slot_ms > 0.0f) slot_ms_ = event.slot_ms;
        if (std::isfinite(event.snr_db)) {
            snr_db_ = event.snr_db;
            has_snr_ = true;
        }
        set_mode_from(event);
    }
    switch (event.type) {
    case EventType::state:
        break;
    case EventType::locked:
        ++locks_;
        mode_memory_ = (event.flags & event_flag_mode_memory) != 0;
        last_event_ = "locked";
        if (event.flags & event_flag_late_join) last_event_ += " (late join)";
        if (mode_memory_) last_event_ += " (mode memory)";
        clear_peaks();
        break;
    case EventType::slot:
        if (!has_peaks_ || event.frame_index != peaks_frame_) {
            clear_peaks();
            has_peaks_ = true;
            peaks_frame_ = event.frame_index;
        }
        if (event.index >= 1 && event.index <= k_max_data_slots) {
            PeakSlot& peak = peaks_[event.index - 1];
            peak.tone = event.tone;
            peak.symbol = event.value;
            peak.level_pct = event.level_pct;
            peak.confidence = event.confidence;
            peak.flags = event.flags;
            peak.present = true;
        }
        last_slot_ = event;
        has_slot_ = true;
        break;
    case EventType::byte:
        last_byte_ = event;
        has_byte_ = true;
        append_text(event.value);
        break;
    case EventType::end:
        ++ends_;
        last_event_ = "end";
        break;
    case EventType::lost:
        ++losses_;
        last_event_ = std::string("lost: ") + reason_name(event.reason);
        break;
    }
}

void Tui::recent_audio(size_t count, std::vector<float>& out) const {
    const size_t available = std::min(count, audio_fill_);
    out.resize(available);
    if (available == 0) return;
    const size_t capacity = audio_.size();
    size_t index = (audio_head_ + capacity - available) % capacity;
    for (size_t i = 0; i < available; ++i) {
        out[i] = audio_[index];
        if (++index == capacity) index = 0;
    }
}

std::string Tui::render(int columns, int rows) const {
    Canvas canvas(columns, rows);
    const bool encoder = mode_ == TuiMode::encoder;
    const GridView grid = {bits_per_peak_, data_slots_, spacing_, side_, tone_hz_, slot_ms_};
    const bool header = encoder && has_status_ && status_.segment == EncoderSegment::header;
    const int peak_cells = (header ? k_header_slots : grid_data_slots(grid)) + k_frame_markers;
    const int footer_rows = encoder ? k_encoder_footer_rows : k_decoder_footer_rows;
    const Layout layout = plan_layout(canvas.columns(), canvas.rows(), footer_rows, peak_cells);
    int row = 0;

    if (layout.status > 0) {
        const std::string tone = tone_hz_ > 0.0f ? fixed(tone_hz_, k_hz_decimals) + " Hz" : "tone --";
        const std::string slot = slot_ms_ > 0.0f ? "T " + fixed(slot_ms_, k_ms_decimals) + " ms " +
                                                       fixed(k_ms_per_s / slot_ms_, k_baud_decimals) + " Bd"
                                                 : "T --";
        const std::string audio = "audio " + fixed(audio_seconds_, k_seconds_decimals) + " s";
        std::vector<std::string> first;
        std::vector<std::string> second;
        first.push_back(std::string(encoder ? "TX" : "RX") + (profile_.empty() ? "" : " " + profile_));
        if (encoder) {
            first.push_back(upper(segment_name(has_status_ ? status_.segment : EncoderSegment::idle)));
            first.push_back(tone);
            first.push_back(slot);
            if (has_status_) first.push_back("slot " + std::to_string(status_.slot_index));
            second.push_back(mode_item(grid));
            second.push_back("bytes " + std::to_string(bytes_));
            second.push_back(audio);
        } else {
            first.push_back(state_name(state_));
            first.push_back(tone);
            first.push_back(slot);
            first.push_back(has_snr_ ? "SNR " + fixed(snr_db_, k_db_decimals) + " dB" : "SNR --");
            second.push_back(mode_item(grid) + (mode_memory_ && bits_per_peak_ > 0 ? " memory" : ""));
            second.push_back("bytes " + std::to_string(bytes_));
            second.push_back("lock " + std::to_string(locks_));
            second.push_back("lost " + std::to_string(losses_));
            second.push_back("end " + std::to_string(ends_));
            if (!last_event_.empty()) second.push_back(last_event_);
            second.push_back(audio);
        }
        for (size_t i = 0; i < fields_.size(); ++i) second.push_back(fields_[i].first + " " + fields_[i].second);
        draw_status(canvas, row, layout.status, join(first), join(second));
        row += layout.status;
    }

    std::vector<float> samples;
    if (layout.scope > 0) {
        const double window_ms = slot_ms_ > 0.0f ? k_scope_slots * slot_ms_ : k_default_scope_ms;
        const size_t window = std::min(
            static_cast<size_t>(std::lround(window_ms * audio_rate_hz_ / k_ms_per_s)), audio_.size());
        const size_t period = tone_hz_ > 0.0f ? static_cast<size_t>(std::ceil(audio_rate_hz_ / tone_hz_)) : 1;
        recent_audio(window, samples);
        draw_scope(canvas, row, layout.scope, samples, window, std::max<size_t>(period, 1), window_ms);
        row += k_title_rows + layout.scope;
    }

    if (layout.bars > 0) {
        const PeaksPanel panel = encoder ? encoder_panel(status_, has_status_, peaks_, has_peaks_, peaks_segment_, grid)
                                         : decoder_panel(peaks_, has_peaks_, last_slot_, grid);
        draw_peaks(canvas, row, layout.bars, panel);
        row += k_title_rows + layout.bars + footer_rows;
    }

    if (layout.spectrum > 0) {
        std::vector<double> grid_hz;
        if (header) {
            for (int tone = 0; tone < k_header_tones; ++tone)
                grid_hz.push_back(tone_frequency_hz(grid, tone, Spacing::standard));
        } else if (bits_per_peak_ > 0) {
            for (int tone = 0; tone < grid_tones(grid); ++tone)
                grid_hz.push_back(tone_frequency_hz(grid, tone, spacing_));
        }
        if (!grid_hz.empty() && grid_hz.front() <= 0.0) grid_hz.clear();
        const size_t window = static_cast<size_t>(std::lround(k_spectrum_window_ms * audio_rate_hz_ / k_ms_per_s));
        recent_audio(std::min(window, k_spectrum_max_samples), samples);
        draw_spectrum(canvas, row, layout.spectrum, samples, audio_rate_hz_, tone_hz_, grid_hz);
        row += k_title_rows + layout.spectrum + k_spectrum_axis_rows;
    }

    if (layout.text > 0) {
        std::string title = std::string(encoder ? "sent" : "received") + "  " + std::to_string(bytes_) +
                            (bytes_ == 1 ? " byte" : " bytes");
        if (!encoder && has_byte_) title += byte_flags(last_byte_.flags);
        draw_text(canvas, row, layout.text, title, text_);
    }
    return canvas.str(color_);
}

std::string Tui::frame(int columns, int rows, bool clear) const {
    // The last column is never written: a full line leaves the cursor in the pending-wrap state, where
    // clear-to-end-of-line erases the last cell and a line break on the bottom row scrolls the screen.
    const std::string body = render(std::max(columns - 1, 0), rows);
    std::string out;
    if (clear) out += k_clear_screen;
    out += k_cursor_home;
    size_t begin = 0;
    while (begin < body.size()) {
        const size_t end = body.find('\n', begin);
        if (begin > 0) out += k_line_break;
        out.append(body, begin, end - begin);
        out += k_clear_line;
        begin = end + 1;
    }
    return out;
}

bool Tui::open(std::FILE* out) {
    if (open_) return true;
    if (!is_tty(out) || !enable_vt(out)) return false;
    out_ = out;
    open_ = true;
    drawn_columns_ = 0;
    drawn_rows_ = 0;
    restore_terminal_on_exit(true);
    std::fputs(k_hide_cursor, out_);
    std::fflush(out_);
    return true;
}

void Tui::draw() {
    if (!open_) return;
    int columns = 0;
    int rows = 0;
    terminal_size(columns, rows, out_);
    const bool clear = columns != drawn_columns_ || rows != drawn_rows_;
    drawn_columns_ = columns;
    drawn_rows_ = rows;
    const std::string bytes = frame(columns, rows, clear);
    std::fwrite(bytes.data(), 1, bytes.size(), out_);
    std::fflush(out_);
}

void Tui::close() {
    if (!open_) return;
    draw();
    const std::string restore = std::string(k_reset_attributes) + k_show_cursor + k_line_break;
    std::fwrite(restore.data(), 1, restore.size(), out_);
    std::fflush(out_);
    restore_terminal_on_exit(false);
    open_ = false;
}

RefreshPacer::RefreshPacer(double rate_hz)
    : period_(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate_hz))),
      next_(Clock::now()) {}

bool RefreshPacer::due() {
    const Clock::time_point now = Clock::now();
    if (now < next_) return false;
    consume(now);
    return true;
}

void RefreshPacer::wait() {
    std::this_thread::sleep_until(next_);
    consume(Clock::now());
}

void RefreshPacer::consume(Clock::time_point now) {
    next_ += period_;
    if (next_ < now) next_ = now + period_;  // after a stall, restart the cadence instead of bursting
}

RealtimePacer::RealtimePacer(uint32_t sample_rate_hz)
    : start_(Clock::now()), samples_(0), sample_rate_hz_(sample_rate_hz) {}

void RealtimePacer::restart() {
    start_ = Clock::now();
    samples_ = 0;
}

void RealtimePacer::advance(size_t samples) {
    samples_ += samples;
    const std::chrono::duration<double> audio_time(static_cast<double>(samples_) / sample_rate_hz_);
    std::this_thread::sleep_until(start_ + std::chrono::duration_cast<Clock::duration>(audio_time));
}

}  // namespace pc
}  // namespace unlimited
