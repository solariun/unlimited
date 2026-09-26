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
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Writes the documentation figures (SVG) into the directory given on the command line. Every waveform, spectrum,
// slot level, decode result and BER point is measured on the output of the real Encoder, the real sim::Channel and
// the real Decoder each time `make docs` runs. The only numbers not measured here are the bound curve of the BER
// figure (computed from its formula) and the v0.2-mfsk reference point quoted from that design's spec.
namespace {

using std::int16_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint8_t;
using unlimited::Band;
using unlimited::DecoderConfig;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSegment;
using unlimited::EncoderStatus;
using unlimited::Event;
using unlimited::EventType;
using unlimited::Passband;
using unlimited::PassbandFit;
using unlimited::Preset;
using unlimited::Profile;
using unlimited::SlotKind;

namespace lb = unlimited::loopback;
namespace sim = unlimited::sim;

// ---------------------------------------------------------------------------
// Units, conventions and shared constants
// ---------------------------------------------------------------------------

const double k_pi = 3.14159265358979323846;
const double k_us_per_s = 1e6;
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
const unsigned k_msb = k_byte_bits - 1;
const uint8_t k_first_printable = 0x20;
const uint8_t k_last_printable = 0x7E;
const char* const k_hi_text = "Hi";

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
const char* const k_teal_light = "#9FDDC6";
const char* const k_purple = "#7F77DD";        // markers: START, STOP, sync, END
const char* const k_purple_light = "#AFA9EC";  // tune tone
const char* const k_purple_fill = "#EEEDFE";
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
const double k_tile_radius = 8.0;
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
const double k_tile_text_left = 12.0;    // text inset inside a tile
const double k_tick_label_pad = 2.0;     // y-axis tick label to its tick
const double k_trace = 1.3;              // measured traces
const double k_curve = 1.8;              // measured BER curves
const double k_spectrum_trace = 1.1;
const double k_reference_trace = 1.2;
const double k_thin = 0.8;
const double k_line = 1.0;
const double k_medium = 1.5;
const double k_bold = 2.0;
const char* const k_dash = "5 3";
const char* const k_dot = "2 2";
const char* const k_guide_dash = "3 3";
const char* const k_short_dash = "4 3";
const char* const k_long_dash = "6 4";
const double k_milli = 1000.0;           // k_band_*_milli: thousandths of a cycle per slot

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

std::string printable_text(const std::vector<int>& values) {
    std::string text;
    for (size_t i = 0; i < values.size(); ++i) {
        const int v = values[i];
        if (v < 0) {
            text += '_';
        } else {
            text += v >= k_first_printable && v <= k_last_printable ? static_cast<char>(v) : '?';
        }
    }
    return text;
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

void diamond(Svg& svg, double x, double y, double half, const char* fill) {
    std::vector<Point> points;
    const Point top = {x, y - half};
    const Point right = {x + half, y};
    const Point bottom = {x, y + half};
    const Point left = {x - half, y};
    points.push_back(top);
    points.push_back(right);
    points.push_back(bottom);
    points.push_back(left);
    svg.polygon(points, fill);
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

void arrow_head(Svg& svg, double x, double y, double dx, double dy, const char* color) {
    const double k_head_length = 7.0;
    const double k_head_half = 3.5;
    const double length = std::sqrt(dx * dx + dy * dy);
    const double ux = dx / length;
    const double uy = dy / length;
    std::vector<Point> head;
    const Point tip = {x, y};
    const Point a = {x - ux * k_head_length - uy * k_head_half, y - uy * k_head_length + ux * k_head_half};
    const Point b = {x - ux * k_head_length + uy * k_head_half, y - uy * k_head_length - ux * k_head_half};
    head.push_back(tip);
    head.push_back(a);
    head.push_back(b);
    svg.polygon(head, color);
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
    std::vector<SlotRun> slots;  // the tail, when there is one, is the last
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

double slot_s_of(const EncoderConfig& config) {
    return config.slot_us / k_us_per_s;
}

double net_bit_rate(const EncoderConfig& config) {
    const double n = config.bits_per_package;
    return n / ((n + 1.0) * slot_s_of(config));
}

EncoderConfig preset_config(Preset preset, uint32_t rate_hz = k_rate_hz) {
    return EncoderConfig::from_preset(preset, rate_hz);
}

bool is_data_slot(const EncoderStatus& st) {
    return st.segment == EncoderSegment::package && st.slot >= 1 && st.slot <= st.package_bits;
}

size_t real_slots(const Rendered& r) {
    size_t count = 0;
    for (size_t s = 0; s < r.slots.size(); ++s) count += r.slots[s].status.segment != EncoderSegment::tail ? 1u : 0u;
    return count;
}

enum class SpanKind { lead_in, tune, sync, package, end, tail };

// A labelled part of the transmission: a package is its data slots and its STOP.
struct Span {
    SpanKind kind;
    size_t first_slot;
    size_t slots;
    size_t first;
    size_t samples;
    unsigned package;
    unsigned bits;
};

std::vector<Span> spans(const Rendered& r) {
    std::vector<Span> list;
    for (size_t s = 0; s < r.slots.size(); ++s) {
        const EncoderStatus& st = r.slots[s].status;
        SpanKind kind = SpanKind::tail;
        switch (st.segment) {
            case EncoderSegment::lead_in:
                kind = SpanKind::lead_in;
                break;
            case EncoderSegment::tune:
                kind = SpanKind::tune;
                break;
            case EncoderSegment::sync:
                kind = SpanKind::sync;
                break;
            case EncoderSegment::package:
                kind = SpanKind::package;
                break;
            case EncoderSegment::end:
                kind = SpanKind::end;
                break;
            case EncoderSegment::idle:
            case EncoderSegment::tail:
                break;
        }
        const bool new_package =
            kind == SpanKind::package && (list.empty() || list.back().package != st.package_index);
        if (list.empty() || list.back().kind != kind || new_package) {
            const Span span = {kind, s, 0, r.slots[s].first, 0, static_cast<unsigned>(st.package_index),
                               static_cast<unsigned>(st.package_bits)};
            list.push_back(span);
        }
        ++list.back().slots;
        list.back().samples += r.slots[s].count;
    }
    return list;
}

std::string span_name(const Span& span, bool short_form = false) {
    switch (span.kind) {
        case SpanKind::lead_in:
            return "lead-in";
        case SpanKind::tune:
            return "tune";
        case SpanKind::sync:
            return "sync";
        case SpanKind::package:
            return format(short_form ? "pkg %u" : "package %u", span.package);
        case SpanKind::end:
            return "END";
        case SpanKind::tail:
            return "tail";
    }
    return "";
}

const char* slot_color(const SlotRun& slot) {
    switch (slot.status.kind) {
        case SlotKind::tone:
            return k_purple_light;
        case SlotKind::marker:
            return k_purple;
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

// ---------------------------------------------------------------------------
// Figure: slot shapes
// ---------------------------------------------------------------------------

// w(u): Tukey alpha 0.5, sin^2 ramps of T/4 (spec 1.1).
double tukey(double u) {
    const double ramp = unlimited::k_tukey_ramp;
    if (u < ramp) return square(std::sin(k_pi / 2.0 * u / ramp));
    if (u > 1.0 - ramp) return square(std::sin(k_pi / 2.0 * (1.0 - u) / ramp));
    return 1.0;
}

// w(u) r(u): the twist, a shaped reversal from +1 to -1 over 0.375 T .. 0.625 T.
double marker_envelope(double u) {
    const double start = unlimited::k_reversal_start;
    const double width = unlimited::k_reversal_width;
    double r = 1.0;
    if (u >= start + width) {
        r = -1.0;
    } else if (u > start) {
        r = std::cos(k_pi * (u - start) / width);
    }
    return tukey(u) * r;
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

const SlotRun& find_slot(const Rendered& r, EncoderSegment segment, SlotKind kind) {
    for (size_t s = 0; s < r.slots.size(); ++s) {
        if (r.slots[s].status.segment == segment && r.slots[s].status.kind == kind) return r.slots[s];
    }
    throw std::runtime_error("slot not found");
}

enum class Shade { none, ramps, twist };

struct ShapePanel {
    const char* title;
    const char* note;
    const SlotRun* run;
    size_t slots;
    const char* color;
    Shade shade;
    std::function<double(double)> envelope;  // of u, in slots from the panel's start
};

void figure_slot_shapes(const std::string& directory, const Rendered& r) {
    const EncoderConfig& config = r.config;
    const std::vector<double> wave = to_double(r.audio, config.amplitude);
    const double rate = config.sample_rate_hz;
    const double slot_ms = slot_ms_of(config);
    const SlotRun& one = find_slot(r, EncoderSegment::package, SlotKind::one);
    const SlotRun& zero = find_slot(r, EncoderSegment::package, SlotKind::zero);
    const SlotRun& marker = find_slot(r, EncoderSegment::package, SlotKind::marker);
    const SlotRun& tune = find_slot(r, EncoderSegment::tune, SlotKind::tone);
    const size_t k_tune_slots_shown = 2;

    const double k_width = 820.0;
    const double k_height = 600.0;
    const double k_panel_left = 52.0;
    const double k_panel_width = 164.0;
    const double k_panel_gap = 30.0;
    const double k_panel_top = 112.0;
    const double k_panel_height = 118.0;
    const double k_amplitude_span = 1.15;
    const double k_slot_tick_ms = 4.0;
    const size_t k_label_every = 2;
    const double k_wave_width = 0.6;
    Svg svg(k_width, k_height);
    heading(svg, format("Slot shapes: real Encoder audio, hf preset (T %.0f ms, %u Hz)", slot_ms,
                        static_cast<unsigned>(config.tone_hz)),
            format("Rendered by the Encoder at %u Hz; amplitude relative to the crest A; dashed: the envelopes of "
                   "spec 1.1.",
                   static_cast<unsigned>(rate)));

    const ShapePanel panels[] = {
        {"Data 1: a beep", "Tukey α 0.5: ramps T/4", &one, 1, k_teal, Shade::ramps,
         [](double u) { return tukey(u); }},
        {"Data 0: silence", "no tone for the whole slot", &zero, 1, k_gray, Shade::none, [](double) { return 0.0; }},
        {"Marker (START/STOP)", "the beep + a twist at T/2", &marker, 1, k_purple, Shade::twist,
         [](double u) { return marker_envelope(u); }},
        {"Tune tone (start)", "ramp T/4, then steady", &tune, k_tune_slots_shown, k_purple_light, Shade::none,
         [](double u) { return u < unlimited::k_tukey_ramp ? tukey(u) : 1.0; }}};
    const Scale y = {-k_amplitude_span, k_amplitude_span, k_panel_top + k_panel_height, k_panel_top};
    for (size_t p = 0; p < sizeof(panels) / sizeof(panels[0]); ++p) {
        const ShapePanel& panel = panels[p];
        const Box box = {k_panel_left + p * (k_panel_width + k_panel_gap), k_panel_top, k_panel_width, k_panel_height};
        const double span_ms = panel.slots * slot_ms;
        const Scale x = {0.0, span_ms, box.left, box.right()};
        const double ramp_ms = unlimited::k_tukey_ramp * slot_ms;
        if (panel.shade == Shade::ramps) {
            svg.rect(x(0.0), box.top, x(ramp_ms) - x(0.0), box.height, k_gray_fill);
            svg.rect(x(slot_ms - ramp_ms), box.top, x(slot_ms) - x(slot_ms - ramp_ms), box.height, k_gray_fill);
        }
        if (panel.shade == Shade::twist) {
            const double a = unlimited::k_reversal_start * slot_ms;
            const double b = (unlimited::k_reversal_start + unlimited::k_reversal_width) * slot_ms;
            svg.rect(x(a), box.top, x(b) - x(a), box.height, k_purple_fill);
        }
        svg.text(box.left, box.top - k_panel_title_rise, panel.title, font(k_label_size, k_ink, "start", true));
        svg.text(box.left, box.top - k_panel_note_rise, panel.note, font(k_note_size, k_muted));
        y_axis(svg, y, box.left, -1.0, 1.0, 1.0, 0, box.right());
        const double tick_ms = panel.slots == 1 ? k_slot_tick_ms : slot_ms / k_label_every;
        x_axis(svg, x, box.bottom(), 0.0, span_ms, tick_ms, 0, k_label_every);
        svg.text(box.left + box.width / 2.0, box.bottom() + k_axis_title_gap, "time (ms)",
                 font(k_small_size, k_muted, "middle"));
        draw_wave(svg, x, y, wave, panel.run->first, panel.slots * panel.run->count, panel.run->first, rate,
                  panel.color, k_wave_width);
        draw_envelope(svg, x, y, 0.0, span_ms, [&](double t) { return panel.envelope(t / slot_ms); });
        if (panel.shade == Shade::none && panel.slots == 1) {
            svg.text(box.left + box.width / 2.0, y(0.0) - 2.0 * k_text_rise, "nothing is sent",
                     font(k_note_size, k_muted, "middle"));
        }
    }

    // Zoom on the marker's flat top, with the carrier before the twist continued (least-squares fit before 0.375 T).
    const double k_zoom_first_u = unlimited::k_tukey_ramp;
    const double k_zoom_last_u = 1.0 - unlimited::k_tukey_ramp;
    const double k_zoom_tick_ms = 1.0;
    const double k_zoom_span = 1.45;  // room above the crest for the labels
    const double k_zoom_top = 346.0;
    const double k_zoom_height = 140.0;
    const double k_zoom_wave_width = 1.4;
    const double k_label_drop = 14.0;
    const Box zoom = {k_panel_left, k_zoom_top, 3.0 * k_panel_gap + 4.0 * k_panel_width, k_zoom_height};
    const double z0 = k_zoom_first_u * slot_ms;
    const double z1 = k_zoom_last_u * slot_ms;
    const Scale zx = {z0, z1, zoom.left, zoom.right()};
    const Scale zy = {-k_zoom_span, k_zoom_span, zoom.bottom(), zoom.top};
    const double omega = 2.0 * k_pi * config.tone_hz / rate;
    const size_t fit_first = marker.first + static_cast<size_t>(unlimited::k_tukey_ramp * marker.count);
    const size_t fit_last = marker.first + static_cast<size_t>(unlimited::k_reversal_start * marker.count);
    double ss = 0.0;
    double sc = 0.0;
    double cc = 0.0;
    double ys = 0.0;
    double yc = 0.0;
    for (size_t n = fit_first; n < fit_last; ++n) {
        const double s = std::sin(omega * n);
        const double c = std::cos(omega * n);
        ss += s * s;
        sc += s * c;
        cc += c * c;
        ys += wave[n] * s;
        yc += wave[n] * c;
    }
    const double determinant = ss * cc - sc * sc;
    const double a = (ys * cc - yc * sc) / determinant;
    const double b = (yc * ss - ys * sc) / determinant;
    const double twist_first_ms = unlimited::k_reversal_start * slot_ms;
    const double twist_last_ms = (unlimited::k_reversal_start + unlimited::k_reversal_width) * slot_ms;
    svg.rect(zx(twist_first_ms), zoom.top, zx(twist_last_ms) - zx(twist_first_ms), zoom.height, k_purple_fill);
    svg.text(zoom.left, zoom.top - k_panel_title_rise,
             format("The twist, zoomed: the flat top of the marker above, u = %.2f…%.2f of the slot", k_zoom_first_u,
                    k_zoom_last_u),
             font(k_label_size, k_ink, "start", true));
    svg.text(zoom.left, zoom.top - k_panel_note_rise,
             "purple: Encoder audio; gray dotted: the carrier before the twist, continued; shaded: 0.375 T – 0.625 T",
             font(k_note_size, k_muted));
    y_axis(svg, zy, zoom.left, -1.0, 1.0, 1.0, 0, zoom.right());
    x_axis(svg, zx, zoom.bottom(), z0, z1, k_zoom_tick_ms, 0, k_label_every);
    std::vector<Point> reference;
    const size_t zoom_first = marker.first + static_cast<size_t>(k_zoom_first_u * marker.count);
    const size_t zoom_last = marker.first + static_cast<size_t>(k_zoom_last_u * marker.count);
    for (size_t n = zoom_first; n <= zoom_last; ++n) {
        const Point p = {zx((n - static_cast<double>(marker.first)) * k_ms_per_s / rate),
                         zy(a * std::sin(omega * n) + b * std::cos(omega * n))};
        reference.push_back(p);
    }
    svg.polyline(reference, k_gray, k_reference_trace, k_dot);
    draw_wave(svg, zx, zy, wave, zoom_first, zoom_last - zoom_first + 1, marker.first, rate, k_purple,
              k_zoom_wave_width);
    draw_envelope(svg, zx, zy, z0, z1, [&](double t) { return marker_envelope(t / slot_ms); });
    const double label_y = zoom.top + k_label_drop;
    svg.text((zoom.left + zx(twist_first_ms)) / 2.0, label_y, "same phase as before",
             font(k_note_size, k_ink, "middle", true));
    svg.text((zx(twist_first_ms) + zx(twist_last_ms)) / 2.0, label_y,
             "the twist: amplitude through 0, phase turns 180°", font(k_note_size, k_purple, "middle", true));
    svg.text((zx(twist_last_ms) + zoom.right()) / 2.0, label_y, "inverted: s ← −s",
             font(k_note_size, k_ink, "middle", true));
    svg.text(zoom.left + zoom.width / 2.0, zoom.bottom() + k_axis_title_gap, "time in the marker slot (ms)",
             font(k_small_size, k_muted, "middle"));
    std::vector<std::string> lines;
    lines.push_back("A marker is a data beep whose second half is upside down: the ear hears the same beep, the "
                    "receiver sees the twist.");
    lines.push_back("After a marker the carrier stays inverted (s ← −s) until the next marker; data slots never "
                    "reverse the phase.");
    lines.push_back(format("Energy per slot (T·A²/2): data 1 %.4g, marker %.4g. Every envelope is 0 at the slot "
                           "edges: no key clicks.",
                           unlimited::k_one_energy, unlimited::k_marker_energy));
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "slot_shapes.svg"), "Unlimited slot shapes",
             "A data 1 (Tukey beep), a data 0 (silence), a START/STOP marker with its mid-slot twist, the start of "
             "the tune tone, and the twist zoomed, rendered by the Encoder (hf preset, 48 kHz).");
    std::printf("slot_shapes.svg: 1, 0, marker and tune slots of hf at %u Hz\n", static_cast<unsigned>(rate));
}

// ---------------------------------------------------------------------------
// Figure: transmission timeline
// ---------------------------------------------------------------------------

std::string span_detail(const Span& span, const Rendered& r) {
    switch (span.kind) {
        case SpanKind::lead_in:
            return format("%u ms", static_cast<unsigned>(r.config.lead_in_ms));
        case SpanKind::tune:
            return format("%u slots, %.0f ms", static_cast<unsigned>(span.slots), span.slots * slot_ms_of(r.config));
        case SpanKind::sync:
            return format("%u markers", static_cast<unsigned>(span.slots));
        case SpanKind::package: {
            const size_t byte = span.package * r.config.bits_per_package / k_byte_bits;
            if (span.bits == k_byte_bits && byte < r.data.size())
                return format("0x%02X %s + STOP", r.data[byte], char_of(r.data[byte]).c_str());
            return format("%u bits + STOP", span.bits);
        }
        case SpanKind::end:
            return format("%u markers", static_cast<unsigned>(span.slots));
        case SpanKind::tail:
            return format("%u ms", static_cast<unsigned>(r.config.tail_ms));
    }
    return "";
}

// Min/max outline per pixel column of each run of slots of one colour (no seams between neighbouring slots).
void draw_envelope_runs(Svg& svg, const Rendered& r, const std::vector<double>& wave, const Scale& x, const Scale& y) {
    const double rate = r.config.sample_rate_hz;
    std::vector<Point> top;
    std::vector<Point> bottom;
    for (size_t s = 0; s < r.slots.size(); ++s) {
        const SlotRun& slot = r.slots[s];
        if (slot.status.kind == SlotKind::silent || slot.status.kind == SlotKind::zero) continue;
        const double x0 = x(slot.first / rate);
        const double x1 = x((slot.first + slot.count) / rate);
        const size_t columns = std::max<size_t>(1, static_cast<size_t>(std::ceil(x1 - x0)));
        for (size_t c = 0; c < columns; ++c) {
            const size_t a = slot.first + slot.count * c / columns;
            const size_t b = slot.first + slot.count * (c + 1) / columns;
            double low = 0.0;
            double high = 0.0;
            for (size_t n = a; n < b; ++n) {
                low = std::min(low, wave[n]);
                high = std::max(high, wave[n]);
            }
            const double px = columns == 1 ? x0 : x0 + (x1 - x0) * c / (columns - 1);
            const Point p = {px, y(high)};
            const Point q = {px, y(low)};
            top.push_back(p);
            bottom.push_back(q);
        }
        const bool run_ends = s + 1 == r.slots.size() || std::string(slot_color(r.slots[s + 1])) != slot_color(slot) ||
                              r.slots[s + 1].status.kind == SlotKind::zero;
        if (!run_ends) continue;
        std::vector<Point> outline(top);
        outline.insert(outline.end(), bottom.rbegin(), bottom.rend());
        svg.polygon(outline, slot_color(slot));
        top.clear();
        bottom.clear();
    }
}

void figure_timeline(const std::string& directory, const Rendered& r) {
    const EncoderConfig& config = r.config;
    const double rate = config.sample_rate_hz;
    const std::vector<double> wave = to_double(r.audio, config.amplitude);
    const std::vector<Span> list = spans(r);
    const double duration_ms = r.audio.size() * k_ms_per_s / rate;

    const double k_width = 820.0;
    const double k_height = 452.0;
    const double k_left = 60.0;
    const double k_right = 796.0;
    const double k_wave_top = 118.0;
    const double k_wave_bottom = 214.0;
    const double k_marker_label_y = 108.0;
    const double k_strip_top = 224.0;
    const double k_strip_height = 16.0;
    const double k_bits_y = 256.0;
    const double k_bracket_y = 270.0;
    const double k_axis_y = 328.0;
    const double k_legend_y = 384.0;
    const double k_time_step_ms = 100.0;
    const double k_axis_title_x = 36.0;
    Svg svg(k_width, k_height);
    heading(svg, format("Transmission layout: “%s” with the hf preset", k_hi_text),
            format("Real Encoder output: T %.0f ms, N %u bits per package, pitch %u Hz; %u slots + %u ms tail = "
                   "%.0f ms.",
                   slot_ms_of(config), static_cast<unsigned>(config.bits_per_package),
                   static_cast<unsigned>(config.tone_hz), static_cast<unsigned>(real_slots(r)),
                   static_cast<unsigned>(config.tail_ms), duration_ms),
            format("“%s” = 0x48 0x69 = 01001000 01101001, most significant bit first: one byte per package. No lead-in "
                   "on the HF presets.",
                   k_hi_text));
    const Scale x = {0.0, duration_ms / k_ms_per_s, k_left, k_right};
    const Scale y = {-1.0, 1.0, k_wave_bottom, k_wave_top};
    svg.line(k_left, y(0.0), k_right, y(0.0), k_grid_line, k_line);
    y_axis(svg, y, k_left, -1.0, 1.0, 1.0, 0);
    svg.text(k_left - k_axis_title_x, (k_wave_top + k_wave_bottom) / 2.0, "audio / A",
             font(k_small_size, k_muted, "middle"), -90.0);
    draw_envelope_runs(svg, r, wave, x, y);

    // Slot strip, the bit of each data slot under it, marker labels and segment brackets.
    const Font bit_one = font(k_note_size, k_teal_dark, "middle", true);
    const Font bit_zero = font(k_note_size, k_muted, "middle", true);
    for (size_t s = 0; s < r.slots.size(); ++s) {
        const SlotRun& slot = r.slots[s];
        const double x0 = x(slot.first / rate);
        const double x1 = x((slot.first + slot.count) / rate);
        svg.rect(x0, k_strip_top, x1 - x0, k_strip_height, slot_color(slot), k_card, k_thin);
        if (!is_data_slot(slot.status)) continue;
        const bool one = slot.status.kind == SlotKind::one;
        svg.text((x0 + x1) / 2.0, k_bits_y, one ? "1" : "0", one ? bit_one : bit_zero);
    }
    const Font marker_font = font(k_small_size, k_purple, "middle", true);
    for (size_t i = 0; i < list.size(); ++i) {
        const Span& span = list[i];
        const double x0 = x(span.first / rate);
        const double x1 = x((span.first + span.samples) / rate);
        const double slot_px = (x1 - x0) / span.slots;
        if (span.kind == SpanKind::sync) svg.text(x1 - slot_px / 2.0, k_marker_label_y, "START", marker_font);
        if (span.kind == SpanKind::package) {
            const bool last = i + 1 < list.size() && list[i + 1].kind == SpanKind::end;
            // The last STOP and the END markers share one label: they are neighbouring slots.
            const double end_px = last ? list[i + 1].slots * slot_px : 0.0;
            svg.text(x1 - slot_px / 2.0 + end_px / 2.0, k_marker_label_y, last ? "STOP + END" : "STOP = START",
                     marker_font);
        }
        bracket_below(svg, x0, x1, k_bracket_y, span_name(span), span_detail(span, r));
    }
    x_axis(svg, x, k_axis_y, 0.0, std::floor(duration_ms / k_time_step_ms) * k_time_step_ms / k_ms_per_s,
           k_time_step_ms / k_ms_per_s, 1);
    svg.text((k_left + k_right) / 2.0, k_axis_y + k_axis_title_gap, "time (s)", font(k_small_size, k_muted, "middle"));

    double lx = k_left;
    lx = legend_item(svg, lx, k_legend_y, k_purple_light, "tune: steady tone");
    lx = legend_item(svg, lx, k_legend_y, k_purple, "marker: beep + twist (sync, START/STOP, END)");
    lx = legend_item(svg, lx, k_legend_y, k_teal, "1: a beep");
    legend_item(svg, lx, k_legend_y, k_gray_fill, "0: silence");
    std::vector<std::string> lines;
    lines.push_back("The last sync marker is the START of package 0; every STOP is the START of the next package; END "
                    "is 2 markers after the last STOP.");
    lines.push_back(format("The receiver measures T on the sync train and N from the first START-to-STOP span: %u "
                           "slots = %u bits + STOP (spec 3.8).",
                           static_cast<unsigned>(config.bits_per_package + 1u),
                           static_cast<unsigned>(config.bits_per_package)));
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "transmission_timeline.svg"), "Unlimited transmission layout",
             "Envelope of the hf transmission of Hi: tune tone, sync train, START, package 0 (0x48), STOP = START, "
             "package 1 (0x69), STOP, END and tail, with the bit of every data slot.");
    std::printf("transmission_timeline.svg: %u slots, %.0f ms\n", static_cast<unsigned>(real_slots(r)), duration_ms);
}

// ---------------------------------------------------------------------------
// Figure: spectrogram of the hf transmission
// ---------------------------------------------------------------------------

const double k_low_hz = unlimited::k_ssb_passband_low_hz;
const double k_high_hz = unlimited::k_ssb_passband_high_hz;

// A vertical bracket at x from y0 to y1, opening to the left.
void bracket_left(Svg& svg, double x, double y0, double y1, const char* color) {
    const double k_arm = 5.0;
    svg.line(x, y0, x, y1, color, k_medium);
    svg.line(x - k_arm, y0, x, y0, color, k_medium);
    svg.line(x - k_arm, y1, x, y1, color, k_medium);
}

void figure_spectrogram(const std::string& directory, const Rendered& r) {
    const EncoderConfig& config = r.config;
    const double rate = config.sample_rate_hz;
    const double duration = r.audio.size() / rate;
    const size_t k_window = 128;
    const size_t k_hop = 8;
    const size_t k_fft = 512;
    const double k_floor_db = -50.0;
    const Spectrogram s =
        spectrogram(to_double(r.audio, k_full_scale), rate, k_window, k_hop, k_fft, k_low_hz, k_high_hz);
    const Band band = occupied_band(config);
    const PassbandFit fit = passband_fit(config);

    const double k_width = 820.0;
    const double k_height = 470.0;
    const Box plot = {64.0, 96.0, 520.0, 300.0};
    Svg svg(k_width, k_height);
    heading(svg, format("Spectrogram of the hf transmission “%s”: everything on one pitch", k_hi_text),
            format("Real Encoder audio at %u Hz; STFT with a %.0f ms Hann window every %.1f ms; power in dB relative "
                   "to the strongest cell.",
                   static_cast<unsigned>(rate), k_window * k_ms_per_s / rate, k_hop * k_ms_per_s / rate));
    const Scale t = {0.0, duration, plot.left, plot.right()};
    const Scale f = {k_low_hz, k_high_hz, plot.bottom(), plot.top};
    const std::string clip_id = svg.clip(plot.left, plot.top, plot.width, plot.height);
    draw_spectrogram(svg, s, t, f, max_power(s), k_floor_db, clip_id);
    svg.rect(plot.left, plot.top, plot.width, plot.height, "none", k_axis, k_line);

    const std::vector<Span> list = spans(r);
    const double k_segment_label_rise = 7.0;
    for (size_t i = 0; i < list.size(); ++i) {
        const double x0 = t(list[i].first / rate);
        const double x1 = t((list[i].first + list[i].samples) / rate);
        if (i > 0) svg.line(x0, plot.top, x0, plot.bottom(), k_gray, k_thin, k_guide_dash);
        svg.text((x0 + x1) / 2.0, plot.top - k_segment_label_rise, span_name(list[i], true),
                 font(k_small_size, k_ink, "middle", true));
    }
    svg.line(plot.left, f(config.tone_hz), plot.right(), f(config.tone_hz), k_ink, k_line, k_dash);

    // Right column: the band, the room to mistune on each side, the colour bar.
    const double k_bracket_x = plot.right() + 14.0;
    const double k_note_x = plot.right() + 24.0;
    const double k_arrow_x = plot.right() + 8.0;
    bracket_left(svg, k_bracket_x, f(band.high_hz), f(band.low_hz), k_teal);
    const double mid = f(config.tone_hz);
    svg.text(k_note_x, mid - k_line_gap + k_text_rise,
             format("pitch %u Hz (dashed)", static_cast<unsigned>(config.tone_hz)),
             font(k_note_size, k_ink, "start", true));
    svg.text(k_note_x, mid + k_text_rise, format("occupied band %u–%u Hz", static_cast<unsigned>(band.low_hz),
                                                 static_cast<unsigned>(band.high_hz)),
             font(k_note_size, k_teal_dark, "start", true));
    svg.text(k_note_x, mid + k_line_gap + k_text_rise,
             format("%u Hz = %.1f/T, occupied_band()", static_cast<unsigned>(band.width_hz),
                    unlimited::k_band_99_milli / k_milli),
             font(k_small_size, k_muted));
    const double k_arrow_gap = 3.0;
    svg.line(k_arrow_x, f(band.high_hz) - k_arrow_gap, k_arrow_x, f(k_high_hz), k_gray, k_line, k_guide_dash);
    arrow_head(svg, k_arrow_x, f(k_high_hz), 0.0, -1.0, k_gray);
    svg.line(k_arrow_x, f(band.low_hz) + k_arrow_gap, k_arrow_x, f(k_low_hz), k_gray, k_line, k_guide_dash);
    arrow_head(svg, k_arrow_x, f(k_low_hz), 0.0, 1.0, k_gray);
    const double up_y = (f(band.high_hz) + f(k_high_hz)) / 2.0;
    const double down_y = (f(band.low_hz) + f(k_low_hz)) / 2.0;
    svg.text(k_note_x, up_y, format("+%d Hz of room above", fit.margin_high_hz), font(k_note_size, k_ink));
    svg.text(k_note_x, up_y + k_line_gap, format("(to the %u Hz filter edge)", static_cast<unsigned>(k_high_hz)),
             font(k_small_size, k_muted));
    svg.text(k_note_x, down_y - k_line_gap, format("%s Hz of room below", number(-fit.margin_low_hz, 0).c_str()),
             font(k_note_size, k_ink));
    svg.text(k_note_x, down_y, format("(to the %u Hz filter edge):", static_cast<unsigned>(k_low_hz)),
             font(k_small_size, k_muted));
    svg.text(k_note_x, down_y + k_line_gap, format("shift tolerance ±%u Hz", static_cast<unsigned>(fit.tolerance_hz)),
             font(k_note_size, k_ink, "start", true));
    const double k_bar_width = 140.0;
    const double k_bar_height = 10.0;
    const double k_bar_rise = 26.0;
    color_bar(svg, k_note_x, plot.bottom() - k_bar_rise, k_bar_width, k_bar_height, k_floor_db);

    const double k_frequency_step = 300.0;
    y_axis(svg, f, plot.left, k_low_hz, k_high_hz, k_frequency_step, 0);
    const double k_axis_title_x = 48.0;
    svg.text(plot.left - k_axis_title_x, (plot.top + plot.bottom()) / 2.0, "audio frequency (Hz)",
             font(k_small_size, k_muted, "middle"), -90.0);
    const double k_time_step_s = 0.1;
    x_axis(svg, t, plot.bottom(), 0.0, std::floor(duration / k_time_step_s) * k_time_step_s, k_time_step_s, 1);
    svg.text((plot.left + plot.right()) / 2.0, plot.bottom() + k_axis_title_gap, "time (s)",
             font(k_small_size, k_muted, "middle"));
    std::vector<std::string> lines;
    lines.push_back(format("The frame is the SSB 2.4 kHz passband (%.0f–%.0f Hz): the tune, the markers and the "
                           "beeps all sit on %u Hz; silence is a 0.",
                           k_low_hz, k_high_hz, static_cast<unsigned>(config.tone_hz)));
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "spectrogram_hf.svg"), "Unlimited hf spectrogram",
             "Waterfall of the hf transmission of Hi in the 300-2700 Hz passband: tune, sync markers, two packages "
             "and END, all on 1500 Hz, with the 276 Hz occupied band and the room to mistune.");
    std::printf("spectrogram_hf.svg: %u x %u cells, pitch %u Hz, band %u-%u Hz, tolerance +-%u Hz\n",
                static_cast<unsigned>(s.columns), static_cast<unsigned>(s.rows), static_cast<unsigned>(config.tone_hz),
                static_cast<unsigned>(band.low_hz), static_cast<unsigned>(band.high_hz),
                static_cast<unsigned>(fit.tolerance_hz));
}

// ---------------------------------------------------------------------------
// Figure: packing bytes into packages (spec 2.2)
// ---------------------------------------------------------------------------

// Spec 2.2, "Hi" with N = 8, 4 and 3: the packages, and the data slots + STOPs.
const size_t k_max_hi_packages = 6;
struct PackingCase {
    uint8_t bits;
    const char* label;
    const char* packages[k_max_hi_packages];
    unsigned data_and_stops;
};

const PackingCase k_packing[] = {{8, "the HF presets", {"01001000", "01101001", 0, 0, 0, 0}, 18},
                                 {4, "two per byte", {"0100", "1000", "0110", "1001", 0, 0}, 20},
                                 {3, "bytes span packages", {"010", "010", "000", "110", "100", "1"}, 22}};

// One slot of a packing row: a marker, or a data slot with its bit and the byte it belongs to.
struct PackedSlot {
    bool marker;
    bool end;
    int bit;
    unsigned byte;
    unsigned package;
};

std::vector<PackedSlot> packed_slots(const Rendered& r) {
    std::vector<PackedSlot> row;
    for (size_t s = 0; s < r.slots.size(); ++s) {
        const EncoderStatus& st = r.slots[s].status;
        const bool first_start = st.segment == EncoderSegment::sync && s + 1 < r.slots.size() &&
                                 r.slots[s + 1].status.segment != EncoderSegment::sync;
        if (first_start || st.segment == EncoderSegment::end) {
            const PackedSlot m = {true, st.segment == EncoderSegment::end, -1, 0, 0};
            row.push_back(m);
        }
        if (st.segment != EncoderSegment::package) continue;
        if (!is_data_slot(st)) {
            const PackedSlot stop = {true, false, -1, 0, static_cast<unsigned>(st.package_index)};
            row.push_back(stop);
            continue;
        }
        const PackedSlot d = {false, false, st.kind == SlotKind::one ? 1 : 0, static_cast<unsigned>(st.byte_index),
                              static_cast<unsigned>(st.package_index)};
        row.push_back(d);
    }
    return row;
}

void check_packing(const PackingCase& c, const Rendered& r) {
    std::vector<std::string> packages;
    size_t data_and_stops = 0;
    for (size_t s = 0; s < r.slots.size(); ++s) {
        const EncoderStatus& st = r.slots[s].status;
        if (st.segment != EncoderSegment::package) continue;
        ++data_and_stops;
        if (!is_data_slot(st)) continue;
        if (packages.size() <= st.package_index) packages.resize(st.package_index + 1);
        packages[st.package_index] += st.kind == SlotKind::one ? '1' : '0';
        const bool one = ((r.data[st.byte_index] >> (k_msb - st.bit_index)) & 1u) != 0;
        if (one != (st.kind == SlotKind::one)) throw std::runtime_error("a data slot's kind is not its bit");
    }
    bool same = data_and_stops == c.data_and_stops && packages.size() <= k_max_hi_packages;
    for (size_t p = 0; same && p < k_max_hi_packages; ++p) {
        same = p < packages.size() ? c.packages[p] != 0 && packages[p] == c.packages[p] : c.packages[p] == 0;
    }
    if (!same) {
        throw std::runtime_error(format("packing N = %u disagrees with spec 2.2", static_cast<unsigned>(c.bits)));
    }
}

void figure_packing(const std::string& directory) {
    const std::vector<uint8_t> hi = text_bytes(k_hi_text);
    const double k_width = 820.0;
    const double k_height = 506.0;
    const double k_cells_left = 150.0;
    const double k_cell = 25.0;
    const double k_cell_gap = 2.0;
    const double k_cell_height = 24.0;
    const double k_cell_text_drop = 16.5;
    const double k_label_x = k_margin;
    const double k_bytes_top = 92.0;
    const double k_byte_height = 22.0;
    const double k_stream_top = 118.0;
    const double k_first_row_top = 186.0;
    const double k_row_pitch = 92.0;
    const double k_package_label_rise = 6.0;
    const double k_byte_bracket_drop = 8.0;
    const double k_byte_label_drop = 20.0;
    const double k_row_note_drop = 42.0;
    const double k_diamond_half = 6.0;
    const double k_marker_label_drop = 11.0;
    const double k_row_sub_drop = 14.0;
    const double k_row_label_drop = 12.5;
    const double k_byte_text_drop = 15.5;
    Svg svg(k_width, k_height);
    heading(svg, format("From bytes to packages: “%s” with N = 8, 4 and 3", k_hi_text),
            "Computed with the Encoder (Encoder::status() per slot); the bytes are one stream of bits, MSB first, "
            "cut into N-bit packages.",
            "◆ = a marker (the beep with the twist): the last sync marker is the first START, every STOP is the next "
            "START, END = 2 markers.");
    const Font cell_font = font(k_note_size, k_ink, "middle", true);
    const Font one_font = font(k_note_size, k_card, "middle", true);
    const auto cell_x = [&](size_t i) { return k_cells_left + i * k_cell; };
    const auto data_cell = [&](double x, double top, int bit) {
        if (bit != 0) {
            svg.rect(x, top, k_cell - k_cell_gap, k_cell_height, k_teal, k_teal, k_line, k_cell_radius);
            svg.text(x + (k_cell - k_cell_gap) / 2.0, top + k_cell_text_drop, "1", one_font);
        } else {
            svg.rect(x, top, k_cell - k_cell_gap, k_cell_height, k_card, k_axis, k_line, k_cell_radius);
            svg.text(x + (k_cell - k_cell_gap) / 2.0, top + k_cell_text_drop, "0", cell_font);
        }
    };

    // The bytes and their bit stream, aligned with the first data slot of the rows below.
    svg.text(k_label_x, k_bytes_top + k_byte_text_drop, "bytes", font(k_label_size, k_ink, "start", true));
    svg.text(k_label_x, k_stream_top + k_cell_text_drop, "bit stream", font(k_label_size, k_ink, "start", true));
    for (size_t b = 0; b < hi.size(); ++b) {
        const double x0 = cell_x(1 + b * k_byte_bits);
        const double w = k_byte_bits * k_cell - k_cell_gap;
        svg.rect(x0, k_bytes_top, w, k_byte_height, k_gray_fill, k_axis, k_line, k_cell_radius);
        svg.text(x0 + w / 2.0, k_bytes_top + k_byte_text_drop,
                 format("0x%02X %s = %s", hi[b], char_of(hi[b]).c_str(), binary(hi[b], k_byte_bits).c_str()),
                 font(k_note_size, k_ink, "middle", true));
        for (unsigned i = 0; i < k_byte_bits; ++i) {
            const int bit = (hi[b] >> (k_msb - i)) & 1;
            data_cell(cell_x(1 + b * k_byte_bits + i), k_stream_top, bit);
        }
    }

    for (size_t c = 0; c < sizeof(k_packing) / sizeof(k_packing[0]); ++c) {
        const PackingCase& pc = k_packing[c];
        EncoderConfig config = preset_config(Preset::hf);
        config.bits_per_package = pc.bits;
        const Rendered r = render(hi, config);
        check_packing(pc, r);
        const std::vector<PackedSlot> row = packed_slots(r);
        const double top = k_first_row_top + c * k_row_pitch;
        svg.text(k_label_x, top + k_row_label_drop, format("N = %u", static_cast<unsigned>(pc.bits)),
                 font(k_title_size - 2.0, k_ink, "start", true));
        svg.text(k_label_x, top + k_row_label_drop + k_row_sub_drop, pc.label, font(k_small_size, k_muted));
        size_t packages = 0;
        for (size_t i = 0; i < row.size(); ++i) {
            const double x0 = cell_x(i);
            const double cx = x0 + (k_cell - k_cell_gap) / 2.0;
            if (row[i].marker) {
                svg.rect(x0, top, k_cell - k_cell_gap, k_cell_height, k_purple, k_purple, k_line, k_cell_radius);
                diamond(svg, cx, top + k_cell_height / 2.0, k_diamond_half, k_card);
                const char* label = i == 0 ? "START" : "";
                if (!row[i].end && i + 1 < row.size() && row[i + 1].end) label = "STOP";
                if (row[i].end && i + 1 < row.size()) label = "END";
                if (label[0] != '\0') {
                    const double lx = row[i].end ? x0 + k_cell - k_cell_gap / 2.0 : cx;
                    svg.text(lx, top + k_cell_height + k_marker_label_drop, label,
                             font(k_small_size - 1.0, k_purple, "middle", true));
                }
                continue;
            }
            data_cell(x0, top, row[i].bit);
            const bool first_of_package = i > 0 && row[i - 1].marker;
            if (!first_of_package) continue;
            size_t last = i;
            while (last + 1 < row.size() && !row[last + 1].marker) ++last;
            const double px = (x0 + cell_x(last) + k_cell - k_cell_gap) / 2.0;
            const double span = (last - i + 1) * k_cell;
            const std::string name = format("package %u", row[i].package);
            svg.text(px, top - k_package_label_rise,
                     text_width(name, k_small_size) < span ? name : format("%u", row[i].package),
                     font(k_small_size, k_muted, "middle"));
            ++packages;
        }
        // Byte brackets under the data slots of each byte.
        for (size_t b = 0; b < hi.size(); ++b) {
            size_t first = row.size();
            size_t last = 0;
            for (size_t i = 0; i < row.size(); ++i) {
                if (row[i].marker || row[i].byte != b) continue;
                first = std::min(first, i);
                last = std::max(last, i);
            }
            const double x0 = cell_x(first) + 1.0;
            const double x1 = cell_x(last) + k_cell - k_cell_gap - 1.0;
            const double by = top + k_cell_height + k_byte_bracket_drop;
            const double k_tick = 4.0;
            svg.line(x0, by, x1, by, k_ink, k_line);
            svg.line(x0, by, x0, by - k_tick, k_ink, k_line);
            svg.line(x1, by, x1, by - k_tick, k_ink, k_line);
            svg.text((x0 + x1) / 2.0, by + k_byte_label_drop - k_byte_bracket_drop,
                     format("0x%02X %s", hi[b], char_of(hi[b]).c_str()), font(k_small_size, k_ink, "middle", true));
        }
        const Encoder encoder(config);
        const uint32_t samples = encoder.duration_samples(hi.size());
        const unsigned short_bits = static_cast<unsigned>(hi.size() * k_byte_bits % pc.bits);
        const std::string short_text =
            short_bits != 0 ? format(" (the last one %u bit%s)", short_bits, short_bits == 1 ? "" : "s") : "";
        svg.text(cell_x(1), top + k_cell_height + k_row_note_drop,
                 format("%u packages%s: %u bits + %u STOPs = %u slots; the whole transmission %u slots, %.0f ms with "
                        "hf timing",
                        static_cast<unsigned>(packages),
                        short_text.c_str(),
                        static_cast<unsigned>(hi.size() * k_byte_bits), static_cast<unsigned>(packages),
                        pc.data_and_stops, static_cast<unsigned>(real_slots(r)), samples * k_ms_per_s / k_rate_hz),
                 font(k_note_size, k_muted));
    }
    std::vector<std::string> lines;
    lines.push_back("A byte may span packages (N = 3: package 2 carries the last two bits of 'H' and the first of "
                    "'i'); the receiver puts the bits back by package index.");
    lines.push_back("The last package holds only the bits left, never padding; its STOP and the 2 END markers are 3 "
                    "markers in a row, which only an end can give.");
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "packing.svg"), "Unlimited packing",
             "The bytes 0x48 0x69 as one bit stream cut into packages of 8, 4 and 3 bits between START and STOP "
             "markers, computed with the Encoder.");
    std::printf("packing.svg: Hi with N = 8, 4, 3 matches spec 2.2\n");
}

// ---------------------------------------------------------------------------
// Decoding helpers
// ---------------------------------------------------------------------------

struct DecodeResult {
    std::string text;
    size_t bytes_sent = 0;
    size_t bytes_correct = 0;
    size_t bit_errors = 0;
    bool locked = false;
    double tone_hz = 0.0;
    double slot_ms = 0.0;
    unsigned bits_per_package = 0;
    double snr_db = 0.0;
};

DecodeResult decode(const lb::Recording& recording, const lb::Capture& capture) {
    const lb::Mapping mapping = lb::map_events(recording, capture);
    DecodeResult result;
    const std::vector<uint8_t>& sent = recording.transmissions[0].data;
    result.bytes_sent = sent.size();
    for (size_t k = 0; k < sent.size(); ++k) {
        if (mapping.received[0][k] == sent[k]) ++result.bytes_correct;
    }
    result.text = printable_text(mapping.received[0]);
    result.bit_errors = mapping.score.bit_errors;
    for (size_t i = 0; i < capture.events.size(); ++i) {
        const Event& e = capture.events[i];
        if (e.type == EventType::locked && !result.locked) {
            result.locked = true;
            result.tone_hz = e.tone_hz;
            result.slot_ms = e.slot_ms;
            result.bits_per_package = e.bits_per_package;
        }
        if (e.type == EventType::byte) result.snr_db = e.snr_db;
    }
    return result;
}

// A package as the Decoder saw it: the slot events of its data slots and its package event (spec 3.10, 5.1).
struct PackageView {
    uint32_t index = 0;
    std::vector<uint8_t> level_pct;
    std::vector<uint8_t> threshold_pct;
    std::vector<int> bits;
    std::vector<int> sent;
    uint8_t start_pct = 0;
    uint8_t stop_pct = 0;
    uint8_t flags = 0;
    double slot_ms = 0.0;
    double snr_db = 0.0;
    size_t errors() const {
        size_t count = 0;
        for (size_t i = 0; i < bits.size(); ++i) count += bits[i] != sent[i] ? 1u : 0u;
        return count;
    }
    double reference(size_t slot) const {  // the reference line at data slot 1..d, % of the running reference
        const double d = static_cast<double>(bits.size());
        const double start = start_pct;
        const double stop = stop_pct;
        return start + (stop - start) * static_cast<double>(slot) / (d + 1.0);
    }
};

// Every package of the first lock of a capture, with the bits that were sent in it.
std::vector<PackageView> package_views(const lb::Capture& capture, const std::vector<uint8_t>& data, unsigned bits) {
    std::vector<PackageView> views;
    PackageView current;
    bool locked_elsewhere = false;
    for (size_t i = 0; i < capture.events.size() && !locked_elsewhere; ++i) {
        const Event& e = capture.events[i];
        if (e.type == EventType::lost || e.type == EventType::end) locked_elsewhere = !views.empty();
        if (e.type == EventType::slot) {
            if (!current.bits.empty() && current.index != e.package_index) current = PackageView();
            current.index = e.package_index;
            current.level_pct.push_back(e.level_pct);
            current.threshold_pct.push_back(e.threshold_pct);
            current.bits.push_back(e.value);
            const size_t stream_bit = static_cast<size_t>(e.package_index) * bits + e.slot - 1u;
            const size_t byte = stream_bit / k_byte_bits;
            current.sent.push_back(byte < data.size() ? (data[byte] >> (k_msb - stream_bit % k_byte_bits)) & 1 : -1);
        }
        if (e.type == EventType::package) {
            if (current.bits.size() == e.value && current.index == e.package_index) {
                current.start_pct = e.start_pct;
                current.stop_pct = e.stop_pct;
                current.flags = e.flags;
                current.slot_ms = e.slot_ms;
                current.snr_db = e.snr_db;
                views.push_back(current);
            }
            current = PackageView();
        }
    }
    return views;
}

// ---------------------------------------------------------------------------
// Figure: the receiver's decision (spec 3.10)
// ---------------------------------------------------------------------------

struct DecisionCase {
    std::string title;
    std::string condition;
    PackageView view;
};

void draw_package(Svg& svg, const Box& tile, const DecisionCase& c, double top_pct) {
    const PackageView& v = c.view;
    const double k_title_drop = 20.0;
    const double k_condition_drop = 36.0;
    const double k_plot_inset_left = 46.0;
    const double k_plot_inset_right = 14.0;
    const double k_plot_top = 62.0;
    const double k_plot_height = 168.0;
    const double k_bar_fraction = 0.62;
    const double k_slot_label_drop = 14.0;
    const double k_bit_drop = 31.0;
    const double k_byte_drop = 49.0;
    const double k_info_rise = 12.0;
    const double k_pct_step = 50.0;
    const double k_marker_radius = 3.0;
    svg.rect(tile.left, tile.top, tile.width, tile.height, k_card, k_card_border, k_line, k_tile_radius);
    svg.text(tile.left + k_tile_text_left, tile.top + k_title_drop, c.title, font(k_label_size, k_ink, "start", true));
    svg.text(tile.left + k_tile_text_left, tile.top + k_condition_drop, c.condition, font(k_small_size, k_muted));
    const Box plot = {tile.left + k_plot_inset_left, tile.top + k_plot_top,
                      tile.width - k_plot_inset_left - k_plot_inset_right, k_plot_height};
    const Scale y = {0.0, top_pct, plot.bottom(), plot.top};
    y_axis(svg, y, plot.left, 0.0, top_pct, k_pct_step, 0, plot.right());
    const double k_axis_title_x = 36.0;
    svg.text(plot.left - k_axis_title_x, (plot.top + plot.bottom()) / 2.0, "level (% of the marker average)",
             font(k_small_size - 1.0, k_muted, "middle"), -90.0);
    const size_t d = v.bits.size();
    const double slot_px = plot.width / (d + 2.0);
    const double bar_w = slot_px * k_bar_fraction;
    const auto centre = [&](size_t i) { return plot.left + (i + 0.5) * slot_px; };
    // Bars: START, the data slots, STOP.
    for (size_t i = 0; i <= d + 1; ++i) {
        const bool marker = i == 0 || i == d + 1;
        const double level = marker ? (i == 0 ? v.start_pct : v.stop_pct)
                                    : v.level_pct[i - 1] / k_percent * v.reference(i);
        const char* fill = marker ? k_purple : (v.bits[i - 1] != 0 ? k_teal : k_gray_fill);
        const char* stroke = marker ? k_purple : (v.bits[i - 1] != 0 ? k_teal : k_axis);
        const double top = y(std::min(level, top_pct));
        svg.rect(centre(i) - bar_w / 2.0, top, bar_w, plot.bottom() - top, fill, stroke, k_thin);
        const std::string label = i == 0 ? "START" : (i == d + 1 ? "STOP" : format("%u", static_cast<unsigned>(i)));
        svg.text(centre(i), plot.bottom() + k_slot_label_drop, label,
                 font(k_small_size - 1.0, marker ? k_purple : k_muted, "middle", marker));
        if (marker) continue;
        const bool one = v.bits[i - 1] != 0;
        const bool wrong = v.bits[i - 1] != v.sent[i - 1];
        svg.text(centre(i), plot.bottom() + k_bit_drop, one ? "1" : "0",
                 font(k_label_size, wrong ? k_coral : (one ? k_teal_dark : k_ink), "middle", true));
    }
    // The reference line from the START crest to the STOP crest, and the decision line under it.
    svg.line(centre(0), y(v.start_pct), centre(d + 1), y(v.stop_pct), k_ink, k_medium, k_dash);
    std::vector<Point> decision;
    const double first_ratio = v.threshold_pct.front() / k_percent;
    const double last_ratio = v.threshold_pct.back() / k_percent;
    const Point start = {centre(0), y(first_ratio * v.start_pct)};
    decision.push_back(start);
    for (size_t i = 1; i <= d; ++i) {
        const Point p = {centre(i), y(v.threshold_pct[i - 1] / k_percent * v.reference(i))};
        decision.push_back(p);
    }
    const Point stop = {centre(d + 1), y(last_ratio * v.stop_pct)};
    decision.push_back(stop);
    svg.polyline(decision, k_amber, k_bold);
    svg.circle(centre(0), y(v.start_pct), k_marker_radius, k_ink);
    svg.circle(centre(d + 1), y(v.stop_pct), k_marker_radius, k_ink);

    unsigned value = 0;
    std::string sent;
    for (size_t i = 0; i < d; ++i) {
        value = (value << 1) | static_cast<unsigned>(v.bits[i]);
        sent += v.sent[i] != 0 ? '1' : '0';
    }
    const size_t errors = v.errors();
    const std::string byte_text =
        d == k_byte_bits ? format(" = 0x%02X %s", value, char_of(static_cast<uint8_t>(value)).c_str()) : "";
    svg.text((centre(1) + centre(d)) / 2.0, plot.bottom() + k_byte_drop,
             errors == 0 ? format("decided%s, as sent", byte_text.c_str())
                         : format("decided%s; sent %s (%u errors)", byte_text.c_str(), sent.c_str(),
                                  static_cast<unsigned>(errors)),
             font(k_note_size, errors == 0 ? k_teal_dark : k_coral, "middle", true));
    const unsigned line_low = *std::min_element(v.threshold_pct.begin(), v.threshold_pct.end());
    const unsigned line_high = *std::max_element(v.threshold_pct.begin(), v.threshold_pct.end());
    const std::string line_text =
        line_low == line_high ? format("%u %%", line_low) : format("%u–%u %%", line_low, line_high);
    svg.text(tile.left + k_tile_text_left, tile.bottom() - k_info_rise,
             format("START %u %%, STOP %u %%; the line at %s of the reference; T %.2f ms",
                    static_cast<unsigned>(v.start_pct), static_cast<unsigned>(v.stop_pct), line_text.c_str(),
                    v.slot_ms),
             font(k_small_size, k_muted));
}

// "Hi" through AWGN: package 0; a longer text through CCIR poor: the package with the steepest reference line that
// was decided right with both markers detected.
std::vector<DecisionCase> decision_cases() {
    const double k_awgn_snr_db = 3.0;
    const double k_awgn_offset_hz = 37.0;
    const uint32_t k_awgn_seed = 11;
    const double k_fading_snr_db = 20.0;
    const uint32_t k_fading_seed = 20260926;
    const char* const k_fading_text = "CQ CQ DE PY2 PY2 TEST UNLIMITED V0.3 FADING PATH 0123456789";
    const double k_silence_ms = 300.0;
    const EncoderConfig config = preset_config(Preset::hf);
    std::vector<DecisionCase> cases;

    DecisionCase steady;
    {
        const std::vector<uint8_t> data = text_bytes(k_hi_text);
        const lb::Recording recording = lb::single(data, config, k_silence_ms);
        sim::ChannelConfig channel;
        channel.mode = sim::Mode::usb;
        channel.snr_db = k_awgn_snr_db;
        channel.freq_offset_hz = k_awgn_offset_hz;
        channel.seed = k_awgn_seed;
        const std::vector<int16_t> received = through_channel(recording.samples, channel, config.amplitude, 1.0);
        const lb::Capture capture = lb::run_decoder(received, DecoderConfig(), k_decoder_chunk);
        const std::vector<PackageView> views = package_views(capture, data, config.bits_per_package);
        if (views.empty()) throw std::runtime_error("receiver_decision: no package decoded in AWGN");
        steady.view = views[0];
        steady.title = format("Steady signal: package %u of “%s”", steady.view.index, k_hi_text);
        steady.condition = format("hf, AWGN at SNR %s dB (key-down, 2500 Hz), USB tuned %s Hz",
                                  signed_number(k_awgn_snr_db, 0).c_str(), signed_number(k_awgn_offset_hz, 0).c_str());
    }
    cases.push_back(steady);

    DecisionCase fading;
    {
        const std::vector<uint8_t> data = text_bytes(k_fading_text);
        const lb::Recording recording = lb::single(data, config, k_silence_ms);
        sim::ChannelConfig channel;
        channel.mode = sim::Mode::usb;
        channel.snr_db = k_fading_snr_db;
        channel.seed = k_fading_seed;
        sim::apply_preset(channel, sim::FadingPreset::ccir_poor);
        const std::vector<int16_t> received =
            through_channel(recording.samples, channel, config.amplitude, k_fading_peak);
        const lb::Capture capture = lb::run_decoder(received, DecoderConfig(), k_decoder_chunk);
        const std::vector<PackageView> views = package_views(capture, data, config.bits_per_package);
        const uint8_t flywheel = unlimited::event_flag_flywheel_start | unlimited::event_flag_flywheel_stop;
        const size_t k_min_each = 3;  // ones and zeros: both kinds of slot against the line
        double steepest = -1.0;
        for (size_t i = 0; i < views.size(); ++i) {
            const PackageView& v = views[i];
            const size_t ones = static_cast<size_t>(std::count(v.bits.begin(), v.bits.end(), 1));
            if (v.errors() != 0 || (v.flags & flywheel) != 0 || v.start_pct == 0 || v.stop_pct == 0) continue;
            if (ones < k_min_each || v.bits.size() - ones < k_min_each) continue;
            const double slope = std::fabs(std::log(static_cast<double>(v.stop_pct) / v.start_pct));
            if (slope <= steepest) continue;
            steepest = slope;
            fading.view = v;
        }
        if (steepest < 0.0) throw std::runtime_error("receiver_decision: no clean package in fading");
        fading.title = format("Fading: package %u of a %u-byte text", fading.view.index,
                              static_cast<unsigned>(data.size()));
        fading.condition = format("hf, CCIR poor (%.0f ms, %.0f Hz Doppler), SNR %.0f dB; the steepest reference line",
                                  channel.path_delay_ms, channel.doppler_spread_hz, k_fading_snr_db);
    }
    cases.push_back(fading);
    return cases;
}

void figure_receiver_decision(const std::string& directory) {
    const std::vector<DecisionCase> cases = decision_cases();
    const double k_width = 820.0;
    const double k_height = 480.0;
    const double k_tile_top = 90.0;
    const double k_tile_height = 310.0;
    const double k_tile_gap = 12.0;
    const double k_pct_step = 50.0;
    const double k_headroom_pct = 10.0;
    const double k_min_top_pct = 150.0;
    const double k_legend_y = 422.0;
    double highest = 0.0;
    for (size_t c = 0; c < cases.size(); ++c) {
        const PackageView& v = cases[c].view;
        highest = std::max(highest, static_cast<double>(std::max(v.start_pct, v.stop_pct)));
        for (size_t i = 0; i < v.bits.size(); ++i)
            highest = std::max(highest, v.level_pct[i] / k_percent * v.reference(i + 1));
    }
    const double top_pct = std::max(k_min_top_pct, std::ceil((highest + k_headroom_pct) / k_pct_step) * k_pct_step);
    Svg svg(k_width, k_height);
    heading(svg, "How the receiver reads a package: the reference line and the decision line",
            "Real Decoder slot and package events (ssb profile, smart line); the levels are the Decoder's slot "
            "amplitudes, in % of its running marker average.");
    const double tile_width = (k_width - 2.0 * k_margin - k_tile_gap) / 2.0;
    for (size_t c = 0; c < cases.size(); ++c) {
        const Box tile = {k_margin + c * (tile_width + k_tile_gap), k_tile_top, tile_width, k_tile_height};
        draw_package(svg, tile, cases[c], top_pct);
        const PackageView& v = cases[c].view;
        std::printf("receiver_decision.svg: %-8s package %u, START %u%%, STOP %u%%, line %u..%u%%, errors %u, "
                    "T %.2f ms\n",
                    c == 0 ? "steady" : "fading", v.index, static_cast<unsigned>(v.start_pct),
                    static_cast<unsigned>(v.stop_pct),
                    static_cast<unsigned>(*std::min_element(v.threshold_pct.begin(), v.threshold_pct.end())),
                    static_cast<unsigned>(*std::max_element(v.threshold_pct.begin(), v.threshold_pct.end())),
                    static_cast<unsigned>(v.errors()), v.slot_ms);
    }
    double lx = k_margin;
    lx = legend_item(svg, lx, k_legend_y, k_purple, "START / STOP crest");
    lx = legend_line(svg, lx, k_legend_y, k_ink, k_medium, k_dash, "reference line: how tall a 1 is");
    lx = legend_line(svg, lx, k_legend_y, k_amber, k_bold, "", "decision line");
    lx = legend_item(svg, lx, k_legend_y, k_teal, "decided 1", k_teal);
    legend_item(svg, lx, k_legend_y, k_gray_fill, "decided 0");
    std::vector<std::string> lines;
    lines.push_back("The markers have the height of a data 1, so the line from the START crest to the STOP crest says "
                    "how tall a 1 is at every slot, even in a fade.");
    lines.push_back("A slot above the decision line is a 1. The smart line sits at 50–75 % of the reference (about "
                    "70 % when weak, lower when clean), never below the noise.");
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "receiver_decision.svg"), "Unlimited receiver decision",
             "Two real packages as the Decoder measured them: slot levels as bars, the dashed reference line from the "
             "START to the STOP crest, the amber decision line and the decided bits, in AWGN and in CCIR poor fading.");
}

// ---------------------------------------------------------------------------
// Figure: bandwidth against typical receiver filters (spec 1.5)
// ---------------------------------------------------------------------------

struct FilterSpec {
    const char* name;
    uint16_t low_hz;
    uint16_t high_hz;
    bool standard;
};

const FilterSpec k_ssb_filters[] = {{"SSB 1.8 kHz", 300, 2100, false},
                                    {"SSB 2.4 kHz", unlimited::k_ssb_passband_low_hz,
                                     unlimited::k_ssb_passband_high_hz, true},
                                    {"SSB 2.7 kHz", 200, 2900, false},
                                    {"SSB 3.0 kHz", 100, 3000, false}};

struct SlotRow {
    uint32_t slot_us;
    const char* presets;
};

const SlotRow k_slot_rows[] = {{128000, ""},     {64000, ""},     {32000, "hf_slow"},
                               {16000, "hf"},    {8000, "hf_fast, am"}, {4000, "fm"}};

std::string room_text(const PassbandFit& fit) {
    if (!fit.fits) return "no fit";
    if (fit.margin_low_hz == fit.margin_high_hz) return format("±%d", fit.margin_low_hz);
    return number(-fit.margin_low_hz, 0) + " / +" + format("%d", fit.margin_high_hz);
}

void figure_bandwidth(const std::string& directory) {
    const uint16_t tone = unlimited::k_default_tone_hz;
    const size_t filters = sizeof(k_ssb_filters) / sizeof(k_ssb_filters[0]);
    const size_t rows = sizeof(k_slot_rows) / sizeof(k_slot_rows[0]);
    const double k_width = 820.0;
    const double k_height = 482.0;
    const double k_draw_left = 176.0;
    const double k_draw_right = 486.0;
    const double k_max_hz = 3000.0;
    const double k_table_left = 506.0;
    const double k_column = 72.5;
    const double k_filter_top = 100.0;
    const double k_filter_pitch = 19.0;
    const double k_bar_height = 11.0;
    const double k_header_y = 196.0;
    const double k_rows_top = 226.0;
    const double k_row_pitch = 26.0;
    const double k_row_bar_height = 12.0;
    const double k_axis_gap = 6.0;
    const uint16_t k_tight_room_hz = 100;
    const double k_header_line = 12.0;
    const double k_row_band_pad = 4.0;
    const double k_preset_label_gap = 6.0;
    const double k_legend_drop = 20.0;
    Svg svg(k_width, k_height);
    heading(svg, "Bandwidth: every slot length against typical SSB filters",
            format("occupied_band(%u Hz, T) against each filter; the table is passband_fit(config): how far the pitch "
                   "may move down / up and still be heard (Hz).",
                   static_cast<unsigned>(tone)));
    const Scale x = {0.0, k_max_hz, k_draw_left, k_draw_right};
    const double rows_bottom = k_rows_top + rows * k_row_pitch;
    svg.line(x(tone), k_filter_top - k_filter_pitch / 2.0, x(tone), rows_bottom - k_row_pitch / 2.0, k_gray, k_line,
             k_guide_dash);
    for (size_t f = 0; f < filters; ++f) {
        const FilterSpec& filter = k_ssb_filters[f];
        const double yc = k_filter_top + f * k_filter_pitch;
        svg.rect(x(filter.low_hz), yc - k_bar_height / 2.0, x(filter.high_hz) - x(filter.low_hz), k_bar_height,
                 filter.standard ? k_gray_mid : k_gray_fill, k_axis, k_thin, k_bar_radius);
        svg.text(k_margin, yc + k_text_rise,
                 format("%s %u–%u Hz%s", filter.name, static_cast<unsigned>(filter.low_hz),
                        static_cast<unsigned>(filter.high_hz), filter.standard ? " (default)" : ""),
                 font(k_small_size, k_ink, "start", filter.standard));
    }
    // Table header.
    svg.text(k_table_left, k_filter_top + k_text_rise,
             format("Shift tolerance at %u Hz (Hz):", static_cast<unsigned>(tone)),
             font(k_label_size, k_ink, "start", true));
    svg.text(k_table_left, k_filter_top + k_filter_pitch + k_text_rise, "down / up, inside the filter and the search",
             font(k_small_size, k_muted));
    for (size_t f = 0; f < filters; ++f) {
        const double cx = k_table_left + (f + 0.5) * k_column;
        svg.text(cx, k_header_y, k_ssb_filters[f].name, font(k_small_size, k_ink, "middle", true));
        svg.text(cx, k_header_y + k_header_line,
                 format("%u–%u", static_cast<unsigned>(k_ssb_filters[f].low_hz),
                        static_cast<unsigned>(k_ssb_filters[f].high_hz)),
                 font(k_small_size - 1.0, k_muted, "middle"));
    }
    svg.text(k_margin, k_header_y + k_header_line, "slot T, occupied band", font(k_small_size, k_muted, "start", true));
    for (size_t r = 0; r < rows; ++r) {
        const SlotRow& row = k_slot_rows[r];
        const double yc = k_rows_top + r * k_row_pitch;
        const Band band = unlimited::occupied_band(tone, row.slot_us);
        const uint16_t w26 = unlimited::width_26db_hz(row.slot_us);
        if (r % 2 == 0) {
            svg.rect(k_margin - k_row_band_pad, yc - k_row_pitch / 2.0, k_width - 2.0 * (k_margin - k_row_band_pad),
                     k_row_pitch, k_row_band);
        }
        svg.rect(x(tone - w26 / 2.0), yc - k_row_bar_height / 2.0, x(tone + w26 / 2.0) - x(tone - w26 / 2.0),
                 k_row_bar_height, k_teal_light);
        svg.rect(x(band.low_hz), yc - k_row_bar_height / 2.0, x(band.high_hz) - x(band.low_hz), k_row_bar_height,
                 k_teal);
        svg.text(k_margin, yc + k_text_rise, format("T = %.0f ms", row.slot_us / k_us_per_ms),
                 font(k_note_size, k_ink, "start", true));
        const double k_width_x = 104.0;
        svg.text(k_margin + k_width_x, yc + k_text_rise, format("%u Hz", static_cast<unsigned>(band.width_hz)),
                 font(k_note_size, k_teal_dark, "end", true));
        if (row.presets[0] != '\0') {
            svg.text(x(tone + w26 / 2.0) + k_preset_label_gap, yc + k_text_rise, row.presets,
                     font(k_small_size, k_muted));
        }
        for (size_t f = 0; f < filters; ++f) {
            EncoderConfig sender = preset_config(Preset::hf);
            sender.slot_us = row.slot_us;
            sender.passband.low_hz = k_ssb_filters[f].low_hz;
            sender.passband.high_hz = k_ssb_filters[f].high_hz;
            const PassbandFit fit = passband_fit(sender);
            const double cx = k_table_left + (f + 0.5) * k_column;
            const bool tight = fit.fits && fit.tolerance_hz < k_tight_room_hz;
            svg.text(cx, yc + k_text_rise, room_text(fit),
                     font(k_small_size, fit.fits ? (tight ? k_coral : k_ink) : k_coral, "middle", tight));
        }
    }
    const double axis_y = rows_bottom - k_row_pitch / 2.0 + k_axis_gap;
    const double k_tick_hz = 500.0;
    x_axis(svg, x, axis_y, 0.0, k_max_hz, k_tick_hz, 0, 1);
    svg.text((k_draw_left + k_draw_right) / 2.0, axis_y + k_axis_title_gap, "audio frequency (Hz)",
             font(k_small_size, k_muted, "middle"));
    const double legend_y = axis_y + k_axis_title_gap + k_legend_drop;
    double lx = k_margin;
    lx = legend_item(svg, lx, legend_y, k_teal,
                     format("occupied band: 99 %% of a slot's energy (%.1f/T), the fit rule",
                            unlimited::k_band_99_milli / k_milli),
                     k_teal);
    legend_item(svg, lx, legend_y, k_teal_light,
                format("−26 dB width (%.1f/T)", unlimited::k_band_26db_milli / k_milli), k_teal_light);
    std::vector<std::string> lines;
    EncoderConfig centred = preset_config(Preset::hf);
    centred.tone_hz = static_cast<uint16_t>((k_ssb_filters[0].low_hz + k_ssb_filters[0].high_hz) / 2);
    centred.passband.low_hz = k_ssb_filters[0].low_hz;
    centred.passband.high_hz = k_ssb_filters[0].high_hz;
    EncoderConfig centred_fm = preset_config(Preset::fm);
    centred_fm.tone_hz = centred.tone_hz;
    centred_fm.passband = centred.passband;
    lines.push_back(format("The receiver searches %u..%u Hz (from %u Hz below 8 ms): no filter leaves more room. Coral: "
                           "less than 100 Hz of room.",
                           static_cast<unsigned>(unlimited::k_min_tone_hz),
                           static_cast<unsigned>(unlimited::k_max_tone_hz),
                           static_cast<unsigned>(unlimited::k_min_fast_tone_hz)));
    lines.push_back(format("In the 1.8 kHz filter a %u Hz pitch centres the band: hf %s Hz, fm %s Hz; T < 8 ms is for "
                           "FM-like channels.",
                           static_cast<unsigned>(centred.tone_hz), room_text(passband_fit(centred)).c_str(),
                           room_text(passband_fit(centred_fm)).c_str()));
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "bandwidth_filters.svg"), "Unlimited bandwidth and filters",
             "Occupied band of slot lengths 4 to 128 ms at 1500 Hz against SSB filters of 1.8, 2.4, 2.7 and 3.0 kHz, "
             "with the shift tolerance each filter leaves, computed by occupied_band() and passband_fit().");
    std::printf("bandwidth_filters.svg: %u slot lengths x %u filters\n", static_cast<unsigned>(rows),
                static_cast<unsigned>(filters));
}

// ---------------------------------------------------------------------------
// Figure: channels
// ---------------------------------------------------------------------------

struct ChannelCase {
    std::string title;
    std::string condition;
    sim::ChannelConfig channel;
    Profile profile;
    const char* profile_name;
    double peak_factor;
};

std::vector<ChannelCase> channel_cases() {
    const double k_ssb_snr_db = 10.0;
    const double k_usb_shift_hz = 150.0;
    const double k_lsb_shift_hz = -150.0;
    const double k_am_snr_db = 10.0;
    const double k_fm_cnr_db = 10.0;
    const double k_fading_snr_db = 20.0;
    const uint32_t k_seed = 20260926;
    std::vector<ChannelCase> cases;
    ChannelCase c;
    c.channel = sim::ChannelConfig();
    c.channel.mode = sim::Mode::clean;
    c.channel.noise = false;
    c.title = "Clean";
    c.condition = "no noise, no filter (control)";
    c.profile = Profile::ssb;
    c.profile_name = "ssb";
    c.peak_factor = 1.0;
    cases.push_back(c);

    c.channel = sim::ChannelConfig();
    c.channel.mode = sim::Mode::usb;
    c.channel.snr_db = k_ssb_snr_db;
    c.channel.freq_offset_hz = k_usb_shift_hz;
    c.title = format("USB, tuned %s Hz", signed_number(k_usb_shift_hz, 0).c_str());
    c.condition = format("SNR %.0f dB, passband 300–2700 Hz", k_ssb_snr_db);
    cases.push_back(c);

    c.channel = sim::ChannelConfig();
    c.channel.mode = sim::Mode::lsb;
    c.channel.snr_db = k_ssb_snr_db;
    c.channel.freq_offset_hz = k_lsb_shift_hz;
    c.title = format("LSB (inverted), tuned %s Hz", signed_number(k_lsb_shift_hz, 0).c_str());
    c.condition = format("SNR %.0f dB, audio f → %.0f − f", k_ssb_snr_db, c.channel.lsb_pivot_hz + k_lsb_shift_hz);
    cases.push_back(c);

    c.channel = sim::ChannelConfig();
    c.channel.mode = sim::Mode::am;
    c.channel.snr_db = k_am_snr_db;
    c.title = "AM";
    c.condition = format("m %.1f, SNR %.0f dB (carrier)", c.channel.am_modulation_index, k_am_snr_db);
    c.profile = Profile::am;
    c.profile_name = "am";
    cases.push_back(c);

    c.channel = sim::ChannelConfig();
    c.channel.mode = sim::Mode::fm;
    c.channel.snr_db = k_fm_cnr_db + db_power(c.channel.fm_if_bandwidth_hz / k_reference_bandwidth_hz);
    c.title = "NBFM";
    c.condition = format("CNR %.0f dB, %.0f µs emphasis", k_fm_cnr_db, c.channel.fm_emphasis_us);
    c.profile = Profile::fm;
    c.profile_name = "fm";
    c.peak_factor = k_fm_peak;
    cases.push_back(c);

    c.channel = sim::ChannelConfig();
    c.channel.mode = sim::Mode::usb;
    c.channel.snr_db = k_fading_snr_db;
    sim::apply_preset(c.channel, sim::FadingPreset::ccir_poor);
    c.title = "CCIR poor fading";
    c.condition = format("%.0f ms / %.0f Hz, SNR %.0f dB (USB)", c.channel.path_delay_ms, c.channel.doppler_spread_hz,
                         k_fading_snr_db);
    c.profile = Profile::ssb;
    c.profile_name = "ssb";
    c.peak_factor = k_fading_peak;
    cases.push_back(c);
    for (size_t i = 0; i < cases.size(); ++i) cases[i].channel.seed = k_seed + static_cast<uint32_t>(i);
    return cases;
}

void figure_channels(const std::string& directory) {
    const EncoderConfig config = preset_config(Preset::hf);
    const double k_silence_ms = 300.0;
    lb::Recording recording;
    lb::append_silence(recording, k_silence_ms);
    lb::append_transmission(recording, text_bytes(k_hi_text), config);
    lb::append_silence(recording, k_silence_ms);
    const double rate = k_rate_hz;
    const double duration = recording.samples.size() / rate;
    const std::vector<ChannelCase> cases = channel_cases();
    // Run 0 of each channel is drawn; all runs (other seeds of the same channel) give the success rate.
    const size_t k_runs = 20;
    const uint32_t k_run_seed_step = 1000;
    const std::vector<DecodeResult> results = parallel_map<DecodeResult>(cases.size() * k_runs, [&](size_t job) {
        const ChannelCase& c = cases[job / k_runs];
        sim::ChannelConfig channel = c.channel;
        channel.seed += static_cast<uint32_t>(job % k_runs) * k_run_seed_step;
        const std::vector<int16_t> received =
            through_channel(recording.samples, channel, config.amplitude, c.peak_factor);
        return decode(recording, lb::run_decoder(received, DecoderConfig::for_profile(c.profile), k_decoder_chunk));
    });

    const double k_width = 820.0;
    const double k_height = 648.0;
    const size_t k_columns = 3;
    const double k_tile_left = 18.0;
    const double k_tile_top = 82.0;
    const double k_tile_width = 256.0;
    const double k_tile_height = 252.0;
    const double k_tile_gap = 10.0;
    const double k_image_inset = 36.0;
    const double k_image_top = 46.0;
    const double k_image_width = 210.0;
    const double k_image_height = 124.0;
    const size_t k_window = 128;
    const size_t k_hop = 32;
    const size_t k_fft = 256;
    const double k_floor_db = -40.0;
    const double k_text_left = 10.0;
    const double k_title_drop = 18.0;
    const double k_condition_drop = 34.0;
    const double k_result_drop = 20.0;
    const double k_detail_gap = 14.0;
    Svg svg(k_width, k_height);
    heading(svg, format("The hf transmission “%s” through six simulated channels", k_hi_text),
            format("sim::Channel output (%.0f–%.0f Hz, %.1f s each), decoded by the real Decoder with the profile "
                   "shown.",
                   k_low_hz, k_high_hz, duration),
            format("The receiver is told only its profile. Last line of each tile: %u runs of that channel with other "
                   "noise and fading seeds.",
                   static_cast<unsigned>(k_runs)));
    for (size_t i = 0; i < cases.size(); ++i) {
        const ChannelCase& c = cases[i];
        const DecodeResult& result = results[i * k_runs];
        size_t exact = 0;
        for (size_t run = 0; run < k_runs; ++run) {
            const DecodeResult& r = results[i * k_runs + run];
            exact += r.bytes_correct == r.bytes_sent ? 1u : 0u;
        }
        const std::vector<int16_t> received =
            through_channel(recording.samples, c.channel, config.amplitude, c.peak_factor);
        const Spectrogram s =
            spectrogram(to_double(received, k_full_scale), rate, k_window, k_hop, k_fft, k_low_hz, k_high_hz);

        const double x0 = k_tile_left + (i % k_columns) * (k_tile_width + k_tile_gap);
        const double y0 = k_tile_top + (i / k_columns) * (k_tile_height + k_tile_gap);
        svg.rect(x0, y0, k_tile_width, k_tile_height, k_card, k_card_border, k_line, k_tile_radius);
        svg.text(x0 + k_text_left, y0 + k_title_drop, c.title, font(k_label_size, k_ink, "start", true));
        svg.text(x0 + k_text_left, y0 + k_condition_drop, c.condition, font(k_small_size, k_muted));
        const Box plot = {x0 + k_image_inset, y0 + k_image_top, k_image_width, k_image_height};
        const Scale t = {0.0, duration, plot.left, plot.right()};
        const Scale f = {k_low_hz, k_high_hz, plot.bottom(), plot.top};
        const std::string clip_id = svg.clip(plot.left, plot.top, plot.width, plot.height);
        draw_spectrogram(svg, s, t, f, max_power(s), k_floor_db, clip_id);
        svg.rect(plot.left, plot.top, plot.width, plot.height, "none", k_axis, k_thin);
        const double k_label_hz[] = {500.0, 1500.0, 2500.0};
        const double k_label_gap = 5.0;
        const double k_label_tick = 3.0;
        const double k_small_text_rise = 3.5;
        for (size_t l = 0; l < sizeof(k_label_hz) / sizeof(k_label_hz[0]); ++l) {
            svg.line(plot.left - k_label_tick, f(k_label_hz[l]), plot.left, f(k_label_hz[l]), k_axis, k_line);
            svg.text(plot.left - k_label_gap, f(k_label_hz[l]) + k_small_text_rise, format("%.0f", k_label_hz[l]),
                     font(k_small_size - 1.0, k_muted, "end"));
        }
        const bool ok = result.bytes_correct == result.bytes_sent;
        const bool nothing = result.text.find_first_not_of('_') == std::string::npos;
        const double text_y = plot.bottom() + k_result_drop;
        svg.text(x0 + k_text_left, text_y,
                 ok ? format("“%s” decoded OK", result.text.c_str())
                    : (nothing ? std::string("no lock in this run: nothing released")
                               : format("“%s” decoded with losses", result.text.c_str())),
                 font(k_note_size, ok ? k_teal_dark : k_coral, "start", true));
        svg.text(x0 + k_text_left, text_y + k_line_gap,
                 format("%u/%u bytes, %u bit errors; %s profile", static_cast<unsigned>(result.bytes_correct),
                        static_cast<unsigned>(result.bytes_sent), static_cast<unsigned>(result.bit_errors),
                        c.profile_name),
                 font(k_small_size, k_ink));
        svg.text(x0 + k_text_left, text_y + k_line_gap + k_detail_gap,
                 result.locked ? format("locked: %.0f Hz, T %.1f ms, N %u; SNR %.1f dB", result.tone_hz,
                                        result.slot_ms, result.bits_per_package, result.snr_db)
                               : std::string("no lock"),
                 font(k_small_size, k_muted));
        svg.text(x0 + k_text_left, text_y + k_line_gap + 2.0 * k_detail_gap,
                 format("%u of %u runs decoded exactly", static_cast<unsigned>(exact), static_cast<unsigned>(k_runs)),
                 font(k_small_size, exact == k_runs ? k_teal_dark : k_coral, "start", true));
        std::printf("channels.svg: %-28s run 0 \"%s\" %u/%u bytes, %u bit errors, locked %.1f Hz T %.2f ms N %u, SNR "
                    "%.1f dB; %u/%u runs exact\n",
                    c.title.c_str(), result.text.c_str(), static_cast<unsigned>(result.bytes_correct),
                    static_cast<unsigned>(result.bytes_sent), static_cast<unsigned>(result.bit_errors), result.tone_hz,
                    result.slot_ms, result.bits_per_package, result.snr_db, static_cast<unsigned>(exact),
                    static_cast<unsigned>(k_runs));
    }
    std::vector<std::string> lines;
    lines.push_back("SNR: key-down tone power over noise in 2500 Hz (AM: carrier power; FM: CNR in 12.5 kHz). LSB "
                    "inverts the audio: the twist survives it.");
    lines.push_back("The SNR in each tile is the Decoder's own estimate. Missing bytes are never replaced by guesses "
                    "(spec 3.13).");
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "channels.svg"), "Unlimited over six channels",
             "Spectrograms of the hf transmission of Hi after clean, USB shifted, LSB inverted, AM, FM and CCIR poor "
             "channels, each with the real decoder's result and its success rate over 20 seeds.");
}

// ---------------------------------------------------------------------------
// Figure: whole-band vs narrow-band envelope at 0 dB
// ---------------------------------------------------------------------------

// RMS over a sliding boxcar of `window` samples, centred.
std::vector<double> rms_envelope(const std::vector<float>& x, size_t window) {
    std::vector<double> out(x.size(), 0.0);
    const long half = static_cast<long>(window / 2);
    double sum = 0.0;
    std::vector<double> prefix(x.size() + 1, 0.0);
    for (size_t i = 0; i < x.size(); ++i) {
        sum += square(x[i]);
        prefix[i + 1] = sum;
    }
    for (size_t i = 0; i < x.size(); ++i) {
        const long a = std::max(0L, static_cast<long>(i) - half);
        const long b = std::min(static_cast<long>(x.size()), a + static_cast<long>(window));
        out[i] = std::sqrt((prefix[static_cast<size_t>(b)] - prefix[static_cast<size_t>(a)]) / window);
    }
    return out;
}

// Magnitude of the boxcar mean of x e^{-j w n}, times 2 (a sine of amplitude A reads A).
std::vector<double> narrow_envelope(const std::vector<float>& x, size_t window, double hz, double rate) {
    const double omega = 2.0 * k_pi * hz / rate;
    std::vector<Complex> prefix(x.size() + 1, Complex(0.0, 0.0));
    for (size_t n = 0; n < x.size(); ++n) {
        prefix[n + 1] = prefix[n] + static_cast<double>(x[n]) * std::polar(1.0, -omega * n);
    }
    std::vector<double> out(x.size(), 0.0);
    const long half = static_cast<long>(window / 2);
    for (size_t i = 0; i < x.size(); ++i) {
        const long a = std::max(0L, static_cast<long>(i) - half);
        const long b = std::min(static_cast<long>(x.size()), a + static_cast<long>(window));
        out[i] = 2.0 * std::abs(prefix[static_cast<size_t>(b)] - prefix[static_cast<size_t>(a)]) / window;
    }
    return out;
}

void figure_envelopes(const std::string& directory, const Rendered& r) {
    const EncoderConfig& config = r.config;
    const double rate = config.sample_rate_hz;
    const double k_snr_db = 0.0;
    const double k_before_ms = 150.0;
    const uint32_t k_seed = 7;
    const size_t lead = static_cast<size_t>(k_before_ms * rate / k_ms_per_s);
    std::vector<int16_t> padded(lead, 0);
    padded.insert(padded.end(), r.audio.begin(), r.audio.end());

    sim::ChannelConfig channel;
    channel.mode = sim::Mode::usb;
    channel.snr_db = k_snr_db;
    channel.seed = k_seed;
    channel.signal_level = config.amplitude / k_full_scale;
    const double delay = lb::channel_delay_samples(sim::Mode::usb);
    std::vector<float> in(padded.size());
    for (size_t i = 0; i < padded.size(); ++i) in[i] = static_cast<float>(padded[i] / k_full_scale);
    sim::Channel noisy_channel(channel);
    const std::vector<float> noisy = noisy_channel.process(in);
    channel.noise = false;
    sim::Channel clean_channel(channel);
    const std::vector<float> clean = clean_channel.process(in);

    // The Decoder's slot window: the central 0.75 T of a slot (spec 3.10), a boxcar of rate / window Hz of noise.
    const double k_slot_window = 0.75;
    const size_t window = static_cast<size_t>(std::lround(k_slot_window * slot_s_of(config) * rate));
    const double narrow_hz = rate / window;
    const double level = channel.signal_level;
    std::vector<double> wide_noisy = rms_envelope(noisy, window);
    std::vector<double> wide_clean = rms_envelope(clean, window);
    std::vector<double> narrow_noisy = narrow_envelope(noisy, window, config.tone_hz, rate);
    std::vector<double> narrow_clean = narrow_envelope(clean, window, config.tone_hz, rate);
    const double rms_to_peak = std::sqrt(2.0);
    for (size_t i = 0; i < wide_noisy.size(); ++i) {
        wide_noisy[i] *= rms_to_peak / level;
        wide_clean[i] *= rms_to_peak / level;
        narrow_noisy[i] /= level;
        narrow_clean[i] /= level;
    }

    const std::vector<Span> list = spans(r);
    const double t0 = -k_before_ms;
    const double t1 = r.audio.size() * k_ms_per_s / rate;

    const double k_width = 820.0;
    const double k_height = 500.0;
    const double k_left = 70.0;
    const double k_right = 796.0;
    const double k_top_panel = 120.0;
    const double k_panel_height = 118.0;
    const double k_panel_gap = 48.0;
    const double k_y_max = 1.85;
    const double k_y_step = 0.5;
    const double k_y_label_max = 1.5;
    const double k_time_step_ms = 100.0;
    const double k_panel_title_gap = 10.0;
    Svg svg(k_width, k_height);
    heading(svg, format("Why the receiver listens in a narrow band around the pitch: %.0f dB SNR", k_snr_db),
            format("hf transmission of “%s” + AWGN at %.0f dB (key-down tone in 2500 Hz), USB receiver %.0f–%.0f Hz, "
                   "%.0f ms of silence before it.",
                   k_hi_text, k_snr_db, k_low_hz, k_high_hz, k_before_ms),
            format("Both envelopes average over the Decoder's slot window, %.2f T = %.0f ms (%.0f Hz of noise "
                   "bandwidth at the pitch); 1.0 = the crest A.",
                   k_slot_window, window * k_ms_per_s / rate, narrow_hz));
    const Scale x = {t0, t1, k_left, k_right};
    const size_t k_decimation = 6;  // about 1.3 points per pixel
    const struct Panel {
        std::string title;
        const std::vector<double>* noisy;
        const std::vector<double>* clean;
        double top;
    } panels[] = {{format("whole passband (%.0f Hz): RMS envelope", k_receiver_bandwidth_hz), &wide_noisy, &wide_clean,
                   k_top_panel},
                  {format("narrow band (%.0f Hz) at the pitch, %u Hz: magnitude", narrow_hz,
                          static_cast<unsigned>(config.tone_hz)),
                   &narrow_noisy, &narrow_clean, k_top_panel + k_panel_height + k_panel_gap}};
    for (size_t p = 0; p < 2; ++p) {
        const Panel& panel = panels[p];
        const Scale y = {0.0, k_y_max, panel.top + k_panel_height, panel.top};
        svg.text(k_left, panel.top - k_panel_title_gap, panel.title, font(k_label_size, k_ink, "start", true));
        y_axis(svg, y, k_left, 0.0, k_y_label_max, k_y_step, 1, k_right);
        for (size_t i = 0; i < list.size(); ++i) {
            const double t = list[i].first * k_ms_per_s / rate;
            svg.line(x(t), panel.top, x(t), panel.top + k_panel_height, k_gray, k_thin, k_guide_dash);
        }
        std::vector<Point> noisy_points;
        std::vector<Point> clean_points;
        // Received sample n carries transmitted sample n - lead - delay.
        for (size_t n = 0; n < panel.noisy->size(); n += k_decimation) {
            const double t = (static_cast<double>(n) - static_cast<double>(lead) - delay) * k_ms_per_s / rate;
            if (t < t0 || t > t1) continue;
            const Point a = {x(t), y(std::min(k_y_max, (*panel.noisy)[n]))};
            const Point b = {x(t), y(std::min(k_y_max, (*panel.clean)[n]))};
            noisy_points.push_back(a);
            clean_points.push_back(b);
        }
        svg.polyline(noisy_points, k_coral, k_trace);
        svg.polyline(clean_points, k_ink, k_trace, k_dash);
    }
    // Segment names inside the first panel, above the traces.
    const double k_names_drop = 12.0;
    const double names_y = k_top_panel + k_names_drop;
    const double k_min_name_ms = 30.0;
    for (size_t i = 0; i < list.size(); ++i) {
        const double a = std::max(t0, list[i].first * k_ms_per_s / rate);
        const double b = std::min(t1, (list[i].first + list[i].samples) * k_ms_per_s / rate);
        if (b - a < k_min_name_ms) continue;
        svg.text(x((a + b) / 2.0), names_y, span_name(list[i], true), font(k_small_size, k_muted, "middle", true));
    }
    svg.text(x(t0 + k_before_ms / 2.0), names_y, "silence", font(k_small_size, k_muted, "middle", true));
    const double legend_y = k_top_panel - k_panel_title_gap;
    const double k_legend_right_first = 214.0;
    double lx = k_right - k_legend_right_first;
    lx = legend_line(svg, lx, legend_y, k_coral, k_bold, "", format("with noise (%.0f dB)", k_snr_db));
    legend_line(svg, lx, legend_y, k_ink, k_medium, k_dash, "clean");
    const double bottom = k_top_panel + 2.0 * k_panel_height + k_panel_gap;
    x_axis(svg, x, bottom, std::ceil(t0 / k_time_step_ms) * k_time_step_ms,
           std::floor(t1 / k_time_step_ms) * k_time_step_ms, k_time_step_ms, 0);
    svg.text((k_left + k_right) / 2.0, bottom + k_axis_title_gap, "time (ms), 0 = start of the tune",
             font(k_small_size, k_muted, "middle"));
    std::vector<std::string> lines;
    lines.push_back(format("In %.0f Hz the noise is as strong as the tone; in the %.0f Hz slot window it is %.1f dB "
                           "weaker (%.0f/%.0f).",
                           k_receiver_bandwidth_hz, narrow_hz, db_power(k_receiver_bandwidth_hz / narrow_hz),
                           k_receiver_bandwidth_hz, narrow_hz));
    lines.push_back(format("The receiver first finds the pitch (the tune tone), then mixes it to 0 Hz and measures "
                           "every slot over %.2f T: never one wideband envelope.",
                           k_slot_window));
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "envelope_wide_vs_narrow.svg"), "Unlimited envelopes at 0 dB SNR",
             "Whole-band RMS envelope against the narrow-band magnitude at the pitch over the Decoder's slot window, "
             "with and without noise at 0 dB SNR.");
    std::printf("envelope_wide_vs_narrow.svg: SNR %.0f dB, window %u samples (%.1f Hz), channel delay %.1f samples\n",
                k_snr_db, static_cast<unsigned>(window), narrow_hz, delay);
}

// ---------------------------------------------------------------------------
// Figure: measured spectrum of each preset
// ---------------------------------------------------------------------------

struct PresetStyle {
    Preset preset;
    const char* name;
    const char* color;
};

const PresetStyle k_presets[] = {{Preset::hf_slow, "hf_slow", k_blue},
                                 {Preset::hf, "hf", k_teal},
                                 {Preset::hf_fast, "hf_fast", k_coral},
                                 {Preset::am, "am", k_purple},
                                 {Preset::fm, "fm", k_pink}};

void figure_presets_spectrum(const std::string& directory) {
    const size_t k_bytes = 256;
    const uint32_t k_seed = 1;
    const size_t k_fft = 1024;
    const double k_floor_db = -60.0;
    const double k_obw_fraction = 0.99;
    const double k_max_hz = 3200.0;
    const double rate = k_rate_hz;
    const size_t count = sizeof(k_presets) / sizeof(k_presets[0]);

    const double k_width = 820.0;
    const double k_height = 604.0;
    const double k_left = 170.0;
    const double k_right = 792.0;
    const double k_top = 106.0;
    const double k_row_height = 66.0;
    const double k_row_gap = 24.0;
    const double k_label_line = 14.0;
    const double k_row_label_rise = 6.0;
    Svg svg(k_width, k_height);
    heading(svg, "Measured spectrum of each preset against its passband",
            format("Welch PSD of the packages of %u random bytes, real Encoder at %u Hz, %.1f Hz resolution; each row "
                   "in dB relative to its peak.",
                   static_cast<unsigned>(k_bytes), static_cast<unsigned>(rate), rate / k_fft),
            format("Shaded: the measured 99 %% band (0.5 %% of the power on each side); dashed: the library's "
                   "occupied_band() (%.1f/T).",
                   unlimited::k_band_99_milli / k_milli));
    const Scale x = {0.0, k_max_hz, k_left, k_right};
    double least_inside = k_percent;
    for (size_t p = 0; p < count; ++p) {
        const EncoderConfig config = preset_config(k_presets[p].preset);
        const Rendered r = render(lb::random_bytes(k_bytes, k_seed + static_cast<uint32_t>(p)), config);
        size_t first = r.audio.size();
        size_t last = 0;
        for (size_t s = 0; s < r.slots.size(); ++s) {
            if (r.slots[s].status.segment != EncoderSegment::package) continue;
            first = std::min(first, r.slots[s].first);
            last = std::max(last, r.slots[s].first + r.slots[s].count);
        }
        const std::vector<double> all = to_double(r.audio, k_full_scale);
        const std::vector<double> packages(all.begin() + first, all.begin() + last);
        const std::vector<double> psd = welch_psd(packages, k_fft);
        const double peak = *std::max_element(psd.begin(), psd.end());
        double total = 0.0;
        for (size_t k = 0; k < psd.size(); ++k) total += psd[k];
        const double tail = (1.0 - k_obw_fraction) / 2.0 * total;
        double sum = 0.0;
        size_t low_bin = 0;
        while (low_bin < psd.size() && sum + psd[low_bin] < tail) sum += psd[low_bin++];
        sum = 0.0;
        size_t high_bin = psd.size() - 1;
        while (high_bin > 0 && sum + psd[high_bin] < tail) sum += psd[high_bin--];
        const double bin_hz = rate / k_fft;
        const double low_hz = low_bin * bin_hz;
        const double high_hz = high_bin * bin_hz;
        const Band band = occupied_band(config);
        double inside = 0.0;
        for (size_t k = 0; k < psd.size(); ++k) {
            if (k * bin_hz >= band.low_hz && k * bin_hz <= band.high_hz) inside += psd[k];
        }
        const double inside_pct = k_percent * inside / total;
        least_inside = std::min(least_inside, inside_pct);

        const double top = k_top + p * (k_row_height + k_row_gap);
        const Scale y = {k_floor_db, 0.0, top + k_row_height, top};
        svg.rect(x(config.passband.low_hz), top, x(config.passband.high_hz) - x(config.passband.low_hz), k_row_height,
                 k_gray_fill);
        svg.rect(x(low_hz), top, x(high_hz) - x(low_hz), k_row_height, k_presets[p].color, "none", 0.0, 0.0, 0.16);
        svg.line(x(band.low_hz), top, x(band.low_hz), top + k_row_height, k_ink, k_line, k_dash);
        svg.line(x(band.high_hz), top, x(band.high_hz), top + k_row_height, k_ink, k_line, k_dash);
        svg.rect(k_left, top, k_right - k_left, k_row_height, "none", k_axis, k_thin);
        std::vector<Point> line;
        for (size_t k = 0; k < psd.size(); ++k) {
            const double hz = k * bin_hz;
            if (hz > k_max_hz) break;
            const double db = std::max(k_floor_db, db_power(std::max(psd[k], 1e-30) / peak));
            const Point point = {x(hz), y(db)};
            line.push_back(point);
        }
        svg.polyline(line, k_presets[p].color, k_spectrum_trace);
        svg.text(k_left - k_tick_length, top + k_text_rise * 2.0, "0", font(k_small_size - 1.0, k_muted, "end"));
        svg.text(k_left - k_tick_length, top + k_row_height, number(k_floor_db, 0),
                 font(k_small_size - 1.0, k_muted, "end"));
        svg.text(k_margin, top + k_label_line, k_presets[p].name,
                 font(k_label_size, k_presets[p].color, "start", true));
        svg.text(k_margin, top + 2.0 * k_label_line + 2.0,
                 format("T %.0f ms, N %u", slot_ms_of(config), static_cast<unsigned>(config.bits_per_package)),
                 font(k_small_size, k_muted));
        svg.text(k_margin, top + 3.0 * k_label_line + 2.0, format("%.1f bit/s net", net_bit_rate(config)),
                 font(k_small_size, k_muted));
        svg.text(k_margin, top + 4.0 * k_label_line + 2.0,
                 format("passband %u–%u Hz", static_cast<unsigned>(config.passband.low_hz),
                        static_cast<unsigned>(config.passband.high_hz)),
                 font(k_small_size, k_muted));
        svg.text(k_right, top - k_row_label_rise,
                 format("99 %% measured %.0f–%.0f Hz (%.0f Hz); occupied_band() %u–%u Hz (%u Hz) holds %.1f %%", low_hz,
                        high_hz, high_hz - low_hz, static_cast<unsigned>(band.low_hz),
                        static_cast<unsigned>(band.high_hz), static_cast<unsigned>(band.width_hz), inside_pct),
                 font(k_small_size, k_presets[p].color, "end", true));
        std::printf("presets_spectrum.svg: %-8s 99%% measured %.0f-%.0f Hz (%.0f Hz), occupied_band %u-%u Hz (%u Hz) "
                    "holds %.2f%%\n",
                    k_presets[p].name, low_hz, high_hz, high_hz - low_hz, static_cast<unsigned>(band.low_hz),
                    static_cast<unsigned>(band.high_hz), static_cast<unsigned>(band.width_hz), inside_pct);
    }
    const double bottom = k_top + count * (k_row_height + k_row_gap) - k_row_gap;
    const double k_tick_hz = 250.0;
    const size_t k_label_every = 2;
    x_axis(svg, x, bottom, 0.0, k_max_hz, k_tick_hz, 0, k_label_every);
    svg.text((k_left + k_right) / 2.0, bottom + k_axis_title_gap,
             "audio frequency (Hz); gray: the preset's passband (EncoderConfig::passband)",
             font(k_small_size, k_muted, "middle"));
    std::vector<std::string> lines;
    lines.push_back(format("Everything sits on 1500 Hz; the spectrum keeps its shape in units of 1/T, so its width "
                           "scales with the speed. occupied_band() holds ≥ %.1f %% of every row's power.",
                           least_inside));
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "presets_spectrum.svg"), "Unlimited preset spectra",
             "Measured power spectral density of the hf_slow, hf, hf_fast, am and fm presets with the measured 99 "
             "percent band and the occupied_band() of the library.");
}

// ---------------------------------------------------------------------------
// Figure: BER vs SNR in AWGN
// ---------------------------------------------------------------------------

// exp(-x) I0(x), x >= 0: power series, then the asymptotic expansion.
double bessel_i0_scaled(double x) {
    const double k_series_limit = 15.0;
    const int k_series_terms = 300;
    const int k_asymptotic_terms = 5;
    const double k_epsilon = 1e-17;
    if (x <= k_series_limit) {
        const double q = x * x / 4.0;
        double term = 1.0;
        double sum = 1.0;
        for (int k = 1; k < k_series_terms && term > sum * k_epsilon; ++k) {
            term *= q / (static_cast<double>(k) * k);
            sum += term;
        }
        return sum * std::exp(-x);
    }
    double coefficient = 1.0;
    double sum = 1.0;
    for (int k = 1; k <= k_asymptotic_terms; ++k) {
        coefficient *= square(2.0 * k - 1.0) / (8.0 * k * x);
        sum += coefficient;
    }
    return sum / std::sqrt(2.0 * k_pi * x);
}

// P(R < t) for a Rician R of parameter nu and unit noise per dimension (1 - Marcum Q1(nu, t)), by Simpson.
double rician_cdf(double t, double nu) {
    const size_t k_steps = 400;  // even
    const double h = t / k_steps;
    double sum = 0.0;
    for (size_t i = 0; i <= k_steps; ++i) {
        const double r = i * h;
        const double density = r * std::exp(-square(r - nu) / 2.0) * bessel_i0_scaled(r * nu);
        const double weight = (i == 0 || i == k_steps) ? 1.0 : (i % 2 == 1 ? 4.0 : 2.0);
        sum += weight * density;
    }
    return sum * h / 3.0;
}

// Noncoherent on-off keying with a matched filter over the whole beep, known timing and level and the best threshold:
// BER = (P(noise alone > t) + P(beep < t)) / 2 at E1/N0 = SNR * 2500 Hz * T * k_one_energy (spec 1.1, 1.3).
double bound_ber(double snr_db, double slot_s) {
    const int k_iterations = 60;
    const double snr = std::pow(10.0, snr_db / k_power_db);
    const double e1_n0 = snr * k_reference_bandwidth_hz * slot_s * unlimited::k_one_energy;
    const double nu = std::sqrt(2.0 * e1_n0);
    const auto ber = [&](double t) { return 0.5 * (std::exp(-t * t / 2.0) + rician_cdf(t, nu)); };
    double low = 0.0;
    double high = nu;
    for (int i = 0; i < k_iterations; ++i) {
        const double a = low + (high - low) / 3.0;
        const double b = high - (high - low) / 3.0;
        if (ber(a) < ber(b)) {
            high = b;
        } else {
            low = a;
        }
    }
    return ber((low + high) / 2.0);
}

double bound_snr_at(double target, double slot_s) {
    const double k_low_db = -30.0;
    const double k_high_db = 30.0;
    const int k_iterations = 50;
    double low = k_low_db;
    double high = k_high_db;
    for (int i = 0; i < k_iterations; ++i) {
        const double mid = (low + high) / 2.0;
        if (bound_ber(mid, slot_s) > target) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return (low + high) / 2.0;
}

struct BerPreset {
    Preset preset;
    const char* name;
    const char* color;
    double first_snr_db;
};

const size_t k_ber_points = 9;
const double k_ber_step_db = 1.0;
const BerPreset k_ber_presets[] = {{Preset::hf_slow, "hf_slow", k_blue, -7.0},
                                   {Preset::hf, "hf", k_teal, -4.0},
                                   {Preset::hf_fast, "hf_fast", k_coral, -1.0}};
const size_t k_ber_transmissions = 100;
const size_t k_ber_bytes = 250;
const double k_ber_silence_ms = 1000.0;
const double k_ber_max_offset_hz = 50.0;
const uint32_t k_ber_seed = 4803;
const double k_ber_marker = 1e-3;
const double k_delivered_gate = 0.9;

// The dropped v0.2 multi-pitch design (branch/tag v0.2-mfsk), its hf preset: T 32 ms, 5 bits per beep, 139 bit/s,
// integrated decoder, USB AWGN with -50..+50 Hz offsets. v0.2 spec 4.8: BER 6.01e-4 at -5.5 dB (A5), 1.07e-4 at
// -4.5 dB (A1'). Quoted, not measured here: that code is not in this tree.
const double k_v02_snr_db[] = {-5.5, -4.5};
const double k_v02_ber[] = {6.01e-4, 1.07e-4};
const double k_v02_bit_s = 139.0;

struct BerOutcome {
    size_t matched_bits = 0;
    size_t bit_errors = 0;
    size_t bytes_sent = 0;
    size_t bytes_matched = 0;
    size_t locks = 0;
};

void figure_ber(const std::string& directory) {
    const size_t presets = sizeof(k_ber_presets) / sizeof(k_ber_presets[0]);
    const size_t jobs = presets * k_ber_points * k_ber_transmissions;
    const auto started = std::chrono::steady_clock::now();
    // Slowest preset first, for an even load across the threads.
    const std::vector<BerOutcome> outcomes = parallel_map<BerOutcome>(jobs, [&](size_t job) {
        const size_t p = job / (k_ber_points * k_ber_transmissions);
        const size_t point = job / k_ber_transmissions % k_ber_points;
        const uint32_t seed = k_ber_seed + static_cast<uint32_t>(job);
        const EncoderConfig config = preset_config(k_ber_presets[p].preset);
        const lb::Recording recording = lb::single(lb::random_bytes(k_ber_bytes, seed), config, k_ber_silence_ms);
        std::mt19937 generator(seed);
        std::uniform_real_distribution<double> offset(-k_ber_max_offset_hz, k_ber_max_offset_hz);
        sim::ChannelConfig channel;
        channel.mode = sim::Mode::usb;
        channel.snr_db = k_ber_presets[p].first_snr_db + point * k_ber_step_db;
        channel.freq_offset_hz = offset(generator);
        channel.seed = seed;
        const std::vector<int16_t> received = through_channel(recording.samples, channel, config.amplitude, 1.0);
        const lb::Capture capture =
            lb::run_decoder(received, DecoderConfig::for_profile(Profile::ssb), k_decoder_chunk);
        const lb::Score score = lb::score(recording, capture);
        BerOutcome o;
        o.matched_bits = score.matched * k_byte_bits;
        o.bit_errors = score.bit_errors;
        o.bytes_sent = score.bytes_sent;
        o.bytes_matched = score.matched;
        o.locks = score.locks;
        return o;
    });
    std::vector<BerOutcome> points(presets * k_ber_points);
    for (size_t job = 0; job < jobs; ++job) {
        BerOutcome& into = points[job / k_ber_transmissions];
        into.matched_bits += outcomes[job].matched_bits;
        into.bit_errors += outcomes[job].bit_errors;
        into.bytes_sent += outcomes[job].bytes_sent;
        into.bytes_matched += outcomes[job].bytes_matched;
        into.locks += outcomes[job].locks;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    const double k_width = 820.0;
    const double k_height = 540.0;
    const Box plot = {70.0, 100.0, 500.0, 330.0};
    const double k_snr_low = -8.0;
    const double k_snr_high = 8.0;
    const double k_ber_low = 1e-6;
    const double k_ber_high = 1e-1;
    const int k_first_decade = -6;
    const int k_last_decade = -1;
    const double k_marker_radius = 4.0;
    Svg svg(k_width, k_height);
    heading(svg, "BER vs SNR in AWGN, measured through the real chain",
            format("Encoder → sim::Channel (USB, AWGN, tuned ±%.0f Hz) → Decoder (ssb profile: it learns the pitch, T "
                   "and N by itself).",
                   k_ber_max_offset_hz),
            format("%u transmissions of %u random bytes per point (%u bits sent); a fresh receiver for each "
                   "transmission; measured by make docs.",
                   static_cast<unsigned>(k_ber_transmissions), static_cast<unsigned>(k_ber_bytes),
                   static_cast<unsigned>(k_ber_transmissions * k_ber_bytes * k_byte_bits)));
    const Scale x = {k_snr_low, k_snr_high, plot.left, plot.right()};
    const LogScale y = {k_ber_low, k_ber_high, plot.bottom(), plot.top};
    const double k_minor_first = 2.0;
    const double k_minor_last = 9.0;
    const double k_minor_tick = 3.0;
    const double k_decade_label_gap = 6.0;
    const double k_line_label_inset = 4.0;
    const double k_line_label_rise = 5.0;
    for (int decade = k_first_decade; decade <= k_last_decade; ++decade) {
        const double value = std::pow(10.0, decade);
        svg.line(plot.left, y(value), plot.right(), y(value), k_grid_line, k_line);
        svg.text(plot.left - k_decade_label_gap, y(value) + k_text_rise, format("1e%d", decade),
                 font(k_tick_size, k_muted, "end"));
        for (double m = k_minor_first; m <= k_minor_last && decade < k_last_decade; m += 1.0)
            svg.line(plot.left - k_minor_tick, y(m * value), plot.left, y(m * value), k_axis, k_thin);
    }
    x_axis(svg, x, plot.bottom(), k_snr_low, k_snr_high, 1.0, 0, 1, plot.top);
    svg.rect(plot.left, plot.top, plot.width, plot.height, "none", k_axis, k_line);
    svg.line(plot.left, y(k_ber_marker), plot.right(), y(k_ber_marker), k_amber, k_reference_trace, k_long_dash);
    svg.text(plot.right() - k_line_label_inset, y(k_ber_marker) - k_line_label_rise, "BER 1e-3",
             font(k_small_size, k_amber, "end", true));
    svg.text((plot.left + plot.right()) / 2.0, plot.bottom() + k_axis_title_gap,
             "SNR (dB): key-down tone power over noise in 2500 Hz", font(k_small_size, k_muted, "middle"));
    const double k_axis_title_x = 50.0;
    svg.text(plot.left - k_axis_title_x, (plot.top + plot.bottom()) / 2.0, "bit error rate",
             font(k_small_size, k_muted, "middle"), -90.0);

    // Bounds (dashed), clipped to the plot.
    const std::string clip_id = svg.clip(plot.left, plot.top - 1.0, plot.width, plot.height + 2.0);
    svg.begin_clip(clip_id);
    const size_t k_bound_steps_per_db = 4;
    const size_t bound_steps = static_cast<size_t>(std::lround((k_snr_high - k_snr_low) * k_bound_steps_per_db));
    std::vector<double> bound_1e3(presets);
    for (size_t p = 0; p < presets; ++p) {
        const EncoderConfig config = preset_config(k_ber_presets[p].preset);
        std::vector<Point> curve;
        for (size_t i = 0; i <= bound_steps; ++i) {
            const double snr = k_snr_low + static_cast<double>(i) / k_bound_steps_per_db;
            const double ber = bound_ber(snr, slot_s_of(config));
            const Point point = {x(snr), y(std::max(ber, k_ber_low / 2.0))};
            curve.push_back(point);
            if (ber < k_ber_low) break;
        }
        svg.polyline(curve, k_ber_presets[p].color, k_line, k_short_dash);
        bound_1e3[p] = bound_snr_at(k_ber_marker, slot_s_of(config));
    }
    // The dropped v0.2 design's hf point, quoted.
    std::vector<Point> v02;
    for (size_t i = 0; i < sizeof(k_v02_ber) / sizeof(k_v02_ber[0]); ++i) {
        const Point point = {x(k_v02_snr_db[i]), y(k_v02_ber[i])};
        v02.push_back(point);
    }
    svg.polyline(v02, k_gray, k_line, k_dot);
    svg.end_group();
    const double k_diamond = 5.0;
    for (size_t i = 0; i < v02.size(); ++i) diamond(svg, v02[i].x, v02[i].y, k_diamond, k_gray);

    std::vector<double> measured_1e3(presets, 0.0);
    for (size_t p = 0; p < presets; ++p) {
        // Lines join neighbouring points with errors only: a point without errors breaks the line.
        std::vector<Point> measured;
        for (size_t point = 0; point <= k_ber_points; ++point) {
            const BerOutcome* o = point < k_ber_points ? &points[p * k_ber_points + point] : nullptr;
            if (o == nullptr || o->matched_bits == 0 || o->bit_errors == 0) {
                svg.polyline(measured, k_ber_presets[p].color, k_curve);
                measured.clear();
                continue;
            }
            const double snr = k_ber_presets[p].first_snr_db + point * k_ber_step_db;
            const Point at = {x(snr), y(static_cast<double>(o->bit_errors) / o->matched_bits)};
            measured.push_back(at);
        }
        for (size_t point = 0; point < k_ber_points; ++point) {
            const BerOutcome& o = points[p * k_ber_points + point];
            const double snr = k_ber_presets[p].first_snr_db + point * k_ber_step_db;
            if (o.matched_bits == 0) continue;
            const bool delivered =
                o.bytes_sent > 0 && static_cast<double>(o.bytes_matched) / o.bytes_sent >= k_delivered_gate;
            if (o.bit_errors == 0) {
                triangle_down(svg, x(snr), y(std::max(k_ber_low, 1.0 / o.matched_bits)), k_marker_radius,
                              k_ber_presets[p].color);
                continue;
            }
            const double ber = static_cast<double>(o.bit_errors) / o.matched_bits;
            svg.circle(x(snr), y(ber), k_marker_radius, delivered ? k_ber_presets[p].color : k_card,
                       k_ber_presets[p].color, k_medium);
            if (point + 1 >= k_ber_points) continue;
            // SNR at 1e-3, interpolated in log BER between the two points around it.
            const BerOutcome& n = points[p * k_ber_points + point + 1];
            if (n.matched_bits == 0 || ber < k_ber_marker) continue;
            const double next = static_cast<double>(std::max<size_t>(n.bit_errors, 1)) / n.matched_bits;
            if (next > k_ber_marker) continue;
            const double f = (std::log10(ber) - std::log10(k_ber_marker)) / (std::log10(ber) - std::log10(next));
            measured_1e3[p] = snr + f * k_ber_step_db;
        }
    }

    // Legend.
    const double lx = plot.right() + 20.0;
    const double k_legend_top = 8.0;
    const double k_legend_group_gap = 4.0;
    const double k_legend_section_gap = 8.0;
    double ly = plot.top + k_legend_top;
    const double k_legend_step = 19.0;
    const double k_sample = 22.0;
    const double k_label_gap = 30.0;
    for (size_t p = 0; p < presets; ++p) {
        const EncoderConfig config = preset_config(k_ber_presets[p].preset);
        svg.line(lx, ly - k_text_rise, lx + k_sample, ly - k_text_rise, k_ber_presets[p].color, k_curve);
        svg.circle(lx + k_sample / 2.0, ly - k_text_rise, k_marker_radius, k_ber_presets[p].color,
                   k_ber_presets[p].color, k_medium);
        svg.text(lx + k_label_gap, ly, format("%s: T %.0f ms, %.1f bit/s", k_ber_presets[p].name, slot_ms_of(config),
                                              net_bit_rate(config)),
                 font(k_note_size, k_ink, "start", true));
        ly += k_legend_step;
    }
    ly += k_legend_group_gap;
    svg.line(lx, ly - k_text_rise, lx + k_sample, ly - k_text_rise, k_ink, k_line, k_short_dash);
    svg.text(lx + k_label_gap, ly, "matched-filter bound (known", font(k_note_size, k_ink));
    ly += k_line_gap - 2.0;
    svg.text(lx + k_label_gap, ly, "timing, level, best threshold)", font(k_note_size, k_ink));
    ly += k_legend_step;
    svg.circle(lx + k_sample / 2.0, ly - k_text_rise, k_marker_radius, k_card, k_ink, k_medium);
    svg.text(lx + k_label_gap, ly, format("< %.0f %% of the bytes delivered", k_delivered_gate * k_percent),
             font(k_note_size, k_ink));
    ly += k_legend_step;
    triangle_down(svg, lx + k_sample / 2.0, ly - k_text_rise, k_marker_radius, k_ink);
    svg.text(lx + k_label_gap, ly, "0 errors (drawn at 1/bits)", font(k_note_size, k_ink));
    ly += k_legend_step;
    diamond(svg, lx + k_sample / 2.0, ly - k_text_rise, k_diamond, k_gray);
    svg.text(lx + k_label_gap, ly, "v0.2-mfsk hf: the dropped", font(k_note_size, k_ink));
    ly += k_line_gap - 2.0;
    svg.text(lx + k_label_gap, ly, format("multi-pitch design, %.0f bit/s", k_v02_bit_s), font(k_note_size, k_ink));
    ly += k_line_gap - 2.0;
    svg.text(lx + k_label_gap, ly, "(its spec 4.8; not rerun)", font(k_small_size, k_muted));

    ly += k_legend_step + k_legend_section_gap;
    svg.text(lx, ly, "SNR at BER 1e-3 (dB):", font(k_note_size, k_ink, "start", true));
    ly += k_line_gap;
    svg.text(lx, ly, "measured / bound / spec gate", font(k_small_size, k_muted));
    const double k_gate_db[] = {-1.5, 1.5, 4.5};  // spec 4.1 release gates (A1) at T = 32, 16 and 8 ms
    for (size_t p = 0; p < presets; ++p) {
        ly += k_line_gap;
        const std::string measured =
            measured_1e3[p] != 0.0 ? signed_number(measured_1e3[p], 1) : std::string("n/a");
        svg.text(lx, ly, format("%s: %s / %s / %s", k_ber_presets[p].name, measured.c_str(),
                                signed_number(bound_1e3[p], 1).c_str(), signed_number(k_gate_db[p], 1).c_str()),
                 font(k_note_size, k_ber_presets[p].color, "start", true));
    }

    std::vector<std::string> lines;
    lines.push_back("BER over the bytes the Decoder released (bit errors / bits matched to what was sent); hollow: the "
                    "receiver missed some transmissions.");
    lines.push_back("+3 dB per doubling of T. Near the left end the limit is acquisition (finding the tune and the "
                    "sync train), not the slot decisions.");
    lines.push_back(format("Spec gate: the release gate A1 of spec 4.1, where the BER must be ≤ 1e-3. The bound uses "
                           "E1/N0 = SNR · 2500 Hz · T · %.4g.",
                           unlimited::k_one_energy));
    notes(svg, k_height, lines);
    svg.save(path_of(directory, "ber_awgn.svg"), "Unlimited BER in AWGN",
             "Measured bit error rate against SNR for the hf_slow, hf and hf_fast presets through the real encoder, "
             "channel simulator and decoder, with the matched-filter bound and the dropped v0.2 hf point.");

    std::printf("ber_awgn.svg: %u jobs in %.1f s\n", static_cast<unsigned>(jobs), seconds);
    for (size_t p = 0; p < presets; ++p) {
        std::printf("  %-8s 1e-3 at: measured %.2f dB, bound %.2f dB\n", k_ber_presets[p].name, measured_1e3[p],
                    bound_1e3[p]);
        for (size_t point = 0; point < k_ber_points; ++point) {
            const BerOutcome& o = points[p * k_ber_points + point];
            const double snr = k_ber_presets[p].first_snr_db + point * k_ber_step_db;
            const EncoderConfig config = preset_config(k_ber_presets[p].preset);
            std::printf("    %+5.1f dB: %7u bits, %5u errors, BER %.2e (bound %.2e), bytes %5.1f%%, locks %u/%u\n",
                        snr, static_cast<unsigned>(o.matched_bits), static_cast<unsigned>(o.bit_errors),
                        o.matched_bits == 0 ? 0.0 : static_cast<double>(o.bit_errors) / o.matched_bits,
                        bound_ber(snr, slot_s_of(config)),
                        o.bytes_sent == 0 ? 0.0 : k_percent * o.bytes_matched / o.bytes_sent,
                        static_cast<unsigned>(o.locks), static_cast<unsigned>(k_ber_transmissions));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int k_exit_usage = 2;
    const int k_exit_failure = 1;
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: doc_figures OUTPUT_DIRECTORY [FIGURE_NAME_PREFIX]\n");
        return k_exit_usage;
    }
    const std::string directory = argv[1];
    const std::string only = argc == 3 ? argv[2] : "";
    const auto wanted = [&](const char* name) { return std::string(name).compare(0, only.size(), only) == 0; };
    try {
        const auto started = std::chrono::steady_clock::now();
        const std::vector<uint8_t> hi = text_bytes(k_hi_text);
        const Rendered hf = render(hi, preset_config(Preset::hf));
        const Rendered hf_wave = render(hi, preset_config(Preset::hf, k_wave_rate_hz));
        unsigned figures = 0;
        if (wanted("slot_shapes") && ++figures) figure_slot_shapes(directory, hf_wave);
        if (wanted("transmission_timeline") && ++figures) figure_timeline(directory, hf_wave);
        if (wanted("spectrogram_hf") && ++figures) figure_spectrogram(directory, hf);
        if (wanted("packing") && ++figures) figure_packing(directory);
        if (wanted("receiver_decision") && ++figures) figure_receiver_decision(directory);
        if (wanted("bandwidth_filters") && ++figures) figure_bandwidth(directory);
        if (wanted("channels") && ++figures) figure_channels(directory);
        if (wanted("envelope_wide_vs_narrow") && ++figures) figure_envelopes(directory, hf);
        if (wanted("presets_spectrum") && ++figures) figure_presets_spectrum(directory);
        if (wanted("ber_awgn") && ++figures) figure_ber(directory);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::printf("doc_figures: %u figures in %.1f s\n", figures, seconds);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "doc_figures: %s\n", error.what());
        return k_exit_failure;
    }
    return 0;
}
