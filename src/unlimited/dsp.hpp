#pragma once

#include "unlimited/protocol.hpp"

// Internal: the decoder's building blocks (spec 3.1, 3.2). Public only so that decoder.hpp can hold them by value and
// the tests can reach them; not part of the API.

namespace unlimited {

static const uint32_t k_decoder_rate_hz = 8000;

// Front end (spec 3.1). A block is T / 8 (clamped to 4..32 samples): 8 blocks per slot from 3.2 to 25 bytes/s, up to
// 25 blocks per slot at 1 byte/s.
static const uint8_t k_blocks_per_slot = 8;
static const uint8_t k_min_block_samples = 4;
static const uint8_t k_max_block_samples = 32;
// The history holds what acquisition needs at every speed (spec 3.3, 3.9): the 2 silent slots before the first START
// and its window (V20: the first window is decided alone), plus a scan catching up (after an end it starts one window
// behind): 22 slots are 550 blocks at 1 byte/s (25 per slot) and 176 at 25 bytes/s. 1024 cells (a power of two: a
// cell is found with a mask) hold 4.1 s at 1 byte/s and 0.51 s at 25 bytes/s. A new START weighed against the old grid
// after a silent START (spec 3.7) is given up when it leaves the history (at 1 byte/s, after 3 of its 4 windows).
static const uint16_t k_history_cells = 1024;
static_assert((k_history_cells & (k_history_cells - 1)) == 0, "the history is a power of two");
static const uint8_t k_mix_shift = 10;
static const uint16_t k_rebase_blocks = 8192;
static_assert(k_rebase_blocks > k_history_cells, "rebase only what the history no longer holds");
// The look-ahead (spec 3.1): the tone search hears every sample this long before the mixer, so the NCO sits on the
// pitch before the first START reaches the history. 2 slots of silence plus the search's latency to a leading bin
// (k_lead_latency_samples: 200 ms, ten search blocks) at the slowest speed: 2 x 800 + 1600 = 3200 samples.
static const uint16_t k_lead_latency_samples = 1600;
static const uint16_t k_lookahead_max_samples = 3200;

static const float k_mixer_gain = 32.0f;          // Q15 table >> k_mix_shift
static const float k_cic_overlap = 1.0f / 3.0f;   // n_eff = (M - 1/3) * B for a window of M blocks
static const float k_g_slot = 0.9394f;            // mean Tukey envelope over the central 0.75 T
static const float k_min_noise_variance = 1.0f / 12.0f;  // int16 quantization, per sample

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

// Two-stage block blanker (spec 3.1): a spike stage and an out-of-bin residual stage with a run limit.
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
    uint32_t end_block() const;    // one past the newest block
    uint32_t first_block() const;  // the oldest block held (== end_block() when none is)
    bool holds(uint32_t block) const;  // prefix P[block] is stored
    bool window(uint32_t origin_block, float from, float to, Complex& sum) const;  // divided by k_mixer_gain
    bool block(uint32_t index, Complex& value) const;  // block `index` alone (P[index + 1] - P[index]), same scale
    bool any_blanked(uint32_t origin_block, float from, float to) const;
    // Re-mixes the held blocks from `first` on to another NCO frequency (spec 3.1) and forgets the older ones: the
    // newest block turns by newest_radians, each older one by angle_per_block more; the prefixes are rebuilt in place
    // (one pass over the blocks kept, int32 blocks rounded).
    void rotate(uint32_t first, float newest_radians, float angle_per_block);

private:
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

// The look-ahead delay line (spec 3.1): the sample written now comes out `delay` samples later; zeros at first.
class Lookahead {
public:
    Lookahead();
    void configure(uint16_t delay);  // 0..k_lookahead_max_samples; clears the line
    int16_t push(int16_t sample);    // the sample pushed `delay` samples ago
    uint16_t delay() const;

private:
    int16_t line_[k_lookahead_max_samples];
    uint16_t delay_;
    uint16_t head_;
};

// Tone search (spec 3.2, v0.3's): up to 49 Goertzel bins 50 Hz apart over 160-sample blocks, plus a guard bin on each
// side, with half-bin powers between them for the lock, a slow floor and a recent floor (the larger one gates locks),
// the block-to-block and half-block phases for the tone estimate, the steady-carrier mask and the bans.
class ToneSearch {
public:
    static const uint8_t k_max_lock_bins = 49;               // the search range, 50 Hz apart
    static const uint8_t k_max_bins = k_max_lock_bins + 2;  // and a guard bin on each side, never locked on
    static const uint16_t k_block_samples = 160;
    static const uint8_t k_long_run_blocks = 8;  // 160 ms
    // A bin quiet for a whole window (10 slots: longer than any silence inside a transmission, 8 slots), and at least
    // the configured quiet time (the fade bridge's 300 ms), holds no transmission; fresh() tells how recently a tone came
    // up after such a quiet (spec 3.3, V6, V16).
    static const uint8_t k_quiet_slots = 10;

    ToneSearch();
    // Bins on the multiples of 50 Hz inside min_hz..max_hz; slot_us sets the leading bin's average (spec 3.2); a quiet
    // run is max(k_quiet_slots, min_quiet_ms). Also clears the bans.
    void configure(uint16_t min_hz, uint16_t max_hz, uint32_t slot_us, uint16_t min_quiet_ms = 0);
    void reset();                                       // statistics only; bans and strikes stay
    bool push(int16_t sample);                          // true when a search block ended
    bool candidate(float& tone_hz) const;               // a lock: a tone that stayed on its bin long enough
    bool steady() const;                                // the lock sees a steady tone (a carrier, a VOX lead)
    float floor() const;                                // noise power per bin, same units as the bin powers
    float recent_floor() const;                         // the same over the last few blocks (follows level steps)
    float onset_floor() const;                          // floor before the tone of the current lock appeared
    bool masked(float tone_hz) const;                   // steady carrier
    bool present(float tone_hz) const;                  // in the fast average now, as strong as a fast lock needs
    bool following(float tone_hz) const;                // its bin still stands out as leading() needs
    void ban(float tone_hz, uint16_t base_blocks);      // base_blocks << min(strikes, 3)
    // A transmission has ended: the averages of every bin restart from the noise floor and the lock is dropped, so
    // what is gone (a strong signal and its leakage stay in the slow average for seconds) is not taken for a new tone.
    // A steady carrier's bin keeps its averages, so it stays masked (else it would be grabbed before the next
    // transmission); the floor, the bans and the quiet runs stay.
    void forget();
    // The provisional tune before a lock (spec 3.2): after the warm-up, the bin whose slot-long average stands above the
    // floor by the Gamma tail of noise at 1e-6, the strongest such local peak. Its centre frequency.
    bool leading(float& tone_hz) const;
    // The tone's bin stands out now (as following() needs) and came out of a quiet of k_quiet_slots at most
    // `within_blocks` search blocks ago: nothing was sent on it before (a receiver that starts, or moves, onto a
    // transmission already running is not fresh; a quiet bin is not fresh either).
    bool fresh(float tone_hz, uint16_t within_blocks) const;
    // The leading bin among the fresh ones: a transmission starting beside a tone held that may be stronger.
    bool fresh_leading(float& tone_hz, uint16_t within_blocks) const;
    // The slot-long average of the tone's bin or a neighbour (a tone between bins shares its power), whichever is
    // larger: how strong the tone is to the leading bin's rule; lead_excess(): the same over the leading threshold.
    float lead_power(float tone_hz) const;
    float lead_excess(float tone_hz) const;

private:
    enum class Lock : uint8_t { none, fast, slow };

    static const uint8_t k_phase_bins = 3;  // best bin and its two neighbours
    static const uint8_t k_half_bins = k_max_bins - 1;

    void end_block();
    void update_bins();
    void update_floor();
    void update_bans();
    void update_quiet();
    void update_lock();
    void update_phase(bool continued, uint8_t bin);
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
    float centre_offset(const float* power) const;
    float coherence() const;
    float pattern_fit(const float* power, float offset) const;
    float steady_offset(const float* power, const Complex& product, const Complex& half, float half_weight,
                        float fallback) const;
    float estimate_tone() const;
    bool inside(float tone_hz) const;
    int16_t bin_of(float tone_hz) const;
    bool lead_bin(uint16_t within_blocks, float& tone_hz) const;

    int16_t coeff_q14_[k_max_bins];
    int32_t s1_[k_max_bins];
    int32_t s2_[k_max_bins];
    float fast_[k_max_bins];
    float lead_[k_max_bins];          // average over about one slot (at least 8 blocks): the leading bin
    float fast_half_[k_half_bins];    // same average of the half-bin power between bins b and b + 1
    float slow_[k_max_bins];
    float slow_square_[k_max_bins];
    uint16_t ban_[k_max_bins];
    uint8_t strikes_[k_max_bins];
    uint16_t quiet_[k_max_bins];      // blocks the bin's slot-long average has been under the leading threshold
    uint16_t since_quiet_[k_max_bins];  // blocks since its last quiet of quiet_blocks_ ended (k_never: none yet)
    Complex previous_[k_phase_bins];  // bin outputs of best_bin_ - 1 .. best_bin_ + 1, previous block
    Complex middle_[k_phase_bins];    // the same bins in the middle of this block (first half only)
    Complex phase_sum_;               // sum of X_n conj(X_n-1) of the best bin over the stable run
    Complex phase_square_sum_;        // same with the phase doubled
    float phase_weight_;              // sum of |X_n conj(X_n-1)|
    Complex half_sum_;                // sum of B_n conj(A_n), second half against first half of each block
    float half_weight_;
    float lock_level_;                // level() of the locked bin over the floor
    float lead_alpha_;
    float lead_ratio_;                // lead_ over the floor above which noise alone comes with probability 1e-6
    float floor_;
    float recent_floor_;
    float previous_floor_;
    float onset_floor_;
    uint16_t min_hz_;                 // the range configured: tones estimated outside it are no candidates
    uint16_t max_hz_;
    uint16_t quiet_blocks_;           // k_quiet_slots of the configured slot, in search blocks
    uint16_t first_hz_;
    uint16_t since_strike_;
    uint8_t bins_;
    uint8_t best_bin_;
    uint8_t middle_bin_;              // best_bin_ when middle_ was taken
    uint8_t stable_blocks_;
    Lock lock_;
    uint16_t sample_index_;
    uint32_t blocks_;
};

// Effective sample count of a window of `blocks` CIC-2 blocks (noise variance of its sum / sigma^2).
float noise_samples(float blocks, uint8_t block_samples);

// Vertex offset of the parabola through (-1, left), (0, centre), (1, right), clamped to +-0.5.
float parabolic_offset(float left, float centre, float right);

// The smart line (spec 3.5, v0.3's): equal-likelihood fraction of the reference for amplitude SNR a^2 (Rician against
// Rayleigh), 3 fixed-point iterations from 0.6, clamped to 0.50..0.75; 0.75 when a^2 <= 0.
float equal_likelihood_ratio(float a_squared);

}  // namespace dsp
}  // namespace unlimited
