#include "terminal.hpp"
#include "test_harness.hpp"
#include "tui.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using unlimited::DecoderState;
using unlimited::EncoderSegment;
using unlimited::EncoderStatus;
using unlimited::Event;
using unlimited::EventType;
using unlimited::GridSide;
using unlimited::LostReason;
using unlimited::SlotKind;
using unlimited::Spacing;
using unlimited::pc::Tui;
using unlimited::pc::TuiMode;
using unlimited::pc::display_width;

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
const float k_ref_hz = 2132.0f;  // hf preset f_ref
const double k_half_scale = 16384.0;
const double k_near_full_scale = 30000.0;
const double k_half_scale_dbfs = -6.02;
const double k_scalloping_tolerance_db = 1.5;  // Hann window, tone up to half a bin off the column frequency
const float k_slot_ms = 32.0f;
const float k_snr_db = 12.5f;
const int k_columns = 80;
const int k_rows = 24;
const int k_wide_columns = 120;
const int k_tall_rows = 30;
const uint32_t k_frame_index = 42;
const uint32_t k_start_slot = 20;

// hf mode: k 5 (32 tones), N 8, standard spacing, grid below f_ref.
const uint8_t k_bits = 5;
const uint8_t k_slots = 8;
const int k_tones = 32;
const int k_cells = k_slots + 2;  // START, 8 peaks, STOP
const double k_guard = 5.0;
const double k_standard_spacing = 8.0 / 7.0;

// Spec 1.5 vector: bytes 48 69 21 00 FF -> symbols and tones of the 8 peaks.
const uint8_t k_vector_bytes[] = {0x48, 0x69, 0x21, 0x00, 0xFF};
const uint8_t k_vector_symbols[k_slots] = {9, 1, 20, 18, 2, 0, 7, 31};
const uint8_t k_vector_tones[k_slots] = {13, 6, 8, 10, 23, 25, 2, 19};
const uint8_t k_levels[k_slots] = {120, 95, 60, 105, 30, 100, 90, 110};  // slots 3 and 5 below the 70 % line
const uint8_t k_confidences[k_slots] = {30, 24, 6, 20, 2, 30, 18, 26};   // 0.5 dB steps, whole dB each
const uint8_t k_hf_header_tones[k_slots] = {4, 5, 7, 1, 0, 3, 6, 2};      // spec 2.2, hf T32 k5 N8
const int k_line_pct = 70;

const uint32_t k_escape = 0x1B;
const uint32_t k_space = 0x20;
const uint32_t k_line_glyph = 0x254C;       // 70 % line
const uint32_t k_crest_glyph = 0x2508;      // 100 % START/STOP crest line
const uint32_t k_full_block = 0x2588;
const uint32_t k_weak_block = 0x2592;
const uint32_t k_lower_eighth = 0x2581;
const uint32_t k_caret = 0x25B2;
const uint32_t k_middle_dot = 0x00B7;
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
    for (size_t i = static_cast<size_t>(from); i < lines.size(); ++i)
        if (contains(lines[i], needle)) return static_cast<int>(i);
    return -1;
}

int find_line_starting(const std::vector<std::string>& lines, const std::string& prefix) {
    for (size_t i = 0; i < lines.size(); ++i)
        if (find_in(cells_of(lines[i]), prefix) == 0) return static_cast<int>(i);
    return -1;
}

uint32_t cell_at(const std::string& line, int column) {
    const Cells cells = cells_of(line);
    if (column < 0 || static_cast<size_t>(column) >= cells.size()) return k_space;
    return cells[static_cast<size_t>(column)];
}

int count_of_glyph(const std::string& line, uint32_t glyph) {
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

// Column of each slot label: START "M", the peak labels (1..N or h0..h7), STOP "M".
std::vector<int> slot_columns(const std::string& labels_line, const std::vector<std::string>& peak_labels) {
    const Cells cells = cells_of(labels_line);
    std::vector<int> columns;
    int position = find_in(cells, "M");
    columns.push_back(position);
    for (size_t i = 0; i < peak_labels.size(); ++i) {
        position = find_in(cells, peak_labels[i], position + 1);
        columns.push_back(position);
    }
    columns.push_back(find_in(cells, "M", position + 1));
    return columns;
}

std::vector<std::string> numbered_labels(int count) {
    std::vector<std::string> labels;
    for (int i = 1; i <= count; ++i) labels.push_back(std::to_string(i));
    return labels;
}

std::vector<std::string> header_labels() {
    std::vector<std::string> labels;
    for (int i = 0; i < k_slots; ++i) labels.push_back("h" + std::to_string(i));
    return labels;
}

// The text starting at `column` up to the next space.
std::string word_at(const std::string& line, int column) {
    const Cells cells = cells_of(line);
    std::string word;
    for (size_t i = static_cast<size_t>(std::max(column, 0)); i < cells.size() && cells[i] != k_space; ++i)
        word += static_cast<char>(cells[i]);
    return word;
}

// Lit cells (full or weak block) of a column over rows [from, to].
std::vector<int> lit_rows(const std::vector<std::string>& lines, int column, int from, int to) {
    std::vector<int> rows;
    for (int row = from; row <= to; ++row) {
        const uint32_t glyph = cell_at(lines[static_cast<size_t>(row)], column);
        if (glyph == k_full_block || glyph == k_weak_block) rows.push_back(row);
    }
    return rows;
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

// An event of a locked hf transmission: mode fields, f_ref, exact T and SNR filled.
Event mode_event(EventType type, uint8_t bits = k_bits, uint8_t slots = k_slots) {
    Event event = make_event(type, DecoderState::track);
    event.bits_per_peak = bits;
    event.data_slots = slots;
    event.spacing = Spacing::standard;
    event.side = -1;
    event.tone_hz = k_ref_hz;
    event.slot_ms = k_slot_ms;
    event.snr_db = k_snr_db;
    return event;
}

Event slot_event(uint32_t frame, int index, uint8_t tone_index, uint8_t level, uint8_t confidence,
                 uint8_t bits = k_bits) {
    Event event = mode_event(EventType::slot, bits);
    event.frame_index = frame;
    event.index = static_cast<uint8_t>(index);
    event.tone = tone_index;
    event.value = k_vector_symbols[(index - 1) % k_slots];
    event.level_pct = level;
    event.confidence = confidence;
    return event;
}

Event byte_event(uint8_t value, uint32_t frame, uint8_t index) {
    Event event = mode_event(EventType::byte);
    event.value = value;
    event.frame_index = frame;
    event.index = index;
    return event;
}

// The 8 slot events of the spec 1.5 vector frame.
void send_slots(Tui& tui, uint32_t frame) {
    for (int i = 0; i < k_slots; ++i)
        tui.on_event(slot_event(frame, i + 1, k_vector_tones[i], k_levels[i], k_confidences[i]));
}

void feed_decoder(Tui& tui) {
    const std::vector<int16_t> audio = tone(k_tone_hz, k_rate, k_rate / 4, k_half_scale);
    tui.push_audio(audio.data(), audio.size(), k_rate);
    tui.on_event(mode_event(EventType::locked));
    send_slots(tui, k_frame_index);
    const std::string text = "CQ CQ DE UNLIMITED";
    for (size_t i = 0; i < text.size(); ++i) tui.on_event(byte_event(static_cast<uint8_t>(text[i]), 0, 0));
}

EncoderStatus make_status(EncoderSegment segment, SlotKind kind, uint8_t slot, uint8_t symbol, uint8_t tone_index,
                          uint32_t slot_index) {
    EncoderStatus status = EncoderStatus();
    status.segment = segment;
    status.kind = kind;
    status.slot = slot;
    status.symbol = symbol;
    status.tone = tone_index;
    status.slot_index = slot_index;
    return status;
}

// Peaks 0..count-1 of a frame, then its STOP; each status reported `repeats` times. Returns the next slot.
uint32_t send_frame(Tui& tui, const uint8_t* symbols, const uint8_t* tones, size_t count, uint32_t slot,
                    int repeats = 1) {
    for (size_t i = 0; i < count; ++i, ++slot)
        for (int r = 0; r < repeats; ++r)
            tui.on_encoder_status(make_status(EncoderSegment::frame, SlotKind::peak, static_cast<uint8_t>(i),
                                              symbols[i], tones[i], slot));
    tui.on_encoder_status(make_status(EncoderSegment::frame, SlotKind::marker, k_slots, 0, 0, slot));
    return slot + 1;
}

void encoder_mode(Tui& tui) {
    tui.set_tone_hz(k_ref_hz);
    tui.set_slot_ms(k_slot_ms);
    tui.set_mode(k_bits, k_slots, Spacing::standard, GridSide::below);
}

void feed_encoder(Tui& tui) {
    const std::vector<int16_t> audio = tone(k_tone_hz, k_high_rate, k_high_rate / 4, k_half_scale);
    tui.push_audio(audio.data(), audio.size(), k_high_rate);
    encoder_mode(tui);
    tui.set_profile("hf");
    uint32_t slot = k_start_slot;
    for (int frame = 0; frame < 3; ++frame) slot = send_frame(tui, k_vector_symbols, k_vector_tones, k_slots, slot);
    tui.on_encoder_status(make_status(EncoderSegment::frame, SlotKind::peak, 1, k_vector_symbols[1],
                                      k_vector_tones[1], slot + 1));
}

std::vector<std::string> render_lines(const Tui& tui, int columns, int rows) {
    return split_lines(tui.render(columns, rows));
}

// Frequency of hf grid tone n: f_ref - (5 + n 8/7) / T.
double hf_tone_hz(int tone_index) {
    return k_ref_hz - (k_guard + tone_index * k_standard_spacing) * 1000.0 / k_slot_ms;
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
    CHECK_EQ(display_width("\xe2\xa0\xbf\xe2\x96\x88"), 2u);  // braille + full block
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
    feed_encoder(encoder);
    Tui wide(TuiMode::decoder);  // N = 32: 34 slot columns
    const uint8_t wide_slots = 32;
    wide.on_event(mode_event(EventType::locked, k_bits, wide_slots));
    for (int i = 1; i <= wide_slots; ++i) wide.on_event(slot_event(0, i, static_cast<uint8_t>(i), 90, 20));
    Tui* const views[] = {&decoder, &encoder, &wide};
    int violations = 0;
    for (size_t v = 0; v < test::count_of(views); ++v) {
        for (size_t c = 0; c < test::count_of(columns); ++c) {
            for (size_t r = 0; r < test::count_of(rows); ++r) {
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

TEST(tui_decoder_tone_grid_levels_and_footer) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    tui.on_event(mode_event(EventType::locked));
    send_slots(tui, k_frame_index);
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);

    const int labels_row = find_line_starting(lines, "slot");
    REQUIRE(labels_row > 0);
    const std::vector<int> columns = slot_columns(lines[static_cast<size_t>(labels_row)], numbered_labels(k_slots));
    REQUIRE(columns.size() == static_cast<size_t>(k_cells));
    for (size_t i = 0; i < columns.size(); ++i) REQUIRE(columns[i] >= 0);

    // Footer: the tone index and the confidence (whole dB) under each peak.
    const std::string& tones_line = lines[static_cast<size_t>(labels_row) + 1];
    const std::string& conf_line = lines[static_cast<size_t>(labels_row) + 2];
    CHECK_EQ(find_in(cells_of(tones_line), "tone"), 0);
    CHECK_EQ(find_in(cells_of(conf_line), "conf"), 0);
    for (int i = 0; i < k_slots; ++i) {
        const int column = columns[static_cast<size_t>(1 + i)];
        CHECK_EQ(word_at(tones_line, column), std::to_string(k_vector_tones[i]));
        CHECK_EQ(word_at(conf_line, column), std::to_string(k_confidences[i] / 2));
    }
    const int title_row = find_line(lines, "frame 42");
    REQUIRE(title_row > 0);
    CHECK(contains(lines[static_cast<size_t>(title_row)], "slot 8/8  tone 19  symbol 31  level 110%  conf 13.0 dB"));

    // Grid: f_ref row with the START/STOP markers, tone 0 just above it, the top tone at the top row.
    const int ref_row = find_line_starting(lines, " ref");
    REQUIRE(ref_row > title_row);
    CHECK(find_line_starting(lines, "   0") == ref_row - 1);
    const int top_row = find_line_starting(lines, "  31");
    REQUIRE(top_row > title_row);
    REQUIRE(top_row < ref_row - 1);
    const int tone_rows = ref_row - top_row;
    const int tones_per_row = (k_tones + tone_rows - 1) / tone_rows;
    CHECK_EQ(cell_at(lines[static_cast<size_t>(ref_row)], columns.front()), k_full_block);
    CHECK_EQ(cell_at(lines[static_cast<size_t>(ref_row)], columns.back()), k_full_block);
    for (int i = 0; i < k_slots; ++i) {
        const int column = columns[static_cast<size_t>(1 + i)];
        const std::vector<int> lit = lit_rows(lines, column, top_row, ref_row);
        REQUIRE(lit.size() == 1u);
        CHECK_EQ(lit[0], ref_row - 1 - k_vector_tones[i] / tones_per_row);
        // Slots under the 70 % line light a weaker glyph (a visual reference only: argmax decided them).
        const uint32_t glyph = cell_at(lines[static_cast<size_t>(lit[0])], column);
        CHECK_EQ(glyph, k_levels[i] >= k_line_pct ? k_full_block : k_weak_block);
    }

    // Levels: the 100 % crest line above the 70 % line, both between the grid and the labels.
    const int crest_row = find_line_starting(lines, "100%");
    const int line_row = find_line_starting(lines, " 70%");
    REQUIRE(crest_row > ref_row);
    REQUIRE(line_row > crest_row);
    REQUIRE(line_row < labels_row);
    for (int i = 0; i < k_slots; ++i) {
        const int column = columns[static_cast<size_t>(1 + i)];
        const uint32_t at_line = cell_at(lines[static_cast<size_t>(line_row)], column);
        if (k_levels[i] >= 100)
            CHECK_EQ(at_line, k_full_block);
        else if (k_levels[i] < k_line_pct - 20)
            CHECK_EQ(at_line, k_line_glyph);
        CHECK(block_eighths(cell_at(lines[static_cast<size_t>(labels_row) - 1], column)) > 0);  // every bar starts
    }
    // START and STOP (100 % by definition) fill every row below the crest line and reach it; the lines cross
    // the gaps between slots.
    for (size_t marker = 0; marker < columns.size(); marker += columns.size() - 1) {
        for (int row = crest_row + 1; row < labels_row; ++row)
            CHECK_EQ(cell_at(lines[static_cast<size_t>(row)], columns[marker]), k_full_block);
        const uint32_t top = cell_at(lines[static_cast<size_t>(crest_row)], columns[marker]);
        CHECK(top == k_crest_glyph || block_eighths(top) > 0);
    }
    CHECK_EQ(cell_at(lines[static_cast<size_t>(line_row)], columns[1] - 1), k_line_glyph);
    CHECK_EQ(cell_at(lines[static_cast<size_t>(crest_row)], columns[1] - 1), k_crest_glyph);
}

TEST(tui_decoder_grid_rows_follow_the_tone) {
    const uint8_t bits = 7;  // 128 tones on a few rows
    const uint8_t tones[] = {0, 127, 64, 32, 96};
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    tui.on_event(mode_event(EventType::locked, bits));
    for (size_t i = 0; i < count_of(tones); ++i)
        tui.on_event(slot_event(0, static_cast<int>(i) + 1, tones[i], 100, 20, bits));
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    const int labels_row = find_line_starting(lines, "slot");
    const int ref_row = find_line_starting(lines, " ref");
    const int top_row = find_line_starting(lines, " 127");
    REQUIRE(labels_row > 0);
    REQUIRE(ref_row > top_row);
    REQUIRE(top_row > 0);
    const std::vector<int> columns = slot_columns(lines[static_cast<size_t>(labels_row)], numbered_labels(k_slots));
    std::vector<int> rows;
    for (size_t i = 0; i < count_of(tones); ++i) {
        const std::vector<int> lit = lit_rows(lines, columns[1 + i], top_row, ref_row);
        REQUIRE(lit.size() == 1u);
        rows.push_back(lit[0]);
    }
    CHECK_EQ(rows[0], ref_row - 1);  // tone 0
    CHECK_EQ(rows[1], top_row);      // tone 127
    CHECK(rows[3] >= rows[2]);       // 32 at or below 64
    CHECK(rows[4] <= rows[2]);       // 96 at or above 64
    CHECK(rows[3] < rows[0]);
    CHECK(rows[4] > rows[1]);
    for (int i = static_cast<int>(count_of(tones)); i < k_slots; ++i)  // slots not decided yet stay dark
        CHECK(lit_rows(lines, columns[static_cast<size_t>(1 + i)], top_row, ref_row).empty());
}

TEST(tui_decoder_new_frame_restarts_the_grid) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    tui.on_event(mode_event(EventType::locked));
    send_slots(tui, 3);
    Event erased = slot_event(4, 1, 7, 40, 1);
    erased.flags = unlimited::event_flag_erasure;
    tui.on_event(erased);
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    const int labels_row = find_line_starting(lines, "slot");
    const int ref_row = find_line_starting(lines, " ref");
    const int top_row = find_line_starting(lines, "  31");
    REQUIRE(labels_row > 0);
    REQUIRE(ref_row > top_row);
    REQUIRE(top_row > 0);
    const std::vector<int> columns = slot_columns(lines[static_cast<size_t>(labels_row)], numbered_labels(k_slots));
    CHECK_EQ(lit_rows(lines, columns[1], top_row, ref_row).size(), 1u);
    for (int i = 2; i <= k_slots; ++i)
        CHECK(lit_rows(lines, columns[static_cast<size_t>(i)], top_row, ref_row).empty());
    const int title_row = find_line(lines, "frame 4");
    REQUIRE(title_row > 0);
    CHECK(contains(lines[static_cast<size_t>(title_row)], "slot 1/8  tone 7"));
    CHECK(contains(lines[static_cast<size_t>(title_row)], "erasure"));
}

TEST(tui_decoder_status_counts_mode_and_text) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    tui.set_profile("ssb");
    tui.set_field("channel", "usb 10 dB");
    tui.on_event(make_event(EventType::state, DecoderState::acquire));
    tui.on_event(mode_event(EventType::locked));
    const uint8_t bytes[] = {'H', 'i', '\n', 0x07};
    for (size_t i = 0; i < count_of(bytes); ++i) tui.on_event(byte_event(bytes[i], 0, static_cast<uint8_t>(i)));
    tui.on_event(make_event(EventType::end, DecoderState::search));

    std::vector<std::string> lines = render_lines(tui, k_wide_columns, k_tall_rows);
    CHECK(contains(lines[0], "RX ssb"));
    CHECK(contains(lines[0], "SEARCH"));
    CHECK(contains(lines[0], "2132.0 Hz"));
    CHECK(contains(lines[0], "T 32.00 ms 31.25 Bd"));
    CHECK(contains(lines[0], "SNR 12.5 dB"));
    CHECK(contains(lines[1], "k5 N8 standard below 138.9 bit/s"));
    CHECK(contains(lines[1], "bytes 4"));
    CHECK(contains(lines[1], "lock 1"));
    CHECK(contains(lines[1], "lost 0"));
    CHECK(contains(lines[1], "end 1"));
    CHECK(contains(lines[1], "channel usb 10 dB"));
    const int text_title = find_line(lines, "received");
    REQUIRE(text_title > 0);
    CHECK(contains(lines[static_cast<size_t>(text_title)], "4 bytes"));
    CHECK(contains(lines[static_cast<size_t>(text_title) + 1], "Hi\xe2\x86\xb5\xc2\xb7"));  // "Hi↵·"

    Event lost = make_event(EventType::lost, DecoderState::acquire);
    lost.reason = LostReason::no_header;
    tui.on_event(lost);
    tui.set_field("channel", "");
    lines = render_lines(tui, k_wide_columns, k_tall_rows);
    CHECK(contains(lines[0], "ACQUIRE"));
    CHECK(contains(lines[1], "lost 1"));
    CHECK(contains(lines[1], "lost: no header"));
    CHECK(!contains(lines[1], "channel"));

    Event remembered = mode_event(EventType::locked);
    remembered.flags = unlimited::event_flag_mode_memory;
    tui.on_event(remembered);
    Event flagged = byte_event('!', 0, 0);
    flagged.flags = unlimited::event_flag_mode_memory | unlimited::event_flag_flywheel_stop;
    tui.on_event(flagged);
    lines = render_lines(tui, k_wide_columns, k_tall_rows);
    CHECK(contains(lines[1], "138.9 bit/s memory"));
    CHECK(contains(lines[1], "locked (mode memory)"));
    CHECK(contains(lines[static_cast<size_t>(find_line(lines, "received"))], "mode-memory  flywheel-stop"));
}

TEST(tui_encoder_header_frame_tones_and_caret) {
    Tui tui(TuiMode::encoder);
    tui.set_color(false);
    encoder_mode(tui);
    uint32_t slot = k_start_slot;
    for (int i = 0; i < k_slots; ++i)
        tui.on_encoder_status(make_status(EncoderSegment::header, SlotKind::peak, static_cast<uint8_t>(i),
                                          k_hf_header_tones[i], k_hf_header_tones[i], slot++));
    std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    int labels_row = find_line_starting(lines, "slot");
    REQUIRE(labels_row > 0);
    std::vector<int> columns = slot_columns(lines[static_cast<size_t>(labels_row)], header_labels());
    for (size_t i = 0; i < columns.size(); ++i) REQUIRE(columns[i] >= 0);
    CHECK(contains(lines[0], "HEADER"));
    CHECK(find_line(lines, "header  slot h7/8  symbol 2  tone 2") > 0);
    int ref_row = find_line_starting(lines, " ref");
    int top_row = find_line_starting(lines, "   7");  // the header uses 8 tones
    REQUIRE(ref_row > top_row);
    REQUIRE(top_row > 0);
    for (int i = 0; i < k_slots; ++i) {
        const std::vector<int> lit = lit_rows(lines, columns[static_cast<size_t>(1 + i)], top_row, ref_row);
        REQUIRE(lit.size() == 1u);
        const int tones_per_row = (k_slots + (ref_row - top_row) - 1) / (ref_row - top_row);
        CHECK_EQ(lit[0], ref_row - 1 - k_hf_header_tones[i] / tones_per_row);
    }
    CHECK_EQ(cell_at(lines[static_cast<size_t>(labels_row) + 3], columns[k_slots]), k_caret);

    // The header STOP, then the first three peaks of a frame: the caret follows the slot being sent.
    tui.on_encoder_status(make_status(EncoderSegment::header, SlotKind::marker, k_slots, 0, 0, slot++));
    lines = render_lines(tui, k_columns, k_rows);
    CHECK_EQ(cell_at(lines[static_cast<size_t>(labels_row) + 3], columns.back()), k_caret);
    CHECK(find_line(lines, "header  STOP marker") > 0);
    for (int i = 0; i < 3; ++i) {
        EncoderStatus status = make_status(EncoderSegment::frame, SlotKind::peak, static_cast<uint8_t>(i),
                                           k_vector_symbols[i], k_vector_tones[i], slot++);
        status.byte = k_vector_bytes[0];
        tui.on_encoder_status(status);
    }
    lines = render_lines(tui, k_columns, k_rows);
    labels_row = find_line_starting(lines, "slot");
    REQUIRE(labels_row > 0);
    columns = slot_columns(lines[static_cast<size_t>(labels_row)], numbered_labels(k_slots));
    for (size_t i = 0; i < columns.size(); ++i) REQUIRE(columns[i] >= 0);
    CHECK(contains(lines[0], "FRAME"));
    const std::string hz = std::to_string(static_cast<int>(std::lround(hf_tone_hz(k_vector_tones[2]) * 10) / 10));
    const int title_row = find_line(lines, "frame  slot 3/8  symbol 20  tone 8 ");
    REQUIRE(title_row > 0);
    CHECK(contains(lines[static_cast<size_t>(title_row)], hz));
    CHECK(contains(lines[static_cast<size_t>(title_row)], "first byte 0x48 'H'"));
    ref_row = find_line_starting(lines, " ref");
    top_row = find_line_starting(lines, "  31");
    REQUIRE(ref_row > top_row);
    REQUIRE(top_row > 0);
    const int tones_per_row = (k_tones + (ref_row - top_row) - 1) / (ref_row - top_row);
    for (int i = 0; i < k_slots; ++i) {
        const std::vector<int> lit = lit_rows(lines, columns[static_cast<size_t>(1 + i)], top_row, ref_row);
        if (i >= 3) {
            CHECK(lit.empty());  // not sent yet
            continue;
        }
        REQUIRE(lit.size() == 1u);
        CHECK_EQ(lit[0], ref_row - 1 - k_vector_tones[i] / tones_per_row);
        CHECK_EQ(word_at(lines[static_cast<size_t>(labels_row) + 1], columns[static_cast<size_t>(1 + i)]),
                 std::to_string(k_vector_tones[i]));
        CHECK_EQ(word_at(lines[static_cast<size_t>(labels_row) + 2], columns[static_cast<size_t>(1 + i)]),
                 std::to_string(k_vector_symbols[i]));
    }
    const std::string& caret_line = lines[static_cast<size_t>(labels_row) + 3];
    CHECK_EQ(count_of_glyph(caret_line, k_caret), 1);
    CHECK_EQ(cell_at(caret_line, columns[3]), k_caret);

    const uint32_t tune_slot = 23;  // cell 23 % 10 = 3
    tui.on_encoder_status(make_status(EncoderSegment::tune, SlotKind::tone, 0, 0, 0, tune_slot));
    lines = render_lines(tui, k_columns, k_rows);
    labels_row = find_line_starting(lines, "slot");
    REQUIRE(labels_row > 0);
    const Cells labels = cells_of(lines[static_cast<size_t>(labels_row)]);
    std::vector<int> tone_cells;
    for (size_t i = 0; i < labels.size(); ++i)
        if (labels[i] == 'T') tone_cells.push_back(static_cast<int>(i));
    REQUIRE(tone_cells.size() == static_cast<size_t>(k_cells));
    CHECK_EQ(count_of_glyph(lines[static_cast<size_t>(labels_row) + 3], k_caret), 1);
    CHECK_EQ(cell_at(lines[static_cast<size_t>(labels_row) + 3], tone_cells[tune_slot % k_cells]), k_caret);
    CHECK(contains(lines[0], "TUNE"));
    ref_row = find_line_starting(lines, " ref");
    REQUIRE(ref_row > 0);
    CHECK_EQ(cell_at(lines[static_cast<size_t>(ref_row)], tone_cells[0]), k_full_block);  // the tune tone is f_ref
}

TEST(tui_encoder_rebuilds_the_sent_bytes) {
    Tui tui(TuiMode::encoder);
    tui.set_color(false);
    encoder_mode(tui);
    uint32_t slot = k_start_slot;
    slot = send_frame(tui, k_vector_symbols, k_vector_tones, k_slots, slot, 3);  // repeated statuses count once
    // Short final frame: 'o' = 0x6F = 01101 111(00) in two 5-bit peaks gives floor(2 * 5 / 8) = 1 byte.
    const uint8_t short_symbols[] = {13, 28};
    slot = send_frame(tui, short_symbols, k_vector_tones, count_of(short_symbols), slot);
    tui.on_encoder_status(make_status(EncoderSegment::eot, SlotKind::marker, 0, 0, 0, slot++));
    tui.on_encoder_status(make_status(EncoderSegment::idle, SlotKind::silent, 0, 0, 0, slot));
    // A new transmission restarts slot_index: its first frame may have the key of an earlier one.
    send_frame(tui, k_vector_symbols, k_vector_tones, k_slots, k_start_slot);

    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    const int title = find_line(lines, "sent");
    REQUIRE(title > 0);
    const std::string vector_text = "Hi!\xc2\xb7\xc2\xb7";  // 48 69 21 00 FF
    CHECK(contains(lines[static_cast<size_t>(title) + 1], vector_text + "o" + vector_text));
    CHECK(contains(lines[1], "bytes 11"));
    CHECK(contains(lines[1], "k5 N8 standard below 138.9 bit/s"));

    Tui without_mode(TuiMode::encoder);  // no set_mode(): no k, no text
    send_frame(without_mode, k_vector_symbols, k_vector_tones, k_slots, k_start_slot);
    CHECK(contains(render_lines(without_mode, k_columns, k_rows)[1], "bytes 0"));
}

TEST(tui_spectrum_draws_the_grid) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    tui.on_event(mode_event(EventType::locked));
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    const int title = find_line(lines, "spectrum");
    REQUIRE(title > 0);
    const int low = static_cast<int>(std::lround(hf_tone_hz(k_tones - 1)));
    const int high = static_cast<int>(std::lround(hf_tone_hz(0)));
    CHECK(contains(lines[static_cast<size_t>(title)], "grid " + std::to_string(low) + "-" + std::to_string(high) +
                                                          " Hz"));
    const int axis = find_line(lines, "\xe2\x96\xb2", title + 1);  // f_ref caret
    REQUIRE(axis > title + 1);
    const Cells floor_row = cells_of(lines[static_cast<size_t>(axis) - 1]);
    const double column_hz = 2700.0 / (k_columns - 1);
    int dots = 0;
    for (size_t column = 0; column < floor_row.size(); ++column) {
        if (floor_row[column] != k_middle_dot) continue;
        ++dots;
        const double hz = 300.0 + column * column_hz;
        CHECK(hz >= low - column_hz && hz <= high + column_hz);
    }
    NOTE("%d grid columns for %d tones", dots, k_tones);
    CHECK(dots >= k_tones * 3 / 4);  // 35.7 Hz apart on 34.2 Hz columns: few share a column
    const int marker = find_in(cells_of(lines[static_cast<size_t>(axis)]), "\xe2\x96\xb2");
    CHECK_NEAR(300.0 + marker * column_hz, k_ref_hz, column_hz);
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
        tui.on_event(byte_event(static_cast<uint8_t>(letter), 0, 0));
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
    CHECK(contains(full, "conf"));
    CHECK(contains(full, "spectrum"));
    CHECK(contains(full, "received"));

    const int short_rows = 6;
    const std::string short_frame = tui.render(k_columns, short_rows);
    CHECK(contains(short_frame, "RX"));
    CHECK(contains(short_frame, "received"));
    CHECK(contains(short_frame, "spectrum"));
    CHECK(!contains(short_frame, "scope"));
    CHECK(!contains(short_frame, "conf"));

    const int tiny_rows = 4;
    const std::string tiny_frame = tui.render(k_columns, tiny_rows);
    CHECK(contains(tiny_frame, "received"));
    CHECK(!contains(tiny_frame, "spectrum"));

    const int narrow = 15;
    const std::string narrow_frame = tui.render(narrow, k_rows);
    CHECK(!contains(narrow_frame, "frame 42"));
    CHECK(contains(narrow_frame, "spec"));

    const std::string one_row = tui.render(k_columns, 1);
    CHECK_EQ(split_lines(one_row).size(), 1u);
    CHECK(contains(one_row, "TRACK"));
}

TEST(tui_scope_draws_the_envelope) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    tui.set_slot_ms(k_slot_ms);  // window = 2 slots = 512 samples at 8 kHz
    const size_t half_window = static_cast<size_t>(k_slot_ms) * k_rate / 1000;
    const std::vector<int16_t> silence(half_window, 0);
    const std::vector<int16_t> burst = tone(k_tone_hz, k_rate, half_window, k_near_full_scale);
    tui.push_audio(silence.data(), silence.size(), k_rate);
    tui.push_audio(burst.data(), burst.size(), k_rate);
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    const int title = find_line(lines, "scope 64 ms");
    REQUIRE(title > 0);
    const int next_title = find_line(lines, "peaks", title + 1);
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
    const size_t window = static_cast<size_t>(k_slot_ms) * 2 * k_rate / 1000;
    const std::vector<int16_t> steady = tone(k_tone_hz, k_rate, window, k_near_full_scale);
    int reached[2] = {0, 0};  // dot columns with any dot in the top braille row
    for (int known = 0; known < 2; ++known) {
        Tui tui(TuiMode::decoder);
        tui.set_color(false);
        tui.set_slot_ms(k_slot_ms);
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
    for (size_t r = 0; r < test::count_of(rates); ++r) {
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
                levels[static_cast<size_t>(column)] +=
                    block_eighths(cell_at(lines[static_cast<size_t>(row)], column));
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
}
