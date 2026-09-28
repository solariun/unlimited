#include "terminal.hpp"
#include "test_harness.hpp"
#include "tui.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using unlimited::DecoderConfig;
using unlimited::DecoderState;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSegment;
using unlimited::EncoderStatus;
using unlimited::Event;
using unlimited::EventType;
using unlimited::LostReason;
using unlimited::Passband;
using unlimited::pc::Level;
using unlimited::pc::LevelMeter;
using unlimited::pc::StatusRing;
using unlimited::pc::Tui;
using unlimited::pc::TuiMode;
using unlimited::pc::display_width;
using unlimited::pc::level_text;

namespace {

using std::int16_t;
using std::size_t;
using std::uint32_t;
using std::uint8_t;
using test::count_of;

typedef std::vector<uint32_t> Cells;
typedef std::chrono::steady_clock Clock;

const double k_pi = 3.14159265358979323846;
const uint32_t k_rate = 8000;
const uint32_t k_high_rate = 48000;
const double k_tone_hz = 1500.0;
const float k_received_hz = 1580.0f;  // 80 Hz mistuned
const double k_half_scale = 16384.0;
const double k_near_full_scale = 30000.0;
const double k_half_scale_dbfs = -6.02;
const double k_scalloping_tolerance_db = 1.5;  // Hann window, tone up to half a bin off the column frequency
const float k_speed = 6.0f;                    // bytes/s
const float k_slot_ms = 16.667f;               // T at 6 bytes/s
const float k_snr_db = 12.5f;
const uint8_t k_bits = 8;
const int k_window_cells = 10;
const int k_columns = 80;
const int k_rows = 24;
const int k_wide_columns = 120;
const int k_tall_rows = 30;
const Passband k_ssb_passband = {300, 2700};
const Passband k_am_passband = {100, 3000};

// Levels and decision lines of the slot events (% of the reference line at the slot).
const uint8_t k_one_pct = 100;
const uint8_t k_zero_pct = 10;
const uint8_t k_line_pct = 70;
const uint8_t k_flat_pct = 100;
const uint8_t k_faded_pct = 20;  // a START under half the running reference: missing

// The 80 x 24 decoder layout (plan_layout): 7 bar rows, the reference crest (100 %) on the 5th from the bottom, 140 %
// at the top, 20 % per row.
const int k_bar_rows = 7;
const int k_reference_row = 4;  // from the bottom
const int k_line_row = 3;       // 70 %: nearest row edge 80 %, the 4th row
const int k_cell_columns = 3;   // bar of 2 columns, 1 gap

const uint32_t k_escape = 0x1B;
const uint32_t k_space = 0x20;
const uint32_t k_decision_glyph = 0x2500;   // ─
const uint32_t k_reference_glyph = 0x254C;  // ╌
const uint32_t k_full_block = 0x2588;
const uint32_t k_lower_eighth = 0x2581;
const uint32_t k_caret = 0x25B2;            // ▲
const uint32_t k_marker = 0x25C6;           // ◆
const uint32_t k_missing_marker = 0x25C7;   // ◇
const uint32_t k_lead_glyph = '~';
const uint32_t k_middle_dot = 0x00B7;       // ·
const uint32_t k_band_glyph = 0x2591;       // ░
const uint32_t k_braille_first = 0x2800;
const uint32_t k_braille_last = 0x28FF;
const uint32_t k_braille_left_dots = 0x47;   // dots 1, 2, 3, 7
const uint32_t k_braille_right_dots = 0xB8;  // dots 4, 5, 6, 8
const int k_eighths = 8;

const double k_refresh_hz = 20.0;
const long long k_refresh_period_ms = 50;
const size_t k_pacer_chunk = 400;
const long long k_pacer_expected_ms = 100;  // 2 chunks of 400 samples at 8 kHz
const long long k_timing_slack_ms = 1000;   // upper bound only guards against a stuck sleep
const size_t k_render_step = 16;            // samples per Encoder::render() in the encoder tests

// --- text helpers (independent of the implementation) ---------------------

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t begin = 0;
    while (begin < text.size()) {
        const size_t end = text.find('\n', begin);
        if (end == std::string::npos) {
            lines.push_back(text.substr(begin));
            break;
        }
        lines.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    return lines;
}

std::string strip_escapes(const std::string& line) {
    std::string out;
    size_t i = 0;
    while (i < line.size()) {
        if (static_cast<uint8_t>(line[i]) != k_escape) {
            out += line[i++];
            continue;
        }
        i += 2;  // ESC [
        while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) ++i;
        ++i;
    }
    return out;
}

std::string trim_right(const std::string& text) {
    size_t end = text.size();
    while (end > 0 && text[end - 1] == ' ') --end;
    return text.substr(0, end);
}

Cells cells_of(const std::string& line) {
    const std::string text = strip_escapes(line);
    Cells cells;
    size_t i = 0;
    while (i < text.size()) {
        const uint8_t lead = static_cast<uint8_t>(text[i]);
        size_t length = 1;
        uint32_t code = lead;
        if (lead >= 0xF0) {
            length = 4;
            code = lead & 0x07;
        } else if (lead >= 0xE0) {
            length = 3;
            code = lead & 0x0F;
        } else if (lead >= 0xC0) {
            length = 2;
            code = lead & 0x1F;
        }
        for (size_t k = 1; k < length && i + k < text.size(); ++k)
            code = (code << 6) | (static_cast<uint8_t>(text[i + k]) & 0x3F);
        cells.push_back(code);
        i += length;
    }
    return cells;
}

int find_in(const Cells& cells, const std::string& needle, int from = 0) {
    const Cells pattern = cells_of(needle);
    if (from < 0 || static_cast<size_t>(from) > cells.size()) return -1;
    const Cells::const_iterator found = std::search(cells.begin() + from, cells.end(), pattern.begin(), pattern.end());
    return found == cells.end() ? -1 : static_cast<int>(found - cells.begin());
}

bool contains(const std::string& line, const std::string& needle) {
    return find_in(cells_of(line), needle) >= 0;
}

int find_line(const std::vector<std::string>& lines, const std::string& needle, int from = 0) {
    for (size_t i = static_cast<size_t>(std::max(from, 0)); i < lines.size(); ++i)
        if (contains(lines[i], needle)) return static_cast<int>(i);
    return -1;
}

uint32_t cell_at(const std::string& line, int column) {
    const Cells cells = cells_of(line);
    if (column < 0 || static_cast<size_t>(column) >= cells.size()) return k_space;
    return cells[static_cast<size_t>(column)];
}

int count_glyph(const std::string& line, uint32_t glyph) {
    const Cells cells = cells_of(line);
    return static_cast<int>(std::count(cells.begin(), cells.end(), glyph));
}

int block_eighths(uint32_t glyph) {
    if (glyph == k_full_block) return k_eighths;
    if (glyph >= k_lower_eighth && glyph < k_full_block) return static_cast<int>(glyph - k_lower_eighth) + 1;
    return 0;
}

bool is_braille(uint32_t glyph) {
    return glyph >= k_braille_first && glyph <= k_braille_last;
}

// Columns of the non-blank cells of a line.
std::vector<int> glyph_columns(const std::string& line) {
    const Cells cells = cells_of(line);
    std::vector<int> columns;
    for (size_t i = 0; i < cells.size(); ++i)
        if (cells[i] != k_space) columns.push_back(static_cast<int>(i));
    return columns;
}

// The text of a line's cells at `columns`, one glyph each.
Cells glyphs_at(const std::string& line, const std::vector<int>& columns) {
    Cells glyphs;
    for (size_t i = 0; i < columns.size(); ++i) glyphs.push_back(cell_at(line, columns[i]));
    return glyphs;
}

std::vector<std::string> render_lines(const Tui& tui, int columns, int rows) {
    return split_lines(tui.render(columns, rows));
}

// The bits row of the windows panel: the first line below `title` holding a marker glyph.
int bits_row(const std::vector<std::string>& lines, int title) {
    for (size_t i = static_cast<size_t>(title) + 1; i < lines.size(); ++i)
        if (count_glyph(lines[i], k_marker) > 0 || count_glyph(lines[i], k_missing_marker) > 0)
            return static_cast<int>(i);
    return -1;
}

// Row `level` of the bars (0 = the bottom one), in a column.
uint32_t bar_cell(const std::vector<std::string>& lines, int bits, int level, int column) {
    return cell_at(lines[static_cast<size_t>(bits - 1 - level)], column);
}

// The glyphs of the bits row as text: M a marker, P a missing marker, ~ the VOX lead, . a silent slot, the bits.
std::string glyph_text(const Cells& glyphs) {
    std::string text;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        const uint32_t glyph = glyphs[i];
        if (glyph == k_marker)
            text += 'M';
        else if (glyph == k_missing_marker)
            text += 'P';
        else if (glyph == k_middle_dot)
            text += '.';
        else if (glyph < 0x80)
            text += static_cast<char>(glyph);
        else
            text += '?';
    }
    return text;
}

// --- fixtures -----------------------------------------------------------------

std::vector<int16_t> tone(double hz, uint32_t rate, size_t count, double amplitude) {
    std::vector<int16_t> samples(count);
    for (size_t i = 0; i < count; ++i)
        samples[i] = static_cast<int16_t>(std::lround(amplitude * std::sin(2.0 * k_pi * hz * i / rate)));
    return samples;
}

Event make_event(EventType type, DecoderState state) {
    Event event = Event();
    event.type = type;
    event.state = state;
    return event;
}

// An event of a lock at 6 bytes/s received 80 Hz high.
Event lock_event(EventType type) {
    Event event = make_event(type, DecoderState::track);
    event.tone_hz = k_received_hz;
    event.slot_ms = k_slot_ms;
    event.snr_db = k_snr_db;
    return event;
}

Event slot_event(uint32_t window, uint8_t slot, uint8_t bit, uint8_t level, uint8_t threshold, uint8_t start,
                 uint8_t stop, uint8_t flags) {
    Event event = lock_event(EventType::slot);
    event.byte_index = window;
    event.slot = slot;
    event.value = bit;
    event.level_pct = level;
    event.threshold_pct = threshold;
    event.start_pct = start;
    event.stop_pct = stop;
    event.flags = flags;
    return event;
}

Event byte_event(uint8_t value, uint32_t window, uint8_t start = k_flat_pct, uint8_t stop = k_flat_pct,
                 uint8_t flags = 0) {
    Event event = lock_event(EventType::byte);
    event.value = value;
    event.byte_index = window;
    event.start_pct = start;
    event.stop_pct = stop;
    event.flags = flags;
    return event;
}

// One window as the decoder reports it (spec 3.5): the slot events of its 8 data slots (ones at k_one_pct, zeros at
// k_zero_pct, all with the same decision line), then its byte event unless it is a framing error.
void send_window(Tui& tui, uint32_t index, uint8_t byte, uint8_t threshold = k_line_pct, uint8_t start = k_flat_pct,
                 uint8_t stop = k_flat_pct, uint8_t flags = 0) {
    for (uint8_t i = 0; i < k_bits; ++i) {
        const uint8_t bit = static_cast<uint8_t>((byte >> (k_bits - 1 - i)) & 1u);
        tui.on_event(slot_event(index, static_cast<uint8_t>(i + 1), bit, bit ? k_one_pct : k_zero_pct, threshold,
                                start, stop, flags));
    }
    if ((flags & unlimited::event_flag_framing) == 0) tui.on_event(byte_event(byte, index, start, stop, flags));
}

void send_text(Tui& tui, const std::string& text, uint32_t first = 0) {
    for (size_t i = 0; i < text.size(); ++i)
        send_window(tui, first + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
}

void feed_decoder(Tui& tui) {
    const std::vector<int16_t> audio = tone(k_tone_hz, k_rate, k_rate / 4, k_half_scale);
    tui.push_audio(audio.data(), audio.size(), k_rate);
    tui.set_speed(k_speed);
    tui.set_passband(k_ssb_passband);
    tui.on_event(lock_event(EventType::locked));
    send_text(tui, "CQ CQ DE UNLIMITED");
}

// Renders `data` with a real encoder and hands the Tui its status after every k_render_step samples (`repeats`
// times each, as a demo might), stopping when `stop` returns true. Returns the samples rendered.
template <typename Stop>
size_t feed_encoder(Tui& tui, const EncoderConfig& config, const std::vector<uint8_t>& data, int repeats, Stop stop) {
    Encoder encoder(config);
    size_t written = encoder.write(data.data(), data.size());
    if (!encoder.start()) return 0;
    int16_t chunk[k_render_step];
    size_t samples = 0;
    while (true) {
        if (written < data.size()) written += encoder.write(data.data() + written, data.size() - written);
        const size_t rendered = encoder.render(chunk, k_render_step);
        if (rendered == 0) break;
        samples += rendered;
        tui.push_audio(chunk, rendered, config.sample_rate_hz);
        const EncoderStatus status = encoder.status();
        for (int r = 0; r < repeats; ++r) tui.on_encoder_status(status);
        if (stop(status)) break;
    }
    tui.on_encoder_status(encoder.status());
    return samples;
}

struct Never {
    bool operator()(const EncoderStatus&) const { return false; }
};

// 6 bytes/s at 1500 Hz in 300..2700 Hz, 8 kHz.
EncoderConfig six_config() {
    EncoderConfig config;
    config.sample_rate_hz = k_rate;
    return config;
}

void encoder_view(Tui& tui, const EncoderConfig& config) {
    tui.set_color(false);
    tui.set_label("test");
    tui.set_speed(unlimited::bytes_per_second(config.slot_us));
    tui.set_tone_hz(config.tone_hz);
    tui.set_slot_ms(config.slot_us / 1000.0f);
    tui.set_passband(config.passband);
    tui.set_search_range(unlimited::search_range(config));
}

std::vector<uint8_t> bytes_of(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

// Column of the spectrum strip at a frequency: 300..3000 Hz over the columns.
int spectrum_column(double hz, int columns) {
    return static_cast<int>(std::lround((hz - 300.0) / 2700.0 * (columns - 1)));
}

}  // namespace

// ---------------------------------------------------------------------------
// U20: terminal and TUI rendering
// ---------------------------------------------------------------------------

TEST(tui_terminal_size_falls_back_when_not_a_tty) {
    std::FILE* file = std::tmpfile();
    REQUIRE(file != nullptr);
    int columns = 0;
    int rows = 0;
    CHECK(!unlimited::pc::terminal_size(columns, rows, file));
    CHECK_EQ(columns, unlimited::pc::k_default_terminal_columns);
    CHECK_EQ(rows, unlimited::pc::k_default_terminal_rows);
    CHECK_EQ(columns, 80);
    CHECK_EQ(rows, 24);
    CHECK(!unlimited::pc::is_tty(file));
#ifndef _WIN32
    CHECK(unlimited::pc::enable_vt(file));  // POSIX: no-op
#endif
    std::fclose(file);
}

extern "C" void tui_test_signal_handler(int) {}

TEST(tui_terminal_restore_hooks_only_default_signals) {
    typedef void (*Handler)(int);
    const Handler original = std::signal(SIGINT, SIG_DFL);
    unlimited::pc::restore_terminal_on_exit(true);
    const Handler while_armed = std::signal(SIGINT, SIG_DFL);
    CHECK(while_armed != SIG_DFL);  // a default SIGINT is hooked to restore the cursor
    std::signal(SIGINT, while_armed);
    unlimited::pc::restore_terminal_on_exit(false);
    CHECK(std::signal(SIGINT, SIG_DFL) == SIG_DFL);

    std::signal(SIGINT, tui_test_signal_handler);
    unlimited::pc::restore_terminal_on_exit(true);
    CHECK(std::signal(SIGINT, tui_test_signal_handler) == tui_test_signal_handler);  // left alone
    unlimited::pc::restore_terminal_on_exit(false);
    CHECK(std::signal(SIGINT, SIG_IGN) == tui_test_signal_handler);

    unlimited::pc::restore_terminal_on_exit(true);
    CHECK(std::signal(SIGINT, SIG_IGN) == SIG_IGN);  // an ignored SIGINT stays ignored
    unlimited::pc::restore_terminal_on_exit(false);
    CHECK(std::signal(SIGINT, original) == SIG_IGN);
}

TEST(tui_open_refuses_a_non_tty) {
    std::FILE* file = std::tmpfile();
    REQUIRE(file != nullptr);
    {
        Tui tui(TuiMode::decoder);
        feed_decoder(tui);
        CHECK(!tui.open(file));
        tui.draw();
        tui.close();
    }
    CHECK_EQ(std::ftell(file), 0L);
    std::fclose(file);
}

TEST(tui_display_width_skips_escapes) {
    CHECK_EQ(display_width(""), 0u);
    CHECK_EQ(display_width("abc"), 3u);
    CHECK_EQ(display_width("\x1b[0;32mab\x1b[0m"), 2u);
    CHECK_EQ(display_width("\x1b[0;38;5;214m\xe2\x94\x80\x1b[0m"), 1u);  // amber decision line
    CHECK_EQ(display_width("\xe2\xa0\xbf\xe2\x96\x88"), 2u);             // braille + full block
    CHECK_EQ(display_width("\x1b[K"), 0u);
}

TEST(tui_render_respects_size) {
    const int columns[] = {0, 1, 2, 5, 10, 19, 20, 24, 25, 40, 60, 80, 132, 200};
    const int rows[] = {0, 1, 2, 3, 5, 8, 12, 17, 24, 40, 60};
    Tui decoder(TuiMode::decoder);
    feed_decoder(decoder);
    decoder.set_field("expect", "a long field value that will not fit in narrow terminals at all");
    decoder.set_field("raw", "\x1b[2J\t\a\r\n\xc2\x9b");  // control characters must never reach the terminal
    Tui encoder(TuiMode::encoder);
    const EncoderConfig config = six_config();
    encoder_view(encoder, config);
    feed_encoder(encoder, config, bytes_of("CQ CQ DE UNLIMITED"), 1, [](const EncoderStatus& status) {
        return status.segment == EncoderSegment::window && status.byte_index == 7;
    });
    Tui vox(TuiMode::encoder);  // in the VOX lead
    EncoderConfig vox_config = six_config();
    vox_config.vox_lead_ms = unlimited::k_default_vox_lead_ms;
    encoder_view(vox, vox_config);
    feed_encoder(vox, vox_config, bytes_of("Hi"), 1, [](const EncoderStatus& status) {
        return status.segment == EncoderSegment::vox_lead && status.slot_index == 4;
    });
    Tui faded(TuiMode::decoder);  // fades, dropped windows, weak bits, new locks
    const uint32_t windows = 60;
    for (uint32_t k = 0; k < windows; ++k) {
        const bool dropped = k % 7 == 3;
        const uint8_t flags = dropped ? unlimited::event_flag_framing : (k % 5 == 1 ? unlimited::event_flag_weak : 0);
        if (k % 13 == 12) faded.on_event(make_event(EventType::end, DecoderState::search));
        send_window(faded, k, static_cast<uint8_t>(k * 37 + 11), static_cast<uint8_t>(50 + k % 41),
                    static_cast<uint8_t>(dropped ? k_faded_pct : 60 + k % 90), static_cast<uint8_t>(40 + k % 200),
                    flags);
    }
    Tui* const views[] = {&decoder, &encoder, &vox, &faded};
    int violations = 0;
    for (size_t v = 0; v < count_of(views); ++v) {
        for (size_t c = 0; c < count_of(columns); ++c) {
            for (size_t r = 0; r < count_of(rows); ++r) {
                views[v]->set_color(false);
                const std::string plain = views[v]->render(columns[c], rows[r]);
                views[v]->set_color(true);
                const std::string colored = views[v]->render(columns[c], rows[r]);
                const std::vector<std::string> plain_lines = split_lines(plain);
                const std::vector<std::string> colored_lines = split_lines(colored);
                const size_t expected_rows = static_cast<size_t>(rows[r]);
                if (plain_lines.size() != expected_rows || colored_lines.size() != expected_rows) {
                    ++violations;
                    continue;
                }
                CHECK_EQ(static_cast<size_t>(std::count(plain.begin(), plain.end(), '\n')), expected_rows);
                if (plain.find('\x1b') != std::string::npos) ++violations;
                for (size_t i = 0; i < expected_rows; ++i) {
                    const size_t limit = static_cast<size_t>(columns[c]);
                    if (display_width(plain_lines[i]) > limit || display_width(colored_lines[i]) > limit)
                        ++violations;
                    if (trim_right(strip_escapes(colored_lines[i])) != trim_right(plain_lines[i])) ++violations;
                }
            }
        }
    }
    CHECK_EQ(violations, 0);
}

// The picture of spec 3.5: the START and STOP bars, the dashed reference line from crest to crest, the decision line
// (70 % of the reference line), one bar per data slot with its bit under it and the byte under the bits.
TEST(tui_decoder_window_picture) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    tui.on_event(lock_event(EventType::locked));
    send_window(tui, 0, 'H');
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);

    const int title = find_line(lines, "window 0  byte 0x48 'H'");
    REQUIRE(title >= 0);
    CHECK(contains(lines[static_cast<size_t>(title)], "START 100%  STOP 100%  line 70% of ref"));
    const int bits = bits_row(lines, title);
    REQUIRE(bits == title + k_bar_rows + 1);
    const std::vector<int> columns = glyph_columns(lines[static_cast<size_t>(bits)]);
    REQUIRE(columns.size() == static_cast<size_t>(k_window_cells));
    CHECK_EQ(glyph_text(glyphs_at(lines[static_cast<size_t>(bits)], columns)), std::string("M01001000M"));
    for (size_t i = 1; i < columns.size(); ++i) CHECK_EQ(columns[i] - columns[i - 1], k_cell_columns);
    CHECK(contains(lines[static_cast<size_t>(bits) + 1], "0x48 'H'"));
    CHECK(find_in(cells_of(lines[static_cast<size_t>(bits - 1 - k_reference_row)]), "100%") == 0);

    const std::string h_bits = "01001000";
    for (size_t i = 0; i < columns.size(); ++i) {
        const int column = columns[i];
        const bool marker = i == 0 || i + 1 == columns.size();
        const bool one = !marker && h_bits[i - 1] == '1';
        if (marker || one) {
            // START, STOP and the ones reach the crest: full up to the reference row, nothing above.
            for (int level = 0; level <= k_reference_row; ++level)
                CHECK_EQ(bar_cell(lines, bits, level, column), k_full_block);
            for (int level = k_reference_row + 1; level < k_bar_rows; ++level)
                CHECK_EQ(bar_cell(lines, bits, level, column), k_space);
        } else {
            // A zero is a short bar under both lines, which stay visible above it.
            CHECK_EQ(block_eighths(bar_cell(lines, bits, 0, column)), 4);  // 10 % of 20 % rows
            CHECK_EQ(bar_cell(lines, bits, k_line_row, column), k_decision_glyph);
            CHECK_EQ(bar_cell(lines, bits, k_reference_row, column), k_reference_glyph);
        }
        if (i + 1 < columns.size()) {  // the gap after each cell: both lines, between START and STOP only
            CHECK_EQ(bar_cell(lines, bits, k_reference_row, column + 2), k_reference_glyph);
            CHECK_EQ(bar_cell(lines, bits, k_line_row, column + 2), k_decision_glyph);
        }
    }
    CHECK_EQ(bar_cell(lines, bits, k_reference_row, columns.back() + 2), k_space);  // no line after STOP
    CHECK_EQ(bar_cell(lines, bits, k_reference_row, columns.front() - 1), uint32_t(0x2524));  // the axis tick ┤

    // In colour the decision line is amber.
    tui.set_color(true);
    const std::vector<std::string> colored = render_lines(tui, k_columns, k_rows);
    CHECK(colored[static_cast<size_t>(bits - 1 - k_line_row)].find("\x1b[0;38;5;214m") != std::string::npos);
}

// A fade across the window: START at 60 %, STOP at 120 %. The reference line climbs from the START crest to the STOP
// crest and the decision line (70 % of it) follows.
TEST(tui_decoder_reference_line_follows_the_crests) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    const uint8_t start = 60;
    const uint8_t stop = 120;
    send_window(tui, 3, 0x00, k_line_pct, start, stop);
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    const int title = find_line(lines, "window 3");
    REQUIRE(title >= 0);
    const int bits = bits_row(lines, title);
    REQUIRE(bits > title);
    const std::vector<int> columns = glyph_columns(lines[static_cast<size_t>(bits)]);
    REQUIRE(columns.size() == static_cast<size_t>(k_window_cells));
    // The line row of a glyph in a column: the lowest row from the top holding it.
    struct Finder {
        const std::vector<std::string>& lines;
        int title;
        int bits;
        int row_of(uint32_t glyph, int column) const {
            for (int row = title + 1; row < bits; ++row)
                if (cell_at(lines[static_cast<size_t>(row)], column) == glyph) return row;
            return -1;
        }
    };
    const Finder find = {lines, title, bits};
    const int first_gap = columns.front() + 2;
    const int last_gap = columns[columns.size() - 2] + 2;
    const int reference_left = find.row_of(k_reference_glyph, first_gap);
    const int reference_right = find.row_of(k_reference_glyph, last_gap);
    const int decision_left = find.row_of(k_decision_glyph, first_gap);
    const int decision_right = find.row_of(k_decision_glyph, last_gap);
    REQUIRE(reference_left > 0);
    REQUIRE(reference_right > 0);
    REQUIRE(decision_left > 0);
    REQUIRE(decision_right > 0);
    NOTE("reference line rows %d -> %d, decision line rows %d -> %d (screen lines)", reference_left, reference_right,
         decision_left, decision_right);
    CHECK(reference_right < reference_left);  // higher on the screen near the stronger STOP
    CHECK(decision_right < decision_left);
    CHECK(decision_left > reference_left);    // the decision line stays under the reference line
    CHECK(decision_right > reference_right);
    // The START bar ends lower than the STOP bar.
    int start_top = 0;
    int stop_top = 0;
    for (int level = 0; level < k_bar_rows; ++level) {
        if (bar_cell(lines, bits, level, columns.front()) != k_space) start_top = level;
        if (bar_cell(lines, bits, level, columns.back()) != k_space) stop_top = level;
    }
    CHECK(stop_top > start_top);
}

// Windows follow each other (spec 1.2): window 5's START comes right after window 4's STOP. A window that never came
// (6) leaves an empty cell, and so does a new lock even when its first window's index follows.
TEST(tui_decoder_windows_follow_each_other) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    send_window(tui, 4, 'H');
    send_window(tui, 5, 'i');
    send_window(tui, 7, '!');
    const std::vector<std::string> lines = render_lines(tui, k_wide_columns, k_rows);
    const int title = find_line(lines, "window 7  byte 0x21 '!'");
    REQUIRE(title >= 0);
    const int bits = bits_row(lines, title);
    REQUIRE(bits > title);
    const std::vector<int> columns = glyph_columns(lines[static_cast<size_t>(bits)]);
    CHECK_EQ(glyph_text(glyphs_at(lines[static_cast<size_t>(bits)], columns)),
             std::string("M01001000MM01101001MM00100001M"));
    REQUIRE(columns.size() == static_cast<size_t>(3 * k_window_cells));
    CHECK_EQ(columns[10] - columns[9], k_cell_columns);       // 4's STOP, 5's START: adjacent
    CHECK_EQ(columns[20] - columns[19], 2 * k_cell_columns);  // an empty cell for the missing window 6
    const std::string& labels = lines[static_cast<size_t>(bits) + 1];
    CHECK(contains(labels, "0x48 'H'"));
    CHECK(contains(labels, "0x69 'i'"));
    CHECK(contains(labels, "0x21 '!'"));
    CHECK_EQ(cell_at(labels, columns[1]), uint32_t(0x2514));      // └ under the first bit of window 4
    CHECK_EQ(cell_at(labels, columns[8] + 1), uint32_t(0x2518));  // ┘ under its last bit
    CHECK_EQ(cell_at(labels, columns[9]), k_space);               // nothing under a marker

    Tui relock(TuiMode::decoder);
    relock.set_color(false);
    send_window(relock, 7, 'H');
    relock.on_event(make_event(EventType::end, DecoderState::search));
    relock.on_event(make_event(EventType::state, DecoderState::acquire));
    relock.on_event(lock_event(EventType::locked));
    send_window(relock, 8, 'i');
    const std::vector<std::string> relocked = render_lines(relock, k_wide_columns, k_rows);
    const int relock_bits = bits_row(relocked, find_line(relocked, "window 8"));
    REQUIRE(relock_bits > 0);
    const std::vector<int> relock_columns = glyph_columns(relocked[static_cast<size_t>(relock_bits)]);
    REQUIRE(relock_columns.size() == static_cast<size_t>(2 * k_window_cells));
    CHECK_EQ(relock_columns[10] - relock_columns[9], 2 * k_cell_columns);
}

// A window whose START is missing is a framing error (spec 3.6): its bits are drawn, its START hollow, "dropped" in red
// under its bits and no byte; the status counts it.
TEST(tui_decoder_marks_a_dropped_window) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    tui.on_event(lock_event(EventType::locked));
    send_window(tui, 0, 'H');
    send_window(tui, 1, 'i', k_line_pct, k_faded_pct, k_flat_pct, unlimited::event_flag_framing);
    std::vector<std::string> lines = render_lines(tui, k_wide_columns, k_tall_rows);
    const int title = find_line(lines, "window 1  dropped  START 20%  STOP 100%  line 70% of ref  framing error");
    REQUIRE(title >= 0);
    const int bits = bits_row(lines, title);
    REQUIRE(bits > title);
    const std::vector<int> columns = glyph_columns(lines[static_cast<size_t>(bits)]);
    CHECK_EQ(glyph_text(glyphs_at(lines[static_cast<size_t>(bits)], columns)), std::string("M01001000MP01101001M"));
    const std::string& labels = lines[static_cast<size_t>(bits) + 1];
    CHECK(contains(labels, "0x48 'H'"));
    CHECK(contains(labels, "dropped"));
    CHECK(!contains(labels, "0x69"));
    CHECK(contains(lines[0] + lines[1] + lines[2], "dropped 1"));
    CHECK(find_line(lines, "received  1 byte") > bits);

    tui.set_color(true);
    lines = render_lines(tui, k_wide_columns, k_tall_rows);
    const std::string& colored = lines[static_cast<size_t>(bits) + 1];
    const size_t red = colored.find("\x1b[0;31m");
    REQUIRE(red != std::string::npos);
    CHECK(colored.find("dropped", red) != std::string::npos);
}

// The decoder view's status: the state and DCD as the receiver has it (set_dcd(): Decoder::dcd() or Modem::dcd(), on
// while tracking, spec 3.10), the counts and the received text.
TEST(tui_decoder_status_counts_and_text) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    tui.set_label("test.wav");
    tui.set_speed(k_speed);
    tui.set_passband(k_ssb_passband);
    tui.set_field("channel", "usb 10 dB");
    std::vector<std::string> lines = render_lines(tui, k_wide_columns, k_tall_rows);
    CHECK(contains(lines[0], "RX test.wav"));
    CHECK(contains(lines[0], "SEARCH, DCD off"));
    CHECK(contains(lines[0], "6.00 bytes/s, 48.0 bit/s"));
    CHECK(contains(lines[0], "pitch --"));
    CHECK(find_line(lines, "windows  waiting for a lock") > 0);

    tui.on_event(make_event(EventType::state, DecoderState::acquire));
    CHECK(contains(render_lines(tui, k_wide_columns, k_tall_rows)[0], "ACQUIRE, DCD off"));  // a candidate: no DCD
    tui.on_event(lock_event(EventType::locked));
    tui.set_dcd(true);  // as the programs do after each event: Decoder::dcd(), on while tracking (spec 3.10)
    const uint8_t bytes[] = {'H', 'i', '\n', 0x07};
    const uint8_t last_flags = unlimited::event_flag_weak | unlimited::event_flag_blanked;
    for (size_t i = 0; i < count_of(bytes); ++i)
        send_window(tui, static_cast<uint32_t>(i), bytes[i], k_line_pct, k_flat_pct, k_flat_pct,
                    i + 1 == count_of(bytes) ? last_flags : 0);
    lines = render_lines(tui, k_wide_columns, k_tall_rows);
    const std::string status = lines[0] + lines[1] + lines[2];
    CHECK(contains(status, "TRACK, DCD on"));
    CHECK(contains(status, "1580.0 Hz"));
    CHECK(contains(status, "T 16.67 ms"));
    CHECK(contains(status, "SNR 12.5 dB"));
    CHECK(contains(status, "band 1448-1712 Hz fits, shift -1148/+988 Hz"));  // received band in the ssb passband
    CHECK(contains(status, "passband 300-2700 Hz"));
    CHECK(contains(status, "4 bytes"));
    CHECK(contains(status, "locks 1  lost 0  ends 0  dropped 0"));
    CHECK(contains(status, "last: locked"));
    CHECK(contains(status, "channel usb 10 dB"));
    const int text_title = find_line(lines, "received");
    REQUIRE(text_title > 0);
    CHECK(contains(lines[static_cast<size_t>(text_title)], "4 bytes  blanked  weak bit"));
    CHECK(contains(lines[static_cast<size_t>(text_title) + 1], "Hi\xe2\x86\xb5\xc2\xb7"));  // "Hi↵·"
    CHECK(find_line(lines, "window 3  byte 0x07  START 100%  STOP 100%  line 70% of ref  blanked") > 0);

    tui.on_event(make_event(EventType::end, DecoderState::search));
    tui.on_event(make_event(EventType::state, DecoderState::search));
    tui.set_dcd(false);
    Event lost = make_event(EventType::lost, DecoderState::acquire);
    lost.reason = LostReason::framing;
    tui.on_event(lost);
    tui.set_field("channel", "");
    lines = render_lines(tui, k_wide_columns, k_tall_rows);
    const std::string after = lines[0] + lines[1] + lines[2];
    CHECK(contains(after, "ACQUIRE, DCD off"));
    CHECK(contains(after, "locks 1  lost 1  ends 1"));
    CHECK(contains(after, "last: lost (framing errors)"));
    CHECK(!contains(after, "channel"));
}

// The encoder view adds a window's byte to the sent text when its STOP is sent (spec 2.2), with or without a VOX lead.
TEST(tui_encoder_shows_the_sent_bytes) {
    const std::string first = "Hello";
    const std::string second = " World";
    const uint16_t vox_leads[] = {0, unlimited::k_default_vox_lead_ms};
    for (size_t v = 0; v < count_of(vox_leads); ++v) {
        EncoderConfig config = six_config();
        config.vox_lead_ms = vox_leads[v];
        Tui tui(TuiMode::encoder);
        encoder_view(tui, config);
        feed_encoder(tui, config, bytes_of(first), 3, Never());   // repeated statuses count once
        feed_encoder(tui, config, bytes_of(second), 1, Never());  // a new transmission restarts slot_index
        const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
        const int title = find_line(lines, "sent  11 bytes");
        if (!CHECK(title > 0)) {
            NOTE("VOX lead %u ms", unsigned(vox_leads[v]));
            continue;
        }
        CHECK(contains(lines[static_cast<size_t>(title) + 1], first + second));
        CHECK(contains(lines[0] + lines[1] + lines[2], "sent 11 bytes"));
        CHECK(find_line(lines, "idle  the transmission has ended") > 0);
    }
    // A byte is sent once its STOP is: the second byte's last data slot leaves one byte sent.
    const EncoderConfig config = six_config();
    Tui partial(TuiMode::encoder);
    encoder_view(partial, config);
    feed_encoder(partial, config, bytes_of("Hi"), 1, [](const EncoderStatus& status) {
        return status.segment == EncoderSegment::window && status.byte_index == 1 && status.slot == k_bits;
    });
    CHECK(find_line(render_lines(partial, k_columns, k_rows), "sent  1 byte") > 0);
}

// The window being sent: the windows before it, its START and the bits sent so far, the caret on the current slot,
// its bits not sent yet, its STOP not sent yet (hollow) and the bytes under the bits.
TEST(tui_encoder_shows_the_window_being_sent) {
    const EncoderConfig config = six_config();
    Tui tui(TuiMode::encoder);
    encoder_view(tui, config);
    const uint32_t window = 1;
    const uint8_t slot = 3;
    feed_encoder(tui, config, bytes_of("Hi!"), 1, [&](const EncoderStatus& status) {
        return status.segment == EncoderSegment::window && status.byte_index == window && status.slot == slot;
    });
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    const std::string status = lines[0] + lines[1];
    CHECK(contains(status, "TX test"));
    CHECK(contains(status, "WINDOW"));
    CHECK(contains(status, "6.00 bytes/s, 48.0 bit/s"));
    CHECK(contains(status, "1500 Hz"));
    CHECK(contains(status, "T 16.67 ms"));
    CHECK(contains(status, "window 1"));
    const int title = find_line(lines, "window 1  slot 3/9  bit 2 of byte 1 = 0x69 'i'");
    REQUIRE(title > 0);
    const int bits = bits_row(lines, title);
    REQUIRE(bits > title);
    const std::vector<int> columns = glyph_columns(lines[static_cast<size_t>(bits)]);
    const Cells glyphs = glyphs_at(lines[static_cast<size_t>(bits)], columns);
    // Window 0 whole; window 1: START, 011 sent, 01001 (the rest of 'i') and its STOP not sent yet.
    const std::string sent = "M01001000MM01101001P";
    REQUIRE(glyphs.size() == sent.size());
    CHECK_EQ(glyph_text(glyphs), sent);
    const int caret = find_in(cells_of(lines[static_cast<size_t>(bits) + 2]), "\xe2\x96\xb2");
    CHECK_EQ(caret, columns[static_cast<size_t>(k_window_cells + slot)]);
    CHECK_EQ(count_glyph(lines[static_cast<size_t>(bits) + 2], k_caret), 1);
    // Bars: a beep is a full bar up to the crest, a silence none, the slots not sent yet none.
    CHECK_EQ(bar_cell(lines, bits, 0, columns[k_window_cells]), k_full_block);      // window 1's START
    CHECK_EQ(bar_cell(lines, bits, 0, columns[k_window_cells + 1]), k_space);       // slot 1 = 0
    CHECK_EQ(bar_cell(lines, bits, 0, columns[k_window_cells + 2]), k_full_block);  // slot 2 = 1
    CHECK_EQ(bar_cell(lines, bits, 0, columns[k_window_cells + 5]), k_space);       // slot 5 = 1, not sent yet
    const std::string& labels = lines[static_cast<size_t>(bits) + 1];
    CHECK(contains(labels, "0x48 'H'"));
    CHECK(contains(labels, "0x69 'i'"));
    CHECK(find_line(lines, "sent  1 byte") > bits);  // the text panel: 'H' so far
}

// The segments before the first START and after the last STOP are named and bracketed (spec 2.1): the lead-in, the
// VOX lead's steady tone, the 2-slot gap and the tail; the view ends idle.
TEST(tui_encoder_names_the_segments) {
    EncoderConfig config = six_config();
    config.vox_lead_ms = unlimited::k_default_vox_lead_ms;  // 9 slots at 6 bytes/s
    const uint32_t vox_slots = 9;
    const uint8_t vox_stop = 3;
    Tui lead(TuiMode::encoder);
    encoder_view(lead, config);
    uint8_t seen = 0;
    uint32_t last_slot = 0;
    feed_encoder(lead, config, bytes_of("Hi"), 1, [&](const EncoderStatus& status) {
        if (status.segment != EncoderSegment::vox_lead) return false;
        if (seen == 0 || status.slot_index != last_slot) ++seen;
        last_slot = status.slot_index;
        return seen == vox_stop;
    });
    std::vector<std::string> lines = render_lines(lead, k_columns, k_rows);
    int title = find_line(lines, "VOX lead  slot 3");
    REQUIRE(title > 0);
    int bits = bits_row(lines, title);
    CHECK(bits < 0);  // no marker yet: the bits row holds the lead's glyphs only
    CHECK(contains(lines[0], "VOX LEAD"));

    Tui gap(TuiMode::encoder);
    encoder_view(gap, config);
    feed_encoder(gap, config, bytes_of("Hi"), 1,
                 [](const EncoderStatus& status) { return status.segment == EncoderSegment::gap; });
    lines = render_lines(gap, k_columns, k_rows);
    title = find_line(lines, "gap  silence before the first START");
    REQUIRE(title > 0);
    // The lead's glyphs and the gap's first slot on the bits row, below the bars.
    int glyph_row = -1;
    for (size_t i = static_cast<size_t>(title) + 1; i < lines.size(); ++i)
        if (count_glyph(lines[i], k_lead_glyph) > 0) {
            glyph_row = static_cast<int>(i);
            break;
        }
    REQUIRE(glyph_row > title);
    const std::vector<int> columns = glyph_columns(lines[static_cast<size_t>(glyph_row)]);
    CHECK_EQ(glyph_text(glyphs_at(lines[static_cast<size_t>(glyph_row)], columns)),
             std::string(vox_slots, '~') + ".");
    const std::string& labels = lines[static_cast<size_t>(glyph_row) + 1];
    CHECK(contains(labels, "VOX lead"));
    // The lead is one steady tone: its bars fill the gap columns between its slots too.
    CHECK_EQ(bar_cell(lines, glyph_row, 0, columns[0] + 2), k_full_block);

    Tui tail(TuiMode::encoder);
    encoder_view(tail, config);
    feed_encoder(tail, config, bytes_of("Hi"), 1,
                 [](const EncoderStatus& status) { return status.segment == EncoderSegment::tail; });
    lines = render_lines(tail, k_columns, k_rows);
    REQUIRE(find_line(lines, "tail  silence") > 0);
    CHECK(contains(lines[0], "TAIL"));

    Tui ended(TuiMode::encoder);
    encoder_view(ended, config);
    feed_encoder(ended, config, bytes_of("Hi"), 1, Never());
    lines = render_lines(ended, k_columns, k_rows);
    CHECK(find_line(lines, "idle  the transmission has ended") > 0);
    CHECK(contains(lines[0], "IDLE"));
}

// Spec 1.3: the status shift is the one a receiver follows: it also stops where the pitch would leave the pitches the
// receiver searches (never below 300 Hz or above 2700 Hz), not only at the filter's edges.
TEST(tui_status_shift_follows_the_search) {
    EncoderConfig config = six_config();
    config.slot_us = unlimited::slot_us_for_speed(1.0f);
    config.passband = k_am_passband;
    Tui encoder(TuiMode::encoder);
    encoder_view(encoder, config);
    std::vector<std::string> lines = render_lines(encoder, k_wide_columns, k_tall_rows);
    CHECK(contains(lines[0] + lines[1] + lines[2], "1.00 bytes/s, 8.0 bit/s"));
    CHECK(contains(lines[0] + lines[1] + lines[2], "band 1478-1522 Hz fits, shift -1200/+1200 Hz"));

    Tui decoder(TuiMode::decoder);
    decoder.set_color(false);
    DecoderConfig receiver;
    receiver.slot_us = config.slot_us;
    receiver.passband = k_am_passband;
    decoder.set_passband(receiver.passband);
    decoder.set_search_range(receiver.search_range());
    decoder.set_tone_hz(1100.0f);
    decoder.set_slot_ms(100.0f);
    lines = render_lines(decoder, k_wide_columns, k_tall_rows);
    CHECK(contains(lines[0] + lines[1] + lines[2], "band 1078-1122 Hz fits, shift -800/+1600 Hz"));
}

TEST(tui_spectrum_marks_the_passband_the_band_and_the_pitch) {
    Tui tui(TuiMode::encoder);
    tui.set_color(false);
    tui.set_passband(k_ssb_passband);
    tui.set_tone_hz(static_cast<float>(k_tone_hz));
    tui.set_slot_ms(k_slot_ms);
    std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    const int title = find_line(lines, "spectrum");
    REQUIRE(title > 0);
    CHECK(contains(lines[static_cast<size_t>(title)], "[ ] passband"));
    const int markers = find_line(lines, "[", title + 1);
    REQUIRE(markers > title);
    const std::string& row = lines[static_cast<size_t>(markers)];
    CHECK_EQ(cell_at(row, spectrum_column(300.0, k_columns)), uint32_t('['));
    CHECK_EQ(cell_at(row, spectrum_column(2700.0, k_columns)), uint32_t(']'));
    const int pitch = spectrum_column(k_tone_hz, k_columns);
    CHECK_EQ(cell_at(row, pitch), k_caret);
    for (int column = spectrum_column(1368.0, k_columns); column <= spectrum_column(1632.0, k_columns); ++column)
        if (column != pitch) CHECK_EQ(cell_at(row, column), k_band_glyph);  // the occupied band at 6 bytes/s
    CHECK(cell_at(row, spectrum_column(1200.0, k_columns)) != k_band_glyph);
    CHECK(contains(lines[static_cast<size_t>(markers) + 1], "1.5k"));

    // A band that leaves the passband is drawn red where it is outside.
    tui.set_tone_hz(2600.0f);  // 6 bytes/s: 2468-2732 Hz in 300-2700 Hz
    tui.set_color(true);
    lines = render_lines(tui, k_columns, k_rows);
    const int colored = find_line(lines, "]", find_line(lines, "spectrum") + 1);
    REQUIRE(colored > 0);
    CHECK(lines[static_cast<size_t>(colored)].find("\x1b[0;31m") != std::string::npos);
    CHECK(contains(lines[0] + lines[1], "does not fit"));
}

TEST(tui_text_panel_shows_the_newest_bytes) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    const int narrow = 40;
    const int count = 300;
    std::string sent;
    for (int i = 0; i < count; ++i) {
        const char letter = static_cast<char>('a' + i % 26);
        sent += letter;
        tui.on_event(byte_event(static_cast<uint8_t>(letter), static_cast<uint32_t>(i)));
    }
    const std::vector<std::string> lines = render_lines(tui, narrow, k_rows);
    std::string shown;
    for (size_t i = static_cast<size_t>(find_line(lines, "received")) + 1; i < lines.size(); ++i)
        shown += strip_escapes(lines[i]);
    REQUIRE(!shown.empty());
    CHECK_EQ(sent.substr(sent.size() - count % narrow), shown.substr(shown.size() - count % narrow));
    CHECK(sent.find(shown) != std::string::npos);
}

TEST(tui_small_terminal_drops_panels) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    feed_decoder(tui);
    const std::string full = tui.render(k_columns, k_rows);
    CHECK(contains(full, "scope"));
    CHECK(contains(full, "window 17"));
    CHECK(contains(full, "spectrum"));
    CHECK(contains(full, "received"));

    const int short_rows = 6;  // status, text, spectrum
    const std::string short_frame = tui.render(k_columns, short_rows);
    CHECK(contains(short_frame, "RX"));
    CHECK(contains(short_frame, "received"));
    CHECK(contains(short_frame, "spectrum"));
    CHECK(!contains(short_frame, "scope"));
    CHECK(!contains(short_frame, "window 17"));

    const int tiny_rows = 4;
    const std::string tiny_frame = tui.render(k_columns, tiny_rows);
    CHECK(contains(tiny_frame, "received"));
    CHECK(!contains(tiny_frame, "spectrum"));

    const std::string one_row = tui.render(k_columns, 1);
    CHECK_EQ(split_lines(one_row).size(), 1u);
    CHECK(contains(one_row, "TRACK"));
}

TEST(tui_scope_draws_the_envelope) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    const float slot_ms = 32.0f;  // window = 2 slots = 512 samples at 8 kHz
    tui.set_slot_ms(slot_ms);
    const size_t half_window = static_cast<size_t>(slot_ms) * k_rate / 1000;
    const std::vector<int16_t> silence(half_window, 0);
    const std::vector<int16_t> burst = tone(k_tone_hz, k_rate, half_window, k_near_full_scale);
    tui.push_audio(silence.data(), silence.size(), k_rate);
    tui.push_audio(burst.data(), burst.size(), k_rate);
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    const int title = find_line(lines, "scope 64 ms");
    REQUIRE(title > 0);
    const int next_title = find_line(lines, "windows", title + 1);
    REQUIRE(next_title > title + 1);
    const int half = k_columns / 2;
    int rows_with_left_dots = 0;
    for (int row = title + 1; row < next_title; ++row) {
        const Cells cells = cells_of(lines[static_cast<size_t>(row)]);
        int left = 0;
        for (size_t i = 0; i < cells.size() && static_cast<int>(i) < half; ++i) left += is_braille(cells[i]) ? 1 : 0;
        if (left > 0) ++rows_with_left_dots;
    }
    CHECK_EQ(rows_with_left_dots, 1);  // silence is a flat line
    const Cells top = cells_of(lines[static_cast<size_t>(title) + 1]);
    int top_left = 0;
    int top_right = 0;
    for (size_t i = 0; i < top.size(); ++i) {
        if (!is_braille(top[i])) continue;
        if (static_cast<int>(i) < half)
            ++top_left;
        else
            ++top_right;
    }
    CHECK_EQ(top_left, 0);
    CHECK(top_right > 0);  // the burst reaches the top row
}

TEST(tui_scope_shows_the_envelope_when_the_tone_is_known) {
    const float slot_ms = 32.0f;
    const size_t window = static_cast<size_t>(slot_ms) * 2 * k_rate / 1000;
    const std::vector<int16_t> steady = tone(k_tone_hz, k_rate, window, k_near_full_scale);
    int reached[2] = {0, 0};  // dot columns with any dot in the top braille row
    for (int known = 0; known < 2; ++known) {
        Tui tui(TuiMode::decoder);
        tui.set_color(false);
        tui.set_slot_ms(slot_ms);
        if (known) tui.set_tone_hz(static_cast<float>(k_tone_hz));
        tui.push_audio(steady.data(), steady.size(), k_rate);
        const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
        const int title = find_line(lines, "scope");
        REQUIRE(title > 0);
        const Cells top = cells_of(lines[static_cast<size_t>(title) + 1]);
        for (size_t i = 0; i < top.size(); ++i) {
            if (!is_braille(top[i])) continue;
            const uint32_t dots = top[i] - k_braille_first;
            reached[known] += ((dots & k_braille_left_dots) ? 1 : 0) + ((dots & k_braille_right_dots) ? 1 : 0);
        }
    }
    const int dot_columns = 2 * k_columns;
    NOTE("top braille row reached by %d of %d dot columns without the tone, %d with it", reached[0], dot_columns,
         reached[1]);
    CHECK(reached[0] < dot_columns);    // 3.2 samples per dot column alias the 1500 Hz carrier
    CHECK_EQ(reached[1], dot_columns);  // one carrier period per dot column: the envelope
}

TEST(tui_spectrum_marks_the_tone) {
    const uint32_t rates[] = {k_rate, k_high_rate};
    for (size_t r = 0; r < count_of(rates); ++r) {
        Tui tui(TuiMode::decoder);
        tui.set_color(false);
        tui.set_tone_hz(static_cast<float>(k_tone_hz));
        const std::vector<int16_t> audio = tone(k_tone_hz, rates[r], rates[r] / 4, k_half_scale);
        tui.push_audio(audio.data(), audio.size(), rates[r]);
        const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
        const int title = find_line(lines, "spectrum");
        REQUIRE(title > 0);
        const int axis = find_line(lines, "\xe2\x96\xb2", title + 1);  // "▲"
        REQUIRE(axis > title + 1);
        const Cells axis_cells = cells_of(lines[static_cast<size_t>(axis)]);
        const int marker = find_in(axis_cells, "\xe2\x96\xb2");
        const double column_hz = 2700.0 / (k_columns - 1);
        CHECK_NEAR(300.0 + marker * column_hz, k_tone_hz, column_hz);

        std::vector<int> levels(static_cast<size_t>(k_columns), 0);
        for (int row = title + 1; row < axis; ++row)
            for (int column = 0; column < k_columns; ++column)
                levels[static_cast<size_t>(column)] += block_eighths(cell_at(lines[static_cast<size_t>(row)], column));
        const int peak = static_cast<int>(std::max_element(levels.begin(), levels.end()) - levels.begin());
        CHECK(std::abs(peak - marker) <= 1);
        CHECK_EQ(levels[0], 0);  // 1200 Hz away: Hann sidelobes are below the -60 dBFS floor

        const std::string& title_line = lines[static_cast<size_t>(title)];
        const size_t at = title_line.find("peak ");
        REQUIRE(at != std::string::npos);
        const double peak_hz = std::atof(title_line.c_str() + at + 5);
        CHECK_NEAR(peak_hz, k_tone_hz, column_hz);
        const size_t hz = title_line.find(" Hz ", at);
        REQUIRE(hz != std::string::npos);
        CHECK_NEAR(std::atof(title_line.c_str() + hz + 4), k_half_scale_dbfs, k_scalloping_tolerance_db);
    }
}

TEST(tui_frame_is_home_lines_and_clear_to_eol) {
    Tui tui(TuiMode::decoder);
    feed_decoder(tui);
    const std::string cleared = tui.frame(k_columns, k_rows, true);
    CHECK_EQ(cleared.find("\x1b[2J\x1b[H"), 0u);
    const std::string frame = tui.frame(k_columns, k_rows, false);
    REQUIRE(frame.find("\x1b[H") == 0);
    std::vector<std::string> lines;
    size_t begin = std::string("\x1b[H").size();
    while (true) {
        const size_t end = frame.find("\r\n", begin);
        lines.push_back(frame.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) break;
        begin = end + 2;
    }
    CHECK_EQ(lines.size(), static_cast<size_t>(k_rows));
    for (size_t i = 0; i < lines.size(); ++i) {
        CHECK(lines[i].find('\n') == std::string::npos);
        const std::string clear_line = "\x1b[K";
        CHECK(lines[i].size() >= clear_line.size() &&
              lines[i].compare(lines[i].size() - clear_line.size(), clear_line.size(), clear_line) == 0);
        CHECK(display_width(lines[i]) < static_cast<size_t>(k_columns));  // the last column is never written
    }
}

TEST(tui_realtime_pacer_follows_audio_time) {
    const Clock::time_point start = Clock::now();
    unlimited::pc::RealtimePacer pacer(k_rate);
    pacer.advance(k_pacer_chunk);
    pacer.advance(k_pacer_chunk);
    const long long elapsed_ms =
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    NOTE("800 samples at 8 kHz paced in %lld ms", elapsed_ms);
    CHECK(elapsed_ms >= k_pacer_expected_ms);
    CHECK(elapsed_ms < k_pacer_expected_ms + k_timing_slack_ms);

    pacer.restart();
    const Clock::time_point restarted = Clock::now();
    pacer.advance(0);
    CHECK(Clock::now() - restarted < std::chrono::milliseconds(k_refresh_period_ms));
}

TEST(tui_refresh_pacer_limits_the_rate) {
    const Clock::time_point start = Clock::now();
    unlimited::pc::RefreshPacer pacer(k_refresh_hz);
    CHECK(pacer.due());
    CHECK(!pacer.due());
    pacer.wait();
    const long long elapsed_ms =
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    CHECK(elapsed_ms >= k_refresh_period_ms);
    CHECK(elapsed_ms < k_refresh_period_ms + k_timing_slack_ms);
    CHECK(!pacer.due());
    CHECK(pacer.next() > Clock::now());  // what a caller waiting on its own events waits until
}

// ---------------------------------------------------------------------------
// Spec 12.6: the level meter, the status hand-off, the live status items, resizes and Ctrl-C
// ---------------------------------------------------------------------------

namespace {

const size_t k_level_window = 2400;          // 300 ms at 8 kHz
const double k_half_scale_rms_dbfs = -9.03;  // a sine's RMS is its crest / sqrt(2): 3.01 dB under it
const double k_quiet_scale = 2048.0;         // 1/16 of full scale
const double k_quiet_dbfs = -24.08;
const double k_db_tolerance = 0.02;
const size_t k_ring_statuses = 100000;

std::string status_rows(const std::vector<std::string>& lines) {
    return lines[0] + lines[1] + lines[2];
}

void stop_counter(void* context) {
    static_cast<std::atomic<int>*>(context)->fetch_add(1);
}

}  // namespace

// Peak and RMS in dBFS against full scale 32768, and the clips (samples at -32768 or +32767).
TEST(tui_level_meter_peak_rms_and_clips) {
    LevelMeter sine;
    const std::vector<int16_t> half = tone(k_tone_hz, k_rate, k_rate, k_half_scale);  // 1 s
    sine.push(half.data(), half.size(), k_rate);
    const Level recent = sine.recent();
    CHECK_EQ(recent.samples, static_cast<uint64_t>(k_level_window));  // the last whole window
    CHECK_NEAR(recent.peak_dbfs, k_half_scale_dbfs, k_db_tolerance);
    CHECK_NEAR(recent.rms_dbfs, k_half_scale_rms_dbfs, k_db_tolerance);
    CHECK_EQ(recent.clips, 0u);
    const Level total = sine.total();
    CHECK_EQ(total.samples, static_cast<uint64_t>(k_rate));
    CHECK_NEAR(total.rms_dbfs, k_half_scale_rms_dbfs, k_db_tolerance);
    CHECK_EQ(level_text(recent, true), std::string("peak -6.0 dBFS, RMS -9.0 dBFS, 0 clips"));
    CHECK_EQ(level_text(recent, false), std::string("peak -6.0 dBFS, RMS -9.0 dBFS"));

    // A square wave between the limits of int16: every sample clips; 0.0 dBFS, never "-0.0".
    std::vector<int16_t> square(k_rate / 10);
    for (size_t i = 0; i < square.size(); ++i)
        square[i] = (i / k_eighths) % 2 ? std::numeric_limits<int16_t>::max() : std::numeric_limits<int16_t>::min();
    LevelMeter loud;
    loud.push(square.data(), square.size(), k_rate);
    const Level clipped = loud.total();
    CHECK_EQ(clipped.clips, static_cast<uint64_t>(square.size()));
    CHECK_NEAR(clipped.peak_dbfs, 0.0, 1e-9);
    CHECK_EQ(level_text(clipped, true), std::string("peak 0.0 dBFS, RMS 0.0 dBFS, 800 clips"));
    LevelMeter single;
    single.push(&square[0], 1, k_rate);
    CHECK_EQ(level_text(single.total(), true), std::string("peak 0.0 dBFS, RMS 0.0 dBFS, 1 clip"));

    LevelMeter quiet;
    const std::vector<int16_t> zeros(k_rate, 0);
    quiet.push(zeros.data(), zeros.size(), k_rate);
    CHECK(std::isinf(quiet.total().peak_dbfs) && quiet.total().peak_dbfs < 0.0);
    CHECK(std::isinf(quiet.total().rms_dbfs));
    CHECK_EQ(level_text(quiet.recent(), true), std::string("digital silence"));
    CHECK_EQ(level_text(LevelMeter().recent(), true), std::string("no audio"));
}

// The recent level is the last whole window (the window being filled until the first one is whole), whatever the
// chunks; the silence counts the zeros since the last other sample; another rate starts both over.
TEST(tui_level_meter_windows_and_silence) {
    const std::vector<int16_t> loud = tone(k_tone_hz, k_rate, 2 * k_level_window, k_half_scale);
    const std::vector<int16_t> quiet = tone(k_tone_hz, k_rate, k_level_window, k_quiet_scale);
    LevelMeter meter;
    meter.push(loud.data(), k_level_window - 1, k_rate);
    CHECK_EQ(meter.recent().samples, static_cast<uint64_t>(k_level_window - 1));
    meter.push(&loud[k_level_window - 1], 1, k_rate);
    CHECK_EQ(meter.recent().samples, static_cast<uint64_t>(k_level_window));
    CHECK_NEAR(meter.recent().peak_dbfs, k_half_scale_dbfs, k_db_tolerance);
    meter.push(quiet.data(), k_level_window / 2, k_rate);
    CHECK_NEAR(meter.recent().peak_dbfs, k_half_scale_dbfs, k_db_tolerance);  // still the loud window
    for (size_t i = k_level_window / 2; i < k_level_window; ++i) meter.push(&quiet[i], 1, k_rate);
    CHECK_NEAR(meter.recent().peak_dbfs, k_quiet_dbfs, k_db_tolerance);
    CHECK_EQ(meter.total().samples, static_cast<uint64_t>(2 * k_level_window));

    const std::vector<int16_t> zeros(3 * k_rate, 0);
    CHECK_EQ(meter.silent_seconds(), 0.0);
    meter.push(zeros.data(), zeros.size(), k_rate);
    CHECK_NEAR(meter.silent_seconds(), 3.0, 1e-9);
    const int16_t one = 1;
    meter.push(&one, 1, k_rate);
    CHECK_EQ(meter.silent_seconds(), 0.0);
    meter.push(zeros.data(), k_rate, k_rate);
    CHECK_NEAR(meter.silent_seconds(), 1.0, 1e-9);

    meter.push(zeros.data(), 1, k_high_rate);  // another stream
    CHECK_NEAR(meter.silent_seconds(), 1.0 / k_high_rate, 1e-12);
    CHECK_EQ(meter.recent().samples, 1u);
    CHECK_EQ(meter.total().samples, static_cast<uint64_t>(2 * k_level_window + 4 * k_rate + 2));
}

// The hand-off from the thread that renders the encoder to the view's thread: in order, never blocking, a full ring
// drops (counted).
TEST(tui_status_ring_hands_over_in_order) {
    StatusRing ring(100);
    CHECK_EQ(ring.capacity(), 128u);  // a power of two
    EncoderStatus status = EncoderStatus();
    for (uint32_t i = 0; i < ring.capacity(); ++i) {
        status.slot_index = i;
        CHECK(ring.push(status));
    }
    status.slot_index = 999;
    CHECK(!ring.push(status));
    CHECK_EQ(ring.dropped(), 1u);
    for (uint32_t i = 0; i < ring.capacity(); ++i) CHECK(ring.pop(status) && status.slot_index == i);
    CHECK(!ring.pop(status));

    // Across threads: every status arrives, in order, whole (a producer that meets a full ring tries again here, to
    // hand over all of them; the program's never waits).
    StatusRing shared(64);
    std::atomic<bool> done(false);
    std::thread producer([&] {
        EncoderStatus produced = EncoderStatus();
        for (uint32_t i = 0; i < k_ring_statuses; ++i) {
            produced.slot_index = i;
            produced.byte_index = i / k_window_cells;
            while (!shared.push(produced)) std::this_thread::yield();
        }
        done.store(true);
    });
    size_t received = 0;
    bool ordered = true;
    uint32_t previous = 0;
    for (;;) {
        const bool finished = done.load();  // read first: every push before it is then visible
        EncoderStatus taken = EncoderStatus();
        bool any = false;
        while (shared.pop(taken)) {
            ordered = ordered && (received == 0 || taken.slot_index > previous) &&
                      taken.byte_index == taken.slot_index / k_window_cells;
            previous = taken.slot_index;
            ++received;
            any = true;
        }
        if (finished) break;
        if (!any) std::this_thread::yield();
    }
    producer.join();
    NOTE("%zu statuses handed over in order; the producer met a full ring %u times", received,
         static_cast<unsigned>(shared.dropped()));
    CHECK(ordered);
    CHECK_EQ(received, k_ring_statuses);
}

// The level meter item follows the state (spec 12.6): the decoder's input with its clips, the encoder's output; the
// warning item follows it and goes away.
TEST(tui_status_shows_the_level_and_the_warning) {
    Tui decoder(TuiMode::decoder);
    decoder.set_color(false);
    decoder.set_label("coreaudio:5");
    decoder.set_speed(k_speed);
    LevelMeter meter;
    const std::vector<int16_t> half = tone(k_tone_hz, k_rate, k_rate, k_half_scale);
    meter.push(half.data(), half.size(), k_rate);
    decoder.set_level(meter.recent());
    std::vector<std::string> lines = render_lines(decoder, k_wide_columns, k_tall_rows);
    CHECK(contains(lines[0], "RX coreaudio:5 \xe2\x94\x82 SEARCH, DCD off \xe2\x94\x82 in peak -6.0 dBFS, RMS -9.0 dBFS, "
                             "0 clips \xe2\x94\x82 6.00 bytes/s"));
    decoder.set_warning("digital silence 5 s: microphone permission?");
    lines = render_lines(decoder, k_wide_columns, k_tall_rows);
    const Cells items = cells_of(status_rows(lines));
    const int level_at = find_in(items, "0 clips");
    const int warning_at = find_in(items, "digital silence 5 s: microphone permission?");
    CHECK(level_at > 0 && warning_at > level_at);  // right after the level (here on the next row)
    CHECK(warning_at < find_in(items, "6.00 bytes/s"));
    decoder.set_warning("");
    lines = render_lines(decoder, k_wide_columns, k_tall_rows);
    CHECK(!contains(status_rows(lines), "digital silence"));
    Level clipping = meter.recent();
    clipping.clips = 7;
    decoder.set_level(clipping);
    CHECK(contains(status_rows(render_lines(decoder, k_wide_columns, k_tall_rows)), ", 7 clips"));
    decoder.set_level(LevelMeter().recent());
    CHECK(contains(status_rows(render_lines(decoder, k_wide_columns, k_tall_rows)), "in no audio"));

    const EncoderConfig config = six_config();
    Tui encoder(TuiMode::encoder);
    encoder_view(encoder, config);
    LevelMeter out;
    Encoder sender(config);
    const std::vector<uint8_t> data = bytes_of("Hi");
    REQUIRE(sender.write(data.data(), data.size()) == data.size());
    REQUIRE(sender.start());
    std::vector<int16_t> audio(sender.duration_samples(data.size()));
    CHECK_EQ(sender.render(audio.data(), audio.size()), audio.size());
    out.push(audio.data(), audio.size(), config.sample_rate_hz);
    encoder.set_level(out.total());
    const std::string status = status_rows(render_lines(encoder, k_wide_columns, k_tall_rows));
    CHECK(contains(status, "TX test \xe2\x94\x82 IDLE \xe2\x94\x82 out peak -3.0 dBFS, RMS -"));
    CHECK(!contains(status, "clip"));
}

// A resize is followed at the next frame: the screen is cleared and the frame has the new size.
TEST(tui_next_frame_follows_a_resize) {
    Tui tui(TuiMode::decoder);
    feed_decoder(tui);
    const std::string clear_home = "\x1b[2J\x1b[H";
    CHECK_EQ(tui.next_frame(k_columns, k_rows).find(clear_home), 0u);  // the first frame clears
    const std::string same = tui.next_frame(k_columns, k_rows);
    CHECK_EQ(same.find("\x1b[H"), 0u);
    CHECK(same.find("\x1b[2J") == std::string::npos);
    const std::string resized = tui.next_frame(k_wide_columns, k_tall_rows);
    CHECK_EQ(resized.find(clear_home), 0u);
    CHECK_EQ(static_cast<int>(std::count(resized.begin(), resized.end(), '\n')), k_tall_rows - 1);  // rows, "\r\n" between
    CHECK(tui.next_frame(k_wide_columns, k_tall_rows).find("\x1b[2J") == std::string::npos);
}

// Ctrl-C, SIGTERM and SIGHUP call the stop function instead of ending the program; the handlers before come back; an
// ignored signal stays ignored; the TUI's restore hook comes back after it (spec 12.6).
TEST(tui_stop_on_signals_calls_the_stop_and_restores) {
    typedef void (*Handler)(int);
    std::atomic<int> stops(0);
    const Handler original_int = std::signal(SIGINT, tui_test_signal_handler);
    const Handler original_term = std::signal(SIGTERM, SIG_DFL);
#ifdef SIGHUP
    const Handler original_hup = std::signal(SIGHUP, SIG_DFL);
#endif
    {
        unlimited::pc::StopOnSignals stop(&stop_counter, &stops);
        std::raise(SIGINT);
        CHECK_EQ(stops.load(), 1);
        CHECK_EQ(stop.count(), 1);
        CHECK_EQ(stop.last_signal(), SIGINT);
        std::raise(SIGINT);  // again: stop() once more, never the end of the program
        std::raise(SIGTERM);
        CHECK_EQ(stops.load(), 3);
        CHECK_EQ(stop.last_signal(), SIGTERM);
#ifdef SIGHUP
        std::raise(SIGHUP);
        CHECK_EQ(stops.load(), 4);
        CHECK_EQ(stop.last_signal(), SIGHUP);
#endif
    }
    CHECK(std::signal(SIGINT, SIG_IGN) == tui_test_signal_handler);  // the handler before is back
    CHECK(std::signal(SIGTERM, SIG_DFL) == SIG_DFL);
    const int before = stops.load();
    {
        unlimited::pc::StopOnSignals stop(&stop_counter, &stops);
        std::raise(SIGINT);  // ignored before: still ignored (nohup, a background job)
        CHECK_EQ(stop.count(), 0);
    }
    CHECK_EQ(stops.load(), before);
    CHECK(std::signal(SIGINT, SIG_DFL) == SIG_IGN);

    unlimited::pc::restore_terminal_on_exit(true);
    const Handler restore_hook = std::signal(SIGINT, SIG_DFL);
    std::signal(SIGINT, restore_hook);
    CHECK(restore_hook != SIG_DFL);
    {
        unlimited::pc::StopOnSignals stop(&stop_counter, &stops);
        std::raise(SIGINT);  // stops, and the process lives on
        CHECK_EQ(stops.load(), before + 1);
    }
    CHECK(std::signal(SIGINT, restore_hook) == restore_hook);  // the TUI's hook is back
    unlimited::pc::restore_terminal_on_exit(false);
    CHECK(std::signal(SIGINT, original_int) == SIG_DFL);
    std::signal(SIGTERM, original_term);
#ifdef SIGHUP
    std::signal(SIGHUP, original_hup);
#endif
}
