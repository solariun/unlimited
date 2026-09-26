#include "regression.hpp"

#include "test_harness.hpp"

#include <cmath>
#include <random>

// C1-C12 (spec 8.4) and C13-C15, v0.2. Every transmission carries packets, so each point also feeds F5; points
// at >= gate + 3 dB feed F6.
namespace unlimited {
namespace regression {

namespace {

const std::uint32_t k_test_c = 100;

const double k_bits_gate = 2e5;       // BER-gated usb points
const double k_bits_gate_rf = 1e5;    // BER-gated am/fm points (4-6x slower channel)
const double k_bits_side = 5e4;       // loss, F6 and report points
const double k_bits_zero = 1.2e4;     // "0 errors in 1e4 bits" points
const double k_bits_shift = 1e5;      // C14 tuning-tolerance points
const std::size_t k_bytes = 300;      // bytes per transmission
const std::size_t k_alias_bytes = 100;  // C3' alias count: 20 hf frames per transmission
const std::size_t k_trial_frames = 2;   // acquisition and header trials: frames 0 and 1
const std::size_t k_header_trials = 120;
const double k_preroll_ms = 4000.0;   // interferer on air before the transmission (steady mask needs 2.56 s)
const double k_no_preroll_ms = 300.0;
const double k_fading_peak = 3.0;     // Rayleigh amplitude above 3x RMS: probability 1.2e-4
const double k_fm_peak = 2.0;         // FM clicks
const double k_f6_margin_db = 3.0;
const int k_byte_bits = 8;

const double k_hf_gate_db = -4.5;      // A1' gate of hf (C7)
const double k_t8_gate_db = 1.5;       // A1' gate of fm (T8 k3) on SSB (C6')
const double k_below_gate_db = 1.0;
const double k_impulse_rate_hz = 20.0;
const double k_impulse_db = 40.0;
const double k_agc_attack_ms = 1.0;
const double k_agc_decay_ms = 300.0;
const double k_ratio_blanker = 1.1;
const double k_ratio_agc = 2.0;
const double k_agc_cost_db = 1.0;      // C7 report: the SNR the AGC costs at the A1' point
const double k_db_per_decade = 20.0;   // amplitude
const std::size_t k_own_seeds = static_cast<std::size_t>(-1);
const double k_percent = 100.0;
const double k_per_mille = 1000.0;
const double k_good_snr_db = 25.0;     // C1 BER point
const double k_fading_snr_db = 20.0;   // C2', C3', C13, C14
const double k_high_snr_db = 30.0;     // C3' alias point, C4, C12
const double k_qsb_snr_db = 15.0;      // C5
const double k_qrn_snr_db = 6.0;       // C6'
const double k_interferer_snr_db = 10.0;  // C8', C9'
const double k_infinite_ratio = 1e9;

// C8'/C9': interferers inside the grid.
const double k_carrier_tone = 3.0;       // data grid tone of the steady carrier
const double k_carrier_db = 0.0;         // relative to key-down
const double k_strong_carrier_db = 6.0;
const double k_header_carrier_db = 10.0;
const double k_header_tone = 4.0;        // on-tone case of the header test
const double k_header_band_low = 0.0;    // header tones 0..7: the random carrier falls anywhere between
const double k_header_band_high = 7.0;
const double k_cw_tone = 10.0;
const double k_cw_db = -6.0;
const double k_cw_wpm = 20.0;

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
    bool f6;           // at >= gate + 3 dB
    bool independent;  // one transmission per job (acquisition and header trials)
    std::size_t seed_point;  // points with the same seed_point get the same data, noise and fading
    double carrier_low_hz;   // > 0: the carrier frequency is drawn per job from [low, high]
    double carrier_high_hz;
};

CPoint make_point(const std::string& id, const std::string& condition, const EncoderConfig& config,
                  const sim::ChannelConfig& channel, Profile profile, double bits, bool f6) {
    CPoint p;
    p.id = id;
    p.condition = condition;
    p.config = config;
    p.channel = channel;
    p.peak_factor = channel.fading ? k_fading_peak : (channel.mode == sim::Mode::fm ? k_fm_peak : 1.0);
    p.decoders.push_back(DecoderConfig::for_profile(profile));
    p.bits = bits;
    p.bytes = k_bytes;
    p.lead_ms = JobPlan().lead_ms;
    p.genie = false;
    p.f6 = f6;
    p.independent = false;
    p.seed_point = k_own_seeds;
    p.carrier_low_hz = 0.0;
    p.carrier_high_hz = 0.0;
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

sim::ChannelConfig am_channel(double snr_db) {
    sim::ChannelConfig c;
    c.mode = sim::Mode::am;
    c.snr_db = snr_db;
    return c;
}

EncoderConfig preset(Preset p) {
    return EncoderConfig::from_preset(p, k_decoder_rate_hz);
}

std::string with_mode(const std::string& condition, const EncoderConfig& config) {
    return condition + " [" + mode_name(config) + "]";
}

// Builds and runs the jobs of every point; result [point][decoder].
std::vector<std::vector<Outcome> > run_cpoints(const std::vector<CPoint>& points, std::uint32_t test) {
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const CPoint& p = points[i];
        if (!p.config.valid()) test::fail(__FILE__, __LINE__, "invalid mode: " + p.condition);
        const std::size_t count = transmissions_for(p.bits, p.bytes);
        const std::size_t per_job =
            p.independent ? 1 : transmissions_per_job(transmission_seconds(p.config, p.bytes, JobPlan().gap_ms), count);
        std::uint32_t job_index = 0;
        for (std::size_t first = 0; first < count; first += per_job, ++job_index) {
            const std::size_t seed_point = p.seed_point == k_own_seeds ? i : p.seed_point;
            const std::uint32_t seed = seed_of(test, static_cast<std::uint32_t>(seed_point), job_index);
            JobPlan job;
            for (std::size_t t = first; t < std::min(count, first + per_job); ++t) {
                job.transmissions.push_back(packet_tx(p.config, p.bytes, data_seed(seed, t)));
            }
            job.lead_ms = p.lead_ms;
            job.channel = p.channel;
            job.channel.seed = seed;
            if (p.carrier_high_hz > p.carrier_low_hz) {
                std::mt19937 generator(seed);
                job.channel.carrier_hz =
                    std::uniform_real_distribution<double>(p.carrier_low_hz, p.carrier_high_hz)(generator);
            }
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
                                upper_95(o.score.bit_errors, o.score.matched * static_cast<double>(k_byte_bits)),
                                o.max_wrong_run);
}

double correct_fraction(const Outcome& o) {
    const Score& s = o.score;
    return s.bytes_sent == 0 ? 0.0 : static_cast<double>(s.matched - s.wrong_bytes) / static_cast<double>(s.bytes_sent);
}

double amplitude_of_db(double db) {
    return std::pow(10.0, db / k_db_per_decade);
}

// a / b, where anything over 0 counts as infinitely worse and 0 / 0 as equal.
double ratio(double a, double b) {
    if (b > 0.0) return a / b;
    return a > 0.0 ? k_infinite_ratio : 1.0;
}

std::string report_line(const CPoint& p) {
    return p.f6 ? "report (F6 point)" : "report";
}

void report(const CPoint& p, const Outcome& o) {
    result(p.id, p.condition, measured(o), report_line(p), true, Kind::report);
}

}  // namespace

void test_c1_ccir_good() {
    const EncoderConfig hf = preset(Preset::hf);
    std::vector<CPoint> points;
    points.push_back(make_point("C1", "CCIR good, hf, 25 dB", hf, usb(k_good_snr_db, sim::FadingPreset::ccir_good),
                                Profile::ssb, k_bits_gate, false));
    points.push_back(make_point("C1", "CCIR good, hf, 28 dB (gate + 3)", hf,
                                usb(k_good_snr_db + k_f6_margin_db, sim::FadingPreset::ccir_good), Profile::ssb,
                                k_bits_side, true));
    const double loss_snrs[] = {10.0, 15.0, 20.0};  // spec: loss <= 2 % at >= 10 dB
    for (std::size_t i = 0; i < sizeof(loss_snrs) / sizeof(loss_snrs[0]); ++i) {
        points.push_back(make_point("C1", format("CCIR good, hf, %.0f dB", loss_snrs[i]), hf,
                                    usb(loss_snrs[i], sim::FadingPreset::ccir_good), Profile::ssb, k_bits_side, false));
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 1);
    const double k_ber_gate = 1e-3;
    const double k_loss_gate = 0.02;
    result("C1", points[0].condition, measured(o[0][0]), "BER <= 1e-3, loss <= 2%",
           o[0][0].score.ber() <= k_ber_gate && o[0][0].score.loss() <= k_loss_gate);
    for (std::size_t i = 1; i < points.size(); ++i) {
        result("C1", points[i].condition, measured(o[i][0]), "loss <= 2%", o[i][0].score.loss() <= k_loss_gate);
    }
}

void test_c2_ccir_moderate() {
    const sim::FadingPreset moderate = sim::FadingPreset::ccir_moderate;
    std::vector<CPoint> points;
    CPoint hf = make_point("C2'", "CCIR moderate, hf, 20 dB", preset(Preset::hf), usb(k_fading_snr_db, moderate),
                           Profile::ssb, k_bits_gate, false);
    hf.genie = true;
    points.push_back(hf);
    points.push_back(make_point("C2'", "CCIR moderate, hf_robust, 20 dB", preset(Preset::hf_robust),
                                usb(k_fading_snr_db, moderate), Profile::ssb, k_bits_gate, false));
    points.push_back(make_point("C2'", "CCIR moderate, hf, 23 dB (gate + 3)", preset(Preset::hf),
                                usb(k_fading_snr_db + k_f6_margin_db, moderate), Profile::ssb, k_bits_side, true));
    const double hf_snrs[] = {10.0, k_high_snr_db};
    for (std::size_t i = 0; i < 2; ++i) {
        points.push_back(make_point("C2'", format("CCIR moderate, hf, %.0f dB", hf_snrs[i]), preset(Preset::hf),
                                    usb(hf_snrs[i], moderate), Profile::ssb, k_bits_side, false));
    }
    points.push_back(make_point("C2'", "CCIR moderate, hf_fast, 20 dB", preset(Preset::hf_fast),
                                usb(k_fading_snr_db, moderate), Profile::ssb, k_bits_side, false));
    points.push_back(make_point("C2'", "CCIR moderate, hf_weak, 20 dB", preset(Preset::hf_weak),
                                usb(k_fading_snr_db, moderate), Profile::ssb, k_bits_side, false));
    const EncoderConfig n16 = mode_config(Preset::hf, 32, 5, 16, Spacing::standard, GridSide::below);
    const EncoderConfig n32 = mode_config(Preset::hf_fast, 16, 4, 32, Spacing::standard, GridSide::below);
    points.push_back(make_point("C2'", with_mode("CCIR moderate, hf N16, 20 dB", n16), n16,
                                usb(k_fading_snr_db, moderate), Profile::ssb, k_bits_side, false));
    points.push_back(make_point("C2'", with_mode("CCIR moderate, hf_fast N32, 20 dB", n32), n32,
                                usb(k_fading_snr_db, moderate), Profile::ssb, k_bits_side, false));
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 2);
    const double k_ber_gate = 1e-3;
    const double k_frames_gate = 0.97;
    const double k_robust_ber_gate = 5e-4;
    const Outcome& main = o[0][0];
    result("C2'", points[0].condition, measured(main), "BER <= 1e-3, frames >= 97% (prov.)",
           main.score.ber() <= k_ber_gate && main.frames_ratio() >= k_frames_gate);
    result("C2'", points[0].condition + ", integrated vs genie (same audio)",
           format("integrated BER %.2e (frames %.1f%%), genie BER %.2e (%zu bits), ratio %.2f", main.score.ber(),
                  k_percent * main.frames_ratio(), main.genie_ber(), main.genie_bits,
                  ratio(main.score.ber(), main.genie_ber())),
           "report (genie spec 4.2: 2.4e-4)", true, Kind::report);
    result("C2'", points[1].condition, measured(o[1][0]), "BER <= 5e-4, frames >= 50% (prov.)",
           o[1][0].score.ber() <= k_robust_ber_gate && delivered(o[1][0]));
    for (std::size_t i = 2; i < points.size(); ++i) report(points[i], o[i][0]);
}

void test_c3_ccir_poor() {
    const sim::FadingPreset poor = sim::FadingPreset::ccir_poor;
    std::vector<CPoint> points;
    points.push_back(make_point("C3'", "CCIR poor, hf, 20 dB", preset(Preset::hf), usb(k_fading_snr_db, poor),
                                Profile::ssb, k_bits_gate, false));
    CPoint alias = make_point("C3'", "CCIR poor, hf, 30 dB, 100-byte transmissions", preset(Preset::hf),
                              usb(k_high_snr_db, poor), Profile::ssb, k_bits_gate, false);
    alias.bytes = k_alias_bytes;
    points.push_back(alias);
    points.push_back(make_point("C3'", "CCIR poor, hf, 23 dB (gate + 3)", preset(Preset::hf),
                                usb(k_fading_snr_db + k_f6_margin_db, poor), Profile::ssb, k_bits_side, true));
    const Preset others[] = {Preset::hf_fast, Preset::hf_robust, Preset::hf_weak};
    const char* names[] = {"hf_fast", "hf_robust", "hf_weak"};
    for (std::size_t i = 0; i < 3; ++i) {
        points.push_back(make_point("C3'", format("CCIR poor, %s, 20 dB", names[i]), preset(others[i]),
                                    usb(k_fading_snr_db, poor), Profile::ssb, k_bits_side, false));
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 3);
    const double k_ber_gate = 2e-3;
    const double k_frames_gate = 0.93;
    const double k_alias_per_tx = 1.0 / 50.0;
    const Outcome& main = o[0][0];
    result("C3'", points[0].condition, measured(main), "BER <= 2e-3, frames >= 93% (prov.)",
           main.score.ber() <= k_ber_gate && main.frames_ratio() >= k_frames_gate);
    const Outcome& a = o[1][0];
    const std::size_t aliases = a.lost_reasons[static_cast<std::size_t>(LostReason::alias)];
    result("C3'", points[1].condition,
           format("%zu alias LOSTs in %zu transmissions (%.2f per 50); ", aliases, a.transmissions,
                  50.0 * static_cast<double>(aliases) / static_cast<double>(a.transmissions)) +
               measured(a),
           "alias LOSTs <= 1 per 50 tx (prov.)",
           static_cast<double>(aliases) <= k_alias_per_tx * static_cast<double>(a.transmissions));
    for (std::size_t i = 2; i < points.size(); ++i) report(points[i], o[i][0]);
}

void test_c4_flat_rayleigh() {
    std::vector<CPoint> points;
    const EncoderConfig hf = preset(Preset::hf);
    points.push_back(make_point("C4", "flat Rayleigh 1 Hz, hf, 30 dB", hf,
                                usb(k_high_snr_db, sim::FadingPreset::flat), Profile::ssb, k_bits_gate, false));
    points.push_back(make_point("C4", "flat Rayleigh 1 Hz, hf, 33 dB (gate + 3)", hf,
                                usb(k_high_snr_db + k_f6_margin_db, sim::FadingPreset::flat), Profile::ssb,
                                k_bits_side, true));
    points.push_back(make_point("C4", "flat Rayleigh 1 Hz, hf, 20 dB", hf,
                                usb(k_fading_snr_db, sim::FadingPreset::flat), Profile::ssb, k_bits_side, false));
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 4);
    const double k_ber_gate = 1.5e-2;
    result("C4", points[0].condition, measured(o[0][0]), "BER <= 1.5e-2, frames >= 50% (v0.1 gate; genie 5e-5)",
           o[0][0].score.ber() <= k_ber_gate && delivered(o[0][0]));
    for (std::size_t i = 1; i < points.size(); ++i) report(points[i], o[i][0]);
}

void test_c5_qsb() {
    const double k_depth_db = 20.0;
    const double k_rate_hz = 0.2;
    std::vector<CPoint> points;
    const double snrs[] = {k_qsb_snr_db, k_qsb_snr_db + k_f6_margin_db};
    for (std::size_t i = 0; i < 2; ++i) {
        sim::ChannelConfig c = usb(snrs[i]);
        c.qsb_depth_db = k_depth_db;
        c.qsb_rate_hz = k_rate_hz;
        points.push_back(make_point("C5", format("QSB 20 dB at 0.2 Hz, hf, %.0f dB at the crest%s", snrs[i],
                                                 i == 1 ? " (gate + 3)" : ""),
                                    preset(Preset::hf), c, Profile::ssb, i == 0 ? k_bits_gate : k_bits_side, i == 1));
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 5);
    const double k_correct_gate = 0.95;
    for (std::size_t i = 0; i < 2; ++i) {
        const double correct = correct_fraction(o[i][0]);
        result("C5", points[i].condition, measured(o[i][0]) + format(", bytes correct %.2f%%", k_percent * correct),
               ">= 95% of bytes correct", correct >= k_correct_gate, i == 0 ? Kind::gate : Kind::report);
    }
}

// C6': T8 k3 on SSB with the HF placement (f_ref 2313 Hz, band centred as on the genie bench), am profile; the fm
// preset itself (f_ref 2650 Hz, at the SSB passband edge) is reported alongside.
void test_c6_qrn() {
    const EncoderConfig t8 = mode_config(Preset::fm, 8, 3, 8, Spacing::standard, GridSide::below);
    DecoderConfig off = DecoderConfig::for_profile(Profile::am);
    off.impulse_blanker = false;
    std::vector<CPoint> points;
    const double snrs[] = {k_qrn_snr_db, k_qrn_snr_db + k_f6_margin_db};
    for (std::size_t i = 0; i < 2; ++i) {
        sim::ChannelConfig c = usb(snrs[i]);
        c.impulse_rate_hz = k_impulse_rate_hz;
        c.impulse_level_db = k_impulse_db;
        CPoint p = make_point("C6'", with_mode(format("QRN 20/s at +40 dB, T8 k3 centred, am profile, %+.0f dB%s",
                                                      snrs[i], i == 1 ? " (gate + 3)" : ""),
                                               t8),
                              t8, c, Profile::am, i == 0 ? k_bits_gate : k_bits_side, i == 1);
        p.decoders.push_back(off);
        points.push_back(p);
    }
    {
        sim::ChannelConfig c = usb(k_qrn_snr_db);
        c.impulse_rate_hz = k_impulse_rate_hz;
        c.impulse_level_db = k_impulse_db;
        const EncoderConfig fm = preset(Preset::fm);
        points.push_back(make_point("C6'",
                                    with_mode("QRN 20/s at +40 dB, fm preset (G1: FM-only), am profile, +6 dB", fm),
                                    fm, c, Profile::am, k_bits_side, false));
    }
    {
        sim::ChannelConfig c = usb(k_hf_gate_db + k_f6_margin_db);
        c.impulse_rate_hz = k_impulse_rate_hz;
        c.impulse_level_db = k_impulse_db;
        points.push_back(make_point("C6'", "QRN 20/s at +40 dB, hf, -1.5 dB (gate + 3)", preset(Preset::hf), c,
                                    Profile::ssb, k_bits_side, true));
    }
    // Clean AWGN: the same audio through the decoder with and without the blanker.
    const double awgn[] = {k_t8_gate_db, k_t8_gate_db - k_below_gate_db};
    const double awgn_bits[] = {2.0 * k_bits_gate, k_bits_gate};
    for (std::size_t i = 0; i < 2; ++i) {
        CPoint p = make_point("C6'", format("clean AWGN, T8 k3 centred, am profile, %+.1f dB, blanker on vs off",
                                            awgn[i]),
                              t8, usb(awgn[i]), Profile::am, awgn_bits[i], false);
        p.decoders.push_back(off);
        points.push_back(p);
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 6);
    const double k_ber_gate = 1e-3;
    result("C6'", points[0].condition, measured(o[0][0]), "BER <= 1e-3 with the slot blanker, frames >= 50%",
           o[0][0].score.ber() <= k_ber_gate && delivered(o[0][0]));
    result("C6'", points[0].condition + ", blanker off", measured(o[0][1]), "report", true, Kind::report);
    for (std::size_t i = 1; i < 4; ++i) report(points[i], o[i][0]);
    for (std::size_t i = 0; i < 2; ++i) {
        const Outcome& on = o[4 + i][0];
        const Outcome& none = o[4 + i][1];
        const double r = ratio(on.score.ber(), none.score.ber());
        result("C6'", points[4 + i].condition,
               format("on: %zu bit errors (BER %.2e, frames %.2f%%), off: %zu (BER %.2e, frames %.2f%%), ratio %.2f",
                      on.score.bit_errors, on.score.ber(), k_percent * on.frames_ratio(), none.score.bit_errors,
                      none.score.ber(), k_percent * none.frames_ratio(), r),
               i == 0 ? "BER on/off <= 1.1, frames >= 50%" : "report (statistics)",
               r <= k_ratio_blanker && delivered(on) && delivered(none),
               i == 0 ? Kind::gate : Kind::report);
    }
}

void test_c7_agc() {
    const double snrs[] = {k_hf_gate_db, k_hf_gate_db - k_below_gate_db};
    const double bits[] = {2.0 * k_bits_gate, k_bits_gate};
    const EncoderConfig hf = preset(Preset::hf);
    std::vector<CPoint> points;
    for (std::size_t i = 0; i < 2; ++i) {
        sim::ChannelConfig c = usb(snrs[i]);
        points.push_back(
            make_point("C7", format("no AGC, hf, %+.1f dB", snrs[i]), hf, c, Profile::ssb, bits[i], false));
        c.agc = true;
        c.agc_attack_ms = k_agc_attack_ms;
        c.agc_decay_ms = k_agc_decay_ms;
        points.push_back(
            make_point("C7", format("AGC 1/300 ms, hf, %+.1f dB", snrs[i]), hf, c, Profile::ssb, bits[i], false));
        points.back().seed_point = points.size() - 2;
    }
    {
        sim::ChannelConfig c = usb(k_hf_gate_db + k_agc_cost_db);
        c.agc = true;
        c.agc_attack_ms = k_agc_attack_ms;
        c.agc_decay_ms = k_agc_decay_ms;
        points.push_back(make_point("C7", format("AGC 1/300 ms, hf, %+.1f dB", k_hf_gate_db + k_agc_cost_db), hf, c,
                                    Profile::ssb, bits[0], false));
        points.back().seed_point = 0;
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 7);
    for (std::size_t i = 0; i < 2; ++i) {
        const Outcome& none = o[2 * i][0];
        const Outcome& agc = o[2 * i + 1][0];
        const double r = ratio(agc.score.ber(), none.score.ber());
        result("C7",
               format("AGC 1 ms / 300 ms vs none, hf, %+.1f dB (%s), same seeds", snrs[i],
                      i == 0 ? "A1' point" : "gate - 1"),
               format("AGC: %zu bit errors (BER %.2e, frames %.2f%%), none: %zu (BER %.2e, frames %.2f%%), ratio %.2f",
                      agc.score.bit_errors, agc.score.ber(), k_percent * agc.frames_ratio(), none.score.bit_errors,
                      none.score.ber(), k_percent * none.frames_ratio(), r),
               i == 0 ? "BER AGC <= 2 x no AGC, frames >= 50%" : "report (statistics)",
               r <= k_ratio_agc && delivered(agc) && delivered(none),
               i == 0 ? Kind::gate : Kind::report);
    }
    const Outcome& none = o[0][0];
    const Outcome& shifted = o[4][0];
    result("C7",
           format("AGC 1 ms / 300 ms at %+.1f dB (A1' point + %.0f dB) vs none at %+.1f dB, same seeds",
                  k_hf_gate_db + k_agc_cost_db, k_agc_cost_db, k_hf_gate_db),
           format("AGC: %zu bit errors (BER %.2e, frames %.2f%%), none: %zu (BER %.2e), ratio %.2f",
                  shifted.score.bit_errors, shifted.score.ber(), k_percent * shifted.frames_ratio(),
                  none.score.bit_errors, none.score.ber(), ratio(shifted.score.ber(), none.score.ber())),
           "report (AGC cost <= 1 dB when ratio <= 1)", true, Kind::report);
}

// C8': a steady carrier on data grid tone 3 (BER), then a +10 dB carrier in the header band (header detection:
// the decoder reaches TRACK with the sent mode, seen in its `slot` events).
void test_c8_carrier() {
    const EncoderConfig hf = preset(Preset::hf);
    const std::size_t trial_bytes = k_trial_frames * hf.frame_bytes();
    std::vector<CPoint> points;
    const double levels[] = {k_carrier_db, k_carrier_db, k_strong_carrier_db};
    const double snrs[] = {k_interferer_snr_db, k_interferer_snr_db + k_f6_margin_db, k_interferer_snr_db};
    for (std::size_t i = 0; i < 3; ++i) {
        sim::ChannelConfig c = usb(snrs[i]);
        c.carrier_hz = data_tone_hz(hf, k_carrier_tone);
        c.carrier_db = levels[i];
        CPoint p = make_point("C8'",
                              format("steady carrier %+.0f dB on grid tone 3 (%.0f Hz), hf, %.0f dB%s", levels[i],
                                     c.carrier_hz, snrs[i], i == 1 ? " (gate + 3)" : ""),
                              hf, c, Profile::ssb, i == 0 ? k_bits_gate : k_bits_side, i == 1);
        p.lead_ms = k_preroll_ms;
        p.peak_factor = 1.0 + amplitude_of_db(levels[i]);
        points.push_back(p);
    }
    {
        sim::ChannelConfig c = usb(k_interferer_snr_db);
        c.carrier_db = k_header_carrier_db;
        const double low = header_tone_hz(hf, k_header_band_high);
        const double high = header_tone_hz(hf, k_header_band_low);
        CPoint p = make_point("C8'",
                              format("carrier +10 dB at a random frequency in the header band (%.0f..%.0f Hz), hf, "
                                     "%.0f dB, %zu independent trials",
                                     low, high, k_interferer_snr_db, k_header_trials),
                              hf, c, Profile::ssb, static_cast<double>(k_header_trials * trial_bytes * k_byte_bits),
                              false);
        p.bytes = trial_bytes;
        p.independent = true;
        p.lead_ms = k_preroll_ms;
        p.peak_factor = 1.0 + amplitude_of_db(k_header_carrier_db);
        p.carrier_low_hz = low;
        p.carrier_high_hz = high;
        points.push_back(p);
        CPoint on = p;
        on.condition = format("carrier +10 dB on header tone 4 (%.0f Hz), hf, %.0f dB, %zu independent trials",
                              header_tone_hz(hf, k_header_tone), k_interferer_snr_db, k_header_trials);
        on.channel.carrier_hz = header_tone_hz(hf, k_header_tone);
        on.carrier_low_hz = 0.0;
        on.carrier_high_hz = 0.0;
        points.push_back(on);
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 8);
    const double k_ber_gate = 2e-2;
    const double k_header_gate = 0.95;
    result("C8'", points[0].condition, measured(o[0][0]), "BER <= 2e-2, frames >= 50%",
           o[0][0].score.ber() <= k_ber_gate && delivered(o[0][0]));
    report(points[1], o[1][0]);
    result("C8'", points[2].condition, measured(o[2][0]), "report (judge, genie + background: 4.7e-2)", true,
           Kind::report);
    for (std::size_t i = 3; i < 5; ++i) {
        const Outcome& h = o[i][0];
        const double rate = static_cast<double>(h.header_transmissions) / static_cast<double>(h.transmissions);
        result("C8'", points[i].condition,
               format("header detected %.1f%% (%zu/%zu, %zu with another mode), locked %zu, frames %.1f%%, ",
                      k_percent * rate, h.header_transmissions, h.transmissions, h.wrong_header_transmissions,
                      h.locked_transmissions, k_percent * h.frames_ratio()) +
                   lost_text(h),
               i == 3 ? "header detection >= 95% (random in-band placement)"
                      : "report (Monte Carlo spec 4.5, carrier 11 dB on a header tone: 65%)",
               rate >= k_header_gate, i == 3 ? Kind::gate : Kind::report);
    }
}

void test_c9_cw() {
    const EncoderConfig hf = preset(Preset::hf);
    const double snrs[] = {k_interferer_snr_db, k_interferer_snr_db + k_f6_margin_db, 2.0 * k_interferer_snr_db};
    std::vector<CPoint> points;
    for (std::size_t i = 0; i < 3; ++i) {
        sim::ChannelConfig c = usb(snrs[i]);
        c.cw_hz = data_tone_hz(hf, k_cw_tone);
        c.cw_db = k_cw_db;
        c.cw_wpm = k_cw_wpm;
        CPoint p = make_point("C9'",
                              format("keyed CW 20 WPM at -6 dB on grid tone 10 (%.0f Hz), hf, %.0f dB%s", c.cw_hz,
                                     snrs[i], i == 1 ? " (gate + 3)" : ""),
                              hf, c, Profile::ssb, k_bits_side, i == 1);
        p.lead_ms = k_preroll_ms;
        p.peak_factor = 1.0 + amplitude_of_db(k_cw_db);
        points.push_back(p);
    }
    {
        sim::ChannelConfig c = usb(k_interferer_snr_db);
        c.cw_hz = data_tone_hz(hf, k_cw_tone);
        c.cw_db = k_cw_db;
        c.cw_wpm = k_cw_wpm;
        const std::size_t trial_bytes = k_trial_frames * hf.frame_bytes();
        CPoint p = make_point("C9'", "keyed CW at -6 dB on grid tone 10 starting with the transmission (no pre-roll), "
                                     "hf, 10 dB",
                              hf, c, Profile::ssb, static_cast<double>(k_header_trials * trial_bytes * k_byte_bits),
                              false);
        p.bytes = trial_bytes;
        p.independent = true;
        p.lead_ms = k_no_preroll_ms;
        p.peak_factor = 1.0 + amplitude_of_db(k_cw_db);
        points.push_back(p);
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 9);
    for (std::size_t i = 0; i < 3; ++i) {
        result("C9'", points[i].condition, measured(o[i][0]), points[i].f6 ? "report (F6 point; needs FEC)"
                                                                           : "report only (needs FEC)",
               true, Kind::report);
    }
    const Outcome& cold = o[3][0];
    result("C9'", points[3].condition,
           format("locked %zu/%zu, header %zu, frames %.1f%%, wrong %zu", cold.locked_transmissions, cold.transmissions,
                  cold.header_transmissions, k_percent * cold.frames_ratio(), cold.score.wrong_bytes),
           "report", true, Kind::report);
}

void test_c10_fm() {
    enum Gate { ber_gate, zero_errors, zero_lost, report_only };
    struct Row {
        Preset preset;
        const char* name;
        double cnr_db;
        bool preemphasis;
        double bits;
        Gate gate;
        bool f6;
    };
    const Row rows[] = {
        {Preset::hf_fast, "hf_fast", 6.0, true, k_bits_gate_rf, ber_gate, false},
        {Preset::hf_fast, "hf_fast", 8.0, true, k_bits_zero, zero_errors, false},
        {Preset::hf_fast, "hf_fast", 10.0, true, k_bits_zero, zero_errors, true},
        {Preset::hf_fast, "hf_fast", 14.0, true, k_bits_side, zero_lost, true},
        {Preset::hf_fast, "hf_fast", 6.0, false, k_bits_gate_rf, ber_gate, false},
        {Preset::hf_fast, "hf_fast", 8.0, false, k_bits_zero, zero_errors, false},
        {Preset::hf_fast, "hf_fast", 10.0, false, k_bits_zero, zero_errors, true},
        {Preset::hf_fast, "hf_fast", 14.0, false, k_bits_side, zero_lost, true},
        {Preset::fm, "fm", 7.5, true, k_bits_side, report_only, true},
        {Preset::fm, "fm", 14.0, true, k_bits_side, zero_lost, true},
        {Preset::fm_fast, "fm_fast", 10.0, true, k_bits_side, report_only, true},
        {Preset::fm_fast, "fm_fast", 14.0, true, k_bits_side, zero_lost, true},
    };
    const std::size_t count = sizeof(rows) / sizeof(rows[0]);
    std::vector<CPoint> points;
    for (std::size_t i = 0; i < count; ++i) {
        const Row& r = rows[i];
        const sim::ChannelConfig c = fm_channel(r.cnr_db, r.preemphasis);
        const EncoderConfig config = preset(r.preset);
        const std::string condition =
            format("FM %s TX + de-emphasis RX, %s (f_ref %u), fm profile, CNR %.1f dB (snr %.1f dB)%s",
                   r.preemphasis ? "pre-emphasis" : "flat", r.name, config.tone_hz, r.cnr_db, c.snr_db,
                   r.f6 ? " (>= gate + 3)" : "");
        points.push_back(make_point("C10", condition, config, c, Profile::fm, r.bits, r.f6));
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 10);
    const double k_ber_gate = 1e-3;
    const double k_zero_bits = 1e4;
    for (std::size_t i = 0; i < count; ++i) {
        const Score& s = o[i][0].score;
        switch (rows[i].gate) {
        case ber_gate:
            result("C10", points[i].condition, measured(o[i][0]), "BER <= 1e-3, frames >= 50%",
                   s.ber() <= k_ber_gate && delivered(o[i][0]));
            break;
        case zero_errors:
            result("C10", points[i].condition, measured(o[i][0]), ">= 1e4 bits, 0 bit errors",
                   s.bit_errors == 0 && static_cast<double>(s.matched * k_byte_bits) >= k_zero_bits);
            break;
        case zero_lost:
            result("C10", points[i].condition, measured(o[i][0]), "0 bytes lost (blanker deadlock)", s.lost_bytes == 0);
            break;
        case report_only:
            report(points[i], o[i][0]);
            break;
        }
    }
}

void test_c11_am() {
    const sim::ChannelConfig reference = am_channel(0.0);
    const double k_cnr_db = 2.0;
    std::vector<CPoint> points;
    const double cnrs[] = {k_cnr_db, k_cnr_db + k_f6_margin_db};
    for (std::size_t i = 0; i < 2; ++i) {
        const double snr = snr_for_am_cnr(cnrs[i], reference);
        const std::string condition = format("AM m=0.8, hf, CNR %.0f dB in the 6 kHz IF (snr %.1f dB in 2500 Hz)%s",
                                             cnrs[i], snr, i == 1 ? " (gate + 3)" : "");
        points.push_back(make_point("C11", condition, preset(Preset::hf), am_channel(snr), Profile::am,
                                    i == 0 ? k_bits_gate_rf : k_bits_side, i == 1));
    }
    points.push_back(make_point("C11", "AM m=0.8, hf, snr 2.0 dB in 2500 Hz (CNR -1.8 dB in 6 kHz)", preset(Preset::hf),
                                am_channel(k_cnr_db), Profile::am, k_bits_side, false));
    const Preset others[] = {Preset::fm, Preset::hf_fast};
    const char* names[] = {"fm preset (G1: FM-only)", "hf_fast"};
    for (std::size_t i = 0; i < 2; ++i) {
        const double snr = snr_for_am_cnr(k_cnr_db, reference);
        points.push_back(make_point("C11", format("AM m=0.8, %s, CNR %.0f dB (snr %.1f dB)", names[i], k_cnr_db, snr),
                                    preset(others[i]), am_channel(snr), Profile::am, k_bits_side, false));
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 11);
    const double k_ber_gate = 1e-3;
    result("C11", points[0].condition, measured(o[0][0]), "BER <= 1e-3, frames >= 50%",
           o[0][0].score.ber() <= k_ber_gate && delivered(o[0][0]));
    for (std::size_t i = 1; i < points.size(); ++i) report(points[i], o[i][0]);
}

void test_c12_flutter() {
    std::vector<CPoint> points;
    points.push_back(make_point("C12", "flutter 0.5 ms / 10 Hz, hf_fast, 30 dB", preset(Preset::hf_fast),
                                usb(k_high_snr_db, sim::FadingPreset::flutter), Profile::ssb, k_bits_gate, false));
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 12);
    const Outcome& f = o[0][0];
    const double released = static_cast<double>(f.score.bytes_released);
    const double unmapped = released > 0.0 ? f.score.extra_bytes / released : 0.0;
    const double wrong = released > 0.0 ? k_per_mille * f.score.wrong_bytes / released : 0.0;
    // C12 decision: BER and wrong bytes are channel errors under flutter (report only); the one criterion is lock
    // integrity, unmapped (extra) bytes <= 1 per 1000 released.
    result("C12", points[0].condition, measured(f), "report only (channel errors under flutter)", true, Kind::report);
    const double k_integrity_gate = 1e-3;
    result("C12", points[0].condition + ", lock integrity",
           format("unmapped (extra) bytes %zu of %.0f released (%.2f per 1000); wrong bytes %.1f per 1000 released "
                  "(channel errors, not gated); longest wrong run %zu",
                  f.score.extra_bytes, released, k_per_mille * unmapped, wrong, f.max_wrong_run),
           "unmapped/extra bytes <= 1 per 1000 released", unmapped <= k_integrity_gate);
}

void test_c13_agc_fading() {
    std::vector<CPoint> points;
    const EncoderConfig hf = preset(Preset::hf);
    sim::ChannelConfig c = usb(k_fading_snr_db, sim::FadingPreset::ccir_moderate);
    points.push_back(make_point("C13", "no AGC, CCIR moderate, hf, 20 dB", hf, c, Profile::ssb, k_bits_gate, false));
    c.agc = true;
    c.agc_attack_ms = k_agc_attack_ms;
    c.agc_decay_ms = k_agc_decay_ms;
    points.push_back(
        make_point("C13", "AGC 1/300 ms, CCIR moderate, hf, 20 dB", hf, c, Profile::ssb, k_bits_gate, false));
    points.back().seed_point = 0;
    c.snr_db = k_fading_snr_db + k_f6_margin_db;
    points.push_back(make_point("C13", "AGC 1/300 ms, CCIR moderate, hf, 23 dB (gate + 3)", hf, c, Profile::ssb,
                                k_bits_side, true));
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 13);
    const double k_ber_gate = 1e-3;
    const Outcome& none = o[0][0];
    const Outcome& agc = o[1][0];
    const double r = ratio(agc.score.ber(), none.score.ber());
    result("C13", "AGC 1 ms / 300 ms + CCIR moderate, hf, 20 dB, same seeds",
           format("AGC: BER %.2e frames %.2f%%; none: BER %.2e frames %.2f%%; ratio %.2f", agc.score.ber(),
                  k_percent * agc.frames_ratio(), none.score.ber(), k_percent * none.frames_ratio(), r),
           "BER <= 1e-3 (C2' gate) and <= 2 x no AGC, frames >= 50% (prov.)",
           agc.score.ber() <= k_ber_gate && r <= k_ratio_agc && delivered(agc) && delivered(none));
    report(points[2], o[2][0]);
}

// C14: USB and LSB, the receiver mistuned to each preset's tuning tolerance (spec 1.4), CCIR moderate 20 dB.
// Each mistuned point shares data, noise and fading with its centred point, and is gated against it.
void test_c14_lsb_offset_fading() {
    struct Tolerance {
        Preset preset;
        const char* name;
        double hz;
    };
    const Tolerance presets[] = {{Preset::hf_fast, "hf_fast", 380.0},
                                 {Preset::hf, "hf", 500.0},
                                 {Preset::hf_robust, "hf_robust", 560.0},
                                 {Preset::hf_weak, "hf_weak", 590.0}};
    const std::size_t preset_count = sizeof(presets) / sizeof(presets[0]);
    const sim::Mode sidebands[] = {sim::Mode::usb, sim::Mode::lsb};
    const double shifts[] = {0.0, -1.0, 1.0};  // x tolerance; the centred point first
    const std::size_t shift_count = sizeof(shifts) / sizeof(shifts[0]);
    const double pivot = sim::ChannelConfig().lsb_pivot_hz;
    std::vector<CPoint> points;
    for (std::size_t p = 0; p < preset_count; ++p) {
        const EncoderConfig config = preset(presets[p].preset);
        for (std::size_t b = 0; b < 2; ++b) {
            const std::size_t centred = points.size();
            for (std::size_t s = 0; s < shift_count; ++s) {
                sim::ChannelConfig c = usb(k_fading_snr_db, sim::FadingPreset::ccir_moderate);
                c.mode = sidebands[b];
                c.freq_offset_hz = shifts[s] * presets[p].hz;
                const bool lsb = sidebands[b] == sim::Mode::lsb;
                const double ref = (lsb ? pivot - config.tone_hz : config.tone_hz) + c.freq_offset_hz;
                const double far = ref + (lsb ? 1.0 : -1.0) * span_hz(config);
                CPoint point = make_point("C14",
                                          format("%s, %s, offset %+.0f Hz (f_ref at %.0f Hz, band to %.0f Hz), CCIR "
                                                 "moderate, 20 dB",
                                                 lsb ? "LSB" : "USB", presets[p].name, c.freq_offset_hz, ref, far),
                                          config, c, Profile::ssb, k_bits_shift, false);
                point.seed_point = centred;
                points.push_back(point);
            }
        }
    }
    {
        sim::ChannelConfig c = usb(k_fading_snr_db + k_f6_margin_db, sim::FadingPreset::ccir_moderate);
        c.mode = sim::Mode::lsb;
        c.freq_offset_hz = presets[1].hz / 2.0;
        points.push_back(make_point("C14", "LSB, hf, offset +250 Hz, CCIR moderate, 23 dB (gate + 3)",
                                    preset(Preset::hf), c, Profile::ssb, k_bits_side, true));
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 14);
    const double k_ber_gate = 1e-3;
    const double k_frames_gate = 0.97;
    const double k_shift_ber_ratio = 2.0;
    const double k_shift_ber_slack = 2e-4;
    const double k_shift_frames_slack = 0.03;
    for (std::size_t i = 0; i + 1 < points.size(); i += shift_count) {
        const Outcome& centre = o[i][0];
        const bool hf_lsb = points[i].condition.find("LSB, hf,") == 0;
        if (hf_lsb) {
            result("C14", points[i].condition, measured(centre), "BER <= 1e-3, frames >= 97% (C2' gate, prov.)",
                   centre.score.ber() <= k_ber_gate && centre.frames_ratio() >= k_frames_gate);
        } else {
            report(points[i], centre);
        }
        for (std::size_t s = 1; s < shift_count; ++s) {
            const Outcome& shifted = o[i + s][0];
            const bool pass = shifted.score.ber() <= k_shift_ber_ratio * centre.score.ber() + k_shift_ber_slack &&
                              shifted.frames_ratio() >= centre.frames_ratio() - k_shift_frames_slack &&
                              delivered(shifted);
            result("C14", points[i + s].condition,
                   measured(shifted) + format("; centred: BER %.2e, frames %.2f%%", centre.score.ber(),
                                              k_percent * centre.frames_ratio()),
                   "BER <= 2 x centred + 2e-4, frames >= centred - 3 points and >= 50% (prov., derived)", pass);
        }
    }
    report(points.back(), o.back()[0]);
}

void test_c15_fm_emphasis_mismatch() {
    enum Gate { ber_gate, zero_errors, report_only };
    struct Row {
        Preset preset;
        const char* name;
        double cnr_db;
        double bits;
        Gate gate;
        bool f6;
    };
    const Row rows[] = {
        {Preset::hf_fast, "hf_fast", 0.0, k_bits_side, report_only, false},
        {Preset::hf_fast, "hf_fast", 2.0, k_bits_side, report_only, false},
        {Preset::hf_fast, "hf_fast", 4.0, k_bits_side, report_only, false},
        {Preset::hf_fast, "hf_fast", 6.0, k_bits_gate_rf, ber_gate, false},
        {Preset::hf_fast, "hf_fast", 8.0, k_bits_zero, zero_errors, false},
        {Preset::hf_fast, "hf_fast", 10.0, k_bits_zero, zero_errors, true},
        {Preset::hf_fast, "hf_fast", 14.0, k_bits_side, zero_errors, true},
        {Preset::fm, "fm", 2.0, k_bits_side, report_only, false},
        {Preset::fm, "fm", 4.5, k_bits_side, report_only, false},
        {Preset::fm, "fm", 6.0, k_bits_side, report_only, false},
        {Preset::fm, "fm", 8.0, k_bits_side, report_only, true},
        {Preset::fm, "fm", 14.0, k_bits_side, report_only, true},
        {Preset::fm_fast, "fm_fast", 4.0, k_bits_side, report_only, false},
        {Preset::fm_fast, "fm_fast", 7.0, k_bits_side, report_only, false},
        {Preset::fm_fast, "fm_fast", 10.0, k_bits_side, report_only, true},
        {Preset::fm_fast, "fm_fast", 14.0, k_bits_side, report_only, true},
    };
    const std::size_t count = sizeof(rows) / sizeof(rows[0]);
    std::vector<CPoint> points;
    for (std::size_t i = 0; i < count; ++i) {
        const sim::ChannelConfig c = fm_channel(rows[i].cnr_db, false);
        const EncoderConfig config = preset(rows[i].preset);
        points.push_back(make_point("C15",
                                    format("FM flat TX / de-emphasis RX, %s (f_ref %u), fm profile, CNR %.1f dB%s",
                                           rows[i].name, config.tone_hz, rows[i].cnr_db,
                                           rows[i].f6 ? " (>= gate + 3)" : ""),
                                    config, c, Profile::fm, rows[i].bits, rows[i].f6));
    }
    const std::vector<std::vector<Outcome> > o = run_cpoints(points, k_test_c + 15);
    const double k_ber_gate = 1e-3;
    const double k_zero_bits = 1e4;
    for (std::size_t i = 0; i < count; ++i) {
        const Score& s = o[i][0].score;
        switch (rows[i].gate) {
        case ber_gate:
            result("C15", points[i].condition, measured(o[i][0]), "BER <= 1e-3, frames >= 50% (C10 gate)",
                   s.ber() <= k_ber_gate && delivered(o[i][0]));
            break;
        case zero_errors:
            result("C15", points[i].condition, measured(o[i][0]), ">= 1e4 bits, 0 bit errors (C10 gate)",
                   s.bit_errors == 0 && static_cast<double>(s.matched * k_byte_bits) >= k_zero_bits);
            break;
        case report_only:
            report(points[i], o[i][0]);
            break;
        }
    }
}

}  // namespace regression
}  // namespace unlimited
