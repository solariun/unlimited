#include "audio.hpp"
#include "cli.hpp"
#include "tui.hpp"
#include "unlimited/audio_io.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/packet.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

using std::int16_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using unlimited::Decoder;
using unlimited::DecoderConfig;
using unlimited::DecoderState;
using unlimited::DecisionMode;
using unlimited::Event;
using unlimited::EventType;
using unlimited::LostReason;
using unlimited::Passband;
using unlimited::SampleSink;
using unlimited::cli::Arguments;
using unlimited::cli::Console;
using unlimited::cli::UsageError;
using unlimited::cli::find_name;
using unlimited::cli::fixed;
using unlimited::cli::k_exit_io;
using unlimited::cli::k_exit_ok;
using unlimited::cli::k_exit_usage;
using unlimited::cli::k_profiles;
using unlimited::cli::k_rules;
using unlimited::cli::to_clamped;
using unlimited::cli::to_number;
using unlimited::cli::to_passband;

namespace pc = unlimited::pc;

const char* const k_program = "unlimited_decode";
const char* const k_default_in_spec = "rx.wav";
const char* const k_default_profile = "ssb";

const int k_exit_nothing = 1;  // nothing decoded, or (--expect) not an exact match

const double k_rate_hz = unlimited::k_decoder_rate_hz;
const int k_bits_per_byte = unlimited::k_bits_per_byte;
const size_t k_clock_step = 8;      // samples per decoder call: events are timed to 1 ms
const size_t k_drain_slots = 8;     // silence after the input, in the longest slots: the last package and END complete
const size_t k_offset_probe = 64;   // bytes of a late-join lock used to find its place in the expected data
const uint8_t k_first_printable = 0x20;
const uint8_t k_last_printable = 0x7E;
const double k_percent = 100.0;

const int k_seconds_decimals = 3;
const int k_hz_decimals = 1;
const int k_ms_decimals = 2;
const int k_db_decimals = 1;
const int k_rate_decimals = 1;
const int k_ber_digits = 2;

const char* const k_usage =
    "usage: unlimited_decode [--in SPEC] [--profile ssb|am|fm] [--min-slot-ms N] [--passband LO:HI]\n"
    "    [--rule adaptive|fixed] [--ratio 0.70] [--no-blanker] [--packet] [--events] [--expect FILE]\n"
    "    [--tui] [--realtime]\n"
    "\n"
    "Receives Unlimited transmissions from audio (any sample rate; resampled to 8000 Hz). It finds the pitch,\n"
    "measures the slot length T and counts the bits per package N by itself: tell it only the range of slot\n"
    "lengths to listen to and the audio passband of the radio.\n"
    "\n"
    "Receiver\n"
    "  --in SPEC               the audio: wav:<path>, <path>.wav or null (default rx.wav)\n"
    "  --profile NAME          ssb (default): slots of 8..64 ms, passband 300..2700 Hz, HF SSB (USB or LSB);\n"
    "                          am: 8..64 ms, 100..3000 Hz; fm: 4..32 ms, 300..3000 Hz, pitches from 1000 Hz\n"
    "  --min-slot-ms N         the shortest slot to hear, %u..%u ms: the receiver then hears N..%uN ms\n"
    "  --passband LO:HI        the radio's audio filter; the pitch search stays inside it\n"
    "  --rule adaptive|fixed   the decision line between a 0 and a 1: adaptive (the smart line, default)\n"
    "                          sits at 50..75 %% of the START-STOP reference line, about 70 %% on weak signals;\n"
    "                          fixed sits at --ratio of it\n"
    "  --ratio R               the fixed decision line, a fraction of the reference line (0.70; implies fixed)\n"
    "  --no-blanker            turn off the impulse (static crash) blanker\n"
    "\n"
    "Output\n"
    "  --packet                print the CRC-checked packets found in the bytes\n"
    "  --events                print every receiver event, each decided bit included\n"
    "  --expect FILE           compare with the data that was sent: bit errors, lost, wrong and extra bytes\n"
    "  --tui                   live view: each package's bars against its reference and decision lines,\n"
    "                          scope, spectrum and status\n"
    "  --realtime              pace file input to audio time\n"
    "\n"
    "On each lock it prints the pitch, T, N, the bit rate, the SNR and the received signal's band against the\n"
    "passband; when a transmission ends, its text.\n"
    "exit codes: 0 decoded (and matches --expect), 1 nothing decoded or no match, 2 usage error,\n"
    "            3 input/output error\n";

std::string hex_byte(uint8_t value) {
    char text[8];
    std::snprintf(text, sizeof(text), "0x%02X", static_cast<unsigned>(value));
    return text;
}

// Quoted, with backslash escapes for everything that is not printable ASCII.
std::string quoted(const std::vector<uint8_t>& bytes) {
    std::string text = "\"";
    for (size_t i = 0; i < bytes.size(); ++i) {
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

int bit_count(unsigned value) {
    int count = 0;
    for (; value != 0; value &= value - 1) ++count;
    return count;
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

struct Options {
    bool help = false;
    std::string in_spec = k_default_in_spec;
    std::string profile = k_default_profile;
    bool has_min_slot = false;
    double min_slot_ms = 0.0;
    bool has_passband = false;
    Passband passband = Passband();
    std::string rule;  // empty: the profile's
    bool has_ratio = false;
    double ratio = 0.0;
    bool no_blanker = false;
    bool packet = false;
    bool events = false;
    std::string expect_path;
    bool tui = false;
    bool realtime = false;
};

Options parse_options(int argc, char** argv) {
    Options o;
    Arguments args(argc, argv);
    std::string option;
    while (args.next(option)) {
        if (option == "--help" || option == "-h") {
            o.help = true;
        } else if (option == "--in") {
            o.in_spec = args.value(option);
        } else if (option == "--profile") {
            o.profile = args.value(option);
            find_name(k_profiles, option, o.profile);
        } else if (option == "--min-slot-ms") {
            o.has_min_slot = true;
            o.min_slot_ms = to_number(option, args.value(option));
        } else if (option == "--passband") {
            o.has_passband = true;
            o.passband = to_passband(option, args.value(option));
        } else if (option == "--rule") {
            o.rule = args.value(option);
            find_name(k_rules, option, o.rule);
        } else if (option == "--ratio") {
            o.has_ratio = true;
            o.ratio = to_number(option, args.value(option));
        } else if (option == "--no-blanker") {
            o.no_blanker = true;
        } else if (option == "--packet") {
            o.packet = true;
        } else if (option == "--events") {
            o.events = true;
        } else if (option == "--expect") {
            o.expect_path = args.value(option);
        } else if (option == "--tui") {
            o.tui = true;
        } else if (option == "--realtime") {
            o.realtime = true;
        } else {
            throw UsageError("unknown option '" + option + "'");
        }
    }
    return o;
}

bool changed_receiver(const Options& o) {
    return o.has_min_slot || o.has_passband || !o.rule.empty() || o.has_ratio || o.no_blanker;
}

DecoderConfig decoder_config(const Options& o) {
    DecoderConfig config = DecoderConfig::for_profile(find_name(k_profiles, "--profile", o.profile).value);
    if (o.has_min_slot) config.min_slot_ms = to_clamped<uint8_t>(o.min_slot_ms);
    if (o.has_passband) config.passband = o.passband;
    if (!o.rule.empty()) config.decision_mode = find_name(k_rules, "--rule", o.rule).value;
    if (o.has_ratio) {
        if (config.decision_mode == DecisionMode::adaptive && !o.rule.empty())
            throw UsageError("--ratio sets the fixed decision line: use it with --rule fixed");
        config.decision_mode = DecisionMode::fixed_ratio;
        config.fixed_ratio = static_cast<float>(o.ratio);
    }
    if (o.no_blanker) config.impulse_blanker = false;
    const std::string problem = unlimited::cli::decoder_problem(config);
    if (!problem.empty()) throw UsageError("refused: " + problem);
    return config;
}

// "profile ssb: slots 8-64 ms, passband 300-2700 Hz, pitch search 335-2665 Hz, smart decision line, impulse
// blanker on"
std::string receiver_text(const Options& o, const DecoderConfig& config) {
    const Passband search = config.search_range();
    return "profile " + o.profile + (changed_receiver(o) ? " (changed)" : "") + ": slots " +
           std::to_string(config.min_slot_ms) + "-" + std::to_string(config.max_slot_ms()) + " ms, passband " +
           unlimited::cli::passband_text(config.passband) + ", pitch search " +
           unlimited::cli::passband_text(search) + ", " + unlimited::cli::rule_text(config) + ", impulse blanker " +
           (config.impulse_blanker ? "on" : "off");
}

// ---------------------------------------------------------------------------
// Reception
// ---------------------------------------------------------------------------

const char* state_name(DecoderState state) {
    switch (state) {
    case DecoderState::search:
        return "search";
    case DecoderState::acquire:
        return "acquire";
    case DecoderState::preamble:
        return "preamble";
    case DecoderState::track:
        return "track";
    }
    return "?";
}

const char* reason_name(LostReason reason) {
    switch (reason) {
    case LostReason::none:
        return "none";
    case LostReason::signal_gone:
        return "signal_gone";
    case LostReason::alias:
        return "alias";
    case LostReason::preamble_timeout:
        return "preamble_timeout";
    case LostReason::reset:
        return "reset";
    case LostReason::unsupported:
        return "unsupported";
    }
    return "?";
}

struct FlagName {
    uint8_t flag;
    const char* name;
};

const FlagName k_flags[] = {{unlimited::event_flag_late_join, "late_join"},
                            {unlimited::event_flag_flywheel_start, "flywheel_start"},
                            {unlimited::event_flag_flywheel_stop, "flywheel_stop"},
                            {unlimited::event_flag_blanked, "blanked"},
                            {unlimited::event_flag_weak, "weak"}};

std::string flag_names(uint8_t flags) {
    std::string names;
    for (size_t i = 0; i < sizeof(k_flags) / sizeof(k_flags[0]); ++i)
        if (flags & k_flags[i].flag) names += std::string(names.empty() ? "" : ",") + k_flags[i].name;
    return names;
}

std::string with_flags(const std::string& text, uint8_t flags) {
    const std::string names = flag_names(flags);
    return names.empty() ? text : text + "  " + names;
}

// "pitch 1580.0 Hz, T 16.01 ms, N 8, 55.5 bit/s, SNR 10.3 dB"
std::string signal_text(const Event& event) {
    std::string text = "pitch " + fixed(event.tone_hz, k_hz_decimals) + " Hz, T " +
                       fixed(event.slot_ms, k_ms_decimals) + " ms";
    if (event.bits_per_package > 0)
        text += ", N " + std::to_string(event.bits_per_package) + " bits per package, " +
                fixed(unlimited::cli::net_bit_rate(event.bits_per_package, event.slot_ms), k_rate_decimals) + " bit/s";
    return text + ", SNR " + fixed(event.snr_db, k_db_decimals) + " dB";
}

std::string soft_list(const int8_t* soft, size_t count) {
    std::string text;
    for (size_t i = 0; i < count; ++i) text += (i == 0 ? "" : " ") + std::to_string(static_cast<int>(soft[i]));
    return text;
}

std::string describe_event(const Event& event) {
    switch (event.type) {
    case EventType::state:
        return std::string("state ") + state_name(event.state) +
               (event.state == DecoderState::search ? " (DCD off)" : " (DCD on)");
    case EventType::locked:
        return with_flags("locked  " + signal_text(event) + ", first package " + std::to_string(event.package_index),
                          event.flags);
    case EventType::slot:
        return with_flags("slot " + std::to_string(event.slot) + "  package " + std::to_string(event.package_index) +
                              "  bit " + std::to_string(event.value) + "  level " +
                              std::to_string(event.level_pct) + "%  line " + std::to_string(event.threshold_pct) +
                              "%  START " + std::to_string(event.start_pct) + "%  STOP " +
                              std::to_string(event.stop_pct) + "%  soft " + std::to_string(event.soft[0]),
                          event.flags);
    case EventType::package:
        return with_flags("package " + std::to_string(event.package_index) + "  " + std::to_string(event.value) +
                              " bits  T " + fixed(event.slot_ms, k_ms_decimals) + " ms  START " +
                              std::to_string(event.start_pct) + "%  STOP " + std::to_string(event.stop_pct) + "%",
                          event.flags);
    case EventType::byte: {
        const std::vector<uint8_t> value(1, event.value);
        return with_flags("byte " + hex_byte(event.value) + " " + quoted(value) + "  index " +
                              std::to_string(event.byte_index) + "  package " + std::to_string(event.package_index) +
                              "  soft " + soft_list(event.soft, k_bits_per_byte),
                          event.flags);
    }
    case EventType::end:
        return "end  last package " + std::to_string(event.package_index);
    case EventType::lost:
        return std::string("lost ") + reason_name(event.reason);
    }
    return "?";
}

struct Reception {  // one lock: from `locked` to end, lost or the end of the input
    bool late_join;
    std::vector<Event> bytes;
    Event last;     // locked or the latest byte: pitch, T, N and SNR
    uint8_t flags;  // every flag seen on its bytes
};

class Receiver {
public:
    Receiver(const Options& options, const DecoderConfig& config, Console& console, pc::Tui* tui)
        : options_(options),
          config_(config),
          console_(console),
          tui_(tui),
          packets_(&Receiver::on_packet, this),
          open_(false),
          samples_(0),
          packet_count_(0) {}

    static void on_event(const Event& event, void* context) { static_cast<Receiver*>(context)->handle(event); }

    void advance(size_t samples) { samples_ += samples; }
    double seconds() const { return samples_ / k_rate_hz; }

    void finish() {
        if (open_) close("the input ended");
    }

    const std::vector<Reception>& receptions() const { return receptions_; }
    size_t packet_count() const { return packet_count_; }
    const std::vector<uint8_t>& packet_payloads() const { return payloads_; }
    uint32_t crc_errors() const { return packets_.crc_errors(); }

    size_t byte_count() const {
        size_t count = 0;
        for (size_t i = 0; i < receptions_.size(); ++i) count += receptions_[i].bytes.size();
        return count;
    }

private:
    static void on_packet(const uint8_t* payload, uint16_t size, uint8_t flags, void* context) {
        Receiver* receiver = static_cast<Receiver*>(context);
        const std::vector<uint8_t> bytes(payload, payload + size);
        receiver->payloads_.insert(receiver->payloads_.end(), bytes.begin(), bytes.end());
        ++receiver->packet_count_;
        receiver->console_.item("packet",
                                with_flags(std::to_string(size) + " bytes, CRC good: " + quoted(bytes), flags));
    }

    std::string at() const { return "t " + fixed(seconds(), k_seconds_decimals) + " s"; }

    void handle(const Event& event) {
        if (options_.events) console_.line("t " + fixed(seconds(), k_seconds_decimals) + "  " + describe_event(event));
        if (tui_ != nullptr) tui_->on_event(event);
        if (options_.packet) packets_.on_event(event);
        switch (event.type) {
        case EventType::locked:
            if (open_) close("a new lock");
            open(event, (event.flags & unlimited::event_flag_late_join) != 0);
            break;
        case EventType::byte:
            if (!open_) open(event, true);
            receptions_.back().bytes.push_back(event);
            receptions_.back().last = event;
            receptions_.back().flags |= event.flags;
            break;
        case EventType::end:
            if (open_) close("end");
            break;
        case EventType::lost:
            if (open_) close(std::string("lost (") + reason_name(event.reason) + ")");
            break;
        case EventType::state:
        case EventType::slot:
        case EventType::package:
            break;
        }
    }

    void open(const Event& event, bool late_join) {
        Reception reception;
        reception.late_join = late_join;
        reception.last = event;
        reception.flags = event.flags;
        receptions_.push_back(reception);
        open_ = true;
        console_.item("locked", at() + ": " + signal_text(event) +
                                    (late_join ? "; late join: the transmission was already running" : ""));
        console_.item("bandwidth", unlimited::cli::bandwidth_line(config_, event.tone_hz, event.slot_ms));
    }

    void close(const std::string& ending) {
        open_ = false;
        const Reception& r = receptions_.back();
        std::vector<uint8_t> text;
        for (size_t i = 0; i < r.bytes.size(); ++i) text.push_back(r.bytes[i].value);
        const std::string count = std::to_string(r.bytes.size()) + (r.bytes.size() == 1 ? " byte" : " bytes");
        console_.item("rx", with_flags(count + ", " + ending + " at " + at() + " (" + signal_text(r.last) + ")",
                                       r.flags));
        console_.item("text", quoted(text));
    }

    const Options& options_;
    const DecoderConfig& config_;
    Console& console_;
    pc::Tui* tui_;
    unlimited::PacketReader packets_;
    std::vector<Reception> receptions_;
    bool open_;
    uint64_t samples_;
    size_t packet_count_;
    std::vector<uint8_t> payloads_;
};

// ---------------------------------------------------------------------------
// Comparison with the expected data (--expect)
// ---------------------------------------------------------------------------

struct Comparison {
    size_t expected = 0;
    size_t received = 0;
    size_t lost = 0;
    size_t wrong = 0;
    size_t extra = 0;
    size_t bit_errors = 0;
    size_t locks = 0;
    size_t transmissions = 0;
    double snr_db = 0.0;
    double slot_ms = 0.0;
    size_t measured = 0;               // byte events behind snr_db and slot_ms
    unsigned bits_per_package = 0;     // N of the first byte
};

int mismatch_bits(const std::vector<uint8_t>& expected, size_t position, uint8_t value) {
    return position < expected.size() ? bit_count(static_cast<unsigned>(expected[position] ^ value)) : k_bits_per_byte;
}

// A late join counts its bytes from the join (spec 3.12): its place is where its first bytes fit best, the exact
// byte_index (offset 0, a relock after a fade) first.
size_t late_join_offset(const Reception& reception, const std::vector<uint8_t>& expected) {
    const size_t probe = std::min(k_offset_probe, reception.bytes.size());
    size_t best_offset = 0;
    size_t best_errors = std::numeric_limits<size_t>::max();
    for (size_t offset = 0; offset < expected.size(); ++offset) {
        size_t errors = 0;
        for (size_t i = 0; i < probe && errors < best_errors; ++i)
            errors += static_cast<size_t>(
                mismatch_bits(expected, offset + reception.bytes[i].byte_index, reception.bytes[i].value));
        if (errors < best_errors) {
            best_errors = errors;
            best_offset = offset;
        }
    }
    return best_offset;
}

// Every lock is placed on a copy of the expected data at its bytes' byte_index: a lock from the preamble starts a
// new transmission; a late join continues the current one where it fits, or starts a new one.
Comparison compare(const std::vector<Reception>& receptions, const std::vector<uint8_t>& expected) {
    const int k_empty = -1;
    Comparison c;
    c.expected = expected.size();
    c.locks = receptions.size();
    std::vector<std::vector<int> > copies;
    for (size_t r = 0; r < receptions.size(); ++r) {
        const Reception& reception = receptions[r];
        if (reception.bytes.empty()) continue;
        const size_t offset = reception.late_join ? late_join_offset(reception, expected) : 0;
        bool fits = reception.late_join && !copies.empty();
        for (size_t i = 0; fits && i < reception.bytes.size(); ++i) {
            const size_t position = offset + reception.bytes[i].byte_index;
            fits = position >= expected.size() || copies.back()[position] == k_empty;
        }
        if (!fits) copies.push_back(std::vector<int>(expected.size(), k_empty));
        std::vector<int>& copy = copies.back();
        for (size_t i = 0; i < reception.bytes.size(); ++i) {
            const Event& event = reception.bytes[i];
            const size_t position = offset + event.byte_index;
            if (position >= expected.size() || copy[position] != k_empty) {
                ++c.extra;
            } else {
                copy[position] = event.value;
            }
            if (c.measured == 0) c.bits_per_package = event.bits_per_package;
            c.snr_db += event.snr_db;
            c.slot_ms += event.slot_ms;
            ++c.measured;
        }
    }
    c.transmissions = copies.size();
    if (copies.empty()) c.lost = expected.size();
    for (size_t t = 0; t < copies.size(); ++t) {
        for (size_t i = 0; i < expected.size(); ++i) {
            if (copies[t][i] == k_empty) {
                ++c.lost;
                continue;
            }
            ++c.received;
            const int errors = mismatch_bits(expected, i, static_cast<uint8_t>(copies[t][i]));
            c.bit_errors += static_cast<size_t>(errors);
            if (errors > 0) ++c.wrong;
        }
    }
    if (c.measured > 0) {
        c.snr_db /= c.measured;
        c.slot_ms /= c.measured;
    }
    return c;
}

// ---------------------------------------------------------------------------
// Audio path
// ---------------------------------------------------------------------------

// Feeds the decoder sink in steps of k_clock_step samples and keeps the receiver's clock, so every event is
// timed to within 1 ms (events are reported from inside the decoder's process()).
class ClockedSink final : public SampleSink {
public:
    ClockedSink(SampleSink& sink, Receiver& receiver) : sink_(sink), receiver_(receiver) {}

    void write(const int16_t* in, size_t count) override {
        for (size_t position = 0; position < count; position += k_clock_step) {
            const size_t step = std::min(k_clock_step, count - position);
            receiver_.advance(step);
            sink_.write(in + position, step);
        }
    }

private:
    SampleSink& sink_;
    Receiver& receiver_;
};

// Watches the audio as the input delivers it: feeds the TUI and paces file input to real time.
class MonitorSink final : public SampleSink {
public:
    MonitorSink(SampleSink& sink, uint32_t rate_hz, pc::Tui* tui, bool realtime)
        : sink_(sink), rate_hz_(rate_hz), tui_(tui), realtime_(realtime), pacer_(rate_hz) {}

    void write(const int16_t* in, size_t count) override {
        if (tui_ != nullptr) tui_->push_audio(in, count, rate_hz_);
        sink_.write(in, count);
        if (tui_ != nullptr && refresh_.due()) tui_->draw();
        if (realtime_) pacer_.advance(count);
    }

private:
    SampleSink& sink_;
    uint32_t rate_hz_;
    pc::Tui* tui_;
    bool realtime_;
    pc::RealtimePacer pacer_;
    pc::RefreshPacer refresh_;
};

int run(const Options& o) {
    const DecoderConfig config = decoder_config(o);

    std::vector<uint8_t> expected;
    if (!o.expect_path.empty()) {
        if (!unlimited::cli::read_file(o.expect_path, expected)) {
            std::fprintf(stderr, "%s: cannot read %s\n", k_program, o.expect_path.c_str());
            return k_exit_io;
        }
        if (expected.empty()) throw UsageError("--expect: " + o.expect_path + " is empty");
    }

    std::string error;
    std::unique_ptr<pc::InputDevice> input = pc::open_input(o.in_spec, error);
    if (input == nullptr) {
        std::fprintf(stderr, "%s: %s\n", k_program, error.c_str());
        return k_exit_io;
    }
    const uint32_t rate_hz = input->sample_rate_hz();

    Console console;
    std::unique_ptr<pc::Tui> tui;
    if (o.tui) {
        tui.reset(new pc::Tui(pc::TuiMode::decoder));
        if (tui->open()) {
            tui->set_profile(o.profile + " " + std::to_string(config.min_slot_ms) + "-" +
                             std::to_string(config.max_slot_ms()) + " ms");
            tui->set_passband(config.passband);
            tui->set_search_range(config.search_range());
            tui->set_field("rule", config.decision_mode == DecisionMode::adaptive
                                       ? std::string("smart")
                                       : "fixed " + fixed(k_percent * config.fixed_ratio, 0) + "%");
            tui->set_field("rate", std::to_string(rate_hz) + " Hz");
            console.hold(true);
        } else {
            std::fprintf(stderr, "%s: --tui needs a terminal on stdout; plain output\n", k_program);
            tui.reset();
        }
    }
    console.item("receiver", receiver_text(o, config));
    console.item("input", o.in_spec + ": " + std::to_string(rate_hz) + " Hz" +
                              (rate_hz == unlimited::k_decoder_rate_hz ? "" : ", resampled to 8000 Hz"));

    Receiver receiver(o, config, console, tui.get());
    Decoder decoder(config, &Receiver::on_event, &receiver);
    unlimited::DecoderSink decoder_sink(decoder);
    ClockedSink clocked(decoder_sink, receiver);
    pc::ResamplingSink resampling(clocked, rate_hz);
    MonitorSink monitor(resampling, rate_hz, tui.get(), o.realtime);
    const bool started = input->start(monitor, rate_hz);
    const bool delivered = started && input->wait();
    resampling.flush();
    const std::vector<int16_t> drain(
        static_cast<size_t>(k_drain_slots * config.max_slot_ms() * k_rate_hz / unlimited::cli::k_ms_per_s), 0);
    clocked.write(drain.data(), drain.size());
    receiver.finish();

    if (tui != nullptr) tui->close();
    console.hold(false);
    if (!delivered) {
        std::fprintf(stderr, "%s: reading %s failed\n", k_program, o.in_spec.c_str());
        return k_exit_io;
    }

    if (o.packet)
        std::printf("packets    %zu valid, %u CRC errors\n", receiver.packet_count(),
                    static_cast<unsigned>(receiver.crc_errors()));
    const bool decoded = o.packet ? receiver.packet_count() > 0 : receiver.byte_count() > 0;
    if (o.expect_path.empty()) {
        if (!decoded) std::fprintf(stderr, "%s: nothing decoded\n", k_program);
        return decoded ? k_exit_ok : k_exit_nothing;
    }

    std::vector<uint8_t> sent = expected;
    if (o.packet) unlimited::cli::packetize(expected, sent);
    const Comparison c = compare(receiver.receptions(), sent);
    const size_t bits = c.received * k_bits_per_byte;
    std::printf("expect     %zu bytes x %zu transmission%s: received %zu, lost %zu, wrong %zu, extra %zu, bit errors "
                "%zu/%zu (BER %.*e), locks %zu",
                c.expected, c.transmissions, c.transmissions == 1 ? "" : "s", c.received, c.lost, c.wrong, c.extra,
                c.bit_errors, bits, k_ber_digits, bits > 0 ? static_cast<double>(c.bit_errors) / bits : 0.0,
                c.locks);
    if (c.measured > 0)
        std::printf(", SNR %s dB, T %s ms, N %u", fixed(c.snr_db, k_db_decimals).c_str(),
                    fixed(c.slot_ms, k_ms_decimals).c_str(), c.bits_per_package);
    std::printf("\n");
    const bool bytes_match = c.transmissions > 0 && c.lost == 0 && c.wrong == 0 && c.extra == 0;
    const bool match = o.packet ? receiver.packet_payloads() == expected && bytes_match : bytes_match;
    std::printf("result     %s\n", match ? "match" : "mismatch");
    return match ? k_exit_ok : k_exit_nothing;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) {
            std::printf(k_usage, static_cast<unsigned>(unlimited::k_min_window_slot_ms),
                        static_cast<unsigned>(unlimited::k_max_window_slot_ms),
                        static_cast<unsigned>(unlimited::k_speed_span));
            return k_exit_ok;
        }
        return run(options);
    } catch (const UsageError& error) {
        std::fprintf(stderr, "%s: %s (see --help)\n", k_program, error.what());
        return k_exit_usage;
    }
}
