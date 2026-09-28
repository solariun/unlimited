#pragma once

#include "channel.hpp"
#include "support/loopback.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <vector>

// The long regression suite of v1.0 (spec 4, 8): the essential families A1/A2 (AWGN per speed, the 70 % line against
// the adaptive one), A3 (acquisition from byte 0), S1 (short transmissions), L5 (clock error), L19 (passband and
// shift), C (channels) and F (false locks), and the integrity row over all of them. One byte per window of 10 slots.
// Its gates are provisional (spec 4): they are measured and reported, and changed only by Gustavo's decision.
namespace unlimited {
namespace regression {

// ---------------------------------------------------------------------------------------------------------
// Parallel map over [0, count) on every core. Work items must be independent; results keep index order, so
// the output is deterministic whatever the scheduling. `order` (optional) is the start order (longest first).
// ---------------------------------------------------------------------------------------------------------
std::size_t worker_count();

template <typename Result, typename Work>
std::vector<Result> parallel_map(std::size_t count, Work work, const std::vector<std::size_t>& order = {}) {
    std::vector<Result> results(count);
    std::vector<std::exception_ptr> errors(count);
    std::atomic<std::size_t> next(0);
    const auto worker = [&]() {
        for (std::size_t n = next++; n < count; n = next++) {
            const std::size_t i = order.empty() ? n : order[n];
            try {
                results[i] = work(i);
            } catch (...) {
                errors[i] = std::current_exception();
            }
        }
    };
    std::vector<std::thread> threads;
    const std::size_t workers = std::min(worker_count(), count);
    for (std::size_t t = 0; t < workers; ++t) threads.emplace_back(worker);
    for (std::size_t t = 0; t < threads.size(); ++t) threads[t].join();
    for (std::size_t i = 0; i < count; ++i) {
        if (errors[i]) std::rethrow_exception(errors[i]);
    }
    return results;
}

// ---------------------------------------------------------------------------------------------------------
// Speeds, senders, receivers and the provisional gates of spec 4.
// ---------------------------------------------------------------------------------------------------------
const std::size_t k_speed_count = 5;
extern const float k_speeds[k_speed_count];  // 1, 3, 6, 12, 25 bytes/s (spec 1.3)

// The provisional A1 gate of a speed (spec 4): v0.3's gate at the same T, key-down SNR in 2500 Hz: 1 byte/s -6.5,
// 3 -1.7, 6 +1.3, 12 +4.3, 25 +8.0 dB.
double gate_db(float speed);
// 8000 Hz at `speed` bytes/s on tone_hz; the passband widened to 100..3000 Hz when the band needs it.
EncoderConfig speed_config(float speed, std::uint16_t tone_hz = k_default_tone_hz);
// The receiver of that sender: its speed and passband, the fixed 70 % line unless adaptive.
DecoderConfig receiver_for(const EncoderConfig& config, bool adaptive = false);
std::string speed_text(float speed);  // "6 bytes/s"
double slot_ms_of(const EncoderConfig& config);

// ---------------------------------------------------------------------------------------------------------
// Jobs: a recording of transmissions separated by silence, a channel, one or more decoders.
// ---------------------------------------------------------------------------------------------------------
struct TxPlan {
    EncoderConfig config;
    std::vector<std::uint8_t> data;
};

const double k_quiet_ms = 1500.0;  // receiver noise before, between and after transmissions

struct JobPlan {
    std::vector<TxPlan> transmissions;
    double lead_ms = k_quiet_ms;  // raised to 15 slots: the decoder needs a silent window before its first START
    double gap_ms = k_quiet_ms;
    double tail_ms = k_quiet_ms;
    bool use_channel = true;
    sim::ChannelConfig channel;
    double peak_factor = 1.0;  // expected received peak over the key-down tone (fading, interferers)
    std::vector<DecoderConfig> decoders;
    double cost() const;       // relative CPU cost, for scheduling
};

struct Outcome {
    loopback::Score score;
    std::size_t transmissions = 0;
    std::size_t shifted_segments = 0;  // locks whose bytes sit at a wrong byte_index (a whole-window shift)
    std::size_t shifted_bytes = 0;
    std::size_t lost_framing = 0;      // `lost` events (framing)
    double slot_error_sum = 0.0;       // |measured slot_ms / heard T - 1| over right byte events (L5)
    double worst_slot_error = 0.0;
    std::size_t slot_events = 0;
    double delivered() const;          // matched / sent
    double mean_slot_error() const;
};

void merge(Outcome& into, const Outcome& from);

// A BER gate needs released bits: at least half of the bytes delivered.
const double k_min_delivered = 0.5;
bool delivered(const Outcome& outcome);
// Spec 0.8 G5, kept: "0 bit errors" is BER <= 1e-4 with 0 extra and 0 shifted bytes.
const double k_near_zero_ber = 1e-4;
bool near_zero_errors(const Outcome& outcome);
bool integrity(const Outcome& outcome);  // 0 extra and 0 shifted bytes

TxPlan random_tx(const EncoderConfig& config, std::size_t bytes, std::uint32_t seed);

// Runs one job; one Outcome per decoder.
std::vector<Outcome> run_job(const JobPlan& job);
// Runs jobs of several points in parallel and merges them per point and decoder.
std::vector<std::vector<Outcome> > run_points(const std::vector<JobPlan>& jobs, const std::vector<std::size_t>& point,
                                              std::size_t points);

// Transmission count so that `bits` data bits are sent in transmissions of `bytes` bytes.
std::size_t transmissions_for(double bits, std::size_t bytes);
// Airtime of one transmission (spec 2.4) plus the gap after it, and how many go into one job: about six minutes
// of audio per job (memory, load balance), and at least eight jobs per point when there are enough.
double transmission_seconds(const EncoderConfig& config, std::size_t bytes, double gap_ms);
std::size_t transmissions_per_job(double transmission_s, std::size_t count);
std::uint32_t data_seed(std::uint32_t job_seed, std::size_t transmission);
std::uint32_t seed_of(std::uint32_t test, std::uint32_t point, std::uint32_t job);
// Jobs of `count` transmissions of `bytes` bytes from `make` (a sender per transmission), `point` recorded per job.
void add_jobs(std::vector<JobPlan>& jobs, std::vector<std::size_t>& points, std::size_t point, const JobPlan& shape,
              const EncoderConfig& config, std::size_t count, std::size_t bytes, std::uint32_t test);

// int16 -> sim::Channel -> int16, scaled so that key-down peaks times peak_factor plus 5 sigma of noise stay
// below full scale; clock_ppm changes the length.
std::vector<std::int16_t> apply_channel(const std::vector<std::int16_t>& samples, sim::ChannelConfig config,
                                        std::int16_t amplitude, double peak_factor);
double channel_delay(sim::Mode mode);  // samples at 8 kHz: the energy centroid of a beep (loopback), cached
double snr_for_fm_cnr(double cnr_db, const sim::ChannelConfig& config);  // CNR in the FM IF -> snr_db
double snr_for_am_cnr(double cnr_db, const sim::ChannelConfig& config);  // CNR in the AM IF -> snr_db
sim::ChannelConfig usb_channel(double snr_db, double offset_hz = 0.0);

// ---------------------------------------------------------------------------------------------------------
// Result lines: "RESULT | id | condition | measured | gate | PASS/FAIL/REPORT". A failing gate fails the
// running test; report-only lines never do. Every line is kept for the final summary.
// ---------------------------------------------------------------------------------------------------------
enum class Kind { gate, report };

bool result(const std::string& id, const std::string& condition, const std::string& measured,
            const std::string& gate, bool pass, Kind kind = Kind::gate);
void note(const std::string& text);
std::string format(const char* pattern, ...);
std::string ber_text(const Outcome& outcome);   // "BER 1.2e-05 (204800 bits), delivered 99.9%, loss ..."
std::string lock_text(const Outcome& outcome);  // "locked 20/20 tx from byte 0, 0 lost"
double upper_95(std::size_t errors, double trials);  // upper 95 % bound of a rate (Poisson; 3/n with none)

// The integrity ledger: every A, S, L and C row adds its extra and shifted bytes; the integrity test checks them.
void ledger(const std::string& where, const Outcome& outcome);
void print_summary();

// Suite entry points (one TEST each, registered in order in regression_suite.cpp).
void test_a1_awgn();
void test_a3_acquisition();
void test_s1_short();
void test_l5_clock();
void test_l19_passband();
void test_c_channels();
void test_f1_noise();
void test_f2_carrier();
void test_f3_cw();
void test_f4_speech();
void test_integrity();

}  // namespace regression
}  // namespace unlimited
