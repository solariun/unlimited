#include "regression.hpp"

#include "test_harness.hpp"

#include <cmath>
#include <random>

// A1', A3', A4, A5 (spec 8.3) and the 10-minute L5 (spec 8.2): AWGN through the usb channel (and the FM
// channel for the fm/fm_fast gates of A1').
namespace unlimited {
namespace regression {

namespace {

const std::uint32_t k_test_a1 = 1;
const std::uint32_t k_test_a3 = 3;
const std::uint32_t k_test_a4 = 4;
const std::uint32_t k_test_a5 = 5;
const std::uint32_t k_test_l5 = 6;

const double k_ber_bits = 2.048e5;     // spec 8.3: >= 2e5 bits per point
const double k_genie_bits = 1.5e5;     // A5, as the genie bench (spec 4.1)
const std::size_t k_ber_bytes = 200;   // bytes per transmission
const double k_ber_gate = 1e-3;
const double k_frames_gate = 0.95;
const double k_genie_ratio_gate = 1.5;
const double k_below_gate_db = 1.0;    // A3' and A5: gate - 1 dB
const double k_f6_margin_db = 3.0;     // F6 applies at >= gate + 3 dB
const double k_offset_hz = 50.0;       // receiver mistuning, uniform per job
const double k_infinite_ratio = 1e9;

const std::size_t k_a3_min_bytes = 16;
const std::size_t k_a3_frames = 2;     // at least frames 0 and 1: the lock is confirmed on one of them
const std::size_t k_a3_at_gate = 400;
const std::size_t k_a3_below_gate = 300;
const double k_a3_gate = 0.99;
const double k_a3_below_gate_rate = 0.90;

const double k_a4_steps_db[] = {0.0, 5.0, 10.0, 15.0, 20.0};
const std::size_t k_a4_jobs = 6;
const std::size_t k_a4_transmissions = 2;
const std::size_t k_a4_bytes = 60;
const double k_a4_tolerance_db = 1.5;
const double k_a4_fm_profile_ssb_db = 1.5;  // the fm profile's report, on T8 k3 centred (A1' gate of fm on SSB)

const double k_l5_gate_db = -1.5;  // hf_fast A1' gate
const double k_l5_minutes = 10.0;
const double k_l5_ppm = 1000.0;
const std::uint32_t k_l5_slot_ms = 16;
const std::uint8_t k_l5_bits = 4;
const std::uint8_t k_long_frame = 32;

const double k_s_per_min = 60.0;
const double k_byte_bits = 8.0;
const double k_ms_per_s = 1000.0;
const double k_percent = 100.0;

// Gate decisions. G1: the fm preset (f_ref 2650 Hz) is FM-only; its SSB/AM rows are report-only and the centred
// T8 k3 mode carries the fm mode's SSB/AM gate. G2: dense spacing needs T >= 32 ms, so dense T16 k5 is not sendable
// and its row is removed. G4: A5 of hf_fast N32 is report-only (design limit: long frames at short T).
const char* const k_g1_reason = "G1: fm preset (f_ref 2650 Hz) is FM-only";
const char* const k_g2_reason = "G2: dense needs T >= 32 ms";
const char* const k_g4_reason = "G4: design limit, long frames at short T";

// Where a point's gate comes from.
enum class Basis {
    spec,     // spec 8.3
    centred,  // G1: the fm mode's SSB/AM gate of spec 8.3, on T8 k3 with the HF placement (f_ref 2313 Hz)
    derived   // N, dense and side variants: derived here from the same T
};

// One A point: a mode, the decoder profile, the channel (usb AWGN or the FM channel) and its gate.
struct APoint {
    std::string name;
    EncoderConfig config;
    Profile profile;
    bool fm_channel;     // gate is the CNR in the 12.5 kHz IF (snr = CNR + 7 dB)
    double gate_db;      // usb: SNR in 2500 Hz; fm channel: CNR
    double offset_low;   // usb mistuning range, Hz
    double offset_high;
    Basis basis;
    std::string report_reason;     // non-empty: report-only in every A test
    std::string a5_report_reason;  // non-empty: report-only in A5
    std::string removed_reason;    // non-empty: the encoder refuses the mode (kept: later points keep their seeds)
};

APoint preset_point(const char* name, Preset preset, Profile profile, double gate_db, bool fm_channel = false) {
    APoint p;
    p.name = name;
    p.config = EncoderConfig::from_preset(preset, k_decoder_rate_hz);
    p.profile = profile;
    p.fm_channel = fm_channel;
    p.gate_db = gate_db;
    p.offset_low = -k_offset_hz;
    p.offset_high = k_offset_hz;
    p.basis = Basis::spec;
    return p;
}

APoint mode_point(const char* name, Preset base, std::uint32_t slot_ms, std::uint8_t k, std::uint8_t n,
                  Spacing spacing, GridSide side, double gate_db) {
    APoint p = preset_point(name, base, Profile::ssb, gate_db);
    p.config = mode_config(base, slot_ms, k, n, spacing, side);
    p.basis = Basis::derived;
    return p;
}

// Spec 8.3 gates first (A1' order), then the derived variants; a point's index is its seed, so rows are never
// reordered. The dense gates are the same-T standard gate shifted by the genie difference (spec 4.1): T32 +0.25,
// T64 +0.05, T128 +0.4 dB. "T8 k3 centred" is the fm mode with the HF placement (f_ref 2313 Hz), as the genie bench
// measured it; the fm preset's f_ref 2650 Hz sits at the SSB receiver's passband edge and is reported only (G1).
std::vector<APoint> a_points() {
    std::vector<APoint> points;
    points.push_back(preset_point("hf_fast", Preset::hf_fast, Profile::ssb, -1.5));
    points.push_back(preset_point("hf", Preset::hf, Profile::ssb, -4.5));
    points.push_back(preset_point("hf_robust", Preset::hf_robust, Profile::ssb, -7.0));
    points.push_back(preset_point("hf_weak", Preset::hf_weak, Profile::ssb, -9.5));
    APoint fm_ssb = preset_point("fm preset (am profile, SSB)", Preset::fm, Profile::am, 1.5);
    fm_ssb.offset_high = 0.0;  // f_ref 2650 Hz sits 50 Hz under the receiver's -6 dB edge
    fm_ssb.report_reason = k_g1_reason;
    points.push_back(fm_ssb);
    points.push_back(preset_point("fm (FM channel)", Preset::fm, Profile::fm, 4.5, true));
    points.push_back(preset_point("fm_fast (FM channel)", Preset::fm_fast, Profile::fm, 7.0, true));
    points.push_back(mode_point("hf N16", Preset::hf, 32, 5, 16, Spacing::standard, GridSide::below, -4.5));
    points.push_back(mode_point("hf N32", Preset::hf, 32, 5, 32, Spacing::standard, GridSide::below, -4.5));
    APoint fast_n32 =
        mode_point("hf_fast N32", Preset::hf_fast, 16, 4, 32, Spacing::standard, GridSide::below, -1.5);
    fast_n32.a5_report_reason = k_g4_reason;
    points.push_back(fast_n32);
    points.push_back(
        mode_point("hf_robust N16", Preset::hf_robust, 64, 6, 16, Spacing::standard, GridSide::below, -7.0));
    points.push_back(mode_point("hf grid above", Preset::hf, 32, 5, 8, Spacing::standard, GridSide::above, -4.5));
    APoint t8 = mode_point("T8 k3 centred (am profile, SSB)", Preset::fm, 8, 3, 8, Spacing::standard, GridSide::below,
                           1.5);
    t8.profile = Profile::am;
    t8.basis = Basis::centred;
    points.push_back(t8);
    APoint dense16 = mode_point("dense T16 k5", Preset::hf_fast, 16, 5, 8, Spacing::dense, GridSide::below, -0.9);
    dense16.removed_reason = k_g2_reason;
    points.push_back(dense16);
    points.push_back(mode_point("dense T32 k6", Preset::hf, 32, 6, 8, Spacing::dense, GridSide::below, -4.25));
    points.push_back(
        mode_point("dense T64 k7", Preset::hf_robust, 64, 7, 8, Spacing::dense, GridSide::below, -6.95));
    points.push_back(mode_point("dense T128 k8", Preset::hf_weak, 128, 8, 8, Spacing::dense, GridSide::below, -9.1));
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (points[i].config.valid() == points[i].removed_reason.empty()) continue;
        test::fail(__FILE__, __LINE__, "A mode valid() disagrees with its row: " + points[i].name);
    }
    return points;
}

const std::vector<APoint>& all_points() {
    static const std::vector<APoint> points = a_points();
    return points;
}

std::string gate_text(const APoint& p, double delta_db) {
    if (p.fm_channel) return format("FM channel, CNR %.1f dB", p.gate_db + delta_db);
    return format("usb AWGN %+.2f dB, offset %+g..%+g Hz", p.gate_db + delta_db, p.offset_low, p.offset_high);
}

std::string point_text(const APoint& p) {
    return format("%s [%s], %s profile", p.name.c_str(), mode_name(p.config).c_str(), profile_name(p.profile).c_str());
}

std::string prov(const APoint& p) {
    switch (p.basis) {
    case Basis::spec:
        return " (prov.)";
    case Basis::centred:
        return " (prov., G1: fm mode on SSB/AM)";
    case Basis::derived:
        return " (prov., derived)";
    }
    return "";
}

Kind kind_of(const std::string& reason) {
    return reason.empty() ? Kind::gate : Kind::report;
}

// The gate column: the gate, or for a report-only row its decision and the gate it is not held to.
std::string gate_label(const std::string& gate, const std::string& reason) {
    return reason.empty() ? gate : "report (" + reason + "); not gated: " + gate;
}

// `count` transmissions of `bytes` random bytes at gate + delta_db, split into jobs.
void add_jobs(std::vector<JobPlan>& jobs, std::vector<std::size_t>& point_of, std::size_t point, const APoint& p,
              double delta_db, std::size_t count, std::size_t bytes, std::uint32_t test, bool genie,
              bool independent = false) {
    if (!p.removed_reason.empty()) return;  // the encoder refuses the mode
    const std::size_t per_job =
        independent ? 1 : transmissions_per_job(transmission_seconds(p.config, bytes, JobPlan().gap_ms), count);
    std::uint32_t job_index = 0;
    for (std::size_t first = 0; first < count; first += per_job, ++job_index) {
        JobPlan job;
        const std::uint32_t seed = seed_of(test, static_cast<std::uint32_t>(point), job_index);
        std::mt19937 generator(seed);
        std::uniform_real_distribution<double> offset(p.offset_low, p.offset_high);
        for (std::size_t t = first; t < std::min(count, first + per_job); ++t) {
            job.transmissions.push_back(random_tx(p.config, bytes, data_seed(seed, t)));
        }
        if (p.fm_channel) {
            job.channel.mode = sim::Mode::fm;
            job.channel.snr_db = snr_for_fm_cnr(p.gate_db + delta_db, job.channel);
        } else {
            job.channel.mode = sim::Mode::usb;
            job.channel.snr_db = p.gate_db + delta_db;
            job.channel.freq_offset_hz = offset(generator);
        }
        job.channel.seed = seed;
        job.decoders.push_back(DecoderConfig::for_profile(p.profile));
        job.genie = genie && !p.fm_channel;
        jobs.push_back(job);
        point_of.push_back(point);
    }
}

double ratio(double a, double b) {
    if (b > 0.0) return a / b;
    return a > 0.0 ? k_infinite_ratio : 1.0;
}

std::string lock_text(const Outcome& o) {
    return format("%zu/%zu transmissions locked", o.locked_transmissions, o.transmissions);
}

}  // namespace

void test_a1_integrated() {
    const std::vector<APoint>& points = all_points();
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    const std::size_t count = transmissions_for(k_ber_bits, k_ber_bytes);
    for (std::size_t i = 0; i < points.size(); ++i) {
        add_jobs(jobs, point_of, i, points[i], 0.0, count, k_ber_bytes, k_test_a1, false);
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, point_of, points.size());
    const double delay_slots_ms = ssb_delay_samples(sim::Mode::usb) * k_ms_per_s / k_decoder_rate_hz;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const APoint& p = points[i];
        if (!p.removed_reason.empty()) {
            result("A1'", point_text(p), "not run: EncoderConfig::valid() refuses the mode",
                   "report (" + p.removed_reason + "; row removed from A1', A3', A5)", true, Kind::report);
            continue;
        }
        const Outcome& o = outcomes[i][0];
        const double slot_ms = static_cast<double>(p.config.slot_us) / k_ms_per_s;
        const double delay_slots = p.fm_channel ? 0.0 : delay_slots_ms / slot_ms;
        const double mean_latency = o.latency_events == 0 ? 0.0 : o.latency_sum / static_cast<double>(o.latency_events);
        const bool pass = o.score.ber() <= k_ber_gate && o.frames_ratio() >= k_frames_gate;
        result("A1'", point_text(p) + ", " + gate_text(p, 0.0),
               ber_text(o) + format(", BER95 <= %.1e, max wrong run %zu, release latency mean %.1f T max %.1f T",
                                    upper_95(o.score.bit_errors, o.score.matched * k_byte_bits), o.max_wrong_run,
                                    mean_latency - delay_slots, o.latency_max - delay_slots),
               gate_label("BER <= 1e-3, frames >= 95%" + prov(p), p.report_reason), pass, kind_of(p.report_reason));
        ledger_runs("A1' " + p.name, o, false);
    }
}

void test_a3_acquisition() {
    const std::vector<APoint>& points = all_points();
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    const double deltas[] = {0.0, -k_below_gate_db};
    const std::size_t counts[] = {k_a3_at_gate, k_a3_below_gate};
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::size_t bytes = std::max(k_a3_min_bytes, k_a3_frames * points[i].config.frame_bytes());
        for (std::size_t n = 0; n < 2; ++n) {
            add_jobs(jobs, point_of, 2 * i + n, points[i], deltas[n], counts[n], bytes, k_test_a3, false, true);
        }
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, point_of, 2 * points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const APoint& p = points[i];
        if (!p.removed_reason.empty()) continue;
        const std::size_t bytes = std::max(k_a3_min_bytes, k_a3_frames * p.config.frame_bytes());
        for (std::size_t n = 0; n < 2; ++n) {
            const Outcome& o = outcomes[2 * i + n][0];
            const double rate = static_cast<double>(o.locked_transmissions) / static_cast<double>(o.transmissions);
            const double gate = n == 0 ? k_a3_gate : k_a3_below_gate_rate;
            result("A3'",
                   point_text(p) + format(", %zu independent %zu-byte transmissions, ", o.transmissions, bytes) +
                       gate_text(p, deltas[n]) + (n == 0 ? " (gate)" : " (gate - 1)"),
                   format("locked %.2f%% (%s, %zu wrong-mode), header %zu/%zu, frames %.2f%%, wrong bytes %zu, ",
                          k_percent * rate, lock_text(o).c_str(), o.wrong_mode_locks, o.header_transmissions,
                          o.transmissions, k_percent * o.frames_ratio(), o.score.wrong_bytes) +
                       lost_text(o),
                   gate_label(format("locked >= %.0f%%", k_percent * gate) + prov(p), p.report_reason), rate >= gate,
                   kind_of(p.report_reason));
            ledger_runs(format("A3' %s %s", p.name.c_str(), n == 0 ? "gate" : "gate-1"), o, false);
        }
    }
}

void test_a4_snr_report() {
    std::vector<APoint> points;
    for (std::size_t i = 0; i < all_points().size(); ++i) {
        const APoint& p = all_points()[i];
        if (p.basis == Basis::spec && !p.fm_channel) points.push_back(p);
    }
    // The fm profile's report (4-sample blocks) on SSB AWGN, where the true SNR is known: fm_fast (T6, a 2167 Hz span
    // under f_ref 2650 Hz) is FM only (spec 1.4) and does not pass an SSB receiver, so the T8 k3 mode with the HF
    // placement carries it.
    APoint fm_profile = mode_point("T8 k3 centred (fm profile, SSB)", Preset::fm, 8, 3, 8, Spacing::standard,
                                   GridSide::below, k_a4_fm_profile_ssb_db);
    fm_profile.profile = Profile::fm;
    points.push_back(fm_profile);
    // G1: the fm mode's SSB/AM row, last so that the rows above keep their seeds.
    for (std::size_t i = 0; i < all_points().size(); ++i) {
        if (all_points()[i].basis == Basis::centred) points.push_back(all_points()[i]);
    }
    const std::size_t steps = sizeof(k_a4_steps_db) / sizeof(k_a4_steps_db[0]);
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (std::size_t s = 0; s < steps; ++s) {
            add_jobs(jobs, point_of, i * steps + s, points[i], k_a4_steps_db[s], k_a4_jobs * k_a4_transmissions,
                     k_a4_bytes, k_test_a4, false);
        }
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, point_of, points.size() * steps);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const APoint& p = points[i];
        std::string gate = "|mean - true| <= 1.5 dB";
        if (p.basis == Basis::centred) gate += " (G1: fm mode on SSB/AM)";
        if (p.basis == Basis::derived) gate += " (derived base)";
        for (std::size_t s = 0; s < steps; ++s) {
            const Outcome& o = outcomes[i * steps + s][0];
            const double truth = p.gate_db + k_a4_steps_db[s];
            std::vector<float> values = o.snr_db;
            double mean = 0.0;
            for (std::size_t n = 0; n < values.size(); ++n) mean += values[n];
            mean = values.empty() ? -99.0 : mean / static_cast<double>(values.size());
            std::sort(values.begin(), values.end());
            const double low = values.empty() ? -99.0 : values[values.size() / 20];
            const double high = values.empty() ? -99.0 : values[values.size() - 1 - values.size() / 20];
            const double error = mean - truth;
            result("A4",
                   point_text(p) + ", " + gate_text(p, k_a4_steps_db[s]) + format(" (gate + %g)", k_a4_steps_db[s]),
                   format("reported mean %+.2f dB (error %+.2f), 5-95%% %+.1f..%+.1f, %zu byte events", mean, error,
                          low, high, values.size()),
                   gate_label(gate, p.report_reason), !values.empty() && std::fabs(error) <= k_a4_tolerance_db,
                   kind_of(p.report_reason));
            ledger_runs(format("A4 %s +%g dB", p.name.c_str(), k_a4_steps_db[s]), o,
                        k_a4_steps_db[s] >= k_f6_margin_db);
        }
    }
}

// A5: the integrated decoder's released bits against the genie receiver (known timing and grid, spec 4.1
// bench) on the same audio, at gate - 1 dB.
void test_a5_genie() {
    std::vector<APoint> points;
    for (std::size_t i = 0; i < all_points().size(); ++i) {
        if (!all_points()[i].fm_channel) points.push_back(all_points()[i]);
    }
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    const std::size_t count = transmissions_for(k_genie_bits, k_ber_bytes);
    for (std::size_t i = 0; i < points.size(); ++i) {
        add_jobs(jobs, point_of, i, points[i], -k_below_gate_db, count, k_ber_bytes, k_test_a5, true);
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, point_of, points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const APoint& p = points[i];
        if (!p.removed_reason.empty()) continue;
        const Outcome& o = outcomes[i][0];
        const double r = ratio(o.score.ber(), o.genie_ber());
        std::string gate = "BER ratio <= 1.5, frames >= 50%";
        if (p.basis == Basis::centred) gate += " (G1: fm mode on SSB/AM)";
        if (p.basis == Basis::derived) gate += " (derived)";
        const std::string& reason = p.report_reason.empty() ? p.a5_report_reason : p.report_reason;
        result("A5", point_text(p) + ", " + gate_text(p, -k_below_gate_db) + " (gate - 1)",
               format("integrated BER %.2e (%zu bits, frames %.1f%%), genie BER %.2e (%zu bits), ratio %.2f",
                      o.score.ber(), o.score.matched * static_cast<std::size_t>(k_byte_bits),
                      k_percent * o.frames_ratio(), o.genie_ber(), o.genie_bits, r),
               gate_label(gate, reason), r <= k_genie_ratio_gate && delivered(o), kind_of(reason));
        ledger_runs("A5 " + p.name, o, false);
    }
}

// L5 long: +-1000 ppm on the transmitter clock (channel resampling), the receiver clock (resampling of the
// received audio) and both in opposite directions, 10-minute transmissions at T = 16 ms, gate + 3 dB; the
// opposite-direction cases also with N = 32 (a 528 ms frame drifts 1 ms per frame).
void test_l5_clock() {
    struct Case {
        double tx_ppm;
        double rx_ppm;
        std::uint8_t data_slots;
    };
    const Case cases[] = {{k_l5_ppm, 0.0, k_default_data_slots},        {-k_l5_ppm, 0.0, k_default_data_slots},
                          {0.0, k_l5_ppm, k_default_data_slots},        {0.0, -k_l5_ppm, k_default_data_slots},
                          {k_l5_ppm, -k_l5_ppm, k_default_data_slots}, {-k_l5_ppm, k_l5_ppm, k_default_data_slots},
                          {k_l5_ppm, -k_l5_ppm, k_long_frame},         {-k_l5_ppm, k_l5_ppm, k_long_frame}};
    const std::size_t count = sizeof(cases) / sizeof(cases[0]);
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    for (std::size_t c = 0; c < count; ++c) {
        const EncoderConfig config = mode_config(Preset::hf_fast, k_l5_slot_ms, k_l5_bits, cases[c].data_slots,
                                                 Spacing::standard, GridSide::below);
        const double frame_s = (config.data_slots + 1.0) * k_l5_slot_ms / k_ms_per_s;
        const std::size_t bytes = static_cast<std::size_t>(k_l5_minutes * k_s_per_min / frame_s) * config.frame_bytes();
        JobPlan job;
        const std::uint32_t seed = seed_of(k_test_l5, static_cast<std::uint32_t>(c), 0);
        job.transmissions.push_back(random_tx(config, bytes, seed));
        job.channel.mode = sim::Mode::usb;
        job.channel.snr_db = k_l5_gate_db + k_f6_margin_db;
        job.channel.clock_ppm = cases[c].tx_ppm;
        job.channel.seed = seed;
        job.rx_ppm = cases[c].rx_ppm;
        job.decoders.push_back(DecoderConfig::for_profile(Profile::ssb));
        jobs.push_back(job);
        point_of.push_back(c);
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, point_of, count);
    for (std::size_t c = 0; c < count; ++c) {
        const Outcome& o = outcomes[c][0];
        const Score& s = o.score;
        const bool pass = s.wrong_bytes == 0 && s.lost_bytes == 0 && s.extra_bytes == 0 && s.locks == 1 &&
                          o.locked_transmissions == 1 && s.lost_events == 0;
        result("L5",
               format("TX %+g ppm, RX %+g ppm, %g min at T=16 ms k4 N%u, usb AWGN %+.1f dB (gate + 3)", cases[c].tx_ppm,
                      cases[c].rx_ppm, k_l5_minutes, cases[c].data_slots, k_l5_gate_db + k_f6_margin_db),
               format("%zu bytes: wrong %zu, lost %zu, extra %zu, locks %zu (%zu with the sent mode), flywheel %zu, ",
                      s.bytes_sent, s.wrong_bytes, s.lost_bytes, s.extra_bytes, s.locks, o.locked_transmissions,
                      s.flywheel_bytes) +
                   lost_text(o),
               "0 slips (1 lock with the exact T, 0 lost/extra), 0 errors", pass);
        ledger_runs(format("L5 case %zu", c), o, true);
    }
}

}  // namespace regression
}  // namespace unlimited
