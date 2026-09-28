#include "portable_random.hpp"
#include "regression.hpp"

#include "test_harness.hpp"

#include <cmath>
#include <random>

// A1/A2 (AWGN per speed; the default adaptive line, and the fixed 70 % line against it), A3 (acquisition from byte 0)
// and S1 (short transmissions), spec 4.
namespace unlimited {
namespace regression {

namespace {

const std::uint32_t k_test_a1 = 10;
const std::uint32_t k_test_a3 = 30;
const std::uint32_t k_test_s1 = 40;

// A1: 32-byte transmissions, about 50,000 bits per point; the gate, gate + 3 and gate + 4.5 dB (A2's second point).
const std::size_t k_a1_bytes = 32;
const double k_a1_bits = 50000.0;
const double k_a1_offsets_db[] = {0.0, 3.0, 4.5};
const std::size_t k_a1_points = sizeof(k_a1_offsets_db) / sizeof(k_a1_offsets_db[0]);
const double k_a1_max_ber = 1e-3;
const double k_a1_max_loss = 0.01;
const double k_mistune_hz = 50.0;  // receivers mistuned within +-50 Hz (one offset per job)

// A3: 16-byte transmissions at gate + 3 dB, mistuned within +-50 Hz.
const std::size_t k_a3_bytes = 16;
const std::size_t k_a3_transmissions = 300;
const double k_a3_margin_db = 3.0;
const double k_a3_min_locked = 0.99;

// S1: 1, 2, 4 and 8 bytes at gate + 3 dB and at 20 dB.
const std::size_t k_s1_sizes[] = {1, 2, 4, 8};
const std::size_t k_s1_size_count = sizeof(k_s1_sizes) / sizeof(k_s1_sizes[0]);
const double k_s1_snrs[] = {3.0, 20.0};  // gate + 3 dB, then an absolute 20 dB
const std::size_t k_s1_transmissions = 150;
const double k_s1_reliable = 0.99;

const double k_percent = 100.0;

double mistune(std::uint32_t seed) {
    std::mt19937 generator(seed);
    return sim::UniformReal(-k_mistune_hz, k_mistune_hz)(generator);
}

}  // namespace

void test_a1_awgn() {
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> points;
    for (std::size_t v = 0; v < k_speed_count; ++v) {
        const EncoderConfig config = speed_config(k_speeds[v]);
        for (std::size_t o = 0; o < k_a1_points; ++o) {
            JobPlan shape;
            shape.channel = usb_channel(gate_db(k_speeds[v]) + k_a1_offsets_db[o]);
            shape.decoders.push_back(receiver_for(config));
            shape.decoders.push_back(fixed_receiver_for(config));
            const std::size_t first = jobs.size();
            add_jobs(jobs, points, v * k_a1_points + o, shape, config, transmissions_for(k_a1_bits, k_a1_bytes),
                     k_a1_bytes, k_test_a1);
            for (std::size_t j = first; j < jobs.size(); ++j) jobs[j].channel.freq_offset_hz = mistune(jobs[j].channel.seed);
        }
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, points, k_speed_count * k_a1_points);
    for (std::size_t v = 0; v < k_speed_count; ++v) {
        for (std::size_t o = 0; o < k_a1_points; ++o) {
            const Outcome& adaptive = outcomes[v * k_a1_points + o][0];
            const Outcome& fixed = outcomes[v * k_a1_points + o][1];
            const double snr = gate_db(k_speeds[v]) + k_a1_offsets_db[o];
            const std::string condition = format("%s, AWGN %.1f dB (gate %+.1f), mistuned +-50 Hz",
                                                 speed_text(k_speeds[v]).c_str(), snr, k_a1_offsets_db[o]);
            ledger("A1 " + condition, adaptive);
            ledger("A2 " + condition, fixed);
            if (o == 0) {
                result("A1", condition + ", adaptive line (the default)", ber_text(adaptive),
                       "BER <= 1e-3, loss <= 1 % (provisional); extra bytes reported (V22)",
                       adaptive.score.ber() <= k_a1_max_ber && adaptive.score.loss() <= k_a1_max_loss);
            } else {
                result("A1", condition + ", adaptive line (the default)", ber_text(adaptive), "report", true,
                       Kind::report);
            }
            result("A2", condition + ", fixed 70 % line against the default adaptive line",
                   format("fixed BER %.2e, loss %.2f%%; adaptive BER %.2e, loss %.2f%%", fixed.score.ber(),
                          k_percent * fixed.score.loss(), adaptive.score.ber(), k_percent * adaptive.score.loss()),
                   "report", true, Kind::report);
        }
    }
}

void test_a3_acquisition() {
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> points;
    for (std::size_t v = 0; v < k_speed_count; ++v) {
        const EncoderConfig config = speed_config(k_speeds[v]);
        JobPlan shape;
        shape.channel = usb_channel(gate_db(k_speeds[v]) + k_a3_margin_db);
        shape.decoders.push_back(receiver_for(config));
        const std::size_t first = jobs.size();
        add_jobs(jobs, points, v, shape, config, k_a3_transmissions, k_a3_bytes, k_test_a3);
        for (std::size_t j = first; j < jobs.size(); ++j) jobs[j].channel.freq_offset_hz = mistune(jobs[j].channel.seed);
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, points, k_speed_count);
    for (std::size_t v = 0; v < k_speed_count; ++v) {
        const Outcome& o = outcomes[v][0];
        const double locked = static_cast<double>(o.score.locked_transmissions) / o.transmissions;
        const std::string condition =
            format("%s, %zu transmissions of %zu bytes at gate + 3 dB (%.1f dB), mistuned +-50 Hz",
                   speed_text(k_speeds[v]).c_str(), o.transmissions, k_a3_bytes, gate_db(k_speeds[v]) + k_a3_margin_db);
        ledger("A3 " + condition, o);
        result("A3", condition, format("decoded from byte 0 %.2f%%; ", k_percent * locked) + ber_text(o),
               ">= 99 % from byte 0 (provisional)", locked >= k_a3_min_locked);
    }
}

void test_s1_short() {
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> points;
    for (std::size_t v = 0; v < k_speed_count; ++v) {
        const EncoderConfig config = speed_config(k_speeds[v]);
        for (std::size_t s = 0; s < k_s1_size_count; ++s) {
            for (std::size_t n = 0; n < 2; ++n) {
                JobPlan shape;
                const double snr = n == 0 ? gate_db(k_speeds[v]) + k_s1_snrs[0] : k_s1_snrs[1];
                shape.channel = usb_channel(snr);
                shape.decoders.push_back(receiver_for(config));
                const std::size_t point = (v * k_s1_size_count + s) * 2 + n;
                const std::size_t first = jobs.size();
                add_jobs(jobs, points, point, shape, config, k_s1_transmissions, k_s1_sizes[s], k_test_s1);
                for (std::size_t j = first; j < jobs.size(); ++j)
                    jobs[j].channel.freq_offset_hz = mistune(jobs[j].channel.seed);
            }
        }
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, points, k_speed_count * k_s1_size_count * 2);
    for (std::size_t v = 0; v < k_speed_count; ++v) {
        for (std::size_t n = 0; n < 2; ++n) {
            std::string measured;
            std::size_t shortest = 0;
            for (std::size_t s = 0; s < k_s1_size_count; ++s) {
                const Outcome& o = outcomes[(v * k_s1_size_count + s) * 2 + n][0];
                const std::string where = format("S1 %s, %zu bytes, %s", speed_text(k_speeds[v]).c_str(),
                                                 k_s1_sizes[s], n == 0 ? "gate + 3 dB" : "20 dB");
                ledger(where, o);
                // Decoded: locked on byte 0 and nothing of it lost.
                const double decoded = static_cast<double>(o.score.locked_transmissions) / o.transmissions;
                const double complete = 1.0 - o.score.loss();
                measured += format("%s%zu B: from byte 0 %.1f%%, bytes %.1f%%", s == 0 ? "" : "; ", k_s1_sizes[s],
                                   k_percent * decoded, k_percent * complete);
                if (shortest == 0 && decoded >= k_s1_reliable && complete >= k_s1_reliable) shortest = k_s1_sizes[s];
            }
            const double snr = n == 0 ? gate_db(k_speeds[v]) + k_s1_snrs[0] : k_s1_snrs[1];
            result("S1", format("%s, %zu transmissions per size, %.1f dB, mistuned +-50 Hz",
                                speed_text(k_speeds[v]).c_str(), k_s1_transmissions, snr),
                   measured + format("; shortest decoded >= 99%%: %s",
                                     shortest == 0 ? "none" : format("%zu bytes", shortest).c_str()),
                   "report: the shortest transmission decoded >= 99 %", true, Kind::report);
        }
    }
}

}  // namespace regression
}  // namespace unlimited
