#pragma once

#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"
#include "unlimited/packet.hpp"

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

// Command-line helpers shared by the demos: argument reader, number parsing, name tables, the bandwidth line, the
// plain-words reason for a refused configuration, file reading, the --packet framing and the console that holds lines
// while the TUI owns the screen.
namespace unlimited {
namespace cli {

const int k_exit_ok = 0;
const int k_exit_usage = 2;
const int k_exit_io = 3;

const int k_range_decimals = 1;  // how an out-of-range value is printed
const int k_ms_max_decimals = 3; // a T given in ms, to the µs
const int k_baud_decimals = 2;
const double k_us_per_ms = 1e3;
const double k_ms_per_s = 1e3;
const uint32_t k_us_per_ms_int = 1000;

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

// A duration in ms, to the µs: 16, 12.5, 16.001.
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

template <typename T, std::size_t N, typename V>
const char* name_of(const T (&table)[N], V value) {
    for (std::size_t i = 0; i < N; ++i)
        if (table[i].value == value) return table[i].name;
    return "?";
}

struct PresetName {
    const char* name;
    Preset value;
};

const PresetName k_presets[] = {{"hf_slow", Preset::hf_slow},
                                {"hf", Preset::hf},
                                {"hf_fast", Preset::hf_fast},
                                {"am", Preset::am},
                                {"fm", Preset::fm}};

struct ProfileName {
    const char* name;
    Profile value;
};

const ProfileName k_profiles[] = {{"ssb", Profile::ssb}, {"am", Profile::am}, {"fm", Profile::fm}};

struct RuleName {
    const char* name;
    DecisionMode value;
};

const RuleName k_rules[] = {{"adaptive", DecisionMode::adaptive}, {"fixed", DecisionMode::fixed_ratio}};

// "300:2700" -> {300, 2700}; each edge a whole number of Hz (check() judges the pair).
inline Passband to_passband(const std::string& option, const std::string& text) {
    const std::vector<double> edges = to_fields(option, text, 2, 2);
    Passband passband;
    passband.low_hz = to_integer<uint16_t>(option, edges[0]);
    passband.high_hz = to_integer<uint16_t>(option, edges[1]);
    return passband;
}

inline double slot_ms_of(uint32_t slot_us) {
    return slot_us / k_us_per_ms;
}

// Net rate (spec 1.7): N bits per (N + 1) slots of T.
inline double net_bit_rate(unsigned bits_per_package, double slot_ms) {
    return slot_ms > 0.0 ? bits_per_package * k_ms_per_s / ((bits_per_package + 1) * slot_ms) : 0.0;
}

inline std::string range_text(uint16_t low_hz, uint16_t high_hz) {
    return std::to_string(low_hz) + "-" + std::to_string(high_hz);
}

inline std::string passband_text(const Passband& passband) {
    return range_text(passband.low_hz, passband.high_hz) + " Hz";
}

// Half of the occupied band at slot_us: the room a pitch needs from each passband edge.
inline uint16_t half_band_hz(uint32_t slot_us) {
    const Band band = occupied_band(k_default_tone_hz, slot_us);
    return static_cast<uint16_t>(band.width_hz / 2);
}

// The bandwidth line (spec 7), e.g. "occupied bandwidth 276 Hz (1362-1638 Hz); passband 300-2700 Hz: fits; shift
// tolerance -1062/+1062 Hz". The tolerance is how far the pitch may move down / up (mistuning) and still be heard:
// `fit` is passband_fit() limited by the receiver's pitch search (spec 1.5), never the pure filter fit.
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

// The band a received signal occupies.
inline Band received_band(float tone_hz, float slot_ms) {
    return occupied_band(received_tone_hz(tone_hz), received_slot_us(slot_ms));
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
    const std::string what = "the signal does not fit the receiver's passband: at T = " +
                             ms_text(slot_ms_of(config.slot_us)) + " ms it is " + std::to_string(band.width_hz) +
                             " Hz wide (" + range_text(band.low_hz, band.high_hz) + " Hz around the pitch " +
                             std::to_string(config.tone_hz) + " Hz) and the passband is " +
                             passband_text(config.passband);
    if (band.width_hz > passband_width)
        return what + ", only " + std::to_string(passband_width) +
               " Hz wide; use longer slots (--slot-ms, or a slower --preset) or a wider --passband";
    const int lowest = std::max<int>(config.passband.low_hz + half, k_min_tone_hz);
    const int highest = std::min<int>(config.passband.high_hz - half, k_max_tone_hz);
    if (lowest > highest)
        return what + "; no pitch between " + std::to_string(k_min_tone_hz) + " and " +
               std::to_string(k_max_tone_hz) + " Hz fits it: move --passband";
    return what + "; move --tone to " + std::to_string(lowest) + ".." + std::to_string(highest) + " Hz";
}

// The EncoderConfig::check() rule `config` breaks, in plain words with the option to change (spec 7); empty when
// valid. Every ConfigError has its text; the receiver-only ones cannot come from an encoder.
inline std::string encoder_problem(const EncoderConfig& config) {
    const double slot_ms = slot_ms_of(config.slot_us);
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
        return "the slot length T = " + ms_text(slot_ms) + " ms is outside " +
               ms_text(slot_ms_of(k_min_slot_us)) + ".." + ms_text(slot_ms_of(k_max_slot_us)) +
               " ms (--slot-ms; --baud is 1000/T)";
    case ConfigError::fast_tone:
        return "slots shorter than " + ms_text(slot_ms_of(k_fast_slot_us)) + " ms need a pitch of at least " +
               std::to_string(k_min_fast_tone_hz) + " Hz, and the pitch is " + std::to_string(config.tone_hz) +
               " Hz: raise --tone or use longer slots (--slot-ms)";
    case ConfigError::bits_per_package:
        return "the bits per package N = " + std::to_string(config.bits_per_package) + " is outside " +
               std::to_string(k_min_bits_per_package) + ".." + std::to_string(k_max_bits_per_package) + " (--bits)";
    case ConfigError::package_length: {
        const uint32_t most = static_cast<uint32_t>(k_max_package_us / config.slot_us) - 1;
        return "a package of " + std::to_string(config.bits_per_package) + " bits at T = " + ms_text(slot_ms) +
               " ms lasts (N + 1) x T = " + ms_text((config.bits_per_package + 1u) * slot_ms) +
               " ms, more than the " + ms_text(slot_ms_of(k_max_package_us)) + " ms limit: use --bits " +
               std::to_string(std::min<uint32_t>(most, k_max_bits_per_package)) +
               " or less, or shorter slots (--slot-ms)";
    }
    case ConfigError::passband:
        return "the passband " + passband_text(config.passband) + " is not valid: it needs LO < HI <= " +
               std::to_string(k_max_passband_hz) + " Hz (--passband LO:HI)";
    case ConfigError::outside_passband:
        return outside_passband_problem(config);
    case ConfigError::sync_markers:
        return "the sync train must have " + std::to_string(k_min_sync_markers) + ".." +
               std::to_string(k_max_sync_markers) + " markers, not " + std::to_string(config.sync_markers) +
               " (--sync)";
    case ConfigError::amplitude:
        return "the level is too low: the beep's crest rounds to 0 (--level-dbfs)";
    case ConfigError::min_slot:
    case ConfigError::decision_mode:
    case ConfigError::fixed_ratio:
        return "a receiver setting was refused";
    }
    return "the encoder refuses this combination";
}

// The same for DecoderConfig::check().
inline std::string decoder_problem(const DecoderConfig& config) {
    switch (config.check()) {
    case ConfigError::none:
        return "";
    case ConfigError::min_slot:
        return "the shortest slot must be 4..32 ms, not " + std::to_string(config.min_slot_ms) +
               " (--min-slot-ms N: the receiver then hears slots of N..8N ms)";
    case ConfigError::passband: {
        if (!passband_valid(config.passband))
            return "the passband " + passband_text(config.passband) + " is not valid: it needs LO < HI <= " +
                   std::to_string(k_max_passband_hz) + " Hz (--passband LO:HI)";
        const uint32_t slowest_us = static_cast<uint32_t>(config.max_slot_ms()) * k_us_per_ms_int;
        const bool fast = static_cast<uint32_t>(config.min_slot_ms) * k_us_per_ms_int < k_fast_slot_us;
        return "the passband " + passband_text(config.passband) + " leaves no pitch to search for: the search keeps " +
               std::to_string(half_band_hz(slowest_us)) + " Hz from each edge (half the band of " +
               std::to_string(config.max_slot_ms()) + " ms slots) and stays within " +
               std::to_string(fast ? k_min_fast_tone_hz : k_min_tone_hz) + ".." + std::to_string(k_max_tone_hz) +
               " Hz; widen --passband";
    }
    case ConfigError::decision_mode:
        return "the decision rule must be adaptive or fixed (--rule)";
    case ConfigError::fixed_ratio:
        return "the fixed decision line must lie between 0 and 1 of the reference line, not " +
               fixed(config.fixed_ratio, 2) + " (--ratio, e.g. 0.70)";
    case ConfigError::sample_rate:
    case ConfigError::tone:
    case ConfigError::slot:
    case ConfigError::fast_tone:
    case ConfigError::bits_per_package:
    case ConfigError::package_length:
    case ConfigError::outside_passband:
    case ConfigError::sync_markers:
    case ConfigError::amplitude:
        return "a sender setting was refused";
    }
    return "the decoder refuses this combination";
}

// "smart decision line" or "fixed decision line at 70 %".
inline std::string rule_text(const DecoderConfig& config) {
    if (config.decision_mode == DecisionMode::fixed_ratio)
        return "fixed decision line at " + fixed(100.0 * config.fixed_ratio, 0) + " % of the reference";
    return "smart decision line";
}

inline bool read_file(const std::string& path, std::vector<std::uint8_t>& data) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) return false;
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

// The framing of unlimited_encode --packet: consecutive packets of up to k_packet_max_payload bytes.
// Returns the number of packets.
inline std::size_t packetize(const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& framed) {
    framed.clear();
    std::size_t packets = 0;
    for (std::size_t position = 0; position < payload.size(); ++packets) {
        const std::size_t size = std::min<std::size_t>(k_packet_max_payload, payload.size() - position);
        const std::size_t start = framed.size();
        framed.resize(start + size + k_packet_overhead);
        packet_build(&payload[position], static_cast<std::uint16_t>(size), &framed[start], size + k_packet_overhead);
        position += size;
    }
    return packets;
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
