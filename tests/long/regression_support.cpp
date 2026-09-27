#include "regression.hpp"

#include "resampler.hpp"
#include "test_harness.hpp"
#include "unlimited/packet.hpp"

#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <set>

namespace unlimited {
namespace regression {

using loopback::Capture;
using loopback::Recording;
using loopback::Score;
using loopback::Transmission;

namespace {

const double k_full_scale = 32768.0;
const double k_int16_min = -32768.0;
const double k_int16_max = 32767.0;
const double k_ms_per_s = 1000.0;
const double k_us_per_ms = 1000.0;
const double k_ppm = 1e-6;
const double k_two_pi = 6.283185307179586;
const double k_reference_bandwidth_hz = 2500.0;
const double k_receiver_bandwidth_hz = 2400.0;
const double k_noise_peak_sigmas = 5.0;
const double k_headroom = 0.9;
const double k_db_per_decade = 20.0;
const double k_infinite_ratio = 1e9;
const int k_byte_bits = 8;
const unsigned k_byte_values = 256;
const unsigned k_msb = 7;

// Spec 4.1 release gates and the receiver windows.
const double k_gate_4_ms = 8.0;
const double k_gate_128_ms = -6.5;
const double k_gate_16_ms = 1.5;
const double k_gate_reference_ms = 16.0;
const double k_db_per_decade_power = 10.0;
const double k_fast_slot_ms = 8.0;        // below it only the fm profile hears T
const double k_ssb_max_slot_ms = 64.0;    // ssb window 8..64 ms
const std::uint8_t k_slow_min_slot_ms = 16;  // window 16..128 ms

// Scoring.
const double k_lock_slot_tolerance = 0.03;  // a `locked` counts when its T is within 3 % and its N is the sent N
const double k_lock_window_slots = 4.0;     // ... and it arrives before the end of the transmission + 4 T
const double k_open_interval_end = 1e18;    // a lock still open at the end of the recording

// Jobs.
const double k_job_audio_s = 360.0;
const std::size_t k_min_jobs = 8;
const std::uint32_t k_data_seed_stride = 31;
const std::uint32_t k_test_stride = 1000003u;
const std::uint32_t k_point_stride = 10007u;

// Scheduling cost per second of audio, relative to the usb channel.
const double k_cost_clean = 0.05;
const double k_cost_usb = 1.0;
const double k_cost_am = 4.0;
const double k_cost_fm = 5.5;
const double k_cost_resample = 1.0;
const double k_cost_decoder = 0.05;
const double k_cost_genie = 0.3;

// Packets of the F5 checks.
const std::size_t k_packet_min_payload = 16;
const std::size_t k_packet_max_test_payload = 64;
const std::size_t k_packet_slack = 16;

// Genie receiver (spec 3.10 formulas with known timing and tone).
const double k_marker_half = 0.35;
const double k_slot_window = 0.75;
const double k_gap_window = 0.15;
const double k_slot_centre = 0.5;
const double k_g_slot = 0.9394;
const double k_g_marker = 0.8355;
const double k_rho_min = 0.50;
const double k_rho_max = 0.75;
const double k_rho_seed = 0.6;
const int k_rho_iterations = 3;
const double k_floor_sigma = 2.6;
const double k_data_noise_scale = 4.0;    // N_a = 4 sigma^2 / (n_d g_s^2)
const double k_marker_noise_scale = 2.0;  // N_m = 2 sigma^2 / (n_W g_m^2)

// Confidence bound helpers.
const double k_zero_error_bound = 3.0;  // 95 % upper bound of a Poisson mean after 0 events
const double k_z95 = 1.96;
const double k_percent = 100.0;
const std::size_t k_max_wrong_run = 8;  // F6
// A lock segment is misplaced when at least half of its bytes (and twice as many as at its own numbering) match
// the sent data at another byte offset; segments shorter than this are not judged.
const std::size_t k_misplaced_min_bytes = 4;
const std::size_t k_misplaced_ratio = 2;

std::mutex& lines_mutex() {
    static std::mutex mutex;
    return mutex;
}

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
    std::size_t misplaced_segments;
    std::size_t misplaced_bytes;
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

// Where one transmission lies in the received audio: the recording's layout stretched by the clock errors and
// delayed by the channel.
class Layout {
public:
    Layout(const Transmission& tx, double stretch, double delay)
        : tx_(&tx),
          stretch_(stretch),
          delay_(delay),
          slot_(loopback::slot_samples(tx.config) * stretch),
          packages_(loopback::package_count(tx)) {}

    const EncoderConfig& config() const {
        return tx_->config;
    }
    const Transmission& tx() const {
        return *tx_;
    }
    const std::vector<std::uint8_t>& data() const {
        return tx_->data;
    }
    double begin() const {
        return stretch_ * static_cast<double>(tx_->start_sample) + delay_;
    }
    double end() const {
        return stretch_ * static_cast<double>(tx_->start_sample + tx_->length) + delay_;
    }
    double slot() const {
        return slot_;
    }
    double slot_ms() const {
        return slot_ms_of(tx_->config) * stretch_;
    }
    std::size_t packages() const {
        return packages_;
    }
    double slot_start(std::size_t slot) const {
        return stretch_ * loopback::slot_start_sample(*tx_, slot) + delay_;
    }
    // End of the STOP slot of the package that holds the byte's last bit: no receiver can release it earlier.
    double byte_ready(std::size_t byte) const {
        const std::size_t last_bit = byte * k_byte_bits + k_msb;
        const std::size_t package = last_bit / tx_->config.bits_per_package;
        return slot_start(loopback::stop_slot(*tx_, package) + 1);
    }
    double airtime_begin() const {
        return slot_start(loopback::first_start_slot(tx_->config));
    }
    double airtime_end() const {
        return packages_ == 0 ? airtime_begin() : slot_start(loopback::stop_slot(*tx_, packages_ - 1) + 1);
    }

private:
    const Transmission* tx_;
    double stretch_;
    double delay_;
    double slot_;
    std::size_t packages_;
};

long transmission_at(const std::vector<Layout>& sent, double sample) {
    long found = -1;
    for (std::size_t t = 0; t < sent.size(); ++t) {
        if (sent[t].begin() <= sample) found = static_cast<long>(t);
    }
    return found;
}

// A cold join counts byte_index from the join (spec 3.12 rule 5): the offset where most of its bytes match, the
// smallest one on a tie. Votes per offset from the positions of each received value in the sent data.
long best_offset(const std::vector<std::uint8_t>& data, const Capture& capture,
                 const std::vector<std::size_t>& segment) {
    const long size = static_cast<long>(data.size());
    std::vector<std::vector<long> > where(k_byte_values);
    for (long k = 0; k < size; ++k) where[data[static_cast<std::size_t>(k)]].push_back(k);
    std::vector<std::size_t> votes(static_cast<std::size_t>(size + 1 + size), 0);  // offsets -size..size
    for (std::size_t i = 0; i < segment.size(); ++i) {
        const Event& e = capture.events[segment[i]];
        const std::vector<long>& positions = where[e.value];
        for (std::size_t p = 0; p < positions.size(); ++p) {
            const long offset = positions[p] - static_cast<long>(e.byte_index);
            if (offset < -size || offset > size) continue;
            ++votes[static_cast<std::size_t>(offset + size)];
        }
    }
    long best = 0;
    std::size_t best_votes = votes[static_cast<std::size_t>(size)];
    for (long offset = -size; offset <= size; ++offset) {
        const std::size_t v = votes[static_cast<std::size_t>(offset + size)];
        if (v > best_votes || (v == best_votes && std::labs(offset) < std::labs(best))) {
            best = offset;
            best_votes = v;
        }
    }
    return best;
}

struct Scored {
    Score score;
    std::size_t cold_joins = 0;
    std::size_t acausal = 0;
    std::size_t misplaced_segments = 0;  // bytes at a wrong byte_index: they match far better at another offset
    std::size_t misplaced_bytes = 0;
    std::vector<char> correct;       // per event: a byte event mapped to a sent byte of the same value
    std::vector<long> transmission;  // per event: the transmission and byte it maps to, -1 when unmapped
    std::vector<long> byte;
};

// How a lock numbers its packages and bytes (spec 3.12, 3.13).
enum class Numbering {
    absolute,  // a lock from the preamble: byte_index 0 is the transmission's first byte
    joined,    // a cold join (late_join, package_index 0): counted from the join
    inherited  // a relock from the station memory: continues the numbering of the lock it remembers
};

// Maps every byte event to a sent byte. The bytes between a `locked` and its `end`/`lost` belong to the
// transmission on air when the first of them came, at their byte_index plus the offset of their numbering: 0 for a
// lock from the preamble; for a cold join the offset where most of its bytes match; a relock keeps the offset of the
// previous lock in the same transmission (it continues that numbering), or is matched like a cold join when there is
// none. A byte is acausal when it was released before the STOP of its package was received (a misplaced offset).
Scored score_capture(const std::vector<Layout>& sent, const Capture& capture) {
    Scored scored;
    Score& s = scored.score;
    const std::size_t events = capture.events.size();
    scored.correct.assign(events, 0);
    scored.transmission.assign(events, -1);
    scored.byte.assign(events, -1);
    std::vector<std::vector<char> > taken(sent.size());
    for (std::size_t t = 0; t < sent.size(); ++t) {
        taken[t].assign(sent[t].data().size(), 0);
        s.bytes_sent += sent[t].data().size();
    }
    std::vector<std::size_t> segment;
    Numbering numbering = Numbering::absolute;
    long numbered_transmission = -1;  // the transmission and offset of the newest numbering with bytes
    long numbered_offset = 0;
    const auto close_segment = [&]() {
        if (segment.empty()) return;
        const long t = transmission_at(sent, static_cast<double>(capture.event_sample[segment.front()]));
        long offset = 0;
        if (t >= 0 && numbering != Numbering::absolute) {
            if (numbering == Numbering::inherited && numbered_transmission == t) {
                offset = numbered_offset;
            } else {
                offset = best_offset(sent[static_cast<std::size_t>(t)].data(), capture, segment);
            }
            if (offset != 0) ++s.shifted_segments;
        }
        numbered_transmission = t;
        numbered_offset = offset;
        if (t >= 0 && segment.size() >= k_misplaced_min_bytes) {
            // Where the numbering puts the bytes against where they really are (spec 3.13: never a shift).
            const std::vector<std::uint8_t>& data = sent[static_cast<std::size_t>(t)].data();
            const long best = best_offset(data, capture, segment);
            std::size_t at_offset = 0;
            std::size_t at_best = 0;
            for (std::size_t i = 0; i < segment.size(); ++i) {
                const Event& e = capture.events[segment[i]];
                const long k = static_cast<long>(e.byte_index) + offset;
                const long b = static_cast<long>(e.byte_index) + best;
                if (k >= 0 && k < static_cast<long>(data.size()) && data[static_cast<std::size_t>(k)] == e.value) {
                    ++at_offset;
                }
                if (b >= 0 && b < static_cast<long>(data.size()) && data[static_cast<std::size_t>(b)] == e.value) ++at_best;
            }
            if (best != offset && at_best * k_misplaced_ratio >= segment.size() &&
                at_best >= k_misplaced_ratio * at_offset) {
                ++scored.misplaced_segments;
                scored.misplaced_bytes += segment.size();
            }
        }
        for (std::size_t i = 0; i < segment.size(); ++i) {
            const Event& e = capture.events[segment[i]];
            const long k = static_cast<long>(e.byte_index) + offset;
            if (t < 0 || k < 0 || k >= static_cast<long>(sent[static_cast<std::size_t>(t)].data().size()) ||
                taken[static_cast<std::size_t>(t)][static_cast<std::size_t>(k)] != 0) {
                ++s.extra_bytes;
                continue;
            }
            const Layout& layout = sent[static_cast<std::size_t>(t)];
            taken[static_cast<std::size_t>(t)][static_cast<std::size_t>(k)] = 1;
            ++s.matched;
            const int errors = popcount(static_cast<unsigned>(e.value ^ layout.data()[static_cast<std::size_t>(k)]));
            s.bit_errors += static_cast<std::size_t>(errors);
            if (errors != 0) ++s.wrong_bytes;
            scored.correct[segment[i]] = errors == 0 ? 1 : 0;
            scored.transmission[segment[i]] = t;
            scored.byte[segment[i]] = k;
            if (static_cast<double>(capture.event_sample[segment[i]]) < layout.byte_ready(static_cast<std::size_t>(k))) {
                ++scored.acausal;
            }
        }
        segment.clear();
    };
    for (std::size_t i = 0; i < events; ++i) {
        const Event& e = capture.events[i];
        switch (e.type) {
        case EventType::byte:
            ++s.bytes_released;
            if ((e.flags & (event_flag_flywheel_start | event_flag_flywheel_stop)) != 0) ++s.flywheel_bytes;
            segment.push_back(i);
            break;
        case EventType::locked: {
            close_segment();
            ++s.locks;
            const bool late = (e.flags & event_flag_late_join) != 0;
            if (late) ++s.late_joins;
            numbering = !late ? Numbering::absolute
                              : (e.package_index == 0 ? Numbering::joined : Numbering::inherited);
            if (numbering == Numbering::joined) ++scored.cold_joins;
            break;
        }
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
    for (std::size_t t = 0; t < taken.size(); ++t) {
        for (std::size_t k = 0; k < taken[t].size(); ++k) s.lost_bytes += taken[t][k] != 0 ? 0 : 1;
    }
    return scored;
}

struct PacketSink {
    const std::set<std::vector<std::uint8_t> >* sent;
    std::size_t ok;
    std::size_t bad;
};

void on_packet(const std::uint8_t* payload, std::uint16_t size, std::uint8_t flags, void* context) {
    (void)flags;
    PacketSink* sink = static_cast<PacketSink*>(context);
    const std::vector<std::uint8_t> packet(payload, payload + size);
    if (sink->sent->count(packet) != 0) {
        ++sink->ok;
    } else {
        ++sink->bad;
    }
}

double equal_likelihood_ratio(double a2) {
    if (a2 <= 0.0 || k_two_pi * a2 * k_rho_max <= 1.0) return k_rho_max;
    double rho = k_rho_seed;
    for (int i = 0; i < k_rho_iterations; ++i) {
        rho = 0.5 + std::log(k_two_pi * a2 * rho) / (2.0 * a2);
        rho = std::max(k_rho_min, std::min(k_rho_max, rho));
    }
    return rho;
}

bool stream_bit(const std::vector<std::uint8_t>& data, std::size_t bit) {
    return ((data[bit / k_byte_bits] >> (k_msb - bit % k_byte_bits)) & 1u) != 0;
}

// Known timing and tone (spec 4.2 ablation): slot levels over the central 0.75 T, START/STOP crests by the
// flip-compensated matched filter, the noise from the slot edges between two data zeros, then the smart line of
// spec 3.10 against three references: the START-STOP line, the START crest alone and one fixed level (the RMS
// crest of the transmission).
void genie_count(const std::vector<Layout>& sent, const std::vector<std::int16_t>& samples, double tone_hz,
                 Outcome& outcome) {
    typedef std::complex<double> Complex;
    for (std::size_t t = 0; t < sent.size(); ++t) {
        const Layout& layout = sent[t];
        const Transmission& tx = layout.tx();
        const std::size_t n = tx.config.bits_per_package;
        const std::size_t packages = layout.packages();
        if (packages == 0) continue;
        const double slot = layout.slot();
        const std::size_t first = loopback::first_start_slot(tx.config);
        const std::size_t last = loopback::stop_slot(tx, packages - 1);
        const long begin = std::max(0L, static_cast<long>(std::floor(layout.slot_start(first))));
        const long end = std::min(static_cast<long>(samples.size()),
                                  static_cast<long>(std::ceil(layout.slot_start(last + 1))) + 1);
        if (end <= begin) continue;
        std::vector<Complex> prefix(static_cast<std::size_t>(end - begin + 1), Complex(0.0, 0.0));
        const double step = k_two_pi * tone_hz / k_decoder_rate_hz;
        for (long i = begin; i < end; ++i) {
            const double phase = step * static_cast<double>(i);
            prefix[static_cast<std::size_t>(i - begin + 1)] =
                prefix[static_cast<std::size_t>(i - begin)] +
                static_cast<double>(samples[static_cast<std::size_t>(i)]) * Complex(std::cos(phase), -std::sin(phase));
        }
        const double span = static_cast<double>(end - begin);
        const auto index = [&](double position) {
            const double local = position - static_cast<double>(begin);
            return static_cast<std::size_t>(std::max(0.0, std::min(span, std::floor(local + 0.5))));
        };
        const auto window = [&](double from, double to, double& count) {
            const std::size_t a = index(from);
            const std::size_t b = index(to);
            count = static_cast<double>(b - a);
            return prefix[b] - prefix[a];
        };
        const auto crest_at = [&](std::size_t marker_slot) {
            const double centre = layout.slot_start(marker_slot) + k_slot_centre * slot;
            double n_before = 0.0;
            double n_after = 0.0;
            const Complex before = window(centre - k_marker_half * slot, centre, n_before);
            const Complex after = window(centre, centre + k_marker_half * slot, n_after);
            const double count = 0.5 * (n_before + n_after);
            return count > 0.0 ? std::abs(before - after) / (count * k_g_marker) : 0.0;
        };
        const auto bit_of = [&](std::size_t package, std::size_t i) {  // i = 1..d
            return stream_bit(tx.data, package * n + i - 1);
        };

        std::vector<double> crest(packages + 1);
        for (std::size_t k = 0; k < packages; ++k) crest[k] = crest_at(loopback::package_start_slot(tx, k));
        crest[packages] = crest_at(last);
        double noise_sum = 0.0;
        std::size_t noise_windows = 0;
        for (std::size_t k = 0; k < packages; ++k) {
            const std::size_t d = loopback::package_bits(tx, k);
            for (std::size_t i = 1; i < d; ++i) {
                if (bit_of(k, i) || bit_of(k, i + 1)) continue;
                const double edge = layout.slot_start(loopback::package_start_slot(tx, k) + i + 1);
                double count = 0.0;
                const Complex s = window(edge - k_slot_centre * k_gap_window * slot,
                                         edge + k_slot_centre * k_gap_window * slot, count);
                if (count <= 0.0) continue;
                noise_sum += std::norm(s) / count;
                ++noise_windows;
            }
        }
        if (noise_windows == 0) continue;
        const double sigma2 = noise_sum / static_cast<double>(noise_windows);
        const double data_samples = k_slot_window * slot;
        const double marker_samples = k_marker_half * slot;
        const double noise_data = k_data_noise_scale * sigma2 / (data_samples * k_g_slot * k_g_slot);
        const double noise_marker = k_marker_noise_scale * sigma2 / (marker_samples * k_g_marker * k_g_marker);
        double crest_power = 0.0;
        for (std::size_t k = 0; k <= packages; ++k) crest_power += crest[k] * crest[k];
        const double fixed_level = std::sqrt(crest_power / static_cast<double>(packages + 1));

        for (std::size_t k = 0; k < packages; ++k) {
            const std::size_t d = loopback::package_bits(tx, k);
            for (std::size_t i = 1; i <= d; ++i) {
                const double centre =
                    layout.slot_start(loopback::package_start_slot(tx, k) + i) + k_slot_centre * slot;
                double count = 0.0;
                const Complex s =
                    window(centre - k_slot_centre * data_samples, centre + k_slot_centre * data_samples, count);
                const double level = count > 0.0 ? 2.0 * std::abs(s) / (count * k_g_slot) : 0.0;
                const double position = static_cast<double>(i) / static_cast<double>(d + 1);
                const double references[genie_rules] = {crest[k] + (crest[k + 1] - crest[k]) * position, crest[k],
                                                        fixed_level};
                for (int rule = 0; rule < genie_rules; ++rule) {
                    const double reference = references[rule];
                    const double a2 = 2.0 * std::max(reference * reference - noise_marker, 0.0) / noise_data;
                    const double threshold =
                        std::max(equal_likelihood_ratio(a2) * reference, k_floor_sigma * std::sqrt(noise_data));
                    if ((level >= threshold) != bit_of(k, i)) ++outcome.genie_errors[rule];
                }
                ++outcome.genie_bits;
            }
        }
    }
}

Outcome evaluate(const std::vector<Layout>& sent, const Capture& capture, const JobPlan& job) {
    Outcome outcome;
    const Scored scored = score_capture(sent, capture);
    outcome.score = scored.score;
    outcome.cold_joins = scored.cold_joins;
    outcome.acausal_bytes = scored.acausal;
    outcome.misplaced_segments = scored.misplaced_segments;
    outcome.misplaced_bytes = scored.misplaced_bytes;
    outcome.transmissions = sent.size();

    // F6: longest run of wrong or unmapped bytes in release order (a correct byte or `end` breaks it).
    std::size_t run = 0;
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        const Event& e = capture.events[i];
        if (e.type == EventType::end) run = 0;
        if (e.type != EventType::byte) continue;
        run = scored.correct[i] != 0 ? 0 : run + 1;
        outcome.max_wrong_run = std::max(outcome.max_wrong_run, run);
    }

    // A3: a `locked` with the sent T and N for each transmission (the one on air when it came; later than 4 T after
    // its end it is also counted late); C3: locked airtime; L5: T accuracy; A4: SNR.
    std::vector<bool> locked(sent.size(), false);
    long open_from = -1;
    std::vector<std::pair<double, double> > intervals;
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        const Event& e = capture.events[i];
        const double sample = static_cast<double>(capture.event_sample[i]);
        switch (e.type) {
        case EventType::locked: {
            if (open_from < 0) open_from = static_cast<long>(capture.event_sample[i]);
            const long t = transmission_at(sent, sample);
            if (t < 0) {
                ++outcome.stray_locks;
                break;
            }
            const Layout& layout = sent[static_cast<std::size_t>(t)];
            const bool slot_ok = std::fabs(e.slot_ms / layout.slot_ms() - 1.0) <= k_lock_slot_tolerance;
            if (slot_ok && e.bits_per_package == layout.config().bits_per_package) {
                locked[static_cast<std::size_t>(t)] = true;
                if (sample > layout.end() + k_lock_window_slots * layout.slot()) ++outcome.late_locks;
            } else {
                ++outcome.wrong_locks;
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
            if (scored.correct[i] != 0) {
                const Layout& layout = sent[static_cast<std::size_t>(scored.transmission[i])];
                const double error = std::fabs(e.slot_ms / layout.slot_ms() - 1.0);
                outcome.slot_error_sum += error;
                outcome.worst_slot_error = std::max(outcome.worst_slot_error, error);
                ++outcome.slot_events;
                const double latency =
                    (sample - layout.byte_ready(static_cast<std::size_t>(scored.byte[i]))) / layout.slot();
                outcome.latency_sum += latency;
                outcome.latency_max = std::max(outcome.latency_max, latency);
            }
            break;
        case EventType::state:
        case EventType::slot:
        case EventType::package:
            break;
        }
    }
    if (open_from >= 0) intervals.push_back(std::make_pair(static_cast<double>(open_from), k_open_interval_end));
    for (std::size_t t = 0; t < sent.size(); ++t) {
        outcome.locked_transmissions += locked[t] ? 1 : 0;
        if (sent[t].packages() == 0) continue;
        const double from = sent[t].airtime_begin();
        const double to = sent[t].airtime_end();
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

// The columns are separated by '|': a text may never hold one.
std::string column(std::string text) {
    std::replace(text.begin(), text.end(), '|', '/');
    return text;
}

double received_tone(const JobPlan& job, const EncoderConfig& config) {
    if (!job.use_channel || job.channel.mode == sim::Mode::clean) return config.tone_hz;
    if (job.channel.mode == sim::Mode::lsb) {
        return job.channel.lsb_pivot_hz - config.tone_hz + job.channel.freq_offset_hz;
    }
    return config.tone_hz + job.channel.freq_offset_hz;
}

}  // namespace

std::size_t worker_count() {
    const unsigned cores = std::thread::hardware_concurrency();
    return cores == 0 ? 1 : cores;
}

EncoderConfig preset(Preset p) {
    return EncoderConfig::from_preset(p, k_decoder_rate_hz);
}

EncoderConfig slot_preset(double slot_ms, std::uint8_t bits) {
    return loopback::slot_config(slot_ms, bits);
}

double slot_ms_of(const EncoderConfig& config) {
    return static_cast<double>(config.slot_us) / k_us_per_ms;
}

DecoderConfig receiver_for(const EncoderConfig& config) {
    const double slot_ms = slot_ms_of(config);
    if (slot_ms < k_fast_slot_ms) return DecoderConfig::for_profile(Profile::fm);
    DecoderConfig receiver = DecoderConfig::for_profile(Profile::ssb);
    if (slot_ms > k_ssb_max_slot_ms) receiver.min_slot_ms = k_slow_min_slot_ms;
    return receiver;
}

double gate_db(double slot_ms) {
    const double k_tolerance_ms = 1e-6;
    if (std::fabs(slot_ms - k_fast_slot_ms / 2.0) < k_tolerance_ms) return k_gate_4_ms;
    if (std::fabs(slot_ms - 2.0 * k_ssb_max_slot_ms) < k_tolerance_ms) return k_gate_128_ms;
    return k_gate_16_ms - k_db_per_decade_power * std::log10(slot_ms / k_gate_reference_ms);
}

std::string preset_name(Preset p) {
    switch (p) {
    case Preset::hf_slow:
        return "hf_slow";
    case Preset::hf:
        return "hf";
    case Preset::hf_fast:
        return "hf_fast";
    case Preset::am:
        return "am";
    case Preset::fm:
        return "fm";
    }
    return "?";
}

std::string config_text(const EncoderConfig& config) {
    return format("T=%g ms N=%u %u Hz", slot_ms_of(config), config.bits_per_package, config.tone_hz);
}

std::string receiver_text(const DecoderConfig& config) {
    return format("receiver %u..%u ms, %u-%u Hz%s", config.min_slot_ms, config.max_slot_ms(), config.passband.low_hz,
                  config.passband.high_hz, config.decision_mode == DecisionMode::fixed_ratio ? ", fixed line" : "");
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
        if (channel.clock_ppm != 0.0) factor += k_cost_resample;
    }
    if (rx_ppm != 0.0) factor += k_cost_resample;
    const double decoding = k_cost_decoder * static_cast<double>(decoders.size()) + (genie ? k_cost_genie : 0.0);
    return seconds * (factor + decoding);
}

double Outcome::delivered() const {
    return score.bytes_sent == 0 ? 0.0 : static_cast<double>(score.matched) / static_cast<double>(score.bytes_sent);
}

double Outcome::correct() const {
    return score.bytes_sent == 0 ? 0.0
                                 : static_cast<double>(score.matched - score.wrong_bytes) /
                                       static_cast<double>(score.bytes_sent);
}

double Outcome::mean_slot_error() const {
    return slot_events == 0 ? 1.0 : slot_error_sum / static_cast<double>(slot_events);
}

double Outcome::genie_ber(GenieRule rule) const {
    return genie_bits == 0 ? 0.0 : static_cast<double>(genie_errors[rule]) / static_cast<double>(genie_bits);
}

bool delivered(const Outcome& outcome) {
    return outcome.delivered() >= k_min_delivered;
}

bool near_zero_errors(const Outcome& outcome) {
    return outcome.score.ber() <= k_near_zero_ber && outcome.score.extra_bytes == 0 && outcome.misplaced_bytes == 0;
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
    a.alias_losts += b.alias_losts;
    a.ends += b.ends;
    a.flywheel_bytes += b.flywheel_bytes;
    a.shifted_segments += b.shifted_segments;
    into.cold_joins += from.cold_joins;
    into.acausal_bytes += from.acausal_bytes;
    into.misplaced_segments += from.misplaced_segments;
    into.misplaced_bytes += from.misplaced_bytes;
    for (std::size_t r = 0; r < k_lost_reasons; ++r) into.lost_reasons[r] += from.lost_reasons[r];
    into.max_wrong_run = std::max(into.max_wrong_run, from.max_wrong_run);
    into.transmissions += from.transmissions;
    into.locked_transmissions += from.locked_transmissions;
    into.wrong_locks += from.wrong_locks;
    into.stray_locks += from.stray_locks;
    into.late_locks += from.late_locks;
    into.packets_sent += from.packets_sent;
    into.packets_ok += from.packets_ok;
    into.packets_bad += from.packets_bad;
    into.snr_db.insert(into.snr_db.end(), from.snr_db.begin(), from.snr_db.end());
    into.airtime += from.airtime;
    into.locked_airtime += from.locked_airtime;
    into.slot_error_sum += from.slot_error_sum;
    into.worst_slot_error = std::max(into.worst_slot_error, from.worst_slot_error);
    into.slot_events += from.slot_events;
    into.latency_sum += from.latency_sum;
    into.latency_max = std::max(into.latency_max, from.latency_max);
    into.genie_bits += from.genie_bits;
    for (int rule = 0; rule < genie_rules; ++rule) into.genie_errors[rule] += from.genie_errors[rule];
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

std::vector<std::int16_t> apply_channel(const std::vector<std::int16_t>& samples, sim::ChannelConfig config,
                                        std::int16_t amplitude, double peak_factor) {
    config.signal_level = amplitude / k_full_scale;
    double sigma = 0.0;
    if (config.noise) {
        const double tone_power = 0.5 * config.signal_level * config.signal_level;
        const double density =
            tone_power / (std::pow(10.0, config.snr_db / k_db_per_decade_power) * k_reference_bandwidth_hz);
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

double channel_delay(sim::Mode mode) {
    static const double usb = loopback::channel_delay_samples(sim::Mode::usb);
    static const double lsb = loopback::channel_delay_samples(sim::Mode::lsb);
    static const double am = loopback::channel_delay_samples(sim::Mode::am);
    static const double fm = loopback::channel_delay_samples(sim::Mode::fm);
    switch (mode) {
    case sim::Mode::usb:
        return usb;
    case sim::Mode::lsb:
        return lsb;
    case sim::Mode::am:
        return am;
    case sim::Mode::fm:
        return fm;
    case sim::Mode::clean:
        break;
    }
    return 0.0;
}

double snr_for_fm_cnr(double cnr_db, const sim::ChannelConfig& config) {
    return cnr_db + k_db_per_decade_power * std::log10(config.fm_if_bandwidth_hz / k_reference_bandwidth_hz);
}

double snr_for_am_cnr(double cnr_db, const sim::ChannelConfig& config) {
    return cnr_db + k_db_per_decade_power * std::log10(config.am_if_bandwidth_hz / k_reference_bandwidth_hz);
}

double amplitude_of_db(double db) {
    return std::pow(10.0, db / k_db_per_decade);
}

double ratio(double a, double b) {
    if (b > 0.0) return a / b;
    return a > 0.0 ? k_infinite_ratio : 1.0;
}

std::uint32_t seed_of(std::uint32_t test, std::uint32_t point, std::uint32_t job) {
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

    // Clock errors stretch the received time line by (1 + rx) / (1 + tx); the channel delays it.
    const double tx_ppm = job.use_channel ? job.channel.clock_ppm : 0.0;
    const double stretch = (1.0 + job.rx_ppm * k_ppm) / (1.0 + tx_ppm * k_ppm);
    const double delay = job.use_channel ? channel_delay(job.channel.mode) : 0.0;
    std::vector<Layout> sent;
    for (std::size_t t = 0; t < recording.transmissions.size(); ++t) {
        sent.push_back(Layout(recording.transmissions[t], stretch, delay));
    }

    Outcome genie;
    if (job.genie && !job.transmissions.empty()) {
        genie_count(sent, samples, received_tone(job, job.transmissions[0].config), genie);
    }
    std::vector<Outcome> outcomes;
    for (std::size_t d = 0; d < job.decoders.size(); ++d) {
        const Capture capture = loopback::run_decoder(samples, job.decoders[d], 0);
        outcomes.push_back(evaluate(sent, capture, job));
        outcomes.back().genie_bits = genie.genie_bits;
        for (int rule = 0; rule < genie_rules; ++rule) outcomes.back().genie_errors[rule] = genie.genie_errors[rule];
    }
    return outcomes;
}

Outcome evaluate_recording(const Recording& recording, const Capture& capture, double shift) {
    std::vector<Layout> sent;
    for (std::size_t t = 0; t < recording.transmissions.size(); ++t) {
        sent.push_back(Layout(recording.transmissions[t], 1.0, shift));
    }
    return evaluate(sent, capture, JobPlan());
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
    Encoder encoder(config);
    const double samples = static_cast<double>(encoder.duration_samples(bytes));
    return samples / config.sample_rate_hz + gap_ms / k_ms_per_s;
}

std::size_t transmissions_per_job(double transmission_s, std::size_t count) {
    const std::size_t by_audio = std::max<std::size_t>(1, static_cast<std::size_t>(k_job_audio_s / transmission_s));
    return std::min(by_audio, std::max<std::size_t>(1, (count + k_min_jobs - 1) / k_min_jobs));
}

std::uint32_t data_seed(std::uint32_t job_seed, std::size_t transmission) {
    return job_seed * k_data_seed_stride + static_cast<std::uint32_t>(transmission);
}

bool result(const std::string& id, const std::string& condition, const std::string& measured,
            const std::string& gate, bool pass, Kind kind) {
    const char* verdict = kind == Kind::report ? "REPORT" : (pass ? "PASS" : "FAIL");
    const std::string text = rule_line(column(id), column(condition), column(measured), column(gate), verdict);
    {
        std::lock_guard<std::mutex> lock(lines_mutex());
        std::printf("%s\n", text.c_str());
        std::fflush(stdout);
        lines().push_back(text);
    }
    if (kind == Kind::gate && !pass) test::fail(__FILE__, __LINE__, id + " failed: " + condition);
    return pass;
}

void note(const std::string& text) {
    std::printf("    note: %s\n", text.c_str());
    std::fflush(stdout);
}

std::string format(const char* pattern, ...) {
    va_list arguments;
    va_start(arguments, pattern);
    va_list copy;
    va_copy(copy, arguments);
    const int length = std::vsnprintf(0, 0, pattern, copy);
    va_end(copy);
    std::string text(static_cast<std::size_t>(std::max(length, 0)) + 1, '\0');
    std::vsnprintf(&text[0], text.size(), pattern, arguments);
    va_end(arguments);
    text.resize(static_cast<std::size_t>(std::max(length, 0)));
    return text;
}

std::string lost_text(const Outcome& o) {
    const std::size_t* r = o.lost_reasons;
    return format("lost-ev %zu (gone %zu, alias %zu, preamble %zu, unsupported %zu)", o.score.lost_events,
                  r[static_cast<std::size_t>(LostReason::signal_gone)], r[static_cast<std::size_t>(LostReason::alias)],
                  r[static_cast<std::size_t>(LostReason::preamble_timeout)],
                  r[static_cast<std::size_t>(LostReason::unsupported)]);
}

std::string lock_text(const Outcome& o) {
    std::string text = format("locks %zu/%zu tx", o.locked_transmissions, o.transmissions);
    if (o.late_locks != 0) text += format(" (%zu later than end + 4 T)", o.late_locks);
    if (o.wrong_locks + o.stray_locks != 0) text += format(" (+%zu wrong T/N, %zu stray)", o.wrong_locks, o.stray_locks);
    if (o.score.late_joins != 0) text += format(", late joins %zu (cold %zu)", o.score.late_joins, o.cold_joins);
    return text;
}

std::string ber_text(const Outcome& o) {
    const Score& s = o.score;
    std::string text = format("BER %.2e (%zu bits), delivered %.2f%%, loss %.2f%%, wrong %zu, extra %zu, ", s.ber(),
                              s.matched * k_byte_bits, k_percent * o.delivered(), k_percent * s.loss(), s.wrong_bytes,
                              s.extra_bytes);
    if (o.misplaced_segments != 0) {
        text += format("SHIFTED %zu bytes in %zu lock(s), ", o.misplaced_bytes, o.misplaced_segments);
    }
    return text + lock_text(o) + ", " + lost_text(o);
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
    run_ledger().push_back(RunEntry{where, outcome.max_wrong_run, at_gate_plus_3, outcome.misplaced_segments,
                                    outcome.misplaced_bytes});
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
    result("F5", format("F1-F4 (all seeds) and every C point (%zu runs)", ledger.size()),
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
    std::size_t over[2] = {0, 0};
    for (std::size_t i = 0; i < ledger.size(); ++i) {
        if (ledger[i].max_run <= k_max_wrong_run) continue;
        ++over[ledger[i].qualifies ? 0 : 1];
        note(format("F6 (%s): %s: longest wrong run %zu", ledger[i].qualifies ? "gated" : "report",
                    ledger[i].where.c_str(), ledger[i].max_run));
    }
    result("F6", format("%zu points of L5/A/C at >= gate + 3 dB", count[0]),
           format("longest run of wrong bytes %zu (%s); %zu points above 8", worst[0], where[0].c_str(), over[0]),
           "<= 8", worst[0] <= k_max_wrong_run);
    result("F6", format("%zu points below gate + 3 dB", count[1]),
           format("longest run of wrong bytes %zu (%s); %zu points above 8", worst[1], where[1].c_str(), over[1]),
           "report only", true, Kind::report);
    // The promise behind F6 (spec 3.13, V5): bytes are never shifted. Every lock whose bytes sit at a wrong
    // byte_index, at any SNR.
    std::size_t segments = 0;
    std::size_t bytes = 0;
    std::size_t points = 0;
    for (std::size_t i = 0; i < ledger.size(); ++i) {
        if (ledger[i].misplaced_segments == 0) continue;
        segments += ledger[i].misplaced_segments;
        bytes += ledger[i].misplaced_bytes;
        ++points;
        note(format("shifted bytes: %s: %zu bytes in %zu lock(s)", ledger[i].where.c_str(), ledger[i].misplaced_bytes,
                    ledger[i].misplaced_segments));
    }
    result("F6", format("every lock of the %zu L5/A/C points, any SNR: bytes released at a wrong byte_index", ledger.size()),
           format("%zu shifted bytes in %zu lock(s) at %zu point(s)", bytes, segments, points),
           "report (spec 3.13 and V5: never a shifted byte)", true, Kind::report);
}

void print_summary() {
    std::size_t pass = 0;
    std::size_t fail = 0;
    std::size_t report = 0;
    for (std::size_t i = 0; i < lines().size(); ++i) {
        const std::string& line = lines()[i];
        if (line.size() >= 4 && line.compare(line.size() - 4, 4, "PASS") == 0) ++pass;
        if (line.size() >= 4 && line.compare(line.size() - 4, 4, "FAIL") == 0) ++fail;
        if (line.size() >= 6 && line.compare(line.size() - 6, 6, "REPORT") == 0) ++report;
    }
    std::printf("\n==== long regression summary (%zu result lines: %zu PASS, %zu FAIL, %zu REPORT) ====\n",
                lines().size(), pass, fail, report);
    for (std::size_t i = 0; i < lines().size(); ++i) std::printf("%s\n", lines()[i].c_str());
}

}  // namespace regression
}  // namespace unlimited
