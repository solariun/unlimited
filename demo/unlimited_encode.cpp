#include "audio.hpp"
#include "audio_live.hpp"
#include "channel.hpp"
#include "cli.hpp"
#include "ptt.hpp"
#include "radio_options.hpp"
#include "terminal.hpp"
#include "tui.hpp"
#include "unlimited/audio_io.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using std::int16_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using unlimited::DecoderConfig;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderStatus;
using unlimited::Passband;
using unlimited::SampleSource;
using unlimited::cli::Arguments;
using unlimited::cli::Console;
using unlimited::cli::UsageError;
using unlimited::cli::fixed;
using unlimited::cli::find_name;
using unlimited::cli::k_exit_io;
using unlimited::cli::k_exit_ok;
using unlimited::cli::k_exit_usage;
using unlimited::cli::ms_text;
using unlimited::cli::to_clamped;
using unlimited::cli::to_fields;
using unlimited::cli::to_integer;
using unlimited::cli::to_number;
using unlimited::cli::to_passband;

namespace pc = unlimited::pc;
namespace sim = unlimited::sim;

const char* const k_program = "unlimited_encode";
const char* const k_default_out_spec = "tx.wav";

const int k_exit_stopped = 1;  // a signal (Ctrl-C) stopped the transmission before its end

const uint32_t k_default_rate_hz = 8000;
// The TX delay when the PTT keys the radio by RTS, DTR or CAT (Gustavo, 2026-09-28): silence before the first START
// while the transmitter switches over. VOX has its lead tone instead.
const uint16_t k_keyed_lead_in_ms = 100;
const double k_default_level_dbfs = -3.0;
const double k_dbfs_reference = 32767.0;  // amplitude of 0 dBFS
const double k_int16_scale = 32768.0;     // int16 <-> float in [-1, 1)
const double k_output_peak_dbfs = -1.0;   // a louder channel output is scaled down to this peak
const double k_amplitude_db = 20.0;
const double k_power_db = 10.0;
const uint32_t k_us_per_ms = 1000;
const uint32_t k_window_slots = unlimited::k_window_slots;

// Samples per Encoder::render() call. A slot has at least 32 samples, so the queue is refilled several
// times per window (it never runs dry while data remains) and every slot is seen by the TUI.
const size_t k_render_step = 16;

// The view of a live transmission: what the output's callback may publish before the main thread takes it.
const size_t k_view_statuses = 1024;         // slots: 4 s at 25 bytes/s
const uint32_t k_view_audio_seconds = 1;     // audio for the scope, the spectrum and the level
const size_t k_view_chunk_samples = 1024;    // samples the main thread takes at a time

const int k_db_decimals = 1;
const int k_seconds_decimals = 3;

const char* const k_usage =
    "usage: unlimited_encode (--text STR | --in FILE) [--out SPEC] [--bps B]\n"
    "    [--tone HZ] [--passband LO:HI] [--rate 8000] [--level-dbfs -3] [--lead-in-ms N] [--vox-lead-ms N]\n"
    "    [--tail-ms N] [--fade-bridge]\n"
    "    [--channel clean|usb|lsb|am|fm [--snr DB] [--offset HZ] [--pivot HZ] [--rx-passband LO:HI]\n"
    "        [--fading none|flat|good|moderate|poor|flutter] [--doppler HZ] [--qsb DEPTH_DB:RATE_HZ]\n"
    "        [--impulses RATE[:LEVEL_DB]] [--carrier HZ:DB] [--cw HZ:DB:WPM] [--agc]\n"
    "        [--fm-deviation HZ] [--no-preemphasis] [--no-deemphasis] [--clock-ppm P] [--seed N]\n"
    "        [--clean-out SPEC]]\n"
    "    [--tui] [--realtime]\n"
    "    [-d SPEC | --output SPEC] [-r HZ] [--list-devices] [--ptt METHOD [--ptt-device DEV] [--ptt-invert]\n"
    "        [--cat-rate BAUD] [--cat-addr ADDR] [--cat-tx-on HEX --cat-tx-off HEX]]\n"
    "\n"
    "Sends bytes through a radio's audio as short beeps on one pitch, like a serial port: every byte is a window\n"
    "of 10 time slots, a START tone, its 8 bits (a beep is a 1, silence is a 0, most significant bit first) and\n"
    "a STOP tone. The receiver must be given the same speed (--bps); it finds the pitch by itself. The audio goes\n"
    "to a sound card (the radio's modulation input, with its PTT keyed) or to a WAV file.\n"
    "\n"
    "What to send\n"
    "  --text STR, --in FILE   the data: a text, or the bytes of a file\n"
    "  --out SPEC              where the audio goes: a sound card (as -d below), wav:<path>, <path>.wav or null\n"
    "                          (default tx.wav); the same as --output\n"
    "\n"
    "The signal\n"
    "  --bps B                 the speed in bytes per second, %.2f..%.2f in steps of 0.01 (default %.2f, for HF\n"
    "                          SSB): the slot is T = 1 / (10 B). Slower is narrower and survives more noise\n"
    "  --tone HZ               the pitch, 300..2700 Hz (default 1500)\n"
    "  --passband LO:HI        the receiver's audio filter the signal must fit (default 300:2700, a 2.4 kHz\n"
    "                          SSB filter; 300:2100 is a 1.8 kHz one). A signal that does not fit is refused\n"
    "  --rate HZ               a file's sample rate, 8000..192000 (default 8000); a sound card plays at its own\n"
    "  --level-dbfs DB         loudness of a beep's crest (default -3)\n"
    "  --lead-in-ms N          silence before the signal, for the PTT and the transmitter to settle (default %u\n"
    "                          when --ptt keys the radio by RTS, DTR or CAT; otherwise 0)\n"
    "  --vox-lead-ms N         a steady tone of N ms (at least 3 slots) and 2 silent slots before the first\n"
    "                          byte, to key a VOX radio (default %u on a sound card keyed by VOX, the default\n"
    "                          --ptt, and with --ptt vox; otherwise 0: none)\n"
    "  --tail-ms N             silence after the last byte, at least 2 slots (default %u)\n"
    "  --fade-bridge           for a receiver that bridges HF fades (its --fade-bridge): at least %u ms of silence\n"
    "                          before the first START (the lead-in, unless a VOX lead precedes it); off by\n"
    "                          default. Both sides must use the same setting, like the speed\n"
    "\n"
    "Channel simulator: hear the signal through a radio path (--out then gets what the receiver hears)\n"
    "  --channel MODE          clean, usb, lsb, am or fm\n"
    "  --snr DB                signal to noise in 2500 Hz: key-down tone (usb, lsb) or carrier (am, fm)\n"
    "  --offset HZ             mistuning: the pitch moves by it; --pivot HZ: the lsb mirror point (3000)\n"
    "  --rx-passband LO:HI     the receiver's filter (default: --passband)\n"
    "  --fading, --doppler, --qsb, --impulses, --carrier, --cw, --agc, --fm-deviation, --no-preemphasis,\n"
    "  --no-deemphasis, --clock-ppm, --seed: fading, interference and radio details\n"
    "  --clean-out SPEC        also write the transmitted audio\n"
    "\n"
    "Display\n"
    "  --tui                   live view: the window being sent, its bits and byte, the output level, scope and\n"
    "                          spectrum\n"
    "  --realtime              pace a file's output to audio time (a sound card plays in real time)\n"
    "\n";

const char* const k_usage_end =
    "\n"
    "Every run prints the speed first, then the occupied bandwidth, whether it fits the passband, and how far the\n"
    "radio may be mistuned (the shift tolerance). On a sound card it keys the PTT before the audio and releases it\n"
    "once the audio has left the device; Ctrl-C stops the transmission and releases the PTT.\n"
    "exit codes: 0 sent, 1 stopped by Ctrl-C before the end, 2 usage error or refused configuration,\n"
    "            3 input/output error (a file, the sound card or the PTT)\n";

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

struct ModeName {
    const char* name;
    sim::Mode mode;
};

const ModeName k_modes[] = {{"clean", sim::Mode::clean}, {"usb", sim::Mode::usb}, {"lsb", sim::Mode::lsb},
                            {"am", sim::Mode::am},       {"fm", sim::Mode::fm}};

struct FadingName {
    const char* name;
    sim::FadingPreset preset;
};

const FadingName k_fadings[] = {{"none", sim::FadingPreset::none},
                                {"flat", sim::FadingPreset::flat},
                                {"good", sim::FadingPreset::ccir_good},
                                {"moderate", sim::FadingPreset::ccir_moderate},
                                {"poor", sim::FadingPreset::ccir_poor},
                                {"flutter", sim::FadingPreset::flutter}};

struct Options {
    bool help = false;
    bool has_text = false;
    std::string text;
    std::string data_path;
    // --output, -d, -r, --list-devices and the --ptt family (and --out)
    pc::RadioOptions radio = pc::RadioOptions(pc::k_radio_output | pc::k_radio_ptt);
    bool ptt_vox_named = false;  // the last --ptt was vox: the VOX lead's default, even for a file

    uint32_t slot_us = unlimited::slot_us_for_centi_speed(unlimited::k_default_centi_bytes_per_second);
    bool has_tone = false;
    double tone_hz = 0.0;
    bool has_passband = false;
    Passband passband = Passband();
    bool has_rate = false;
    double rate_hz = k_default_rate_hz;
    double level_dbfs = k_default_level_dbfs;
    bool has_lead_in = false;
    double lead_in_ms = 0.0;
    bool has_vox_lead = false;
    double vox_lead_ms = 0.0;
    bool has_tail = false;
    double tail_ms = 0.0;
    bool fade_bridge = false;  // --fade-bridge: silence before the first START for a receiver bridging fades

    bool channel = false;
    std::string channel_name;
    std::string channel_option;  // first channel option seen, for "needs --channel"
    sim::ChannelConfig channel_config;
    std::string fading;
    bool has_doppler = false;
    double doppler_hz = 0.0;
    bool has_rx_passband = false;
    Passband rx_passband = Passband();
    std::string clean_out;

    bool tui = false;
    bool realtime = false;

    std::string out_spec() const { return radio.output.empty() ? k_default_out_spec : radio.output; }
    bool live() const { return unlimited::cli::is_live(out_spec()); }
};

// Channel options; false when `option` is not one of them.
bool parse_channel_option(const std::string& option, Arguments& args, Options& o) {
    sim::ChannelConfig& c = o.channel_config;
    if (option == "--snr") {
        c.snr_db = to_number(option, args.value(option));
    } else if (option == "--offset") {
        c.freq_offset_hz = to_number(option, args.value(option));
    } else if (option == "--pivot") {
        c.lsb_pivot_hz = to_number(option, args.value(option));
    } else if (option == "--rx-passband") {
        o.has_rx_passband = true;
        o.rx_passband = to_passband(option, args.value(option));
    } else if (option == "--fading") {
        o.fading = args.value(option);
        find_name(k_fadings, option, o.fading);
    } else if (option == "--doppler") {
        o.has_doppler = true;
        o.doppler_hz = to_number(option, args.value(option));
    } else if (option == "--qsb") {
        const std::vector<double> v = to_fields(option, args.value(option), 2, 2);
        c.qsb_depth_db = v[0];
        c.qsb_rate_hz = v[1];
    } else if (option == "--impulses") {
        const std::vector<double> v = to_fields(option, args.value(option), 1, 2);
        c.impulse_rate_hz = v[0];
        if (v.size() > 1) c.impulse_level_db = v[1];
    } else if (option == "--carrier") {
        const std::vector<double> v = to_fields(option, args.value(option), 2, 2);
        c.carrier_hz = v[0];
        c.carrier_db = v[1];
    } else if (option == "--cw") {
        const std::vector<double> v = to_fields(option, args.value(option), 3, 3);
        c.cw_hz = v[0];
        c.cw_db = v[1];
        c.cw_wpm = v[2];
    } else if (option == "--agc") {
        c.agc = true;
    } else if (option == "--fm-deviation") {
        c.fm_deviation_hz = to_number(option, args.value(option));
    } else if (option == "--no-preemphasis") {
        c.fm_tx_preemphasis = false;
    } else if (option == "--no-deemphasis") {
        c.fm_rx_deemphasis = false;
    } else if (option == "--clock-ppm") {
        c.clock_ppm = to_number(option, args.value(option));
    } else if (option == "--seed") {
        c.seed = to_integer<uint32_t>(option, to_number(option, args.value(option)));
    } else if (option == "--clean-out") {
        o.clean_out = args.value(option);
    } else {
        return false;
    }
    if (o.channel_option.empty()) o.channel_option = option;
    return true;
}

// The rules between a sound card and the other options (spec 12.6): -r and the keying PTT methods need a sound card,
// --rate and --realtime are for files, and the channel simulator's output never keys a radio.
void check_output_rules(const Options& o) {
    const bool live = o.live();
    const std::string ptt = std::string("--ptt ") + pc::ptt_method_name(o.radio.ptt.method);
    const bool keyed = o.radio.ptt.method != pc::PttMethod::vox;
    if (o.radio.rate_hz != 0 && !live)
        throw UsageError("-r opens a sound card at a rate; " + o.out_spec() + " is not one (--rate sets a file's)");
    if (o.has_rate && live)
        throw UsageError("--rate sets a file's sample rate; the sound card " + o.out_spec() +
                         " plays at its own (-r asks a Linux device for another)");
    if (o.realtime && live) throw UsageError("--realtime paces a file; a sound card plays in real time");
    if (keyed && !live)
        throw UsageError(ptt + " keys a radio: it needs a sound card (--output coreaudio:<#|name part|UID>, "
                               "alsa:<name> or default), not " + o.out_spec());
    if (keyed && o.channel)
        throw UsageError("--channel plays what a receiver would hear, not a signal for the air: it keys no radio (" +
                         ptt + ")");
}

Options parse_options(int argc, char** argv) {
    Options o;
    Arguments args(argc, argv);
    std::string option;
    std::string error;
    while (args.next(option)) {
        bool has_value = false;
        if (o.radio.takes(option, has_value)) {
            if (!o.radio.apply(option, has_value ? args.value(option) : std::string(), error)) throw UsageError(error);
            if (option == "--ptt") o.ptt_vox_named = o.radio.ptt.method == pc::PttMethod::vox;
        } else if (option == "--help" || option == "-h") {
            o.help = true;
        } else if (option == "--text") {
            o.has_text = true;
            o.text = args.value(option);
        } else if (option == "--in") {
            o.data_path = args.value(option);
        } else if (option == "--out") {
            o.radio.output = unlimited::cli::to_device_spec(option, args.value(option));
        } else if (option == "--bps") {
            o.slot_us = unlimited::cli::to_slot_us(option, args.value(option));
        } else if (option == "--tone") {
            o.has_tone = true;
            o.tone_hz = to_number(option, args.value(option));
        } else if (option == "--passband") {
            o.has_passband = true;
            o.passband = to_passband(option, args.value(option));
        } else if (option == "--rate") {
            o.has_rate = true;
            o.rate_hz = to_number(option, args.value(option));
        } else if (option == "--level-dbfs") {
            o.level_dbfs = to_number(option, args.value(option));
        } else if (option == "--lead-in-ms") {
            o.has_lead_in = true;
            o.lead_in_ms = to_number(option, args.value(option));
        } else if (option == "--vox-lead-ms") {
            o.has_vox_lead = true;
            o.vox_lead_ms = to_number(option, args.value(option));
        } else if (option == "--tail-ms") {
            o.has_tail = true;
            o.tail_ms = to_number(option, args.value(option));
        } else if (option == "--fade-bridge") {
            o.fade_bridge = true;
        } else if (option == "--channel") {
            o.channel = true;
            o.channel_name = args.value(option);
            o.channel_config.mode = find_name(k_modes, option, o.channel_name).mode;
        } else if (option == "--tui") {
            o.tui = true;
        } else if (option == "--realtime") {
            o.realtime = true;
        } else if (!parse_channel_option(option, args, o)) {
            throw UsageError("unknown option '" + option + "'");
        }
    }
    if (o.help || o.radio.list_devices) return o;
    if (o.has_text == !o.data_path.empty()) throw UsageError("give exactly one of --text and --in");
    if (!o.channel && !o.channel_option.empty()) throw UsageError(o.channel_option + " needs --channel");
    if (o.has_doppler && (o.fading.empty() || o.fading == "none")) throw UsageError("--doppler needs --fading");
    if (!o.radio.check(error)) throw UsageError(error);
    check_output_rules(o);
    return o;
}

// The VOX lead (spec 2.1, V8): --vox-lead-ms when given (0: none); otherwise 150 ms whenever a radio is keyed by VOX,
// that is a sound card without another --ptt (VOX is the default) or --ptt vox named; none for a file.
uint16_t vox_lead_ms(const Options& o) {
    if (o.has_vox_lead) return to_integer<uint16_t>("--vox-lead-ms", o.vox_lead_ms);
    const bool vox = o.radio.ptt.method == pc::PttMethod::vox && (o.live() || o.ptt_vox_named);
    return vox ? unlimited::k_default_vox_lead_ms : 0;
}

// The options applied to the library's defaults, at `sample_rate_hz` (a file's --rate, or the sound card's rate).
// Values far out of range are clamped into the field, so that check() names the rule they break. The lead-in:
// --lead-in-ms when given; otherwise k_keyed_lead_in_ms when the PTT keys the radio by RTS, DTR or CAT (only a sound
// card has such a PTT), else none. With --fade-bridge and no VOX lead, it is at least k_fade_bridge_silence_ms (a VOX
// lead and its gap already qualify).
EncoderConfig encoder_config(const Options& o, uint32_t sample_rate_hz) {
    EncoderConfig config;
    config.sample_rate_hz = sample_rate_hz;
    config.slot_us = o.slot_us;
    if (o.has_tone) config.tone_hz = to_clamped<uint16_t>(o.tone_hz);
    if (o.has_passband) config.passband = o.passband;
    if (o.level_dbfs > 0.0) throw UsageError("--level-dbfs must be 0 or below (0 dBFS is full scale)");
    config.amplitude = to_clamped<int16_t>(k_dbfs_reference * std::pow(10.0, o.level_dbfs / k_amplitude_db));
    if (o.has_lead_in)
        config.lead_in_ms = to_integer<uint16_t>("--lead-in-ms", o.lead_in_ms);
    else if (o.radio.ptt.method != pc::PttMethod::vox)
        config.lead_in_ms = k_keyed_lead_in_ms;
    config.vox_lead_ms = vox_lead_ms(o);
    if (o.fade_bridge && config.vox_lead_ms == 0)
        config.lead_in_ms = std::max(config.lead_in_ms, unlimited::cli::k_fade_bridge_silence_ms);
    if (o.has_tail) config.tail_ms = to_integer<uint16_t>("--tail-ms", o.tail_ms);
    return config;
}

// The pitch and T are in range, so the band means something.
bool band_known(const EncoderConfig& config) {
    return config.tone_hz >= unlimited::k_min_tone_hz && config.tone_hz <= unlimited::k_max_tone_hz &&
           unlimited::slot_valid(config.slot_us);
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

int bit_count(uint8_t value) {
    int count = 0;
    for (unsigned v = value; v != 0; v &= v - 1) ++count;
    return count;
}

// Average power of the windows over the key-down power (spec 1.1): START, STOP and each data 1 are beeps of
// k_beep_energy.
double average_power_ratio(const std::vector<uint8_t>& data) {
    const size_t markers_per_window = 2;
    size_t beeps = 0;
    for (size_t i = 0; i < data.size(); ++i) beeps += markers_per_window + static_cast<size_t>(bit_count(data[i]));
    return beeps * unlimited::k_beep_energy / static_cast<double>(data.size() * k_window_slots);
}

// max(ceil(vox_lead_ms / T), k_min_vox_lead_slots), 0 without a VOX lead (spec 2.1).
uint32_t vox_lead_slots(const EncoderConfig& config) {
    if (config.vox_lead_ms == 0) return 0;
    const uint32_t lead_us = static_cast<uint32_t>(config.vox_lead_ms) * k_us_per_ms;
    return std::max<uint32_t>((lead_us + config.slot_us - 1) / config.slot_us, unlimited::k_min_vox_lead_slots);
}

std::string count_text(size_t count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// "lead-in 0 ms, VOX lead 9 slots and a gap of 2, 2 windows of 10 slots, tail 100 ms"
std::string layout_text(const EncoderConfig& config, size_t bytes) {
    std::string text = "lead-in " + std::to_string(config.lead_in_ms) + " ms, ";
    const uint32_t vox = vox_lead_slots(config);
    if (vox > 0)
        text += "VOX lead " + count_text(vox, "slot", "slots") + " and a gap of " +
                std::to_string(unlimited::k_vox_gap_slots) + ", ";
    const double min_tail_ms = unlimited::k_min_tail_slots * unlimited::cli::slot_ms_of(config.slot_us);
    return text + count_text(bytes, "window", "windows") + " of 10 slots, tail " +
           ms_text(std::max<double>(config.tail_ms, min_tail_ms)) + " ms";
}

// ---------------------------------------------------------------------------
// Audio path
// ---------------------------------------------------------------------------

struct StatusMark {
    uint64_t sample;  // samples rendered when the status was read
    EncoderStatus status;
};

// Where TransmitSource reports each new slot's status (and the idle status at the end) for the view.
class StatusSink {
public:
    virtual void status(uint64_t sample, const EncoderStatus& status) = 0;

protected:
    ~StatusSink() {}
};

// For a file, and for the channel simulator's first pass: every status in order, handed to the view as the audio passes
// (MonitorSource, LiveSource).
class MarkRecorder final : public StatusSink {
public:
    void status(uint64_t sample, const EncoderStatus& status) override {
        const StatusMark mark = {sample, status};
        marks.push_back(mark);
    }

    std::vector<StatusMark> marks;
};

// For a sound card: straight into the view's lock-free ring (the output's real-time callback: no lock, no allocation).
class RingPublisher final : public StatusSink {
public:
    explicit RingPublisher(pc::StatusRing& ring) : ring_(ring) {}
    void status(uint64_t, const EncoderStatus& status) override { ring_.push(status); }

private:
    pc::StatusRing& ring_;
};

// Renders the transmission, writing the data into the encoder queue as it drains, and reports each new slot's status
// (spec 2.5: this is the encoder's producer and its consumer, the one caller of status()). A sound card pulls it from
// its real-time callback: the data is fixed before start(), and nothing here locks or allocates.
class TransmitSource final : public SampleSource {
public:
    TransmitSource(Encoder& encoder, const std::vector<uint8_t>& data, StatusSink* statuses)
        : encoder_(encoder), data_(data), statuses_(statuses), written_(0), rendered_(0), last_(), has_last_(false) {}

    bool start() {
        refill();
        return encoder_.start();
    }

    size_t read(int16_t* out, size_t count) override {
        size_t done = 0;
        while (done < count) {
            refill();
            const size_t rendered = encoder_.render(out + done, std::min(k_render_step, count - done));
            if (rendered == 0) break;
            done += rendered;
            rendered_ += rendered;
            record();
        }
        return done;
    }

    // Once the device has stopped (or for a file, after it).
    bool complete() const { return written_ == data_.size() && !encoder_.busy(); }
    size_t sent() const { return written_ - encoder_.queued(); }  // bytes whose window was sent whole

private:
    void refill() {
        if (written_ < data_.size()) written_ += encoder_.write(&data_[written_], data_.size() - written_);
    }

    void record() {
        if (statuses_ == nullptr) return;
        const EncoderStatus status = encoder_.status();
        if (has_last_ && last_.slot_index == status.slot_index && last_.segment == status.segment) return;
        last_ = status;
        has_last_ = true;
        statuses_->status(rendered_, status);
    }

    Encoder& encoder_;
    const std::vector<uint8_t>& data_;
    StatusSink* statuses_;
    size_t written_;
    uint64_t rendered_;
    EncoderStatus last_;
    bool has_last_;
};

// Watches the audio on its way to a file: feeds the TUI (statuses, audio, the output level) and paces the output to
// real time. It runs where the file device pulls: the main thread.
class MonitorSource final : public SampleSource {
public:
    MonitorSource(SampleSource& source, uint32_t rate_hz, pc::Tui* tui, const std::vector<StatusMark>* marks,
                  bool realtime)
        : source_(source),
          rate_hz_(rate_hz),
          tui_(tui),
          marks_(marks),
          next_mark_(0),
          position_(0),
          realtime_(realtime),
          pacer_(rate_hz) {}

    size_t read(int16_t* out, size_t count) override {
        const size_t samples = source_.read(out, count);
        position_ += samples;
        if (tui_ != nullptr) {
            while (next_mark_ < marks_->size() && (*marks_)[next_mark_].sample <= position_)
                tui_->on_encoder_status((*marks_)[next_mark_++].status);
            tui_->push_audio(out, samples, rate_hz_);
            meter_.push(out, samples, rate_hz_);
            if (refresh_.due()) draw();
        }
        if (realtime_) pacer_.advance(samples);
        return samples;
    }

    const pc::LevelMeter& meter() const { return meter_; }

private:
    void draw() {
        tui_->set_level(meter_.recent());
        tui_->draw();
    }

    SampleSource& source_;
    uint32_t rate_hz_;
    pc::Tui* tui_;
    const std::vector<StatusMark>* marks_;
    size_t next_mark_;
    uint64_t position_;
    bool realtime_;
    pc::RealtimePacer pacer_;
    pc::RefreshPacer refresh_;
    pc::LevelMeter meter_;
};

// The view of a transmission through a sound card (spec 12.6): the output's real-time callback publishes each new
// slot's status and the audio it played into lock-free rings (allocated here, before the device starts); the main
// thread takes them at each frame, measures the output level and draws, while a helper thread waits in drain().
class LiveView {
public:
    LiveView(pc::Tui& tui, uint32_t rate_hz)
        : tui_(tui),
          rate_hz_(rate_hz),
          statuses_(k_view_statuses),
          publisher_(statuses_),
          audio_(static_cast<size_t>(rate_hz) * k_view_audio_seconds),
          chunk_(k_view_chunk_samples) {}

    StatusSink& publisher() { return publisher_; }  // TransmitSource's, in the callback
    pc::StatusRing& statuses() { return statuses_; }
    void audio(const int16_t* samples, size_t count) { audio_.push(samples, count, 1); }  // the callback

    // Draws at the refresh rate until drain() returns (the audio left the device, or a stop); returns what it returned.
    bool run(pc::OutputDevice& output) {
        std::mutex mutex;
        std::condition_variable changed;
        bool done = false;
        bool drained = false;
        std::thread waiter([&] {
            const bool result = output.drain();
            {
                std::lock_guard<std::mutex> lock(mutex);
                drained = result;
                done = true;
            }
            changed.notify_all();
        });
        pc::RefreshPacer refresh;
        std::unique_lock<std::mutex> lock(mutex);
        while (!done) {
            changed.wait_until(lock, refresh.next(), [&] { return done; });
            if (done || !refresh.due()) continue;
            lock.unlock();
            take();
            tui_.draw();
            lock.lock();
        }
        lock.unlock();
        waiter.join();
        take();
        return drained;
    }

    // Everything published so far, into the view.
    void take() {
        EncoderStatus status = EncoderStatus();
        while (statuses_.pop(status)) tui_.on_encoder_status(status);
        size_t count = 0;
        while ((count = audio_.pop(&chunk_[0], chunk_.size())) > 0) {
            tui_.push_audio(&chunk_[0], count, rate_hz_);
            meter_.push(&chunk_[0], count, rate_hz_);
        }
        tui_.set_level(meter_.recent());
    }

private:
    pc::Tui& tui_;
    uint32_t rate_hz_;
    pc::StatusRing statuses_;
    RingPublisher publisher_;
    pc::SampleRing audio_;
    std::vector<int16_t> chunk_;
    pc::LevelMeter meter_;
};

// What a sound card's real-time callback pulls: the transmission (or the audio the channel simulator made of it), its
// level measured and, for a view, its audio published, with the marks of the simulator's first pass (replay) handed
// over as the audio passes. No lock, no allocation.
class LiveSource final : public SampleSource {
public:
    LiveSource(SampleSource& source, uint32_t rate_hz, LiveView* view, const std::vector<StatusMark>* replay)
        : source_(source), rate_hz_(rate_hz), view_(view), replay_(replay), next_mark_(0), position_(0) {}

    size_t read(int16_t* out, size_t count) override {
        const size_t samples = source_.read(out, count);
        position_ += samples;
        meter_.push(out, samples, rate_hz_);
        if (view_ == nullptr) return samples;
        while (replay_ != nullptr && next_mark_ < replay_->size() && (*replay_)[next_mark_].sample <= position_)
            view_->statuses().push((*replay_)[next_mark_++].status);
        view_->audio(out, samples);
        return samples;
    }

    // Once the device has stopped.
    const pc::LevelMeter& meter() const { return meter_; }
    uint64_t samples() const { return position_; }

private:
    SampleSource& source_;
    uint32_t rate_hz_;
    LiveView* view_;
    const std::vector<StatusMark>* replay_;
    size_t next_mark_;
    uint64_t position_;
    pc::LevelMeter meter_;
};

void stop_output(void* output) {
    static_cast<pc::OutputDevice*>(output)->stop();
}

struct LiveOutcome {
    bool keyed = false;      // key(true) succeeded
    bool started = false;    // the device started
    bool drained = false;    // the audio left the device
    bool unkeyed = false;    // key(false) succeeded
    bool device_ok = false;  // the device stopped without an error
    int signal_number = 0;   // the signal that stopped it; 0 for none
};

// A transmission through a sound card (spec 12.4 to 12.6): the PTT keyed, the device started (its callback pulls
// `source`), drain() until the audio has left the device, the PTT released, the device stopped. Ctrl-C, SIGTERM and
// SIGHUP stop the device instead of the program, so the PTT is always released. With a view, a helper thread waits
// in drain() and this thread draws.
LiveOutcome transmit_live(pc::OutputDevice& output, pc::Ptt& ptt, SampleSource& source, uint32_t rate_hz,
                          LiveView* view) {
    LiveOutcome outcome;
    pc::StopOnSignals stop(&stop_output, &output);
    outcome.keyed = ptt.key(true);
    if (outcome.keyed) outcome.started = output.start(source, rate_hz);
    if (outcome.started) outcome.drained = view != nullptr ? view->run(output) : output.drain();
    outcome.unkeyed = ptt.key(false);
    output.stop();
    outcome.device_ok = outcome.started && output.wait();
    outcome.signal_number = stop.last_signal();
    return outcome;
}

// Runs the clean audio through the channel. The output is scaled down when its peak would exceed
// k_output_peak_dbfs, so noise and interference never clip; gain_db reports that scaling.
std::vector<int16_t> through_channel(sim::Channel& channel, const std::vector<int16_t>& clean, double& gain_db) {
    std::vector<float> in(clean.size());
    for (size_t i = 0; i < clean.size(); ++i) in[i] = static_cast<float>(clean[i] / k_int16_scale);
    const std::vector<float> out = channel.process(in);
    double peak = 0.0;
    for (size_t i = 0; i < out.size(); ++i) peak = std::max(peak, static_cast<double>(std::fabs(out[i])));
    const double limit = std::pow(10.0, k_output_peak_dbfs / k_amplitude_db);
    const double gain = peak > limit ? limit / peak : 1.0;
    gain_db = k_amplitude_db * std::log10(gain);
    const double low = std::numeric_limits<int16_t>::min();
    const double high = std::numeric_limits<int16_t>::max();
    std::vector<int16_t> result(out.size());
    for (size_t i = 0; i < out.size(); ++i)
        result[i] = static_cast<int16_t>(std::max(low, std::min(high, std::round(out[i] * gain * k_int16_scale))));
    return result;
}

// The channel simulator at the encoder's rate, the receiver's filter from the options.
std::unique_ptr<sim::Channel> make_channel(const Options& o, const EncoderConfig& config,
                                           sim::ChannelConfig& channel_config) {
    channel_config = o.channel_config;
    if (!o.fading.empty()) sim::apply_preset(channel_config, find_name(k_fadings, "--fading", o.fading).preset);
    if (o.has_doppler) channel_config.doppler_spread_hz = o.doppler_hz;
    const Passband receiver = o.has_rx_passband ? o.rx_passband : config.passband;
    channel_config.rx_low_hz = receiver.low_hz;
    channel_config.rx_high_hz = receiver.high_hz;
    if (channel_config.mode == sim::Mode::fm) channel_config.fm_audio_high_hz = receiver.high_hz;
    channel_config.sample_rate = config.sample_rate_hz;
    channel_config.signal_level = config.amplitude / k_int16_scale;
    try {
        return std::unique_ptr<sim::Channel>(new sim::Channel(channel_config));
    } catch (const std::invalid_argument& error) {
        throw UsageError(std::string("--channel: ") + error.what());
    }
}

bool play(pc::OutputDevice& device, SampleSource& source, uint32_t rate_hz) {
    return device.start(source, rate_hz) && device.wait();
}

std::unique_ptr<pc::OutputDevice> open_device(const std::string& spec, uint32_t rate_hz) {
    std::string error;
    std::unique_ptr<pc::OutputDevice> device = pc::open_output(spec, error, rate_hz);
    if (device == nullptr) std::fprintf(stderr, "%s: %s\n", k_program, error.c_str());
    return device;
}

std::string signed_hz(double hz) {
    return (hz > 0.0 ? "+" : "") + fixed(hz, 0) + " Hz";
}

// "usb, SNR 10.0 dB key-down (5.7 dB average power), offset +80 Hz, receiver filter 300-2700 Hz"
std::string channel_text(const Options& o, const sim::ChannelConfig& c, double power_ratio, double gain_db) {
    std::string text = o.channel_name;
    if (c.mode == sim::Mode::usb || c.mode == sim::Mode::lsb) {
        text += ", SNR " + fixed(c.snr_db, k_db_decimals) + " dB key-down (" +
                fixed(c.snr_db + k_power_db * std::log10(power_ratio), k_db_decimals) + " dB average power)";
    } else if (c.mode != sim::Mode::clean) {
        text += ", SNR " + fixed(c.snr_db, k_db_decimals) + " dB carrier";
    }
    if (c.mode == sim::Mode::fm)
        text += ", CNR " + fixed(sim::fm_cnr_db(c), k_db_decimals) + " dB in " + fixed(c.fm_if_bandwidth_hz, 0) + " Hz";
    if (c.mode != sim::Mode::clean) {
        text += ", offset " + signed_hz(c.freq_offset_hz);
        const double high = c.mode == sim::Mode::fm ? c.fm_audio_high_hz : c.rx_high_hz;
        text += ", receiver filter " + fixed(c.rx_low_hz, 0) + "-" + fixed(high, 0) + " Hz";
    }
    if (!o.fading.empty() && o.fading != "none") text += ", fading " + o.fading;
    return text + "; output gain " + fixed(gain_db, k_db_decimals) + " dB";
}

std::unique_ptr<pc::Tui> open_view(const Options& o, const EncoderConfig& config, const std::string& ptt,
                                   const sim::ChannelConfig& channel_config, Console& console) {
    std::unique_ptr<pc::Tui> tui(new pc::Tui(pc::TuiMode::encoder));
    if (!tui->open()) {
        std::fprintf(stderr, "%s: --tui needs a terminal on stdout; plain output\n", k_program);
        return std::unique_ptr<pc::Tui>();
    }
    tui->set_label(o.out_spec());
    tui->set_speed(unlimited::bytes_per_second(config.slot_us));
    tui->set_tone_hz(config.tone_hz);
    tui->set_slot_ms(static_cast<float>(unlimited::cli::slot_ms_of(config.slot_us)));
    tui->set_passband(config.passband);
    tui->set_search_range(unlimited::search_range(config));
    tui->set_field("rate", std::to_string(config.sample_rate_hz) + " Hz");
    if (!ptt.empty()) tui->set_field("ptt", ptt);
    if (o.channel)
        tui->set_field("channel", o.channel_name + " " + fixed(channel_config.snr_db, k_db_decimals) + " dB");
    console.hold(true);
    return tui;
}

// What run() opened and checked for send(): the encoder's configuration at the output's rate, the data, the channel
// simulator (--channel), the output, the file of --clean-out and, for a sound card, the PTT.
struct Opened {
    EncoderConfig config;
    std::vector<uint8_t> data;
    sim::ChannelConfig channel_config;
    std::unique_ptr<sim::Channel> channel;
    std::unique_ptr<pc::OutputDevice> output;
    std::unique_ptr<pc::OutputDevice> clean_output;
    std::unique_ptr<pc::Ptt> ptt;
};

// Sends the data through the open output (a file, or a sound card with its PTT) and prints the lines and the summary.
int send(const Options& o, Opened& opened) {
    const bool live = o.live();
    const EncoderConfig& config = opened.config;
    const std::vector<uint8_t>& data = opened.data;
    pc::OutputDevice& output = *opened.output;
    Console console;
    const uint32_t rate_hz = config.sample_rate_hz;
    const double power_ratio = average_power_ratio(data);
    const uint32_t duration = Encoder(config).duration_samples(data.size());
    std::string what = std::to_string(data.size()) + (data.size() == 1 ? " byte" : " bytes");
    what += o.has_text ? " of text" : " from " + o.data_path;

    std::unique_ptr<pc::Tui> tui;
    if (o.tui)
        tui = open_view(o, config, live ? pc::ptt_method_name(o.radio.ptt.method) : "", opened.channel_config, console);

    console.item("speed", unlimited::cli::speed_text(config.slot_us) + "  (the receiver needs --bps " +
                              unlimited::cli::speed_number(config.slot_us) + (o.fade_bridge ? " --fade-bridge)" : ")"));
    console.item("signal", "pitch " + std::to_string(config.tone_hz) + " Hz, one byte per window of 10 slots: " +
                               "START, 8 bits (most significant first), STOP");
    console.item("bandwidth", unlimited::cli::bandwidth_line(config));
    console.item("emission", "-26 dB width " + std::to_string(unlimited::width_26db_hz(config.slot_us)) +
                                 " Hz, -40 dB width " + std::to_string(unlimited::width_40db_hz(config.slot_us)) +
                                 " Hz");
    console.item("data", what);
    console.item("airtime", fixed(static_cast<double>(duration) / rate_hz, k_seconds_decimals) + " s: " +
                                layout_text(config, data.size()));
    const double crest_dbfs = k_amplitude_db * std::log10(config.amplitude / k_dbfs_reference);
    console.item("level", "crest " + fixed(crest_dbfs, k_db_decimals) + " dBFS; the windows' average power is " +
                              fixed(-k_power_db * std::log10(power_ratio), k_db_decimals) +
                              " dB below the key-down tone");
    if (live) {
        console.item("output", output.description());
        console.item("ptt", opened.ptt->description());
    }

    Encoder encoder(config);
    std::unique_ptr<LiveView> view;
    if (live && tui != nullptr) view.reset(new LiveView(*tui, rate_hz));
    MarkRecorder recorder;  // a file's view, and the channel simulator's first pass
    StatusSink* statuses = nullptr;
    if (tui != nullptr) statuses = view != nullptr && opened.channel == nullptr ? &view->publisher() : &recorder;
    TransmitSource transmit(encoder, data, statuses);
    if (!transmit.start()) throw UsageError("the encoder did not start");

    bool written = true;
    uint64_t samples = 0;
    double gain_db = 0.0;
    std::vector<int16_t> received;  // the channel simulator's output
    SampleSource* played = &transmit;
    std::unique_ptr<pc::MemorySource> received_source;
    if (opened.channel != nullptr) {
        pc::MemoryOutput capture;
        capture.start(transmit, rate_hz);
        const std::vector<int16_t>& clean = capture.samples();
        if (opened.clean_output != nullptr) {
            pc::MemorySource clean_source(clean);
            written = play(*opened.clean_output, clean_source, rate_hz);
        }
        received = through_channel(*opened.channel, clean, gain_db);
        received_source.reset(new pc::MemorySource(received));
        played = received_source.get();
    }
    LiveOutcome outcome;
    if (live) {
        const std::vector<StatusMark>* replay = opened.channel != nullptr && view != nullptr ? &recorder.marks : nullptr;
        LiveSource source(*played, rate_hz, view.get(), replay);
        outcome = transmit_live(output, *opened.ptt, source, rate_hz, view.get());
        written = written && outcome.drained && outcome.device_ok;
        samples = source.samples();
        console.item("audio", std::to_string(samples) + " samples at " + std::to_string(rate_hz) + " Hz -> " +
                                  output.description());
        console.item("output", pc::level_text(source.meter().total(), false) + "; " +
                                   std::to_string(output.xruns()) + (output.xruns() == 1 ? " xrun" : " xruns"));
        if (outcome.signal_number != 0 && !outcome.drained)
            console.item("stopped", "by " + unlimited::cli::signal_name(outcome.signal_number) + " after " +
                                        std::to_string(transmit.sent()) + " of " + std::to_string(data.size()) +
                                        " bytes; the PTT " + (outcome.unkeyed ? "released" : "NOT released"));
    } else {
        MonitorSource monitor(*played, rate_hz, tui.get(), &recorder.marks, o.realtime);
        written = play(output, monitor, rate_hz) && written;
        if (tui != nullptr) tui->set_level(monitor.meter().recent());
        samples = opened.channel != nullptr ? received.size() : encoder.status().samples_rendered;
        console.item("audio", std::to_string(samples) + " samples at " + std::to_string(rate_hz) + " Hz -> " +
                                  o.out_spec() +
                                  (o.clean_out.empty() ? "" : " (transmitted audio -> " + o.clean_out + ")"));
    }
    if (tui != nullptr) tui->close();
    if (o.channel) console.item("channel", channel_text(o, opened.channel_config, power_ratio, gain_db));
    console.hold(false);

    if (live) {
        const std::string ptt = opened.ptt->description();
        if (!outcome.unkeyed) {
            std::fprintf(stderr, "%s: PTT: releasing failed; the radio may still be keyed (%s)\n", k_program,
                         ptt.c_str());
            return k_exit_io;
        }
        if (!outcome.keyed) {
            std::fprintf(stderr, "%s: PTT: keying failed (%s)\n", k_program, ptt.c_str());
            return k_exit_io;
        }
        if (!outcome.started || !outcome.device_ok) {
            const std::string why = output.error();
            std::fprintf(stderr, "%s: playing through %s failed%s\n", k_program, o.out_spec().c_str(),
                         why.empty() ? "" : (": " + why).c_str());
            return k_exit_io;
        }
        if (!outcome.drained) {
            std::fprintf(stderr, "%s: the transmission was stopped before its end\n", k_program);
            return k_exit_stopped;
        }
    }
    if (!transmit.complete()) {
        std::fprintf(stderr, "%s: the transmission ended before all data was sent\n", k_program);
        return k_exit_io;
    }
    if (!written) {
        std::fprintf(stderr, "%s: writing the audio failed\n", k_program);
        return k_exit_io;
    }
    return k_exit_ok;
}

int run(const Options& o) {
    const bool live = o.live();
    Opened opened;
    // A sound card's rate is known once it is open: the rules are checked first at the default rate (every rule that
    // holds there holds at any higher rate), then at the card's.
    EncoderConfig& config = opened.config;
    config = encoder_config(o, live ? k_default_rate_hz : to_clamped<uint32_t>(o.rate_hz));
    const std::string problem = unlimited::cli::encoder_problem(config);
    if (!problem.empty()) {
        if (band_known(config))
            std::fprintf(stderr, "%s: %s\n", k_program, unlimited::cli::bandwidth_line(config).c_str());
        throw UsageError("refused: " + problem);
    }

    if (o.has_text) {
        opened.data.assign(o.text.begin(), o.text.end());
    } else if (!unlimited::cli::read_file(o.data_path, opened.data)) {
        std::fprintf(stderr, "%s: cannot read %s\n", k_program, o.data_path.c_str());
        return k_exit_io;
    }
    if (opened.data.empty()) throw UsageError("nothing to send");

    opened.channel_config = o.channel_config;
    if (o.channel) opened.channel = make_channel(o, config, opened.channel_config);

    opened.output = open_device(o.out_spec(), o.radio.rate_hz);
    if (opened.output == nullptr) return k_exit_io;
    if (!o.clean_out.empty()) {
        opened.clean_output = open_device(o.clean_out, 0);
        if (opened.clean_output == nullptr) return k_exit_io;
    }
    if (live) {
        config.sample_rate_hz = opened.output->sample_rate_hz();
        const std::string at_rate = unlimited::cli::encoder_problem(config);
        if (!at_rate.empty()) {
            std::fprintf(stderr, "%s: %s plays at %u Hz: %s\n", k_program, opened.output->description().c_str(),
                         static_cast<unsigned>(config.sample_rate_hz), at_rate.c_str());
            return k_exit_io;
        }
        if (o.channel) opened.channel = make_channel(o, config, opened.channel_config);
        std::string error;
        opened.ptt = pc::open_ptt(o.radio.ptt, error);
        if (opened.ptt == nullptr) {
            std::fprintf(stderr, "%s: PTT: %s\n", k_program, error.c_str());
            return k_exit_io;
        }
    }
    return send(o, opened);
}

void print_help(const Options& o) {
    std::printf(k_usage, static_cast<double>(unlimited::k_min_bytes_per_second),
                static_cast<double>(unlimited::k_max_bytes_per_second),
                static_cast<double>(unlimited::k_default_bytes_per_second), static_cast<unsigned>(k_keyed_lead_in_ms),
                static_cast<unsigned>(unlimited::k_default_vox_lead_ms),
                static_cast<unsigned>(unlimited::k_default_tail_ms),
                static_cast<unsigned>(unlimited::cli::k_fade_bridge_silence_ms));
    std::fputs(o.radio.help().c_str(), stdout);
    std::fputs(k_usage_end, stdout);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) {
            print_help(options);
            return k_exit_ok;
        }
        if (options.radio.list_devices) {
            pc::print_devices(stdout);
            return k_exit_ok;
        }
        return run(options);
    } catch (const UsageError& error) {
        std::fprintf(stderr, "%s: %s (see --help)\n", k_program, error.what());
        return k_exit_usage;
    }
}
