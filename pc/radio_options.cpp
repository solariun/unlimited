#include "radio_options.hpp"

#include "audio.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>

namespace unlimited {
namespace pc {

using std::size_t;
using std::uint32_t;
using std::uint8_t;

namespace {

const unsigned k_any_device = 0;  // an option of every program with a sound device
const size_t k_max_rate_digits = 9;
const uint8_t k_max_civ_radio_address = 0xDF;  // E0h is the computer's address, FDh and FEh frame CI-V messages
const std::string k_hex_prefix = "0x";
const std::string k_hex_prefix_upper = "0X";
const char k_hex_suffix = 'h';
const size_t k_max_address_digits = 2;
const int k_decimal = 10;
const int k_hexadecimal = 16;
const size_t k_help_text_column = 21;

struct OptionName {
    const char* name;
    unsigned uses;
    bool has_value;
};

const OptionName k_options[] = {{"--list-devices", k_any_device, false},
                                {"-d", k_any_device, true},
                                {"--input", k_radio_input, true},
                                {"--output", k_radio_output, true},
                                {"-r", k_any_device, true},
                                {"--ptt", k_radio_ptt, true},
                                {"--ptt-device", k_radio_ptt, true},
                                {"--ptt-invert", k_radio_ptt, false},
                                {"--cat-rate", k_radio_ptt, true},
                                {"--cat-addr", k_radio_ptt, true},
                                {"--cat-tx-on", k_radio_ptt, true},
                                {"--cat-tx-off", k_radio_ptt, true}};

enum class Invert { keep, off, on };

struct MethodName {
    const char* name;
    PttMethod method;
    Invert invert;
};

const MethodName k_methods[] = {{"vox", PttMethod::vox, Invert::keep},     {"rts", PttMethod::rts, Invert::keep},
                                {"+rts", PttMethod::rts, Invert::off},     {"-rts", PttMethod::rts, Invert::on},
                                {"dtr", PttMethod::dtr, Invert::keep},     {"+dtr", PttMethod::dtr, Invert::off},
                                {"-dtr", PttMethod::dtr, Invert::on},      {"icom", PttMethod::icom, Invert::keep},
                                {"yaesu", PttMethod::yaesu, Invert::keep}, {"kenwood", PttMethod::kenwood, Invert::keep},
                                {"cat", PttMethod::cat, Invert::keep}};

bool all_digits(const std::string& text) {
    if (text.empty()) return false;
    for (size_t i = 0; i < text.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    return true;
}

bool all_hex(const std::string& text) {
    if (text.empty()) return false;
    for (size_t i = 0; i < text.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(text[i]))) return false;
    return true;
}

bool uses_line(PttMethod method) {
    return method == PttMethod::rts || method == PttMethod::dtr;
}

bool uses_cat(PttMethod method) {
    return method == PttMethod::icom || method == PttMethod::yaesu || method == PttMethod::kenwood ||
           method == PttMethod::cat;
}

bool device_spec(const std::string& option, const std::string& value, std::string& error) {
    if (parse_device_spec(value).kind != DeviceKind::unknown) return true;
    error = option + ": '" + value +
            "' is not a device (coreaudio:<#|name part|UID> on macOS, alsa:<name> on Linux, default, wav:<path> or "
            "null; --list-devices shows the devices)";
    return false;
}

std::string method_names() {
    std::string names;
    for (size_t i = 0; i < sizeof(k_methods) / sizeof(k_methods[0]); ++i)
        names += std::string(i == 0 ? "" : ", ") + k_methods[i].name;
    return names;
}

std::string rate_names() {
    std::string names;
    for (size_t i = 0; i < sizeof(k_cat_rates) / sizeof(k_cat_rates[0]); ++i)
        names += (i == 0 ? "" : ", ") + std::to_string(k_cat_rates[i]);
    return names;
}

std::string line(const std::string& option, const std::string& text) {
    std::string padded = "  " + option;
    padded.resize(std::max(padded.size() + 1, k_help_text_column), ' ');
    return padded + text + "\n";
}

}  // namespace

RadioOptions::RadioOptions(unsigned program_uses) : uses(program_uses), list_devices(false), rate_hz(0) {}

bool RadioOptions::takes(const std::string& option, bool& has_value) const {
    for (size_t i = 0; i < sizeof(k_options) / sizeof(k_options[0]); ++i) {
        if (option != k_options[i].name) continue;
        const unsigned needed = k_options[i].uses == k_any_device ? (k_radio_input | k_radio_output) : k_options[i].uses;
        if ((uses & needed) == 0) return false;
        has_value = k_options[i].has_value;
        return true;
    }
    return false;
}

bool RadioOptions::apply(const std::string& option, const std::string& value, std::string& error) {
    if (option == "--list-devices") {
        list_devices = true;
    } else if (option == "-d" || option == "--input" || option == "--output") {
        if (!device_spec(option, value, error)) return false;
        if (option != "--output" && (uses & k_radio_input) != 0) input = value;
        if (option != "--input" && (uses & k_radio_output) != 0) output = value;
    } else if (option == "-r") {
        const unsigned long rate = all_digits(value) && value.size() <= k_max_rate_digits
                                       ? std::strtoul(value.c_str(), nullptr, k_decimal)
                                       : 0;
        if (rate < k_min_device_rate_hz || rate > k_max_device_rate_hz) {
            error = "-r: '" + value + "' is not a sample rate of " + std::to_string(k_min_device_rate_hz) + ".." +
                    std::to_string(k_max_device_rate_hz) + " Hz";
            return false;
        }
        rate_hz = static_cast<uint32_t>(rate);
    } else if (option == "--ptt") {
        return parse_ptt_method(value, ptt, error);
    } else if (option == "--ptt-device") {
        if (value.empty() || value[0] == '-') {
            error = "--ptt-device needs a serial port (/dev/cu.usbserial-..., /dev/ttyUSB0), not '" + value + "'";
            return false;
        }
        ptt.device = value;
    } else if (option == "--ptt-invert") {
        ptt.invert = true;
    } else if (option == "--cat-rate") {
        return parse_cat_rate(value, ptt.cat_rate, error);
    } else if (option == "--cat-addr") {
        return parse_cat_address(value, ptt.cat_address, error);
    } else if (option == "--cat-tx-on" || option == "--cat-tx-off") {
        std::vector<uint8_t>& command = option == "--cat-tx-on" ? ptt.cat_on : ptt.cat_off;
        if (!parse_hex_bytes(value, command, error)) {
            error = option + ": " + error;
            return false;
        }
    } else {
        error = option + " is not a sound device or PTT option";
        return false;
    }
    return true;
}

bool RadioOptions::check(std::string& error) const {
    const PttMethod method = ptt.method;
    const std::string name = std::string("--ptt ") + ptt_method_name(method);
    if ((uses_line(method) || uses_cat(method)) && ptt.device.empty()) {
        error = name + " needs --ptt-device (the serial port)";
    } else if (method == PttMethod::vox && !ptt.device.empty()) {
        error = "--ptt-device " + ptt.device + " needs --ptt rts, dtr, icom, yaesu, kenwood or cat (the PTT is vox)";
    } else if (ptt.invert && !uses_line(method)) {
        error = "--ptt-invert (and -rts, -dtr) applies to --ptt rts and dtr only, not to " + name;
    } else if (ptt.cat_rate != k_default_cat_rate && !uses_cat(method)) {
        error = "--cat-rate applies to --ptt icom, yaesu, kenwood and cat, not to " + name;
    } else if (ptt.cat_address != k_default_icom_address && method != PttMethod::icom) {
        error = "--cat-addr applies to --ptt icom, not to " + name;
    } else if ((!ptt.cat_on.empty() || !ptt.cat_off.empty()) && method != PttMethod::cat) {
        error = "--cat-tx-on and --cat-tx-off apply to --ptt cat, not to " + name;
    } else if (method == PttMethod::cat && (ptt.cat_on.empty() || ptt.cat_off.empty())) {
        error = "--ptt cat needs both --cat-tx-on and --cat-tx-off";
    } else {
        return true;
    }
    return false;
}

std::string RadioOptions::help() const {
    const bool in = (uses & k_radio_input) != 0;
    const bool out = (uses & k_radio_output) != 0;
    std::string text;
    if (in || out) {
        text += "Sound devices:\n";
        text += line("--list-devices", "list every audio input and output (number, name, channels, rates, UID); exit");
        text += line("-d SPEC", std::string("the ") + (in && out ? "input and output" : (in ? "input" : "output")) +
                                    " device: coreaudio:<#|name part|UID> (macOS), alsa:<name> (Linux),");
        text += line("", "default, wav:<path> or null; a name part matching several devices is refused");
        if (in && out) {
            text += line("--input SPEC", "the input device alone (the radio's receive audio)");
            text += line("--output SPEC", "the output device alone (audio to the radio's modulation input)");
        } else if (in) {
            text += line("--input SPEC", "the same as -d");
        } else {
            text += line("--output SPEC", "the same as -d");
        }
        text += line("-r HZ", "open a live device at this rate (default: the device's own; CoreAudio: its nominal rate)");
    }
    if ((uses & k_radio_ptt) != 0) {
        text += "PTT:\n";
        text += line("--ptt METHOD", method_names() + " (default vox)");
        text += line("--ptt-device DEV", "the serial port of rts, dtr and the CAT methods");
        text += line("--ptt-invert", "lower the line to key (the same as -rts or -dtr)");
        text += line("--cat-rate BAUD", "CAT serial speed, 8N1: " + rate_names() + " (default " +
                                            std::to_string(k_default_cat_rate) + ")");
        text += line("--cat-addr ADDR", "icom: the radio's CI-V address in hex, 0x94 or 94h (default 0x94, the IC-7300's;");
        text += line("", "IC-705: 0xA4; the radio's CI-V menu shows it)");
        text += line("--cat-tx-on HEX", "cat: the bytes that key the radio, e.g. FEFE94E01C0001FD");
        text += line("--cat-tx-off HEX", "cat: the bytes that unkey it, e.g. FEFE94E01C0000FD");
    }
    return text;
}

bool parse_ptt_method(const std::string& text, PttOptions& options, std::string& error) {
    for (size_t i = 0; i < sizeof(k_methods) / sizeof(k_methods[0]); ++i) {
        if (text != k_methods[i].name) continue;
        options.method = k_methods[i].method;
        if (k_methods[i].invert != Invert::keep) options.invert = k_methods[i].invert == Invert::on;
        return true;
    }
    error = "--ptt: '" + text + "' is not one of " + method_names();
    return false;
}

bool parse_cat_address(const std::string& text, uint8_t& address, std::string& error) {
    std::string digits;
    if (text.compare(0, k_hex_prefix.size(), k_hex_prefix) == 0 ||
        text.compare(0, k_hex_prefix_upper.size(), k_hex_prefix_upper) == 0)
        digits = text.substr(k_hex_prefix.size());
    else if (text.size() > 1 && std::tolower(static_cast<unsigned char>(text[text.size() - 1])) == k_hex_suffix)
        digits = text.substr(0, text.size() - 1);
    if (!all_hex(digits) || digits.size() > k_max_address_digits) {
        error = "--cat-addr: '" + text + "' is not a CI-V address: write it in hex, 0x94 or 94h (the radio's CI-V menu "
                                         "shows it)";
        return false;
    }
    const unsigned long value = std::strtoul(digits.c_str(), nullptr, k_hexadecimal);
    if (value > k_max_civ_radio_address) {
        error = "--cat-addr: " + text + " is not a radio's address (E0h is the computer's, FDh and FEh frame CI-V messages)";
        return false;
    }
    address = static_cast<uint8_t>(value);
    return true;
}

bool parse_cat_rate(const std::string& text, uint32_t& rate, std::string& error) {
    if (all_digits(text) && text.size() <= k_max_rate_digits) {
        const unsigned long value = std::strtoul(text.c_str(), nullptr, k_decimal);
        for (size_t i = 0; i < sizeof(k_cat_rates) / sizeof(k_cat_rates[0]); ++i) {
            if (k_cat_rates[i] != value) continue;
            rate = k_cat_rates[i];
            return true;
        }
    }
    error = "--cat-rate: '" + text + "' is not one of " + rate_names() + " baud";
    return false;
}

}  // namespace pc
}  // namespace unlimited
