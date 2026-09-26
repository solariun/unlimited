#include "loopback.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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
const std::size_t k_render_chunk = 32;        // shorter than any frame, so the queue never runs dry
const std::uint8_t k_min_tune_slots = 6;
const double k_release_margin_slots = 2.0;    // a frame is released at least 2 T after its STOP centre
const double k_trailing_slots = 4.0;          // single(): silence after a transmission, at least 4 T
const double k_reference_bandwidth_hz = 2500.0;
const double k_receiver_bandwidth_hz = 2400.0;
const double k_noise_peak_sigmas = 5.0;
const double k_headroom = 0.9;
const int k_byte_bits = 8;
const unsigned k_byte_mask = 0xFFu;
const double k_pi = 3.14159265358979323846;

// slot_config(): bits per peak by T, as the presets.
const double k_k3_below_ms = 12.0;
const double k_k4_below_ms = 24.0;
const double k_k5_below_ms = 48.0;
const double k_k6_below_ms = 96.0;

// channel_delay(): energy envelopes over 16 samples, lags up to 600 samples.
const std::size_t k_envelope_samples = 16;
const long k_max_delay = 600;
const std::size_t k_delay_stride = 3;

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

std::size_t tune_slots(const EncoderConfig& config) {
    const std::size_t slots =
        static_cast<std::size_t>(std::ceil(config.tune_ms * (k_us_per_s / k_ms_per_s) / config.slot_us - 1e-9));
    return std::max<std::size_t>(slots, k_min_tune_slots);
}

std::size_t lead_in_samples(const EncoderConfig& config) {
    return static_cast<std::size_t>(config.lead_in_ms) * config.sample_rate_hz / static_cast<std::size_t>(k_ms_per_s);
}

std::size_t frame_bytes(const Transmission& transmission) {
    return transmission.config.frame_bytes();
}

// Latest frame whose release time (STOP + margin) is not after `sample`, or -1.
long latest_frame(const Transmission& transmission, std::size_t sample) {
    const double margin = k_release_margin_slots * slot_samples(transmission.config);
    long latest = -1;
    for (std::size_t f = 0; f < frame_count(transmission); ++f) {
        if (stop_centre_sample(transmission, f) + margin > static_cast<double>(sample)) break;
        latest = static_cast<long>(f);
    }
    return latest;
}

long transmission_at(const Recording& recording, std::size_t sample) {
    long found = -1;
    for (std::size_t t = 0; t < recording.transmissions.size(); ++t) {
        if (recording.transmissions[t].start_sample <= sample) found = static_cast<long>(t);
    }
    return found;
}

// Offset of the farthest tone from f_ref, Hz (spec 1.3).
double span_hz(const EncoderConfig& config) {
    const double tones = std::pow(2.0, config.bits_per_peak);
    const double c = config.spacing == Spacing::standard
                         ? static_cast<double>(k_standard_spacing_num) / k_standard_spacing_den
                         : 1.0;
    const double units = std::max(k_grid_guard + (tones - 1.0) * c, k_grid_guard + static_cast<double>(k_header_slots));
    return units * k_us_per_s / config.slot_us;
}

// Tukey alpha 0.25 at u in [0, 1).
double peak_weight(double u) {
    const double ramp = k_data_ramp;
    if (u < 0.0 || u >= 1.0) return 0.0;
    if (u < ramp) return std::pow(std::sin(k_pi * u / (2.0 * ramp)), 2.0);
    if (u > 1.0 - ramp) return std::pow(std::sin(k_pi * (1.0 - u) / (2.0 * ramp)), 2.0);
    return 1.0;
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

EncoderConfig mode_config(std::uint32_t slot_ms, std::uint8_t bits_per_peak, std::uint8_t data_slots, Spacing spacing,
                          GridSide side, std::uint16_t tone_hz) {
    EncoderConfig config = EncoderConfig::from_preset(Preset::hf, k_decoder_rate_hz);
    config.slot_us = static_cast<std::uint32_t>(slot_ms * k_us_per_ms);
    config.bits_per_peak = bits_per_peak;
    config.data_slots = data_slots;
    config.spacing = spacing;
    config.side = side;
    const double half = 0.5 * span_hz(config);
    if (tone_hz != 0) {
        config.tone_hz = tone_hz;
        return config;
    }
    // A band wider than 3000 Hz puts f_ref below 0 (or above 3000 Hz): kept in uint16 range, valid() refuses it.
    const double centred =
        side == GridSide::below ? std::ceil(k_band_centre_hz + half) : std::floor(k_band_centre_hz - half);
    const double top = static_cast<double>(std::numeric_limits<std::uint16_t>::max());
    config.tone_hz = static_cast<std::uint16_t>(std::max(0.0, std::min(centred, top)));
    return config;
}

EncoderConfig slot_config(double slot_ms, std::uint16_t tone_hz) {
    const std::uint32_t ms = static_cast<std::uint32_t>(std::lround(slot_ms));
    std::uint8_t k = 7;
    if (ms < k_k3_below_ms) {
        k = 3;
    } else if (ms < k_k4_below_ms) {
        k = 4;
    } else if (ms < k_k5_below_ms) {
        k = 5;
    } else if (ms < k_k6_below_ms) {
        k = 6;
    }
    const GridSide side = tone_hz >= k_band_centre_hz ? GridSide::below : GridSide::above;
    EncoderConfig config = mode_config(ms, k, k_default_data_slots, Spacing::standard, side, tone_hz);
    while (!config.valid() && config.bits_per_peak > 1) --config.bits_per_peak;
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
    return static_cast<double>(transmission.start_sample + lead_in_samples(transmission.config)) +
           static_cast<double>(slot) * slot_samples(transmission.config);
}

std::size_t header_start_slot(const EncoderConfig& config) {
    return tune_slots(config) + config.sync_markers - 1;
}

std::size_t first_frame_slot(const EncoderConfig& config) {
    return header_start_slot(config) + k_header_slots + 1;
}

std::size_t frame_count(const Transmission& transmission) {
    const std::size_t bytes = frame_bytes(transmission);
    return (transmission.data.size() + bytes - 1) / bytes;
}

std::size_t frame_peaks(const Transmission& transmission, std::size_t frame) {
    const std::size_t bytes = frame_bytes(transmission);
    const std::size_t left = transmission.data.size() - frame * bytes;
    if (left >= bytes) return transmission.config.data_slots;
    const std::size_t k = transmission.config.bits_per_peak;
    return (k_byte_bits * left + k - 1) / k;
}

std::size_t frame_start_slot(const Transmission& transmission, std::size_t frame) {
    return first_frame_slot(transmission.config) + frame * (transmission.config.data_slots + 1u);
}

std::size_t frame_of_byte(const Transmission& transmission, std::size_t byte_index) {
    return byte_index / frame_bytes(transmission);
}

double stop_centre_sample(const Transmission& transmission, std::size_t frame) {
    const std::size_t slot = frame_start_slot(transmission, frame) + frame_peaks(transmission, frame) + 1;
    return slot_start_sample(transmission, slot) + 0.5 * slot_samples(transmission.config);
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

long channel_delay(const std::vector<std::int16_t>& samples, sim::ChannelConfig config, std::int16_t amplitude) {
    config.noise = false;
    config.fading = false;
    config.impulse_rate_hz = 0.0;
    config.agc = false;
    const std::vector<std::int16_t> out = through_channel(samples, config, amplitude);
    const std::size_t n = std::min(samples.size(), out.size());
    std::vector<double> a(n, 0.0);
    std::vector<double> b(n, 0.0);
    double sum_a = 0.0;
    double sum_b = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sum_a += static_cast<double>(samples[i]) * samples[i];
        sum_b += static_cast<double>(out[i]) * out[i];
        if (i >= k_envelope_samples) {
            sum_a -= static_cast<double>(samples[i - k_envelope_samples]) * samples[i - k_envelope_samples];
            sum_b -= static_cast<double>(out[i - k_envelope_samples]) * out[i - k_envelope_samples];
        }
        a[i] = sum_a;
        b[i] = sum_b;
    }
    long best = 0;
    double best_correlation = -1.0;
    for (long lag = 0; lag < k_max_delay; ++lag) {
        double correlation = 0.0;
        for (std::size_t i = 0; i + static_cast<std::size_t>(lag) < n; i += k_delay_stride) {
            correlation += a[i] * b[i + static_cast<std::size_t>(lag)];
        }
        if (correlation > best_correlation) {
            best_correlation = correlation;
            best = lag;
        }
    }
    return best;
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
        s.frames_sent += frame_count(recording.transmissions[t]);
        mapping.received.push_back(std::vector<int>(recording.transmissions[t].data.size(), -1));
        mapping.byte_events.push_back(std::vector<Event>(recording.transmissions[t].data.size()));
    }

    std::vector<std::size_t> segment;  // indices of the byte events of the current lock
    const auto close_segment = [&]() {
        if (segment.empty()) return;
        // Frames are released promptly, held ones later: each frame's release time gives an upper bound of the
        // offset; the most common bound (ties: the smaller) is the offset of the lock.
        const long t = transmission_at(recording, capture.event_sample[segment.front()]);
        long offset = 0;
        if (t >= 0) {
            std::vector<long> candidates;
            for (std::size_t i = 0; i < segment.size(); ++i) {
                const Event& e = capture.events[segment[i]];
                const long f = latest_frame(recording.transmissions[t], capture.event_sample[segment[i]]);
                candidates.push_back(f - static_cast<long>(e.frame_index));
            }
            std::sort(candidates.begin(), candidates.end());
            std::size_t best_count = 0;
            for (std::size_t i = 0; i < candidates.size();) {
                std::size_t j = i;
                while (j < candidates.size() && candidates[j] == candidates[i]) ++j;
                if (j - i > best_count) {
                    best_count = j - i;
                    offset = candidates[i];
                }
                i = j;
            }
        }
        for (std::size_t i = 0; i < segment.size(); ++i) {
            const Event& e = capture.events[segment[i]];
            const long frame = static_cast<long>(e.frame_index) + offset;
            const long k = t < 0 ? -1 : frame * static_cast<long>(frame_bytes(recording.transmissions[t])) + e.index;
            if (t < 0 || frame < 0 || k < 0 || k >= static_cast<long>(recording.transmissions[t].data.size()) ||
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
            if ((e.flags & event_flag_late_join) != 0) ++s.late_joins;
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
            break;
        }
    }
    close_segment();
    for (std::size_t t = 0; t < mapping.received.size(); ++t) {
        const Transmission& transmission = recording.transmissions[t];
        for (std::size_t k = 0; k < mapping.received[t].size(); ++k) {
            if (mapping.received[t][k] < 0) ++s.lost_bytes;
        }
        for (std::size_t f = 0; f < frame_count(transmission); ++f) {
            const std::size_t first = f * frame_bytes(transmission);
            const std::size_t last = std::min(first + frame_bytes(transmission), transmission.data.size());
            bool delivered = true;
            for (std::size_t k = first; k < last; ++k) delivered = delivered && mapping.received[t][k] >= 0;
            if (delivered) ++s.frames_delivered;
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

// Double-precision matched Goertzel per peak slot at the sent grid; argmax.
std::vector<std::uint8_t> genie_bytes(const std::vector<std::int16_t>& received, const Transmission& transmission,
                                      double tone_hz, int side, long delay) {
    const EncoderConfig& config = transmission.config;
    const double length = slot_samples(config);
    const double slot_s = config.slot_us / k_us_per_s;
    const double spacing = config.spacing == Spacing::standard
                               ? static_cast<double>(k_standard_spacing_num) / k_standard_spacing_den
                               : 1.0;
    const std::size_t tones = static_cast<std::size_t>(1u) << config.bits_per_peak;
    const std::size_t k = config.bits_per_peak;
    std::vector<double> coefficient(tones);
    for (std::size_t t = 0; t < tones; ++t) {
        const double hz = tone_hz + side * (k_grid_guard + t * spacing) / slot_s;
        coefficient[t] = 2.0 * std::cos(2.0 * k_pi * hz / config.sample_rate_hz);
    }
    std::vector<std::uint8_t> bytes;
    for (std::size_t f = 0; f < frame_count(transmission); ++f) {
        unsigned accumulator = 0;
        std::size_t bits = 0;
        std::size_t produced = 0;
        const std::size_t wanted = std::min<std::size_t>(frame_bytes(transmission), transmission.data.size() - bytes.size());
        for (std::size_t i = 1; i <= frame_peaks(transmission, f); ++i) {
            const double start = slot_start_sample(transmission, frame_start_slot(transmission, f) + i) + delay;
            const long first = static_cast<long>(std::ceil(start));
            const long last = static_cast<long>(std::ceil(start + length));
            std::size_t best = 0;
            double best_energy = -1.0;
            for (std::size_t t = 0; t < tones; ++t) {
                double s1 = 0.0;
                double s2 = 0.0;
                for (long n = first; n < last; ++n) {
                    const double x = n >= 0 && n < static_cast<long>(received.size()) ? received[n] : 0.0;
                    const double s0 = x * peak_weight((n - start) / length) + coefficient[t] * s1 - s2;
                    s2 = s1;
                    s1 = s0;
                }
                const double energy = s1 * s1 + s2 * s2 - coefficient[t] * s1 * s2;
                if (energy > best_energy) {
                    best_energy = energy;
                    best = t;
                }
            }
            const unsigned symbol = peak_symbol(static_cast<std::uint8_t>(best), static_cast<std::uint8_t>(i - 1), config.bits_per_peak);
            for (std::size_t b = 0; b < k && produced < wanted; ++b) {
                accumulator = (accumulator << 1) | ((symbol >> (k - 1 - b)) & 1u);
                if (++bits < static_cast<std::size_t>(k_byte_bits)) continue;
                bytes.push_back(static_cast<std::uint8_t>(accumulator & k_byte_mask));
                ++produced;
                accumulator = 0;
                bits = 0;
            }
        }
    }
    return bytes;
}

}  // namespace loopback
}  // namespace unlimited
