#include "regression.hpp"

#include "test_harness.hpp"

#include <cmath>
#include <random>

// C1-C15 (spec 8.4): hf_slow (T = 32 ms, N = 8) unless stated; hf (T = 16 ms) run beside it on the fading paths.
// Every transmission carries packets, so every point feeds F5; each test's point at its own gate SNR + 3 dB (as
// v0.1b) and the other points at or above it feed the F6 gate.
namespace unlimited {
namespace regression {

namespace {

const std::uint32_t k_test_c = 100;

const double k_bits_gate = 2e5;       // BER-gated usb points
const double k_bits_side = 1e5;       // loss, F6 and report points
const double k_bits_rf = 1e5;         // BER-gated AM/FM points (a 4-6x slower channel)
const double k_bits_low_ber = 1.2e4;       // "BER <= 1e-4 in >= 1e4 bits" (gate decision G5)
const double k_bits_low_ber_long = 2.4e4;  // the same with more margin (blanker deadlock, evidence rows)
const double k_bits_blanker = 4e5;    // C6 blanker on/off ratio on clean AWGN at the A1 point
const std::size_t k_pair = 2;         // the two arms of a comparison (gate point and gate + 3, no AGC and AGC)
const double k_bits_ratio = 1e6;      // ratio gates at the A1 point (tens of errors per arm)
const double k_min_gate_bits = 1e4;
const std::size_t k_bytes = 300;      // bytes per transmission (packets)
const std::size_t k_trial_bytes = 16;  // acquisition trials: one packet
const std::size_t k_trials = 100;
const double k_preroll_ms = 4000.0;   // interferer on air before the transmission (steady mask needs 2.56 s)
const double k_fading_peak = 3.0;     // Rayleigh amplitude above 3x RMS: probability 1.2e-4
const double k_fm_peak = 2.0;         // FM clicks
const double k_f6_margin_db = 3.0;
const std::size_t k_bits_per_byte_count = k_bits_per_byte;

const double k_high_snr_db = 30.0;    // C2, C3, C4, C12, C13, C14
const double k_good_snr_db = 25.0;    // C1
const double k_fading_report_db = 20.0;
const double k_qsb_snr_db = 15.0;     // C5, at the crest
const double k_qsb_depth_db = 20.0;
const double k_qsb_rate_hz = 0.2;
const double k_qrn_snr_db = 6.0;      // C6
const double k_qrn_rate_hz = 20.0;
const double k_qrn_amplitude = 30.0;  // x the key-down amplitude
const double k_db_per_decade = 20.0;
const double k_agc_attack_ms = 1.0;
const double k_agc_decay_ms = 300.0;
const double k_carrier_snr_db = 0.0;  // C8
const double k_carrier_db = 6.0;
const double k_strong_carrier_db = 12.0;
const double k_carrier_offsets_hz[] = {-1000.0, -500.0, -250.0, 250.0, 500.0, 1000.0};
const double k_near_carrier_hz[] = {-100.0, 100.0};
// Acquisition at 250 Hz with weaker carriers, and a +12 dB carrier 300..400 Hz away.
const double k_evidence_offsets_hz[] = {250.0, 250.0, 300.0, 350.0, 400.0};
const double k_evidence_levels_db[] = {6.0, 9.0, 12.0, 12.0, 12.0};
// Gate decision C8 (spec 8.4): acquisition is gated for a +6 dB carrier from 250 Hz away and for a +12 dB carrier
// from 350 Hz away; a +12 dB carrier 250..300 Hz away (and +9 dB at 250 Hz) is reported (open problem, spec 11.2).
const double k_strong_gate_offset_hz = 350.0;
const double k_cw_snr_db = 3.0;       // C9
const double k_cw_offsets_hz[] = {-300.0, 300.0};
const double k_cw_db = 0.0;           // equal PEP
const double k_cw_wpm = 20.0;
const double k_am_cnr_db = 2.0;       // C11: hf_slow
// Gate decision C11 (spec 8.4): the am preset (8 ms slots, a quarter of the energy of hf_slow's 32 ms) is gated at
// CNR 6 dB; below it, reported.
const double k_am_preset_cnr_db = 6.0;
const double k_am_sweep_cnr_db[] = {2.0, 3.0, 4.0, 5.0};  // the am preset below its gate (report)
const double k_fm_gate_cnr_db = 6.0;  // C10, C15
const double k_shift_margin_hz = 10.0;  // C14: tolerance - 10 Hz

const double k_ber_gate_awgn = 1e-3;
const double k_percent = 100.0;
const double k_per_mille = 1000.0;

struct CPoint {
    std::string id;
    std::string condition;
    EncoderConfig config;
    sim::ChannelConfig channel;
    double peak_factor;
    std::vector<DecoderConfig> decoders;
    double bits;
    std::size_t bytes;
    double lead_ms;
    bool genie;
    bool f6;           // at >= its gate + 3 dB
    bool independent;  // one transmission per job (acquisition trials)
    long seed_point;   // points with the same seed_point get the same data, noise and fading; -1 = own
};

CPoint make_point(const std::string& id, const std::string& condition, const EncoderConfig& config,
                  const sim::ChannelConfig& channel, const DecoderConfig& receiver, double bits, bool f6) {
    CPoint p;
    p.id = id;
    p.condition = condition;
    p.config = config;
    p.channel = channel;
    p.peak_factor = channel.fading ? k_fading_peak : (channel.mode == sim::Mode::fm ? k_fm_peak : 1.0);
    p.decoders.push_back(receiver);
    p.bits = bits;
    p.bytes = k_bytes;
    p.lead_ms = JobPlan().lead_ms;
    p.genie = false;
    p.f6 = f6;
    p.independent = false;
    p.seed_point = -1;
    return p;
}

sim::ChannelConfig usb(double snr_db, sim::FadingPreset fading = sim::FadingPreset::none) {
    sim::ChannelConfig c;
    c.mode = sim::Mode::usb;
    c.snr_db = snr_db;
    sim::apply_preset(c, fading);
    return c;
}

sim::ChannelConfig fm_channel(double cnr_db, bool tx_preemphasis) {
    sim::ChannelConfig c;
    c.mode = sim::Mode::fm;
    c.snr_db = snr_for_fm_cnr(cnr_db, c);
    c.fm_tx_preemphasis = tx_preemphasis;
    c.fm_rx_deemphasis = true;
    return c;
}

sim::ChannelConfig am_channel(double cnr_db) {
    sim::ChannelConfig c;
    c.mode = sim::Mode::am;
    c.snr_db = snr_for_am_cnr(cnr_db, c);
    return c;
}

DecoderConfig ssb() {
    return DecoderConfig::for_profile(Profile::ssb);
}

std::string name_of(const EncoderConfig& config) {
    const Preset presets[] = {Preset::hf_slow, Preset::hf, Preset::hf_fast, Preset::am, Preset::fm};
    for (std::size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); ++i) {
        const EncoderConfig p = preset(presets[i]);
        if (p.slot_us == config.slot_us && p.bits_per_package == config.bits_per_package &&
            p.tone_hz == config.tone_hz) {
            return preset_name(presets[i]) + " (" + config_text(config) + ")";
        }
    }
    return config_text(config);
}

// Builds and runs the jobs of every point; result [point][decoder].
std::vector<std::vector<Outcome> > run_cpoints(const std::vector<CPoint>& points, std::uint32_t test) {
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const CPoint& p = points[i];
        if (!p.config.valid()) test::fail(__FILE__, __LINE__, "invalid sender: " + p.condition);
        const std::size_t count = transmissions_for(p.bits, p.bytes);
        const std::size_t per_job =
            p.independent ? 1 : transmissions_per_job(transmission_seconds(p.config, p.bytes, JobPlan().gap_ms), count);
        std::uint32_t job_index = 0;
        for (std::size_t first = 0; first < count; first += per_job, ++job_index) {
            const std::size_t seed_point = p.seed_point < 0 ? i : static_cast<std::size_t>(p.seed_point);
            const std::uint32_t seed = seed_of(test, static_cast<std::uint32_t>(seed_point), job_index);
            JobPlan job;
            for (std::size_t t = first; t < std::min(count, first + per_job); ++t) {
                job.transmissions.push_back(packet_tx(p.config, p.bytes, data_seed(seed, t)));
            }
            job.lead_ms = p.lead_ms;
            job.channel = p.channel;
            job.channel.seed = seed;
            job.peak_factor = p.peak_factor;
            job.decoders = p.decoders;
            job.genie = p.genie;
            jobs.push_back(job);
            point_of.push_back(i);
        }
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, point_of, points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (std::size_t d = 0; d < outcomes[i].size(); ++d) {
            const std::string decoder = d > 0 ? format(" [decoder %zu]", d) : "";
            const std::string where = points[i].id + " " + points[i].condition + decoder;
            ledger_packets(where, outcomes[i][d]);
            ledger_runs(where, outcomes[i][d], points[i].f6);
        }
    }
    return outcomes;
}

std::string measured(const Outcome& o) {
    return ber_text(o) + format(", BER95 <= %.1e, max wrong run %zu",
                                upper_95(o.score.bit_errors, static_cast<double>(o.score.matched * k_bits_per_byte_count)),
                                o.max_wrong_run);
}

std::string with_gate(const std::string& condition, bool at_gate_plus_3) {
    return at_gate_plus_3 ? condition + " (gate + 3)" : condition;
}

void report(const CPoint& p, const Outcome& o, const std::string& why = "report") {
    result(p.id, p.condition, measured(o), p.f6 ? why + " (F6 point)" : why, true, Kind::report);
}

double impulse_db() {
    return k_db_per_decade * std::log10(k_qrn_amplitude);
}

// C1-C4: one fading preset, hf_slow gated and hf reported at the same SNRs.
struct FadingRow {
    double snr_db;
    bool gated;   // for hf_slow
    bool f6;
    double bits;
};

}  // namespace

void test_c1_ccir_good() {
    const FadingRow rows[] = {{k_good_snr_db, true, false, k_bits_gate},
                              {10.0, true, false, k_bits_side},
                              {15.0, true, false, k_bits_side},
                              {20.0, true, false, k_bits_side},
                              {k_good_snr_db + k_f6_margin_db, true, true, k_bits_side}};
    const std::size_t row_count = sizeof(rows) / sizeof(rows[0]);
    const Preset presets[] = {Preset::hf_slow, Preset::hf};
    std::vector<CPoint> points;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        for (std::size_t r = 0; r < row_count; ++r) {
            const EncoderConfig config = preset(presets[p]);
            points.push_back(make_point("C1",
                                        with_gate(format("CCIR good (0.5 ms, 0.1 Hz), %s, %.0f dB", name_of(config).c_str(),
                                                         rows[r].snr_db),
                                                  rows[r].f6),
                                        config, usb(rows[r].snr_db, sim::FadingPreset::ccir_good), ssb(), rows[r].bits,
                                        rows[r].f6));
        }
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 1);
    const double k_ber_gate = 1e-3;
    const double k_loss_gate = 0.02;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        for (std::size_t r = 0; r < row_count; ++r) {
            const std::size_t i = p * row_count + r;
            const Outcome& out = o[i][0];
            const bool ber_row = r == 0;
            const bool pass = ber_row ? out.score.ber() <= k_ber_gate && delivered(out)
                                      : out.score.loss() <= k_loss_gate;
            const std::string gate = ber_row ? "BER <= 1e-3, delivered >= 50%" : "loss <= 2%";
            if (p == 0) {
                result("C1", points[i].condition, measured(out), gate, pass);
            } else {
                report(points[i], out, "report (hf_slow's gate: " + gate + (pass ? ", met)" : ", NOT met)"));
            }
        }
    }
}

// C2: the gates for hf_slow and hf at 30 dB, and the ablation of spec 4.2 on the same audio (genie receiver with
// known timing: START-STOP line <= 0.67 x START only and <= 0.2 x one fixed level).
void test_c2_ccir_moderate() {
    const sim::FadingPreset moderate = sim::FadingPreset::ccir_moderate;
    std::vector<CPoint> points;
    const Preset gated[] = {Preset::hf_slow, Preset::hf};
    for (std::size_t p = 0; p < test::count_of(gated); ++p) {
        const EncoderConfig config = preset(gated[p]);
        CPoint point = make_point("C2", format("CCIR moderate (1 ms, 0.5 Hz), %s, %.0f dB", name_of(config).c_str(),
                                               k_high_snr_db),
                                  config, usb(k_high_snr_db, moderate), ssb(), k_bits_gate, false);
        point.genie = true;
        points.push_back(point);
    }
    for (std::size_t p = 0; p < test::count_of(gated); ++p) {
        const EncoderConfig config = preset(gated[p]);
        points.push_back(make_point("C2",
                                    with_gate(format("CCIR moderate, %s, %.0f dB", name_of(config).c_str(),
                                                     k_high_snr_db + k_f6_margin_db),
                                              true),
                                    config, usb(k_high_snr_db + k_f6_margin_db, moderate), ssb(), k_bits_side, true));
        points.push_back(make_point("C2", format("CCIR moderate, %s, %.0f dB", name_of(config).c_str(), k_fading_report_db),
                                    config, usb(k_fading_report_db, moderate), ssb(), k_bits_side, false));
    }
    const Preset others[] = {Preset::hf_fast, Preset::am};
    for (std::size_t p = 0; p < test::count_of(others); ++p) {
        const EncoderConfig config = preset(others[p]);
        points.push_back(make_point("C2", format("CCIR moderate, %s, %.0f dB", name_of(config).c_str(), k_high_snr_db),
                                    config, usb(k_high_snr_db, moderate), ssb(), k_bits_side, false));
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 2);
    const double k_ber_gate = 2.5e-2;
    const double k_start_only_ratio = 0.67;
    const double k_fixed_ratio = 0.2;
    for (std::size_t p = 0; p < test::count_of(gated); ++p) {
        const Outcome& main = o[p][0];
        result("C2", points[p].condition, measured(main), "BER <= 2.5e-2, delivered >= 50%",
               main.score.ber() <= k_ber_gate && delivered(main));
        const double interpolated = main.genie_ber(genie_interpolated);
        const double start_only = main.genie_ber(genie_start_only);
        const double fixed = main.genie_ber(genie_fixed_level);
        const double r_start = ratio(interpolated, start_only);
        const double r_fixed = ratio(interpolated, fixed);
        const std::string text = format("known timing (%zu bits): START-STOP line %.2e, START only %.2e (ratio %.2f), "
                                        "fixed level %.2e (ratio %.3f); decoder %.2e",
                                        main.genie_bits, interpolated, start_only, r_start, fixed, r_fixed,
                                        main.score.ber());
        const bool pass = main.genie_bits > 0 && r_start <= k_start_only_ratio && r_fixed <= k_fixed_ratio;
        if (p == 0) {
            result("C2", points[p].condition + ", ablation", text,
                   "START-STOP <= 0.67 x START only and <= 0.2 x fixed level", pass);
        } else {
            result("C2", points[p].condition + ", ablation", text,
                   std::string("report (the ablation gate is set at T = 32 ms, spec 4.2; ") + (pass ? "met)" : "NOT met)"),
                   true, Kind::report);
        }
    }
    for (std::size_t i = test::count_of(gated); i < points.size(); ++i) report(points[i], o[i][0]);
}

void test_c3_ccir_poor() {
    const sim::FadingPreset poor = sim::FadingPreset::ccir_poor;
    const FadingRow rows[] = {{k_high_snr_db, true, false, k_bits_gate},
                              {k_high_snr_db + k_f6_margin_db, false, true, k_bits_side},
                              {k_fading_report_db, false, false, k_bits_side}};
    const std::size_t row_count = sizeof(rows) / sizeof(rows[0]);
    const Preset presets[] = {Preset::hf_slow, Preset::hf};
    std::vector<CPoint> points;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        for (std::size_t r = 0; r < row_count; ++r) {
            const EncoderConfig config = preset(presets[p]);
            points.push_back(make_point("C3",
                                        with_gate(format("CCIR poor (2 ms, 1 Hz), %s, %.0f dB", name_of(config).c_str(),
                                                         rows[r].snr_db),
                                                  rows[r].f6),
                                        config, usb(rows[r].snr_db, poor), ssb(), rows[r].bits, rows[r].f6));
        }
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 3);
    const double k_ber_gate = 6e-2;
    const double k_airtime_gate = 0.90;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        for (std::size_t r = 0; r < row_count; ++r) {
            const std::size_t i = p * row_count + r;
            const Outcome& out = o[i][0];
            const double airtime = out.airtime > 0.0 ? out.locked_airtime / out.airtime : 0.0;
            const bool pass = out.score.ber() <= k_ber_gate && airtime >= k_airtime_gate && delivered(out);
            const std::string text = measured(out) + format(", locked %.1f%% of the airtime", k_percent * airtime);
            if (p == 0 && rows[r].gated) {
                result("C3", points[i].condition, text, "BER <= 6e-2, locked >= 90% of the airtime", pass);
            } else {
                const std::string verdict = pass ? "met)" : "NOT met)";
                result("C3", points[i].condition, text,
                       std::string(rows[r].f6 ? "report (F6 point; " : "report (") +
                           "hf_slow's 30 dB gate BER <= 6e-2, locked >= 90%: " + verdict,
                       true, Kind::report);
            }
        }
    }
}

void test_c4_flat_rayleigh() {
    const FadingRow rows[] = {{k_high_snr_db, true, false, k_bits_gate},
                              {k_high_snr_db + k_f6_margin_db, false, true, k_bits_side},
                              {k_fading_report_db, false, false, k_bits_side}};
    const std::size_t row_count = sizeof(rows) / sizeof(rows[0]);
    const Preset presets[] = {Preset::hf_slow, Preset::hf};
    std::vector<CPoint> points;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        for (std::size_t r = 0; r < row_count; ++r) {
            const EncoderConfig config = preset(presets[p]);
            points.push_back(make_point("C4",
                                        with_gate(format("flat Rayleigh 1 Hz, %s, %.0f dB", name_of(config).c_str(),
                                                         rows[r].snr_db),
                                                  rows[r].f6),
                                        config, usb(rows[r].snr_db, sim::FadingPreset::flat), ssb(), rows[r].bits,
                                        rows[r].f6));
        }
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 4);
    const double k_ber_gate = 1.5e-2;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        for (std::size_t r = 0; r < row_count; ++r) {
            const std::size_t i = p * row_count + r;
            const Outcome& out = o[i][0];
            const bool pass = out.score.ber() <= k_ber_gate && delivered(out);
            if (p == 0 && rows[r].gated) {
                result("C4", points[i].condition, measured(out), "BER <= 1.5e-2, delivered >= 50%", pass);
            } else {
                report(points[i], out,
                       std::string("report (hf_slow's 30 dB gate BER <= 1.5e-2: ") + (pass ? "met)" : "NOT met)"));
            }
        }
    }
}

void test_c5_qsb() {
    const double snrs[] = {k_qsb_snr_db, k_qsb_snr_db + k_f6_margin_db};
    const Preset presets[] = {Preset::hf_slow, Preset::hf};
    std::vector<CPoint> points;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        for (std::size_t i = 0; i < test::count_of(snrs); ++i) {
            sim::ChannelConfig c = usb(snrs[i]);
            c.qsb_depth_db = k_qsb_depth_db;
            c.qsb_rate_hz = k_qsb_rate_hz;
            const EncoderConfig config = preset(presets[p]);
            points.push_back(make_point("C5",
                                        with_gate(format("QSB 20 dB at 0.2 Hz, %s, %.0f dB at the crest",
                                                         name_of(config).c_str(), snrs[i]),
                                                  i == 1),
                                        config, c, ssb(), i == 0 ? k_bits_gate : k_bits_side, i == 1));
        }
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 5);
    const double k_correct_gate = 0.95;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Outcome& out = o[i][0];
        const double correct = out.correct();
        const std::string text = measured(out) + format(", bytes correct %.2f%%", k_percent * correct);
        if (i == 0) {
            result("C5", points[i].condition, text, ">= 95% of bytes correct", correct >= k_correct_gate);
        } else {
            result("C5", points[i].condition, text,
                   std::string(points[i].f6 ? "report (F6 point; " : "report (") + ">= 95% correct: " +
                       (correct >= k_correct_gate ? "met)" : "NOT met)"),
                   true, Kind::report);
        }
    }
}

// C6: QRN on hf_slow with the blanker (on and off), and the blanker's cost on clean AWGN at the A1 point.
void test_c6_qrn() {
    DecoderConfig off = ssb();
    off.impulse_blanker = false;
    const EncoderConfig hf_slow = preset(Preset::hf_slow);
    std::vector<CPoint> points;
    const double snrs[] = {k_qrn_snr_db, k_qrn_snr_db + k_f6_margin_db};
    for (std::size_t i = 0; i < test::count_of(snrs); ++i) {
        sim::ChannelConfig c = usb(snrs[i]);
        c.impulse_rate_hz = k_qrn_rate_hz;
        c.impulse_level_db = impulse_db();
        CPoint p = make_point("C6",
                              with_gate(format("QRN 20/s at 30x key-down (%+.1f dB), %s, %+.0f dB", impulse_db(),
                                               name_of(hf_slow).c_str(), snrs[i]),
                                        i == 1),
                              hf_slow, c, ssb(), i == 0 ? k_bits_gate : k_bits_side, i == 1);
        p.decoders.push_back(off);
        points.push_back(p);
    }
    const double a1_db = gate_db(slot_ms_of(hf_slow));
    CPoint clean = make_point("C6",
                              format("clean AWGN, %s, %+.1f dB (A1 point), blanker on vs off, same audio",
                                     name_of(hf_slow).c_str(), a1_db),
                              hf_slow, usb(a1_db), ssb(), k_bits_blanker, false);
    clean.decoders.push_back(off);
    points.push_back(clean);
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 6);
    const double k_ber_gate = 2e-3;
    const double k_ratio_gate = 1.2;
    result("C6", points[0].condition, measured(o[0][0]), "BER <= 2e-3 with the blanker, delivered >= 50%",
           o[0][0].score.ber() <= k_ber_gate && delivered(o[0][0]));
    result("C6", points[0].condition + ", blanker off", measured(o[0][1]), "report", true, Kind::report);
    report(points[1], o[1][0]);
    const Outcome& on = o[2][0];
    const Outcome& none = o[2][1];
    const double r = ratio(on.score.ber(), none.score.ber());
    result("C6", points[2].condition,
           format("on: %zu bit errors (BER %.2e, delivered %.2f%%), off: %zu (BER %.2e, delivered %.2f%%), ratio %.2f",
                  on.score.bit_errors, on.score.ber(), k_percent * on.delivered(), none.score.bit_errors,
                  none.score.ber(), k_percent * none.delivered(), r),
           "BER on/off <= 1.2, delivered >= 50%", r <= k_ratio_gate && delivered(on) && delivered(none));
}

// C7: receiver AGC 1 ms / 300 ms at the A1 point against no AGC on the same data, noise and seeds.
void test_c7_agc() {
    const Preset presets[] = {Preset::hf_slow, Preset::hf};
    std::vector<CPoint> points;
    std::vector<std::string> conditions;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        const EncoderConfig config = preset(presets[p]);
        const double a1_db = gate_db(slot_ms_of(config));
        const double bits = p == 0 ? k_bits_ratio : k_bits_gate;
        const std::string where = format("%s, %+.1f dB (A1 point)", name_of(config).c_str(), a1_db);
        conditions.push_back("AGC 1 ms / 300 ms vs none, same seeds, " + where);
        sim::ChannelConfig c = usb(a1_db);
        const long twin = static_cast<long>(points.size());
        points.push_back(make_point("C7", "no AGC, " + where, config, c, ssb(), bits, false));
        c.agc = true;
        c.agc_attack_ms = k_agc_attack_ms;
        c.agc_decay_ms = k_agc_decay_ms;
        points.push_back(make_point("C7", "AGC 1/300 ms, " + where, config, c, ssb(), bits, false));
        points.back().seed_point = twin;
    }
    {
        const EncoderConfig config = preset(Preset::hf_slow);
        sim::ChannelConfig c = usb(gate_db(slot_ms_of(config)) + k_f6_margin_db);
        c.agc = true;
        c.agc_attack_ms = k_agc_attack_ms;
        c.agc_decay_ms = k_agc_decay_ms;
        points.push_back(make_point("C7",
                                    with_gate(format("AGC 1/300 ms, %s, %+.1f dB", name_of(config).c_str(), c.snr_db),
                                              true),
                                    config, c, ssb(), k_bits_side, true));
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 7);
    const double k_ratio_gate = 2.0;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        const Outcome& none = o[k_pair * p][0];
        const Outcome& agc = o[k_pair * p + 1][0];
        const double r = ratio(agc.score.ber(), none.score.ber());
        const bool pass = r <= k_ratio_gate && delivered(agc) && delivered(none);
        const std::string text =
            format("AGC: %zu bit errors (BER %.2e, delivered %.2f%%), none: %zu (BER %.2e, delivered %.2f%%), "
                   "ratio %.2f; AGC %s",
                   agc.score.bit_errors, agc.score.ber(), k_percent * agc.delivered(), none.score.bit_errors,
                   none.score.ber(), k_percent * none.delivered(), r, lost_text(agc).c_str());
        if (p == 0) {
            result("C7", conditions[p], text, "BER AGC <= 2 x no AGC, delivered >= 50%", pass);
        } else {
            result("C7", conditions[p], text,
                   std::string("report (the gate is set for hf_slow: ") + (pass ? "met)" : "NOT met)"), true,
                   Kind::report);
        }
    }
    report(points.back(), o.back()[0]);
}

// C8: a steady carrier on air from 4 s before the transmission (the steady mask needs 2.56 s).
void test_c8_carrier() {
    const EncoderConfig hf_slow = preset(Preset::hf_slow);
    const std::size_t offsets = sizeof(k_carrier_offsets_hz) / sizeof(k_carrier_offsets_hz[0]);
    const std::size_t near = sizeof(k_near_carrier_hz) / sizeof(k_near_carrier_hz[0]);
    std::vector<CPoint> points;
    const auto add = [&](double offset_hz, double carrier_db, double snr_db, bool trials, bool f6) {
        sim::ChannelConfig c = usb(snr_db);
        c.carrier_hz = hf_slow.tone_hz + offset_hz;
        c.carrier_db = carrier_db;
        CPoint p = make_point("C8",
                              with_gate(format("steady carrier %+.0f dB at %+.0f Hz (%.0f Hz), %s, %+.0f dB%s",
                                               carrier_db, offset_hz, c.carrier_hz, name_of(hf_slow).c_str(), snr_db,
                                               trials ? format(", %zu independent trials", k_trials).c_str() : ""),
                                        f6),
                              hf_slow, c, ssb(),
                              trials ? static_cast<double>(k_trials * k_trial_bytes * k_bits_per_byte_count) : k_bits_side,
                              f6);
        p.lead_ms = k_preroll_ms;
        p.peak_factor = 1.0 + amplitude_of_db(carrier_db);
        if (trials) {
            p.bytes = k_trial_bytes;
            p.independent = true;
        }
        points.push_back(p);
    };
    for (std::size_t i = 0; i < offsets; ++i) add(k_carrier_offsets_hz[i], k_carrier_db, k_carrier_snr_db, false, false);
    for (std::size_t i = 0; i < offsets; ++i) add(k_carrier_offsets_hz[i], k_strong_carrier_db, k_carrier_snr_db, true, false);
    for (std::size_t i = 0; i < near; ++i) add(k_near_carrier_hz[i], k_carrier_db, k_carrier_snr_db, false, false);
    for (std::size_t i = 0; i < near; ++i) add(k_near_carrier_hz[i], k_strong_carrier_db, k_carrier_snr_db, true, false);
    add(k_carrier_offsets_hz[2], k_carrier_db, k_carrier_snr_db + k_f6_margin_db, false, true);
    add(k_carrier_offsets_hz[3], k_carrier_db, k_carrier_snr_db + k_f6_margin_db, false, true);
    const std::size_t first_evidence = points.size();
    const std::size_t evidence = sizeof(k_evidence_offsets_hz) / sizeof(k_evidence_offsets_hz[0]);
    for (std::size_t n = 0; n < evidence; ++n) {
        add(k_evidence_offsets_hz[n], k_evidence_levels_db[n], k_carrier_snr_db, true, false);
        add(-k_evidence_offsets_hz[n], k_evidence_levels_db[n], k_carrier_snr_db, true, false);
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 8);
    const double k_acquisition_gate = 0.90;
    std::size_t i = 0;
    for (std::size_t n = 0; n < offsets; ++n, ++i) {
        result("C8", points[i].condition, measured(o[i][0]), "BER <= 1e-3, delivered >= 50%",
               o[i][0].score.ber() <= k_ber_gate_awgn && delivered(o[i][0]));
    }
    const auto acquisition = [&](std::size_t k, Kind kind, const std::string& gate) {
        const Outcome& a = o[k][0];
        const double rate = static_cast<double>(a.locked_transmissions) / static_cast<double>(a.transmissions);
        const bool pass = rate >= k_acquisition_gate;
        result("C8", points[k].condition,
               format("locked %.1f%% (%zu/%zu, %zu wrong T/N, %zu stray), delivered %.1f%%, wrong bytes %zu, "
                      "shifted %zu, ",
                      k_percent * rate, a.locked_transmissions, a.transmissions, a.wrong_locks, a.stray_locks,
                      k_percent * a.delivered(), a.score.wrong_bytes, a.misplaced_bytes) +
                   lost_text(a),
               kind == Kind::gate ? gate : gate + (pass ? ": met)" : ": NOT met)"), pass, kind);
    };
    // Gate decision C8: which carriers the acquisition gate covers.
    const auto gated = [&](double offset_hz, double carrier_db) {
        return carrier_db <= k_carrier_db ||
               (carrier_db >= k_strong_carrier_db && std::fabs(offset_hz) >= k_strong_gate_offset_hz);
    };
    const std::string acquisition_gate = "acquisition >= 90% (gate decision C8: +6 dB from 250 Hz, +12 dB from 350 Hz)";
    const std::string close_report =
        "report (gate decision C8: a carrier above +6 dB closer than 350 Hz is an open problem, spec 11.2; acquisition "
        ">= 90%";
    for (std::size_t n = 0; n < offsets; ++n, ++i) {
        if (gated(k_carrier_offsets_hz[n], k_strong_carrier_db)) {
            acquisition(i, Kind::gate, acquisition_gate);
        } else {
            acquisition(i, Kind::report, close_report);
        }
    }
    for (std::size_t n = 0; n < near; ++n, ++i) {
        report(points[i], o[i][0], "report (100 Hz away, spec 11 q10; v0.1b: no lock)");
    }
    for (std::size_t n = 0; n < near; ++n, ++i) {
        acquisition(i, Kind::report, "report (100 Hz away; v0.1b: no lock; acquisition >= 90%");
    }
    for (; i < first_evidence; ++i) report(points[i], o[i][0]);
    for (std::size_t n = 0; i < points.size(); ++i, ++n) {
        const double level_db = k_evidence_levels_db[n / 2];
        const double offset_hz = (n % 2 == 0 ? 1.0 : -1.0) * k_evidence_offsets_hz[n / 2];
        if (gated(offset_hz, level_db)) {
            acquisition(i, Kind::gate, acquisition_gate);
        } else {
            acquisition(i, Kind::report, close_report);
        }
    }
}

void test_c9_cw() {
    const EncoderConfig hf_slow = preset(Preset::hf_slow);
    std::vector<CPoint> points;
    const std::size_t sides = sizeof(k_cw_offsets_hz) / sizeof(k_cw_offsets_hz[0]);
    for (std::size_t f6 = 0; f6 < k_pair; ++f6) {
        for (std::size_t i = 0; i < sides; ++i) {
            sim::ChannelConfig c = usb(k_cw_snr_db + (f6 != 0 ? k_f6_margin_db : 0.0));
            c.cw_hz = hf_slow.tone_hz + k_cw_offsets_hz[i];
            c.cw_db = k_cw_db;
            c.cw_wpm = k_cw_wpm;
            CPoint p = make_point("C9",
                                  with_gate(format("keyed CW 20 WPM, equal PEP, at %+.0f Hz (%.0f Hz), %s, %+.0f dB",
                                                   k_cw_offsets_hz[i], c.cw_hz, name_of(hf_slow).c_str(), c.snr_db),
                                            f6 != 0),
                                  hf_slow, c, ssb(), f6 != 0 ? k_bits_side : k_bits_gate, f6 != 0);
            p.lead_ms = k_preroll_ms;
            p.peak_factor = 1.0 + amplitude_of_db(k_cw_db);
            points.push_back(p);
        }
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 9);
    const double k_ber_gate = 2e-3;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (points[i].f6) {
            report(points[i], o[i][0]);
            continue;
        }
        result("C9", points[i].condition, measured(o[i][0]), "BER <= 2e-3, delivered >= 50%",
               o[i][0].score.ber() <= k_ber_gate && delivered(o[i][0]));
    }
}

// C10 (pre- and de-emphasis) and C15 (flat TX, de-emphasising RX): the fm preset and hf over FM, fm profile. From
// CNR 8 dB on, gate decision G5 (spec 0.8): BER <= 1e-4 with 0 extra and 0 shifted bytes (it was 0 bit errors).
void run_fm(const char* id, bool preemphasis, std::uint32_t test) {
    enum Gate { ber_gate, low_ber, low_ber_no_loss };
    struct Row {
        double cnr_db;
        Gate gate;
        double bits;
    };
    const Row rows[] = {{k_fm_gate_cnr_db, ber_gate, k_bits_rf},
                        {8.0, low_ber, k_bits_low_ber},
                        {10.0, low_ber, k_bits_low_ber},
                        {14.0, low_ber_no_loss, k_bits_low_ber_long}};
    const std::size_t row_count = sizeof(rows) / sizeof(rows[0]);
    const Preset presets[] = {Preset::fm, Preset::hf};
    std::vector<CPoint> points;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        const EncoderConfig config = preset(presets[p]);
        for (std::size_t r = 0; r < row_count; ++r) {
            const sim::ChannelConfig c = fm_channel(rows[r].cnr_db, preemphasis);
            const bool f6 = rows[r].cnr_db >= k_fm_gate_cnr_db + k_f6_margin_db;
            points.push_back(make_point(id,
                                        format("FM %s TX + de-emphasis RX, %s, fm profile, CNR %.0f dB (snr %.1f dB)%s",
                                               preemphasis ? "pre-emphasis" : "flat", name_of(config).c_str(),
                                               rows[r].cnr_db, c.snr_db, f6 ? " (>= gate + 3)" : ""),
                                        config, c, DecoderConfig::for_profile(Profile::fm), rows[r].bits, f6));
        }
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, test);
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        for (std::size_t r = 0; r < row_count; ++r) {
            const std::size_t i = p * row_count + r;
            const Outcome& out = o[i][0];
            const loopback::Score& s = out.score;
            const bool enough = static_cast<double>(s.matched * k_bits_per_byte_count) >= k_min_gate_bits;
            switch (rows[r].gate) {
            case ber_gate:
                result(id, points[i].condition, measured(out), "BER <= 1e-3, delivered >= 50%",
                       s.ber() <= k_ber_gate_awgn && delivered(out));
                break;
            case low_ber:
                result(id, points[i].condition, measured(out), ">= 1e4 bits, BER <= 1e-4, 0 extra, 0 shifted",
                       near_zero_errors(out) && enough);
                break;
            case low_ber_no_loss:
                result(id, points[i].condition, measured(out),
                       ">= 1e4 bits, BER <= 1e-4, 0 extra, 0 shifted, 0 bytes lost (blanker deadlock)",
                       near_zero_errors(out) && enough && s.lost_bytes == 0);
                break;
            }
        }
    }
}

void test_c10_fm() {
    run_fm("C10", true, k_test_c + 10);
}

void test_c15_fm_emphasis_mismatch() {
    run_fm("C15", false, k_test_c + 15);
}

// C11: AM (m = 0.8, 6 kHz IF), am profile: the am preset and hf_slow.
void test_c11_am() {
    // Gate decision C11: the am preset is gated at CNR 6 dB, hf_slow keeps 2 dB; each also runs at its gate + 3 dB (F6).
    const Preset presets[] = {Preset::am, Preset::hf_slow};
    const double gates_db[] = {k_am_preset_cnr_db, k_am_cnr_db};
    std::vector<CPoint> points;
    for (std::size_t p = 0; p < test::count_of(presets); ++p) {
        const EncoderConfig config = preset(presets[p]);
        for (std::size_t f6 = 0; f6 < k_pair; ++f6) {
            const double cnr = gates_db[p] + (f6 != 0 ? k_f6_margin_db : 0.0);
            const sim::ChannelConfig c = am_channel(cnr);
            points.push_back(make_point("C11",
                                        with_gate(format("AM m=0.8, %s, am profile, CNR %.0f dB in the 6 kHz IF (snr %.1f "
                                                         "dB)",
                                                         name_of(config).c_str(), cnr, c.snr_db),
                                                  f6 != 0),
                                        config, c, DecoderConfig::for_profile(Profile::am),
                                        f6 != 0 ? k_bits_low_ber : k_bits_rf, f6 != 0));
        }
    }
    const std::size_t first_evidence = points.size();
    {
        const EncoderConfig config = preset(Preset::am);
        const std::size_t sweep = sizeof(k_am_sweep_cnr_db) / sizeof(k_am_sweep_cnr_db[0]);
        for (std::size_t n = 0; n < sweep; ++n) {
            const sim::ChannelConfig c = am_channel(k_am_sweep_cnr_db[n]);
            points.push_back(make_point("C11",
                                        format("AM m=0.8, %s, am profile, CNR %.0f dB in the 6 kHz IF (snr %.1f dB)",
                                               name_of(config).c_str(), k_am_sweep_cnr_db[n], c.snr_db),
                                        config, c, DecoderConfig::for_profile(Profile::am), k_bits_low_ber_long, false));
        }
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 11);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const bool pass = o[i][0].score.ber() <= k_ber_gate_awgn && delivered(o[i][0]);
        if (i >= first_evidence) {
            report(points[i], o[i][0],
                   std::string("report (the am preset below its CNR 6 dB gate, gate decision C11; BER <= 1e-3: ") +
                       (pass ? "met)" : "NOT met)"));
            continue;
        }
        if (points[i].f6) {
            report(points[i], o[i][0]);
            continue;
        }
        const bool am_preset = presets[i / k_pair] == Preset::am;
        result("C11", points[i].condition, measured(o[i][0]),
               am_preset ? "BER <= 1e-3, delivered >= 50% (the am preset at CNR 6 dB: gate decision C11)"
                         : "BER <= 1e-3, delivered >= 50%",
               pass);
    }
}

void test_c12_flutter() {
    std::vector<CPoint> points;
    const EncoderConfig hf = preset(Preset::hf);
    points.push_back(make_point("C12", format("flutter 0.5 ms / 10 Hz, %s, %.0f dB", name_of(hf).c_str(), k_high_snr_db),
                                hf, usb(k_high_snr_db, sim::FadingPreset::flutter), ssb(), k_bits_side, false));
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 12);
    const Outcome& f = o[0][0];
    const double released = static_cast<double>(f.score.bytes_released);
    const double extra = released > 0.0 ? static_cast<double>(f.score.extra_bytes) / released : 0.0;
    const double wrong = released > 0.0 ? k_per_mille * static_cast<double>(f.score.wrong_bytes) / released : 0.0;
    result("C12", points[0].condition, measured(f), "report (BER and wrong bytes)", true, Kind::report);
    const double k_extra_gate = 1e-3;
    result("C12", points[0].condition + ", integrity",
           format("extra bytes %zu of %.0f released (%.2f per 1000); wrong bytes %.1f per 1000 released", f.score.extra_bytes,
                  released, k_per_mille * extra, wrong),
           "extra bytes <= 1 per 1000 released", extra <= k_extra_gate);
}

void test_c13_agc_fading() {
    const EncoderConfig hf_slow = preset(Preset::hf_slow);
    std::vector<CPoint> points;
    sim::ChannelConfig c = usb(k_high_snr_db, sim::FadingPreset::ccir_moderate);
    points.push_back(make_point("C13", format("no AGC, CCIR moderate, %s, %.0f dB", name_of(hf_slow).c_str(), k_high_snr_db),
                                hf_slow, c, ssb(), k_bits_gate, false));
    c.agc = true;
    c.agc_attack_ms = k_agc_attack_ms;
    c.agc_decay_ms = k_agc_decay_ms;
    points.push_back(make_point("C13",
                                format("AGC 1/300 ms, CCIR moderate, %s, %.0f dB", name_of(hf_slow).c_str(), k_high_snr_db),
                                hf_slow, c, ssb(), k_bits_gate, false));
    points.back().seed_point = 0;
    c.snr_db = k_high_snr_db + k_f6_margin_db;
    points.push_back(make_point("C13",
                                with_gate(format("AGC 1/300 ms, CCIR moderate, %s, %.0f dB", name_of(hf_slow).c_str(),
                                                 c.snr_db),
                                          true),
                                hf_slow, c, ssb(), k_bits_side, true));
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 13);
    const double k_ber_gate = 2.5e-2;
    const double k_ratio_gate = 2.0;
    const Outcome& none = o[0][0];
    const Outcome& agc = o[1][0];
    const double r = ratio(agc.score.ber(), none.score.ber());
    result("C13", format("AGC 1 ms / 300 ms + CCIR moderate, %s, %.0f dB, same seeds", name_of(hf_slow).c_str(),
                         k_high_snr_db),
           format("AGC: BER %.2e (delivered %.2f%%, %s); none: BER %.2e (delivered %.2f%%); ratio %.2f", agc.score.ber(),
                  k_percent * agc.delivered(), lost_text(agc).c_str(), none.score.ber(), k_percent * none.delivered(), r),
           "BER <= 2.5e-2 and <= 2 x no AGC, delivered >= 50%",
           agc.score.ber() <= k_ber_gate && r <= k_ratio_gate && delivered(agc) && delivered(none));
    report(points[2], o[2][0]);
}

// C14: USB and LSB, the pitch shifted to +-(tolerance - 10 Hz) in the SSB 2.4 kHz filter (spec 1.5), CCIR moderate
// 30 dB. The C2 gate is set for hf_slow and hf; hf_fast (T = 8 ms, for paths <= 0.8 ms, spec 1.4) is reported.
void test_c14_sideband_shift_fading() {
    const Preset presets[] = {Preset::hf_slow, Preset::hf, Preset::hf_fast};
    const std::size_t preset_count = sizeof(presets) / sizeof(presets[0]);
    const sim::Mode sidebands[] = {sim::Mode::usb, sim::Mode::lsb};
    const double directions[] = {0.0, -1.0, 1.0};  // the centred point first
    const std::size_t direction_count = sizeof(directions) / sizeof(directions[0]);
    std::vector<CPoint> points;
    std::vector<double> shifts;
    for (std::size_t p = 0; p < preset_count; ++p) {
        const EncoderConfig config = preset(presets[p]);
        const PassbandFit fit = passband_fit(config);
        const double shift = static_cast<double>(fit.tolerance_hz) - k_shift_margin_hz;
        for (std::size_t b = 0; b < test::count_of(sidebands); ++b) {
            const std::size_t centred = points.size();
            for (std::size_t d = 0; d < direction_count; ++d) {
                sim::ChannelConfig c = usb(k_high_snr_db, sim::FadingPreset::ccir_moderate);
                c.mode = sidebands[b];
                c.freq_offset_hz = directions[d] * shift;
                const bool lsb = sidebands[b] == sim::Mode::lsb;
                const double pitch = (lsb ? c.lsb_pivot_hz - config.tone_hz : config.tone_hz) + c.freq_offset_hz;
                CPoint point = make_point("C14",
                                          format("%s, %s, tolerance %u Hz, shift %+.0f Hz (pitch %.0f Hz), CCIR "
                                                 "moderate, %.0f dB",
                                                 lsb ? "LSB" : "USB", name_of(config).c_str(), fit.tolerance_hz,
                                                 c.freq_offset_hz, pitch, k_high_snr_db),
                                          config, c, ssb(), k_bits_side, false);
                point.seed_point = static_cast<long>(centred);
                points.push_back(point);
                shifts.push_back(c.freq_offset_hz);
            }
        }
    }
    {
        const EncoderConfig config = preset(Preset::hf_slow);
        sim::ChannelConfig c = usb(k_high_snr_db + k_f6_margin_db, sim::FadingPreset::ccir_moderate);
        c.mode = sim::Mode::lsb;
        c.freq_offset_hz = static_cast<double>(passband_fit(config).tolerance_hz) - k_shift_margin_hz;
        points.push_back(make_point("C14",
                                    with_gate(format("LSB, %s, shift %+.0f Hz, CCIR moderate, %.0f dB",
                                                     name_of(config).c_str(), c.freq_offset_hz, c.snr_db),
                                              true),
                                    config, c, ssb(), k_bits_side, true));
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 14);
    const double k_ber_gate = 2.5e-2;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const Outcome& out = o[i][0];
        const std::size_t p = i / (test::count_of(sidebands) * direction_count);
        const bool gated = presets[p] != Preset::hf_fast && shifts[i] != 0.0;
        const bool pass = out.score.ber() <= k_ber_gate && delivered(out);
        if (gated) {
            result("C14", points[i].condition, measured(out), "BER <= 2.5e-2 (C2 gate), delivered >= 50%", pass);
        } else {
            report(points[i], out,
                   std::string(shifts[i] == 0.0 ? "report (centred reference" : "report (C2 gate set for hf_slow and hf") +
                       "; BER <= 2.5e-2: " + (pass ? "met)" : "NOT met)"));
        }
    }
    report(points.back(), o.back()[0]);
}

}  // namespace regression
}  // namespace unlimited
