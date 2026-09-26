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
const uint8_t AuditRing::k_frames;

namespace {

const float k_pi = 3.14159265f;
const float k_two_pi = 2.0f * k_pi;
const float k_phase_per_hz = 4294967296.0f / static_cast<float>(k_decoder_rate_hz);  // NCO step per Hz
const float k_tiny = 1e-20f;

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

// ImpulseBlanker (spec 3.3.2).
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

// ToneSearch (spec 3.6).
const uint16_t k_search_step_hz = 50;
const uint8_t k_goertzel_shift = 14;
const float k_goertzel_one = 16384.0f;
const float k_fast_alpha = 1.0f / 8.0f;
const float k_slow_alpha = 1.0f / 128.0f;
// The mean of the lower half of 49 bin powers, each averaged over N blocks, is (1 - 0.8 / sqrt(N)) of the
// noise (N >= 2; the slow average counts as 2 / alpha - 1 = 255 blocks).
const float k_lower_half_bias = 0.8f;
const float k_slow_blocks = 2.0f / k_slow_alpha - 1.0f;
const float k_min_floor = static_cast<float>(ToneSearch::k_block_samples);  // noise of 1 LSB rms
// The recent floor: the mean of the lower half of one block's 49 bin powers is 0.309 of the noise; averaged over about
// 4 blocks. It follows a receiver AGC within 100 ms: after a strong signal ends the AGC raises the noise by the signal's
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
// A tone below k_fast_lock (the hf_weak gate: 7.5 dB in a bin) locks after k_long_run_blocks stable blocks, not
// k_lock_blocks: its phase products need that many for the tone estimate (only weak modes, with long tunes, are heard
// that low). Half-bin powers |X_b - X_b+1|^2 / 2 (the DFT halfway between two bins, same noise) cut the scalloping
// loss of a tone between two bins from 3.9 dB to 1.2 dB at worst.
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
const uint8_t k_count_limit_u8 = 0xFF;

// FineAfc (spec 3.3.5): 65 bins on the squared signal, 1 Hz apart within +-20 Hz (tone +-10 Hz, the spec
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

// SlotBank (spec 3.2). Bins stay inside 100..3900 Hz, where 1 / sin(w) <= 12.7 keeps the int32 state of a
// full-scale 1024-sample window below 2^29 and the Q14 coefficient inside int16.
const float k_bank_min_hz = 100.0f;
const float k_bank_max_hz = 3900.0f;
const uint8_t k_q15_shift = 15;
#if !defined(UNLIMITED_BANK_FLOAT)
const int32_t k_q15_round = 1 << (k_q15_shift - 1);
#endif
const int32_t k_q15_one = 32767;
const float k_q15_scale = 32768.0f;
const uint32_t k_ramp_end = 0x20000000u;    // u = 1/8: the Tukey alpha 0.25 ramp
const uint32_t k_ramp_begin = 0xE0000000u;  // u = 7/8
const float k_full_turn = 4294967296.0f;    // 2^32
const float k_sample_centre = 0.5f;         // sample n of a window sits at u = (n + 0.5) / L
const float k_min_skip = -k_sample_centre;  // a window may start up to half a sample before its first sample

// log2 Q8.8 (spec 3.10).
const float k_log_q8_one = 256.0f;
const float k_ln2 = 0.69314718f;
const int32_t k_int16_min = -32768;
const int32_t k_int16_max = 32767;

// SlotBlanker (spec 3.2).
const float k_slot_blank_power_ratio = static_cast<float>(k_slot_blank_ratio) * static_cast<float>(k_slot_blank_ratio);
const float k_slot_min_power = 1.0f;  // digital silence: 1 LSB^2
// Trigger density: +2 per trigger, -1 per other sample. A filtered impulse triggers a few tens of samples; a level
// change (a tone starting after silence) keeps triggering and passes k_slot_blank_run: r then follows it fast.
const uint8_t k_slot_blank_run = 64;
const uint8_t k_slot_run_step = 2;
const float k_slot_fast_alpha = 1.0f / 8.0f;
const uint8_t k_slot_zero_span = 2 * k_slot_blank_hold + 1;
const uint8_t k_slot_blank_tail = 32;  // 4 ms: the ringing of an impulse through the receiver filters
const float k_slot_neighbours = static_cast<float>(2 * k_slot_blank_reach);

// Window noise (spec 3.10): a low rank of the STOP slot's bin energies, clear of the neighbour peaks' leakage.
const uint16_t k_noise_rank_divisor = 4;

// Header ML (spec 3.8).
const uint8_t k_header_tones = k_header_slots;
const uint8_t k_header_sides = 2;
// E[mean of the 4 smallest of 8 unit exponentials] - E[smallest] = 0.366 - 0.125: the spread of noise, over its mean.
const float k_header_spread_scale = 1.0f / 0.2411f;
const float k_header_carrier_level = 3.0f;
const float k_header_carrier_steady = 0.3f;
const float k_header_side_ratio = 4.0f;  // sides' own noise this far apart: one side is outside the receiver passband
const float k_header_carrier_margin = 2.0f;    // x k_header_margin: the decision under a carrier ...
const uint8_t k_header_carrier_disagree = 1;   // ... may have this many slots more against it
const uint16_t k_header_bases = k_header_words / k_header_tones;  // words with a = 0; a XORs every tone
const uint8_t k_header_word_a_bits = 3;
const float k_no_score = -1e30f;

// Slot decision (spec 3.10).
const uint16_t k_min_presence_bins = k_min_grid_bins;
const float k_confidence_limit = 255.0f;
const float k_db_per_decade = 10.0f;

// ln I0(0.5 i), i = 0..32.
const float k_ln_i0_table[k_ln_i0_points] = {
    0.00000f, 0.06155f, 0.23591f, 0.49879f, 0.82399f, 1.19084f, 1.58531f, 1.99853f, 2.42497f, 2.86112f, 3.30468f,
    3.75407f, 4.20819f, 4.66620f, 5.12749f, 5.59159f, 6.05810f, 6.52673f, 6.99722f, 7.46936f, 7.94297f, 8.41791f,
    8.89405f, 9.37128f, 9.84950f, 10.32864f, 10.80861f, 11.28935f, 11.77081f, 12.25293f, 12.73567f, 13.21899f,
    13.70284f};

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

void ToneSearch::configure(uint16_t min_hz, uint16_t max_hz) {
    first_hz_ = min_hz;
    const uint16_t span_bins = static_cast<uint16_t>((max_hz - min_hz) / k_search_step_hz + 1);
    bins_ = static_cast<uint8_t>(span_bins < k_max_bins ? span_bins : k_max_bins);
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

// Strong products build the steady reference; once it rests on k_train_steady_products of them, it takes
// only the products that agree with it, a product clearly off its phase is a break, and the reference then
// stays as it was. The steady products before the first break are kept: noise lines up a few by chance (an FM
// receiver near threshold, whose de-emphasised noise is strongest at the low end of the band), a tune tone many.
void ToneSearch::update_breaks(const Complex& product, const Complex& half, float magnitude, float break_cosine) {
    const float along = product.re * steady_sum_.re + product.im * steady_sum_.im;
    const float reference = sqrtf(steady_sum_.re * steady_sum_.re + steady_sum_.im * steady_sum_.im);
    if (steady_products_ >= k_train_steady_products) {
        if (along < break_cosine * magnitude * reference) {
            if (breaks_ == 0) onset_products_ = steady_products_;
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
    return true;
}

bool ToneSearch::long_run() const {
    return lock_ != Lock::none && stable_blocks_ >= k_long_run_blocks;
}

bool ToneSearch::train_onset(float& tone_hz, uint8_t min_products) const {
    if (lock_ != Lock::fast || steady_tone_hz_ <= 0.0f || breaks_ < k_train_breaks || onset_products_ < min_products) {
        return false;
    }
    const float reference = sqrtf(steady_sum_.re * steady_sum_.re + steady_sum_.im * steady_sum_.im);
    if (reference < k_min_phase_coherence * steady_weight_) return false;  // the reference was not a steady tone
    tone_hz = steady_tone_hz_;
    return true;
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
        // Data with reversals and gaps: the doubled phase is exact modulo half a bin; the alias nearest the
        // power interpolation is taken (a wrong one is 25 Hz off, inside the fine AFC pull-in). A tone steady within
        // its blocks (a weak tune whose products look incoherent, or one the slow average found) gives the half-block
        // estimate instead.
        half_offset(half_sum_, half_weight_, offset);
        const float phase = atan2f(phase_square_sum_.im, phase_square_sum_.re) / (2.0f * k_two_pi);
        float best = phase;
        for (int8_t half_bins = -k_phase_aliases; half_bins <= k_phase_aliases; ++half_bins) {
            const float alias = phase + k_half_bin * static_cast<float>(half_bins);
            if (fabsf(alias - offset) < fabsf(best - offset)) best = alias;
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
}

void ToneSearch::clear_exclusion() {
    excluded_bin_ = -1;
    excluded_span_ = 0;
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

bool CandidateList::add(const Candidate& candidate) {
    if (count_ > 0) {
        Candidate& last = items_[head_];
        const float distance =
            static_cast<float>(as_signed(candidate.block - last.block)) + candidate.fraction - last.fraction;
        if (distance <= k_candidate_merge_blocks && distance >= -k_candidate_merge_blocks) {
            if (candidate.q <= last.q) return false;
            last = candidate;
            return true;
        }
    }
    head_ = static_cast<uint8_t>((head_ + 1) % k_size);
    items_[head_] = candidate;
    if (count_ < k_size) ++count_;
    return true;
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

void AuditRing::next_frame() {
    head_ = static_cast<uint8_t>((head_ + 1) % (k_frames + 1));
    memset(evidence_[head_], 0, sizeof(evidence_[head_]));
    if (count_ < k_frames) ++count_;
}

float AuditRing::max_evidence() const {
    int16_t best = 0;
    for (uint8_t k = 0; k < positions_; ++k) {
        int16_t sum = 0;
        for (uint8_t f = 1; f <= count_; ++f) sum = static_cast<int16_t>(sum + evidence_[(head_ + k_frames + 1 - f) % (k_frames + 1)][k]);
        if (k == 0 || sum > best) best = sum;
    }
    return static_cast<float>(best) / static_cast<float>(k_audit_scale);
}

uint8_t AuditRing::frames() const {
    return count_;
}

uint8_t AuditRing::positions() const {
    return positions_;
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


// ln I0 from the table, linear between its points; above it the asymptote x - ln(2 pi x) / 2.
float ln_i0(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= k_ln_i0_max) return x - 0.5f * logf(k_two_pi * x);
    const float position = x * static_cast<float>(k_ln_i0_points - 1) / k_ln_i0_max;
    const uint8_t index = static_cast<uint8_t>(position);
    const float fraction = position - static_cast<float>(index);
    return k_ln_i0_table[index] + fraction * (k_ln_i0_table[index + 1] - k_ln_i0_table[index]);
}

int16_t log2_q8(float value) {
    if (value <= 0.0f) return static_cast<int16_t>(k_int16_min);
    const int32_t log_q8 = round_to_int(k_log_q8_one * logf(value) / k_ln2);
    return static_cast<int16_t>(log_q8 < k_int16_min ? k_int16_min : (log_q8 > k_int16_max ? k_int16_max : log_q8));
}

float exp2_q8(int32_t log_q8) {
    return expf(static_cast<float>(log_q8) * k_ln2 / k_log_q8_one);
}

// ---------------------------------------------------------------------------
// SlotBank: one Goertzel per bin over w x, w = p(u) (Tukey alpha 0.25) from the slot phase.
// ---------------------------------------------------------------------------

namespace {

// p(u) in Q15; u is the position in the window, 2^32 = the whole window.
int32_t peak_weight_q15(uint32_t u) {
    if (u >= k_ramp_end && u <= k_ramp_begin) return k_q15_one;
    const int32_t s = sine_q15(u < k_ramp_end ? u << 1 : (0u - u) << 1);
    return (s * s) >> k_q15_shift;
}

}  // namespace

template <uint16_t Bins>
void SlotBank<Bins>::reset() {
    memset(this, 0, sizeof(*this));
}

template <uint16_t Bins>
void SlotBank<Bins>::set_bins(uint16_t count) {
    bins_ = count < Bins ? count : Bins;
}

template <uint16_t Bins>
void SlotBank<Bins>::set_frequency(uint16_t bin, float hz) {
    if (bin >= Bins) return;
    const float coefficient = 2.0f * cosf(k_two_pi * clamp(hz, k_bank_min_hz, k_bank_max_hz) /
                                          static_cast<float>(k_decoder_rate_hz));
#if defined(UNLIMITED_BANK_FLOAT)
    coeff_[bin] = coefficient;
#else
    const int32_t q14 = round_to_int(coefficient * k_goertzel_one);
    coeff_q14_[bin] = static_cast<int16_t>(q14 > k_int16_max ? k_int16_max : (q14 < -k_int16_max ? -k_int16_max : q14));
#endif
}

template <uint16_t Bins>
uint16_t SlotBank<Bins>::bins() const {
    return bins_;
}

template <uint16_t Bins>
void SlotBank<Bins>::open(float length_samples, float skip_samples) {
    const float length = max_of(length_samples, 1.0f);
    const float skip = clamp(skip_samples, k_min_skip, length - 1.0f);
    slot_step_ = static_cast<uint32_t>(k_full_turn / length);
    slot_phase_ = static_cast<uint32_t>((skip + k_sample_centre) * static_cast<float>(slot_step_));
    sum_w_ = 0;
    sum_w2_ = 0;
    for (uint16_t i = 0; i < bins_; ++i) {
        s1_[i] = 0;
        s2_[i] = 0;
    }
    active_ = true;
}

template <uint16_t Bins>
bool SlotBank<Bins>::push(int16_t sample) {
    if (!active_) return false;
    const int32_t w = peak_weight_q15(slot_phase_);
    sum_w_ += static_cast<uint32_t>(w);
    sum_w2_ += static_cast<uint32_t>((w * w) >> k_q15_shift);
#if defined(UNLIMITED_BANK_FLOAT)
    const float x = static_cast<float>(sample) * static_cast<float>(w) / k_q15_scale;
    for (uint32_t i = 0; i < bins_; ++i) {
        const float s0 = x + coeff_[i] * s1_[i] - s2_[i];
        s2_[i] = s1_[i];
        s1_[i] = s0;
    }
#else
    const int32_t x = (static_cast<int32_t>(sample) * w + k_q15_round) >> k_q15_shift;
    for (uint32_t i = 0; i < bins_; ++i) {
        const int64_t product = static_cast<int64_t>(coeff_q14_[i]) * s1_[i];
        const int32_t s0 = x + static_cast<int32_t>(product >> k_goertzel_shift) - s2_[i];
        s2_[i] = s1_[i];
        s1_[i] = s0;
    }
#endif
    const uint32_t before = slot_phase_;
    slot_phase_ += slot_step_;
    if (slot_phase_ >= before) return false;
    active_ = false;
    return true;
}

template <uint16_t Bins>
bool SlotBank<Bins>::active() const {
    return active_;
}

template <uint16_t Bins>
float SlotBank<Bins>::energy(uint16_t bin) const {
    if (bin >= bins_) return 0.0f;
#if defined(UNLIMITED_BANK_FLOAT)
    const float s1 = s1_[bin];
    const float s2 = s2_[bin];
    return max_of(s1 * s1 + s2 * s2 - coeff_[bin] * s1 * s2, 0.0f);
#else
    const int64_t s1 = s1_[bin];
    const int64_t s2 = s2_[bin];
    const int64_t cross = ((static_cast<int64_t>(coeff_q14_[bin]) * s1) >> k_goertzel_shift) * s2;
    return max_of(static_cast<float>(s1 * s1 + s2 * s2 - cross), 0.0f);
#endif
}

template <uint16_t Bins>
float SlotBank<Bins>::window_sum() const {
    return static_cast<float>(sum_w_) / k_q15_scale;
}

template <uint16_t Bins>
float SlotBank<Bins>::window_square_sum() const {
    return static_cast<float>(sum_w2_) / k_q15_scale;
}

template class SlotBank<k_header_bins>;
#if UNLIMITED_MAX_BITS_PER_PEAK != 4
template class SlotBank<k_grid_bins>;  // the same type as HeaderBank when the cap is 4
#endif

// ---------------------------------------------------------------------------
// BinBackground: log2 Q8.8 25 % quantile per bin.
// ---------------------------------------------------------------------------

namespace {

int16_t clamp_log(int32_t value) {
    return static_cast<int16_t>(value < k_int16_min ? k_int16_min : (value > k_int16_max ? k_int16_max : value));
}

// Value of rank `rank` (0 = smallest) of values[0..count-1] (quickselect); reorders them.
int16_t select_rank(int16_t* values, uint16_t count, uint16_t rank) {
    uint16_t low = 0;
    uint16_t high = static_cast<uint16_t>(count - 1u);
    while (low < high) {
        const uint16_t middle = static_cast<uint16_t>((low + high) / 2u);
        int16_t swap = values[middle];
        values[middle] = values[high];
        values[high] = swap;
        const int16_t pivot = values[high];
        uint16_t store = low;
        for (uint16_t i = low; i < high; ++i) {
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
            high = static_cast<uint16_t>(store - 1u);
        } else {
            low = static_cast<uint16_t>(store + 1u);
        }
    }
    return values[rank];
}

}  // namespace

void BinBackground::reset(uint16_t bins, float initial_mean) {
    bins_ = bins < k_grid_bins ? bins : k_grid_bins;
    slots_ = 0;
    const int16_t initial = clamp_log(static_cast<int32_t>(log2_q8(initial_mean)) - k_bg_mean_offset);
    for (uint16_t i = 0; i < k_grid_bins; ++i) log_q8_[i] = initial;
}

void BinBackground::push(uint16_t bin, float energy) {
    if (bin >= bins_) return;
    if (bin == 0 && slots_ < k_bg_warmup_slots) ++slots_;
    const int32_t factor = slots_ < k_bg_warmup_slots ? k_bg_warmup_factor : 1;
    const int32_t current = log_q8_[bin];
    log_q8_[bin] = clamp_log(log2_q8(energy) > current ? current + factor * k_bg_step_up : current - factor * k_bg_step_down);
}

void BinBackground::set_mean(uint16_t bin, float mean) {
    if (bin >= bins_) return;
    log_q8_[bin] = clamp_log(static_cast<int32_t>(log2_q8(mean)) - k_bg_mean_offset);
}

float BinBackground::mean(uint16_t bin) const {
    if (bin >= bins_) return 0.0f;
    return exp2_q8(static_cast<int32_t>(log_q8_[bin]) + k_bg_mean_offset);
}

// Median of the bins' means (in log2 Q8.8: the mean of the two middle values for an even count).
float BinBackground::noise() const {
    if (bins_ == 0) return 0.0f;
    int16_t values[k_grid_bins];
    memcpy(values, log_q8_, bins_ * sizeof(values[0]));
    const uint16_t upper = static_cast<uint16_t>(bins_ / 2u);
    int32_t middle = select_rank(values, bins_, upper);
    if ((bins_ & 1u) == 0) middle = (middle + select_rank(values, upper, static_cast<uint16_t>(upper - 1u))) / 2;
    return exp2_q8(middle + k_bg_mean_offset);
}

// ---------------------------------------------------------------------------
// SlotBlanker
// ---------------------------------------------------------------------------

SlotBlanker::SlotBlanker() {
    reset();
}

void SlotBlanker::reset() {
    memset(window_, 0, sizeof(window_));
    window_energy_ = 0;
    power_ = 0.0f;
    count_ = 0;
    head_ = 0;
    zero_left_ = 0;
    tail_left_ = 0;
    run_ = 0;
    blanked_ = false;
}

// window_ holds samples n - 2 reach .. n. The decision is on n - reach; a trigger zeroes it and hold samples on each
// side, and the output is n - reach - hold.
int16_t SlotBlanker::push(int16_t sample) {
    const int32_t oldest = window_[head_];
    const int32_t newest = sample;
    window_energy_ = window_energy_ - static_cast<uint64_t>(oldest * oldest) + static_cast<uint64_t>(newest * newest);
    window_[head_] = sample;
    head_ = static_cast<uint8_t>((head_ + 1u) % k_window);
    const float x = static_cast<float>(window_[(head_ + k_slot_blank_reach) % k_window]);
    const float square = x * x;
    const float neighbours = static_cast<float>(window_energy_) - square;
    const float reference = max_of(power_, k_slot_min_power);
    const bool warm = count_ >= k_slot_rms_samples;
    const bool loud = warm && square > k_slot_blank_power_ratio * reference;
    const bool peaky = square * k_slot_neighbours > static_cast<float>(k_slot_blank_crest) * neighbours;
    if (tail_left_ > 0) --tail_left_;
    if (loud && (peaky || tail_left_ > 0)) {
        zero_left_ = k_slot_zero_span;
        if (peaky) tail_left_ = k_slot_blank_tail;
        run_ = static_cast<uint8_t>(run_ > k_count_limit_u8 - k_slot_run_step ? k_count_limit_u8 : run_ + k_slot_run_step);
        if (run_ > k_slot_blank_run) power_ += k_slot_fast_alpha * (square - power_);
    } else {
        if (run_ > 0) --run_;
        if (!warm) {
            ++count_;
            power_ += (square - power_) / static_cast<float>(count_);
        } else if (zero_left_ <= k_slot_blank_hold) {  // not inside the zeroed span of an earlier trigger
            power_ += (min_of(square, k_slot_blank_power_ratio * reference) - power_) /
                      static_cast<float>(k_slot_rms_samples);
        }
    }
    const int16_t out = window_[(head_ + k_slot_blank_reach - k_slot_blank_hold) % k_window];
    blanked_ = zero_left_ > 0;
    if (!blanked_) return out;
    --zero_left_;
    return 0;
}

bool SlotBlanker::blanked() const {
    return blanked_;
}

// ---------------------------------------------------------------------------
// Header ML (spec 3.8)
// ---------------------------------------------------------------------------

namespace {

void sort_ascending(float* values, uint8_t count) {
    for (uint8_t i = 1; i < count; ++i) {
        const float value = values[i];
        uint8_t j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
}

// floor[d][h]: mean of the smallest k_header_bg_slots energies of tone h over the header slots. A codeword uses a tone
// at most 3 times, so those slots hold noise or a steady interferer only: the floor is what z subtracts. The spread of
// those slots (their mean less the smallest, scaled to the mean of exponential noise) is the tone's noise, and N_h
// a low rank of the spreads: a steady carrier at +10 dB over the peaks leaks into every header bin, and its level,
// unlike its fluctuation, is not noise. Noise alone: the floor shifts every z by the same amount, which leaves the ML
// and the agreement as they are.
void slot_floor(const float (&energy)[k_header_sides][k_header_slots][k_header_tones], uint8_t side, uint8_t tone,
                float& floor, float& spread, float& lowest) {
    float values[k_header_slots];
    for (uint8_t j = 0; j < k_header_slots; ++j) values[j] = energy[side][j][tone];
    sort_ascending(values, k_header_slots);
    float sum = 0.0f;
    for (uint8_t j = 0; j < k_header_bg_slots; ++j) sum += values[j];
    floor = sum / static_cast<float>(k_header_bg_slots);
    spread = k_header_spread_scale * (floor - values[0]);
    lowest = values[0];
}

// Floor and spread of every side and tone, N_h of each side and z = (E - floor) / N_h. N_h is the
// k_header_noise_rank-th smallest spread of both sides' tones. When the sides' own noise (the
// k_header_side_noise_rank-th smallest spread of their tones) differs by more than k_header_side_ratio, one side lies
// in a receiver stopband (a grid near the passband edge has the other side outside it) or under a carrier: each side
// then takes the larger of its own noise and N_h, so a stopband side scores near 0 and the other side is not scaled
// by the stopband's noise. A tone whose floor is k_header_carrier_level x N_h or more (noise alone: 0.37 x N_h) and
// that stays near it in every slot (its smallest above k_header_carrier_steady x the floor; a neighbour slot's peak
// leaking in comes and goes) holds a carrier: its products with the noise and the peaks swing its bin by as much as a
// peak, so its z is over its own spread when that is larger. A side without noise (digital silence) has z = 0.
void header_scores(const float (&energy)[k_header_sides][k_header_slots][k_header_tones],
                   float (&background)[k_header_sides][k_header_tones],
                   float (&z)[k_header_sides][k_header_slots][k_header_tones], float (&noise)[k_header_sides],
                   bool (&carrier_sides)[k_header_sides]) {
    float spread[k_header_sides][k_header_tones];
    float lowest[k_header_sides][k_header_tones];
    float ranked[k_header_sides * k_header_tones];
    for (uint8_t d = 0; d < k_header_sides; ++d) {
        float side[k_header_tones];
        for (uint8_t h = 0; h < k_header_tones; ++h) {
            slot_floor(energy, d, h, background[d][h], spread[d][h], lowest[d][h]);
            side[h] = spread[d][h];
            ranked[d * k_header_tones + h] = spread[d][h];
        }
        sort_ascending(side, k_header_tones);
        noise[d] = side[k_header_side_noise_rank - 1];
    }
    sort_ascending(ranked, k_header_sides * k_header_tones);
    const float pooled = ranked[k_header_noise_rank - 1];
    const bool agree = noise[0] <= k_header_side_ratio * noise[1] && noise[1] <= k_header_side_ratio * noise[0];
    for (uint8_t d = 0; d < k_header_sides; ++d) noise[d] = agree ? pooled : max_of(noise[d], pooled);
    const float clean = min_of(noise[0], noise[1]);
    for (uint8_t d = 0; d < k_header_sides; ++d) {
        bool carriers[k_header_tones];
        bool carrier_side = false;
        for (uint8_t h = 0; h < k_header_tones; ++h) {
            carriers[h] = background[d][h] > k_header_carrier_level * noise[d] &&
                          lowest[d][h] > k_header_carrier_steady * background[d][h];
            carrier_side = carrier_side || carriers[h];
        }
        // A side whose noise stands above the other's because of a carrier on it: the carrier leaks into every tone of
        // the side (the header window's sidelobes), and its products with the peaks' own leakage swing each tone by as
        // much as its leak, not by the noise. Each tone is then scaled by its own spread, at least the other side's
        // noise (spec 8.4 C8': a carrier +10 dB between two header tones).
        carrier_side = carrier_side && !agree && noise[d] > clean;
        for (uint8_t h = 0; h < k_header_tones; ++h) {
            float scale = noise[d];
            if (carrier_side) {
                scale = max_of(spread[d][h], clean);
            } else if (carriers[h]) {
                scale = max_of(spread[d][h], noise[d]);
            }
            for (uint8_t j = 0; j < k_header_slots; ++j) {
                z[d][j][h] = noise[d] > 0.0f ? (energy[d][j][h] - background[d][h]) / scale : 0.0f;
            }
        }
        if (carrier_side) noise[d] = clean;
        carrier_sides[d] = carrier_side;
    }
}

uint8_t strongest_tone(const float (&z)[k_header_tones]) {
    uint8_t strongest = 0;
    for (uint8_t h = 1; h < k_header_tones; ++h) {
        if (z[h] > z[strongest]) strongest = h;
    }
    return strongest;
}

}  // namespace

HeaderDecision decide_header(const float (&energy)[2][k_header_slots][k_header_slots]) {
    HeaderDecision decision;
    decision.word = 0;
    decision.side = 1;
    decision.margin = 0.0f;
    decision.noise = 0.0f;
    memset(decision.background, 0, sizeof(decision.background));
    decision.agreement = 0;
    decision.accepted = false;

    float background[k_header_sides][k_header_tones];
    float z[k_header_sides][k_header_slots][k_header_tones];
    float noise[k_header_sides];
    bool carrier_sides[k_header_sides];
    header_scores(energy, background, z, noise, carrier_sides);
    if (!(noise[0] > 0.0f) && !(noise[1] > 0.0f)) return decision;

    // Tones of word (a, b, c) are those of (0, b, c) XOR a.
    float best = k_no_score;
    float second = k_no_score;
    uint8_t best_side = 0;
    for (uint16_t base = 0; base < k_header_bases; ++base) {
        const uint16_t base_word = static_cast<uint16_t>(base << k_header_word_a_bits);
        uint8_t tones[k_header_slots];
        for (uint8_t j = 0; j < k_header_slots; ++j) tones[j] = header_symbol(base_word, j);
        for (uint8_t d = 0; d < k_header_sides; ++d) {
            for (uint8_t a = 0; a < k_header_tones; ++a) {
                float score = 0.0f;
                for (uint8_t j = 0; j < k_header_slots; ++j) score += z[d][j][tones[j] ^ a];
                if (score > best) {
                    second = best;
                    best = score;
                    decision.word = static_cast<uint16_t>(base_word | a);
                    best_side = d;
                } else if (score > second) {
                    second = score;
                }
            }
        }
    }
    decision.side = best_side == 0 ? 1 : -1;
    decision.margin = best - second;
    decision.noise = noise[best_side];
    for (uint8_t h = 0; h < k_header_tones; ++h) decision.background[h] = background[best_side][h];
    for (uint8_t j = 0; j < k_header_slots; ++j) {
        if (strongest_tone(z[best_side][j]) == header_symbol(decision.word, j)) ++decision.agreement;
    }
    // Under a carrier the peaks of the tones next to it are lost (a slot's strongest tone is then another): one more slot
    // may disagree when the margin is twice the usual.
    const bool carrier_side = carrier_sides[best_side];
    const uint8_t agree = carrier_side && decision.margin >= k_header_carrier_margin * k_header_margin
                              ? static_cast<uint8_t>(k_header_agree - k_header_carrier_disagree)
                              : k_header_agree;
    decision.accepted = decision.margin >= k_header_margin && decision.agreement >= agree;
    return decision;
}

HeaderMatch match_header(const float (&energy)[2][k_header_slots][k_header_slots], uint16_t word, int8_t side) {
    HeaderMatch match;
    match.agreement = 0;
    match.peaks = 0;
    float background[k_header_sides][k_header_tones];
    float z[k_header_sides][k_header_slots][k_header_tones];
    float noise[k_header_sides];
    bool carrier_sides[k_header_sides];
    header_scores(energy, background, z, noise, carrier_sides);
    const uint8_t d = side > 0 ? 0 : 1;
    if (!(noise[d] > 0.0f)) return match;
    for (uint8_t j = 0; j < k_header_slots; ++j) {
        const uint8_t strongest = strongest_tone(z[d][j]);
        if (strongest == header_symbol(word, j)) ++match.agreement;
        if (z[d][j][strongest] >= k_header_peak_z) ++match.peaks;
    }
    return match;
}

// ---------------------------------------------------------------------------
// Slot decision (spec 3.10)
// ---------------------------------------------------------------------------

namespace {

// N_bin: the median background of the grid bins; below k_bg_min_bits (tones on half or a quarter of the slots)
// the mean background of the unused bins M..7.
float slot_noise(const BinBackground& background, uint16_t tones, uint16_t bins) {
    if (tones >= bins) return background.noise();
    float sum = 0.0f;
    for (uint16_t t = tones; t < bins; ++t) sum += background.mean(t);
    return sum / static_cast<float>(bins - tones);
}

// z_t = E_t less the bin's excess background over N_bin (an in-grid carrier), at least 0.
float peak_energy(const GridBank& bank, const BinBackground& background, uint16_t bin, bool subtract, float noise) {
    const float excess = subtract ? max_of(background.mean(bin) - noise, 0.0f) : 0.0f;
    return max_of(bank.energy(bin) - excess, 0.0f);
}

float harmonic_number(uint16_t count) {
    float sum = 0.0f;
    for (uint16_t j = 1; j <= count; ++j) sum += 1.0f / static_cast<float>(j);
    return sum;
}

}  // namespace

// The (bins / 4)-th smallest energy (0-based) over its expectation for exponential noise of mean 1: the sum of
// 1 / (bins - i) for i = 0..rank. The median over ln 2 read 1 dB high at 8 bins (the 5th of 8 is 0.88, not 0.69).
float window_noise(const GridBank& bank) {
    const uint16_t bins = bank.bins();
    if (bins == 0) return 0.0f;
    int16_t values[k_grid_bins];
    for (uint16_t t = 0; t < bins; ++t) values[t] = log2_q8(bank.energy(t));
    const uint16_t rank = static_cast<uint16_t>(bins / k_noise_rank_divisor);
    float expected = 0.0f;
    for (uint16_t i = 0; i <= rank; ++i) expected += 1.0f / static_cast<float>(bins - i);
    return exp2_q8(select_rank(values, bins, rank)) / expected;
}

SlotDecision decide_slot(const GridBank& bank, const BinBackground& background, uint8_t bits_per_peak,
                         uint8_t slot) {
    const uint16_t tones = static_cast<uint16_t>(1u << bits_per_peak);
    const uint16_t bins = bank.bins() > tones ? bank.bins() : tones;
    const bool subtract = bits_per_peak >= k_bg_min_bits;
    const float noise = max_of(slot_noise(background, tones, bins), k_tiny);

    SlotDecision d;
    memset(&d, 0, sizeof(d));

    // Argmax of z over the M tones; the sum over every open bin gives the presence ratio.
    float z_best = 0.0f;
    float z_second = 0.0f;
    float z_sum = 0.0f;
    uint16_t best = 0;
    for (uint16_t t = 0; t < bins; ++t) {
        const float z = peak_energy(bank, background, t, subtract, noise);
        z_sum += z;
        if (t >= tones) continue;
        if (t == 0 || z > z_best) {
            if (t > 0) z_second = z_best;
            z_best = z;
            best = t;
        } else if (z > z_second) {
            z_second = z;
        }
    }
    d.tone = static_cast<uint8_t>(best);
    d.symbol = peak_symbol(d.tone, slot, bits_per_peak);
    d.noise = noise;

    // Max-log LLRs with the slot's own amplitude: m_t = ln I0(2 a sqrt(z_t) / N).
    const float amplitude = sqrtf(max_of(z_best - noise, k_amp_floor * noise));
    float one[k_max_bits_per_peak];
    float zero[k_max_bits_per_peak];
    for (uint8_t b = 0; b < bits_per_peak; ++b) {
        one[b] = k_no_score;
        zero[b] = k_no_score;
    }
    for (uint16_t t = 0; t < tones; ++t) {
        const float metric = ln_i0(2.0f * amplitude * sqrtf(peak_energy(bank, background, t, subtract, noise)) / noise);
        const uint8_t label = peak_symbol(static_cast<uint8_t>(t), slot, bits_per_peak);
        for (uint8_t b = 0; b < bits_per_peak; ++b) {
            const bool bit = ((label >> (bits_per_peak - 1u - b)) & 1u) != 0;
            float& side = bit ? one[b] : zero[b];
            if (metric > side) side = metric;
        }
    }
    for (uint8_t b = 0; b < bits_per_peak; ++b) {
        const bool decided = ((d.symbol >> (bits_per_peak - 1u - b)) & 1u) != 0;
        // Clamped as a float: at a high per-slot Es/N0 the metrics pass the int32 range.
        const float limit = static_cast<float>(k_llr_q4_max);
        int32_t q4 = round_to_int(clamp(one[b] - zero[b], -limit, limit));
        if (decided && q4 < 1) q4 = 1;  // the sign always carries the hard decision
        if (!decided && q4 > -1) q4 = -1;
        d.soft[b] = static_cast<int8_t>(q4);
    }

    const float others = (z_sum - z_best) / static_cast<float>(bins - 1u);
    const uint16_t presence_bins = bins > k_min_presence_bins ? bins : k_min_presence_bins;
    d.confident = others > 0.0f ? z_best >= k_presence_factor * harmonic_number(presence_bins) * others : z_best > 0.0f;
    d.erasure = z_best < k_erasure_ratio * z_second;
    float ratio_db = 0.0f;
    if (z_best > 0.0f) ratio_db = z_second > 0.0f ? k_db_per_decade * log10f(z_best / z_second) : k_confidence_limit;
    d.confidence = static_cast<uint8_t>(round_to_int(clamp(ratio_db / k_confidence_step_db, 0.0f, k_confidence_limit)));
    // A peak's envelope is the window itself: sqrt(E) = A sum(w^2) / 2.
    const float window = bank.window_square_sum();
    d.crest = window > 0.0f ? 2.0f * sqrtf(max_of(z_best - noise, 0.0f)) / window : 0.0f;
    return d;
}

}  // namespace dsp
}  // namespace unlimited
