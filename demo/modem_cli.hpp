#pragma once

#include "cli.hpp"
#include "kiss_port.hpp"
#include "radio_options.hpp"
#include "tui.hpp"
#include "unlimited/modem.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

// unlimited_modem's command line (spec 12.3): the options, their rules, the help text, and the monitor's words for a
// frame (text, AX.25 addresses, hex). The program and its tests share them.
namespace unlimited {
namespace modem_cli {

const char* const k_program = "unlimited_modem";
const int k_exit_ok = 0;
const int k_exit_failure = 1;  // a device, the pseudo-terminal, the serial port or PTT failed; a configuration refused;
                               // --loopback failed
const int k_exit_usage = 2;

const unsigned k_kiss_unit_ms = 10;      // --txdelay, --txtail, --slottime: KISS's units of 10 ms
const unsigned k_max_kiss_units = 255;   // a KISS parameter is a byte
const unsigned k_max_persist = 255;
const unsigned k_max_ms = 65535;
const unsigned k_max_debug = 3;
const double k_percent = 100.0;
const double k_default_level_dbfs = -3.0;
const double k_dbfs_reference = 32767.0;  // the crest of 0 dBFS
const double k_amplitude_db = 20.0;
const char* const k_test_destination = "CQ";
const std::size_t k_max_call_length = 6;
const unsigned k_max_ssid = 15;
const std::size_t k_hex_per_line = 24;
const double k_us_per_ms = 1000.0;
const uint32_t k_us_per_ms_whole = 1000;
const long long k_ms_per_s_whole = 1000;
const int k_lead_decimals = 1;  // the lead and tail in the banner, to 0.1 ms
const uint8_t k_first_printable = 0x20;
const uint8_t k_last_printable = 0x7E;

// AX.25 (spec 12.1 does not look inside frames; the monitor and --test-tx do): addresses of 7 bytes, the characters
// shifted left by one, then the SSID byte 0bCRRSSSSE (C: command bit, RR: reserved = 11, E: the last address).
const std::size_t k_ax25_address_bytes = 7;
const std::size_t k_ax25_call_bytes = 6;
const std::size_t k_ax25_min_addresses = 2;   // destination and source
const std::size_t k_ax25_max_addresses = 10;  // and 8 digipeaters
const uint8_t k_ax25_last = 0x01;
const uint8_t k_ax25_reserved = 0x60;
const uint8_t k_ax25_command = 0x80;
const uint8_t k_ax25_ssid_mask = 0x0F;
const uint8_t k_ax25_ui = 0x03;               // unnumbered information, poll/final off
const uint8_t k_ax25_poll_final = 0x10;
const uint8_t k_ax25_no_layer_3 = 0xF0;       // PID of plain text
const uint8_t k_ax25_i_mask = 0x01;           // bit 0 clear: an I frame
const uint8_t k_ax25_su_mask = 0x03;          // 01: S frame, 11: U frame
const uint8_t k_ax25_s_frame = 0x01;

typedef std::vector<uint8_t> Bytes;

struct Options {
    Options()
        : help(false),
          radio(pc::k_radio_input | pc::k_radio_output | pc::k_radio_ptt),
          slot_us(slot_us_for_centi_speed(k_default_centi_bytes_per_second)),
          tone_hz(k_default_tone_hz),
          passband(),
          decision_mode(DecisionMode::adaptive),
          threshold_percent(k_default_threshold_percent),
          has_level(false),
          level_dbfs(k_default_level_dbfs),
          has_volume(false),
          volume(0.0),
          link(pc::k_default_kiss_link),
          has_link(false),
          serial_baud(pc::k_default_serial_baud),
          has_serial_baud(false),
          txdelay(k_default_txdelay_ms / k_kiss_unit_ms),
          has_txdelay(false),
          txtail(k_default_tail_ms / k_kiss_unit_ms),
          persist(k_default_persist),
          slottime(k_default_slot_time_ms / k_kiss_unit_ms),
          dwait_ms(k_default_dwait_ms),
          vox_lead_ms(k_default_vox_lead_ms),
          has_vox_lead(false),
          full_duplex(false),
          fade_bridge(k_default_fade_bridge),
          min_frame(k_default_min_frame_bytes),
          monitor(false),
          tui(false),
          loopback(false),
          has_loopback_snr(false),
          loopback_snr_db(0.0),
          test_ptt(false),
          has_test_tx(false),
          debug(0) {
        passband.low_hz = k_ssb_passband_low_hz;
        passband.high_hz = k_ssb_passband_high_hz;
    }

    bool help;
    pc::RadioOptions radio;
    uint32_t slot_us;
    uint16_t tone_hz;
    Passband passband;
    DecisionMode decision_mode;
    uint8_t threshold_percent;
    bool has_level;
    double level_dbfs;
    bool has_volume;
    double volume;
    std::string link;
    bool has_link;
    std::string serial;
    uint32_t serial_baud;
    bool has_serial_baud;
    unsigned txdelay;      // 10 ms units
    bool has_txdelay;
    unsigned txtail;       // 10 ms units
    unsigned persist;
    unsigned slottime;     // 10 ms units
    unsigned dwait_ms;
    unsigned vox_lead_ms;
    bool has_vox_lead;
    bool full_duplex;
    bool fade_bridge;
    unsigned min_frame;    // receptions shorter than this never reach the computer (V23); 0: off
    std::string callsign;  // upper case, with its SSID
    bool monitor;
    bool tui;
    bool loopback;
    bool has_loopback_snr;
    double loopback_snr_db;
    bool test_ptt;
    bool has_test_tx;
    std::string test_tx;
    unsigned debug;

    bool vox() const { return radio.ptt.method == pc::PttMethod::vox; }
};

inline unsigned to_unsigned(const std::string& option, const std::string& text, unsigned low, unsigned high) {
    const double value = cli::to_number(option, text);
    if (value != static_cast<double>(static_cast<long long>(value)) || value < low || value > high)
        throw cli::UsageError(option + ": '" + text + "' is not a whole number of " + std::to_string(low) + ".." +
                              std::to_string(high));
    return static_cast<unsigned>(value);
}

inline bool is_number(const std::string& text) {
    if (text.empty()) return false;
    char* end = nullptr;
    std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size();
}

// A callsign of 1..6 letters and digits and an optional SSID 0..15 (N0CALL, PU2UIT-7); upper case.
inline std::string to_callsign(const std::string& option, const std::string& text) {
    const std::size_t dash = text.find('-');
    const std::string call = text.substr(0, dash);
    bool ok = !call.empty() && call.size() <= k_max_call_length;
    for (std::size_t i = 0; i < call.size(); ++i) ok = ok && std::isalnum(static_cast<unsigned char>(call[i])) != 0;
    if (ok && dash != std::string::npos) {
        const std::string ssid = text.substr(dash + 1);
        ok = !ssid.empty() && ssid.size() <= 2 && ssid.find_first_not_of("0123456789") == std::string::npos &&
             std::stoul(ssid) <= k_max_ssid;
    }
    if (!ok)
        throw cli::UsageError(option + ": '" + text + "' is not a callsign (1 to 6 letters and digits, an optional "
                                                      "-SSID of 0..15: N0CALL, PU2UIT-7)");
    std::string upper = text;
    for (std::size_t i = 0; i < upper.size(); ++i)
        upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(upper[i])));
    return upper;
}

// The command line; throws cli::UsageError for a usage error (exit 2). The configuration itself (does the signal fit
// the passband, is the threshold in range) is judged afterwards by config_problem().
inline Options parse_options(int argc, char** argv) {
    Options o;
    std::string error;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        const bool has_next = i + 1 < argc;
        const auto value = [&]() -> std::string {
            if (!has_next) throw cli::UsageError(option + " needs a value");
            return argv[++i];
        };
        bool radio_value = false;
        if (o.radio.takes(option, radio_value)) {
            if (!o.radio.apply(option, radio_value ? value() : std::string(), error)) throw cli::UsageError(error);
        } else if (option == "-h" || option == "--help") {
            o.help = true;
        } else if (option == "--bps") {
            o.slot_us = cli::to_slot_us(option, value());
        } else if (option == "--tone") {
            o.tone_hz = cli::to_clamped<uint16_t>(cli::to_number(option, value()));
        } else if (option == "--passband") {
            o.passband = cli::to_passband(option, value());
        } else if (option == "--threshold") {
            DecoderConfig receiver;
            cli::to_threshold(option, value(), receiver);
            o.decision_mode = receiver.decision_mode;
            o.threshold_percent = receiver.threshold_percent;
        } else if (option == "--level-dbfs") {
            o.has_level = true;
            o.level_dbfs = cli::to_number(option, value());
            if (o.level_dbfs > 0.0) throw cli::UsageError("--level-dbfs must be 0 or below (0 dBFS is full scale)");
        } else if (option == "--volume") {
            o.has_volume = true;
            o.volume = cli::to_number(option, value());
            if (!(o.volume >= 1.0 && o.volume <= k_percent))
                throw cli::UsageError("--volume: a percentage of full scale, 1..100");
        } else if (option == "--link") {
            o.has_link = true;
            o.link = value();
        } else if (option == "--serial") {
            o.serial = value();
        } else if (option == "--serial-baud") {
            o.has_serial_baud = true;
            o.serial_baud = to_unsigned(option, value(), 1, 0xFFFFFFFFu);
            if (!pc::serial_baud_supported(o.serial_baud)) {
                std::string speeds;
                for (std::size_t b = 0; b < sizeof(pc::k_serial_bauds) / sizeof(pc::k_serial_bauds[0]); ++b)
                    speeds += (b == 0 ? "" : ", ") + std::to_string(pc::k_serial_bauds[b]);
                throw cli::UsageError("--serial-baud: " + std::to_string(o.serial_baud) + " is not one of " + speeds);
            }
        } else if (option == "--txdelay") {
            o.has_txdelay = true;
            o.txdelay = to_unsigned(option, value(), 0, k_max_kiss_units);
        } else if (option == "--txtail") {
            o.txtail = to_unsigned(option, value(), 0, k_max_kiss_units);
        } else if (option == "--persist") {
            o.persist = to_unsigned(option, value(), 0, k_max_persist);
        } else if (option == "--slottime") {
            o.slottime = to_unsigned(option, value(), 1, k_max_kiss_units);
        } else if (option == "--dwait") {
            o.dwait_ms = to_unsigned(option, value(), 0, k_max_ms);
        } else if (option == "--vox-lead-ms") {
            o.has_vox_lead = true;
            o.vox_lead_ms = to_unsigned(option, value(), 0, k_max_ms);
        } else if (option == "--full-duplex") {
            o.full_duplex = true;
        } else if (option == "--fade-bridge") {
            o.fade_bridge = true;
        } else if (option == "--no-fade-bridge") {
            o.fade_bridge = false;
        } else if (option == "--min-frame") {
            o.min_frame = to_unsigned(option, value(), 0, k_max_min_frame_bytes);
        } else if (option == "-c") {
            o.callsign = to_callsign(option, value());
        } else if (option == "--monitor") {
            o.monitor = true;
        } else if (option == "--tui") {
            o.tui = true;
        } else if (option == "--loopback") {
            o.loopback = true;
            if (has_next && is_number(argv[i + 1])) {
                o.has_loopback_snr = true;
                o.loopback_snr_db = cli::to_number(option, argv[++i]);
            }
        } else if (option == "--test-ptt") {
            o.test_ptt = true;
        } else if (option == "--test-tx") {
            o.has_test_tx = true;
            o.test_tx = value();
            if (o.test_tx.empty()) throw cli::UsageError("--test-tx needs some text to send");
        } else if (option == "--debug") {
            o.debug = to_unsigned(option, value(), 0, k_max_debug);
        } else {
            throw cli::UsageError("unknown option '" + option + "'");
        }
    }
    if (o.help) return o;
    if (!o.radio.check(error)) throw cli::UsageError(error);
    const int modes = (o.radio.list_devices ? 1 : 0) + (o.loopback ? 1 : 0) + (o.test_ptt ? 1 : 0) +
                      (o.has_test_tx ? 1 : 0);
    if (modes > 1) throw cli::UsageError("--list-devices, --loopback, --test-ptt and --test-tx are one at a time");
    if (o.has_level && o.has_volume) throw cli::UsageError("give --level-dbfs or --volume, not both");
    if (!o.serial.empty() && o.has_link)
        throw cli::UsageError("--serial and --link are two ways to reach the computer: give one");
    if (o.has_serial_baud && o.serial.empty()) throw cli::UsageError("--serial-baud needs --serial DEV");
    if (o.has_txdelay && o.vox())
        throw cli::UsageError("--txdelay applies to --ptt rts, dtr and the CAT methods; with --ptt vox the lead tone "
                              "keys the radio: --vox-lead-ms (default " + std::to_string(k_default_vox_lead_ms) + ")");
    if (o.has_vox_lead && !o.vox())
        throw cli::UsageError("--vox-lead-ms applies to --ptt vox; with --ptt " +
                              std::string(pc::ptt_method_name(o.radio.ptt.method)) + " the TX delay is --txdelay");
    if (o.tui && modes > 0) throw cli::UsageError("--tui shows the running modem: not with --list-devices, --loopback, "
                                                  "--test-ptt or --test-tx");
    if (o.tui && o.radio.input == "null")
        throw cli::UsageError("--tui shows what the radio hears: it needs an input device, not null");
    return o;
}

inline int16_t amplitude(const Options& o) {
    const double crest = o.has_volume ? k_dbfs_reference * o.volume / k_percent
                                      : k_dbfs_reference * std::pow(10.0, o.level_dbfs / k_amplitude_db);
    return cli::to_clamped<int16_t>(crest);
}

// The modem the options describe. With --ptt vox the VOX lead replaces the TX delay (spec 2.1, 12.1); the output
// latency is the caller's (the device's, known once it is open).
inline ModemConfig modem_config(const Options& o, uint16_t output_latency_ms = 0) {
    ModemConfig config;
    config.signal.slot_us = o.slot_us;
    config.signal.tone_hz = o.tone_hz;
    config.signal.passband = o.passband;
    config.signal.amplitude = amplitude(o);
    config.signal.lead_in_ms = o.vox() ? 0 : static_cast<uint16_t>(o.txdelay * k_kiss_unit_ms);
    config.signal.vox_lead_ms = o.vox() ? static_cast<uint16_t>(o.vox_lead_ms) : 0;
    config.signal.tail_ms = static_cast<uint16_t>(o.txtail * k_kiss_unit_ms);
    config.receiver.slot_us = o.slot_us;
    config.receiver.passband = o.passband;
    config.receiver.decision_mode = o.decision_mode;
    config.receiver.threshold_percent = o.threshold_percent;
    config.access.dwait_ms = static_cast<uint16_t>(o.dwait_ms);
    config.access.persist = static_cast<uint8_t>(o.persist);
    config.access.slot_time_ms = static_cast<uint16_t>(o.slottime * k_kiss_unit_ms);
    config.access.output_latency_ms = output_latency_ms;
    config.access.full_duplex = o.full_duplex;
    config.fade_bridge = o.fade_bridge;
    config.min_frame_bytes = static_cast<uint8_t>(o.min_frame);
    return config;
}

// Why the configuration is refused (exit 1), in plain words with the option to change; empty when it is not.
inline std::string config_problem(const ModemConfig& config) {
    const std::string sent = cli::encoder_problem(config.sent());
    if (!sent.empty()) return sent;
    const std::string heard = cli::decoder_problem(config.heard());
    if (!heard.empty()) return heard;
    return config.valid() ? std::string() : std::string("the modem refuses this combination");
}

// ---------------------------------------------------------------------------
// Words for the banner and the monitor
// ---------------------------------------------------------------------------

inline std::string seconds_text(uint32_t samples) {
    return cli::trimmed(static_cast<double>(samples) / k_modem_rate_hz, 2) + " s";
}

// "1 byte 0.87 s, 64 bytes 11.1 s, 144 bytes 24.4 s": transmissions from the key to the release, lead and tail in.
inline std::string airtime_text(const EncoderConfig& signal) {
    const std::size_t sizes[] = {1, 64, 144};
    const Encoder encoder(signal);
    std::string text;
    for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        text += (i == 0 ? "" : ", ") + std::to_string(sizes[i]) + (sizes[i] == 1 ? " byte " : " bytes ") +
                seconds_text(encoder.duration_samples(sizes[i]));
    return text;
}

inline std::string clock_text() {
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % k_ms_per_s_whole;
    std::tm local;
    localtime_r(&seconds, &local);
    char text[32];
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03lld", local.tm_hour, local.tm_min, local.tm_sec, ms);
    return text;
}

inline bool printable(const Bytes& bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const uint8_t c = bytes[i];
        if ((c < k_first_printable || c > k_last_printable) && c != '\n' && c != '\r' && c != '\t') return false;
    }
    return true;
}

// Quoted, with backslash escapes for what is not printable ASCII.
inline std::string quoted(const Bytes& bytes, std::size_t from = 0) {
    std::string text = "\"";
    for (std::size_t i = from; i < bytes.size(); ++i) {
        const uint8_t c = bytes[i];
        if (c == '"' || c == '\\') {
            text += '\\';
            text += static_cast<char>(c);
        } else if (c == '\n') {
            text += "\\n";
        } else if (c == '\r') {
            text += "\\r";
        } else if (c == '\t') {
            text += "\\t";
        } else if (c >= k_first_printable && c <= k_last_printable) {
            text += static_cast<char>(c);
        } else {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\x%02X", static_cast<unsigned>(c));
            text += escape;
        }
    }
    return text + "\"";
}

inline std::vector<std::string> hex_lines(const Bytes& bytes) {
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < bytes.size(); i += k_hex_per_line) {
        std::string line = "hex";
        for (std::size_t j = i; j < bytes.size() && j < i + k_hex_per_line; ++j) {
            char byte[4];
            std::snprintf(byte, sizeof(byte), " %02X", static_cast<unsigned>(bytes[j]));
            line += byte;
        }
        lines.push_back(line);
    }
    return lines;
}

// One AX.25 address at `at`: "N0CALL" or "N0CALL-7"; false when the bytes are not a callsign.
inline bool ax25_address(const Bytes& frame, std::size_t at, std::string& call, bool& last) {
    call.clear();
    bool ended = false;
    for (std::size_t i = 0; i < k_ax25_call_bytes; ++i) {
        const uint8_t byte = frame[at + i];
        const char c = static_cast<char>(byte >> 1);
        if ((byte & k_ax25_last) != 0) return false;
        if (c == ' ') {
            ended = true;
            continue;
        }
        if (ended || !(std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c))))
            return false;
        call += c;
    }
    if (call.empty()) return false;
    const uint8_t ssid_byte = frame[at + k_ax25_call_bytes];
    const unsigned ssid = (ssid_byte >> 1) & k_ax25_ssid_mask;
    if (ssid != 0) call += "-" + std::to_string(ssid);
    last = (ssid_byte & k_ax25_last) != 0;
    return true;
}

// "AX.25 N0CALL>CQ,RELAY [UI pid F0] "text"" when the frame reads as AX.25; empty otherwise.
inline std::string ax25_text(const Bytes& frame) {
    std::vector<std::string> calls;
    std::size_t at = 0;
    bool last = false;
    while (!last) {
        if (calls.size() == k_ax25_max_addresses || at + k_ax25_address_bytes > frame.size()) return std::string();
        std::string call;
        if (!ax25_address(frame, at, call, last)) return std::string();
        calls.push_back(call);
        at += k_ax25_address_bytes;
    }
    if (calls.size() < k_ax25_min_addresses || at >= frame.size()) return std::string();
    std::string text = "AX.25 " + calls[1] + ">" + calls[0];
    for (std::size_t i = k_ax25_min_addresses; i < calls.size(); ++i) text += "," + calls[i];
    const uint8_t control = frame[at++];
    bool has_pid = false;
    if ((control & k_ax25_i_mask) == 0) {
        text += " [I";
        has_pid = true;
    } else if ((control & k_ax25_su_mask) == k_ax25_s_frame) {
        text += " [S";
    } else if ((control & ~k_ax25_poll_final) == k_ax25_ui) {
        text += " [UI";
        has_pid = true;
    } else {
        text += " [U";
    }
    if (has_pid && at < frame.size()) {
        char pid[16];
        std::snprintf(pid, sizeof(pid), " pid %02X", static_cast<unsigned>(frame[at++]));
        text += pid;
    }
    text += "]";
    if (at < frame.size()) text += " " + quoted(frame, at);
    return text;
}

// The monitor's lines for a frame: its text (AX.25 addresses when it reads as AX.25), then hex unless it is plain text.
inline std::vector<std::string> frame_lines(const Bytes& frame) {
    std::vector<std::string> lines;
    const std::string ax25 = ax25_text(frame);
    lines.push_back(!ax25.empty() ? ax25 : quoted(frame));
    if (!ax25.empty() || !printable(frame)) {
        const std::vector<std::string> hex = hex_lines(frame);
        lines.insert(lines.end(), hex.begin(), hex.end());
    }
    return lines;
}

inline void add_address(Bytes& frame, const std::string& callsign, bool command, bool last) {
    const std::size_t dash = callsign.find('-');
    const std::string call = callsign.substr(0, dash);
    for (std::size_t i = 0; i < k_ax25_call_bytes; ++i)
        frame.push_back(static_cast<uint8_t>((i < call.size() ? call[i] : ' ') << 1));
    const unsigned ssid = dash == std::string::npos ? 0 : static_cast<unsigned>(std::stoul(callsign.substr(dash + 1)));
    frame.push_back(static_cast<uint8_t>((command ? k_ax25_command : 0) | k_ax25_reserved | (ssid << 1) |
                                         (last ? k_ax25_last : 0)));
}

// An AX.25 UI frame CALL>CQ with the text (--test-tx with -c, as kiss_modem sends one).
inline Bytes ui_frame(const std::string& callsign, const std::string& text) {
    Bytes frame;
    add_address(frame, k_test_destination, true, false);
    add_address(frame, callsign, false, true);
    frame.push_back(k_ax25_ui);
    frame.push_back(k_ax25_no_layer_3);
    frame.insert(frame.end(), text.begin(), text.end());
    return frame;
}

inline Bytes kiss_frame(const Bytes& data) {
    Bytes out(1, k_kiss_fend);
    out.push_back(k_kiss_data);
    for (std::size_t i = 0; i < data.size(); ++i) {
        uint8_t escaped[k_kiss_escaped_max];
        const uint8_t size = kiss_escape(data[i], escaped);
        out.insert(out.end(), escaped, escaped + size);
    }
    out.push_back(k_kiss_fend);
    return out;
}

// The modem's items on the --tui status line (spec 12.6), from snapshots any thread may take: the channel as its check
// sees it (DCD, V19: a transmission is being decoded), PTT and its method, what is on the air ("sending 12 of 40
// bytes"), the frames waiting, and the frames each way (and the receptions dropped as shorter than --min-frame, V23).
inline std::vector<std::pair<std::string, std::string> > modem_fields(const ModemCounters& c, bool dcd, bool keyed,
                                                                      ChannelState state, const std::string& method) {
    std::string sending;
    switch (state) {
        case ChannelState::idle:
            sending = "idle";
            break;
        case ChannelState::waiting:
            sending = "waiting for the channel";
            break;
        case ChannelState::keyed:
            sending = "sending " + std::to_string(c.on_air_sent) +
                      (c.on_air_size != 0 ? " of " + std::to_string(c.on_air_size) : std::string()) + " bytes";
            break;
        case ChannelState::releasing:
            sending = "sent, PTT releasing";
            break;
    }
    // The frame on the air stays in the send queue's count until its audio ends; the queue shows what waits behind it.
    const uint32_t on_air = state == ChannelState::keyed && c.on_air_size != 0 && c.queued_frames > 0 ? 1u : 0u;
    const uint32_t waiting = c.queued_frames - on_air;
    std::vector<std::pair<std::string, std::string> > fields;
    fields.push_back(std::make_pair(std::string("channel"), std::string(dcd ? "busy (DCD on)" : "clear (DCD off)")));
    fields.push_back(std::make_pair(std::string("PTT"), std::string(keyed ? "on" : "off") + " (" + method + ")"));
    fields.push_back(std::make_pair(std::string("TX"), sending));
    fields.push_back(std::make_pair(std::string("queue"), std::to_string(waiting) + (waiting == 1 ? " frame waiting" :
                                                                                                     " frames waiting")));
    std::string frames = "tx " + std::to_string(c.transmissions) + ", rx " + std::to_string(c.frames_received);
    if (c.short_frames > 0) frames += ", " + std::to_string(c.short_frames) + " short dropped";
    fields.push_back(std::make_pair(std::string("frames"), frames));
    return fields;
}

// The --tui view's modem items, from the modem's snapshots (the receiving side sets them before each frame), right
// after the receiver's state (whose DCD is the modem's, spec 3.10): they stay on screen in a small terminal.
inline void show_modem(pc::Tui& tui, const Modem& modem, const std::string& method) {
    const std::vector<std::pair<std::string, std::string> > fields =
        modem_fields(modem.counters(), modem.dcd(), modem.transmitting(), modem.channel_state(), method);
    for (std::size_t i = 0; i < fields.size(); ++i) tui.set_front_field(fields[i].first, fields[i].second);
    tui.set_dcd(modem.dcd());
}

// "15 bytes (--min-frame 15): shorter receptions never reach the computer; its first byte waits 14 windows (2.33 s)",
// or "off (--min-frame 0): every reception from its first byte" (V23).
inline std::string min_frame_text(const Options& o) {
    if (o.min_frame == 0) return "off (--min-frame 0): every reception goes to the computer from its first byte";
    const unsigned waits = o.min_frame - 1u;
    const double wait_s = waits * k_window_slots * (o.slot_us / k_us_per_ms) / static_cast<double>(k_ms_per_s_whole);
    return std::to_string(o.min_frame) + " bytes (--min-frame " + std::to_string(o.min_frame) +
           "): shorter receptions never reach the computer; the first byte waits " + std::to_string(waits) +
           (waits == 1 ? " window (" : " windows (") + cli::trimmed(wait_s, 2) + " s)";
}

// "10 = 100 ms": a value in KISS's 10 ms units and in ms.
inline std::string units_text(unsigned ms) {
    return std::to_string(ms / k_kiss_unit_ms) + " = " + std::to_string(ms) + " ms";
}

// "TX delay 100 ms (--txdelay 10)", "TX delay 300 ms (--txdelay 10, raised to 300 ms by --fade-bridge)", or the VOX
// lead "lead tone 150 ms (--vox-lead-ms 150), then 2 silent slots (33 ms)": what comes before the first START.
inline std::string lead_text(const Options& o, const EncoderConfig& sent) {
    if (o.vox()) {
        if (sent.vox_lead_ms == 0) return "no lead tone (--vox-lead-ms 0)";
        const uint32_t lead_slots = std::max<uint32_t>((sent.vox_lead_ms * k_us_per_ms_whole + sent.slot_us - 1) / sent.slot_us,
                                                       k_min_vox_lead_slots);
        return "lead tone " + cli::trimmed(lead_slots * sent.slot_us / k_us_per_ms, k_lead_decimals) +
               " ms (--vox-lead-ms " + std::to_string(o.vox_lead_ms) + "), then " + std::to_string(k_vox_gap_slots) +
               " silent slots (" + cli::trimmed(k_vox_gap_slots * sent.slot_us / k_us_per_ms, k_lead_decimals) +
               " ms)";
    }
    const unsigned asked_ms = o.txdelay * k_kiss_unit_ms;
    std::string text = "TX delay " + std::to_string(sent.lead_in_ms) + " ms (--txdelay " + std::to_string(o.txdelay);
    if (sent.lead_in_ms != asked_ms) text += ", raised to " + std::to_string(sent.lead_in_ms) + " ms by --fade-bridge";
    return text + ")";
}

// "TX tail 100 ms (--txtail 10)": the silence after the last STOP, at least 2 slots.
inline std::string tail_text(const Options& o, const EncoderConfig& sent) {
    const double minimum_ms = k_min_tail_slots * sent.slot_us / k_us_per_ms;
    const double tail_ms = std::max<double>(sent.tail_ms, minimum_ms);
    std::string text = "TX tail " + cli::trimmed(tail_ms, k_lead_decimals) + " ms (--txtail " + std::to_string(o.txtail);
    if (tail_ms > sent.tail_ms) text += ", 2 slots at least";
    return text + ")";
}

inline std::string help_text(const Options& o) {
    std::string text =
        "usage: unlimited_modem [options]\n"
        "\n"
        "A KISS modem for radio audio. AX.25 programs (any KISS program) open its pseudo-terminal; every KISS frame\n"
        "they send goes on the air as one Unlimited transmission, and every transmission heard comes back to them\n"
        "byte by byte as it is decoded. Both stations must use the same speed (--bps); the receiver finds the pitch\n"
        "by itself, so SSB mistuning does not matter.\n"
        "\n"
        "The signal (both stations must use the same --bps)\n"
        "  --bps B            the speed in bytes per second, 1.00..25.00 in steps of 0.01 (default 6.00, for HF SSB)\n"
        "  --tone HZ          the pitch this station sends, 300..2700 Hz (default 1500)\n"
        "  --passband LO:HI   the radios' audio passband: the signal must fit it, the pitch search stays inside it\n"
        "                     (default 300:2700, a 2.4 kHz SSB filter)\n"
        "  --level-dbfs DB    loudness of a beep's crest (default -3); or --volume N, a percentage of full scale\n"
        "  --threshold PCT|auto  the receiver's decision line: auto, the adaptive line (default), or PCT % of the\n"
        "                     reference the START and STOP tones give, 50..90\n"
        "  --fade-bridge      HF fades: a transmission ends only after 2 silent windows, and this station leaves\n"
        "                     300 ms of silence before each START (a TX delay of at least 300 ms, or the VOX lead);\n"
        "                     both stations must agree (default " +
        std::string(k_default_fade_bridge ? "on" : "off") +
        "; --no-fade-bridge turns it off)\n"
        "\n"
        "The computer (KISS)\n"
        "  --link PATH        the pseudo-terminal's symbolic link KISS programs open (default " +
        std::string(pc::k_default_kiss_link) +
        ")\n"
        "  --serial DEV       KISS on a serial port instead; --serial-baud N its speed, 8N1 (default 115200)\n"
        "  --min-frame N      hand a reception to the computer only once it is N bytes long, 0..64 (default " +
        std::to_string(k_default_min_frame_bytes) +
        ",\n"
        "                     the shortest AX.25 frame): speech, CW or noise makes a byte or two readable now and\n"
        "                     then, and those never reach the computer; the first byte of a frame then waits N-1\n"
        "                     windows (2.3 s at 6 bytes/s with 15); 0 passes every reception from its first byte\n"
        "\n" +
        o.radio.help() +
        "\n"
        "Channel access and timing (10 ms units, as kiss_modem; KISS parameter frames are ignored)\n"
        "  --txdelay N        rts, dtr, CAT: silence after keying and before the first byte, for the radio to\n"
        "                     switch to transmit (default " +
        units_text(k_default_txdelay_ms) +
        "; 300 ms at least with --fade-bridge)\n"
        "  --vox-lead-ms MS   vox: the lead tone that keys the radio, then 2 silent slots (default " +
        std::to_string(k_default_vox_lead_ms) +
        " ms)\n"
        "  --txtail N         silence after the last byte before PTT is released (default " +
        units_text(k_default_tail_ms) +
        ")\n"
        "  --persist N        p-persistence: transmit when a random 0..255 is <= N (default " +
        std::to_string(k_default_persist) +
        ")\n"
        "  --slottime N       time between p-persistence draws (default " +
        units_text(k_default_slot_time_ms) +
        ")\n"
        "  --dwait MS         after the channel goes quiet (DCD off), wait this long before contending (default " +
        std::to_string(k_default_dwait_ms) +
        ")\n"
        "  --full-duplex      transmit at once, with no channel check, and hear while sending\n"
        "\n"
        "Display\n"
        "  -c CALL            this station's callsign: --test-tx sends an AX.25 UI frame CALL>CQ\n"
        "  --monitor          each frame sent and received: time, direction, length, text or hex; received frames\n"
        "                     with the speed measured, the pitch and the SNR; sent ones with the PTT\n"
        "  --debug N          on stderr: 1 PTT, transmissions, receptions, DCD; 2 + the computer's bytes, the send\n"
        "                     queue, the p-persistence draws; 3 + every receiver event\n"
        "  --tui              the live view: what the radio hears (each window's bars, scope, spectrum, the input\n"
        "                     level) and the modem (DCD, PTT, sending n of m bytes, the send queue, frames each way);\n"
        "                     the monitor and debug lines wait until it closes\n"
        "\n"
        "Tests\n"
        "  --loopback [SNR]   two modems in memory, through the channel simulator at SNR dB (key-down, in 2500 Hz;\n"
        "                     clean without it): PASS when every frame comes back byte for byte, exit 1 otherwise\n"
        "  --test-ptt         key and release PTT three times (1 s on, 1 s off) and exit\n"
        "  --test-tx TEXT     send TEXT as one transmission, with no channel check, and exit (with -c CALL: an\n"
        "                     AX.25 UI frame CALL>CQ)\n"
        "  -h, --help         this text\n"
        "\n"
        "Exit codes: 0 stopped cleanly (Ctrl-C, SIGTERM, SIGHUP); 1 a device, the pseudo-terminal, the serial port or\n"
        "PTT failed, the configuration was refused, or --loopback failed; 2 a usage error.\n";
    return text;
}

}  // namespace modem_cli
}  // namespace unlimited
