#pragma once

#include "unlimited/protocol.hpp"

// Internal: the decoder's building blocks (spec 3, 5.3). Public only so that decoder.hpp can hold them by value and
// the tests can reach them; not part of the frozen API.

namespace unlimited {

static const uint32_t k_decoder_rate_hz = 8000;
static const uint8_t k_blocks_per_min_slot = 8;
// The history holds the longest package, its END check and the search margins at the slowest accepted T
// (spec 3.3, 3.15): (N_max + 5) slots of k_speed_span * k_blocks_per_min_slot blocks, plus guard cells.
static const uint8_t k_history_margin_slots = 5;
static const uint16_t k_history_slots = k_max_bits_per_package + k_history_margin_slots;
static const uint16_t k_history_guard_cells = 32;
static const uint16_t k_history_cells =
    k_history_slots * k_speed_span * k_blocks_per_min_slot + k_history_guard_cells;
static const uint8_t k_mix_shift = 10;
static const uint16_t k_rebase_blocks = 8192;
static_assert(k_rebase_blocks > k_history_cells, "rebase only what the history no longer holds");
static const uint8_t k_candidate_scales = 7;
static const uint8_t k_candidate_scale_blocks[k_candidate_scales] = {3, 4, 6, 8, 11, 16, 22};
// Detections of one marker at several scales agree within a block; distinct markers are at least T_min
// (8 blocks) apart. Merging within the T_min half-window keeps every marker of a T_min train. A detection merges only
// into the newest entry, so one that arrives after another marker's takes an entry of its own: duplicates of one
// marker can fill the ring (the lead of the slow cold joins, spec 11.2).
static const float k_candidate_merge_blocks = 0.35f * k_blocks_per_min_slot;

static const float k_mixer_gain = 32.0f;          // Q15 table >> k_mix_shift
static const float k_cic_overlap = 1.0f / 3.0f;   // n_eff = (M - 1/3) * B for a window of M blocks
static const float k_g_slot = 0.9394f;            // mean Tukey envelope over the central 0.75 T
static const float k_g_marker = 0.8355f;          // mean |w r| over each 0.35 T marker half
static const float k_min_noise_variance = 1.0f / 12.0f;  // int16 quantization, per sample
static const uint8_t k_audit_scale = 15;          // audit evidence stored as int8 in 1/15 units

namespace dsp {

struct Complex {
    float re;
    float im;
};

class Nco {
public:
    Nco();
    void set_frequency(float hz);
    void adjust_frequency(float delta_hz);
    float frequency() const;
    void next(int16_t& cos_q15, int16_t& sin_q15);

private:
    uint32_t phase_;
    uint32_t step_;
};

class Cic2 {
public:
    Cic2();
    void reset();
    void push(int32_t re, int32_t im);
    void dump(uint8_t block_samples, int32_t& re, int32_t& im);

private:
    uint32_t integrator1_[2];
    uint32_t integrator2_[2];
    uint32_t comb1_[2];
    uint32_t comb2_[2];
};

class QuantileTracker {
public:
    QuantileTracker();
    void reset(float initial);  // initial mean estimate; 0 = take the first input
    void push(float value);
    float mean_estimate() const;
    bool primed() const;

private:
    float quantile_;
    uint16_t count_;
};

// Mean power of noise windows: a running mean for the first inputs, then an exponential average over about
// 64. A value above ten times the estimate counts as ten times, so an impulse the blanker missed moves it
// only a little. Unlike a quantile it stays unbiased when the noise is impulsive (FM clicks).
class NoiseTracker {
public:
    NoiseTracker();
    void reset(float initial);  // initial mean, weighted as a few inputs; 0 = take the first inputs
    void push(float value);
    float mean_estimate() const;

private:
    float mean_;
    uint16_t count_;
};

// Two-stage block blanker (spec 3.3): a spike stage and an out-of-bin residual stage with a run limit.
class ImpulseBlanker {
public:
    static const uint8_t k_latency = 2;  // push_block() for block k returns the decision for block k - k_latency

    ImpulseBlanker();
    void reset();
    bool push_block(float energy, float in_bin_energy);

private:
    static const uint8_t k_delay = 2 * k_latency + 1;

    float nearby_tone() const;
    bool spike(float floor) const;
    bool residual_trigger(float residual) const;

    float energy_[k_delay];
    float in_bin_[k_delay];
    float residual_average_;
    QuantileTracker floor_;
    uint8_t run_;
    uint8_t fill_;
    bool pending_;
};

// Wrapping int32 prefix sums of the CIC-2 block outputs (stored uint32), k_history_cells deep, plus a blank bitmap.
class PrefixHistory {
public:
    PrefixHistory();
    void reset();  // forgets the contents; the block count continues
    void push(int32_t re, int32_t im, bool blanked);
    uint32_t end_block() const;  // one past the newest block
    bool window(uint32_t origin_block, float from, float to, Complex& sum) const;  // divided by k_mixer_gain
    bool any_blanked(uint32_t origin_block, float from, float to) const;

private:
    bool holds(uint32_t block) const;  // prefix P[block] is stored
    uint16_t cell(uint32_t block) const;
    bool blanked_bit(uint16_t cell) const;

    uint32_t re_[k_history_cells];
    uint32_t im_[k_history_cells];
    uint8_t blanked_[(k_history_cells + 7) / 8];
    uint32_t sum_re_;
    uint32_t sum_im_;
    uint32_t end_block_;
    uint16_t head_;  // cell of P[end_block_]
    uint16_t fill_;  // stored prefixes
};

// Tone search (spec 3.6): up to 49 Goertzel bins 50 Hz apart over 160-sample blocks, plus a guard bin on each side,
// with half-bin powers between them for the lock, a slow floor and a recent floor (the larger one gates locks), and
// the block-to-block and half-block phases for the tone estimate.
class ToneSearch {
public:
    static const uint8_t k_max_lock_bins = 49;               // the search range, 50 Hz apart
    static const uint8_t k_max_bins = k_max_lock_bins + 2;  // and a guard bin on each side, never locked on
    static const uint16_t k_block_samples = 160;
    static const uint8_t k_long_run_blocks = 8;  // 160 ms

    ToneSearch();
    void configure(uint16_t min_hz, uint16_t max_hz);  // bins on the multiples of 50 Hz inside; also clears bans
    void reset();                                       // statistics only; bans and strikes stay
    void interrupt();                                   // samples were skipped: drop the block, end the lock's run
    bool push(int16_t sample);                          // true when a search block ended
    bool candidate(float& tone_hz) const;
    bool steady() const;                                // the lock sees a steady tone (a tune tone)
    float floor() const;                                // noise power per bin, same units as the bin powers
    float recent_floor() const;                         // the same over the last few blocks (follows level steps)
    float onset_floor() const;                          // floor before the tone of the current lock appeared
    bool masked(float tone_hz) const;                   // steady carrier
    bool present(float tone_hz) const;                  // in the fast average now, as strong as a fast lock needs
    void ban(float tone_hz, uint16_t base_blocks);      // base_blocks << min(strikes, 3)
    void exclude(float tone_hz, float sideband_hz);     // no lock near this tone or its train's lines
    void clear_exclusion();
    // A tone steady over at least min_products (>= 3) block products turned into a marker train.
    bool train_onset(float& tone_hz, uint8_t min_products) const;
    bool long_run() const;  // the lock has held k_long_run_blocks blocks (160 ms)

private:
    enum class Lock : uint8_t { none, fast, slow };

    static const uint8_t k_phase_bins = 3;  // best bin and its two neighbours
    static const uint8_t k_half_bins = k_max_bins - 1;

    void end_block();
    void update_bins();
    void update_floor();
    void update_bans();
    void update_lock();
    void update_phase(bool continued, bool resumed, uint8_t bin);
    void update_breaks(const Complex& product, const Complex& half, float magnitude, float break_cosine);
    Complex bin_output(uint8_t bin) const;
    Complex dft_output(uint8_t bin) const;
    Complex half_product(uint8_t bin) const;
    bool half_offset(const Complex& half, float weight, float& offset) const;
    float block_power(uint8_t bin) const;
    float block_level(uint8_t bin) const;
    float level(uint8_t bin) const;
    bool eligible(uint8_t bin) const;
    bool local_peak(const float* power, uint8_t bin) const;
    bool steady_carrier(uint8_t bin) const;
    float excess_variance(uint8_t bin) const;
    float power_offset(const float* power) const;
    float coherence() const;
    float pattern_fit(const float* power, float offset) const;
    float steady_offset(const float* power, const Complex& product, const Complex& half, float half_weight,
                        float fallback) const;
    float estimate_tone() const;
    bool inside(float tone_hz) const;
    int16_t bin_of(float tone_hz) const;

    int16_t coeff_q14_[k_max_bins];
    int32_t s1_[k_max_bins];
    int32_t s2_[k_max_bins];
    float fast_[k_max_bins];
    float fast_half_[k_half_bins];    // same average of the half-bin power between bins b and b + 1
    float slow_[k_max_bins];
    float slow_square_[k_max_bins];
    uint16_t ban_[k_max_bins];
    uint8_t strikes_[k_max_bins];
    Complex previous_[k_phase_bins];  // bin outputs of best_bin_ - 1 .. best_bin_ + 1, previous block
    Complex middle_[k_phase_bins];    // the same bins in the middle of this block (first half only)
    Complex phase_sum_;               // sum of X_n conj(X_n-1) of the best bin over the stable run
    Complex phase_square_sum_;        // same with the phase doubled
    float phase_weight_;              // sum of |X_n conj(X_n-1)|
    Complex half_sum_;                // sum of B_n conj(A_n), second half against first half of each block
    float half_weight_;
    Complex steady_sum_;              // strong products of the run before its first break
    Complex steady_half_sum_;         // their half-block products
    float steady_weight_;             // sum of their magnitudes
    float steady_half_weight_;
    float steady_tone_hz_;            // tone estimate from the steady part of the run, 0 if none
    float lock_level_;                // level() of the locked bin over the floor
    float floor_;
    float recent_floor_;
    float previous_floor_;
    float onset_floor_;
    uint16_t min_hz_;                 // the range configured: tones estimated outside it are no candidates
    uint16_t max_hz_;
    uint16_t first_hz_;
    uint16_t since_strike_;
    int16_t excluded_bin_;
    int16_t excluded_span_;
    uint8_t bins_;
    uint8_t best_bin_;
    uint8_t middle_bin_;              // best_bin_ when middle_ was taken
    uint8_t stable_blocks_;
    uint8_t steady_products_;
    uint8_t breaks_;                  // strong products of the run off its steady phase
    uint8_t onset_products_;          // steady products before the first break
    uint8_t fast_gap_;                // search blocks since the last fast lock
    Lock lock_;
    uint16_t sample_index_;
    uint32_t blocks_;
};

class FineAfc {
public:
    static const uint8_t k_bins = 65;

    FineAfc();
    void configure(float input_rate_hz);  // 125 Hz by default
    void reset();
    void push(const Complex& decimated);
    uint16_t inputs() const;
    bool offset(float& tone_offset_hz, bool wide) const;  // wide: +-30 Hz, otherwise +-16.7 Hz
    static float bin_hz(uint8_t bin);  // frequency of a bin on the squared signal
    static float bin_lambda(uint8_t bin);

private:
    Complex accumulator_[k_bins];
    Complex rotator_[k_bins];
    uint16_t count_;
};

struct Candidate {
    uint32_t block;
    float fraction;
    float q;
    uint8_t scale;
};

class CandidateList {
public:
    static const uint8_t k_size = 16;

    CandidateList();
    void reset();
    bool add(const Candidate& candidate);  // false when merged into a stronger newest entry
    uint8_t count() const;
    const Candidate& newest(uint8_t age) const;

private:
    Candidate items_[k_size];
    uint8_t head_;
    uint8_t count_;
};

// Alias audit (spec 3.11): 2N + 1 positions per package, evidence clamped by the caller to [-4, 8] and stored as
// int8 in 1/k_audit_scale units. Positions are filled as they are measured; next_package() commits them.
class AuditRing {
public:
    static const uint8_t k_max_positions = 2 * k_max_bits_per_package + 1;
    static const uint8_t k_packages = 4;

    AuditRing();
    void reset(uint8_t positions);                 // clears; packages then have `positions` positions (2N + 1)
    void set(uint8_t position, float evidence);   // position 0..positions-1 of the package being measured
    void next_package();                           // commits the package being measured and starts a clear one
    float max_evidence() const;                    // max over positions of the sum over committed packages
    uint8_t packages() const;                      // committed packages, at most k_packages
    uint8_t positions() const;

private:
    int8_t evidence_[k_packages + 1][k_max_positions];
    uint8_t positions_;
    uint8_t head_;  // row of the package being measured
    uint8_t count_;
};

struct FlipMeasure {
    float q;
    float q_balanced;
    float kappa;
    float amplitude;      // |before - after| on the marker crest scale
    float steady;         // |before + after| on the same scale: a carrier continuous across the centre
    float energy;         // (|before|^2 + |after|^2) / (2 noise_energy): mean half-window energy over the noise
    float phase_step;
    float position;
    bool valid;
};

// Package learning in PREAMBLE (spec 3.8). It is fed the grid index of every marker found after the sync train. A
// gap of one slot continues the train; the first longer gap d gives a candidate N = d - 1 (and, for the first
// package, the reading N = d - 2 with a faded last train marker); the next gap confirms a candidate when it is
// N + 1 again. Grid indices count slots from the train's anchor.
enum class LearnStep : uint8_t {
    train,        // a gap of 1: the train goes on, any candidate is dropped
    candidate,    // a new candidate from this gap
    confirmed,    // this gap repeats a candidate: N is learnt
    rejected,     // the gap contradicts the candidate and is too long to be a new one
    unsupported   // two equal gaps longer than k_max_bits_per_package + 1: N is above this build's cap
};

// No bound on package 0's START (the train's length unknown).
const int32_t k_no_start = -0x7FFFFFFF;

class PackageLearner {
public:
    PackageLearner();
    // Newest train marker L; gaps of 1 seen in the train; the earliest START the train's length allows.
    void reset(int32_t train_index, uint8_t train_ones, int32_t min_start = k_no_start);
    LearnStep push(int32_t index);  // a marker at this grid index (after the previous one)
    // A train marker after faded ones (known from the train's length or from its carrier); adjacent: a gap of 1.
    void extend_train(int32_t index, bool adjacent);
    uint8_t bits() const;             // confirmed N, else the candidate from the newest gap, 0 = none
    uint8_t faded_bits() const;       // the first package's faded-START reading (bits() - 1), 0 = none
    bool faded_start() const;         // the confirmed candidate was the faded-START reading
    bool start_faded() const;         // the candidate's own START is a faded marker at min_start (the only reading)
    int32_t candidate_start() const;  // grid index of the confirmed (or newest) candidate's START
    int32_t start_base() const;       // package 0 starts here or later: max(L, min_start)
    int32_t first_start() const;      // grid index of package 0's START (fewest-fades rule), once confirmed
    bool start_exact() const;         // first_start() can be exact: always for N >= 2, and for N = 1 when the first
                                      // marker after L came within 4 slots of start_base() (spec 3.8 step 6, V4)
    uint8_t rejections() const;       // candidates dropped so far
    uint8_t train_ones() const;
    int32_t train_index() const;      // L
    int32_t min_start() const;
    int32_t first_marker() const;     // g1: the first marker after L (0: none yet)
    void shift_candidate(int32_t slots);  // the confirmed candidate's START recounted by `slots`

private:
    int32_t train_index_;      // L
    int32_t min_start_;
    int32_t first_marker_;     // the first marker after L, 0 = none yet
    int32_t last_marker_;
    int32_t candidate_start_;
    uint8_t candidate_bits_;
    uint8_t faded_bits_;
    uint8_t unsupported_gap_;  // a first gap above the cap, waiting for its repeat
    uint8_t train_ones_;
    uint8_t rejections_;
    bool faded_start_;
    bool start_faded_;
    bool confirmed_;
};

// Effective sample count of a window of `blocks` CIC-2 blocks (noise variance of its sum / sigma^2).
float noise_samples(float blocks, uint8_t block_samples);

// Flip statistics of the half-window sums before/after a centre (spec 3.4); noise_energy = sigma^2 * n_eff(W).
FlipMeasure flip_measure(const Complex& before, const Complex& after, float noise_energy, float half_samples);

// Vertex offset of the parabola through (-1, left), (0, centre), (1, right), clamped to +-0.5.
float parabolic_offset(float left, float centre, float right);

// The smart line (spec 3.10): equal-likelihood fraction of the reference for amplitude SNR a^2 (Rician against
// Rayleigh), 3 fixed-point iterations from 0.6, clamped to 0.50..0.75; 0.75 when a^2 <= 0.
float equal_likelihood_ratio(float a_squared);

}  // namespace dsp
}  // namespace unlimited
