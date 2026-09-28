// The computer's side of unlimited_modem (spec 12.3): the PTY is raw both ways and linked at --link, the link goes
// when the modem does (when it is still ours), and a serial port is raw 8N1 at its speed.
#include "kiss_port.hpp"
#include "test_harness.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

using unlimited::pc::KissPort;

namespace {

using std::size_t;
using std::uint8_t;

const int k_wait_ms = 2000;
const size_t k_path_max = 4096;

// A fresh directory for the links of one test.
std::string temporary_directory() {
    const char* base = std::getenv("TMPDIR");
    std::string pattern = std::string(base != nullptr && *base != '\0' ? base : "/tmp") + "/unlimited_kiss_XXXXXX";
    std::vector<char> path(pattern.begin(), pattern.end());
    path.push_back('\0');
    return ::mkdtemp(&path[0]) != nullptr ? std::string(&path[0]) : std::string();
}

bool exists(const std::string& path) {
    struct stat status;
    return ::lstat(path.c_str(), &status) == 0;
}

std::string link_target(const std::string& path) {
    std::vector<char> buffer(k_path_max);
    const ssize_t size = ::readlink(path.c_str(), &buffer[0], buffer.size() - 1);
    return size < 0 ? std::string() : std::string(&buffer[0], static_cast<size_t>(size));
}

// Reads exactly `count` bytes from fd, waiting at most k_wait_ms for each piece.
std::vector<uint8_t> read_exactly(int fd, size_t count) {
    std::vector<uint8_t> bytes;
    while (bytes.size() < count) {
        pollfd waiting = {fd, POLLIN, 0};
        if (::poll(&waiting, 1, k_wait_ms) <= 0) break;
        uint8_t buffer[512];
        const ssize_t got = ::read(fd, buffer, std::min(sizeof(buffer), count - bytes.size()));
        if (got <= 0) break;
        bytes.insert(bytes.end(), buffer, buffer + got);
    }
    return bytes;
}

bool write_all(int fd, const std::vector<uint8_t>& bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t wrote = ::write(fd, &bytes[done], bytes.size() - done);
        if (wrote < 0 && errno != EAGAIN && errno != EINTR) return false;
        if (wrote > 0) done += static_cast<size_t>(wrote);
    }
    return true;
}

bool raw(int fd) {
    termios settings;
    if (::tcgetattr(fd, &settings) != 0) return false;
    const bool no_input_processing = (settings.c_iflag & (ICRNL | INLCR | IGNCR | ISTRIP | IXON | IXOFF | BRKINT)) == 0;
    const bool no_output_processing = (settings.c_oflag & OPOST) == 0;
    const bool no_line_discipline = (settings.c_lflag & (ICANON | ECHO | ECHONL | ISIG | IEXTEN)) == 0;
    const bool eight_bits = (settings.c_cflag & CSIZE) == CS8 && (settings.c_cflag & PARENB) == 0;
    const bool reads_wait_for_a_byte = settings.c_cc[VMIN] == 1 && settings.c_cc[VTIME] == 0;
    return no_input_processing && no_output_processing && no_line_discipline && eight_bits && reads_wait_for_a_byte;
}

std::vector<uint8_t> every_byte() {
    std::vector<uint8_t> bytes;
    for (unsigned value = 0; value < 256; ++value) bytes.push_back(static_cast<uint8_t>(value));
    return bytes;
}

}  // namespace

// The client opens the link as a KISS program would; every byte value passes unchanged both ways (C0, DB, CR, LF,
// ^C, ^S/^Q, DEL): the slave is raw. The link points to the slave and is removed with the port.
TEST(kiss_port_pty_is_raw_and_linked) {
    const std::string directory = temporary_directory();
    REQUIRE(!directory.empty());
    const std::string link = directory + "/kiss";
    std::string error;
    {
        std::unique_ptr<KissPort> port = KissPort::open_pty(link, error);
        REQUIRE(port != nullptr);
        CHECK(error.empty());
        CHECK_EQ(port->link(), link);
        CHECK(port->replaced().empty());
        CHECK_EQ(link_target(link), port->name());
        const int client = ::open(link.c_str(), O_RDWR | O_NOCTTY);
        REQUIRE(client >= 0);
        CHECK(raw(client));
        const std::vector<uint8_t> bytes = every_byte();
        CHECK(write_all(client, bytes));
        CHECK(read_exactly(port->fd(), bytes.size()) == bytes);
        CHECK(write_all(port->fd(), bytes));
        CHECK(read_exactly(client, bytes.size()) == bytes);
        // The client goes away: the master is not hung up (the modem keeps its own slave open).
        ::close(client);
        uint8_t nothing = 0;
        errno = 0;
        const ssize_t got = ::read(port->fd(), &nothing, 1);
        CHECK(got < 0 && errno == EAGAIN);
        const int again = ::open(link.c_str(), O_RDWR | O_NOCTTY);
        REQUIRE(again >= 0);
        CHECK(write_all(port->fd(), std::vector<uint8_t>(1, 0xC0)));
        CHECK(read_exactly(again, 1) == std::vector<uint8_t>(1, 0xC0));
        ::close(again);
    }
    CHECK(!exists(link));
    ::rmdir(directory.c_str());
}

// A link another program took over (it points elsewhere now) is left alone.
TEST(kiss_port_leaves_a_link_it_no_longer_owns) {
    const std::string directory = temporary_directory();
    REQUIRE(!directory.empty());
    const std::string link = directory + "/kiss";
    const std::string other = directory + "/other";
    std::string error;
    {
        std::unique_ptr<KissPort> port = KissPort::open_pty(link, error);
        REQUIRE(port != nullptr);
        ::unlink(link.c_str());
        REQUIRE(::symlink(other.c_str(), link.c_str()) == 0);
    }
    CHECK(exists(link));
    CHECK_EQ(link_target(link), other);
    ::unlink(link.c_str());
    ::rmdir(directory.c_str());
}

// An old link (a run that crashed) is replaced and named; a file at the path is refused and kept.
TEST(kiss_port_replaces_old_links_and_refuses_files) {
    const std::string directory = temporary_directory();
    REQUIRE(!directory.empty());
    const std::string link = directory + "/kiss";
    const std::string stale = "/dev/ttys999-gone";
    REQUIRE(::symlink(stale.c_str(), link.c_str()) == 0);
    std::string error;
    {
        std::unique_ptr<KissPort> port = KissPort::open_pty(link, error);
        REQUIRE(port != nullptr);
        CHECK_EQ(port->replaced(), stale);
        CHECK_EQ(link_target(link), port->name());
    }
    CHECK(!exists(link));

    const int file = ::open(link.c_str(), O_CREAT | O_WRONLY, 0600);
    REQUIRE(file >= 0);
    ::close(file);
    std::unique_ptr<KissPort> refused = KissPort::open_pty(link, error);
    CHECK(refused == nullptr);
    CHECK(error.find("not a symbolic link") != std::string::npos);
    struct stat status;
    CHECK(::lstat(link.c_str(), &status) == 0 && S_ISREG(status.st_mode));
    ::unlink(link.c_str());
    ::rmdir(directory.c_str());
}

// --serial: raw 8N1 at the speed asked (a PTY's slave stands in for the serial device); unknown speeds refused.
TEST(kiss_port_serial_is_raw_8n1) {
    int master = -1;
    int slave = -1;
    REQUIRE(::openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
    const std::string device = ::ttyname(slave);
    std::string error;
    {
        std::unique_ptr<KissPort> port = KissPort::open_serial(device, 57600, error);
        REQUIRE(port != nullptr);
        CHECK(port->link().empty());
        CHECK(raw(port->fd()));
        termios settings;
        REQUIRE(::tcgetattr(port->fd(), &settings) == 0);
        CHECK(::cfgetospeed(&settings) == B57600);
        CHECK((settings.c_cflag & CRTSCTS) == 0);
        const std::vector<uint8_t> bytes = every_byte();
        CHECK(write_all(port->fd(), bytes));
        CHECK(read_exactly(master, bytes.size()) == bytes);
    }
    CHECK(KissPort::open_serial(device, 12345, error) == nullptr);
    CHECK(error.find("12345") != std::string::npos);
    CHECK(KissPort::open_serial("/dev/unlimited-no-such-port", 115200, error) == nullptr);
    CHECK(unlimited::pc::serial_baud_supported(115200));
    CHECK(!unlimited::pc::serial_baud_supported(110));
    ::close(slave);
    ::close(master);
}
