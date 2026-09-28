#include "modem_link.hpp"

#include <algorithm>
#include <cmath>

namespace unlimited {
namespace pc {

using std::size_t;
using std::uint32_t;
using std::uint8_t;

const uint32_t ModemLink::k_step_ms;

namespace {

const size_t k_stations = 2;
const uint32_t k_ms_per_s = 1000;
const uint32_t k_step_samples = ModemLink::k_step_ms * k_modem_rate_hz / k_ms_per_s;
const double k_full_scale = 32768.0;
const double k_int16_max = 32767.0;
const double k_int16_min = -32768.0;
// The channel's output is scaled so that the key-down tone plus 5 sigma of its noise stays below 0.9 of full scale
// (as the tests' loopback does): noise peaks do not clip.
const double k_reference_bandwidth_hz = 2500.0;
const double k_receiver_bandwidth_hz = 2400.0;
const double k_noise_peak_sigmas = 5.0;
const double k_headroom = 0.9;
const double k_power_ratio_db = 10.0;
const double k_half = 0.5;
// The receivers may still be busy this long after the last PTT release: the end comes a window or two after the last
// STOP (up to 4 more while a silent START is weighed), plus the look-ahead.
const uint32_t k_settle_windows = 5;
const uint32_t k_settle_margin_ms = 500;
const uint32_t k_us_per_ms = 1000;

sim::ChannelConfig direction(const sim::ChannelConfig& base, const ModemConfig& sender, bool reverse) {
    sim::ChannelConfig config = base;
    config.sample_rate = k_modem_rate_hz;
    config.signal_level = sender.signal.amplitude / k_full_scale;
    if (reverse) {
        config.freq_offset_hz = -base.freq_offset_hz;
        config.seed = base.seed + 1;
    }
    if (config.noise) {
        const double snr = std::pow(10.0, config.snr_db / k_power_ratio_db);
        const double tone_power = k_half * config.signal_level * config.signal_level;
        const double sigma = std::sqrt(tone_power / (snr * k_reference_bandwidth_hz) * k_receiver_bandwidth_hz);
        config.output_gain = std::min(1.0, k_headroom / (config.signal_level + k_noise_peak_sigmas * sigma));
    }
    return config;
}

int16_t to_int16(float x) {
    const double value = std::round(static_cast<double>(x) * k_full_scale);
    return static_cast<int16_t>(std::max(k_int16_min, std::min(k_int16_max, value)));
}

}  // namespace

LinkConfig::LinkConfig() : noisy(false) {}

struct ModemLink::Station {
    Station(ModemLink& owner, size_t number, const ModemConfig& config)
        : link(owner),
          index(number),
          modem(config, &Station::on_host, &Station::on_ptt, this),
          pending_from(0),
          vox(config.signal.vox_lead_ms > 0) {
        modem.set_event_tap(&Station::on_event, this);
    }

    static void on_host(const uint8_t* data, size_t size, void* context) {
        Station& station = *static_cast<Station*>(context);
        for (size_t i = 0; i < size; ++i) {
            uint8_t value = 0;
            const KissStep step = station.kiss.feed(data[i], value);
            if (step == KissStep::data) station.frame.push_back(value);
            if (step == KissStep::end) {
                station.frames.push_back(station.frame);
                station.frame.clear();
                if (station.link.handler_ != nullptr)
                    station.link.handler_(station.link, station.index, station.frames.back(),
                                          station.link.handler_context_);
            }
            if (!station.kiss.in_data()) station.frame.clear();
        }
    }

    static void on_ptt(bool on, void* context) {
        Station& station = *static_cast<Station*>(context);
        if (on) {
            const LinkKey key = {station.link.now_ms_, 0};
            station.keys.push_back(key);
        } else if (!station.keys.empty()) {
            station.keys.back().off_ms = station.link.now_ms_;
            station.link.last_release_ms_ = station.link.now_ms_;
        }
    }

    static void on_event(const Event& event, void* context) {
        Station& station = *static_cast<Station*>(context);
        const uint32_t now = station.link.now_ms_;
        switch (event.type) {
            case EventType::locked: {
                LinkReception reception = LinkReception();
                reception.locked_ms = now;
                reception.tone_hz = event.tone_hz;
                reception.slot_ms = event.slot_ms;
                reception.snr_db = event.snr_db;
                station.receptions.push_back(reception);
                break;
            }
            case EventType::byte:
                if (station.receptions.empty() || station.receptions.back().closed) break;
                station.receptions.back().bytes.push_back(event.value);
                station.receptions.back().indexes.push_back(event.byte_index);
                station.receptions.back().snr_db = event.snr_db;
                station.receptions.back().slot_ms = event.slot_ms;
                break;
            case EventType::slot:
                // The last data slot of a window dropped as a framing error: its byte never comes.
                if ((event.flags & event_flag_framing) != 0 && event.slot == k_stop_slot - 1 &&
                    !station.receptions.empty() && !station.receptions.back().closed)
                    ++station.receptions.back().dropped;
                break;
            case EventType::end:
            case EventType::lost:
                if (station.receptions.empty() || station.receptions.back().closed) break;
                station.receptions.back().closed = true;
                station.receptions.back().closed_ms = now;
                station.receptions.back().lost = event.type == EventType::lost;
                break;
            case EventType::state:
                break;
        }
    }

    bool reception_open() const { return !receptions.empty() && !receptions.back().closed; }

    ModemLink& link;
    size_t index;
    Modem modem;
    std::vector<uint8_t> pending;  // KISS bytes its computer sent that the modem has not taken yet
    size_t pending_from;
    bool vox;                      // a VOX lead: the audio keys the radio, so it is always on the air
    KissDecoder kiss;              // what the modem sends its computer
    std::vector<uint8_t> frame;
    std::vector<std::vector<uint8_t> > frames;
    std::vector<LinkReception> receptions;
    std::vector<LinkKey> keys;
};

ModemLink::ModemLink(const LinkConfig& config)
    : config_(config), now_ms_(0), last_release_ms_(0), handler_(nullptr), handler_context_(nullptr) {
    for (size_t s = 0; s < k_stations; ++s) stations_[s].reset(new Station(*this, s, config.stations[s]));
    if (!config.noisy) return;
    for (size_t s = 0; s < k_stations; ++s)
        channels_[s].reset(new sim::Channel(direction(config.channel, config.stations[s], s != 0)));
}

ModemLink::~ModemLink() {}

void ModemLink::send(size_t station, const std::vector<uint8_t>& data) {
    Station& sender = *stations_[station];
    sender.pending.push_back(k_kiss_fend);
    sender.pending.push_back(k_kiss_data);
    for (size_t i = 0; i < data.size(); ++i) {
        uint8_t escaped[k_kiss_escaped_max];
        const uint8_t size = kiss_escape(data[i], escaped);
        sender.pending.insert(sender.pending.end(), escaped, escaped + size);
    }
    sender.pending.push_back(k_kiss_fend);
}

void ModemLink::step() {
    for (size_t s = 0; s < k_stations; ++s) {
        Station& station = *stations_[s];
        if (station.pending_from < station.pending.size()) {
            station.pending_from += station.modem.host_input(&station.pending[station.pending_from],
                                                             station.pending.size() - station.pending_from);
            if (station.pending_from == station.pending.size()) {
                station.pending.clear();
                station.pending_from = 0;
            }
        }
    }
    for (size_t s = 0; s < k_stations; ++s) stations_[s]->modem.tick(now_ms_);
    int16_t played[k_stations][k_step_samples];
    for (size_t s = 0; s < k_stations; ++s) stations_[s]->modem.audio_output(played[s], k_step_samples);
    for (size_t s = 0; s < k_stations; ++s) {
        const Station& sender = *stations_[s];
        const bool on_air = sender.vox || sender.modem.transmitting();
        int16_t heard[k_step_samples];
        if (channels_[s] == nullptr) {
            for (size_t i = 0; i < k_step_samples; ++i) heard[i] = on_air ? played[s][i] : 0;
        } else {
            float in[k_step_samples];
            float out[k_step_samples];
            for (size_t i = 0; i < k_step_samples; ++i)
                in[i] = on_air ? static_cast<float>(played[s][i] / k_full_scale) : 0.0f;
            channels_[s]->process(in, out, k_step_samples);
            for (size_t i = 0; i < k_step_samples; ++i) heard[i] = to_int16(out[i]);
        }
        stations_[k_stations - 1 - s]->modem.audio_input(heard, k_step_samples);
    }
    now_ms_ += k_step_ms;
}

void ModemLink::run(uint32_t ms) {
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += k_step_ms) step();
}

bool ModemLink::idle() const {
    for (size_t s = 0; s < k_stations; ++s) {
        const Station& station = *stations_[s];
        if (!station.pending.empty() || station.reception_open()) return false;
        if (station.modem.channel_state() != ChannelState::idle) return false;
        const ModemCounters counters = station.modem.counters();
        if (counters.queued_bytes != 0 || counters.queued_frames != 0) return false;
    }
    return now_ms_ - last_release_ms_ >= settle_ms();
}

bool ModemLink::run_until_idle(uint32_t max_ms) {
    for (uint32_t elapsed = 0; elapsed < max_ms; elapsed += k_step_ms) {
        if (idle()) return true;
        step();
    }
    return idle();
}

void ModemLink::set_frame_handler(FrameHandler handler, void* context) {
    handler_ = handler;
    handler_context_ = context;
}

uint32_t ModemLink::now_ms() const {
    return now_ms_;
}

const std::vector<std::vector<uint8_t> >& ModemLink::frames(size_t station) const {
    return stations_[station]->frames;
}

const std::vector<LinkReception>& ModemLink::receptions(size_t station) const {
    return stations_[station]->receptions;
}

const std::vector<LinkKey>& ModemLink::keys(size_t station) const {
    return stations_[station]->keys;
}

const Modem& ModemLink::modem(size_t station) const {
    return stations_[station]->modem;
}

uint32_t ModemLink::settle_ms() const {
    uint32_t slowest_us = 0;
    for (size_t s = 0; s < k_stations; ++s) slowest_us = std::max(slowest_us, config_.stations[s].receiver.slot_us);
    return k_settle_windows * k_window_slots * slowest_us / k_us_per_ms + k_settle_margin_ms;
}

}  // namespace pc
}  // namespace unlimited
