#include "audio.hpp"
#include "cli.hpp"
#include "tui.hpp"
#include "unlimited/audio_io.hpp"
#include "unlimited/decoder.hpp"

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
using std::int64_t;
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
using unlimited::cli::fixed;
using unlimited::cli::k_exit_io;
using unlimited::cli::k_exit_ok;
using unlimited::cli::k_exit_usage;
using unlimited::cli::to_number;
using unlimited::cli::to_passband;

namespace pc = unlimited::pc;

const char* const k_program = "unlimited_decode";
const char* const k_default_in_spec = "rx.wav";

const int k_exit_nothing = 1;  // nothing decoded, or (--expect) not an exact match

const double k_rate_hz = unlimited::k_decoder_rate_hz;
const int k_bits_per_byte = unlimited::k_bits_per_byte;
const size_t k_clock_step = 8;      // samples per decoder call: events are timed to 1 ms
const size_t k_drain_windows = 2;   // silence after the input, besides the look-ahead: the last window and the end
// A file is taken to begin between transmissions: the decoder hears this much silence before it (a whole window and a
// margin), since it never takes what came before its first sample for silence (spec V6).
const double k_lead_slots = 15.0;
const uint8_t k_first_printable = 0x20;
const uint8_t k_last_printable = 0x7E;

const int k_seconds_decimals = 3;
const int k_hz_decimals = 1;
const int k_ms_decimals = 2;
const int k_db_decimals = 1;
const int k_ber_digits = 2;

const char* const k_usage =
    "usage: unlimited_decode [--in SPEC] [--bps B] [--passband LO:HI] [--threshold PCT|auto] [--no-blanker]\n"
    "    [--events] [--expect FILE] [--tui] [--realtime]\n"
    "\n"
    "Receives Unlimited transmissions from audio (any sample rate; resampled to 8000 Hz). Every byte is a window of\n"
    "10 slots: a START tone, 8 bits (a beep is a 1, silence is a 0) and a STOP tone. Give it the sender's speed;\n"
    "it finds the pitch by itself, and each byte comes out as soon as its STOP is heard.\n"
    "\n"
    "Receiver\n"
    "  --in SPEC               the audio: wav:<path>, <path>.wav or null (default rx.wav)\n"
    "  --bps B                 the sender's speed in bytes per second, %.2f..%.2f (default %.2f)\n"
    "  --passband LO:HI        the radio's audio filter; the pitch search stays inside it (default 300:2700)\n"
    "  --threshold PCT|auto    the decision line between a 0 and a 1, in %% of the reference line the START and\n"
    "                          STOP tones give (default %u, from %u to %u); auto: the adaptive line, 50..75 %%\n"
    "                          of the reference, about 70 %% on weak signals\n"
    "  --no-blanker            turn off the impulse (static crash) blanker\n"
    "\n"
    "Output\n"
    "  --events                print every receiver event, each decided bit included\n"
    "  --expect FILE           compare with the data that was sent: bit errors, lost, wrong and extra bytes\n"
    "  --tui                   live view: each window's bars against its reference and decision lines,\n"
    "                          scope, spectrum and status\n"
    "  --realtime              pace file input to audio time\n"
    "\n"
    "It prints the speed first; on each lock the pitch, T, the SNR and the received signal's band against the\n"
    "passband; when a transmission ends, its text. A file is taken to begin between transmissions.\n"
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
    DecoderConfig config;
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
        } else if (option == "--bps") {
            o.config.slot_us = unlimited::cli::to_slot_us(option, args.value(option));
        } else if (option == "--passband") {
            o.config.passband = to_passband(option, args.value(option));
        } else if (option == "--threshold") {
            unlimited::cli::to_threshold(option, args.value(option), o.config);
        } else if (option == "--no-blanker") {
            o.config.impulse_blanker = false;
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

// "passband 300-2700 Hz, pitch search 432-2568 Hz, decision line at 70 % of the reference, impulse blanker on"
std::string receiver_text(const DecoderConfig& config) {
    return "passband " + unlimited::cli::passband_text(config.passband) + ", pitch search " +
           unlimited::cli::passband_text(config.search_range()) + ", " + unlimited::cli::threshold_text(config) +
           ", impulse blanker " + (config.impulse_blanker ? "on" : "off");
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
    case DecoderState::track:
        return "track";
    }
    return "?";
}

const char* reason_name(LostReason reason) {
    switch (reason) {
    case LostReason::none:
        return "none";
    case LostReason::framing:
        return "framing";
    case LostReason::reset:
        return "reset";
    }
    return "?";
}

struct FlagName {
    uint8_t flag;
    const char* name;
};

const FlagName k_flags[] = {{unlimited::event_flag_framing, "framing"},
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

// "pitch 1580.0 Hz, T 16.67 ms, SNR 10.3 dB"
std::string signal_text(const Event& event) {
    return "pitch " + fixed(event.tone_hz, k_hz_decimals) + " Hz, T " + fixed(event.slot_ms, k_ms_decimals) +
           " ms, SNR " + fixed(event.snr_db, k_db_decimals) + " dB";
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
        return "locked  " + signal_text(event);
    case EventType::slot:
        return with_flags("slot " + std::to_string(event.slot) + "  window " + std::to_string(event.byte_index) +
                              "  bit " + std::to_string(event.value) + "  level " +
                              std::to_string(event.level_pct) + "%  line " + std::to_string(event.threshold_pct) +
                              "%  START " + std::to_string(event.start_pct) + "%  STOP " +
                              std::to_string(event.stop_pct) + "%  soft " + std::to_string(event.soft[0]),
                          event.flags);
    case EventType::byte: {
        const std::vector<uint8_t> value(1, event.value);
        return with_flags("byte " + hex_byte(event.value) + " " + quoted(value) + "  index " +
                              std::to_string(event.byte_index) + "  soft " + soft_list(event.soft, k_bits_per_byte),
                          event.flags);
    }
    case EventType::end:
        return "end";
    case EventType::lost:
        return std::string("lost ") + reason_name(event.reason);
    }
    return "?";
}

struct Reception {  // one lock: from `locked` to end, lost or the end of the input
    std::vector<Event> bytes;
    Event last;         // locked or the latest byte: pitch, T and SNR
    uint8_t flags;      // every flag seen on its bytes
    uint32_t dropped;   // windows dropped as framing errors
};

class Receiver {
public:
    Receiver(const Options& options, Console& console, pc::Tui* tui)
        : options_(options), console_(console), tui_(tui), open_(false), samples_(0), dropped_(0) {}

    // The clock of the silence heard before the input: event times count from the input's first sample.
    void lead(size_t samples) { samples_ = -static_cast<int64_t>(samples); }

    static void on_event(const Event& event, void* context) { static_cast<Receiver*>(context)->handle(event); }

    void advance(size_t samples) { samples_ += static_cast<int64_t>(samples); }
    double seconds() const { return static_cast<double>(samples_) / k_rate_hz; }

    void finish() {
        if (open_) close("the input ended");
    }

    const std::vector<Reception>& receptions() const { return receptions_; }
    uint32_t dropped() const { return dropped_; }

    size_t byte_count() const {
        size_t count = 0;
        for (size_t i = 0; i < receptions_.size(); ++i) count += receptions_[i].bytes.size();
        return count;
    }

private:
    std::string at() const { return "t " + fixed(seconds(), k_seconds_decimals) + " s"; }

    void handle(const Event& event) {
        if (options_.events) console_.line("t " + fixed(seconds(), k_seconds_decimals) + "  " + describe_event(event));
        if (tui_ != nullptr) tui_->on_event(event);
        switch (event.type) {
        case EventType::locked:
            if (open_) close("a new lock");
            open(event);
            break;
        case EventType::slot:
            // The last data slot of a window dropped as a framing error: it has no byte event.
            if (open_ && (event.flags & unlimited::event_flag_framing) != 0 &&
                event.slot == unlimited::k_stop_slot - 1) {
                ++receptions_.back().dropped;
                ++dropped_;
            }
            break;
        case EventType::byte:
            if (!open_) break;
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
            break;
        }
    }

    void open(const Event& event) {
        Reception reception;
        reception.last = event;
        reception.flags = 0;
        reception.dropped = 0;
        receptions_.push_back(reception);
        open_ = true;
        console_.item("locked", at() + ": " + signal_text(event));
        console_.item("bandwidth", unlimited::cli::bandwidth_line(options_.config, event.tone_hz, event.slot_ms));
    }

    void close(const std::string& ending) {
        open_ = false;
        const Reception& r = receptions_.back();
        std::vector<uint8_t> text;
        for (size_t i = 0; i < r.bytes.size(); ++i) text.push_back(r.bytes[i].value);
        std::string count = std::to_string(r.bytes.size()) + (r.bytes.size() == 1 ? " byte" : " bytes");
        if (r.dropped > 0) count += " (" + std::to_string(r.dropped) + " dropped as framing errors)";
        console_.item("rx", with_flags(count + ", " + ending + " at " + at() + " (" + signal_text(r.last) + ")",
                                       r.flags));
        console_.item("text", quoted(text));
    }

    const Options& options_;
    Console& console_;
    pc::Tui* tui_;
    std::vector<Reception> receptions_;
    bool open_;
    int64_t samples_;
    uint32_t dropped_;
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
    size_t measured = 0;  // byte events behind snr_db and slot_ms
};

int mismatch_bits(const std::vector<uint8_t>& expected, size_t position, uint8_t value) {
    return position < expected.size() ? bit_count(static_cast<unsigned>(expected[position] ^ value)) : k_bits_per_byte;
}

// Every lock that released bytes is one copy of the expected data (spec 3.3: a lock is always on a transmission's
// first byte), its bytes at their byte_index; a byte beyond the data or a second one at the same place is extra.
Comparison compare(const std::vector<Reception>& receptions, const std::vector<uint8_t>& expected) {
    const int k_empty = -1;
    Comparison c;
    c.expected = expected.size();
    c.locks = receptions.size();
    std::vector<std::vector<int> > copies;
    for (size_t r = 0; r < receptions.size(); ++r) {
        const Reception& reception = receptions[r];
        if (reception.bytes.empty()) continue;
        copies.push_back(std::vector<int>(expected.size(), k_empty));
        std::vector<int>& copy = copies.back();
        for (size_t i = 0; i < reception.bytes.size(); ++i) {
            const Event& event = reception.bytes[i];
            const size_t position = event.byte_index;
            if (position >= expected.size() || copy[position] != k_empty) {
                ++c.extra;
            } else {
                copy[position] = event.value;
            }
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
    const DecoderConfig& config = o.config;
    const std::string problem = unlimited::cli::decoder_problem(config);
    if (!problem.empty()) throw UsageError("refused: " + problem);

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
            tui->set_label(o.in_spec);
            tui->set_speed(unlimited::bytes_per_second(config.slot_us));
            tui->set_slot_ms(static_cast<float>(unlimited::cli::slot_ms_of(config.slot_us)));
            tui->set_passband(config.passband);
            tui->set_search_range(config.search_range());
            tui->set_field("threshold", config.decision_mode == DecisionMode::adaptive
                                            ? std::string("auto")
                                            : std::to_string(config.threshold_percent) + "%");
            tui->set_field("rate", std::to_string(rate_hz) + " Hz");
            console.hold(true);
        } else {
            std::fprintf(stderr, "%s: --tui needs a terminal on stdout; plain output\n", k_program);
            tui.reset();
        }
    }
    console.item("speed", unlimited::cli::speed_text(config.slot_us) + "  (the sender must use --bps " +
                              unlimited::cli::speed_number(config.slot_us) + ")");
    console.item("receiver", receiver_text(config));
    console.item("input", o.in_spec + ": " + std::to_string(rate_hz) + " Hz" +
                              (rate_hz == unlimited::k_decoder_rate_hz ? "" : ", resampled to 8000 Hz"));

    Receiver receiver(o, console, tui.get());
    Decoder decoder(config, &Receiver::on_event, &receiver);
    unlimited::DecoderSink decoder_sink(decoder);
    ClockedSink clocked(decoder_sink, receiver);
    pc::ResamplingSink resampling(clocked, rate_hz);
    MonitorSink monitor(resampling, rate_hz, tui.get(), o.realtime);
    const double slot_samples = unlimited::cli::slot_ms_of(config.slot_us) * k_rate_hz / unlimited::cli::k_ms_per_s;
    const std::vector<int16_t> lead(static_cast<size_t>(std::ceil(k_lead_slots * slot_samples)), 0);
    receiver.lead(lead.size());
    clocked.write(lead.data(), lead.size());
    const bool started = input->start(monitor, rate_hz);
    const bool delivered = started && input->wait();
    resampling.flush();
    const double window_samples = unlimited::k_window_slots * slot_samples;
    const std::vector<int16_t> drain(
        decoder.lookahead_samples() + static_cast<size_t>(std::ceil(k_drain_windows * window_samples)), 0);
    clocked.write(drain.data(), drain.size());
    receiver.finish();

    if (tui != nullptr) tui->close();
    console.hold(false);
    if (!delivered) {
        std::fprintf(stderr, "%s: reading %s failed\n", k_program, o.in_spec.c_str());
        return k_exit_io;
    }

    const bool decoded = receiver.byte_count() > 0;
    if (o.expect_path.empty()) {
        if (!decoded) std::fprintf(stderr, "%s: nothing decoded\n", k_program);
        return decoded ? k_exit_ok : k_exit_nothing;
    }

    const Comparison c = compare(receiver.receptions(), expected);
    const size_t bits = c.received * k_bits_per_byte;
    std::printf("expect     %zu bytes x %zu transmission%s: received %zu, lost %zu, wrong %zu, extra %zu, bit errors "
                "%zu/%zu (BER %.*e), locks %zu, dropped %u",
                c.expected, c.transmissions, c.transmissions == 1 ? "" : "s", c.received, c.lost, c.wrong, c.extra,
                c.bit_errors, bits, k_ber_digits, bits > 0 ? static_cast<double>(c.bit_errors) / bits : 0.0,
                c.locks, static_cast<unsigned>(receiver.dropped()));
    if (c.measured > 0)
        std::printf(", SNR %s dB, T %s ms", fixed(c.snr_db, k_db_decimals).c_str(),
                    fixed(c.slot_ms, k_ms_decimals).c_str());
    std::printf("\n");
    const bool match = c.transmissions > 0 && c.lost == 0 && c.wrong == 0 && c.extra == 0;
    std::printf("result     %s\n", match ? "match" : "mismatch");
    return match ? k_exit_ok : k_exit_nothing;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) {
            std::printf(k_usage, static_cast<double>(unlimited::k_min_bytes_per_second),
                        static_cast<double>(unlimited::k_max_bytes_per_second),
                        static_cast<double>(unlimited::k_default_bytes_per_second),
                        static_cast<unsigned>(unlimited::k_default_threshold_percent),
                        static_cast<unsigned>(unlimited::k_min_threshold_percent),
                        static_cast<unsigned>(unlimited::k_max_threshold_percent));
            return k_exit_ok;
        }
        return run(options);
    } catch (const UsageError& error) {
        std::fprintf(stderr, "%s: %s (see --help)\n", k_program, error.what());
        return k_exit_usage;
    }
}
