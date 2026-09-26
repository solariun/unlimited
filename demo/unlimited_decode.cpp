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
using unlimited::Event;
using unlimited::EventType;
using unlimited::LostReason;
using unlimited::SampleSink;
using unlimited::cli::Arguments;
using unlimited::cli::UsageError;
using unlimited::cli::find_name;
using unlimited::cli::fixed;
using unlimited::cli::k_exit_io;
using unlimited::cli::k_exit_ok;
using unlimited::cli::k_exit_usage;
using unlimited::cli::k_profiles;
using unlimited::cli::mode_text;
using unlimited::cli::side_name;
using unlimited::cli::to_fields;
using unlimited::cli::to_integer;
using unlimited::cli::to_number;

namespace pc = unlimited::pc;

const char* const k_program = "unlimited_decode";
const char* const k_default_in_spec = "rx.wav";

const int k_exit_nothing = 1;  // nothing decoded, or (--expect) not an exact match

const double k_rate_hz = unlimited::k_decoder_rate_hz;
const double k_ms_per_s = 1e3;
const int k_bits_per_byte = unlimited::k_bits_per_byte;
const double k_confidence_step_db = 0.5;  // Event::confidence unit
const size_t k_clock_step = 8;      // samples per decoder call: events are timed to 1 ms
const size_t k_drain_slots = 4;     // silence after the input, in the longest slots, so the last frame completes
const size_t k_offset_probe = 64;   // bytes of a late-join lock used to find its place in the expected data
const uint8_t k_first_printable = 0x20;
const uint8_t k_last_printable = 0x7E;

const int k_seconds_decimals = 3;
const int k_hz_decimals = 1;
const int k_ms_decimals = 2;
const int k_db_decimals = 1;
const int k_ber_digits = 2;

const char* const k_usage =
    "usage: unlimited_decode [--in SPEC] [--profile ssb|am|fm] [--min-slot-ms N] [--tone-range LO:HI]\n"
    "    [--no-blanker] [--packet] [--events] [--expect FILE] [--tui] [--realtime]\n"
    "\n"
    "Decodes Unlimited transmissions from an audio input (any rate; resampled to 8 kHz). The sender's header\n"
    "gives the mode (T, bits per peak, data slots, spacing, grid side): nothing else is configured. The grid\n"
    "side is reported as received: an inverted path (LSB against a USB sender) mirrors it.\n"
    "  --in SPEC               wav:<path>, <path>.wav or null (default rx.wav)\n"
    "  --profile               ssb: T 16..128 ms, am: 8..64 ms, fm: 4..32 ms (f_ref 1000..2700 Hz)\n"
    "  --min-slot-ms, --tone-range, --no-blanker   override the profile (f_ref search range)\n"
    "  --packet                print the CRC-valid packets found in the bytes\n"
    "  --events                print every decoder event, slot decisions included\n"
    "  --expect FILE           compare with the data that was sent: BER, loss, wrong bytes, locks, mode\n"
    "  --tui                   terminal view of the reception; --realtime paces file input to audio time\n"
    "output: one 'rx' line (mode, SNR, slot confidence, erasures, flags) and one 'text' line per reception\n"
    "        (lock to end, loss or end of input)\n"
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
    std::string profile = "ssb";
    bool has_min_slot = false;
    double min_slot_ms = 0.0;
    bool has_tone_range = false;
    double min_tone_hz = 0.0;
    double max_tone_hz = 0.0;
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
        } else if (option == "--tone-range") {
            const std::vector<double> range = to_fields(option, args.value(option), 2, 2);
            o.has_tone_range = true;
            o.min_tone_hz = range[0];
            o.max_tone_hz = range[1];
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

DecoderConfig decoder_config(const Options& o) {
    DecoderConfig config = DecoderConfig::for_profile(find_name(k_profiles, "--profile", o.profile).profile);
    if (o.has_min_slot) config.min_slot_ms = to_integer<uint8_t>("--min-slot-ms", o.min_slot_ms);
    if (o.has_tone_range) {
        config.min_tone_hz = to_integer<uint16_t>("--tone-range", o.min_tone_hz);
        config.max_tone_hz = to_integer<uint16_t>("--tone-range", o.max_tone_hz);
    }
    if (o.no_blanker) config.impulse_blanker = false;
    if (!config.valid())
        throw UsageError("invalid decoder settings: needs min slot 4..32 ms, 300 <= LO < HI <= 2700 Hz "
                         "(LO >= 1000 Hz below 8 ms)");
    return config;
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
    case LostReason::no_header:
        return "no_header";
    case LostReason::unsupported_mode:
        return "unsupported_mode";
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
                            {unlimited::event_flag_erasure, "erasure"},
                            {unlimited::event_flag_mode_memory, "mode_memory"},
                            {unlimited::event_flag_blind_mode, "blind_mode"}};

std::string flag_names(uint8_t flags) {
    std::string names;
    for (size_t i = 0; i < sizeof(k_flags) / sizeof(k_flags[0]); ++i)
        if (flags & k_flags[i].flag) names += std::string(names.empty() ? "" : ",") + k_flags[i].name;
    return names;
}

bool has_mode(const Event& event) {
    return event.bits_per_peak > 0 && event.data_slots > 0;
}

// "f_ref 2132.0 Hz  T 32 ms  k 5  N 8  standard  grid below  138.9 bit/s  snr 12.3 dB"
std::string signal_text(const Event& event) {
    const std::string mode = has_mode(event) ? mode_text(event.slot_ms, event.bits_per_peak, event.data_slots,
                                                         event.spacing, side_name(event.side))
                                             : "T " + fixed(event.slot_ms, k_ms_decimals) + " ms";
    return "f_ref " + fixed(event.tone_hz, k_hz_decimals) + " Hz  " + mode + "  snr " +
           fixed(event.snr_db, k_db_decimals) + " dB";
}

std::string soft_list(const int8_t* soft, size_t count) {
    std::string text;
    for (size_t i = 0; i < count; ++i) text += (i == 0 ? "" : " ") + std::to_string(static_cast<int>(soft[i]));
    return text;
}

std::string with_flags(const std::string& text, uint8_t flags) {
    const std::string names = flag_names(flags);
    return names.empty() ? text : text + "  " + names;
}

std::string describe_event(const Event& event) {
    switch (event.type) {
    case EventType::state:
        return std::string("state ") + state_name(event.state);
    case EventType::locked:
        return with_flags("locked  " + signal_text(event), event.flags);
    case EventType::slot:
        return with_flags("slot " + std::to_string(event.index) + "/" + std::to_string(event.data_slots) +
                              "  frame " + std::to_string(event.frame_index) + "  tone " +
                              std::to_string(event.tone) + "  symbol " + std::to_string(event.value) + "  level " +
                              std::to_string(event.level_pct) + "%  conf " +
                              fixed(event.confidence * k_confidence_step_db, k_db_decimals) + " dB  soft " +
                              soft_list(event.soft, std::min<size_t>(event.bits_per_peak, k_bits_per_byte)),
                          event.flags);
    case EventType::byte: {
        const std::vector<uint8_t> value(1, event.value);
        return with_flags("byte " + hex_byte(event.value) + " " + quoted(value) + "  frame " +
                              std::to_string(event.frame_index) + "." + std::to_string(event.index) + "  snr " +
                              fixed(event.snr_db, k_db_decimals) + " dB  soft " + soft_list(event.soft, k_bits_per_byte),
                          event.flags);
    }
    case EventType::end:
        return "end";
    case EventType::lost:
        return std::string("lost ") + reason_name(event.reason);
    }
    return "?";
}

// Lines go to stdout at once, or wait while the TUI owns the screen.
class Console {
public:
    Console() : held_(false) {}

    void hold(bool held) {
        held_ = held;
        if (held_) return;
        for (size_t i = 0; i < lines_.size(); ++i) std::printf("%s\n", lines_[i].c_str());
        lines_.clear();
    }

    void line(const std::string& text) {
        if (held_) {
            lines_.push_back(text);
        } else {
            std::printf("%s\n", text.c_str());
        }
    }

private:
    bool held_;
    std::vector<std::string> lines_;
};

struct Reception {  // one lock: from `locked` to end, lost or the end of the input
    bool late_join;
    std::vector<Event> bytes;
    Event last;     // locked or the latest byte: f_ref, mode, T and SNR
    uint8_t flags;  // every flag seen on its bytes
};

// Slot decisions since the last reception closed: they start before `locked` (spec 5.1).
struct SlotTally {
    size_t slots = 0;
    size_t erasures = 0;
    double confidence_db = 0.0;  // summed

    std::string text() const {
        if (slots == 0) return "slots 0";
        return "slots " + std::to_string(slots) + "  conf " + fixed(confidence_db / slots, k_db_decimals) +
               " dB  erasures " + std::to_string(erasures);
    }
};

class Receiver {
public:
    Receiver(const Options& options, Console& console, pc::Tui* tui)
        : options_(options),
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
        if (open_) close("input ended");
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
        const std::string names = flag_names(flags);
        receiver->console_.line("packet  " + std::to_string(size) + " bytes  " + quoted(bytes) +
                                (names.empty() ? "" : "  " + names));
    }

    void handle(const Event& event) {
        if (options_.events) console_.line("t " + fixed(seconds(), k_seconds_decimals) + "  " + describe_event(event));
        if (tui_ != nullptr) tui_->on_event(event);
        if (options_.packet) packets_.on_event(event);
        switch (event.type) {
        case EventType::locked:
            if (open_) close("relocked");
            open(event, (event.flags & unlimited::event_flag_late_join) != 0);
            break;
        case EventType::slot:
            ++tally_.slots;
            tally_.confidence_db += event.confidence * k_confidence_step_db;
            if (event.flags & unlimited::event_flag_erasure) ++tally_.erasures;
            break;
        case EventType::byte:
            if (!open_) open(event, true);
            receptions_.back().bytes.push_back(event);
            receptions_.back().last = event;
            receptions_.back().flags |= event.flags;
            break;
        case EventType::end:
            if (open_) close("end");
            tally_ = SlotTally();
            break;
        case EventType::lost:
            if (open_) close(std::string("lost ") + reason_name(event.reason));
            tally_ = SlotTally();
            break;
        case EventType::state:
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
    }

    void close(const std::string& ending) {
        open_ = false;
        const Reception& r = receptions_.back();
        std::vector<uint8_t> text;
        for (size_t i = 0; i < r.bytes.size(); ++i) text.push_back(r.bytes[i].value);
        console_.line(with_flags("rx      " + std::to_string(r.bytes.size()) + " bytes  " + signal_text(r.last) + "  " +
                                     tally_.text() + "  " + ending + "  t " + fixed(seconds(), k_seconds_decimals) +
                                     " s",
                                 r.flags));
        console_.line("text    " + quoted(text));
    }

    const Options& options_;
    Console& console_;
    pc::Tui* tui_;
    unlimited::PacketReader packets_;
    std::vector<Reception> receptions_;
    SlotTally tally_;
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
    size_t measured = 0;  // byte events behind snr_db and slot_ms
    Event mode = Event();  // the first byte event: the decoded mode
};

// Place of a byte in its transmission: frame_index B + index, B = N k / 8 bytes per frame (spec 5.1).
size_t byte_position(const Event& event) {
    const size_t frame_bytes = static_cast<size_t>(event.data_slots) * event.bits_per_peak / k_bits_per_byte;
    return static_cast<size_t>(event.frame_index) * frame_bytes + event.index;
}

int mismatch_bits(const std::vector<uint8_t>& expected, size_t position, uint8_t value) {
    return position < expected.size() ? bit_count(expected[position] ^ value) : k_bits_per_byte;
}

// A late-join lock numbers its frames from the join: its place is where its first bytes fit best.
size_t late_join_offset(const Reception& reception, const std::vector<uint8_t>& expected) {
    const size_t probe = std::min(k_offset_probe, reception.bytes.size());
    size_t best_offset = 0;
    size_t best_errors = std::numeric_limits<size_t>::max();
    for (size_t offset = 0; offset < expected.size(); ++offset) {
        size_t errors = 0;
        for (size_t i = 0; i < probe && errors < best_errors; ++i)
            errors += mismatch_bits(expected, offset + byte_position(reception.bytes[i]), reception.bytes[i].value);
        if (errors < best_errors) {
            best_errors = errors;
            best_offset = offset;
        }
    }
    return best_offset;
}

// Every lock is placed on a copy of the expected data: a sync lock numbers frames from byte 0 and starts a new
// transmission; a late join continues the current transmission where it fits, or starts a new one.
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
            const size_t position = offset + byte_position(reception.bytes[i]);
            fits = position >= expected.size() || copies.back()[position] == k_empty;
        }
        if (!fits) copies.push_back(std::vector<int>(expected.size(), k_empty));
        std::vector<int>& copy = copies.back();
        for (size_t i = 0; i < reception.bytes.size(); ++i) {
            const Event& event = reception.bytes[i];
            const size_t position = offset + byte_position(event);
            if (position >= expected.size() || copy[position] != k_empty) {
                ++c.extra;
            } else {
                copy[position] = event.value;
            }
            if (c.measured == 0) c.mode = event;
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
            c.bit_errors += errors;
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
            tui->set_profile(o.profile);
            tui->set_field("rate", std::to_string(rate_hz) + " Hz");
            console.hold(true);
        } else {
            std::fprintf(stderr, "%s: --tui needs a terminal on stdout; plain output\n", k_program);
            tui.reset();
        }
    }

    Receiver receiver(o, console, tui.get());
    Decoder decoder(config, &Receiver::on_event, &receiver);
    unlimited::DecoderSink decoder_sink(decoder);
    ClockedSink clocked(decoder_sink, receiver);
    pc::ResamplingSink resampling(clocked, rate_hz);
    MonitorSink monitor(resampling, rate_hz, tui.get(), o.realtime);
    const bool started = input->start(monitor, rate_hz);
    const bool delivered = started && input->wait();
    resampling.flush();
    const std::vector<int16_t> drain(
        static_cast<size_t>(k_drain_slots * config.max_slot_ms() * k_rate_hz / k_ms_per_s), 0);
    clocked.write(drain.data(), drain.size());
    receiver.finish();

    if (tui != nullptr) tui->close();
    console.hold(false);
    if (!delivered) {
        std::fprintf(stderr, "%s: reading %s failed\n", k_program, o.in_spec.c_str());
        return k_exit_io;
    }

    if (o.packet)
        std::printf("packets %zu valid, %u crc errors\n", receiver.packet_count(),
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
    std::printf("expect  %zu bytes x %zu transmission%s  received %zu  lost %zu  wrong %zu  extra %zu  "
                "bit errors %zu/%zu (BER %.*e)  locks %zu",
                c.expected, c.transmissions, c.transmissions == 1 ? "" : "s", c.received, c.lost, c.wrong, c.extra,
                c.bit_errors, bits, k_ber_digits, bits > 0 ? static_cast<double>(c.bit_errors) / bits : 0.0,
                c.locks);
    if (c.measured > 0)
        std::printf("  snr %s dB  T %s ms  mode %s", fixed(c.snr_db, k_db_decimals).c_str(),
                    fixed(c.slot_ms, k_ms_decimals).c_str(),
                    mode_text(c.mode.slot_ms, c.mode.bits_per_peak, c.mode.data_slots, c.mode.spacing,
                              side_name(c.mode.side))
                        .c_str());
    std::printf("\n");
    const bool bytes_match = c.transmissions > 0 && c.lost == 0 && c.wrong == 0 && c.extra == 0;
    const bool match = o.packet ? receiver.packet_payloads() == expected && bytes_match : bytes_match;
    std::printf("result  %s\n", match ? "match" : "mismatch");
    return match ? k_exit_ok : k_exit_nothing;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) {
            std::fputs(k_usage, stdout);
            return k_exit_ok;
        }
        return run(options);
    } catch (const UsageError& error) {
        std::fprintf(stderr, "%s: %s (see --help)\n", k_program, error.what());
        return k_exit_usage;
    }
}
