#include "regression.hpp"

#include "test_harness.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>

namespace unlimited {
namespace regression {

using loopback::Capture;
using loopback::Recording;
using loopback::Score;
using loopback::Transmission;

const float k_speeds[k_speed_count] = {1.0f, 3.0f, 6.0f, 12.0f, 25.0f};

namespace {

const double k_full_scale = 32768.0;
const double k_int16_min = -32768.0;
const double k_int16_max = 32767.0;
const double k_ms_per_s = 1000.0;
const double k_us_per_ms = 1000.0;
const double k_ppm = 1e-6;
const double k_reference_bandwidth_hz = 2500.0;
const double k_receiver_bandwidth_hz = 2400.0;
const double k_noise_peak_sigmas = 5.0;
const double k_headroom = 0.9;
const double k_db_per_decade_power = 10.0;
const int k_byte_bits = 8;
const double k_percent = 100.0;

// Spec 4's provisional gates (v0.3's at the same T), per speed in k_speeds order.
const double k_gates_db[k_speed_count] = {-6.5, -1.7, 1.3, 4.3, 8.0};
const double k_leading_slots = 15.0;  // a whole silent window before the first START (V6), and a margin

// Scoring: a lock belongs to the latest transmission whose first START lies 9.5 to 100 slots before it (loopback's
// rule: the lock comes once the first window is in), and a lock segment is shifted when at least half of its bytes (and twice as many as at its own numbering)
// match the sent data at another byte offset; segments shorter than this are not judged.
const double k_lock_min_slots = 9.5;
const double k_lock_max_slots = 100.0;
const std::size_t k_shift_min_bytes = 4;
const std::size_t k_shift_ratio = 2;
const std::size_t k_settle_windows = 8;  // L5: T is judged after the loops settled

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
const double k_cost_decoder = 0.05;

// Confidence bound helpers.
const double k_zero_error_bound = 3.0;  // 95 % upper bound of a Poisson mean after 0 events
const double k_z95 = 1.96;

std::mutex& lines_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::string>& lines() {
    static std::vector<std::string> all;
    return all;
}

struct LedgerEntry {
    std::string where;
    std::size_t extra;
    std::size_t shifted;
    std::size_t released;
    bool fade_bridge;
};

std::vector<LedgerEntry>& ledger_entries() {
    static std::vector<LedgerEntry> all;
    return all;
}

// Where one transmission lies in the received audio: the recording's layout stretched by the sender's clock error
// and delayed by the channel.
struct Layout {
    const Transmission* tx;
    double stretch;
    double delay;
    double first_start() const { return stretch * loopback::first_start_sample(*tx) + delay; }
    double slot() const { return stretch * loopback::slot_samples(tx->config); }
};

long transmission_of_lock(const std::vector<Layout>& sent, double heard) {
    long found = -1;
    double latest = 0.0;
    for (std::size_t t = 0; t < sent.size(); ++t) {
        const double start = sent[t].first_start();
        if (start > heard - k_lock_min_slots * sent[t].slot() || start < heard - k_lock_max_slots * sent[t].slot())
            continue;
        if (found < 0 || start > latest) {
            found = static_cast<long>(t);
            latest = start;
        }
    }
    return found;
}

// The transmission on air at `heard` (the lock of a relock inside a transmission), -1 for none.
long transmission_at(const std::vector<Layout>& sent, double heard) {
    for (std::size_t t = 0; t < sent.size(); ++t) {
        const double end = sent[t].first_start() + sent[t].slot() * unlimited::k_window_slots * sent[t].tx->data.size();
        if (heard >= sent[t].first_start() && heard <= end + k_lock_max_slots * sent[t].slot())
            return static_cast<long>(t);
    }
    return -1;
}

struct Segment {
    long transmission;  // the lock's, -1 for none
    double heard;       // the lock's position, look-ahead removed
    std::vector<Event> bytes;
};

// Bytes released at a wrong byte_index: the offset (in bytes) at which most of the segment's bytes match the data.
void count_shifted(const Segment& segment, const std::vector<Layout>& sent, Outcome& outcome) {
    if (segment.bytes.size() < k_shift_min_bytes) return;
    long t = segment.transmission;
    if (t < 0) t = transmission_at(sent, segment.heard);
    if (t < 0) return;
    const std::vector<std::uint8_t>& data = sent[static_cast<std::size_t>(t)].tx->data;
    const long size = static_cast<long>(data.size());
    std::size_t own = 0;
    std::size_t best = 0;
    for (long offset = -size; offset <= size; ++offset) {
        std::size_t matches = 0;
        for (std::size_t i = 0; i < segment.bytes.size(); ++i) {
            const long position = static_cast<long>(segment.bytes[i].byte_index) + offset;
            if (position >= 0 && position < size && data[static_cast<std::size_t>(position)] == segment.bytes[i].value)
                ++matches;
        }
        if (offset == 0 && segment.transmission >= 0) {
            own = matches;
        } else {
            best = std::max(best, matches);
        }
    }
    if (2 * best >= segment.bytes.size() && best >= k_shift_ratio * own) {
        ++outcome.shifted_segments;
        outcome.shifted_bytes += best;
    }
}

Outcome evaluate(const Recording& recording, const Capture& capture, const std::vector<Layout>& sent, double delay,
                 double stretch) {
    Outcome outcome;
    outcome.score = loopback::map_events(recording, capture, delay).score;
    outcome.transmissions = recording.transmissions.size();
    Segment segment = {-1, 0.0, std::vector<Event>()};
    bool open = false;
    std::size_t windows = 0;
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        const Event& e = capture.events[i];
        const double heard = static_cast<double>(capture.event_sample[i]) - static_cast<double>(capture.lookahead);
        switch (e.type) {
            case EventType::locked:
                if (open) count_shifted(segment, sent, outcome);
                segment.transmission = transmission_of_lock(sent, heard);
                segment.heard = heard;
                segment.bytes.clear();
                open = true;
                windows = 0;
                break;
            case EventType::byte:
                if (!open) break;
                segment.bytes.push_back(e);
                if (++windows > k_settle_windows && segment.transmission >= 0) {
                    const double heard_ms = slot_ms_of(sent[static_cast<std::size_t>(segment.transmission)].tx->config) *
                                            stretch;
                    const double error = std::fabs(e.slot_ms / heard_ms - 1.0);
                    outcome.slot_error_sum += error;
                    outcome.worst_slot_error = std::max(outcome.worst_slot_error, error);
                    ++outcome.slot_events;
                }
                break;
            case EventType::end:
            case EventType::lost:
                if (e.type == EventType::lost) ++outcome.lost_framing;
                if (open) count_shifted(segment, sent, outcome);
                open = false;
                break;
            case EventType::state:
            case EventType::slot:
                break;
        }
    }
    if (open) count_shifted(segment, sent, outcome);
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

}  // namespace

std::size_t worker_count() {
    const unsigned cores = std::thread::hardware_concurrency();
    return cores == 0 ? 1 : cores;
}

double gate_db(float speed) {
    for (std::size_t i = 0; i < k_speed_count; ++i) {
        if (k_speeds[i] == speed) return k_gates_db[i];
    }
    // Between the speeds: v0.3's gate formula at the same T (+1.5 dB at 16 ms, -10 dB per decade of T).
    const double k_reference_ms = 16.0;
    const double k_reference_db = 1.5;
    return k_reference_db - k_db_per_decade_power * std::log10(slot_ms_of(speed_config(speed)) / k_reference_ms);
}

EncoderConfig speed_config(float speed, std::uint16_t tone_hz) {
    return loopback::speed_config(speed, tone_hz);
}

DecoderConfig receiver_for(const EncoderConfig& config) {
    return loopback::receiver_for(config);
}

DecoderConfig fixed_receiver_for(const EncoderConfig& config) {
    DecoderConfig receiver = loopback::receiver_for(config);
    receiver.decision_mode = DecisionMode::fixed;
    receiver.threshold_percent = k_default_threshold_percent;
    return receiver;
}

DecoderConfig fade_receiver_for(const EncoderConfig& config) {
    DecoderConfig receiver = loopback::receiver_for(config);
    receiver.fade_bridge = true;
    return receiver;
}

std::string speed_text(float speed) {
    return format("%g bytes/s", static_cast<double>(speed));
}

double slot_ms_of(const EncoderConfig& config) {
    return static_cast<double>(config.slot_us) / k_us_per_ms;
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
    return seconds * (factor + k_cost_decoder * static_cast<double>(decoders.size()));
}

double Outcome::delivered() const {
    return score.bytes_sent == 0 ? 0.0 : static_cast<double>(score.matched) / static_cast<double>(score.bytes_sent);
}

double Outcome::mean_slot_error() const {
    return slot_events == 0 ? 0.0 : slot_error_sum / static_cast<double>(slot_events);
}

bool delivered(const Outcome& outcome) {
    return outcome.delivered() >= k_min_delivered;
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
    a.lost_events += b.lost_events;
    a.ends += b.ends;
    a.framing_windows += b.framing_windows;
    a.locked_transmissions += b.locked_transmissions;
    into.transmissions += from.transmissions;
    into.shifted_segments += from.shifted_segments;
    into.shifted_bytes += from.shifted_bytes;
    into.lost_framing += from.lost_framing;
    into.slot_error_sum += from.slot_error_sum;
    into.worst_slot_error = std::max(into.worst_slot_error, from.worst_slot_error);
    into.slot_events += from.slot_events;
}

TxPlan random_tx(const EncoderConfig& config, std::size_t bytes, std::uint32_t seed) {
    TxPlan tx;
    tx.config = config;
    tx.data = loopback::random_bytes(bytes, seed);
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

sim::ChannelConfig usb_channel(double snr_db, double offset_hz) {
    sim::ChannelConfig channel;
    channel.mode = sim::Mode::usb;
    channel.snr_db = snr_db;
    channel.freq_offset_hz = offset_hz;
    return channel;
}

std::uint32_t seed_of(std::uint32_t test, std::uint32_t point, std::uint32_t job) {
    return test * k_test_stride + point * k_point_stride + job + 1u;
}

std::vector<Outcome> run_job(const JobPlan& job) {
    Recording recording;
    const double first_slot_ms =
        job.transmissions.empty() ? 0.0 : slot_ms_of(job.transmissions[0].config);
    loopback::append_silence(recording, std::max(job.lead_ms, k_leading_slots * first_slot_ms));
    for (std::size_t t = 0; t < job.transmissions.size(); ++t) {
        loopback::append_transmission(recording, job.transmissions[t].data, job.transmissions[t].config);
        loopback::append_silence(recording, t + 1 < job.transmissions.size() ? job.gap_ms : job.tail_ms);
    }
    const std::int16_t amplitude =
        job.transmissions.empty() ? EncoderConfig().amplitude : job.transmissions[0].config.amplitude;
    const std::vector<std::int16_t> samples =
        job.use_channel ? apply_channel(recording.samples, job.channel, amplitude, job.peak_factor) : recording.samples;
    // A sender clock fast by ppm plays everything shorter: the received time line is stretched by 1 / (1 + ppm).
    const double stretch = job.use_channel ? 1.0 / (1.0 + job.channel.clock_ppm * k_ppm) : 1.0;
    const double delay = job.use_channel ? channel_delay(job.channel.mode) : 0.0;
    std::vector<Layout> sent;
    for (std::size_t t = 0; t < recording.transmissions.size(); ++t)
        sent.push_back(Layout{&recording.transmissions[t], stretch, delay});
    std::vector<Outcome> outcomes;
    for (std::size_t d = 0; d < job.decoders.size(); ++d) {
        const Capture capture = loopback::run_decoder(samples, job.decoders[d], k_decoder_rate_hz / 100u);
        outcomes.push_back(evaluate(recording, capture, sent, delay, stretch));
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

void add_jobs(std::vector<JobPlan>& jobs, std::vector<std::size_t>& points, std::size_t point, const JobPlan& shape,
              const EncoderConfig& config, std::size_t count, std::size_t bytes, std::uint32_t test) {
    const std::size_t per_job = transmissions_per_job(transmission_seconds(config, bytes, shape.gap_ms), count);
    std::uint32_t job_index = 0;
    for (std::size_t first = 0; first < count; first += per_job, ++job_index) {
        JobPlan job = shape;
        const std::uint32_t seed = seed_of(test, static_cast<std::uint32_t>(point), job_index);
        job.channel.seed = seed;
        for (std::size_t t = first; t < std::min(count, first + per_job); ++t)
            job.transmissions.push_back(random_tx(config, bytes, data_seed(seed, t)));
        jobs.push_back(job);
        points.push_back(point);
    }
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

std::string lock_text(const Outcome& o) {
    return format("locked %zu/%zu tx from byte 0, %zu locks, %zu lost (framing), %zu windows dropped",
                  o.score.locked_transmissions, o.transmissions, o.score.locks, o.lost_framing,
                  o.score.framing_windows);
}

std::string ber_text(const Outcome& o) {
    const Score& s = o.score;
    std::string text = format("BER %.2e (%zu bits), delivered %.2f%%, loss %.2f%%, wrong %zu, extra %zu, ", s.ber(),
                              s.matched * k_byte_bits, k_percent * o.delivered(), k_percent * s.loss(), s.wrong_bytes,
                              s.extra_bytes);
    if (o.shifted_segments != 0) text += format("SHIFTED %zu bytes in %zu lock(s), ", o.shifted_bytes, o.shifted_segments);
    return text + lock_text(o);
}

double upper_95(std::size_t errors, double trials) {
    if (trials <= 0.0) return 1.0;
    if (errors == 0) return k_zero_error_bound / trials;
    const double k = static_cast<double>(errors);
    return (k + k_z95 * std::sqrt(k) + k_z95 * k_z95 / 2.0) / trials;
}

void ledger(const std::string& where, const Outcome& outcome, bool fade_bridge) {
    std::lock_guard<std::mutex> lock(lines_mutex());
    ledger_entries().push_back(LedgerEntry{where, outcome.score.extra_bytes, outcome.shifted_bytes,
                                           outcome.score.bytes_released, fade_bridge});
}

void test_integrity() {
    const std::vector<LedgerEntry>& entries = ledger_entries();
    REQUIRE(!entries.empty());
    for (int bridge = 0; bridge < 2; ++bridge) {
        std::size_t extra = 0;
        std::size_t shifted = 0;
        std::size_t released = 0;
        std::size_t rows = 0;
        std::size_t problem_rows = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].fade_bridge != (bridge != 0)) continue;
            ++rows;
            extra += entries[i].extra;
            shifted += entries[i].shifted;
            released += entries[i].released;
            if (entries[i].extra == 0 && entries[i].shifted == 0) continue;
            ++problem_rows;
            note(format("integrity: %s: %zu extra, %zu shifted bytes", entries[i].where.c_str(), entries[i].extra,
                        entries[i].shifted));
        }
        if (rows == 0) continue;
        const std::string what = bridge != 0 ? "every C row with the fade bridge" : "every A, S, L and C row";
        result(bridge != 0 ? "Integrity-fade" : "Integrity", format("%s (%zu rows, %zu bytes released)", what.c_str(),
                                                                  rows, released),
               format("%zu extra bytes, %zu shifted bytes, in %zu rows", extra, shifted, problem_rows),
               "report: the upper protocol rejects them (V22; was a gate of 0 and 0)", true, Kind::report);
    }
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
