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

// Command-line helpers shared by the demos: argument reader, number parsing, name tables, mode text, file
// reading and the --packet framing.
namespace unlimited {
namespace cli {

const int k_exit_ok = 0;
const int k_exit_usage = 2;
const int k_exit_io = 3;

const int k_range_decimals = 1;  // how an out-of-range value is printed

class UsageError : public std::runtime_error {
public:
    explicit UsageError(const std::string& message) : std::runtime_error(message) {}
};

inline std::string fixed(double value, int decimals) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
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

template <typename T, std::size_t N>
const T& find_name(const T (&table)[N], const std::string& option, const std::string& name) {
    for (std::size_t i = 0; i < N; ++i)
        if (name == table[i].name) return table[i];
    std::string names;
    for (std::size_t i = 0; i < N; ++i) names += std::string(i == 0 ? "" : "|") + table[i].name;
    throw UsageError(option + ": '" + name + "' is not one of " + names);
}

struct PresetName {
    const char* name;
    Preset preset;
};

const PresetName k_presets[] = {{"fm_fast", Preset::fm_fast}, {"fm", Preset::fm},
                                {"hf_fast", Preset::hf_fast}, {"hf", Preset::hf},
                                {"hf_robust", Preset::hf_robust}, {"hf_weak", Preset::hf_weak}};

struct ProfileName {
    const char* name;
    Profile profile;
    Preset preset;  // the profile's default sender speed
};

const ProfileName k_profiles[] = {
    {"ssb", Profile::ssb, Preset::hf}, {"am", Profile::am, Preset::hf}, {"fm", Profile::fm, Preset::fm}};

struct SpacingName {
    const char* name;
    Spacing spacing;
};

const SpacingName k_spacings[] = {{"standard", Spacing::standard}, {"dense", Spacing::dense}};

struct SideName {
    const char* name;
    GridSide side;
};

const SideName k_sides[] = {{"above", GridSide::above}, {"below", GridSide::below}};

inline const char* spacing_name(Spacing spacing) {
    return spacing == Spacing::dense ? "dense" : "standard";
}

inline const char* side_name(GridSide side) {
    return side == GridSide::above ? "above" : "below";
}

inline const char* side_name(std::int8_t side) {  // decoder events: +1 grid above f_ref as received, -1 below
    return side > 0 ? "above" : "below";
}

const double k_ms_per_s = 1e3;
const double k_us_per_ms = 1e3;
const int k_slot_ms_decimals = 0;  // T is a whole number of ms once the mode is known
const int k_rate_decimals = 1;
const int k_us_decimals = 3;       // a T in ms, to the µs
const int k_hz_decimals = 0;

// Net rate of a mode (spec 1.4): N k bits per (N + 1) slots.
inline double net_bit_rate(unsigned bits_per_peak, unsigned data_slots, double slot_ms) {
    return data_slots * bits_per_peak * k_ms_per_s / ((data_slots + 1) * slot_ms);
}

// Distance of grid tone n from f_ref (spec 1.3): (G + n c) / T, c = 8/7 (standard) or 1 (dense).
inline double tone_offset_hz(unsigned tone, Spacing spacing, double slot_ms) {
    const double standard = static_cast<double>(k_standard_spacing_num) / k_standard_spacing_den;
    const double spacing_units = spacing == Spacing::dense ? 1.0 : standard;
    return (k_grid_guard + tone * spacing_units) * k_ms_per_s / slot_ms;
}

// Distance from f_ref to the farthest data or header tone: header tones use the standard grid in every mode.
inline double span_hz(unsigned bits_per_peak, Spacing spacing, double slot_ms) {
    const double data = tone_offset_hz((1u << bits_per_peak) - 1u, spacing, slot_ms);
    const double header = tone_offset_hz(k_header_slots - 1u, Spacing::standard, slot_ms);
    return std::max(data, header);
}

// Lowest and highest data or header tone (f_ref included), Hz.
inline void band_hz(const EncoderConfig& config, double& low, double& high) {
    const double span = span_hz(config.bits_per_peak, config.spacing, config.slot_us / k_us_per_ms);
    low = config.side == GridSide::below ? config.tone_hz - span : config.tone_hz;
    high = config.side == GridSide::below ? config.tone_hz : config.tone_hz + span;
}

// The EncoderConfig::check() rule that `config` breaks, as the usage error that names it (spec 7); empty when valid.
inline std::string config_problem(const EncoderConfig& config) {
    const double slot_ms = config.slot_us / k_us_per_ms;
    const uint32_t us_per_ms = static_cast<uint32_t>(k_us_per_ms);
    switch (config.check()) {
    case ConfigError::none:
        return "";
    case ConfigError::sample_rate:
        return "--rate must be " + std::to_string(k_min_sample_rate_hz) + ".." + std::to_string(k_max_sample_rate_hz) +
               " Hz";
    case ConfigError::tone:
        return "f_ref must be " + std::to_string(k_min_tone_hz) + ".." + std::to_string(k_max_tone_hz) + " Hz";
    case ConfigError::slot:
        return "T " + fixed(slot_ms, k_us_decimals) + " ms: T must be a whole number of ms in " +
               std::to_string(k_min_slot_us / us_per_ms) + ".." + std::to_string(k_max_slot_us / us_per_ms);
    case ConfigError::bits_per_peak:
        return "bits per peak must be 1.." + std::to_string(k_max_bits_per_peak);
    case ConfigError::data_slots:
        return "data slots must be " + std::to_string(k_min_data_slots) + ", " + std::to_string(2 * k_min_data_slots) +
               " or " + std::to_string(k_max_data_slots);
    case ConfigError::frame_length:
        return "a frame of (N + 1) T = " + fixed((config.data_slots + 1u) * slot_ms, 0) + " ms exceeds " +
               std::to_string(k_max_frame_us / us_per_ms) + " ms: fewer data slots or shorter T";
    case ConfigError::spacing:
        return "the spacing must be standard or dense";
    case ConfigError::dense_slot:
        return "dense spacing needs T >= " + std::to_string(k_min_dense_slot_us / us_per_ms) + " ms (T " +
               fixed(slot_ms, k_us_decimals) + " ms): use standard spacing or a longer T";
    case ConfigError::side:
        return "the grid side must be above or below";
    case ConfigError::band: {
        double low = 0.0;
        double high = 0.0;
        band_hz(config, low, high);
        return "the band " + fixed(low, k_hz_decimals) + ".." + fixed(high, k_hz_decimals) + " Hz (f_ref " +
               std::to_string(config.tone_hz) + " Hz, grid " + side_name(config.side) + ") leaves " +
               std::to_string(k_min_tone_hz) + ".." + std::to_string(k_max_tone_hz) +
               " Hz: move --tone, or use fewer bits per peak, standard spacing or longer slots";
    }
    case ConfigError::queue:
        return "a frame of " + std::to_string(config.frame_bytes()) + " bytes exceeds half of the " +
               std::to_string(Encoder::k_queue_size) + "-byte encoder queue";
    case ConfigError::sync_markers:
        return "--sync must be " + std::to_string(k_min_sync_markers) + ".." + std::to_string(k_max_sync_markers);
    case ConfigError::amplitude:
        return "--level-dbfs is below the encoder resolution";
    }
    return "the encoder refuses this combination";
}

// "T 32 ms  k 5  N 8  standard  grid below  138.9 bit/s"
inline std::string mode_text(double slot_ms, unsigned bits_per_peak, unsigned data_slots, Spacing spacing,
                             const char* side) {
    return "T " + fixed(slot_ms, k_slot_ms_decimals) + " ms  k " + std::to_string(bits_per_peak) + "  N " +
           std::to_string(data_slots) + "  " + spacing_name(spacing) + "  grid " + side + "  " +
           fixed(net_bit_rate(bits_per_peak, data_slots, slot_ms), k_rate_decimals) + " bit/s";
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

}  // namespace cli
}  // namespace unlimited
