#pragma once

#include "unlimited/protocol.hpp"

// Compile-time decoder caps (spec 3.15), only ever defined for the whole build (dsp.cpp and decoder.cpp must
// agree). A header announcing more bits per peak or more bytes per frame is refused (lost(unsupported_mode)).
// RAM-tight MCUs (Cortex-M3, ESP8266) build with 7 and 16; spec 3.15 lists sizes and CPU.
#ifndef UNLIMITED_MAX_BITS_PER_PEAK
#define UNLIMITED_MAX_BITS_PER_PEAK 8
#endif
#ifndef UNLIMITED_MAX_FRAME_BYTES
#define UNLIMITED_MAX_FRAME_BYTES 32
#endif

namespace unlimited {

static_assert(UNLIMITED_MAX_BITS_PER_PEAK >= 1 && UNLIMITED_MAX_BITS_PER_PEAK <= k_max_bits_per_peak,
              "UNLIMITED_MAX_BITS_PER_PEAK must be 1..8");
static_assert(UNLIMITED_MAX_FRAME_BYTES >= 1 &&
                  UNLIMITED_MAX_FRAME_BYTES <= k_max_data_slots * k_max_bits_per_peak / k_bits_per_byte,
              "UNLIMITED_MAX_FRAME_BYTES must be 1..32");

static const uint32_t k_decoder_rate_hz = 8000;
static const uint8_t k_blocks_per_min_slot = 8;
static const uint8_t k_history_slots = 12;
static const uint16_t k_history_guard_cells = 32;
static const uint16_t k_history_cells =
    k_history_slots * k_speed_span * k_blocks_per_min_slot + k_history_guard_cells;
static const uint8_t k_mix_shift = 10;
static const uint16_t k_rebase_blocks = 4096;
static const uint8_t k_candidate_scales = 7;
static const uint8_t k_candidate_scale_blocks[k_candidate_scales] = {3, 4, 6, 8, 11, 16, 22};
// Detections of one marker at several scales agree within a block; distinct markers are at least T_min
// (8 blocks) apart. Merging within the T_min half-window keeps every marker of a T_min train.
static const float k_candidate_merge_blocks = 0.35f * k_blocks_per_min_slot;

static const float k_mixer_gain = 32.0f;          // Q15 table >> k_mix_shift
static const float k_cic_overlap = 1.0f / 3.0f;   // n_eff = (M - 1/3) * B for a window of M blocks
static const float k_g_marker = 0.8355f;          // mean |w r| over each 0.35 T marker half
static const float k_min_noise_variance = 1.0f / 12.0f;  // int16 quantization, per sample

// Slot path (spec 3.2, 3.10). Bank bins: 8 header tones x 2 sides in PREAMBLE, max(M, 8) grid tones in TRACK
// (k <= 2 still opens tones M..7, for the noise).
static const uint16_t k_header_bins = 2 * k_header_slots;
static const uint16_t k_max_grid_tones = 1u << UNLIMITED_MAX_BITS_PER_PEAK;
static const uint16_t k_min_grid_bins = 8;
static const uint16_t k_grid_bins = k_max_grid_tones > k_min_grid_bins ? k_max_grid_tones : k_min_grid_bins;
static const float k_peak_window_mean = 0.875f;        // sum of w / L for the Tukey alpha 0.25 window

static const uint8_t k_header_bg_slots = 4;           // smallest slot energies per side and tone
static const uint8_t k_header_noise_rank = 9;         // N_h = 9th smallest of the 16 tone spreads (both sides)
static const uint8_t k_header_side_noise_rank = 5;    // a side's own noise: 5th smallest of its 8 tone spreads
static const float k_header_margin = 12.0f;           // S1 - S2, noise units
static const uint8_t k_header_agree = 6;              // of k_header_slots
static const float k_header_peak_z = 8.0f;            // a slot holds a peak: noise alone reaches it with p = 3e-3

static const int16_t k_bg_step_up = 22;               // log2 Q8.8; settles on the 25 % quantile
static const int16_t k_bg_step_down = 66;
static const int16_t k_bg_mean_offset = 460;          // 25 % quantile -> mean of an exponential
static const uint8_t k_bg_warmup_slots = 32;          // first slots after reset(): steps x k_bg_warmup_factor, so a
static const int16_t k_bg_warmup_factor = 4;          // carrier 30 dB over the noise is learnt within 32 slots
static const uint8_t k_bg_min_bits = 3;               // no background below k = 3 (noise from tones M..7)

static const float k_presence_factor = 2.0f;          // x H_M
static const float k_amp_floor = 0.05f;               // x N_bin
static const float k_erasure_ratio = 2.0f;            // z_best < 2 z_second (background-subtracted)
static const float k_confidence_step_db = 0.5f;
static const int8_t k_llr_q4_max = 7;                 // stored LLR, 1 nat per step
static const int8_t k_soft_scale = 16;                // Event.soft per nat: |soft| <= 112

static const uint8_t k_ln_i0_points = 33;             // ln I0 table over [0, k_ln_i0_max]
static const float k_ln_i0_max = 16.0f;

static const uint8_t k_slot_blank_ratio = 4;          // |x| > 4 RMS
static const uint8_t k_slot_blank_hold = 4;           // samples zeroed each side of a trigger
static const uint8_t k_slot_blank_reach = 16;         // an impulse's neighbours: 16 samples (2 ms) each side
static const uint8_t k_slot_blank_crest = 5;          // impulse: x^2 > 5 x the mean x^2 of its neighbours
static const uint8_t k_slot_blank_delay = k_slot_blank_reach + k_slot_blank_hold;  // output delay
static const uint16_t k_slot_rms_samples = 800;       // RMS EMA constant

static const uint8_t k_audit_scale = 15;              // audit evidence stored as int8 in 1/15 units

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

// Tone search (spec 3.6): 49 Goertzel bins 50 Hz apart over 160-sample blocks, with half-bin powers between them
// for the lock, a slow floor and a recent floor (the larger one gates locks), and the block-to-block and half-block
// phases for the tone estimate.
class ToneSearch {
public:
    static const uint8_t k_max_bins = 49;
    static const uint16_t k_block_samples = 160;
    static const uint8_t k_long_run_blocks = 8;  // 160 ms

    ToneSearch();
    void configure(uint16_t min_hz, uint16_t max_hz);  // also clears bans
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
    bool long_run() const;  // the lock has held k_long_run_blocks blocks: longer than a data peak (T <= 128 ms)

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
    uint16_t first_hz_;
    uint16_t since_strike_;
    int16_t excluded_bin_;
    int16_t excluded_span_;
    uint8_t bins_;
    uint8_t best_bin_;
    uint8_t middle_bin_;              // best_bin_ when middle_ was taken
    uint8_t stable_blocks_;
    uint8_t steady_products_;
    uint8_t breaks_;     // strong products of the run off its steady phase
    uint8_t onset_products_;  // steady products before the first break
    uint8_t fast_gap_;   // search blocks since the last fast lock
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

// Alias audit (spec 3.11): 2N + 1 positions per frame, evidence clamped by the caller to [-4, 8] and stored as
// int8 in 1/k_audit_scale units. Positions are filled as their windows complete; next_frame() commits them.
class AuditRing {
public:
    static const uint8_t k_max_positions = 2 * k_max_data_slots + 1;
    static const uint8_t k_frames = 4;

    AuditRing();
    void reset(uint8_t positions);                 // clears; frames then have `positions` positions (2N + 1)
    void set(uint8_t position, float evidence);   // position 0..positions-1 of the frame being measured
    void next_frame();                             // commits the frame being measured and starts a clear one
    float max_evidence() const;                    // max over positions of the sum over committed frames
    uint8_t frames() const;                        // committed frames, at most k_frames
    uint8_t positions() const;

private:
    int8_t evidence_[k_frames + 1][k_max_positions];
    uint8_t positions_;
    uint8_t head_;  // row of the frame being measured
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

// Effective sample count of a window of `blocks` CIC-2 blocks (noise variance of its sum / sigma^2).
float noise_samples(float blocks, uint8_t block_samples);

// Flip statistics of the half-window sums before/after a centre (spec 3.4); noise_energy = sigma^2 * n_eff(W).
FlipMeasure flip_measure(const Complex& before, const Complex& after, float noise_energy, float half_samples);

// Vertex offset of the parabola through (-1, left), (0, centre), (1, right), clamped to +-0.5.
float parabolic_offset(float left, float centre, float right);

// Streaming Goertzel bank over one slot window on the raw 8 kHz input (spec 3.2), weighted by the matched
// Tukey alpha 0.25 envelope from a 32-bit slot-phase accumulator. No sample buffer: a window is opened at its
// predicted start and each sample is fed once; one window at a time. With UNLIMITED_BANK_FLOAT the state is
// float, otherwise int32 with Q14 coefficients and int64 products (10 B per bin). No constructor: call reset()
// first. Defined in dsp.cpp for HeaderBank and GridBank (one type when UNLIMITED_MAX_BITS_PER_PEAK is 4).
template <uint16_t Bins>
class SlotBank {
public:
    static const uint16_t k_max_bins = Bins;

    void reset();                                   // no bins, no window
    void set_bins(uint16_t count);                  // at most k_max_bins
    void set_frequency(uint16_t bin, float hz);     // between windows only; clamped to 100..3900 Hz
    uint16_t bins() const;
    // The next push() is the window's first sample, at u = (skip_samples + 0.5) / length. A window opened late
    // skips its first skip_samples (they count as zeros: the state stays 0), so it still ends where it was placed;
    // -0.5..0 places a window starting between two samples.
    void open(float length_samples, float skip_samples = 0.0f);
    bool push(int16_t sample);                      // true when this sample closed the window; idle: ignored
    bool active() const;
    float energy(uint16_t bin) const;               // |sum x w e^-jwn|^2 of the last window, input units
    float window_sum() const;                       // sum of w over the last window
    float window_square_sum() const;                // sum of w^2 over the last window

private:
#if defined(UNLIMITED_BANK_FLOAT)
    float coeff_[Bins];
    float s1_[Bins];
    float s2_[Bins];
#else
    int16_t coeff_q14_[Bins];
    int32_t s1_[Bins];
    int32_t s2_[Bins];
#endif
    uint32_t slot_phase_;  // u of the next sample, a full window = 2^32
    uint32_t slot_step_;
    uint32_t sum_w_;       // Q15
    uint32_t sum_w2_;      // Q15
    uint16_t bins_;
    bool active_;
};

typedef SlotBank<k_header_bins> HeaderBank;  // PREAMBLE
typedef SlotBank<k_grid_bins> GridBank;      // TRACK

// Per-bin background of the grid slot energies (spec 3.10): a log2 Q8.8 tracker per bin, +k_bg_step_up above,
// -k_bg_step_down below, settling on the 25 % quantile. No constructor: call reset() first.
class BinBackground {
public:
    void reset(uint16_t bins, float initial_mean);
    void push(uint16_t bin, float energy);          // every slot pushes bins 0..bins-1 in order
    void set_mean(uint16_t bin, float mean);        // a known background, e.g. a carrier seen in the header
    float mean(uint16_t bin) const;                 // 2^((bg + k_bg_mean_offset) / 256)
    float noise() const;                            // N_bin: median over the bins of mean()

private:
    int16_t log_q8_[k_grid_bins];
    uint16_t bins_;
    uint8_t slots_;  // slots pushed since reset(), up to k_bg_warmup_slots
};

// Impulse blanker of the slot path (spec 3.2): r <- r + (min(x^2, 16 r) - r) / 800 over unblanked samples. A
// sample over 4 RMS is loud; a loud sample that is also peaky (x^2 > k_slot_blank_crest x the mean x^2 of the
// k_slot_blank_reach samples on each side) is an impulse. It zeroes itself and k_slot_blank_hold samples on each
// side, and so does every loud sample of its ringing for k_slot_blank_tail samples after it. A tone is not peaky (a
// sine's crest is 2 x its mean; 5 leaves room for the noise on it), so a peak much louder than r (a tilted channel, a
// fade up) is not blanked. Output is delayed by k_slot_blank_delay samples.
class SlotBlanker {
public:
    SlotBlanker();
    void reset();
    int16_t push(int16_t sample);                   // returns the sample k_slot_blank_delay samples earlier
    bool blanked() const;                           // the sample just returned was zeroed

private:
    static const uint8_t k_window = 2 * k_slot_blank_reach + 1;

    int16_t window_[k_window];  // the newest samples (age 0..2 reach); the decision is on age k_slot_blank_reach
    uint64_t window_energy_;    // sum of their squares
    float power_;        // r
    uint16_t count_;     // inputs averaged so far, up to k_slot_rms_samples (warm-up: plain mean)
    uint8_t head_;       // slot of the next sample: the oldest one
    uint8_t zero_left_;  // outputs still to zero
    uint8_t tail_left_;  // samples after an impulse whose loud samples are its ringing
    uint8_t run_;        // trigger density: a lasting one is a level change, not an impulse
    bool blanked_;
};

// Header ML (spec 3.8). energy[d][j][h]: header slot j, tone h, on side d (0: grid above f_ref as received,
// 1: below). Self-background per side and tone, then S(w, d) over all 512 words and both sides.
struct HeaderDecision {
    uint16_t word;
    int8_t side;          // +1 above f_ref as received, -1 below
    float margin;         // S1 - S2, noise units
    float noise;          // N_h of the decided side, input units: seeds the grid background at TRACK entry
    float background[k_header_slots];  // floor of each tone on the decided side (steady part), input units
    uint8_t agreement;    // slots whose strongest tone is the codeword's
    bool accepted;        // margin >= k_header_margin and agreement >= k_header_agree (one less under a carrier)
};

HeaderDecision decide_header(const float (&energy)[2][k_header_slots][k_header_slots]);

// A known word (the mode memory's) against the header slots on one side (+1 above f_ref as received, -1 below).
struct HeaderMatch {
    uint8_t agreement;    // slots whose strongest tone is the word's
    uint8_t peaks;        // slots whose strongest tone reaches k_header_peak_z: the header window holds peaks
};

HeaderMatch match_header(const float (&energy)[2][k_header_slots][k_header_slots], uint16_t word, int8_t side);

// Slot decision (spec 3.10) from a closed bank window: background-subtracted argmax, max-log LLRs scaled by the
// slot's own amplitude, presence, erasure and telemetry. `slot` is i - 1 (rotation); bins as set for the grid.
struct SlotDecision {
    uint8_t tone;                          // strongest grid tone as received
    uint8_t symbol;                        // its k-bit label
    uint8_t confidence;                    // 10 log10(z_best / z_second) in 0.5 dB steps, <= 255
    int8_t soft[k_max_bits_per_peak];      // q4 LLRs, MSB first, > 0 means 1, |soft| <= k_llr_q4_max
    float crest;                           // 2 sqrt(max(z_best - N_bin, 0)) / sum w^2: the peak's crest, input units
    float noise;                           // N_bin of this decision, input units
    bool confident;                        // z_best / mean of the other z >= k_presence_factor H_M
    bool erasure;                          // z_best < k_erasure_ratio z_second
};

SlotDecision decide_slot(const GridBank& bank, const BinBackground& background, uint8_t bits_per_peak,
                         uint8_t slot);

// Mean noise energy per bin of a bank window that holds no peak (the STOP slot): the (bins / 4)-th smallest of the
// bins' energies over its expectation for exponential noise. A peak's matched window leaks -29..-37 dB into every
// other grid bin; a marker leaks below -40 dB into the grid, so this measures the noise up to per-slot Es/N0 of
// about 40 dB.
float window_noise(const GridBank& bank);

// ln I0(x): k_ln_i0_points table over [0, k_ln_i0_max] with linear interpolation; above, x - ln(2 pi x) / 2.
float ln_i0(float x);

// log2 Q8.8 of a positive value (clamped at the int16 range) and back.
int16_t log2_q8(float value);
float exp2_q8(int32_t log_q8);

}  // namespace dsp
}  // namespace unlimited
