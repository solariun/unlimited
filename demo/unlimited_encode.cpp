#include "audio.hpp"
#include "channel.hpp"
#include "cli.hpp"
#include "tui.hpp"
#include "unlimited/audio_io.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

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

const uint32_t k_default_rate_hz = 8000;
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

const int k_db_decimals = 1;
const int k_seconds_decimals = 3;

const char* const k_usage =
    "usage: unlimited_encode (--text STR | --in FILE) [--out SPEC] [--bps B]\n"
    "    [--tone HZ] [--passband LO:HI] [--rate 8000] [--level-dbfs -3] [--lead-in-ms N] [--vox-lead-ms N]\n"
    "    [--tail-ms N]\n"
    "    [--channel clean|usb|lsb|am|fm [--snr DB] [--offset HZ] [--pivot HZ] [--rx-passband LO:HI]\n"
    "        [--fading none|flat|good|moderate|poor|flutter] [--doppler HZ] [--qsb DEPTH_DB:RATE_HZ]\n"
    "        [--impulses RATE[:LEVEL_DB]] [--carrier HZ:DB] [--cw HZ:DB:WPM] [--agc]\n"
    "        [--fm-deviation HZ] [--no-preemphasis] [--no-deemphasis] [--clock-ppm P] [--seed N]\n"
    "        [--clean-out SPEC]]\n"
    "    [--tui] [--realtime]\n"
    "\n"
    "Sends bytes through a radio's audio as short beeps on one pitch, like a serial port: every byte is a window\n"
    "of 10 time slots, a START tone, its 8 bits (a beep is a 1, silence is a 0, most significant bit first) and\n"
    "a STOP tone. The receiver must be given the same speed (--bps); it finds the pitch by itself.\n"
    "\n"
    "What to send\n"
    "  --text STR, --in FILE   the data: a text, or the bytes of a file\n"
    "  --out SPEC              where the audio goes: wav:<path>, <path>.wav or null (default tx.wav)\n"
    "\n"
    "The signal\n"
    "  --bps B                 the speed in bytes per second, %.2f..%.2f in steps of 0.01 (default %.2f, for HF\n"
    "                          SSB): the slot is T = 1 / (10 B). Slower is narrower and survives more noise\n"
    "  --tone HZ               the pitch, 300..2700 Hz (default 1500)\n"
    "  --passband LO:HI        the receiver's audio filter the signal must fit (default 300:2700, a 2.4 kHz\n"
    "                          SSB filter; 300:2100 is a 1.8 kHz one). A signal that does not fit is refused\n"
    "  --rate HZ               audio sample rate, 8000..192000 (default 8000)\n"
    "  --level-dbfs DB         loudness of a beep's crest (default -3)\n"
    "  --lead-in-ms N          silence before the signal, for the PTT and the transmitter to settle (default 0)\n"
    "  --vox-lead-ms N         a steady tone of N ms (at least 3 slots) and 2 silent slots before the first\n"
    "                          byte, to key a VOX radio (default 0: none; %u is typical)\n"
    "  --tail-ms N             silence after the last byte, at least 2 slots (default %u)\n"
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
    "  --tui                   live view: the window being sent, its bits and byte, scope and spectrum\n"
    "  --realtime              pace the output to audio time\n"
    "\n"
    "Every run prints the speed first, then the occupied bandwidth, whether it fits the passband, and how far the\n"
    "radio may be mistuned (the shift tolerance).\n"
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

    uint32_t slot_us = unlimited::slot_us_for_centi_speed(unlimited::k_default_centi_bytes_per_second);
    bool has_tone = false;
    double tone_hz = 0.0;
    bool has_passband = false;
    Passband passband = Passband();
    double rate_hz = k_default_rate_hz;
    double level_dbfs = k_default_level_dbfs;
    bool has_lead_in = false;
    double lead_in_ms = 0.0;
    bool has_vox_lead = false;
    double vox_lead_ms = 0.0;
    bool has_tail = false;
    double tail_ms = 0.0;

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
        } else if (option == "--bps") {
            o.slot_us = unlimited::cli::to_slot_us(option, args.value(option));
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
        } else if (option == "--vox-lead-ms") {
            o.has_vox_lead = true;
            o.vox_lead_ms = to_number(option, args.value(option));
        } else if (option == "--tail-ms") {
            o.has_tail = true;
            o.tail_ms = to_number(option, args.value(option));
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
    if (!o.channel && !o.channel_option.empty()) throw UsageError(o.channel_option + " needs --channel");
    if (o.has_doppler && (o.fading.empty() || o.fading == "none")) throw UsageError("--doppler needs --fading");
    return o;
}

// The options applied to the library's defaults. Values far out of range are clamped into the field, so that
// check() names the rule they break.
EncoderConfig encoder_config(const Options& o) {
    EncoderConfig config;
    config.sample_rate_hz = to_clamped<uint32_t>(o.rate_hz);
    config.slot_us = o.slot_us;
    if (o.has_tone) config.tone_hz = to_clamped<uint16_t>(o.tone_hz);
    if (o.has_passband) config.passband = o.passband;
    if (o.level_dbfs > 0.0) throw UsageError("--level-dbfs must be 0 or below (0 dBFS is full scale)");
    config.amplitude = to_clamped<int16_t>(k_dbfs_reference * std::pow(10.0, o.level_dbfs / k_amplitude_db));
    if (o.has_lead_in) config.lead_in_ms = to_integer<uint16_t>("--lead-in-ms", o.lead_in_ms);
    if (o.has_vox_lead) config.vox_lead_ms = to_integer<uint16_t>("--vox-lead-ms", o.vox_lead_ms);
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

    std::vector<uint8_t> data;
    if (o.has_text) {
        data.assign(o.text.begin(), o.text.end());
    } else if (!unlimited::cli::read_file(o.data_path, data)) {
        std::fprintf(stderr, "%s: cannot read %s\n", k_program, o.data_path.c_str());
        return k_exit_io;
    }
    if (data.empty()) throw UsageError("nothing to send");

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
    const double power_ratio = average_power_ratio(data);
    const uint32_t duration = Encoder(config).duration_samples(data.size());
    std::string what = std::to_string(data.size()) + (data.size() == 1 ? " byte" : " bytes");
    what += o.has_text ? " of text" : " from " + o.data_path;

    std::unique_ptr<pc::Tui> tui;
    if (o.tui) {
        tui.reset(new pc::Tui(pc::TuiMode::encoder));
        if (tui->open()) {
            tui->set_label(o.out_spec);
            tui->set_speed(unlimited::bytes_per_second(config.slot_us));
            tui->set_tone_hz(config.tone_hz);
            tui->set_slot_ms(static_cast<float>(slot_ms));
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

    console.item("speed", unlimited::cli::speed_text(config.slot_us) + "  (the receiver needs --bps " +
                              unlimited::cli::speed_number(config.slot_us) + ")");
    console.item("signal", "pitch " + std::to_string(config.tone_hz) + " Hz, one byte per window of 10 slots: " +
                               "START, 8 bits (most significant first), STOP");
    console.item("bandwidth", unlimited::cli::bandwidth_line(config));
    console.item("emission", "-26 dB width " + std::to_string(unlimited::width_26db_hz(config.slot_us)) +
                                 " Hz, -40 dB width " + std::to_string(unlimited::width_40db_hz(config.slot_us)) +
                                 " Hz");
    console.item("data", what);
    console.item("airtime", fixed(static_cast<double>(duration) / config.sample_rate_hz, k_seconds_decimals) +
                                " s: " + layout_text(config, data.size()));
    const double crest_dbfs = k_amplitude_db * std::log10(config.amplitude / k_dbfs_reference);
    console.item("level", "crest " + fixed(crest_dbfs, k_db_decimals) + " dBFS; the windows' average power is " +
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
            std::printf(k_usage, static_cast<double>(unlimited::k_min_bytes_per_second),
                        static_cast<double>(unlimited::k_max_bytes_per_second),
                        static_cast<double>(unlimited::k_default_bytes_per_second),
                        static_cast<unsigned>(unlimited::k_default_vox_lead_ms),
                        static_cast<unsigned>(unlimited::k_default_tail_ms));
            return k_exit_ok;
        }
        return run(options);
    } catch (const UsageError& error) {
        std::fprintf(stderr, "%s: %s (see --help)\n", k_program, error.what());
        return k_exit_usage;
    }
}
