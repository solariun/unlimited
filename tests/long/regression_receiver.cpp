#include "regression.hpp"

#include "test_harness.hpp"

#include <cmath>

// L5 (sender clock error over 10-minute transmissions) and L19 (every pitch of the search range in SSB filters of
// 1.8 to 3.0 kHz), spec 4.
namespace unlimited {
namespace regression {

namespace {

const std::uint32_t k_test_l5 = 50;
const std::uint32_t k_test_l19 = 60;

// L5: one 10-minute transmission per speed and sign of a 1000 ppm clock error, at 20 dB.
const double k_l5_minutes = 10.0;
const double k_s_per_min = 60.0;
const double k_l5_ppm = 1000.0;
const double k_l5_snr_db = 20.0;
const float k_l5_speeds[] = {1.0f, 6.0f, 25.0f};
const std::size_t k_l5_speed_count = sizeof(k_l5_speeds) / sizeof(k_l5_speeds[0]);
const double k_percent = 100.0;

// L19: pitches across each filter's search range, gate + 6 dB, 16-byte transmissions.
struct Filter {
    const char* name;
    std::uint16_t low_hz;
    std::uint16_t high_hz;
};
const Filter k_filters[] = {{"1.8 kHz", 300, 2100}, {"2.4 kHz", 300, 2700}, {"2.7 kHz", 200, 2900},
                            {"3.0 kHz", 100, 3100}};
const std::size_t k_filter_count = sizeof(k_filters) / sizeof(k_filters[0]);
const std::size_t k_l19_pitches = 7;          // the search's edges and 5 pitches between
const std::uint16_t k_l19_edge_inset_hz = 5;  // the pitch on the edge itself leaves no room for the receiver's error
const double k_l19_margin_db = 6.0;
const std::size_t k_l19_bytes = 16;
const std::size_t k_l19_transmissions = 24;

}  // namespace

void test_l5_clock() {
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> points;
    for (std::size_t v = 0; v < k_l5_speed_count; ++v) {
        const EncoderConfig config = speed_config(k_l5_speeds[v]);
        const std::size_t bytes = static_cast<std::size_t>(k_l5_minutes * k_s_per_min * k_l5_speeds[v]);
        for (std::size_t sign = 0; sign < 2; ++sign) {
            JobPlan job;
            job.channel = usb_channel(k_l5_snr_db);
            job.channel.clock_ppm = sign == 0 ? k_l5_ppm : -k_l5_ppm;
            job.channel.seed = seed_of(k_test_l5, static_cast<std::uint32_t>(v), static_cast<std::uint32_t>(sign));
            job.decoders.push_back(receiver_for(config, false));
            job.transmissions.push_back(random_tx(config, bytes, job.channel.seed));
            jobs.push_back(job);
            points.push_back(v * 2 + sign);
        }
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, points, k_l5_speed_count * 2);
    for (std::size_t v = 0; v < k_l5_speed_count; ++v) {
        for (std::size_t sign = 0; sign < 2; ++sign) {
            const Outcome& o = outcomes[v * 2 + sign][0];
            const std::string condition =
                format("%s, one %g-minute transmission (%zu bytes), sender clock %+g ppm, %.0f dB",
                       speed_text(k_l5_speeds[v]).c_str(), k_l5_minutes, o.score.bytes_sent,
                       sign == 0 ? k_l5_ppm : -k_l5_ppm, k_l5_snr_db);
            ledger("L5 " + condition, o);
            const bool pass = o.score.locks == 1 && o.score.lost_bytes == 0 && o.score.wrong_bytes == 0 && integrity(o);
            result("L5", condition,
                   ber_text(o) + format("; T error mean %.4f%%, worst %.4f%%", k_percent * o.mean_slot_error(),
                                        k_percent * o.worst_slot_error),
                   "no slip: every byte, one lock", pass);
        }
    }
}

void test_l19_passband() {
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> points;
    std::vector<std::uint16_t> tones;
    for (std::size_t v = 0; v < k_speed_count; ++v) {
        for (std::size_t f = 0; f < k_filter_count; ++f) {
            DecoderConfig receiver;
            receiver.slot_us = speed_config(k_speeds[v]).slot_us;
            receiver.passband.low_hz = k_filters[f].low_hz;
            receiver.passband.high_hz = k_filters[f].high_hz;
            const Passband search = receiver.search_range();
            for (std::size_t p = 0; p < k_l19_pitches; ++p) {
                const std::size_t point = (v * k_filter_count + f) * k_l19_pitches + p;
                const double low = search.low_hz + k_l19_edge_inset_hz;
                const double high = search.high_hz - k_l19_edge_inset_hz;
                const std::uint16_t tone =
                    static_cast<std::uint16_t>(std::lround(low + (high - low) * p / (k_l19_pitches - 1)));
                tones.push_back(tone);
                EncoderConfig config = speed_config(k_speeds[v], tone);
                config.passband = receiver.passband;
                if (!config.valid()) continue;
                JobPlan shape;
                shape.channel = usb_channel(gate_db(k_speeds[v]) + k_l19_margin_db);
                shape.channel.rx_low_hz = k_filters[f].low_hz;
                shape.channel.rx_high_hz = k_filters[f].high_hz;
                shape.decoders.push_back(receiver);
                add_jobs(jobs, points, point, shape, config, k_l19_transmissions, k_l19_bytes, k_test_l19);
            }
        }
    }
    const std::vector<std::vector<Outcome> > outcomes =
        run_points(jobs, points, k_speed_count * k_filter_count * k_l19_pitches);
    for (std::size_t v = 0; v < k_speed_count; ++v) {
        for (std::size_t f = 0; f < k_filter_count; ++f) {
            Outcome total;
            std::string worst;
            double worst_ber = -1.0;
            for (std::size_t p = 0; p < k_l19_pitches; ++p) {
                const std::size_t point = (v * k_filter_count + f) * k_l19_pitches + p;
                if (outcomes[point].empty()) continue;
                const Outcome& o = outcomes[point][0];
                merge(total, o);
                const double ber = o.score.ber() + o.score.loss();
                if (ber > worst_ber) {
                    worst_ber = ber;
                    worst = format("worst pitch %u Hz: BER %.2e, loss %.2f%%", tones[point], o.score.ber(),
                                   k_percent * o.score.loss());
                }
            }
            const std::string condition =
                format("%s, %zu pitches over the search range, %s SSB filter (%u-%u Hz), gate + 6 dB",
                       speed_text(k_speeds[v]).c_str(), k_l19_pitches, k_filters[f].name, k_filters[f].low_hz,
                       k_filters[f].high_hz);
            ledger("L19 " + condition, total);
            result("L19", condition, ber_text(total) + "; " + worst, "BER <= 1e-4, 0 extra, 0 shifted (provisional)",
                   near_zero_errors(total));
        }
    }
}

}  // namespace regression
}  // namespace unlimited
