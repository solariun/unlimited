#include "unlimited/decoder.hpp"

#include <math.h>
#include <string.h>

namespace unlimited {

// Spec 3.15: the history grows with the cap; everything else stays within 7 KB.
static const uint32_t k_decoder_base_bytes = 7168;
static const uint32_t k_decoder_bytes_per_bit = 576;
static_assert(sizeof(Decoder) <= k_decoder_base_bytes + k_decoder_bytes_per_bit * UNLIMITED_MAX_BITS_PER_PACKAGE,
              "Decoder must fit 7168 + 576 * cap bytes (spec 3.15)");

const uint8_t Decoder::k_held_packages;
const uint8_t Decoder::k_readings;
const uint8_t Decoder::k_cold_hypotheses;
const uint8_t Decoder::k_fold_grids;

namespace {

using dsp::Candidate;
using dsp::Complex;
using dsp::FlipMeasure;
using dsp::LearnStep;

// Profiles (spec 1.7, 5.1).
const uint8_t k_ssb_min_slot_ms = 8;
const uint8_t k_fm_min_slot_ms = 4;
const uint8_t k_min_block_samples = k_min_window_slot_ms;  // a block is min_slot_ms samples (spec 3.1)
const float k_default_fixed_ratio = 0.70f;

// Units.
const float k_ms_per_s = 1000.0f;
const uint32_t k_us_per_ms = 1000;
const float k_samples_per_ms = static_cast<float>(k_decoder_rate_hz) / k_ms_per_s;
const float k_pi = 3.14159265f;
const float k_two_pi = 2.0f * k_pi;
const uint32_t k_count_limit = 0xFFFFFFFFu;
const uint8_t k_count_limit_u8 = 0xFF;
const float k_reference_bandwidth_hz = 2500.0f;
const float k_snr_floor_db = -99.0f;
const float k_db_per_decade = 10.0f;

// Front end (spec 3.2, 3.3).
const uint8_t k_energy_shift = 4;
const float k_energy_scale = 256.0f;                                                 // (1 << k_energy_shift)^2
const float k_in_bin_scale = 2.0f / (k_mixer_gain * k_mixer_gain * k_energy_scale);  // e_k = p_k for a steady tone
const uint8_t k_settle_blocks = dsp::ImpulseBlanker::k_latency + 1;  // blocks mixed before the NCO was set
const uint8_t k_noise_blocks = 8;
const float k_noise_outlier = 4.0f;  // ACQUIRE windows above this many times the noise are signal
// A tune lock keeps the noise measured before its tone: behind a receiver AGC that noise was raised by the AGC while
// nothing else was heard, up to the signal's SNR. The search's recent floor, under the tone, replaces it when it is
// less than half of it.
const float k_noise_drop = 0.5f;
const uint16_t k_afc_decimation_samples = 64;  // 8 ms: 125 Hz AFC input
const float k_afc_eval_ms = 250.0f;
const uint16_t k_afc_min_inputs = 16;
const float k_afc_wait_ms = 1000.0f;  // a lock on data may start joining without an AFC look after this long
const float k_afc_min_offset_hz = 0.2f;
const float k_afc_reset_rotation = 0.5f;  // radians over the slowest marker half window
const float k_preamble_afc_limit = 0.25f;  // cycles per slot

// Watch (spec 3.7).
const uint8_t k_onset_products = 3;       // steady search products before a train onset (80 ms)
const uint8_t k_onset_products_tune = 4;  // ... while ACQUIRE holds a tune tone (100 ms)
// ... in PREAMBLE: the preamble's own markers stay steady in the search's other bins for at most 128 ms; a tune
// (>= 250 ms) is steady longer (200 ms).
const uint8_t k_onset_products_preamble = 10;
// A train of period T has lines at odd multiples of 1 / (2 T) from the tone: its power is in the first orders
// (2 m + 1) / (2 T), m <= 7. The search's estimate of a steady tone is good to about 1 Hz.
const uint8_t k_train_line_orders = 7;
const float k_train_line_hz = 5.0f;
// A saturated input adds odd harmonics of the tune, which alias into the band: the image reverses with the tune and
// reads as a tune and train of its own. Reach: one search bin.
const uint8_t k_first_image_order = 3;
const uint8_t k_last_image_order = 7;
const float k_image_reach_hz = 50.0f;
const float k_memory_search_margin_hz = 50.0f;  // one tone-search bin

// Geometry, in slots (spec 3.4, 3.8, 3.9).
const float k_marker_half = 0.35f;
const float k_slot_window = 0.75f;
const float k_gap_window = 0.15f;
const float k_search_step = 0.05f;
const float k_position_search = 0.10f;
const float k_first_stop_search = 0.25f;
const float k_stop_search_growth = 0.005f;  // per slot beyond k_stop_search_after
const int32_t k_stop_search_after = 9;
const float k_stop_search_max = 0.35f;
const float k_track_search = 0.15f;
const float k_track_search_miss = 0.5f;
const uint8_t k_max_search_steps = 10;  // k_track_search_miss / k_search_step
const float k_boundary_half = 0.1f;
const float k_slot_centre = 0.5f;
const float k_audit_step = 0.5f;
const float k_audit_edge_half = 0.3f;  // half window at the slot edges next to START and STOP
const float k_search_edge = 0.25f;  // of a search step: a best this close to the end of its span sits on the edge
const float k_anti_half = 2.0f * k_marker_half;

// Flips (spec 3.4).
const float k_q_candidate = 3.0f;
const float k_kappa_candidate = 0.5f;
const float k_q_track = 4.0f;
const float k_q_expected_stop = 0.0f;  // PREAMBLE: q_bal of a STOP where the held candidate expects it
const float k_kappa_track = 0.3f;
const float k_kappa_after_miss = 0.5f;
const float k_q_present = 1.0f;
const float k_evidence_clip = 8.0f;
const float k_kappa_spread = 4.2f;  // 3 sigma of the kappa of a true flip: 3 sqrt(2) / sqrt(E)
const float k_kappa_floor_max = 0.75f;
const float k_no_value = -1e30f;
const float k_no_guard_q = 1e30f;

// Sync (spec 3.7).
const uint8_t k_sync_positions = 8;
const uint8_t k_sync_midpoints = 4;           // midpoints between the newest train positions
const uint8_t k_sync_max_midpoint_flips = 2;  // this many flips between markers rejects the hypothesis
// Between two train markers the envelope is null: the Tukey tails of both meet at the slot edge, and over +-0.1 T
// around it their mean is 0.12 of the crest (energy 0.015). Interference whose flips pass for markers is loud there.
const float k_boundary_max = 0.25f;
const uint8_t k_sync_min_positions = 5;
// Hits: 5, or 4 among the newest 5 positions with at least 2 at odd and 2 at even positions. A T / 2 reading of a
// 2T train hits one parity only, and a wrong T scatters its hits over the 8 positions.
const uint8_t k_sync_min_hits = 5;
const uint8_t k_sync_dense_hits = 4;
const uint8_t k_sync_dense_mask = 0x1F;  // positions 0..4
const uint8_t k_sync_odd_mask = 0xAA;
const uint8_t k_sync_min_parity_hits = 2;
// Near the gate a third of the markers flip at q 2..4: a weak marker (q >= 2, marker-like crest) completes a train of
// at least 3 hits, one at each parity, when every position scored but one holds a marker (at least 5, 2 per parity).
const float k_sync_weak_q = 2.0f;
const uint8_t k_sync_min_strong = 3;
const uint8_t k_sync_min_parity_strong = 1;
const uint8_t k_sync_missing_markers = 1;
const float k_sync_evidence = 24.0f;
const float k_prefer_smaller_t = 0.8f;
const float k_hypothesis_merge = 0.03f;
const uint8_t k_max_hypotheses = 64;
const uint8_t k_hypothesis_passes = 2;  // a least-squares T per group, then again without its outliers
const uint8_t k_max_accepted = 16;
const uint8_t k_refine_passes = 2;
const uint8_t k_refine_min_points = 3;
const float k_refine_max_change = 0.1f;  // a least-squares T further than 10 % away is ignored
// A train's T is accepted up to 6 % outside T_min..T_max: its estimate is noisy at the window's ends.
const float k_range_tolerance = 0.06f;
const uint8_t k_half_rate_odd_hits = 1;  // at most one (noise) hit at the odd positions of a T / 2 reading
const float k_marker_edge_sigmas = 3.0f;

// Preamble (spec 3.8).
const float k_train_nudge = 0.2f;  // T += 0.2 (measured - T) at each train marker before the fit takes over
const float k_fit_min_weight = 1.0f;
const float k_fit_min_points = 3.0f;
const float k_train_amplitude_ratio = 0.5f;
const float k_flip_amplitude_ratio = 0.3f;
const float k_reference_alpha = 0.25f;
// A train marker faded by the channel still flips far above the noise where the train puts it.
// The carrier across a gap between two markers (spec 1.1: each marker reverses it, data slots keep it): a decisive
// reversal means a hidden (faded) marker between them.
const float k_kappa_bridge = 0.5f;
const uint8_t k_hidden_midpoints = 2;  // reversals between sync hits one T apart that reveal a train of T / 2
// The tune tone ends where the train begins: two steady slots in a row within this many slots before the sync anchor
// place the train's first marker, and package 0's START k_min_sync_markers - 1 slots after it or later.
const int32_t k_tune_search_slots = 12;
const int32_t k_tune_bound_slots = 2;
const float k_tune_bound_crest = 0.5f;  // of the train's crest

const float k_lookahead_ratio = 2.0f;  // a flip in the data this much weaker than the candidate's STOP is noise
const float k_lookahead_q = 16.0f;     // a flip this clear is taken without looking ahead
const float k_faded_marker_q = 16.0f;
const float k_faded_marker_kappa = 0.8f;
const float k_faded_marker_ratio = 0.2f;
const float k_faded_marker_offset = 0.05f;
// A candidate from faded train markers spans at most this many of them (any 3 of 8 sync markers may fade, spec 8.2 L6):
// a lone marker one slot after a longer candidate's STOP is noise, not the train going on.
const uint8_t k_faded_train_bits = 3;
// A confirmed N whose packages hold markers at a whole fraction 1/m of their span is m packages read as one (every other
// STOP missed in the preamble): refused when those positions flip at this mean evidence.
const float k_sub_chain_evidence = 3.0f;
const uint32_t k_recovered_packages = 4;  // packages before a confirmation further on (Decoder::k_held_packages)
const float k_slot_count_tolerance = 0.25f;  // slots from L to the confirmed START, counted with the confirmed T
const uint8_t k_min_train_ones = 3;
const uint8_t k_sub_rate_gaps = 2;   // equal train gaps k >= 2 that reveal a T / k reading
const uint8_t k_sub_rate_ratio = 2;  // ... outnumbering the gaps of 1 this many times
const uint8_t k_max_rejections = 4;
const int32_t k_preamble_base_slots = 40;
const int32_t k_preamble_packages = 4;
const int32_t k_package_slots_max = static_cast<int32_t>(k_max_bits_per_package) + 1;
const int32_t k_preamble_max_slots = k_preamble_base_slots + k_preamble_packages * k_package_slots_max;
const int32_t k_marker_gap_limit = 2 * k_package_slots_max + 1;
const uint8_t k_train_min_markers = 6;

// Track (spec 3.9).
const float k_timing_gain = 0.2f;
const float k_stop_reach_min = 0.10f;
const float k_stop_reach_per_slot = 0.02f;
const float k_stop_reach_slots = 9.0f;
const float k_stop_reach_per_miss = 0.05f;
const float k_stop_reach_miss_max = 0.25f;  // half the search span after a miss
const uint8_t k_stop_reach_misses = 4;
const float k_afc_gain = 0.1f;
const float k_afc_min_q = 8.0f;
const float k_afc_clamp = 0.1f;  // cycles per slot
// A frequency step after the lock rotates the carrier across a STOP's halves; the rotation of a strong missed STOP
// measures the offset. A two-path channel mimics it at T below 32 ms, so the rotation AFC is off there (v0.2 H2).
const float k_rotation_energy = 16.0f;
const float k_rotation_min_rad = 0.6f;
const float k_rotation_span = 0.383f;  // T between the centroids of |w r| over a marker's two 0.35 T halves
const float k_rotation_agreement = 0.5f;
const float k_rotation_gain = 0.75f;
const float k_rotation_min_slot_ms = 32.0f;
const float k_rotation_balance = 0.25f;
const float k_rotation_max_level = 1.5f;

// End and loss (spec 3.9).
const float k_end_reach = static_cast<float>(k_end_markers) + k_marker_half;  // END positions after the STOP, slots
const float k_end_evidence = 10.0f;
const float k_end_single_evidence = 4.0f;  // N = 1
const uint8_t k_end_single_quiet_slot = 4;  // N = 1: no flip at STOP + 4 T
const uint8_t k_end_single_last_slot = 6;   // ... nor at STOP + 6 T, where the chain's next STOPs would be
const uint8_t k_end_clean_slot = 3;         // the slot after the END markers
const float k_short_eot_evidence = 15.0f;
const float k_short_eot_min = 3.0f;
const uint8_t k_short_eot_first = 2;  // a short final package holds at least one bit
const uint8_t k_short_eot_flips = 3;
const uint8_t k_presence_slots = 36;
const uint8_t k_min_presence_stops = 4;
const uint8_t k_loss_numerator = 3;  // LOST when 3/4 of the presence window is missing
const uint8_t k_loss_denominator = 4;
const float k_tune_edge_energy = 0.25f;
const float k_tune_kappa = -0.5f;
const uint8_t k_tune_edges = 3;

// Guard and audit (spec 3.11).
const uint8_t k_guard_slots = 18;
const uint8_t k_full_guard_slots = 36;
const uint8_t k_min_guard_packages = 2;
const uint8_t k_full_guard_packages = 4;
const float k_quick_stop_q = 16.0f;
const float k_quick_audit = 4.0f;
const float k_guard_min_detected = 0.8f;
const uint8_t k_guard_min_balanced = 2;
const uint8_t k_guard_max_inner = 2;
const float k_guard_inner_share = 0.25f;  // of the guard's packages
const uint8_t k_guard_share_positions = 2u * k_hf_bits_per_package + 1u;  // audit positions the share was set for
const float k_guard_max_level = 2.0f;
const float k_marker_edge_max = 0.3f;
// Decodability: key-down SNR for BER 1e-3 is -3.2 dB at T = 32 ms and 3 dB lower per doubling of T (spec 4.1). A new
// lock is confirmed only when its measured SNR is within k_confirm_margin_db of that.
const float k_model_slot_ms = 32.0f;
const float k_model_snr_db = -3.2f;
const float k_confirm_margin_db = 5.0f;
const float k_audit_low = -4.0f;
const float k_audit_high = 8.0f;
const float k_audit_threshold = 12.0f;
const uint8_t k_strong_packages = 3;  // of the last 4 packages with a full-strength inner flip
const uint8_t k_strong_window_mask = 0x0F;
const uint8_t k_anti_packages = 2;    // of the last 8 STOPs that read as a steady carrier, once confirmed
const float k_kappa_anti = 0.5f;
// The zeros of a real package carry the noise that was there before the lock (or less); a guard is refused when
// most of its zeros are ten times louder.
const float k_loud_zero = 10.0f;

// Decision (spec 3.10).
const float k_floor_sigma = 2.6f;
const float k_soft_scale = 64.0f;
const float k_soft_limit = 127.0f;
const float k_weak_margin = 0.125f;
const float k_percent = 100.0f;
const float k_half_quiet_snr_db = 15.0f;  // a marker's tail equals the noise in its gap window about here (T = T_min)
const float k_percent_limit = 255.0f;
const float k_noise_amplitude = 4.0f;  // E|2 S / (n g)|^2 = 4 sigma^2 n_eff / (n g)^2
const uint8_t k_top_bit = 0x80;

// Station memory, late joins, bans (spec 3.7, 3.12).
const float k_memory_ms = 60000.0f;
const float k_memory_hz = 10.0f;
const float k_relock_packages = 64.0f;
const float k_relock_fraction = 0.2f;
const uint8_t k_late_join_intervals = 3;
const float k_late_join_q = 4.0f;
const float k_late_join_t = 0.03f;
const float k_stream_marker_ratio = 0.35f;  // of the remembered crest
const uint8_t k_stream_timeout_packages = 8;
const float k_stream_tune_half = 0.75f;     // of the remembered T
const float k_tune_crest = 0.5f;
const float k_tune_energy = 4.0f;
const float k_acquire_timeout_ms = 3000.0f;
const uint8_t k_acquire_timeout_slots = 40;
const float k_alias_ban_ms = 10000.0f;
const float k_alias_ban_band = 0.05f;
const float k_search_ban_ms = 10000.0f;

// Cold late join (spec 3.12, V7): N = 8, 16, 24, 32 and the odd sub-grids of each (a grid of 3 T has its edges on
// edges of T too: N + 1 = 3 (N' + 1) would pass for N').
const uint8_t k_cold_join_intervals = 3;
const uint8_t k_cold_join_step_bits = 8;
const uint8_t k_fold_sub_grids[] = {1, 3, 5, 7};
const uint8_t k_cold_join_fold_packages = 2;
const uint8_t k_cold_join_max_packages = 4;
const float k_cold_join_tolerance = 0.03f;  // equal intervals
const float k_fold_edge_ratio = 0.35f;
const float k_fold_half = 0.1f;             // edge and centre windows +-0.1 T
const float k_fold_min_centre = 2.0f;       // mean centre energy over the noise of its window
const uint8_t k_cold_stray_candidates = 1;  // train-like candidates allowed inside the chain
const float k_cold_stray_ratio = 0.5f;      // of the chain's weakest marker q: weaker candidates are noise
const float k_marker_search = 0.05f;        // a chain marker is taken within 5 % of P of its prediction
const float k_fold_rival_ratio = 0.5f;      // another N' whose edges are this much quieter is the chain's N

float clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

float max_of(float a, float b) {
    return a > b ? a : b;
}

float min_of(float a, float b) {
    return a < b ? a : b;
}

float magnitude_squared(const Complex& value) {
    return value.re * value.re + value.im * value.im;
}

int32_t round_to_int(float value) {
    return static_cast<int32_t>(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

uint8_t count_bits(uint32_t bits) {
    uint8_t count = 0;
    for (; bits != 0; bits &= bits - 1u) ++count;
    return count;
}

uint8_t percent_of(float value, float reference) {
    if (reference <= 0.0f) return static_cast<uint8_t>(k_percent_limit);
    return static_cast<uint8_t>(round_to_int(clamp(k_percent * value / reference, 0.0f, k_percent_limit)));
}

float clipped_evidence(const FlipMeasure& m) {
    if (!m.valid) return 0.0f;
    if (m.kappa >= k_kappa_track) return clamp(m.q, -k_evidence_clip, k_evidence_clip);
    return clamp(min_of(m.q, 0.0f), -k_evidence_clip, 0.0f);
}

// Energy of both halves over their noise: q / kappa.
float flip_energy(const FlipMeasure& m) {
    return m.kappa > 0.0f ? m.q / m.kappa : 0.0f;
}

// A true reversal of energy E has kappa = 1 - 1/E with a spread of about sqrt(2/E): a strong "flip" far below that
// is something else (a marker of another speed smeared over a wide window, a mistuned tone). Capped: channels distort
// every marker alike (fading multipath reaches 0.85 at 30 dB, CCIR moderate).
float kappa_floor(const FlipMeasure& m) {
    const float energy = flip_energy(m);
    if (energy <= 0.0f) return 0.0f;
    return min_of(1.0f - 1.0f / energy - k_kappa_spread / sqrtf(energy), k_kappa_floor_max);
}

bool is_flip(const FlipMeasure& m, float kappa_min) {
    return m.valid && m.q >= k_q_track && m.kappa >= kappa_min && m.kappa >= kappa_floor(m);
}

FlipMeasure invalid_measure(float position) {
    FlipMeasure invalid;
    memset(&invalid, 0, sizeof(invalid));
    invalid.kappa = k_no_value;
    invalid.position = position;
    invalid.valid = false;
    return invalid;
}

// A search's best on the edge of its span (search_flip steps of k_search_step half windows).
bool on_edge(const FlipMeasure& m, float predicted, float span, float half) {
    const float step = half * (k_search_step / k_marker_half);
    return fabsf(m.position - predicted) >= span - k_search_edge * step;
}

// A flip with both halves alike: a marker, not the edge of a beep against silence (one-sided energy).
bool balanced_flip(const FlipMeasure& m) {
    return is_flip(m, k_kappa_track) && m.q_balanced >= k_q_candidate;
}

// +1: the carrier reversed across a gap (an odd number of hidden markers), -1: it did not, 0: undecided.
int8_t carrier_parity(const FlipMeasure& m) {
    if (!m.valid) return 0;
    if (m.q >= k_q_track && m.kappa >= k_kappa_bridge) return 1;
    if (m.q <= -k_q_track && m.kappa <= -k_kappa_bridge) return -1;
    return 0;
}

bool strong_hits(uint8_t hit_bits, uint8_t hits) {
    if (hits >= k_sync_min_hits) return true;
    const uint8_t dense = static_cast<uint8_t>(hit_bits & k_sync_dense_mask);
    const uint8_t odd = count_bits(dense & k_sync_odd_mask);
    const uint8_t even = static_cast<uint8_t>(count_bits(dense) - odd);
    return count_bits(dense) >= k_sync_dense_hits && odd >= k_sync_min_parity_hits && even >= k_sync_min_parity_hits;
}

bool enough_hits(uint8_t hit_bits, uint8_t marker_bits, uint8_t hits, uint8_t positions) {
    if (strong_hits(hit_bits, hits)) return true;
    const uint8_t markers = count_bits(marker_bits);
    const uint8_t odd_markers = count_bits(marker_bits & k_sync_odd_mask);
    const uint8_t odd_hits = count_bits(hit_bits & k_sync_odd_mask);
    return hits >= k_sync_min_strong && odd_hits >= k_sync_min_parity_strong &&
           hits - odd_hits >= k_sync_min_parity_strong && markers >= k_sync_min_hits &&
           markers + k_sync_missing_markers >= positions && odd_markers >= k_sync_min_parity_hits &&
           markers - odd_markers >= k_sync_min_parity_hits;
}

bool package_bit(const uint8_t* bits, uint8_t index) {
    return (bits[index / k_bits_per_byte] & (k_top_bit >> (index % k_bits_per_byte))) != 0;
}

// A grid at least as fine as the window's shortest T cannot be resolved by the history's blocks.
bool resolvable(float slot_blocks) {
    return slot_blocks >= static_cast<float>(k_blocks_per_min_slot) * (1.0f - k_range_tolerance);
}

// N divides every 8 n bits: no short final package exists.
bool divides_bytes(uint8_t bits) {
    return k_bits_per_byte % bits == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// DecoderConfig
// ---------------------------------------------------------------------------

DecoderConfig::DecoderConfig()
    : min_slot_ms(k_ssb_min_slot_ms),
      passband(),
      decision_mode(DecisionMode::adaptive),
      fixed_ratio(k_default_fixed_ratio),
      impulse_blanker(true) {
    passband.low_hz = k_ssb_passband_low_hz;
    passband.high_hz = k_ssb_passband_high_hz;
}

DecoderConfig DecoderConfig::for_profile(Profile profile) {
    DecoderConfig config;
    switch (profile) {
    case Profile::ssb:
        break;
    case Profile::am:
        config.passband.low_hz = k_am_passband_low_hz;
        config.passband.high_hz = k_am_passband_high_hz;
        break;
    case Profile::fm:
        config.min_slot_ms = k_fm_min_slot_ms;
        config.passband.low_hz = k_fm_passband_low_hz;
        config.passband.high_hz = k_fm_passband_high_hz;
        break;
    }
    return config;
}

ConfigError DecoderConfig::check() const {
    if (min_slot_ms < k_min_window_slot_ms || min_slot_ms > k_max_window_slot_ms) return ConfigError::min_slot;
    if (!passband_valid(passband)) return ConfigError::passband;
    const Passband range = search_range();
    if (range.low_hz > range.high_hz) return ConfigError::passband;
    if (decision_mode != DecisionMode::adaptive && decision_mode != DecisionMode::fixed_ratio) {
        return ConfigError::decision_mode;
    }
    if (!(fixed_ratio > 0.0f && fixed_ratio < 1.0f)) return ConfigError::fixed_ratio;
    return ConfigError::none;
}

bool DecoderConfig::valid() const {
    return check() == ConfigError::none;
}

uint16_t DecoderConfig::max_slot_ms() const {
    return static_cast<uint16_t>(static_cast<uint16_t>(min_slot_ms) * k_speed_span);
}

// Spec 1.5, V14: the rule every receiver and the senders' shift tolerance share (protocol.hpp); empty when
// low_hz > high_hz. A block of min_slot_ms samples is T_min / 8, so blocks under 8 samples are slots under 8 ms.
Passband DecoderConfig::search_range() const {
    return unlimited::search_range(passband, static_cast<uint32_t>(min_slot_ms) * k_us_per_ms);
}

// ---------------------------------------------------------------------------
// Construction and public API
// ---------------------------------------------------------------------------

Decoder::Decoder(const DecoderConfig& config, EventHandler handler, void* context)
    : config_(config), handler_(handler), context_(context), state_(DecoderState::search) {
    initialize();
}

void Decoder::initialize() {
    block_samples_ = config_.valid() ? config_.min_slot_ms : 0;
    block_fill_ = 0;
    block_energy_ = 0;
    settle_blocks_ = 0;
    const uint8_t samples = block_samples_ > 0 ? block_samples_ : k_min_block_samples;
    afc_decimation_ = static_cast<uint8_t>((k_afc_decimation_samples + samples / 2) / samples);
    if (afc_decimation_ == 0) afc_decimation_ = 1;

    nco_ = dsp::Nco();
    cic_.reset();
    blanker_.reset();
    memset(blank_delay_re_, 0, sizeof(blank_delay_re_));
    memset(blank_delay_im_, 0, sizeof(blank_delay_im_));
    history_.reset();
    const Passband range = config_.search_range();
    search_.configure(range.low_hz, range.high_hz >= range.low_hz ? range.high_hz : range.low_hz);
    afc_.configure(static_cast<float>(k_decoder_rate_hz) / static_cast<float>(samples * afc_decimation_));
    afc_sum_.re = 0.0f;
    afc_sum_.im = 0.0f;
    afc_fill_ = 0;
    noise_.reset(0.0f);
    track_noise_.reset(0.0f);
    candidates_.reset();
    audit_.reset(dsp::AuditRing::k_max_positions);
    learner_.reset(0, 0);
    reset_scales();

    origin_block_ = history_.end_block();
    slot_blocks_ = k_blocks_per_min_slot;
    grid_position_ = 0.0f;
    grid_last_ = 0;
    grid_index_ = 0;
    end_stop_ = -1;
    fit_weight_ = 0.0f;
    fit_x_ = 0.0f;
    fit_y_ = 0.0f;
    fit_xx_ = 0.0f;
    fit_xy_ = 0.0f;
    memset(&last_marker_, 0, sizeof(last_marker_));
    memset(&train_last_, 0, sizeof(train_last_));
    memset(&end_flip_, 0, sizeof(end_flip_));
    train_gap_ = 0;
    train_gap_count_ = 0;
    train_markers_ = 0;
    sync_hits_ = 0;
    min_start_ = dsp::k_no_start;
    tuned_train_ = false;
    first_after_ = 0.0f;
    candidate_reading_ = 0;
    previous_reading_ = k_readings;
    confirm_pending_ = false;
    data_flip_ = false;
    memset(readings_, 0, sizeof(readings_));

    memset(&start_, 0, sizeof(start_));
    memset(&last_detected_, 0, sizeof(last_detected_));
    last_detected_package_ = 0;
    reference_average_ = 0.0f;
    package_index_ = 0;
    memset(short_evidence_, 0, sizeof(short_evidence_));
    short_cursor_ = k_short_eot_first;
    short_run_ = 0;
    short_found_ = 0;
    start_rotated_ = false;
    bits_per_package_ = 0;
    memset(held_, 0, sizeof(held_));
    held_count_ = 0;
    presence_window_ = k_min_presence_stops;
    presence_bits_ = 0;
    detected_bits_ = 0;
    strong_bits_ = 0;
    anti_bits_ = 0;
    tune_run_ = 0;
    rotation_hz_ = 0.0f;
    guard_slots_ = 0;
    guard_packages_ = 0;
    guard_markers_ = 0;
    guard_detected_ = 0;
    guard_balanced_ = 0;
    guard_inner_ = 0;
    guard_edges_ = 0;
    guard_zeros_ = 0;
    guard_loud_zeros_ = 0;
    guard_ones_ = 0;
    guard_bits_ = 0;
    guard_edge_excess_ = 0.0f;
    guard_edge_noise_ = 0.0f;
    guard_edge_energy_ = 0.0f;
    guard_one_energy_ = 0.0f;
    guard_stop_q_ = k_no_guard_q;
    guard_threshold_sum_ = 0.0f;
    guard_ones_level_sum_ = 0.0f;
    guard_started_ = false;
    late_join_ = false;
    confirmed_ = false;
    tone_confirmed_ = false;
    tone_steady_ = false;
    afc_looked_ = false;
    noise_frozen_ = false;
    watch_ = false;
    searching_ = true;

    byte_index_ = 0;
    next_bit_ = 0;
    byte_value_ = 0;
    byte_bits_ = 0;
    byte_skip_ = 0;
    byte_flags_ = 0;
    assembling_ = false;
    memset(byte_soft_, 0, sizeof(byte_soft_));

    memset(&memory_, 0, sizeof(memory_));
    memset(&cold_, 0, sizeof(cold_));
    state_blocks_ = 0;
    tone_blocks_ = 0;
    watch_left_hz_ = 0.0f;
    ban_slot_blocks_ = 0.0f;
    ban_until_block_ = history_.end_block();
    state_ = DecoderState::search;
}

void Decoder::reset() {
    const DecoderState old = state_;
    if (old == DecoderState::preamble || old == DecoderState::track) {
        Event event = make_event(EventType::lost);
        event.reason = LostReason::reset;
        emit(event);
    }
    initialize();
    if (old != DecoderState::search) emit(make_event(EventType::state));
}

void Decoder::process(const int16_t* samples, size_t count) {
    for (size_t i = 0; i < count; ++i) process_sample(samples[i]);
}

void Decoder::process_sample(int16_t sample) {
    if (block_samples_ == 0) return;
    const int32_t x = sample;
    const int32_t scaled = x >> k_energy_shift;
    block_energy_ += static_cast<uint32_t>(scaled * scaled);
    int16_t cos_q15;
    int16_t sin_q15;
    nco_.next(cos_q15, sin_q15);
    cic_.push((x * cos_q15) >> k_mix_shift, -((x * sin_q15) >> k_mix_shift));
    // The search pauses in PREAMBLE after the train and in TRACK; a block it missed samples of is dropped on return.
    const bool searching = state_ == DecoderState::search || watching();
    if (searching && !searching_) search_.interrupt();
    searching_ = searching;
    if (searching && search_.push(sample)) run_search();
    if (++block_fill_ < block_samples_) return;
    block_fill_ = 0;
    int32_t re;
    int32_t im;
    cic_.dump(block_samples_, re, im);
    const uint32_t energy = block_energy_;
    block_energy_ = 0;
    on_block(re, im, energy);
}

DecoderState Decoder::state() const {
    return state_;
}

bool Decoder::dcd() const {
    return state_ != DecoderState::search;
}

float Decoder::tone_hz() const {
    return state_ == DecoderState::search ? 0.0f : nco_.frequency();
}

float Decoder::slot_ms() const {
    if (state_ != DecoderState::preamble && state_ != DecoderState::track) return 0.0f;
    return blocks_to_ms(slot_blocks_);
}

// The marker crest average carries the noise of the crest measurement (its square is A^2 + N_m on average): it is
// removed, or the report reads about 0.3 dB high at the release gates.
float Decoder::snr_db() const {
    if (state_ != DecoderState::preamble && state_ != DecoderState::track) return 0.0f;
    const float crest = reference_average_ * reference_average_ - marker_noise(noise_variance(), slot_blocks_);
    const float power = crest * static_cast<float>(k_decoder_rate_hz);
    const float noise = k_noise_amplitude * k_reference_bandwidth_hz * noise_variance();
    if (power <= 0.0f) return k_snr_floor_db;
    return k_db_per_decade * log10f(power / noise);
}

uint8_t Decoder::bits_per_package() const {
    return bits_per_package_;
}

const DecoderConfig& Decoder::config() const {
    return config_;
}

// ---------------------------------------------------------------------------
// Block pipeline (spec 3.3)
// ---------------------------------------------------------------------------

void Decoder::on_block(int32_t re, int32_t im, uint32_t energy) {
    bool blanked = false;
    if (config_.impulse_blanker) {
        const float fre = static_cast<float>(re);
        const float fim = static_cast<float>(im);
        const float in_bin = k_in_bin_scale * (fre * fre + fim * fim) / static_cast<float>(block_samples_);
        blanked = blanker_.push_block(static_cast<float>(energy), in_bin);
    }
    int32_t out_re = blank_delay_re_[0];
    int32_t out_im = blank_delay_im_[0];
    for (uint8_t i = 0; i + 1 < dsp::ImpulseBlanker::k_latency; ++i) {
        blank_delay_re_[i] = blank_delay_re_[i + 1];
        blank_delay_im_[i] = blank_delay_im_[i + 1];
    }
    blank_delay_re_[dsp::ImpulseBlanker::k_latency - 1] = re;
    blank_delay_im_[dsp::ImpulseBlanker::k_latency - 1] = im;
    if (blanked) {
        out_re = 0;
        out_im = 0;
    }
    history_.push(out_re, out_im, blanked);
    if (settle_blocks_ > 0) {
        --settle_blocks_;
        history_.reset();
    }
    if (state_blocks_ < k_count_limit) ++state_blocks_;
    if (tone_blocks_ < k_count_limit) ++tone_blocks_;
    rebase();

    switch (state_) {
    case DecoderState::search:
        break;
    case DecoderState::acquire:
        if (!tone_confirmed_) run_afc(out_re, out_im);  // a confirmed tone is right; do not chase others
        run_acquire();
        if (state_ == DecoderState::track) run_track();
        break;
    case DecoderState::preamble:
        run_afc(out_re, out_im);
        run_preamble();
        if (state_ == DecoderState::track) run_track();
        break;
    case DecoderState::track:
        run_track();
        break;
    }
}

void Decoder::rebase() {
    const uint32_t span = history_.end_block() - origin_block_;
    if (span <= k_rebase_blocks) return;
    const uint32_t shift = span - k_history_cells;
    origin_block_ += shift;
    const float delta = static_cast<float>(shift);
    grid_position_ -= delta;
    fit_y_ -= delta * fit_weight_;
    fit_xy_ -= delta * fit_x_;
    last_marker_.position -= delta;
    train_last_.position -= delta;
    first_after_ -= delta;
    end_flip_.position -= delta;
    start_.position -= delta;
    last_detected_.position -= delta;
    cold_.anchor -= delta;
    for (uint8_t r = 0; r < k_readings; ++r) {
        readings_[r].start.position -= delta;
        readings_[r].stop.position -= delta;
    }
}

// ---------------------------------------------------------------------------
// State changes
// ---------------------------------------------------------------------------

void Decoder::set_state(DecoderState state) {
    state_blocks_ = 0;
    if (state == state_) return;
    state_ = state;
    emit(make_event(EventType::state));
}

void Decoder::enter_search(bool keep_statistics) {
    if (!keep_statistics) search_.reset();
    search_.clear_exclusion();
    watch_ = false;
    watch_left_hz_ = 0.0f;
    held_count_ = 0;
    bits_per_package_ = 0;
    assembling_ = false;
    cold_.pending = false;
    confirm_pending_ = false;
    end_stop_ = -1;
    set_state(DecoderState::search);
}

void Decoder::lock_tone(float tone_hz) {
    nco_.set_frequency(tone_hz);
    forget_history();
    tone_blocks_ = 0;
    // The floor from before this tone appeared: at high SNR the tone's own sidelobes fill every search bin.
    noise_.reset(max_of(search_.onset_floor() / static_cast<float>(dsp::ToneSearch::k_block_samples),
                        k_min_noise_variance));
    tone_confirmed_ = false;
    afc_looked_ = false;
    noise_frozen_ = true;
    // A train at the fastest accepted T has lines 1 / (2 T_min) away from the tone.
    search_.exclude(tone_hz, k_ms_per_s / (2.0f * static_cast<float>(config_.min_slot_ms)));
    watch_ = true;
    enter_acquire();
}

void Decoder::enter_acquire() {
    afc_.reset();
    afc_sum_.re = 0.0f;
    afc_sum_.im = 0.0f;
    afc_fill_ = 0;
    reset_scales();
    held_count_ = 0;
    bits_per_package_ = 0;
    late_join_ = false;
    assembling_ = false;
    cold_.pending = false;
    confirm_pending_ = false;
    end_stop_ = -1;
    set_state(DecoderState::acquire);
}

// PREAMBLE on a sync train that follows a tune; min_start is its train_end_bound().
void Decoder::enter_preamble(float anchor, float slot_blocks, float amplitude, uint8_t marker_bits, int32_t min_start) {
    slot_blocks_ = slot_blocks;
    seed_train_fit(anchor, slot_blocks);
    end_stop_ = -1;
    min_start_ = min_start;
    tuned_train_ = true;
    // A sync taken past the first START (on package markers): the walk starts again where the train's length ends
    // it, and the markers after it are measured as the packages they are.
    const int32_t origin = min_start_ != dsp::k_no_start && min_start_ < 0 ? min_start_ : 0;
    grid_position_ = anchor + static_cast<float>(origin) * slot_blocks;
    grid_last_ = origin;
    grid_index_ = origin;
    // The sync's markers one T apart are gaps of one already: a few faded markers after them are no T / k reading.
    learner_.reset(origin, sync_train_ones(anchor, slot_blocks, marker_bits), min_start_);
    train_gap_ = 0;
    train_gap_count_ = 0;
    train_markers_ = count_bits(marker_bits);
    sync_hits_ = marker_bits;
    tone_confirmed_ = true;
    reference_average_ = amplitude;  // the train's crest: the newest marker may not be measurable yet
    last_marker_ = measure_marker(grid_position_, slot_blocks);
    last_marker_.detected = origin == 0 || last_marker_.detected;
    train_last_ = last_marker_;
    for (uint8_t r = 0; r < k_readings; ++r) readings_[r].valid = false;
    candidate_reading_ = 0;
    previous_reading_ = k_readings;
    confirm_pending_ = false;
    data_flip_ = false;
    set_state(DecoderState::preamble);
}

// TRACK from the START `start` of package `package_index` with N = bits; the guard starts empty.
void Decoder::enter_track(const Marker& start, uint32_t package_index, uint8_t bits, bool late_join) {
    start_ = start;
    start_rotated_ = false;
    short_cursor_ = k_short_eot_first;
    short_run_ = 0;
    short_found_ = 0;
    package_index_ = package_index;
    bits_per_package_ = bits;
    watch_ = false;
    late_join_ = late_join;
    confirmed_ = false;
    const uint8_t window =
        static_cast<uint8_t>((k_presence_slots + static_cast<uint8_t>(bits + 1u) - 1u) / static_cast<uint8_t>(bits + 1u));
    presence_window_ = window > k_min_presence_stops ? window : k_min_presence_stops;
    presence_bits_ = k_count_limit;
    detected_bits_ = k_count_limit;
    strong_bits_ = 0;
    anti_bits_ = 0;
    tune_run_ = 0;
    rotation_hz_ = 0.0f;
    guard_slots_ = 0;
    guard_packages_ = 0;
    guard_markers_ = 1;  // the first START
    guard_detected_ = start.detected ? 1 : 0;
    guard_balanced_ = 0;
    guard_inner_ = 0;
    guard_edges_ = 0;
    guard_zeros_ = 0;
    guard_loud_zeros_ = 0;
    guard_ones_ = 0;
    guard_bits_ = 0;
    guard_edge_excess_ = 0.0f;
    guard_edge_noise_ = 0.0f;
    guard_edge_energy_ = 0.0f;
    guard_one_energy_ = 0.0f;
    guard_stop_q_ = k_no_guard_q;
    guard_threshold_sum_ = 0.0f;
    guard_ones_level_sum_ = 0.0f;
    guard_started_ = start.detected;
    held_count_ = 0;
    assembling_ = false;
    audit_.reset(static_cast<uint8_t>(2u * bits + 1u));
    track_noise_.reset(noise_.mean_estimate());
    if (late_join) reference_average_ = start.amplitude;
    last_detected_ = start;
    last_detected_package_ = package_index;
    cold_.pending = false;
    confirm_pending_ = false;
    end_stop_ = -1;
    set_state(DecoderState::track);
}

// After a lost signal the tone is kept (a fade on the same tone relocks from the station memory); the guard's
// refusals search again (the next transmission may be on another tone). ACQUIRE continues from the noise TRACK
// measured.
void Decoder::lose(LostReason reason, bool keep_tone) {
    if (state_ == DecoderState::track) {
        noise_.reset(track_noise_.mean_estimate());
        noise_frozen_ = true;
        if (confirmed_ && reason == LostReason::signal_gone) remember_station(false);
        if (confirmed_) tone_blocks_ = 0;
        watch_ = false;
        tone_steady_ = false;  // the held tone is the stream's pitch now, not a tune
    }
    held_count_ = 0;
    assembling_ = false;
    Event event = make_event(EventType::lost);
    event.reason = reason;
    emit(event);
    bits_per_package_ = 0;
    confirmed_ = false;
    if (reason == LostReason::signal_gone && !keep_tone) {
        enter_search();
    } else {
        enter_acquire();
    }
}

// END: a confirmed lock releases what it holds and remembers the station; `last_package` is the END's package.
void Decoder::finish(uint32_t last_package) {
    if (confirmed_) {
        release_held();
        remember_station(true);
    }
    held_count_ = 0;
    assembling_ = false;
    Event event = make_event(EventType::end);
    event.package_index = last_package;
    emit(event);
    bits_per_package_ = 0;
    confirmed_ = false;
    enter_search();
}

// ---------------------------------------------------------------------------
// SEARCH and ACQUIRE (spec 3.6, 3.7)
// ---------------------------------------------------------------------------

// In SEARCH: lock on the search's candidate. While watching (a lock taken from the search that has not reached its
// first package): move to another tone as soon as it turns from steady into a marker train. A watched tone that the
// search masks as a steady carrier is dropped; the search keeps its statistics, so the mask stands and a tone next
// to the carrier can be taken.
void Decoder::run_search() {
    float tone = 0.0f;
    if (state_ == DecoderState::search) {
        if (!search_.candidate(tone)) return;
        tone_steady_ = search_.steady();
        // An unsteady tone near the pitch of the station heard in the last minute is its stream (spec 3.12).
        if (!tone_steady_ && memory_valid() && fabsf(tone - memory_.tone_hz) <= k_memory_search_margin_hz) {
            lock_tone(memory_.tone_hz);
            watch_ = false;
            afc_looked_ = true;  // the remembered pitch was measured by the AFC already
            noise_frozen_ = false;
            return;
        }
    } else {
        if (state_ == DecoderState::acquire && search_.masked(nco_.frequency())) {
            enter_search(true);
            return;
        }
        const float recent = search_.recent_floor() / static_cast<float>(dsp::ToneSearch::k_block_samples);
        if (noise_frozen_ && recent < k_noise_drop * noise_.mean_estimate()) {
            noise_.reset(max_of(recent, k_min_noise_variance));
        }
        const bool preamble = state_ == DecoderState::preamble;
        const uint8_t products = preamble ? k_onset_products_preamble
                                          : (tone_steady_ ? k_onset_products_tune : k_onset_products);
        if (!search_.train_onset(tone, products)) return;
        // The tone the watch left for this train is not a new transmission: a beep of this one breaks its phase.
        if (preamble && fabsf(tone - watch_left_hz_) <= k_memory_search_margin_hz) return;
        if ((preamble && train_line(tone)) || harmonic_image(tone)) return;
        watch_left_hz_ = nco_.frequency();
        tone_steady_ = true;  // measured on the tune tone before the train began
    }
    lock_tone(tone);
}

bool Decoder::watching() const {
    if (!watch_) return false;
    if (state_ == DecoderState::acquire) return true;
    return state_ == DecoderState::preamble && learner_.bits() == 0 && !confirm_pending_ && end_stop_ < 0;
}

// A line (2 m + 1) / (2 T) of this preamble's own train.
bool Decoder::train_line(float tone_hz) const {
    const float slot_s = slot_blocks_ * static_cast<float>(block_samples_) / static_cast<float>(k_decoder_rate_hz);
    const float orders = fabsf(tone_hz - nco_.frequency()) * slot_s - k_slot_centre;  // m + fraction, in 1 / T
    const float order = floorf(orders + k_slot_centre);
    return order >= 0.0f && order <= k_train_line_orders && fabsf(orders - order) <= k_train_line_hz * slot_s;
}

// An odd harmonic of the held tone, folded into 0..4000 Hz by the 8 kHz sampling.
bool Decoder::harmonic_image(float tone_hz) const {
    const float rate = static_cast<float>(k_decoder_rate_hz);
    for (uint8_t order = k_first_image_order; order <= k_last_image_order; order = static_cast<uint8_t>(order + 2u)) {
        float image = fmodf(static_cast<float>(order) * nco_.frequency(), rate);
        if (image > 0.5f * rate) image = rate - image;
        if (fabsf(tone_hz - image) <= k_image_reach_hz) return true;
    }
    return false;
}

void Decoder::run_acquire() {
    if (state_blocks_ % k_noise_blocks == 0) push_block_noise();
    // A lock on data (no tune: a mid-stream start) can be tens of Hz off until the fine AFC has had one look; a
    // continuous beep then rotates across a half window and reads as a flip. Weak signals wait a bounded time.
    const uint32_t wait_blocks = static_cast<uint32_t>(ms_to_blocks(k_afc_wait_ms));
    const bool tuned = tone_steady_ || tone_confirmed_ || afc_looked_ || state_blocks_ >= wait_blocks;
    if (cold_.pending) {
        cold_join_step();
        if (state_ != DecoderState::acquire) return;
    }
    run_candidates(tuned);
    if (state_ != DecoderState::acquire) return;
    // Counted from the tone's lock or the end of a confirmed lock: PREAMBLEs that found nothing do not extend it.
    float timeout_ms = max_of(k_acquire_timeout_ms, k_acquire_timeout_slots * static_cast<float>(config_.max_slot_ms()));
    if (stream_relock()) {
        const float package_ms =
            blocks_to_ms(static_cast<float>(memory_.bits_per_package + 1u) * memory_.slot_blocks);
        timeout_ms = max_of(timeout_ms, static_cast<float>(k_stream_timeout_packages) * package_ms);
    }
    // A cold late join sees k_cold_join_intervals + 1 markers of the longest package it can join, then folds.
    const uint16_t joinable =
        static_cast<uint16_t>(k_max_bits_per_package / k_cold_join_step_bits * k_cold_join_step_bits);
    if (joinable > 0) {
        const float longest_ms = min_of(static_cast<float>(k_max_package_us / k_us_per_ms),
                                        static_cast<float>((joinable + 1u) * config_.max_slot_ms()));
        timeout_ms = max_of(timeout_ms, static_cast<float>(k_cold_join_intervals + 1u) * longest_ms);
    }
    if (cold_.pending || tone_blocks_ < static_cast<uint32_t>(ms_to_blocks(timeout_ms))) return;
    // Banned only while still there (a carrier, keyed CW, speech): a tune heard alone whose train was missed does not
    // ban the station's retry, nor does the pitch of the station heard in the last minute.
    const bool remembered = memory_valid() && fabsf(nco_.frequency() - memory_.tone_hz) <= k_memory_search_margin_hz;
    if (!tone_confirmed_ && !remembered && search_.present(nco_.frequency())) {
        const float base = k_search_ban_ms * k_samples_per_ms / static_cast<float>(dsp::ToneSearch::k_block_samples);
        search_.ban(nco_.frequency(), static_cast<uint16_t>(base));
    }
    enter_search();
}

void Decoder::run_afc(int32_t re, int32_t im) {
    afc_sum_.re += static_cast<float>(re);
    afc_sum_.im += static_cast<float>(im);
    if (++afc_fill_ < afc_decimation_) return;
    const float scale = 1.0f / (k_mixer_gain * static_cast<float>(block_samples_ * afc_decimation_));
    Complex decimated;
    decimated.re = afc_sum_.re * scale;
    decimated.im = afc_sum_.im * scale;
    afc_.push(decimated);
    afc_sum_.re = 0.0f;
    afc_sum_.im = 0.0f;
    afc_fill_ = 0;
    // First look after k_afc_min_inputs inputs, then every k_afc_eval_ms.
    const uint16_t inputs = afc_.inputs();
    const uint16_t period = static_cast<uint16_t>(ms_to_blocks(k_afc_eval_ms) / static_cast<float>(afc_decimation_));
    if (inputs < k_afc_min_inputs || (inputs - k_afc_min_inputs) % period != 0) return;
    // A running stream on the remembered pitch: the AFC measured it already (spec 3.12).
    if (stream_relock()) return;
    float offset = 0.0f;
    if (!afc_.offset(offset, !tone_steady_)) return;
    // The train that started PREAMBLE held its flips over half windows of 0.35 T: the tone is within a fraction of
    // 1 / T. A larger correction there is a noise peak that would walk the lock off the tone.
    if (state_ == DecoderState::preamble && fabsf(offset) * blocks_to_ms(slot_blocks_) / k_ms_per_s > k_preamble_afc_limit) {
        return;
    }
    const bool first_look = !afc_looked_;
    afc_looked_ = true;
    if (fabsf(offset) < k_afc_min_offset_hz) return;
    nco_.adjust_frequency(offset);
    afc_.reset();
    // Windows mixed before a large first correction rotate across a marker half window and read as flips: a lock on
    // data (no tune) can start 25 Hz off. In ACQUIRE they are measured again at the new tone.
    const float slowest_s = static_cast<float>(config_.max_slot_ms()) / k_ms_per_s;
    const float rotation = k_two_pi * fabsf(offset) * k_marker_half * slowest_s;
    if (first_look && !tone_steady_ && state_ == DecoderState::acquire && rotation > k_afc_reset_rotation) {
        forget_history();
    }
}

void Decoder::forget_history() {
    history_.reset();
    origin_block_ = history_.end_block();
    settle_blocks_ = k_settle_blocks;
    candidates_.reset();
    reset_scales();
    cold_.pending = false;
}

// The noise is measured in ACQUIRE only on a stream relock (between its markers and beeps the windows under 4 times
// the estimate are noise); a lock from the search keeps the floor from before its tone (spec 3.3).
void Decoder::push_block_noise() {
    if (noise_frozen_ && !stream_relock()) return;
    const float end = end_position();
    const float from = end - static_cast<float>(k_noise_blocks);
    Complex sum;
    if (!history_.window(origin_block_, from, end, sum) || history_.any_blanked(origin_block_, from, end)) return;
    const float power = magnitude_squared(sum) / dsp::noise_samples(k_noise_blocks, block_samples_);
    // A strong tone fills every window: it would drag the 25 % point up by 1/64 per push.
    if (power > k_noise_outlier * noise_variance()) return;
    noise_.push(power);
}

void Decoder::run_candidates(bool tuned) {
    const float end = end_position();
    const bool stream = stream_relock();
    for (uint8_t s = 0; s < k_candidate_scales; ++s) {
        const float half = static_cast<float>(k_candidate_scale_blocks[s]);
        const float centre = end - half - 1.0f;
        const FlipMeasure m = measure_flip(centre, half);
        const float q = m.valid ? m.q_balanced : k_no_value;
        const float older = scale_q_[s][0];
        const float newer = scale_q_[s][1];
        const float newer_kappa = scale_kappa_[s];
        scale_q_[s][0] = newer;
        scale_q_[s][1] = q;
        scale_kappa_[s] = m.valid ? m.kappa : k_no_value;
        if (stream && s + 1u == k_candidate_scales && stream_tune(end, half)) tone_steady_ = true;
        if (newer < k_q_candidate || newer_kappa < k_kappa_candidate || newer <= older || newer < q) continue;
        if (stream && m.amplitude < k_stream_marker_ratio * memory_.crest) continue;

        const float fraction = (older > k_no_value && q > k_no_value) ? dsp::parabolic_offset(older, newer, q) : 0.0f;
        const float position = centre - 1.0f + fraction;
        const float whole = floorf(position);
        Candidate candidate;
        candidate.block = origin_block_ + static_cast<uint32_t>(static_cast<int32_t>(whole));
        candidate.fraction = position - whole;
        candidate.q = newer;
        candidate.scale = s;
        if (!candidates_.add(candidate)) continue;
        // A train on the station's pitch ends its stream: the next transmission (its END may have been missed) or data
        // beeps twisted into one. The station memory's package count must not carry past it.
        if (memory_usable() && train_behind(candidate, memory_.slot_blocks)) memory_.ended = true;
        if (!tuned) continue;
        if (try_sync(candidate) || try_late_join(candidate) || try_cold_join(candidate)) return;
    }
}

bool Decoder::try_sync(const Candidate& candidate) {
    const float centre = candidate_position(candidate);
    // Hypotheses T = (c - b) / m merged within 3 % of the first of their group; each group's T is fitted to all of its
    // pairs (c - b = m T, least squares), again without the pairs that miss the first fit by 10 % of T: one imprecise
    // or noise candidate does not set the grid.
    float hypothesis[k_max_hypotheses];
    float sum_md[k_max_hypotheses];
    float sum_mm[k_max_hypotheses];
    uint8_t hypotheses = 0;
    for (uint8_t pass = 0; pass < k_hypothesis_passes; ++pass) {
        float fitted[k_max_hypotheses];
        for (uint8_t h = 0; h < hypotheses; ++h) {
            fitted[h] = sum_md[h] / sum_mm[h];
            sum_md[h] = 0.0f;
            sum_mm[h] = 0.0f;
        }
        for (uint8_t age = 1; age < candidates_.count(); ++age) {
            const float distance = centre - candidate_position(candidates_.newest(age));
            if (distance <= 0.0f) continue;
            for (uint8_t m = 1; m < k_sync_positions; ++m) {
                const float slot = distance / static_cast<float>(m);
                if (!in_range(slot) || banned(slot)) continue;
                uint8_t group = hypotheses;
                for (uint8_t h = 0; h < hypotheses && group == hypotheses; ++h) {
                    if (fabsf(slot - hypothesis[h]) <= k_hypothesis_merge * hypothesis[h]) group = h;
                }
                if (group == hypotheses) {
                    if (pass > 0 || hypotheses == k_max_hypotheses) continue;
                    hypothesis[hypotheses] = slot;
                    sum_md[hypotheses] = 0.0f;
                    sum_mm[hypotheses] = 0.0f;
                    ++hypotheses;
                }
                const float weight = static_cast<float>(m);
                const bool outlier = fabsf(distance - weight * fitted[group]) > k_refine_max_change * fitted[group];
                if (pass > 0 && outlier) continue;
                sum_md[group] += weight * distance;
                sum_mm[group] += weight * weight;
            }
        }
        for (uint8_t h = 0; h < hypotheses && pass > 0; ++h) {
            if (sum_mm[h] <= 0.0f) {
                sum_md[h] = fitted[h];
                sum_mm[h] = 1.0f;
            }
        }
    }
    float accepted_slot[k_max_accepted];
    float accepted_evidence[k_max_accepted];
    bool accepted_strong[k_max_accepted];
    uint8_t accepted_count = 0;
    for (uint8_t h = 0; h < hypotheses; ++h) {
        const float slot = sum_md[h] / sum_mm[h];
        if (!in_range(slot) || banned(slot)) continue;
        const SyncScore score = sync_score(centre, slot);
        if (score.evidence < k_sync_evidence ||
            !enough_hits(score.hit_bits, score.marker_bits, score.hits, score.positions)) {
            continue;
        }
        uint8_t index = accepted_count;
        if (accepted_count < k_max_accepted) {
            ++accepted_count;
        } else {
            index = 0;
            for (uint8_t i = 1; i < k_max_accepted; ++i) {
                if (accepted_evidence[i] < accepted_evidence[index]) index = i;
            }
            if (accepted_evidence[index] >= score.evidence) continue;
        }
        accepted_slot[index] = slot;
        accepted_evidence[index] = score.evidence;
        accepted_strong[index] = strong_hits(score.hit_bits, score.hits);
    }
    if (accepted_count == 0) return false;
    // A reading taken on weak markers yields to one on hits alone within 10 % of its T.
    for (uint8_t i = 0; i < accepted_count; ++i) {
        for (uint8_t j = 0; j < accepted_count && !accepted_strong[i]; ++j) {
            if (accepted_strong[j] && fabsf(accepted_slot[i] / accepted_slot[j] - 1.0f) <= k_refine_max_change) {
                accepted_evidence[i] = k_no_value;
            }
        }
    }
    uint8_t best = 0;
    for (uint8_t i = 1; i < accepted_count; ++i) {
        if (accepted_evidence[i] > accepted_evidence[best]) best = i;
    }
    // A long train also supports 2T, 3T...: prefer the smallest integer sub-multiple of the best with comparable
    // evidence (a non-harmonic smaller T is a different, weaker reading).
    float slot = accepted_slot[best];
    for (uint8_t i = 0; i < accepted_count; ++i) {
        if (accepted_evidence[i] < k_prefer_smaller_t * accepted_evidence[best] || accepted_slot[i] >= slot) continue;
        const float ratio = accepted_slot[best] / accepted_slot[i];
        const float whole = floorf(ratio + k_slot_centre);
        if (whole >= 2.0f && fabsf(ratio - whole) <= k_hypothesis_merge * whole) slot = accepted_slot[i];
    }
    // A train at 2T read at T / 2 has its markers at the even positions only: prefer the double while it has more
    // evidence.
    SyncScore chosen = sync_score(centre, slot);
    while (count_bits(chosen.hit_bits & k_sync_odd_mask) <= k_half_rate_odd_hits) {
        // A half-rate reading whose double is out of range belongs to a slower sender: not ours.
        if (!in_range(2.0f * slot)) return false;
        if (banned(2.0f * slot)) break;
        const SyncScore doubled = sync_score(centre, 2.0f * slot);
        if (doubled.evidence <= chosen.evidence) break;
        slot *= 2.0f;
        chosen = doubled;
    }
    float anchor = centre;
    refine_sync(anchor, slot);
    // The least-squares T may leave the window (a sender we must not hear) or move onto a banned alias.
    if (!in_range(slot) || banned(slot)) return false;
    // The refined reading must still be a train: markers of a package chain a little longer than the window's
    // slowest T line up with a train only at its first positions, and the fit moves off them.
    const SyncScore refined = sync_score(anchor, slot);
    if (refined.evidence < k_sync_evidence ||
        !enough_hits(refined.hit_bits, refined.marker_bits, refined.hits, refined.positions)) {
        return false;
    }
    // A train with every other marker faded reads at 2 T, but the carrier still reversed at the markers it lost
    // (spec 1.1): T / 2, with the hidden markers counted.
    uint8_t marker_bits = chosen.marker_bits;
    uint8_t hidden = 0;
    if (bridges_trusted() && hidden_midpoints(anchor, slot, refined.hit_bits, hidden) &&
        in_range(k_slot_centre * slot) && !banned(k_slot_centre * slot)) {
        uint8_t spread = 0;
        for (uint8_t i = 0; 2u * i + 1u < k_sync_positions; ++i) {
            if ((marker_bits & (1u << i)) != 0) spread = static_cast<uint8_t>(spread | (1u << (2u * i)));
            if ((hidden & (1u << i)) != 0) spread = static_cast<uint8_t>(spread | (1u << (2u * i + 1u)));
        }
        marker_bits = spread;
        slot *= k_slot_centre;
    }
    // A transmission starts with its tune (spec 2.1): a train with none before it is data beeps of a transmission
    // this receiver lost (fading and filter edges twist them), or a new transmission whose tune faded. Neither can
    // number the bytes: not from its package 0, and not from the station memory, whose count a new transmission breaks
    // (its END may have been missed). The memory is dropped; a cold join takes the transmission further on.
    bool silent = false;
    const int32_t min_start = train_end_bound(anchor, slot, chosen.amplitude, silent);
    if (!train_follows_tune(min_start, silent, slot)) {
        if (stream_relock()) memory_.valid = false;
        return false;
    }
    enter_preamble(anchor, slot, chosen.amplitude, marker_bits, min_start);
    return true;
}

// Hits one T apart whose carrier reversed between them: markers faded at the midpoints. Decisive reversals only, and
// no steady carrier between two hits (a real train of T keeps it).
bool Decoder::hidden_midpoints(float centre, float slot_blocks, uint8_t hit_bits, uint8_t& hidden_bits) const {
    uint8_t reversed = 0;
    uint8_t steady = 0;
    hidden_bits = 0;
    for (uint8_t i = 0; i + 1u < k_sync_positions; ++i) {
        const uint32_t pair = (1u << i) | (1u << (i + 1u));
        if ((hit_bits & pair) != pair) continue;
        const float newer = centre - static_cast<float>(i) * slot_blocks;
        const int8_t parity = carrier_parity(bridge(newer - slot_blocks, newer, k_slot_centre * slot_blocks));
        if (parity > 0) {
            ++reversed;
            hidden_bits = static_cast<uint8_t>(hidden_bits | (1u << i));
        } else if (parity < 0) {
            ++steady;
        }
    }
    return reversed >= k_hidden_midpoints && steady == 0;
}

// Evidence of a marker train of period slot_blocks ending at centre. The best prefix of at least
// k_sync_min_positions positions counts, so a train whose older positions still fall in the tune tone (strongly
// anti-flip) is judged on the markers it already has.
Decoder::SyncScore Decoder::sync_score(float centre, float slot_blocks) const {
    const float span = k_position_search * slot_blocks;
    const float half = k_marker_half * slot_blocks;
    FlipMeasure measures[k_sync_positions];
    float places[k_sync_positions];  // the markers found, else their predictions
    float strongest = 0.0f;
    float predicted = centre;
    for (uint8_t i = 0; i < k_sync_positions; ++i) {
        FlipMeasure m = search_flip(predicted, span, half, k_kappa_track);
        // A flip peaking beyond the span belongs to a marker off this grid (a wrong T): no marker here.
        if (peak_beyond(m, predicted, span, half)) m.valid = false;
        measures[i] = m;
        if (balanced_flip(m)) {
            strongest = max_of(strongest, m.amplitude);
            // Each train marker found inside the span places the next one: the hypothesis's T error does not add up
            // over the train.
            if (m.amplitude >= k_train_amplitude_ratio * strongest && !on_edge(m, predicted, span, half)) {
                predicted = m.position;
            }
        }
        places[i] = predicted;
        predicted -= slot_blocks;
    }
    SyncScore best;
    best.evidence = k_no_value;
    best.amplitude = 0.0f;
    best.boundary = 0.0f;
    best.hit_bits = 0;
    best.marker_bits = 0;
    best.hits = 0;
    best.positions = 0;
    // Between two train markers the carrier is continuous. A tone far off the NCO rotates in every window and reads as
    // a flip everywhere, midpoints included, with both halves alike: that is not a train. (Next to a faded marker a
    // midpoint sees a beep on one side only.)
    uint8_t midpoint_flips = 0;
    for (uint8_t i = 0; i < k_sync_midpoints; ++i) {
        const FlipMeasure m = measure_flip(k_slot_centre * (places[i] + places[i + 1]), half);
        if (balanced_flip(m)) ++midpoint_flips;
    }
    if (midpoint_flips >= k_sync_max_midpoint_flips) return best;
    bool hit[k_sync_positions];
    bool marker[k_sync_positions];
    for (uint8_t i = 0; i < k_sync_positions; ++i) {
        const FlipMeasure& m = measures[i];
        hit[i] = balanced_flip(m) && m.amplitude >= k_train_amplitude_ratio * strongest;
        marker[i] = hit[i] || (m.valid && m.q >= k_sync_weak_q && m.kappa >= k_kappa_track &&
                               m.amplitude >= k_flip_amplitude_ratio * strongest);
    }
    float evidence = 0.0f;
    float amplitude = 0.0f;
    uint8_t hits = 0;
    uint8_t hit_bits = 0;
    uint8_t marker_bits = 0;
    for (uint8_t i = 0; i < k_sync_positions; ++i) {
        // Train markers share one crest: a far weaker flip is noise or ringing, not a train marker.
        const bool weak = is_flip(measures[i], k_kappa_track) && !hit[i];
        evidence += weak ? 0.0f : clipped_evidence(measures[i]);
        if (hit[i]) {
            ++hits;
            amplitude += measures[i].amplitude;
            hit_bits = static_cast<uint8_t>(hit_bits | (1u << i));
        }
        if (marker[i]) marker_bits = static_cast<uint8_t>(marker_bits | (1u << i));
        if (i + 1 >= k_sync_min_positions && evidence > best.evidence) {
            best.evidence = evidence;
            best.hit_bits = hit_bits;
            best.marker_bits = marker_bits;
            best.amplitude = hits > 0 ? amplitude / static_cast<float>(hits) : 0.0f;
            best.hits = hits;
            best.positions = static_cast<uint8_t>(i + 1u);
        }
    }
    // Checked last: most hypotheses fail on evidence already.
    if (best.evidence >= k_sync_evidence && enough_hits(best.hit_bits, best.marker_bits, best.hits, best.positions)) {
        float allowance = 0.0f;
        best.boundary = boundary_energy(measures, marker, slot_blocks, best.amplitude, allowance);
        if (best.boundary > k_boundary_max + allowance) best.evidence = k_no_value;
    }
    return best;
}

// Energy in +-k_boundary_half slots around a slot boundary, less the noise, over the energy a tone of the given crest
// has there (a real boundary: 0.015); noise_ratio receives the noise's share on the same scale.
bool Decoder::boundary_excess(float boundary, float slot_blocks, float crest, float& excess, float& noise_ratio) const {
    const float half = k_boundary_half * slot_blocks;
    const float full = k_slot_centre * crest * 2.0f * half * static_cast<float>(block_samples_);
    Complex sum;
    if (full <= 0.0f || !history_.window(origin_block_, boundary - half, boundary + half, sum)) return false;
    const float noise = noise_variance() * dsp::noise_samples(2.0f * half, block_samples_);
    excess = (magnitude_squared(sum) - noise) / (full * full);
    noise_ratio = noise / (full * full);
    return true;
}

// Mean boundary_excess() between consecutive markers of a train; 0 when no two markers are neighbours. `allowance`
// receives k_marker_edge_sigmas standard deviations of that mean on noise alone.
float Decoder::boundary_energy(const FlipMeasure* measures, const bool* marker, float slot_blocks, float crest,
                               float& allowance) const {
    float total = 0.0f;
    float noise = 0.0f;
    uint8_t pairs = 0;
    for (uint8_t i = 0; i + 1 < k_sync_positions; ++i) {
        if (!marker[i] || !marker[i + 1]) continue;
        float excess = 0.0f;
        float noise_ratio = 0.0f;
        if (!boundary_excess(k_slot_centre * (measures[i].position + measures[i + 1].position), slot_blocks, crest,
                             excess, noise_ratio)) {
            continue;
        }
        total += excess;
        noise += noise_ratio;
        ++pairs;
    }
    if (pairs == 0) {
        allowance = 0.0f;
        return 0.0f;
    }
    const float count = static_cast<float>(pairs);
    allowance = k_marker_edge_sigmas * (noise / count) / sqrtf(count);
    return total / count;
}

void Decoder::refine_sync(float& centre, float& slot_blocks) const {
    for (uint8_t pass = 0; pass < k_refine_passes; ++pass) {
        const float span = k_position_search * slot_blocks;
        const float half = k_marker_half * slot_blocks;
        float sum_x = 0.0f;
        float sum_y = 0.0f;
        float sum_xx = 0.0f;
        float sum_xy = 0.0f;
        uint8_t points = 0;
        float newest = centre;
        bool have_newest = false;
        float predicted = centre + slot_blocks;
        for (uint8_t i = 0; i < k_sync_positions; ++i) {
            predicted -= slot_blocks;
            const FlipMeasure m = search_flip(predicted, span, half, k_kappa_track);
            if (!balanced_flip(m) || peak_beyond(m, predicted, span, half)) continue;
            predicted = m.position;
            if (i == 0) {
                newest = m.position;
                have_newest = true;
            }
            // x relative to the anchor keeps the float sums well conditioned
            const float x = -static_cast<float>(i);
            const float y = m.position - centre;
            sum_x += x;
            sum_y += y;
            sum_xx += x * x;
            sum_xy += x * y;
            ++points;
        }
        if (points < k_refine_min_points) {
            if (have_newest) centre = newest;
            return;
        }
        const float n = static_cast<float>(points);
        const float denominator = n * sum_xx - sum_x * sum_x;
        if (denominator <= 0.0f) return;
        const float slope = (n * sum_xy - sum_x * sum_y) / denominator;
        const float intercept = (sum_y - slope * sum_x) / n;
        if (fabsf(slope - slot_blocks) > k_refine_max_change * slot_blocks) return;
        slot_blocks = slope;
        centre += intercept;
    }
}

// A tune on the held pitch (the next transmission, not the stream): steady over the widest candidate scale or
// k_stream_tune_half of the remembered T if longer, just before `end`.
bool Decoder::stream_tune(float end, float widest_half) const {
    const float half = max_of(widest_half, k_stream_tune_half * memory_.slot_blocks);
    const FlipMeasure m = measure_flip(end - half - 1.0f, half);
    return m.valid && m.kappa <= k_tune_kappa && m.energy >= k_tune_energy && m.steady >= k_tune_crest * memory_.crest;
}

// Relock after a fade (spec 3.12): three equal intervals of the remembered package period behind the newest candidate;
// the elapsed packages give its package index.
bool Decoder::try_late_join(const Candidate& candidate) {
    if (!memory_usable()) return false;
    const float slots = static_cast<float>(memory_.bits_per_package + 1u);
    const float centre = candidate_position(candidate);
    float best = 0.0f;
    for (uint8_t age = 1; age < candidates_.count() && best <= 0.0f; ++age) {
        const float distance = centre - candidate_position(candidates_.newest(age));
        if (distance <= 0.0f) continue;
        for (uint8_t k = 1; k <= k_late_join_intervals; ++k) {
            const float slot = distance / (slots * static_cast<float>(k));
            if (fabsf(slot / memory_.slot_blocks - 1.0f) > k_late_join_t || banned(slot)) continue;
            uint8_t hits = 0;
            for (uint8_t j = 1; j <= k_late_join_intervals; ++j) {
                if (has_candidate_near(centre - slots * static_cast<float>(j) * slot, k_position_search * slot,
                                       k_late_join_q)) {
                    ++hits;
                }
            }
            if (hits == k_late_join_intervals) {
                best = slot;
                break;
            }
        }
    }
    if (best <= 0.0f) return false;
    const FlipMeasure m = search_flip(centre, k_position_search * best, k_marker_half * best, k_kappa_track);
    const float position = is_flip(m, k_kappa_track) ? m.position : centre;
    const float packages = (position - memory_position()) / (slots * memory_.slot_blocks);
    const float whole = floorf(packages + k_slot_centre);
    if (fabsf(packages - whole) > k_relock_fraction || whole < 0.0f || whole > k_relock_packages) return false;
    const uint32_t index = memory_.marker_package + static_cast<uint32_t>(whole);
    // TRACK starts at the oldest marker of the chain the history still holds: the guard's packages are there already.
    float start = position;
    uint8_t back = 0;
    chain_start(position, slots * best, best, k_late_join_intervals, start, back);
    if (back > index) back = static_cast<uint8_t>(index);
    slot_blocks_ = best;
    enter_track(measure_marker(start, best), index - back, memory_.bits_per_package, true);
    tone_confirmed_ = true;
    return true;
}

// The oldest of the chain's markers newest - j period (j <= intervals) whose package the history still holds whole;
// each is taken at its candidate when one is near, else where predicted.
bool Decoder::chain_start(float newest, float period, float slot_blocks, uint8_t intervals, float& start,
                          uint8_t& back) const {
    const float half = k_marker_half * slot_blocks;
    for (uint8_t j = intervals; j > 0; --j) {
        float position = newest - static_cast<float>(j) * period;
        for (uint8_t age = 0; age < candidates_.count(); ++age) {
            const float at = candidate_position(candidates_.newest(age));
            if (fabsf(at - position) <= k_position_search * slot_blocks) position = at;
        }
        Complex sum;
        if (history_.window(origin_block_, position - half - 1.0f, position, sum)) {
            start = position;
            back = j;
            return true;
        }
    }
    start = newest;
    back = 0;
    return false;
}

// Candidates one T apart behind `candidate`, k_sync_min_hits in a row: a train (data slots never flip, so packages
// never show more than their STOP and two END markers in a row).
bool Decoder::train_behind(const Candidate& candidate, float slot_blocks) const {
    const float centre = candidate_position(candidate);
    for (uint8_t k = 1; k < k_sync_min_hits; ++k) {
        if (!has_candidate_near(centre - static_cast<float>(k) * slot_blocks, k_position_search * slot_blocks,
                                k_q_candidate)) {
            return false;
        }
    }
    return true;
}

bool Decoder::has_candidate_near(float position, float tolerance, float q_min) const {
    return candidate_q_near(position, tolerance) >= q_min;
}

// The strongest candidate within `tolerance` of `position`, 0 = none.
float Decoder::candidate_q_near(float position, float tolerance) const {
    float best = 0.0f;
    for (uint8_t age = 0; age < candidates_.count(); ++age) {
        const Candidate& c = candidates_.newest(age);
        if (fabsf(candidate_position(c) - position) <= tolerance) best = max_of(best, c.q);
    }
    return best;
}

// Cold late join (spec 3.12, V7): with no station memory and no tune, candidates forming three equal intervals P
// longer than any train of the window are a package chain; its slot grid is found by folding the pitch's energy.
bool Decoder::try_cold_join(const Candidate& candidate) {
    if (cold_.pending || memory_usable() || candidate.q < k_late_join_q) return false;
    const float centre = candidate_position(candidate);
    for (uint8_t age = 1; age < candidates_.count(); ++age) {
        const float period = centre - candidate_position(candidates_.newest(age));
        if (period <= 0.0f || in_range(period)) continue;
        const float tolerance = k_cold_join_tolerance * period;
        float weakest = min_of(candidate.q, candidates_.newest(age).q);
        for (uint8_t j = 2; j <= k_cold_join_intervals && weakest >= k_late_join_q; ++j) {
            weakest = min_of(weakest, candidate_q_near(centre - static_cast<float>(j) * period, tolerance));
        }
        if (weakest < k_late_join_q) continue;
        // No tune or train inside it: data slots never flip, so only noise, far weaker than the chain's markers, may
        // add a candidate between them.
        uint8_t stray = 0;
        for (uint8_t a = 0; a < candidates_.count(); ++a) {
            if (candidates_.newest(a).q < k_cold_stray_ratio * weakest) continue;
            const float at = candidate_position(candidates_.newest(a));
            const float offset = centre - at;
            if (offset <= tolerance || offset >= static_cast<float>(k_cold_join_intervals) * period + tolerance) continue;
            const float whole = floorf(offset / period + k_slot_centre);
            if (fabsf(offset - whole * period) > tolerance) ++stray;
        }
        if (stray > k_cold_stray_candidates) continue;
        cold_.period = period;
        bool any = false;
        for (uint8_t h = 0; h < k_cold_hypotheses; ++h) any = any || cold_bits(h) != 0;
        if (!any) continue;
        // Fold from the oldest chain marker whose package the history holds.
        float start = centre;
        uint8_t back = 0;
        chain_start(centre, period, period / static_cast<float>(k_cold_join_step_bits + 1u), k_cold_join_intervals,
                    start, back);
        if (back == 0) continue;
        memset(cold_.edge, 0, sizeof(cold_.edge));
        memset(cold_.centre, 0, sizeof(cold_.centre));
        cold_.anchor = start;
        cold_.folded = 0;
        cold_.pending = true;
        cold_join_step();
        return true;
    }
    return false;
}

// Folds each package of the chain once the history holds it; decides after k_cold_join_fold_packages and at most
// k_cold_join_max_packages: exactly one N whose grid has quiet edges, and none of its odd sub-grids.
void Decoder::cold_join_step() {
    const float period = cold_.period;
    const float margin = (k_marker_half + k_slot_centre) * period / static_cast<float>(k_cold_join_step_bits + 1u);
    while (cold_.pending && cold_.folded < k_cold_join_max_packages) {
        const float stop = chain_marker(cold_.folded + 1u);
        if (stop + margin + 1.0f > end_position()) return;
        fold_package(chain_marker(cold_.folded), stop);
        ++cold_.folded;
        if (cold_.folded < k_cold_join_fold_packages) continue;
        uint8_t passing = 0;
        uint8_t chosen = 0;
        for (uint8_t h = 0; h < k_cold_hypotheses; ++h) {
            if (cold_bits(h) == 0 || !fold_passes(h, 0)) continue;
            bool alias = false;
            for (uint8_t g = 1; g < k_fold_grids; ++g) alias = alias || fold_passes(h, g);
            if (alias || !fold_unrivalled(cold_bits(h))) continue;
            ++passing;
            chosen = h;
        }
        if (passing == 1) {
            const uint8_t bits = cold_bits(chosen);
            const float slot = period / static_cast<float>(bits + 1u);
            cold_.pending = false;
            // TRACK from the oldest folded package the history still holds whole.
            float start = cold_.anchor;
            for (uint8_t k = 0; k < cold_.folded; ++k) {
                Complex sum;
                if (history_.window(origin_block_, start - k_marker_half * slot - 1.0f, start, sum)) break;
                start += period;
            }
            slot_blocks_ = slot;
            enter_track(measure_marker(start, slot), 0, bits, true);
            return;
        }
        if (cold_.folded >= k_cold_join_max_packages) cold_.pending = false;  // no lock, no bytes
    }
}

// The chain's marker that starts folded package `package`: taken at a candidate within k_marker_search of the period
// when one is near, else where the period puts it.
float Decoder::chain_marker(uint8_t package) const {
    const float period = cold_.period;
    float position = cold_.anchor + static_cast<float>(package) * period;
    for (uint8_t age = 0; age < candidates_.count(); ++age) {
        const float at = candidate_position(candidates_.newest(age));
        if (fabsf(at - position) <= k_marker_search * period) position = at;
    }
    return position;
}

// The slot-edge energy over the slot-centre energy of the chain's folded packages the history still holds, on the grid
// of `bits` bits per package; false when nothing can be judged (no package held, or centres not above the noise).
bool Decoder::fold_ratio(uint8_t bits, float& ratio) const {
    const uint16_t slots = static_cast<uint16_t>(bits + 1u);
    const float slot = cold_.period / static_cast<float>(slots);
    const float half = k_fold_half * slot;
    const float noise = noise_variance() * dsp::noise_samples(2.0f * half, block_samples_);
    float edge = 0.0f;
    float centre = 0.0f;
    uint8_t packages = 0;
    for (uint8_t k = cold_.folded; k > 0; --k) {
        const float start = chain_marker(static_cast<uint8_t>(k - 1u));
        Complex sum;
        if (!history_.window(origin_block_, start - half, start + half, sum)) break;
        const float step = (chain_marker(k) - start) / static_cast<float>(slots);
        for (uint16_t i = 0; i < slots; ++i) {
            edge += window_energy(start + (static_cast<float>(i) + k_slot_centre) * step, half) - noise;
            if (i > 0) centre += window_energy(start + static_cast<float>(i) * step, half) - noise;
        }
        ++packages;
    }
    if (packages == 0) return false;
    const float edges = static_cast<float>(packages) * static_cast<float>(slots);
    const float centres = static_cast<float>(packages) * static_cast<float>(slots - 1u);
    if (centre / centres < k_fold_min_centre * noise) return false;
    ratio = (edge / edges) / (centre / centres);
    return true;
}

// No other package length explains the chain's slot edges clearly better than `bits` (spec 1.1: the edges of the true
// grid are nulls): a chain of another N', even one no cold join may take (not a multiple of 8, V7), reads quiet edges
// on its own grid only. Grids whose edges are those of `bits` (N' + 1 dividing N + 1) tie with it.
bool Decoder::fold_unrivalled(uint8_t bits) const {
    float own = 0.0f;
    if (!fold_ratio(bits, own)) return false;
    for (uint8_t n = 1; n <= k_max_bits_per_package; ++n) {
        if (n == bits || (bits + 1u) % (n + 1u) == 0u) continue;
        const float slot = cold_.period / static_cast<float>(n + 1u);
        float rival = 0.0f;
        if (!in_range(slot) || !resolvable(slot) || !fold_ratio(n, rival)) continue;
        if (rival < k_fold_rival_ratio * own) return false;
    }
    return true;
}

// Adds one package's edge and centre energies (less the noise of their windows) on every hypothesis grid.
void Decoder::fold_package(float start, float stop) {
    for (uint8_t h = 0; h < k_cold_hypotheses; ++h) {
        const uint8_t bits = cold_bits(h);
        if (bits == 0) continue;
        for (uint8_t g = 0; g < k_fold_grids; ++g) {
            const uint16_t slots = static_cast<uint16_t>(k_fold_sub_grids[g] * (bits + 1u));
            const float slot = (stop - start) / static_cast<float>(slots);
            if (g > 0 && !resolvable(slot)) continue;
            const float half = k_fold_half * slot;
            const float noise = noise_variance() * dsp::noise_samples(2.0f * half, block_samples_);
            for (uint16_t i = 0; i < slots; ++i) {
                cold_.edge[h][g] += window_energy(start + (static_cast<float>(i) + k_slot_centre) * slot, half) - noise;
                if (i > 0) cold_.centre[h][g] += window_energy(start + static_cast<float>(i) * slot, half) - noise;
            }
        }
    }
}

// N of hypothesis h when its T lies in the window and its package is not too long, else 0.
uint8_t Decoder::cold_bits(uint8_t hypothesis) const {
    const uint16_t bits = static_cast<uint16_t>((hypothesis + 1u) * k_cold_join_step_bits);
    if (bits > k_max_bits_per_package) return 0;
    const float slot = cold_.period / static_cast<float>(bits + 1u);
    if (!in_range(slot)) return 0;
    const float package_ms = blocks_to_ms(cold_.period);
    if (package_ms > (1.0f + k_range_tolerance) * static_cast<float>(k_max_package_us / k_us_per_ms)) return 0;
    return static_cast<uint8_t>(bits);
}

// Quiet edges: the mean edge energy at most k_fold_edge_ratio of the mean centre energy, which must be well above the
// noise (a chain of zeros gives nothing to judge).
bool Decoder::fold_passes(uint8_t hypothesis, uint8_t grid) const {
    const uint8_t bits = cold_bits(hypothesis);
    const uint16_t slots = static_cast<uint16_t>(k_fold_sub_grids[grid] * (bits + 1u));
    const float slot = cold_.period / static_cast<float>(slots);
    if (grid > 0 && !resolvable(slot)) return false;
    const float folded = static_cast<float>(cold_.folded);
    const float edge = cold_.edge[hypothesis][grid] / (folded * static_cast<float>(slots));
    const float centre = cold_.centre[hypothesis][grid] / (folded * static_cast<float>(slots - 1u));
    const float noise = noise_variance() * dsp::noise_samples(2.0f * k_fold_half * slot, block_samples_);
    return centre >= k_fold_min_centre * noise && edge <= k_fold_edge_ratio * centre;
}

// ---------------------------------------------------------------------------
// PREAMBLE: the rest of the train, then the package length N (spec 3.8), on the history clock
// ---------------------------------------------------------------------------

void Decoder::run_preamble() {
    while (state_ == DecoderState::preamble && preamble_step()) {
    }
}

// One grid index g after the newest one evaluated, once the history holds its search window.
bool Decoder::preamble_step() {
    if (confirm_pending_) return confirmation_step();
    if (end_stop_ >= 0) return end_before_confirmation();
    const float slot = slot_blocks_;
    const int32_t g = grid_index_ + 1;
    const int32_t gap = g - grid_last_;
    const float position = grid_position(g);
    const float span = preamble_span(gap) * slot;
    const float half = k_marker_half * slot;
    if (position + span + half + 1.0f > end_position()) return false;
    grid_index_ = g;

    const FlipMeasure m = search_flip(position, span, half, k_kappa_track);
    // One slot after the newest marker a train marker keeps half the train's crest, or is a faded one that still flips
    // clearly where the train puts it; further out a STOP needs TRACK's 0.3 and balanced halves: searched over a wide
    // span among data slots, the onset of a data one (one-sided energy) must not pass for it.
    const bool faded = gap == 1 && m.valid && m.q >= k_faded_marker_q && m.kappa >= k_faded_marker_kappa &&
                       m.amplitude >= k_faded_marker_ratio * reference_average_ &&
                       fabsf(m.position - position) <= k_faded_marker_offset * slot;
    const float ratio = gap == 1 ? k_train_amplitude_ratio : k_flip_amplitude_ratio;
    // Where a held candidate puts its next STOP, the STOP is expected: measured like TRACK's, with halves that need
    // not be as balanced as those of a STOP found anywhere among data slots. (Not reading B's: that slot is reading
    // A's last data slot.)
    const uint8_t bits = learner_.bits();
    const bool expected_stop = bits > 0 && gap == bits + 1;
    const float q_balanced_min = expected_stop ? k_q_expected_stop : k_q_track;
    bool marker = is_flip(m, k_kappa_track) && (m.amplitude >= ratio * reference_average_ || faded) &&
                  (gap == 1 || m.q_balanced >= q_balanced_min);
    const int32_t expected = grid_last_ + static_cast<int32_t>(bits) + 1;
    if (marker && gap > 1 && bits > 0 && g < expected && m.q_balanced < k_lookahead_q) {
        // A flip among the candidate's data slots: noise when the candidate's own STOP, further on, is a far stronger
        // marker (its span must be in the history first). A real STOP there (the candidate spanned a missed one)
        // compares with it.
        const float ahead = grid_position(expected);
        const float ahead_span = preamble_span(expected - grid_last_) * slot;
        if (ahead + ahead_span + half + 1.0f > end_position()) {
            grid_index_ = g - 1;
            return false;
        }
        const FlipMeasure there = search_flip(ahead, ahead_span, half, k_kappa_track);
        if (is_flip(there, k_kappa_track) && there.amplitude >= k_flip_amplitude_ratio * reference_average_ &&
            there.q_balanced >= k_q_track && there.q_balanced > k_lookahead_ratio * m.q_balanced) {
            marker = false;
            data_flip_ = true;
        }
    }
    if (marker) {
        on_marker(g, m);
        if (state_ != DecoderState::preamble) return false;
    } else if (gap == 1 && previous_reading_ < k_readings) {
        // No marker right after a rejecting one: it was no short final package, the rejection stands.
        readings_[previous_reading_].valid = false;
        if (previous_reading_ + 1u < k_readings && previous_reading_ + 1u != candidate_reading_) {
            readings_[previous_reading_ + 1u].valid = false;
        }
        previous_reading_ = k_readings;
    }
    if (grid_index_ - grid_last_ > k_marker_gap_limit || grid_index_ > k_preamble_max_slots ||
        learner_.rejections() > k_max_rejections) {
        lose(LostReason::preamble_timeout);
        return false;
    }
    return true;
}

void Decoder::on_marker(int32_t g, const FlipMeasure& m) {
    const int32_t gap = g - grid_last_;
    const bool held = learner_.bits() > 0;
    Marker marker;
    marker.position = m.position;
    marker.amplitude = m.amplitude;
    marker.balanced = m.q_balanced;
    marker.detected = true;
    if (gap == 1) {
        if (!held) {
            // Past the train's end a marker one slot after another is noise in the data, not the train going on.
            if (train_closed()) {
                data_flip_ = true;
            } else {
                continue_train(g, m);
            }
            return;
        }
        // A marker right after a STOP may be the first END marker (step 7), or the train went on.
        end_stop_ = grid_last_;
        end_flip_ = m;
        return;
    }
    if (!held && min_start_ > grid_last_) {
        // The train reaches min_start_ (its length from the tune): markers up to it are train markers. One right after
        // a faded START is noise in package 0 (data slots never flip) or a longer train, which shows itself next.
        if (g <= min_start_) {
            continue_train(g, m);
            return;
        }
        if (g == min_start_ + 1) {
            data_flip_ = true;
            return;
        }
    }
    const bool hidden_marker = gap == 2 && !held && !train_closed() && bridges_trusted() &&
                               carrier_parity(bridge(grid_position_, m.position, slot_blocks_)) > 0;
    if (hidden_marker) {
        continue_train(g, m);  // a faded train marker between: its twist still reversed the carrier
        return;
    }
    // While the train has shown few gaps of one, equal longer gaps are a train read at T / k (spec 3.8 step 4); a
    // multiple beyond the window cannot be a train of this receiver: those gaps are packages, for the learner.
    if (learner_.train_ones() < k_min_train_ones) {
        const uint8_t multiple = sub_rate_train(gap);
        if (multiple > 1 && in_range(static_cast<float>(multiple) * slot_blocks_) && sync_in_phase(g, multiple)) {
            restart_grid(static_cast<float>(multiple) * slot_blocks_, m.position);
            return;
        }
    }
    const uint8_t candidate_before = candidate_reading_;
    const LearnStep step = learner_.push(g);
    if (learner_.first_marker() == g) first_after_ = m.position;
    switch (step) {
    case LearnStep::candidate: {
        // A rejected candidate is kept until this marker's END check: it may be the first of two packages whose
        // second, short one ends here.
        previous_reading_ = held ? candidate_before : k_readings;
        for (uint8_t r = 0; r < k_readings; ++r) {
            const bool kept = previous_reading_ < k_readings &&
                              (r == previous_reading_ || (previous_reading_ == 0 && r == 1));
            if (!kept) readings_[r].valid = false;
        }
        candidate_reading_ = previous_reading_ == 0 ? static_cast<uint8_t>(k_readings - 1u) : 0;
        Marker start = last_marker_;
        if (learner_.start_faded()) {
            // The train stopped short of its length: package 0's START faded where the length puts it.
            start = measure_marker(grid_position(learner_.candidate_start()), slot_blocks_);
            start.detected = false;
        } else if (start.amplitude <= 0.0f) {
            // The sync's newest marker, not measurable yet when PREAMBLE began: its crest starts the reference line.
            const Marker measured = measure_marker(start.position, slot_blocks_);
            start.amplitude = measured.amplitude;
            start.balanced = measured.balanced;
        }
        const bool first = learner_.first_marker() == g;
        measure_reading(readings_[candidate_reading_], start, marker, learner_.bits(), first);
        if (learner_.faded_bits() > 0) {
            // Reading B: the train's newest marker was followed by the first START, faded (spec 3.8, V2).
            Marker faded_start = measure_marker(train_last_.position + slot_blocks_, slot_blocks_);
            faded_start.detected = false;
            const FlipMeasure there = measure_flip(faded_start.position, k_marker_half * slot_blocks_);
            // A data one there is no faded marker.
            if (!(there.valid && there.q <= -k_q_track && there.kappa <= -k_kappa_anti)) {
                measure_reading(readings_[candidate_reading_ + 1u], faded_start, marker, learner_.faded_bits(), first);
            }
        }
        break;
    }
    case LearnStep::confirmed:
        if (previous_reading_ < k_readings) {
            for (uint8_t r = 0; r < k_readings; ++r) {
                if (r != candidate_reading_ && r != candidate_reading_ + 1u) readings_[r].valid = false;
            }
        }
        previous_reading_ = k_readings;
        confirm_pending_ = true;
        break;
    case LearnStep::rejected:
        for (uint8_t r = 0; r < k_readings; ++r) readings_[r].valid = false;
        previous_reading_ = k_readings;
        break;
    case LearnStep::unsupported:
        ban_alias(slot_blocks_);
        lose(LostReason::unsupported);
        return;
    case LearnStep::train:
        break;
    }
    grid_position_ = m.position;
    grid_last_ = g;
    last_marker_ = marker;
    data_flip_ = false;
}

// A train marker at g (one slot after the newest, or after faded ones): the train goes on, L = g.
void Decoder::continue_train(int32_t g, const FlipMeasure& m) {
    const int32_t gap = g - grid_last_;
    const float index = static_cast<float>(g);
    add_train_point(index, m);
    float position = m.position;
    float period = slot_blocks_ + k_train_nudge * ((m.position - grid_position_) / static_cast<float>(gap) - slot_blocks_);
    float fitted_slot = 0.0f;
    float fitted_position = 0.0f;
    if (train_fit(index, fitted_slot, fitted_position) &&
        fabsf(fitted_slot - slot_blocks_) <= k_refine_max_change * slot_blocks_) {
        period = fitted_slot;
        position = fitted_position;
    }
    slot_blocks_ = period;
    if (!in_range(slot_blocks_)) {
        ban_alias(slot_blocks_);
        lose(LostReason::alias);
        return;
    }
    grid_position_ = position;
    grid_last_ = g;
    update_reference(m.amplitude);
    learner_.extend_train(g, gap == 1);
    data_flip_ = false;
    if (train_markers_ < k_count_limit_u8) ++train_markers_;
    last_marker_.position = position;
    last_marker_.amplitude = m.amplitude;
    last_marker_.balanced = m.q_balanced;
    last_marker_.detected = true;
    train_last_ = last_marker_;
}

void Decoder::add_train_point(float index, const FlipMeasure& m) {
    const float weight = clamp(m.q, k_fit_min_weight, k_evidence_clip);
    fit_weight_ += weight;
    fit_x_ += weight * index;
    fit_y_ += weight * m.position;
    fit_xx_ += weight * index * index;
    fit_xy_ += weight * index * m.position;
}

// The train's markers at grid indices 0, -1, ... -7 before `anchor` (the sync's positions) start the fit. A marker
// found at the edge of the sync's +-0.1 T is searched again over +-0.25 T: the sync's T may be a few % off, which
// moves the oldest markers beyond +-0.1 T.
void Decoder::seed_train_fit(float anchor, float slot_blocks) {
    fit_weight_ = 0.0f;
    fit_x_ = 0.0f;
    fit_y_ = 0.0f;
    fit_xx_ = 0.0f;
    fit_xy_ = 0.0f;
    const float span = k_position_search * slot_blocks;
    const float half = k_marker_half * slot_blocks;
    const float edge = span - k_search_step * slot_blocks;
    for (uint8_t i = 0; i < k_sync_positions; ++i) {
        const float index = -static_cast<float>(i);
        const float position = anchor + index * slot_blocks;
        FlipMeasure m = search_flip(position, span, half, k_kappa_track);
        if (m.valid && fabsf(m.position - position) > edge) {
            m = search_flip(position, k_first_stop_search * slot_blocks, half, k_kappa_track);
        }
        if (m.valid && m.q >= k_sync_weak_q && m.kappa >= k_kappa_track) add_train_point(index, m);
    }
}

// The fitted T and the fitted position of grid index `index`; false below three markers' worth of weight.
bool Decoder::train_fit(float index, float& slot_blocks, float& position) const {
    if (fit_weight_ < k_fit_min_points * k_fit_min_weight) return false;
    const float denominator = fit_weight_ * fit_xx_ - fit_x_ * fit_x_;
    if (denominator <= 0.0f) return false;
    slot_blocks = (fit_weight_ * fit_xy_ - fit_x_ * fit_y_) / denominator;
    position = (fit_y_ - slot_blocks * fit_x_) / fit_weight_ + slot_blocks * index;
    return true;
}

// Returns k > 1 when markers came k slots apart at least k_sub_rate_gaps times and at least k_sub_rate_ratio times as
// often as one apart.
uint8_t Decoder::sub_rate_train(int32_t gap) {
    const uint8_t clipped = static_cast<uint8_t>(gap > k_count_limit_u8 ? k_count_limit_u8 : gap);
    if (clipped != train_gap_) {
        train_gap_ = clipped;
        train_gap_count_ = 0;
    }
    if (train_gap_count_ < k_count_limit_u8) ++train_gap_count_;
    const bool sub_rate =
        train_gap_count_ >= k_sub_rate_gaps && train_gap_count_ >= k_sub_rate_ratio * learner_.train_ones();
    return sub_rate ? train_gap_ : 1;
}

// A train of period k T read at T keeps one phase: the sync's hits (grid index -i for hit i) lie on the grid of the
// markers k apart, but for at most one (noise). A package chain after a train with faded markers does not.
bool Decoder::sync_in_phase(int32_t g, uint8_t multiple) const {
    uint8_t out_of_phase = 0;
    for (uint8_t i = 0; i < k_sync_positions; ++i) {
        if ((sync_hits_ & (1u << i)) != 0 && (g + i) % multiple != 0) ++out_of_phase;
    }
    return out_of_phase <= k_half_rate_odd_hits;
}

// A train read at T / k: T x k (in the window, else an alias), fitted again, the grid restarted at `anchor`.
void Decoder::restart_grid(float slot_blocks, float anchor) {
    if (!in_range(slot_blocks)) {
        ban_alias(slot_blocks_);
        lose(LostReason::alias);
        return;
    }
    float slot = slot_blocks;
    float centre = anchor;
    refine_sync(centre, slot);
    if (!in_range(slot)) slot = slot_blocks;
    slot_blocks_ = slot;
    grid_position_ = centre;
    seed_train_fit(centre, slot);
    grid_index_ = 0;
    grid_last_ = 0;
    bool silent = false;
    min_start_ = train_end_bound(centre, slot, reference_average_, silent);
    tuned_train_ = train_follows_tune(min_start_, silent, slot);
    learner_.reset(0, 0, min_start_);
    train_gap_ = 0;
    train_gap_count_ = 0;
    train_markers_ = 1;
    sync_hits_ = 1;
    for (uint8_t r = 0; r < k_readings; ++r) readings_[r].valid = false;
    previous_reading_ = k_readings;
    data_flip_ = false;
    last_marker_ = measure_marker(centre, slot);
    last_marker_.detected = true;
    train_last_ = last_marker_;
}

// A reading of the package from `start` to `stop`; `first`: package 0 (its STOP is the first marker after the train).
// The walk's noise markers since `start` lie among its data slots.
void Decoder::measure_reading(Reading& reading, const Marker& start, const Marker& stop, uint8_t bits, bool first) {
    decide_package(start, stop, bits, reading.package, reading.stats, reading.level_pct, reading.threshold_pct,
                   reading.audit);
    if (start.detected) {
        const float slot = (stop.position - start.position) / static_cast<float>(bits + 1u);
        add_marker_edges(start.position, slot, start.amplitude, reading.stats);
    }
    reading.start = start;
    reading.stop = stop;
    reading.reference = reference_average_;
    reading.first = first;
    reading.flipped = data_flip_;
    reading.valid = true;
}

// Step 7 of spec 3.8: two markers right after the newest one (end_stop_) and silence after them end a transmission of
// one or two packages before N was confirmed. The reading whose bit count is a whole number of bytes is taken.
bool Decoder::end_before_confirmation() {
    const float slot = slot_blocks_;
    const float stop = last_marker_.position;
    if (stop + (static_cast<float>(k_end_clean_slot) + k_slot_centre) * slot + 1.0f > end_position()) return false;
    const int32_t end_stop = end_stop_;
    end_stop_ = -1;
    const bool end = end_evidence(stop + slot) + end_evidence(stop + 2.0f * slot) >= k_end_evidence && end_clean(stop);
    // The held packages are the whole transmission only when the first of them is package 0 of a train the tune
    // placed (after missed STOPs they may be its last packages) and no data slot of theirs flipped (data slots never
    // do: the walk took a marker among them for noise).
    const uint8_t first_slot = previous_reading_ < k_readings ? previous_reading_ : candidate_reading_;
    const bool whole = tuned_train_ && readings_[first_slot].first && !readings_[first_slot].flipped &&
                       (previous_reading_ >= k_readings || !readings_[candidate_reading_].flipped);
    if (end && whole) {
        // The candidate's readings A and B, or the rejected candidate's with the short package ending here.
        const Reading* second = previous_reading_ < k_readings ? &readings_[candidate_reading_] : 0;
        const Reading* options[2] = {&readings_[first_slot], 0};
        if (first_slot == 0 && &readings_[1] != second) options[1] = &readings_[1];  // reading B
        const Reading* choice = 0;
        uint8_t choices = 0;
        for (uint8_t i = 0; i < 2; ++i) {
            if (options[i] == 0 || !options[i]->valid || !whole_bytes(*options[i], second)) continue;
            choice = options[i];
            ++choices;
        }
        if (choices == 1) {
            if (readings_clean(*choice, second, stop)) {
                bits_per_package_ = choice->package.count;
                Package first = choice->package;
                first.index = 0;
                emit_reading(*choice, 0);
                Package last = first;
                if (second != 0) {
                    last = second->package;
                    last.index = 1;
                    emit_reading(*second, 1);
                }
                confirmed_ = true;
                emit_locked(0);
                assemble(first);
                if (second != 0) assemble(last);
                last_detected_ = last_marker_;
                last_detected_package_ = last.index + 1u;
            }
            finish(second != 0 ? 1u : 0u);
            return false;
        }
    }
    // Not an END. A rejection at the newest marker stands (it was no short final package).
    if (previous_reading_ < k_readings) {
        for (uint8_t r = 0; r < k_readings; ++r) {
            if (r != candidate_reading_) readings_[r].valid = false;
        }
        previous_reading_ = k_readings;
    }
    // A lone marker past the train's end is noise in the data: the candidate stays and the walk goes on. Otherwise
    // the candidate came from faded train markers and the train goes on (step 5).
    if (train_closed()) {
        data_flip_ = true;
        grid_index_ = end_stop + 1;
        return true;
    }
    for (uint8_t r = 0; r < k_readings; ++r) readings_[r].valid = false;
    continue_train(end_stop + 1, end_flip_);
    grid_index_ = end_stop + 1;
    return state_ == DecoderState::preamble;
}

// A transmission carries whole bytes; a short final package is shorter than the one before it.
bool Decoder::whole_bytes(const Reading& first, const Reading* second) const {
    const uint16_t bits = static_cast<uint16_t>(first.package.count + (second != 0 ? second->package.count : 0u));
    if (bits % k_bits_per_byte != 0) return false;
    return second == 0 || second->package.count < first.package.count;
}

// The guard's clean-lock tests on the readings of a short transmission (spec 3.8 step 7, 3.11).
bool Decoder::readings_clean(const Reading& first, const Reading* second, float end_stop) {
    const Reading* readings[2] = {&first, second};
    float edge_excess = 0.0f;
    float edge_noise = 0.0f;
    uint16_t edges = 0;
    uint16_t zeros = 0;
    uint16_t loud_zeros = 0;
    uint16_t ones = 0;
    float slot_edge_energy = 0.0f;
    float one_energy = 0.0f;
    uint16_t slot_edges = 0;
    for (uint8_t i = 0; i < 2; ++i) {
        const Reading* reading = readings[i];
        if (reading == 0) continue;
        edge_excess += reading->stats.edge_excess;
        edge_noise += reading->stats.edge_noise;
        edges = static_cast<uint16_t>(edges + reading->stats.edges);
        zeros = static_cast<uint16_t>(zeros + reading->stats.zeros);
        loud_zeros = static_cast<uint16_t>(loud_zeros + reading->stats.loud_zeros);
        ones = static_cast<uint16_t>(ones + reading->stats.ones);
        slot_edge_energy += reading->stats.edge_energy;
        one_energy += reading->stats.one_energy;
        slot_edges = static_cast<uint16_t>(slot_edges + reading->package.count + 1u);
        if (reading->stats.strongest >= k_q_track) return false;
        for (uint8_t b = 0; b < reading->package.count; ++b) {
            if (static_cast<float>(reading->threshold_pct[b]) >= k_percent) return false;
        }
    }
    PackageStats end_edges;
    memset(&end_edges, 0, sizeof(end_edges));
    for (uint8_t j = 1; j <= k_end_markers; ++j) {
        add_marker_edges(end_stop + static_cast<float>(j) * slot_blocks_, slot_blocks_, reference_average_, end_edges);
    }
    edge_excess += end_edges.edge_excess;
    edge_noise += end_edges.edge_noise;
    edges = static_cast<uint16_t>(edges + end_edges.edges);
    if (edges > 0) {
        const float count = static_cast<float>(edges);
        const float allowance = k_marker_edge_sigmas * (edge_noise / count) / sqrtf(count);
        if (edge_excess / count > k_marker_edge_max + allowance) return false;
    }
    return ones > 0 && 2u * loud_zeros <= zeros && decodable() &&
           beep_shaped(slot_edge_energy, slot_edges, one_energy, ones);
}

void Decoder::emit_reading(const Reading& reading, uint32_t index) {
    Package package = reading.package;
    package.index = index;
    emit_package(package, reading.start, reading.stop, reading.reference, reading.level_pct, reading.threshold_pct);
}

// N confirmed at the newest marker (step 6): once the history holds its END positions, the END rule; otherwise TRACK
// with the two decided packages held by the guard.
bool Decoder::confirmation_step() {
    const uint8_t bits = learner_.bits();
    const float slot = slot_blocks_;
    const Marker stop = last_marker_;
    const float needed = bits == 1 ? static_cast<float>(k_end_single_last_slot) + k_marker_half
                                   : static_cast<float>(k_end_clean_slot) + k_slot_centre;
    if (stop.position + (needed + k_track_search) * slot + 1.0f > end_position()) return false;
    confirm_pending_ = false;
    const uint8_t first_slot = static_cast<uint8_t>(candidate_reading_ + (learner_.faded_start() ? 1u : 0u));
    if (first_slot >= k_readings || !readings_[first_slot].valid) {
        lose(LostReason::preamble_timeout);
        return false;
    }
    Reading& first = readings_[first_slot];
    const int32_t span = static_cast<int32_t>(bits) + 1;
    const float track_slot = (stop.position - first.start.position) / static_cast<float>(2 * span);
    if (track_slot <= 0.0f) {
        lose(LostReason::preamble_timeout);
        return false;
    }
    // A train with no tune before it may be a run of data beeps of a transmission whose lock was lost (fading and
    // filter edges twist them): package 0 is unknown, so no package index is either. Refused; a cold join takes a
    // transmission of whole-byte packages further on (spec 3.12, V7).
    if (!tuned_train_) {
        lose(LostReason::preamble_timeout);
        return false;
    }
    // The walk counts slots with the train's T, whose error over a long first gap can reach a slot. The two confirmed
    // packages measure T: they recount the slots from L to the confirmed START (not near a whole number: refused).
    const float counted = (first.start.position - train_last_.position) / track_slot;
    const int32_t slots = round_to_int(counted);
    if (fabsf(counted - static_cast<float>(slots)) > k_slot_count_tolerance) {
        lose(LostReason::preamble_timeout);
        return false;
    }
    const int32_t shift = slots - (learner_.candidate_start() - learner_.train_index());
    learner_.shift_candidate(shift);
    grid_last_ += shift;
    // The confirmed START lies a whole number of packages after package 0's START, which lies as many slots after L as
    // train markers faded at the train's end (at most k_faded_train_bits): a larger remainder means L, the train's
    // newest marker, was a noise flip in the data, and every package index would be off.
    const int32_t remainder = (learner_.candidate_start() - learner_.start_base()) % span;
    if (!learner_.start_exact() || remainder > k_faded_train_bits || !start_certain(first, bits)) {
        lose(LostReason::preamble_timeout);  // package 0 not placed without doubt: a shift would ruin every byte
        return false;
    }
    const uint32_t index = static_cast<uint32_t>((learner_.candidate_start() - learner_.first_start()) / span);

    // Two packages that are 2 m packages of N' = (N + 1) / m - 1 whose other STOPs the preamble missed: after a train
    // that followed its tune (its length places package 0), TRACK goes on with N' from the confirming STOP (the
    // packages read so far are lost); otherwise the lock is refused.
    const uint8_t sub = sub_chain(first.start.position, first.stop.position, stop.position, bits);
    const bool placed = min_start_ != dsp::k_no_start && learner_.train_index() <= min_start_;
    if (sub > 1 && !placed) {
        lose(LostReason::preamble_timeout);
        return false;
    }
    if (sub > 1) {
        const uint8_t sub_bits = static_cast<uint8_t>(span / sub - 1);
        const int32_t sub_span = static_cast<int32_t>(sub_bits) + 1;
        const int32_t candidate = learner_.candidate_start();
        const int32_t sub_first = candidate - sub_span * ((candidate - learner_.start_base()) / sub_span);
        const uint32_t sub_index = static_cast<uint32_t>((grid_last_ - sub_first) / sub_span);
        enter_track(stop, sub_index, sub_bits, false);
        const float sub_slot = track_slot;
        if (in_range(sub_slot)) slot_blocks_ = sub_slot;
        for (uint8_t r = 0; r < k_readings; ++r) readings_[r].valid = false;
        return false;
    }

    Package second;
    PackageStats stats;
    uint8_t level_pct[k_max_bits_per_package];
    uint8_t threshold_pct[k_max_bits_per_package];
    int8_t audit[dsp::AuditRing::k_max_positions];
    decide_package(first.stop, stop, bits, second, stats, level_pct, threshold_pct, audit);
    const float reference = reference_average_;
    if (sub_package_zeros(first.package, second, bits)) {
        lose(LostReason::preamble_timeout);
        return false;
    }

    enter_track(stop, index + 2u, bits, false);
    if (in_range(track_slot)) slot_blocks_ = track_slot;
    guard_started_ = first.start.detected;
    guard_detected_ = first.start.detected ? 1 : 0;
    if (index > 0 && index <= k_recovered_packages) recover_packages(first, index, bits);
    first.package.index = index;
    second.index = index + 1u;
    emit_package(first.package, first.start, first.stop, first.reference, first.level_pct, first.threshold_pct);
    emit_package(second, first.stop, stop, reference, level_pct, threshold_pct);
    push_audit(first.audit, first.package.count);
    push_audit(audit, bits);
    guard_package(first.package, first.stats, first.stop);
    guard_package(second, stats, stop);
    for (uint8_t r = 0; r < k_readings; ++r) readings_[r].valid = false;

    if (end_of_transmission(stop.position)) {
        if (!guard_refused() && guard_clean() && end_clean(stop.position)) confirm();
        finish(index + 1u);
        return false;
    }
    check_guard();
    return false;
}

// Package 0 right after the train may be the train itself with its last N markers faded (its data slots then read
// 0): every package index would be one off. The train's length rules that out; otherwise, for N = 1 always (V4) and
// for N <= 3 when package 0 reads all zeros, the carrier across package 0 must agree with the reading: reversed only
// by a faded START. N faded markers or none keep it alike for even N: refused.
bool Decoder::start_certain(const Reading& first, uint8_t bits) const {
    if (min_start_ != dsp::k_no_start && learner_.train_index() <= min_start_) return true;
    if (bits > k_faded_train_bits) return true;
    const int32_t train = learner_.train_index();
    if (bits == 1) {
        if (!bridges_trusted()) return false;
        // The markers hidden between L and the first marker after it: the START's offset from L (faded train
        // markers or a faded START) and the STOPs missed before g1. Their parity must be the carrier's.
        const int32_t start = learner_.first_start();
        const int32_t g1 = learner_.first_marker();
        const int32_t missed = (g1 - start) / 2 - 1;
        const int32_t hidden = (start - train) + (missed > 0 ? missed : 0);
        const int8_t parity = carrier_parity(bridge(train_last_.position, first_after_, slot_blocks_));
        return parity == (hidden % 2 != 0 ? 1 : -1);
    }
    const int32_t offset = learner_.candidate_start() - train;
    if (offset > 1) return true;
    bool zeros = true;
    for (uint8_t b = 0; b < bits && zeros; ++b) zeros = !package_bit(first.package.bits, b);
    if (!zeros) return true;
    if (bits % 2 == 0 || !bridges_trusted()) return false;
    const int8_t parity = carrier_parity(bridge(train_last_.position, first.stop.position, slot_blocks_));
    return parity == (offset == 1 ? 1 : -1);
}

// Packages 0 .. count - 1 before the confirmed one (their STOPs the preamble missed; spec 3.8 step 6 loses them):
// decided on the slot grid from the train's newest marker to the confirmed START (the slots between them recounted
// by confirmation_step), measured where they must be like TRACK's flywheel, and held with the guard's packages, ahead
// of them (the guard judges the confirmed ones). Read on noise, they are erasures.
void Decoder::recover_packages(const Reading& first, uint32_t count, uint8_t bits) {
    const int32_t train = learner_.train_index();
    const int32_t package_zero = learner_.first_start();
    const float slot = (first.start.position - train_last_.position) /
                       static_cast<float>(learner_.candidate_start() - train);
    const float span = static_cast<float>(bits + 1u) * slot;
    Marker start = measure_marker(train_last_.position + static_cast<float>(package_zero - train) * slot, slot);
    start.detected = package_zero == train;
    Package package;
    PackageStats stats;
    uint8_t level_pct[k_max_bits_per_package];
    uint8_t threshold_pct[k_max_bits_per_package];
    int8_t audit[dsp::AuditRing::k_max_positions];
    for (uint32_t k = 0; k < count; ++k) {
        Marker stop = k + 1 == count ? first.start : measure_marker(start.position + span, slot);
        decide_package(start, stop, bits, package, stats, level_pct, threshold_pct, audit);
        package.index = k;
        package.faded = package.faded || (!stop.detected && faded_package(package, start, stop));
        emit_package(package, start, stop, reference_average_, level_pct, threshold_pct);
        hold(package);
        start = stop;
    }
}

// Markers at START + j (STOP - START) / m in both confirmed packages (m >= 2 dividing N + 1): the packages are
// m shorter ones whose STOPs the preamble missed in turn. Returns m, or 0.
uint8_t Decoder::sub_chain(float first, float middle, float last, uint8_t bits) const {
    const uint16_t slots = static_cast<uint16_t>(bits + 1u);
    const float bounds[3] = {first, middle, last};
    for (uint16_t m = 2; 2u * m <= slots; ++m) {
        if (slots % m != 0) continue;
        float evidence = 0.0f;
        for (uint8_t p = 0; p < 2; ++p) {
            const float step = (bounds[p + 1] - bounds[p]) / static_cast<float>(m);
            for (uint16_t j = 1; j < m; ++j) evidence += end_evidence(bounds[p] + static_cast<float>(j) * step);
        }
        if (evidence >= k_sub_chain_evidence * 2.0f * static_cast<float>(m - 1u)) return static_cast<uint8_t>(m);
    }
    return 0;
}

// N + 1 = m (N' + 1) with N' a whole number of bytes (8, 16, 24, 32): if every slot where a STOP of N' would sit decided 0
// in both confirmed packages, they may be 2 m packages of N' whose every other STOP faded below the noise (a marker's
// halves cancel in the slot window, so a faded STOP always decides 0). The lock is refused: the cold join takes the
// transmission further on. A sender of N = 17 or 26 loses its preamble with probability 1/4 or 1/16.
bool Decoder::sub_package_zeros(const Package& first, const Package& second, uint8_t bits) const {
    const uint16_t slots = static_cast<uint16_t>(bits + 1u);
    for (uint16_t m = 2; 2u * m <= slots; ++m) {
        if (slots % m != 0) continue;
        const uint16_t sub_slots = static_cast<uint16_t>(slots / m);
        if ((sub_slots - 1u) % k_bits_per_byte != 0) continue;
        bool zeros = true;
        for (uint16_t j = 1; j < m && zeros; ++j) {
            const uint8_t bit = static_cast<uint8_t>(j * sub_slots - 1u);
            zeros = !package_bit(first.bits, bit) && !package_bit(second.bits, bit);
        }
        if (zeros) return true;
    }
    return false;
}

// A transmission starts with its tune: the tune placed the train in the history, or the history does not reach back
// to it and the tone was locked on a tune (steady, or turning into this train) at most the longest train of the
// sync's T before it. A train after silence, or with no tune before it, may be data beeps of a transmission whose lock
// was lost.
bool Decoder::train_follows_tune(int32_t min_start, bool silent, float slot_blocks) const {
    if (min_start != dsp::k_no_start) return true;
    const float train_blocks = static_cast<float>(k_max_sync_markers + k_tune_bound_slots) * slot_blocks;
    return !silent && tone_steady_ && static_cast<float>(tone_blocks_) <= train_blocks;
}

// At most k_faded_train_bits train markers fade at the train's end (spec 3.8 step 5): once the walk has a marker
// further than that past the train's newest marker L, the train is over, and later markers are packages or noise.
bool Decoder::train_closed() const {
    return grid_last_ - learner_.train_index() > static_cast<int32_t>(k_faded_train_bits) + 1;
}

float Decoder::grid_position(int32_t g) const {
    return grid_position_ + static_cast<float>(g - grid_last_) * slot_blocks_;
}

// Search span around the prediction, in slots: the train's T is known to about 0.5 %, so the prediction error grows
// with the distance from the newest marker.
float Decoder::preamble_span(int32_t gap) const {
    if (gap <= 1) return k_position_search;
    const int32_t beyond = gap > k_stop_search_after ? gap - k_stop_search_after : 0;
    return min_of(k_first_stop_search + k_stop_search_growth * static_cast<float>(beyond), k_stop_search_max);
}

// ---------------------------------------------------------------------------
// TRACK: one package at a time (spec 3.9), on the history clock
// ---------------------------------------------------------------------------

void Decoder::run_track() {
    while (state_ == DecoderState::track && track_step()) {
    }
}

bool Decoder::track_step() {
    const float slot = slot_blocks_;
    const uint8_t bits = bits_per_package_;
    const float slots = static_cast<float>(bits + 1u);
    const float half = k_marker_half * slot;
    const float predicted = start_.position + slots * slot;
    const bool missed = (detected_bits_ & 1u) == 0;
    const float span = (missed ? k_track_search_miss : k_track_search) * slot;
    const float kappa_min = missed ? k_kappa_after_miss : k_kappa_track;
    // 5: a short final package, its STOP and the two END markers at three slot centres in a row, found as soon as
    // they are in the history (not when the full package would be).
    if (short_found_ == 0) short_found_ = scan_short_end();
    if (short_found_ != 0) {
        const float short_stop = start_.position + static_cast<float>(short_found_) * slot;
        const float clean_reach = confirmed_ ? 0.0f : static_cast<float>(k_end_clean_slot) + k_slot_centre;
        if (short_stop + (clean_reach + k_track_search) * slot + half + 1.0f > end_position()) return false;
        end_short_package(short_found_);
        return false;
    }
    // The END positions; + 1 slot while the lock is unconfirmed (its clean END looks at +3 T), + 4 for N = 1 (+6 T).
    float reach = k_end_reach + (confirmed_ ? 0.0f : 1.0f);
    if (bits == 1) reach += static_cast<float>(k_end_single_last_slot - k_end_markers);
    if (predicted + reach * slot + span + 1.0f > end_position()) return false;

    // 1-2: the STOP, found where predicted, and measured there whether or not it flips ("measure, don't detect").
    FlipMeasure m = search_flip(predicted, span, half, kappa_min);
    // After missed STOPs the flywheel's grid drifts by its T error: the reach grows by k_stop_reach_per_miss with each
    // miss, never beyond k_stop_reach_miss_max (a noise flip half a slot away still cannot pass).
    uint8_t misses = 0;
    while (misses < k_stop_reach_misses && ((detected_bits_ >> misses) & 1u) == 0) ++misses;
    const float base_reach = max_of(k_stop_reach_min, k_stop_reach_per_slot * min_of(slots, k_stop_reach_slots));
    const float grown = min_of(base_reach + k_stop_reach_per_miss * static_cast<float>(misses), k_stop_reach_miss_max);
    const float stop_reach = max_of(base_reach, grown) * slot;
    const bool detected = is_flip(m, kappa_min) && m.amplitude >= k_flip_amplitude_ratio * reference_average_ &&
                          fabsf(m.position - predicted) <= stop_reach;
    if (!detected) m = measure_flip(predicted, half);
    Marker stop;
    stop.position = detected ? m.position : predicted;
    stop.amplitude = m.amplitude;
    stop.balanced = m.q_balanced;
    stop.detected = detected;
    const bool present = m.valid && m.q >= k_q_present;
    presence_bits_ = (presence_bits_ << 1) | (present ? 1u : 0u);
    detected_bits_ = (detected_bits_ << 1) | (detected ? 1u : 0u);
    // A STOP can fade into noise but never turns into a steady carrier: that is two data ones meeting where the STOP
    // should be, and the grid is off. (A STOP turned by a frequency step reads steady over the wide window too: its
    // own halves say so.)
    const float anti_half = k_anti_half * slot;
    const FlipMeasure across = detected ? m : measure_flip(predicted, anti_half);
    const bool anti = !detected && across.valid && across.q <= -k_q_track && across.kappa <= -k_kappa_anti &&
                      !rotated_marker(m) &&
                      !history_.any_blanked(origin_block_, predicted - anti_half, predicted + anti_half);
    anti_bits_ = static_cast<uint8_t>((anti_bits_ << 1) | (anti ? 1u : 0u));

    Package package;
    PackageStats stats;
    uint8_t level_pct[k_max_bits_per_package];
    uint8_t threshold_pct[k_max_bits_per_package];
    int8_t audit[dsp::AuditRing::k_max_positions];

    // 3-4: timing and AFC.
    if (detected) {
        // A short package measures T over few slots: its gain shrinks with them, so one STOP read off by noise does
        // not pull the grid of the packages after it.
        const float gain = k_timing_gain * min_of(slots / k_stop_reach_slots, 1.0f);
        slot_blocks_ += gain * ((stop.position - start_.position) / slots - slot_blocks_);
        update_reference(stop.amplitude);
        const float package_ms = blocks_to_ms(stop.position - start_.position);
        if (m.q >= k_afc_min_q && start_.detected && package_ms <= static_cast<float>(k_max_package_us / k_us_per_ms)) {
            track_afc(start_, stop);
        }
        last_detected_ = stop;
        last_detected_package_ = package_index_ + 1u;
    }
    rotation_afc(m, detected);
    if (!in_range(slot_blocks_)) {  // a sender outside the window: never release its bytes
        ban_alias(slot_blocks_);
        lose(LostReason::alias);
        return false;
    }

    // 6: the bits, against the reference line from START to STOP.
    decide_package(start_, stop, bits, package, stats, level_pct, threshold_pct, audit);
    package.index = package_index_;
    // 8: the next transmission's tune: slot edges on a steady carrier (a real slot edge is nearly silent).
    uint8_t truncate = bits;
    const uint8_t tune = tune_edges(start_, stop, bits, truncate);
    if (!detected && truncate < package.count) package.count = truncate;
    package.faded = package.faded || (!detected && faded_package(package, start_, stop));
    package.stop_gone = marker_gone(stop);
    // Two STOPs gone in a row: the fade began inside the package before (held, its STOP missed).
    if (package.stop_gone && held_count_ > 0 && held_[held_count_ - 1u].stop_gone) held_[held_count_ - 1u].faded = true;
    emit_package(package, start_, stop, reference_average_, level_pct, threshold_pct);
    // A package framed by a STOP turned by a frequency step (before the rotation AFC pulls the NCO back) turns inside
    // too: its audit positions say nothing about the grid.
    const bool rotated = !detected && rotated_marker(m);
    const bool turned = rotated || start_rotated_;
    start_rotated_ = rotated;
    if (turned) {
        int8_t neutral[dsp::AuditRing::k_max_positions];
        memset(neutral, 0, sizeof(neutral));
        push_audit(neutral, bits);
    } else {
        push_audit(audit, bits);
    }
    for (uint8_t g = 0; g < stats.gaps; ++g) track_noise_.push(stats.gap_noise / static_cast<float>(stats.gaps));

    // 7: alias checks.
    const bool strong = !turned && stats.strongest >= k_audit_high;
    strong_bits_ = static_cast<uint8_t>(((strong_bits_ << 1) | (strong ? 1u : 0u)) & k_strong_window_mask);
    if (audit_.max_evidence() >= k_audit_threshold || count_bits(strong_bits_) >= k_strong_packages) {
        ban_alias(slot_blocks_);
        lose(LostReason::alias);
        return false;
    }
    // A wrong grid phase (T right) or a T / 2 reading: no ban, a relock finds the true grid.
    if (count_bits(anti_bits_) >= (confirmed_ ? k_anti_packages : 1u)) {
        lose(LostReason::alias);
        return false;
    }
    if (tune >= k_tune_edges && !detected) {
        lose(LostReason::signal_gone);
        return false;
    }

    // 9: release, or the guard.
    if (confirmed_) {
        release(package, detected);
    } else {
        guard_package(package, stats, stop);
        check_guard();
        if (state_ != DecoderState::track) return false;
    }

    // 10: END after the STOP.
    if (end_of_transmission(stop.position)) {
        if (!confirmed_ && !guard_refused() && guard_clean() && end_clean(stop.position)) confirm();
        finish(package_index_);
        return false;
    }

    // 11: LOST when 3/4 of the presence window's STOPs are missing.
    const uint32_t window_mask = presence_window_ >= 32u ? k_count_limit : ((1u << presence_window_) - 1u);
    const uint8_t absent = static_cast<uint8_t>(presence_window_ - count_bits(presence_bits_ & window_mask));
    const uint8_t limit = static_cast<uint8_t>((k_loss_numerator * presence_window_ + k_loss_denominator - 1u) /
                                               k_loss_denominator);
    if (absent >= limit) {
        lose(LostReason::signal_gone);
        return false;
    }
    start_ = stop;
    ++package_index_;
    short_cursor_ = k_short_eot_first;
    short_run_ = 0;
    return true;
}

// The short-END triple (spec 3.9 step 5) over the slot centres of the current package that the history holds now:
// the index of the short package's STOP (a centre j with 2 <= j <= N), or 0. The cursor keeps the scan incremental.
uint8_t Decoder::scan_short_end() {
    const uint8_t n = bits_per_package_;
    if (divides_bytes(n)) return 0;
    const float slot = slot_blocks_;
    const float half = k_marker_half * slot;
    while (short_cursor_ <= n + k_end_markers) {
        const float centre = start_.position + static_cast<float>(short_cursor_) * slot;
        if (centre + half + 1.0f > end_position()) return 0;
        const FlipMeasure m = measure_flip(centre, half);
        const bool marker_like = m.valid && m.kappa >= k_kappa_track &&
                                 m.amplitude >= k_flip_amplitude_ratio * reference_average_;
        const float evidence = marker_like ? clamp(m.q, -k_evidence_clip, k_evidence_clip)
                                           : clamp(min_of(m.q, 0.0f), -k_evidence_clip, 0.0f);
        short_evidence_[0] = short_evidence_[1];
        short_evidence_[1] = short_evidence_[2];
        short_evidence_[2] = evidence;
        if (short_run_ < k_short_eot_flips) ++short_run_;
        ++short_cursor_;
        if (short_run_ < k_short_eot_flips) continue;
        const float sum = short_evidence_[0] + short_evidence_[1] + short_evidence_[2];
        if (short_evidence_[0] >= k_short_eot_min && short_evidence_[1] >= k_short_eot_min &&
            short_evidence_[2] >= k_short_eot_min && sum >= k_short_eot_evidence) {
            return static_cast<uint8_t>(short_cursor_ - k_short_eot_flips);
        }
    }
    return 0;
}

// The package ends short at centre `stop_slot` (d = stop_slot - 1 bits), then END.
void Decoder::end_short_package(uint8_t stop_slot) {
    const float slot = slot_blocks_;
    const float half = k_marker_half * slot;
    const uint8_t bits = static_cast<uint8_t>(stop_slot - 1u);
    const float centre = start_.position + static_cast<float>(stop_slot) * slot;
    const FlipMeasure m = search_flip(centre, k_track_search * slot, half, k_kappa_track);
    Marker end_stop = measure_marker(is_flip(m, k_kappa_track) ? m.position : centre, slot);
    end_stop.detected = true;
    Package package;
    PackageStats stats;
    uint8_t level_pct[k_max_bits_per_package];
    uint8_t threshold_pct[k_max_bits_per_package];
    int8_t audit[dsp::AuditRing::k_max_positions];
    decide_package(start_, end_stop, bits, package, stats, level_pct, threshold_pct, audit);
    package.index = package_index_;
    emit_package(package, start_, end_stop, reference_average_, level_pct, threshold_pct);
    if (confirmed_) {
        release_held();
        assemble(package);
    } else {
        guard_package(package, stats, end_stop);
        if (!guard_refused() && guard_clean() && end_clean(end_stop.position)) confirm();
    }
    last_detected_ = end_stop;
    last_detected_package_ = package_index_ + 1u;
    finish(package_index_);
}

// Carrier phase advance from START to STOP, comparing the same half of both markers: the receiver filter shifts both
// alike, so the estimate is unbiased. Unambiguous for |offset| < 1 / (2 (N + 1) T), which the fine AFC ensures.
void Decoder::track_afc(const Marker& start, const Marker& stop) {
    const float half = k_marker_half * slot_blocks_;
    Complex start_before;
    Complex start_after;
    Complex stop_before;
    Complex stop_after;
    if (!history_.window(origin_block_, start.position - half, start.position, start_before) ||
        !history_.window(origin_block_, start.position, start.position + half, start_after) ||
        !history_.window(origin_block_, stop.position - half, stop.position, stop_before) ||
        !history_.window(origin_block_, stop.position, stop.position + half, stop_after)) {
        return;
    }
    // The carrier sign flips once per marker, so each half of the STOP is minus the matching START half.
    const float re = -(stop_before.re * start_before.re + stop_before.im * start_before.im) -
                     (stop_after.re * start_after.re + stop_after.im * start_after.im);
    const float im = -(stop_before.im * start_before.re - stop_before.re * start_before.im) -
                     (stop_after.im * start_after.re - stop_after.re * start_after.im);
    if (re == 0.0f && im == 0.0f) return;
    const float span_s = blocks_to_ms(stop.position - start.position) / k_ms_per_s;
    if (span_s <= 0.0f) return;
    const float offset = atan2f(im, re) / (k_two_pi * span_s);
    const float limit = k_afc_clamp * k_ms_per_s / blocks_to_ms(slot_blocks_);
    nco_.adjust_frequency(clamp(k_afc_gain * offset, -limit, limit));
}

// A missed STOP whose halves are balanced, strong and turned by a steady rotation: a frequency step. Two in a row that
// agree move the NCO (T >= 32 ms only, v0.2 H2).
// A missed STOP whose balanced halves hold a marker's level but turned by a frequency error (spec 3.9 step 4); a turn
// near pi is a steady carrier (no reversal at all), not a turned one.
bool Decoder::rotated_marker(const FlipMeasure& m) const {
    const float level = sqrtf(m.amplitude * m.amplitude + m.steady * m.steady);
    const bool balanced = m.valid && m.q - m.q_balanced <= k_rotation_balance * m.energy;
    const float turn = fabsf(m.phase_step);
    return balanced && m.energy >= k_rotation_energy && level >= k_train_amplitude_ratio * reference_average_ &&
           level <= k_rotation_max_level * reference_average_ && turn >= k_rotation_min_rad &&
           turn <= k_pi - k_rotation_min_rad;
}

void Decoder::rotation_afc(const FlipMeasure& m, bool detected) {
    const float slot_ms = blocks_to_ms(slot_blocks_);
    const bool rotated = !detected && slot_ms >= k_rotation_min_slot_ms && rotated_marker(m);
    if (!rotated) {
        rotation_hz_ = 0.0f;
        return;
    }
    const float offset = m.phase_step * k_ms_per_s / (k_two_pi * k_rotation_span * slot_ms);
    const float larger = max_of(fabsf(offset), fabsf(rotation_hz_));
    if (offset * rotation_hz_ > 0.0f && fabsf(offset - rotation_hz_) <= k_rotation_agreement * larger) {
        nco_.adjust_frequency(k_rotation_gain * k_slot_centre * (offset + rotation_hz_));
        rotation_hz_ = 0.0f;
        return;
    }
    rotation_hz_ = offset;
}

// Spec 3.9 step 5: flips at slot centres j, j + 1, j + 2 (2 <= j <= N) are a STOP and the two END markers: the
// package holds j - 1 bits. Data slots never flip, so nothing else gives three in a row.
// Flip evidence at an END position: a marker-like flip counts its q, clipped; anything else 0.
float Decoder::end_evidence(float position) const {
    const FlipMeasure m = measure_flip(position, k_marker_half * slot_blocks_);
    if (!m.valid || m.kappa < k_kappa_track || m.amplitude < k_flip_amplitude_ratio * reference_average_) return 0.0f;
    return clamp(m.q, -k_evidence_clip, k_evidence_clip);
}

// Flips at +T and +2T after the STOP. With N = 1 the chain's next STOP sits where the second END marker is: the first
// END marker must then flip on its own and nothing may flip at +4 T (V16).
bool Decoder::end_of_transmission(float stop) const {
    const float slot = slot_blocks_;
    const float first = end_evidence(stop + slot);
    if (first + end_evidence(stop + 2.0f * slot) < k_end_evidence) return false;
    if (bits_per_package_ != 1) return true;
    if (first < k_end_single_evidence) return false;
    // N = 1: the chain's next STOPs would sit at +4 T and +6 T. Neither may flip, and together they must hold less
    // than one clear flip's evidence: a weak STOP there and a noise flip at +T would end the transmission mid-chain.
    float chain = 0.0f;
    for (uint8_t j = k_end_single_quiet_slot; j <= k_end_single_last_slot; j = static_cast<uint8_t>(j + 2u)) {
        const FlipMeasure after = measure_flip(stop + static_cast<float>(j) * slot, k_marker_half * slot);
        if (is_flip(after, k_kappa_track)) return false;
        if (after.valid && after.kappa > 0.0f) chain += after.q;
    }
    return chain < k_q_track;
}

// The END markers have quiet edges like every marker, nothing flips after them and the slot after them is silent
// (spec 3.11 "clean END").
bool Decoder::end_clean(float stop) {
    const float slot = slot_blocks_;
    PackageStats edges;
    memset(&edges, 0, sizeof(edges));
    for (uint8_t j = 1; j <= k_end_markers; ++j) {
        add_marker_edges(stop + static_cast<float>(j) * slot, slot, reference_average_, edges);
    }
    if (edges.edges > 0) {
        const float count = static_cast<float>(edges.edges);
        const float allowance = k_marker_edge_sigmas * (edges.edge_noise / count) / sqrtf(count);
        if (edges.edge_excess / count > k_marker_edge_max + allowance) return false;
    }
    const float after = stop + static_cast<float>(k_end_clean_slot) * slot;
    if (is_flip(measure_flip(after, k_marker_half * slot), k_kappa_track)) return false;
    return slot_amplitude(after, slot) < decision_threshold(slot_noise(slot), reference_average_);
}

// Slot edges of the package on a steady carrier (the next tune): the longest run so far, carried across packages.
// `truncate` receives the bits before the first slot that holds such a tone or a marker (spec 3.9 step 8).
uint8_t Decoder::tune_edges(const Marker& start, const Marker& stop, uint8_t bits, uint8_t& truncate) {
    const float slot = (stop.position - start.position) / static_cast<float>(bits + 1u);
    const float half = k_marker_half * slot;
    uint8_t longest = tune_run_;
    truncate = bits;
    for (uint8_t i = 0; i <= bits; ++i) {
        const float edge = start.position + (static_cast<float>(i) + k_slot_centre) * slot;
        float excess = 0.0f;
        float noise_ratio = 0.0f;
        bool tune = false;
        if (boundary_excess(edge, slot, reference_average_, excess, noise_ratio) && excess >= k_tune_edge_energy) {
            const FlipMeasure m = measure_flip(edge, half);
            tune = m.valid && m.kappa <= k_tune_kappa;
        }
        tune_run_ = tune ? static_cast<uint8_t>(tune_run_ < k_count_limit_u8 ? tune_run_ + 1u : tune_run_) : 0;
        if (tune_run_ > longest) longest = tune_run_;
        if (i == 0 || truncate < bits) continue;
        // Slot i (1..N) holds the tone when the edge after it does, or a marker at its centre (a full one: a noise flip
        // there would cut a package whose STOP merely faded).
        const FlipMeasure centre = measure_flip(start.position + static_cast<float>(i) * slot, half);
        const bool marker = is_flip(centre, k_kappa_track) && centre.q >= k_audit_high &&
                            centre.amplitude >= k_train_amplitude_ratio * reference_average_;
        if (tune || marker) truncate = static_cast<uint8_t>(i - 1u);
    }
    if (stop.detected) tune_run_ = 0;
    return longest;
}

// Spec 3.10: slot levels against the reference line from the START crest to the STOP crest, the smart or fixed
// decision line, soft values, quiet-gap noise, the audit positions (spec 3.11) and the slot edges.
void Decoder::decide_package(const Marker& start, const Marker& stop, uint8_t bits, Package& package,
                             PackageStats& stats, uint8_t* level_pct, uint8_t* threshold_pct, int8_t* audit) {
    memset(&package, 0, sizeof(package));
    memset(&stats, 0, sizeof(stats));
    package.count = bits;
    const float slots = static_cast<float>(bits + 1u);
    const float slot = (stop.position - start.position) / slots;
    const SlotNoise noise = slot_noise(slot);
    // N_a of the noise measured before the lock (ACQUIRE's estimate, not updated in TRACK)
    const float prelock_noise = noise.slot * max_of(noise_.mean_estimate(), k_min_noise_variance) / noise_variance();
    bool weak = false;
    for (uint8_t i = 1; i <= bits; ++i) {
        const float amplitude = slot_amplitude(start.position + static_cast<float>(i) * slot, slot);
        const float reference = start.amplitude + (stop.amplitude - start.amplitude) * static_cast<float>(i) / slots;
        const float threshold = decision_threshold(noise, reference);
        const bool bit = amplitude >= threshold;
        const uint8_t level = percent_of(amplitude, reference);
        const uint8_t line = percent_of(threshold, reference);
        if (bit) {
            package.bits[(i - 1u) / k_bits_per_byte] |= static_cast<uint8_t>(k_top_bit >> ((i - 1u) % k_bits_per_byte));
            ++stats.ones;
            stats.ones_level_sum += static_cast<float>(level);
        } else {
            ++stats.zeros;
            if (amplitude * amplitude > k_loud_zero * prelock_noise) ++stats.loud_zeros;
        }
        stats.threshold_sum += static_cast<float>(line);
        const float margin = threshold > 0.0f ? (amplitude - threshold) / threshold : 0.0f;
        package.soft[i - 1u] = static_cast<int8_t>(round_to_int(clamp(k_soft_scale * margin, -k_soft_limit, k_soft_limit)));
        if (fabsf(margin) < k_weak_margin) weak = true;
        if (level_pct != 0) level_pct[i - 1u] = level;
        if (threshold_pct != 0) threshold_pct[i - 1u] = line;
    }
    // Noise from the gaps between two decided zeros (V11); a package with none uses the gaps between a marker and a
    // zero. Next to a one the window holds the Tukey tail widened by the CIC-2; next to a marker too, which outweighs
    // the noise above k_half_quiet_snr_db: there the estimate holds (U27).
    const uint8_t passes = snr_db() < k_half_quiet_snr_db ? 2 : 1;
    for (uint8_t pass = 0; pass < passes && stats.gaps == 0; ++pass) {
        for (uint8_t i = 0; i <= bits; ++i) {
            const bool left_zero = i >= 1 && !package_bit(package.bits, static_cast<uint8_t>(i - 1u));
            const bool right_zero = i + 1u <= bits && !package_bit(package.bits, i);
            const bool quiet = pass == 0 ? left_zero && right_zero
                                         : (i == 0 && right_zero) || (i == bits && left_zero);
            if (!quiet) continue;
            const float centre = start.position + (static_cast<float>(i) + k_slot_centre) * slot;
            const float half_gap = k_slot_centre * k_gap_window * slot;
            Complex sum;
            if (!history_.window(origin_block_, centre - half_gap, centre + half_gap, sum) ||
                history_.any_blanked(origin_block_, centre - half_gap, centre + half_gap)) {
                continue;
            }
            stats.gap_noise += magnitude_squared(sum) / dsp::noise_samples(k_gap_window * slot, block_samples_);
            ++stats.gaps;
        }
    }
    if (!start.detected) package.flags |= event_flag_flywheel_start;
    if (!stop.detected) package.flags |= event_flag_flywheel_stop;
    if (history_.any_blanked(origin_block_, start.position, stop.position)) package.flags |= event_flag_blanked;
    if (weak) package.flags |= event_flag_weak;
    if (late_join_ && state_ == DecoderState::track) package.flags |= event_flag_late_join;

    // Audit: slot centres and slot edges, 2 d + 1 positions, none within 0.35 T of a marker under a right lock. The
    // edges next to START and STOP keep their windows k_audit_edge_half from them: 0.35 T would end 0.025 T before
    // the twist, inside the jitter of the markers' measured positions.
    const float half = k_marker_half * slot;
    const float edge_half = k_audit_edge_half * slot;
    stats.strongest = k_audit_low;
    for (uint8_t k = 1; k <= 2u * bits + 1u; ++k) {
        const float fraction = k_audit_step * static_cast<float>(k);
        const bool next_to_marker = k == 1 || k == 2u * bits + 1u;
        const FlipMeasure m = measure_flip(start.position + fraction * slot, next_to_marker ? edge_half : half);
        const float reference = start.amplitude + (stop.amplitude - start.amplitude) * fraction / slots;
        float evidence = 0.0f;
        if (m.valid) {
            const bool marker_like = m.kappa > 0.0f && m.amplitude >= k_flip_amplitude_ratio * reference;
            evidence = marker_like ? clamp(m.q_balanced, k_audit_low, k_audit_high)
                                   : clamp(m.q_balanced, k_audit_low, 0.0f);
        }
        stats.strongest = max_of(stats.strongest, evidence);
        if (k % 2u == 0u && evidence >= k_audit_high) ++stats.twisted;
        audit[k - 1u] = static_cast<int8_t>(round_to_int(evidence * static_cast<float>(k_audit_scale)));
    }
    if (stop.detected) add_marker_edges(stop.position, slot, stop.amplitude, stats);
    measure_shape(start, slot, bits, package, stats);
    // Data slots never flip (spec 1.1): most of them flipping at full strength are no data (a twist cancels a slot's
    // level, so they read 0). An erasure: its bytes are missing, not wrong.
    package.faded = 2u * stats.twisted > bits;
}

// slot events (1..d), then the package event (spec 5.1).
void Decoder::emit_package(const Package& package, const Marker& start, const Marker& stop, float reference,
                           const uint8_t* level_pct, const uint8_t* threshold_pct) {
    const uint8_t start_pct = percent_of(start.amplitude, reference);
    const uint8_t stop_pct = percent_of(stop.amplitude, reference);
    for (uint8_t i = 0; i < package.count; ++i) {
        Event event = make_event(EventType::slot);
        event.value = package_bit(package.bits, i) ? 1 : 0;
        event.slot = static_cast<uint8_t>(i + 1u);
        event.level_pct = level_pct[i];
        event.threshold_pct = threshold_pct[i];
        event.start_pct = start_pct;
        event.stop_pct = stop_pct;
        event.soft[0] = package.soft[i];
        event.flags = package.flags;
        event.package_index = package.index;
        emit(event);
    }
    Event event = make_event(EventType::package);
    event.value = package.count;
    event.start_pct = start_pct;
    event.stop_pct = stop_pct;
    event.flags = package.flags;
    event.package_index = package.index;
    event.slot_ms = blocks_to_ms((stop.position - start.position) / static_cast<float>(package.count + 1u));
    emit(event);
}

void Decoder::push_audit(const int8_t* audit, uint8_t bits) {
    for (uint8_t k = 0; k < 2u * bits + 1u; ++k) {
        audit_.set(k, static_cast<float>(audit[k]) / static_cast<float>(k_audit_scale));
    }
    audit_.next_package();
}

// A package of an unconfirmed lock: held by the guard, with its STOP and its tallies (spec 3.11).
void Decoder::guard_package(const Package& package, const PackageStats& stats, const Marker& stop) {
    hold(package);
    if (guard_packages_ < k_count_limit_u8) ++guard_packages_;
    guard_slots_ = static_cast<uint16_t>(guard_slots_ + package.count + 1u);
    if (guard_markers_ < k_count_limit_u8) ++guard_markers_;
    const bool balanced = stop.detected && stop.balanced >= k_q_track;
    if (stop.detected && guard_detected_ < k_count_limit_u8) ++guard_detected_;
    if (balanced && guard_balanced_ < k_count_limit_u8) ++guard_balanced_;
    guard_stop_q_ = min_of(guard_stop_q_, balanced ? stop.balanced : 0.0f);
    guard_edge_excess_ += stats.edge_excess;
    guard_edge_noise_ += stats.edge_noise;
    guard_edges_ = static_cast<uint8_t>(min_of(static_cast<float>(guard_edges_) + stats.edges, k_count_limit_u8));
    guard_edge_energy_ += stats.edge_energy;
    guard_one_energy_ += stats.one_energy;
    guard_zeros_ = static_cast<uint8_t>(min_of(static_cast<float>(guard_zeros_) + stats.zeros, k_count_limit_u8));
    guard_loud_zeros_ =
        static_cast<uint8_t>(min_of(static_cast<float>(guard_loud_zeros_) + stats.loud_zeros, k_count_limit_u8));
    guard_ones_ = static_cast<uint16_t>(guard_ones_ + stats.ones);
    guard_bits_ = static_cast<uint16_t>(guard_bits_ + package.count);
    guard_threshold_sum_ += stats.threshold_sum;
    guard_ones_level_sum_ += stats.ones_level_sum;
    if (stats.strongest >= k_q_track && guard_inner_ < k_count_limit_u8) ++guard_inner_;
}

// A confirmed lock releases a package with its held predecessors when its STOP was detected, else holds it; a full
// hold buffer releases its oldest.
void Decoder::release(const Package& package, bool detected) {
    if (detected) {
        release_held();
        assemble(package);
        return;
    }
    if (held_count_ == k_held_packages) {
        assemble(held_[0]);
        for (uint8_t i = 1; i < held_count_; ++i) held_[i - 1] = held_[i];
        --held_count_;
    }
    hold(package);
}

// Spec 3.11: refusals, the quick confirmation of a clean fresh lock, and the full guard.
void Decoder::check_guard() {
    if (confirmed_) return;
    if (guard_slots_ < k_guard_slots || guard_packages_ < k_min_guard_packages) return;
    if (guard_refused()) {
        lose(LostReason::signal_gone, false);  // not a signal of ours: search again
        return;
    }
    if (!late_join_ && guard_clean() && guard_stop_q_ >= k_quick_stop_q && audit_.max_evidence() < k_quick_audit) {
        confirm();
        return;
    }
    if (guard_slots_ < k_full_guard_slots || guard_packages_ < k_full_guard_packages) return;
    // A train read as a package chain decodes its ones far above the markers; a grid off the packages carries
    // markers inside them.
    const bool loud_ones = guard_ones_ > 0 && guard_ones_level_sum_ >= k_guard_max_level * k_percent * guard_ones_;
    // A grid off the packages puts markers inside most of them; a long guard (N = 1: 18 packages) also meets a few
    // noise flips next to its markers' one-sided edges, and so does a package of many audit positions (the share was
    // set for N = 8's 17): noise touches packages in proportion to their positions. All of them inner is refused.
    const float positions =
        static_cast<float>(2u * bits_per_package_ + 1u) / static_cast<float>(k_guard_share_positions);
    const float share = ceilf(k_guard_inner_share * static_cast<float>(guard_packages_) * max_of(positions, 1.0f));
    const uint8_t inner_limit = static_cast<uint8_t>(
        min_of(max_of(static_cast<float>(k_guard_max_inner), share), static_cast<float>(guard_packages_)));
    if (loud_ones || guard_inner_ >= inner_limit) {
        ban_alias(slot_blocks_);
        lose(LostReason::alias);
        return;
    }
    if (static_cast<float>(guard_detected_) >= k_guard_min_detected * static_cast<float>(guard_markers_) &&
        guard_balanced_ >= k_guard_min_balanced) {
        confirm();
        return;
    }
    lose(LostReason::signal_gone);  // too weak to confirm yet: the tone and the candidates stay
}

// The decision lines average at or above the reference (the markers are not above the noise), the SNR does not
// decode, the slot edges or the zeros are loud: not a signal of ours.
bool Decoder::guard_refused() const {
    if (guard_bits_ > 0 && guard_threshold_sum_ >= k_percent * static_cast<float>(guard_bits_)) return true;
    return !decodable() || !slot_edges_quiet() || !zeros_quiet() ||
           !beep_shaped(guard_edge_energy_, guard_bits_ + guard_packages_, guard_one_energy_, guard_ones_);
}

// Every STOP of the guard a balanced flip, no package with a flip inside, the first START a detected train marker of a
// train of at least 6 markers that followed a steady tune, and at least one decided 1 (spec 3.11).
bool Decoder::guard_clean() const {
    return guard_started_ && guard_packages_ > 0 && guard_balanced_ == guard_packages_ && guard_inner_ == 0 &&
           guard_ones_ > 0 && tone_steady_ && train_markers_ >= k_train_min_markers && !late_join_;
}

bool Decoder::decodable() const {
    const float model_db = k_model_snr_db - k_db_per_decade * log10f(slot_ms() / k_model_slot_ms);
    return snr_db() >= model_db - k_confirm_margin_db;
}

// Data beeps fall silent at every slot edge (spec 1.1): the mean slot-edge energy is at most k_fold_edge_ratio of the
// mean energy at the ones' centres, the cold join's fold test. With no one there is nothing to judge.
bool Decoder::beep_shaped(float edge_energy, uint16_t edges, float one_energy, uint16_t ones) const {
    if (ones == 0 || edges == 0) return true;
    return edge_energy / static_cast<float>(edges) <= k_fold_edge_ratio * one_energy / static_cast<float>(ones);
}

bool Decoder::zeros_quiet() const {
    return 2u * guard_loud_zeros_ <= guard_zeros_;
}

bool Decoder::slot_edges_quiet() const {
    if (guard_edges_ == 0) return true;
    const float count = static_cast<float>(guard_edges_);
    const float allowance = k_marker_edge_sigmas * (guard_edge_noise_ / count) / sqrtf(count);
    return guard_edge_excess_ / count <= k_marker_edge_max + allowance;
}

// Boundary excess at the +-0.5 T edges of a marker (its slot's edges are nulls, whatever the data).
void Decoder::add_marker_edges(float marker, float slot_blocks, float crest, PackageStats& stats) const {
    for (int8_t side = -1; side <= 1; side += 2) {
        float excess = 0.0f;
        float noise_ratio = 0.0f;
        if (!boundary_excess(marker + static_cast<float>(side) * k_slot_centre * slot_blocks, slot_blocks, crest, excess,
                             noise_ratio)) {
            continue;
        }
        stats.edge_excess += excess;
        stats.edge_noise += noise_ratio;
        if (stats.edges < k_count_limit_u8) ++stats.edges;
    }
}

// The shape of the data (spec 1.1): every beep falls silent at its slot edges (Tukey alpha 0.5), so the energy at the
// package's slot edges is a small part of the energy at the centres of its ones. A steady carrier, keyed CW or two
// tones beating are as loud at the edges. Windows of +-k_fold_half T, less the noise, as the cold join's fold.
void Decoder::measure_shape(const Marker& start, float slot_blocks, uint8_t bits, const Package& package,
                            PackageStats& stats) const {
    const float half = k_fold_half * slot_blocks;
    const float noise = noise_variance() * dsp::noise_samples(2.0f * half, block_samples_);
    for (uint8_t i = 0; i <= bits; ++i) {
        const float edge = start.position + (static_cast<float>(i) + k_slot_centre) * slot_blocks;
        stats.edge_energy += window_energy(edge, half) - noise;
        if (i >= 1 && package_bit(package.bits, static_cast<uint8_t>(i - 1u))) {
            stats.one_energy += window_energy(start.position + static_cast<float>(i) * slot_blocks, half) - noise;
        }
    }
}

// The held packages are released up to the newest whose STOP was detected; those after it stay held like TRACK's
// flywheel packages (spec 3.9 step 9): past a missed END they are no packages at all, and LOST discards them.
void Decoder::confirm() {
    confirmed_ = true;
    tone_confirmed_ = true;
    emit_locked(held_count_ > 0 ? held_[0].index : package_index_);
    uint8_t framed = 0;
    for (uint8_t i = 0; i < held_count_; ++i) {
        if ((held_[i].flags & event_flag_flywheel_stop) == 0) framed = static_cast<uint8_t>(i + 1u);
    }
    for (uint8_t i = 0; i < framed; ++i) assemble(held_[i]);
    for (uint8_t i = framed; i < held_count_; ++i) held_[i - framed] = held_[i];
    held_count_ = static_cast<uint8_t>(held_count_ - framed);
}

void Decoder::release_held() {
    for (uint8_t i = 0; i < held_count_; ++i) assemble(held_[i]);
    held_count_ = 0;
}

void Decoder::hold(const Package& package) {
    if (held_count_ < k_held_packages) held_[held_count_++] = package;
}

// Spec 3.13: package k carries stream bits k N .. k N + d - 1; a byte is emitted when all its bits came from released
// packages. After a gap (a lost package, or one cut short) the byte being assembled is dropped and assembly restarts at
// the next byte boundary.
// A marker read at the noise floor: gone, not merely missed.
bool Decoder::marker_gone(const Marker& marker) const {
    return !marker.detected && marker.amplitude < k_floor_sigma * sqrtf(slot_noise(slot_blocks_).marker);
}

// A package inside a fade that took the signal is an erasure (its bytes are missing): both its markers gone; or the
// START gone and every bit 0 (the fade may end inside it). One whose STOP is gone becomes one when the next STOP is
// gone too (flag_fade), whatever its bits: the fade began inside it.
bool Decoder::faded_package(const Package& package, const Marker& start, const Marker& stop) const {
    const bool start_gone = marker_gone(start);
    if (start_gone && marker_gone(stop)) return true;
    if (!start_gone) return false;
    for (uint8_t i = 0; i < package.count; ++i) {
        if (package_bit(package.bits, i)) return false;
    }
    return true;
}

void Decoder::assemble(const Package& package) {
    if (package.faded) return;
    const uint32_t first_bit = package.index * bits_per_package_;
    if (!assembling_ || first_bit != next_bit_) {
        const uint8_t offset = static_cast<uint8_t>(first_bit % k_bits_per_byte);
        byte_skip_ = offset == 0 ? 0 : static_cast<uint8_t>(k_bits_per_byte - offset);
        byte_index_ = (first_bit + byte_skip_) / k_bits_per_byte;
        byte_bits_ = 0;
        byte_value_ = 0;
        byte_flags_ = 0;
    }
    assembling_ = true;
    next_bit_ = first_bit + package.count;
    for (uint8_t i = 0; i < package.count; ++i) {
        if (byte_skip_ > 0) {
            --byte_skip_;
            continue;
        }
        byte_value_ = static_cast<uint8_t>((byte_value_ << 1) | (package_bit(package.bits, i) ? 1u : 0u));
        byte_soft_[byte_bits_] = package.soft[i];
        byte_flags_ = static_cast<uint8_t>(byte_flags_ | package.flags);
        if (++byte_bits_ < k_bits_per_byte) continue;
        Event event = make_event(EventType::byte);
        event.value = byte_value_;
        event.flags = byte_flags_;
        memcpy(event.soft, byte_soft_, sizeof(event.soft));
        event.package_index = package.index;
        event.byte_index = byte_index_;
        emit(event);
        ++byte_index_;
        byte_bits_ = 0;
        byte_value_ = 0;
        byte_flags_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Station memory (spec 3.12)
// ---------------------------------------------------------------------------

void Decoder::remember_station(bool ended) {
    const float whole = floorf(last_detected_.position);
    memory_.tone_hz = nco_.frequency();
    memory_.slot_blocks = slot_blocks_;
    memory_.crest = reference_average_;
    memory_.marker_block = origin_block_ + static_cast<uint32_t>(static_cast<int32_t>(whole));
    memory_.marker_fraction = last_detected_.position - whole;
    memory_.marker_package = last_detected_package_;
    memory_.expires_block = history_.end_block() + static_cast<uint32_t>(ms_to_blocks(k_memory_ms));
    memory_.bits_per_package = bits_per_package_;
    memory_.ended = ended;
    memory_.valid = bits_per_package_ > 0;
}

bool Decoder::memory_valid() const {
    return memory_.valid && static_cast<int32_t>(memory_.expires_block - history_.end_block()) > 0;
}

// Not ended, on the remembered pitch within 10 Hz, at most 64 packages after the remembered marker.
bool Decoder::memory_usable() const {
    if (!memory_valid() || memory_.ended || fabsf(nco_.frequency() - memory_.tone_hz) > k_memory_hz) return false;
    const float package = static_cast<float>(memory_.bits_per_package + 1u) * memory_.slot_blocks;
    return (end_position() - memory_position()) / package <= k_relock_packages;
}

// ACQUIRE on the remembered pitch with no tune heard: the station's running stream.
bool Decoder::stream_relock() const {
    return state_ == DecoderState::acquire && !tone_steady_ && memory_usable();
}

float Decoder::memory_position() const {
    return static_cast<float>(static_cast<int32_t>(memory_.marker_block - origin_block_)) + memory_.marker_fraction;
}

// ---------------------------------------------------------------------------
// Measurement and decision (spec 3.4, 3.10)
// ---------------------------------------------------------------------------

FlipMeasure Decoder::measure_flip(float centre, float half) const {
    Complex before;
    Complex after;
    if (!history_.window(origin_block_, centre - half, centre, before) ||
        !history_.window(origin_block_, centre, centre + half, after)) {
        return invalid_measure(centre);
    }
    const float noise_energy = noise_variance() * dsp::noise_samples(half, block_samples_);
    FlipMeasure m = dsp::flip_measure(before, after, noise_energy, half * static_cast<float>(block_samples_));
    m.position = centre;
    return m;
}

FlipMeasure Decoder::search_flip(float centre, float span, float half, float kappa_min) const {
    const float step = half * (k_search_step / k_marker_half);
    int32_t steps = round_to_int(span / step);
    if (steps > k_max_search_steps) steps = k_max_search_steps;
    float q[2 * k_max_search_steps + 1];
    bool valid[2 * k_max_search_steps + 1];
    FlipMeasure best;
    memset(&best, 0, sizeof(best));
    int32_t best_index = -1;
    for (int32_t i = -steps; i <= steps; ++i) {
        const FlipMeasure m = measure_flip(centre + static_cast<float>(i) * step, half);
        const int32_t index = i + steps;
        valid[index] = m.valid;
        q[index] = m.q;
        if (!m.valid || m.kappa < kappa_min) continue;
        if (best_index < 0 || m.q > best.q) {
            best = m;
            best_index = index;
        }
    }
    if (best_index < 0) return measure_flip(centre, half);
    if (best_index > 0 && best_index < 2 * steps && valid[best_index - 1] && valid[best_index + 1]) {
        const float fraction = dsp::parabolic_offset(q[best_index - 1], q[best_index], q[best_index + 1]);
        if (fraction != 0.0f) {
            const FlipMeasure refined = measure_flip(best.position + fraction * step, half);
            if (refined.valid && refined.kappa >= kappa_min) best = refined;
        }
    }
    return best;
}

// A search whose best flip sits on the edge of its span with a stronger one just outside: the flip belongs to a
// marker off the predicted grid.
bool Decoder::peak_beyond(const FlipMeasure& m, float predicted, float span, float half) const {
    if (!m.valid || !on_edge(m, predicted, span, half)) return false;
    const float step = half * (k_search_step / k_marker_half);
    const FlipMeasure outside = measure_flip(predicted + (m.position > predicted ? span + step : -(span + step)), half);
    return outside.valid && outside.q > m.q;
}

// The carrier's turn across slots is only known on a tone the AFC measured on its tune: elsewhere a frequency error
// of a fraction of 1/T turns a steady carrier into a reversal over two slots.
bool Decoder::bridges_trusted() const {
    return tone_steady_ && afc_looked_;
}

// The carrier from the older marker's second half to the newer marker's first half (spec 1.1: each marker reverses
// it, data slots keep it): reversed, an odd number of hidden markers lies between them.
FlipMeasure Decoder::bridge(float older, float newer, float slot_blocks) const {
    const float half = k_marker_half * slot_blocks;
    Complex before;
    Complex after;
    if (!history_.window(origin_block_, older, older + half, before) ||
        !history_.window(origin_block_, newer - half, newer, after)) {
        return invalid_measure(older);
    }
    const float noise_energy = noise_variance() * dsp::noise_samples(half, block_samples_);
    FlipMeasure m = dsp::flip_measure(before, after, noise_energy, half * static_cast<float>(block_samples_));
    m.position = older;
    return m;
}

// The tune tone ends where the train begins (spec 2.1): the newer of k_tune_bound_slots steady slots in a row, with a
// steady slot edge between them, before the anchor is the tune's last slot, and package 0's START lies
// k_min_sync_markers - 1 slots after the train's first marker or later; the slots between the tune and the anchor are
// the train's markers. The grid index of that bound, or dsp::k_no_start without a tune; `silent` then tells that no
// tune leads to this train (more slots than a train's end may fade were empty, or the slots after the steady ones are
// no train), rather than that the history does not reach back to it.
int32_t Decoder::train_end_bound(float anchor, float slot_blocks, float crest, bool& silent) const {
    const float half = k_marker_half * slot_blocks;
    int32_t steady = 0;
    int32_t markers = 0;  // marker-like slots between the anchor and the newest steady slot
    uint8_t empty = 0;
    silent = false;
    for (int32_t i = 1; i <= k_tune_search_slots; ++i) {
        const float centre = anchor - static_cast<float>(i) * slot_blocks;
        const FlipMeasure m = measure_flip(centre, half);
        if (!m.valid) break;
        bool tone = m.q <= -k_q_track && m.kappa <= -k_kappa_anti && m.steady >= k_tune_bound_crest * crest;
        // Two data ones read steady at their centres too, but they fall silent at the slot edge between them; a tune
        // does not.
        float excess = 0.0f;
        float noise_ratio = 0.0f;
        if (tone && steady > 0) {
            tone = boundary_excess(centre + k_slot_centre * slot_blocks, slot_blocks, crest, excess, noise_ratio) &&
                   excess >= k_tune_edge_energy;
        }
        steady = tone ? steady + 1 : 0;
        if (steady == k_tune_bound_slots) {
            // The train runs from the tune's end to the anchor: every slot between is a marker, but for the few that
            // fade at the train's end. Anything else between is no train of this tune.
            if (markers + static_cast<int32_t>(k_faded_train_bits) < i - k_tune_bound_slots) {
                silent = true;
                break;
            }
            return k_tune_bound_slots - i + static_cast<int32_t>(k_min_sync_markers) - 1;
        }
        const bool loud = m.amplitude >= k_flip_amplitude_ratio * crest;
        if (!tone && loud && m.q >= k_sync_weak_q && m.kappa >= k_kappa_track) ++markers;
        const bool nothing = !tone && !loud && m.steady < k_tune_bound_crest * crest;
        empty = nothing ? static_cast<uint8_t>(empty + 1u) : 0;
        if (empty > k_faded_train_bits) {
            silent = true;
            break;
        }
    }
    return dsp::k_no_start;
}

// Gaps of one among the sync's markers, and two for a pair two slots apart whose carrier reversed between them (a
// faded train marker).
uint8_t Decoder::sync_train_ones(float anchor, float slot_blocks, uint8_t marker_bits) const {
    uint8_t ones = count_bits(static_cast<uint8_t>(marker_bits & (marker_bits >> 1)));
    if (!bridges_trusted()) return ones;
    for (uint8_t i = 0; i + 2 < k_sync_positions; ++i) {
        const uint32_t pair = (1u << i) | (1u << (i + 2));
        if ((marker_bits & pair) != pair || (marker_bits & (1u << (i + 1))) != 0) continue;
        const float newer = anchor - static_cast<float>(i) * slot_blocks;
        if (carrier_parity(bridge(newer - 2.0f * slot_blocks, newer, slot_blocks)) > 0) ones += 2;
    }
    return ones;
}

// A marker measured at `position` (its crest whether or not it flips).
Decoder::Marker Decoder::measure_marker(float position, float slot_blocks) const {
    const FlipMeasure m = measure_flip(position, k_marker_half * slot_blocks);
    Marker marker;
    marker.position = position;
    marker.amplitude = m.amplitude;
    marker.balanced = m.q_balanced;
    marker.detected = is_flip(m, k_kappa_track);
    return marker;
}

float Decoder::slot_amplitude(float centre, float slot_blocks) const {
    const float half_window = k_slot_centre * k_slot_window * slot_blocks;
    Complex sum;
    if (!history_.window(origin_block_, centre - half_window, centre + half_window, sum)) return 0.0f;
    const float gain = k_slot_window * slot_blocks * static_cast<float>(block_samples_) * k_g_slot;
    return 2.0f * sqrtf(magnitude_squared(sum)) / gain;
}

// |S|^2 over centre +- half, 0 when the history does not hold it.
float Decoder::window_energy(float centre, float half) const {
    Complex sum;
    if (!history_.window(origin_block_, centre - half, centre + half, sum)) return 0.0f;
    return magnitude_squared(sum);
}

// Expected squares of a data slot's and a marker's amplitude on noise alone (N_a, N_m of spec 3.10) and the floor
// of the decision.
Decoder::SlotNoise Decoder::slot_noise(float slot_blocks) const {
    const float sigma2 = noise_variance();
    const float data_blocks = k_slot_window * slot_blocks;
    const float slot_gain = data_blocks * static_cast<float>(block_samples_) * k_g_slot;
    SlotNoise noise;
    noise.slot = k_noise_amplitude * sigma2 * dsp::noise_samples(data_blocks, block_samples_) / (slot_gain * slot_gain);
    noise.marker = marker_noise(sigma2, slot_blocks);
    noise.floor = k_floor_sigma * sqrtf(noise.slot);
    return noise;
}

// The smart line (equal likelihood, 50..75 % of the reference) or the fixed fraction, never below the noise floor.
float Decoder::decision_threshold(const SlotNoise& noise, float reference) const {
    float ratio = config_.fixed_ratio;
    if (config_.decision_mode == DecisionMode::adaptive) {
        ratio = dsp::equal_likelihood_ratio(2.0f * max_of(reference * reference - noise.marker, 0.0f) / noise.slot);
    }
    return max_of(ratio * reference, noise.floor);
}

float Decoder::noise_variance() const {
    const float estimate = state_ == DecoderState::track ? track_noise_.mean_estimate() : noise_.mean_estimate();
    return max_of(estimate, k_min_noise_variance);
}

// Expected square of a marker crest measured on noise alone: N_m.
float Decoder::marker_noise(float sigma2, float slot_blocks) const {
    const float half = k_marker_half * slot_blocks;
    const float gain = 2.0f * half * static_cast<float>(block_samples_) * k_g_marker;
    return k_noise_amplitude * sigma2 * 2.0f * dsp::noise_samples(half, block_samples_) / (gain * gain);
}

float Decoder::end_position() const {
    return static_cast<float>(history_.end_block() - origin_block_);
}

float Decoder::candidate_position(const Candidate& candidate) const {
    return static_cast<float>(static_cast<int32_t>(candidate.block - origin_block_)) + candidate.fraction;
}

float Decoder::blocks_to_ms(float blocks) const {
    return blocks * static_cast<float>(block_samples_) / k_samples_per_ms;
}

float Decoder::ms_to_blocks(float ms) const {
    return ms * k_samples_per_ms / static_cast<float>(block_samples_ > 0 ? block_samples_ : k_min_block_samples);
}

bool Decoder::in_range(float slot_blocks) const {
    const float low = k_blocks_per_min_slot * (1.0f - k_range_tolerance);
    const float high = k_blocks_per_min_slot * k_speed_span * (1.0f + k_range_tolerance);
    return slot_blocks >= low && slot_blocks <= high;
}

bool Decoder::banned(float slot_blocks) const {
    if (static_cast<int32_t>(ban_until_block_ - history_.end_block()) <= 0) return false;
    return fabsf(slot_blocks - ban_slot_blocks_) <= k_alias_ban_band * ban_slot_blocks_;
}

void Decoder::ban_alias(float slot_blocks) {
    ban_slot_blocks_ = slot_blocks;
    ban_until_block_ = history_.end_block() + static_cast<uint32_t>(ms_to_blocks(k_alias_ban_ms));
}

void Decoder::update_reference(float amplitude) {
    reference_average_ += k_reference_alpha * (amplitude - reference_average_);
}

void Decoder::reset_scales() {
    for (uint8_t s = 0; s < k_candidate_scales; ++s) {
        scale_q_[s][0] = k_no_value;
        scale_q_[s][1] = k_no_value;
        scale_kappa_[s] = k_no_value;
    }
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

Event Decoder::make_event(EventType type) const {
    Event event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.reason = LostReason::none;
    event.state = state_;
    event.bits_per_package = bits_per_package_;
    event.package_index = package_index_;
    event.tone_hz = tone_hz();
    event.slot_ms = slot_ms();
    event.snr_db = snr_db();
    return event;
}

void Decoder::emit(const Event& event) {
    if (handler_ != 0) handler_(event, context_);
}

void Decoder::emit_locked(uint32_t package_index) {
    Event event = make_event(EventType::locked);
    event.flags = late_join_ ? event_flag_late_join : 0;
    event.package_index = package_index;
    emit(event);
}

}  // namespace unlimited
