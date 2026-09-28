#include "kiss_port.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace unlimited {
namespace pc {

using std::size_t;
using std::uint32_t;

namespace {

const speed_t k_serial_speeds[] = {B1200, B2400, B4800, B9600, B19200, B38400, B57600, B115200, B230400};
static_assert(sizeof(k_serial_speeds) / sizeof(k_serial_speeds[0]) == sizeof(k_serial_bauds) / sizeof(k_serial_bauds[0]),
              "one termios speed per serial rate");
const size_t k_path_max = 4096;
const char* const k_temporary_suffix = ".new.";

std::string system_error(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

bool termios_speed(uint32_t baud, speed_t& speed) {
    for (size_t i = 0; i < sizeof(k_serial_bauds) / sizeof(k_serial_bauds[0]); ++i) {
        if (k_serial_bauds[i] != baud) continue;
        speed = k_serial_speeds[i];
        return true;
    }
    return false;
}

// Raw 8N1: no echo, no line editing or signals, no CR/LF translation, no flow control. A blocking read waits for
// a byte (VMIN 1, VTIME 0: a KISS program reading the PTY's slave never takes "no byte yet" for its end); the modem's
// own descriptor is non-blocking.
bool make_raw(int fd, termios& settings) {
    if (::tcgetattr(fd, &settings) != 0) return false;
    ::cfmakeraw(&settings);
    settings.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB | CSTOPB | CRTSCTS);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;
    return true;
}

bool non_blocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

bool read_link(const std::string& path, std::string& target) {
    std::vector<char> buffer(k_path_max);
    const ssize_t size = ::readlink(path.c_str(), &buffer[0], buffer.size() - 1);
    if (size < 0) return false;
    target.assign(&buffer[0], static_cast<size_t>(size));
    return true;
}

}  // namespace

bool serial_baud_supported(uint32_t baud) {
    speed_t speed = B0;
    return termios_speed(baud, speed);
}

KissPort::KissPort(int fd, int slave_fd, const std::string& name) : fd_(fd), slave_fd_(slave_fd), name_(name) {}

KissPort::~KissPort() {
    remove_link();
    if (slave_fd_ >= 0) ::close(slave_fd_);
    ::close(fd_);
}

std::unique_ptr<KissPort> KissPort::open_pty(const std::string& link, std::string& error) {
    int master = -1;
    int slave = -1;
    if (::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) {
        error = system_error("cannot open a pseudo-terminal");
        return std::unique_ptr<KissPort>();
    }
    const char* slave_name = ::ttyname(slave);
    std::unique_ptr<KissPort> port(new KissPort(master, slave, slave_name != nullptr ? slave_name : ""));
    termios settings;
    if (port->name_.empty() || !make_raw(slave, settings) || ::tcsetattr(slave, TCSANOW, &settings) != 0 ||
        !non_blocking(master) || ::fcntl(slave, F_SETFD, FD_CLOEXEC) != 0) {
        error = system_error("cannot set up the pseudo-terminal");
        return std::unique_ptr<KissPort>();
    }
    if (link.empty()) return port;

    struct stat status;
    if (::lstat(link.c_str(), &status) == 0) {
        if (!S_ISLNK(status.st_mode)) {
            error = "--link " + link + " exists and is not a symbolic link: it is left as it is (choose another path)";
            return std::unique_ptr<KissPort>();
        }
        read_link(link, port->replaced_);
    }
    // A new link beside the path, renamed over it: the path always names a link, never nothing.
    const std::string temporary = link + k_temporary_suffix + std::to_string(::getpid());
    ::unlink(temporary.c_str());
    if (::symlink(port->name_.c_str(), temporary.c_str()) != 0) {
        error = system_error("cannot create the link " + link);
        return std::unique_ptr<KissPort>();
    }
    if (::rename(temporary.c_str(), link.c_str()) != 0) {
        error = system_error("cannot create the link " + link);
        ::unlink(temporary.c_str());
        return std::unique_ptr<KissPort>();
    }
    port->link_ = link;
    return port;
}

std::unique_ptr<KissPort> KissPort::open_serial(const std::string& device, uint32_t baud, std::string& error) {
    speed_t speed = B0;
    if (!termios_speed(baud, speed)) {
        error = "--serial-baud " + std::to_string(baud) + " is not a supported serial speed";
        return std::unique_ptr<KissPort>();
    }
    const int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        error = system_error("cannot open " + device);
        return std::unique_ptr<KissPort>();
    }
    std::unique_ptr<KissPort> port(new KissPort(fd, -1, device));
    termios settings;
    if (!make_raw(fd, settings) || ::cfsetispeed(&settings, speed) != 0 || ::cfsetospeed(&settings, speed) != 0 ||
        ::tcsetattr(fd, TCSANOW, &settings) != 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        error = system_error("cannot set " + device + " to " + std::to_string(baud) + " baud 8N1");
        return std::unique_ptr<KissPort>();
    }
    ::tcflush(fd, TCIOFLUSH);
    return port;
}

int KissPort::fd() const {
    return fd_;
}

const std::string& KissPort::name() const {
    return name_;
}

const std::string& KissPort::link() const {
    return link_;
}

const std::string& KissPort::replaced() const {
    return replaced_;
}

void KissPort::remove_link() {
    if (link_.empty()) return;
    std::string target;
    if (read_link(link_, target) && target == name_) ::unlink(link_.c_str());
    link_.clear();
}

}  // namespace pc
}  // namespace unlimited
