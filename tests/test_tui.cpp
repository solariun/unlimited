#include "terminal.hpp"
#include "test_harness.hpp"
#include "tui.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

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
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSegment;
using unlimited::EncoderStatus;
using unlimited::Event;
using unlimited::EventType;
using unlimited::LostReason;
using unlimited::Passband;
using unlimited::Preset;
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
const float k_received_hz = 1580.0f;  // the hf pitch 80 Hz mistuned
const double k_half_scale = 16384.0;
const double k_near_full_scale = 30000.0;
const double k_half_scale_dbfs = -6.02;
const double k_scalloping_tolerance_db = 1.5;  // Hann window, tone up to half a bin off the column frequency
const float k_slot_ms = 16.0f;
const float k_snr_db = 12.5f;
const uint8_t k_bits = 8;
const int k_columns = 80;
const int k_rows = 24;
const int k_wide_columns = 120;
const int k_tall_rows = 30;
const Passband k_ssb_passband = {300, 2700};

// "H" = 0x48: the bits of one N = 8 package, their levels and decision lines (% of the reference line).
const uint8_t k_h_bits[k_bits] = {0, 1, 0, 0, 1, 0, 0, 0};
const uint8_t k_one_pct = 100;
const uint8_t k_zero_pct = 10;
const uint8_t k_line_pct = 60;
const uint8_t k_flat_pct = 100;

// The 80 x 24 decoder layout (plan_layout): 7 bar rows, the reference crest (100 %) on the 5th from the bottom, 140 %
// at the top, 20 % per row.
const int k_bar_rows = 7;
const int k_reference_row = 4;  // from the bottom
const int k_line_row = 2;       // 60 %: nearest row edge 60 %, the 3rd row
const int k_cell_columns = 3;   // bar of 2 columns, 1 gap

const uint32_t k_escape = 0x1B;
const uint32_t k_space = 0x20;
const uint32_t k_decision_glyph = 0x2500;   // ─
const uint32_t k_reference_glyph = 0x254C;  // ╌
const uint32_t k_full_block = 0x2588;
const uint32_t k_lower_eighth = 0x2581;
const uint32_t k_caret = 0x25B2;            // ▲
const uint32_t k_marker = 0x25C6;           // ◆
const uint32_t k_pending_marker = 0x25C7;   // ◇
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

// The bits row of the packages panel: the first line below `title` holding a marker glyph.
int bits_row(const std::vector<std::string>& lines, int title) {
    for (size_t i = static_cast<size_t>(title) + 1; i < lines.size(); ++i)
        if (count_glyph(lines[i], k_marker) > 0 || count_glyph(lines[i], k_pending_marker) > 0)
            return static_cast<int>(i);
    return -1;
}

// Row `level` of the bars (0 = the bottom one), in a column.
uint32_t bar_cell(const std::vector<std::string>& lines, int bits, int level, int column) {
    return cell_at(lines[static_cast<size_t>(bits - 1 - level)], column);
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

// An event of an hf lock (N = 8, T = 16 ms) received 80 Hz high.
Event lock_event(EventType type, uint8_t bits = k_bits) {
    Event event = make_event(type, DecoderState::track);
    event.bits_per_package = bits;
    event.tone_hz = k_received_hz;
    event.slot_ms = k_slot_ms;
    event.snr_db = k_snr_db;
    return event;
}

Event slot_event(uint32_t package, uint8_t slot, uint8_t bit, uint8_t level, uint8_t threshold, uint8_t start,
                 uint8_t stop, uint8_t bits = k_bits) {
    Event event = lock_event(EventType::slot, bits);
    event.package_index = package;
    event.slot = slot;
    event.value = bit;
    event.level_pct = level;
    event.threshold_pct = threshold;
    event.start_pct = start;
    event.stop_pct = stop;
    return event;
}

Event package_event(uint32_t package, uint8_t count, uint8_t start, uint8_t stop, uint8_t bits = k_bits) {
    Event event = lock_event(EventType::package, bits);
    event.package_index = package;
    event.value = count;
    event.start_pct = start;
    event.stop_pct = stop;
    event.slot_ms = k_slot_ms;
    return event;
}

Event byte_event(uint8_t value, uint32_t byte_index, uint32_t package) {
    Event event = lock_event(EventType::byte);
    event.value = value;
    event.byte_index = byte_index;
    event.package_index = package;
    return event;
}

// One package: its slot events (ones at k_one_pct, zeros at k_zero_pct, all with the same decision line) and its
// package event.
void send_package(Tui& tui, uint32_t index, const uint8_t* bits, uint8_t count, uint8_t threshold, uint8_t start,
                  uint8_t stop, uint8_t bits_per_package = k_bits) {
    for (uint8_t i = 0; i < count; ++i)
        tui.on_event(slot_event(index, static_cast<uint8_t>(i + 1), bits[i], bits[i] ? k_one_pct : k_zero_pct,
                                threshold, start, stop, bits_per_package));
    tui.on_event(package_event(index, count, start, stop, bits_per_package));
}

void feed_decoder(Tui& tui) {
    const std::vector<int16_t> audio = tone(k_tone_hz, k_rate, k_rate / 4, k_half_scale);
    tui.push_audio(audio.data(), audio.size(), k_rate);
    tui.set_profile("ssb 8-64 ms");
    tui.set_passband(k_ssb_passband);
    send_package(tui, 0, k_h_bits, k_bits, k_line_pct, k_flat_pct, k_flat_pct);
    tui.on_event(lock_event(EventType::locked));
    const std::string text = "CQ CQ DE UNLIMITED";
    for (size_t i = 0; i < text.size(); ++i)
        tui.on_event(byte_event(static_cast<uint8_t>(text[i]), static_cast<uint32_t>(i), static_cast<uint32_t>(i)));
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
    return samples;
}

struct Never {
    bool operator()(const EncoderStatus&) const { return false; }
};

EncoderConfig hf_config(uint8_t bits_per_package) {
    EncoderConfig config = EncoderConfig::from_preset(Preset::hf, k_rate);
    config.bits_per_package = bits_per_package;
    return config;
}

void encoder_view(Tui& tui, const EncoderConfig& config) {
    tui.set_color(false);
    tui.set_profile("hf");
    tui.set_tone_hz(config.tone_hz);
    tui.set_slot_ms(config.slot_us / 1000.0f);
    tui.set_package(config.bits_per_package);
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
    const EncoderConfig config = hf_config(k_bits);
    encoder_view(encoder, config);
    size_t slots = 0;
    feed_encoder(encoder, config, bytes_of("CQ CQ DE UNLIMITED"), 1, [&](const EncoderStatus& status) {
        return status.segment == EncoderSegment::package && status.package_index == 7 && ++slots > 0;
    });
    Tui wide(TuiMode::decoder);  // N = 32: 34 cells in one package
    const uint8_t wide_bits = 32;
    std::vector<uint8_t> pattern(wide_bits);
    for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<uint8_t>(i % 3 == 0);
    send_package(wide, 0, pattern.data(), wide_bits, k_line_pct, k_flat_pct, k_flat_pct, wide_bits);
    Tui single(TuiMode::decoder);  // N = 1: many short packages
    const uint8_t one_bit = 1;
    for (uint32_t k = 0; k < 40; ++k) {
        const uint8_t bit = static_cast<uint8_t>(k % 2);
        send_package(single, k, &bit, one_bit, k_line_pct, static_cast<uint8_t>(90 + k % 20),
                     static_cast<uint8_t>(95 + k % 15), one_bit);
    }
    Tui* const views[] = {&decoder, &encoder, &wide, &single};
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

// The picture of spec 3.10: the START and STOP bars, the dashed reference line from crest to crest, the decision
// line, one bar per data slot with its bit under it and the byte under the bits.
TEST(tui_decoder_package_picture) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    send_package(tui, 0, k_h_bits, k_bits, k_line_pct, k_flat_pct, k_flat_pct);
    tui.on_event(lock_event(EventType::locked));
    tui.on_event(byte_event('H', 0, 0));
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);

    const int title = find_line(lines, "package 0  8 bits");
    REQUIRE(title >= 0);
    CHECK(contains(lines[static_cast<size_t>(title)], "START 100%  STOP 100%  line 60% of ref"));
    const int bits = bits_row(lines, title);
    REQUIRE(bits == title + k_bar_rows + 1);
    const std::vector<int> columns = glyph_columns(lines[static_cast<size_t>(bits)]);
    REQUIRE(columns.size() == static_cast<size_t>(k_bits) + 2);
    const Cells glyphs = glyphs_at(lines[static_cast<size_t>(bits)], columns);
    CHECK_EQ(glyphs.front(), k_marker);
    CHECK_EQ(glyphs.back(), k_marker);
    for (int i = 0; i < k_bits; ++i)
        CHECK_EQ(glyphs[static_cast<size_t>(i) + 1], k_h_bits[i] ? uint32_t('1') : uint32_t('0'));
    for (size_t i = 1; i < columns.size(); ++i) CHECK_EQ(columns[i] - columns[i - 1], k_cell_columns);
    CHECK(contains(lines[static_cast<size_t>(bits) + 1], "0x48 'H'"));
    CHECK(find_in(cells_of(lines[static_cast<size_t>(bits - 1 - k_reference_row)]), "100%") == 0);

    for (size_t i = 0; i < columns.size(); ++i) {
        const int column = columns[i];
        const bool marker = i == 0 || i + 1 == columns.size();
        const bool one = !marker && k_h_bits[i - 1] != 0;
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

// A fade across the package: START at 60 %, STOP at 120 %. The reference line climbs from the START crest to the
// STOP crest and the decision line (fixed 70 % of it) follows.
TEST(tui_decoder_reference_line_follows_the_crests) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    const uint8_t start = 60;
    const uint8_t stop = 120;
    const uint8_t fixed_line = 70;
    const uint8_t zeros[k_bits] = {0, 0, 0, 0, 0, 0, 0, 0};
    send_package(tui, 3, zeros, k_bits, fixed_line, start, stop);
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    const int title = find_line(lines, "package 3");
    REQUIRE(title >= 0);
    const int bits = bits_row(lines, title);
    REQUIRE(bits > title);
    const std::vector<int> columns = glyph_columns(lines[static_cast<size_t>(bits)]);
    REQUIRE(columns.size() == static_cast<size_t>(k_bits) + 2);
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

TEST(tui_decoder_consecutive_packages_share_their_marker) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    const uint8_t n = 4;
    const uint8_t first[n] = {0, 1, 0, 0};
    const uint8_t second[n] = {1, 0, 0, 0};
    const uint8_t third[n] = {0, 1, 1, 0};
    send_package(tui, 4, first, n, k_line_pct, k_flat_pct, k_flat_pct, n);
    send_package(tui, 5, second, n, k_line_pct, k_flat_pct, k_flat_pct, n);
    send_package(tui, 7, third, n, k_line_pct, k_flat_pct, k_flat_pct, n);  // package 6 was lost
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    const int title = find_line(lines, "package 7  4 bits");
    REQUIRE(title >= 0);
    const int bits = bits_row(lines, title);
    REQUIRE(bits > title);
    const std::vector<int> columns = glyph_columns(lines[static_cast<size_t>(bits)]);
    const Cells glyphs = glyphs_at(lines[static_cast<size_t>(bits)], columns);
    const std::string expected = "M0100M1000MM0110M";  // M: a marker; 4 and 5 share one, 7 starts after a gap
    REQUIRE(glyphs.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        CHECK_EQ(glyphs[i], expected[i] == 'M' ? k_marker : static_cast<uint32_t>(expected[i]));
    const int shared = columns[10];
    const int restart = columns[11];
    CHECK_EQ(restart - shared, 2 * k_cell_columns);  // one empty cell between package 5's STOP and package 7's START
    // The byte of packages 4 and 5 (N = 4: two packages per byte) is one bracket across their shared marker.
    tui.on_event(byte_event(0x48, 2, 5));
    const std::vector<std::string> labelled = render_lines(tui, k_columns, k_rows);
    const std::string& labels = labelled[static_cast<size_t>(bits) + 1];
    const int label = find_in(cells_of(labels), "0x48 'H'");
    REQUIRE(label > 0);
    CHECK(label > columns[1] && label < columns[9]);
    CHECK_EQ(cell_at(labels, columns[1]), uint32_t(0x2514));  // └ under the first bit of package 4
    CHECK_EQ(cell_at(labels, columns[9] + 1), uint32_t(0x2518));  // ┘ under the last bit of package 5
}

TEST(tui_decoder_status_counts_and_text) {
    Tui tui(TuiMode::decoder);
    tui.set_color(false);
    tui.set_profile("ssb 8-64 ms");
    tui.set_passband(k_ssb_passband);
    tui.set_field("channel", "usb 10 dB");
    std::vector<std::string> lines = render_lines(tui, k_wide_columns, k_tall_rows);
    CHECK(contains(lines[0], "RX ssb 8-64 ms"));
    CHECK(contains(lines[0], "SEARCH, DCD off"));
    CHECK(contains(lines[0], "pitch --"));
    CHECK(find_line(lines, "packages  waiting for a lock") > 0);

    tui.on_event(make_event(EventType::state, DecoderState::acquire));
    Event locked = lock_event(EventType::locked);
    locked.flags = unlimited::event_flag_late_join;
    tui.on_event(locked);
    const uint8_t bytes[] = {'H', 'i', '\n', 0x07};
    for (size_t i = 0; i < count_of(bytes); ++i) {
        Event byte = byte_event(bytes[i], static_cast<uint32_t>(i), static_cast<uint32_t>(i));
        byte.flags = unlimited::event_flag_late_join | unlimited::event_flag_flywheel_stop;
        tui.on_event(byte);
    }
    lines = render_lines(tui, k_wide_columns, k_tall_rows);
    const std::string status = lines[0] + lines[1] + lines[2];
    CHECK(contains(status, "TRACK, DCD on"));
    CHECK(contains(status, "1580.0 Hz"));
    CHECK(contains(status, "T 16.00 ms"));
    CHECK(contains(status, "N 8"));
    CHECK(contains(status, "55.6 bit/s"));
    CHECK(contains(status, "SNR 12.5 dB"));
    CHECK(contains(status, "band 1442-1718 Hz fits, shift -1142/+982 Hz"));  // received band in the ssb passband
    CHECK(contains(status, "passband 300-2700 Hz"));
    CHECK(contains(status, "4 bytes"));
    CHECK(contains(status, "locks 1  lost 0  ends 0"));
    CHECK(contains(status, "last: locked (late join)"));
    CHECK(contains(status, "channel usb 10 dB"));
    const int text_title = find_line(lines, "received");
    REQUIRE(text_title > 0);
    CHECK(contains(lines[static_cast<size_t>(text_title)], "4 bytes  late join  STOP flywheeled"));
    CHECK(contains(lines[static_cast<size_t>(text_title) + 1], "Hi\xe2\x86\xb5\xc2\xb7"));  // "Hi↵·"

    tui.on_event(make_event(EventType::end, DecoderState::search));
    tui.on_event(make_event(EventType::state, DecoderState::search));
    Event lost = make_event(EventType::lost, DecoderState::acquire);
    lost.reason = LostReason::signal_gone;
    tui.on_event(lost);
    tui.set_field("channel", "");
    lines = render_lines(tui, k_wide_columns, k_tall_rows);
    const std::string after = lines[0] + lines[1] + lines[2];
    CHECK(contains(after, "ACQUIRE, DCD on"));
    CHECK(contains(after, "locks 1  lost 1  ends 1"));
    CHECK(contains(after, "last: lost (signal gone)"));
    CHECK(!contains(after, "channel"));
}

// The encoder view rebuilds the sent text from the bits of every package, so a short final package is exact.
TEST(tui_encoder_rebuilds_the_sent_bytes) {
    const uint8_t sizes[] = {1, 3, 8, 16, 32};
    const std::string first = "Hello";
    const std::string second = " World";
    for (size_t s = 0; s < count_of(sizes); ++s) {
        const EncoderConfig config = hf_config(sizes[s]);
        Tui tui(TuiMode::encoder);
        encoder_view(tui, config);
        feed_encoder(tui, config, bytes_of(first), 3, Never());   // repeated statuses count once
        feed_encoder(tui, config, bytes_of(second), 1, Never());  // a new transmission restarts slot_index
        const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
        const int title = find_line(lines, "sent  11 bytes");
        if (!CHECK(title > 0)) {
            NOTE("N = %u", unsigned(sizes[s]));
            continue;
        }
        CHECK(contains(lines[static_cast<size_t>(title) + 1], first + second));
        CHECK(contains(lines[0] + lines[1] + lines[2], "sent 11 bytes"));
    }
    Tui without_package(TuiMode::encoder);  // no set_package(): the text still comes from the bits
    without_package.set_color(false);
    feed_encoder(without_package, hf_config(k_bits), bytes_of("Hi"), 1, Never());
    CHECK(find_line(render_lines(without_package, k_columns, k_rows), "sent  2 bytes") > 0);
}

// The package being sent: its START, the bits sent so far, the caret on the current slot, the bits of the current
// byte not sent yet, the pending STOP and the bytes under the bits.
TEST(tui_encoder_shows_the_package_being_sent) {
    const EncoderConfig config = hf_config(k_bits);
    Tui tui(TuiMode::encoder);
    encoder_view(tui, config);
    const uint32_t package = 1;
    const uint8_t slot = 3;
    feed_encoder(tui, config, bytes_of("Hi!"), 1, [&](const EncoderStatus& status) {
        return status.segment == EncoderSegment::package && status.package_index == package && status.slot == slot;
    });
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    CHECK(contains(lines[0], "TX hf"));
    CHECK(contains(lines[0], "PACKAGE"));
    CHECK(contains(lines[0], "1500 Hz"));
    const int title = find_line(lines, "package 1  slot 3/9  bit 2 of byte 1 = 0x69 'i'");
    REQUIRE(title > 0);
    const int bits = bits_row(lines, title);
    REQUIRE(bits > title);
    const std::vector<int> columns = glyph_columns(lines[static_cast<size_t>(bits)]);
    const Cells glyphs = glyphs_at(lines[static_cast<size_t>(bits)], columns);
    // ... START 01001000 STOP=START 011 | 01001 (the rest of 'i', not sent yet) STOP (not sent yet).
    const std::string tail = "M01001000M01101001P";
    REQUIRE(glyphs.size() >= tail.size());
    const size_t offset = glyphs.size() - tail.size();
    for (size_t i = 0; i < tail.size(); ++i) {
        const uint32_t expected = tail[i] == 'M' ? k_marker : tail[i] == 'P' ? k_pending_marker : uint32_t(tail[i]);
        CHECK_EQ(glyphs[offset + i], expected);
    }
    // The caret sits under the third bit of package 1.
    const int caret = find_in(cells_of(lines[static_cast<size_t>(bits) + 2]), "\xe2\x96\xb2");
    CHECK_EQ(caret, columns[offset + 10 + slot - 1]);
    CHECK_EQ(count_glyph(lines[static_cast<size_t>(bits) + 2], k_caret), 1);
    // Bars: a beep is a full bar up to the crest, a silence none, the unsent slots none.
    CHECK_EQ(bar_cell(lines, bits, 0, columns[offset + 11]), k_full_block);  // package 1, bit 1 = 1
    CHECK_EQ(bar_cell(lines, bits, 0, columns[offset + 10]), k_space);       // bit 0 = 0
    CHECK_EQ(bar_cell(lines, bits, 0, columns[offset + 13]), k_space);       // bit 3, not sent yet
    const std::string& labels = lines[static_cast<size_t>(bits) + 1];
    CHECK(contains(labels, "0x48 'H'"));
    CHECK(contains(labels, "0x69 'i'"));
    CHECK(find_line(lines, "sent  1 byte") > bits);  // the text panel: 'H' so far

    // The view starts on a marker (whole packages) and the tune and sync segments are named.
    CHECK(glyphs.front() == k_marker);
    Tui tune(TuiMode::encoder);
    encoder_view(tune, config);
    feed_encoder(tune, config, bytes_of("Hi"), 1, [](const EncoderStatus& status) {
        return status.segment == EncoderSegment::tune && status.slot_index == 4;
    });
    CHECK(find_line(render_lines(tune, k_columns, k_rows), "tune tone  slot 5") > 0);
    Tui sync(TuiMode::encoder);
    encoder_view(sync, config);
    feed_encoder(sync, config, bytes_of("Hi"), 1,
                 [](const EncoderStatus& status) { return status.segment == EncoderSegment::sync; });
    const std::vector<std::string> sync_lines = render_lines(sync, k_columns, k_rows);
    CHECK(find_line(sync_lines, "sync train  marker 1") > 0);
    CHECK(find_line(sync_lines, "tune tone") > 0);  // the bracket under the tune slots
}

// Segments and bytes never share a bracket: in a 5-byte message the END markers follow byte 4.
TEST(tui_encoder_end_markers_have_their_own_bracket) {
    const EncoderConfig config = hf_config(k_bits);
    Tui tui(TuiMode::encoder);
    encoder_view(tui, config);
    bool in_end = false;
    uint32_t first_end = 0;
    feed_encoder(tui, config, bytes_of("Hello"), 1, [&](const EncoderStatus& status) {
        if (status.segment != EncoderSegment::end) return false;
        if (!in_end) first_end = status.slot_index;
        in_end = true;
        return status.slot_index == first_end + 1;
    });
    const std::vector<std::string> lines = render_lines(tui, k_columns, k_rows);
    const int title = find_line(lines, "END  marker 2/2");
    REQUIRE(title > 0);
    const int bits = bits_row(lines, title);
    REQUIRE(bits > title);
    const std::vector<int> columns = glyph_columns(lines[static_cast<size_t>(bits)]);
    REQUIRE(columns.size() >= 3u);
    const std::string& labels = lines[static_cast<size_t>(bits) + 1];
    const int end_label = find_in(cells_of(labels), "END");
    const int byte_label = find_in(cells_of(labels), "0x6F 'o'");
    REQUIRE(end_label > 0);
    REQUIRE(byte_label > 0);
    CHECK(end_label > columns[columns.size() - 3]);  // under the two END markers, after the last STOP
    CHECK(byte_label < columns[columns.size() - 3]);
    CHECK_EQ(cell_at(labels, columns[columns.size() - 3]), k_space);  // nothing under the last STOP
}

// Spec 1.5: the status shift is the one a receiver follows. The fm preset's receiver searches from 1000 Hz: 500 Hz
// below 1500 Hz, not the filter's 650 Hz; a decoder prints what its own search_range() still follows.
TEST(tui_status_shift_follows_the_search) {
    Tui encoder(TuiMode::encoder);
    encoder_view(encoder, EncoderConfig::from_preset(Preset::fm, k_rate));
    std::vector<std::string> lines = render_lines(encoder, k_wide_columns, k_tall_rows);
    CHECK(contains(lines[0] + lines[1] + lines[2], "band 950-2050 Hz fits, shift -500/+950 Hz"));

    Tui decoder(TuiMode::decoder);
    decoder.set_color(false);
    const unlimited::DecoderConfig fm = unlimited::DecoderConfig::for_profile(unlimited::Profile::fm);
    decoder.set_passband(fm.passband);
    decoder.set_search_range(fm.search_range());
    decoder.set_tone_hz(1100.0f);
    decoder.set_slot_ms(4.0f);
    lines = render_lines(decoder, k_wide_columns, k_tall_rows);
    CHECK(contains(lines[0] + lines[1] + lines[2], "band 550-1650 Hz fits, shift -100/+1350 Hz"));
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
    for (int column = spectrum_column(1362.0, k_columns); column <= spectrum_column(1638.0, k_columns); ++column)
        if (column != pitch) CHECK_EQ(cell_at(row, column), k_band_glyph);  // the occupied band of hf
    CHECK(cell_at(row, spectrum_column(1200.0, k_columns)) != k_band_glyph);
    CHECK(contains(lines[static_cast<size_t>(markers) + 1], "1.5k"));

    // A band that leaves the passband is drawn red where it is outside.
    tui.set_tone_hz(2600.0f);  // 16 ms: 2462-2738 Hz in 300-2700 Hz
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
        tui.on_event(byte_event(static_cast<uint8_t>(letter), static_cast<uint32_t>(i), static_cast<uint32_t>(i)));
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
    CHECK(contains(full, "package 0"));
    CHECK(contains(full, "spectrum"));
    CHECK(contains(full, "received"));

    const int short_rows = 6;  // status, text, spectrum
    const std::string short_frame = tui.render(k_columns, short_rows);
    CHECK(contains(short_frame, "RX"));
    CHECK(contains(short_frame, "received"));
    CHECK(contains(short_frame, "spectrum"));
    CHECK(!contains(short_frame, "scope"));
    CHECK(!contains(short_frame, "package 0"));

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
    const int next_title = find_line(lines, "packages", title + 1);
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
}
