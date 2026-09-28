#include "ptt.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace unlimited {
namespace pc {

using std::size_t;
using std::uint32_t;
using std::uint8_t;

namespace {

const uint8_t k_civ_preamble = 0xFE;
const uint8_t k_civ_end = 0xFD;
const uint8_t k_civ_ptt = 0x1C;  // command 1C 00: the transmitter
const uint8_t k_civ_ptt_sub = 0x00;
const uint8_t k_civ_transmit = 0x01;
const uint8_t k_civ_receive = 0x00;
const char* const k_yaesu_on = "TX1;";
const char* const k_yaesu_off = "TX0;";
const char* const k_kenwood_on = "TX;";
const char* const k_kenwood_off = "RX;";
const unsigned k_nibble_bits = 4;
const uint8_t k_low_nibble = 0x0F;
const char k_hex_digits[] = "0123456789ABCDEF";
const size_t k_hex_digit_count = sizeof(k_hex_digits) - 1;
const speed_t k_cat_speeds[] = {B1200, B2400, B4800, B9600, B19200, B38400, B57600, B115200};  // of k_cat_rates
static_assert(sizeof(k_cat_speeds) / sizeof(k_cat_speeds[0]) == sizeof(k_cat_rates) / sizeof(k_cat_rates[0]),
              "one termios speed per CAT rate");

std::string system_error(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

bool termios_speed(uint32_t rate, speed_t& speed) {
    for (size_t i = 0; i < sizeof(k_cat_rates) / sizeof(k_cat_rates[0]); ++i) {
        if (k_cat_rates[i] != rate) continue;
        speed = k_cat_speeds[i];
        return true;
    }
    return false;
}

std::vector<uint8_t> text_bytes(const char* text) {
    return std::vector<uint8_t>(text, text + std::strlen(text));
}

int hex_value(char c) {
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (size_t i = 0; i < k_hex_digit_count; ++i)
        if (k_hex_digits[i] == upper) return static_cast<int>(i);
    return -1;
}

int open_port(const std::string& device, std::string& error) {
    if (device.empty()) {
        error = "the PTT needs a serial port (--ptt-device)";
        return -1;
    }
    const int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) error = system_error("cannot open " + device);
    return fd;
}

// An inverted line keys when low; closing a port lowers RTS and DTR when HUPCL is set, which would key the radio
// after the program ends, so HUPCL is cleared and the line stays high (unkeyed).
bool keep_lines_on_close(int fd, const std::string& device, std::string& error) {
    termios settings;
    if (::tcgetattr(fd, &settings) == 0) {
        settings.c_cflag &= ~static_cast<tcflag_t>(HUPCL);
        if (::tcsetattr(fd, TCSANOW, &settings) == 0) return true;
    }
    error = system_error("cannot keep the lines of " + device + " on close");
    return false;
}

// Raw 8N1 at rate: no echo, no line editing, no flow control (RTS may carry something else), reads that never wait.
bool configure_cat_port(int fd, const std::string& device, uint32_t rate, std::string& error) {
    speed_t speed = B0;
    if (!termios_speed(rate, speed)) {
        error = "--cat-rate " + std::to_string(rate) + " is not a supported serial speed";
        return false;
    }
    termios settings;
    if (::tcgetattr(fd, &settings) != 0) {
        error = system_error("cannot read the settings of " + device);
        return false;
    }
    ::cfmakeraw(&settings);
    settings.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB | CSTOPB | CRTSCTS);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    if (::cfsetispeed(&settings, speed) != 0 || ::cfsetospeed(&settings, speed) != 0 ||
        ::tcsetattr(fd, TCSANOW, &settings) != 0) {
        error = system_error("cannot set " + device + " to " + std::to_string(rate) + " baud 8N1");
        return false;
    }
    ::tcflush(fd, TCIOFLUSH);
    return true;
}

std::string port_text(const PttOptions& options) {
    return " on " + options.device + " at " + std::to_string(options.cat_rate) + " baud";
}

// RTS or DTR: TIOCMBIS raises the line, TIOCMBIC lowers it; the other line is never written.
class LinePtt final : public Ptt {
public:
    LinePtt(int fd, const PttOptions& options, ModemLineControl control)
        : fd_(fd), options_(options), control_(control) {}

    ~LinePtt() {
        if (keyed_) key(false);
        ::close(fd_);
    }

    bool drive(bool on) {
        const int line = options_.method == PttMethod::rts ? TIOCM_RTS : TIOCM_DTR;
        const bool high = on != options_.invert;
        return control_(fd_, high ? TIOCMBIS : TIOCMBIC, line) == 0;
    }

    bool key(bool on) override {
        if (on == keyed_) return true;
        if (!drive(on)) return false;
        keyed_ = on;
        return true;
    }

    std::string description() const override {
        return std::string(options_.method == PttMethod::rts ? "RTS" : "DTR") + " on " + options_.device +
               (options_.invert ? ", inverted (low keys)" : " (high keys)");
    }

private:
    int fd_;
    PttOptions options_;
    ModemLineControl control_;
};

// A CAT command per key or unkey. What the radio sends back (the echo of "CI-V USB Echo Back", its FB/FA answer,
// transceive data) is flushed before each command: never read, never waited for. Writes never block: a port that
// does not take a whole command at once fails the key.
class CatPtt final : public Ptt {
public:
    CatPtt(int fd, const PttOptions& options)
        : fd_(fd), options_(options), on_(cat_command(options, true)), off_(cat_command(options, false)) {}

    ~CatPtt() {
        if (keyed_) key(false);
        ::close(fd_);
    }

    bool send(bool on) {
        const std::vector<uint8_t>& command = on ? on_ : off_;
        ::tcflush(fd_, TCIFLUSH);
        ssize_t written = 0;
        do {
            written = ::write(fd_, &command[0], command.size());
        } while (written < 0 && errno == EINTR);
        return written == static_cast<ssize_t>(command.size());
    }

    bool key(bool on) override {
        if (on == keyed_) return true;
        if (!send(on)) return false;
        keyed_ = on;
        return true;
    }

    std::string description() const override {
        switch (options_.method) {
        case PttMethod::icom:
            return "Icom CI-V, address " + hex_text(std::vector<uint8_t>(1, options_.cat_address)) + "h" +
                   port_text(options_);
        case PttMethod::yaesu:
            return std::string("Yaesu CAT ") + k_yaesu_on + " / " + k_yaesu_off + port_text(options_);
        case PttMethod::kenwood:
            return std::string("Kenwood CAT ") + k_kenwood_on + " / " + k_kenwood_off + port_text(options_);
        default:
            return "CAT " + hex_text(on_) + " / " + hex_text(off_) + port_text(options_);
        }
    }

private:
    int fd_;
    PttOptions options_;
    std::vector<uint8_t> on_;
    std::vector<uint8_t> off_;
};

}  // namespace

PttOptions::PttOptions()
    : method(PttMethod::vox),
      invert(false),
      cat_rate(k_default_cat_rate),
      cat_address(k_default_icom_address) {}

Ptt::Ptt() : keyed_(false) {}

Ptt::~Ptt() {}

bool Ptt::key(bool on) {
    keyed_ = on;
    return true;
}

std::string Ptt::description() const {
    return "VOX (the lead tone keys the radio)";
}

bool Ptt::keyed() const {
    return keyed_;
}

int ioctl_modem_lines(int fd, unsigned long request, int bits) {
    return ::ioctl(fd, request, &bits);
}

std::unique_ptr<Ptt> open_ptt(const PttOptions& options, std::string& error, ModemLineControl control) {
    switch (options.method) {
    case PttMethod::vox:
        return std::unique_ptr<Ptt>(new Ptt());
    case PttMethod::rts:
    case PttMethod::dtr: {
        const int fd = open_port(options.device, error);
        if (fd < 0) return std::unique_ptr<Ptt>();
        if (options.invert && !keep_lines_on_close(fd, options.device, error)) {
            ::close(fd);
            return std::unique_ptr<Ptt>();
        }
        std::unique_ptr<LinePtt> ptt(new LinePtt(fd, options, control));
        if (!ptt->drive(false)) {
            error = system_error("cannot set " + std::string(options.method == PttMethod::rts ? "RTS" : "DTR") +
                                 " on " + options.device);
            return std::unique_ptr<Ptt>();
        }
        return std::unique_ptr<Ptt>(ptt.release());
    }
    case PttMethod::icom:
    case PttMethod::yaesu:
    case PttMethod::kenwood:
    case PttMethod::cat: {
        if (cat_command(options, true).empty() || cat_command(options, false).empty()) {
            error = "--ptt cat needs both --cat-tx-on and --cat-tx-off";
            return std::unique_ptr<Ptt>();
        }
        const int fd = open_port(options.device, error);
        if (fd < 0) return std::unique_ptr<Ptt>();
        if (!configure_cat_port(fd, options.device, options.cat_rate, error)) {
            ::close(fd);
            return std::unique_ptr<Ptt>();
        }
        std::unique_ptr<CatPtt> ptt(new CatPtt(fd, options));
        if (!ptt->send(false)) {
            error = system_error("cannot send the unkey command to " + options.device);
            return std::unique_ptr<Ptt>();
        }
        return std::unique_ptr<Ptt>(ptt.release());
    }
    }
    error = "unknown PTT method";
    return std::unique_ptr<Ptt>();
}

std::vector<uint8_t> cat_command(const PttOptions& options, bool on) {
    switch (options.method) {
    case PttMethod::icom: {
        const uint8_t command[] = {k_civ_preamble, k_civ_preamble, options.cat_address, k_icom_controller_address,
                                   k_civ_ptt,      k_civ_ptt_sub,  on ? k_civ_transmit : k_civ_receive, k_civ_end};
        return std::vector<uint8_t>(command, command + sizeof(command));
    }
    case PttMethod::yaesu:
        return text_bytes(on ? k_yaesu_on : k_yaesu_off);
    case PttMethod::kenwood:
        return text_bytes(on ? k_kenwood_on : k_kenwood_off);
    case PttMethod::cat:
        return on ? options.cat_on : options.cat_off;
    case PttMethod::vox:
    case PttMethod::rts:
    case PttMethod::dtr:
        break;
    }
    return std::vector<uint8_t>();
}

bool parse_hex_bytes(const std::string& text, std::vector<uint8_t>& bytes, std::string& error) {
    std::vector<uint8_t> parsed;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        const int high = hex_value(text[i]);
        const int low = i + 1 < text.size() ? hex_value(text[i + 1]) : -1;
        if (high < 0 || low < 0) {
            error = "'" + text + "' is not hex bytes: pairs of hex digits, spaces allowed between them (FEFE94E01C0001FD)";
            return false;
        }
        parsed.push_back(static_cast<uint8_t>((high << k_nibble_bits) | low));
        i += 2;
    }
    if (parsed.empty()) {
        error = "'" + text + "' holds no bytes";
        return false;
    }
    bytes = parsed;
    return true;
}

std::string hex_text(const std::vector<uint8_t>& bytes) {
    std::string text;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) text += ' ';
        text += k_hex_digits[bytes[i] >> k_nibble_bits];
        text += k_hex_digits[bytes[i] & k_low_nibble];
    }
    return text;
}

const char* ptt_method_name(PttMethod method) {
    switch (method) {
    case PttMethod::vox: return "vox";
    case PttMethod::rts: return "rts";
    case PttMethod::dtr: return "dtr";
    case PttMethod::icom: return "icom";
    case PttMethod::yaesu: return "yaesu";
    case PttMethod::kenwood: return "kenwood";
    case PttMethod::cat: return "cat";
    }
    return "?";
}

}  // namespace pc
}  // namespace unlimited
