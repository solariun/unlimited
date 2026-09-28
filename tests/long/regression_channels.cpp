#include "regression.hpp"

#include "test_harness.hpp"

#include <cmath>

// C (spec 4): the channel simulator's conditions, re-measured for v1.0 and reported; their gates are Gustavo's to set.
namespace unlimited {
namespace regression {

namespace {

const std::uint32_t k_test_c = 100;
const std::size_t k_c_bytes = 16;
const std::size_t k_c_transmissions = 60;
const double k_fading_peak = 3.0;  // Rayleigh amplitude above 3x RMS: probability 1.2e-4
const double k_fm_peak = 2.0;      // FM clicks
const double k_interferer_peak = 2.0;

struct Condition {
    const char* id;
    const char* name;
    std::vector<float> speeds;
    sim::ChannelConfig channel;
    double peak_factor;
    Passband passband;  // the sender's and the receiver's
};

Passband band(std::uint16_t low_hz, std::uint16_t high_hz) {
    Passband p;
    p.low_hz = low_hz;
    p.high_hz = high_hz;
    return p;
}

std::vector<Condition> conditions() {
    const std::vector<float> hf = {1.0f, 3.0f, 6.0f, 12.0f};
    const Passband ssb = band(k_ssb_passband_low_hz, k_ssb_passband_high_hz);
    std::vector<Condition> list;
    const auto faded = [&](const char* id, const char* name, sim::FadingPreset preset, double snr_db) {
        sim::ChannelConfig c = usb_channel(snr_db);
        sim::apply_preset(c, preset);
        list.push_back(Condition{id, name, hf, c, k_fading_peak, ssb});
    };
    faded("C1", "CCIR good (0.5 ms, 0.1 Hz), USB 10 dB", sim::FadingPreset::ccir_good, 10.0);
    faded("C2", "CCIR moderate (1 ms, 0.5 Hz), USB 15 dB", sim::FadingPreset::ccir_moderate, 15.0);
    faded("C3", "CCIR poor (2 ms, 1 Hz), USB 20 dB", sim::FadingPreset::ccir_poor, 20.0);
    faded("C4", "flat Rayleigh (1 Hz), USB 15 dB", sim::FadingPreset::flat, 15.0);
    faded("C12", "flutter (0.5 ms, 10 Hz), USB 15 dB", sim::FadingPreset::flutter, 15.0);
    sim::ChannelConfig qsb = usb_channel(10.0);
    qsb.qsb_depth_db = 10.0;
    qsb.qsb_rate_hz = 0.2;
    list.push_back(Condition{"C5", "QSB 10 dB deep at 0.2 Hz, USB 10 dB", hf, qsb, 1.0, ssb});
    sim::ChannelConfig qrn = usb_channel(10.0);
    qrn.impulse_rate_hz = 5.0;
    qrn.impulse_level_db = 20.0;
    list.push_back(Condition{"C6", "QRN 5 crashes/s +20 dB, blanker on, USB 10 dB", hf, qrn, k_interferer_peak, ssb});
    sim::ChannelConfig agc = usb_channel(10.0);
    agc.agc = true;
    list.push_back(Condition{"C7", "receiver AGC, USB 10 dB", hf, agc, 1.0, ssb});
    sim::ChannelConfig carrier = usb_channel(10.0);
    carrier.carrier_hz = 1200.0;
    carrier.carrier_db = -6.0;
    list.push_back(Condition{"C8", "a steady carrier 300 Hz below, -6 dB, USB 10 dB", hf, carrier, k_interferer_peak, ssb});
    sim::ChannelConfig cw = usb_channel(10.0);
    cw.cw_hz = 1750.0;
    cw.cw_db = -6.0;
    cw.cw_wpm = 20.0;
    list.push_back(Condition{"C9", "keyed CW 250 Hz above, 20 WPM, -6 dB, USB 10 dB", hf, cw, k_interferer_peak, ssb});
    sim::ChannelConfig agc_fading = usb_channel(15.0);
    sim::apply_preset(agc_fading, sim::FadingPreset::ccir_moderate);
    agc_fading.agc = true;
    list.push_back(Condition{"C13", "CCIR moderate with receiver AGC, USB 15 dB", hf, agc_fading, k_fading_peak, ssb});
    sim::ChannelConfig lsb = usb_channel(15.0);
    lsb.mode = sim::Mode::lsb;
    sim::apply_preset(lsb, sim::FadingPreset::ccir_good);
    lsb.freq_offset_hz = 120.0;
    list.push_back(Condition{"C14", "LSB, mistuned 120 Hz, CCIR good, 15 dB", hf, lsb, k_fading_peak, ssb});
    sim::ChannelConfig fm;
    fm.mode = sim::Mode::fm;
    fm.snr_db = snr_for_fm_cnr(12.0, fm);
    fm.rx_high_hz = k_fm_passband_high_hz;
    fm.fm_audio_high_hz = k_fm_passband_high_hz;
    list.push_back(Condition{"C10", "FM, CNR 12 dB in 12.5 kHz", std::vector<float>{6.0f, 12.0f, 25.0f}, fm, k_fm_peak,
                             band(k_fm_passband_low_hz, k_fm_passband_high_hz)});
    sim::ChannelConfig fm_flat = fm;
    fm_flat.fm_rx_deemphasis = false;
    list.push_back(Condition{"C15", "FM, CNR 12 dB, receiver without de-emphasis (data port)",
                             std::vector<float>{12.0f, 25.0f}, fm_flat, k_fm_peak,
                             band(k_fm_passband_low_hz, k_fm_passband_high_hz)});
    sim::ChannelConfig am;
    am.mode = sim::Mode::am;
    am.snr_db = snr_for_am_cnr(15.0, am);
    am.rx_low_hz = k_am_passband_low_hz;
    am.rx_high_hz = k_am_passband_high_hz;
    list.push_back(Condition{"C11", "AM, CNR 15 dB in 6 kHz", std::vector<float>{3.0f, 6.0f, 12.0f}, am, k_fm_peak,
                             band(k_am_passband_low_hz, k_am_passband_high_hz)});
    return list;
}

}  // namespace

void test_c_channels() {
    const std::vector<Condition> list = conditions();
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> points;
    struct Row {
        std::size_t condition;
        float speed;
    };
    std::vector<Row> rows;
    for (std::size_t c = 0; c < list.size(); ++c) {
        for (std::size_t v = 0; v < list[c].speeds.size(); ++v) {
            EncoderConfig config = speed_config(list[c].speeds[v]);
            config.passband = list[c].passband;
            if (!config.valid()) continue;
            JobPlan shape;
            shape.channel = list[c].channel;
            shape.peak_factor = list[c].peak_factor;
            shape.decoders.push_back(receiver_for(config, false));
            shape.decoders.push_back(receiver_for(config, true));
            add_jobs(jobs, points, rows.size(), shape, config, k_c_transmissions, k_c_bytes,
                     k_test_c + static_cast<std::uint32_t>(c));
            rows.push_back(Row{c, list[c].speeds[v]});
        }
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, points, rows.size());
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const Condition& condition = list[rows[r].condition];
        const std::string text = format("%s, %s, %zu x %zu bytes", condition.name, speed_text(rows[r].speed).c_str(),
                                        k_c_transmissions, k_c_bytes);
        const Outcome& fixed = outcomes[r][0];
        const Outcome& adaptive = outcomes[r][1];
        ledger(std::string(condition.id) + " " + text, fixed);
        ledger(std::string(condition.id) + " adaptive " + text, adaptive);
        result(condition.id, text + ", fixed 70 % line", ber_text(fixed), "report (gate: Gustavo's decision)", true,
               Kind::report);
        result(condition.id, text + ", adaptive line", ber_text(adaptive), "report", true, Kind::report);
    }
}

}  // namespace regression
}  // namespace unlimited
