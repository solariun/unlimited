#include "regression.hpp"

#include "test_harness.hpp"

#include <cmath>
#include <random>

// L19 (spec 8.2, 1.5): the receiver's audio filter and passband setting, every preset shifted down and up by its
// printed shift tolerance - 10 Hz on each side; stations outside the search range ignored. L20 long (spec 8.2, 3.12
// V7): a receiver started inside a running transmission joins it when N is a multiple of 8, bytes aligned, and waits
// otherwise.
namespace unlimited {
namespace regression {

namespace {

const std::uint32_t k_test_l19 = 300;
const std::uint32_t k_test_l20 = 301;

struct Filter {
    const char* name;
    std::uint16_t low_hz;
    std::uint16_t high_hz;
};

// Spec 1.5 typical passbands (-6 dB points).
const Filter k_filters[] = {{"SSB 1.8 kHz", 300, 2100},
                            {"SSB 2.4 kHz", 300, 2700},
                            {"SSB 2.7 kHz", 200, 2900},
                            {"3.0 kHz", 100, 3000}};
const Preset k_l19_presets[] = {Preset::hf_slow, Preset::hf, Preset::hf_fast, Preset::am, Preset::fm};
const double k_l19_bits = 2.4e4;
const double k_min_gate_bits = 1e4;
const std::size_t k_l19_bytes = 200;
const double k_shift_margin_hz = 10.0;
const double k_gate_margin_db = 3.0;
const double k_loss_gate = 0.01;

// Stations outside the receiver's search range, heard through a wide filter at 20 dB.
struct Outside {
    std::uint16_t receiver_low_hz;
    std::uint16_t receiver_high_hz;
    std::uint16_t tone_hz;
};
const Outside k_outside[] = {{300, 2100, 2300}, {300, 2100, 2500}, {1000, 2000, 700}, {1000, 2000, 2300}};
const std::uint16_t k_wide_low_hz = 100;
const std::uint16_t k_wide_high_hz = 3000;
const double k_outside_snr_db = 20.0;
const std::size_t k_outside_transmissions = 10;
const std::size_t k_outside_bytes = 50;

const std::uint8_t k_join_bits[] = {8, 16, 24, 32};
const std::uint8_t k_wait_bits[] = {3, 4, 5, 7, 12};
const double k_join_slots_ms[] = {8.0, 16.0, 32.0};
const std::size_t k_join_bytes = 200;
const std::size_t k_starts = 20;
const std::size_t k_first_join_package = 1;   // the receiver starts after the first STOP: the preamble is missed
const std::size_t k_packages_left = 7;        // ... and at least this many packages before the last one
const double k_join_packages = 6.0;
const double k_join_rate = 0.95;
const double k_never = 1e9;                   // lock time of a start that never joined
const std::size_t k_lock_details = 3;
const double k_join_snr_db = 20.0;
const double k_lock_slot_tolerance = 0.03;
const double k_ms_per_s = 1000.0;
const double k_percent = 100.0;

std::string filter_text(const Filter& f) {
    return format("%s filter %u-%u Hz", f.name, f.low_hz, f.high_hz);
}

}  // namespace

void test_l19_passband() {
    const std::size_t filters = sizeof(k_filters) / sizeof(k_filters[0]);
    const std::size_t presets = sizeof(k_l19_presets) / sizeof(k_l19_presets[0]);
    const double sides[] = {-1.0, 1.0};
    struct Row {
        std::string condition;
        EncoderConfig config;
        DecoderConfig receiver;
        double shift;
        bool in_range;
    };
    std::vector<Row> rows;
    std::vector<JobPlan> jobs;
    std::vector<std::size_t> point_of;
    const auto add_jobs = [&](const Row& row, const Filter& filter) {
        const std::size_t point = rows.size();
        rows.push_back(row);
        const std::size_t count = transmissions_for(k_l19_bits, k_l19_bytes);
        const std::size_t per_job =
            transmissions_per_job(transmission_seconds(row.config, k_l19_bytes, JobPlan().gap_ms), count);
        std::uint32_t job_index = 0;
        for (std::size_t first = 0; first < count; first += per_job, ++job_index) {
            JobPlan job;
            const std::uint32_t seed = seed_of(k_test_l19, static_cast<std::uint32_t>(point), job_index);
            for (std::size_t t = first; t < std::min(count, first + per_job); ++t) {
                job.transmissions.push_back(random_tx(row.config, k_l19_bytes, data_seed(seed, t)));
            }
            job.channel.mode = sim::Mode::usb;
            job.channel.snr_db = gate_db(slot_ms_of(row.config)) + k_gate_margin_db;
            job.channel.freq_offset_hz = row.shift;
            job.channel.rx_low_hz = filter.low_hz;
            job.channel.rx_high_hz = filter.high_hz;
            job.channel.seed = seed;
            job.decoders.push_back(row.receiver);
            jobs.push_back(job);
            point_of.push_back(point);
        }
    };
    for (std::size_t f = 0; f < filters; ++f) {
        for (std::size_t p = 0; p < presets; ++p) {
            EncoderConfig config = preset(k_l19_presets[p]);
            config.passband.low_hz = k_filters[f].low_hz;
            config.passband.high_hz = k_filters[f].high_hz;
            // The shift tolerance the sender prints (spec 1.5): the filter room, each side also ending where the pitch
            // would leave the search of the receiver that hears it.
            const PassbandFit fit = passband_fit(config);
            if (!fit.fits || !config.valid()) {
                result("L19", format("%s in the %s", preset_name(k_l19_presets[p]).c_str(), filter_text(k_filters[f]).c_str()),
                       format("does not fit (margins %d / %d Hz)", fit.margin_low_hz, fit.margin_high_hz),
                       "report (only presets that fit are gated)", true, Kind::report);
                continue;
            }
            DecoderConfig receiver = receiver_for(config);
            receiver.passband = config.passband;
            const Passband range = receiver.search_range();
            const double margins[] = {static_cast<double>(fit.margin_low_hz), static_cast<double>(fit.margin_high_hz)};
            for (std::size_t s = 0; s < test::count_of(sides); ++s) {
                Row row;
                row.shift = sides[s] * (margins[s] - k_shift_margin_hz);
                row.config = config;
                row.receiver = receiver;
                const double pitch = config.tone_hz + row.shift;
                row.in_range = pitch >= range.low_hz && pitch <= range.high_hz;
                row.condition =
                    format("%s (%s) in the %s, shift tolerance -%d/+%d Hz, shift %+.0f Hz -> pitch %.0f Hz; receiver "
                           "passband %u-%u Hz, search %u-%u Hz, usb %+.1f dB (gate + 3)",
                           preset_name(k_l19_presets[p]).c_str(), config_text(config).c_str(),
                           filter_text(k_filters[f]).c_str(), fit.margin_low_hz, fit.margin_high_hz, row.shift, pitch,
                           receiver.passband.low_hz, receiver.passband.high_hz, range.low_hz, range.high_hz,
                           gate_db(slot_ms_of(config)) + k_gate_margin_db);
                add_jobs(row, k_filters[f]);
            }
        }
    }
    // Stations outside the receiver's search range.
    const std::size_t outside = sizeof(k_outside) / sizeof(k_outside[0]);
    const std::size_t first_outside = rows.size();
    for (std::size_t i = 0; i < outside; ++i) {
        EncoderConfig config = preset(Preset::hf);
        config.tone_hz = k_outside[i].tone_hz;
        config.passband.low_hz = k_wide_low_hz;
        config.passband.high_hz = k_wide_high_hz;
        DecoderConfig receiver;
        receiver.passband.low_hz = k_outside[i].receiver_low_hz;
        receiver.passband.high_hz = k_outside[i].receiver_high_hz;
        const Passband range = receiver.search_range();
        Row row;
        row.config = config;
        row.receiver = receiver;
        row.shift = 0.0;
        row.in_range = false;
        row.condition = format("hf station at %u Hz, receiver passband %u-%u Hz (search %u-%u Hz), channel filter "
                               "%u-%u Hz, usb %.0f dB, %zu transmissions",
                               config.tone_hz, receiver.passband.low_hz, receiver.passband.high_hz, range.low_hz,
                               range.high_hz, k_wide_low_hz, k_wide_high_hz, k_outside_snr_db, k_outside_transmissions);
        const std::size_t point = rows.size();
        rows.push_back(row);
        JobPlan job;
        const std::uint32_t seed = seed_of(k_test_l19, static_cast<std::uint32_t>(point), 0);
        for (std::size_t t = 0; t < k_outside_transmissions; ++t) {
            job.transmissions.push_back(random_tx(config, k_outside_bytes, data_seed(seed, t)));
        }
        job.channel.mode = sim::Mode::usb;
        job.channel.snr_db = k_outside_snr_db;
        job.channel.rx_low_hz = k_wide_low_hz;
        job.channel.rx_high_hz = k_wide_high_hz;
        job.channel.seed = seed;
        job.decoders.push_back(receiver);
        jobs.push_back(job);
        point_of.push_back(point);
    }
    const std::vector<std::vector<Outcome> > outcomes = run_points(jobs, point_of, rows.size());
    for (std::size_t i = 0; i < first_outside; ++i) {
        const Outcome& o = outcomes[i][0];
        const loopback::Score& s = o.score;
        const std::string text = ber_text(o) + format(", max wrong run %zu, shifted bytes %zu", o.max_wrong_run,
                                                      o.misplaced_bytes);
        if (!rows[i].in_range) {
            // A shift the printed tolerance allows but the receiver's search does not hold: decoding is reported, the
            // integrity still gated (gate decision G5: BER <= 1e-4, never an extra or shifted byte).
            result("L19", rows[i].condition, text + "; the shifted pitch is outside the receiver's search range",
                   "report (decoding outside the search range)", true, Kind::report);
            result("L19", rows[i].condition + ", integrity",
                   format("BER %.2e, wrong %zu, extra %zu, shifted %zu", s.ber(), s.wrong_bytes, s.extra_bytes,
                          o.misplaced_bytes),
                   "BER <= 1e-4, 0 extra, 0 shifted bytes", near_zero_errors(o));
            continue;
        }
        // Gate decision G5 (spec 0.8): BER <= 1e-4 with 0 extra and 0 shifted bytes, no longer 0 bit errors.
        const bool enough = static_cast<double>(s.matched * k_bits_per_byte) >= k_min_gate_bits;
        const bool pass = near_zero_errors(o) && s.loss() <= k_loss_gate && enough;
        result("L19", rows[i].condition, text, "decodes: BER <= 1e-4, 0 extra, 0 shifted, loss <= 1%, >= 1e4 bits",
               pass);
    }
    for (std::size_t i = first_outside; i < rows.size(); ++i) {
        const Outcome& o = outcomes[i][0];
        result("L19", rows[i].condition,
               format("locked %zu, bytes %zu, %s", o.score.locks, o.score.bytes_released, lost_text(o).c_str()),
               "ignored: 0 locked, 0 bytes", o.score.locks == 0 && o.score.bytes_released == 0);
    }
}

namespace {

struct JoinCase {
    EncoderConfig config;
    double snr_db;
    bool joins;  // N is a multiple of 8
};

struct JoinResult {
    std::size_t starts = 0;
    std::size_t joined = 0;       // a late-join `locked` with the sent T and N
    std::size_t in_time = 0;      // ... within 6 packages of the receiver's start
    std::size_t wrong_locks = 0;  // a `locked` with another T or N, or without late_join
    std::size_t locks = 0;
    std::size_t bytes = 0;
    std::size_t matched = 0;
    std::size_t wrong_bytes = 0;
    std::size_t extra_bytes = 0;
    std::size_t acausal_bytes = 0;
    std::vector<double> lock_packages;  // packages from the start to the lock, per start (k_never: no lock)
    std::vector<std::string> odd_locks;  // the first k_lock_details locks that are wrong, or any lock when N waits
    double package_ms = 0.0;
};

JoinResult run_join_case(const JoinCase& c, std::uint32_t seed) {
    JoinResult r;
    const std::vector<std::uint8_t> data = loopback::random_bytes(k_join_bytes, seed);
    loopback::Recording recording;
    loopback::append_silence(recording, k_quiet_ms);
    loopback::append_transmission(recording, data, c.config);
    loopback::append_silence(recording, k_quiet_ms);
    sim::ChannelConfig channel;
    channel.mode = sim::Mode::usb;
    channel.snr_db = c.snr_db;
    channel.seed = seed;
    const std::vector<std::int16_t> samples = apply_channel(recording.samples, channel, c.config.amplitude, 1.0);
    const double delay = channel_delay(sim::Mode::usb);
    const loopback::Transmission& tx = recording.transmissions[0];
    const std::size_t packages = loopback::package_count(tx);
    const double package_samples = (c.config.bits_per_package + 1.0) * loopback::slot_samples(c.config);
    r.package_ms = package_samples * k_ms_per_s / k_decoder_rate_hz;
    const std::size_t low =
        static_cast<std::size_t>(loopback::slot_start_sample(tx, loopback::package_start_slot(tx, k_first_join_package)));
    const std::size_t high = static_cast<std::size_t>(
        loopback::slot_start_sample(tx, loopback::package_start_slot(tx, packages - k_packages_left)));
    std::mt19937 generator(seed);
    std::uniform_int_distribution<std::size_t> start(low, high);
    const DecoderConfig receiver = receiver_for(c.config);
    for (std::size_t n = 0; n < k_starts; ++n) {
        const std::size_t from = start(generator);
        const std::vector<std::int16_t> tail(samples.begin() + static_cast<long>(from), samples.end());
        const loopback::Capture capture = loopback::run_decoder(tail, receiver, 0);
        const Outcome o = evaluate_recording(recording, capture, delay - static_cast<double>(from));
        ++r.starts;
        r.locks += o.score.locks;
        r.bytes += o.score.bytes_released;
        r.matched += o.score.matched;
        r.wrong_bytes += o.score.wrong_bytes;
        r.extra_bytes += o.score.extra_bytes;
        r.acausal_bytes += o.acausal_bytes;
        bool first_lock = true;
        double lock_at = k_never;
        for (std::size_t i = 0; i < capture.events.size(); ++i) {
            const Event& e = capture.events[i];
            if (e.type != EventType::locked) continue;
            const double true_ms = slot_ms_of(c.config);
            const bool right = (e.flags & event_flag_late_join) != 0 &&
                               e.bits_per_package == c.config.bits_per_package &&
                               std::fabs(e.slot_ms / true_ms - 1.0) <= k_lock_slot_tolerance;
            if (!right || !c.joins) {
                if (r.odd_locks.size() < k_lock_details) {
                    r.odd_locks.push_back(format("start %zu +%.1f packages: T %.2f ms N %u, %s, package %u",
                                                 from, static_cast<double>(capture.event_sample[i]) / package_samples,
                                                 e.slot_ms, e.bits_per_package,
                                                 (e.flags & event_flag_late_join) != 0 ? "late_join" : "from a preamble",
                                                 e.package_index));
                }
            }
            if (!right) {
                ++r.wrong_locks;
                continue;
            }
            if (!first_lock) continue;
            first_lock = false;
            lock_at = static_cast<double>(capture.event_sample[i]) / package_samples;
            ++r.joined;
            if (lock_at <= k_join_packages) ++r.in_time;
        }
        r.lock_packages.push_back(lock_at);
    }
    return r;
}

}  // namespace

// L20 long: 20 random starts inside a 200-byte transmission per N, T and SNR (gate + 3 dB and 20 dB).
void test_l20_cold_late_join() {
    std::vector<JoinCase> cases;
    const std::size_t slots = sizeof(k_join_slots_ms) / sizeof(k_join_slots_ms[0]);
    const auto add = [&](const std::uint8_t* bits, std::size_t count, bool joins) {
        for (std::size_t b = 0; b < count; ++b) {
            if (bits[b] > k_max_bits_per_package) continue;
            for (std::size_t t = 0; t < slots; ++t) {
                const EncoderConfig config = slot_preset(k_join_slots_ms[t], bits[b]);
                if (!config.valid()) continue;
                const double snrs[] = {gate_db(k_join_slots_ms[t]) + k_gate_margin_db, k_join_snr_db};
                for (std::size_t s = 0; s < test::count_of(snrs); ++s) {
                    cases.push_back(JoinCase{config, snrs[s], joins});
                }
            }
        }
    };
    add(k_join_bits, sizeof(k_join_bits) / sizeof(k_join_bits[0]), true);
    add(k_wait_bits, sizeof(k_wait_bits) / sizeof(k_wait_bits[0]), false);
    const std::vector<JoinResult> results = parallel_map<JoinResult>(cases.size(), [&](std::size_t i) {
        return run_join_case(cases[i], seed_of(k_test_l20, static_cast<std::uint32_t>(i), 0));
    });
    std::size_t all_starts = 0;
    std::size_t all_in_time = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const JoinCase& c = cases[i];
        const JoinResult& r = results[i];
        const std::string condition =
            format("%s, %zu bytes, receiver started at %zu random points after the preamble, usb %+.1f dB%s",
                   config_text(c.config).c_str(), k_join_bytes, r.starts, c.snr_db,
                   c.snr_db < k_join_snr_db ? " (gate + 3)" : "");
        std::string details;
        for (std::size_t n = 0; n < r.odd_locks.size(); ++n) details += (n == 0 ? "; locks: " : ", ") + r.odd_locks[n];
        if (!c.joins) {
            result("L20", condition,
                   format("locked %zu, bytes %zu, wrong %zu, extra %zu", r.locks, r.bytes, r.wrong_bytes,
                          r.extra_bytes) +
                       details,
                   "waits: 0 locked, 0 bytes", r.locks == 0 && r.bytes == 0);
            continue;
        }
        all_starts += r.starts;
        all_in_time += r.in_time;
        std::vector<double> times = r.lock_packages;
        std::sort(times.begin(), times.end());
        const double median = times[times.size() / 2];
        const std::size_t rank = static_cast<std::size_t>(std::ceil(k_join_rate * static_cast<double>(times.size())));
        const double p95 = times[rank - 1];
        const double rate = static_cast<double>(r.in_time) / static_cast<double>(r.starts);
        const bool pass = rate >= k_join_rate && r.wrong_bytes == 0 && r.extra_bytes == 0 && r.acausal_bytes == 0 &&
                          r.wrong_locks == 0;
        const auto when = [&](double packages) {
            return packages >= k_never ? std::string("never")
                                       : format("%.1f packages (%.0f ms)", packages, packages * r.package_ms);
        };
        result("L20", condition,
               format("joined %zu/%zu, within 6 packages %zu (%.0f%%); lock after median %s, 95%% of starts %s; wrong "
                      "T/N or not late locks %zu; bytes %zu released, %zu placed, wrong %zu, extra %zu, shifted "
                      "(acausal) %zu",
                      r.joined, r.starts, r.in_time, k_percent * rate, when(median).c_str(), when(p95).c_str(),
                      r.wrong_locks, r.bytes, r.matched, r.wrong_bytes, r.extra_bytes, r.acausal_bytes) +
                   details,
               "late_join lock within 6 packages in >= 95% of the starts, 0 wrong, 0 shifted (gate kept: slow cold "
               "joins are an open defect, spec 11.2)",
               pass);
    }
    result("L20", "every joining N, T and SNR together",
           format("%zu of %zu starts locked within 6 packages (%.1f%%)", all_in_time, all_starts,
                  all_starts == 0 ? 0.0 : k_percent * static_cast<double>(all_in_time) / all_starts),
           "report (the gate is per row)", true, Kind::report);
}

}  // namespace regression
}  // namespace unlimited
