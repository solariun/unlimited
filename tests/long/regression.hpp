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

// Long regression suites (spec 8.3-8.5 and the long L5), v0.2: shared job runner, scoring and result lines.
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
// Jobs: a recording of transmissions separated by silence, a channel, one or more decoders.
// ---------------------------------------------------------------------------------------------------------
struct TxPlan {
    EncoderConfig config;
    std::vector<std::uint8_t> data;
    std::vector<std::vector<std::uint8_t> > packets;  // payloads when data is a sequence of packets
};

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
    bool genie = false;        // genie slot decisions on the received audio (usb/lsb, no clock error)
    double cost() const;       // relative CPU cost, for scheduling
};

// Byte-level score of one decoder. Byte events map to sent bytes by frame_index * B + index, one frame
// offset per lock segment (found from the release times, then refined by matching).
struct Score {
    std::size_t bytes_sent = 0;
    std::size_t bytes_released = 0;
    std::size_t matched = 0;      // released bytes mapped to a sent byte
    std::size_t wrong_bytes = 0;
    std::size_t bit_errors = 0;
    std::size_t lost_bytes = 0;   // sent bytes never released
    std::size_t extra_bytes = 0;  // released bytes that map to nothing (or a duplicate)
    std::size_t locks = 0;
    std::size_t late_joins = 0;
    std::size_t lost_events = 0;
    std::size_t ends = 0;
    std::size_t flywheel_bytes = 0;
    std::size_t erasure_bytes = 0;
    std::size_t mode_memory_bytes = 0;
    double ber() const;
    double loss() const;
};

const std::size_t k_lost_reasons = static_cast<std::size_t>(LostReason::unsupported_mode) + 1;

struct Outcome {
    Score score;
    std::size_t frames_sent = 0;
    std::size_t frames_delivered = 0;      // every byte of the data frame released and mapped
    std::size_t max_wrong_run = 0;         // consecutive wrong or unmapped bytes (F6)
    std::size_t transmissions = 0;
    std::size_t locked_transmissions = 0;  // a `locked` with the sent mode and orientation during it (A3')
    std::size_t wrong_mode_locks = 0;      // a `locked` during a transmission with another k, N, spacing, T or side
    std::size_t stray_locks = 0;           // a `locked` outside every transmission
    std::size_t header_transmissions = 0;  // TRACK reached with the sent mode: a `slot` event of that mode
    std::size_t wrong_header_transmissions = 0;  // a `slot` event of another mode during the transmission
    std::size_t lost_reasons[k_lost_reasons] = {};
    std::size_t packets_sent = 0;
    std::size_t packets_ok = 0;
    std::size_t packets_bad = 0;           // CRC-valid packets that were never sent (F5)
    std::vector<float> snr_db;             // per byte event (A4)
    double airtime = 0.0;                  // frame time of all transmissions, samples
    double locked_airtime = 0.0;           // part of it between `locked` and `lost`/`end`
    std::size_t latency_events = 0;        // correct byte events (latency statistics)
    double latency_sum = 0.0;              // release time after the STOP centre of the frame, in slots
    double latency_max = 0.0;
    std::size_t genie_bits = 0;            // genie receiver: known timing and grid, argmax (A5)
    std::size_t genie_errors = 0;
    double frames_ratio() const;
    double genie_ber() const;
};

void merge(Outcome& into, const Outcome& from);

// A BER or ratio gate needs released bits: it also requires half of the frames delivered, so a decoder that
// releases nothing cannot pass on "BER 0 (0 bits)".
const double k_min_delivered = 0.5;
bool delivered(const Outcome& outcome);

TxPlan random_tx(const EncoderConfig& config, std::size_t bytes, std::uint32_t seed);
TxPlan packet_tx(const EncoderConfig& config, std::size_t bytes, std::uint32_t seed);  // >= bytes of packets

// A mode that is not a preset: the base preset's tune/sync/lead-in with T, k, N, spacing and side replaced, and
// f_ref placed by the HF rule (band centred on 1500 Hz: f_ref = ceil(1500 + W / 2) below, floor(1500 - W / 2)
// above).
EncoderConfig mode_config(Preset base, std::uint32_t slot_ms, std::uint8_t bits_per_peak, std::uint8_t data_slots,
                          Spacing spacing, GridSide side);
std::string mode_name(const EncoderConfig& config);  // "T32 k5 N8 std below f_ref 2132"

// Transmitted audio frequency of data tone n and of header tone h (spec 1.3).
double data_tone_hz(const EncoderConfig& config, double tone);
double header_tone_hz(const EncoderConfig& config, double tone);
double span_hz(const EncoderConfig& config);  // f_ref to the farthest data or header tone

// Runs one job; one Outcome per decoder.
std::vector<Outcome> run_job(const JobPlan& job);

// Runs jobs of several points in parallel and merges them per point and decoder.
std::vector<std::vector<Outcome> > run_points(const std::vector<JobPlan>& jobs, const std::vector<std::size_t>& point,
                                              std::size_t points);

// Transmission count so that `bits` data bits are sent in transmissions of `bytes` bytes.
std::size_t transmissions_for(double bits, std::size_t bytes);

// Airtime of one transmission plus the gap after it, and how many go into one job: about six minutes of
// audio per job (memory, load balance), and at least eight jobs per point when there are enough.
double transmission_seconds(const EncoderConfig& config, std::size_t bytes, double gap_ms);
std::size_t transmissions_per_job(double transmission_s, std::size_t count);
std::uint32_t data_seed(std::uint32_t job_seed, std::size_t transmission);

std::string profile_name(Profile profile);

// int16 -> sim::Channel -> int16, scaled so that key-down peaks times peak_factor plus 5 sigma of noise stay
// below full scale; clock_ppm changes the length.
std::vector<std::int16_t> apply_channel(const std::vector<std::int16_t>& samples, sim::ChannelConfig config,
                                        std::int16_t amplitude, double peak_factor);

// Channel delay (samples) of the usb or lsb path at 8 kHz, for the genie receiver.
std::size_t ssb_delay_samples(sim::Mode mode);

double snr_for_fm_cnr(double cnr_db, const sim::ChannelConfig& config);
double snr_for_am_cnr(double cnr_db, const sim::ChannelConfig& config);

std::uint32_t seed_of(std::uint32_t test, std::uint32_t point, std::uint32_t job);

// ---------------------------------------------------------------------------------------------------------
// Result lines: "RESULT | id | condition | measured | gate | PASS/FAIL/REPORT". A failing gate fails the
// running test; report-only lines never do. Every line is kept for the final summary.
// ---------------------------------------------------------------------------------------------------------
enum class Kind { gate, report };

bool result(const std::string& id, const std::string& condition, const std::string& measured,
            const std::string& gate, bool pass, Kind kind = Kind::gate);
void note(const std::string& text);
std::string format(const char* pattern, ...);
std::string ber_text(const Outcome& outcome);  // "BER 1.2e-05 (204800 bits), frames 99.5%, loss 0.10%, ..."
std::string lost_text(const Outcome& outcome); // "lost: gone 2, alias 0, ..."

// Upper 95 % confidence bound of a rate after `errors` in `trials` (Poisson; 3/n with no error).
double upper_95(std::size_t errors, double trials);

// F5 and F6 ledgers, filled by the A, C, F and L5 tests and checked by the F5/F6 tests.
void ledger_packets(const std::string& where, const Outcome& outcome);
void ledger_runs(const std::string& where, const Outcome& outcome, bool at_gate_plus_3);
void print_summary();

// Suite entry points (one TEST each, registered in order in regression_suite.cpp).
void test_a1_integrated();
void test_a3_acquisition();
void test_a4_snr_report();
void test_a5_genie();
void test_l5_clock();
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
void test_c14_lsb_offset_fading();
void test_c15_fm_emphasis_mismatch();
void test_f1_noise();
void test_f2_carrier();
void test_f3_cw();
void test_f4_speech();
void test_f5_packets();
void test_f6_runs();
void test_f7_header_statistics();

}  // namespace regression
}  // namespace unlimited
