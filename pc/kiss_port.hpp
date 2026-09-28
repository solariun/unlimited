#pragma once

#include <cstdint>
#include <memory>
#include <string>

// The computer's side of unlimited_modem (spec 12.3): a pseudo-terminal whose slave a KISS program opens through a
// symbolic link (--link), or a serial port (--serial). Either way the descriptor is non-blocking and raw both ways
// (8 bits, no echo, no line editing, no CR/LF translation, no flow control): every KISS byte passes as it is.
// POSIX calls only.
namespace unlimited {
namespace pc {

const char* const k_default_kiss_link = "/tmp/unlimited";
const std::uint32_t k_default_serial_baud = 115200;
const std::uint32_t k_serial_bauds[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400};

class KissPort {
public:
    ~KissPort();  // removes the link when it still points to this PTY, then closes
    KissPort(const KissPort&) = delete;
    KissPort& operator=(const KissPort&) = delete;

    // A PTY (openpty): the slave is set raw and kept open for the modem's life (a client closing it never hangs up
    // the master), and `link` is made a symbolic link to it. An old symbolic link there is replaced (a crashed run
    // leaves one; replaced() names its target); anything else at that path is refused, never removed.
    static std::unique_ptr<KissPort> open_pty(const std::string& link, std::string& error);
    // A serial port, raw 8N1 at `baud` (one of k_serial_bauds), no flow control.
    static std::unique_ptr<KissPort> open_serial(const std::string& device, std::uint32_t baud, std::string& error);

    int fd() const;                       // read what the computer sends, write what goes to it
    const std::string& name() const;      // the PTY's slave (/dev/ttys012) or the serial device
    const std::string& link() const;      // empty for a serial port
    const std::string& replaced() const;  // the target of the old link open_pty() replaced; empty when none
    void remove_link();                   // now, if it still points to this PTY (another program may have taken it)

private:
    KissPort(int fd, int slave_fd, const std::string& name);

    int fd_;
    int slave_fd_;  // the PTY's own slave, kept open; -1 for a serial port
    std::string name_;
    std::string link_;
    std::string replaced_;
};

bool serial_baud_supported(std::uint32_t baud);

}  // namespace pc
}  // namespace unlimited
