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
// decode result and BER point is measured on the output of the real Encoder, the real sim::Channel and the real
// Decoder each time `make docs` runs; nothing is drawn from hand-copied numbers except the v0.1 reference point.
namespace {

using std::int16_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint8_t;
using unlimited::DecoderConfig;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSegment;
using unlimited::EncoderStatus;
using unlimited::Event;
using unlimited::EventType;
using unlimited::GridSide;
using unlimited::Preset;
using unlimited::Profile;
using unlimited::SlotKind;
using unlimited::Spacing;

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
const double k_dense_spacing = 1.0;             // c = 1: dense spacing 1 / T
const double k_reference_bandwidth_hz = 2500.0; // SNR: key-down tone power over noise in 2500 Hz (spec D19)
const double k_receiver_bandwidth_hz = 2400.0;  // 300..2700 Hz
const double k_noise_peak_sigmas = 5.0;         // channel output scaled so that 5 sigma of noise does not clip
const double k_headroom = 0.9;
const double k_fading_peak = 3.0;               // Rayleigh amplitude above 3x RMS: probability 1.2e-4
const double k_fm_peak = 2.0;                   // FM clicks
const uint32_t k_rate_hz = unlimited::k_decoder_rate_hz;
const uint32_t k_wave_rate_hz = 48000;          // slot_shapes: smooth carrier cycles
const size_t k_decoder_chunk = 256;
const size_t k_format_buffer = 1024;
const char* const k_example_text = "CQ DE PY2";

// ---------------------------------------------------------------------------
// Palette and fonts (light card: legible on GitHub in light and dark themes)
// ---------------------------------------------------------------------------

const char* const k_ink = "#2C2C2A";
const char* const k_muted = "#5F5E5A";
const char* const k_card = "#FFFFFF";
const char* const k_card_border = "#D3D1C7";
const char* const k_axis = "#888780";
const char* const k_grid_line = "#ECEAE2";
const char* const k_teal = "#1D9E75";          // data peaks
const char* const k_teal_dark = "#0F6E56";     // header peaks
const char* const k_teal_fill = "#E1F5EE";
const char* const k_purple = "#7F77DD";        // markers (f_ref)
const char* const k_purple_light = "#AFA9EC";  // tune tone (f_ref)
const char* const k_purple_fill = "#EEEDFE";
const char* const k_gray = "#888780";          // guides
const char* const k_gray_fill = "#F1EFE8";
const char* const k_coral = "#D85A30";         // noise, impairments
const char* const k_amber = "#BA7517";         // thresholds
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
const double k_line_gap = 16.0;       // baseline to baseline, 11-12 px text
const double k_tick_length = 4.0;
const double k_tick_label_gap = 14.0; // axis line to tick label baseline
const double k_text_rise = 4.0;       // lifts a label above a line

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
        if (stroke != "none") body_ << " stroke=\"" << stroke << "\" stroke-width=\"" << coordinate(stroke_width) << "\"";
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
        if (stroke != "none") body_ << " stroke=\"" << stroke << "\" stroke-width=\"" << coordinate(stroke_width) << "\"";
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

    void image(double x, double y, double w, double h, const std::string& png, const std::string& clip_id = "") {
        body_ << "<image x=\"" << coordinate(x) << "\" y=\"" << coordinate(y) << "\" width=\"" << coordinate(w)
              << "\" height=\"" << coordinate(h) << "\" preserveAspectRatio=\"none\"";
        if (!clip_id.empty()) body_ << " clip-path=\"url(#" << clip_id << ")\"";
        body_ << " xlink:href=\"data:image/png;base64," << base64(png) << "\"/>\n";
    }

    void raw(const std::string& element) { body_ << element << "\n"; }

    void save(const std::string& path, const std::string& title, const std::string& description) const {
        std::ofstream file(path.c_str(), std::ios::binary);
        file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             << "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\""
             << coordinate(width_) << "\" height=\"" << coordinate(height_) << "\" viewBox=\"0 0 "
             << coordinate(width_) << " " << coordinate(height_) << "\" font-family=\"" << k_font_family
             << "\" role=\"img\">\n"
             << "<title>" << escape(title) << "</title>\n"
             << "<desc>" << escape(description) << "</desc>\n"
             << "<rect x=\"0.5\" y=\"0.5\" width=\"" << coordinate(width_ - 1.0) << "\" height=\""
             << coordinate(height_ - 1.0) << "\" rx=\"" << coordinate(k_card_radius) << "\" fill=\"" << k_card
             << "\" stroke=\"" << k_card_border << "\"/>\n"
             << body_.str() << "</svg>\n";
        if (!file) throw std::runtime_error("cannot write " + path);
    }

private:
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
        const unsigned k_byte_bits = 8;
        std::string out;
        for (size_t i = 0; i < bytes.size(); i += k_group_bytes) {
            unsigned group = 0;
            const size_t count = std::min<size_t>(k_group_bytes, bytes.size() - i);
            for (size_t j = 0; j < k_group_bytes; ++j)
                group = (group << k_byte_bits) | (j < count ? static_cast<uint8_t>(bytes[i + j]) : 0u);
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

// Ticks first, first + step, ... last on a horizontal axis at y; labels every `label_every` ticks.
void x_axis(Svg& svg, const Scale& x, double y, double first, double last, double step, int decimals,
            size_t label_every = 1, double grid_top = 0.0) {
    svg.line(x.to0, y, x.to1, y, k_axis, 1.0);
    const size_t count = static_cast<size_t>(std::lround((last - first) / step));
    for (size_t i = 0; i <= count; ++i) {
        const double value = first + i * step;
        const double px = x(value);
        if (grid_top > 0.0) svg.line(px, grid_top, px, y, k_grid_line, 1.0);
        svg.line(px, y, px, y + k_tick_length, k_axis, 1.0);
        if (i % label_every == 0)
            svg.text(px, y + k_tick_label_gap, number(value, decimals), font(k_tick_size, k_muted, "middle"));
    }
}

void y_axis(Svg& svg, const Scale& y, double x, double first, double last, double step, int decimals,
            double grid_right = 0.0) {
    svg.line(x, y.to0, x, y.to1, k_axis, 1.0);
    const size_t count = static_cast<size_t>(std::lround((last - first) / step));
    for (size_t i = 0; i <= count; ++i) {
        const double value = first + i * step;
        const double py = y(value);
        if (grid_right > 0.0) svg.line(x, py, grid_right, py, k_grid_line, 1.0);
        svg.line(x - k_tick_length, py, x, py, k_axis, 1.0);
        svg.text(x - k_tick_length - 2.0, py + k_text_rise, number(value, decimals),
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
const unsigned k_byte_bits = 8;
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
const uint8_t k_length_extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
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

// Image of a spectrogram placed on time and frequency scales (rows are bin centres).
void draw_spectrogram(Svg& svg, const Spectrogram& s, const Scale& t, const Scale& f, double reference,
                      double floor_db, const std::string& clip_id) {
    const double top = f(s.low_hz + (s.rows - 0.5) * s.bin_hz);
    const double bottom = f(s.low_hz - 0.5 * s.bin_hz);
    svg.image(t(0.0), top, t(s.columns * s.column_s) - t(0.0), bottom - top, spectrogram_png(s, reference, floor_db),
              clip_id);
}

// Colour bar with its end labels.
void color_bar(Svg& svg, double x, double y, double w, double h, double floor_db) {
    const size_t k_bar_steps = 48;
    for (size_t i = 0; i < k_bar_steps; ++i) {
        const double t = (i + 0.5) / k_bar_steps;
        svg.rect(x + w * i / k_bar_steps, y, w / k_bar_steps + 0.5, h, hex_color(heat_color(t)));
    }
    svg.rect(x, y, w, h, "none", k_axis, 0.8);
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
    std::vector<int16_t> audio;
    std::vector<SlotRun> slots;
};

Rendered render(const std::vector<uint8_t>& data, const EncoderConfig& config) {
    Rendered r;
    r.config = config;
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

std::vector<uint8_t> text_bytes() {
    const std::string text = k_example_text;
    return std::vector<uint8_t>(text.begin(), text.end());
}

double slot_ms_of(const EncoderConfig& config) {
    return config.slot_us / k_us_per_ms;
}

double slot_s_of(const EncoderConfig& config) {
    return config.slot_us / k_us_per_s;
}

unsigned tone_count(const EncoderConfig& config) {
    return 1u << config.bits_per_peak;
}

double standard_spacing() {
    return static_cast<double>(unlimited::k_standard_spacing_num) / unlimited::k_standard_spacing_den;
}

// Audio frequency of grid tone n (spec 1.3), as sent; header tones use the standard grid.
double tone_hz(const EncoderConfig& config, double tone, bool header = false) {
    const double side = config.side == GridSide::above ? 1.0 : -1.0;
    const double c = header || config.spacing == Spacing::standard ? standard_spacing() : k_dense_spacing;
    return config.tone_hz + side * (unlimited::k_grid_guard + tone * c) / slot_s_of(config);
}

double net_bit_rate(const EncoderConfig& config) {
    return config.data_slots * config.bits_per_peak / ((config.data_slots + 1.0) * slot_s_of(config));
}

enum class SpanKind { lead_in, tune, sync, header, frame, short_frame, eot, tail };

// A labelled part of the transmission: a frame is its peaks and its STOP.
struct Span {
    SpanKind kind;
    size_t first_slot;
    size_t slots;
    size_t first;
    size_t samples;
    unsigned peaks;
    unsigned frame;
};

std::vector<Span> spans(const Rendered& r) {
    std::vector<Span> list;
    unsigned frames = 0;
    for (size_t s = 0; s < r.slots.size(); ++s) {
        const EncoderStatus& st = r.slots[s].status;
        SpanKind kind = SpanKind::tail;
        bool begin = false;
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
            case EncoderSegment::header:
                kind = SpanKind::header;
                break;
            case EncoderSegment::frame:
                kind = SpanKind::frame;
                begin = st.kind == SlotKind::peak && st.slot == 0;
                break;
            case EncoderSegment::eot:
                kind = SpanKind::eot;
                break;
            case EncoderSegment::idle:
            case EncoderSegment::tail:
                break;
        }
        if (list.empty() || list.back().kind != kind || begin) {
            const Span span = {kind, s, 0, r.slots[s].first, 0, 0, kind == SpanKind::frame ? frames++ : 0u};
            list.push_back(span);
        }
        ++list.back().slots;
        list.back().samples += r.slots[s].count;
        if (st.kind == SlotKind::peak) ++list.back().peaks;
    }
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].kind == SpanKind::frame && list[i].peaks < r.config.data_slots) list[i].kind = SpanKind::short_frame;
    }
    return list;
}

std::string span_name(const Span& span) {
    switch (span.kind) {
        case SpanKind::lead_in:
            return "lead-in";
        case SpanKind::tune:
            return "tune";
        case SpanKind::sync:
            return "sync";
        case SpanKind::header:
            return "header";
        case SpanKind::frame:
            return format("frame %u", span.frame);
        case SpanKind::short_frame:
            return "short final";
        case SpanKind::eot:
            return "EOT";
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
        case SlotKind::peak:
            return slot.status.segment == EncoderSegment::header ? k_teal_dark : k_teal;
        case SlotKind::silent:
            break;
    }
    return k_gray_fill;
}

// int16 -> sim::Channel -> int16, scaled so that key-down peaks times peak_factor plus 5 sigma of noise do not
// clip (as the long regression suite does).
std::vector<int16_t> through_channel(const std::vector<int16_t>& samples, sim::ChannelConfig config,
                                     int16_t amplitude, double peak_factor) {
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

// Tukey envelope with sin^2 ramps of `ramp` (fraction of T) at both ends (spec 1.1).
double tukey(double u, double ramp) {
    if (u < ramp) return square(std::sin(k_pi / 2.0 * u / ramp));
    if (u > 1.0 - ramp) return square(std::sin(k_pi / 2.0 * (1.0 - u) / ramp));
    return 1.0;
}

// Marker envelope w(u) r(u): Tukey alpha 0.5 times the shaped reversal from +1 to -1.
double marker_envelope(double u) {
    const double start = unlimited::k_reversal_start;
    const double width = unlimited::k_reversal_width;
    double r = 1.0;
    if (u >= start + width) {
        r = -1.0;
    } else if (u > start) {
        r = std::cos(k_pi * (u - start) / width);
    }
    return tukey(u, unlimited::k_tukey_ramp) * r;
}

struct Box {
    double left;
    double top;
    double width;
    double height;
    double right() const { return left + width; }
    double bottom() const { return top + height; }
};

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
    svg.polyline(upper, k_ink, 1.2, "5 3");
    svg.polyline(lower, k_ink, 1.2, "5 3");
}

void wave_frame(Svg& svg, const Box& box, const Scale& x, const Scale& y, double t0, double t1, double tick_ms,
                const std::string& title, const std::string& subtitle) {
    svg.text(box.left, box.top - 26.0, title, font(k_label_size, k_ink, "start", true));
    svg.text(box.left, box.top - 11.0, subtitle, font(k_note_size, k_muted));
    y_axis(svg, y, box.left, -1.0, 1.0, 1.0, 0, box.right());
    x_axis(svg, x, box.bottom(), t0, t1, tick_ms, 0);
}

const SlotRun& find_slot(const Rendered& r, EncoderSegment segment, SlotKind kind) {
    for (size_t s = 0; s < r.slots.size(); ++s) {
        if (r.slots[s].status.segment == segment && r.slots[s].status.kind == kind) return r.slots[s];
    }
    throw std::runtime_error("slot not found");
}

void figure_slot_shapes(const std::string& directory, const Rendered& r) {
    const EncoderConfig& config = r.config;
    const std::vector<double> wave = to_double(r.audio, config.amplitude);
    const double rate = config.sample_rate_hz;
    const double slot_ms = slot_ms_of(config);
    const SlotRun& peak = find_slot(r, EncoderSegment::frame, SlotKind::peak);
    const SlotRun& marker = find_slot(r, EncoderSegment::frame, SlotKind::marker);
    const SlotRun& tune = find_slot(r, EncoderSegment::tune, SlotKind::tone);
    const size_t k_tune_slots_shown = 2;

    const double k_width = 820.0;
    const double k_height = 572.0;
    const double k_panel_left = 58.0;
    const double k_panel_width = 215.0;
    const double k_panel_gap = 50.0;
    const double k_panel_top = 108.0;
    const double k_panel_height = 130.0;
    const double k_amplitude_span = 1.15;
    const double k_peak_tick_ms = 8.0;
    const double k_tune_tick_ms = 16.0;
    Svg svg(k_width, k_height);
    heading(svg, "Slot shapes: real Encoder audio, hf preset (T 32 ms)",
            "Rendered by the Encoder at 48 kHz; amplitude relative to the crest A; dashed: the envelope of spec 1.1.");

    // Panel 1: data peak.
    Box box = {k_panel_left, k_panel_top, k_panel_width, k_panel_height};
    Scale x = {0.0, slot_ms, box.left, box.right()};
    const Scale y = {-k_amplitude_span, k_amplitude_span, box.bottom(), box.top};
    const double ramp_ms = unlimited::k_data_ramp * slot_ms;
    svg.rect(x(0.0), box.top, x(ramp_ms) - x(0.0), box.height, k_gray_fill);
    svg.rect(x(slot_ms - ramp_ms), box.top, x(slot_ms) - x(slot_ms - ramp_ms), box.height, k_gray_fill);
    wave_frame(svg, box, x, y, 0.0, slot_ms, k_peak_tick_ms, "Data peak (header and frames)",
               "grid tone, Tukey α 0.25: ramps T/8, no flip");
    draw_wave(svg, x, y, wave, peak.first, peak.count, peak.first, rate, k_teal, 0.6);
    draw_envelope(svg, x, y, 0.0, slot_ms, [&](double t) { return tukey(t / slot_ms, unlimited::k_data_ramp); });

    // Panel 2: marker.
    box.left += k_panel_width + k_panel_gap;
    x = Scale{0.0, slot_ms, box.left, box.right()};
    const double reversal_first_ms = unlimited::k_reversal_start * slot_ms;
    const double reversal_last_ms = (unlimited::k_reversal_start + unlimited::k_reversal_width) * slot_ms;
    svg.rect(x(reversal_first_ms), box.top, x(reversal_last_ms) - x(reversal_first_ms), box.height, k_purple_fill);
    wave_frame(svg, box, x, y, 0.0, slot_ms, k_peak_tick_ms, "START/STOP marker",
               "f_ref, Tukey α 0.5 + shaped 180° reversal");
    draw_wave(svg, x, y, wave, marker.first, marker.count, marker.first, rate, k_purple, 0.6);
    draw_envelope(svg, x, y, 0.0, slot_ms, [&](double t) { return marker_envelope(t / slot_ms); });

    // Panel 3: the first tune slots.
    box.left += k_panel_width + k_panel_gap;
    const double tune_ms = k_tune_slots_shown * slot_ms;
    x = Scale{0.0, tune_ms, box.left, box.right()};
    wave_frame(svg, box, x, y, 0.0, tune_ms, k_tune_tick_ms, "Tune tone (start)",
               "f_ref, ramp-up T/4, then steady");
    draw_wave(svg, x, y, wave, tune.first, static_cast<size_t>(k_tune_slots_shown * tune.count), tune.first, rate,
              k_purple_light, 0.6);
    draw_envelope(svg, x, y, 0.0, tune_ms, [&](double t) {
        const double u = t / slot_ms;
        return u < unlimited::k_tukey_ramp ? tukey(u, unlimited::k_tukey_ramp) : 1.0;
    });
    for (size_t p = 0; p < 3; ++p) {
        const double left = k_panel_left + p * (k_panel_width + k_panel_gap);
        svg.text(left + k_panel_width / 2.0, k_panel_top + k_panel_height + 30.0, "time in slot (ms)",
                 font(k_small_size, k_muted, "middle"));
    }

    // Zoom on the marker's reversal, with the carrier before it continued (least-squares fit on the flat top).
    const double k_zoom_first_u = unlimited::k_tukey_ramp;
    const double k_zoom_last_u = 1.0 - unlimited::k_tukey_ramp;
    const double k_zoom_tick_ms = 2.0;
    const double k_zoom_span = 1.45;  // room above the crest for the labels
    const Box zoom = {k_panel_left, 336.0, 2.0 * k_panel_gap + 3.0 * k_panel_width, 140.0};
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
    svg.rect(zx(reversal_first_ms), zoom.top, zx(reversal_last_ms) - zx(reversal_first_ms), zoom.height, k_purple_fill);
    svg.text(zoom.left, zoom.top - 26.0,
             format("Marker reversal, zoomed: the flat top of the STOP slot above, u = %.2f…%.2f",
                    k_zoom_first_u, k_zoom_last_u),
             font(k_label_size, k_ink, "start", true));
    svg.text(zoom.left, zoom.top - 11.0,
             "purple: Encoder audio; gray dotted: the carrier before the reversal, continued; shaded: 0.375T–0.625T",
             font(k_note_size, k_muted));
    y_axis(svg, zy, zoom.left, -1.0, 1.0, 1.0, 0, zoom.right());
    x_axis(svg, zx, zoom.bottom(), std::ceil(z0 / k_zoom_tick_ms) * k_zoom_tick_ms,
           std::floor(z1 / k_zoom_tick_ms) * k_zoom_tick_ms, k_zoom_tick_ms, 0);
    std::vector<Point> reference;
    const size_t zoom_first = marker.first + static_cast<size_t>(k_zoom_first_u * marker.count);
    const size_t zoom_last = marker.first + static_cast<size_t>(k_zoom_last_u * marker.count);
    for (size_t n = zoom_first; n <= zoom_last; ++n) {
        const Point p = {zx((n - static_cast<double>(marker.first)) * k_ms_per_s / rate),
                         zy(a * std::sin(omega * n) + b * std::cos(omega * n))};
        reference.push_back(p);
    }
    svg.polyline(reference, k_gray, 1.2, "2 2");
    draw_wave(svg, zx, zy, wave, zoom_first, zoom_last - zoom_first + 1, marker.first, rate, k_purple, 1.4);
    draw_envelope(svg, zx, zy, z0, z1, [&](double t) { return marker_envelope(t / slot_ms); });
    const double label_y = zoom.top + 14.0;
    svg.text((zoom.left + zx(reversal_first_ms)) / 2.0, label_y, "same phase as before",
             font(k_note_size, k_ink, "middle", true));
    svg.text((zx(reversal_first_ms) + zx(reversal_last_ms)) / 2.0, label_y, "shaped reversal: amplitude through 0",
             font(k_note_size, k_purple, "middle", true));
    svg.text((zx(reversal_last_ms) + zoom.right()) / 2.0, label_y, "inverted: s ← −s",
             font(k_note_size, k_ink, "middle", true));
    svg.text(zoom.left + zoom.width / 2.0, zoom.bottom() + 30.0, "time in slot (ms)",
             font(k_small_size, k_muted, "middle"));
    svg.text(k_margin, k_height - 32.0,
             "Markers sit on f_ref and flip its sign for good (s ← −s); peaks sit on a grid tone and never flip.",
             font(k_note_size, k_muted));
    svg.text(k_margin, k_height - 16.0, "Every envelope is 0 at the slot edges: no key clicks.",
             font(k_note_size, k_muted));
    svg.save(path_of(directory, "slot_shapes.svg"), "Unlimited slot shapes",
             "Data peak, START/STOP marker with its 180 degree phase reversal, and tune tone start, rendered by the "
             "Encoder (hf preset, 48 kHz).");
    std::printf("slot_shapes.svg: peak, marker and tune slots of hf at %u Hz\n", static_cast<unsigned>(rate));
}

// ---------------------------------------------------------------------------
// Figure: transmission timeline
// ---------------------------------------------------------------------------

std::string span_detail(const Span& span, const EncoderConfig& config) {
    switch (span.kind) {
        case SpanKind::lead_in:
            return format("%u ms", static_cast<unsigned>(config.lead_in_ms));
        case SpanKind::tune:
            return format("%u × T at f_ref", static_cast<unsigned>(span.slots));
        case SpanKind::sync:
            return format("%u markers", static_cast<unsigned>(span.slots));
        case SpanKind::header:
        case SpanKind::frame:
        case SpanKind::short_frame:
            return format("%u peaks + STOP", span.peaks);
        case SpanKind::eot:
            return format("×%u", static_cast<unsigned>(span.slots));
        case SpanKind::tail:
            return format("%u ms", static_cast<unsigned>(config.tail_ms));
    }
    return "";
}

void figure_timeline(const std::string& directory, const Rendered& r) {
    const EncoderConfig& config = r.config;
    const double rate = config.sample_rate_hz;
    const std::vector<double> wave = to_double(r.audio, config.amplitude);
    const std::vector<Span> list = spans(r);
    const double duration = r.audio.size() / rate;

    const double k_width = 820.0;
    const double k_height = 422.0;
    const double k_left = 60.0;
    const double k_right = 796.0;
    const double k_wave_top = 112.0;
    const double k_wave_bottom = 222.0;
    const double k_strip_top = 232.0;
    const double k_strip_height = 14.0;
    const double k_bracket_y = 254.0;
    const double k_axis_y = 304.0;
    const double k_legend_y = 352.0;
    const double k_time_step_s = 0.2;
    Svg svg(k_width, k_height);
    heading(svg, format("Transmission layout: hf preset, “%s” (%u bytes)", k_example_text,
                        static_cast<unsigned>(text_bytes().size())),
            format("Real Encoder output (48 kHz): T %.0f ms, k %u, N %u, f_ref %u Hz; %u slots + %u ms tail = %.3f s.",
                   slot_ms_of(config), static_cast<unsigned>(config.bits_per_peak),
                   static_cast<unsigned>(config.data_slots), static_cast<unsigned>(config.tone_hz),
                   static_cast<unsigned>(r.slots.size() - 1), static_cast<unsigned>(config.tail_ms), duration),
            format("No lead-in: %u ms on the HF presets (%u ms on the FM presets, for the FM transmitter's delay).",
                   static_cast<unsigned>(config.lead_in_ms),
                   static_cast<unsigned>(EncoderConfig::from_preset(Preset::fm, k_rate_hz).lead_in_ms)));
    const Scale x = {0.0, duration, k_left, k_right};
    const Scale y = {-1.0, 1.0, k_wave_bottom, k_wave_top};
    svg.line(k_left, y(0.0), k_right, y(0.0), k_grid_line, 1.0);
    y_axis(svg, y, k_left, -1.0, 1.0, 1.0, 0);
    svg.text(k_left - 36.0, (k_wave_top + k_wave_bottom) / 2.0, "audio / A", font(k_small_size, k_muted, "middle"),
             -90.0);

    // Waveform: min/max per pixel column, one outline per run of slots of the same kind (no seams between them).
    std::vector<Point> top;
    std::vector<Point> bottom;
    for (size_t s = 0; s < r.slots.size(); ++s) {
        const SlotRun& slot = r.slots[s];
        if (slot.status.kind == SlotKind::silent) continue;
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
            // The outline runs from the slot's first edge to its last, so adjacent slots meet without a gap.
            const double px = columns == 1 ? x0 : x0 + (x1 - x0) * c / (columns - 1);
            const Point p = {px, y(high)};
            const Point q = {px, y(low)};
            top.push_back(p);
            bottom.push_back(q);
        }
        const bool run_ends = s + 1 == r.slots.size() || std::string(slot_color(r.slots[s + 1])) != slot_color(slot);
        if (!run_ends) continue;
        std::vector<Point> outline(top);
        outline.insert(outline.end(), bottom.rbegin(), bottom.rend());
        svg.polygon(outline, slot_color(slot));
        top.clear();
        bottom.clear();
    }

    // Slot strip, marker labels and segment brackets.
    for (size_t s = 0; s < r.slots.size(); ++s) {
        const SlotRun& slot = r.slots[s];
        const double x0 = x(slot.first / rate);
        const double x1 = x((slot.first + slot.count) / rate);
        svg.rect(x0, k_strip_top, x1 - x0, k_strip_height, slot_color(slot), k_card, 0.8);
    }
    const Font marker_font = font(k_small_size, k_purple, "middle", true);
    for (size_t i = 0; i < list.size(); ++i) {
        const Span& span = list[i];
        const double x0 = x(span.first / rate);
        const double x1 = x((span.first + span.samples) / rate);
        const double slot_px = (x1 - x0) / span.slots;
        if (span.kind == SpanKind::sync) svg.text(x1 - slot_px / 2.0, k_wave_top - 8.0, "START", marker_font);
        if (span.kind == SpanKind::header || span.kind == SpanKind::frame || span.kind == SpanKind::short_frame) {
            const bool last = i + 1 < list.size() && list[i + 1].kind == SpanKind::eot;
            // The final STOP and the EOT markers share one label: they are adjacent slots.
            const double eot_px = last ? list[i + 1].slots * slot_px : 0.0;
            svg.text(x1 - slot_px / 2.0 + eot_px / 2.0, k_wave_top - 8.0, last ? "STOP + EOT" : "STOP = START",
                     marker_font);
        }
        const double k_bracket_tick = 4.0;
        const double k_bracket_inset = 1.5;
        svg.line(x0 + k_bracket_inset, k_bracket_y, x1 - k_bracket_inset, k_bracket_y, k_axis, 1.0);
        svg.line(x0 + k_bracket_inset, k_bracket_y, x0 + k_bracket_inset, k_bracket_y - k_bracket_tick, k_axis, 1.0);
        svg.line(x1 - k_bracket_inset, k_bracket_y, x1 - k_bracket_inset, k_bracket_y - k_bracket_tick, k_axis, 1.0);
        svg.text((x0 + x1) / 2.0, k_bracket_y + 15.0, span_name(span), font(k_note_size, k_ink, "middle", true));
        svg.text((x0 + x1) / 2.0, k_bracket_y + 29.0, span_detail(span, config), font(k_small_size, k_muted, "middle"));
    }
    x_axis(svg, x, k_axis_y, 0.0, std::floor(duration / k_time_step_s) * k_time_step_s, k_time_step_s, 1);
    svg.text((k_left + k_right) / 2.0, k_axis_y + 30.0, "time (s)", font(k_small_size, k_muted, "middle"));

    struct Legend {
        const char* color;
        const char* label;
    };
    const Legend legend[] = {{k_purple_light, "tune (f_ref)"},
                             {k_purple, "marker (f_ref, phase flip)"},
                             {k_teal_dark, "header peak (grid tone)"},
                             {k_teal, "data peak (grid tone)"},
                             {k_gray_fill, "silence"}};
    const double k_legend_gap = 28.0;
    const double k_swatch = 12.0;
    const double k_swatch_gap = 6.0;
    double lx = k_left;
    for (size_t i = 0; i < sizeof(legend) / sizeof(legend[0]); ++i) {
        svg.rect(lx, k_legend_y - k_swatch + 2.0, k_swatch, k_swatch, legend[i].color, k_axis, 0.5, 2.0);
        svg.text(lx + k_swatch + k_swatch_gap, k_legend_y, legend[i].label, font(k_note_size, k_ink));
        lx += k_swatch + k_swatch_gap + text_width(legend[i].label, k_note_size) + k_legend_gap;
    }
    const Span& last_frame = list[list.size() - 3];
    const unsigned last_bytes = static_cast<unsigned>(text_bytes().size() % config.frame_bytes());
    svg.text(k_margin, k_legend_y + 32.0,
             "Each STOP is the next frame's START; the last sync marker is the header START.", font(k_note_size, k_muted));
    svg.text(k_margin, k_legend_y + 48.0,
             format("The short final frame carries the last %u bytes in ⌈%u/%u⌉ = %u peaks; its STOP and the "
                    "%u EOT markers end the transmission.",
                    last_bytes, last_bytes * unlimited::k_bits_per_byte, static_cast<unsigned>(config.bits_per_peak),
                    last_frame.peaks, static_cast<unsigned>(unlimited::k_eot_markers)),
             font(k_note_size, k_muted));
    svg.save(path_of(directory, "transmission_timeline.svg"), "Unlimited transmission layout",
             "Envelope of one hf transmission of CQ DE PY2: tune, sync train, header, a data frame, a short final "
             "frame, EOT and tail.");
    std::printf("transmission_timeline.svg: %u slots, %.3f s\n", static_cast<unsigned>(r.slots.size() - 1), duration);
}

// ---------------------------------------------------------------------------
// Figure: spectrogram of the hf transmission
// ---------------------------------------------------------------------------

const double k_low_hz = unlimited::k_min_tone_hz;
const double k_high_hz = unlimited::k_max_tone_hz;

// A vertical bracket at x from y0 to y1 (y0 < y1), opening to the left.
void bracket(Svg& svg, double x, double y0, double y1, const char* color) {
    const double k_arm = 5.0;
    svg.line(x, y0, x, y1, color, 1.5);
    svg.line(x - k_arm, y0, x, y0, color, 1.5);
    svg.line(x - k_arm, y1, x, y1, color, 1.5);
}

void figure_spectrogram(const std::string& directory, const Rendered& r) {
    const EncoderConfig& config = r.config;
    const double rate = config.sample_rate_hz;
    const double duration = r.audio.size() / rate;
    const size_t k_window = 256;
    const size_t k_hop = 16;
    const size_t k_fft = 1024;
    const double k_floor_db = -48.0;
    const Spectrogram s =
        spectrogram(to_double(r.audio, k_full_scale), rate, k_window, k_hop, k_fft, k_low_hz, k_high_hz);

    const double k_width = 820.0;
    const double k_height = 440.0;
    const Box plot = {64.0, 90.0, 540.0, 300.0};
    Svg svg(k_width, k_height);
    heading(svg, format("Spectrogram of the hf transmission “%s”", k_example_text),
            format("Real Encoder audio at 8 kHz; STFT with a %.0f ms Hann window every %.0f ms; power in dB relative "
                   "to the strongest cell.",
                   k_window * k_ms_per_s / rate, k_hop * k_ms_per_s / rate));
    const Scale t = {0.0, duration, plot.left, plot.right()};
    const Scale f = {k_low_hz, k_high_hz, plot.bottom(), plot.top};
    const std::string clip_id = svg.clip(plot.left, plot.top, plot.width, plot.height);
    draw_spectrogram(svg, s, t, f, max_power(s), k_floor_db, clip_id);
    svg.rect(plot.left, plot.top, plot.width, plot.height, "none", k_axis, 1.0);

    const std::vector<Span> list = spans(r);
    for (size_t i = 0; i < list.size(); ++i) {
        const double x0 = t(list[i].first / rate);
        const double x1 = t((list[i].first + list[i].samples) / rate);
        if (i > 0) svg.line(x0, plot.top, x0, plot.bottom(), k_gray, 0.8, "3 3");
        svg.text((x0 + x1) / 2.0, plot.top - 7.0, span_name(list[i]), font(k_small_size, k_ink, "middle", true));
    }
    const double reference = config.tone_hz;
    const double tone0 = tone_hz(config, 0.0);
    const double last_tone = tone_hz(config, tone_count(config) - 1.0);
    const double header_last = tone_hz(config, unlimited::k_header_slots - 1.0, true);
    svg.line(plot.left, f(reference), plot.right(), f(reference), k_purple, 1.0, "6 4");

    const double k_note_x = plot.right() + 22.0;
    const double k_bracket_x = plot.right() + 14.0;
    svg.text(k_note_x, f(reference) - 6.0, format("f_ref %.0f Hz (tune, markers)", reference),
             font(k_note_size, k_purple));
    bracket(svg, k_bracket_x, f(reference), f(tone0), k_amber);
    svg.text(k_note_x, (f(reference) + f(tone0)) / 2.0 + k_text_rise + 4.0,
             format("guard 5/T = %.0f Hz", reference - tone0), font(k_note_size, k_amber, "start", true));
    bracket(svg, k_bracket_x, f(tone0) + 2.0, f(last_tone), k_teal);
    const double grid_mid = (f(tone0) + f(last_tone)) / 2.0;
    svg.text(k_note_x, grid_mid - 2.0 * k_line_gap,
             format("grid: %u tones below f_ref", tone_count(config)), font(k_note_size, k_teal, "start", true));
    svg.text(k_note_x, grid_mid - k_line_gap, format("8/(7T) = %.1f Hz apart", standard_spacing() / slot_s_of(config)),
             font(k_note_size, k_ink));
    svg.text(k_note_x, grid_mid, format("tone 0 at %.0f Hz", tone0), font(k_note_size, k_ink));
    svg.text(k_note_x, grid_mid + k_line_gap, format("tone %u at %.0f Hz", tone_count(config) - 1u, last_tone),
             font(k_note_size, k_ink));
    svg.text(k_note_x, grid_mid + 2.0 * k_line_gap, "header: tones 0–7", font(k_note_size, k_teal_dark));
    svg.text(k_note_x, grid_mid + 3.0 * k_line_gap, format("(%.0f–%.0f Hz)", header_last, tone0),
             font(k_note_size, k_teal_dark));
    color_bar(svg, k_note_x, plot.bottom() - 26.0, 140.0, 10.0, k_floor_db);

    const double k_frequency_step = 500.0;
    y_axis(svg, f, plot.left, k_frequency_step, k_high_hz - k_frequency_step / 2.0, k_frequency_step, 0);
    svg.text(plot.left - 44.0, (plot.top + plot.bottom()) / 2.0, "audio frequency (Hz)",
             font(k_small_size, k_muted, "middle"), -90.0);
    const double k_time_step_s = 0.2;
    x_axis(svg, t, plot.bottom(), 0.0, std::floor(duration / k_time_step_s) * k_time_step_s, k_time_step_s, 1);
    svg.text((plot.left + plot.right()) / 2.0, plot.bottom() + 32.0, "time (s)", font(k_small_size, k_muted, "middle"));
    svg.save(path_of(directory, "spectrogram_hf.svg"), "Unlimited hf spectrogram",
             "Waterfall of one hf transmission: tune at f_ref, sync markers, header staircase and data peaks on the "
             "32-tone grid below f_ref.");
    std::printf("spectrogram_hf.svg: %u x %u cells, f_ref %.0f Hz, tone 0 %.2f Hz, tone %u %.2f Hz\n",
                static_cast<unsigned>(s.columns), static_cast<unsigned>(s.rows), reference, tone0,
                tone_count(config) - 1u, last_tone);
}

// ---------------------------------------------------------------------------
// Figure: tone grid
// ---------------------------------------------------------------------------

const double k_hf_tuning_tolerance_hz = 500.0;  // spec 1.4 preset table, gated by C14 (not derivable from the code)

void arrow_head(Svg& svg, double x, double y, double direction, const char* color) {
    const double k_head_length = 7.0;
    const double k_head_half = 4.0;
    std::vector<Point> head;
    const Point tip = {x, y};
    const Point a = {x - direction * k_head_length, y - k_head_half};
    const Point b = {x - direction * k_head_length, y + k_head_half};
    head.push_back(tip);
    head.push_back(a);
    head.push_back(b);
    svg.polygon(head, color);
}

void figure_tone_grid(const std::string& directory) {
    const EncoderConfig config = EncoderConfig::from_preset(Preset::hf, k_rate_hz);
    const unsigned tones = tone_count(config);
    const double pivot = sim::ChannelConfig().lsb_pivot_hz;
    const double reference = config.tone_hz;
    const double tone0 = tone_hz(config, 0.0);
    const double last_tone = tone_hz(config, tones - 1.0);
    const double spacing_hz = standard_spacing() / slot_s_of(config);

    const double k_width = 820.0;
    const double k_height = 482.0;
    const double k_axis_low_hz = 200.0;
    const double k_axis_high_hz = 2800.0;
    const Scale x = {k_axis_low_hz, k_axis_high_hz, 44.0, 796.0};
    const double k_band_top = 78.0;
    const double k_band_bottom = 390.0;
    const double k_usb_tick_top = 122.0;
    const double k_usb_tick_bottom = 156.0;
    const double k_header_y = 208.0;
    const double k_tolerance_y = 244.0;
    const double k_lsb_tick_top = 318.0;
    const double k_lsb_tick_bottom = 352.0;
    const unsigned k_index_label_step = 8;
    Svg svg(k_width, k_height);
    heading(svg, "The hf tone grid",
            format("T %.0f ms, k %u (M = %u tones), N %u, standard spacing 8/(7T), grid below f_ref; exact grid of "
                   "spec 1.3.",
                   slot_ms_of(config), static_cast<unsigned>(config.bits_per_peak), tones,
                   static_cast<unsigned>(config.data_slots)));
    svg.rect(x(k_low_hz), k_band_top, x(k_high_hz) - x(k_low_hz), k_band_bottom - k_band_top, k_gray_fill);
    svg.text(x(k_low_hz) + 6.0, k_band_top + 14.0,
             format("receiver passband %.0f–%.0f Hz", k_low_hz, k_high_hz), font(k_note_size, k_muted));

    // USB, as sent.
    svg.text(x(k_low_hz) + 6.0, k_usb_tick_top - 2.0, "USB, as sent", font(k_label_size, k_ink, "start", true));
    svg.line(x(reference), k_usb_tick_top - 22.0, x(reference), k_usb_tick_bottom + 6.0, k_purple, 2.5);
    svg.text(x(reference), k_usb_tick_top - 28.0, format("f_ref %.0f Hz", reference),
             font(k_note_size, k_purple, "middle", true));
    for (unsigned n = 0; n < tones; ++n) {
        const double px = x(tone_hz(config, n));
        svg.line(px, k_usb_tick_top, px, k_usb_tick_bottom, k_teal, 1.6);
        if (n % k_index_label_step == 0 || n + 1 == tones)
            svg.text(px, k_usb_tick_bottom + 13.0, format("%u", n), font(k_small_size, k_teal, "middle", true));
    }
    const double guard_y = k_usb_tick_top - 8.0;
    svg.line(x(tone0), guard_y, x(reference), guard_y, k_amber, 1.5);
    arrow_head(svg, x(tone0), guard_y, -1.0, k_amber);
    arrow_head(svg, x(reference), guard_y, 1.0, k_amber);
    svg.text(x(tone0) - 6.0, guard_y + k_text_rise, format("guard 5/T = %.2f Hz", reference - tone0),
             font(k_note_size, k_amber, "end", true));
    svg.text((x(last_tone) + x(tone0)) / 2.0, k_usb_tick_bottom + 30.0,
             format("data tone index n: f_n = f_ref − (5 + 8n/7)/T, spacing 8/(7T) = %.2f Hz", spacing_hz),
             font(k_small_size, k_muted, "middle"));

    // Header tones.
    for (unsigned h = 0; h < unlimited::k_header_slots; ++h)
        svg.circle(x(tone_hz(config, h, true)), k_header_y, 3.5, k_teal_dark);
    svg.text(x(tone_hz(config, unlimited::k_header_slots - 1.0, true)) - 10.0, k_header_y + k_text_rise,
             "header tones h = 0–7 (standard grid in every mode)", font(k_note_size, k_teal_dark, "end"));

    // Occupied band and tuning tolerance.
    svg.rect(x(last_tone), k_tolerance_y - 5.0, x(reference) - x(last_tone), 10.0, k_teal_fill, k_teal, 1.0, 2.0);
    svg.text((x(last_tone) + x(reference)) / 2.0, k_tolerance_y + k_text_rise,
             format("occupied %.0f–%.0f Hz (%.0f Hz), centred on %u Hz", last_tone, reference, reference - last_tone,
                    static_cast<unsigned>(unlimited::k_band_centre_hz)),
             font(k_small_size, k_ink, "middle"));
    const double low_limit = last_tone - k_hf_tuning_tolerance_hz;
    const double high_limit = reference + k_hf_tuning_tolerance_hz;
    svg.line(x(low_limit), k_tolerance_y, x(last_tone), k_tolerance_y, k_amber, 1.5, "4 3");
    svg.line(x(reference), k_tolerance_y, x(high_limit), k_tolerance_y, k_amber, 1.5, "4 3");
    arrow_head(svg, x(low_limit), k_tolerance_y, -1.0, k_amber);
    arrow_head(svg, x(high_limit), k_tolerance_y, 1.0, k_amber);
    svg.text(x(1500.0), k_tolerance_y + 22.0,
             format("tuning tolerance ±%.0f Hz (spec 1.4): the whole band stays in %.0f–%.0f Hz (room: %.0f Hz below, "
                    "%.0f Hz above)",
                    k_hf_tuning_tolerance_hz, k_low_hz, k_high_hz, last_tone - k_low_hz, k_high_hz - reference),
             font(k_small_size, k_amber, "middle"));

    // LSB: the receiver hears pivot - f.
    const double lsb_reference = pivot - reference;
    svg.text(x(k_low_hz) + 6.0, k_lsb_tick_top - 22.0,
             format("LSB receiver: f → %.0f − f", pivot), font(k_label_size, k_ink, "start", true));
    svg.line(x(lsb_reference), k_lsb_tick_top - 10.0, x(lsb_reference), k_lsb_tick_bottom + 6.0, k_purple, 2.5);
    svg.text(x(lsb_reference) - 6.0, k_lsb_tick_top, format("f_ref %.0f Hz", lsb_reference),
             font(k_note_size, k_purple, "end", true));
    for (unsigned n = 0; n < tones; ++n) {
        const double px = x(pivot - tone_hz(config, n));
        svg.line(px, k_lsb_tick_top, px, k_lsb_tick_bottom, k_teal, 1.6);
        if (n % k_index_label_step == 0 || n + 1 == tones)
            svg.text(px, k_lsb_tick_bottom + 13.0, format("%u", n), font(k_small_size, k_teal, "middle", true));
    }
    svg.text(x(pivot - last_tone) + 12.0, (k_lsb_tick_top + k_lsb_tick_bottom) / 2.0 + k_text_rise,
             "grid above f_ref: the header", font(k_note_size, k_ink));
    svg.text(x(pivot - last_tone) + 12.0, (k_lsb_tick_top + k_lsb_tick_bottom) / 2.0 + k_text_rise + k_line_gap,
             "tells the decoder (σ_rx = +1)", font(k_note_size, k_ink));

    const double k_axis_y = k_band_bottom + 8.0;
    const double k_tick_hz = 250.0;
    const double k_label_hz = 500.0;
    x_axis(svg, x, k_axis_y, k_label_hz, k_axis_high_hz - k_label_hz / 2.0, k_tick_hz, 0,
           static_cast<size_t>(k_label_hz / k_tick_hz));
    svg.text((x.to0 + x.to1) / 2.0, k_axis_y + 30.0, "audio frequency (Hz)", font(k_small_size, k_muted, "middle"));
    svg.text(k_margin, k_height - 30.0, "LSB mirrors the audio, so the grid lands on the other side of f_ref.",
             font(k_note_size, k_muted));
    svg.text(k_margin, k_height - 14.0,
             "The tone index is the distance from f_ref, so the decoder only flips σ_rx; the header's ML decides it.",
             font(k_note_size, k_muted));
    svg.save(path_of(directory, "tone_grid.svg"), "Unlimited hf tone grid",
             "Frequency layout of the hf preset: passband, f_ref, guard, 32 data tones, header tones, tuning "
             "tolerance, and the LSB mirror.");
    std::printf("tone_grid.svg: f_ref %.0f Hz, guard %.2f Hz, spacing %.2f Hz, band %.1f-%.1f Hz, LSB f_ref %.0f Hz\n",
                reference, reference - tone0, spacing_hz, last_tone, reference, lsb_reference);
}

// ---------------------------------------------------------------------------
// Figure: frame mapping (spec 1.5 vector)
// ---------------------------------------------------------------------------

std::string binary(unsigned value, unsigned bits) {
    std::string text;
    for (unsigned b = bits; b-- > 0;) text += ((value >> b) & 1u) != 0 ? '1' : '0';
    return text;
}

void figure_frame_mapping(const std::string& directory) {
    const uint8_t k_bytes[] = {0x48, 0x69, 0x21, 0x00, 0xFF};
    const uint8_t k_bits_per_peak = 5;
    const size_t byte_count = sizeof(k_bytes);
    const EncoderConfig config = EncoderConfig::from_preset(Preset::hf, k_rate_hz);
    if (config.bits_per_peak != k_bits_per_peak) throw std::runtime_error("the hf preset is no longer k = 5");
    const unsigned k = k_bits_per_peak;
    const unsigned tones = 1u << k;
    const unsigned slots = config.data_slots;
    const unsigned r = unlimited::tone_rotation(k_bits_per_peak);
    const unsigned bit_count = static_cast<unsigned>(byte_count * unlimited::k_bits_per_byte);

    std::vector<unsigned> bits(bit_count);
    for (unsigned b = 0; b < bit_count; ++b) {
        const unsigned shift = unlimited::k_bits_per_byte - 1u - b % unlimited::k_bits_per_byte;
        bits[b] = (k_bytes[b / unlimited::k_bits_per_byte] >> shift) & 1u;
    }
    std::vector<unsigned> symbols(slots, 0);
    std::vector<unsigned> tone(slots, 0);
    for (unsigned i = 0; i < slots; ++i) {
        for (unsigned b = 0; b < k; ++b) symbols[i] = (symbols[i] << 1) | bits[i * k + b];
        tone[i] = unlimited::peak_tone(static_cast<uint8_t>(symbols[i]), static_cast<uint8_t>(i), k_bits_per_peak);
        const unsigned expected = (unlimited::gray_decode(static_cast<uint8_t>(symbols[i])) + i * r) % tones;
        if (tone[i] != expected ||
            unlimited::peak_symbol(static_cast<uint8_t>(tone[i]), static_cast<uint8_t>(i), k_bits_per_peak) != symbols[i])
            throw std::runtime_error("peak_tone/peak_symbol disagree with spec 1.5");
    }

    const double k_width = 820.0;
    const double k_height = 462.0;
    const double k_cells_left = 96.0;
    const double k_cell = 17.5;
    const double k_byte_row = 86.0;
    const double k_row_height = 22.0;
    const double k_bit_row = k_byte_row + k_row_height + 12.0;
    const double k_table_top = 206.0;
    const double k_label_left = k_margin;
    const double k_column_left = 170.0;
    const double k_column = 62.0;
    Svg svg(k_width, k_height);
    heading(svg, format("Frame mapping: the spec 1.5 vector (k %u, N %u)", k, slots),
            format("%u bytes → %u-bit string, MSB first → %u symbols of k bits → gray⁻¹ → + (i − 1)·r mod %u (r = %u) → "
                   "tone index n_i",
                   static_cast<unsigned>(byte_count), bit_count, slots, tones, r));
    const Font cell_font = font(k_note_size, k_ink, "middle");
    const Font row_font = font(k_note_size, k_ink, "start", true);

    svg.text(k_label_left, k_byte_row + 15.0, "bytes", row_font);
    for (size_t b = 0; b < byte_count; ++b) {
        const double x0 = k_cells_left + b * unlimited::k_bits_per_byte * k_cell;
        svg.rect(x0, k_byte_row, unlimited::k_bits_per_byte * k_cell, k_row_height, k_gray_fill, k_axis, 1.0);
        svg.text(x0 + unlimited::k_bits_per_byte * k_cell / 2.0, k_byte_row + 15.0, format("0x%02X", k_bytes[b]),
                 font(k_note_size, k_ink, "middle", true));
    }
    svg.text(k_label_left, k_bit_row + 15.0, "bit string", row_font);
    for (unsigned b = 0; b < bit_count; ++b) {
        const unsigned group = b / k;
        const double x0 = k_cells_left + b * k_cell;
        svg.rect(x0, k_bit_row, k_cell, k_row_height, group % 2 == 0 ? k_teal_fill : k_purple_fill, k_card, 1.0);
        svg.text(x0 + k_cell / 2.0, k_bit_row + 15.0, format("%u", bits[b]), cell_font);
    }
    for (unsigned i = 0; i < slots; ++i) {
        const double x0 = k_cells_left + i * k * k_cell;
        const double x1 = x0 + k * k_cell;
        const double y = k_bit_row + k_row_height + 6.0;
        svg.line(x0 + 2.0, y, x1 - 2.0, y, k_axis, 1.0);
        svg.text((x0 + x1) / 2.0, y + 15.0, format("s%u = %u", i + 1, symbols[i]), cell_font);
    }

    // Table: START, the N slots, STOP.
    struct Row {
        const char* label;
        std::function<std::string(unsigned)> value;
    };
    const EncoderConfig& hf = config;
    const Row rows[] = {
        {"slot i", [&](unsigned i) { return format("%u", i + 1); }},
        {"bits", [&](unsigned i) { return binary(symbols[i], k); }},
        {"symbol s_i", [&](unsigned i) { return format("%u", symbols[i]); }},
        {"gray⁻¹(s_i)", [&](unsigned i) { return format("%u", unlimited::gray_decode(static_cast<uint8_t>(symbols[i]))); }},
        {"+ (i−1)·r mod 32", [&](unsigned i) { return format("+%u", (i * r) % tones); }},
        {"tone n_i", [&](unsigned i) { return format("%u", tone[i]); }},
        {"Hz (hf)", [&](unsigned i) { return format("%.1f", tone_hz(hf, tone[i])); }},
    };
    const size_t row_count = sizeof(rows) / sizeof(rows[0]);
    const double table_bottom = k_table_top + row_count * k_row_height;
    const double start_x = k_column_left;
    const double stop_x = k_column_left + (slots + 1) * k_column;
    for (size_t row = 0; row < row_count; ++row) {
        const double y = k_table_top + row * k_row_height;
        const bool strong = row + 2 == row_count;
        svg.text(k_label_left, y + 15.0, rows[row].label, font(k_note_size, k_ink, "start", strong || row == 0));
        for (unsigned i = 0; i < slots; ++i) {
            const double x0 = k_column_left + (i + 1) * k_column;
            svg.rect(x0, y, k_column, k_row_height, row == 0 ? k_gray_fill : (strong ? k_teal_fill : k_card),
                     k_card_border, 1.0);
            svg.text(x0 + k_column / 2.0, y + 15.0, rows[row].value(i),
                     font(k_note_size, strong ? k_teal_dark : k_ink, "middle", strong || row == 0));
        }
    }
    const double table_height = table_bottom - k_table_top;
    for (size_t m = 0; m < 2; ++m) {
        const double x0 = m == 0 ? start_x : stop_x;
        svg.rect(x0, k_table_top, k_column, table_height, k_purple_fill, k_purple, 1.2);
        const double mid = x0 + k_column / 2.0;
        svg.text(mid, k_table_top + table_height / 2.0 - 8.0, m == 0 ? "START" : "STOP",
                 font(k_note_size, k_purple, "middle", true));
        svg.text(mid, k_table_top + table_height / 2.0 + 8.0, "f_ref", font(k_small_size, k_purple, "middle"));
        svg.text(mid, k_table_top + table_height / 2.0 + 22.0, format("%u Hz", static_cast<unsigned>(hf.tone_hz)),
                 font(k_small_size, k_purple, "middle"));
    }
    const char* const notes[] = {
        ("Receiver: s_i = gray((n_i − (i − 1)·r) mod M) = peak_symbol(n_i, i − 1, k). De-rotated "
         "neighbouring tones differ in one bit,"),
        "so an adjacent-tone error costs one bit; the rotation spreads repeated bytes over the band.",
        "Every value is computed by the library: peak_tone, peak_symbol, gray_decode, tone_rotation."};
    for (size_t n = 0; n < sizeof(notes) / sizeof(notes[0]); ++n)
        svg.text(k_margin, table_bottom + 30.0 + n * k_line_gap, notes[n], font(k_note_size, k_muted));
    svg.text(k_margin, table_bottom + 30.0 + 3.0 * k_line_gap,
             format("Frequencies for the hf preset: f_n = f_ref − (5 + 8n/7)/T with f_ref %u Hz, T %.0f ms.",
                    static_cast<unsigned>(hf.tone_hz), slot_ms_of(hf)),
             font(k_note_size, k_muted));
    svg.save(path_of(directory, "frame_mapping.svg"), "Unlimited frame mapping",
             "Bytes 48 69 21 00 FF to bit string, 5-bit symbols, Gray and rotation, and tone indexes 14 6 2 11 23 25 3 "
             "24 between START and STOP.");
    std::string list;
    for (unsigned i = 0; i < slots; ++i) list += format("%s%u", i == 0 ? "" : " ", tone[i]);
    std::printf("frame_mapping.svg: tones %s\n", list.c_str());
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

struct DecodeResult {
    std::string text;
    size_t bytes_sent = 0;
    size_t bytes_correct = 0;
    size_t bit_errors = 0;
    bool locked = false;
    int side = 0;
    double snr_db = 0.0;
};

DecodeResult decode(const lb::Recording& recording, const std::vector<int16_t>& received, Profile profile) {
    const uint8_t k_first_printable = 0x20;
    const uint8_t k_last_printable = 0x7E;
    const lb::Capture capture = lb::run_decoder(received, DecoderConfig::for_profile(profile), k_decoder_chunk);
    const lb::Mapping mapping = lb::map_events(recording, capture);
    DecodeResult result;
    const std::vector<uint8_t>& sent = recording.transmissions[0].data;
    result.bytes_sent = sent.size();
    for (size_t k = 0; k < sent.size(); ++k) {
        const int value = mapping.received[0][k];
        if (value < 0) {
            result.text += '_';
            continue;
        }
        if (value == sent[k]) ++result.bytes_correct;
        const bool printable = value >= k_first_printable && value <= k_last_printable;
        result.text += printable ? static_cast<char>(value) : '?';
    }
    result.bit_errors = mapping.score.bit_errors;
    for (size_t i = 0; i < capture.events.size(); ++i) {
        const Event& e = capture.events[i];
        if (e.type == EventType::locked && !result.locked) {
            result.locked = true;
            result.side = e.side;
        }
        if (e.type == EventType::byte) result.snr_db = e.snr_db;
    }
    return result;
}

std::vector<ChannelCase> channel_cases() {
    const double k_ssb_snr_db = 10.0;
    const double k_usb_shift_hz = 150.0;
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
    c.title = format("USB, tuned %+.0f Hz", k_usb_shift_hz);
    c.condition = format("SNR %.0f dB, passband 300–2700 Hz", k_ssb_snr_db);
    cases.push_back(c);

    c.channel = sim::ChannelConfig();
    c.channel.mode = sim::Mode::lsb;
    c.channel.snr_db = k_ssb_snr_db;
    c.title = "LSB (inverted audio)";
    c.condition = format("SNR %.0f dB, f → %.0f − f", k_ssb_snr_db, c.channel.lsb_pivot_hz);
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
    const EncoderConfig config = EncoderConfig::from_preset(Preset::hf, k_rate_hz);
    const double k_silence_ms = 300.0;
    lb::Recording recording;
    lb::append_silence(recording, k_silence_ms);
    lb::append_transmission(recording, text_bytes(), config);
    lb::append_silence(recording, k_silence_ms);
    const double rate = k_rate_hz;
    const double duration = recording.samples.size() / rate;
    const std::vector<ChannelCase> cases = channel_cases();

    const double k_width = 820.0;
    const double k_height = 620.0;
    const size_t k_columns = 3;
    const double k_tile_left = 18.0;
    const double k_tile_top = 82.0;
    const double k_tile_width = 256.0;
    const double k_tile_height = 236.0;
    const double k_tile_gap = 10.0;
    const double k_image_inset = 36.0;
    const double k_image_top = 46.0;
    const double k_image_width = 210.0;
    const double k_image_height = 124.0;
    const size_t k_window = 256;
    const size_t k_hop = 64;
    const size_t k_fft = 512;
    const double k_floor_db = -40.0;
    Svg svg(k_width, k_height);
    heading(svg, "One hf transmission through six simulated channels",
            format("“%s” (%u bytes, hf preset) through sim::Channel; spectrograms %.0f–%.0f Hz of the received "
                   "audio (%.1f s each);",
                   k_example_text, static_cast<unsigned>(text_bytes().size()), k_low_hz, k_high_hz, duration),
            "every tile is decoded by the real Decoder with the profile shown; the result is what it released.");
    for (size_t i = 0; i < cases.size(); ++i) {
        const ChannelCase& c = cases[i];
        const std::vector<int16_t> received =
            through_channel(recording.samples, c.channel, config.amplitude, c.peak_factor);
        const DecodeResult result = decode(recording, received, c.profile);
        const Spectrogram s =
            spectrogram(to_double(received, k_full_scale), rate, k_window, k_hop, k_fft, k_low_hz, k_high_hz);

        const double x0 = k_tile_left + (i % k_columns) * (k_tile_width + k_tile_gap);
        const double y0 = k_tile_top + (i / k_columns) * (k_tile_height + k_tile_gap);
        svg.rect(x0, y0, k_tile_width, k_tile_height, k_card, k_card_border, 1.0, 8.0);
        svg.text(x0 + 10.0, y0 + 18.0, c.title, font(k_label_size, k_ink, "start", true));
        svg.text(x0 + 10.0, y0 + 34.0, c.condition, font(k_small_size, k_muted));
        const Box plot = {x0 + k_image_inset, y0 + k_image_top, k_image_width, k_image_height};
        const Scale t = {0.0, duration, plot.left, plot.right()};
        const Scale f = {k_low_hz, k_high_hz, plot.bottom(), plot.top};
        const std::string clip_id = svg.clip(plot.left, plot.top, plot.width, plot.height);
        draw_spectrogram(svg, s, t, f, max_power(s), k_floor_db, clip_id);
        svg.rect(plot.left, plot.top, plot.width, plot.height, "none", k_axis, 0.8);
        const double k_label_hz[] = {500.0, 1500.0, 2500.0};
        for (size_t l = 0; l < sizeof(k_label_hz) / sizeof(k_label_hz[0]); ++l) {
            svg.line(plot.left - 3.0, f(k_label_hz[l]), plot.left, f(k_label_hz[l]), k_axis, 1.0);
            svg.text(plot.left - 5.0, f(k_label_hz[l]) + 3.5, format("%.0f", k_label_hz[l]),
                     font(k_small_size - 1.0, k_muted, "end"));
        }
        const bool ok = result.bytes_correct == result.bytes_sent;
        const double text_y = plot.bottom() + 20.0;
        svg.text(x0 + 10.0, text_y,
                 format("“%s” %s", result.text.c_str(), ok ? "decoded OK" : "decoded with losses"),
                 font(k_note_size, ok ? k_teal_dark : k_coral, "start", true));
        svg.text(x0 + 10.0, text_y + 16.0,
                 format("%u/%u bytes, %u bit errors, %s profile", static_cast<unsigned>(result.bytes_correct),
                        static_cast<unsigned>(result.bytes_sent), static_cast<unsigned>(result.bit_errors),
                        c.profile_name),
                 font(k_small_size, k_ink));
        svg.text(x0 + 10.0, text_y + 30.0,
                 result.locked ? format("grid %s f_ref, SNR est. %.1f dB", result.side > 0 ? "above" : "below",
                                        result.snr_db)
                               : std::string("no lock"),
                 font(k_small_size, k_muted));
        std::printf("channels.svg: %-22s \"%s\" %u/%u bytes correct, %u bit errors, grid %s, SNR est %.1f dB\n",
                    c.title.c_str(), result.text.c_str(), static_cast<unsigned>(result.bytes_correct),
                    static_cast<unsigned>(result.bytes_sent), static_cast<unsigned>(result.bit_errors),
                    result.side > 0 ? "above" : "below", result.snr_db);
    }
    svg.text(k_margin, k_height - 30.0,
             "SNR: key-down tone power over noise in 2500 Hz (AM: carrier power; FM: CNR in 12.5 kHz). The receiver "
             "sets only its profile;",
             font(k_note_size, k_muted));
    svg.text(k_margin, k_height - 14.0,
             "f_ref, T, k, N and the grid side come from the tune tone, the markers and the 8-slot header.",
             font(k_note_size, k_muted));
    svg.save(path_of(directory, "channels.svg"), "Unlimited over six channels",
             "Spectrograms of one hf transmission after clean, USB shifted, LSB, AM, FM and CCIR poor channels, each "
             "with the real decoder's result.");
}

// ---------------------------------------------------------------------------
// Figure: wideband vs narrowband envelope at 0 dB
// ---------------------------------------------------------------------------

const double k_narrow_bandwidth_hz = 60.0;
const double k_hann_enbw_bins = 1.5;  // equivalent noise bandwidth of a Hann window, in 1/N

// Hann-weighted mean of x^2 around each sample: RMS over the window.
std::vector<double> rms_envelope(const std::vector<float>& x, const std::vector<double>& w) {
    double weight = 0.0;
    for (size_t j = 0; j < w.size(); ++j) weight += w[j];
    std::vector<double> out(x.size(), 0.0);
    const long half = static_cast<long>(w.size() / 2);
    for (size_t i = 0; i < x.size(); ++i) {
        double sum = 0.0;
        for (size_t j = 0; j < w.size(); ++j) {
            const long n = static_cast<long>(i) - half + static_cast<long>(j);
            if (n >= 0 && n < static_cast<long>(x.size())) sum += w[j] * square(x[static_cast<size_t>(n)]);
        }
        out[i] = std::sqrt(sum / weight);
    }
    return out;
}

// Magnitude of the Hann-weighted mean of x e^{-j w n}: the complex amplitude at `hz`, times 2 (a sine of amplitude
// A reads A).
std::vector<double> narrow_envelope(const std::vector<float>& x, const std::vector<double>& w, double hz, double rate) {
    double weight = 0.0;
    for (size_t j = 0; j < w.size(); ++j) weight += w[j];
    const double omega = 2.0 * k_pi * hz / rate;
    std::vector<Complex> mixed(x.size());
    for (size_t n = 0; n < x.size(); ++n) mixed[n] = static_cast<double>(x[n]) * std::polar(1.0, -omega * n);
    std::vector<double> out(x.size(), 0.0);
    const long half = static_cast<long>(w.size() / 2);
    for (size_t i = 0; i < x.size(); ++i) {
        Complex sum(0.0, 0.0);
        for (size_t j = 0; j < w.size(); ++j) {
            const long n = static_cast<long>(i) - half + static_cast<long>(j);
            if (n >= 0 && n < static_cast<long>(x.size())) sum += w[j] * mixed[static_cast<size_t>(n)];
        }
        out[i] = 2.0 * std::abs(sum) / weight;
    }
    return out;
}

void figure_envelopes(const std::string& directory, const Rendered& r) {
    const EncoderConfig& config = r.config;
    const double rate = config.sample_rate_hz;
    const double k_snr_db = 0.0;
    const double k_before_ms = 150.0;
    const uint32_t k_seed = 7;
    const size_t k_header_end_slot = 1;  // slots shown after the header STOP
    const size_t lead = static_cast<size_t>(k_before_ms * rate / k_ms_per_s);
    std::vector<int16_t> padded(lead, 0);
    padded.insert(padded.end(), r.audio.begin(), r.audio.end());

    sim::ChannelConfig channel;
    channel.mode = sim::Mode::usb;
    channel.snr_db = k_snr_db;
    channel.seed = k_seed;
    channel.signal_level = config.amplitude / k_full_scale;
    const long delay = lb::channel_delay(padded, channel, config.amplitude);
    std::vector<float> in(padded.size());
    for (size_t i = 0; i < padded.size(); ++i) in[i] = static_cast<float>(padded[i] / k_full_scale);
    sim::Channel noisy_channel(channel);
    const std::vector<float> noisy = noisy_channel.process(in);
    channel.noise = false;
    sim::Channel clean_channel(channel);
    const std::vector<float> clean = clean_channel.process(in);

    const size_t window = static_cast<size_t>(std::lround(k_hann_enbw_bins * rate / k_narrow_bandwidth_hz));
    const std::vector<double> w = hann(window);
    const double level = channel.signal_level;
    std::vector<double> wide_noisy = rms_envelope(noisy, w);
    std::vector<double> wide_clean = rms_envelope(clean, w);
    std::vector<double> narrow_noisy = narrow_envelope(noisy, w, config.tone_hz, rate);
    std::vector<double> narrow_clean = narrow_envelope(clean, w, config.tone_hz, rate);
    const double rms_to_peak = std::sqrt(2.0);
    for (size_t i = 0; i < wide_noisy.size(); ++i) {
        wide_noisy[i] *= rms_to_peak / level;
        wide_clean[i] *= rms_to_peak / level;
        narrow_noisy[i] /= level;
        narrow_clean[i] /= level;
    }

    // Shown: from the silence before the tune to one slot after the header STOP.
    const std::vector<Span> list = spans(r);
    size_t end_sample = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].kind == SpanKind::header) end_sample = list[i].first + list[i].samples;
    }
    end_sample += k_header_end_slot * r.slots[0].count;
    const double t0 = -k_before_ms;
    const double t1 = end_sample * k_ms_per_s / rate;

    const double k_width = 820.0;
    const double k_height = 490.0;
    const double k_left = 70.0;
    const double k_right = 796.0;
    const double k_top_panel = 116.0;
    const double k_panel_height = 118.0;
    const double k_panel_gap = 48.0;
    const double k_y_max = 1.85;
    const double k_y_step = 0.5;
    const double k_time_step_ms = 100.0;
    Svg svg(k_width, k_height);
    heading(svg, format("Why the decoder works per band: %.0f dB SNR", k_snr_db),
            format("hf transmission (silence, tune, sync, header) + AWGN at %.0f dB (key-down tone in 2500 Hz), USB "
                   "receiver %.0f–%.0f Hz.",
                   k_snr_db, k_low_hz, k_high_hz),
            format("Both envelopes use the same %.0f ms Hann window (%.0f Hz noise bandwidth); 1.0 = the key-down "
                   "tone's amplitude A.",
                   window * k_ms_per_s / rate, k_narrow_bandwidth_hz));
    const Scale x = {t0, t1, k_left, k_right};
    const size_t k_decimation = 4;  // about two points per pixel; the envelopes are smooth over 25 ms
    const struct Panel {
        const char* title;
        const std::vector<double>* noisy;
        const std::vector<double>* clean;
        double top;
    } panels[] = {{"whole band (2.4 kHz): RMS envelope", &wide_noisy, &wide_clean, k_top_panel},
                  {"narrow band (60 Hz) at f_ref: magnitude", &narrow_noisy, &narrow_clean,
                   k_top_panel + k_panel_height + k_panel_gap}};
    for (size_t p = 0; p < 2; ++p) {
        const Panel& panel = panels[p];
        const Scale y = {0.0, k_y_max, panel.top + k_panel_height, panel.top};
        svg.text(k_left, panel.top - 10.0,
                 p == 0 ? std::string(panel.title)
                        : format("narrow band (%.0f Hz) at f_ref %u Hz: magnitude", k_narrow_bandwidth_hz,
                                 static_cast<unsigned>(config.tone_hz)),
                 font(k_label_size, k_ink, "start", true));
        y_axis(svg, y, k_left, 0.0, 1.5, k_y_step, 1, k_right);
        for (size_t i = 0; i < list.size(); ++i) {
            const double t = list[i].first * k_ms_per_s / rate;
            if (t > t1) break;
            svg.line(x(t), panel.top, x(t), panel.top + k_panel_height, k_gray, 0.8, "3 3");
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
        svg.polyline(noisy_points, k_coral, 1.3);
        svg.polyline(clean_points, k_ink, 1.3, "5 3");
    }
    // Segment names at the top of the first panel, above the traces.
    const double names_y = k_top_panel + 12.0;
    const double k_min_name_ms = 60.0;
    for (size_t i = 0; i < list.size(); ++i) {
        const double a = std::max(t0, list[i].first * k_ms_per_s / rate);
        const double b = std::min(t1, (list[i].first + list[i].samples) * k_ms_per_s / rate);
        if (b - a < k_min_name_ms) continue;
        svg.text(x((a + b) / 2.0), names_y, span_name(list[i]), font(k_small_size, k_muted, "middle", true));
    }
    svg.text(x(t0 + k_before_ms / 2.0), names_y, "silence", font(k_small_size, k_muted, "middle", true));
    // Legend.
    const double legend_y = k_top_panel - 10.0;
    svg.line(k_right - 190.0, legend_y - 4.0, k_right - 166.0, legend_y - 4.0, k_coral, 2.0);
    svg.text(k_right - 160.0, legend_y, format("with noise (%.0f dB)", k_snr_db), font(k_note_size, k_ink));
    svg.line(k_right - 60.0, legend_y - 4.0, k_right - 36.0, legend_y - 4.0, k_ink, 1.5, "5 3");
    svg.text(k_right - 30.0, legend_y, "clean", font(k_note_size, k_ink));
    const double bottom = k_top_panel + 2.0 * k_panel_height + k_panel_gap;
    x_axis(svg, x, bottom, std::ceil(t0 / k_time_step_ms) * k_time_step_ms,
           std::floor(t1 / k_time_step_ms) * k_time_step_ms, k_time_step_ms, 0);
    svg.text((k_left + k_right) / 2.0, bottom + 30.0, "time (ms), 0 = start of the tune",
             font(k_small_size, k_muted, "middle"));
    svg.text(k_margin, k_height - 32.0,
             format("In 2.4 kHz the noise is as strong as the tone; in %.0f Hz it is %.0f dB weaker (%.0f/%.0f).",
                    k_narrow_bandwidth_hz, db_power(k_receiver_bandwidth_hz / k_narrow_bandwidth_hz),
                    k_receiver_bandwidth_hz, k_narrow_bandwidth_hz),
             font(k_note_size, k_muted));
    svg.text(k_margin, k_height - 16.0,
             "The decoder measures f_ref and every grid tone in its own narrow band (a Goertzel slot bank), never one "
             "wideband envelope.",
             font(k_note_size, k_muted));
    svg.save(path_of(directory, "envelope_wide_vs_narrow.svg"), "Unlimited envelopes at 0 dB SNR",
             "Whole-band RMS envelope against a 60 Hz narrow-band envelope at f_ref, with and without noise at 0 dB "
             "SNR.");
    std::printf("envelope_wide_vs_narrow.svg: SNR %.0f dB, window %u samples, channel delay %ld samples\n", k_snr_db,
                static_cast<unsigned>(window), delay);
}

// ---------------------------------------------------------------------------
// Figure: measured spectrum of each preset
// ---------------------------------------------------------------------------

struct PresetStyle {
    Preset preset;
    const char* name;
    const char* color;
};

const PresetStyle k_spectrum_presets[] = {{Preset::hf_fast, "hf_fast", k_coral},
                                          {Preset::hf, "hf", k_teal},
                                          {Preset::hf_robust, "hf_robust", k_purple},
                                          {Preset::hf_weak, "hf_weak", k_blue},
                                          {Preset::fm, "fm", k_pink}};

void figure_presets_spectrum(const std::string& directory) {
    const size_t k_bytes = 256;
    const size_t k_fft = 1024;
    const double k_floor_db = -60.0;
    const double k_obw_fraction = 0.99;
    const double k_max_hz = 3500.0;
    const double rate = k_rate_hz;
    const size_t count = sizeof(k_spectrum_presets) / sizeof(k_spectrum_presets[0]);

    const double k_width = 820.0;
    const double k_height = 566.0;
    const double k_left = 150.0;
    const double k_right = 792.0;
    const double k_top = 100.0;
    const double k_row_height = 66.0;
    const double k_row_gap = 22.0;
    Svg svg(k_width, k_height);
    heading(svg, "Measured spectrum of each preset",
            format("Welch PSD of the data frames of %u random bytes, real Encoder at 8 kHz, %.1f Hz resolution.",
                   static_cast<unsigned>(k_bytes), rate / k_fft),
            "Shaded: the 99% occupied bandwidth (0.5% of the power below it, 0.5% above).");
    const Scale x = {0.0, k_max_hz, k_left, k_right};
    for (size_t p = 0; p < count; ++p) {
        const EncoderConfig config = EncoderConfig::from_preset(k_spectrum_presets[p].preset, k_rate_hz);
        const Rendered r = render(lb::random_bytes(k_bytes, static_cast<uint32_t>(p + 1)), config);
        size_t first = r.audio.size();
        size_t last = 0;
        for (size_t s = 0; s < r.slots.size(); ++s) {
            if (r.slots[s].status.segment != EncoderSegment::frame) continue;
            first = std::min(first, r.slots[s].first);
            last = std::max(last, r.slots[s].first + r.slots[s].count);
        }
        const std::vector<double> all = to_double(r.audio, k_full_scale);
        const std::vector<double> frames(all.begin() + first, all.begin() + last);
        const std::vector<double> psd = welch_psd(frames, k_fft);
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

        const double top = k_top + p * (k_row_height + k_row_gap);
        const Scale y = {k_floor_db, 0.0, top + k_row_height, top};
        svg.rect(x(k_low_hz), top, x(k_high_hz) - x(k_low_hz), k_row_height, k_gray_fill);
        svg.rect(x(low_hz), top, x(high_hz) - x(low_hz), k_row_height, k_spectrum_presets[p].color, "none", 0.0, 0.0,
                 0.14);
        svg.line(x(low_hz), top, x(low_hz), top + k_row_height, k_spectrum_presets[p].color, 1.0, "3 2");
        svg.line(x(high_hz), top, x(high_hz), top + k_row_height, k_spectrum_presets[p].color, 1.0, "3 2");
        svg.rect(k_left, top, k_right - k_left, k_row_height, "none", k_axis, 0.8);
        std::vector<Point> line;
        for (size_t k = 0; k < psd.size(); ++k) {
            const double hz = k * bin_hz;
            if (hz > k_max_hz) break;
            const double db = std::max(k_floor_db, db_power(std::max(psd[k], 1e-30) / peak));
            const Point point = {x(hz), y(db)};
            line.push_back(point);
        }
        svg.polyline(line, k_spectrum_presets[p].color, 1.1);
        svg.text(k_left - 4.0, top + 9.0, "0", font(k_small_size - 1.0, k_muted, "end"));
        svg.text(k_left - 4.0, top + k_row_height, number(k_floor_db, 0), font(k_small_size - 1.0, k_muted, "end"));
        svg.text(k_margin, top + 14.0, k_spectrum_presets[p].name,
                 font(k_label_size, k_spectrum_presets[p].color, "start", true));
        svg.text(k_margin, top + 30.0,
                 format("T %.0f ms, k %u", slot_ms_of(config), static_cast<unsigned>(config.bits_per_peak)),
                 font(k_small_size, k_muted));
        svg.text(k_margin, top + 44.0, format("%.1f bit/s net", net_bit_rate(config)), font(k_small_size, k_muted));
        if (k_spectrum_presets[p].preset == Preset::fm)
            svg.text(k_margin, top + 58.0, "FM only (spec G1)", font(k_small_size, k_muted));
        const std::string obw =
            format("99%% bandwidth %.0f–%.0f Hz (%.2f kHz)", low_hz, high_hz, (high_hz - low_hz) / k_ms_per_s);
        svg.text(k_right, top - 6.0, obw, font(k_small_size, k_spectrum_presets[p].color, "end", true));
        svg.line(x(config.tone_hz), top + k_row_height - 8.0, x(config.tone_hz), top + k_row_height, k_purple, 2.0);
        std::printf("presets_spectrum.svg: %-9s 99%% bandwidth %.0f-%.0f Hz (%.0f Hz), f_ref %u Hz\n",
                    k_spectrum_presets[p].name, low_hz, high_hz, high_hz - low_hz, static_cast<unsigned>(config.tone_hz));
    }
    const double bottom = k_top + count * (k_row_height + k_row_gap) - k_row_gap;
    const double k_tick_hz = 250.0;
    const size_t k_label_every = 2;
    x_axis(svg, x, bottom, 0.0, k_max_hz, k_tick_hz, 0, k_label_every);
    svg.text((k_left + k_right) / 2.0, bottom + 30.0,
             format("audio frequency (Hz); gray: %.0f–%.0f Hz passband; purple tick: f_ref; rows in dB relative to their "
                    "peak",
                    k_low_hz, k_high_hz),
             font(k_small_size, k_muted, "middle"));
    svg.save(path_of(directory, "presets_spectrum.svg"), "Unlimited preset spectra",
             "Measured power spectral density of the hf_fast, hf, hf_robust, hf_weak and fm presets with the 99 percent "
             "occupied bandwidth.");
}

// ---------------------------------------------------------------------------
// Figure: BER vs SNR in AWGN
// ---------------------------------------------------------------------------

// exp(-x) I0(x), x >= 0: power series, then the asymptotic expansion.
double bessel_i0_scaled(double x) {
    const double k_series_limit = 15.0;
    const int k_series_terms = 200;
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

// Symbol error rate of noncoherent orthogonal M-FSK at Es/N0 (linear): 1 - P(the signal bin beats M - 1 noise bins).
double mfsk_symbol_error(double es_n0, unsigned tones) {
    const double k_sigmas = 9.0;
    const size_t k_steps = 4000;  // even (Simpson)
    const double a = std::sqrt(es_n0);
    const double first = std::max(0.0, a - k_sigmas);
    const double last = a + k_sigmas;
    const double h = (last - first) / k_steps;
    double sum = 0.0;
    for (size_t i = 0; i <= k_steps; ++i) {
        const double r = first + i * h;
        const double density = 2.0 * r * std::exp(-square(r - a)) * bessel_i0_scaled(2.0 * a * r);
        const double error = -std::expm1((tones - 1.0) * std::log1p(-std::exp(-r * r)));
        const double weight = (i == 0 || i == k_steps) ? 1.0 : (i % 2 == 1 ? 4.0 : 2.0);
        sum += weight * density * error;
    }
    return sum * h / 3.0;
}

// Bit error rate at SNR (key-down tone in 2500 Hz): Es/N0 = SNR * 2500 Hz * T * E_env, E_env = k_peak_energy.
double theory_ber(double snr_db, const EncoderConfig& config) {
    const unsigned tones = tone_count(config);
    const double es_n0 = std::pow(10.0, snr_db / k_power_db) * k_reference_bandwidth_hz * slot_s_of(config) *
                         unlimited::k_peak_energy;
    return mfsk_symbol_error(es_n0, tones) * (tones / 2.0) / (tones - 1.0);
}

double theory_snr_at(double ber, const EncoderConfig& config) {
    const double k_low_db = -30.0;
    const double k_high_db = 20.0;
    const int k_iterations = 60;
    double low = k_low_db;
    double high = k_high_db;
    for (int i = 0; i < k_iterations; ++i) {
        const double mid = (low + high) / 2.0;
        if (theory_ber(mid, config) > ber) {
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

const size_t k_ber_points = 7;
const double k_ber_step_db = 1.0;
const BerPreset k_ber_presets[] = {{Preset::hf_fast, "hf_fast", k_coral, -6.0},
                                   {Preset::hf, "hf", k_teal, -9.0},
                                   {Preset::hf_robust, "hf_robust", k_purple, -12.0},
                                   {Preset::hf_weak, "hf_weak", k_blue, -14.0}};
const size_t k_ber_transmissions = 50;
const size_t k_ber_bytes = 250;
const double k_ber_silence_ms = 1000.0;
const double k_ber_max_offset_hz = 50.0;
const uint32_t k_ber_seed = 4801;
const double k_v01_ook_hf_snr_db = -3.2;  // spec 4.1: v0.1 OOK at T 32 ms, BER 1e-3 (superseded, not measurable)
const double k_ber_marker = 1e-3;
const double k_delivered_gate = 0.9;

struct BerOutcome {
    size_t matched_bits = 0;
    size_t bit_errors = 0;
    size_t frames_sent = 0;
    size_t frames_delivered = 0;
    size_t locks = 0;
};

void figure_ber(const std::string& directory) {
    const size_t presets = sizeof(k_ber_presets) / sizeof(k_ber_presets[0]);
    const size_t jobs = presets * k_ber_points * k_ber_transmissions;
    const auto started = std::chrono::steady_clock::now();
    // Slowest presets first, for an even load across the threads.
    const std::vector<BerOutcome> outcomes = parallel_map<BerOutcome>(jobs, [&](size_t i) {
        const size_t job = jobs - 1 - i;
        const size_t p = job / (k_ber_points * k_ber_transmissions);
        const size_t point = job / k_ber_transmissions % k_ber_points;
        const uint32_t seed = k_ber_seed + static_cast<uint32_t>(job);
        const EncoderConfig config = EncoderConfig::from_preset(k_ber_presets[p].preset, k_rate_hz);
        const lb::Recording recording = lb::single(lb::random_bytes(k_ber_bytes, seed), config, k_ber_silence_ms);
        std::mt19937 generator(seed);
        std::uniform_real_distribution<double> offset(-k_ber_max_offset_hz, k_ber_max_offset_hz);
        sim::ChannelConfig channel;
        channel.mode = sim::Mode::usb;
        channel.snr_db = k_ber_presets[p].first_snr_db + point * k_ber_step_db;
        channel.freq_offset_hz = offset(generator);
        channel.seed = seed;
        const std::vector<int16_t> received = through_channel(recording.samples, channel, config.amplitude, 1.0);
        const lb::Capture capture = lb::run_decoder(received, DecoderConfig::for_profile(Profile::ssb), k_decoder_chunk);
        const lb::Score score = lb::score(recording, capture);
        BerOutcome o;
        o.matched_bits = score.matched * unlimited::k_bits_per_byte;
        o.bit_errors = score.bit_errors;
        o.frames_sent = score.frames_sent;
        o.frames_delivered = score.frames_delivered;
        o.locks = score.locks;
        return o;
    });
    // outcomes[i] holds job jobs - 1 - i.
    std::vector<BerOutcome> points(presets * k_ber_points);
    for (size_t i = 0; i < jobs; ++i) {
        const size_t job = jobs - 1 - i;
        BerOutcome& into = points[job / k_ber_transmissions];
        into.matched_bits += outcomes[i].matched_bits;
        into.bit_errors += outcomes[i].bit_errors;
        into.frames_sent += outcomes[i].frames_sent;
        into.frames_delivered += outcomes[i].frames_delivered;
        into.locks += outcomes[i].locks;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    const double k_width = 820.0;
    const double k_height = 516.0;
    const Box plot = {70.0, 96.0, 520.0, 320.0};
    const double k_snr_low = -15.0;
    const double k_snr_high = 1.0;
    const double k_ber_low = 1e-5;
    const double k_ber_high = 1e-1;
    Svg svg(k_width, k_height);
    heading(svg, "BER vs SNR in AWGN, measured on the integrated decoder",
            format("Real Encoder → sim::Channel (USB, AWGN, tuned ±%.0f Hz) → real Decoder (ssb profile).",
                   k_ber_max_offset_hz),
            format("%u transmissions of %u random bytes per point (%u bits sent), measured by make docs.",
                   static_cast<unsigned>(k_ber_transmissions), static_cast<unsigned>(k_ber_bytes),
                   static_cast<unsigned>(k_ber_transmissions * k_ber_bytes * unlimited::k_bits_per_byte)));
    const Scale x = {k_snr_low, k_snr_high, plot.left, plot.right()};
    const LogScale y = {k_ber_low, k_ber_high, plot.bottom(), plot.top};
    const std::string clip_id = svg.clip(plot.left, plot.top - 1.0, plot.width, plot.height + 2.0);
    for (int decade = -5; decade <= -1; ++decade) {
        const double value = std::pow(10.0, decade);
        svg.line(plot.left, y(value), plot.right(), y(value), k_grid_line, 1.0);
        svg.text(plot.left - 6.0, y(value) + k_text_rise, format("1e%d", decade), font(k_tick_size, k_muted, "end"));
        const int k_minor_ticks = 9;
        for (int m = 2; m <= k_minor_ticks && decade < -1; ++m)
            svg.line(plot.left - 3.0, y(m * value), plot.left, y(m * value), k_axis, 0.8);
    }
    svg.line(plot.left, plot.top, plot.left, plot.bottom(), k_axis, 1.0);
    x_axis(svg, x, plot.bottom(), k_snr_low, k_snr_high, 1.0, 0, 1, plot.top);
    svg.rect(plot.left, plot.top, plot.width, plot.height, "none", k_axis, 1.0);
    svg.line(plot.left, y(k_ber_marker), plot.right(), y(k_ber_marker), k_amber, 1.2, "6 4");
    svg.text(plot.right() - 4.0, y(k_ber_marker) - 5.0, "BER 1e-3", font(k_small_size, k_amber, "end", true));
    svg.text((plot.left + plot.right()) / 2.0, plot.bottom() + 32.0, "SNR (dB): key-down tone power over noise in 2500 Hz",
             font(k_small_size, k_muted, "middle"));
    svg.text(plot.left - 50.0, (plot.top + plot.bottom()) / 2.0, "bit error rate", font(k_small_size, k_muted, "middle"),
             -90.0);

    std::string clip_open = "<g clip-path=\"url(#" + clip_id + ")\">";
    svg.raw(clip_open);
    const size_t k_theory_steps_per_db = 4;
    const size_t theory_steps = static_cast<size_t>(std::lround((k_snr_high - k_snr_low) * k_theory_steps_per_db));
    for (size_t p = 0; p < presets; ++p) {
        const EncoderConfig config = EncoderConfig::from_preset(k_ber_presets[p].preset, k_rate_hz);
        std::vector<Point> curve;
        for (size_t i = 0; i <= theory_steps; ++i) {
            const double snr = k_snr_low + static_cast<double>(i) / k_theory_steps_per_db;
            const double ber = theory_ber(snr, config);
            const Point point = {x(snr), y(ber)};
            curve.push_back(point);
            if (ber < k_ber_low) break;
        }
        svg.polyline(curve, k_ber_presets[p].color, 1.0, "4 3");
    }
    svg.raw("</g>");
    const double k_marker_radius = 4.0;
    for (size_t p = 0; p < presets; ++p) {
        // Lines join neighbouring points with errors only: a point without errors breaks the line.
        std::vector<Point> measured;
        for (size_t point = 0; point <= k_ber_points; ++point) {
            const BerOutcome* o = point < k_ber_points ? &points[p * k_ber_points + point] : nullptr;
            if (o == nullptr || o->matched_bits == 0 || o->bit_errors == 0) {
                svg.polyline(measured, k_ber_presets[p].color, 1.8);
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
                o.frames_sent > 0 && static_cast<double>(o.frames_delivered) / o.frames_sent >= k_delivered_gate;
            if (o.bit_errors == 0) {
                const double px = x(snr);
                const double py = y(std::max(k_ber_low, 1.0 / o.matched_bits));
                std::vector<Point> triangle;
                const Point a = {px - k_marker_radius, py - k_marker_radius};
                const Point b = {px + k_marker_radius, py - k_marker_radius};
                const Point c = {px, py + k_marker_radius};
                triangle.push_back(a);
                triangle.push_back(b);
                triangle.push_back(c);
                svg.polygon(triangle, k_ber_presets[p].color);
                continue;
            }
            const double ber = static_cast<double>(o.bit_errors) / o.matched_bits;
            svg.circle(x(snr), y(ber), k_marker_radius, delivered ? k_ber_presets[p].color : k_card,
                       k_ber_presets[p].color, 1.5);
        }
    }
    // v0.1 reference.
    const double vx = x(k_v01_ook_hf_snr_db);
    const double vy = y(k_ber_marker);
    const double k_diamond = 5.5;
    std::vector<Point> diamond;
    const Point d0 = {vx, vy - k_diamond};
    const Point d1 = {vx + k_diamond, vy};
    const Point d2 = {vx, vy + k_diamond};
    const Point d3 = {vx - k_diamond, vy};
    diamond.push_back(d0);
    diamond.push_back(d1);
    diamond.push_back(d2);
    diamond.push_back(d3);
    svg.polygon(diamond, k_gray);
    const double label_x = x(k_v01_ook_hf_snr_db + 0.4);
    const double label_y = y(1.5e-2);
    svg.line(vx + 3.0, vy - 5.0, label_x + 8.0, label_y + 30.0, k_gray, 0.8);
    svg.text(label_x, label_y, "v0.1 OOK hf, T 32 ms", font(k_small_size, k_muted));
    svg.text(label_x, label_y + 13.0, "(1 bit per peak):", font(k_small_size, k_muted));
    svg.text(label_x, label_y + 26.0, format("1e-3 at %s dB", number(k_v01_ook_hf_snr_db, 1).c_str()),
             font(k_small_size, k_muted));

    // Legend.
    const double lx = plot.right() + 22.0;
    double ly = plot.top + 8.0;
    const double k_legend_step = 19.0;
    for (size_t p = 0; p < presets; ++p) {
        const EncoderConfig config = EncoderConfig::from_preset(k_ber_presets[p].preset, k_rate_hz);
        svg.line(lx, ly - 4.0, lx + 22.0, ly - 4.0, k_ber_presets[p].color, 1.8);
        svg.circle(lx + 11.0, ly - 4.0, k_marker_radius, k_ber_presets[p].color, k_ber_presets[p].color, 1.5);
        svg.text(lx + 30.0, ly, format("%s  T%.0f k%u", k_ber_presets[p].name, slot_ms_of(config),
                                       static_cast<unsigned>(config.bits_per_peak)),
                 font(k_note_size, k_ink, "start", true));
        ly += k_legend_step;
    }
    ly += 6.0;
    svg.line(lx, ly - 4.0, lx + 22.0, ly - 4.0, k_ink, 1.0, "4 3");
    svg.text(lx + 30.0, ly, "theory: noncoherent MFSK", font(k_note_size, k_ink));
    ly += k_legend_step;
    svg.circle(lx + 11.0, ly - 4.0, k_marker_radius, k_card, k_ink, 1.5);
    svg.text(lx + 30.0, ly, format("< %.0f%% of frames delivered", k_delivered_gate * 100.0), font(k_note_size, k_ink));
    ly += k_legend_step;
    std::vector<Point> triangle;
    const Point ta = {lx + 11.0 - k_marker_radius, ly - 4.0 - k_marker_radius};
    const Point tb = {lx + 11.0 + k_marker_radius, ly - 4.0 - k_marker_radius};
    const Point tc = {lx + 11.0, ly - 4.0 + k_marker_radius};
    triangle.push_back(ta);
    triangle.push_back(tb);
    triangle.push_back(tc);
    svg.polygon(triangle, k_ink);
    svg.text(lx + 30.0, ly, "0 errors (drawn at 1/bits)", font(k_note_size, k_ink));
    ly += k_legend_step;
    std::vector<Point> small_diamond;
    const Point s0 = {lx + 11.0, ly - 4.0 - k_diamond};
    const Point s1 = {lx + 11.0 + k_diamond, ly - 4.0};
    const Point s2 = {lx + 11.0, ly - 4.0 + k_diamond};
    const Point s3 = {lx + 11.0 - k_diamond, ly - 4.0};
    small_diamond.push_back(s0);
    small_diamond.push_back(s1);
    small_diamond.push_back(s2);
    small_diamond.push_back(s3);
    svg.polygon(small_diamond, k_gray);
    svg.text(lx + 30.0, ly, "v0.1 OOK hf (spec 4.1)", font(k_note_size, k_ink));

    ly += k_legend_step + 10.0;
    svg.text(lx, ly, "SNR at BER 1e-3, theory:", font(k_note_size, k_ink, "start", true));
    for (size_t p = 0; p < presets; ++p) {
        ly += k_line_gap;
        const EncoderConfig config = EncoderConfig::from_preset(k_ber_presets[p].preset, k_rate_hz);
        svg.text(lx, ly, format("%s %s dB", k_ber_presets[p].name, number(theory_snr_at(k_ber_marker, config), 2).c_str()),
                 font(k_note_size, k_ber_presets[p].color));
    }

    svg.text(k_margin, k_height - 46.0,
             "BER over the bytes the decoder released (bit errors / bits matched to what was sent); hollow points lost "
             "frames to acquisition.",
             font(k_note_size, k_muted));
    svg.text(k_margin, k_height - 30.0,
             format("Theory: noncoherent orthogonal MFSK with the matched α 0.25 window, Es/N0 = SNR · 2500 Hz "
                    "· T · %.5f.",
                    unlimited::k_peak_energy),
             font(k_note_size, k_muted));
    svg.text(k_margin, k_height - 14.0,
             "Near threshold the limit is acquisition, not the slot decisions (spec 4.8).", font(k_note_size, k_muted));
    svg.save(path_of(directory, "ber_awgn.svg"), "Unlimited BER in AWGN",
             "Measured bit error rate against SNR for the hf_fast, hf, hf_robust and hf_weak presets, with noncoherent "
             "MFSK theory and the v0.1 OOK point.");

    std::printf("ber_awgn.svg: %u jobs in %.1f s\n", static_cast<unsigned>(jobs), seconds);
    for (size_t p = 0; p < presets; ++p) {
        const EncoderConfig config = EncoderConfig::from_preset(k_ber_presets[p].preset, k_rate_hz);
        std::printf("  %-9s theory 1e-3 at %.2f dB\n", k_ber_presets[p].name, theory_snr_at(k_ber_marker, config));
        for (size_t point = 0; point < k_ber_points; ++point) {
            const BerOutcome& o = points[p * k_ber_points + point];
            const double snr = k_ber_presets[p].first_snr_db + point * k_ber_step_db;
            std::printf("    %6.1f dB: %7u bits, %5u errors, BER %.2e (theory %.2e), frames %5.1f%%, locks %u/%u\n", snr,
                        static_cast<unsigned>(o.matched_bits), static_cast<unsigned>(o.bit_errors),
                        o.matched_bits == 0 ? 0.0 : static_cast<double>(o.bit_errors) / o.matched_bits,
                        theory_ber(snr, config),
                        o.frames_sent == 0 ? 0.0 : 100.0 * o.frames_delivered / o.frames_sent,
                        static_cast<unsigned>(o.locks), static_cast<unsigned>(k_ber_transmissions));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int k_exit_usage = 2;
    const int k_exit_failure = 1;
    if (argc != 2) {
        std::fprintf(stderr, "usage: doc_figures OUTPUT_DIRECTORY\n");
        return k_exit_usage;
    }
    const std::string directory = argv[1];
    try {
        const Rendered hf = render(text_bytes(), EncoderConfig::from_preset(Preset::hf, k_rate_hz));
        const Rendered hf_wave = render(text_bytes(), EncoderConfig::from_preset(Preset::hf, k_wave_rate_hz));
        figure_slot_shapes(directory, hf_wave);
        figure_timeline(directory, hf_wave);
        figure_spectrogram(directory, hf);
        figure_tone_grid(directory);
        figure_frame_mapping(directory);
        figure_channels(directory);
        figure_envelopes(directory, hf);
        figure_presets_spectrum(directory);
        figure_ber(directory);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "doc_figures: %s\n", error.what());
        return k_exit_failure;
    }
    return 0;
}
