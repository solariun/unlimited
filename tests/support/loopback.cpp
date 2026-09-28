#include "loopback.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>

namespace unlimited {
namespace loopback {

namespace {

const double k_us_per_s = 1e6;
const double k_ms_per_s = 1e3;
const double k_us_per_ms = 1e3;
const double k_full_scale = 32768.0;
const double k_int16_max = 32767.0;
const double k_int16_min = -32768.0;
const std::size_t k_render_chunk = 32;        // shorter than any slot, so the queue never runs dry
const double k_trailing_windows = 2.0;        // single(): silence after a transmission, at least 2 windows
const double k_leading_slots = 15.0;          // a whole silent window, the search's warm-up and a margin
const double k_reference_bandwidth_hz = 2500.0;
const double k_receiver_bandwidth_hz = 2400.0;
const double k_noise_peak_sigmas = 5.0;
const double k_headroom = 0.9;
const int k_byte_bits = 8;
const double k_pi = 3.14159265358979323846;
const unsigned k_byte_mask = 0xFFu;
// A lock comes once the history holds its check windows: at least 2 windows and half a slot after the anchor (spec
// 3.3); after a silent START (back to back, spec 3.7) up to about 7 windows after it, when the old grid's next
// windows are weighed too.
const double k_lock_min_slots = 19.5;
const double k_lock_max_slots = 75.0;

struct Sink {
    Capture* capture;
    const std::size_t* consumed;
};

void on_event(const Event& event, void* context) {
    Sink* sink = static_cast<Sink*>(context);
    sink->capture->events.push_back(event);
    sink->capture->event_sample.push_back(*sink->consumed);
}

int popcount(unsigned value) {
    int count = 0;
    for (; value != 0; value &= value - 1) ++count;
    return count;
}

double lead_in_samples(const EncoderConfig& config) {
    return static_cast<double>(config.lead_in_ms) * config.sample_rate_hz / k_ms_per_s;
}

// The transmission a lock at `sample` belongs to: the latest whose first START lies between k_lock_max_slots and
// k_lock_min_slots before the lock (look-ahead and channel delay removed); -1 when none does.
long transmission_of_lock(const Recording& recording, double sample, std::size_t lookahead, double delay) {
    long found = -1;
    double latest = 0.0;
    for (std::size_t t = 0; t < recording.transmissions.size(); ++t) {
        const Transmission& tx = recording.transmissions[t];
        const double slot = slot_samples(tx.config) * k_decoder_rate_hz / tx.config.sample_rate_hz;
        const double heard = sample - static_cast<double>(lookahead) - delay;
        const double start = first_start_sample(tx) * k_decoder_rate_hz / tx.config.sample_rate_hz;
        if (start > heard - k_lock_min_slots * slot || start < heard - k_lock_max_slots * slot) continue;
        if (found < 0 || start > latest) {
            found = static_cast<long>(t);
            latest = start;
        }
    }
    return found;
}

}  // namespace

std::vector<std::uint8_t> random_bytes(std::size_t count, std::uint32_t seed) {
    std::mt19937 generator(seed);
    std::vector<std::uint8_t> data(count);
    for (std::size_t i = 0; i < count; ++i) data[i] = static_cast<std::uint8_t>(generator() & k_byte_mask);
    return data;
}

std::vector<std::int16_t> encode(const std::vector<std::uint8_t>& data, const EncoderConfig& config) {
    Encoder encoder(config);
    std::size_t written = encoder.write(data.data(), data.size());
    std::vector<std::int16_t> out;
    if (!encoder.start()) return out;
    std::int16_t chunk[k_render_chunk];
    while (true) {
        if (written < data.size()) written += encoder.write(data.data() + written, data.size() - written);
        const std::size_t rendered = encoder.render(chunk, k_render_chunk);
        if (rendered == 0) break;
        out.insert(out.end(), chunk, chunk + rendered);
    }
    return out;
}

EncoderConfig speed_config(float bytes_per_second, std::uint16_t tone_hz) {
    EncoderConfig config;
    config.sample_rate_hz = k_decoder_rate_hz;
    config.slot_us = slot_us_for_speed(bytes_per_second);
    config.tone_hz = tone_hz;
    if (!passband_fit(config).fits) {
        config.passband.low_hz = k_am_passband_low_hz;
        config.passband.high_hz = k_am_passband_high_hz;
    }
    return config;
}

DecoderConfig receiver_for(const EncoderConfig& config) {
    DecoderConfig receiver;
    receiver.slot_us = config.slot_us;
    receiver.passband = config.passband;
    return receiver;
}

void append_silence(Recording& recording, double ms, std::uint32_t rate_hz) {
    const std::size_t count = static_cast<std::size_t>(std::lround(ms * rate_hz / k_ms_per_s));
    recording.samples.insert(recording.samples.end(), count, 0);
}

void append_transmission(Recording& recording, const std::vector<std::uint8_t>& data, const EncoderConfig& config) {
    Transmission transmission;
    transmission.config = config;
    transmission.data = data;
    transmission.start_sample = recording.samples.size();
    const std::vector<std::int16_t> audio = encode(data, config);
    transmission.length = audio.size();
    recording.samples.insert(recording.samples.end(), audio.begin(), audio.end());
    recording.transmissions.push_back(transmission);
}

double leading_silence_ms(const EncoderConfig& config, double ms) {
    return std::max(ms, k_leading_slots * config.slot_us / k_us_per_ms);
}

Recording single(const std::vector<std::uint8_t>& data, const EncoderConfig& config, double silence_ms) {
    Recording recording;
    append_silence(recording, leading_silence_ms(config, silence_ms), config.sample_rate_hz);
    append_transmission(recording, data, config);
    const double trailing_ms = k_trailing_windows * k_window_slots * config.slot_us / k_us_per_ms;
    append_silence(recording, std::max(silence_ms, trailing_ms), config.sample_rate_hz);
    return recording;
}

double slot_samples(const EncoderConfig& config) {
    return static_cast<double>(config.slot_us) * config.sample_rate_hz / k_us_per_s;
}

std::size_t vox_slots(const EncoderConfig& config) {
    if (config.vox_lead_ms == 0) return 0;
    const std::size_t lead = static_cast<std::size_t>(
        std::ceil(config.vox_lead_ms * k_us_per_ms / static_cast<double>(config.slot_us) - 1e-9));
    return std::max<std::size_t>(lead, k_min_vox_lead_slots) + k_vox_gap_slots;
}

double first_start_sample(const Transmission& transmission) {
    return static_cast<double>(transmission.start_sample) + lead_in_samples(transmission.config) +
           static_cast<double>(vox_slots(transmission.config)) * slot_samples(transmission.config);
}

double slot_start_sample(const Transmission& transmission, std::size_t byte, std::size_t slot) {
    return first_start_sample(transmission) +
           static_cast<double>(byte * k_window_slots + slot) * slot_samples(transmission.config);
}

double end_sample(const Transmission& transmission) {
    return slot_start_sample(transmission, transmission.data.size(), 0);
}

void scale_slot(Recording& recording, std::size_t transmission, std::size_t byte, std::size_t slot, double gain) {
    const Transmission& t = recording.transmissions[transmission];
    const double begin = slot_start_sample(t, byte, slot);
    const std::size_t first = static_cast<std::size_t>(std::floor(begin));
    const std::size_t last = static_cast<std::size_t>(std::ceil(begin + slot_samples(t.config)));
    for (std::size_t i = first; i < last && i < recording.samples.size(); ++i) {
        recording.samples[i] = static_cast<std::int16_t>(std::lround(recording.samples[i] * gain));
    }
}

std::vector<std::int16_t> through_channel(const std::vector<std::int16_t>& samples, sim::ChannelConfig config,
                                          std::int16_t amplitude) {
    config.signal_level = amplitude / k_full_scale;
    if (config.noise) {
        const double snr = std::pow(10.0, config.snr_db / 10.0);
        const double tone_power = 0.5 * config.signal_level * config.signal_level;
        const double noise_density = tone_power / (snr * k_reference_bandwidth_hz);
        const double sigma = std::sqrt(noise_density * k_receiver_bandwidth_hz);
        const double peak = config.signal_level + k_noise_peak_sigmas * sigma;
        config.output_gain = std::min(1.0, k_headroom / peak);
    }
    std::vector<float> in(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) in[i] = static_cast<float>(samples[i] / k_full_scale);
    sim::Channel channel(config);
    const std::vector<float> out = channel.process(in);
    std::vector<std::int16_t> result(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double value = std::round(out[i] * k_full_scale);
        result[i] = static_cast<std::int16_t>(std::max(k_int16_min, std::min(k_int16_max, value)));
    }
    return result;
}

double channel_delay_samples(sim::Mode mode) {
    const std::size_t length = 8192;
    const std::size_t at = 2048;
    const std::size_t width = 128;
    const double amplitude = 10000.0;
    const double tone_hz = 1500.0;
    std::vector<std::int16_t> burst(length, 0);
    for (std::size_t n = 0; n < width; ++n) {
        const double envelope = std::pow(std::sin(k_pi * n / width), 2.0);
        burst[at + n] = static_cast<std::int16_t>(
            std::lround(amplitude * envelope * std::sin(2.0 * k_pi * tone_hz * n / k_decoder_rate_hz)));
    }
    sim::ChannelConfig config;
    config.mode = mode;
    config.noise = false;
    const std::vector<std::int16_t> out = through_channel(burst, config, static_cast<std::int16_t>(amplitude));
    double in_moment = 0.0;
    double in_energy = 0.0;
    double out_moment = 0.0;
    double out_energy = 0.0;
    for (std::size_t i = 0; i < length && i < out.size(); ++i) {
        const double a = double(burst[i]) * burst[i];
        const double b = double(out[i]) * out[i];
        in_moment += a * i;
        in_energy += a;
        out_moment += b * i;
        out_energy += b;
    }
    return out_moment / out_energy - in_moment / in_energy;
}

std::vector<std::int16_t> usb(const std::vector<std::int16_t>& samples, double snr_db, std::uint32_t seed,
                              std::int16_t amplitude, double offset_hz) {
    sim::ChannelConfig config;
    config.mode = sim::Mode::usb;
    config.snr_db = snr_db;
    config.seed = seed;
    config.freq_offset_hz = offset_hz;
    return through_channel(samples, config, amplitude);
}

Capture run_decoder(const std::vector<std::int16_t>& samples, const DecoderConfig& config, std::size_t chunk) {
    Capture capture;
    std::size_t consumed = 0;
    Sink sink = {&capture, &consumed};
    Decoder decoder(config, &on_event, &sink);
    capture.lookahead = decoder.lookahead_samples();
    if (chunk == 0) {
        for (std::size_t i = 0; i < samples.size(); ++i) {
            consumed = i + 1;
            decoder.process_sample(samples[i]);
        }
        return capture;
    }
    for (std::size_t i = 0; i < samples.size(); i += chunk) {
        const std::size_t count = std::min(chunk, samples.size() - i);
        consumed = i + count;
        decoder.process(samples.data() + i, count);
    }
    return capture;
}

double Score::ber() const {
    return matched == 0 ? 0.0 : static_cast<double>(bit_errors) / (k_byte_bits * static_cast<double>(matched));
}

double Score::loss() const {
    return bytes_sent == 0 ? 0.0 : static_cast<double>(lost_bytes) / static_cast<double>(bytes_sent);
}

Mapping map_events(const Recording& recording, const Capture& capture, double delay_samples) {
    Mapping mapping;
    Score& s = mapping.score;
    for (std::size_t t = 0; t < recording.transmissions.size(); ++t) {
        s.bytes_sent += recording.transmissions[t].data.size();
        mapping.received.push_back(std::vector<int>(recording.transmissions[t].data.size(), -1));
        mapping.byte_events.push_back(std::vector<Event>(recording.transmissions[t].data.size()));
    }
    std::vector<bool> locked(recording.transmissions.size(), false);
    long current = -1;  // the transmission of the open lock, -1 when none (bytes outside a lock are extra)
    bool open = false;
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        const Event& e = capture.events[i];
        switch (e.type) {
            case EventType::locked:
                ++s.locks;
                open = true;
                current = transmission_of_lock(recording, static_cast<double>(capture.event_sample[i]),
                                               capture.lookahead, delay_samples);
                break;
            case EventType::byte: {
                ++s.bytes_released;
                const long t = open ? current : -1;
                const long k = static_cast<long>(e.byte_index);
                if (t < 0 || k >= static_cast<long>(recording.transmissions[static_cast<std::size_t>(t)].data.size()) ||
                    mapping.received[static_cast<std::size_t>(t)][static_cast<std::size_t>(k)] >= 0) {
                    ++s.extra_bytes;
                    break;
                }
                const std::size_t tt = static_cast<std::size_t>(t);
                const std::size_t kk = static_cast<std::size_t>(k);
                if (kk == 0) locked[tt] = true;
                mapping.received[tt][kk] = e.value;
                mapping.byte_events[tt][kk] = e;
                ++s.matched;
                const int errors = popcount(static_cast<unsigned>(e.value ^ recording.transmissions[tt].data[kk]));
                s.bit_errors += static_cast<std::size_t>(errors);
                if (errors != 0) ++s.wrong_bytes;
                break;
            }
            case EventType::slot:
                if ((e.flags & event_flag_framing) != 0 && e.slot == k_first_data_slot) ++s.framing_windows;
                break;
            case EventType::lost:
                ++s.lost_events;
                open = false;
                break;
            case EventType::end:
                ++s.ends;
                open = false;
                break;
            case EventType::state:
                break;
        }
    }
    for (std::size_t t = 0; t < mapping.received.size(); ++t) {
        if (locked[t]) ++s.locked_transmissions;
        for (std::size_t k = 0; k < mapping.received[t].size(); ++k) {
            if (mapping.received[t][k] < 0) ++s.lost_bytes;
        }
    }
    return mapping;
}

Score score(const Recording& recording, const Capture& capture, double delay_samples) {
    return map_events(recording, capture, delay_samples).score;
}

std::size_t count_events(const Capture& capture, EventType type) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        if (capture.events[i].type == type) ++count;
    }
    return count;
}

}  // namespace loopback
}  // namespace unlimited
