#include "support/loopback.hpp"

#include "channel.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"
#include "unlimited/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <functional>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Writes the documentation figures (SVG) into the directory given on the command line. Every waveform, spectrum,
// slot level, decode result and BER point is measured on the output of the real Encoder, the real sim::Channel and
// the real Decoder each time `make docs` runs, with fixed seeds: a rerun writes the same bytes.
namespace {

using std::int16_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint8_t;
using unlimited::Band;
using unlimited::DecoderConfig;
using unlimited::DecoderState;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSegment;
using unlimited::EncoderStatus;
using unlimited::Event;
using unlimited::EventType;
using unlimited::Passband;
using unlimited::PassbandFit;
using unlimited::SlotKind;

namespace lb = unlimited::loopback;
namespace sim = unlimited::sim;

// ---------------------------------------------------------------------------
// Units, conventions and shared constants
// ---------------------------------------------------------------------------

const double k_pi = 3.14159265358979323846;
const double k_us_per_ms = 1e3;
const double k_ms_per_s = 1e3;
const double k_full_scale = 32768.0;
const double k_int16_max = 32767.0;
const double k_int16_min = -32768.0;
const double k_power_db = 10.0;
const double k_percent = 100.0;
const double k_reference_bandwidth_hz = 2500.0;  // SNR: key-down tone power over noise in 2500 Hz (spec 1.3)
const double k_receiver_bandwidth_hz = 2400.0;   // the ssb profile's 300..2700 Hz
const double k_noise_peak_sigmas = 5.0;          // channel output scaled so that 5 sigma of noise does not clip
const double k_headroom = 0.9;
const double k_fading_peak = 3.0;                // Rayleigh amplitude above 3x RMS: probability 1.2e-4
const double k_fm_peak = 2.0;                    // FM clicks
const uint32_t k_rate_hz = unlimited::k_decoder_rate_hz;
const uint32_t k_wave_rate_hz = 48000;           // slot shapes and timeline: smooth carrier cycles
const size_t k_decoder_chunk = 256;
const size_t k_format_buffer = 1024;
const unsigned k_byte_bits = unlimited::k_bits_per_byte;
const uint8_t k_first_printable = 0x20;
const uint8_t k_last_printable = 0x7E;
const char* const k_hi_text = "Hi";
const char* const k_channel_text = "CQ CQ DE UNLIMITED";
const double k_silence_ms = 700.0;
const uint32_t k_seed = 20260928;
const double k_figure_offset_hz = 37.0;          // the receiver mistuned: the pitch is found, not given
const double k_narrow_filter_high_hz = 2100.0;   // a 1.8 kHz SSB filter's upper edge
const double k_figure_impulse_rate_hz = 5.0;
const double k_figure_carrier_hz = 1200.0;
const double k_figure_interferer_db = -6.0;
const double k_spectrogram_snr_db = 10.0;
const double k_carrier_silence_ms = 4000.0;      // the search masks a steady carrier after 2.5 s
const double k_spectrogram_margin_s = 0.3;
const float k_speeds[] = {1.0f, 3.0f, 6.0f, 12.0f, 25.0f};  // spec 1.3
// v0.3's gates at the same T (spec 4, provisional): the key-down tone in 2500 Hz.
const double k_gate_db[] = {-6.5, -1.7, 1.3, 4.3, 8.0};

// ---------------------------------------------------------------------------
// Palette and fonts (light card: legible on GitHub in light and dark themes)
// ---------------------------------------------------------------------------

const char* const k_ink = "#2C2C2A";
const char* const k_muted = "#5F5E5A";
const char* const k_card = "#FFFFFF";
const char* const k_card_border = "#D3D1C7";
const char* const k_axis = "#888780";
const char* const k_grid_line = "#ECEAE2";
const char* const k_teal = "#1D9E75";          // data 1: a beep
const char* const k_teal_dark = "#0F6E56";
const char* const k_purple = "#7F77DD";        // START and STOP
const char* const k_purple_light = "#AFA9EC";  // the VOX lead
const char* const k_gray = "#888780";          // guides
const char* const k_gray_fill = "#F1EFE8";     // data 0: silence
const char* const k_gray_mid = "#D8D6CC";
const char* const k_row_band = "#F8F7F3";
const char* const k_coral = "#D85A30";         // noise, impairments
const char* const k_amber = "#BA7517";         // decision line
const char* const k_blue = "#378ADD";
const char* const k_pink = "#D4537E";
const char* const k_font_family = "-apple-system, 'Segoe UI', Helvetica, Arial, sans-serif";

const double k_title_size = 16.0;
const double k_subtitle_size = 12.0;
const double k_label_size = 12.0;
const double k_tick_size = 11.0;
const double k_note_size = 11.0;
const double k_small_size = 10.0;
const double k_card_radius = 12.0;
const double k_margin = 24.0;
const double k_title_y = 32.0;
const double k_subtitle_y = 52.0;
const double k_line_gap = 16.0;         // baseline to baseline, 11-12 px text
const double k_tick_length = 4.0;
const double k_tick_label_gap = 14.0;   // axis line to tick label baseline
const double k_axis_title_gap = 30.0;   // axis line to axis title baseline
const double k_text_rise = 4.0;         // lifts a label above a line, or centres 11 px text on one
const double k_panel_title_rise = 26.0; // panel title baseline above the panel
const double k_panel_note_rise = 11.0;  // panel subtitle baseline above the panel
const double k_hairline = 0.5;
const double k_card_inset = 0.5;         // the card's border on the pixel grid
const double k_cell_radius = 3.0;        // rounded corners of cells and small boxes
const double k_bar_radius = 2.0;
const double k_tick_label_pad = 2.0;     // y-axis tick label to its tick
const double k_curve = 1.8;              // measured BER curves
const double k_spectrum_trace = 1.1;
const double k_thin = 0.8;
const double k_line = 1.0;
const double k_medium = 1.5;
const char* const k_dash = "5 3";
const char* const k_guide_dash = "3 3";

struct Font {
    double size;
    const char* color;
    const char* anchor;
    bool bold;
};

Font font(double size, const char* color = k_ink, const char* anchor = "start", bool bold = false) {
    const Font f = {size, color, anchor, bold};
    return f;
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

std::string format(const char* pattern, ...) __attribute__((format(printf, 1, 2)));

std::string format(const char* pattern, ...) {
    char buffer[k_format_buffer];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(buffer, sizeof(buffer), pattern, args);
    va_end(args);
    return buffer;
}

// A number with a typographic minus, never "-0".
std::string number(double value, int decimals) {
    std::string text = format("%.*f", decimals, value);
    if (text[0] != '-') return text;
    if (text.find_first_not_of("-0.") == std::string::npos) return text.substr(1);
    return "\xE2\x88\x92" + text.substr(1);
}

// "+3" / "−2": a signed number with a typographic minus.
std::string signed_number(double value, int decimals) {
    const std::string text = number(value, decimals);
    return value > 0.0 && text.find_first_not_of("0.") != std::string::npos ? "+" + text : text;
}

std::string coordinate(double value) {
    std::string text = format("%.2f", value);
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    return text == "-0" ? "0" : text;
}

std::string escape(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        switch (text[i]) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out += text[i];
        }
    }
    return out;
}

double square(double value) {
    return value * value;
}

double db_power(double ratio) {
    return k_power_db * std::log10(ratio);
}

// Rough rendered width of a text in a sans-serif font, for laying out legends and label boxes.
double text_width(const std::string& text, double size) {
    const double k_em_per_char = 0.52;
    const unsigned char k_continuation_mask = 0xC0;
    const unsigned char k_continuation = 0x80;
    size_t chars = 0;
    for (char c : text) {
        if ((static_cast<unsigned char>(c) & k_continuation_mask) != k_continuation) ++chars;
    }
    return chars * k_em_per_char * size;
}

std::string char_of(uint8_t byte) {
    return byte >= k_first_printable && byte <= k_last_printable ? format("'%c'", byte) : std::string("");
}

std::string binary(unsigned value, unsigned bits) {
    std::string text;
    for (unsigned b = bits; b-- > 0;) text += ((value >> b) & 1u) != 0 ? '1' : '0';
    return text;
}

// ---------------------------------------------------------------------------
// SVG writer
// ---------------------------------------------------------------------------

struct Point {
    double x;
    double y;
};

class Svg {
public:
    Svg(double width, double height) : width_(width), height_(height) {}

    void rect(double x, double y, double w, double h, const std::string& fill, const std::string& stroke = "none",
              double stroke_width = 0.0, double radius = 0.0, double opacity = 1.0) {
        body_ << "<rect x=\"" << coordinate(x) << "\" y=\"" << coordinate(y) << "\" width=\"" << coordinate(w)
              << "\" height=\"" << coordinate(h) << "\" fill=\"" << fill << "\"";
        if (stroke != "none") stroke_attributes(stroke, stroke_width);
        if (radius > 0.0) body_ << " rx=\"" << coordinate(radius) << "\"";
        if (opacity < 1.0) body_ << " fill-opacity=\"" << coordinate(opacity) << "\"";
        body_ << "/>\n";
    }

    void line(double x1, double y1, double x2, double y2, const std::string& stroke, double width,
              const std::string& dash = "") {
        body_ << "<line x1=\"" << coordinate(x1) << "\" y1=\"" << coordinate(y1) << "\" x2=\"" << coordinate(x2)
              << "\" y2=\"" << coordinate(y2) << "\" stroke=\"" << stroke << "\" stroke-width=\"" << coordinate(width)
              << "\"";
        if (!dash.empty()) body_ << " stroke-dasharray=\"" << dash << "\"";
        body_ << "/>\n";
    }

    void polyline(const std::vector<Point>& points, const std::string& stroke, double width,
                  const std::string& dash = "") {
        if (points.size() < 2) return;
        body_ << "<polyline fill=\"none\" stroke=\"" << stroke << "\" stroke-width=\"" << coordinate(width)
              << "\" stroke-linejoin=\"round\"";
        if (!dash.empty()) body_ << " stroke-dasharray=\"" << dash << "\"";
        body_ << " points=\"" << point_list(points) << "\"/>\n";
    }

    void polygon(const std::vector<Point>& points, const std::string& fill, double opacity = 1.0) {
        if (points.size() < 3) return;
        body_ << "<polygon fill=\"" << fill << "\"";
        if (opacity < 1.0) body_ << " fill-opacity=\"" << coordinate(opacity) << "\"";
        body_ << " points=\"" << point_list(points) << "\"/>\n";
    }

    void circle(double x, double y, double r, const std::string& fill, const std::string& stroke = "none",
                double stroke_width = 0.0) {
        body_ << "<circle cx=\"" << coordinate(x) << "\" cy=\"" << coordinate(y) << "\" r=\"" << coordinate(r)
              << "\" fill=\"" << fill << "\"";
        if (stroke != "none") stroke_attributes(stroke, stroke_width);
        body_ << "/>\n";
    }

    void text(double x, double y, const std::string& content, const Font& f, double angle = 0.0) {
        body_ << "<text x=\"" << coordinate(x) << "\" y=\"" << coordinate(y) << "\" font-size=\"" << coordinate(f.size)
              << "\" fill=\"" << f.color << "\"";
        if (std::string(f.anchor) != "start") body_ << " text-anchor=\"" << f.anchor << "\"";
        if (f.bold) body_ << " font-weight=\"bold\"";
        if (angle != 0.0)
            body_ << " transform=\"rotate(" << coordinate(angle) << " " << coordinate(x) << " " << coordinate(y)
                  << ")\"";
        body_ << ">" << escape(content) << "</text>\n";
    }

    // A clip rectangle for the elements drawn with clip_id; returns the id.
    std::string clip(double x, double y, double w, double h) {
        const std::string id = format("clip%u", static_cast<unsigned>(++clips_));
        body_ << "<clipPath id=\"" << id << "\"><rect x=\"" << coordinate(x) << "\" y=\"" << coordinate(y)
              << "\" width=\"" << coordinate(w) << "\" height=\"" << coordinate(h) << "\"/></clipPath>\n";
        return id;
    }

    void begin_clip(const std::string& clip_id) { body_ << "<g clip-path=\"url(#" << clip_id << ")\">\n"; }
    void end_group() { body_ << "</g>\n"; }

    void image(double x, double y, double w, double h, const std::string& png, const std::string& clip_id = "") {
        body_ << "<image x=\"" << coordinate(x) << "\" y=\"" << coordinate(y) << "\" width=\"" << coordinate(w)
              << "\" height=\"" << coordinate(h) << "\" preserveAspectRatio=\"none\"";
        if (!clip_id.empty()) body_ << " clip-path=\"url(#" << clip_id << ")\"";
        body_ << " xlink:href=\"data:image/png;base64," << base64(png) << "\"/>\n";
    }

    void save(const std::string& path, const std::string& title, const std::string& description) const {
        std::ofstream file(path.c_str(), std::ios::binary);
        file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             << "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\""
             << coordinate(width_) << "\" height=\"" << coordinate(height_) << "\" viewBox=\"0 0 "
             << coordinate(width_) << " " << coordinate(height_) << "\" font-family=\"" << k_font_family
             << "\" role=\"img\">\n"
             << "<title>" << escape(title) << "</title>\n"
             << "<desc>" << escape(description) << "</desc>\n"
             << "<rect x=\"" << coordinate(k_card_inset) << "\" y=\"" << coordinate(k_card_inset)
             << "\" width=\"" << coordinate(width_ - 2.0 * k_card_inset) << "\" height=\""
             << coordinate(height_ - 2.0 * k_card_inset) << "\" rx=\"" << coordinate(k_card_radius)
             << "\" fill=\"" << k_card << "\" stroke=\"" << k_card_border << "\"/>\n"
             << body_.str() << "</svg>\n";
        if (!file) throw std::runtime_error("cannot write " + path);
    }

private:
    void stroke_attributes(const std::string& stroke, double width) {
        body_ << " stroke=\"" << stroke << "\" stroke-width=\"" << coordinate(width) << "\"";
    }

    static std::string point_list(const std::vector<Point>& points) {
        std::string list;
        for (size_t i = 0; i < points.size(); ++i)
            list += (i == 0 ? "" : " ") + coordinate(points[i].x) + "," + coordinate(points[i].y);
        return list;
    }

    static std::string base64(const std::string& bytes) {
        static const char k_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const unsigned k_group_bytes = 3;
        const unsigned k_sextet_bits = 6;
        const unsigned k_sextet_mask = 0x3F;
        const unsigned k_bits = 8;
        std::string out;
        for (size_t i = 0; i < bytes.size(); i += k_group_bytes) {
            unsigned group = 0;
            const size_t count = std::min<size_t>(k_group_bytes, bytes.size() - i);
            for (size_t j = 0; j < k_group_bytes; ++j)
                group = (group << k_bits) | (j < count ? static_cast<uint8_t>(bytes[i + j]) : 0u);
            for (size_t j = 0; j <= k_group_bytes; ++j) {
                const unsigned shift = static_cast<unsigned>(k_group_bytes - j) * k_sextet_bits;
                out += j <= count ? k_alphabet[(group >> shift) & k_sextet_mask] : '=';
            }
        }
        return out;
    }

    double width_;
    double height_;
    unsigned clips_ = 0;
    std::ostringstream body_;
};

void heading(Svg& svg, const std::string& title, const std::string& subtitle, const std::string& subtitle2 = "") {
    svg.text(k_margin, k_title_y, title, font(k_title_size, k_ink, "start", true));
    svg.text(k_margin, k_subtitle_y, subtitle, font(k_subtitle_size, k_muted));
    if (!subtitle2.empty()) svg.text(k_margin, k_subtitle_y + k_line_gap, subtitle2, font(k_subtitle_size, k_muted));
}

void notes(Svg& svg, double height, const std::vector<std::string>& lines) {
    const double k_last_baseline = 16.0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const double y = height - k_last_baseline - (lines.size() - 1 - i) * k_line_gap;
        svg.text(k_margin, y, lines[i], font(k_note_size, k_muted));
    }
}

// A filled swatch and its label; returns the x after it.
double legend_item(Svg& svg, double x, double y, const char* fill, const std::string& label,
                   const char* stroke = k_axis) {
    const double k_swatch = 12.0;
    const double k_swatch_gap = 6.0;
    const double k_item_gap = 24.0;
    const double k_swatch_drop = 2.0;  // the swatch sits on the text's baseline
    svg.rect(x, y - k_swatch + k_swatch_drop, k_swatch, k_swatch, fill, stroke, k_hairline, k_bar_radius);
    svg.text(x + k_swatch + k_swatch_gap, y, label, font(k_note_size, k_ink));
    return x + k_swatch + k_swatch_gap + text_width(label, k_note_size) + k_item_gap;
}

// A line sample and its label; returns the x after it.
double legend_line(Svg& svg, double x, double y, const char* stroke, double width, const std::string& dash,
                   const std::string& label) {
    const double k_sample = 24.0;
    const double k_sample_gap = 6.0;
    const double k_item_gap = 24.0;
    svg.line(x, y - k_text_rise, x + k_sample, y - k_text_rise, stroke, width, dash);
    svg.text(x + k_sample + k_sample_gap, y, label, font(k_note_size, k_ink));
    return x + k_sample + k_sample_gap + text_width(label, k_note_size) + k_item_gap;
}

void triangle_down(Svg& svg, double x, double y, double half, const char* fill) {
    std::vector<Point> points;
    const Point a = {x - half, y - half};
    const Point b = {x + half, y - half};
    const Point c = {x, y + half};
    points.push_back(a);
    points.push_back(b);
    points.push_back(c);
    svg.polygon(points, fill);
}

// A bracket under [x0, x1] at y (ticks up) with a bold name and a muted detail below it.
void bracket_below(Svg& svg, double x0, double x1, double y, const std::string& name, const std::string& detail) {
    const double k_bracket_tick = 4.0;
    const double k_bracket_inset = 1.5;
    const double k_name_drop = 15.0;
    const double k_detail_drop = 29.0;
    svg.line(x0 + k_bracket_inset, y, x1 - k_bracket_inset, y, k_axis, k_line);
    svg.line(x0 + k_bracket_inset, y, x0 + k_bracket_inset, y - k_bracket_tick, k_axis, k_line);
    svg.line(x1 - k_bracket_inset, y, x1 - k_bracket_inset, y - k_bracket_tick, k_axis, k_line);
    svg.text((x0 + x1) / 2.0, y + k_name_drop, name, font(k_note_size, k_ink, "middle", true));
    if (!detail.empty()) svg.text((x0 + x1) / 2.0, y + k_detail_drop, detail, font(k_small_size, k_muted, "middle"));
}

// ---------------------------------------------------------------------------
// Scales and axes
// ---------------------------------------------------------------------------

struct Scale {
    double from0;
    double from1;
    double to0;
    double to1;
    double operator()(double value) const { return to0 + (value - from0) * (to1 - to0) / (from1 - from0); }
};

struct LogScale {
    double from0;
    double from1;
    double to0;
    double to1;
    double operator()(double value) const {
        const double span = std::log10(from1) - std::log10(from0);
        return to0 + (std::log10(value) - std::log10(from0)) * (to1 - to0) / span;
    }
};

struct Box {
    double left;
    double top;
    double width;
    double height;
    double right() const { return left + width; }
    double bottom() const { return top + height; }
};

// Ticks first, first + step, ... last on a horizontal axis at y; labels every `label_every` ticks.
void x_axis(Svg& svg, const Scale& x, double y, double first, double last, double step, int decimals,
            size_t label_every = 1, double grid_top = 0.0) {
    svg.line(x.to0, y, x.to1, y, k_axis, k_line);
    const size_t count = static_cast<size_t>(std::lround((last - first) / step));
    for (size_t i = 0; i <= count; ++i) {
        const double value = first + i * step;
        const double px = x(value);
        if (grid_top > 0.0) svg.line(px, grid_top, px, y, k_grid_line, k_line);
        svg.line(px, y, px, y + k_tick_length, k_axis, k_line);
        if (i % label_every == 0)
            svg.text(px, y + k_tick_label_gap, number(value, decimals), font(k_tick_size, k_muted, "middle"));
    }
}

void y_axis(Svg& svg, const Scale& y, double x, double first, double last, double step, int decimals,
            double grid_right = 0.0) {
    svg.line(x, y.to0, x, y.to1, k_axis, k_line);
    const size_t count = static_cast<size_t>(std::lround((last - first) / step));
    for (size_t i = 0; i <= count; ++i) {
        const double value = first + i * step;
        const double py = y(value);
        if (grid_right > 0.0) svg.line(x, py, grid_right, py, k_grid_line, k_line);
        svg.line(x - k_tick_length, py, x, py, k_axis, k_line);
        svg.text(x - k_tick_length - k_tick_label_pad, py + k_text_rise, number(value, decimals),
                 font(k_tick_size, k_muted, "end"));
    }
}

// ---------------------------------------------------------------------------
// PNG (indexed colour, deflate with fixed Huffman codes and run-length matches) for spectrogram images
// ---------------------------------------------------------------------------

struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct ColorStop {
    double at;
    Rgb color;
};

// White (no energy) through light teal to dark teal (strongest).
const ColorStop k_heat[] = {{0.0, {255, 255, 255}},
                            {0.35, {225, 245, 238}},
                            {0.6, {93, 202, 165}},
                            {0.8, {29, 158, 117}},
                            {1.0, {4, 52, 44}}};
const size_t k_palette_size = 256;

Rgb heat_color(double t) {
    const size_t stops = sizeof(k_heat) / sizeof(k_heat[0]);
    t = std::max(0.0, std::min(1.0, t));
    for (size_t i = 1; i < stops; ++i) {
        if (t > k_heat[i].at) continue;
        const double f = (t - k_heat[i - 1].at) / (k_heat[i].at - k_heat[i - 1].at);
        const Rgb& a = k_heat[i - 1].color;
        const Rgb& b = k_heat[i].color;
        const Rgb c = {static_cast<uint8_t>(std::lround(a.r + f * (b.r - a.r))),
                       static_cast<uint8_t>(std::lround(a.g + f * (b.g - a.g))),
                       static_cast<uint8_t>(std::lround(a.b + f * (b.b - a.b)))};
        return c;
    }
    return k_heat[stops - 1].color;
}

std::string hex_color(const Rgb& c) {
    return format("#%02X%02X%02X", c.r, c.g, c.b);
}

const uint32_t k_crc32_polynomial = 0xEDB88320u;
const uint32_t k_adler_modulus = 65521u;
const unsigned k_half_word_bits = 16;
const uint32_t k_byte_mask = 0xFFu;

uint32_t crc32(const std::string& data, size_t first) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = first; i < data.size(); ++i) {
        crc ^= static_cast<uint8_t>(data[i]);
        for (unsigned b = 0; b < k_byte_bits; ++b) crc = (crc >> 1) ^ ((crc & 1u) != 0 ? k_crc32_polynomial : 0u);
    }
    return ~crc;
}

uint32_t adler32(const std::vector<uint8_t>& data) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        a = (a + data[i]) % k_adler_modulus;
        b = (b + a) % k_adler_modulus;
    }
    return (b << k_half_word_bits) | a;
}

void put_u32(std::string& out, uint32_t value) {
    for (int shift = 3 * k_byte_bits; shift >= 0; shift -= k_byte_bits)
        out += static_cast<char>((value >> shift) & k_byte_mask);
}

// Deflate bit stream: values LSB first, Huffman codes MSB first (RFC 1951 3.1.1).
class BitWriter {
public:
    void put(uint32_t value, unsigned bits) {
        for (unsigned i = 0; i < bits; ++i) put_bit((value >> i) & 1u);
    }

    void put_code(uint32_t code, unsigned bits) {
        for (unsigned i = bits; i-- > 0;) put_bit((code >> i) & 1u);
    }

    std::string finish() {
        if (fill_ != 0) bytes_ += static_cast<char>(current_);
        fill_ = 0;
        current_ = 0;
        return bytes_;
    }

private:
    void put_bit(uint32_t bit) {
        current_ = static_cast<uint8_t>(current_ | (bit << fill_));
        if (++fill_ < k_byte_bits) return;
        bytes_ += static_cast<char>(current_);
        fill_ = 0;
        current_ = 0;
    }

    std::string bytes_;
    uint8_t current_ = 0;
    unsigned fill_ = 0;
};

// Fixed literal/length code table (RFC 1951 3.2.6) and length symbols 257..285 (3.2.5).
struct FixedCode {
    unsigned first;
    unsigned last;
    uint32_t code;
    unsigned bits;
};

const FixedCode k_fixed_codes[] = {{0, 143, 0x30, 8}, {144, 255, 0x190, 9}, {256, 279, 0x00, 7}, {280, 287, 0xC0, 8}};
const uint16_t k_length_base[] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t k_length_extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const unsigned k_end_of_block = 256;
const unsigned k_first_length_symbol = 257;
const unsigned k_min_match = 3;
const unsigned k_max_match = 258;
const unsigned k_distance_bits = 5;  // fixed distance code 0 = distance 1
const unsigned k_fixed_block = 1;    // BTYPE 01
const unsigned k_block_type_bits = 2;

void put_symbol(BitWriter& out, unsigned symbol) {
    for (size_t i = 0; i < sizeof(k_fixed_codes) / sizeof(k_fixed_codes[0]); ++i) {
        const FixedCode& c = k_fixed_codes[i];
        if (symbol < c.first || symbol > c.last) continue;
        out.put_code(c.code + (symbol - c.first), c.bits);
        return;
    }
}

void put_run(BitWriter& out, unsigned length) {
    size_t index = sizeof(k_length_base) / sizeof(k_length_base[0]);
    while (k_length_base[index - 1] > length) --index;
    --index;
    put_symbol(out, k_first_length_symbol + static_cast<unsigned>(index));
    out.put(length - k_length_base[index], k_length_extra[index]);
    out.put_code(0, k_distance_bits);
}

// One fixed-Huffman block; runs of a repeated byte become matches at distance 1.
std::string deflate(const std::vector<uint8_t>& raw) {
    BitWriter out;
    out.put(1, 1);  // BFINAL
    out.put(k_fixed_block, k_block_type_bits);
    for (size_t i = 0; i < raw.size();) {
        put_symbol(out, raw[i]);
        size_t run = 0;
        while (i + 1 + run < raw.size() && raw[i + 1 + run] == raw[i]) ++run;
        size_t left = run;
        while (left >= k_min_match) {
            const unsigned length = static_cast<unsigned>(std::min<size_t>(left, k_max_match));
            put_run(out, length);
            left -= length;
        }
        for (; left > 0; --left) put_symbol(out, raw[i]);
        i += 1 + run;
    }
    put_symbol(out, k_end_of_block);
    return out.finish();
}

void put_chunk(std::string& png, const char* type, const std::string& data) {
    put_u32(png, static_cast<uint32_t>(data.size()));
    const size_t start = png.size();
    png += type;
    png += data;
    put_u32(png, crc32(png, start));
}

// 8-bit indexed PNG of `pixels` (row-major, top row first) with the heat palette.
std::string png_indexed(size_t width, size_t height, const std::vector<uint8_t>& pixels) {
    const unsigned char k_signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const uint8_t k_bit_depth = 8;
    const uint8_t k_indexed_colour = 3;
    const uint8_t k_zlib_cmf = 0x78;  // deflate, 32 KiB window
    const uint8_t k_zlib_flg = 0x01;  // (CMF * 256 + FLG) % 31 == 0, no dictionary
    std::string png(reinterpret_cast<const char*>(k_signature), sizeof(k_signature));
    std::string header;
    put_u32(header, static_cast<uint32_t>(width));
    put_u32(header, static_cast<uint32_t>(height));
    header += static_cast<char>(k_bit_depth);
    header += static_cast<char>(k_indexed_colour);
    header += std::string(3, '\0');  // compression, filter, interlace
    put_chunk(png, "IHDR", header);
    std::string palette;
    for (size_t i = 0; i < k_palette_size; ++i) {
        const Rgb c = heat_color(static_cast<double>(i) / (k_palette_size - 1));
        palette += static_cast<char>(c.r);
        palette += static_cast<char>(c.g);
        palette += static_cast<char>(c.b);
    }
    put_chunk(png, "PLTE", palette);
    std::vector<uint8_t> raw;
    raw.reserve((width + 1) * height);
    for (size_t row = 0; row < height; ++row) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), pixels.begin() + row * width, pixels.begin() + (row + 1) * width);
    }
    std::string stream;
    stream += static_cast<char>(k_zlib_cmf);
    stream += static_cast<char>(k_zlib_flg);
    stream += deflate(raw);
    put_u32(stream, adler32(raw));
    put_chunk(png, "IDAT", stream);
    put_chunk(png, "IEND", "");
    return png;
}

// ---------------------------------------------------------------------------
// Spectral analysis
// ---------------------------------------------------------------------------

typedef std::complex<double> Complex;

void fft(std::vector<Complex>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t length = 2; length <= n; length <<= 1) {
        const double angle = -2.0 * k_pi / static_cast<double>(length);
        const Complex step(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += length) {
            Complex w(1.0, 0.0);
            for (size_t j = 0; j < length / 2; ++j) {
                const Complex u = a[i + j];
                const Complex v = a[i + j + length / 2] * w;
                a[i + j] = u + v;
                a[i + j + length / 2] = u - v;
                w *= step;
            }
        }
    }
}

std::vector<double> hann(size_t n) {
    std::vector<double> w(n);
    for (size_t i = 0; i < n; ++i) w[i] = 0.5 - 0.5 * std::cos(2.0 * k_pi * static_cast<double>(i) / n);
    return w;
}

struct Spectrogram {
    size_t columns = 0;
    size_t rows = 0;
    double low_hz = 0.0;  // centre of row 0
    double bin_hz = 0.0;
    double column_s = 0.0;
    std::vector<double> power;  // [row * columns + column], row 0 = lowest frequency
};

// Column c analyses the window centred on sample (c + 1/2) hop, so the image spans columns * hop samples.
Spectrogram spectrogram(const std::vector<double>& x, double rate, size_t window, size_t hop, size_t fft_size,
                        double low_hz, double high_hz) {
    Spectrogram s;
    s.bin_hz = rate / static_cast<double>(fft_size);
    const size_t first_bin = static_cast<size_t>(std::ceil(low_hz / s.bin_hz));
    const size_t last_bin = static_cast<size_t>(std::floor(high_hz / s.bin_hz));
    s.rows = last_bin - first_bin + 1;
    s.low_hz = first_bin * s.bin_hz;
    s.columns = x.size() / hop;
    s.column_s = hop / rate;
    s.power.assign(s.rows * s.columns, 0.0);
    const std::vector<double> w = hann(window);
    std::vector<Complex> buffer(fft_size);
    for (size_t c = 0; c < s.columns; ++c) {
        const long start = static_cast<long>(c * hop + hop / 2) - static_cast<long>(window / 2);
        std::fill(buffer.begin(), buffer.end(), Complex(0.0, 0.0));
        for (size_t i = 0; i < window; ++i) {
            const long n = start + static_cast<long>(i);
            if (n >= 0 && n < static_cast<long>(x.size())) buffer[i] = x[static_cast<size_t>(n)] * w[i];
        }
        fft(buffer);
        for (size_t r = 0; r < s.rows; ++r) s.power[r * s.columns + c] = std::norm(buffer[first_bin + r]);
    }
    return s;
}

double max_power(const Spectrogram& s) {
    return *std::max_element(s.power.begin(), s.power.end());
}

// Palette indices, top row = highest frequency: 0 at floor_db (and below) .. 255 at 0 dB of `reference`.
std::string spectrogram_png(const Spectrogram& s, double reference, double floor_db) {
    std::vector<uint8_t> pixels(s.rows * s.columns);
    const double top = static_cast<double>(k_palette_size - 1);
    for (size_t r = 0; r < s.rows; ++r) {
        for (size_t c = 0; c < s.columns; ++c) {
            const double p = s.power[r * s.columns + c];
            const double db = p > 0.0 ? db_power(p / reference) : floor_db;
            const double t = std::max(0.0, std::min(1.0, (db - floor_db) / -floor_db));
            pixels[(s.rows - 1 - r) * s.columns + c] = static_cast<uint8_t>(std::lround(t * top));
        }
    }
    return png_indexed(s.columns, s.rows, pixels);
}

// Image of a spectrogram placed on time and frequency scales (rows are bin centres); t_offset_s shifts time.
void draw_spectrogram(Svg& svg, const Spectrogram& s, const Scale& t, const Scale& f, double reference,
                      double floor_db, const std::string& clip_id, double t_offset_s = 0.0) {
    const double top = f(s.low_hz + (s.rows - 0.5) * s.bin_hz);
    const double bottom = f(s.low_hz - 0.5 * s.bin_hz);
    svg.image(t(t_offset_s), top, t(t_offset_s + s.columns * s.column_s) - t(t_offset_s), bottom - top,
              spectrogram_png(s, reference, floor_db), clip_id);
}

// Colour bar with its end labels.
void color_bar(Svg& svg, double x, double y, double w, double h, double floor_db) {
    const size_t k_bar_steps = 48;
    const double k_step_overlap = 0.5;
    for (size_t i = 0; i < k_bar_steps; ++i) {
        const double t = (i + 0.5) / k_bar_steps;
        svg.rect(x + w * i / k_bar_steps, y, w / k_bar_steps + k_step_overlap, h, hex_color(heat_color(t)));
    }
    svg.rect(x, y, w, h, "none", k_axis, k_thin);
    svg.text(x, y + h + k_tick_label_gap, number(floor_db, 0) + " dB", font(k_small_size, k_muted, "start"));
    svg.text(x + w, y + h + k_tick_label_gap, "0 dB", font(k_small_size, k_muted, "end"));
}

// Welch power spectral density: Hann windows of fft_size, half overlap; fft_size / 2 + 1 bins.
std::vector<double> welch_psd(const std::vector<double>& x, size_t fft_size) {
    const std::vector<double> w = hann(fft_size);
    std::vector<double> psd(fft_size / 2 + 1, 0.0);
    std::vector<Complex> buffer(fft_size);
    size_t segments = 0;
    for (size_t start = 0; start + fft_size <= x.size(); start += fft_size / 2, ++segments) {
        for (size_t i = 0; i < fft_size; ++i) buffer[i] = x[start + i] * w[i];
        fft(buffer);
        for (size_t k = 0; k < psd.size(); ++k) psd[k] += std::norm(buffer[k]);
    }
    for (size_t k = 0; k < psd.size(); ++k) psd[k] /= static_cast<double>(std::max<size_t>(segments, 1));
    return psd;
}

// ---------------------------------------------------------------------------
// Transmissions: the Encoder's audio with its status per slot
// ---------------------------------------------------------------------------

struct SlotRun {
    EncoderStatus status;  // read before the slot's first sample
    size_t first;
    size_t count;
};

struct Rendered {
    EncoderConfig config;
    std::vector<uint8_t> data;
    std::vector<int16_t> audio;
    std::vector<SlotRun> slots;  // the tail is the last
};

Rendered render(const std::vector<uint8_t>& data, const EncoderConfig& config) {
    Rendered r;
    r.config = config;
    r.data = data;
    Encoder encoder(config);
    size_t written = encoder.write(data.data(), data.size());
    if (!encoder.start()) throw std::runtime_error("the encoder did not start");
    while (encoder.busy()) {
        if (written < data.size()) written += encoder.write(&data[written], data.size() - written);
        const EncoderStatus status = encoder.status();
        if (r.slots.empty() || r.slots.back().status.slot_index != status.slot_index ||
            r.slots.back().status.segment != status.segment) {
            const SlotRun run = {status, r.audio.size(), 0};
            r.slots.push_back(run);
        }
        r.audio.push_back(encoder.next_sample());
        ++r.slots.back().count;
    }
    return r;
}

std::vector<uint8_t> text_bytes(const char* text) {
    const std::string s = text;
    return std::vector<uint8_t>(s.begin(), s.end());
}

double slot_ms_of(const EncoderConfig& config) {
    return config.slot_us / k_us_per_ms;
}

// A sender at `speed` bytes/s; the AM passband (100..3000 Hz) holds every speed at 1500 Hz.
EncoderConfig speed_config(float speed, uint32_t rate_hz = k_rate_hz) {
    EncoderConfig config;
    config.sample_rate_hz = rate_hz;
    config.slot_us = unlimited::slot_us_for_speed(speed);
    config.passband.low_hz = unlimited::k_am_passband_low_hz;
    config.passband.high_hz = unlimited::k_am_passband_high_hz;
    return config;
}

DecoderConfig receiver_of(const EncoderConfig& config) {
    DecoderConfig receiver;
    receiver.slot_us = config.slot_us;
    receiver.passband = config.passband;
    return receiver;
}

const char* slot_color(const SlotRun& slot) {
    switch (slot.status.kind) {
        case SlotKind::start:
        case SlotKind::stop:
            return k_purple;
        case SlotKind::lead:
            return k_purple_light;
        case SlotKind::one:
            return k_teal;
        case SlotKind::zero:
        case SlotKind::silent:
            break;
    }
    return k_gray_fill;
}

// int16 -> sim::Channel -> int16, scaled so that key-down peaks times peak_factor plus 5 sigma of noise do not
// clip (as the long regression suite does).
std::vector<int16_t> through_channel(const std::vector<int16_t>& samples, sim::ChannelConfig config, int16_t amplitude,
                                     double peak_factor) {
    config.signal_level = amplitude / k_full_scale;
    double sigma = 0.0;
    if (config.noise) {
        const double tone_power = 0.5 * config.signal_level * config.signal_level;
        const double density = tone_power / (std::pow(10.0, config.snr_db / k_power_db) * k_reference_bandwidth_hz);
        sigma = std::sqrt(density * k_receiver_bandwidth_hz);
    }
    const double peak = peak_factor * config.signal_level + k_noise_peak_sigmas * sigma;
    config.output_gain = std::min(1.0, k_headroom / peak);
    std::vector<float> in(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) in[i] = static_cast<float>(samples[i] / k_full_scale);
    sim::Channel channel(config);
    const std::vector<float> out = channel.process(in);
    std::vector<int16_t> result(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        const double value = std::round(out[i] * k_full_scale);
        result[i] = static_cast<int16_t>(std::max(k_int16_min, std::min(k_int16_max, value)));
    }
    return result;
}

std::vector<double> to_double(const std::vector<int16_t>& samples, double scale) {
    std::vector<double> out(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) out[i] = samples[i] / scale;
    return out;
}

std::string path_of(const std::string& directory, const char* name) {
    return directory + "/" + name;
}

// Parallel map over [0, count): independent items, results in index order (deterministic).
template <typename Result, typename Work>
std::vector<Result> parallel_map(size_t count, Work work) {
    std::vector<Result> results(count);
    std::vector<std::exception_ptr> errors(count);
    std::atomic<size_t> next(0);
    const auto worker = [&]() {
        for (size_t i = next++; i < count; i = next++) {
            try {
                results[i] = work(i);
            } catch (...) {
                errors[i] = std::current_exception();
            }
        }
    };
    const unsigned cores = std::thread::hardware_concurrency();
    const size_t workers = std::min<size_t>(cores == 0 ? 1 : cores, count);
    std::vector<std::thread> threads;
    for (size_t t = 0; t < workers; ++t) threads.emplace_back(worker);
    for (size_t t = 0; t < threads.size(); ++t) threads[t].join();
    for (size_t i = 0; i < count; ++i) {
        if (errors[i]) std::rethrow_exception(errors[i]);
    }
    return results;
}

// A decode of a recording: every event, and the bytes per lock.
struct Decoded {
    std::vector<Event> events;
    size_t locks = 0;
    size_t ends = 0;
    size_t losts = 0;
    std::vector<int> bytes;  // the first lock's bytes at their byte_index; -1 where none came
};

Decoded decode(const std::vector<int16_t>& samples, const DecoderConfig& receiver, size_t expected) {
    lb::Capture capture = lb::run_decoder(samples, receiver, k_decoder_chunk);
    Decoded d;
    d.events = capture.events;
    d.bytes.assign(expected, -1);
    for (size_t i = 0; i < capture.events.size(); ++i) {
        const Event& e = capture.events[i];
        if (e.type == EventType::locked) ++d.locks;
        if (e.type == EventType::end) ++d.ends;
        if (e.type == EventType::lost) ++d.losts;
        if (e.type == EventType::byte && d.locks == 1 && e.byte_index < expected) d.bytes[e.byte_index] = e.value;
    }
    return d;
}

// ---------------------------------------------------------------------------
// Waveform drawing
// ---------------------------------------------------------------------------

// w(u): Tukey alpha 0.5, sin^2 ramps of T/4 (spec 1.1).
double tukey(double u) {
    const double ramp = unlimited::k_tukey_ramp;
    if (u < ramp) return square(std::sin(k_pi / 2.0 * u / ramp));
    if (u > 1.0 - ramp) return square(std::sin(k_pi / 2.0 * (1.0 - u) / ramp));
    return 1.0;
}

// Encoder samples [first, first + count) of `wave` against time in ms from `origin`.
void draw_wave(Svg& svg, const Scale& x, const Scale& y, const std::vector<double>& wave, size_t first, size_t count,
               size_t origin, double rate, const char* color, double width) {
    std::vector<Point> points;
    points.reserve(count);
    for (size_t i = first; i < first + count && i < wave.size(); ++i) {
        const Point p = {x((i - static_cast<double>(origin)) * k_ms_per_s / rate), y(wave[i])};
        points.push_back(p);
    }
    svg.polyline(points, color, width);
}

// +-|envelope(t_ms)| dashed over [t0, t1] ms.
void draw_envelope(Svg& svg, const Scale& x, const Scale& y, double t0, double t1,
                   const std::function<double(double)>& envelope) {
    const size_t k_envelope_points = 240;
    const double k_envelope_width = 1.2;
    std::vector<Point> upper;
    std::vector<Point> lower;
    for (size_t i = 0; i <= k_envelope_points; ++i) {
        const double t = t0 + (t1 - t0) * i / k_envelope_points;
        const double e = std::fabs(envelope(t));
        const Point a = {x(t), y(e)};
        const Point b = {x(t), y(-e)};
        upper.push_back(a);
        lower.push_back(b);
    }
    svg.polyline(upper, k_ink, k_envelope_width, k_dash);
    svg.polyline(lower, k_ink, k_envelope_width, k_dash);
}

// The peak |sample| per pixel column of [first, first + count): a transmission's envelope at any zoom.
void draw_peaks(Svg& svg, const Scale& x, const Scale& y, const std::vector<double>& wave, size_t first, size_t count,
                double rate, const char* color) {
    const double left = x(first * k_ms_per_s / rate);
    const double right = x((first + count) * k_ms_per_s / rate);
    const size_t columns = static_cast<size_t>(std::max(1.0, std::ceil(right - left)));
    for (size_t c = 0; c < columns; ++c) {
        const size_t a = first + c * count / columns;
        const size_t b = first + (c + 1) * count / columns;
        double peak = 0.0;
        for (size_t n = a; n < b && n < wave.size(); ++n) peak = std::max(peak, std::fabs(wave[n]));
        if (peak <= 0.0) continue;
        svg.line(left + c + 0.5, y(peak), left + c + 0.5, y(-peak), color, 1.0);
    }
}

// Slot cells in a row: one rounded cell per slot, coloured by its kind, the bit or marker letter inside.
void draw_slot_cells(Svg& svg, const Rendered& r, const Scale& x, double top, double height, bool letters) {
    const double k_cell_gap = 1.0;
    const double rate = r.config.sample_rate_hz;
    for (size_t s = 0; s < r.slots.size(); ++s) {
        const SlotRun& slot = r.slots[s];
        if (slot.status.segment == EncoderSegment::tail || slot.status.segment == EncoderSegment::lead_in) continue;
        const double x0 = x(slot.first * k_ms_per_s / rate);
        const double x1 = x((slot.first + slot.count) * k_ms_per_s / rate);
        svg.rect(x0 + k_cell_gap / 2.0, top, std::max(x1 - x0 - k_cell_gap, 1.0), height, slot_color(slot), k_axis,
                 k_hairline, k_cell_radius);
        if (!letters) continue;
        std::string letter;
        switch (slot.status.kind) {
            case SlotKind::start:
                letter = "S";
                break;
            case SlotKind::stop:
                letter = "E";
                break;
            case SlotKind::one:
                letter = "1";
                break;
            case SlotKind::zero:
                letter = "0";
                break;
            case SlotKind::lead:
                letter = "~";
                break;
            case SlotKind::silent:
                break;
        }
        const char* ink = slot.status.kind == SlotKind::zero || slot.status.kind == SlotKind::silent ? k_muted : k_card;
        if (!letter.empty())
            svg.text((x0 + x1) / 2.0, top + height / 2.0 + k_text_rise, letter, font(k_small_size, ink, "middle", true));
    }
}

// ---------------------------------------------------------------------------
// Figure: the beep
// ---------------------------------------------------------------------------

enum class Shade { none, ramps };

struct BeepPanel {
    const char* title;
    const char* note;
    size_t first_slot;  // in the rendered transmission
    size_t slots;
    const char* color;
    Shade shade;
    std::function<double(double)> envelope;  // of u, in slots from the panel's start
};

void figure_beep(const std::string& directory) {
    // 0xC0 = 1100 0000: START 1 1 0 0 0 0 0 0 STOP, then window 1's START.
    const EncoderConfig config = speed_config(6.0f, k_wave_rate_hz);
    const Rendered r = render(std::vector<uint8_t>{0xC0, 0x00}, config);
    const std::vector<double> wave = to_double(r.audio, config.amplitude);
    const double rate = config.sample_rate_hz;
    const double slot_ms = slot_ms_of(config);

    const double k_width = 820.0;
    const double k_height = 400.0;
    const double k_panel_left = 52.0;
    const double k_panel_width = 164.0;
    const double k_panel_gap = 30.0;
    const double k_panel_top = 118.0;
    const double k_panel_height = 150.0;
    const double k_amplitude_span = 1.15;
    const double k_tick_ms = 5.0;
    const size_t k_label_every = 2;
    const double k_wave_width = 0.6;
    Svg svg(k_width, k_height);
    heading(svg, format("One beep: real Encoder audio at 6 bytes/s (T %.3f ms, %u Hz)", slot_ms,
                        static_cast<unsigned>(config.tone_hz)),
            format("Rendered by the Encoder at %u Hz; amplitude relative to the crest A; dashed: the Tukey envelope "
                   "of spec 1.1.",
                   static_cast<unsigned>(rate)));

    const BeepPanel panels[] = {
        {"A 1: one beep", "Tukey α 0.5: ramps of T/4", 1, 1, k_teal, Shade::ramps, [](double u) { return tukey(u); }},
        {"A 0: silence", "nothing for the whole slot", 3, 1, k_gray, Shade::none, [](double) { return 0.0; }},
        {"Two 1s in a row", "each beep ends in its own slot", 1, 2, k_teal, Shade::none,
         [](double u) { return tukey(u - std::floor(u)); }},
        {"STOP, then the next START", "the same beeps: a window's edge", 9, 2, k_purple, Shade::none,
         [](double u) { return tukey(u - std::floor(u)); }}};
    const Scale y = {-k_amplitude_span, k_amplitude_span, k_panel_top + k_panel_height, k_panel_top};
    for (size_t p = 0; p < sizeof(panels) / sizeof(panels[0]); ++p) {
        const BeepPanel& panel = panels[p];
        const Box box = {k_panel_left + p * (k_panel_width + k_panel_gap), k_panel_top, k_panel_width, k_panel_height};
        const double span_ms = panel.slots * slot_ms;
        const Scale x = {0.0, span_ms, box.left, box.right()};
        const double ramp_ms = unlimited::k_tukey_ramp * slot_ms;
        if (panel.shade == Shade::ramps) {
            svg.rect(x(0.0), box.top, x(ramp_ms) - x(0.0), box.height, k_gray_fill);
            svg.rect(x(slot_ms - ramp_ms), box.top, x(slot_ms) - x(slot_ms - ramp_ms), box.height, k_gray_fill);
        }
        if (panel.slots > 1) svg.line(x(slot_ms), box.top, x(slot_ms), box.bottom(), k_axis, k_thin, k_guide_dash);
        svg.text(box.left, box.top - k_panel_title_rise, panel.title, font(k_label_size, k_ink, "start", true));
        svg.text(box.left, box.top - k_panel_note_rise, panel.note, font(k_note_size, k_muted));
        y_axis(svg, y, box.left, -1.0, 1.0, 1.0, 0, box.right());
        x_axis(svg, x, box.bottom(), 0.0, span_ms - std::fmod(span_ms, k_tick_ms), k_tick_ms, 0, k_label_every);
        svg.text(box.left + box.width / 2.0, box.bottom() + k_axis_title_gap, "time (ms)",
                 font(k_small_size, k_muted, "middle"));
        const SlotRun& first = r.slots[panel.first_slot];
        size_t count = 0;
        for (size_t s = panel.first_slot; s < panel.first_slot + panel.slots; ++s) count += r.slots[s].count;
        draw_wave(svg, x, y, wave, first.first, count, first.first, rate, panel.color, k_wave_width);
        draw_envelope(svg, x, y, 0.0, span_ms, [&](double t) { return panel.envelope(t / slot_ms); });
        if (panel.shade == Shade::none && panel.slots == 1) {
            svg.text(box.left + box.width / 2.0, y(0.0) - 2.0 * k_text_rise, "nothing is sent",
                     font(k_note_size, k_muted, "middle"));
        }
    }
    std::vector<std::string> lines;
    lines.push_back("A beep is the carrier times a Tukey window (α 0.5): cosine ramps over the first and last quarter "
                    "of the slot, flat between them.");
    lines.push_back(format("Every beep is 0 at its slot's edges, so consecutive tones still show where each slot "
                           "begins and ends; energy %.4g T·A²/2.",
                           unlimited::k_beep_energy));
    lines.push_back("START, STOP and every 1 are the same beep; the carrier runs on through silent slots (one "
                    "phase-continuous pitch, no reversal).");
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "beep.svg"), "Unlimited beep",
             "A data 1 (Tukey beep), a data 0 (silence), two 1s in a row and a STOP followed by the next START, "
             "rendered by the Encoder at 6 bytes/s, 48 kHz.");
    std::printf("beep.svg: 1, 0, two 1s and STOP/START at 6 bytes/s, %u Hz\n", static_cast<unsigned>(rate));
}

// ---------------------------------------------------------------------------
// Figure: the byte window
// ---------------------------------------------------------------------------

void figure_byte_window(const std::string& directory) {
    const uint8_t byte = 'H';
    const EncoderConfig config = speed_config(6.0f, k_wave_rate_hz);
    const Rendered r = render(std::vector<uint8_t>(1, byte), config);
    const std::vector<double> wave = to_double(r.audio, config.amplitude);
    const double rate = config.sample_rate_hz;
    const double slot_ms = slot_ms_of(config);
    const double window_ms = unlimited::k_window_slots * slot_ms;

    const double k_width = 820.0;
    const double k_height = 400.0;
    const Box plot = {60.0, 100.0, 720.0, 120.0};
    const double k_cell_top = 236.0;
    const double k_cell_height = 22.0;
    const double k_index_drop = 16.0;
    const double k_bracket_y = 296.0;
    const double k_amplitude_span = 1.15;
    const double k_tick_ms = 20.0;
    Svg svg(k_width, k_height);
    heading(svg, format("One byte, one window: 0x%02X '%c' = %s at 6 bytes/s", byte, byte, binary(byte, 8).c_str()),
            format("10 slots of %.3f ms: START, the 8 bits (most significant first; a beep is 1, silence is 0), STOP. "
                   "Encoder audio at %u Hz.",
                   slot_ms, static_cast<unsigned>(rate)));
    const Scale x = {0.0, window_ms, plot.left, plot.right()};
    const Scale y = {-k_amplitude_span, k_amplitude_span, plot.bottom(), plot.top};
    for (unsigned s = 1; s < unlimited::k_window_slots; ++s)
        svg.line(x(s * slot_ms), plot.top, x(s * slot_ms), k_cell_top + k_cell_height, k_grid_line, k_line);
    y_axis(svg, y, plot.left, -1.0, 1.0, 1.0, 0);
    draw_peaks(svg, x, y, wave, 0, r.slots.back().first, rate, k_teal);
    draw_slot_cells(svg, r, x, k_cell_top, k_cell_height, true);
    for (unsigned s = 0; s < unlimited::k_window_slots; ++s) {
        const double centre = x((s + 0.5) * slot_ms);
        std::string label = format("slot %u", s);
        svg.text(centre, k_cell_top + k_cell_height + k_index_drop, label, font(k_small_size, k_muted, "middle"));
        if (s >= unlimited::k_first_data_slot && s < unlimited::k_stop_slot)
            svg.text(centre, k_cell_top + k_cell_height + 2.0 * k_index_drop, format("bit %u", 8 - s),
                     font(k_small_size, k_muted, "middle"));
    }
    bracket_below(svg, x(0.0), x(slot_ms), k_bracket_y, "START", "a beep: the level of a 1 now");
    bracket_below(svg, x(slot_ms), x(9.0 * slot_ms), k_bracket_y, format("the 8 bits of 0x%02X, MSB first", byte),
                  binary(byte, 8));
    bracket_below(svg, x(9.0 * slot_ms), x(window_ms), k_bracket_y, "STOP", "a beep: the level again");
    x_axis(svg, x, plot.top - 6.0, 0.0, window_ms - std::fmod(window_ms, k_tick_ms), k_tick_ms, 0);
    std::vector<std::string> lines;
    lines.push_back("Like a serial port's 8N1 frame: a start bit, 8 data bits, a stop bit. START and STOP are always "
                    "tones: they tell the receiver how loud a 1 is.");
    lines.push_back("The receiver decides each data slot against 70 % of the line from the START's level to the "
                    "STOP's; a window without its START or STOP is dropped.");
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "byte_window.svg"), "Unlimited byte window",
             "The window of the byte 0x48 'H': a START beep, the 8 data slots 01001000 and a STOP beep, rendered by "
             "the Encoder at 6 bytes/s.");
    std::printf("byte_window.svg: 0x48 at 6 bytes/s, %u Hz\n", static_cast<unsigned>(rate));
}

// ---------------------------------------------------------------------------
// Figure: "Hi", a whole transmission
// ---------------------------------------------------------------------------

struct TimelineRow {
    const char* title;
    Rendered r;
};

void figure_hi_transmission(const std::string& directory) {
    EncoderConfig plain = speed_config(6.0f, k_wave_rate_hz);
    EncoderConfig vox = plain;
    vox.vox_lead_ms = unlimited::k_default_vox_lead_ms;
    const std::vector<uint8_t> hi = text_bytes(k_hi_text);
    std::vector<TimelineRow> rows;
    rows.push_back(TimelineRow{"\"Hi\" at 6 bytes/s: two windows, then the tail", render(hi, plain)});
    rows.push_back(TimelineRow{"The same with a 150 ms VOX lead: a steady tone, 2 silent slots, the windows",
                               render(hi, vox)});
    const double rate = plain.sample_rate_hz;
    const double slot_ms = slot_ms_of(plain);
    double longest_ms = 0.0;
    for (size_t i = 0; i < rows.size(); ++i) longest_ms = std::max(longest_ms, rows[i].r.audio.size() * k_ms_per_s / rate);

    const double k_width = 820.0;
    const double k_height = 520.0;
    const double k_left = 40.0;
    const double k_right = 790.0;
    const double k_row_top = 110.0;
    const double k_row_pitch = 190.0;
    const double k_wave_height = 60.0;
    const double k_cell_gap = 8.0;
    const double k_cell_height = 18.0;
    const double k_bracket_gap = 16.0;
    const double k_tick_ms = 50.0;
    Svg svg(k_width, k_height);
    heading(svg, "A transmission: \"Hi\" = 0x48 0x69 (spec 2.2)",
            format("Encoder audio at %u Hz, 6 bytes/s (T %.3f ms, 1500 Hz), no lead-in, the 100 ms tail. Purple: "
                   "START and STOP; teal: 1; gray: 0.",
                   static_cast<unsigned>(rate), slot_ms));
    const Scale x = {0.0, longest_ms, k_left, k_right};
    for (size_t i = 0; i < rows.size(); ++i) {
        const Rendered& r = rows[i].r;
        const double top = k_row_top + i * k_row_pitch;
        const std::vector<double> wave = to_double(r.audio, r.config.amplitude);
        const Scale y = {-1.1, 1.1, top + k_wave_height, top};
        svg.text(k_left, top - k_panel_note_rise, rows[i].title, font(k_label_size, k_ink, "start", true));
        draw_peaks(svg, x, y, wave, 0, r.audio.size(), rate, k_teal_dark);
        const double cells = top + k_wave_height + k_cell_gap;
        draw_slot_cells(svg, r, x, cells, k_cell_height, true);
        // Brackets: the lead, the gap, each window with its byte, the tail.
        size_t s = 0;
        while (s < r.slots.size()) {
            const EncoderStatus& st = r.slots[s].status;
            size_t e = s;
            while (e + 1 < r.slots.size() && r.slots[e + 1].status.segment == st.segment &&
                   (st.segment != EncoderSegment::window || r.slots[e + 1].status.byte_index == st.byte_index))
                ++e;
            const double x0 = x(r.slots[s].first * k_ms_per_s / rate);
            const double x1 = x((r.slots[e].first + r.slots[e].count) * k_ms_per_s / rate);
            const double by = cells + k_cell_height + k_bracket_gap;
            switch (st.segment) {
                case EncoderSegment::window:
                    bracket_below(svg, x0, x1, by, format("window %u", static_cast<unsigned>(st.byte_index)),
                                  format("0x%02X '%c' %s", st.byte, st.byte, binary(st.byte, 8).c_str()));
                    break;
                case EncoderSegment::vox_lead:
                    bracket_below(svg, x0, x1, by, "VOX lead",
                                  format("%u slots", static_cast<unsigned>(e - s + 1)));
                    break;
                case EncoderSegment::gap:
                    bracket_below(svg, x0, x1, by, "gap", "2 slots");
                    break;
                case EncoderSegment::tail:
                    bracket_below(svg, x0, x1, by, "tail", format("%.0f ms", r.slots[e].count * k_ms_per_s / rate));
                    break;
                case EncoderSegment::lead_in:
                case EncoderSegment::idle:
                    break;
            }
            s = e + 1;
        }
        x_axis(svg, x, top - 4.0, 0.0, longest_ms - std::fmod(longest_ms, k_tick_ms), k_tick_ms, 0, 2);
    }
    std::vector<std::string> lines;
    lines.push_back("No tune, no sync train, no END: the first tone after silence is the START of byte 0, and a "
                    "silent window after the last STOP is the end.");
    lines.push_back(format("20 slots = %.0f ms of signal for 2 bytes; the tail is max(100 ms, 2 slots) of silence. "
                           "The VOX lead only keys a VOX radio.",
                           20.0 * slot_ms));
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "hi_transmission.svg"), "Unlimited transmission",
             "\"Hi\" at 6 bytes/s: window 0 (0x48) and window 1 (0x69), then the tail; below, the same after a 150 ms "
             "VOX lead and its 2-slot gap. Rendered by the Encoder.");
    std::printf("hi_transmission.svg: \"Hi\" at 6 bytes/s, with and without a VOX lead\n");
}

// ---------------------------------------------------------------------------
// Figure: what the receiver sees, window by window
// ---------------------------------------------------------------------------

// One decided window from the decoder's own slot and byte events (spec 3.5).
struct WindowBars {
    uint32_t index = 0;
    uint8_t start_pct = 0;
    uint8_t stop_pct = 0;
    uint8_t level_pct[k_byte_bits] = {};
    uint8_t threshold_pct[k_byte_bits] = {};
    uint8_t bits[k_byte_bits] = {};
    bool complete = false;
    bool dropped = false;
    uint8_t value = 0;
};

// The windows of the first lock, in order.
std::vector<WindowBars> window_bars(const Decoded& d) {
    std::vector<WindowBars> windows;
    size_t locks = 0;
    for (size_t i = 0; i < d.events.size(); ++i) {
        const Event& e = d.events[i];
        if (e.type == EventType::locked) ++locks;
        if (locks != 1 || (e.type != EventType::slot && e.type != EventType::byte)) continue;
        if (windows.empty() || windows.back().index != e.byte_index) {
            WindowBars w;
            w.index = e.byte_index;
            windows.push_back(w);
        }
        WindowBars& w = windows.back();
        w.start_pct = e.start_pct;
        w.stop_pct = e.stop_pct;
        if (e.type == EventType::slot && e.slot >= unlimited::k_first_data_slot && e.slot < unlimited::k_stop_slot) {
            const size_t b = e.slot - unlimited::k_first_data_slot;
            w.level_pct[b] = e.level_pct;
            w.threshold_pct[b] = e.threshold_pct;
            w.bits[b] = e.value;
            if ((e.flags & unlimited::event_flag_framing) != 0) w.dropped = true;
        }
        if (e.type == EventType::byte) {
            w.complete = true;
            w.value = e.value;
        }
    }
    return windows;
}

void figure_receiver_windows(const std::string& directory) {
    const std::vector<uint8_t> data = text_bytes("Hi!~");
    const float speed = 6.0f;
    const EncoderConfig config = speed_config(speed);
    const lb::Recording recording = lb::single(data, config, k_silence_ms);
    struct Row {
        double snr_db;
        const char* label;
    };
    const Row rows[] = {{20.0, "20 dB: clean"}, {k_gate_db[2], "the provisional gate (1.3 dB) at 6 bytes/s"}};

    const double k_width = 820.0;
    const double k_height = 560.0;
    const double k_left = 60.0;
    const double k_row_top = 112.0;
    const double k_row_pitch = 205.0;
    const double k_plot_height = 120.0;
    const double k_tile_width = 172.0;
    const double k_tile_gap = 12.0;
    const double k_top_pct = 150.0;
    const double k_bar_fraction = 0.64;
    const double k_bits_drop = 16.0;
    const double k_byte_drop = 32.0;
    Svg svg(k_width, k_height);
    heading(svg, "What the receiver sees: each window's reference and decision lines (spec 3.5)",
            "\"Hi!~\" at 6 bytes/s through a USB channel: the decoder's own slot events. Bars in % of the running "
            "reference: purple START/STOP, teal a 1, gray a 0.",
            "Dashed: the reference line from the START's level to the STOP's; amber: the decision line, 70 % of it.");
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); ++r) {
        sim::ChannelConfig channel;
        channel.mode = sim::Mode::usb;
        channel.snr_db = rows[r].snr_db;
        channel.seed = k_seed + static_cast<uint32_t>(r);
        channel.freq_offset_hz = k_figure_offset_hz;
        const std::vector<int16_t> samples = through_channel(recording.samples, channel, config.amplitude, 1.0);
        const Decoded d = decode(samples, receiver_of(config), data.size());
        const std::vector<WindowBars> windows = window_bars(d);
        double snr = 0.0;
        for (size_t i = 0; i < d.events.size(); ++i)
            if (d.events[i].type == EventType::locked) snr = d.events[i].snr_db;
        const double top = k_row_top + r * k_row_pitch;
        svg.text(k_left, top - k_panel_title_rise, rows[r].label, font(k_label_size, k_ink, "start", true));
        svg.text(k_left, top - k_panel_note_rise,
                 format("SNR set %s dB (key-down tone in 2500 Hz); measured by the receiver at the lock: %s dB",
                        number(rows[r].snr_db, 1).c_str(), number(snr, 1).c_str()),
                 font(k_note_size, k_muted));
        const Scale y = {0.0, k_top_pct, top + k_plot_height, top};
        y_axis(svg, y, k_left - 6.0, 0.0, k_top_pct, 50.0, 0);
        for (size_t w = 0; w < windows.size() && w < data.size(); ++w) {
            const WindowBars& bars = windows[w];
            const double left = k_left + w * (k_tile_width + k_tile_gap);
            const double cell = k_tile_width / unlimited::k_window_slots;
            const double bar = cell * k_bar_fraction;
            svg.rect(left, top, k_tile_width, k_plot_height, k_row_band, "none", 0.0, k_bar_radius);
            const double start_level = bars.start_pct;
            const double stop_level = bars.stop_pct;
            const auto reference_at = [&](double slot) {
                return start_level + (stop_level - start_level) * slot / unlimited::k_stop_slot;
            };
            // The reference line from the START's centre to the STOP's, the decision line under it.
            svg.line(left + cell / 2.0, y(start_level), left + (unlimited::k_stop_slot + 0.5) * cell, y(stop_level),
                     k_muted, k_line, k_dash);
            for (unsigned s = 0; s < unlimited::k_window_slots; ++s) {
                const double centre = left + (s + 0.5) * cell;
                double level = 0.0;
                const char* color = k_purple;
                if (s == unlimited::k_start_slot) {
                    level = start_level;
                } else if (s == unlimited::k_stop_slot) {
                    level = stop_level;
                } else {
                    const size_t b = s - unlimited::k_first_data_slot;
                    level = bars.level_pct[b] * reference_at(s) / k_percent;
                    color = bars.bits[b] != 0 ? k_teal : k_gray_mid;
                    const double line = bars.threshold_pct[b] * reference_at(s) / k_percent;
                    svg.line(centre - cell / 2.0, y(line), centre + cell / 2.0, y(line), k_amber, k_medium);
                    svg.text(centre, top + k_plot_height + k_bits_drop, format("%u", bars.bits[b]),
                             font(k_small_size, bars.bits[b] != 0 ? k_teal_dark : k_muted, "middle", true));
                }
                const double height = y(0.0) - y(std::min(level, k_top_pct));
                svg.rect(centre - bar / 2.0, y(0.0) - height, bar, height, color, "none", 0.0, k_bar_radius);
            }
            const std::string label =
                bars.dropped ? std::string("dropped (framing)")
                             : (bars.complete ? format("0x%02X %s", bars.value, char_of(bars.value).c_str())
                                              : std::string("…"));
            const bool right = bars.complete && bars.value == data[w];
            svg.text(left + k_tile_width / 2.0, top + k_plot_height + k_byte_drop, label,
                     font(k_note_size, bars.dropped || !right ? k_coral : k_ink, "middle", true));
        }
    }
    std::vector<std::string> lines;
    lines.push_back("Each window carries its own reference: the START and STOP levels, so a fade across the window "
                    "tilts the line and the 70 % line tilts with it.");
    lines.push_back("Near the gate a 0 may reach up towards the line and a 1 dip towards it: the decoder flags a bit "
                    "within 12.5 % of its line as weak.");
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "receiver_windows.svg"), "Unlimited receiver windows",
             "The decoder's slot events for \"Hi!~\" at 6 bytes/s at 20 dB and at the gate: START and STOP bars, the "
             "reference line between them, the 70 % decision line and the data bars.");
    std::printf("receiver_windows.svg: \"Hi!~\" at 6 bytes/s, 20 dB and %.1f dB\n", k_gate_db[2]);
}

// ---------------------------------------------------------------------------
// Figure: spectra per speed against SSB filters
// ---------------------------------------------------------------------------

void figure_speeds_spectrum(const std::string& directory) {
    const char* const colors[] = {k_blue, k_teal, k_amber, k_pink, k_purple};
    const size_t k_bytes = 64;
    const size_t k_fft = 8192;
    const double k_low_hz = 200.0;
    const double k_high_hz = 2800.0;
    const double k_floor_db = -70.0;
    const double k_width = 820.0;
    const double k_height = 520.0;
    const Box plot = {70.0, 124.0, 700.0, 280.0};
    const double k_band_row = 14.0;
    const double k_band_gap = 6.0;
    Svg svg(k_width, k_height);
    heading(svg, "Spectrum per speed against SSB filters (spec 1.3)",
            format("Welch power spectrum of %u random bytes from the Encoder at 8000 Hz, 1500 Hz pitch; each curve to "
                   "its own peak. Bars: the occupied band (99 %% of the power).",
                   static_cast<unsigned>(k_bytes)));
    const Scale x = {k_low_hz, k_high_hz, plot.left, plot.right()};
    const Scale y = {k_floor_db, 0.0, plot.bottom(), plot.top};
    // The 2.4 kHz SSB filter (300-2700 Hz) shaded; the 1.8 kHz filter's top edge (2100 Hz) dashed.
    svg.rect(x(unlimited::k_ssb_passband_low_hz), plot.top, x(unlimited::k_ssb_passband_high_hz) -
             x(unlimited::k_ssb_passband_low_hz), plot.height, k_row_band);
    svg.line(x(k_narrow_filter_high_hz), plot.top, x(k_narrow_filter_high_hz), plot.bottom(), k_axis, k_line, k_dash);
    svg.text(x(k_narrow_filter_high_hz) + k_text_rise, plot.top + k_tick_label_gap, "1.8 kHz filter ends (2100 Hz)",
             font(k_small_size, k_muted));
    svg.text(x(unlimited::k_ssb_passband_high_hz) - k_text_rise, plot.bottom() - k_text_rise,
             "2.4 kHz filter: 300–2700 Hz", font(k_small_size, k_muted, "end"));
    x_axis(svg, x, plot.bottom(), k_low_hz, k_high_hz, 200.0, 0, 2, plot.top);
    y_axis(svg, y, plot.left, k_floor_db, 0.0, 10.0, 0, plot.right());
    svg.text(plot.left + plot.width / 2.0, plot.bottom() + k_axis_title_gap, "frequency (Hz)",
             font(k_small_size, k_muted, "middle"));
    svg.text(plot.left - 44.0, plot.top + plot.height / 2.0, "dB", font(k_small_size, k_muted, "middle"), -90.0);
    for (size_t v = 0; v < sizeof(k_speeds) / sizeof(k_speeds[0]); ++v) {
        const EncoderConfig config = speed_config(k_speeds[v]);
        const Rendered r = render(lb::random_bytes(k_bytes, k_seed + static_cast<uint32_t>(v)), config);
        std::vector<double> x_samples = to_double(r.audio, config.amplitude);
        x_samples.resize(r.slots.back().first);  // the windows, not the tail
        const std::vector<double> psd = welch_psd(x_samples, k_fft);
        double peak = 0.0;
        for (size_t k = 0; k < psd.size(); ++k) peak = std::max(peak, psd[k]);
        std::vector<Point> points;
        const double bin_hz = static_cast<double>(k_rate_hz) / k_fft;
        for (size_t k = 0; k < psd.size(); ++k) {
            const double hz = k * bin_hz;
            if (hz < k_low_hz || hz > k_high_hz) continue;
            const double db = std::max(k_floor_db, db_power(std::max(psd[k], 1e-30) / peak));
            const Point p = {x(hz), y(db)};
            points.push_back(p);
        }
        svg.polyline(points, colors[v], k_spectrum_trace);
        const Band band = unlimited::occupied_band(config);
        const double bar_y = plot.bottom() + 44.0 + v * (k_band_row + k_band_gap);
        svg.rect(x(band.low_hz), bar_y, x(band.high_hz) - x(band.low_hz), k_band_row, colors[v], "none", 0.0,
                 k_bar_radius, 0.8);
        svg.text(x(band.high_hz) + k_text_rise * 2.0, bar_y + k_band_row - 3.0,
                 format("%s bytes/s: %u Hz, %u–%u Hz", number(k_speeds[v], 0).c_str(),
                        static_cast<unsigned>(band.width_hz), static_cast<unsigned>(band.low_hz),
                        static_cast<unsigned>(band.high_hz)),
                 font(k_small_size, k_ink));
    }
    svg.save(path_of(directory, "speeds_spectrum.svg"), "Unlimited spectrum per speed",
             "The power spectrum of random data at 1, 3, 6, 12 and 25 bytes per second with their occupied bands, "
             "against the 2.4 kHz and 1.8 kHz SSB filters.");
    std::printf("speeds_spectrum.svg: 5 speeds, %u bytes each\n", static_cast<unsigned>(k_bytes));
}

// ---------------------------------------------------------------------------
// Figure: through radio channels
// ---------------------------------------------------------------------------

struct ChannelCase {
    const char* name;
    const char* detail;
    float speed;
    sim::ChannelConfig channel;
    double peak_factor;
    double silence_ms;  // before the transmission (a carrier is there long before it)
};

std::vector<ChannelCase> channel_cases() {
    std::vector<ChannelCase> cases;
    const auto usb = [](double snr) {
        sim::ChannelConfig c;
        c.mode = sim::Mode::usb;
        c.snr_db = snr;
        c.freq_offset_hz = k_figure_offset_hz;
        return c;
    };
    sim::ChannelConfig clean;
    clean.mode = sim::Mode::clean;
    clean.noise = false;
    cases.push_back(ChannelCase{"clean", "no noise", 6.0f, clean, 1.0, k_silence_ms});
    cases.push_back(ChannelCase{"AWGN", "USB, gate + 3 dB (4.3 dB)", 6.0f, usb(k_gate_db[2] + 3.0), 1.0, k_silence_ms});
    cases.push_back(ChannelCase{"AWGN", "USB, the gate (1.3 dB)", 6.0f, usb(k_gate_db[2]), 1.0, k_silence_ms});
    sim::ChannelConfig good = usb(10.0);
    sim::apply_preset(good, sim::FadingPreset::ccir_good);
    cases.push_back(ChannelCase{"CCIR good", "USB 10 dB, 0.1 Hz Doppler", 6.0f, good, k_fading_peak, k_silence_ms});
    sim::ChannelConfig moderate = usb(15.0);
    sim::apply_preset(moderate, sim::FadingPreset::ccir_moderate);
    cases.push_back(ChannelCase{"CCIR moderate", "USB 15 dB, 0.5 Hz", 6.0f, moderate, k_fading_peak, k_silence_ms});
    sim::ChannelConfig poor = usb(20.0);
    sim::apply_preset(poor, sim::FadingPreset::ccir_poor);
    cases.push_back(ChannelCase{"CCIR poor", "USB 20 dB, 1 Hz, 3 bytes/s", 3.0f, poor, k_fading_peak, k_silence_ms});
    sim::ChannelConfig qrn = usb(10.0);
    qrn.impulse_rate_hz = k_figure_impulse_rate_hz;
    cases.push_back(ChannelCase{"QRN", "USB 10 dB, 5 crashes/s, blanker on", 6.0f, qrn, 1.0, k_silence_ms});
    sim::ChannelConfig carrier = usb(10.0);
    carrier.carrier_hz = k_figure_carrier_hz;
    carrier.carrier_db = k_figure_interferer_db;
    carrier.agc = true;
    cases.push_back(ChannelCase{"carrier + AGC", "USB 10 dB, a carrier at 1200 Hz, −6 dB", 6.0f, carrier, 1.0,
                                k_carrier_silence_ms});
    sim::ChannelConfig am;
    am.mode = sim::Mode::am;
    am.snr_db = 12.0;
    am.rx_low_hz = unlimited::k_am_passband_low_hz;
    am.rx_high_hz = unlimited::k_am_passband_high_hz;
    cases.push_back(ChannelCase{"AM", "12 dB carrier, 12 bytes/s", 12.0f, am, k_fm_peak, k_silence_ms});
    sim::ChannelConfig fm;
    fm.mode = sim::Mode::fm;
    fm.snr_db = 22.0;
    fm.rx_high_hz = unlimited::k_fm_passband_high_hz;
    fm.fm_audio_high_hz = unlimited::k_fm_passband_high_hz;
    cases.push_back(ChannelCase{"FM", "22 dB, 25 bytes/s", 25.0f, fm, k_fm_peak, k_silence_ms});
    return cases;
}

void figure_channels(const std::string& directory) {
    const std::vector<uint8_t> data = text_bytes(k_channel_text);
    const std::vector<ChannelCase> cases = channel_cases();
    const std::vector<Decoded> results = parallel_map<Decoded>(cases.size(), [&](size_t i) {
        EncoderConfig config = speed_config(cases[i].speed);
        const lb::Recording recording = lb::single(data, config, cases[i].silence_ms);
        sim::ChannelConfig channel = cases[i].channel;
        channel.seed = k_seed + static_cast<uint32_t>(i);
        const std::vector<int16_t> samples =
            through_channel(recording.samples, channel, config.amplitude, cases[i].peak_factor);
        return decode(samples, receiver_of(config), data.size());
    });
    const double k_width = 820.0;
    const double k_row_top = 96.0;
    const double k_row_pitch = 34.0;
    const double k_name_x = 24.0;
    const double k_cells_x = 250.0;
    const double k_cell = 22.0;
    const double k_cell_height = 22.0;
    const double k_result_x = k_cells_x + k_cell * 18.0 + 12.0;
    const double height = k_row_top + cases.size() * k_row_pitch + 70.0;
    Svg svg(k_width, height);
    heading(svg, format("Through radio channels: \"%s\" (the channel simulator, spec 6)", k_channel_text),
            "Encoder → sim::Channel → Decoder, told the speed; each cell one byte as released: ink right, coral "
            "wrong, gray not released.");
    for (size_t i = 0; i < cases.size(); ++i) {
        const Decoded& d = results[i];
        const double top = k_row_top + i * k_row_pitch;
        if (i % 2 == 0) svg.rect(k_margin / 2.0, top - 4.0, k_width - k_margin, k_row_pitch - 2.0, k_row_band);
        svg.text(k_name_x, top + k_cell_height / 2.0, cases[i].name, font(k_label_size, k_ink, "start", true));
        svg.text(k_name_x, top + k_cell_height / 2.0 + 12.0, cases[i].detail, font(k_small_size, k_muted));
        size_t right = 0;
        for (size_t b = 0; b < data.size(); ++b) {
            const int value = d.bytes[b];
            const double x0 = k_cells_x + b * k_cell;
            const char* fill = k_card;
            const char* ink = k_ink;
            std::string text = format("%c", data[b]);
            if (value < 0) {
                fill = k_gray_fill;
                ink = k_muted;
                text = "·";
            } else if (value != data[b]) {
                fill = k_coral;
                ink = k_card;
                text = value >= k_first_printable && value <= k_last_printable ? format("%c", value) : "?";
            } else {
                ++right;
            }
            svg.rect(x0 + 1.0, top, k_cell - 2.0, k_cell_height, fill, k_axis, k_hairline, k_cell_radius);
            svg.text(x0 + k_cell / 2.0, top + k_cell_height / 2.0 + k_text_rise, text,
                     font(k_note_size, ink, "middle", true));
        }
        svg.text(k_result_x, top + k_cell_height / 2.0 + k_text_rise,
                 format("%u/%u right, %u lock%s%s", static_cast<unsigned>(right), static_cast<unsigned>(data.size()),
                        static_cast<unsigned>(d.locks), d.locks == 1 ? "" : "s", d.losts > 0 ? ", lost" : ""),
                 font(k_note_size, right == data.size() ? k_teal_dark : k_coral));
    }
    std::vector<std::string> lines;
    lines.push_back("One run each, fixed seeds: a picture of the conditions, not a measurement; the long suite "
                    "(make test_long) measures them over many transmissions.");
    lines.push_back("A window whose START or STOP faded is dropped, never delivered wrong; a deep fade across a "
                    "START ends the transmission.");
    notes(svg, height, lines);
    svg.save(path_of(directory, "channels.svg"), "Unlimited through channels",
             "One transmission of a 18-byte text per channel condition (AWGN, CCIR fading, QRN, a carrier with AGC, "
             "AM and FM) through the channel simulator and the real decoder.");
    std::printf("channels.svg: %u channel cases\n", static_cast<unsigned>(cases.size()));
}

// ---------------------------------------------------------------------------
// Figure: a transmission in noise, time against frequency
// ---------------------------------------------------------------------------

void figure_spectrogram(const std::string& directory) {
    const std::vector<uint8_t> data = text_bytes(k_channel_text);
    const EncoderConfig config = speed_config(6.0f);
    const lb::Recording recording = lb::single(data, config, k_silence_ms);
    sim::ChannelConfig channel;
    channel.mode = sim::Mode::usb;
    channel.snr_db = k_spectrogram_snr_db;
    channel.seed = k_seed;
    channel.freq_offset_hz = k_figure_offset_hz;
    const std::vector<int16_t> samples = through_channel(recording.samples, channel, config.amplitude, 1.0);
    const double rate = k_rate_hz;
    const double start_s = lb::first_start_sample(recording.transmissions[0]) / rate - k_spectrogram_margin_s;
    const double end_s = lb::end_sample(recording.transmissions[0]) / rate + k_spectrogram_margin_s;
    const size_t first = static_cast<size_t>(start_s * rate);
    const size_t last = std::min(samples.size(), static_cast<size_t>(end_s * rate));
    const std::vector<int16_t> part(samples.begin() + static_cast<std::ptrdiff_t>(first),
                                    samples.begin() + static_cast<std::ptrdiff_t>(last));
    const size_t k_window = 128;  // 16 ms, about one slot at 6 bytes/s
    const size_t k_hop = 32;
    const size_t k_fft = 512;
    const double k_low_hz = 300.0;
    const double k_high_hz = 2700.0;
    const double k_floor_db = -45.0;
    const Spectrogram s = spectrogram(to_double(part, k_full_scale), rate, k_window, k_hop, k_fft, k_low_hz, k_high_hz);
    const double k_width = 820.0;
    const double k_height = 470.0;
    const Box plot = {70.0, 100.0, 700.0, 240.0};
    Svg svg(k_width, k_height);
    heading(svg, format("\"%s\" at 6 bytes/s in noise: time against frequency", k_channel_text),
            format("USB channel at %s dB (key-down tone in 2500 Hz), mistuned %s Hz; 16 ms Hann windows every 4 ms. "
                   "Brackets: the windows, one per byte.",
                   number(k_spectrogram_snr_db, 0).c_str(), signed_number(k_figure_offset_hz, 0).c_str()));
    const double span_s = part.size() / rate;
    const Scale t = {0.0, span_s, plot.left, plot.right()};
    const Scale f = {k_low_hz, k_high_hz, plot.bottom(), plot.top};
    const std::string clip = svg.clip(plot.left, plot.top, plot.width, plot.height);
    draw_spectrogram(svg, s, t, f, max_power(s), k_floor_db, clip);
    svg.rect(plot.left, plot.top, plot.width, plot.height, "none", k_axis, k_thin);
    x_axis(svg, t, plot.bottom(), 0.0, span_s - std::fmod(span_s, 0.5), 0.5, 1);
    y_axis(svg, f, plot.left, 500.0, 2500.0, 500.0, 0);
    svg.text(plot.left - 50.0, plot.top + plot.height / 2.0, "Hz", font(k_small_size, k_muted, "middle"), -90.0);
    const lb::Transmission& tx = recording.transmissions[0];
    for (size_t b = 0; b < data.size(); ++b) {
        const double a = lb::slot_start_sample(tx, b, 0) / rate - start_s;
        const double z = lb::slot_start_sample(tx, b + 1, 0) / rate - start_s;
        bracket_below(svg, t(a), t(z), plot.bottom() + 34.0, format("%c", data[b]), "");
    }
    color_bar(svg, plot.right() - 160.0, plot.bottom() + 70.0, 160.0, 10.0, k_floor_db);
    legend_item(svg, plot.left, plot.bottom() + 80.0, k_teal, "each beep: a START, a 1 or a STOP, on one pitch");
    svg.text(plot.left + plot.width / 2.0, plot.bottom() + k_axis_title_gap, "time (s)",
             font(k_small_size, k_muted, "middle"));
    std::vector<std::string> lines;
    lines.push_back("One pitch, beeps and silences: the receiver finds the pitch by itself (USB or LSB mistuning only "
                    "moves the line up or down) and reads the windows by counting.");
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "spectrogram.svg"), "Unlimited spectrogram",
             "The text CQ CQ DE UNLIMITED at 6 bytes per second through a USB channel with noise, as a spectrogram "
             "with one bracket per byte window.");
    std::printf("spectrogram.svg: %u bytes at 6 bytes/s, %s dB\n", static_cast<unsigned>(data.size()),
                number(k_spectrogram_snr_db, 0).c_str());
}

// ---------------------------------------------------------------------------
// Figure: bit error rate in AWGN
// ---------------------------------------------------------------------------

struct BerCount {
    size_t bits = 0;       // bits of the bytes released and matched to a sent byte
    size_t errors = 0;
    size_t sent = 0;       // bytes sent
    size_t lost = 0;       // bytes never released
    size_t extra = 0;
    size_t transmissions = 0;
    size_t locked = 0;     // transmissions locked on byte 0
};

struct BerJob {
    size_t speed;
    size_t offset;
    size_t transmission;
};

void add(BerCount& total, const lb::Score& s) {
    total.bits += s.matched * k_byte_bits;
    total.errors += s.bit_errors;
    total.sent += s.bytes_sent;
    total.lost += s.lost_bytes;
    total.extra += s.extra_bytes;
    total.transmissions += 1;
    total.locked += s.locked_transmissions;
}

void figure_ber(const std::string& directory) {
    const double offsets_db[] = {-3.0, -1.5, 0.0, 1.5, 3.0, 4.5, 6.0};
    const size_t k_offsets = sizeof(offsets_db) / sizeof(offsets_db[0]);
    const size_t k_speed_count = sizeof(k_speeds) / sizeof(k_speeds[0]);
    const size_t k_transmissions = 16;
    const size_t k_bytes = 16;
    const size_t k_modes = 2;  // fixed 70 %, adaptive
    std::vector<BerJob> jobs;
    for (size_t v = 0; v < k_speed_count; ++v)
        for (size_t o = 0; o < k_offsets; ++o)
            for (size_t t = 0; t < k_transmissions; ++t) jobs.push_back(BerJob{v, o, t});
    typedef std::vector<lb::Score> Scores;
    const std::vector<Scores> scores = parallel_map<Scores>(jobs.size(), [&](size_t i) {
        const BerJob& job = jobs[i];
        const EncoderConfig config = speed_config(k_speeds[job.speed]);
        const uint32_t seed = k_seed + static_cast<uint32_t>(i);
        const lb::Recording recording = lb::single(lb::random_bytes(k_bytes, seed), config, k_silence_ms);
        const std::vector<int16_t> samples =
            lb::usb(recording.samples, k_gate_db[job.speed] + offsets_db[job.offset], seed, config.amplitude,
                    k_figure_offset_hz);
        Scores out;
        for (size_t m = 0; m < k_modes; ++m) {
            DecoderConfig receiver = receiver_of(config);
            receiver.decision_mode = m == 0 ? unlimited::DecisionMode::fixed : unlimited::DecisionMode::adaptive;
            const lb::Capture capture = lb::run_decoder(samples, receiver, k_decoder_chunk);
            out.push_back(lb::score(recording, capture, lb::channel_delay_samples()));
        }
        return out;
    });
    std::vector<BerCount> totals(k_speed_count * k_offsets * k_modes);
    for (size_t i = 0; i < jobs.size(); ++i)
        for (size_t m = 0; m < k_modes; ++m)
            add(totals[(jobs[i].speed * k_offsets + jobs[i].offset) * k_modes + m], scores[i][m]);

    const char* const colors[] = {k_blue, k_teal, k_amber, k_pink, k_purple};
    const double k_width = 820.0;
    const double k_height = 560.0;
    const Box plot = {80.0, 110.0, 520.0, 330.0};
    const double k_low_db = -10.0;
    const double k_high_db = 16.0;
    const double k_ber_top = 0.5;
    const double k_ber_floor = 1e-5;
    const double k_marker = 3.0;
    const double k_legend_x = 630.0;
    Svg svg(k_width, k_height);
    heading(svg, "Bit error rate in white noise, per speed (spec 4 A1)",
            format("%u transmissions of %u random bytes per point through the USB channel simulator, mistuned %s Hz; "
                   "solid: fixed 70 %% line, dashed: adaptive (auto).",
                   static_cast<unsigned>(k_transmissions), static_cast<unsigned>(k_bytes),
                   signed_number(k_figure_offset_hz, 0).c_str()),
            "SNR: the key-down tone over the noise in 2500 Hz. Triangles: the provisional gate of each speed (v0.3's "
            "at the same T). Open circles: no error (plotted at 1 / 2n).");
    const Scale x = {k_low_db, k_high_db, plot.left, plot.right()};
    const LogScale y = {k_ber_floor, k_ber_top, plot.bottom(), plot.top};
    x_axis(svg, x, plot.bottom(), k_low_db, k_high_db, 2.0, 0, 1, plot.top);
    const double decades[] = {1e-5, 1e-4, 1e-3, 1e-2, 1e-1};
    for (size_t d = 0; d < sizeof(decades) / sizeof(decades[0]); ++d) {
        svg.line(plot.left, y(decades[d]), plot.right(), y(decades[d]), k_grid_line, k_line);
        svg.text(plot.left - k_tick_length - k_tick_label_pad, y(decades[d]) + k_text_rise,
                 format("1e%d", static_cast<int>(std::lround(std::log10(decades[d])))),
                 font(k_tick_size, k_muted, "end"));
    }
    svg.line(plot.left, plot.top, plot.left, plot.bottom(), k_axis, k_line);
    svg.text(plot.left + plot.width / 2.0, plot.bottom() + k_axis_title_gap, "SNR (dB, key-down tone in 2500 Hz)",
             font(k_small_size, k_muted, "middle"));
    svg.text(plot.left - 50.0, plot.top + plot.height / 2.0, "bit error rate", font(k_small_size, k_muted, "middle"),
             -90.0);
    double legend_y = plot.top + k_line_gap;
    for (size_t v = 0; v < k_speed_count; ++v) {
        triangle_down(svg, x(k_gate_db[v]), plot.bottom() - 6.0, 5.0, colors[v]);
        for (size_t m = 0; m < k_modes; ++m) {
            std::vector<Point> points;
            for (size_t o = 0; o < k_offsets; ++o) {
                const BerCount& c = totals[(v * k_offsets + o) * k_modes + m];
                if (c.bits == 0) continue;
                const bool none = c.errors == 0;
                const double ber = none ? 0.5 / c.bits : static_cast<double>(c.errors) / c.bits;
                const Point p = {x(k_gate_db[v] + offsets_db[o]), y(std::max(ber, k_ber_floor))};
                points.push_back(p);
                if (none) {
                    svg.circle(p.x, p.y, k_marker, k_card, colors[v], k_line);
                } else {
                    svg.circle(p.x, p.y, k_marker, colors[v]);
                }
            }
            svg.polyline(points, colors[v], k_curve, m == 0 ? std::string() : std::string(k_dash));
        }
        const BerCount& at_gate = totals[(v * k_offsets + 2) * k_modes];
        const BerCount& above = totals[(v * k_offsets + 4) * k_modes];
        legend_line(svg, k_legend_x, legend_y, colors[v], k_curve, "", format("%s bytes/s", number(k_speeds[v], 0).c_str()));
        svg.text(k_legend_x + 30.0, legend_y + 14.0,
                 format("gate %s dB: locked %u/%u; +3 dB: %u/%u", number(k_gate_db[v], 1).c_str(),
                        static_cast<unsigned>(at_gate.locked), static_cast<unsigned>(at_gate.transmissions),
                        static_cast<unsigned>(above.locked), static_cast<unsigned>(above.transmissions)),
                 font(k_small_size, k_muted));
        legend_y += 3.0 * k_line_gap;
    }
    size_t extra = 0;
    for (size_t i = 0; i < totals.size(); ++i) extra += totals[i].extra;
    std::vector<std::string> lines;
    lines.push_back(format("The fixed 70 %% line needs about 3 dB more than the adaptive line for the same bit error "
                           "rate near the gate. Bytes released that were not sent: %u.",
                           static_cast<unsigned>(extra)));
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "ber_awgn.svg"), "Unlimited bit error rate",
             "Measured bit error rate against SNR for 1, 3, 6, 12 and 25 bytes per second, with the fixed 70 % "
             "decision line and the adaptive line, through the USB channel simulator.");
    std::printf("ber_awgn.svg: %u transmissions\n", static_cast<unsigned>(jobs.size()));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: doc_figures <output directory>\n");
        return 1;
    }
    const std::string directory = argv[1];
    try {
        figure_beep(directory);
        figure_byte_window(directory);
        figure_hi_transmission(directory);
        figure_receiver_windows(directory);
        figure_speeds_spectrum(directory);
        figure_channels(directory);
        figure_spectrogram(directory);
        figure_ber(directory);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "doc_figures: %s\n", error.what());
        return 1;
    }
    return 0;
}
