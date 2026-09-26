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
#include <iterator>
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
using unlimited::DecoderConfig;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderStatus;
using unlimited::GridSide;
using unlimited::Preset;
using unlimited::SampleSource;
using unlimited::Spacing;
using unlimited::cli::Arguments;
using unlimited::cli::UsageError;
using unlimited::cli::find_name;
using unlimited::cli::band_hz;
using unlimited::cli::config_problem;
using unlimited::cli::fixed;
using unlimited::cli::k_exit_io;
using unlimited::cli::k_exit_ok;
using unlimited::cli::k_exit_usage;
using unlimited::cli::k_presets;
using unlimited::cli::k_profiles;
using unlimited::cli::k_sides;
using unlimited::cli::k_spacings;
using unlimited::cli::mode_text;
using unlimited::cli::side_name;
using unlimited::cli::span_hz;
using unlimited::cli::tone_offset_hz;
using unlimited::cli::to_fields;
using unlimited::cli::to_integer;
using unlimited::cli::to_number;

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
const double k_us_per_ms = 1e3;
const double k_ms_per_s = 1e3;
const uint32_t k_us_per_ms_int = 1000;

// Samples per Encoder::render() call. A slot has at least 32 samples, so the queue is refilled several
// times per frame (it never runs dry while data remains) and every slot is seen by the TUI.
const size_t k_render_step = 16;

const int k_us_decimals = 3;  // a T in ms, to the µs
const int k_db_decimals = 1;
const int k_hz_decimals = 0;
const int k_tone_decimals = 1;
const int k_power_decimals = 2;
const int k_seconds_decimals = 3;

const char* const k_usage =
    "usage: unlimited_encode (--text STR | --in FILE) [--out SPEC] [--packet]\n"
    "    [--profile ssb|am|fm] [--preset hf|hf_fast|hf_robust|hf_weak|fm|fm_fast] [--slot-ms X | --baud B]\n"
    "    [--bits-per-peak|-k K] [--data-slots|-N 8|16|32] [--spacing standard|dense] [--side above|below]\n"
    "    [--tone HZ] [--rate 8000] [--level-dbfs -3] [--lead-in-ms N] [--tune-ms N] [--sync N]\n"
    "    [--channel clean|usb|lsb|am|fm [--snr DB] [--offset HZ] [--pivot HZ]\n"
    "        [--fading none|flat|good|moderate|poor|flutter] [--doppler HZ] [--qsb DEPTH_DB:RATE_HZ]\n"
    "        [--impulses RATE[:LEVEL_DB]] [--carrier HZ:DB] [--cw HZ:DB:WPM] [--agc]\n"
    "        [--fm-deviation HZ] [--no-preemphasis] [--no-deemphasis] [--clock-ppm P] [--seed N]\n"
    "        [--clean-out SPEC]]\n"
    "    [--tui] [--realtime]\n"
    "\n"
    "Encodes data as Unlimited tone peaks and writes the audio to an output device.\n"
    "  --text STR, --in FILE   data to send (text, or the bytes of a file)\n"
    "  --out SPEC              wav:<path>, <path>.wav or null (default tx.wav)\n"
    "  --packet                frame the data as CRC-16 packets of up to %u bytes\n"
    "  --profile               receiver profile; picks the preset (ssb -> hf, am -> hf, fm -> fm)\n"
    "  --preset                mode: T, bits per peak k, data slots N, spacing, side and f_ref\n"
    "  --slot-ms, --baud       slot T, a whole number of ms (6..128)\n"
    "  --bits-per-peak, -k     k = 1..8: each peak is one of 2^k tones (also --bits)\n"
    "  --data-slots, -N        peaks per START/STOP frame, (N + 1) T <= 1152 ms (also --slots)\n"
    "  --spacing, --side       tone spacing 8/(7T) or 1/T; grid above or below f_ref\n"
    "  --tone HZ               f_ref (tune tone and markers). Without it a changed mode keeps the preset's\n"
    "                          placement: fm presets 2650 Hz with the grid below, otherwise the band centred\n"
    "                          on 1500 Hz\n"
    "  --channel MODE          pass the audio through the channel simulator; --out gets the received\n"
    "                          audio, --clean-out the transmitted one\n"
    "  --tui                   terminal view of the transmission; --realtime paces it to audio time\n"
    "exit codes: 0 written, 2 usage error, 3 input/output error\n";

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

    std::string profile = "ssb";
    std::string preset;  // empty: the profile's default
    bool has_slot_ms = false;
    double slot_ms = 0.0;
    bool has_baud = false;
    double baud = 0.0;
    bool has_bits = false;
    double bits_per_peak = 0.0;
    bool has_slots = false;
    double data_slots = 0.0;
    std::string spacing;  // empty: the preset's
    std::string side;
    bool has_tone = false;
    double tone_hz = 0.0;
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
        } else if (option == "--profile") {
            o.profile = args.value(option);
            find_name(k_profiles, option, o.profile);
        } else if (option == "--preset") {
            o.preset = args.value(option);
            find_name(k_presets, option, o.preset);
        } else if (option == "--slot-ms") {
            o.has_slot_ms = true;
            o.slot_ms = to_number(option, args.value(option));
        } else if (option == "--baud") {
            o.has_baud = true;
            o.baud = to_number(option, args.value(option));
        } else if (option == "--bits-per-peak" || option == "-k" || option == "--bits") {
            o.has_bits = true;
            o.bits_per_peak = to_number(option, args.value(option));
        } else if (option == "--data-slots" || option == "-N" || option == "--slots") {
            o.has_slots = true;
            o.data_slots = to_number(option, args.value(option));
        } else if (option == "--spacing") {
            o.spacing = args.value(option);
            find_name(k_spacings, option, o.spacing);
        } else if (option == "--side") {
            o.side = args.value(option);
            find_name(k_sides, option, o.side);
        } else if (option == "--tone") {
            o.has_tone = true;
            o.tone_hz = to_number(option, args.value(option));
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

bool fm_preset(Preset preset) {
    return preset == Preset::fm || preset == Preset::fm_fast;
}

Preset base_preset(const Options& o) {
    return o.preset.empty() ? find_name(k_profiles, "--profile", o.profile).preset
                            : find_name(k_presets, "--preset", o.preset).preset;
}

bool mode_changed(const Options& o) {
    return o.has_slot_ms || o.has_baud || o.has_bits || o.has_slots || !o.spacing.empty() || !o.side.empty();
}

// T in µs; --slot-ms and --baud must give a whole number of ms.
uint32_t whole_ms_slot_us(const std::string& option, double slot_ms) {
    const uint32_t slot_us = to_integer<uint32_t>(option, slot_ms * k_us_per_ms);
    if (slot_us % k_us_per_ms_int != 0)
        throw UsageError(option + " gives T " + fixed(slot_us / k_us_per_ms, k_us_decimals) +
                         " ms; T must be a whole number of ms");
    return slot_us;
}

// f_ref of a changed mode without --tone (spec 1.3): fm presets keep k_fm_tone_hz with the grid below,
// otherwise the band is centred on k_band_centre_hz. A band that cannot fit is left to valid() to refuse.
uint16_t placed_tone_hz(const EncoderConfig& config, bool fm) {
    if (fm && config.side == GridSide::below) return unlimited::k_fm_tone_hz;
    const double half_span = span_hz(config.bits_per_peak, config.spacing, config.slot_us / k_us_per_ms) / 2.0;
    const double tone = config.side == GridSide::below ? std::ceil(unlimited::k_band_centre_hz + half_span)
                                                       : std::floor(unlimited::k_band_centre_hz - half_span);
    return static_cast<uint16_t>(std::max<double>(unlimited::k_min_tone_hz,
                                                  std::min<double>(unlimited::k_max_tone_hz, tone)));
}

bool bits_in_range(const EncoderConfig& config) {
    return config.bits_per_peak >= 1 && config.bits_per_peak <= unlimited::k_max_bits_per_peak;
}

EncoderConfig encoder_config(const Options& o) {
    const Preset base = base_preset(o);
    EncoderConfig config = EncoderConfig::from_preset(base, to_integer<uint32_t>("--rate", o.rate_hz));
    if (o.has_slot_ms) config.slot_us = whole_ms_slot_us("--slot-ms", o.slot_ms);
    if (o.has_baud) {
        if (!(o.baud > 0.0)) throw UsageError("--baud must be positive");
        config.slot_us = whole_ms_slot_us("--baud", k_ms_per_s / o.baud);
    }
    if (o.has_bits) config.bits_per_peak = to_integer<uint8_t>("--bits-per-peak", o.bits_per_peak);
    if (o.has_slots) config.data_slots = to_integer<uint8_t>("--data-slots", o.data_slots);
    if (!o.spacing.empty()) config.spacing = find_name(k_spacings, "--spacing", o.spacing).spacing;
    if (!o.side.empty()) config.side = find_name(k_sides, "--side", o.side).side;
    if (o.has_tone)
        config.tone_hz = to_integer<uint16_t>("--tone", o.tone_hz);
    else if (mode_changed(o) && bits_in_range(config) && config.slot_us > 0)
        config.tone_hz = placed_tone_hz(config, fm_preset(base));
    if (o.level_dbfs > 0.0) throw UsageError("--level-dbfs must be <= 0");
    config.amplitude = to_integer<int16_t>("--level-dbfs",
                                            k_dbfs_reference * std::pow(10.0, o.level_dbfs / k_amplitude_db));
    if (o.has_lead_in) config.lead_in_ms = to_integer<uint16_t>("--lead-in-ms", o.lead_in_ms);
    if (o.has_tune) config.tune_ms = to_integer<uint16_t>("--tune-ms", o.tune_ms);
    if (o.has_sync) config.sync_markers = to_integer<uint8_t>("--sync", o.sync);
    if (!config.valid()) throw UsageError("invalid signal: " + config_problem(config));
    return config;
}

// The preset in use: named, the profile's default, or "custom" when the mode was given directly.
std::string preset_name(const Options& o) {
    if (mode_changed(o)) return "custom";
    if (!o.preset.empty()) return o.preset;
    const Preset preset = base_preset(o);
    for (size_t i = 0; i < sizeof(k_presets) / sizeof(k_presets[0]); ++i)
        if (k_presets[i].preset == preset) return k_presets[i].name;
    return o.profile;
}

// Empty when the --profile receiver decodes this signal; otherwise why not (the sender may still mean
// another receiver profile).
std::string profile_note(const Options& o, const EncoderConfig& config) {
    const DecoderConfig receiver = DecoderConfig::for_profile(find_name(k_profiles, "--profile", o.profile).profile);
    const uint32_t slot_ms = config.slot_us / k_us_per_ms_int;
    if (slot_ms < receiver.min_slot_ms || slot_ms > receiver.max_slot_ms() || config.tone_hz < receiver.min_tone_hz ||
        config.tone_hz > receiver.max_tone_hz)
        return "the " + o.profile + " profile decodes T " + std::to_string(receiver.min_slot_ms) + ".." +
               std::to_string(receiver.max_slot_ms()) + " ms and f_ref " + std::to_string(receiver.min_tone_hz) +
               ".." + std::to_string(receiver.max_tone_hz) + " Hz; decode this signal with another profile";
    return "";
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

// Average power of the frames over the key-down power (spec 1.3): every data slot holds a peak, every frame
// one marker (its STOP; the START is the previous STOP), each by its slot energy.
double average_power_ratio(const EncoderConfig& config, size_t bytes) {
    const double peaks = std::ceil(static_cast<double>(bytes) * unlimited::k_bits_per_byte / config.bits_per_peak);
    const double frames = std::ceil(peaks / config.data_slots);
    return (peaks * unlimited::k_peak_energy + frames * unlimited::k_marker_energy) / (peaks + frames);
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

int run(const Options& o) {
    const EncoderConfig config = encoder_config(o);

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

    std::unique_ptr<pc::Tui> tui;
    std::vector<StatusMark> marks;
    if (o.tui) {
        tui.reset(new pc::Tui(pc::TuiMode::encoder));
        if (tui->open()) {
            tui->set_profile(preset_name(o));
            tui->set_tone_hz(config.tone_hz);
            tui->set_slot_ms(static_cast<float>(config.slot_us / k_us_per_ms));
            tui->set_mode(config.bits_per_peak, config.data_slots, config.spacing, config.side);
            tui->set_field("rate", std::to_string(config.sample_rate_hz) + " Hz");
            if (o.channel)
                tui->set_field("channel", o.channel_name + " " + fixed(channel_config.snr_db, k_db_decimals) + " dB");
        } else {
            std::fprintf(stderr, "%s: --tui needs a terminal on stdout; plain output\n", k_program);
            tui.reset();
        }
    }
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

    const double slot_ms = config.slot_us / k_us_per_ms;
    double low_hz = 0.0;
    double high_hz = 0.0;
    band_hz(config, low_hz, high_hz);
    const double side = config.side == GridSide::above ? 1.0 : -1.0;
    const unsigned top_tone = (1u << config.bits_per_peak) - 1u;
    const double first_tone_hz = config.tone_hz + side * tone_offset_hz(0, config.spacing, slot_ms);
    const double last_tone_hz = config.tone_hz + side * tone_offset_hz(top_tone, config.spacing, slot_ms);
    std::printf("data     %zu byte%s", payload.size(), payload.size() == 1 ? "" : "s");
    if (o.packet) std::printf(" in %zu packet%s, %zu bytes framed", packets, packets == 1 ? "" : "s", data.size());
    std::printf("\n");
    std::printf("signal   %s: %s, %u bytes per frame\n", preset_name(o).c_str(),
                mode_text(slot_ms, config.bits_per_peak, config.data_slots, config.spacing, side_name(config.side))
                    .c_str(),
                static_cast<unsigned>(config.frame_bytes()));
    std::printf("tones    f_ref %u Hz, tone 0 at %s Hz, tone %u at %s Hz, band %s..%s Hz\n",
                static_cast<unsigned>(config.tone_hz), fixed(first_tone_hz, k_tone_decimals).c_str(),
                top_tone, fixed(last_tone_hz, k_tone_decimals).c_str(),
                fixed(low_hz, k_hz_decimals).c_str(), fixed(high_hz, k_hz_decimals).c_str());
    std::printf("audio    %s s, %zu samples at %u Hz -> %s, level %s dBFS, average %s dB of key-down\n",
                fixed(static_cast<double>(samples) / config.sample_rate_hz, k_seconds_decimals).c_str(), samples,
                static_cast<unsigned>(config.sample_rate_hz), o.out_spec.c_str(),
                fixed(k_amplitude_db * std::log10(config.amplitude / k_dbfs_reference), k_db_decimals).c_str(),
                fixed(k_power_db * std::log10(average_power_ratio(config, data.size())), k_power_decimals).c_str());
    if (!o.clean_out.empty()) std::printf("clean    -> %s\n", o.clean_out.c_str());
    if (o.channel) {
        const sim::Mode mode = channel_config.mode;
        std::printf("channel  %s", o.channel_name.c_str());
        if (mode == sim::Mode::usb || mode == sim::Mode::lsb) {
            std::printf(", snr %s dB key-down, %s dB average power",
                        fixed(channel_config.snr_db, k_db_decimals).c_str(),
                        fixed(channel_config.snr_db + k_power_db * std::log10(average_power_ratio(config, data.size())),
                              k_db_decimals)
                            .c_str());
        } else if (mode != sim::Mode::clean) {
            std::printf(", snr %s dB carrier", fixed(channel_config.snr_db, k_db_decimals).c_str());
        }
        if (mode == sim::Mode::fm)
            std::printf(", cnr %s dB in %s Hz", fixed(sim::fm_cnr_db(channel_config), k_db_decimals).c_str(),
                        fixed(channel_config.fm_if_bandwidth_hz, 0).c_str());
        std::printf(", output gain %s dB\n", fixed(gain_db, k_db_decimals).c_str());
    }
    const std::string note = profile_note(o, config);
    if (!note.empty()) std::fprintf(stderr, "%s: note: %s\n", k_program, note.c_str());

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
            std::printf(k_usage, static_cast<unsigned>(unlimited::k_packet_max_payload));
            return k_exit_ok;
        }
        return run(options);
    } catch (const UsageError& error) {
        std::fprintf(stderr, "%s: %s (see --help)\n", k_program, error.what());
        return k_exit_usage;
    }
}
