#include "audio.hpp"
#include "channel.hpp"
#include "cli.hpp"
#include "tui.hpp"
#include "unlimited/audio_io.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"
#include "unlimited/packet.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
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
using unlimited::Preset;
using unlimited::SampleSource;
using unlimited::cli::Arguments;
using unlimited::cli::Console;
using unlimited::cli::UsageError;
using unlimited::cli::fixed;
using unlimited::cli::find_name;
using unlimited::cli::k_exit_io;
using unlimited::cli::k_exit_ok;
using unlimited::cli::k_exit_usage;
using unlimited::cli::k_presets;
using unlimited::cli::k_profiles;
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
const char* const k_default_preset = "hf";

const uint32_t k_default_rate_hz = 8000;
const double k_default_level_dbfs = -3.0;
const double k_dbfs_reference = 32767.0;  // amplitude of 0 dBFS
const double k_int16_scale = 32768.0;     // int16 <-> float in [-1, 1)
const double k_output_peak_dbfs = -1.0;   // a louder channel output is scaled down to this peak
const double k_amplitude_db = 20.0;
const double k_power_db = 10.0;
const double k_us_per_s = 1e6;
const uint32_t k_us_per_ms = 1000;

// Samples per Encoder::render() call. A slot has at least 32 samples, so the queue is refilled several
// times per package (it never runs dry while data remains) and every slot is seen by the TUI.
const size_t k_render_step = 16;

const int k_db_decimals = 1;
const int k_seconds_decimals = 3;
const int k_rate_decimals = 1;

const char* const k_usage =
    "usage: unlimited_encode (--text STR | --in FILE) [--out SPEC] [--packet]\n"
    "    [--preset hf_slow|hf|hf_fast|am|fm] [--slot-ms X | --baud B] [--bits N]\n"
    "    [--tone HZ] [--passband LO:HI] [--rate 8000] [--level-dbfs -3] [--lead-in-ms N] [--tune-ms N] [--sync N]\n"
    "    [--channel clean|usb|lsb|am|fm [--snr DB] [--offset HZ] [--pivot HZ] [--rx-passband LO:HI]\n"
    "        [--fading none|flat|good|moderate|poor|flutter] [--doppler HZ] [--qsb DEPTH_DB:RATE_HZ]\n"
    "        [--impulses RATE[:LEVEL_DB]] [--carrier HZ:DB] [--cw HZ:DB:WPM] [--agc]\n"
    "        [--fm-deviation HZ] [--no-preemphasis] [--no-deemphasis] [--clock-ppm P] [--seed N]\n"
    "        [--clean-out SPEC]]\n"
    "    [--tui] [--realtime]\n"
    "\n"
    "Sends data through a radio's audio as short beeps on one pitch: a beep in a time slot is a 1, silence is\n"
    "a 0. START/STOP markers (the same beep with an inaudible twist) frame every package of N bits and tell\n"
    "the receiver the timing and how loud a 1 is. The receiver finds the pitch, the slot length and N itself.\n"
    "\n"
    "What to send\n"
    "  --text STR, --in FILE   the data: a text, or the bytes of a file\n"
    "  --packet                wrap the data in CRC-16 packets of up to %u bytes, so the receiver can check it\n"
    "  --out SPEC              where the audio goes: wav:<path>, <path>.wav or null (default tx.wav)\n"
    "\n"
    "The signal: start from a preset and change what you need\n"
    "  --preset NAME           hf_slow (32 ms slots), hf (16 ms, the default), hf_fast (8 ms): 8 bits per\n"
    "                          package, for HF SSB; am (8 ms) and fm (4 ms): 16 bits per package\n"
    "  --slot-ms X, --baud B   the slot length T, 4..128 ms (decimals allowed), or slots per second (1000/T).\n"
    "                          Longer slots are slower but survive more noise and fading\n"
    "  --bits N, -N N          bits per package between START and STOP, 1..%u (also --bits-per-package)\n"
    "  --tone HZ               the pitch, 300..2700 Hz (default 1500)\n"
    "  --passband LO:HI        the receiver's audio filter the signal must fit (default 300:2700, a 2.4 kHz\n"
    "                          SSB filter; 300:2100 is a 1.8 kHz one). A signal that does not fit is refused\n"
    "  --rate HZ               audio sample rate, 8000..192000 (default 8000)\n"
    "  --level-dbfs DB         loudness of a beep's crest (default -3)\n"
    "  --lead-in-ms N          silence before the signal, for the PTT and the transmitter to settle\n"
    "  --tune-ms N, --sync N   length of the tune tone (default 250 ms), markers in the sync train (8..32)\n"
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
    "  --tui                   live view: the beep being sent, its package and byte, scope and spectrum\n"
    "  --realtime              pace the output to audio time\n"
    "\n"
    "Every run prints the occupied bandwidth, whether it fits the passband, and how far the radio may be\n"
    "mistuned (the shift tolerance).\n"
    "exit codes: 0 written, 2 usage error or refused configuration, 3 input/output error\n";

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
    std::string out_spec = k_default_out_spec;
    bool packet = false;

    std::string preset = k_default_preset;
    bool has_slot_ms = false;
    double slot_ms = 0.0;
    bool has_baud = false;
    double baud = 0.0;
    bool has_bits = false;
    double bits = 0.0;
    bool has_tone = false;
    double tone_hz = 0.0;
    bool has_passband = false;
    Passband passband = Passband();
    double rate_hz = k_default_rate_hz;
    double level_dbfs = k_default_level_dbfs;
    bool has_lead_in = false;
    double lead_in_ms = 0.0;
    bool has_tune = false;
    double tune_ms = 0.0;
    bool has_sync = false;
    double sync = 0.0;

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

Options parse_options(int argc, char** argv) {
    Options o;
    Arguments args(argc, argv);
    std::string option;
    while (args.next(option)) {
        if (option == "--help" || option == "-h") {
            o.help = true;
        } else if (option == "--text") {
            o.has_text = true;
            o.text = args.value(option);
        } else if (option == "--in") {
            o.data_path = args.value(option);
        } else if (option == "--out") {
            o.out_spec = args.value(option);
        } else if (option == "--packet") {
            o.packet = true;
        } else if (option == "--preset") {
            o.preset = args.value(option);
            find_name(k_presets, option, o.preset);
        } else if (option == "--slot-ms") {
            o.has_slot_ms = true;
            o.slot_ms = to_number(option, args.value(option));
        } else if (option == "--baud") {
            o.has_baud = true;
            o.baud = to_number(option, args.value(option));
        } else if (option == "--bits" || option == "--bits-per-package" || option == "-N") {
            o.has_bits = true;
            o.bits = to_number(option, args.value(option));
        } else if (option == "--tone") {
            o.has_tone = true;
            o.tone_hz = to_number(option, args.value(option));
        } else if (option == "--passband") {
            o.has_passband = true;
            o.passband = to_passband(option, args.value(option));
        } else if (option == "--rate") {
            o.rate_hz = to_number(option, args.value(option));
        } else if (option == "--level-dbfs") {
            o.level_dbfs = to_number(option, args.value(option));
        } else if (option == "--lead-in-ms") {
            o.has_lead_in = true;
            o.lead_in_ms = to_number(option, args.value(option));
        } else if (option == "--tune-ms") {
            o.has_tune = true;
            o.tune_ms = to_number(option, args.value(option));
        } else if (option == "--sync") {
            o.has_sync = true;
            o.sync = to_number(option, args.value(option));
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
    if (o.help) return o;
    if (o.has_text == !o.data_path.empty()) throw UsageError("give exactly one of --text and --in");
    if (o.has_slot_ms && o.has_baud) throw UsageError("give at most one of --slot-ms and --baud");
    if (!o.channel && !o.channel_option.empty()) throw UsageError(o.channel_option + " needs --channel");
    if (o.has_doppler && (o.fading.empty() || o.fading == "none")) throw UsageError("--doppler needs --fading");
    return o;
}

bool changed_signal(const Options& o) {
    return o.has_slot_ms || o.has_baud || o.has_bits || o.has_tone || o.has_passband;
}

// The preset with the options applied. Values far out of range are clamped into the field, so that check() names
// the rule they break.
EncoderConfig encoder_config(const Options& o) {
    const uint32_t rate_hz = to_clamped<uint32_t>(o.rate_hz);
    EncoderConfig config = EncoderConfig::from_preset(find_name(k_presets, "--preset", o.preset).value, rate_hz);
    if (o.has_slot_ms) config.slot_us = to_clamped<uint32_t>(o.slot_ms * k_us_per_ms);
    if (o.has_baud) {
        if (!(o.baud > 0.0)) throw UsageError("--baud must be above 0");
        config.slot_us = to_clamped<uint32_t>(k_us_per_s / o.baud);
    }
    if (o.has_bits) config.bits_per_package = to_clamped<uint8_t>(o.bits);
    if (o.has_tone) config.tone_hz = to_clamped<uint16_t>(o.tone_hz);
    if (o.has_passband) config.passband = o.passband;
    if (o.level_dbfs > 0.0) throw UsageError("--level-dbfs must be 0 or below (0 dBFS is full scale)");
    config.amplitude = to_clamped<int16_t>(k_dbfs_reference * std::pow(10.0, o.level_dbfs / k_amplitude_db));
    if (o.has_lead_in) config.lead_in_ms = to_integer<uint16_t>("--lead-in-ms", o.lead_in_ms);
    if (o.has_tune) config.tune_ms = to_integer<uint16_t>("--tune-ms", o.tune_ms);
    if (o.has_sync) config.sync_markers = to_clamped<uint8_t>(o.sync);
    return config;
}

// "preset hf", or "custom, from preset hf" when the signal was changed.
std::string preset_text(const Options& o) {
    return (changed_signal(o) ? "custom, from preset " : "preset ") + o.preset;
}

// The pitch and T are in range, so the band means something.
bool band_known(const EncoderConfig& config) {
    return config.tone_hz >= unlimited::k_min_tone_hz && config.tone_hz <= unlimited::k_max_tone_hz &&
           config.slot_us >= unlimited::k_min_slot_us && config.slot_us <= unlimited::k_max_slot_us;
}

// The receiver profiles whose window holds T and whose tone search holds the pitch (spec 1.7 "heard by").
std::string heard_by(const EncoderConfig& config) {
    std::vector<std::string> names;
    for (size_t i = 0; i < sizeof(k_profiles) / sizeof(k_profiles[0]); ++i) {
        const DecoderConfig receiver = DecoderConfig::for_profile(k_profiles[i].value);
        const Passband search = receiver.search_range();
        const bool window = config.slot_us >= receiver.min_slot_ms * k_us_per_ms &&
                            config.slot_us <= receiver.max_slot_ms() * k_us_per_ms;
        if (window && config.tone_hz >= search.low_hz && config.tone_hz <= search.high_hz)
            names.push_back(k_profiles[i].name);
    }
    if (names.empty()) {
        const uint32_t needed = (config.slot_us + unlimited::k_speed_span * k_us_per_ms - 1) /
                                (unlimited::k_speed_span * k_us_per_ms);
        const uint32_t min_slot_ms = std::max<uint32_t>(unlimited::k_min_window_slot_ms,
                                                        std::min<uint32_t>(unlimited::k_max_window_slot_ms, needed));
        return "no receiver profile as it stands: give the receiver --min-slot-ms " + std::to_string(min_slot_ms) +
               " and a --passband that holds the band";
    }
    std::string text = "receiver profile" + std::string(names.size() > 1 ? "s " : " ");
    for (size_t i = 0; i < names.size(); ++i)
        text += (i == 0 ? "" : (i + 1 == names.size() ? " and " : ", ")) + names[i];
    return "heard by the " + text;
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

int bit_count(uint8_t value) {
    int count = 0;
    for (unsigned v = value; v != 0; v &= v - 1) ++count;
    return count;
}

size_t package_count(const EncoderConfig& config, size_t bytes) {
    const size_t bits = bytes * unlimited::k_bits_per_byte;
    return (bits + config.bits_per_package - 1) / config.bits_per_package;
}

// Average power of the packages over the key-down power (spec 1.3): each data 1 and each STOP by its slot energy.
double average_power_ratio(const EncoderConfig& config, const std::vector<uint8_t>& data) {
    size_t ones = 0;
    for (size_t i = 0; i < data.size(); ++i) ones += static_cast<size_t>(bit_count(data[i]));
    const double packages = static_cast<double>(package_count(config, data.size()));
    const double bits = static_cast<double>(data.size() * unlimited::k_bits_per_byte);
    return (ones * unlimited::k_one_energy + packages * unlimited::k_marker_energy) / (bits + packages);
}

size_t tune_slots(const EncoderConfig& config) {
    const size_t slots = (static_cast<size_t>(config.tune_ms) * k_us_per_ms + config.slot_us - 1) / config.slot_us;
    return std::max<size_t>(slots, unlimited::k_min_tune_slots);
}

std::string count_text(size_t count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// "34 packages of 8 bits", "5 packages of 3 bits and a last one of 1".
std::string packages_text(const EncoderConfig& config, size_t bytes) {
    const size_t packages = package_count(config, bytes);
    const size_t last = bytes * unlimited::k_bits_per_byte - (packages - 1) * config.bits_per_package;
    if (last == config.bits_per_package)
        return count_text(packages, "package", "packages") + " of " + std::to_string(last) + " bits";
    if (packages == 1) return "1 package of " + std::to_string(last) + " bits";
    return count_text(packages - 1, "package", "packages") + " of " + std::to_string(config.bits_per_package) +
           " bits and a last one of " + std::to_string(last);
}

// ---------------------------------------------------------------------------
// Audio path
// ---------------------------------------------------------------------------

struct StatusMark {
    uint64_t sample;  // samples rendered when the status was read
    EncoderStatus status;
};

// Renders the transmission, writing the data into the encoder queue as it drains, and optionally records
// the encoder status once per slot (for the TUI).
class TransmitSource final : public SampleSource {
public:
    TransmitSource(Encoder& encoder, const std::vector<uint8_t>& data, std::vector<StatusMark>* marks)
        : encoder_(encoder), data_(data), marks_(marks), written_(0), rendered_(0) {}

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

    bool complete() const { return written_ == data_.size() && !encoder_.busy(); }

private:
    void refill() {
        if (written_ < data_.size()) written_ += encoder_.write(&data_[written_], data_.size() - written_);
    }

    void record() {
        if (marks_ == nullptr) return;
        const EncoderStatus status = encoder_.status();
        if (!marks_->empty()) {
            const EncoderStatus& last = marks_->back().status;
            if (last.slot_index == status.slot_index && last.segment == status.segment) return;
        }
        const StatusMark mark = {rendered_, status};
        marks_->push_back(mark);
    }

    Encoder& encoder_;
    const std::vector<uint8_t>& data_;
    std::vector<StatusMark>* marks_;
    size_t written_;
    uint64_t rendered_;
};

// Watches the audio on its way to the output device: feeds the TUI and paces the output to real time.
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
            if (refresh_.due()) tui_->draw();
        }
        if (realtime_) pacer_.advance(samples);
        return samples;
    }

private:
    SampleSource& source_;
    uint32_t rate_hz_;
    pc::Tui* tui_;
    const std::vector<StatusMark>* marks_;
    size_t next_mark_;
    uint64_t position_;
    bool realtime_;
    pc::RealtimePacer pacer_;
    pc::RefreshPacer refresh_;
};

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

bool play(pc::OutputDevice& device, SampleSource& source, uint32_t rate_hz) {
    return device.start(source, rate_hz) && device.wait();
}

std::unique_ptr<pc::OutputDevice> open_device(const std::string& spec) {
    std::string error;
    std::unique_ptr<pc::OutputDevice> device = pc::open_output(spec, error);
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

int run(const Options& o) {
    const EncoderConfig config = encoder_config(o);
    const std::string problem = unlimited::cli::encoder_problem(config);
    if (!problem.empty()) {
        if (band_known(config))
            std::fprintf(stderr, "%s: %s\n", k_program, unlimited::cli::bandwidth_line(config).c_str());
        throw UsageError("refused: " + problem);
    }

    std::vector<uint8_t> payload;
    if (o.has_text) {
        payload.assign(o.text.begin(), o.text.end());
    } else if (!unlimited::cli::read_file(o.data_path, payload)) {
        std::fprintf(stderr, "%s: cannot read %s\n", k_program, o.data_path.c_str());
        return k_exit_io;
    }
    if (payload.empty()) throw UsageError("nothing to send");
    std::vector<uint8_t> data = payload;
    const size_t packets = o.packet ? unlimited::cli::packetize(payload, data) : 0;

    sim::ChannelConfig channel_config = o.channel_config;
    std::unique_ptr<sim::Channel> channel;
    if (o.channel) {
        if (!o.fading.empty()) sim::apply_preset(channel_config, find_name(k_fadings, "--fading", o.fading).preset);
        if (o.has_doppler) channel_config.doppler_spread_hz = o.doppler_hz;
        const Passband receiver = o.has_rx_passband ? o.rx_passband : config.passband;
        channel_config.rx_low_hz = receiver.low_hz;
        channel_config.rx_high_hz = receiver.high_hz;
        if (channel_config.mode == sim::Mode::fm) channel_config.fm_audio_high_hz = receiver.high_hz;
        channel_config.sample_rate = config.sample_rate_hz;
        channel_config.signal_level = config.amplitude / k_int16_scale;
        try {
            channel.reset(new sim::Channel(channel_config));
        } catch (const std::invalid_argument& error) {
            throw UsageError(std::string("--channel: ") + error.what());
        }
    }

    std::unique_ptr<pc::OutputDevice> output = open_device(o.out_spec);
    if (output == nullptr) return k_exit_io;
    std::unique_ptr<pc::OutputDevice> clean_output;
    if (!o.clean_out.empty()) {
        clean_output = open_device(o.clean_out);
        if (clean_output == nullptr) return k_exit_io;
    }

    Console console;
    const double slot_ms = unlimited::cli::slot_ms_of(config.slot_us);
    const double power_ratio = average_power_ratio(config, data);
    const uint32_t duration = Encoder(config).duration_samples(data.size());
    std::string what = std::to_string(payload.size()) + (payload.size() == 1 ? " byte" : " bytes");
    what += o.has_text ? " of text" : " from " + o.data_path;
    if (o.packet)
        what += " in " + std::to_string(packets) + (packets == 1 ? " packet (" : " packets (") +
                std::to_string(data.size()) + " bytes with the framing)";

    std::unique_ptr<pc::Tui> tui;
    if (o.tui) {
        tui.reset(new pc::Tui(pc::TuiMode::encoder));
        if (tui->open()) {
            tui->set_profile(changed_signal(o) ? "custom" : o.preset);
            tui->set_tone_hz(config.tone_hz);
            tui->set_slot_ms(static_cast<float>(slot_ms));
            tui->set_package(config.bits_per_package);
            tui->set_passband(config.passband);
            tui->set_search_range(unlimited::search_range(config));
            tui->set_field("rate", std::to_string(config.sample_rate_hz) + " Hz");
            if (o.channel)
                tui->set_field("channel", o.channel_name + " " + fixed(channel_config.snr_db, k_db_decimals) + " dB");
            console.hold(true);
        } else {
            std::fprintf(stderr, "%s: --tui needs a terminal on stdout; plain output\n", k_program);
            tui.reset();
        }
    }

    console.item("data", what);
    console.item("signal", preset_text(o) + ": pitch " + std::to_string(config.tone_hz) + " Hz, slot T " +
                               ms_text(slot_ms) + " ms (" +
                               unlimited::cli::trimmed(unlimited::cli::k_ms_per_s / slot_ms,
                                                       unlimited::cli::k_baud_decimals) +
                               " baud), N " + std::to_string(config.bits_per_package) + " bits per package, " +
                               fixed(unlimited::cli::net_bit_rate(config.bits_per_package, slot_ms), k_rate_decimals) +
                               " bit/s net");
    console.item("bandwidth", unlimited::cli::bandwidth_line(config));
    console.item("emission", "-26 dB width " + std::to_string(unlimited::width_26db_hz(config.slot_us)) +
                                 " Hz, -40 dB width " + std::to_string(unlimited::width_40db_hz(config.slot_us)) +
                                 " Hz");
    console.item("receivers", heard_by(config));
    console.item("airtime", fixed(static_cast<double>(duration) / config.sample_rate_hz, k_seconds_decimals) +
                                " s: lead-in " + std::to_string(config.lead_in_ms) + " ms, tune tone " +
                                std::to_string(tune_slots(config)) + " slots, sync " +
                                std::to_string(config.sync_markers) + " markers, " +
                                packages_text(config, data.size()) + ", END, tail " + std::to_string(config.tail_ms) +
                                " ms");
    const double crest_dbfs = k_amplitude_db * std::log10(config.amplitude / k_dbfs_reference);
    console.item("level", "crest " + fixed(crest_dbfs, k_db_decimals) + " dBFS; the packages' average power is " +
                              fixed(-k_power_db * std::log10(power_ratio), k_db_decimals) +
                              " dB below the key-down tone");

    std::vector<StatusMark> marks;
    std::vector<StatusMark>* recorded = tui != nullptr ? &marks : nullptr;
    Encoder encoder(config);
    TransmitSource transmit(encoder, data, recorded);
    if (!transmit.start()) throw UsageError("the encoder did not start");

    bool written = true;
    size_t samples = 0;
    double gain_db = 0.0;
    if (channel == nullptr) {
        MonitorSource monitor(transmit, config.sample_rate_hz, tui.get(), recorded, o.realtime);
        written = play(*output, monitor, config.sample_rate_hz);
        samples = encoder.status().samples_rendered;
    } else {
        pc::MemoryOutput capture;
        capture.start(transmit, config.sample_rate_hz);
        const std::vector<int16_t>& clean = capture.samples();
        if (clean_output != nullptr) {
            pc::MemorySource clean_source(clean);
            written = play(*clean_output, clean_source, config.sample_rate_hz);
        }
        const std::vector<int16_t> received = through_channel(*channel, clean, gain_db);
        pc::MemorySource received_source(received);
        MonitorSource monitor(received_source, config.sample_rate_hz, tui.get(), recorded, o.realtime);
        written = play(*output, monitor, config.sample_rate_hz) && written;
        samples = received.size();
    }
    if (tui != nullptr) tui->close();

    console.item("audio", std::to_string(samples) + " samples at " + std::to_string(config.sample_rate_hz) + " Hz -> " +
                              o.out_spec + (o.clean_out.empty() ? "" : " (transmitted audio -> " + o.clean_out + ")"));
    if (o.channel) console.item("channel", channel_text(o, channel_config, power_ratio, gain_db));
    console.hold(false);

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

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) {
            std::printf(k_usage, static_cast<unsigned>(unlimited::k_packet_max_payload),
                        static_cast<unsigned>(unlimited::k_max_bits_per_package));
            return k_exit_ok;
        }
        return run(options);
    } catch (const UsageError& error) {
        std::fprintf(stderr, "%s: %s (see --help)\n", k_program, error.what());
        return k_exit_usage;
    }
}
