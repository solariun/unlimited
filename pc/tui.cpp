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
const float k_percent = 100.0f;
const int k_format_buffer = 64;
const unsigned k_bits_per_byte_u = k_bits_per_byte;

// Display precision (decimals)
const int k_hz_decimals = 1;
const int k_ms_decimals = 2;
const int k_db_decimals = 1;
const int k_seconds_decimals = 1;
const int k_rate_decimals = 1;
const int k_whole = 0;
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
const size_t k_separator_cells = 3;
const uint8_t k_escape = 0x1B;
const char k_csi_introducer = '[';
const uint8_t k_csi_final_first = 0x40;
const uint8_t k_csi_final_last = 0x7E;

// Unicode code points
const uint32_t k_glyph_space = 0x20;
const uint32_t k_glyph_delete = 0x7F;
const uint32_t k_glyph_c1_first = 0x80;
const uint32_t k_glyph_c1_last = 0x9F;
const uint32_t k_glyph_middle_dot = 0x00B7;      // unprintable byte, silent or unsent slot
const uint32_t k_glyph_return = 0x21B5;          // newline byte
const uint32_t k_glyph_rule = 0x2500;            // panel title rule, bracket under a byte
const uint32_t k_glyph_decision = 0x2500;        // the decision line
const uint32_t k_glyph_reference = 0x254C;       // the dashed START-STOP reference line
const uint32_t k_glyph_axis = 0x2502;
const uint32_t k_glyph_axis_tick = 0x2524;
const uint32_t k_glyph_corner_left = 0x2514;     // bracket under the bits of a byte
const uint32_t k_glyph_corner_right = 0x2518;
const uint32_t k_glyph_caret = 0x25B2;           // current slot, the pitch on the spectrum
const uint32_t k_glyph_marker = 0x25C6;          // START / STOP / sync / END marker
const uint32_t k_glyph_flywheel = 0x25C7;        // a marker the receiver did not detect, or not sent yet
const uint32_t k_glyph_tune = '~';
const uint32_t k_glyph_one = '1';
const uint32_t k_glyph_zero = '0';
const uint32_t k_glyph_band = 0x2591;            // the occupied band on the spectrum
const uint32_t k_glyph_passband_low = '[';
const uint32_t k_glyph_passband_high = ']';
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

// Layout: rows of each panel; the scope, packages, spectrum and text panels have one title row.
const int k_title_rows = 1;
const int k_status_rows_min = 1;
const int k_status_rows_preferred = 2;
const int k_status_rows_max = 3;
const int k_text_rows_min = 1;
const int k_text_rows_preferred = 2;
const int k_text_rows_max = 4;
const int k_bar_rows_min = 3;
const int k_bar_rows_preferred = 6;
const int k_bar_rows_tall = 8;
const int k_bar_rows_max = 12;
const int k_decoder_footer_rows = 2;  // bits, bytes
const int k_encoder_footer_rows = 3;  // bits, bytes, the caret on the slot being sent
const int k_spectrum_rows_min = 1;
const int k_spectrum_rows_max = 2;
const int k_spectrum_marker_rows = 1;  // passband, occupied band, pitch
const int k_spectrum_label_rows = 1;   // frequencies
const int k_scope_rows_min = 2;
const int k_scope_rows_preferred = 3;
const int k_scope_rows_tall = 5;
const int k_scope_rows_max = 8;
const int k_spectrum_min_columns = 8;
const int k_scope_min_columns = 4;
const int k_title_lead = 2;     // rule cells before a panel title
const int k_status_margin = 1;  // blank cell before the status text

// Packages panel: a cell is its bar plus one gap column; the widest cell that fits the newest package is used.
const int k_cell_widths[] = {3, 2, 1};
const int k_gap_columns = 1;
const int k_min_cell_columns = 2;
const int k_min_bracket_columns = 3;  // └─┘: narrower groups show only their label
const int k_axis_label_columns = 4;  // "100%"
const int k_axis_columns = k_axis_label_columns + 1;
const float k_reference_share = 2.0f / 3.0f;  // rows below the reference crest: the bars reach about 150 %
const float k_reference_pct = 100.0f;
const float k_weak_margin = 0.125f;     // a bit within 12.5 % of its decision line (event_flag_weak)
const int k_max_strip_slots = 40;       // data slots shown at most (spec 6.5)
const size_t k_max_packages = 40;       // decoder packages kept
const size_t k_max_byte_views = 256;
const size_t k_max_sent_slots = 256;    // encoder slots kept
const int k_no_cell = -1;
const int k_no_group = -1;
const unsigned k_group_key_shift = 32;

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
const double k_us_per_ms = 1000.0;

const size_t k_text_capacity = 1 << 16;

// ===========================================================================
// Text helpers
// ===========================================================================

enum class Color : uint8_t { plain, dim, green, yellow, amber, cyan, red, reverse };

const char* sgr(Color color) {
    switch (color) {
    case Color::plain: return "\x1b[0m";
    case Color::dim: return "\x1b[0;90m";
    case Color::green: return "\x1b[0;32m";
    case Color::yellow: return "\x1b[0;33m";
    case Color::amber: return "\x1b[0;38;5;214m";
    case Color::cyan: return "\x1b[0;36m";
    case Color::red: return "\x1b[0;31m";
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

bool printable(uint8_t byte) {
    return byte >= k_ascii_first_printable && byte < k_ascii_delete;
}

std::string fixed(double value, int decimals) {
    char buffer[k_format_buffer];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

std::string hex_byte(uint8_t value) {
    char buffer[k_format_buffer];
    std::snprintf(buffer, sizeof(buffer), "0x%02X", value);
    return buffer;
}

// "0x48 'H'", or "0x0A" for a byte that is not printable.
std::string byte_text(uint8_t value) {
    if (!printable(value)) return hex_byte(value);
    return hex_byte(value) + " '" + std::string(1, static_cast<char>(value)) + "'";
}

// Labels of a byte under its bits, widest first.
std::vector<std::string> byte_labels(uint8_t value) {
    std::vector<std::string> labels;
    labels.push_back(byte_text(value));
    if (printable(value)) {
        labels.push_back("'" + std::string(1, static_cast<char>(value)) + "'");
        labels.push_back(std::string(1, static_cast<char>(value)));
    } else {
        labels.push_back(hex_byte(value).substr(2));
    }
    return labels;
}

std::vector<std::string> labels_of(const char* wide, const char* narrow) {
    std::vector<std::string> labels;
    labels.push_back(wide);
    labels.push_back(narrow);
    return labels;
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
    case LostReason::unsupported: return "unsupported N";
    }
    return "?";
}

const char* segment_name(EncoderSegment segment) {
    switch (segment) {
    case EncoderSegment::idle: return "idle";
    case EncoderSegment::lead_in: return "lead-in";
    case EncoderSegment::tune: return "tune";
    case EncoderSegment::sync: return "sync";
    case EncoderSegment::package: return "package";
    case EncoderSegment::end: return "end";
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

// Flags of a byte or a package in words.
std::string flag_text(uint8_t flags) {
    std::string text;
    if (flags & event_flag_late_join) text += "  late join";
    if (flags & event_flag_flywheel_start) text += "  START flywheeled";
    if (flags & event_flag_flywheel_stop) text += "  STOP flywheeled";
    if (flags & event_flag_blanked) text += "  blanked";
    if (flags & event_flag_weak) text += "  weak bit";
    return text;
}

// Net rate (spec 1.7): N bits per (N + 1) slots of T.
double net_bit_rate(unsigned bits_per_package, double slot_ms) {
    return bits_per_package * k_ms_per_s / ((bits_per_package + 1) * slot_ms);
}

std::string range_text(unsigned low_hz, unsigned high_hz) {
    return std::to_string(low_hz) + "-" + std::to_string(high_hz) + " Hz";
}

// The widest pitch search of any receiver (spec 1.5): the status shift stops there until set_search_range().
Passband widest_search() {
    Passband search;
    search.low_hz = k_min_tone_hz;
    search.high_hz = k_max_tone_hz;
    return search;
}

uint16_t tone_of(float tone_hz) {
    return static_cast<uint16_t>(std::lround(tone_hz));
}

uint32_t slot_us_of(float slot_ms) {
    return static_cast<uint32_t>(std::lround(slot_ms * k_us_per_ms));
}

Band band_of(float tone_hz, float slot_ms) {
    return occupied_band(tone_of(tone_hz), slot_us_of(slot_ms));
}

// "band 1362-1638 Hz fits, shift -1062/+1062 Hz" (spec 7's bandwidth line, short): the shift stops where the band
// would leave the passband or the pitch the receiver's search (spec 1.5).
std::string band_item(float tone_hz, float slot_ms, const Passband& passband, const Passband& search) {
    const Band band = band_of(tone_hz, slot_ms);
    std::string item = "band " + range_text(band.low_hz, band.high_hz);
    if (!passband_valid(passband)) return item;
    const PassbandFit fit = passband_fit(tone_of(tone_hz), slot_us_of(slot_ms), passband, search);
    if (!fit.fits) return item + " does not fit " + range_text(passband.low_hz, passband.high_hz);
    return item + " fits, shift -" + std::to_string(fit.margin_low_hz) + "/+" + std::to_string(fit.margin_high_hz) +
           " Hz";
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

int level_eighths(double level, double top, int rows) {
    const int total = rows * k_eighths;
    return clamp_int(static_cast<int>(std::lround(level / top * total)), 0, total);
}

// Row (0 = the bottom one) of a line at `level`: the row whose top edge is nearest to it, so a line within half a
// row of a bar's top meets it, and a line near a row edge does not hop between two rows.
int level_row(double level, double top, int rows) {
    return clamp_int((level_eighths(level, top, rows) + k_eighths / 2) / k_eighths - 1, 0, rows - 1);
}

// ===========================================================================
// Layout: panels are dropped from the bottom of the priority list when rows run out
// ===========================================================================

struct Layout {
    int status;           // rows
    int scope;            // braille rows, 0 = hidden
    int bars;             // package bar rows, 0 = hidden
    int spectrum;         // spectrum bar rows, 0 = hidden
    int spectrum_labels;  // frequency label rows
    int text;             // text rows, 0 = hidden
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

Layout plan_layout(int columns, int rows, int footer_rows) {
    Layout layout = {0, 0, 0, 0, 0, 0};
    if (columns <= 0 || rows <= 0) return layout;
    int free_rows = rows - k_status_rows_min;
    layout.status = k_status_rows_min;
    // Priority: status, text, packages, spectrum, scope.
    reserve(layout.text, k_text_rows_min, k_title_rows, true, free_rows);
    reserve(layout.bars, k_bar_rows_min, k_title_rows + footer_rows, true, free_rows);
    reserve(layout.spectrum, k_spectrum_rows_min, k_title_rows + k_spectrum_marker_rows,
            columns >= k_spectrum_min_columns, free_rows);
    reserve(layout.scope, k_scope_rows_min, k_title_rows, columns >= k_scope_min_columns, free_rows);
    grow(layout.status, k_status_rows_preferred, free_rows);
    grow(layout.bars, k_bar_rows_preferred, free_rows);
    if (layout.spectrum > 0 && free_rows >= k_spectrum_label_rows) {
        layout.spectrum_labels = k_spectrum_label_rows;
        free_rows -= k_spectrum_label_rows;
    }
    grow(layout.scope, k_scope_rows_preferred, free_rows);
    grow(layout.text, k_text_rows_preferred, free_rows);
    grow(layout.status, k_status_rows_max, free_rows);
    grow(layout.bars, k_bar_rows_tall, free_rows);
    grow(layout.scope, k_scope_rows_tall, free_rows);
    grow(layout.text, k_text_rows_max, free_rows);
    grow(layout.spectrum, k_spectrum_rows_max, free_rows);
    grow(layout.bars, k_bar_rows_max, free_rows);
    grow(layout.scope, k_scope_rows_max, free_rows);
    return layout;
}

// ===========================================================================
// Status, text and scope panels
// ===========================================================================

// The items flow over the status rows; those that do not fit are left out. The first row is in reverse video.
void draw_status(Canvas& canvas, int top, int rows, const std::vector<std::string>& items) {
    const size_t width = static_cast<size_t>(std::max(canvas.columns() - k_status_margin, 0));
    std::vector<std::string> lines(static_cast<size_t>(rows));
    std::vector<size_t> used(static_cast<size_t>(rows), 0);
    size_t line = 0;
    for (size_t i = 0; i < items.size() && line < lines.size(); ++i) {
        const size_t cells = display_width(items[i]);
        if (lines[line].empty()) {
            lines[line] = items[i];
            used[line] = cells;
            continue;
        }
        if (used[line] + k_separator_cells + cells <= width) {
            lines[line] += k_separator + items[i];
            used[line] += k_separator_cells + cells;
            continue;
        }
        if (++line >= lines.size()) break;
        lines[line] = items[i];
        used[line] = cells;
    }
    canvas.fill(top, 0, canvas.columns(), k_glyph_space, Color::reverse);
    for (size_t row = 0; row < lines.size(); ++row)
        canvas.text(top + static_cast<int>(row), k_status_margin, lines[row], row == 0 ? Color::reverse : Color::plain);
}

void draw_text(Canvas& canvas, int top, int rows, const std::string& title, const std::string& text) {
    draw_title(canvas, top, title);
    const size_t width = static_cast<size_t>(canvas.columns());
    if (width == 0) return;
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

// ===========================================================================
// Spectrum strip: 300-3000 Hz, the passband, the occupied band and the pitch
// ===========================================================================

double column_hz(int column, int columns) {
    return k_spectrum_low_hz + (k_spectrum_high_hz - k_spectrum_low_hz) * column / std::max(columns - 1, 1);
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

struct SpectrumView {
    float tone_hz;  // 0: unknown
    bool has_passband;
    Passband passband;
    bool has_band;
    Band band;
};

// Goertzel bank, one bin per column, Hann window; levels in dBFS (a full-scale sine reads 0). Below the bars: the
// passband as a line between '[' and ']', the occupied band shaded (red where it leaves the passband), the pitch as
// a caret; then the frequency labels.
void draw_spectrum(Canvas& canvas, int top, int bar_rows, int label_rows, const std::vector<float>& samples,
                   uint32_t rate_hz, const SpectrumView& view) {
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

    std::string title = "spectrum";
    if (peak_column >= 0)
        title += "  peak " + fixed(column_hz(peak_column, columns), k_whole) + " Hz " +
                 fixed(dbfs[static_cast<size_t>(peak_column)], k_db_decimals) + " dBFS";
    if (view.has_passband) title += "  [ ] passband";
    if (view.has_band) title += "  \xe2\x96\x91 band";
    if (view.tone_hz > 0.0f) title += "  \xe2\x96\xb2 pitch";
    draw_title(canvas, top, title);

    const int band_low = view.has_band ? hz_column(view.band.low_hz, columns) : 0;
    const int band_high = view.has_band ? hz_column(view.band.high_hz, columns) : -1;
    const int bottom_row = top + k_title_rows + bar_rows - 1;
    const double range = k_spectrum_top_dbfs - k_spectrum_floor_dbfs;
    for (int column = 0; column < columns; ++column) {
        const double fraction = (dbfs[static_cast<size_t>(column)] - k_spectrum_floor_dbfs) / range;
        const int eighths = level_eighths(fraction, 1.0, bar_rows);
        const bool in_band = column >= band_low && column <= band_high;
        for (int level = 0; level < bar_rows; ++level) {
            const uint32_t block = block_glyph(eighths - level * k_eighths);
            if (block != 0) canvas.put(bottom_row - level, column, block, in_band ? Color::cyan : Color::green);
        }
    }

    const int marker_row = bottom_row + 1;
    const int pass_low = view.has_passband ? std::max(hz_column(view.passband.low_hz, columns), 0) : columns;
    const int pass_high = view.has_passband ? std::min(hz_column(view.passband.high_hz, columns), columns - 1) : -1;
    canvas.fill(marker_row, pass_low, pass_high + 1, k_glyph_rule, Color::dim);
    const bool fits = !view.has_passband || !view.has_band || passband_fit(view.band, view.passband).fits;
    for (int column = std::max(band_low, 0); column <= std::min(band_high, columns - 1); ++column) {
        const double hz = column_hz(column, columns);
        const bool inside = fits || (hz >= view.passband.low_hz && hz <= view.passband.high_hz);
        canvas.put(marker_row, column, k_glyph_band, inside ? Color::cyan : Color::red);
    }
    // The edges on top: where a band that spills over the passband is cut.
    if (view.has_passband && view.passband.low_hz >= k_spectrum_low_hz)
        canvas.put(marker_row, pass_low, k_glyph_passband_low, Color::plain);
    if (view.has_passband && view.passband.high_hz <= k_spectrum_high_hz)
        canvas.put(marker_row, pass_high, k_glyph_passband_high, Color::plain);
    if (view.tone_hz >= k_spectrum_low_hz && view.tone_hz <= k_spectrum_high_hz)
        canvas.put(marker_row, hz_column(view.tone_hz, columns), k_glyph_caret, Color::amber);

    if (label_rows <= 0) return;
    const int label_row = marker_row + 1;
    int next_free = 0;
    for (size_t i = 0; i < sizeof(k_spectrum_axis_hz) / sizeof(k_spectrum_axis_hz[0]); ++i) {
        const std::string label = hz_label(k_spectrum_axis_hz[i]);
        const int width = static_cast<int>(label.size());
        const int column = std::min(hz_column(k_spectrum_axis_hz[i], columns), columns - width);
        if (column < next_free) continue;
        canvas.text(label_row, column, label, Color::dim);
        next_free = column + width + 1;
    }
}

// ===========================================================================
// Packages panel: bars, reference and decision lines, bits and bytes
// ===========================================================================

enum class CellKind : uint8_t { spacer, silent, tone, marker, data, pending };

struct StripCell {
    CellKind kind;
    float level;       // bar height, % of the reference crest
    Color bar_color;
    uint32_t glyph;    // under the bar
    Color glyph_color;
    int group;         // index in Strip::groups, k_no_group for none
    bool joined;       // a continuous tone: the gap column after it is filled too
};

// One package: the reference line from the START crest to the STOP crest, and the decision line over its bits.
struct StripSpan {
    int start_cell;
    int stop_cell;
    float start_level;
    float stop_level;
    std::vector<float> lines;  // data cells start_cell + 1 .. stop_cell - 1; < 0 for none
};

struct StripGroup {           // cells under one bracket: the bits of a byte, or a segment
    std::vector<std::string> labels;  // widest first
    Color color;
};

struct Strip {
    std::string title;
    std::vector<StripCell> cells;
    std::vector<StripSpan> spans;
    std::vector<StripGroup> groups;
    std::vector<int> starts;  // cells a view may begin at (whole packages); empty: any cell
    int current;              // cell under the caret, k_no_cell for none
    int fit_cells;            // cells the chosen width must fit: the newest package with its markers
    bool caret_row;
};

StripCell strip_cell(CellKind kind, float level, Color bar_color, uint32_t glyph, Color glyph_color, int group) {
    StripCell cell;
    cell.kind = kind;
    cell.level = level;
    cell.bar_color = bar_color;
    cell.glyph = glyph;
    cell.glyph_color = glyph_color;
    cell.group = group;
    cell.joined = false;
    return cell;
}

// Consecutive cells with the same key share a group (a bracket in the label row).
class Groups {
public:
    explicit Groups(Strip& strip) : strip_(strip), key_(0), group_(k_no_group) {}

    int of(uint64_t key, const std::vector<std::string>& labels, Color color) {
        if (group_ != k_no_group && key == key_) return group_;
        StripGroup group;
        group.labels = labels;
        group.color = color;
        strip_.groups.push_back(group);
        key_ = key;
        group_ = static_cast<int>(strip_.groups.size()) - 1;
        return group_;
    }

    void end() { group_ = k_no_group; }

private:
    Strip& strip_;
    uint64_t key_;
    int group_;
};

// Keys of the groups: the transmission (or lock), whether it is a segment, then the byte index or the segment.
uint64_t group_key(uint32_t generation, bool segment, uint32_t value) {
    return (static_cast<uint64_t>(generation) << (k_group_key_shift + 1)) |
           (static_cast<uint64_t>(segment ? 1 : 0) << k_group_key_shift) | value;
}

enum class SegmentKey : uint32_t { lead_in = 1, tune, sync, end, tail };

// The group of the slots of a segment (tune, sync, END, lead-in, tail) of transmission `generation`.
int segment_group(Groups& groups, uint32_t generation, SegmentKey key, const char* wide, const char* narrow,
                  Color color) {
    return groups.of(group_key(generation, true, static_cast<uint32_t>(key)), labels_of(wide, narrow), color);
}

const Tui::ByteView* find_byte(const std::deque<Tui::ByteView>& bytes, uint32_t generation, uint32_t byte_index) {
    for (size_t i = bytes.size(); i > 0; --i) {
        const Tui::ByteView& view = bytes[i - 1];
        if (view.generation == generation && view.byte_index == byte_index) return &view;
    }
    return nullptr;
}

// Decoder: the newest packages with at most k_max_strip_slots data slots (at least one package). Levels are drawn in
// % of the running marker reference: a slot's reference is the START-STOP line at its place (spec 3.10), its bar
// level_pct of that and its decision line threshold_pct of that.
Strip decoder_strip(const std::deque<Tui::PackageView>& packages, const std::deque<Tui::ByteView>& bytes) {
    Strip strip;
    strip.current = k_no_cell;
    strip.caret_row = false;
    strip.fit_cells = 0;
    if (packages.empty()) {
        strip.title = "packages  waiting for a lock";
        return strip;
    }
    size_t first = packages.size();
    int slots = 0;
    while (first > 0) {
        const Tui::PackageView& view = packages[first - 1];
        if (first < packages.size() && slots + view.count > k_max_strip_slots) break;
        slots += view.count;
        --first;
    }
    Groups groups(strip);
    for (size_t k = first; k < packages.size(); ++k) {
        const Tui::PackageView& p = packages[k];
        const bool joined = k > first && packages[k - 1].generation == p.generation &&
                            packages[k - 1].index + 1 == p.index;
        const uint32_t marker_glyph_start = (p.flags & event_flag_flywheel_start) ? k_glyph_flywheel : k_glyph_marker;
        if (!joined) {
            if (k > first) strip.cells.push_back(strip_cell(CellKind::spacer, 0.0f, Color::plain, k_glyph_space,
                                                            Color::plain, k_no_group));
            groups.end();
            strip.cells.push_back(strip_cell(CellKind::marker, p.start_pct, Color::cyan, marker_glyph_start,
                                             Color::cyan, k_no_group));
        } else {
            strip.cells.back().level = p.start_pct;  // the previous STOP is this START
        }
        StripSpan span;
        span.start_cell = static_cast<int>(strip.cells.size()) - 1;
        span.start_level = p.start_pct;
        span.stop_level = p.stop_pct;
        strip.starts.push_back(span.start_cell);
        for (uint8_t i = 0; i < p.count; ++i) {
            const SlotBar& slot = p.slots[i];
            const float reference =
                p.start_pct + (static_cast<float>(p.stop_pct) - p.start_pct) * (i + 1) / (p.count + 1);
            const float level = slot.level_pct * reference / k_percent;
            const float line = slot.threshold_pct > 0 ? slot.threshold_pct * reference / k_percent : -1.0f;
            const float margin = std::fabs(static_cast<float>(slot.level_pct) - slot.threshold_pct);
            const bool weak = slot.threshold_pct > 0 && margin < k_weak_margin * slot.threshold_pct;
            const Color color = weak ? Color::yellow : (slot.bit ? Color::green : Color::dim);
            int group = k_no_group;
            if (p.bits_per_package > 0) {
                const uint32_t bit = p.index * p.bits_per_package + i;
                const uint32_t byte_index = bit / k_bits_per_byte_u;
                const Tui::ByteView* byte = find_byte(bytes, p.generation, byte_index);
                group = groups.of(group_key(p.generation, false, byte_index),
                                  byte != nullptr ? byte_labels(byte->value) : std::vector<std::string>(),
                                  Color::plain);
            }
            strip.cells.push_back(strip_cell(CellKind::data, level, color, slot.bit ? k_glyph_one : k_glyph_zero,
                                             slot.bit ? Color::green : Color::plain, group));
            span.lines.push_back(line);
        }
        const uint32_t marker_glyph_stop = (p.flags & event_flag_flywheel_stop) ? k_glyph_flywheel : k_glyph_marker;
        strip.cells.push_back(strip_cell(CellKind::marker, p.stop_pct, Color::cyan, marker_glyph_stop, Color::cyan,
                                         k_no_group));
        span.stop_cell = static_cast<int>(strip.cells.size()) - 1;
        strip.spans.push_back(span);
    }

    const Tui::PackageView& newest = packages.back();
    strip.fit_cells = newest.count + 2;
    float line_sum = 0.0f;
    int lines = 0;
    for (uint8_t i = 0; i < newest.count; ++i) {
        if (newest.slots[i].threshold_pct == 0) continue;
        line_sum += newest.slots[i].threshold_pct;
        ++lines;
    }
    strip.title = "package " + std::to_string(newest.index) + "  " + std::to_string(newest.count) +
                  (newest.count == 1 ? " bit" : " bits");
    if (newest.slot_ms > 0.0f) strip.title += "  T " + fixed(newest.slot_ms, k_ms_decimals) + " ms";
    strip.title += "  START " + std::to_string(newest.start_pct) + "%  STOP " + std::to_string(newest.stop_pct) + "%";
    if (lines > 0) strip.title += "  line " + fixed(line_sum / lines, k_whole) + "% of ref";
    strip.title += flag_text(newest.flags & static_cast<uint8_t>(~event_flag_weak));
    return strip;
}

// Encoder: the slots sent so far in this transmission, and the rest of the package being sent (not sent yet: its
// bits in the byte being sent are known, the others are dots), with the caret on the slot being sent.
Strip encoder_strip(const std::deque<Tui::SentSlot>& sent, uint32_t generation, const EncoderStatus& status,
                    bool has_status, uint8_t bits_per_package) {
    Strip strip;
    strip.current = k_no_cell;
    strip.caret_row = true;
    const int package_cells = (bits_per_package > 0 ? bits_per_package : k_hf_bits_per_package) + 2;
    strip.fit_cells = package_cells;
    Groups groups(strip);
    const float beep = k_reference_pct;
    int tune_slots = 0;
    int sync_markers = 0;
    int end_markers = 0;
    for (size_t i = 0; i < sent.size(); ++i) {
        if (sent[i].generation != generation) continue;
        const EncoderStatus& s = sent[i].status;
        const bool marker = s.segment == EncoderSegment::sync || s.segment == EncoderSegment::end ||
                            (s.segment == EncoderSegment::package && s.slot > s.package_bits);
        if (marker || strip.cells.empty()) strip.starts.push_back(static_cast<int>(strip.cells.size()));
        switch (s.segment) {
        case EncoderSegment::idle:
            break;
        case EncoderSegment::lead_in:
            strip.cells.push_back(strip_cell(CellKind::silent, 0.0f, Color::plain, k_glyph_middle_dot, Color::dim,
                                             segment_group(groups, generation, SegmentKey::lead_in, "lead-in", "lead",
                                                           Color::dim)));
            break;
        case EncoderSegment::tune: {
            StripCell cell = strip_cell(CellKind::tone, beep, Color::green, k_glyph_tune, Color::green,
                                        segment_group(groups, generation, SegmentKey::tune, "tune tone", "tune",
                                                      Color::green));
            cell.joined = true;
            strip.cells.push_back(cell);
            ++tune_slots;
            break;
        }
        case EncoderSegment::sync:
            strip.cells.push_back(strip_cell(CellKind::marker, beep, Color::cyan, k_glyph_marker, Color::cyan,
                                             segment_group(groups, generation, SegmentKey::sync, "sync train", "sync",
                                                           Color::cyan)));
            ++sync_markers;
            break;
        case EncoderSegment::package:
            if (s.slot >= 1 && s.slot <= s.package_bits) {
                const bool one = s.kind == SlotKind::one;
                const int group =
                    groups.of(group_key(generation, false, s.byte_index), byte_labels(s.byte), Color::plain);
                strip.cells.push_back(strip_cell(CellKind::data, one ? beep : 0.0f, Color::green,
                                                 one ? k_glyph_one : k_glyph_zero, one ? Color::green : Color::plain,
                                                 group));
            } else {  // the STOP: a byte whose bits span two packages keeps one bracket across it
                strip.cells.push_back(strip_cell(CellKind::marker, beep, Color::cyan, k_glyph_marker, Color::cyan,
                                                 k_no_group));
            }
            break;
        case EncoderSegment::end:
            strip.cells.push_back(strip_cell(CellKind::marker, beep, Color::cyan, k_glyph_marker, Color::cyan,
                                             segment_group(groups, generation, SegmentKey::end, "END", "E",
                                                           Color::cyan)));
            ++end_markers;
            break;
        case EncoderSegment::tail:
            strip.cells.push_back(strip_cell(CellKind::silent, 0.0f, Color::plain, k_glyph_middle_dot, Color::dim,
                                             segment_group(groups, generation, SegmentKey::tail, "tail", "t",
                                                           Color::dim)));
            break;
        }
    }
    const bool sending = has_status && status.segment != EncoderSegment::idle && !strip.cells.empty();
    if (sending) strip.current = static_cast<int>(strip.cells.size()) - 1;

    // The rest of the package being sent.
    if (sending && status.segment == EncoderSegment::package && status.slot >= 1 &&
        status.slot <= status.package_bits) {
        const int group = strip.cells.back().group;
        for (int slot = status.slot + 1; slot <= status.package_bits; ++slot) {
            const int bit_index = status.bit_index + (slot - status.slot);
            if (bit_index < static_cast<int>(k_bits_per_byte)) {
                const bool one = ((status.byte >> (k_bits_per_byte - 1 - bit_index)) & 1u) != 0;
                strip.cells.push_back(strip_cell(CellKind::pending, 0.0f, Color::dim, one ? k_glyph_one : k_glyph_zero,
                                                 Color::dim, group));
            } else {
                strip.cells.push_back(strip_cell(CellKind::pending, 0.0f, Color::dim, k_glyph_middle_dot, Color::dim,
                                                 k_no_group));
            }
        }
        strip.cells.push_back(strip_cell(CellKind::pending, 0.0f, Color::dim, k_glyph_flywheel, Color::dim,
                                         k_no_group));
    }

    if (!has_status || status.segment == EncoderSegment::idle) {
        strip.title = strip.cells.empty() ? "idle" : "idle  the transmission has ended";
        return strip;
    }
    switch (status.segment) {
    case EncoderSegment::package:
        strip.title = "package " + std::to_string(status.package_index) + "  slot " + std::to_string(status.slot) +
                      "/" + std::to_string(status.package_bits + 1);
        if (status.slot >= 1 && status.slot <= status.package_bits)
            strip.title += "  bit " + std::to_string(status.bit_index) + " of byte " +
                           std::to_string(status.byte_index) + " = " + byte_text(status.byte);
        else
            strip.title += "  STOP (the next START)";
        break;
    case EncoderSegment::tune:
        strip.title = "tune tone  slot " + std::to_string(tune_slots);
        break;
    case EncoderSegment::sync:
        strip.title = "sync train  marker " + std::to_string(sync_markers) + " (the last one is START)";
        break;
    case EncoderSegment::end:
        strip.title = "END  marker " + std::to_string(end_markers) + "/" + std::to_string(k_end_markers);
        break;
    case EncoderSegment::lead_in:
        strip.title = "lead-in  silence";
        break;
    case EncoderSegment::tail:
        strip.title = "tail  silence";
        break;
    case EncoderSegment::idle:
        break;
    }
    return strip;
}

// Glyph of a line in its row.
void put_line(Canvas& canvas, int bottom_row, int bar_rows, float top, int column, float level, uint32_t glyph,
              Color color) {
    if (level < 0.0f) return;
    canvas.put(bottom_row - level_row(level, top, bar_rows), column, glyph, color);
}

// `text` centered in [from, to] when it fits.
bool centered_text(Canvas& canvas, int row, int from, int to, const std::string& text, Color color) {
    const int width = static_cast<int>(display_width(text));
    const int room = to - from + 1;
    if (width > room || width == 0) return false;
    canvas.text(row, from + (room - width) / 2, text, color);
    return true;
}

// Title, the bars over rows [top + 1, top + bar_rows], then the bits, the byte brackets and (encoder) the caret.
void draw_strip(Canvas& canvas, int top, int bar_rows, const Strip& strip) {
    draw_title(canvas, top, strip.title);
    const int count = static_cast<int>(strip.cells.size());
    const int columns = canvas.columns();
    if (count == 0 || bar_rows <= 0 || columns <= 0) return;
    const int fit = std::max(strip.fit_cells, 1);
    const int axis = columns >= fit * k_min_cell_columns + k_axis_columns ? k_axis_columns : 0;
    const int available = columns - axis;
    int width = k_cell_widths[sizeof(k_cell_widths) / sizeof(k_cell_widths[0]) - 1];
    for (size_t i = 0; i < sizeof(k_cell_widths) / sizeof(k_cell_widths[0]); ++i) {
        if (fit * k_cell_widths[i] > available) continue;
        width = k_cell_widths[i];
        break;
    }
    const int bar_width = width > k_gap_columns ? width - k_gap_columns : width;
    const int capacity = std::max(available / width, 1);
    int first = std::max(count - capacity, 0);
    for (size_t i = 0; i < strip.starts.size(); ++i) {
        if (strip.starts[i] < first) continue;
        first = strip.starts[i];
        break;
    }

    // The scale puts the reference crest (100 %) on a row boundary, about two thirds up.
    const int reference_rows = std::max(1, static_cast<int>(std::lround(bar_rows * k_reference_share)));
    const float scale_top = k_reference_pct * bar_rows / reference_rows;
    const int bottom_row = top + k_title_rows + bar_rows - 1;
    const int bits_row = bottom_row + 1;
    const int labels_row = bits_row + 1;
    const int caret_row = labels_row + 1;
    struct Placement {
        int first;
        int axis;
        int width;
        int x(int cell) const { return axis + (cell - first) * width; }
    };
    const Placement place = {first, axis, width};

    if (axis > 0) {
        const int reference_row = reference_rows - 1;
        for (int level = 0; level < bar_rows; ++level) {
            const int row = bottom_row - level;
            if (level == reference_row) canvas.text(row, 0, fixed(k_reference_pct, k_whole) + "%", Color::dim);
            canvas.put(row, k_axis_label_columns, level == reference_row ? k_glyph_axis_tick : k_glyph_axis,
                       Color::dim);
        }
    }

    // The lines of each package: the dashed reference from the START crest to the STOP crest, the decision line over
    // the bits. Remembered per column so that a bar top below a line leaves the line visible.
    std::vector<float> reference(static_cast<size_t>(columns), -1.0f);
    std::vector<float> decision(static_cast<size_t>(columns), -1.0f);
    for (size_t s = 0; s < strip.spans.size(); ++s) {
        const StripSpan& span = strip.spans[s];
        if (span.start_cell < first) continue;
        const int start_x = place.x(span.start_cell);
        const int stop_x = place.x(span.stop_cell);
        const double start_centre = start_x + (bar_width - 1) * k_half;
        const double stop_centre = stop_x + (bar_width - 1) * k_half;
        for (int x = start_x + bar_width; x < stop_x && x < columns; ++x) {
            const double position = (x - start_centre) / std::max(stop_centre - start_centre, 1.0);
            reference[static_cast<size_t>(x)] =
                static_cast<float>(span.start_level + (span.stop_level - span.start_level) * position);
            int cell = first + (x - axis) / width;
            cell = std::min(std::max(cell, span.start_cell + 1), span.stop_cell - 1);
            const float line = span.lines[static_cast<size_t>(cell - span.start_cell - 1)];
            if (line >= 0.0f) decision[static_cast<size_t>(x)] = line;
        }
    }
    for (int x = 0; x < columns; ++x) {
        put_line(canvas, bottom_row, bar_rows, scale_top, x, reference[static_cast<size_t>(x)], k_glyph_reference,
                 Color::dim);
        put_line(canvas, bottom_row, bar_rows, scale_top, x, decision[static_cast<size_t>(x)], k_glyph_decision,
                 Color::amber);
    }

    for (int c = first; c < count; ++c) {
        const StripCell& cell = strip.cells[static_cast<size_t>(c)];
        const int x0 = place.x(c);
        const bool joined = cell.joined && c + 1 < count && strip.cells[static_cast<size_t>(c) + 1].joined;
        const int x1 = x0 + (joined ? width : bar_width);
        if (cell.level > 0.0f) {
            const int eighths = level_eighths(cell.level, scale_top, bar_rows);
            for (int level = 0; level < bar_rows; ++level) {
                const int filled = eighths - level * k_eighths;
                const uint32_t block = block_glyph(filled);
                if (block == 0) break;
                for (int x = x0; x < x1 && x < columns; ++x) {
                    // A line in this row above the bar's top stays visible: the bar does not reach it.
                    const float lines[] = {reference[static_cast<size_t>(x)], decision[static_cast<size_t>(x)]};
                    bool line_above = false;
                    for (size_t l = 0; l < sizeof(lines) / sizeof(lines[0]); ++l)
                        line_above = line_above ||
                                     (lines[l] >= 0.0f && level_row(lines[l], scale_top, bar_rows) == level &&
                                      level_eighths(lines[l], scale_top, bar_rows) > eighths);
                    if (filled < k_eighths && line_above) continue;
                    canvas.put(bottom_row - level, x, block, cell.bar_color);
                }
            }
        }
        canvas.put(bits_row, x0 + (bar_width - 1) / 2, cell.glyph, cell.glyph_color);
    }

    // Brackets under the bits of each byte (or segment), its value in the middle.
    std::vector<int> group_first(strip.groups.size(), k_no_cell);
    std::vector<int> group_last(strip.groups.size(), k_no_cell);
    std::vector<bool> group_cut(strip.groups.size(), false);
    for (int c = 0; c < count; ++c) {
        const int group = strip.cells[static_cast<size_t>(c)].group;
        if (group == k_no_group) continue;
        const size_t g = static_cast<size_t>(group);
        if (c < first) {
            group_cut[g] = true;
            continue;
        }
        if (group_first[g] == k_no_cell) group_first[g] = c;
        group_last[g] = c;
    }
    for (size_t g = 0; g < strip.groups.size(); ++g) {
        if (group_first[g] == k_no_cell) continue;
        const StripGroup& group = strip.groups[g];
        const int from = place.x(group_first[g]);
        const int to = place.x(group_last[g]) + bar_width - 1;
        const bool bracket = to - from + 1 >= k_min_bracket_columns;
        if (bracket) {
            canvas.fill(labels_row, from, to + 1, k_glyph_rule, Color::dim);
            if (!group_cut[g]) canvas.put(labels_row, from, k_glyph_corner_left, Color::dim);
            canvas.put(labels_row, to, k_glyph_corner_right, Color::dim);
        }
        // Inside the corners when there is a bracket, else the label alone.
        const int label_from = bracket ? from + 1 : from;
        const int label_to = bracket ? to - 1 : to;
        for (size_t l = 0; l < group.labels.size(); ++l)
            if (centered_text(canvas, labels_row, label_from, label_to, group.labels[l], group.color)) break;
    }

    if (strip.caret_row && strip.current >= first)
        canvas.put(caret_row, place.x(strip.current) + (bar_width - 1) / 2, k_glyph_caret, Color::amber);
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
      bits_per_package_(0),
      passband_(),
      has_passband_(false),
      search_(widest_search()),
      audio_head_(0),
      audio_fill_(0),
      audio_rate_hz_(0),
      audio_seconds_(0.0),
      generation_(0),
      status_(),
      has_status_(false),
      sent_bits_(0),
      sent_bit_count_(0),
      state_(DecoderState::search),
      last_flags_(0),
      late_join_(false),
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

void Tui::set_package(uint8_t bits_per_package) {
    bits_per_package_ = std::min<uint8_t>(bits_per_package, k_max_bits_per_package);
}

void Tui::set_passband(const Passband& passband) {
    passband_ = passband;
    has_passband_ = true;
}

void Tui::set_search_range(const Passband& search) {
    search_ = search;
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

void Tui::append_text(uint8_t byte) {
    ++bytes_;
    text_ += static_cast<char>(byte);
    if (text_.size() > k_text_capacity) text_.erase(0, text_.size() - k_text_capacity / 2);
}

void Tui::new_generation() {
    ++generation_;
}

// Every new slot is kept; its bit, when it carries one, goes into the byte being rebuilt (MSB first, spec 2.2).
void Tui::on_encoder_status(const EncoderStatus& status) {
    if (status.segment == EncoderSegment::idle) {
        status_ = status;
        has_status_ = true;
        return;
    }
    const bool restarted =
        !has_status_ || status_.segment == EncoderSegment::idle || status.slot_index < status_.slot_index;
    if (restarted) {
        new_generation();
        sent_bits_ = 0;
        sent_bit_count_ = 0;
    } else if (status.slot_index == status_.slot_index) {
        status_ = status;
        return;
    }
    status_ = status;
    has_status_ = true;
    const SentSlot slot = {status, generation_};
    sent_.push_back(slot);
    while (sent_.size() > k_max_sent_slots) sent_.pop_front();
    if (status.segment != EncoderSegment::package || status.slot < 1 || status.slot > status.package_bits) return;
    sent_bits_ = static_cast<uint8_t>((sent_bits_ << 1) | (status.kind == SlotKind::one ? 1u : 0u));
    if (++sent_bit_count_ < k_bits_per_byte) return;
    append_text(sent_bits_);
    sent_bits_ = 0;
    sent_bit_count_ = 0;
}

Tui::PackageView& Tui::open_package(const Event& event) {
    if (packages_.empty() || packages_.back().complete || packages_.back().generation != generation_ ||
        packages_.back().index != event.package_index) {
        PackageView view = PackageView();
        view.generation = generation_;
        view.index = event.package_index;
        view.bits_per_package = event.bits_per_package;
        packages_.push_back(view);
        while (packages_.size() > k_max_packages) packages_.pop_front();
    }
    return packages_.back();
}

void Tui::add_slot(const Event& event) {
    PackageView& view = open_package(event);
    if (event.slot < 1 || event.slot > k_max_bits_per_package) return;
    const SlotBar bar = {event.level_pct, event.threshold_pct, event.value, event.flags};
    view.slots[event.slot - 1] = bar;
    view.count = std::max(view.count, event.slot);
    view.start_pct = event.start_pct;
    view.stop_pct = event.stop_pct;
    view.flags = event.flags;
}

void Tui::finish_package(const Event& event) {
    PackageView& view = open_package(event);
    view.count = std::min<uint8_t>(event.value, k_max_bits_per_package);
    view.start_pct = event.start_pct;
    view.stop_pct = event.stop_pct;
    view.flags = event.flags;
    view.slot_ms = event.slot_ms;
    view.complete = true;
}

void Tui::on_event(const Event& event) {
    state_ = event.state;
    const bool measured = event.type == EventType::locked || event.type == EventType::slot ||
                          event.type == EventType::package || event.type == EventType::byte;
    if (measured) {
        if (event.tone_hz > 0.0f) tone_hz_ = event.tone_hz;
        if (event.slot_ms > 0.0f && event.type != EventType::package) slot_ms_ = event.slot_ms;
        if (event.bits_per_package > 0) bits_per_package_ = event.bits_per_package;
        if (std::isfinite(event.snr_db) && event.state != DecoderState::search) {
            snr_db_ = event.snr_db;
            has_snr_ = true;
        }
    }
    switch (event.type) {
    case EventType::state:
        if (event.state == DecoderState::search || event.state == DecoderState::acquire) new_generation();
        break;
    case EventType::locked:
        ++locks_;
        late_join_ = (event.flags & event_flag_late_join) != 0;
        last_event_ = late_join_ ? "locked (late join)" : "locked";
        break;
    case EventType::slot:
        add_slot(event);
        break;
    case EventType::package:
        finish_package(event);
        break;
    case EventType::byte: {
        const ByteView view = {generation_, event.byte_index, event.value};
        byte_views_.push_back(view);
        while (byte_views_.size() > k_max_byte_views) byte_views_.pop_front();
        last_flags_ = event.flags;
        append_text(event.value);
        break;
    }
    case EventType::end:
        ++ends_;
        last_event_ = "end";
        new_generation();
        break;
    case EventType::lost:
        ++losses_;
        last_event_ = std::string("lost (") + reason_name(event.reason) + ")";
        new_generation();
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

std::vector<std::string> Tui::status_items() const {
    const bool encoder = mode_ == TuiMode::encoder;
    std::vector<std::string> items;
    items.push_back(std::string(encoder ? "TX" : "RX") + (profile_.empty() ? "" : " " + profile_));
    if (encoder) {
        items.push_back(upper(segment_name(has_status_ ? status_.segment : EncoderSegment::idle)));
    } else {
        items.push_back(std::string(state_name(state_)) +
                        (state_ == DecoderState::search ? ", DCD off" : ", DCD on"));
    }
    items.push_back(tone_hz_ > 0.0f ? fixed(tone_hz_, encoder ? k_whole : k_hz_decimals) + " Hz" : "pitch --");
    items.push_back(slot_ms_ > 0.0f ? "T " + fixed(slot_ms_, k_ms_decimals) + " ms" : "T --");
    items.push_back(bits_per_package_ > 0 ? "N " + std::to_string(bits_per_package_) : "N --");
    if (bits_per_package_ > 0 && slot_ms_ > 0.0f)
        items.push_back(fixed(net_bit_rate(bits_per_package_, slot_ms_), k_rate_decimals) + " bit/s");
    if (encoder) {
        if (has_status_ && status_.segment == EncoderSegment::package)
            items.push_back("package " + std::to_string(status_.package_index));
    } else {
        items.push_back(has_snr_ ? "SNR " + fixed(snr_db_, k_db_decimals) + " dB" : "SNR --");
    }
    if (tone_hz_ > 0.0f && slot_ms_ > 0.0f)
        items.push_back(band_item(tone_hz_, slot_ms_, has_passband_ ? passband_ : Passband(), search_));
    if (has_passband_) items.push_back("passband " + range_text(passband_.low_hz, passband_.high_hz));
    items.push_back(std::string(encoder ? "sent " : "") + std::to_string(bytes_) + (bytes_ == 1 ? " byte" : " bytes"));
    if (!encoder) {
        items.push_back("locks " + std::to_string(locks_) + "  lost " + std::to_string(losses_) + "  ends " +
                        std::to_string(ends_));
        if (!last_event_.empty()) items.push_back("last: " + last_event_);
    }
    items.push_back("audio " + fixed(audio_seconds_, k_seconds_decimals) + " s");
    for (size_t i = 0; i < fields_.size(); ++i) items.push_back(fields_[i].first + " " + fields_[i].second);
    return items;
}

std::string Tui::render(int columns, int rows) const {
    Canvas canvas(columns, rows);
    const bool encoder = mode_ == TuiMode::encoder;
    const int footer_rows = encoder ? k_encoder_footer_rows : k_decoder_footer_rows;
    const Layout layout = plan_layout(canvas.columns(), canvas.rows(), footer_rows);
    int row = 0;

    if (layout.status > 0) {
        draw_status(canvas, row, layout.status, status_items());
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
        const Strip strip = encoder ? encoder_strip(sent_, generation_, status_, has_status_, bits_per_package_)
                                    : decoder_strip(packages_, byte_views_);
        draw_strip(canvas, row, layout.bars, strip);
        row += k_title_rows + layout.bars + footer_rows;
    }

    if (layout.spectrum > 0) {
        SpectrumView view;
        view.tone_hz = tone_hz_;
        view.has_passband = has_passband_;
        view.passband = passband_;
        view.has_band = tone_hz_ > 0.0f && slot_ms_ > 0.0f;
        view.band = view.has_band ? band_of(tone_hz_, slot_ms_) : Band();
        const size_t window = static_cast<size_t>(std::lround(k_spectrum_window_ms * audio_rate_hz_ / k_ms_per_s));
        recent_audio(std::min(window, k_spectrum_max_samples), samples);
        draw_spectrum(canvas, row, layout.spectrum, layout.spectrum_labels, samples, audio_rate_hz_, view);
        row += k_title_rows + layout.spectrum + k_spectrum_marker_rows + layout.spectrum_labels;
    }

    if (layout.text > 0) {
        std::string title = std::string(encoder ? "sent" : "received") + "  " + std::to_string(bytes_) +
                            (bytes_ == 1 ? " byte" : " bytes");
        if (!encoder && bytes_ > 0) title += flag_text(last_flags_);
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
