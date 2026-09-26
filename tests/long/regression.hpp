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

// Long regression suites of v0.3 (spec 8.3-8.5, the long L5 and L20, the L19 passband checks): shared job runner,
// scoring and result lines. One pitch, a beep = 1, silence = 0, packages of N bits framed by twisted markers.
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
// Presets, receivers and the release gates of spec 4.1.
// ---------------------------------------------------------------------------------------------------------
EncoderConfig preset(Preset preset);  // 8000 Hz
// Preset::hf with T and N replaced (loopback::slot_config: the passband widened when the band needs it).
EncoderConfig slot_preset(double slot_ms, std::uint8_t bits);
double slot_ms_of(const EncoderConfig& config);
// The receiver whose window holds T: the fm profile below 8 ms, ssb to 64 ms, ssb with min_slot_ms 16 above.
DecoderConfig receiver_for(const EncoderConfig& config);
// Spec 4.1 release gate (key-down SNR in 2500 Hz) of a slot length: 4 ms +8.0, 8 ms +4.5, 16 ms +1.5, 32 ms -1.5,
// 64 ms -4.5, 128 ms -6.5 dB.
double gate_db(double slot_ms);
std::string preset_name(Preset preset);
std::string config_text(const EncoderConfig& config);  // "T=16 ms N=8 1500 Hz"
std::string receiver_text(const DecoderConfig& config);  // "receiver 8..64 ms, 300-2700 Hz"
std::string profile_name(Profile profile);

// ---------------------------------------------------------------------------------------------------------
// Jobs: a recording of transmissions separated by silence, a channel, one or more decoders.
// ---------------------------------------------------------------------------------------------------------
struct TxPlan {
    EncoderConfig config;
    std::vector<std::uint8_t> data;
    std::vector<std::vector<std::uint8_t> > packets;  // payloads when data is a sequence of packets
};

// The genie receiver of the C2 ablation (spec 4.2): known timing and tone, three reference lines.
enum GenieRule { genie_interpolated, genie_start_only, genie_fixed_level, genie_rules };

const double k_quiet_ms = 1500.0;  // receiver noise before, between and after transmissions

struct JobPlan {
    std::vector<TxPlan> transmissions;
    double lead_ms = k_quiet_ms;
    double gap_ms = k_quiet_ms;
    double tail_ms = k_quiet_ms;
    bool use_channel = true;
    sim::ChannelConfig channel;
    double peak_factor = 1.0;  // expected received peak over the key-down tone (fading, interferers)
    double rx_ppm = 0.0;       // receiver sample-clock error (channel.clock_ppm is the transmitter's)
    std::vector<DecoderConfig> decoders;
    bool genie = false;        // also run the genie receiver (usb or clean channel, no clock error)
    double cost() const;       // relative CPU cost, for scheduling
};

const std::size_t k_lost_reasons = static_cast<std::size_t>(LostReason::unsupported) + 1;

struct Outcome {
    loopback::Score score;
    std::size_t cold_joins = 0;            // late-join locks that count bytes from the join (spec 3.12, V7)
    std::size_t acausal_bytes = 0;         // bytes mapped to a package whose STOP had not been received yet
    std::size_t misplaced_segments = 0;    // locks whose bytes sit at a wrong byte_index (a shift, spec 3.13)
    std::size_t misplaced_bytes = 0;
    std::size_t lost_reasons[k_lost_reasons] = {};
    std::size_t max_wrong_run = 0;         // consecutive wrong or unmapped bytes (F6)
    std::size_t transmissions = 0;
    std::size_t locked_transmissions = 0;  // a `locked` with the sent T (3 %) and N for the transmission (A3)
    std::size_t wrong_locks = 0;           // a `locked` with another T or N than the transmission on air
    std::size_t stray_locks = 0;           // a `locked` before the first transmission
    std::size_t late_locks = 0;            // a right `locked` more than 4 T after its transmission ended
    std::size_t packets_sent = 0;
    std::size_t packets_ok = 0;
    std::size_t packets_bad = 0;           // CRC-valid packets that were never sent (F5)
    std::vector<float> snr_db;             // per byte event (A4)
    double airtime = 0.0;                  // START of package 0 to the end of the last STOP, all transmissions
    double locked_airtime = 0.0;           // part of it between `locked` and `lost`/`end` (C3)
    double slot_error_sum = 0.0;           // |measured slot_ms / true - 1| over correct byte events (L5)
    double worst_slot_error = 0.0;
    std::size_t slot_events = 0;
    double latency_sum = 0.0;              // release after the end of the byte's STOP slot, in slots
    double latency_max = 0.0;
    std::size_t genie_bits = 0;
    std::size_t genie_errors[genie_rules] = {};
    double delivered() const;              // matched / sent
    double correct() const;                // (matched - wrong) / sent
    double mean_slot_error() const;
    double genie_ber(GenieRule rule) const;
};

void merge(Outcome& into, const Outcome& from);

// A BER or ratio gate needs released bits (spec 8 conventions): at least half of the bytes delivered.
const double k_min_delivered = 0.5;
bool delivered(const Outcome& outcome);

TxPlan random_tx(const EncoderConfig& config, std::size_t bytes, std::uint32_t seed);
TxPlan packet_tx(const EncoderConfig& config, std::size_t bytes, std::uint32_t seed);  // >= bytes of packets

// Runs one job; one Outcome per decoder.
std::vector<Outcome> run_job(const JobPlan& job);

// Scores a decoder's events against a recording as the decoder received it, without clock error: `shift` samples
// are added to every position of the recording (the channel delay, less the samples a late receiver missed).
Outcome evaluate_recording(const loopback::Recording& recording, const loopback::Capture& capture, double shift);

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

// int16 -> sim::Channel -> int16, scaled so that key-down peaks times peak_factor plus 5 sigma of noise stay
// below full scale; clock_ppm changes the length.
std::vector<std::int16_t> apply_channel(const std::vector<std::int16_t>& samples, sim::ChannelConfig config,
                                        std::int16_t amplitude, double peak_factor);
// Channel delay (samples) of a mode at 8 kHz: the energy centroid of a beep (loopback), cached.
double channel_delay(sim::Mode mode);

double snr_for_fm_cnr(double cnr_db, const sim::ChannelConfig& config);  // CNR in the FM IF -> snr_db
double snr_for_am_cnr(double cnr_db, const sim::ChannelConfig& config);  // CNR in the AM IF -> snr_db
double amplitude_of_db(double db);
// a / b where anything over 0 counts as infinitely worse and 0 / 0 as equal.
double ratio(double a, double b);

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
std::string lost_text(const Outcome& outcome);  // "lost-ev 2 (gone 2, alias 0, preamble 0, unsupported 0)"
std::string lock_text(const Outcome& outcome);  // "locks 20/20 tx (+0 wrong T/N, 0 stray)"

// Upper 95 % confidence bound of a rate after `errors` in `trials` (Poisson; 3/n with no error).
double upper_95(std::size_t errors, double trials);

// F5 and F6 ledgers, filled by the A, C, F, L5 and L19 tests and checked by the F5/F6 tests.
void ledger_packets(const std::string& where, const Outcome& outcome);
void ledger_runs(const std::string& where, const Outcome& outcome, bool at_gate_plus_3);
void print_summary();

// Suite entry points (one TEST each, registered in order in regression_suite.cpp).
void test_l5_clock();
void test_a1_smart_line();
void test_a1_n_sweep();
void test_a2_fixed_line();
void test_a3_acquisition();
void test_a4_snr_report();
void test_c1_ccir_good();
void test_c2_ccir_moderate();
void test_c3_ccir_poor();
void test_c4_flat_rayleigh();
void test_c5_qsb();
void test_c6_qrn();
void test_c7_agc();
void test_c8_carrier();
void test_c9_cw();
void test_c10_fm();
void test_c11_am();
void test_c12_flutter();
void test_c13_agc_fading();
void test_c14_sideband_shift_fading();
void test_c15_fm_emphasis_mismatch();
void test_l19_passband();
void test_l20_cold_late_join();
void test_f1_noise();
void test_f2_carrier();
void test_f3_cw();
void test_f4_speech();
void test_f7_package_learning();
void test_f5_packets();
void test_f6_runs();

}  // namespace regression
}  // namespace unlimited
