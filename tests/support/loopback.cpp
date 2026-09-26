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
const std::size_t k_render_chunk = 32;        // shorter than any package, so the queue never runs dry
const std::size_t k_min_tune_slots = 6;
const double k_trailing_slots = 4.0;          // single(): silence after a transmission, at least 4 T
const double k_reference_bandwidth_hz = 2500.0;
const double k_receiver_bandwidth_hz = 2400.0;
const double k_noise_peak_sigmas = 5.0;
const double k_headroom = 0.9;
const int k_byte_bits = 8;
const double k_pi = 3.14159265358979323846;
const unsigned k_byte_mask = 0xFFu;

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

std::size_t data_bits(const Transmission& transmission) {
    return transmission.data.size() * static_cast<std::size_t>(k_byte_bits);
}

long transmission_at(const Recording& recording, std::size_t sample) {
    long found = -1;
    for (std::size_t t = 0; t < recording.transmissions.size(); ++t) {
        if (recording.transmissions[t].start_sample <= sample) found = static_cast<long>(t);
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

EncoderConfig preset_config(Preset preset) {
    return EncoderConfig::from_preset(preset, k_decoder_rate_hz);
}

EncoderConfig slot_config(double slot_ms, std::uint8_t bits, std::uint16_t tone_hz) {
    EncoderConfig config = EncoderConfig::from_preset(Preset::hf, k_decoder_rate_hz);
    config.slot_us = static_cast<std::uint32_t>(std::lround(slot_ms * k_us_per_ms));
    config.bits_per_package = bits;
    config.tone_hz = tone_hz;
    if (!passband_fit(config).fits) {
        config.passband.low_hz = k_am_passband_low_hz;
        config.passband.high_hz = k_am_passband_high_hz;
    }
    return config;
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

Recording single(const std::vector<std::uint8_t>& data, const EncoderConfig& config, double silence_ms) {
    Recording recording;
    append_silence(recording, silence_ms, config.sample_rate_hz);
    append_transmission(recording, data, config);
    const double trailing_ms = k_trailing_slots * config.slot_us / k_us_per_ms;
    append_silence(recording, std::max(silence_ms, trailing_ms), config.sample_rate_hz);
    return recording;
}

double slot_samples(const EncoderConfig& config) {
    return static_cast<double>(config.slot_us) * config.sample_rate_hz / k_us_per_s;
}

double slot_start_sample(const Transmission& transmission, std::size_t slot) {
    return static_cast<double>(transmission.start_sample) + lead_in_samples(transmission.config) +
           static_cast<double>(slot) * slot_samples(transmission.config);
}

std::size_t tune_slots(const EncoderConfig& config) {
    const std::size_t slots =
        static_cast<std::size_t>(std::ceil(config.tune_ms * (k_us_per_s / k_ms_per_s) / config.slot_us - 1e-9));
    return std::max<std::size_t>(slots, k_min_tune_slots);
}

std::size_t first_start_slot(const EncoderConfig& config) {
    return tune_slots(config) + config.sync_markers - 1;
}

std::size_t package_count(const Transmission& transmission) {
    const std::size_t n = transmission.config.bits_per_package;
    return (data_bits(transmission) + n - 1) / n;
}

std::size_t package_bits(const Transmission& transmission, std::size_t package) {
    const std::size_t n = transmission.config.bits_per_package;
    const std::size_t left = data_bits(transmission) - package * n;
    return left < n ? left : n;
}

std::size_t package_start_slot(const Transmission& transmission, std::size_t package) {
    return first_start_slot(transmission.config) + package * (transmission.config.bits_per_package + 1u);
}

std::size_t stop_slot(const Transmission& transmission, std::size_t package) {
    return package_start_slot(transmission, package) + package_bits(transmission, package) + 1u;
}

std::size_t end_slot(const Transmission& transmission) {
    return stop_slot(transmission, package_count(transmission) - 1u) + 1u;
}

void scale_slot(Recording& recording, std::size_t transmission, std::size_t slot, double gain) {
    const Transmission& t = recording.transmissions[transmission];
    const double begin = slot_start_sample(t, slot);
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

Mapping map_events(const Recording& recording, const Capture& capture) {
    Mapping mapping;
    Score& s = mapping.score;
    for (std::size_t t = 0; t < recording.transmissions.size(); ++t) {
        s.bytes_sent += recording.transmissions[t].data.size();
        mapping.received.push_back(std::vector<int>(recording.transmissions[t].data.size(), -1));
        mapping.byte_events.push_back(std::vector<Event>(recording.transmissions[t].data.size()));
    }

    std::vector<std::size_t> segment;  // indices of the byte events of the current lock
    bool late = false;
    const auto close_segment = [&]() {
        if (segment.empty()) return;
        const long t = transmission_at(recording, capture.event_sample[segment.front()]);
        long offset = 0;
        if (t >= 0 && late) {
            // A cold join counts bytes from the join: the offset where most of them match.
            const std::vector<std::uint8_t>& data = recording.transmissions[t].data;
            const long size = static_cast<long>(data.size());
            std::size_t best = 0;
            for (long o = -size; o < size; ++o) {
                std::size_t matches = 0;
                for (std::size_t i = 0; i < segment.size(); ++i) {
                    const Event& e = capture.events[segment[i]];
                    const long k = static_cast<long>(e.byte_index) + o;
                    if (k >= 0 && k < size && data[k] == e.value) ++matches;
                }
                if (matches > best || (matches == best && std::labs(o) < std::labs(offset))) {
                    best = matches;
                    offset = o;
                }
            }
            if (offset != 0) ++s.shifted_segments;
        }
        mapping.offsets.push_back(offset);
        for (std::size_t i = 0; i < segment.size(); ++i) {
            const Event& e = capture.events[segment[i]];
            const long k = t < 0 ? -1 : static_cast<long>(e.byte_index) + offset;
            if (t < 0 || k < 0 || k >= static_cast<long>(recording.transmissions[t].data.size()) ||
                mapping.received[t][k] >= 0) {
                ++s.extra_bytes;
                continue;
            }
            mapping.received[t][k] = e.value;
            mapping.byte_events[t][k] = e;
            ++s.matched;
            const int errors = popcount(static_cast<unsigned>(e.value ^ recording.transmissions[t].data[k]));
            s.bit_errors += static_cast<std::size_t>(errors);
            if (errors != 0) ++s.wrong_bytes;
        }
        segment.clear();
    };

    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        const Event& e = capture.events[i];
        switch (e.type) {
        case EventType::locked:
            close_segment();
            ++s.locks;
            late = (e.flags & event_flag_late_join) != 0;
            if (late) ++s.late_joins;
            break;
        case EventType::byte:
            ++s.bytes_released;
            if ((e.flags & (event_flag_flywheel_start | event_flag_flywheel_stop)) != 0) ++s.flywheel_bytes;
            segment.push_back(i);
            break;
        case EventType::lost:
            close_segment();
            ++s.lost_events;
            if (e.reason == LostReason::alias) ++s.alias_losts;
            break;
        case EventType::end:
            close_segment();
            ++s.ends;
            break;
        case EventType::state:
        case EventType::slot:
        case EventType::package:
            break;
        }
    }
    close_segment();
    for (std::size_t t = 0; t < mapping.received.size(); ++t) {
        for (std::size_t k = 0; k < mapping.received[t].size(); ++k) {
            if (mapping.received[t][k] < 0) ++s.lost_bytes;
        }
    }
    return mapping;
}

Score score(const Recording& recording, const Capture& capture) {
    return map_events(recording, capture).score;
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
