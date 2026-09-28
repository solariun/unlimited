#include "ptt.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

// PTT and CAT (spec 12.4): the bytes each CAT method sends, hex parsing, keying through a PTY pair (the test plays the
// radio on the master side), and RTS/DTR through a stand-in for the modem-control call, since a PTY has no RTS or
// DTR lines. A PTY may pass bytes on asynchronously (Linux), so the tests wait for them with poll() and compare the
// whole stream: a command sent twice would show up in it.

using unlimited::pc::PttMethod;
using unlimited::pc::PttOptions;
using unlimited::pc::Ptt;

namespace {

using std::size_t;
using std::uint32_t;
using std::uint8_t;
using test::count_of;

const size_t k_read_chunk = 256;
const int k_patience_ms = 10000;  // bytes take microseconds; this only stops a hung test

std::vector<uint8_t> bytes(const char* text) {
    return std::vector<uint8_t>(text, text + std::string(text).size());
}

std::vector<uint8_t> civ(uint8_t address, bool on) {
    const uint8_t command[] = {0xFE, 0xFE, address, 0xE0, 0x1C, 0x00, static_cast<uint8_t>(on ? 0x01 : 0x00), 0xFD};
    return std::vector<uint8_t>(command, command + count_of(command));
}

std::vector<uint8_t> joined(const std::vector<std::vector<uint8_t> >& parts) {
    std::vector<uint8_t> all;
    for (size_t i = 0; i < parts.size(); ++i) all.insert(all.end(), parts[i].begin(), parts[i].end());
    return all;
}

bool readable(int fd) {
    pollfd waiting = {fd, POLLIN, 0};
    return ::poll(&waiting, 1, k_patience_ms) > 0;
}

// A PTY pair: the PTT opens the slave by its path, the test plays the radio on the master.
class PtyPair {
public:
    PtyPair() : master(-1), slave(-1) {
        if (::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) return;
        const char* name = ::ttyname(slave);
        if (name != nullptr) path = name;
        ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
    }

    ~PtyPair() {
        if (master >= 0) ::close(master);
        if (slave >= 0) ::close(slave);
    }

    bool ok() const { return master >= 0 && !path.empty(); }

    // The next `count` bytes the PTT wrote, waiting for them; fewer when they do not come.
    std::vector<uint8_t> receive(size_t count) {
        std::vector<uint8_t> all;
        while (all.size() < count && readable(master)) {
            uint8_t chunk[k_read_chunk];
            const ssize_t got = ::read(master, chunk, std::min(sizeof(chunk), count - all.size()));
            if (got <= 0) break;
            all.insert(all.end(), chunk, chunk + got);
        }
        return all;
    }

    // Whatever is there now, without waiting.
    void discard() {
        uint8_t chunk[k_read_chunk];
        while (::read(master, chunk, sizeof(chunk)) > 0) {
        }
    }

    void answer(const std::vector<uint8_t>& reply) {
        const ssize_t written = ::write(master, &reply[0], reply.size());
        CHECK_EQ(written, static_cast<ssize_t>(reply.size()));
    }

    termios settings() const {
        termios t;
        ::tcgetattr(slave, &t);
        return t;
    }

    // A new PTY's HUPCL differs between systems: set it, as on a serial port.
    void hang_up_on_close() {
        termios t = settings();
        t.c_cflag |= HUPCL;
        CHECK_EQ(::tcsetattr(slave, TCSANOW, &t), 0);
    }

    int master;
    int slave;
    std::string path;
};

// The stand-in for ioctl(TIOCMBIS/TIOCMBIC): records each call and keeps the lines' state.
struct LineLog {
    std::vector<unsigned long> requests;
    std::vector<int> bits;
    int lines = 0;
};

LineLog g_lines;

int fake_modem_lines(int, unsigned long request, int bits) {
    g_lines.requests.push_back(request);
    g_lines.bits.push_back(bits);
    if (request == static_cast<unsigned long>(TIOCMBIS)) g_lines.lines |= bits;
    if (request == static_cast<unsigned long>(TIOCMBIC)) g_lines.lines &= ~bits;
    return 0;
}

int failing_modem_lines(int, unsigned long, int) {
    errno = ENOTTY;
    return -1;
}

}  // namespace

TEST(ptt_cat_command_bytes) {
    PttOptions options;
    options.method = PttMethod::icom;
    CHECK(unlimited::pc::cat_command(options, true) == civ(0x94, true));  // IC-7300 by default
    CHECK(unlimited::pc::cat_command(options, false) == civ(0x94, false));
    options.cat_address = 0xA4;  // IC-705
    CHECK(unlimited::pc::cat_command(options, true) == civ(0xA4, true));
    CHECK_EQ(unlimited::pc::hex_text(unlimited::pc::cat_command(options, false)), std::string("FE FE A4 E0 1C 00 00 FD"));
    options.method = PttMethod::yaesu;
    CHECK(unlimited::pc::cat_command(options, true) == bytes("TX1;"));
    CHECK(unlimited::pc::cat_command(options, false) == bytes("TX0;"));
    options.method = PttMethod::kenwood;
    CHECK(unlimited::pc::cat_command(options, true) == bytes("TX;"));
    CHECK(unlimited::pc::cat_command(options, false) == bytes("RX;"));
    options.method = PttMethod::cat;
    options.cat_on = bytes("ON");
    options.cat_off = bytes("OFF");
    CHECK(unlimited::pc::cat_command(options, true) == bytes("ON"));
    CHECK(unlimited::pc::cat_command(options, false) == bytes("OFF"));
    const PttMethod none[] = {PttMethod::vox, PttMethod::rts, PttMethod::dtr};
    for (size_t i = 0; i < count_of(none); ++i) {
        options.method = none[i];
        CHECK(unlimited::pc::cat_command(options, true).empty());
    }
}

TEST(ptt_hex_bytes) {
    std::vector<uint8_t> parsed;
    std::string error;
    CHECK(unlimited::pc::parse_hex_bytes("FEFE94E01C0001FD", parsed, error));
    CHECK(parsed == civ(0x94, true));
    CHECK(unlimited::pc::parse_hex_bytes(" fe fe 94 e0 1c 00 00 fd ", parsed, error));
    CHECK(parsed == civ(0x94, false));
    CHECK(unlimited::pc::parse_hex_bytes("54 58 31 3B", parsed, error));
    CHECK(parsed == bytes("TX1;"));
    const char* const refused[] = {"", "   ", "F", "FEF", "FE F E", "FG", "0x94", "FE,FE", "FE:FE"};
    for (size_t i = 0; i < count_of(refused); ++i) {
        parsed = bytes("kept");
        error.clear();
        if (!CHECK(!unlimited::pc::parse_hex_bytes(refused[i], parsed, error))) NOTE("'%s' accepted", refused[i]);
        CHECK(!error.empty());
        CHECK(parsed == bytes("kept"));  // untouched on an error
    }
    CHECK_EQ(unlimited::pc::hex_text(bytes("TX;")), std::string("54 58 3B"));
    CHECK_EQ(unlimited::pc::hex_text(std::vector<uint8_t>()), std::string());
}

TEST(ptt_vox_drives_nothing) {
    PttOptions options;
    std::string error;
    std::unique_ptr<Ptt> ptt = unlimited::pc::open_ptt(options, error);
    REQUIRE(ptt != nullptr);
    CHECK(!ptt->keyed());
    CHECK(ptt->key(true));
    CHECK(ptt->keyed());
    CHECK(ptt->key(false));
    CHECK(ptt->description().find("VOX") != std::string::npos);
}

TEST(ptt_cat_keying_on_a_pty) {
    struct Case {
        PttMethod method;
        uint32_t rate;
        std::vector<uint8_t> on;
        std::vector<uint8_t> off;
        speed_t speed;
    };
    const Case cases[] = {{PttMethod::icom, 19200, civ(0x94, true), civ(0x94, false), B19200},
                          {PttMethod::yaesu, 38400, bytes("TX1;"), bytes("TX0;"), B38400},
                          {PttMethod::kenwood, 115200, bytes("TX;"), bytes("RX;"), B115200},
                          {PttMethod::cat, 4800, bytes("K1"), bytes("K0"), B4800}};
    for (size_t c = 0; c < count_of(cases); ++c) {
        PtyPair pty;
        REQUIRE(pty.ok());
        PttOptions options;
        options.method = cases[c].method;
        options.device = pty.path;
        options.cat_rate = cases[c].rate;
        if (cases[c].method == PttMethod::cat) {
            options.cat_on = cases[c].on;
            options.cat_off = cases[c].off;
        }
        std::string error;
        std::unique_ptr<Ptt> ptt = unlimited::pc::open_ptt(options, error);
        if (!CHECK(ptt != nullptr)) {
            NOTE("%s", error.c_str());
            continue;
        }
        NOTE("%s", ptt->description().c_str());
        // Raw 8N1 at the CAT rate, no flow control.
        const termios t = pty.settings();
        CHECK_EQ(cfgetospeed(&t), cases[c].speed);
        CHECK_EQ(t.c_cflag & CSIZE, static_cast<tcflag_t>(CS8));
        CHECK_EQ(t.c_cflag & (PARENB | CSTOPB | CRTSCTS), static_cast<tcflag_t>(0));
        CHECK_EQ(t.c_cflag & (CLOCAL | CREAD), static_cast<tcflag_t>(CLOCAL | CREAD));
        CHECK_EQ(t.c_lflag & (ICANON | ECHO | ISIG), static_cast<tcflag_t>(0));
        CHECK_EQ(t.c_iflag & (IXON | IXOFF | ICRNL), static_cast<tcflag_t>(0));
        CHECK_EQ(t.c_oflag & OPOST, static_cast<tcflag_t>(0));

        CHECK(ptt->key(true));
        CHECK(ptt->keyed());
        CHECK(ptt->key(true));  // idempotent: nothing sent again
        CHECK(ptt->key(false));
        CHECK(ptt->key(false));
        CHECK(!ptt->keyed());
        CHECK(ptt->key(true));
        ptt.reset();  // destroyed while keyed: unkeys
        std::vector<std::vector<uint8_t> > sent;
        sent.push_back(cases[c].off);  // unkeyed at start
        sent.push_back(cases[c].on);
        sent.push_back(cases[c].off);
        sent.push_back(cases[c].on);
        sent.push_back(cases[c].off);
        const std::vector<uint8_t> expected = joined(sent);
        CHECK(pty.receive(expected.size()) == expected);
    }
}

// The radio's echo ("CI-V USB Echo Back"), its FB answer and transceive data are dropped, never read or waited for.
TEST(ptt_cat_drops_what_the_radio_sends) {
    PtyPair pty;
    REQUIRE(pty.ok());
    PttOptions options;
    options.method = PttMethod::icom;
    options.device = pty.path;
    std::string error;
    std::unique_ptr<Ptt> ptt = unlimited::pc::open_ptt(options, error);
    REQUIRE(ptt != nullptr);
    CHECK(pty.receive(civ(0x94, false).size()) == civ(0x94, false));
    const uint8_t fb[] = {0xFE, 0xFE, 0xE0, 0x94, 0xFB, 0xFD};
    std::vector<uint8_t> reply = civ(0x94, false);  // the echo of the unkey at start
    reply.insert(reply.end(), fb, fb + count_of(fb));
    pty.answer(reply);
    REQUIRE(readable(pty.slave));  // the reply waits in the port
    CHECK(ptt->key(true));
    CHECK(pty.receive(civ(0x94, true).size()) == civ(0x94, true));
    int pending = -1;
    CHECK_EQ(::ioctl(pty.slave, FIONREAD, &pending), 0);
    CHECK_EQ(pending, 0);  // dropped before the command, without reading
}

// A radio that stops reading: the port fills and key() fails at once instead of blocking.
TEST(ptt_cat_never_blocks) {
    PtyPair pty;
    REQUIRE(pty.ok());
    PttOptions options;
    options.method = PttMethod::kenwood;
    options.device = pty.path;
    std::string error;
    std::unique_ptr<Ptt> ptt = unlimited::pc::open_ptt(options, error);
    REQUIRE(ptt != nullptr);
    const size_t most_keyings = 1000000;  // far more bytes than a PTY holds
    size_t keyings = 0;
    bool refused = false;
    for (; keyings < most_keyings && !refused; ++keyings) refused = !ptt->key(!ptt->keyed());
    NOTE("the port refused a command after %zu keyings", keyings);
    CHECK(refused);
    pty.discard();
}

TEST(ptt_rts_and_dtr_lines) {
    struct Case {
        PttMethod method;
        bool invert;
        int line;
        int other;
    };
    const Case cases[] = {{PttMethod::rts, false, TIOCM_RTS, TIOCM_DTR},
                          {PttMethod::rts, true, TIOCM_RTS, TIOCM_DTR},
                          {PttMethod::dtr, false, TIOCM_DTR, TIOCM_RTS},
                          {PttMethod::dtr, true, TIOCM_DTR, TIOCM_RTS}};
    const unsigned long raise_line = TIOCMBIS;
    const unsigned long lower_line = TIOCMBIC;
    for (size_t c = 0; c < count_of(cases); ++c) {
        PtyPair pty;
        REQUIRE(pty.ok());
        pty.hang_up_on_close();
        PttOptions options;
        options.method = cases[c].method;
        options.invert = cases[c].invert;
        options.device = pty.path;
        g_lines = LineLog();
        g_lines.lines = cases[c].other;  // the other line is up and must stay so
        std::string error;
        std::unique_ptr<Ptt> ptt = unlimited::pc::open_ptt(options, error, fake_modem_lines);
        REQUIRE(ptt != nullptr);
        NOTE("%s", ptt->description().c_str());
        const unsigned long unkey = cases[c].invert ? raise_line : lower_line;
        const unsigned long key = cases[c].invert ? lower_line : raise_line;
        REQUIRE(g_lines.requests.size() == 1);  // unkeyed at start
        CHECK_EQ(g_lines.requests[0], unkey);
        CHECK(ptt->key(true));
        CHECK(ptt->key(true));  // idempotent
        REQUIRE(g_lines.requests.size() == 2);
        CHECK_EQ(g_lines.requests[1], key);
        CHECK_EQ((g_lines.lines & cases[c].line) != 0, !cases[c].invert);  // the keyed level
        CHECK(ptt->key(false));
        REQUIRE(g_lines.requests.size() == 3);
        CHECK_EQ(g_lines.requests[2], unkey);
        CHECK(ptt->key(true));
        ptt.reset();  // destroyed while keyed: unkeys
        REQUIRE(g_lines.requests.size() == 5);
        CHECK_EQ(g_lines.requests[4], unkey);
        CHECK_EQ((g_lines.lines & cases[c].line) != 0, cases[c].invert);  // the unkeyed level
        for (size_t i = 0; i < g_lines.bits.size(); ++i) CHECK_EQ(g_lines.bits[i], cases[c].line);
        CHECK((g_lines.lines & cases[c].other) != 0);  // never touched
        // An inverted line keys when low: HUPCL is cleared so that closing the port leaves it high.
        CHECK_EQ((pty.settings().c_cflag & HUPCL) == 0, cases[c].invert);
    }
}

TEST(ptt_open_errors) {
    std::string error;
    PttOptions options;
    options.method = PttMethod::rts;
    CHECK(unlimited::pc::open_ptt(options, error) == nullptr);
    CHECK(error.find("--ptt-device") != std::string::npos);

    options.device = "/dev/no-such-serial-port";
    error.clear();
    CHECK(unlimited::pc::open_ptt(options, error) == nullptr);
    CHECK(error.find("/dev/no-such-serial-port") != std::string::npos);
    options.method = PttMethod::icom;
    error.clear();
    CHECK(unlimited::pc::open_ptt(options, error) == nullptr);
    CHECK(error.find("/dev/no-such-serial-port") != std::string::npos);

    PtyPair pty;
    REQUIRE(pty.ok());
    options.method = PttMethod::cat;  // without its commands
    options.device = pty.path;
    error.clear();
    CHECK(unlimited::pc::open_ptt(options, error) == nullptr);
    CHECK(error.find("--cat-tx-on") != std::string::npos);

    // A port without modem-control lines refuses RTS: the error names the port.
    options.method = PttMethod::rts;
    error.clear();
    CHECK(unlimited::pc::open_ptt(options, error, failing_modem_lines) == nullptr);
    NOTE("%s", error.c_str());
    CHECK(error.find("cannot set RTS on " + pty.path) != std::string::npos);
    error.clear();
    std::unique_ptr<Ptt> real = unlimited::pc::open_ptt(options, error);
    NOTE("RTS on a PTY with the real ioctl: %s", real ? "accepted" : error.c_str());
}
