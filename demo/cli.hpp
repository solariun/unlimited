#pragma once

#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// Command-line helpers shared by the demos: argument reader, number parsing, name tables, the speed and bandwidth
// lines, the plain-words reason for a refused configuration, file reading and the console that holds lines while the
// TUI owns the screen.
namespace unlimited {
namespace cli {

const int k_exit_ok = 0;
const int k_exit_usage = 2;
const int k_exit_io = 3;

const int k_range_decimals = 1;   // how an out-of-range value is printed
const int k_ms_max_decimals = 3;  // a T in ms, to the µs
const int k_speed_decimals = 2;   // bytes/s, the resolution of spec 1.3
const int k_bit_rate_decimals = 2;
const double k_us_per_ms = 1e3;
const double k_ms_per_s = 1e3;
const double k_percent = 100.0;
const double k_bits_per_byte_f = 8.0;
const char* const k_auto_threshold = "auto";

class UsageError : public std::runtime_error {
public:
    explicit UsageError(const std::string& message) : std::runtime_error(message) {}
};

inline std::string fixed(double value, int decimals) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

// A number without trailing zeros: 16, 12.5, 31.25.
inline std::string trimmed(double value, int max_decimals) {
    std::string text = fixed(value, max_decimals);
    if (text.find('.') == std::string::npos) return text;
    while (!text.empty() && text[text.size() - 1] == '0') text.erase(text.size() - 1);
    if (!text.empty() && text[text.size() - 1] == '.') text.erase(text.size() - 1);
    return text;
}

// A duration in ms, to the µs: 16, 12.5, 16.667.
inline std::string ms_text(double ms) {
    return trimmed(ms, k_ms_max_decimals);
}

class Arguments {
public:
    Arguments(int argc, char** argv) : argc_(argc), argv_(argv), index_(1) {}

    bool next(std::string& option) {
        if (index_ >= argc_) return false;
        option = argv_[index_++];
        return true;
    }

    std::string value(const std::string& option) {
        if (index_ >= argc_) throw UsageError(option + " needs a value");
        return argv_[index_++];
    }

private:
    int argc_;
    char** argv_;
    int index_;
};

inline double to_number(const std::string& option, const std::string& text) {
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size() || errno != 0 || !std::isfinite(value))
        throw UsageError(option + ": '" + text + "' is not a number");
    return value;
}

// "A:B[:C]" -> numbers; between `required` and `allowed` fields.
inline std::vector<double> to_fields(const std::string& option, const std::string& text, std::size_t required,
                                     std::size_t allowed) {
    std::vector<double> values;
    std::size_t begin = 0;
    while (true) {
        const std::size_t colon = text.find(':', begin);
        values.push_back(to_number(option, text.substr(begin, colon - begin)));
        if (colon == std::string::npos) break;
        begin = colon + 1;
    }
    if (values.size() < required || values.size() > allowed) {
        const std::string count = std::to_string(required) + (allowed > required ? "-" + std::to_string(allowed) : "");
        throw UsageError(option + ": '" + text + "' must be " + count + " numbers separated by ':'");
    }
    return values;
}

template <typename T>
T to_integer(const std::string& option, double value) {  // rounded; must fit T and be >= 0
    if (!(value >= 0.0 && value <= static_cast<double>(std::numeric_limits<T>::max())))
        throw UsageError(option + ": " + fixed(value, k_range_decimals) + " is out of range");
    return static_cast<T>(std::llround(value));
}

// Rounded and clamped to T's range, so that a value far outside a rule still reaches EncoderConfig::check() or
// DecoderConfig::check(), which name the rule in plain words.
template <typename T>
T to_clamped(double value) {
    const double high = static_cast<double>(std::numeric_limits<T>::max());
    return static_cast<T>(std::llround(std::max(0.0, std::min(high, value))));
}

template <typename T, std::size_t N>
const T& find_name(const T (&table)[N], const std::string& option, const std::string& name) {
    for (std::size_t i = 0; i < N; ++i)
        if (name == table[i].name) return table[i];
    std::string names;
    for (std::size_t i = 0; i < N; ++i) names += std::string(i == 0 ? "" : "|") + table[i].name;
    throw UsageError(option + ": '" + name + "' is not one of " + names);
}

// "300:2700" -> {300, 2700}; each edge a whole number of Hz (check() judges the pair).
inline Passband to_passband(const std::string& option, const std::string& text) {
    const std::vector<double> edges = to_fields(option, text, 2, 2);
    Passband passband;
    passband.low_hz = to_integer<uint16_t>(option, edges[0]);
    passband.high_hz = to_integer<uint16_t>(option, edges[1]);
    return passband;
}

// --bps B: the slot of B rounded to 0.01 bytes/s (spec 1.3). A speed outside 1..25 gives a slot outside the range,
// which check() refuses in plain words.
inline uint32_t to_slot_us(const std::string& option, const std::string& text) {
    const double speed = to_number(option, text);
    if (!(speed > 0.0)) throw UsageError(option + ": the speed must be above 0 bytes/s");
    return slot_us_for_centi_speed(to_clamped<uint16_t>(speed * k_percent));
}

// --threshold PCT|auto: the fixed line at PCT % of the reference, or the adaptive line.
inline void to_threshold(const std::string& option, const std::string& text, DecoderConfig& config) {
    if (text == k_auto_threshold) {
        config.decision_mode = DecisionMode::adaptive;
        return;
    }
    config.decision_mode = DecisionMode::fixed;
    config.threshold_percent = to_clamped<uint8_t>(to_number(option, text));
}

inline double slot_ms_of(uint32_t slot_us) {
    return slot_us / k_us_per_ms;
}

inline std::string speed_number(uint32_t slot_us) {
    return fixed(bytes_per_second(slot_us), k_speed_decimals);
}

// "6.00 bytes/s = 48 bit/s, slot T 16.667 ms"
inline std::string speed_text(uint32_t slot_us) {
    const double speed = bytes_per_second(slot_us);
    return speed_number(slot_us) + " bytes/s = " + trimmed(k_bits_per_byte_f * speed, k_bit_rate_decimals) +
           " bit/s, slot T " + ms_text(slot_ms_of(slot_us)) + " ms";
}

inline std::string range_text(uint16_t low_hz, uint16_t high_hz) {
    return std::to_string(low_hz) + "-" + std::to_string(high_hz);
}

inline std::string passband_text(const Passband& passband) {
    return range_text(passband.low_hz, passband.high_hz) + " Hz";
}

// The bandwidth line (spec 1.3), e.g. "occupied bandwidth 264 Hz (1368-1632 Hz); passband 300-2700 Hz: fits; shift
// tolerance -1068/+1068 Hz". The tolerance is how far the pitch may move down / up (mistuning) and still be heard:
// `fit` is passband_fit() limited by the receiver's pitch search, never the pure filter fit.
inline std::string bandwidth_line(const Band& band, const Passband& passband, const PassbandFit& fit) {
    const std::string line = "occupied bandwidth " + std::to_string(band.width_hz) + " Hz (" +
                             range_text(band.low_hz, band.high_hz) + " Hz); passband " + passband_text(passband) +
                             ": ";
    if (!passband_valid(passband))
        return line + "not a valid passband (it needs LO < HI <= " + std::to_string(k_max_passband_hz) + " Hz)";
    if (fit.fits)
        return line + "fits; shift tolerance -" + std::to_string(fit.margin_low_hz) + "/+" +
               std::to_string(fit.margin_high_hz) + " Hz";
    std::string outside;
    if (fit.margin_low_hz < 0) outside = std::to_string(-fit.margin_low_hz) + " Hz below";
    if (fit.margin_high_hz < 0)
        outside += (outside.empty() ? "" : " and ") + std::to_string(-fit.margin_high_hz) + " Hz above";
    return line + "does not fit (" + outside + " the passband)";
}

// The sender's line: the receiver that hears it by default (passband_fit(config)).
inline std::string bandwidth_line(const EncoderConfig& config) {
    return bandwidth_line(occupied_band(config), config.passband, passband_fit(config));
}

// A received signal's measured pitch and T, rounded to the integers the band formulas take.
inline uint16_t received_tone_hz(float tone_hz) {
    return static_cast<uint16_t>(std::lround(std::max(0.0f, tone_hz)));
}

inline uint32_t received_slot_us(float slot_ms) {
    return static_cast<uint32_t>(std::lround(std::max(0.0f, slot_ms) * k_us_per_ms));
}

// The receiver's line: the received signal against its own passband, the tolerance limited by its own pitch search.
inline std::string bandwidth_line(const DecoderConfig& config, float tone_hz, float slot_ms) {
    const uint16_t tone = received_tone_hz(tone_hz);
    const uint32_t slot_us = received_slot_us(slot_ms);
    return bandwidth_line(occupied_band(tone, slot_us), config.passband,
                          passband_fit(tone, slot_us, config.passband, config.search_range()));
}

// Why the passband refuses the signal, and what to change.
inline std::string outside_passband_problem(const EncoderConfig& config) {
    const Band band = occupied_band(config);
    const uint16_t half = static_cast<uint16_t>(band.width_hz / 2);
    const int passband_width = config.passband.high_hz - config.passband.low_hz;
    const std::string what = "the signal does not fit the receiver's passband: at " + speed_number(config.slot_us) +
                             " bytes/s it is " + std::to_string(band.width_hz) + " Hz wide (" +
                             range_text(band.low_hz, band.high_hz) + " Hz around the pitch " +
                             std::to_string(config.tone_hz) + " Hz) and the passband is " +
                             passband_text(config.passband);
    if (band.width_hz > passband_width)
        return what + ", only " + std::to_string(passband_width) +
               " Hz wide; use a slower speed (--bps) or a wider --passband";
    const int lowest = std::max<int>(config.passband.low_hz + half, k_min_tone_hz);
    const int highest = std::min<int>(config.passband.high_hz - half, k_max_tone_hz);
    if (lowest > highest)
        return what + "; no pitch between " + std::to_string(k_min_tone_hz) + " and " +
               std::to_string(k_max_tone_hz) + " Hz fits it: move --passband";
    return what + "; move --tone to " + std::to_string(lowest) + ".." + std::to_string(highest) + " Hz";
}

inline std::string slot_problem(uint32_t slot_us) {
    return "the speed " + speed_number(slot_us) + " bytes/s is outside " +
           fixed(k_min_bytes_per_second, k_speed_decimals) + ".." + fixed(k_max_bytes_per_second, k_speed_decimals) +
           " bytes/s (--bps)";
}

inline std::string passband_problem(const Passband& passband) {
    return "the passband " + passband_text(passband) + " is not valid: it needs LO < HI <= " +
           std::to_string(k_max_passband_hz) + " Hz (--passband LO:HI)";
}

// The EncoderConfig::check() rule `config` breaks, in plain words with the option to change (spec 7); empty when
// valid. Every ConfigError has its text; the receiver-only ones cannot come from an encoder.
inline std::string encoder_problem(const EncoderConfig& config) {
    switch (config.check()) {
    case ConfigError::none:
        return "";
    case ConfigError::sample_rate:
        return "the sample rate " + std::to_string(config.sample_rate_hz) + " Hz is outside " +
               std::to_string(k_min_sample_rate_hz) + ".." + std::to_string(k_max_sample_rate_hz) + " Hz (--rate)";
    case ConfigError::tone:
        return "the pitch " + std::to_string(config.tone_hz) + " Hz is outside " + std::to_string(k_min_tone_hz) +
               ".." + std::to_string(k_max_tone_hz) + " Hz (--tone)";
    case ConfigError::slot:
        return slot_problem(config.slot_us);
    case ConfigError::passband:
        return passband_problem(config.passband);
    case ConfigError::outside_passband:
        return outside_passband_problem(config);
    case ConfigError::amplitude:
        return "the level is too low: the beep's crest rounds to 0 (--level-dbfs)";
    case ConfigError::threshold:
    case ConfigError::decision_mode:
        return "a receiver setting was refused";
    }
    return "the encoder refuses this combination";
}

// The same for DecoderConfig::check().
inline std::string decoder_problem(const DecoderConfig& config) {
    switch (config.check()) {
    case ConfigError::none:
        return "";
    case ConfigError::slot:
        return slot_problem(config.slot_us);
    case ConfigError::passband: {
        if (!passband_valid(config.passband)) return passband_problem(config.passband);
        const Band band = occupied_band(k_default_tone_hz, config.slot_us);
        return "the passband " + passband_text(config.passband) + " leaves no pitch to search for at " +
               speed_number(config.slot_us) + " bytes/s: the search keeps " + std::to_string(band.width_hz / 2) +
               " Hz from each edge (half the occupied band) and stays within " + std::to_string(k_min_tone_hz) +
               ".." + std::to_string(k_max_tone_hz) + " Hz; widen --passband or use a slower --bps";
    }
    case ConfigError::threshold:
        return "the decision threshold " + std::to_string(config.threshold_percent) + " % is outside " +
               std::to_string(k_min_threshold_percent) + ".." + std::to_string(k_max_threshold_percent) +
               " % of the reference (--threshold PCT, or auto)";
    case ConfigError::decision_mode:
        return "the decision rule must be fixed or adaptive (--threshold PCT or auto)";
    case ConfigError::sample_rate:
    case ConfigError::tone:
    case ConfigError::outside_passband:
    case ConfigError::amplitude:
        return "a sender setting was refused";
    }
    return "the decoder refuses this combination";
}

// "decision line at 70 % of the reference" or "adaptive decision line (auto)".
inline std::string threshold_text(const DecoderConfig& config) {
    if (config.decision_mode == DecisionMode::adaptive) return "adaptive decision line (auto: 50-75 % of the reference)";
    return "decision line at " + std::to_string(config.threshold_percent) + " % of the reference";
}

inline bool read_file(const std::string& path, std::vector<std::uint8_t>& data) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) return false;
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

// Lines go to stdout at once, or wait while the TUI owns the screen.
class Console {
public:
    Console() : held_(false) {}

    void hold(bool held) {
        held_ = held;
        if (held_) return;
        for (std::size_t i = 0; i < lines_.size(); ++i) std::printf("%s\n", lines_[i].c_str());
        lines_.clear();
        std::fflush(stdout);
    }

    void line(const std::string& text) {
        if (held_) {
            lines_.push_back(text);
        } else {
            std::printf("%s\n", text.c_str());
        }
    }

    // "label      text": the first column is k_label_width wide.
    void item(const std::string& label, const std::string& text) {
        std::string padded = label;
        if (padded.size() < k_label_width) padded.resize(k_label_width, ' ');
        line(padded + text);
    }

private:
    static const std::size_t k_label_width = 11;

    bool held_;
    std::vector<std::string> lines_;
};

}  // namespace cli
}  // namespace unlimited
