#include "regression.hpp"

#include "test_harness.hpp"

#include <cmath>
#include <random>

// A1-A4 (spec 8.3), the N sweep at the hf gate (spec 4.1) and the 10-minute L5 (spec 8.2, 4.3): AWGN through the
// usb channel, the receiver mistuned by up to +-50 Hz.
namespace unlimited {
namespace regression {

namespace {

const std::uint32_t k_test_a1 = 1;
const std::uint32_t k_test_a2 = 2;
const std::uint32_t k_test_a3 = 3;
const std::uint32_t k_test_a4 = 4;
const std::uint32_t k_test_l5 = 5;
const std::uint32_t k_test_sweep = 6;

const double k_ber_bits = 2.048e5;       // spec 8.3: >= 2e5 bits per point
const std::size_t k_ber_bytes = 200;     // bytes per transmission
const double k_ber_gate = 1e-3;
const double k_loss_gate = 0.01;
const double k_fixed_margin_db = 4.5;    // A2: gate + 4.5 dB
const double k_f6_margin_db = 3.0;       // F6 applies at >= gate + 3 dB
const double k_offset_hz = 50.0;         // receiver mistuning, uniform +-50 Hz per job

const std::size_t k_a3_bytes = 16;
const std::size_t k_a3_at_gate = 400;
const std::size_t k_a3_below_gate = 300;
const double k_a3_below_db = 2.0;
const std::size_t k_a3_long_bytes = 64;   // N = 32 gated with 16 packages per transmission
const std::uint8_t k_long_package_bits = 32;
// Gate decision A3 (spec 8.3): the 99 % / 90 % gates apply to messages of at least 8 packages; a 16-byte message at
// N = 32 has 4, so that row is reported and N = 32 is gated on 64-byte messages.
const std::size_t k_a3_min_packages = 8;
const double k_a3_gate = 0.99;
const double k_a3_below_gate_rate = 0.90;

const double k_a4_steps_db[] = {0.0, 5.0, 10.0, 15.0, 20.0};
const std::size_t k_a4_jobs = 6;
const std::size_t k_a4_transmissions = 2;
const std::size_t k_a4_bytes = 60;
const double k_a4_tolerance_db = 1.5;
const double k_a4_no_value = -99.0;
const std::size_t k_a4_tail = 20;        // 5 % quantiles

const double k_sweep_step_db = 0.5;      // spec 4.1: N = 1, 4, 16, 32 within 0.5 dB of N = 8
const std::uint8_t k_sweep_bits[] = {1, 4, 8, 16, 32};
const double k_hf_slot_ms = 16.0;

const double k_l5_minutes = 10.0;
const double k_l5_ppm = 1000.0;
const double k_l5_slot_tolerance = 0.002;  // spec 4.3: measured T within 0.2 %
const std::uint8_t k_l5_bits[] = {8, 32};

const double k_s_per_min = 60.0;
const double k_byte_bits = 8.0;
const double k_ms_per_s = 1000.0;
const double k_percent = 100.0;

// One A row: a sender, its receiver and the spec 4.1 gate. The index of a row is its seed: never reorder.
struct APoint {
    std::string name;
    EncoderConfig config;
    DecoderConfig receiver;
    double gate_db;
    bool provisional;
    bool n_variant;  // N = 1, 4, 16, 32 at T = 16 ms (spec 8.3 A1 "plus")
};

APoint make_point(const std::string& name, const EncoderConfig& config, bool provisional) {
    APoint p;
    p.name = name;
    p.config = config;
    p.receiver = receiver_for(config);
    p.gate_db = gate_db(slot_ms_of(config));
    p.provisional = provisional;
    p.n_variant = false;
    return p;
}

const double k_slow_row_ms = 64.0;
const double k_slowest_row_ms = 128.0;

std::vector<APoint> a_points() {
    std::vector<APoint> points;
    points.push_back(make_point("fm", preset(Preset::fm), true));
    points.push_back(make_point("hf_fast", preset(Preset::hf_fast), false));
    APoint am = make_point("am", preset(Preset::am), false);
    am.receiver = DecoderConfig::for_profile(Profile::am);
    points.push_back(am);
    points.push_back(make_point("hf", preset(Preset::hf), false));
    points.push_back(make_point("hf_slow", preset(Preset::hf_slow), false));
    points.push_back(make_point("T64 N8", slot_preset(k_slow_row_ms, k_hf_bits_per_package), false));
    points.push_back(make_point("T128 N8", slot_preset(k_slowest_row_ms, k_hf_bits_per_package), true));
    const std::uint8_t variants[] = {1, 4, 16, 32};
    for (std::size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
        APoint p = make_point(format("hf N%u", variants[i]), slot_preset(k_hf_slot_ms, variants[i]), false);
        p.n_variant = true;
        points.push_back(p);
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!points[i].config.valid()) test::fail(__FILE__, __LINE__, "invalid A sender: " + points[i].name);
    }
    return points;
}

const std::vector<APoint>& all_points() {
    static const std::vector<APoint> points = a_points();
    return points;
}

const std::size_t k_preset_rows = 7;  // rows 0..6: one per spec 4.1 T (8 ms twice: hf_fast and am)

std::string point_text(const APoint& p) {
    return format("%s (%s), %s", p.name.c_str(), config_text(p.config).c_str(), receiver_text(p.receiver).c_str());
}

std::string prov(const APoint& p) {
    return p.provisional ? " (prov.)" : "";
}

// `count` transmissions of `bytes` random bytes over usb AWGN at `snr_db`, split into jobs (or one per job).
void add_awgn_jobs(std::vector<JobPlan>& jobs, std::vector<std::size_t>& point_of, std::size_t point,
                   std::size_t seed_point, const APoint& p, double snr_db, std::size_t count, std::size_t bytes,
                   DecisionMode mode, std::uint32_t test, bool independent) {
    const std::size_t per_job =
        independent ? 1 : transmissions_per_job(transmission_seconds(p.config, bytes, JobPlan().gap_ms), count);
    std::uint32_t job_index = 0;
    for (std::size_t first = 0; first < count; first += per_job, ++job_index) {
        JobPlan job;
        const std::uint32_t seed = seed_of(test, static_cast<std::uint32_t>(seed_point), job_index);
        std::mt19937 generator(seed);
        std::uniform_real_distribution<double> offset(-k_offset_hz, k_offset_hz);
        for (std::size_t t = first; t < std::min(count, first + per_job); ++t) {
            job.transmissions.push_back(random_tx(p.config, bytes, data_seed(seed, t)));
        }
        job.channel.mode = sim::Mode::usb;
        job.channel.snr_db = snr_db;
        job.channel.freq_offset_hz = offset(generator);
        job.channel.seed = seed;
        DecoderConfig receiver = p.receiver;
        receiver.decision_mode = mode;
        job.decoders.push_back(receiver);
        jobs.push_back(job);
        point_of.push_back(point);
    }
}

std::string latency_text(const Outcome& o) {
    const double mean = o.slot_events == 0 ? 0.0 : o.latency_sum / static_cast<double>(o.slot_events);
    return format("release latency mean %.1f T max %.1f T after the STOP", mean, o.latency_max);
}

// A1: every row at its gate, BER <= 1e-3 and loss <= 1 %. Run once, shared with the N sweep.
const std::vector<std::vector<Outcome> >& a1_results() {
    static const std::vector<std::vector<Outcome> > results = []() {
        const std::vector<APoint>& points = all_points();
        std::vector<JobPlan> jobs;
        std::vector<std::size_t> point_of;
        const std::size_t count = transmissions_for(k_ber_bits, k_ber_bytes);
        for (std::size_t i = 0; i < points.size(); ++i) {
            add_awgn_jobs(jobs, point_of, i, i, points[i], points[i].gate_db, count, k_ber_bytes,
                          DecisionMode::adaptive, k_test_a1, false);
        }
        return run_points(jobs, point_of, points.size());
    }();
    return results;
}

std::size_t packages_of(const APoint& p, std::size_t bytes) {
    const std::size_t bits = bytes * k_bits_per_byte;
    return (bits + p.config.bits_per_package - 1) / p.config.bits_per_package;
}

// A3 for N = 32 (gate decision A3): the same trials with 64-byte transmissions (16 packages instead of 4).
void a3_long_packages(const APoint& p, std::size_t row) {
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    const std::size_t counts[] = {k_a3_at_gate, k_a3_below_gate};
    const double deltas[] = {0.0, -k_a3_below_db};
    const std::size_t levels = test::count_of(counts);
    for (std::size_t n = 0; n < levels; ++n) {
        add_awgn_jobs(jobs, point_of, n, levels * row + n, p, p.gate_db + deltas[n], counts[n], k_a3_long_bytes,
                      DecisionMode::adaptive, k_test_a3, true);
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, point_of, levels);
    for (std::size_t n = 0; n < levels; ++n) {
        const Outcome& o = outcomes[n][0];
        const double rate = static_cast<double>(o.locked_transmissions) / static_cast<double>(o.transmissions);
        const double gate = n == 0 ? k_a3_gate : k_a3_below_gate_rate;
        const bool pass = rate >= gate && o.wrong_locks == 0 && o.stray_locks == 0;
        result("A3", point_text(p) + format(", %zu independent %zu-byte transmissions (%zu packages), usb AWGN %+.1f dB (%s)",
                                            o.transmissions, k_a3_long_bytes, packages_of(p, k_a3_long_bytes),
                                            p.gate_db + deltas[n], n == 0 ? "gate" : "gate - 2"),
               format("locked %.2f%% (%zu/%zu, %zu later than end + 4 T), wrong T/N locks %zu, stray locks %zu, "
                      "delivered %.2f%%, wrong bytes %zu, extra %zu, shifted %zu, ",
                      k_percent * rate, o.locked_transmissions, o.transmissions, o.late_locks, o.wrong_locks,
                      o.stray_locks, k_percent * o.delivered(), o.score.wrong_bytes, o.score.extra_bytes,
                      o.misplaced_bytes) +
                   lost_text(o),
               format("locked >= %.0f%%, 0 locks at a wrong T or N (messages of >= %zu packages: gate decision A3)",
                      k_percent * gate, k_a3_min_packages),
               pass);
        ledger_runs(format("A3 %s %zu-byte %s", p.name.c_str(), k_a3_long_bytes, n == 0 ? "gate" : "gate-2"), o, false);
    }
}

}  // namespace

void test_a1_smart_line() {
    const std::vector<APoint>& points = all_points();
    const std::vector<std::vector<Outcome> >& outcomes = a1_results();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const APoint& p = points[i];
        const Outcome& o = outcomes[i][0];
        const loopback::Score& s = o.score;
        const bool pass = s.ber() <= k_ber_gate && s.loss() <= k_loss_gate;
        result("A1", point_text(p) + format(", usb AWGN %+.1f dB (gate), offset +-%g Hz", p.gate_db, k_offset_hz),
               ber_text(o) + format(", BER95 <= %.1e, max wrong run %zu, ", upper_95(s.bit_errors, s.matched * k_byte_bits),
                                    o.max_wrong_run) +
                   latency_text(o),
               "BER <= 1e-3, loss <= 1%" + prov(p), pass);
        ledger_runs("A1 " + p.name, o, false);
    }
}

// Spec 4.1 "expected: the A1 rows at T = 16 ms with N = 1, 4, 16 and 32 within 0.5 dB of N = 8": every N at
// gate - 0.5 dB too; N is within 0.5 dB of N = 8 when its BER at the gate is at most N = 8's at gate - 0.5 dB.
void test_a1_n_sweep() {
    const std::vector<APoint>& points = all_points();
    const std::vector<std::vector<Outcome> >& at_gate = a1_results();
    const std::size_t sweep = sizeof(k_sweep_bits) / sizeof(k_sweep_bits[0]);
    std::vector<std::size_t> rows(sweep, 0);
    for (std::size_t n = 0; n < sweep; ++n) {
        for (std::size_t i = 0; i < points.size(); ++i) {
            const bool hf_row = points[i].config.slot_us == preset(Preset::hf).slot_us;
            if (hf_row && points[i].config.bits_per_package == k_sweep_bits[n]) rows[n] = i;
        }
    }
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    const std::size_t count = transmissions_for(k_ber_bits, k_ber_bytes);
    for (std::size_t n = 0; n < sweep; ++n) {
        const APoint& p = points[rows[n]];
        add_awgn_jobs(jobs, point_of, n, n, p, p.gate_db - k_sweep_step_db, count, k_ber_bytes, DecisionMode::adaptive,
                      k_test_sweep, false);
    }
    const std::vector<std::vector<Outcome> > below = run_points(jobs, point_of, sweep);
    std::size_t reference = 0;
    for (std::size_t n = 0; n < sweep; ++n) {
        if (k_sweep_bits[n] == k_hf_bits_per_package) reference = n;
    }
    const Outcome& n8_below = below[reference][0];
    for (std::size_t n = 0; n < sweep; ++n) {
        const APoint& p = points[rows[n]];
        const Outcome& gate = at_gate[rows[n]][0];
        const Outcome& low = below[n][0];
        const double net = static_cast<double>(p.config.bits_per_package) /
                           ((p.config.bits_per_package + 1.0) * k_hf_slot_ms / k_ms_per_s);
        std::string measured = format("net %.1f bit/s; BER %.2e at %+.1f dB (%zu bits, loss %.2f%%), %.2e at %+.1f dB "
                                      "(%zu bits, loss %.2f%%)",
                                      net, gate.score.ber(), p.gate_db, gate.score.matched * k_bits_per_byte,
                                      k_percent * gate.score.loss(), low.score.ber(), p.gate_db - k_sweep_step_db,
                                      low.score.matched * k_bits_per_byte, k_percent * low.score.loss());
        if (n != reference) {
            const bool within = gate.score.ber() <= n8_below.score.ber() && gate.score.loss() <= k_loss_gate;
            measured += format("; N=8 at %+.1f dB: %.2e -> %s", p.gate_db - k_sweep_step_db, n8_below.score.ber(),
                               within ? "within 0.5 dB of N=8" : "NOT within 0.5 dB of N=8");
        }
        result("A1-N", format("N sweep at T=16 ms: N=%u, usb AWGN, gate +1.5 dB and gate - 0.5 dB",
                              p.config.bits_per_package),
               measured, "report (spec 4.1 expected: within 0.5 dB of N=8)", true, Kind::report);
        ledger_runs(format("A1-N N%u gate-0.5", p.config.bits_per_package), low, false);
    }
}

void test_a2_fixed_line() {
    const std::vector<APoint>& points = all_points();
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    const std::size_t count = transmissions_for(k_ber_bits, k_ber_bytes);
    for (std::size_t i = 0; i < k_preset_rows; ++i) {
        add_awgn_jobs(jobs, point_of, i, i, points[i], points[i].gate_db + k_fixed_margin_db, count, k_ber_bytes,
                      DecisionMode::fixed_ratio, k_test_a2, false);
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, point_of, k_preset_rows);
    for (std::size_t i = 0; i < k_preset_rows; ++i) {
        const APoint& p = points[i];
        const Outcome& o = outcomes[i][0];
        result("A2",
               point_text(p) + format(", fixed 70%% line, usb AWGN %+.1f dB (gate + 4.5)", p.gate_db + k_fixed_margin_db),
               ber_text(o) + format(", BER95 <= %.1e, max wrong run %zu",
                                    upper_95(o.score.bit_errors, o.score.matched * k_byte_bits), o.max_wrong_run),
               "BER <= 1e-3, delivered >= 50%" + prov(p), o.score.ber() <= k_ber_gate && delivered(o));
        ledger_runs("A2 " + p.name, o, true);
    }
}

void test_a3_acquisition() {
    const std::vector<APoint>& points = all_points();
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    const std::size_t counts[] = {k_a3_at_gate, k_a3_below_gate};
    const double deltas[] = {0.0, -k_a3_below_db};
    const std::size_t levels = test::count_of(counts);
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (std::size_t n = 0; n < levels; ++n) {
            add_awgn_jobs(jobs, point_of, levels * i + n, levels * i + n, points[i], points[i].gate_db + deltas[n],
                          counts[n], k_a3_bytes, DecisionMode::adaptive, k_test_a3, true);
        }
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, point_of, levels * points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const APoint& p = points[i];
        for (std::size_t n = 0; n < levels; ++n) {
            const Outcome& o = outcomes[levels * i + n][0];
            const double rate = static_cast<double>(o.locked_transmissions) / static_cast<double>(o.transmissions);
            const double gate = n == 0 ? k_a3_gate : k_a3_below_gate_rate;
            const bool pass = rate >= gate && o.wrong_locks == 0 && o.stray_locks == 0;
            const std::size_t packages = packages_of(p, k_a3_bytes);
            const std::string condition =
                point_text(p) + format(", %zu independent %zu-byte transmissions (%zu packages), usb AWGN %+.1f dB (%s)",
                                       o.transmissions, k_a3_bytes, packages, p.gate_db + deltas[n],
                                       n == 0 ? "gate" : "gate - 2");
            const std::string measured =
                format("locked %.2f%% (%zu/%zu, %zu later than end + 4 T), wrong T/N locks %zu, stray locks %zu, "
                       "delivered %.2f%%, wrong bytes %zu, extra %zu, shifted %zu, ",
                       k_percent * rate, o.locked_transmissions, o.transmissions, o.late_locks, o.wrong_locks,
                       o.stray_locks, k_percent * o.delivered(), o.score.wrong_bytes, o.score.extra_bytes,
                       o.misplaced_bytes) +
                lost_text(o);
            if (packages < k_a3_min_packages) {
                result("A3", condition, measured,
                       format("report (%zu packages per message: the A3 gates apply to messages of >= %zu packages, "
                              "gate decision A3; locked >= %.0f%%: %s)",
                              packages, k_a3_min_packages, k_percent * gate, pass ? "met" : "NOT met"),
                       true, Kind::report);
            } else {
                result("A3", condition, measured,
                       format("locked >= %.0f%%, 0 locks at a wrong T or N%s", k_percent * gate, prov(p).c_str()), pass);
            }
            ledger_runs(format("A3 %s %s", p.name.c_str(), n == 0 ? "gate" : "gate-2"), o, false);
        }
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (points[i].config.bits_per_package == k_long_package_bits) a3_long_packages(points[i], i);
    }
}

void test_a4_snr_report() {
    const std::vector<APoint>& points = all_points();
    const std::size_t steps = sizeof(k_a4_steps_db) / sizeof(k_a4_steps_db[0]);
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (std::size_t s = 0; s < steps; ++s) {
            add_awgn_jobs(jobs, point_of, i * steps + s, i * steps + s, points[i], points[i].gate_db + k_a4_steps_db[s],
                          k_a4_jobs * k_a4_transmissions, k_a4_bytes, DecisionMode::adaptive, k_test_a4, false);
        }
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, point_of, points.size() * steps);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const APoint& p = points[i];
        for (std::size_t s = 0; s < steps; ++s) {
            const Outcome& o = outcomes[i * steps + s][0];
            const double truth = p.gate_db + k_a4_steps_db[s];
            std::vector<float> values = o.snr_db;
            double mean = 0.0;
            for (std::size_t n = 0; n < values.size(); ++n) mean += values[n];
            mean = values.empty() ? k_a4_no_value : mean / static_cast<double>(values.size());
            std::sort(values.begin(), values.end());
            const double low = values.empty() ? k_a4_no_value : values[values.size() / k_a4_tail];
            const double high = values.empty() ? k_a4_no_value : values[values.size() - 1 - values.size() / k_a4_tail];
            const double error = mean - truth;
            const bool pass = !values.empty() && std::fabs(error) <= k_a4_tolerance_db;
            const std::string step = s == 0 ? "gate" : format("gate + %g", k_a4_steps_db[s]);
            result("A4", point_text(p) + format(", usb AWGN %+.1f dB (%s)", truth, step.c_str()),
                   format("reported mean %+.2f dB (error %+.2f), 5-95%% %+.1f..%+.1f, %zu byte events", mean, error,
                          low, high, values.size()),
                   "abs(mean - true) <= 1.5 dB", pass);
            ledger_runs(format("A4 %s +%g dB", p.name.c_str(), k_a4_steps_db[s]), o, k_a4_steps_db[s] >= k_f6_margin_db);
        }
    }
}

// L5 long: +-1000 ppm on the transmitter clock (channel resampling), the receiver clock (resampling of the
// received audio) and both in opposite directions, 10-minute transmissions at T = 16 ms with N = 8 and 32,
// gate + 3 dB: no slip, no error, the measured T within 0.2 % of the true one.
void test_l5_clock() {
    struct Case {
        double tx_ppm;
        double rx_ppm;
    };
    const Case cases[] = {{k_l5_ppm, 0.0},  {-k_l5_ppm, 0.0},       {0.0, k_l5_ppm},
                          {0.0, -k_l5_ppm}, {k_l5_ppm, -k_l5_ppm}, {-k_l5_ppm, k_l5_ppm}};
    const std::size_t case_count = sizeof(cases) / sizeof(cases[0]);
    const std::size_t bit_count = sizeof(k_l5_bits) / sizeof(k_l5_bits[0]);
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    for (std::size_t b = 0; b < bit_count; ++b) {
        const EncoderConfig config = slot_preset(k_hf_slot_ms, k_l5_bits[b]);
        const double package_s = (config.bits_per_package + 1.0) * k_hf_slot_ms / k_ms_per_s;
        const std::size_t packages = static_cast<std::size_t>(k_l5_minutes * k_s_per_min / package_s);
        const std::size_t bytes = packages * config.bits_per_package / static_cast<std::size_t>(k_byte_bits);
        for (std::size_t c = 0; c < case_count; ++c) {
            JobPlan job;
            const std::uint32_t seed = seed_of(k_test_l5, static_cast<std::uint32_t>(b * case_count + c), 0);
            job.transmissions.push_back(random_tx(config, bytes, seed));
            job.channel.mode = sim::Mode::usb;
            job.channel.snr_db = gate_db(k_hf_slot_ms) + k_f6_margin_db;
            job.channel.clock_ppm = cases[c].tx_ppm;
            job.channel.seed = seed;
            job.rx_ppm = cases[c].rx_ppm;
            job.decoders.push_back(DecoderConfig::for_profile(Profile::ssb));
            jobs.push_back(job);
            point_of.push_back(b * case_count + c);
        }
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, point_of, bit_count * case_count);
    for (std::size_t b = 0; b < bit_count; ++b) {
        for (std::size_t c = 0; c < case_count; ++c) {
            const Outcome& o = outcomes[b * case_count + c][0];
            const loopback::Score& s = o.score;
            const bool slips = s.locks != 1 || o.locked_transmissions != 1 || o.wrong_locks + o.stray_locks != 0 ||
                               s.lost_events != 0 || s.ends != 1 || s.late_joins != 0;
            const bool pass = !slips && s.wrong_bytes == 0 && s.lost_bytes == 0 && s.extra_bytes == 0 &&
                              o.mean_slot_error() <= k_l5_slot_tolerance;
            result("L5",
                   format("TX %+g ppm, RX %+g ppm, %g min at T=16 ms N=%u, usb AWGN %+.1f dB (gate + 3)",
                          cases[c].tx_ppm, cases[c].rx_ppm, k_l5_minutes, k_l5_bits[b],
                          gate_db(k_hf_slot_ms) + k_f6_margin_db),
                   format("%zu bytes: wrong %zu, lost %zu, extra %zu, locks %zu (%zu with the sent T and N), ends %zu, "
                          "late joins %zu, flywheel bytes %zu, %s; measured T error mean %.3f%% worst %.3f%%",
                          s.bytes_sent, s.wrong_bytes, s.lost_bytes, s.extra_bytes, s.locks, o.locked_transmissions,
                          s.ends, s.late_joins, s.flywheel_bytes, lost_text(o).c_str(),
                          k_percent * o.mean_slot_error(), k_percent * o.worst_slot_error),
                   "0 slips (1 lock, 1 end, no LOST), 0 errors, mean T within 0.2%", pass);
            ledger_runs(format("L5 N%u case %zu", k_l5_bits[b], c), o, true);
        }
    }
}

}  // namespace regression
}  // namespace unlimited
