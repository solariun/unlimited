#include "regression.hpp"

#include "resampler.hpp"
#include "test_harness.hpp"
#include "unlimited/packet.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <random>
#include <set>

namespace unlimited {
namespace regression {

using loopback::Capture;
using loopback::Recording;
using loopback::Transmission;

namespace {

const double k_full_scale = 32768.0;
const double k_int16_min = -32768.0;
const double k_int16_max = 32767.0;
const double k_ms_per_s = 1000.0;
const double k_us_per_ms = 1000.0;
const double k_ppm = 1e-6;
const double k_pi = 3.141592653589793;
const double k_two_pi = 2.0 * k_pi;
const double k_reference_bandwidth_hz = 2500.0;
const double k_receiver_bandwidth_hz = 2400.0;
const double k_noise_peak_sigmas = 5.0;
const double k_headroom = 0.9;
const int k_byte_bits = 8;
const std::size_t k_min_tune_slots = 6;
const std::uint32_t k_band_centre = k_band_centre_hz;
const std::uint32_t k_sevenths = k_standard_spacing_den;
const std::uint32_t k_header_top_sevenths = (k_header_slots - 1) * k_standard_spacing_num;
const std::size_t k_header_frame_slots = k_header_slots + 1;  // 8 header peaks and the STOP

// Scoring: a frame is released at least 2 T after its STOP centre (the late step runs at STOP + 2.35 T).
const double k_release_margin_slots = 2.0;
const double k_lock_slot_tolerance_ms = 0.25;  // a `locked` counts when its T is the sent whole-ms T
const double k_lock_window_slots = 4.0;     // ... and it arrives before the end of the transmission + 4 T
const double k_open_interval_end = 1e18;    // a lock still open at the end of the recording
const long k_offset_search = 4;             // frames around the release-time estimate of a segment's offset

// Jobs.
const double k_job_audio_s = 360.0;
const std::size_t k_min_jobs = 8;
const std::uint32_t k_data_seed_stride = 31;

// Scheduling cost per second of audio, relative to the usb channel.
const double k_cost_clean = 0.05;
const double k_cost_usb = 1.0;
const double k_cost_am = 4.0;
const double k_cost_fm = 5.5;
const double k_cost_genie = 0.3;

// Packets of the F5 checks.
const std::size_t k_packet_min_payload = 16;
const std::size_t k_packet_max_test_payload = 64;
const std::size_t k_packet_slack = 16;

// Genie receiver: Tukey alpha 0.25 window (spec 1.1).
const double k_peak_ramp = 0.125;
const double k_ramp_turns = 4.0;  // sin^2(4 pi u) over the ramp
const std::size_t k_envelope_samples = 8;  // 1 ms energy average for the delay search
const std::size_t k_delay_search = 400;

// Confidence bound helpers.
const double k_zero_error_bound = 3.0;  // 95 % upper bound of a Poisson mean after 0 events
const double k_z95 = 1.96;
const double k_percent = 100.0;

std::vector<std::string>& lines() {
    static std::vector<std::string> all;
    return all;
}

struct PacketEntry {
    std::string where;
    std::size_t sent;
    std::size_t ok;
    std::size_t bad;
};

struct RunEntry {
    std::string where;
    std::size_t max_run;
    bool qualifies;
};

std::vector<PacketEntry>& packet_ledger() {
    static std::vector<PacketEntry> all;
    return all;
}

std::vector<RunEntry>& run_ledger() {
    static std::vector<RunEntry> all;
    return all;
}

int popcount(unsigned value) {
    int count = 0;
    for (; value != 0; value &= value - 1) ++count;
    return count;
}

std::uint32_t slot_ms_of(const EncoderConfig& config) {
    return config.slot_us / static_cast<std::uint32_t>(k_us_per_ms);
}

// Distance from f_ref to the farthest data or header tone, in sevenths of 1/T.
std::uint32_t span_sevenths(const EncoderConfig& config) {
    const std::uint32_t spacing = config.spacing == Spacing::standard ? k_standard_spacing_num : k_sevenths;
    const std::uint32_t data = ((1u << config.bits_per_peak) - 1u) * spacing;
    return k_grid_guard * k_sevenths + std::max(data, k_header_top_sevenths);
}

double spacing_of(const EncoderConfig& config) {
    return config.spacing == Spacing::standard
               ? static_cast<double>(k_standard_spacing_num) / k_standard_spacing_den
               : 1.0;
}

double side_of(const EncoderConfig& config) {
    return config.side == GridSide::above ? 1.0 : -1.0;
}

std::size_t tune_slots(const EncoderConfig& config) {
    const std::size_t tune_us = static_cast<std::size_t>(config.tune_ms) * static_cast<std::size_t>(k_us_per_ms);
    return std::max<std::size_t>((tune_us + config.slot_us - 1) / config.slot_us, k_min_tune_slots);
}

std::size_t peak_slots(std::size_t bytes, std::size_t bits_per_peak) {
    return (bytes * k_byte_bits + bits_per_peak - 1) / bits_per_peak;
}

// Slot layout of one transmission as the decoder receives it (spec 2.1), stretched by the clock errors.
class Layout {
public:
    Layout(const Transmission& tx, double stretch) : config_(tx.config), data_(&tx.data) {
        begin_ = static_cast<double>(tx.start_sample) * stretch;
        length_ = static_cast<double>(tx.length) * stretch;
        slot_ = static_cast<double>(config_.slot_us) * config_.sample_rate_hz / (k_us_per_ms * k_ms_per_s) * stretch;
        first_slot_ = begin_ + static_cast<double>(config_.lead_in_ms) * config_.sample_rate_hz / k_ms_per_s * stretch;
        header_start_ = tune_slots(config_) + config_.sync_markers - 1;
        frame_bytes_ = config_.frame_bytes();
        frames_ = (data_->size() + frame_bytes_ - 1) / frame_bytes_;
    }

    const EncoderConfig& config() const {
        return config_;
    }
    const std::vector<std::uint8_t>& data() const {
        return *data_;
    }
    double begin() const {
        return begin_;
    }
    double end() const {
        return begin_ + length_;
    }
    double slot_samples() const {
        return slot_;
    }
    std::size_t frames() const {
        return frames_;
    }
    std::size_t frame_bytes() const {
        return frame_bytes_;
    }
    std::size_t bytes_in_frame(std::size_t frame) const {
        return std::min(frame_bytes_, data_->size() - frame * frame_bytes_);
    }
    std::size_t frame_peaks(std::size_t frame) const {
        return peak_slots(bytes_in_frame(frame), config_.bits_per_peak);
    }
    double slot_start(std::size_t slot) const {
        return first_slot_ + static_cast<double>(slot) * slot_;
    }
    std::size_t frame_start_slot(std::size_t frame) const {  // START marker of data frame `frame`
        return header_start_ + k_header_frame_slots + frame * (config_.data_slots + 1u);
    }
    double stop_centre(std::size_t frame) const {
        return slot_start(frame_start_slot(frame) + frame_peaks(frame) + 1) + 0.5 * slot_;
    }
    long latest_frame(double sample) const {  // last frame released by `sample`, or -1
        long latest = -1;
        for (std::size_t f = 0; f < frames_; ++f) {
            if (stop_centre(f) + k_release_margin_slots * slot_ > sample) break;
            latest = static_cast<long>(f);
        }
        return latest;
    }

private:
    EncoderConfig config_;
    const std::vector<std::uint8_t>* data_;
    double begin_ = 0.0;
    double length_ = 0.0;
    double slot_ = 0.0;
    double first_slot_ = 0.0;
    std::size_t header_start_ = 0;
    std::size_t frame_bytes_ = 1;
    std::size_t frames_ = 0;
};

long transmission_at(const std::vector<Layout>& sent, double sample) {
    long found = -1;
    for (std::size_t t = 0; t < sent.size(); ++t) {
        if (sent[t].begin() <= sample) found = static_cast<long>(t);
    }
    return found;
}

struct Scored {
    Score score;
    std::vector<bool> correct;       // per event: a byte event that maps to a sent byte of the same value
    std::vector<long> transmission;  // per event: the transmission and frame it maps to, -1 when unmapped
    std::vector<long> frame;
    std::vector<std::vector<bool> > taken;  // per transmission and byte
};

// Maps every byte event to a sent byte, byte = (frame_index + shift) * B + index, one shift per lock segment.
// The release times give a first estimate: a frame is released after its STOP, so `latest_frame - frame_index`
// is the shift plus the release latency in frames; the most frequent value is taken. The shift within
// +-k_offset_search frames of it that matches the most bytes is then used (late joins number frames from the
// lock, not from the header), so the scoring does not depend on the decoder's latency.
Scored score_capture(const std::vector<Layout>& sent, const Capture& capture) {
    Scored scored;
    Score& s = scored.score;
    scored.correct.assign(capture.events.size(), false);
    scored.transmission.assign(capture.events.size(), -1);
    scored.frame.assign(capture.events.size(), -1);
    scored.taken.resize(sent.size());
    for (std::size_t t = 0; t < sent.size(); ++t) {
        scored.taken[t].assign(sent[t].data().size(), false);
        s.bytes_sent += sent[t].data().size();
    }
    std::vector<std::size_t> segment;
    const auto close_segment = [&]() {
        if (segment.empty()) return;
        const long t = transmission_at(sent, static_cast<double>(capture.event_sample[segment.front()]));
        long shift = 0;
        const auto position = [&](const Event& e, long frame_shift) {
            const long frame = static_cast<long>(e.frame_index) + frame_shift;
            if (t < 0 || frame < 0 || frame >= static_cast<long>(sent[t].frames())) return -1L;
            if (e.index >= sent[t].bytes_in_frame(static_cast<std::size_t>(frame))) return -1L;
            return frame * static_cast<long>(sent[t].frame_bytes()) + e.index;
        };
        if (t >= 0) {
            std::vector<long> candidates;
            for (std::size_t i = 0; i < segment.size(); ++i) {
                const long f = sent[t].latest_frame(static_cast<double>(capture.event_sample[segment[i]]));
                candidates.push_back(f - static_cast<long>(capture.events[segment[i]].frame_index));
            }
            std::sort(candidates.begin(), candidates.end());
            std::size_t best_count = 0;
            for (std::size_t i = 0; i < candidates.size();) {
                std::size_t j = i;
                while (j < candidates.size() && candidates[j] == candidates[i]) ++j;
                if (j - i > best_count) {
                    best_count = j - i;
                    shift = candidates[i];
                }
                i = j;
            }
            const std::vector<std::uint8_t>& data = sent[t].data();
            const auto matches = [&](long frame_shift) {
                std::size_t count = 0;
                for (std::size_t i = 0; i < segment.size(); ++i) {
                    const Event& e = capture.events[segment[i]];
                    const long k = position(e, frame_shift);
                    if (k >= 0 && data[static_cast<std::size_t>(k)] == e.value) ++count;
                }
                return count;
            };
            const long estimate = shift;
            std::size_t best_matches = matches(estimate);
            for (long candidate = estimate - k_offset_search; candidate <= estimate + k_offset_search; ++candidate) {
                const std::size_t count = matches(candidate);
                if (count > best_matches) {
                    best_matches = count;
                    shift = candidate;
                }
            }
        }
        for (std::size_t i = 0; i < segment.size(); ++i) {
            const Event& e = capture.events[segment[i]];
            const long k = position(e, shift);
            if (k < 0 || scored.taken[t][static_cast<std::size_t>(k)]) {
                ++s.extra_bytes;
                continue;
            }
            scored.taken[t][static_cast<std::size_t>(k)] = true;
            ++s.matched;
            const std::uint8_t value = sent[t].data()[static_cast<std::size_t>(k)];
            const int errors = popcount(static_cast<unsigned>(e.value ^ value));
            s.bit_errors += static_cast<std::size_t>(errors);
            if (errors != 0) ++s.wrong_bytes;
            scored.correct[segment[i]] = errors == 0;
            scored.transmission[segment[i]] = t;
            scored.frame[segment[i]] = static_cast<long>(e.frame_index) + shift;
        }
        segment.clear();
    };
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        const Event& e = capture.events[i];
        switch (e.type) {
        case EventType::byte:
            ++s.bytes_released;
            if ((e.flags & (event_flag_flywheel_start | event_flag_flywheel_stop)) != 0) ++s.flywheel_bytes;
            if ((e.flags & event_flag_erasure) != 0) ++s.erasure_bytes;
            if ((e.flags & event_flag_mode_memory) != 0) ++s.mode_memory_bytes;
            segment.push_back(i);
            break;
        case EventType::locked:
            close_segment();
            ++s.locks;
            if ((e.flags & event_flag_late_join) != 0) ++s.late_joins;
            break;
        case EventType::lost:
            close_segment();
            ++s.lost_events;
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
    for (std::size_t t = 0; t < scored.taken.size(); ++t) {
        for (std::size_t k = 0; k < scored.taken[t].size(); ++k) s.lost_bytes += scored.taken[t][k] ? 0 : 1;
    }
    return scored;
}

struct PacketSink {
    const std::set<std::vector<std::uint8_t> >* sent;
    std::size_t ok;
    std::size_t bad;
};

// Deduces the size type of PacketHandler, so this compiles for 8- and 16-bit LEN alike.
template <typename Size>
void on_packet(const std::uint8_t* payload, Size size, std::uint8_t flags, void* context) {
    (void)flags;
    PacketSink* sink = static_cast<PacketSink*>(context);
    const std::vector<std::uint8_t> packet(payload, payload + size);
    if (sink->sent->count(packet) != 0) {
        ++sink->ok;
    } else {
        ++sink->bad;
    }
}

bool inverted(const JobPlan& job) {
    return job.use_channel && job.channel.mode == sim::Mode::lsb;
}

bool mode_matches(const Event& e, const EncoderConfig& config, bool lsb) {
    const double true_ms = static_cast<double>(slot_ms_of(config));
    const int side = static_cast<int>(side_of(config)) * (lsb ? -1 : 1);
    return e.bits_per_peak == config.bits_per_peak && e.data_slots == config.data_slots &&
           e.spacing == config.spacing && e.side == side &&
           std::fabs(e.slot_ms - true_ms) <= k_lock_slot_tolerance_ms;
}

double peak_window(double u) {
    if (u < k_peak_ramp) return std::pow(std::sin(k_ramp_turns * k_pi * u), 2);
    if (u > 1.0 - k_peak_ramp) return std::pow(std::sin(k_ramp_turns * k_pi * (1.0 - u)), 2);
    return 1.0;
}

// Genie receiver (A5, spec 4.1 bench): known slot timing and channel delay, received f_ref and orientation;
// matched Tukey alpha 0.25 window, argmax over the M grid tones, spec 1.5 mapping, every data byte counted.
void genie_count(const std::vector<Layout>& sent, const std::vector<std::int16_t>& samples, const JobPlan& job,
                 Outcome& outcome) {
    const bool lsb = inverted(job);
    const double delay = static_cast<double>(ssb_delay_samples(job.channel.mode));
    for (std::size_t t = 0; t < sent.size(); ++t) {
        const Layout& layout = sent[t];
        const EncoderConfig& config = layout.config();
        const std::size_t k = config.bits_per_peak;
        const std::size_t tones = std::size_t(1) << k;
        const double ref = lsb ? job.channel.lsb_pivot_hz - config.tone_hz + job.channel.freq_offset_hz
                               : config.tone_hz + job.channel.freq_offset_hz;
        const double side = side_of(config) * (lsb ? -1.0 : 1.0);
        const double unit_hz = k_ms_per_s / static_cast<double>(slot_ms_of(config));
        std::vector<double> coefficient(tones);
        for (std::size_t n = 0; n < tones; ++n) {
            const double hz = ref + side * (k_grid_guard + static_cast<double>(n) * spacing_of(config)) * unit_hz;
            coefficient[n] = 2.0 * std::cos(k_two_pi * hz / k_decoder_rate_hz);
        }
        const std::size_t length = static_cast<std::size_t>(std::lround(layout.slot_samples()));
        std::vector<double> weight(length);
        for (std::size_t m = 0; m < length; ++m) weight[m] = peak_window(static_cast<double>(m) / length);
        std::vector<double> windowed(length);
        for (std::size_t f = 0; f < layout.frames(); ++f) {
            std::vector<std::uint8_t> bytes;
            std::uint32_t accumulator = 0;
            std::size_t pending = 0;
            for (std::size_t i = 1; i <= layout.frame_peaks(f); ++i) {
                const long start = std::lround(layout.slot_start(layout.frame_start_slot(f) + i) + delay);
                for (std::size_t m = 0; m < length; ++m) {
                    const long n = start + static_cast<long>(m);
                    const bool inside = n >= 0 && n < static_cast<long>(samples.size());
                    windowed[m] = inside ? weight[m] * samples[static_cast<std::size_t>(n)] : 0.0;
                }
                std::size_t best = 0;
                double best_energy = -1.0;
                for (std::size_t n = 0; n < tones; ++n) {
                    double s1 = 0.0;
                    double s2 = 0.0;
                    for (std::size_t m = 0; m < length; ++m) {
                        const double s0 = windowed[m] + coefficient[n] * s1 - s2;
                        s2 = s1;
                        s1 = s0;
                    }
                    const double energy = s1 * s1 + s2 * s2 - coefficient[n] * s1 * s2;
                    if (energy > best_energy) {
                        best_energy = energy;
                        best = n;
                    }
                }
                accumulator = (accumulator << k) | peak_symbol(static_cast<std::uint8_t>(best),
                                                               static_cast<std::uint8_t>(i - 1),
                                                               static_cast<std::uint8_t>(k));
                pending += k;
                while (pending >= static_cast<std::size_t>(k_byte_bits)) {
                    pending -= k_byte_bits;
                    bytes.push_back(static_cast<std::uint8_t>(accumulator >> pending));
                }
            }
            for (std::size_t b = 0; b < layout.bytes_in_frame(f); ++b) {
                const std::uint8_t sent_byte = layout.data()[f * layout.frame_bytes() + b];
                outcome.genie_errors += static_cast<std::size_t>(popcount(static_cast<unsigned>(bytes[b] ^ sent_byte)));
                outcome.genie_bits += k_byte_bits;
            }
        }
    }
}

Outcome evaluate(const std::vector<Layout>& sent, const Capture& capture, const JobPlan& job) {
    Outcome outcome;
    const Scored scored = score_capture(sent, capture);
    outcome.score = scored.score;
    outcome.transmissions = sent.size();
    for (std::size_t t = 0; t < sent.size(); ++t) {
        for (std::size_t f = 0; f < sent[t].frames(); ++f) {
            bool all = true;
            for (std::size_t b = 0; b < sent[t].bytes_in_frame(f); ++b) {
                all = all && scored.taken[t][f * sent[t].frame_bytes() + b];
            }
            ++outcome.frames_sent;
            outcome.frames_delivered += all ? 1 : 0;
        }
    }

    // F6: longest run of wrong or unmapped bytes in release order (a correct byte or `end` breaks it).
    std::size_t run = 0;
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        const Event& e = capture.events[i];
        if (e.type == EventType::end) run = 0;
        if (e.type != EventType::byte) continue;
        run = scored.correct[i] ? 0 : run + 1;
        outcome.max_wrong_run = std::max(outcome.max_wrong_run, run);
    }

    // A3': a `locked` with the sent mode during each transmission; locked airtime; release latency; A4 SNR.
    const bool lsb = inverted(job);
    std::vector<bool> locked(sent.size(), false);
    std::vector<bool> header(sent.size(), false);
    std::vector<bool> wrong_header(sent.size(), false);
    const auto during = [&](double sample) {
        long found = -1;
        for (std::size_t t = 0; t < sent.size(); ++t) {
            const double to = sent[t].end() + k_lock_window_slots * sent[t].slot_samples();
            if (sample >= sent[t].begin() && sample <= to) found = static_cast<long>(t);
        }
        return found;
    };
    long open_from = -1;
    std::vector<std::pair<double, double> > intervals;
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        const Event& e = capture.events[i];
        const double sample = static_cast<double>(capture.event_sample[i]);
        switch (e.type) {
        case EventType::locked: {
            if (open_from < 0) open_from = static_cast<long>(capture.event_sample[i]);
            const long t = during(sample);
            if (t < 0) {
                ++outcome.stray_locks;
            } else if (mode_matches(e, job.transmissions[static_cast<std::size_t>(t)].config, lsb)) {
                locked[static_cast<std::size_t>(t)] = true;
            } else {
                ++outcome.wrong_mode_locks;
            }
            break;
        }
        case EventType::slot: {
            const long t = during(sample);
            if (t < 0) break;
            if (mode_matches(e, job.transmissions[static_cast<std::size_t>(t)].config, lsb)) {
                header[static_cast<std::size_t>(t)] = true;
            } else {
                wrong_header[static_cast<std::size_t>(t)] = true;
            }
            break;
        }
        case EventType::lost:
            ++outcome.lost_reasons[static_cast<std::size_t>(e.reason)];
            if (open_from >= 0) intervals.push_back(std::make_pair(static_cast<double>(open_from), sample));
            open_from = -1;
            break;
        case EventType::end:
            if (open_from >= 0) intervals.push_back(std::make_pair(static_cast<double>(open_from), sample));
            open_from = -1;
            break;
        case EventType::byte:
            outcome.snr_db.push_back(e.snr_db);
            if (scored.correct[i]) {
                const Layout& layout = sent[static_cast<std::size_t>(scored.transmission[i])];
                const std::size_t frame = static_cast<std::size_t>(scored.frame[i]);
                const double latency = (sample - layout.stop_centre(frame)) / layout.slot_samples();
                outcome.latency_sum += latency;
                outcome.latency_max = std::max(outcome.latency_max, latency);
                ++outcome.latency_events;
            }
            break;
        case EventType::state:
            break;
        }
    }
    if (open_from >= 0) intervals.push_back(std::make_pair(static_cast<double>(open_from), k_open_interval_end));
    for (std::size_t t = 0; t < sent.size(); ++t) {
        outcome.locked_transmissions += locked[t] ? 1 : 0;
        outcome.header_transmissions += header[t] ? 1 : 0;
        outcome.wrong_header_transmissions += wrong_header[t] ? 1 : 0;
        if (sent[t].frames() == 0) continue;
        const double from = sent[t].slot_start(sent[t].frame_start_slot(0));
        const double to = sent[t].stop_centre(sent[t].frames() - 1);
        outcome.airtime += to - from;
        for (std::size_t n = 0; n < intervals.size(); ++n) {
            const double overlap = std::min(to, intervals[n].second) - std::max(from, intervals[n].first);
            outcome.locked_airtime += std::max(0.0, overlap);
        }
    }

    // F5: CRC-valid packets that were never sent.
    std::set<std::vector<std::uint8_t> > packets;
    for (std::size_t t = 0; t < job.transmissions.size(); ++t) {
        for (std::size_t p = 0; p < job.transmissions[t].packets.size(); ++p) {
            packets.insert(job.transmissions[t].packets[p]);
            ++outcome.packets_sent;
        }
    }
    PacketSink sink = {&packets, 0, 0};
    PacketReader reader(&on_packet, &sink);
    for (std::size_t i = 0; i < capture.events.size(); ++i) reader.on_event(capture.events[i]);
    outcome.packets_ok = sink.ok;
    outcome.packets_bad = sink.bad;
    return outcome;
}

std::string rule_line(const std::string& id, const std::string& condition, const std::string& measured,
                      const std::string& gate, const char* verdict) {
    return "RESULT | " + id + " | " + condition + " | " + measured + " | " + gate + " | " + verdict;
}

std::size_t measure_delay(sim::Mode mode) {
    const EncoderConfig config = EncoderConfig::from_preset(Preset::hf, k_decoder_rate_hz);
    Recording recording;
    loopback::append_silence(recording, k_quiet_ms / 4.0);
    loopback::append_transmission(recording, loopback::random_bytes(config.frame_bytes(), 1), config);
    loopback::append_silence(recording, k_quiet_ms / 4.0);
    sim::ChannelConfig channel;
    channel.mode = mode;
    channel.noise = false;
    const std::vector<std::int16_t> out = apply_channel(recording.samples, channel, config.amplitude, 1.0);
    const auto envelope = [](const std::vector<std::int16_t>& x) {
        std::vector<double> e(x.size(), 0.0);
        double sum = 0.0;
        for (std::size_t n = 0; n < x.size(); ++n) {
            sum += static_cast<double>(x[n]) * x[n];
            if (n >= k_envelope_samples) {
                const double old = x[n - k_envelope_samples];
                sum -= old * old;
            }
            e[n] = sum;
        }
        return e;
    };
    const std::vector<double> ex = envelope(recording.samples);
    const std::vector<double> ey = envelope(out);
    std::size_t best = 0;
    double best_value = -1.0;
    for (std::size_t d = 0; d < k_delay_search; ++d) {
        double value = 0.0;
        for (std::size_t n = d; n < ey.size(); ++n) value += ey[n] * ex[n - d];
        if (value > best_value) {
            best_value = value;
            best = d;
        }
    }
    return best;
}

}  // namespace

std::size_t worker_count() {
    const unsigned cores = std::thread::hardware_concurrency();
    return cores == 0 ? 1 : cores;
}

double Score::ber() const {
    return matched == 0 ? 0.0 : static_cast<double>(bit_errors) / (k_byte_bits * static_cast<double>(matched));
}

double Score::loss() const {
    return bytes_sent == 0 ? 0.0 : static_cast<double>(lost_bytes) / static_cast<double>(bytes_sent);
}

double Outcome::frames_ratio() const {
    return frames_sent == 0 ? 0.0 : static_cast<double>(frames_delivered) / static_cast<double>(frames_sent);
}

bool delivered(const Outcome& outcome) {
    return outcome.frames_ratio() >= k_min_delivered;
}

double Outcome::genie_ber() const {
    return genie_bits == 0 ? 0.0 : static_cast<double>(genie_errors) / static_cast<double>(genie_bits);
}

double JobPlan::cost() const {
    double seconds = (lead_ms + tail_ms) / k_ms_per_s;
    for (std::size_t t = 0; t < transmissions.size(); ++t) {
        seconds += transmission_seconds(transmissions[t].config, transmissions[t].data.size(), gap_ms);
    }
    double factor = k_cost_clean;
    if (use_channel) {
        switch (channel.mode) {
        case sim::Mode::usb:
        case sim::Mode::lsb:
            factor = k_cost_usb;
            break;
        case sim::Mode::am:
            factor = k_cost_am;
            break;
        case sim::Mode::fm:
            factor = k_cost_fm;
            break;
        case sim::Mode::clean:
            break;
        }
    }
    const double decoding = k_cost_clean * static_cast<double>(decoders.size()) + (genie ? k_cost_genie : 0.0);
    return seconds * (factor + decoding);
}

void merge(Outcome& into, const Outcome& from) {
    Score& a = into.score;
    const Score& b = from.score;
    a.bytes_sent += b.bytes_sent;
    a.bytes_released += b.bytes_released;
    a.matched += b.matched;
    a.wrong_bytes += b.wrong_bytes;
    a.bit_errors += b.bit_errors;
    a.lost_bytes += b.lost_bytes;
    a.extra_bytes += b.extra_bytes;
    a.locks += b.locks;
    a.late_joins += b.late_joins;
    a.lost_events += b.lost_events;
    a.ends += b.ends;
    a.flywheel_bytes += b.flywheel_bytes;
    a.erasure_bytes += b.erasure_bytes;
    a.mode_memory_bytes += b.mode_memory_bytes;
    into.frames_sent += from.frames_sent;
    into.frames_delivered += from.frames_delivered;
    into.max_wrong_run = std::max(into.max_wrong_run, from.max_wrong_run);
    into.transmissions += from.transmissions;
    into.locked_transmissions += from.locked_transmissions;
    into.wrong_mode_locks += from.wrong_mode_locks;
    into.stray_locks += from.stray_locks;
    into.header_transmissions += from.header_transmissions;
    into.wrong_header_transmissions += from.wrong_header_transmissions;
    for (std::size_t r = 0; r < k_lost_reasons; ++r) into.lost_reasons[r] += from.lost_reasons[r];
    into.packets_sent += from.packets_sent;
    into.packets_ok += from.packets_ok;
    into.packets_bad += from.packets_bad;
    into.snr_db.insert(into.snr_db.end(), from.snr_db.begin(), from.snr_db.end());
    into.airtime += from.airtime;
    into.locked_airtime += from.locked_airtime;
    into.latency_events += from.latency_events;
    into.latency_sum += from.latency_sum;
    into.latency_max = std::max(into.latency_max, from.latency_max);
    into.genie_bits += from.genie_bits;
    into.genie_errors += from.genie_errors;
}

TxPlan random_tx(const EncoderConfig& config, std::size_t bytes, std::uint32_t seed) {
    TxPlan tx;
    tx.config = config;
    tx.data = loopback::random_bytes(bytes, seed);
    return tx;
}

TxPlan packet_tx(const EncoderConfig& config, std::size_t bytes, std::uint32_t seed) {
    TxPlan tx;
    tx.config = config;
    std::mt19937 generator(seed);
    std::uniform_int_distribution<std::size_t> length(k_packet_min_payload, k_packet_max_test_payload);
    std::uniform_int_distribution<int> value(0, 0xFF);
    while (tx.data.size() < bytes) {
        std::vector<std::uint8_t> payload(length(generator));
        for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(value(generator));
        std::vector<std::uint8_t> packet(payload.size() + k_packet_slack);
        const std::size_t size = packet_build(payload.data(), static_cast<std::uint16_t>(payload.size()), packet.data(),
                                              packet.size());
        if (size == 0) break;
        tx.data.insert(tx.data.end(), packet.begin(), packet.begin() + static_cast<long>(size));
        tx.packets.push_back(payload);
    }
    return tx;
}

EncoderConfig mode_config(Preset base, std::uint32_t slot_ms, std::uint8_t bits_per_peak, std::uint8_t data_slots,
                          Spacing spacing, GridSide side) {
    EncoderConfig config = EncoderConfig::from_preset(base, k_decoder_rate_hz);
    config.slot_us = slot_ms * static_cast<std::uint32_t>(k_us_per_ms);
    config.bits_per_peak = bits_per_peak;
    config.data_slots = data_slots;
    config.spacing = spacing;
    config.side = side;
    // W / 2 = span_sevenths * 1000 / (14 T_ms), rounded away from the band centre.
    const std::uint32_t numerator = span_sevenths(config) * static_cast<std::uint32_t>(k_ms_per_s);
    const std::uint32_t denominator = 2u * k_sevenths * slot_ms;
    const std::uint32_t half = (numerator + denominator - 1) / denominator;
    config.tone_hz = static_cast<std::uint16_t>(side == GridSide::below ? k_band_centre + half : k_band_centre - half);
    return config;
}

std::string mode_name(const EncoderConfig& config) {
    return format("T%u k%u N%u %s %s f_ref %u", slot_ms_of(config), config.bits_per_peak, config.data_slots,
                  config.spacing == Spacing::standard ? "std" : "dense",
                  config.side == GridSide::below ? "below" : "above", config.tone_hz);
}

double data_tone_hz(const EncoderConfig& config, double tone) {
    return config.tone_hz + side_of(config) * (k_grid_guard + tone * spacing_of(config)) * k_ms_per_s /
                                slot_ms_of(config);
}

double header_tone_hz(const EncoderConfig& config, double tone) {
    const double spacing = static_cast<double>(k_standard_spacing_num) / k_standard_spacing_den;
    return config.tone_hz + side_of(config) * (k_grid_guard + tone * spacing) * k_ms_per_s / slot_ms_of(config);
}

double span_hz(const EncoderConfig& config) {
    return static_cast<double>(span_sevenths(config)) * k_ms_per_s / (k_sevenths * slot_ms_of(config));
}

std::vector<std::int16_t> apply_channel(const std::vector<std::int16_t>& samples, sim::ChannelConfig config,
                                        std::int16_t amplitude, double peak_factor) {
    config.signal_level = amplitude / k_full_scale;
    double sigma = 0.0;
    if (config.noise) {
        const double tone_power = 0.5 * config.signal_level * config.signal_level;
        const double density = tone_power / (std::pow(10.0, config.snr_db / 10.0) * k_reference_bandwidth_hz);
        sigma = std::sqrt(density * k_receiver_bandwidth_hz);
    }
    const double peak = peak_factor * config.signal_level + k_noise_peak_sigmas * sigma;
    config.output_gain = std::min(1.0, k_headroom / peak);
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

std::size_t ssb_delay_samples(sim::Mode mode) {
    static const std::size_t usb = measure_delay(sim::Mode::usb);
    static const std::size_t lsb = measure_delay(sim::Mode::lsb);
    return mode == sim::Mode::lsb ? lsb : usb;
}

double snr_for_fm_cnr(double cnr_db, const sim::ChannelConfig& config) {
    return cnr_db + 10.0 * std::log10(config.fm_if_bandwidth_hz / k_reference_bandwidth_hz);
}

double snr_for_am_cnr(double cnr_db, const sim::ChannelConfig& config) {
    return cnr_db + 10.0 * std::log10(config.am_if_bandwidth_hz / k_reference_bandwidth_hz);
}

std::uint32_t seed_of(std::uint32_t test, std::uint32_t point, std::uint32_t job) {
    const std::uint32_t k_test_stride = 1000003u;
    const std::uint32_t k_point_stride = 10007u;
    return test * k_test_stride + point * k_point_stride + job + 1u;
}

std::vector<Outcome> run_job(const JobPlan& job) {
    Recording recording;
    loopback::append_silence(recording, job.lead_ms);
    for (std::size_t t = 0; t < job.transmissions.size(); ++t) {
        loopback::append_transmission(recording, job.transmissions[t].data, job.transmissions[t].config);
        loopback::append_silence(recording, t + 1 < job.transmissions.size() ? job.gap_ms : job.tail_ms);
    }
    const std::int16_t amplitude =
        job.transmissions.empty() ? EncoderConfig().amplitude : job.transmissions[0].config.amplitude;
    std::vector<std::int16_t> samples =
        job.use_channel ? apply_channel(recording.samples, job.channel, amplitude, job.peak_factor) : recording.samples;
    if (job.rx_ppm != 0.0) {
        std::vector<float> in(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) in[i] = static_cast<float>(samples[i]);
        const std::vector<float> out =
            pc::resample(in, k_decoder_rate_hz, k_decoder_rate_hz * (1.0 + job.rx_ppm * k_ppm));
        samples.resize(out.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            const double value = std::round(static_cast<double>(out[i]));
            samples[i] = static_cast<std::int16_t>(std::max(k_int16_min, std::min(k_int16_max, value)));
        }
    }

    // Clock errors stretch the received time line by (1 + rx) / (1 + tx): score against a stretched layout.
    const double tx_ppm = job.use_channel ? job.channel.clock_ppm : 0.0;
    const double stretch = (1.0 + job.rx_ppm * k_ppm) / (1.0 + tx_ppm * k_ppm);
    std::vector<Layout> sent;
    for (std::size_t t = 0; t < recording.transmissions.size(); ++t) {
        sent.push_back(Layout(recording.transmissions[t], stretch));
    }

    Outcome genie;
    if (job.genie) genie_count(sent, samples, job, genie);
    std::vector<Outcome> outcomes;
    for (std::size_t d = 0; d < job.decoders.size(); ++d) {
        const Capture capture = loopback::run_decoder(samples, job.decoders[d], 0);
        outcomes.push_back(evaluate(sent, capture, job));
        outcomes.back().genie_bits = genie.genie_bits;
        outcomes.back().genie_errors = genie.genie_errors;
    }
    return outcomes;
}

std::vector<std::vector<Outcome> > run_points(const std::vector<JobPlan>& jobs, const std::vector<std::size_t>& point,
                                              std::size_t points) {
    std::vector<std::size_t> order(jobs.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::vector<double> cost(jobs.size());
    for (std::size_t i = 0; i < jobs.size(); ++i) cost[i] = jobs[i].cost();
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return cost[a] > cost[b]; });
    const std::vector<std::vector<Outcome> > per_job =
        parallel_map<std::vector<Outcome> >(jobs.size(), [&](std::size_t i) { return run_job(jobs[i]); }, order);
    std::vector<std::vector<Outcome> > merged(points);
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        std::vector<Outcome>& target = merged[point[i]];
        if (target.size() < per_job[i].size()) target.resize(per_job[i].size());
        for (std::size_t d = 0; d < per_job[i].size(); ++d) merge(target[d], per_job[i][d]);
    }
    return merged;
}

std::size_t transmissions_for(double bits, std::size_t bytes) {
    return static_cast<std::size_t>(std::ceil(bits / (k_byte_bits * static_cast<double>(bytes))));
}

double transmission_seconds(const EncoderConfig& config, std::size_t bytes, double gap_ms) {
    const std::size_t peaks = peak_slots(bytes, config.bits_per_peak);
    const std::size_t frames = (peaks + config.data_slots - 1) / config.data_slots;
    const std::size_t eot = bytes == 0 ? 0 : k_eot_markers;
    const std::size_t slots =
        tune_slots(config) + config.sync_markers + k_header_frame_slots + peaks + frames + eot;
    const double slot_s = static_cast<double>(config.slot_us) / (k_us_per_ms * k_ms_per_s);
    return (config.lead_in_ms + config.tail_ms + gap_ms) / k_ms_per_s + static_cast<double>(slots) * slot_s;
}

std::size_t transmissions_per_job(double transmission_s, std::size_t count) {
    const std::size_t by_audio = std::max<std::size_t>(1, static_cast<std::size_t>(k_job_audio_s / transmission_s));
    return std::min(by_audio, std::max<std::size_t>(1, (count + k_min_jobs - 1) / k_min_jobs));
}

std::uint32_t data_seed(std::uint32_t job_seed, std::size_t transmission) {
    return job_seed * k_data_seed_stride + static_cast<std::uint32_t>(transmission);
}

std::string profile_name(Profile profile) {
    switch (profile) {
    case Profile::ssb:
        return "ssb";
    case Profile::am:
        return "am";
    case Profile::fm:
        return "fm";
    }
    return "?";
}

bool result(const std::string& id, const std::string& condition, const std::string& measured,
            const std::string& gate, bool pass, Kind kind) {
    const char* verdict = kind == Kind::report ? "REPORT" : (pass ? "PASS" : "FAIL");
    const std::string text = rule_line(id, condition, measured, gate, verdict);
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
    lines().push_back(text);
    if (kind == Kind::gate && !pass) test::fail(__FILE__, __LINE__, id + " failed: " + condition);
    return pass;
}

void note(const std::string& text) {
    std::printf("    note: %s\n", text.c_str());
    std::fflush(stdout);
}

std::string format(const char* pattern, ...) {
    const std::size_t k_buffer = 1024;
    char buffer[k_buffer];
    va_list arguments;
    va_start(arguments, pattern);
    std::vsnprintf(buffer, sizeof(buffer), pattern, arguments);
    va_end(arguments);
    return std::string(buffer);
}

std::string lost_text(const Outcome& o) {
    const std::size_t* r = o.lost_reasons;
    return format("lost-ev %zu (gone %zu, alias %zu, no_header %zu, unsupported %zu, preamble %zu)",
                  o.score.lost_events, r[static_cast<std::size_t>(LostReason::signal_gone)],
                  r[static_cast<std::size_t>(LostReason::alias)], r[static_cast<std::size_t>(LostReason::no_header)],
                  r[static_cast<std::size_t>(LostReason::unsupported_mode)],
                  r[static_cast<std::size_t>(LostReason::preamble_timeout)]);
}

std::string ber_text(const Outcome& o) {
    const Score& s = o.score;
    std::string text = format("BER %.2e (%zu bits), frames %.2f%% (%zu/%zu), loss %.2f%%, wrong %zu, extra %zu, "
                              "headers %zu/%zu tx, locks %zu/%zu tx",
                              s.ber(), s.matched * k_byte_bits, k_percent * o.frames_ratio(), o.frames_delivered,
                              o.frames_sent, k_percent * s.loss(), s.wrong_bytes, s.extra_bytes,
                              o.header_transmissions, o.transmissions, o.locked_transmissions, o.transmissions);
    if (o.wrong_mode_locks + o.stray_locks != 0) {
        text += format(" (+%zu wrong-mode, %zu stray)", o.wrong_mode_locks, o.stray_locks);
    }
    if (s.late_joins + s.mode_memory_bytes != 0) {
        text += format(", late joins %zu, mode-memory bytes %zu", s.late_joins, s.mode_memory_bytes);
    }
    return text + ", " + lost_text(o);
}

double upper_95(std::size_t errors, double trials) {
    if (trials <= 0.0) return 1.0;
    if (errors == 0) return k_zero_error_bound / trials;
    const double k = static_cast<double>(errors);
    return (k + k_z95 * std::sqrt(k) + k_z95 * k_z95 / 2.0) / trials;
}

void ledger_packets(const std::string& where, const Outcome& outcome) {
    packet_ledger().push_back(PacketEntry{where, outcome.packets_sent, outcome.packets_ok, outcome.packets_bad});
}

void ledger_runs(const std::string& where, const Outcome& outcome, bool at_gate_plus_3) {
    run_ledger().push_back(RunEntry{where, outcome.max_wrong_run, at_gate_plus_3});
}

void test_f5_packets() {
    const std::vector<PacketEntry>& ledger = packet_ledger();
    REQUIRE(!ledger.empty());
    std::size_t sent = 0;
    std::size_t ok = 0;
    std::size_t bad = 0;
    for (std::size_t i = 0; i < ledger.size(); ++i) {
        sent += ledger[i].sent;
        ok += ledger[i].ok;
        bad += ledger[i].bad;
        if (ledger[i].bad == 0) continue;
        note(format("F5: %s: %zu CRC-valid wrong packets", ledger[i].where.c_str(), ledger[i].bad));
    }
    result("F5", format("F1-F4 and every C point (%zu runs)", ledger.size()),
           format("%zu CRC-valid wrong packets; %zu of %zu sent packets delivered", bad, ok, sent),
           "0 CRC-valid wrong packets", bad == 0);
}

void test_f6_runs() {
    const std::vector<RunEntry>& ledger = run_ledger();
    REQUIRE(!ledger.empty());
    std::size_t count[2] = {0, 0};  // [0]: at >= gate + 3 dB, [1]: below
    std::size_t worst[2] = {0, 0};
    std::string where[2] = {"-", "-"};
    for (std::size_t i = 0; i < ledger.size(); ++i) {
        const int group = ledger[i].qualifies ? 0 : 1;
        ++count[group];
        if (where[group] == "-" || ledger[i].max_run > worst[group]) {
            worst[group] = ledger[i].max_run;
            where[group] = ledger[i].where;
        }
    }
    const std::size_t k_max_run = 8;
    result("F6", format("%zu points of L5/A/C at >= gate + 3 dB", count[0]),
           format("longest run of wrong bytes %zu (%s)", worst[0], where[0].c_str()), "<= 8", worst[0] <= k_max_run);
    result("F6", format("%zu points below gate + 3 dB", count[1]),
           format("longest run of wrong bytes %zu (%s)", worst[1], where[1].c_str()), "report only", true,
           Kind::report);
}

void print_summary() {
    std::printf("\n==== long regression summary (%zu result lines) ====\n", lines().size());
    for (std::size_t i = 0; i < lines().size(); ++i) std::printf("%s\n", lines()[i].c_str());
}

}  // namespace regression
}  // namespace unlimited
