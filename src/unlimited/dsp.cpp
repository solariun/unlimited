#include "unlimited/dsp.hpp"

#include <math.h>
#include <string.h>

namespace unlimited {
namespace dsp {

const uint8_t ImpulseBlanker::k_latency;
const uint8_t ImpulseBlanker::k_delay;
const uint8_t ToneSearch::k_max_bins;
const uint16_t ToneSearch::k_block_samples;
const uint8_t ToneSearch::k_long_run_blocks;
const uint8_t ToneSearch::k_phase_bins;
const uint8_t FineAfc::k_bins;
const uint8_t CandidateList::k_size;
const uint8_t AuditRing::k_max_positions;
const uint8_t AuditRing::k_packages;

namespace {

const float k_pi = 3.14159265f;
const float k_two_pi = 2.0f * k_pi;
const float k_phase_per_hz = 4294967296.0f / static_cast<float>(k_decoder_rate_hz);  // NCO step per Hz
const float k_tiny = 1e-20f;
const int32_t k_int16_max = 32767;

// QuantileTracker: 25 % quantile of an exponential variable is mean * ln(4/3).
const float k_quantile_scale = 3.476f;
const float k_quantile_step = 1.0f / 64.0f;
const float k_quantile_warmup_step = 1.0f / 8.0f;  // unseeded: converge from the first input quickly
const uint16_t k_quantile_warmup = 32;
const float k_quantile_down_ratio = 3.0f;  // (1 - 0.25) / 0.25 keeps the 25 % point in balance
const uint16_t k_quantile_primed = 16;
const uint16_t k_count_limit = 0xFFFF;

// NoiseTracker.
const float k_noise_clip = 10.0f;       // exponential noise exceeds 10 x its mean with probability 5e-5
const uint16_t k_noise_seed_weight = 8;  // a seed counts as this many inputs
const uint16_t k_noise_average = 64;     // exponential average over about 64 inputs after the warm-up

// ImpulseBlanker (spec 3.3).
const float k_spike_ratio = 10.0f;
const float k_spike_floor = 10.0f;
const float k_residual_ratio = 8.0f;
const float k_residual_tone = 2.0f;
const uint8_t k_residual_run = 2;
const float k_residual_adapt = 0.2f;
const float k_residual_alpha = 1.0f / 64.0f;
const uint8_t k_residual_prime_blocks = 64;
const float k_min_block_energy = 16.0f;  // sum of (x >> 4)^2: below any impulse worth blanking (digital silence)
const uint8_t k_fill_limit = 0xFF;

// ToneSearch (spec 3.6). Bins sit on whole multiples of k_search_step_hz: they are then exact DFT bins of the
// 160-sample block, which the phase estimates rely on.
const uint16_t k_search_step_hz = 50;
const float k_estimate_accuracy_hz = 5.0f;  // of estimate_tone() for a tone between bins (U13)
const uint8_t k_goertzel_shift = 14;
const float k_goertzel_one = 16384.0f;
const float k_fast_alpha = 1.0f / 8.0f;
const float k_slow_alpha = 1.0f / 128.0f;
// The mean of the lower half of 49 bin powers, each averaged over N blocks, is (1 - 0.8 / sqrt(N)) of the
// noise (N >= 2; the slow average counts as 2 / alpha - 1 = 255 blocks).
const float k_lower_half_bias = 0.8f;
const float k_slow_blocks = 2.0f / k_slow_alpha - 1.0f;
const float k_min_floor = static_cast<float>(ToneSearch::k_block_samples);  // noise of 1 LSB rms
// The recent floor: the mean of the lower half of one block's bin powers is 0.309 of the noise; averaged over about 4
// blocks. It follows a receiver AGC within 100 ms: after a strong signal ends the AGC raises the noise by the signal's
// SNR over a second or so, which the slow floor (2.5 s) would take for tones everywhere; the locks compare with the
// larger of the two floors.
const float k_block_floor_bias = 0.309f;
const float k_recent_alpha = 1.0f / 4.0f;
const float k_fast_lock = 6.0f;
// A weak tune tone between two bins reaches 6 x the floor only after about 350 ms, beyond a 250 ms tune: in a bin
// whose slow average is still near the floor it locks at 4 x. A bin whose noise stands above the floor (FM noise
// after de-emphasis, a receiver's audio slope) keeps the 6 x.
const float k_fast_lock_quiet = 4.0f;
const float k_quiet_bin = 2.0f;
// A tone below k_fast_lock locks after k_long_run_blocks stable blocks, not k_lock_blocks: its phase products need
// that many for the tone estimate. Half-bin powers |X_b - X_b+1|^2 / 2 (the DFT halfway between two bins, same noise)
// cut the scalloping loss of a tone between two bins from 3.9 dB to 1.2 dB at worst.
const uint8_t k_half_block_samples = ToneSearch::k_block_samples / 2;
// The second half of a block against its first half turns by 2 pi f / 100 Hz for a tone f off the bin: the whole-bin
// alias of the block-to-block phase (f mod 50 Hz) is the one whose half-block turn matches. Used when the half-block
// products of the run add up coherently to this fraction of their magnitudes.
const float k_half_coherence = 0.3f;
const float k_present_lock = 2.0f;  // current block of the fast-lock bin, over the floor
const float k_slow_lock = 2.0f;
const float k_keyed_min = 1.0f;
const float k_steady_max = 0.05f;
const float k_steady_level = 4.0f;
const uint32_t k_steady_after_blocks = 128;
const uint32_t k_slow_after_blocks = 96;
const uint8_t k_lock_blocks = 3;
const uint8_t k_warmup_blocks = 8;
const uint16_t k_strike_decay_blocks = 15000;  // 5 min of 20 ms search blocks
const uint8_t k_max_strike_shift = 3;
const uint8_t k_strike_limit = 0xFF;
const uint32_t k_ban_limit = 0xFFFF;
const uint32_t k_blocks_limit = 0xFFFFFFFFu;
const float k_max_alias = 0.75f;           // the tone lies within 3/4 bin of the strongest bin
const int8_t k_phase_aliases = 2;          // doubled phase: offsets phase + m / 2, m = -2..2
const float k_half_bin = 0.5f;
const float k_min_phase_coherence = 0.9f;  // |sum of products| / sum of |products|: a steady tone
// Marker train onset: a tone that was steady (the tune) and then shows a block-to-block product off its
// steady phase (a reversal between the blocks, or inside one of them: with the tone off a search bin such
// a block takes any phase). Off means more than 37 degrees and more than 3.6 sigma of the product's phase
// noise, from the power of its two blocks over the floor; both blocks must be 10 dB over the floor.
// Carriers never move, keyed CW keeps its phase while keyed (a keyed gap fails the level test), noise and
// speech are never steady first.
const float k_steady_cosine = 0.8f;
const float k_break_sigmas = 3.6f;
const float k_break_snr = 10.0f;
const uint8_t k_train_breaks = 1;
const uint8_t k_train_steady_products = 3;  // 80 ms of steady tone before the break
const uint8_t k_resume_blocks = 2;
// The echo test of a train onset (onset_echo()): a marker of the held signal can null its own carrier in the block of
// the break; the held tone's turn is then read against its last strong block when it is strong again, at most this
// many blocks later.
const uint8_t k_held_gap_blocks = 3;
const uint8_t k_count_limit_u8 = 0xFF;
const float k_no_score = -1e30f;

// FineAfc (spec 3.3): 65 bins on the squared signal, 1 Hz apart within +-20 Hz (tone +-10 Hz, the spec
// resolution) and 3.33 Hz apart out to +-60 Hz (tone +-30 Hz: on data the coarse search can be half a
// search bin off). A far offset is pulled in by a first correction and refined by the next.
const float k_afc_default_rate_hz = 125.0f;
const float k_afc_bin_step_hz = 1.0f;
const int8_t k_afc_centre_bin = 32;  // (FineAfc::k_bins - 1) / 2
const int8_t k_afc_fine_bins = 20;   // 1 Hz bins on each side: +-20 Hz squared
const float k_afc_span_hz = 60.0f;   // outermost bin on the squared signal (tone +-30 Hz)
const float k_afc_outer_step_hz =
    (k_afc_span_hz - k_afc_fine_bins * k_afc_bin_step_hz) / static_cast<float>(k_afc_centre_bin - k_afc_fine_bins);
const int8_t k_afc_narrow_bins = 24;  // 20 fine bins + 4 outer: 33.3 Hz squared, 16.7 Hz tone
const uint8_t k_afc_lobe_bins = 3;    // bins on each side of the peak left out of the floor
const float k_afc_lambda = 0.984f;
const float k_afc_outer_lambda = 0.95f;
const float k_afc_peak_ratio = 8.0f;
const uint16_t k_afc_min_inputs = 16;
const float k_squared_to_tone = 0.5f;  // squaring doubles the offset

const float k_parabola_limit = 0.5f;

// PrefixHistory::rotate: the rotator steps by one block's angle; it is recomputed from the angle this often, so its
// float rounding never adds up (spec 3.3).
const uint16_t k_rotate_reseed_blocks = 64;

// The smart line (spec 3.10): rho = 0.5 + ln(2 pi a^2 rho) / (2 a^2), iterated from 0.6.
const float k_rho_min = 0.50f;
const float k_rho_max = 0.75f;
const float k_rho_seed = 0.6f;
const uint8_t k_rho_iterations = 3;
const float k_rho_offset = 0.5f;

// PackageLearner (spec 3.8).
const int32_t k_no_marker = 0;           // grid indices after the train's newest marker are > 0
const int32_t k_exact_start_slots = 4;   // N = 1: the first marker within 4 slots of L can place package 0 exactly
const uint8_t k_gap_limit = 0xFF;

float clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

float max_of(float a, float b) {
    return a > b ? a : b;
}

float min_of(float a, float b) {
    return a < b ? a : b;
}

int32_t round_to_int(float value) {
    return static_cast<int32_t>(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

int32_t as_signed(uint32_t value) {
    return static_cast<int32_t>(value);
}

// value into [-1, 1) modulo 2 (outside ToneSearch, whose floor() hides floorf where libm defines it as a macro).
float wrap_two(float value) {
    return value - 2.0f * floorf(0.5f * (value + 1.0f));
}

// Sum of the `lower` smallest of values[0..count-1] (quickselect partition; reorders them).
float lower_sum(float* values, uint8_t count, uint8_t lower) {
    uint8_t low = 0;
    uint8_t high = static_cast<uint8_t>(count - 1u);
    const uint8_t rank = static_cast<uint8_t>(lower - 1u);
    while (low < high) {
        const uint8_t middle = static_cast<uint8_t>((low + high) / 2u);
        float swap = values[middle];
        values[middle] = values[high];
        values[high] = swap;
        const float pivot = values[high];
        uint8_t store = low;
        for (uint8_t i = low; i < high; ++i) {
            if (values[i] >= pivot) continue;
            swap = values[i];
            values[i] = values[store];
            values[store] = swap;
            ++store;
        }
        values[high] = values[store];
        values[store] = pivot;
        if (rank == store) break;
        if (rank < store) {
            high = static_cast<uint8_t>(store - 1u);
        } else {
            low = static_cast<uint8_t>(store + 1u);
        }
    }
    float sum = 0.0f;
    for (uint8_t i = 0; i < lower; ++i) sum += values[i];
    return sum;
}

}  // namespace

// ---------------------------------------------------------------------------
// Nco
// ---------------------------------------------------------------------------

Nco::Nco() : phase_(0), step_(0) {}

void Nco::set_frequency(float hz) {
    step_ = static_cast<uint32_t>(round_to_int(hz * k_phase_per_hz));
}

void Nco::adjust_frequency(float delta_hz) {
    step_ += static_cast<uint32_t>(round_to_int(delta_hz * k_phase_per_hz));
}

float Nco::frequency() const {
    return static_cast<float>(step_) / k_phase_per_hz;
}

void Nco::next(int16_t& cos_q15, int16_t& sin_q15) {
    cos_q15 = cosine_q15(phase_);
    sin_q15 = sine_q15(phase_);
    phase_ += step_;
}

// ---------------------------------------------------------------------------
// Cic2
// ---------------------------------------------------------------------------

Cic2::Cic2() {
    reset();
}

void Cic2::reset() {
    memset(integrator1_, 0, sizeof(integrator1_));
    memset(integrator2_, 0, sizeof(integrator2_));
    memset(comb1_, 0, sizeof(comb1_));
    memset(comb2_, 0, sizeof(comb2_));
}

void Cic2::push(int32_t re, int32_t im) {
    integrator1_[0] += static_cast<uint32_t>(re);
    integrator2_[0] += integrator1_[0];
    integrator1_[1] += static_cast<uint32_t>(im);
    integrator2_[1] += integrator1_[1];
}

void Cic2::dump(uint8_t block_samples, int32_t& re, int32_t& im) {
    int32_t out[2];
    for (uint8_t i = 0; i < 2; ++i) {
        const uint32_t d1 = integrator2_[i] - comb1_[i];
        comb1_[i] = integrator2_[i];
        const uint32_t d2 = d1 - comb2_[i];
        comb2_[i] = d1;
        out[i] = as_signed(d2) / static_cast<int32_t>(block_samples);
    }
    re = out[0];
    im = out[1];
}

// ---------------------------------------------------------------------------
// QuantileTracker
// ---------------------------------------------------------------------------

QuantileTracker::QuantileTracker() : quantile_(0.0f), count_(0) {}

void QuantileTracker::reset(float initial) {
    quantile_ = initial / k_quantile_scale;
    count_ = initial > 0.0f ? k_quantile_warmup : 0;  // a seeded tracker skips the warm-up
}

void QuantileTracker::push(float value) {
    const float step = count_ < k_quantile_warmup ? k_quantile_warmup_step : k_quantile_step;
    if (quantile_ <= 0.0f) {
        quantile_ = value / k_quantile_scale;
    } else if (value > quantile_) {
        quantile_ += quantile_ * step;
    } else {
        quantile_ -= quantile_ * (step * k_quantile_down_ratio);
    }
    if (count_ < k_count_limit) ++count_;
}

float QuantileTracker::mean_estimate() const {
    return quantile_ * k_quantile_scale;
}

bool QuantileTracker::primed() const {
    return count_ >= k_quantile_primed;
}

// ---------------------------------------------------------------------------
// NoiseTracker
// ---------------------------------------------------------------------------

NoiseTracker::NoiseTracker() : mean_(0.0f), count_(0) {}

void NoiseTracker::reset(float initial) {
    mean_ = initial > 0.0f ? initial : 0.0f;
    count_ = initial > 0.0f ? k_noise_seed_weight : 0;
}

void NoiseTracker::push(float value) {
    if (count_ < k_noise_average) ++count_;
    const float clipped = count_ > 1 ? min_of(value, k_noise_clip * mean_) : value;
    mean_ += (clipped - mean_) / static_cast<float>(count_);
}

float NoiseTracker::mean_estimate() const {
    return mean_;
}

// ---------------------------------------------------------------------------
// ImpulseBlanker: index k_delay - 1 is the newest block, k_latency the block being decided.
// ---------------------------------------------------------------------------

ImpulseBlanker::ImpulseBlanker() {
    reset();
}

void ImpulseBlanker::reset() {
    memset(energy_, 0, sizeof(energy_));
    memset(in_bin_, 0, sizeof(in_bin_));
    residual_average_ = 0.0f;
    floor_.reset(0.0f);
    run_ = 0;
    fill_ = 0;
    pending_ = false;
}

// In-bin energy around the block, k-2..k+2. At T_min a marker covers 8 blocks with two energy lobes and
// a reversal dip between them: its blocks are in-bin, an impulse is broadband.
float ImpulseBlanker::nearby_tone() const {
    float tone = 0.0f;
    for (uint8_t i = 0; i < k_delay; ++i) tone = max_of(tone, in_bin_[i]);
    return tone;
}

bool ImpulseBlanker::spike(float floor) const {
    const float energy = energy_[k_latency];
    const float neighbours = max_of(energy_[0], energy_[k_delay - 1]);
    return energy > k_spike_ratio * neighbours && energy > k_spike_floor * max_of(floor, k_min_block_energy) &&
           energy > k_residual_tone * nearby_tone();
}

bool ImpulseBlanker::residual_trigger(float residual) const {
    return residual > k_residual_ratio * max_of(residual_average_, k_min_block_energy) &&
           residual > k_residual_tone * nearby_tone();
}

bool ImpulseBlanker::push_block(float energy, float in_bin_energy) {
    for (uint8_t i = 0; i + 1 < k_delay; ++i) {
        energy_[i] = energy_[i + 1];
        in_bin_[i] = in_bin_[i + 1];
    }
    energy_[k_delay - 1] = energy;
    in_bin_[k_delay - 1] = in_bin_energy;
    floor_.push(energy);
    if (fill_ < k_fill_limit) ++fill_;

    bool blank = pending_;
    pending_ = false;
    if (fill_ < k_delay) return blank;

    if (floor_.primed() && spike(floor_.mean_estimate())) {
        blank = true;
        pending_ = true;
    }
    const float residual = max_of(energy_[k_latency] - in_bin_[k_latency], 0.0f);
    if (fill_ < k_residual_prime_blocks) {
        residual_average_ += (residual - residual_average_) / static_cast<float>(fill_ - k_delay + 1);
        return blank;
    }
    if (blank) return true;
    if (residual_trigger(residual)) {
        if (run_ < k_residual_run) {
            ++run_;
            return true;
        }
        residual_average_ += k_residual_adapt * (residual - residual_average_);  // a level change, not an impulse
        return false;
    }
    run_ = 0;
    residual_average_ += k_residual_alpha * (residual - residual_average_);
    return false;
}

// ---------------------------------------------------------------------------
// PrefixHistory: re_/im_ hold P[j] = sum of h[i < j] (uint32 wrap); the blank bit of h[j] sits with P[j + 1].
// ---------------------------------------------------------------------------

PrefixHistory::PrefixHistory() : sum_re_(0), sum_im_(0), end_block_(0), head_(0), fill_(0) {
    memset(re_, 0, sizeof(re_));
    memset(im_, 0, sizeof(im_));
    memset(blanked_, 0, sizeof(blanked_));
    reset();
}

void PrefixHistory::reset() {
    head_ = 0;
    re_[head_] = sum_re_;
    im_[head_] = sum_im_;
    blanked_[0] &= static_cast<uint8_t>(~1u);
    fill_ = 1;
}

void PrefixHistory::push(int32_t re, int32_t im, bool blanked) {
    sum_re_ += static_cast<uint32_t>(re);
    sum_im_ += static_cast<uint32_t>(im);
    head_ = static_cast<uint16_t>(head_ + 1 == k_history_cells ? 0 : head_ + 1);
    re_[head_] = sum_re_;
    im_[head_] = sum_im_;
    const uint8_t mask = static_cast<uint8_t>(1u << (head_ & 7u));
    if (blanked) {
        blanked_[head_ >> 3] |= mask;
    } else {
        blanked_[head_ >> 3] &= static_cast<uint8_t>(~mask);
    }
    ++end_block_;
    if (fill_ < k_history_cells) ++fill_;
}

uint32_t PrefixHistory::end_block() const {
    return end_block_;
}

uint32_t PrefixHistory::first_block() const {
    return end_block_ - fill_ + 1u;
}

// Block j (P[j + 1] - P[j]) is multiplied by e^{i a_j}, a_j = newest_radians + angle_per_block (newest - j): each block
// value is rebuilt from the prefixes, turned, rounded to int32 and summed again from the oldest prefix, which stays.
void PrefixHistory::rotate(float newest_radians, float angle_per_block) {
    const uint16_t blocks = static_cast<uint16_t>(fill_ - 1u);
    if (blocks == 0) return;
    uint16_t at = static_cast<uint16_t>((head_ + k_history_cells - blocks) % k_history_cells);
    uint32_t old_re = re_[at];
    uint32_t old_im = im_[at];
    uint32_t new_re = old_re;
    uint32_t new_im = old_im;
    const float step_cos = cosf(angle_per_block);
    const float step_sin = sinf(angle_per_block);
    float turn_cos = 1.0f;
    float turn_sin = 0.0f;
    for (uint16_t i = 0; i < blocks; ++i) {
        if (i % k_rotate_reseed_blocks == 0) {
            const float angle = newest_radians + angle_per_block * static_cast<float>(blocks - 1u - i);
            turn_cos = cosf(angle);
            turn_sin = sinf(angle);
        }
        at = static_cast<uint16_t>(at + 1 == k_history_cells ? 0 : at + 1);
        const float re = static_cast<float>(as_signed(re_[at] - old_re));
        const float im = static_cast<float>(as_signed(im_[at] - old_im));
        old_re = re_[at];
        old_im = im_[at];
        new_re += static_cast<uint32_t>(round_to_int(re * turn_cos - im * turn_sin));
        new_im += static_cast<uint32_t>(round_to_int(re * turn_sin + im * turn_cos));
        re_[at] = new_re;
        im_[at] = new_im;
        // The next (newer) block turns by angle_per_block less.
        const float next_cos = turn_cos * step_cos + turn_sin * step_sin;
        turn_sin = turn_sin * step_cos - turn_cos * step_sin;
        turn_cos = next_cos;
    }
    sum_re_ = new_re;
    sum_im_ = new_im;
}

bool PrefixHistory::holds(uint32_t block) const {
    return end_block_ - block < fill_;
}

uint16_t PrefixHistory::cell(uint32_t block) const {
    const uint32_t age = end_block_ - block;
    return static_cast<uint16_t>((head_ + k_history_cells - age) % k_history_cells);
}

bool PrefixHistory::blanked_bit(uint16_t index) const {
    return (blanked_[index >> 3] & (1u << (index & 7u))) != 0;
}

bool PrefixHistory::window(uint32_t origin_block, float from, float to, Complex& sum) const {
    const float floor_from = floorf(from);
    const float floor_to = floorf(to);
    const uint32_t a = origin_block + static_cast<uint32_t>(static_cast<int32_t>(floor_from));
    const uint32_t b = origin_block + static_cast<uint32_t>(static_cast<int32_t>(floor_to));
    const float fraction_a = from - floor_from;
    const float fraction_b = to - floor_to;
    if (!holds(a) || !holds(b)) return false;
    if (fraction_a > 0.0f && !holds(a + 1)) return false;
    if (fraction_b > 0.0f && !holds(b + 1)) return false;

    const uint16_t cell_a = cell(a);
    const uint16_t cell_b = cell(b);
    float re = static_cast<float>(as_signed(re_[cell_b] - re_[cell_a]));
    float im = static_cast<float>(as_signed(im_[cell_b] - im_[cell_a]));
    if (fraction_b > 0.0f) {
        const uint16_t next = cell(b + 1);
        re += fraction_b * static_cast<float>(as_signed(re_[next] - re_[cell_b]));
        im += fraction_b * static_cast<float>(as_signed(im_[next] - im_[cell_b]));
    }
    if (fraction_a > 0.0f) {
        const uint16_t next = cell(a + 1);
        re -= fraction_a * static_cast<float>(as_signed(re_[next] - re_[cell_a]));
        im -= fraction_a * static_cast<float>(as_signed(im_[next] - im_[cell_a]));
    }
    sum.re = re / k_mixer_gain;
    sum.im = im / k_mixer_gain;
    return true;
}

bool PrefixHistory::any_blanked(uint32_t origin_block, float from, float to) const {
    const uint32_t first = origin_block + static_cast<uint32_t>(static_cast<int32_t>(floorf(from)));
    const uint32_t last = origin_block + static_cast<uint32_t>(static_cast<int32_t>(ceilf(to)));
    for (uint32_t block = first; block != last; ++block) {
        if (holds(block + 1) && blanked_bit(cell(block + 1))) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ToneSearch
// ---------------------------------------------------------------------------

ToneSearch::ToneSearch() {
    configure(k_min_tone_hz, k_max_tone_hz);
}

// Bins on the multiples of 50 Hz inside min_hz..max_hz (at least one), and a guard bin on each side: a tone just outside
// the range peaks in a guard, where no lock is taken, instead of in the edge bin at a wrong 50 Hz alias.
void ToneSearch::configure(uint16_t min_hz, uint16_t max_hz) {
    min_hz_ = min_hz;
    max_hz_ = max_hz;
    const uint16_t first_lock =
        static_cast<uint16_t>((min_hz + k_search_step_hz - 1u) / k_search_step_hz * k_search_step_hz);
    const uint16_t span_bins =
        static_cast<uint16_t>(max_hz >= first_lock ? (max_hz - first_lock) / k_search_step_hz + 1u : 1u);
    first_hz_ = static_cast<uint16_t>(first_lock > k_search_step_hz ? first_lock - k_search_step_hz : 0u);
    bins_ = static_cast<uint8_t>((span_bins < k_max_lock_bins ? span_bins : k_max_lock_bins) + 2u);
    for (uint8_t b = 0; b < k_max_bins; ++b) {
        const float hz = static_cast<float>(first_hz_ + b * k_search_step_hz);
        const int32_t q14 = round_to_int(2.0f * cosf(k_two_pi * hz / static_cast<float>(k_decoder_rate_hz)) *
                                         k_goertzel_one);
        coeff_q14_[b] = static_cast<int16_t>(q14 > k_int16_max ? k_int16_max : (q14 < -k_int16_max ? -k_int16_max : q14));
    }
    memset(ban_, 0, sizeof(ban_));
    memset(strikes_, 0, sizeof(strikes_));
    since_strike_ = 0;
    reset();
}

void ToneSearch::reset() {
    memset(s1_, 0, sizeof(s1_));
    memset(s2_, 0, sizeof(s2_));
    memset(fast_, 0, sizeof(fast_));
    memset(fast_half_, 0, sizeof(fast_half_));
    memset(slow_, 0, sizeof(slow_));
    memset(slow_square_, 0, sizeof(slow_square_));
    memset(previous_, 0, sizeof(previous_));
    memset(middle_, 0, sizeof(middle_));
    phase_sum_.re = 0.0f;
    phase_sum_.im = 0.0f;
    phase_square_sum_.re = 0.0f;
    phase_square_sum_.im = 0.0f;
    phase_weight_ = 0.0f;
    half_sum_.re = 0.0f;
    half_sum_.im = 0.0f;
    half_weight_ = 0.0f;
    steady_sum_.re = 0.0f;
    steady_sum_.im = 0.0f;
    steady_half_sum_.re = 0.0f;
    steady_half_sum_.im = 0.0f;
    steady_weight_ = 0.0f;
    steady_half_weight_ = 0.0f;
    steady_tone_hz_ = 0.0f;
    lock_level_ = 0.0f;
    floor_ = k_min_floor;
    recent_floor_ = 0.0f;
    previous_floor_ = k_min_floor;
    onset_floor_ = k_min_floor;
    excluded_bin_ = -1;
    excluded_span_ = 0;
    excluded_hz_ = 0.0f;
    held_strong_.re = 0.0f;
    held_strong_.im = 0.0f;
    held_middle_ = held_strong_;
    held_gap_ = k_count_limit_u8;
    onset_wait_ = 0;
    held_reversed_ = false;
    held_unread_ = false;
    onset_echo_ = false;
    onset_pending_ = false;
    best_bin_ = 0;
    middle_bin_ = 0;
    stable_blocks_ = 0;
    steady_products_ = 0;
    breaks_ = 0;
    onset_products_ = 0;
    fast_gap_ = k_count_limit_u8;
    lock_ = Lock::none;
    sample_index_ = 0;
    blocks_ = 0;
}

void ToneSearch::interrupt() {
    memset(s1_, 0, sizeof(s1_));
    memset(s2_, 0, sizeof(s2_));
    sample_index_ = 0;
    stable_blocks_ = 0;
    lock_ = Lock::none;
    fast_gap_ = k_count_limit_u8;
}

bool ToneSearch::push(int16_t sample) {
    // Per-sample bin loops count in a native word: a narrower index costs a zero-extension per bin on xtensa and ARM.
    for (uint32_t b = 0; b < bins_; ++b) {
        const int64_t product = static_cast<int64_t>(coeff_q14_[b]) * s1_[b];
        const int32_t s0 = sample + static_cast<int32_t>(product >> k_goertzel_shift) - s2_[b];
        s2_[b] = s1_[b];
        s1_[b] = s0;
    }
    if (++sample_index_ == k_half_block_samples) {
        if (excluded_bin_ >= 0) held_middle_ = bin_output(static_cast<uint8_t>(excluded_bin_));
        middle_bin_ = best_bin_;
        for (uint8_t i = 0; i < k_phase_bins; ++i) {
            const int16_t b = static_cast<int16_t>(best_bin_ + i) - 1;
            if (b >= 0 && b < bins_) {
                middle_[i] = bin_output(static_cast<uint8_t>(b));
            } else {
                middle_[i].re = 0.0f;
                middle_[i].im = 0.0f;
            }
        }
    }
    if (sample_index_ < k_block_samples) return false;
    sample_index_ = 0;
    end_block();
    return true;
}

void ToneSearch::end_block() {
    update_bins();
    update_floor();
    update_bans();
    update_held();
    update_lock();
    memset(s1_, 0, sizeof(s1_));
    memset(s2_, 0, sizeof(s2_));
}

// Goertzel output s1 - e^{-jw} s2: the DFT value up to a phase factor that is the same in every block.
Complex ToneSearch::bin_output(uint8_t bin) const {
    const float cos_w = static_cast<float>(coeff_q14_[bin]) / (2.0f * k_goertzel_one);
    const float sin_w = sqrtf(max_of(1.0f - cos_w * cos_w, 0.0f));
    Complex out;
    out.re = static_cast<float>(s1_[bin]) - cos_w * static_cast<float>(s2_[bin]);
    out.im = sin_w * static_cast<float>(s2_[bin]);
    return out;
}

// The DFT value X_b of the block: y e^{jw}, y the Goertzel output after the block's last sample (bins are whole
// multiples of 50 Hz, so e^{-jwN} = 1). Neighbour bins then differ by pi for a tone halfway between them.
Complex ToneSearch::dft_output(uint8_t bin) const {
    const Complex y = bin_output(bin);
    const float cos_w = static_cast<float>(coeff_q14_[bin]) / (2.0f * k_goertzel_one);
    const float sin_w = sqrtf(max_of(1.0f - cos_w * cos_w, 0.0f));
    Complex out;
    out.re = cos_w * y.re - sin_w * y.im;
    out.im = sin_w * y.re + cos_w * y.im;
    return out;
}

// B conj(A) of a bin, A and B the DFT sums over the first and second half of the block: from the Goertzel outputs y80
// (middle_) and y160, B conj(A) = e^{jw 80} y160 conj(y80) - |y80|^2, and e^{jw 80} = (-1)^(f / 50 Hz). Its phase is pi
// times the tone's offset from the bin in bins: it is returned as seen from bin 0, (-1)^bin times, so that the products
// of a run that moves between neighbour bins add up (half_offset() refers the sum to best_bin_).
Complex ToneSearch::half_product(uint8_t bin) const {
    Complex out;
    out.re = 0.0f;
    out.im = 0.0f;
    const int16_t index = static_cast<int16_t>(bin + 1) - static_cast<int16_t>(middle_bin_);
    if (index < 0 || index >= k_phase_bins) return out;
    const Complex& first = middle_[index];
    const Complex full = bin_output(bin);
    const float sign = ((first_hz_ / k_search_step_hz + bin) & 1u) != 0 ? -1.0f : 1.0f;
    const float to_first_bin = (bin & 1u) != 0 ? -1.0f : 1.0f;
    out.re = to_first_bin * (sign * (full.re * first.re + full.im * first.im) - (first.re * first.re + first.im * first.im));
    out.im = to_first_bin * sign * (full.im * first.re - full.re * first.im);
    return out;
}

// Offset of the tone from best_bin_ in bins (-1..1] from a sum of half-block products, if they are coherent.
bool ToneSearch::half_offset(const Complex& half, float weight, float& offset) const {
    const float magnitude = sqrtf(half.re * half.re + half.im * half.im);
    if (weight <= 0.0f || magnitude < k_half_coherence * weight) return false;
    offset = wrap_two(atan2f(half.im, half.re) / k_pi + static_cast<float>(best_bin_));
    return true;
}

// Bins are exact DFT bins (160 samples, 50 Hz), so a tone advances the phase of every bin output by
// 2 pi (f / 50 Hz) per block: the phase of X_n conj(X_n-1) is the tone offset modulo one bin.
void ToneSearch::update_phase(bool continued, bool resumed, uint8_t bin) {
    if (!continued) {
        phase_sum_.re = 0.0f;
        phase_sum_.im = 0.0f;
        phase_square_sum_.re = 0.0f;
        phase_square_sum_.im = 0.0f;
        phase_weight_ = 0.0f;
        half_sum_ = phase_sum_;
        half_weight_ = 0.0f;
    }
    if (!continued && !resumed) {
        steady_sum_ = phase_sum_;
        steady_half_sum_ = phase_sum_;
        steady_weight_ = 0.0f;
        steady_half_weight_ = 0.0f;
        steady_tone_hz_ = 0.0f;
        steady_products_ = 0;
        breaks_ = 0;
        onset_products_ = 0;
        onset_echo_ = false;
        onset_pending_ = false;
    }
    if (continued) {
        const Complex current = bin_output(bin);
        const Complex& before = previous_[bin + 1 - best_bin_];
        Complex product;
        product.re = current.re * before.re + current.im * before.im;
        product.im = current.im * before.re - current.re * before.im;
        const float magnitude = sqrtf(product.re * product.re + product.im * product.im);
        phase_sum_.re += product.re;
        phase_sum_.im += product.im;
        phase_weight_ += magnitude;
        const Complex half = half_product(bin);
        half_sum_.re += half.re;
        half_sum_.im += half.im;
        half_weight_ += sqrtf(half.re * half.re + half.im * half.im);
        if (magnitude > 0.0f) {  // phase doubled, magnitude kept: immune to marker sign reversals
            phase_square_sum_.re += (product.re * product.re - product.im * product.im) / magnitude;
            phase_square_sum_.im += 2.0f * product.re * product.im / magnitude;
        }
        const float weakest = min_of(current.re * current.re + current.im * current.im,
                                     before.re * before.re + before.im * before.im);
        if (weakest >= k_break_snr * floor_) {
            // phase variance of X_n conj(X_n-1): floor / (2 |X|^2) from each block
            const float variance = 0.5f * floor_ *
                                   (1.0f / (current.re * current.re + current.im * current.im) +
                                    1.0f / (before.re * before.re + before.im * before.im));
            const float angle = k_break_sigmas * sqrtf(variance);
            update_breaks(product, half, magnitude, angle < k_pi ? min_of(cosf(angle), k_steady_cosine) : -1.0f);
        }
    }
    for (uint8_t i = 0; i < k_phase_bins; ++i) {
        const int16_t b = static_cast<int16_t>(bin + i) - 1;
        if (b >= 0 && b < bins_) {
            previous_[i] = bin_output(static_cast<uint8_t>(b));
        } else {
            previous_[i].re = 0.0f;
            previous_[i].im = 0.0f;
        }
    }
}

// The held (excluded) tone turned over: its product with its last strong block (at most k_held_gap_blocks before) lies
// within the break angle of a full reversal of the advance its frequency gives (bins are exact DFT bins: 2 pi f / 50 Hz
// per block), this block strong too; or, a flip inside this block, its second half against its first half (B conj(A),
// as half_product(), turning by pi (f - f_bin) / 50 Hz), both halves strong. A marker of the held signal nulls its own
// carrier in the block it falls in (its twist), so the turn often shows only against the block after it.
void ToneSearch::update_held() {
    held_reversed_ = false;
    held_unread_ = false;
    if (excluded_bin_ < 0) return;
    const uint8_t bin = static_cast<uint8_t>(excluded_bin_);
    const Complex current = bin_output(bin);
    const float current_power = current.re * current.re + current.im * current.im;
    const bool strong = current_power >= k_break_snr * floor_;
    const float strong_power = held_strong_.re * held_strong_.re + held_strong_.im * held_strong_.im;
    if (strong && strong_power > 0.0f && held_gap_ < k_held_gap_blocks) {
        const float re = current.re * held_strong_.re + current.im * held_strong_.im;
        const float im = current.im * held_strong_.re - current.re * held_strong_.im;
        const float advance = k_two_pi * excluded_hz_ * static_cast<float>(held_gap_ + 1u) /
                              static_cast<float>(k_search_step_hz);
        const float along = re * cosf(advance) + im * sinf(advance);
        if (along < -k_steady_cosine * sqrtf(re * re + im * im)) held_reversed_ = true;
    }
    // Halves: A from the middle output, B = sign y160 - A (sign = e^{jw 80}, see half_product()); a half block holds
    // half the noise of a block.
    const Complex& first = held_middle_;
    const float sign = ((first_hz_ / k_search_step_hz + bin) & 1u) != 0 ? -1.0f : 1.0f;
    Complex second;
    second.re = sign * current.re - first.re;
    second.im = sign * current.im - first.im;
    const float first_power = first.re * first.re + first.im * first.im;
    const float second_power = second.re * second.re + second.im * second.im;
    if (min_of(first_power, second_power) >= k_break_snr * k_half_bin * floor_) {
        const float re = second.re * first.re + second.im * first.im;
        const float im = second.im * first.re - second.re * first.im;
        const float bin_hz = static_cast<float>(first_hz_ + bin * k_search_step_hz);
        const float turn = k_pi * (excluded_hz_ - bin_hz) / static_cast<float>(k_search_step_hz);
        const float along = re * cosf(turn) + im * sinf(turn);
        if (along < -k_steady_cosine * sqrtf(re * re + im * im)) held_reversed_ = true;
    }
    held_unread_ = !strong && !held_reversed_;
    if (strong) {
        held_strong_ = current;
        held_gap_ = 0;
    } else if (held_gap_ < k_count_limit_u8) {
        ++held_gap_;
    }
    // A first break the held tone could not tell about is decided at its next reading (or not an echo after the wait).
    if (onset_pending_ && !held_unread_) {
        onset_echo_ = held_reversed_;
        onset_pending_ = false;
    } else if (onset_pending_ && ++onset_wait_ > k_held_gap_blocks) {
        onset_pending_ = false;
    }
}

// A marker's twist moves its beep's energy from the carrier bin into sidebands 0.75 / T away (spec 1.1), inside the
// exclusion around the held tone: any of those bins loud in this block.
bool ToneSearch::held_sidebands() const {
    for (int16_t b = static_cast<int16_t>(excluded_bin_ - excluded_span_); b <= excluded_bin_ + excluded_span_; ++b) {
        if (b < 0 || b >= bins_ || b == excluded_bin_) continue;
        if (block_power(static_cast<uint8_t>(b)) >= k_break_snr * floor_) return true;
    }
    return false;
}

// Strong products build the steady reference; once it rests on k_train_steady_products of them, it takes
// only the products that agree with it, a product clearly off its phase is a break, and the reference then
// stays as it was. The steady products before the first break are kept: noise lines up a few by chance (an FM
// receiver near threshold, whose de-emphasised noise is strongest at the low end of the band), a tune tone many.
void ToneSearch::update_breaks(const Complex& product, const Complex& half, float magnitude, float break_cosine) {
    const float along = product.re * steady_sum_.re + product.im * steady_sum_.im;
    const float reference = sqrtf(steady_sum_.re * steady_sum_.re + steady_sum_.im * steady_sum_.im);
    if (steady_products_ >= k_train_steady_products) {
        if (along < break_cosine * magnitude * reference) {
            if (breaks_ == 0) {
                onset_products_ = steady_products_;
                onset_echo_ = held_reversed_;
                // Undecided only when a marker of the held signal can have nulled its carrier in this very block: the
                // carrier bin weak, its sidebands loud. A held signal keyed off leaves them quiet too, and plays no
                // keying line that could break now.
                onset_pending_ = held_unread_ && held_sidebands();
                onset_wait_ = 0;
            }
            if (breaks_ < k_count_limit_u8) ++breaks_;
            return;
        }
        if (along < k_steady_cosine * magnitude * reference) return;  // neither steady nor a clear break
    }
    if (breaks_ > 0) return;
    steady_sum_.re += product.re;
    steady_sum_.im += product.im;
    steady_weight_ += magnitude;
    steady_half_sum_.re += half.re;
    steady_half_sum_.im += half.im;
    steady_half_weight_ += sqrtf(half.re * half.re + half.im * half.im);
    if (steady_products_ < k_count_limit_u8) ++steady_products_;
}

float ToneSearch::block_power(uint8_t bin) const {
    const int64_t s1 = s1_[bin];
    const int64_t s2 = s2_[bin];
    const int64_t cross = ((static_cast<int64_t>(coeff_q14_[bin]) * s1) >> k_goertzel_shift) * s2;
    return static_cast<float>(s1 * s1 + s2 * s2 - cross);
}

void ToneSearch::update_bins() {
    if (blocks_ < k_blocks_limit) ++blocks_;
    const float warmup = 1.0f / static_cast<float>(blocks_);
    const float fast_alpha = max_of(k_fast_alpha, warmup);
    const float slow_alpha = max_of(k_slow_alpha, warmup);
    Complex previous = dft_output(0);
    for (uint8_t b = 0; b < bins_; ++b) {
        const float power = block_power(b);
        fast_[b] += fast_alpha * (power - fast_[b]);
        slow_[b] += slow_alpha * (power - slow_[b]);
        slow_square_[b] += slow_alpha * (power * power - slow_square_[b]);
        if (b + 1 >= bins_) continue;
        const Complex next = dft_output(static_cast<uint8_t>(b + 1));
        const float re = previous.re - next.re;
        const float im = previous.im - next.im;
        fast_half_[b] += fast_alpha * (0.5f * (re * re + im * im) - fast_half_[b]);
        previous = next;
    }
}

// The larger of the bin's average and the half-bin averages on either side.
float ToneSearch::level(uint8_t bin) const {
    float value = fast_[bin];
    if (bin > 0) value = max_of(value, fast_half_[bin - 1]);
    if (bin + 1 < bins_) value = max_of(value, fast_half_[bin]);
    return value;
}

// The same for the current block alone.
float ToneSearch::block_level(uint8_t bin) const {
    float value = block_power(bin);
    const Complex centre = dft_output(bin);
    for (int8_t side = -1; side <= 1; side += 2) {
        const int16_t b = static_cast<int16_t>(bin + side);
        if (b < 0 || b >= bins_) continue;
        const Complex other = dft_output(static_cast<uint8_t>(b));
        const float re = centre.re - other.re;
        const float im = centre.im - other.im;
        value = max_of(value, 0.5f * (re * re + im * im));
    }
    return value;
}

void ToneSearch::update_floor() {
    previous_floor_ = floor_;
    float sorted[k_max_bins];
    for (uint8_t b = 0; b < bins_; ++b) {
        const float value = slow_[b];
        uint8_t i = b;
        while (i > 0 && sorted[i - 1] > value) {
            sorted[i] = sorted[i - 1];
            --i;
        }
        sorted[i] = value;
    }
    const uint8_t lower = static_cast<uint8_t>(bins_ / 2 > 0 ? bins_ / 2 : 1);
    float sum = 0.0f;
    for (uint8_t i = 0; i < lower; ++i) sum += sorted[i];
    const float averaged = min_of(static_cast<float>(blocks_), k_slow_blocks);
    const float bias = max_of(1.0f - k_lower_half_bias / sqrtf(averaged), k_lower_half_bias / k_slow_blocks);
    floor_ = max_of(sum / (static_cast<float>(lower) * bias), k_min_floor);

    for (uint8_t b = 0; b < bins_; ++b) sorted[b] = block_power(b);
    const float block_floor =
        max_of(lower_sum(sorted, bins_, lower) / (static_cast<float>(lower) * k_block_floor_bias), k_min_floor);
    recent_floor_ = recent_floor_ > 0.0f ? recent_floor_ + k_recent_alpha * (block_floor - recent_floor_) : block_floor;
}

void ToneSearch::update_bans() {
    for (uint8_t b = 0; b < bins_; ++b) {
        if (ban_[b] > 0) --ban_[b];
    }
    if (++since_strike_ < k_strike_decay_blocks) return;
    since_strike_ = 0;
    for (uint8_t b = 0; b < bins_; ++b) {
        if (strikes_[b] > 0) --strikes_[b];
    }
}

float ToneSearch::excess_variance(uint8_t bin) const {
    const float slow = slow_[bin];
    return (slow_square_[bin] - slow * slow) - (2.0f * slow * floor_ - floor_ * floor_);
}

bool ToneSearch::steady_carrier(uint8_t bin) const {
    if (blocks_ < k_steady_after_blocks || slow_[bin] < k_steady_level * floor_) return false;
    const float excess = slow_[bin] - floor_;
    return excess_variance(bin) < k_steady_max * excess * excess;
}

bool ToneSearch::local_peak(const float* power, uint8_t bin) const {
    if (bin > 0 && power[bin - 1] > power[bin]) return false;
    return bin + 1 >= bins_ || power[bin + 1] <= power[bin];
}

bool ToneSearch::eligible(uint8_t bin) const {
    if (bin == 0 || bin + 1u >= bins_) return false;  // the guard bins
    if (excluded_bin_ >= 0 && bin <= excluded_bin_ + excluded_span_ && bin + excluded_span_ >= excluded_bin_) {
        return false;
    }
    return ban_[bin] == 0 && !steady_carrier(bin);
}

void ToneSearch::update_lock() {
    int16_t fast_bin = -1;
    float fast_best = 0.0f;
    int16_t slow_bin = -1;
    float slow_best = 0.0f;
    const float keyed = k_keyed_min * floor_ * floor_;
    const float lock_floor = max_of(floor_, recent_floor_);
    for (uint8_t b = 0; b < bins_; ++b) {
        if (!eligible(b)) continue;
        // Only a local peak (neighbours counted even when banned): the shoulder of a banned tone is not a tone. The
        // half-bin powers count in quiet bins only: elsewhere (FM noise after de-emphasis) the larger of three noisy
        // averages would let the noise outrank a tone.
        const bool quiet = slow_[b] <= k_quiet_bin * floor_;
        const float lock = quiet ? k_fast_lock_quiet : k_fast_lock;
        const float value = quiet ? level(b) : fast_[b];
        if (value >= lock * lock_floor && value > fast_best && local_peak(fast_, b)) {
            fast_best = value;
            fast_bin = b;
        }
        if (blocks_ >= k_slow_after_blocks && slow_[b] >= k_slow_lock * floor_ && excess_variance(b) >= keyed &&
            slow_[b] > slow_best && local_peak(slow_, b)) {
            slow_best = slow_[b];
            slow_bin = b;
        }
    }
    Lock mode = Lock::none;
    int16_t bin = -1;
    // The tone must also be in the current block: the fast average still holds the end of a transmission
    // that has just finished when the search restarts.
    if (blocks_ >= k_warmup_blocks && fast_bin >= 0 &&
        block_level(static_cast<uint8_t>(fast_bin)) >= k_present_lock * lock_floor) {
        mode = Lock::fast;
        bin = fast_bin;
    } else if (slow_bin >= 0) {
        mode = Lock::slow;
        bin = slow_bin;
    }
    const bool near = bin - best_bin_ <= 1 && best_bin_ - bin <= 1;
    const bool continued = mode != Lock::none && mode == lock_ && stable_blocks_ > 0 && near;
    // The tune tone ramps down into the first marker: a fast run that drops out for a block or two on the
    // same tone keeps its steady reference, so the train's first reversal is still seen against it.
    const bool resumed = mode == Lock::fast && !continued && near && fast_gap_ <= k_resume_blocks;
    if (mode == Lock::fast) {
        fast_gap_ = 0;
    } else if (fast_gap_ < k_count_limit_u8) {
        ++fast_gap_;
    }
    if (mode == Lock::none) {
        stable_blocks_ = 0;
    } else if (continued) {
        if (stable_blocks_ < k_long_run_blocks) ++stable_blocks_;
    } else {
        stable_blocks_ = 1;
    }
    lock_level_ = mode == Lock::fast ? fast_best / lock_floor : k_fast_lock;
    if (mode != Lock::none) update_phase(continued, resumed, static_cast<uint8_t>(bin));
    lock_ = mode;
    if (bin >= 0) best_bin_ = static_cast<uint8_t>(bin);
    if (!continued) onset_floor_ = previous_floor_;  // the run's tone is not yet in the floor before its start
    // The tune's tone, from the steady reference (it survives a short dropout and stops at the first break).
    const float reference = sqrtf(steady_sum_.re * steady_sum_.re + steady_sum_.im * steady_sum_.im);
    if (lock_ == Lock::fast && breaks_ == 0 && steady_products_ > 0 && reference >= k_min_phase_coherence * steady_weight_) {
        const float offset = steady_offset(fast_, steady_sum_, steady_half_sum_, steady_half_weight_, power_offset(fast_));
        steady_tone_hz_ = static_cast<float>(first_hz_) + (static_cast<float>(best_bin_) + offset) * k_search_step_hz;
    }
}

// Rectangular window: neighbour/peak amplitude ratio r gives the offset r / (1 + r) bins.
float ToneSearch::power_offset(const float* power) const {
    const uint8_t b = best_bin_;
    const float centre = sqrtf(max_of(power[b] - floor_, 0.0f));
    const float left = b > 0 ? sqrtf(max_of(power[b - 1] - floor_, 0.0f)) : 0.0f;
    const float right = b + 1 < bins_ ? sqrtf(max_of(power[b + 1] - floor_, 0.0f)) : 0.0f;
    if (right >= left && centre + right > 0.0f) return right / (centre + right);
    if (centre + left > 0.0f) return -left / (centre + left);
    return 0.0f;
}

// The balance of the two neighbours over the three bins (floor subtracted), in -1..1: 0 for a spectrum symmetric about
// best_bin_ (keyed data spreads a tone's power evenly into both neighbours, which power_offset() reads as a third of a
// bin off), and on the tone's side of best_bin_ for a steady tone between bins.
float ToneSearch::centre_offset(const float* power) const {
    const uint8_t b = best_bin_;
    const float centre = max_of(power[b] - floor_, 0.0f);
    const float left = b > 0 ? max_of(power[b - 1] - floor_, 0.0f) : 0.0f;
    const float right = b + 1 < bins_ ? max_of(power[b + 1] - floor_, 0.0f) : 0.0f;
    const float total = left + centre + right;
    return total > 0.0f ? (right - left) / total : 0.0f;
}

// Least-squares fit (best amplitude) of the rectangular-window pattern sinc^2 to the three bins around
// the peak, for a tone at `offset` bins from best_bin_; larger is better.
float ToneSearch::pattern_fit(const float* power, float offset) const {
    float cross = 0.0f;
    float norm = 0.0f;
    for (int8_t i = -1; i <= 1; ++i) {
        const int16_t b = static_cast<int16_t>(best_bin_ + i);
        if (b < 0 || b >= bins_) continue;
        const float distance = static_cast<float>(i) - offset;
        const float argument = k_pi * distance;
        const float expected = fabsf(argument) < k_tiny ? 1.0f : sinf(argument) / argument;
        const float pattern = expected * expected;
        cross += max_of(power[b] - floor_, 0.0f) * pattern;
        norm += pattern * pattern;
    }
    return cross > 0.0f && norm > 0.0f ? cross * cross / norm : 0.0f;
}

float ToneSearch::coherence() const {
    if (phase_weight_ <= 0.0f) return 0.0f;
    return sqrtf(phase_sum_.re * phase_sum_.re + phase_sum_.im * phase_sum_.im) / phase_weight_;
}

bool ToneSearch::steady() const {
    return lock_ == Lock::fast && coherence() >= k_min_phase_coherence;
}

bool ToneSearch::candidate(float& tone_hz) const {
    const uint8_t needed = lock_level_ < k_fast_lock ? k_long_run_blocks : k_lock_blocks;
    if (stable_blocks_ < needed || lock_ == Lock::none) return false;
    tone_hz = estimate_tone();
    return inside(tone_hz);
}

// A tone just outside the range leaks into the edge bin: the estimate, not the bin, decides, within its accuracy (a
// tone on the range's edge may read a few Hz beyond it).
bool ToneSearch::inside(float tone_hz) const {
    return tone_hz >= static_cast<float>(min_hz_) - k_estimate_accuracy_hz &&
           tone_hz <= static_cast<float>(max_hz_) + k_estimate_accuracy_hz;
}

bool ToneSearch::long_run() const {
    return lock_ != Lock::none && stable_blocks_ >= k_long_run_blocks;
}

bool ToneSearch::leading(float& tone_hz) const {
    if (blocks_ == 0) return false;
    const float lock_floor = max_of(floor_, recent_floor_);
    int16_t best = -1;
    float best_value = 0.0f;
    for (uint8_t b = 0; b < bins_; ++b) {
        if (!eligible(b)) continue;
        const bool quiet = slow_[b] <= k_quiet_bin * floor_;
        const float value = quiet ? level(b) : fast_[b];
        if (value < (quiet ? k_fast_lock_quiet : k_fast_lock) * lock_floor || value <= best_value ||
            !local_peak(fast_, b)) {
            continue;
        }
        best = b;
        best_value = value;
    }
    if (best < 0) return false;
    tone_hz = static_cast<float>(first_hz_ + best * k_search_step_hz);
    return true;
}

bool ToneSearch::train_onset(float& tone_hz, uint8_t min_products) const {
    if (lock_ != Lock::fast || steady_tone_hz_ <= 0.0f || breaks_ < k_train_breaks || onset_products_ < min_products ||
        onset_pending_) {
        return false;
    }
    const float reference = sqrtf(steady_sum_.re * steady_sum_.re + steady_sum_.im * steady_sum_.im);
    if (reference < k_min_phase_coherence * steady_weight_) return false;  // the reference was not a steady tone
    tone_hz = steady_tone_hz_;
    return inside(tone_hz);
}

// A steady tone (the tune tone): the phase of the block-to-block product is the offset modulo one bin
// (the same in every bin: 2 pi f / 50 Hz per block). The half-block products pick the alias when they are coherent
// (pi per bin of offset), otherwise the power pattern does.
float ToneSearch::steady_offset(const float* power, const Complex& product, const Complex& half, float half_weight,
                                float fallback) const {
    const float phase = atan2f(product.im, product.re) / k_two_pi;
    float half_estimate = 0.0f;
    const bool by_half = half_offset(half, half_weight, half_estimate);
    float offset = fallback;
    float best_fit = k_no_score;
    for (int8_t whole = -1; whole <= 1; ++whole) {
        const float alias = phase + static_cast<float>(whole);
        const float bin = static_cast<float>(best_bin_) + alias;
        if (alias < -k_max_alias || alias > k_max_alias || bin < -k_parabola_limit ||
            bin > static_cast<float>(bins_ - 1) + k_parabola_limit) {
            continue;
        }
        const float fit = by_half ? cosf(k_pi * (half_estimate - alias)) : pattern_fit(power, alias);
        if (fit > best_fit) {
            best_fit = fit;
            offset = alias;
        }
    }
    return offset;
}

float ToneSearch::estimate_tone() const {
    const float* power = lock_ == Lock::fast ? fast_ : slow_;
    float offset = power_offset(power);
    if (steady()) {
        // Marker reversals or gaps make the products incoherent; the power interpolation is kept then and
        // the fine AFC pulls in the rest.
        offset = steady_offset(power, phase_sum_, half_sum_, half_weight_, offset);
    } else if (phase_weight_ > 0.0f) {
        // Data with reversals and gaps: the doubled phase is exact modulo half a bin for a steady tone (keyed data
        // off a bin spreads it by some 15 Hz); the alias nearest the reference is taken. The reference is the
        // neighbours' balance, centred for keyed data (whose power spreads evenly into both neighbours; the power
        // interpolation reads it a third of a bin off). A tone steady within its blocks (a weak tune whose products
        // look incoherent) gives the half-block estimate instead, which a marker reversal between the halves of a
        // block moves by a whole bin: it is taken only within half a bin of the balance. The decoder does not rely on
        // this estimate for a lock on data: its fine AFC measures the tone against the provisional tune (spec 3.6).
        const float balance = centre_offset(power);
        float reference = balance;
        float half = 0.0f;
        if (half_offset(half_sum_, half_weight_, half) && fabsf(half - balance) <= k_half_bin) reference = half;
        const float phase = atan2f(phase_square_sum_.im, phase_square_sum_.re) / (2.0f * k_two_pi);
        float best = phase;
        for (int8_t half_bins = -k_phase_aliases; half_bins <= k_phase_aliases; ++half_bins) {
            const float alias = phase + k_half_bin * static_cast<float>(half_bins);
            if (fabsf(alias - reference) < fabsf(best - reference)) best = alias;
        }
        offset = best;
    }
    return static_cast<float>(first_hz_) + (static_cast<float>(best_bin_) + offset) * k_search_step_hz;
}

float ToneSearch::floor() const {
    return floor_;
}

float ToneSearch::recent_floor() const {
    return recent_floor_;
}

float ToneSearch::onset_floor() const {
    return onset_floor_;
}

// The tone lies within half a bin of its bin; a marker train at T adds lines 1 / (2 T) away (125 Hz at 4 ms):
// they must not pass for a new tone while the train is being acquired. Wider exclusions would hide a
// real tone next to a carrier that holds ACQUIRE.
void ToneSearch::exclude(float tone_hz, float sideband_hz) {
    excluded_bin_ = bin_of(tone_hz);
    excluded_span_ = static_cast<int16_t>(round_to_int(k_half_bin + sideband_hz / k_search_step_hz));
    excluded_hz_ = tone_hz;
    held_strong_.re = 0.0f;
    held_strong_.im = 0.0f;
    held_middle_ = held_strong_;
    held_gap_ = k_count_limit_u8;
    held_reversed_ = false;
    held_unread_ = false;
}

void ToneSearch::clear_exclusion() {
    excluded_bin_ = -1;
    excluded_span_ = 0;
    held_reversed_ = false;
    held_unread_ = false;
}

bool ToneSearch::onset_echo() const {
    return onset_echo_;
}

int16_t ToneSearch::bin_of(float tone_hz) const {
    const int32_t bin = round_to_int((tone_hz - static_cast<float>(first_hz_)) / k_search_step_hz);
    return bin >= 0 && bin < bins_ ? static_cast<int16_t>(bin) : static_cast<int16_t>(-1);
}

bool ToneSearch::masked(float tone_hz) const {
    const int16_t bin = bin_of(tone_hz);
    return bin >= 0 && steady_carrier(static_cast<uint8_t>(bin));
}

bool ToneSearch::present(float tone_hz) const {
    const int16_t bin = bin_of(tone_hz);
    return bin >= 0 && level(static_cast<uint8_t>(bin)) >= k_fast_lock_quiet * max_of(floor_, recent_floor_);
}

void ToneSearch::ban(float tone_hz, uint16_t base_blocks) {
    const int16_t bin = bin_of(tone_hz);
    if (bin < 0) return;
    const uint8_t shift = strikes_[bin] < k_max_strike_shift ? strikes_[bin] : k_max_strike_shift;
    uint32_t duration = static_cast<uint32_t>(base_blocks) << shift;
    if (duration > k_ban_limit) duration = k_ban_limit;
    for (int16_t b = static_cast<int16_t>(bin - 1); b <= bin + 1; ++b) {
        if (b < 0 || b >= bins_) continue;
        if (ban_[b] < duration) ban_[b] = static_cast<uint16_t>(duration);
    }
    if (strikes_[bin] < k_strike_limit) ++strikes_[bin];
    since_strike_ = 0;
}

// ---------------------------------------------------------------------------
// FineAfc
// ---------------------------------------------------------------------------

FineAfc::FineAfc() {
    configure(k_afc_default_rate_hz);
}

float FineAfc::bin_hz(uint8_t bin) {
    const int8_t index = static_cast<int8_t>(static_cast<int8_t>(bin) - k_afc_centre_bin);
    const int8_t distance = index < 0 ? static_cast<int8_t>(-index) : index;
    float hz = static_cast<float>(distance) * k_afc_bin_step_hz;
    if (distance > k_afc_fine_bins) {
        hz = static_cast<float>(k_afc_fine_bins) * k_afc_bin_step_hz +
             static_cast<float>(distance - k_afc_fine_bins) * k_afc_outer_step_hz;
    }
    return index < 0 ? -hz : hz;
}

// Leak of a bin: the outer bins are wider so that a tone between them loses about 3 dB, not 14.
float FineAfc::bin_lambda(uint8_t bin) {
    const int8_t index = static_cast<int8_t>(static_cast<int8_t>(bin) - k_afc_centre_bin);
    const int8_t distance = index < 0 ? static_cast<int8_t>(-index) : index;
    return distance > k_afc_fine_bins ? k_afc_outer_lambda : k_afc_lambda;
}

void FineAfc::configure(float input_rate_hz) {
    for (uint8_t k = 0; k < k_bins; ++k) {
        const float hz = bin_hz(k);
        const float angle = k_two_pi * hz / input_rate_hz;
        rotator_[k].re = bin_lambda(k) * cosf(angle);
        rotator_[k].im = bin_lambda(k) * sinf(angle);
    }
    reset();
}

void FineAfc::reset() {
    memset(accumulator_, 0, sizeof(accumulator_));
    count_ = 0;
}

void FineAfc::push(const Complex& decimated) {
    const float square_re = decimated.re * decimated.re - decimated.im * decimated.im;
    const float square_im = 2.0f * decimated.re * decimated.im;
    for (uint8_t k = 0; k < k_bins; ++k) {
        Complex& acc = accumulator_[k];
        const Complex& rot = rotator_[k];
        const float re = rot.re * acc.re - rot.im * acc.im + square_re;
        const float im = rot.re * acc.im + rot.im * acc.re + square_im;
        acc.re = re;
        acc.im = im;
    }
    if (count_ < k_count_limit) ++count_;
}

uint16_t FineAfc::inputs() const {
    return count_;
}

bool FineAfc::offset(float& tone_offset_hz, bool wide) const {
    if (count_ < k_afc_min_inputs) return false;
    // Narrow: tone +-16.7 Hz, the spec range (a tune tone lock is a few Hz off); wide: +-30 Hz.
    const uint8_t first = wide ? 0 : static_cast<uint8_t>(k_afc_centre_bin - k_afc_narrow_bins);
    const uint8_t last = wide ? static_cast<uint8_t>(k_bins - 1) : static_cast<uint8_t>(k_afc_centre_bin + k_afc_narrow_bins);
    float power[k_bins];
    uint8_t peak = first;
    for (uint8_t k = 0; k < k_bins; ++k) {
        // on-bin gain after count_ inputs: (1 - lambda^N) / (1 - lambda); bins have different lambdas
        const float lambda = bin_lambda(k);
        const float gain = (1.0f - powf(lambda, static_cast<float>(count_))) / (1.0f - lambda);
        power[k] = (accumulator_[k].re * accumulator_[k].re + accumulator_[k].im * accumulator_[k].im) / (gain * gain);
        if (k >= first && k <= last && power[k] > power[peak]) peak = k;
    }
    // Peak against the bins away from it: shortly after a reset the main lobe still spans several bins.
    float total = 0.0f;
    uint8_t count = 0;
    for (uint8_t k = first; k <= last; ++k) {
        if (k + k_afc_lobe_bins >= peak && k <= peak + k_afc_lobe_bins) continue;
        total += power[k];
        ++count;
    }
    if (power[peak] <= 0.0f || count == 0 || power[peak] < k_afc_peak_ratio * total / static_cast<float>(count)) {
        return false;
    }
    float squared_hz = bin_hz(peak);
    if (peak > 0 && peak + 1 < k_bins) {
        // The leaky DFT response is Lorentzian: 1/|acc|^2 is a parabola in frequency (bins may be uneven).
        const float x0 = bin_hz(static_cast<uint8_t>(peak - 1)) - squared_hz;
        const float x2 = bin_hz(static_cast<uint8_t>(peak + 1)) - squared_hz;
        const float y0 = 1.0f / max_of(power[peak - 1], k_tiny);
        const float y1 = 1.0f / power[peak];
        const float y2 = 1.0f / max_of(power[peak + 1], k_tiny);
        const float numerator = x0 * x0 * (y1 - y2) - x2 * x2 * (y1 - y0);
        const float denominator = x0 * (y1 - y2) - x2 * (y1 - y0);
        if (denominator != 0.0f) squared_hz += clamp(0.5f * numerator / denominator, 0.5f * x0, 0.5f * x2);
    }
    tone_offset_hz = squared_hz * k_squared_to_tone;
    return true;
}

// ---------------------------------------------------------------------------
// CandidateList
// ---------------------------------------------------------------------------

CandidateList::CandidateList() {
    reset();
}

void CandidateList::reset() {
    memset(items_, 0, sizeof(items_));
    head_ = 0;
    count_ = 0;
}

// A stronger detection that replaces the newest entry keeps the entry's finest position when that came from a finer
// scale: a chain of ever wider echoes, each within k_candidate_merge_blocks of the last, moves the position but not it.
bool CandidateList::add(const Candidate& candidate) {
    if (count_ > 0) {
        Candidate& last = items_[head_];
        const float distance =
            static_cast<float>(as_signed(candidate.block - last.block)) + candidate.fraction - last.fraction;
        if (distance <= k_candidate_merge_blocks && distance >= -k_candidate_merge_blocks) {
            const bool stronger = candidate.q > last.q;
            const bool finer = candidate.finest_scale < last.finest_scale;
            if (!stronger && !finer) return false;
            // Finest positions, in blocks after the entry's block before the merge.
            const uint32_t base = last.block;
            const float kept = last.fraction + finest(last);
            const float offered = distance + last.fraction + finest(candidate);
            const float at = finer ? offered : kept;
            const uint8_t finest_scale = finer ? candidate.finest_scale : last.finest_scale;
            if (stronger) {
                last.block = candidate.block;
                last.fraction = candidate.fraction;
                last.q = candidate.q;
                last.scale = candidate.scale;
            }
            last.finest_scale = finest_scale;
            const float from = static_cast<float>(as_signed(last.block - base)) + last.fraction;
            last.finest_offset = static_cast<int16_t>(round_to_int((at - from) * k_candidate_offset_scale));
            return stronger;
        }
    }
    head_ = static_cast<uint8_t>((head_ + 1) % k_size);
    items_[head_] = candidate;
    if (count_ < k_size) ++count_;
    return true;
}

float CandidateList::finest(const Candidate& candidate) {
    return static_cast<float>(candidate.finest_offset) / k_candidate_offset_scale;
}

uint8_t CandidateList::count() const {
    return count_;
}

const Candidate& CandidateList::newest(uint8_t age) const {
    return items_[(head_ + k_size - age % k_size) % k_size];
}

// ---------------------------------------------------------------------------
// AuditRing
// ---------------------------------------------------------------------------

AuditRing::AuditRing() {
    reset(k_max_positions);
}

void AuditRing::reset(uint8_t positions) {
    memset(evidence_, 0, sizeof(evidence_));
    positions_ = positions < k_max_positions ? positions : k_max_positions;
    head_ = 0;
    count_ = 0;
}

void AuditRing::set(uint8_t position, float evidence) {
    if (position >= positions_) return;
    evidence_[head_][position] = static_cast<int8_t>(round_to_int(evidence * static_cast<float>(k_audit_scale)));
}

void AuditRing::next_package() {
    head_ = static_cast<uint8_t>((head_ + 1) % (k_packages + 1));
    memset(evidence_[head_], 0, sizeof(evidence_[head_]));
    if (count_ < k_packages) ++count_;
}

float AuditRing::max_evidence() const {
    int16_t best = 0;
    for (uint8_t k = 0; k < positions_; ++k) {
        int16_t sum = 0;
        for (uint8_t p = 1; p <= count_; ++p) {
            sum = static_cast<int16_t>(sum + evidence_[(head_ + k_packages + 1 - p) % (k_packages + 1)][k]);
        }
        if (k == 0 || sum > best) best = sum;
    }
    return static_cast<float>(best) / static_cast<float>(k_audit_scale);
}

uint8_t AuditRing::packages() const {
    return count_;
}

uint8_t AuditRing::positions() const {
    return positions_;
}

// ---------------------------------------------------------------------------
// PackageLearner (spec 3.8)
// ---------------------------------------------------------------------------

PackageLearner::PackageLearner() {
    reset(0, 0);
}

void PackageLearner::reset(int32_t train_index, uint8_t train_ones, int32_t min_start) {
    train_index_ = train_index;
    min_start_ = min_start;
    first_marker_ = k_no_marker;
    last_marker_ = train_index;
    candidate_start_ = train_index;
    candidate_bits_ = 0;
    faded_bits_ = 0;
    unsupported_gap_ = 0;
    train_ones_ = train_ones;
    rejections_ = 0;
    faded_start_ = false;
    start_faded_ = false;
    confirmed_ = false;
}

void PackageLearner::extend_train(int32_t index, bool adjacent) {
    if (adjacent && train_ones_ < k_gap_limit) ++train_ones_;
    train_index_ = index;
    last_marker_ = index;
    first_marker_ = k_no_marker;
    candidate_bits_ = 0;
    faded_bits_ = 0;
    faded_start_ = false;
    start_faded_ = false;
    unsupported_gap_ = 0;
}

// Data slots never flip: after the train, the first marker that is not one slot away is a STOP, and a candidate N is
// confirmed when the next span repeats it.
LearnStep PackageLearner::push(int32_t index) {
    const int32_t gap = index - last_marker_;
    if (gap <= 1) {
        extend_train(index, true);
        return LearnStep::train;
    }
    const bool first_gap = first_marker_ == k_no_marker;
    // Package 0 cannot start before the train's end: a train that stops short of min_start_ faded there, and so did
    // the first START.
    const int32_t start = first_gap && min_start_ > last_marker_ ? min_start_ : last_marker_;
    const int32_t bits = index - start - 1;
    if (bits < 1) {
        extend_train(index, false);
        return LearnStep::train;
    }
    last_marker_ = index;
    const uint8_t clipped_gap = static_cast<uint8_t>(bits + 1 > k_gap_limit ? k_gap_limit : bits + 1);
    if (first_gap) first_marker_ = index;
    if (candidate_bits_ > 0) {
        if (bits == candidate_bits_) {
            confirmed_ = true;
            faded_start_ = false;
            return LearnStep::confirmed;
        }
        if (faded_bits_ > 0 && bits == faded_bits_) {
            confirmed_ = true;
            faded_start_ = true;
            candidate_bits_ = faded_bits_;
            candidate_start_ = train_index_ + 1;
            return LearnStep::confirmed;
        }
        if (rejections_ < k_gap_limit) ++rejections_;
        candidate_bits_ = 0;
        faded_bits_ = 0;
    } else if (bits > k_max_bits_per_package && unsupported_gap_ == clipped_gap) {
        return LearnStep::unsupported;
    }
    if (bits > k_max_bits_per_package) {
        unsupported_gap_ = clipped_gap;
        return LearnStep::rejected;
    }
    unsupported_gap_ = 0;
    candidate_bits_ = static_cast<uint8_t>(bits);
    candidate_start_ = start;
    start_faded_ = start != index - gap;
    // Only the first package may start on a faded marker: the train's last marker, the first START.
    faded_bits_ = first_gap && !start_faded_ && bits >= 2 ? static_cast<uint8_t>(bits - 1) : 0;
    return LearnStep::candidate;
}

uint8_t PackageLearner::bits() const {
    return candidate_bits_;
}

uint8_t PackageLearner::faded_bits() const {
    return confirmed_ ? 0 : faded_bits_;
}

bool PackageLearner::faded_start() const {
    return faded_start_;
}

bool PackageLearner::start_faded() const {
    return start_faded_;
}

int32_t PackageLearner::candidate_start() const {
    return candidate_start_;
}

int32_t PackageLearner::start_base() const {
    return min_start_ > train_index_ ? min_start_ : train_index_;
}

// The reading with the fewest faded markers: package 0 starts a whole number of packages before the confirmed START,
// the first marker after L that the two equal spans vouch for (a noise flip before it may have formed a candidate).
int32_t PackageLearner::first_start() const {
    const int32_t span = static_cast<int32_t>(candidate_bits_) + 1;
    if (candidate_bits_ == 0) return candidate_start_;
    return candidate_start_ - span * ((candidate_start_ - start_base()) / span);
}

bool PackageLearner::start_exact() const {
    return candidate_bits_ >= 2 ||
           (first_marker_ != k_no_marker && first_marker_ - start_base() <= k_exact_start_slots);
}

uint8_t PackageLearner::rejections() const {
    return rejections_;
}

uint8_t PackageLearner::train_ones() const {
    return train_ones_;
}

int32_t PackageLearner::min_start() const {
    return min_start_;
}

int32_t PackageLearner::first_marker() const {
    return first_marker_;
}

void PackageLearner::shift_candidate(int32_t slots) {
    candidate_start_ += slots;
    last_marker_ += slots;
}

int32_t PackageLearner::train_index() const {
    return train_index_;
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

float noise_samples(float blocks, uint8_t block_samples) {
    const float effective = max_of(blocks - k_cic_overlap, 0.5f * blocks);
    return effective * static_cast<float>(block_samples);
}

FlipMeasure flip_measure(const Complex& before, const Complex& after, float noise_energy, float half_samples) {
    FlipMeasure m;
    const float cross = before.re * after.re + before.im * after.im;
    const float energy_before = before.re * before.re + before.im * before.im;
    const float energy_after = after.re * after.re + after.im * after.im;
    const float noise = max_of(noise_energy, k_tiny);
    const float total = energy_before + energy_after;
    m.q = -cross / noise;
    m.q_balanced = m.q - fabsf(energy_before - energy_after) / (2.0f * noise);
    m.kappa = total > 0.0f ? -2.0f * cross / total : 0.0f;
    const float diff_re = before.re - after.re;
    const float diff_im = before.im - after.im;
    const float sum_re = before.re + after.re;
    const float sum_im = before.im + after.im;
    m.amplitude = sqrtf(diff_re * diff_re + diff_im * diff_im) / max_of(half_samples * k_g_marker, k_tiny);
    m.steady = sqrtf(sum_re * sum_re + sum_im * sum_im) / max_of(half_samples * k_g_marker, k_tiny);
    m.energy = 0.5f * total / noise;
    // phase of -S_a * conj(S_b): the carrier advance over one half window
    m.phase_step = atan2f(-(after.im * before.re - after.re * before.im), -cross);
    m.position = 0.0f;
    m.valid = true;
    return m;
}

float parabolic_offset(float left, float centre, float right) {
    const float curvature = left - 2.0f * centre + right;
    if (curvature == 0.0f) return 0.0f;
    return clamp(0.5f * (left - right) / curvature, -k_parabola_limit, k_parabola_limit);
}

float equal_likelihood_ratio(float a_squared) {
    // Below 2 pi a^2 rho_max = 1 the equation has no solution in range (the signal is below the noise).
    if (k_two_pi * a_squared * k_rho_max <= 1.0f) return k_rho_max;
    float rho = k_rho_seed;
    for (uint8_t i = 0; i < k_rho_iterations; ++i) {
        rho = clamp(k_rho_offset + logf(k_two_pi * a_squared * rho) / (2.0f * a_squared), k_rho_min, k_rho_max);
    }
    return rho;
}

}  // namespace dsp
}  // namespace unlimited
