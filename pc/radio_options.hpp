#pragma once

#include "ptt.hpp"

#include <cstdint>
#include <string>

// The command-line options every program shares for its sound devices and its PTT (spec 12.3 to 12.5), with the
// names of AX25Toolkit's kiss_modem: --list-devices, -d, --input, --output, -r, --ptt, --ptt-device, --ptt-invert,
// --cat-rate, --cat-addr, --cat-tx-on, --cat-tx-off. A program reads its own options and hands every other one here:
//
//   pc::RadioOptions radio(pc::k_radio_output | pc::k_radio_ptt);
//   while (args.next(option)) {
//       bool has_value = false;
//       if (radio.takes(option, has_value)) {
//           if (!radio.apply(option, has_value ? args.value(option) : std::string(), error)) fail(error);
//           continue;
//       }
//       ...the program's own options...
//   }
//   if (!radio.check(error)) fail(error);
namespace unlimited {
namespace pc {

// Which of the shared options a program takes (bits); --list-devices and -r come with any device.
const unsigned k_radio_input = 1u;   // --input; -d names the input
const unsigned k_radio_output = 2u;  // --output; -d names the output
const unsigned k_radio_ptt = 4u;     // --ptt and the rest of the PTT family

const std::uint32_t k_min_device_rate_hz = 8000;
const std::uint32_t k_max_device_rate_hz = 384000;

struct RadioOptions {
    explicit RadioOptions(unsigned uses);

    // Whether `option` is one of the shared options this program takes; has_value tells whether a value follows it.
    bool takes(const std::string& option, bool& has_value) const;
    // Applies one option taken by takes() with its value (empty for a flag); false with the reason when refused.
    bool apply(const std::string& option, const std::string& value, std::string& error);
    // The rules between options, once all are read: a serial port for rts, dtr and the CAT methods, both commands for
    // cat, and no PTT option that the chosen method does not use.
    bool check(std::string& error) const;
    // The --help lines of the options this program takes.
    std::string help() const;

    unsigned uses;           // k_radio_* bits
    bool list_devices;       // --list-devices
    std::string input;       // --input or -d: a device spec (pc::open_input); empty when not given
    std::string output;      // --output or -d: a device spec (pc::open_output); empty when not given
    std::uint32_t rate_hz;   // -r: the rate to open a live device at; 0 when not given (the device's own)
    PttOptions ptt;
};

// The pieces, for tests and for programs that read options their own way.
bool parse_ptt_method(const std::string& text, PttOptions& options, std::string& error);  // sets invert for +/- forms
bool parse_cat_address(const std::string& text, std::uint8_t& address, std::string& error);  // 0x94 or 94h
bool parse_cat_rate(const std::string& text, std::uint32_t& rate, std::string& error);

}  // namespace pc
}  // namespace unlimited
