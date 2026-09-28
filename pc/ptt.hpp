#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Keying the transmitter (spec 12.4): VOX, an RTS or DTR line of a serial port, or a CAT command to the radio over a
// serial port (Icom CI-V, Yaesu, Kenwood, custom hex). Standard library and POSIX serial calls only.
namespace unlimited {
namespace pc {

enum class PttMethod { vox, rts, dtr, icom, yaesu, kenwood, cat };

const std::uint32_t k_default_cat_rate = 19200;
const std::uint32_t k_cat_rates[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};  // termios speeds on both systems
// The radio's CI-V address is set in its CI-V menu: 94h is the IC-7300's default, A4h the IC-705's.
const std::uint8_t k_default_icom_address = 0x94;
const std::uint8_t k_icom_controller_address = 0xE0;  // the computer's own CI-V address

struct PttOptions {
    PttOptions();

    PttMethod method;                   // --ptt (vox)
    std::string device;                 // --ptt-device: the serial port of rts, dtr and the CAT methods
    bool invert;                        // --ptt-invert, -rts, -dtr: the line is lowered to key
    std::uint32_t cat_rate;             // --cat-rate: baud, 8N1
    std::uint8_t cat_address;           // --cat-addr: icom
    std::vector<std::uint8_t> cat_on;   // --cat-tx-on: cat
    std::vector<std::uint8_t> cat_off;  // --cat-tx-off: cat
};

// The base class is VOX: keying drives nothing, the VOX lead tone keys the radio (spec 2.1). key() is idempotent
// (keying twice sends nothing the second time) and false when the line or the radio could not be driven; one thread
// keys. The serial methods start unkeyed and unkey in their destructors.
class Ptt {
public:
    Ptt();
    virtual ~Ptt();
    Ptt(const Ptt&) = delete;
    Ptt& operator=(const Ptt&) = delete;

    virtual bool key(bool on);
    virtual std::string description() const;
    bool keyed() const;

protected:
    bool keyed_;
};

// The modem-control call of an RTS or DTR PTT: sets (TIOCMBIS) or clears (TIOCMBIC) the lines in `bits` of the serial
// port fd; 0, or -1 with errno, like ioctl(). Tests pass a stand-in, since a PTY has no RTS or DTR.
typedef int (*ModemLineControl)(int fd, unsigned long request, int bits);
int ioctl_modem_lines(int fd, unsigned long request, int bits);

// The PTT `options` describe, its serial port open and set: RTS/DTR lowered to the unkeyed level (the other line is
// never touched), a CAT port raw 8N1 at cat_rate with the unkey command sent. nullptr and an error when the port
// cannot be opened or set.
std::unique_ptr<Ptt> open_ptt(const PttOptions& options, std::string& error,
                              ModemLineControl control = ioctl_modem_lines);

// The bytes a CAT method sends to key (on) or unkey the radio; empty for vox, rts and dtr.
//   icom:    FE FE <address> E0 1C 00 01 FD / FE FE <address> E0 1C 00 00 FD
//   yaesu:   "TX1;" / "TX0;"
//   kenwood: "TX;" / "RX;"
//   cat:     cat_on / cat_off
std::vector<std::uint8_t> cat_command(const PttOptions& options, bool on);

// "FEFE94E01C0001FD" or "FE FE 94 E0 1C 00 01 FD" (either case) -> bytes; false with the reason otherwise.
bool parse_hex_bytes(const std::string& text, std::vector<std::uint8_t>& bytes, std::string& error);
std::string hex_text(const std::vector<std::uint8_t>& bytes);  // "FE FE 94 E0 1C 00 01 FD"

const char* ptt_method_name(PttMethod method);

}  // namespace pc
}  // namespace unlimited
