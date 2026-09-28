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
const uint8_t ToneSearch::k_quiet_slots;
const uint8_t ToneSearch::k_phase_bins;

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

// ImpulseBlanker (spec 3.1, v0.3's).
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

// ToneSearch (spec 3.2, v0.3's). Bins sit on whole multiples of k_search_step_hz: they are then exact DFT bins of the
// 160-sample block, which the phase estimates rely on.
const uint16_t k_search_step_hz = 50;
const float k_estimate_accuracy_hz = 5.0f;  // of estimate_tone() for a tone between bins
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
// A weak tone between two bins reaches 6 x the floor late: in a bin whose slow average is still near the floor it
// locks at 4 x. A bin whose noise stands above the floor (FM noise after de-emphasis, a receiver's audio slope) keeps
// the 6 x.
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
const uint16_t k_never = 0xFFFF;             // quiet counters saturate here
const float k_max_alias = 0.75f;           // the tone lies within 3/4 bin of the strongest bin
const int8_t k_phase_aliases = 2;          // doubled phase: offsets phase + m / 2, m = -2..2
const float k_half_bin = 0.5f;
const float k_min_phase_coherence = 0.9f;  // |sum of products| / sum of |products|: a steady tone
const float k_no_score = -1e30f;
const float k_us_per_s = 1e6f;
const float k_lead_z = 4.75f;              // leading bin: noise alone above the threshold with probability 1e-6
const float k_wilson_third = 1.0f / 9.0f;

const float k_parabola_limit = 0.5f;

// PrefixHistory::rotate: the rotator steps by one block's angle; it is recomputed from the angle this often, so its
// float rounding never adds up (spec 3.1).
const uint16_t k_rotate_reseed_blocks = 64;
const uint32_t k_history_mask = k_history_cells - 1u;

// The smart line (spec 3.5): rho = 0.5 + ln(2 pi a^2 rho) / (2 a^2), iterated from 0.6.
const float k_rho_min = 0.50f;
const float k_rho_max = 0.75f;
const float k_rho_seed = 0.6f;
const uint8_t k_rho_iterations = 3;
const float k_rho_offset = 0.5f;

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

// In-bin energy around the block, k-2..k+2. A beep covers at least 8 blocks: its blocks are in-bin, an impulse is
// broadband.
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
    head_ = static_cast<uint16_t>((head_ + 1u) & k_history_mask);
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

// Block j (P[j + 1] - P[j]), j >= first, is multiplied by e^{i a_j}, a_j = newest_radians + angle_per_block (newest -
// j): each block value is rebuilt from the prefixes, turned, rounded to int32 and summed again from P[first], which
// stays; the prefixes before it are forgotten.
void PrefixHistory::rotate(uint32_t first, float newest_radians, float angle_per_block) {
    if (!holds(first)) first = first_block();
    const uint16_t blocks = static_cast<uint16_t>(end_block_ - first);
    fill_ = static_cast<uint16_t>(blocks + 1u);
    if (blocks == 0) return;
    uint16_t at = cell(first);
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
        at = static_cast<uint16_t>((at + 1u) & k_history_mask);
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
    return static_cast<uint16_t>((head_ - (end_block_ - block)) & k_history_mask);
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

bool PrefixHistory::block(uint32_t index, Complex& value) const {
    if (!holds(index) || !holds(index + 1)) return false;
    const uint16_t from = cell(index);
    const uint16_t to = cell(index + 1);
    value.re = static_cast<float>(as_signed(re_[to] - re_[from])) / k_mixer_gain;
    value.im = static_cast<float>(as_signed(im_[to] - im_[from])) / k_mixer_gain;
    return true;
}

bool PrefixHistory::any_blanked(uint32_t origin_block, float from, float to) const {
    const uint32_t first = origin_block + static_cast<uint32_t>(static_cast<int32_t>(floorf(from)));
    const uint32_t last = origin_block + static_cast<uint32_t>(static_cast<int32_t>(ceilf(to)));
    for (uint32_t index = first; index != last; ++index) {
        if (holds(index + 1) && blanked_bit(cell(index + 1))) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Lookahead
// ---------------------------------------------------------------------------

Lookahead::Lookahead() : delay_(0), head_(0) {
    memset(line_, 0, sizeof(line_));
}

void Lookahead::configure(uint16_t delay) {
    delay_ = delay < k_lookahead_max_samples ? delay : k_lookahead_max_samples;
    head_ = 0;
    memset(line_, 0, sizeof(line_));
}

int16_t Lookahead::push(int16_t sample) {
    if (delay_ == 0) return sample;
    const int16_t out = line_[head_];
    line_[head_] = sample;
    head_ = static_cast<uint16_t>(head_ + 1u == delay_ ? 0u : head_ + 1u);
    return out;
}

uint16_t Lookahead::delay() const {
    return delay_;
}

// ---------------------------------------------------------------------------
// ToneSearch
// ---------------------------------------------------------------------------

ToneSearch::ToneSearch() {
    configure(k_min_tone_hz, k_max_tone_hz, k_min_slot_us);
}

// Bins on the multiples of 50 Hz inside min_hz..max_hz (at least one), and a guard bin on each side: a tone just outside
// the range peaks in a guard, where no lock is taken, instead of in the edge bin at a wrong 50 Hz alias.
void ToneSearch::configure(uint16_t min_hz, uint16_t max_hz, uint32_t slot_us) {
    min_hz_ = min_hz;
    max_hz_ = max_hz;
    // The leading bin's average spans about one slot (a START alone lifts it at slow speeds), never fewer than the
    // fast average's 8 blocks. An exponential average with alpha of noise is close to Gamma(k, 1/k), k = (2 - alpha) /
    // alpha: its 1e-6 tail by Wilson-Hilferty.
    const float slot_blocks = static_cast<float>(slot_us) * static_cast<float>(k_decoder_rate_hz) /
                              (k_us_per_s * static_cast<float>(k_block_samples));
    lead_alpha_ = slot_blocks > 1.0f ? max_of(k_fast_alpha, 1.0f / slot_blocks) : k_fast_alpha;
    quiet_blocks_ = static_cast<uint16_t>(ceilf(static_cast<float>(k_quiet_slots) * slot_blocks));
    const float shape = (2.0f - lead_alpha_) / lead_alpha_;
    const float root = 1.0f - k_wilson_third / shape + k_lead_z * sqrtf(k_wilson_third / shape);
    lead_ratio_ = root * root * root;
    // The lock bins are the ones nearest to min_hz and max_hz and those between: a tone on the range's edge has its
    // nearest bin among them (the guard bin beside it would otherwise take its power and leave no local peak).
    const uint16_t half_step = k_search_step_hz / 2u;
    const uint16_t first_lock = static_cast<uint16_t>((min_hz + half_step) / k_search_step_hz * k_search_step_hz);
    const uint16_t last_lock = static_cast<uint16_t>((max_hz + half_step) / k_search_step_hz * k_search_step_hz);
    const uint16_t span_bins =
        static_cast<uint16_t>(last_lock >= first_lock ? (last_lock - first_lock) / k_search_step_hz + 1u : 1u);
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
    memset(lead_, 0, sizeof(lead_));
    memset(fast_half_, 0, sizeof(fast_half_));
    memset(slow_, 0, sizeof(slow_));
    memset(slow_square_, 0, sizeof(slow_square_));
    memset(quiet_, 0, sizeof(quiet_));
    for (uint8_t b = 0; b < k_max_bins; ++b) since_quiet_[b] = k_never;
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
    lock_level_ = 0.0f;
    floor_ = k_min_floor;
    recent_floor_ = 0.0f;
    previous_floor_ = k_min_floor;
    onset_floor_ = k_min_floor;
    best_bin_ = 0;
    middle_bin_ = 0;
    stable_blocks_ = 0;
    lock_ = Lock::none;
    sample_index_ = 0;
    blocks_ = 0;
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
    update_quiet();
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
void ToneSearch::update_phase(bool continued, uint8_t bin) {
    if (!continued) {
        phase_sum_.re = 0.0f;
        phase_sum_.im = 0.0f;
        phase_square_sum_.re = 0.0f;
        phase_square_sum_.im = 0.0f;
        phase_weight_ = 0.0f;
        half_sum_ = phase_sum_;
        half_weight_ = 0.0f;
    } else {
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
        if (magnitude > 0.0f) {  // phase doubled, magnitude kept: immune to a sign change between blocks
            phase_square_sum_.re += (product.re * product.re - product.im * product.im) / magnitude;
            phase_square_sum_.im += 2.0f * product.re * product.im / magnitude;
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
        lead_[b] += max_of(lead_alpha_, warmup) * (power - lead_[b]);
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
    // The lock bins only: the guard bins lie half a bin beyond the range's edge bins, where a filter's edge may begin.
    float sorted[k_max_bins];
    const uint8_t count = static_cast<uint8_t>(bins_ - 2u);
    for (uint8_t b = 0; b < count; ++b) {
        const float value = slow_[b + 1u];
        uint8_t i = b;
        while (i > 0 && sorted[i - 1] > value) {
            sorted[i] = sorted[i - 1];
            --i;
        }
        sorted[i] = value;
    }
    const uint8_t lower = static_cast<uint8_t>(count / 2 > 0 ? count / 2 : 1);
    float sum = 0.0f;
    for (uint8_t i = 0; i < lower; ++i) sum += sorted[i];
    const float averaged = min_of(static_cast<float>(blocks_), k_slow_blocks);
    const float bias = max_of(1.0f - k_lower_half_bias / sqrtf(averaged), k_lower_half_bias / k_slow_blocks);
    floor_ = max_of(sum / (static_cast<float>(lower) * bias), k_min_floor);

    for (uint8_t b = 0; b < count; ++b) sorted[b] = block_power(static_cast<uint8_t>(b + 1u));
    const float block_floor =
        max_of(lower_sum(sorted, count, lower) / (static_cast<float>(lower) * k_block_floor_bias), k_min_floor);
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

// Per bin: the quiet run under the leading threshold, and the blocks since the last run of quiet_blocks_ ended.
void ToneSearch::update_quiet() {
    const float threshold = lead_ratio_ * max_of(floor_, recent_floor_);
    for (uint8_t b = 0; b < bins_; ++b) {
        if (lead_[b] < threshold) {
            if (quiet_[b] < k_never) ++quiet_[b];
            if (quiet_[b] >= quiet_blocks_) {
                since_quiet_[b] = 0;
                continue;
            }
        } else {
            quiet_[b] = 0;
        }
        if (since_quiet_[b] < k_never) ++since_quiet_[b];
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
    if (mode == Lock::none) {
        stable_blocks_ = 0;
    } else if (continued) {
        if (stable_blocks_ < k_long_run_blocks) ++stable_blocks_;
    } else {
        stable_blocks_ = 1;
    }
    lock_level_ = mode == Lock::fast ? fast_best / lock_floor : k_fast_lock;
    if (mode != Lock::none) update_phase(continued, static_cast<uint8_t>(bin));
    lock_ = mode;
    if (bin >= 0) best_bin_ = static_cast<uint8_t>(bin);
    if (!continued) onset_floor_ = previous_floor_;  // the run's tone is not yet in the floor before its start
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

bool ToneSearch::leading(float& tone_hz) const {
    return lead_bin(k_never, tone_hz);
}

bool ToneSearch::fresh_leading(float& tone_hz, uint16_t within_blocks) const {
    return lead_bin(within_blocks, tone_hz);
}

// The strongest eligible local peak of the slot-long averages above the leading threshold, among the bins that came
// out of a quiet at most `within_blocks` ago (k_never: every bin).
bool ToneSearch::lead_bin(uint16_t within_blocks, float& tone_hz) const {
    if (blocks_ < k_warmup_blocks) return false;
    const float threshold = lead_ratio_ * max_of(floor_, recent_floor_);
    int16_t best = -1;
    float best_value = 0.0f;
    for (uint8_t b = 0; b < bins_; ++b) {
        if (since_quiet_[b] > within_blocks) continue;
        if (!eligible(b) || lead_[b] < threshold || lead_[b] <= best_value || !local_peak(lead_, b)) continue;
        best = b;
        best_value = lead_[b];
    }
    if (best < 0) return false;
    tone_hz = static_cast<float>(first_hz_ + best * k_search_step_hz);
    return true;
}

// A steady tone: the phase of the block-to-block product is the offset modulo one bin (the same in every bin: 2 pi f /
// 50 Hz per block). The half-block products pick the alias when they are coherent (pi per bin of offset), otherwise
// the power pattern does.
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
        offset = steady_offset(power, phase_sum_, half_sum_, half_weight_, offset);
    } else if (phase_weight_ > 0.0f) {
        // Keyed data: the doubled phase is exact modulo half a bin for a steady tone (keyed data off a bin spreads it by
        // some 15 Hz); the alias nearest the reference is taken. The reference is the neighbours' balance, centred for
        // keyed data (whose power spreads evenly into both neighbours; the power interpolation reads it a third of a
        // bin off), or the half-block estimate when it lies within half a bin of the balance. The decoder refines the
        // pitch from the history's tone blocks before it trusts slot energies (spec 3.2).
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

int16_t ToneSearch::bin_of(float tone_hz) const {
    const int32_t bin = round_to_int((tone_hz - static_cast<float>(first_hz_)) / k_search_step_hz);
    return bin >= 0 && bin < bins_ ? static_cast<int16_t>(bin) : static_cast<int16_t>(-1);
}

bool ToneSearch::masked(float tone_hz) const {
    const int16_t bin = bin_of(tone_hz);
    return bin >= 0 && steady_carrier(static_cast<uint8_t>(bin));
}

bool ToneSearch::following(float tone_hz) const {
    const int16_t bin = bin_of(tone_hz);
    return bin >= 0 && lead_[bin] >= lead_ratio_ * max_of(floor_, recent_floor_);
}

bool ToneSearch::fresh(float tone_hz, uint16_t within_blocks) const {
    const int16_t bin = bin_of(tone_hz);
    return bin >= 0 && since_quiet_[bin] <= within_blocks &&
           lead_[bin] >= lead_ratio_ * max_of(floor_, recent_floor_);
}

bool ToneSearch::present(float tone_hz) const {
    const int16_t bin = bin_of(tone_hz);
    return bin >= 0 && level(static_cast<uint8_t>(bin)) >= k_fast_lock_quiet * max_of(floor_, recent_floor_);
}

void ToneSearch::forget() {
    for (uint8_t b = 0; b < bins_; ++b) {
        if (steady_carrier(b)) continue;
        fast_[b] = floor_;
        lead_[b] = floor_;
        slow_[b] = floor_;
        slow_square_[b] = 2.0f * floor_ * floor_;  // an exponential power's second moment: no excess variance
        if (b + 1 < bins_) fast_half_[b] = floor_;
    }
    lock_ = Lock::none;
    stable_blocks_ = 0;
    phase_sum_.re = 0.0f;
    phase_sum_.im = 0.0f;
    phase_square_sum_.re = 0.0f;
    phase_square_sum_.im = 0.0f;
    phase_weight_ = 0.0f;
    half_sum_.re = 0.0f;
    half_sum_.im = 0.0f;
    half_weight_ = 0.0f;
}

float ToneSearch::lead_power(float tone_hz) const {
    const int16_t bin = bin_of(tone_hz);
    if (bin < 0) return 0.0f;
    float value = lead_[bin];
    if (bin > 0) value = max_of(value, lead_[bin - 1]);
    if (bin + 1 < bins_) value = max_of(value, lead_[bin + 1]);
    return value;
}

float ToneSearch::lead_excess(float tone_hz) const {
    return lead_power(tone_hz) / (lead_ratio_ * max_of(floor_, recent_floor_));
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
// Free functions
// ---------------------------------------------------------------------------

float noise_samples(float blocks, uint8_t block_samples) {
    const float effective = max_of(blocks - k_cic_overlap, 0.5f * blocks);
    return effective * static_cast<float>(block_samples);
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
