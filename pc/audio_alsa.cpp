#include "audio_live.hpp"

#if defined(__linux__)

#include <alsa/asoundlib.h>

#include <cerrno>
#include <cstdlib>

// ALSA backend (spec 12.5): one PCM per direction, 16-bit interleaved at the rate the device accepted
// (snd_pcm_hw_params_get_rate after the parameters are installed), each on a thread of its own blocked in
// snd_pcm_readi or snd_pcm_writei: the device's clock paces them, so they need no timer and see stop() within one
// period. An xrun (-EPIPE) or a suspend (-ESTRPIPE) is counted and the PCM prepared again; nothing is ever drained
// (the output plays silence between transmissions), so no write can meet the state a drain leaves (-EBADFD).
namespace unlimited {
namespace pc {

using std::int16_t;
using std::size_t;
using std::uint32_t;

namespace {

const char* const k_backend = "alsa";
const char* const k_default_pcm = "default";
const unsigned k_default_rate_hz = 48000;  // asked when -r is not given: a USB codec's own rate
const unsigned k_period_us = 20000;        // one blocking read or write
const unsigned k_buffer_us = 100000;       // the device's queue; the output latency
const unsigned k_mono = 1;                 // asked; the plug layer converts, a hw: device may give more
const unsigned k_ms_per_s = 1000;
const char* const k_card_marker = "CARD=";  // hint names of a card's own PCMs, listed from the cards instead
const char* const k_null_pcm = "null";

std::string alsa_error(const std::string& what, int error) {
    return what + ": " + snd_strerror(error);
}

// hw_params, freed with their owner.
class HwParams {
public:
    HwParams() : params_(nullptr) { snd_pcm_hw_params_malloc(&params_); }
    ~HwParams() {
        if (params_ != nullptr) snd_pcm_hw_params_free(params_);
    }
    HwParams(const HwParams&) = delete;
    HwParams& operator=(const HwParams&) = delete;

    snd_pcm_hw_params_t* get() const { return params_; }

private:
    snd_pcm_hw_params_t* params_;
};

// A PCM open for one direction, configured for 16-bit interleaved audio.
class Pcm {
public:
    Pcm() : pcm_(nullptr), rate_hz_(0), channels_(0), period_frames_(0), buffer_frames_(0) {}
    ~Pcm() {
        if (pcm_ != nullptr) snd_pcm_close(pcm_);
    }
    Pcm(const Pcm&) = delete;
    Pcm& operator=(const Pcm&) = delete;

    bool open(const std::string& name, snd_pcm_stream_t stream, unsigned requested_hz, std::string& error) {
        int result = snd_pcm_open(&pcm_, name.c_str(), stream, 0);
        if (result < 0) {
            pcm_ = nullptr;
            error = alsa_error("cannot open ALSA device '" + name + "'", result);
            return false;
        }
        HwParams params;
        snd_pcm_hw_params_t* hw = params.get();
        if (hw == nullptr) {
            error = "out of memory";
            return false;
        }
        unsigned rate = requested_hz != 0 ? requested_hz : k_default_rate_hz;
        unsigned channels = k_mono;
        unsigned period_us = k_period_us;
        unsigned buffer_us = k_buffer_us;
        result = snd_pcm_hw_params_any(pcm_, hw);
        if (result >= 0) result = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        if (result >= 0) result = snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16);
        if (result >= 0) result = snd_pcm_hw_params_set_channels_near(pcm_, hw, &channels);
        if (result >= 0) result = snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr);
        if (result >= 0) result = snd_pcm_hw_params_set_period_time_near(pcm_, hw, &period_us, nullptr);
        if (result >= 0) result = snd_pcm_hw_params_set_buffer_time_near(pcm_, hw, &buffer_us, nullptr);
        if (result >= 0) result = snd_pcm_hw_params(pcm_, hw);
        if (result < 0) {
            error = alsa_error("ALSA device '" + name + "' refuses 16-bit audio (try its plughw: name)", result);
            return false;
        }
        // The installed parameters: what the device accepted, which the programs resample from and to.
        snd_pcm_hw_params_get_rate(hw, &rate_hz_, nullptr);
        snd_pcm_hw_params_get_channels(hw, &channels_);
        snd_pcm_hw_params_get_period_size(hw, &period_frames_, nullptr);
        snd_pcm_hw_params_get_buffer_size(hw, &buffer_frames_);
        if (rate_hz_ == 0 || channels_ == 0 || period_frames_ == 0) {
            error = "ALSA device '" + name + "' reports no usable format";
            return false;
        }
        name_ = name;
        return true;
    }

    snd_pcm_t* get() const { return pcm_; }
    uint32_t rate_hz() const { return rate_hz_; }
    unsigned channels() const { return channels_; }
    snd_pcm_uframes_t period_frames() const { return period_frames_; }
    snd_pcm_uframes_t buffer_frames() const { return buffer_frames_; }

    std::string description() const {
        return std::string(k_backend) + ":" + name_ + " (" + std::to_string(rate_hz_) + " Hz, " +
               std::to_string(channels_) + (channels_ == 1 ? " channel)" : " channels)");
    }

private:
    snd_pcm_t* pcm_;
    std::string name_;
    unsigned rate_hz_;
    unsigned channels_;
    snd_pcm_uframes_t period_frames_;
    snd_pcm_uframes_t buffer_frames_;
};

// An xrun or a suspend: counted, and the PCM prepared again. False when it cannot be prepared.
bool recover(snd_pcm_t* pcm, DeviceStatus& status, int error) {
    status.xrun();
    const int result = snd_pcm_prepare(pcm);
    if (result >= 0) return true;
    status.fail(alsa_error("cannot prepare the ALSA device again after " + std::string(snd_strerror(error)), result));
    return false;
}

class AlsaCapture final : public CaptureBackend {
public:
    AlsaCapture() : feed_(nullptr), stopping_(false) {}
    ~AlsaCapture() { stop(); }

    bool open(const std::string& name, unsigned rate_hz, std::string& error) {
        return pcm_.open(name, SND_PCM_STREAM_CAPTURE, rate_hz, error);
    }

    bool start(InputFeed& feed, std::string&) override {
        feed_ = &feed;
        stopping_.store(false);
        reader_ = std::thread(&AlsaCapture::run, this);
        return true;
    }

    void stop() override {
        stopping_.store(true);
        if (reader_.joinable()) reader_.join();
    }

    uint32_t sample_rate_hz() const override { return pcm_.rate_hz(); }
    std::string description() const override { return pcm_.description(); }

private:
    // The first read starts the capture; each read waits for one period of the device.
    void run() {
        const snd_pcm_uframes_t period = pcm_.period_frames();
        std::vector<int16_t> frames(static_cast<size_t>(period) * pcm_.channels());
        while (!stopping_.load()) {
            const snd_pcm_sframes_t got = snd_pcm_readi(pcm_.get(), &frames[0], period);
            if (got >= 0) {
                feed_->deliver(&frames[0], static_cast<size_t>(got), pcm_.channels());
            } else if (got == -EPIPE || got == -ESTRPIPE) {
                if (!recover(pcm_.get(), *feed_, static_cast<int>(got))) break;
            } else if (got != -EINTR) {
                feed_->fail(alsa_error("ALSA read failed", static_cast<int>(got)));
                break;
            }
        }
        snd_pcm_drop(pcm_.get());
    }

    Pcm pcm_;
    InputFeed* feed_;
    std::atomic<bool> stopping_;
    std::thread reader_;
};

class AlsaPlayback final : public PlaybackBackend {
public:
    AlsaPlayback() : feed_(nullptr), stopping_(false) {}
    ~AlsaPlayback() { stop(); }

    bool open(const std::string& name, unsigned rate_hz, std::string& error) {
        return pcm_.open(name, SND_PCM_STREAM_PLAYBACK, rate_hz, error);
    }

    bool start(OutputFeed& feed, std::string&) override {
        feed_ = &feed;
        stopping_.store(false);
        writer_ = std::thread(&AlsaPlayback::run, this);
        return true;
    }

    void stop() override {
        stopping_.store(true);
        if (writer_.joinable()) writer_.join();
    }

    uint32_t sample_rate_hz() const override { return pcm_.rate_hz(); }

    // A sample waits behind the whole buffer the writer keeps full.
    uint32_t latency_ms() const override {
        const double frames = static_cast<double>(pcm_.buffer_frames());
        return static_cast<uint32_t>(std::ceil(frames * k_ms_per_s / pcm_.rate_hz()));
    }

    std::string description() const override {
        return pcm_.description() + ", latency " + std::to_string(latency_ms()) + " ms";
    }

private:
    // Pulls one period from the source, then blocks until the device has room for it. Playback starts once the
    // buffer is full; silence keeps it running between transmissions.
    void run() {
        const snd_pcm_uframes_t period = pcm_.period_frames();
        const unsigned channels = pcm_.channels();
        std::vector<int16_t> frames(static_cast<size_t>(period) * channels);
        bool running = true;
        while (running && !stopping_.load()) {
            feed_->fill(&frames[0], static_cast<size_t>(period), channels);
            const int16_t* next = &frames[0];
            snd_pcm_uframes_t left = period;
            while (left > 0 && !stopping_.load()) {
                const snd_pcm_sframes_t put = snd_pcm_writei(pcm_.get(), next, left);
                if (put >= 0) {
                    next += static_cast<size_t>(put) * channels;
                    left -= static_cast<snd_pcm_uframes_t>(put);
                } else if (put == -EPIPE || put == -ESTRPIPE) {
                    running = recover(pcm_.get(), *feed_, static_cast<int>(put));
                    if (!running) break;
                } else if (put != -EINTR) {
                    feed_->fail(alsa_error("ALSA write failed", static_cast<int>(put)));
                    running = false;
                    break;
                }
            }
        }
        snd_pcm_drop(pcm_.get());  // stop() means now: what is still queued is not played
    }

    Pcm pcm_;
    OutputFeed* feed_;
    std::atomic<bool> stopping_;
    std::thread writer_;
};

// ---------------------------------------------------------------------------
// Device list: the configuration's PCMs (default, pulse, pipewire, ...) from the name hints, then each card's PCM
// devices from its control interface, by their plughw: name (any rate, format and channel count converted).
// ---------------------------------------------------------------------------

// A malloc'ed hint string, freed with its owner.
std::string take_hint(void* hint, const char* id) {
    char* value = snd_device_name_get_hint(hint, id);
    if (value == nullptr) return std::string();
    const std::string text = value;
    std::free(value);
    return text;
}

std::string first_line(const std::string& text) {
    return text.substr(0, text.find('\n'));
}

void add_hint_devices(std::vector<DeviceInfo>& devices) {
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0 || hints == nullptr) return;
    for (void** hint = hints; *hint != nullptr; ++hint) {
        const std::string name = take_hint(*hint, "NAME");
        if (name.empty() || name == k_null_pcm || name.find(k_card_marker) != std::string::npos) continue;
        const std::string description = take_hint(*hint, "DESC");
        const std::string io = take_hint(*hint, "IOID");  // "Input", "Output", or none for both
        DeviceInfo info;
        info.backend = k_backend;
        info.number = static_cast<unsigned>(devices.size());
        info.name = description.empty() ? name : first_line(description);
        info.uid = name;
        info.input_channels = io != "Output" ? k_channels_unknown : 0;
        info.output_channels = io != "Input" ? k_channels_unknown : 0;
        info.default_input = name == k_default_pcm && info.input_channels != 0;
        info.default_output = name == k_default_pcm && info.output_channels != 0;
        devices.push_back(info);
    }
    snd_device_name_free_hint(hints);
}

// The most channels and the standard rates of a card's PCM, opened without blocking; unknown when it is busy.
void probe(const std::string& hw_name, snd_pcm_stream_t stream, unsigned& channels, std::vector<uint32_t>& rates) {
    channels = k_channels_unknown;
    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, hw_name.c_str(), stream, SND_PCM_NONBLOCK) < 0) return;
    HwParams params;
    if (params.get() != nullptr && snd_pcm_hw_params_any(pcm, params.get()) >= 0) {
        unsigned most = 0;
        if (snd_pcm_hw_params_get_channels_max(params.get(), &most) >= 0 && most > 0) channels = most;
        for (size_t i = 0; i < sizeof(k_standard_rates_hz) / sizeof(k_standard_rates_hz[0]); ++i)
            if (snd_pcm_hw_params_test_rate(pcm, params.get(), k_standard_rates_hz[i], 0) == 0 &&
                std::find(rates.begin(), rates.end(), k_standard_rates_hz[i]) == rates.end())
                rates.push_back(k_standard_rates_hz[i]);
    }
    snd_pcm_close(pcm);
}

// probe_formats: open each card's PCM for its channels and rates (--list-devices); resolving a name needs only which
// directions exist.
void add_card_devices(std::vector<DeviceInfo>& devices, bool probe_formats) {
    snd_ctl_card_info_t* card_info = nullptr;
    snd_pcm_info_t* pcm_info = nullptr;
    if (snd_ctl_card_info_malloc(&card_info) < 0 || snd_pcm_info_malloc(&pcm_info) < 0) {
        if (card_info != nullptr) snd_ctl_card_info_free(card_info);
        return;
    }
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        snd_ctl_t* control = nullptr;
        if (snd_ctl_open(&control, ("hw:" + std::to_string(card)).c_str(), 0) < 0) continue;
        if (snd_ctl_card_info(control, card_info) < 0) {
            snd_ctl_close(control);
            continue;
        }
        const std::string card_id = snd_ctl_card_info_get_id(card_info);
        const std::string card_name = snd_ctl_card_info_get_name(card_info);
        int pcm_device = -1;
        while (snd_ctl_pcm_next_device(control, &pcm_device) == 0 && pcm_device >= 0) {
            const std::string where = "CARD=" + card_id + ",DEV=" + std::to_string(pcm_device);
            DeviceInfo info;
            info.backend = k_backend;
            info.uid = "plughw:" + where;
            std::string pcm_name;
            const snd_pcm_stream_t streams[] = {SND_PCM_STREAM_CAPTURE, SND_PCM_STREAM_PLAYBACK};
            for (size_t s = 0; s < sizeof(streams) / sizeof(streams[0]); ++s) {
                snd_pcm_info_set_device(pcm_info, static_cast<unsigned>(pcm_device));
                snd_pcm_info_set_subdevice(pcm_info, 0);
                snd_pcm_info_set_stream(pcm_info, streams[s]);
                if (snd_ctl_pcm_info(control, pcm_info) < 0) continue;
                if (pcm_name.empty()) pcm_name = snd_pcm_info_get_name(pcm_info);
                unsigned& channels = streams[s] == SND_PCM_STREAM_CAPTURE ? info.input_channels : info.output_channels;
                channels = k_channels_unknown;
                if (probe_formats) probe("hw:" + where, streams[s], channels, info.rates);
            }
            if (info.input_channels == 0 && info.output_channels == 0) continue;
            std::sort(info.rates.begin(), info.rates.end());
            info.name = pcm_name.empty() ? card_name : card_name + ", " + pcm_name;
            info.number = static_cast<unsigned>(devices.size());
            devices.push_back(info);
        }
        snd_ctl_close(control);
    }
    snd_pcm_info_free(pcm_info);
    snd_ctl_card_info_free(card_info);
}

std::vector<DeviceInfo> enumerate(bool probe_formats) {
    std::vector<DeviceInfo> devices;
    add_hint_devices(devices);
    add_card_devices(devices, probe_formats);
    return devices;
}

// The PCM name a target means: "default", a listed device, or (nothing listed matching) the target itself.
bool resolve(const std::string& target, Direction direction, std::string& pcm, std::string& error) {
    if (target == k_default_pcm) {
        pcm = k_default_pcm;
        return true;
    }
    const std::vector<DeviceInfo> devices = enumerate(false);
    const int index = select_device(devices, target, direction, error);
    if (index >= 0) {
        pcm = devices[static_cast<size_t>(index)].uid;
        return true;
    }
    if (index == k_device_unmatched) {
        pcm = target;  // any PCM of the ALSA configuration: hw:1,0, dmix, a name from ~/.asoundrc
        return true;
    }
    error = std::string(k_backend) + ":" + target + ": " + error;
    return false;
}

}  // namespace

const char* live_backend_name() {
    return k_backend;
}

std::vector<DeviceInfo> live_devices() {
    return enumerate(true);
}

std::unique_ptr<InputDevice> open_live_input(const std::string& target, uint32_t rate_hz, std::string& error) {
    std::string pcm;
    if (!resolve(target, Direction::input, pcm, error)) return std::unique_ptr<InputDevice>();
    std::unique_ptr<AlsaCapture> backend(new AlsaCapture());
    if (!backend->open(pcm, rate_hz, error)) return std::unique_ptr<InputDevice>();
    return std::unique_ptr<InputDevice>(new LiveInput(std::move(backend)));
}

std::unique_ptr<OutputDevice> open_live_output(const std::string& target, uint32_t rate_hz, std::string& error) {
    std::string pcm;
    if (!resolve(target, Direction::output, pcm, error)) return std::unique_ptr<OutputDevice>();
    std::unique_ptr<AlsaPlayback> backend(new AlsaPlayback());
    if (!backend->open(pcm, rate_hz, error)) return std::unique_ptr<OutputDevice>();
    return std::unique_ptr<OutputDevice>(new LiveOutput(std::move(backend)));
}

}  // namespace pc
}  // namespace unlimited

#endif  // __linux__
