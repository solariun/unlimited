#include "unlimited/decoder.hpp"

#include <math.h>
#include <string.h>

namespace unlimited {

#if UNLIMITED_MAX_BITS_PER_PEAK <= 7 && UNLIMITED_MAX_FRAME_BYTES <= 16
static_assert(sizeof(Decoder) <= 12288, "Decoder must fit 12 KB at the MCU caps (7, 16) (spec 3.15)");
#endif

const uint8_t Decoder::k_frame_buffers;
const uint8_t Decoder::k_max_held_frames;
const uint8_t Decoder::k_presence_window;
const uint8_t Decoder::k_header_ring;

namespace {

using dsp::Candidate;
using dsp::Complex;
using dsp::FlipMeasure;

// Profiles (spec 1.4, 5.1).
const uint8_t k_ssb_min_slot_ms = 16;
const uint8_t k_am_min_slot_ms = 8;
const uint8_t k_fm_min_slot_ms = 4;
const uint16_t k_fm_min_tone_hz = 1000;
const uint8_t k_min_block_samples = 4;
const uint8_t k_max_block_samples = 32;
const uint8_t k_small_block_samples = 8;  // below this the CIC-2 image needs tones >= k_fm_min_tone_hz

// Units.
const float k_ms_per_s = 1000.0f;
const float k_us_per_ms = 1000.0f;
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
const float k_energy_scale = 256.0f;                                 // (1 << k_energy_shift)^2
const float k_in_bin_scale = 2.0f / (k_mixer_gain * k_mixer_gain * k_energy_scale);  // e_k = p_k for a steady tone
const uint8_t k_settle_blocks = dsp::ImpulseBlanker::k_latency + 1;  // blocks mixed before the NCO was set
// History block j holds the CIC-2 output of block j - k_latency, a triangle centred on that block's first
// sample: history position p is input sample (p - k_history_lag) B.
const float k_history_lag = static_cast<float>(dsp::ImpulseBlanker::k_latency) + 0.5f;
const uint8_t k_noise_blocks = 8;
const float k_noise_outlier = 4.0f;  // ACQUIRE windows above this many times the noise are signal
const uint16_t k_afc_decimation_samples = 64;                        // 8 ms: 125 Hz AFC input
const float k_afc_eval_ms = 250.0f;
const uint16_t k_afc_min_inputs = 16;
const float k_afc_wait_ms = 1000.0f;  // a data lock may start joining without an AFC look after this long
const float k_afc_min_offset_hz = 0.2f;
const uint8_t k_onset_products = 3;       // steady search products before a train onset (80 ms)
const uint8_t k_onset_products_tune = 4;  // ... while ACQUIRE holds a tune tone (100 ms)
// ... in PREAMBLE, whose own markers and the peaks of a stream stay steady at most 128 ms in the search's other bins
// (their sidelobes, a clean channel): a tune (>= 250 ms) is steady longer (200 ms).
const uint8_t k_onset_products_preamble = 10;
// Onsets that are no new transmission. A train of period 2 T has lines at odd multiples of 1 / (2 T) from f_ref, steady
// while it runs and broken when the header starts; its power is in the first orders (2 m + 1) / (2 T), m <= 7. The
// search's estimate of a steady tone is good to about 1 Hz.
const uint8_t k_train_line_orders = 7;
const float k_train_line_hz = 5.0f;
// A saturated input (int16 or ADC clipping) adds odd harmonics of the tune, which alias into the band (8000 - 3 f above
// 1333 Hz): the image reverses with the tune and reads as a tune and train of its own. Reach: one search bin, the NCO's
// error times the order.
const uint8_t k_first_image_order = 3;
const uint8_t k_last_image_order = 7;
const float k_image_reach_hz = 50.0f;
const float k_afc_reset_rotation = 0.5f;  // radians over the slowest marker half window
// A tune lock keeps the noise measured before its tone (lock_tone): behind a receiver AGC that noise was raised by the
// AGC while nothing else was heard, up to the signal's SNR. The search's recent floor, under the tone, replaces it
// when it is less than half of it.
const float k_noise_drop = 0.5f;
const float k_preamble_afc_limit = 0.25f;  // cycles per slot

// Window geometry, in slots (spec 3.4, 3.9).
const float k_marker_half = 0.35f;
const float k_search_step = 0.05f;
const float k_position_search = 0.10f;
const float k_first_stop_search = 0.25f;
const float k_track_search = 0.15f;
const float k_track_search_miss = 0.5f;
const uint8_t k_max_search_steps = 10;  // k_track_search_miss / k_search_step
const float k_slot_centre = 0.5f;
const float k_audit_step = 0.5f;

// Flip thresholds.
const float k_q_candidate = 3.0f;
const float k_kappa_candidate = 0.5f;
const float k_q_track = 4.0f;
const float k_kappa_track = 0.3f;
const float k_kappa_after_miss = 0.5f;
const float k_q_present = 1.0f;
const float k_evidence_clip = 8.0f;
const float k_kappa_spread = 4.2f;  // 3 sigma of the kappa of a true flip: 3 * sqrt(2) / sqrt(E)
const float k_kappa_floor_max = 0.75f;
const float k_no_value = -1e30f;

// Sync (spec 3.7).
const uint8_t k_sync_positions = 8;
const uint8_t k_sync_midpoints = 4;           // midpoints between the newest train positions
const uint8_t k_sync_max_midpoint_flips = 2;  // this many flips between markers rejects the hypothesis
// Between two train markers the envelope is null: the Tukey tails of both meet at the slot edge, and over
// +-0.1 T around it their mean is 0.12 of the crest (energy 0.015). Interference whose flips pass for markers
// is loud there: two tones beating reverse at the beat nulls and peak halfway, and a hypothesis at half the
// train's period puts its midpoints inside the markers.
const float k_boundary_half = 0.1f;  // slots
const float k_boundary_max = 0.25f;  // excess energy over the crest's in the same window (amplitude 0.5)
const uint8_t k_sync_min_positions = 5;
// Hits: 5, or 4 among the newest 5 positions with at least 2 at odd and 2 at even positions: a train near the gate
// (one marker of 5 missed at q < 4) is still taken. A T / 2 reading of a 2T train hits one parity only, and a wrong T
// scatters its hits over the 8 positions.
const uint8_t k_sync_min_hits = 5;
const uint8_t k_sync_dense_hits = 4;
const uint8_t k_sync_dense_mask = 0x1F;  // positions 0..4
const uint8_t k_sync_odd_mask = 0xAA;
const uint8_t k_sync_min_parity_hits = 2;
// Near the gate (T = 8 ms at +1.5 dB) a third of the markers flip at q 2..4: a weak marker (q >= 2, marker-like crest)
// completes a train of at least 3 hits, one at each parity, when every position scored but one holds a marker (at
// least 5, 2 at each parity). The other positions of a T / 2 or 3T / 2 reading are slot edges between markers, where
// noise alone reaches q 2 (p ~ 3 % at T = 128 ms near the gate), but seldom at all of them, and seldom a hit.
const float k_sync_weak_q = 2.0f;
const uint8_t k_sync_min_strong = 3;
const uint8_t k_sync_min_parity_strong = 1;
const uint8_t k_sync_missing_markers = 1;
const float k_sync_evidence = 24.0f;
const float k_prefer_smaller_t = 0.8f;
const float k_hypothesis_merge = 0.03f;
const uint8_t k_max_hypotheses = 64;
const uint8_t k_max_accepted = 16;
const uint8_t k_refine_passes = 2;
const uint8_t k_refine_min_points = 3;
const float k_refine_max_change = 0.1f;  // a least-squares T further than 10 % away is ignored
// A train's T is accepted up to 6 % outside T_min..T_max: its estimate is noisy at the range's ends (T = 128 ms at
// the hf_weak gate), and the header's exact T is checked against the range itself.
const float k_range_tolerance = 0.06f;
const uint8_t k_half_rate_odd_hits = 1;  // at most one (noise) hit at the odd positions of a T/2 reading

// Late join with mode memory (spec 3.7, 3.12).
const uint8_t k_late_join_intervals = 3;
const float k_late_join_q = 4.0f;
// ACQUIRE on the remembered f_ref with no tune heard is the station's running stream (a fade, a late start). At high SNR
// its data peaks leak into f_ref and flip there every few blocks: at up to 0.16 of the marker crest, where the chain's
// markers read 0.6..1 at any candidate scale. Only marker-like candidates enter the list then (the leaks filled it in
// less than a frame, and the late join needs 3 frame periods), a train must have the remembered T, and the search does
// not watch (the stream's peaks at T >= 64 ms look like tunes and trains to it).
const float k_stream_marker_ratio = 0.35f;  // of the remembered crest
// ACQUIRE on the stream lasts at least this many remembered frames: LOST comes k_lost_absent frames into a fade, and
// the late join needs k_late_join_intervals frame periods of chain after the signal is back, plus one to find it.
const uint8_t k_stream_timeout_frames = 8;
// A tune on the held f_ref ends the stream reading: steady over halves of 0.75 T_mem at least. A marker (a reversal in
// the middle of its slot) and the silent data slots around it fill such a half by less than half, so a stream at
// T = 128 ms, whose marker halves pass for steady over the widest candidate scale (44 ms), reads 0.3 of a tune.
const float k_stream_tune_half = 0.75f;  // of the remembered T

// Preamble (spec 3.8).
const int32_t k_preamble_max_slots = 72;
const float k_train_nudge = 0.2f;        // T += 0.2 (measured - T) at each train flip
// From the third marker on, T and the grid come from a least-squares line through every marker of the train (the
// sync's included), each weighted by its q (up to k_evidence_clip): 5 markers near the gate read T within 1.6 %, which
// is 0.2 T at the header's last slot; the 8 of a whole train halve that.
const float k_fit_min_weight = 1.0f;
const float k_fit_min_points = 3.0f;
const int32_t k_train_max_gap = 2;       // a flip this close to the previous one continues the train
const uint8_t k_sub_rate_gaps = 2;       // equal train gaps k >= 2 that reveal a T/k reading
const uint8_t k_sub_rate_ratio = 2;      // ... outnumbering the gaps of 1 this many times
const int32_t k_header_stop = k_header_slots + 1;  // the header STOP, in slots after its START
const uint8_t k_header_max_inner_flips = 1;
// The header START may follow the last detected train flip L by up to this many slots (the train's last markers
// faded: spec 8.2 L6' zeroes any 3 of the 8); spec 3.8 names L + 1.
const int32_t k_header_max_skip = 3;
const uint8_t k_header_min_peaks = 4;   // slots of H(s) with a peak: a header is there, faded or not
const float k_band_margin_hz = 100.0f;  // received grid against the sender's band, 300..2700 Hz
const float k_snap_reach_ms = 6.0f;     // exact T at most this far from the train's T (the snap is +-4 ms)
// ... and within 5 % of it: header windows placed at a T 5 % off have drifted by half a slot at the header's end, so a
// header decoded there is not one (at T <= 16 ms the +-4 ms of the snap alone would accept a train 25 % off).
const float k_snap_tolerance = 0.05f;
const float k_header_carrier = 2.0f;    // a header tone's background this far over N_h is a carrier
// The header is decided on the sample clock one slot after its last peak; this bounds the wait on the history
// clock (L + 11 in spec 3.8) in case no header window closes.
const int32_t k_header_search_end = 11 + k_header_max_skip - 1;

// Track (spec 3.9).
const float k_end_reach = k_eot_markers + k_marker_half;  // latest position a frame needs, in slots after its STOP
const float k_phase_gain = 0.5f;
const float k_drift_gain = 0.05f;
const float k_max_drift = 1e-3f;
const float k_frame_t_tolerance = 0.02f;
const float k_frame_t_slots = static_cast<float>(k_min_data_slots + 1u);
const float k_afc_gain = 0.1f;
const float k_afc_min_q = 8.0f;
const float k_afc_clamp = 0.1f;  // cycles per slot
// A frequency step after the lock (a VFO or RIT click, drift the START/STOP AFC cannot follow) rotates the carrier
// across a STOP's halves: from about a quarter of a tone spacing kappa fails and the STOP is missed, and the START/STOP
// AFC needs detected STOPs. The rotation of a strong missed STOP measures the offset instead; two in a row that agree
// (a step, not flutter) move the NCO. A rotated marker keeps its level, |S_b - S_a| and |S_b + S_a| together, and its
// two halves stay equal. A two-path channel whose paths cancel on f_ref mimics a rotation: the delayed path's reversal
// moves into the other half, and a slow fade keeps the mimic for several frames. It is rejected by the halves' balance
// and the level, and T below 32 ms is left out: the mimic grows with delay / T (1 ms at T = 16 ms moved the NCO by
// 20..60 Hz).
const float k_rotation_energy = 16.0f;       // noise turns such a STOP by 0.25 rad at most
const float k_rotation_min_rad = 0.6f;       // below this a STOP is detected unless it is weak
const float k_rotation_span = 0.383f;        // T between the centroids of |w r| over a marker's two 0.35 T halves
const float k_rotation_agreement = 0.5f;     // of the larger estimate
const float k_rotation_gain = 0.75f;
const float k_rotation_min_slot_ms = 32.0f;
const float k_rotation_balance = 0.25f;      // |E_b - E_a| / (E_b + E_a) at most
const float k_rotation_max_level = 1.5f;     // of the reference crest
const uint8_t k_lost_absent = 3;
const float k_audit_low = -4.0f;
const float k_audit_high = 8.0f;
const float k_audit_threshold = 12.0f;
const float k_audit_purity = 0.5f;      // a flip counts against the alias audit only if it is this marker-like
const uint8_t k_strong_frames = 3;      // of the last k_presence_window frames with a full-strength inner flip
const float k_end_evidence = 10.0f;
// A steady carrier on f_ref during TRACK is the tune tone of a new transmission (the last one's EOT was missed):
// between markers f_ref is silent. Continuous (kappa <= -0.5), at half the markers' crest or more and 6 dB over the
// noise in both half windows, at k_tune_positions audit positions in a row (3 slots; noise passes one with p ~ 1e-3).
const float k_tune_kappa = -0.5f;
const float k_tune_crest = 0.5f;
const float k_tune_energy = 4.0f;
const uint8_t k_tune_positions = 6;
// Short EOT (spec 2.4): three flips in a row at slot centres j, j + 1, j + 2 of a frame. Two strong flips (the
// STOP and the first EOT marker after a full frame) already reach 16, so each of the three must be a flip: noise
// at a peak's slot centre reaches q >= 3 with probability about 1e-3.
const float k_short_eot_evidence = 15.0f;
const float k_short_eot_min = 3.0f;
const uint8_t k_short_eot_flips = 3;
const uint8_t k_short_eot_first = 2;  // a short frame holds at least one peak
const float k_reference_alpha = 0.25f;
// Train flips must keep at least this fraction of the train crest; weaker flips are noise, e.g. after a
// frame chain read as a train has ended.
const float k_train_amplitude_ratio = 0.5f;
// STOP, audit and EOT evidence count a flip only when its crest is marker-like relative to the reference.
const float k_flip_amplitude_ratio = 0.3f;
// A train marker faded by the channel (Rayleigh fading swings the crest by 10 dB within a train at T = 128 ms) still
// flips far above the noise, where the train puts it: a flip that strong, right after the last train flip and that
// close to its grid position counts down to a lower fraction of the train crest. A header or data peak leaks into
// f_ref (its nearest tone is 5 / T away), strongly at high SNR and against a crest lowered by a fade: it flips
// anywhere in the search span, and after the train it does not continue it.
const float k_faded_marker_q = 16.0f;
const float k_faded_marker_ratio = 0.2f;
const float k_faded_marker_offset = 0.05f;  // slots from the grid position
const float k_faded_marker_kappa = 0.8f;    // a clean reversal
// The slot edges next to a marker are nulls in the f_ref path, whatever the data (the peaks are on grid tones at
// least 5 / T away). A guard confirms only markers whose edges stay quiet: multipath fills them (CCIR moderate
// at T = 16 ms reaches 0.2), a flip of interference that happens to sit on the grid (two tones beating, the
// onset of a syllable) is loud there. The allowance covers the noise of the estimate at low SNR.
const float k_marker_edge_max = 0.3f;
const float k_marker_edge_sigmas = 3.0f;
// Decodability: key-down SNR for BER 1e-3 is -5.9 dB at T = 32 ms and 3 dB lower per doubling of T (spec 4.1).
// A new lock is confirmed only when its measured SNR is within k_confirm_margin_db of that.
const float k_model_slot_ms = 32.0f;
const float k_model_snr_db = -5.9f;
const float k_confirm_margin_db = 5.0f;
const uint8_t k_confirm_frames = 2;     // a fresh lock is confirmed by frame 0 or 1
const uint8_t k_guard_frames = 4;       // a late join is confirmed after 4 frames
const uint8_t k_guard_min_detected = 4; // of the guard's five markers (its START and four STOPs)
const uint8_t k_guard_min_present = 3;  // of its four frames
const uint8_t k_guard_released = 2;     // guard frames 2 and 3 are held and released
const uint8_t k_eot_clean_slot = 3;     // the slot after the EOT markers, counted from the STOP
const uint8_t k_min_tone_changes = 5;   // confident slots of a frame below which tones_vary() does not judge
const float k_weak_stop_factor = 3.0f;  // STOP evidence of a lock on a weak header, in units of k_q_track
const float k_presence = 0.5f;          // of a frame's slots confident: the frame is present
const float k_strong_presence = 0.75f;  // released without its STOP
const uint32_t k_all_slots = 0xFFFFFFFFu;

// Mode memory (spec 3.12).
const float k_mode_memory_ms = 60000.0f;
const float k_mode_memory_hz = 10.0f;
const float k_mode_memory_t = 0.03f;
const float k_memory_search_margin_hz = 50.0f;  // one tone-search bin

// Timeouts and bans.
const float k_acquire_timeout_ms = 3000.0f;
const uint8_t k_acquire_timeout_slots = 40;
const float k_alias_ban_ms = 10000.0f;
const float k_alias_ban_band = 0.05f;
const float k_search_ban_ms = 10000.0f;

// Events.
const float k_percent = 100.0f;
const float k_percent_limit = 255.0f;
const float k_noise_amplitude = 4.0f;  // E|2 S / (n g)|^2 = 4 sigma^2 n_eff / (n g)^2
const uint8_t k_nibble_bits = 4;
const uint8_t k_nibble_mask = 0x0F;
const uint8_t k_nibble_sign = 0x08;
const uint16_t k_crest_limit = 0xFFFF;

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

// A true reversal of energy E has kappa = 1 - 1/E with a spread of about sqrt(2/E): a strong "flip" far
// below that is something else (a marker of another speed smeared over a wide window, a mistuned tone).
// Capped: channels distort every marker alike (fading multipath reaches 0.85 at 30 dB, CCIR moderate),
// while the wrong readings seen stay below 0.6.
float kappa_floor(const FlipMeasure& m) {
    const float energy = flip_energy(m);
    if (energy <= 0.0f) return 0.0f;
    return min_of(1.0f - 1.0f / energy - k_kappa_spread / sqrtf(energy), k_kappa_floor_max);
}

bool is_flip(const FlipMeasure& m, float kappa_min) {
    return m.valid && m.q >= k_q_track && m.kappa >= kappa_min && m.kappa >= kappa_floor(m);
}

uint16_t grid_tones(uint8_t bits_per_peak) {
    return static_cast<uint16_t>(1u << bits_per_peak);
}

uint16_t grid_bins(uint8_t bits_per_peak) {
    const uint16_t tones = grid_tones(bits_per_peak);
    return tones > k_min_grid_bins ? tones : k_min_grid_bins;
}

uint8_t frame_bytes(uint8_t bits_per_peak, uint8_t peaks) {
    return static_cast<uint8_t>(static_cast<uint16_t>(peaks) * bits_per_peak / k_bits_per_byte);
}

int8_t soft_nibble(const uint8_t* soft, uint16_t bit) {
    const uint8_t packed = soft[bit / 2u];
    uint8_t nibble = static_cast<uint8_t>((bit & 1u) != 0 ? packed >> k_nibble_bits : packed & k_nibble_mask);
    if ((nibble & k_nibble_sign) != 0) nibble = static_cast<uint8_t>(nibble | ~k_nibble_mask);
    return static_cast<int8_t>(nibble);
}

void set_soft_nibble(uint8_t* soft, uint16_t bit, int8_t value) {
    const uint8_t nibble = static_cast<uint8_t>(static_cast<uint8_t>(value) & k_nibble_mask);
    uint8_t& packed = soft[bit / 2u];
    if ((bit & 1u) != 0) {
        packed = static_cast<uint8_t>((packed & k_nibble_mask) | (nibble << k_nibble_bits));
    } else {
        packed = static_cast<uint8_t>((packed & ~k_nibble_mask) | nibble);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// DecoderConfig
// ---------------------------------------------------------------------------

DecoderConfig::DecoderConfig()
    : min_slot_ms(k_ssb_min_slot_ms),
      min_tone_hz(k_min_tone_hz),
      max_tone_hz(k_max_tone_hz),
      impulse_blanker(true) {}

DecoderConfig DecoderConfig::for_profile(Profile profile) {
    DecoderConfig config;
    if (profile == Profile::am) config.min_slot_ms = k_am_min_slot_ms;
    if (profile == Profile::fm) {
        config.min_slot_ms = k_fm_min_slot_ms;
        config.min_tone_hz = k_fm_min_tone_hz;
    }
    return config;
}

bool DecoderConfig::valid() const {
    if (min_slot_ms < k_min_block_samples || min_slot_ms > k_max_block_samples) return false;
    if (min_tone_hz < k_min_tone_hz || min_tone_hz >= max_tone_hz || max_tone_hz > k_max_tone_hz) return false;
    if (min_slot_ms < k_small_block_samples && min_tone_hz < k_fm_min_tone_hz) return false;
    return true;
}

uint16_t DecoderConfig::max_slot_ms() const {
    return static_cast<uint16_t>(min_slot_ms * k_speed_span);
}

// ---------------------------------------------------------------------------
// Construction and public API
// ---------------------------------------------------------------------------

Decoder::Decoder(const DecoderConfig& config, EventHandler handler, void* context)
    : config_(config),
      handler_(handler),
      context_(context),
      state_(DecoderState::search),
      afc_(),
      sample_index_(0),
      end_sample_(0) {
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
    search_.configure(config_.min_tone_hz, config_.max_tone_hz);
    afc_.configure(static_cast<float>(k_decoder_rate_hz) / static_cast<float>(samples * afc_decimation_));
    afc_sum_.re = 0.0f;
    afc_sum_.im = 0.0f;
    afc_fill_ = 0;
    noise_.reset(0.0f);
    track_noise_.reset(0.0f);
    candidates_.reset();
    audit_.reset(dsp::AuditRing::k_max_positions);
    reset_scales();

    slot_blanker_.reset();
    header_bank_.reset();
    window_pending_ = false;
    window_open_ = false;
    window_blanked_ = false;
    window_index_ = 0;
    window_start_ = 0;
    window_offset_ = 0.0f;
    window_length_ = 0.0f;
    memset(header_log_, 0, sizeof(header_log_));
    for (uint8_t r = 0; r < k_header_ring; ++r) header_row_[r] = -1;
    memset(&header_pending_, 0, sizeof(header_pending_));
    header_has_pending_ = false;
    header_seen_ = false;
    header_noise_ = 0.0f;
    flip_bits_ = 0;
    last_flip_ = 0;

    memset(&mode_, 0, sizeof(mode_));
    memset(&memory_, 0, sizeof(memory_));
    drift_ = 0.0f;
    rotation_hz_ = 0.0f;
    memset(&track_start_, 0, sizeof(track_start_));
    track_frame_ = 0;
    memset(&start_before_, 0, sizeof(start_before_));
    memset(&start_after_, 0, sizeof(start_after_));
    start_halves_position_ = 0.0f;
    start_halves_valid_ = false;
    stop_missed_ = false;
    audit_next_ = 1;
    first_strong_ = 0;
    first_active_ = 0;
    eot_centres_ = 0;
    start_eot_ = 0;
    memset(eot_evidence_, 0, sizeof(eot_evidence_));
    memset(&late_, 0, sizeof(late_));
    memset(frames_, 0, sizeof(frames_));
    bank_start_ = 0.0f;
    bank_reference_ = 0.0f;
    bank_frame_ = 0;
    memset(&bank_slots_, 0, sizeof(bank_slots_));
    memset(&finished_slots_, 0, sizeof(finished_slots_));
    bank_decided_ = 0;
    byte_bits_ = 0;
    byte_flags_ = 0;
    memset(slot_crest_, 0, sizeof(slot_crest_));
    memset(held_, 0, sizeof(held_));
    held_count_ = 0;

    origin_block_ = history_.end_block();
    slot_blocks_ = k_blocks_per_min_slot;
    grid_position_ = 0.0f;
    fit_weight_ = 0.0f;
    fit_x_ = 0.0f;
    fit_y_ = 0.0f;
    fit_xx_ = 0.0f;
    fit_xy_ = 0.0f;
    grid_last_flip_ = 0;
    grid_index_ = 0;
    train_ones_ = 0;
    train_gap_ = 0;
    train_gap_count_ = 0;
    reference_average_ = 0.0f;
    guard_edge_excess_ = 0.0f;
    guard_edge_noise_ = 0.0f;
    guard_edges_ = 0;
    presence_bits_ = 0;
    strong_bits_ = 0;
    tune_run_ = 0;
    guard_markers_ = 0;
    guard_present_ = 0;
    guard_frames_ = 0;
    lock_flags_ = 0;
    confirmed_ = false;
    stop_evidence_ = 0.0f;
    weak_header_ = false;
    noise_frozen_ = false;
    tone_confirmed_ = false;
    tone_steady_ = false;
    afc_looked_ = false;
    watch_ = false;
    searching_ = true;
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
    if (old != DecoderState::search) {
        emit(make_event(EventType::state));
    }
}

void Decoder::process(const int16_t* samples, size_t count) {
    for (size_t i = 0; i < count; ++i) process_sample(samples[i]);
}

void Decoder::process_sample(int16_t sample) {
    if (block_samples_ == 0) return;
    run_slot_path(sample);
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
    ++sample_index_;
    if (++block_fill_ < block_samples_) return;
    block_fill_ = 0;
    int32_t re;
    int32_t im;
    cic_.dump(block_samples_, re, im);
    const uint32_t energy = block_energy_;
    block_energy_ = 0;
    end_sample_ = sample_index_;
    on_block(re, im, energy);
}

DecoderState Decoder::state() const {
    return state_;
}

float Decoder::tone_hz() const {
    return state_ == DecoderState::search ? 0.0f : nco_.frequency();
}

float Decoder::slot_ms() const {
    if (mode_.slot_us != 0) return static_cast<float>(mode_.slot_us) / k_us_per_ms;
    if (state_ != DecoderState::preamble && state_ != DecoderState::track) return 0.0f;
    return slot_blocks_ * static_cast<float>(block_samples_) / k_samples_per_ms;
}

// The marker crest average carries the noise of the crest measurement (its square is A^2 + N_m on average):
// it is removed, or the report reads about 0.3 dB high at the release gates.
float Decoder::snr_db() const {
    if (state_ != DecoderState::preamble && state_ != DecoderState::track) return 0.0f;
    const float crest = reference_average_ * reference_average_ - marker_noise(noise_variance(), slot_blocks_);
    const float power = crest * static_cast<float>(k_decoder_rate_hz);
    const float noise = 4.0f * k_reference_bandwidth_hz * noise_variance();
    if (power <= 0.0f) return k_snr_floor_db;
    return k_db_per_decade * log10f(power / noise);
}

uint8_t Decoder::bits_per_peak() const {
    return mode_.bits_per_peak;
}

uint8_t Decoder::data_slots() const {
    return mode_.data_slots;
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
        break;
    case DecoderState::preamble:
        run_afc(out_re, out_im);
        run_preamble();
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
    track_start_.position -= delta;
    start_halves_position_ -= delta;
    late_.start.position -= delta;
    late_.stop.position -= delta;
    bank_start_ -= delta;
}

// ---------------------------------------------------------------------------
// Slot path (spec 3.2): windows are placed on the grid before their first sample arrives.
// ---------------------------------------------------------------------------

void Decoder::run_slot_path(int16_t sample) {
    int16_t x = sample;
    uint32_t index = sample_index_;
    bool blanked = false;
    if (config_.impulse_blanker) {
        x = slot_blanker_.push(sample);
        blanked = slot_blanker_.blanked();
        index -= k_slot_blank_delay;
    }
    if (state_ != DecoderState::preamble && state_ != DecoderState::track) return;
    if (window_pending_ && static_cast<int32_t>(index - window_start_) >= 0) open_window(index);
    if (!window_open_) return;
    window_blanked_ = window_blanked_ || blanked;
    if (window_push(x)) on_window_closed();
}

// A window of length_blocks centred on grid position `centre`: its first sample n0 has u = 0.5 / L, i.e. the
// window starts half a sample before n0 at centre - L / 2.
void Decoder::schedule_window(float centre, float length_blocks, int32_t index) {
    const float block = static_cast<float>(block_samples_);
    window_length_ = length_blocks * block;
    const float start = (centre - end_position() - k_history_lag) * block - 0.5f * window_length_;
    const float first = ceilf(start);
    window_start_ = end_sample_ + static_cast<uint32_t>(static_cast<int32_t>(first));
    window_offset_ = first - start;
    window_index_ = index;
    window_pending_ = true;
    window_open_ = false;
}

void Decoder::open_window(uint32_t index) {
    const float skip = static_cast<float>(static_cast<int32_t>(index - window_start_)) + window_offset_ - 0.5f;
    window_pending_ = false;
    window_open_ = true;
    window_blanked_ = false;
    if (state_ == DecoderState::preamble) {
        open_header_window();
        header_bank_.open(window_length_, skip);
    } else {
        open_data_window();
        grid_.bank.open(window_length_, skip);
    }
}

bool Decoder::window_push(int16_t sample) {
    return state_ == DecoderState::preamble ? header_bank_.push(sample) : grid_.bank.push(sample);
}

void Decoder::on_window_closed() {
    window_open_ = false;
    if (state_ == DecoderState::preamble) {
        close_header_window(window_index_);
    } else {
        close_data_window();
    }
}

// ---------------------------------------------------------------------------
// State changes
// ---------------------------------------------------------------------------

void Decoder::set_state(DecoderState state) {
    state_blocks_ = 0;
    window_pending_ = false;
    window_open_ = false;
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
    memset(&mode_, 0, sizeof(mode_));
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

// The fine AFC shares its storage with TRACK's grid bank: it is set up again on every entry.
void Decoder::enter_acquire() {
    afc_.configure(static_cast<float>(k_decoder_rate_hz) / static_cast<float>(block_samples_ * afc_decimation_));
    afc_sum_.re = 0.0f;
    afc_sum_.im = 0.0f;
    afc_fill_ = 0;
    reset_scales();
    held_count_ = 0;
    late_.pending = false;
    memset(&mode_, 0, sizeof(mode_));
    set_state(DecoderState::acquire);
}

void Decoder::enter_preamble(float anchor, float slot_blocks, float amplitude, uint8_t hit_bits) {
    slot_blocks_ = slot_blocks;
    grid_position_ = anchor;
    seed_train_fit(anchor, slot_blocks);
    grid_last_flip_ = 0;
    grid_index_ = 0;
    last_flip_ = 0;
    flip_bits_ = hit_bits;
    // The sync's hits one T apart are steps of one already: a few faded markers after them are no T / k reading.
    train_ones_ = count_bits(static_cast<uint8_t>(hit_bits & (hit_bits >> 1)));
    train_gap_ = 0;
    train_gap_count_ = 0;
    tone_confirmed_ = true;
    reference_average_ = amplitude;  // the train's crest: the newest marker may not be measurable yet
    header_bank_.reset();
    header_bank_.set_bins(k_header_bins);
    for (uint8_t r = 0; r < k_header_ring; ++r) header_row_[r] = -1;
    header_has_pending_ = false;
    header_noise_ = 0.0f;
    header_seen_ = false;
    set_state(DecoderState::preamble);
    schedule_header_window(1);
}

// TRACK starts on a known mode. After a header (or a header-less lock whose mode memory matched) the header
// frame comes first: its STOP is searched like any other and data frame 0 follows it.
void Decoder::enter_track(const Marker& start, const Mode& mode, uint8_t flags, const dsp::HeaderDecision* header,
                          bool header_frame) {
    mode_ = mode;
    drift_ = 0.0f;
    rotation_hz_ = 0.0f;
    slot_blocks_ = exact_slot_blocks();
    watch_ = false;
    confirmed_ = false;
    stop_evidence_ = 0.0f;
    weak_header_ = false;
    lock_flags_ = flags;
    const uint16_t bins = grid_bins(mode.bits_per_peak);
    grid_.bank.reset();
    grid_.bank.set_bins(bins);
    // N_h of the header (or of this preamble's hypotheses, header-less); a late join has only the marker noise.
    const float header_noise = header != 0 ? header->noise : (header_frame ? header_noise_ : 0.0f);
    const float seed = header_noise > 0.0f ? header_noise
                                           : noise_.mean_estimate() * k_peak_energy * slot_blocks_ *
                                                 static_cast<float>(block_samples_);
    grid_.background.reset(bins, seed);
    // Grid tones 0..7 of the standard spacing are the header's tones: a carrier the header saw on one of them is
    // part of its background from the first slot.
    if (header != 0 && mode.spacing == Spacing::standard) {
        for (uint8_t h = 0; h < k_header_slots; ++h) {
            if (header->background[h] > k_header_carrier * seed) grid_.background.set_mean(h, header->background[h]);
        }
    }
    track_noise_.reset(noise_.mean_estimate());
    audit_.reset(static_cast<uint8_t>(2u * mode.data_slots + 1u));
    guard_edge_excess_ = 0.0f;
    guard_edge_noise_ = 0.0f;
    guard_edges_ = 0;
    presence_bits_ = static_cast<uint8_t>((1u << k_presence_window) - 1u);
    strong_bits_ = 0;
    tune_run_ = 0;
    guard_markers_ = start.detected ? 1 : 0;
    guard_present_ = 0;
    guard_frames_ = 0;
    held_count_ = 0;
    late_.pending = false;
    if ((flags & event_flag_late_join) != 0) reference_average_ = start.amplitude;

    track_start_ = start;
    track_frame_ = header_frame ? -1 : 0;
    stop_missed_ = false;
    start_eot_ = 0;
    begin_audit();
    start_halves_valid_ = false;
    if (start.detected) cache_start_halves(start.position);

    bank_frame_ = 0;
    bank_start_ = header_frame ? start.position + static_cast<float>(k_header_stop) * slot_blocks_ : start.position;
    set_state(DecoderState::track);
    begin_bank_frame();
    schedule_data_window(1);
}

// After a lost signal, an alias or a missing header the tone is kept: ACQUIRE again (a fade on the same tone
// re-locks, mid-stream with the mode memory; the search does not watch other tones, whose peaks would pull it
// away). ACQUIRE continues from the noise TRACK measured: its own estimate skips windows far above it, so it could
// not follow a rise of the noise that happened meanwhile.
void Decoder::lose(LostReason reason) {
    if (state_ == DecoderState::track) {
        noise_.reset(track_noise_.mean_estimate());
        remember_mode();
        if (confirmed_) tone_blocks_ = 0;
        watch_ = false;
        tone_steady_ = false;  // the held tone is the stream's f_ref now, not a tune
    }
    held_count_ = 0;
    late_.pending = false;
    Event event = make_event(EventType::lost);
    event.reason = reason;
    emit(event);
    memset(&mode_, 0, sizeof(mode_));
    enter_acquire();
}

// END: the first `bytes` bytes of the last frame (in `buffer`) follow the held frames.
void Decoder::finish(uint8_t buffer, uint8_t bytes) {
    if (confirmed_) {
        release_held();
        release_frame(buffer, bytes);
    }
    remember_mode();
    held_count_ = 0;
    late_.pending = false;
    emit(make_event(EventType::end));
    enter_search();
}

// ---------------------------------------------------------------------------
// SEARCH and ACQUIRE (spec 3.6, 3.7)
// ---------------------------------------------------------------------------

// In SEARCH: lock on the search's candidate. While watching (ACQUIRE on a tone that has shown no flips yet,
// e.g. a carrier that grabbed the search at the same time as a tune tone): move to another tone as soon as
// it turns from steady into a marker train, in time for the train's last markers. A watched tone that the
// search masks as a steady carrier (steady for longer than any tune tone) is dropped; the search keeps its
// statistics, so the mask stands and a tone next to the carrier can be taken.
void Decoder::run_search() {
    float tone = 0.0f;
    if (state_ == DecoderState::search) {
        if (!search_.candidate(tone)) return;
        tone_steady_ = search_.steady();
        // In a running stream the strongest tone is a peak's: an unsteady one inside the grid of the station heard
        // last minute brings the search back to that station's f_ref (spec 3.12). A steady tone is a tune tone
        // (another station's may lie inside that grid) and locks as usual once it has lasted longer than a peak: a
        // peak repeated on a tone looks steady for a few blocks.
        const bool inside = memory_holds(tone);
        const bool at_memory = inside && fabsf(tone - memory_.tone_hz) <= k_memory_search_margin_hz;
        if (tone_steady_ && inside && !at_memory && !search_.long_run()) return;
        if (!tone_steady_ && inside) {
            lock_tone(memory_.tone_hz);
            watch_ = false;
            tone_steady_ = false;
            afc_looked_ = true;  // the remembered f_ref was measured by the AFC already
            // A tone on f_ref itself is the station's next tune (not yet steady to the search): keep the noise.
            noise_frozen_ = at_memory;
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
        // Leaving a tune tone takes a longer steady run elsewhere: an FM receiver's noise near threshold (strongest at
        // the low end of the band after de-emphasis) can line up three steady products and a break.
        const bool preamble = state_ == DecoderState::preamble;
        const uint8_t products = preamble ? k_onset_products_preamble
                                          : (tone_steady_ ? k_onset_products_tune : k_onset_products);
        if (!search_.train_onset(tone, products)) return;
        // The tone the watch left for this train (a carrier that held ACQUIRE) is not a new transmission: a peak of
        // this one on its frequency breaks its steady phase.
        if (preamble && fabsf(tone - watch_left_hz_) <= k_memory_search_margin_hz) return;
        if ((preamble && train_line(tone)) || harmonic_image(tone)) return;
        watch_left_hz_ = nco_.frequency();
        tone_steady_ = true;  // measured on the tune tone before the train began
    }
    lock_tone(tone);
}

void Decoder::run_acquire() {
    if (state_blocks_ % k_noise_blocks == 0) push_block_noise();
    // A lock on data (no steady tune tone: a mid-stream start) can be tens of Hz off until the fine AFC has
    // had one look; a continuous pulse then rotates across a half window and reads as a flip. Weak signals
    // wait a bounded time.
    const uint32_t wait_blocks =
        static_cast<uint32_t>(k_afc_wait_ms * k_samples_per_ms / static_cast<float>(block_samples_));
    const bool tuned = tone_steady_ || tone_confirmed_ || afc_looked_ || state_blocks_ >= wait_blocks;
    run_candidates(tuned);
    if (state_ != DecoderState::acquire) return;
    // Counted from the tone's lock: PREAMBLEs that found no header (a fade over the train, data peaks passing for a
    // train on a peak's tone) do not extend it, or a lock on a peak would outlast the next transmission's tune.
    float timeout_ms = max_of(k_acquire_timeout_ms, k_acquire_timeout_slots * static_cast<float>(config_.max_slot_ms()));
    if (stream_relock()) {
        const float frame_ms = static_cast<float>((memory_.mode.data_slots + 1u) * memory_.mode.slot_us) / k_us_per_ms;
        timeout_ms = max_of(timeout_ms, static_cast<float>(k_stream_timeout_frames) * frame_ms);
    }
    const uint32_t timeout_blocks =
        static_cast<uint32_t>(timeout_ms * k_samples_per_ms / static_cast<float>(block_samples_));
    if (tone_blocks_ < timeout_blocks) return;
    // The f_ref of the station heard last minute is not banned: its next tune comes there. Nor is a tone gone quiet (a
    // tune whose train was missed): only one still there (a carrier, keyed CW, speech, a peak of a stream) holds the
    // search back.
    const bool remembered = memory_valid() && fabsf(nco_.frequency() - memory_.tone_hz) <= k_memory_search_margin_hz;
    if (!tone_confirmed_ && !remembered && search_.present(nco_.frequency())) {
        const float base = k_search_ban_ms * k_samples_per_ms / static_cast<float>(dsp::ToneSearch::k_block_samples);
        search_.ban(nco_.frequency(), static_cast<uint16_t>(base));
    }
    enter_search();
}

// ACQUIRE on a tone from the search, and PREAMBLE on it while its train runs (the flips go on at L and L + 1): peaks
// of a stream (the tone may be one) pass for a train, and the PREAMBLE they start must not hide a new transmission's
// tune and train on another tone. From the header on the watch stops: a header peak at T >= 64 ms lasts long enough
// to look like a tune.
bool Decoder::watching() const {
    if (!watch_) return false;
    return state_ != DecoderState::preamble || grid_index_ <= grid_last_flip_ + 1;
}

// A line (2 m + 1) / (2 T) of this preamble's own train.
bool Decoder::train_line(float tone_hz) const {
    const float slot_s = slot_blocks_ * static_cast<float>(block_samples_) / static_cast<float>(k_decoder_rate_hz);
    const float orders = fabsf(tone_hz - nco_.frequency()) * slot_s - 0.5f;  // m + fraction, in 1 / T
    const float order = floorf(orders + 0.5f);
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

void Decoder::run_afc(int32_t re, int32_t im) {
    afc_sum_.re += static_cast<float>(re);
    afc_sum_.im += static_cast<float>(im);
    if (++afc_fill_ >= afc_decimation_) {
        const float scale = 1.0f / (k_mixer_gain * static_cast<float>(block_samples_ * afc_decimation_));
        Complex decimated;
        decimated.re = afc_sum_.re * scale;
        decimated.im = afc_sum_.im * scale;
        afc_.push(decimated);
        afc_sum_.re = 0.0f;
        afc_sum_.im = 0.0f;
        afc_fill_ = 0;
    } else {
        return;
    }
    // First look after k_afc_min_inputs inputs, then every k_afc_eval_ms.
    const uint16_t inputs = afc_.inputs();
    const uint16_t period = static_cast<uint16_t>(k_afc_eval_ms * k_samples_per_ms /
                                                  static_cast<float>(block_samples_ * afc_decimation_));
    if (inputs < k_afc_min_inputs || (inputs - k_afc_min_inputs) % period != 0) return;
    // A running stream on the remembered f_ref: the AFC measured it already, and the stream's peaks next to it (39 Hz
    // below it at T = 128 ms, where f_ref carries one marker in N + 1 slots) would pull it off.
    if (stream_relock()) return;
    float offset = 0.0f;
    if (!afc_.offset(offset, !tone_steady_)) return;
    // The train that started PREAMBLE held its flips over half windows of 0.35 T: the tone is within a
    // fraction of 1 / T. A larger correction there is a noise peak (or a header peak's product) that would walk
    // the lock off the tone.
    const float slot_s = slot_blocks_ * static_cast<float>(block_samples_) / static_cast<float>(k_decoder_rate_hz);
    if (state_ == DecoderState::preamble && fabsf(offset) * slot_s > k_preamble_afc_limit) return;
    const bool first_look = !afc_looked_;
    afc_looked_ = true;
    if (fabsf(offset) >= k_afc_min_offset_hz) {
        nco_.adjust_frequency(offset);
        afc_.reset();
        // Windows mixed before a large first correction rotate across a marker half window and read as
        // flips: a lock on data (no tune tone) can start 25 Hz off. In ACQUIRE they are measured again at
        // the new tone. A tune tone lock is a few Hz off at most and may already hold part of the train.
        const float slowest_s = static_cast<float>(config_.max_slot_ms()) / k_ms_per_s;
        const float rotation = k_two_pi * fabsf(offset) * k_marker_half * slowest_s;
        if (first_look && !tone_steady_ && state_ == DecoderState::acquire && rotation > k_afc_reset_rotation) {
            forget_history();
        }
    }
}

void Decoder::forget_history() {
    history_.reset();
    origin_block_ = history_.end_block();
    settle_blocks_ = k_settle_blocks;
    candidates_.reset();
    reset_scales();
}

// Only a lock on a running stream (the mode memory's f_ref) measures its noise here: between its markers f_ref is
// quiet. On a tone from the search f_ref holds the tune and then the train until PREAMBLE, and a weak tune (5 dB over
// the noise in a window at the hf_weak gate, before the search calls it steady) passes the outlier test and drags the
// 25 % point up by 3 dB: the floor measured before the tone appeared (lock_tone) is kept, as is TRACK's after LOST.
void Decoder::push_block_noise() {
    if (noise_frozen_) return;
    const float end = end_position();
    const float from = end - static_cast<float>(k_noise_blocks);
    Complex sum;
    if (!history_.window(origin_block_, from, end, sum) || history_.any_blanked(origin_block_, from, end)) return;
    const float power = magnitude_squared(sum) / dsp::noise_samples(k_noise_blocks, block_samples_);
    // A strong tune tone fills every window: it would drag the 25 % point up by 1/64 per push.
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
        if (!candidates_.add(candidate) || !tuned) continue;
        if (try_sync(candidate) || try_late_join(candidate)) return;
    }
}

bool Decoder::try_sync(const Candidate& candidate) {
    const float centre = candidate_position(candidate);
    float tried[k_max_hypotheses];
    uint8_t tried_count = 0;
    float accepted_slot[k_max_accepted];
    float accepted_evidence[k_max_accepted];
    bool accepted_strong[k_max_accepted];
    uint8_t accepted_count = 0;

    for (uint8_t age = 1; age < candidates_.count(); ++age) {
        const float distance = centre - candidate_position(candidates_.newest(age));
        if (distance <= 0.0f) continue;
        for (uint8_t m = 1; m < k_sync_positions; ++m) {
            const float slot = distance / static_cast<float>(m);
            if (!in_range(slot) || banned(slot)) continue;
            bool seen = false;
            for (uint8_t i = 0; i < tried_count && !seen; ++i) {
                seen = fabsf(slot - tried[i]) <= k_hypothesis_merge * tried[i];
            }
            if (seen) continue;
            if (tried_count < k_max_hypotheses) tried[tried_count++] = slot;

            const SyncScore score = sync_score(centre, slot);
            if (score.evidence < k_sync_evidence || !enough_hits(score)) continue;
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
            accepted_strong[index] = strong_hits(score);
        }
    }
    if (accepted_count == 0) return false;
    // A reading taken on weak markers yields to one on hits alone within 10 % of its T: both then fit the same markers,
    // and the weak markers' evidence favours a T a few % off (the position search spans +-0.1 T).
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
    // A long train also supports 2T, 3T...: prefer the smallest integer sub-multiple of the best with
    // comparable evidence (a non-harmonic smaller T is a different, weaker reading).
    float slot = accepted_slot[best];
    for (uint8_t i = 0; i < accepted_count; ++i) {
        if (accepted_evidence[i] < k_prefer_smaller_t * accepted_evidence[best] || accepted_slot[i] >= slot) continue;
        const float ratio = accepted_slot[best] / accepted_slot[i];
        const float whole = floorf(ratio + 0.5f);
        if (whole >= 2.0f && fabsf(ratio - whole) <= k_hypothesis_merge * whole) slot = accepted_slot[i];
    }
    // A train at 2T read at T/2 has its markers at the even positions only (the odd ones are the
    // low-energy boundaries between markers, where noise may pass for a weak marker): then prefer the double while it
    // has more evidence.
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
    if (!in_range(slot)) return false;  // the least-squares T may leave the range: a sender we must not hear
    // The remembered station's stream: its peaks' leaks read as trains of other T.
    if (stream_relock() && fabsf(slot / mode_slot_blocks(memory_.mode) - 1.0f) > k_refine_max_change) return false;
    enter_preamble(anchor, slot, chosen.amplitude, chosen.marker_bits);
    return true;
}

bool Decoder::strong_hits(const SyncScore& score) {
    if (score.hits >= k_sync_min_hits) return true;
    const uint8_t dense = static_cast<uint8_t>(score.hit_bits & k_sync_dense_mask);
    const uint8_t odd = count_bits(dense & k_sync_odd_mask);
    const uint8_t even = static_cast<uint8_t>(count_bits(dense) - odd);
    return count_bits(dense) >= k_sync_dense_hits && odd >= k_sync_min_parity_hits && even >= k_sync_min_parity_hits;
}

bool Decoder::enough_hits(const SyncScore& score) {
    if (strong_hits(score)) return true;
    const uint8_t markers = count_bits(score.marker_bits);
    const uint8_t odd_markers = count_bits(score.marker_bits & k_sync_odd_mask);
    const uint8_t odd_hits = count_bits(score.hit_bits & k_sync_odd_mask);
    return score.hits >= k_sync_min_strong && odd_hits >= k_sync_min_parity_strong &&
           score.hits - odd_hits >= k_sync_min_parity_strong && markers >= k_sync_min_hits &&
           markers + k_sync_missing_markers >= score.positions && odd_markers >= k_sync_min_parity_hits &&
           markers - odd_markers >= k_sync_min_parity_hits;
}

// Evidence of a marker train of period slot_blocks ending at centre. The best prefix of at least
// k_sync_min_positions positions counts, so a train whose older positions still fall in the tune tone (strongly
// anti-flip) is judged on the markers it already has.
Decoder::SyncScore Decoder::sync_score(float centre, float slot_blocks) const {
    const float span = k_position_search * slot_blocks;
    const float half = k_marker_half * slot_blocks;
    FlipMeasure measures[k_sync_positions];
    float strongest = 0.0f;
    for (uint8_t i = 0; i < k_sync_positions; ++i) {
        measures[i] = search_flip(centre - static_cast<float>(i) * slot_blocks, span, half, k_kappa_track);
        if (is_flip(measures[i], k_kappa_track)) strongest = max_of(strongest, measures[i].amplitude);
    }
    SyncScore best;
    best.evidence = k_no_value;
    best.amplitude = 0.0f;
    best.boundary = 0.0f;
    best.hit_bits = 0;
    best.marker_bits = 0;
    best.hits = 0;
    best.positions = 0;
    // Between two train markers the carrier is continuous. A tone far off the NCO rotates in every
    // window and reads as a flip everywhere, midpoints included: that is not a train.
    uint8_t midpoint_flips = 0;
    for (uint8_t i = 0; i < k_sync_midpoints; ++i) {
        const FlipMeasure m = measure_flip(centre - (static_cast<float>(i) + k_slot_centre) * slot_blocks, half);
        if (is_flip(m, k_kappa_track)) ++midpoint_flips;
    }
    if (midpoint_flips >= k_sync_max_midpoint_flips) return best;
    bool hit[k_sync_positions];
    bool marker[k_sync_positions];
    for (uint8_t i = 0; i < k_sync_positions; ++i) {
        const FlipMeasure& m = measures[i];
        hit[i] = is_flip(m, k_kappa_track) && m.amplitude >= k_train_amplitude_ratio * strongest;
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
    if (best.evidence >= k_sync_evidence && enough_hits(best)) {
        float allowance = 0.0f;
        best.boundary = boundary_energy(measures, marker, slot_blocks, best.amplitude, allowance);
        if (best.boundary > k_boundary_max + allowance) best.evidence = k_no_value;
    }
    return best;
}

// Energy in +-k_boundary_half slots around a slot boundary, less the noise, over the energy a tone of the
// given crest has there (a real boundary: 0.015); noise_ratio receives the noise's share on the same scale.
bool Decoder::boundary_excess(float boundary, float slot_blocks, float crest, float& excess, float& noise_ratio) const {
    const float half = k_boundary_half * slot_blocks;
    const float full = 0.5f * crest * 2.0f * half * static_cast<float>(block_samples_);
    Complex sum;
    if (full <= 0.0f || !history_.window(origin_block_, boundary - half, boundary + half, sum)) return false;
    const float noise = noise_variance() * dsp::noise_samples(2.0f * half, block_samples_);
    excess = (magnitude_squared(sum) - noise) / (full * full);
    noise_ratio = noise / (full * full);
    return true;
}

// Mean boundary_excess() between consecutive markers of a train; 0 when no two markers are neighbours. `allowance`
// receives k_marker_edge_sigmas standard deviations of that mean on noise alone: at the gates the noise in the
// 0.2 T boundary windows alone reaches the 0.25 limit (hf_fast at -1.5 dB).
float Decoder::boundary_energy(const FlipMeasure* measures, const bool* marker, float slot_blocks, float crest,
                               float& allowance) const {
    float total = 0.0f;
    float noise = 0.0f;
    uint8_t pairs = 0;
    for (uint8_t i = 0; i + 1 < k_sync_positions; ++i) {
        if (!marker[i] || !marker[i + 1]) continue;
        float excess = 0.0f;
        float noise_ratio = 0.0f;
        if (!boundary_excess(0.5f * (measures[i].position + measures[i + 1].position), slot_blocks, crest, excess,
                             noise_ratio)) {
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
        for (uint8_t i = 0; i < k_sync_positions; ++i) {
            const FlipMeasure m = search_flip(centre - static_cast<float>(i) * slot_blocks, span, half, k_kappa_track);
            if (!is_flip(m, k_kappa_track)) continue;
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

// Late join (v0.2: with mode memory only). A frame chain has one marker every (N + 1) T: three equal
// intervals of the remembered period (within 3 %) behind the newest candidate.
bool Decoder::try_late_join(const Candidate& candidate) {
    if (!memory_usable()) return false;
    const float memory_slot = mode_slot_blocks(memory_.mode);
    const float frame = static_cast<float>(memory_.mode.data_slots + 1u);
    const float centre = candidate_position(candidate);
    float best = 0.0f;
    for (uint8_t age = 1; age < candidates_.count() && best <= 0.0f; ++age) {
        const float distance = centre - candidate_position(candidates_.newest(age));
        if (distance <= 0.0f) continue;
        for (uint8_t k = 1; k <= k_late_join_intervals; ++k) {
            const float slot = distance / (frame * static_cast<float>(k));
            if (fabsf(slot / memory_slot - 1.0f) > k_mode_memory_t || banned(slot)) continue;
            uint8_t hits = 0;
            for (uint8_t j = 1; j <= k_late_join_intervals; ++j) {
                if (has_candidate_near(centre - frame * static_cast<float>(j) * slot, k_position_search * slot)) ++hits;
            }
            if (hits == k_late_join_intervals) {
                best = slot;
                break;
            }
        }
    }
    if (best <= 0.0f) return false;

    const FlipMeasure m = search_flip(centre, k_position_search * best, k_marker_half * best, k_kappa_track);
    Marker start;
    start.position = is_flip(m, k_kappa_track) ? m.position : centre;
    start.amplitude = m.amplitude;
    start.detected = true;
    enter_track(start, memory_.mode, event_flag_late_join | event_flag_mode_memory, 0, false);
    return true;
}

bool Decoder::has_candidate_near(float position, float tolerance) const {
    for (uint8_t age = 0; age < candidates_.count(); ++age) {
        const Candidate& c = candidates_.newest(age);
        if (c.q >= k_late_join_q && fabsf(candidate_position(c) - position) <= tolerance) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// PREAMBLE: the sync train (spec 3.8), on the history clock
// ---------------------------------------------------------------------------

void Decoder::run_preamble() {
    while (state_ == DecoderState::preamble && preamble_step()) {
    }
}

bool Decoder::preamble_step() {
    const float slot = slot_blocks_;
    const int32_t g = grid_index_ + 1;
    const float position = grid_centre(g);
    // Nine slots after the last train marker (the header STOP) a T error of 2 % (a sync on few markers) has
    // moved it by 0.18 T: search it wider. Header peaks are off f_ref, so only noise competes there.
    const bool header_stop = g - grid_last_flip_ >= k_header_stop;
    const float span = (header_stop ? k_first_stop_search : k_position_search) * slot;
    const float half = k_marker_half * slot;
    if (position + span + half + 1.0f > end_position()) return false;
    grid_index_ = g;

    const FlipMeasure m = search_flip(position, span, half, k_kappa_track);
    const float ratio = header_stop ? k_flip_amplitude_ratio : k_train_amplitude_ratio;
    const bool faded = g == grid_last_flip_ + 1 && m.q >= k_faded_marker_q && m.kappa >= k_faded_marker_kappa &&
                       m.amplitude >= k_faded_marker_ratio * reference_average_ &&
                       fabsf(m.position - position) <= k_faded_marker_offset * slot;
    const bool flip = is_flip(m, k_kappa_track) && (m.amplitude >= ratio * reference_average_ || faded);
    flip_bits_ = (flip_bits_ << 1) | (flip ? 1u : 0u);
    if (flip) {
        const int32_t gap = g - last_flip_;
        last_flip_ = g;
        if (gap <= k_train_max_gap) continue_train(g, m);
        if (state_ != DecoderState::preamble) return false;
    }
    if (g > k_preamble_max_slots) {
        lose(LostReason::preamble_timeout);
        return false;
    }
    if (g - grid_last_flip_ > k_header_search_end) {
        header_timeout();
        return false;
    }
    return true;
}

// A flip within k_train_max_gap of the previous one: the train goes on, L = g. Header peaks never flip, so a
// lone noise flip in the header does not move the grid.
void Decoder::continue_train(int32_t g, const FlipMeasure& m) {
    const int32_t gap = g - grid_last_flip_;
    const float index = static_cast<float>(g);
    add_train_point(index, m);
    float position = m.position;
    float period = slot_blocks_ + k_train_nudge * ((m.position - grid_position_) / static_cast<float>(gap) - slot_blocks_);
    float fitted_slot = 0.0f;
    float fitted_position = 0.0f;
    if (train_fit(index, fitted_slot, fitted_position) && fabsf(fitted_slot - slot_blocks_) <= k_refine_max_change * slot_blocks_) {
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
    grid_last_flip_ = g;
    update_reference(m.amplitude);
    const uint8_t multiple = sub_rate_train(gap);
    if (multiple <= 1) return;
    // Flips only every `multiple` indices: the grid is that much too fine (a 2T train read at T/2). Out of range,
    // the gaps are faded markers of this T (at T_max near the gate); the header decides.
    float slot = static_cast<float>(multiple) * slot_blocks_;
    if (!in_range(slot)) {
        train_gap_count_ = 0;
        return;
    }
    // The fine reading's error is multiplied too: fit the train again at the new period.
    float anchor = grid_position_;
    refine_sync(anchor, slot);
    if (!in_range(slot)) slot = static_cast<float>(multiple) * slot_blocks_;
    slot_blocks_ = slot;
    grid_position_ = anchor;
    seed_train_fit(anchor, slot);
    grid_index_ = 0;
    grid_last_flip_ = 0;
    last_flip_ = 0;
    flip_bits_ = 1;
    train_ones_ = 0;
    train_gap_ = 0;
    train_gap_count_ = 0;
    for (uint8_t r = 0; r < k_header_ring; ++r) header_row_[r] = -1;
    header_has_pending_ = false;
    header_seen_ = false;
    schedule_header_window(1);
}

// The train's markers at grid indices 0, -1, ... -7 before `anchor` (the sync's positions) start the fit. A marker
// found at the edge of the sync's +-0.1 T is searched again over +-0.25 T: the sync's T may be 5 % off (a few markers
// at CCIR poor), which moves the oldest markers beyond +-0.1 T, and the edge would hold the fit at the sync's T.
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

void Decoder::add_train_point(float index, const FlipMeasure& m) {
    const float weight = clamp(m.q, k_fit_min_weight, k_evidence_clip);
    fit_weight_ += weight;
    fit_x_ += weight * index;
    fit_y_ += weight * m.position;
    fit_xx_ += weight * index * index;
    fit_xy_ += weight * index * m.position;
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

// A real train flips at every index (a faded marker leaves an occasional longer gap). Returns k > 1 when
// the flips came k indices apart at least k_sub_rate_gaps times and at least twice as often as one apart
// (a lone step of one is the anchor or a noise flip between the markers of a T/k reading).
uint8_t Decoder::sub_rate_train(int32_t gap) {
    if (gap == 1) {
        if (train_ones_ < k_count_limit_u8) ++train_ones_;
        return 1;
    }
    if (gap != train_gap_) {
        train_gap_ = static_cast<uint8_t>(gap);
        train_gap_count_ = 0;
    }
    if (train_gap_count_ < k_count_limit_u8) ++train_gap_count_;
    const bool sub_rate = train_gap_count_ >= k_sub_rate_gaps && train_gap_count_ >= k_sub_rate_ratio * train_ones_;
    return sub_rate ? train_gap_ : 1;
}

bool Decoder::flip_at(int32_t g) const {
    const int32_t age = grid_index_ - g;
    if (age < 0 || age >= static_cast<int32_t>(sizeof(flip_bits_) * k_bits_per_byte)) return false;
    return ((flip_bits_ >> age) & 1u) != 0;
}

float Decoder::grid_centre(int32_t g) const {
    return grid_position_ + static_cast<float>(g - grid_last_flip_) * slot_blocks_;
}

// ---------------------------------------------------------------------------
// PREAMBLE: the mode header (spec 3.8), on the sample clock
// ---------------------------------------------------------------------------

void Decoder::schedule_header_window(int32_t g) {
    schedule_window(grid_centre(g), slot_blocks_, g);
}

// 8 tones on each side of f_ref at the train's T; coefficients follow the AFC.
void Decoder::open_header_window() {
    const float slot_s = slot_blocks_ * static_cast<float>(block_samples_) / static_cast<float>(k_decoder_rate_hz);
    const float tone = nco_.frequency();
    for (uint8_t side = 0; side < 2; ++side) {
        const float sign = side == 0 ? 1.0f : -1.0f;
        for (uint8_t h = 0; h < k_header_slots; ++h) {
            const float offset = (static_cast<float>(k_grid_guard) +
                                  static_cast<float>(h * k_standard_spacing_num) / k_standard_spacing_den) / slot_s;
            header_bank_.set_frequency(static_cast<uint16_t>(side * k_header_slots + h), tone + sign * offset);
        }
    }
}

void Decoder::close_header_window(int32_t g) {
    const uint8_t row = static_cast<uint8_t>(static_cast<uint32_t>(g) % k_header_ring);
    for (uint16_t b = 0; b < k_header_bins; ++b) header_log_[row][b] = dsp::log2_q8(header_bank_.energy(b));
    header_row_[row] = g;
    header_step(g);
    if (state_ == DecoderState::preamble && !window_pending_) schedule_header_window(g + 1);
}

// At the close of window g: H(g - 8) (peaks g - 7..g) is complete. An accepted hypothesis waits one slot for
// the look-ahead H(s + 1); its STOP is then the slot just closed, and data frame 0 starts with the next window.
void Decoder::header_step(int32_t g) {
    const int32_t start = g - k_header_slots;
    float energy[2][k_header_slots][k_header_slots];
    const bool rows = header_energy(start, energy);
    HeaderScore score;
    const bool accepted = rows && try_header(start, energy, score);
    if (header_has_pending_) {
        if (accepted && score.decision.margin > header_pending_.decision.margin) {
            header_pending_ = score;
            return;
        }
        // An H(s) before L competes with every hypothesis up to H(L + 1): one misaligned by 2 slots can pass.
        if (header_pending_.start < grid_last_flip_ && start <= grid_last_flip_ &&
            header_candidate(header_pending_.start)) {
            return;
        }
        header_has_pending_ = false;
        // Train flips after its START that the window did not hold yet (the train went on): not a header.
        if (header_candidate(header_pending_.start)) {
            commit_header(header_pending_);
            return;
        }
    }
    if (accepted) {
        header_pending_ = score;
        header_has_pending_ = true;
        return;
    }
    // No header on H(s). A station heard in the last minute: its remembered word in 6 of the 8 slots gives the mode
    // and the slot phase (a faint header; or one after faded train markers, read on a later H(s)).
    if (rows && memory_fits()) {
        const Mode& mode = memory_.mode;
        const uint16_t word = header_word(mode.bits_per_peak, mode.data_slots, mode.slot_us, mode.spacing);
        const dsp::HeaderMatch match = dsp::match_header(energy, word, mode.side);
        if (match.peaks >= k_header_min_peaks) header_seen_ = true;
        if (match.agreement >= k_header_agree) {
            enter_track(header_start(start), mode, event_flag_mode_memory, 0, true);
            weak_header_ = match.agreement == k_header_agree;
            return;
        }
    }
    // No peaks in H(L) or H(L + 1) (the whole header faded) and no marker after L (a weak train whose last markers
    // were missed goes on there): the memory gives the mode on the train's phase, while data frame 0 is still ahead.
    // Like any fresh lock, it is confirmed only once a STOP is found where it expects them.
    if (start == grid_last_flip_ + 1 && !header_seen_ && memory_fits() && train_ended(grid_last_flip_)) {
        enter_track(header_start(grid_last_flip_), memory_.mode, event_flag_mode_memory, 0, true);
        weak_header_ = true;
        return;
    }
    if (start >= grid_last_flip_ + k_header_max_skip) header_timeout();
}

// H(s): START at s = L (the last train flip), a train position after it whose marker faded, or up to
// k_train_max_gap before it: a noise flip on one of the first header peaks continues the train by one step and moves
// L onto a peak. Its peaks s + 1..s + 8 hold at most one detected flip (that noise flip), so a train that went on
// through the "header" is refused. A window misaligned by 2 slots still matches 6 slots of some codeword: an H(s)
// before L is committed only after H(L) and H(L + 1) had their chance (header_step).
bool Decoder::header_candidate(int32_t start) const {
    if (start < grid_last_flip_ - k_train_max_gap || start > grid_last_flip_ + k_header_max_skip) return false;
    uint8_t inner = 0;
    for (int32_t g = start + 1; g <= start + k_header_slots; ++g) inner = static_cast<uint8_t>(inner + (flip_at(g) ? 1u : 0u));
    return inner <= k_header_max_inner_flips;
}

// Fills the energies of H(s)'s slots.
bool Decoder::header_energy(int32_t start, float (&energy)[2][k_header_slots][k_header_slots]) const {
    if (!header_candidate(start)) return false;
    for (uint8_t j = 0; j < k_header_slots; ++j) {
        const int32_t g = start + 1 + j;
        const uint8_t row = static_cast<uint8_t>(static_cast<uint32_t>(g) % k_header_ring);
        if (header_row_[row] != g) return false;
        for (uint8_t side = 0; side < 2; ++side) {
            for (uint8_t h = 0; h < k_header_slots; ++h) {
                energy[side][j][h] = dsp::exp2_q8(header_log_[row][side * k_header_slots + h]);
            }
        }
    }
    return true;
}

// No marker at the two train positions after `last`: their flip statistic stays at the noise's (q < 1).
bool Decoder::train_ended(int32_t last) const {
    const float half = k_marker_half * slot_blocks_;
    for (int32_t g = last + 1; g <= last + k_train_max_gap; ++g) {
        const FlipMeasure m = measure_flip(grid_centre(g), half);
        if (!m.valid || m.q >= k_q_present) return false;
    }
    return true;
}

bool Decoder::try_header(int32_t start, const float (&energy)[2][k_header_slots][k_header_slots], HeaderScore& score) {
    score.decision = dsp::decide_header(energy);
    score.start = start;
    header_noise_ = score.decision.noise;
    return score.decision.accepted && header_plausible(score.decision);
}

// Every tone a sender puts out lies in 300..2700 Hz (spec 1.3): a decoded header whose grid, taken from f_ref as
// received, leaves that band by more than k_band_margin_hz is not a header (data peaks read as one); nor is one whose
// exact T is more than k_snap_tolerance from the train's.
bool Decoder::header_plausible(const dsp::HeaderDecision& decision) const {
    Mode mode;
    LostReason refusal = LostReason::none;
    if (!mode_from_header(decision, mode, refusal)) return true;  // refused later as unsupported_mode
    const float train_ms = slot_blocks_ * static_cast<float>(block_samples_) / k_samples_per_ms;
    if (fabsf(static_cast<float>(mode.slot_us) / (k_us_per_ms * train_ms) - 1.0f) > k_snap_tolerance) return false;
    const float slot_s = static_cast<float>(mode.slot_us) / (k_us_per_ms * k_ms_per_s);
    const float spacing = mode.spacing == Spacing::standard
                              ? static_cast<float>(k_standard_spacing_num) / static_cast<float>(k_standard_spacing_den)
                              : 1.0f;
    const float data_span = static_cast<float>(k_grid_guard) + static_cast<float>(grid_tones(mode.bits_per_peak) - 1u) * spacing;
    const float header_span = static_cast<float>(k_grid_guard + k_header_slots);
    const float span = (data_span > header_span ? data_span : header_span) / slot_s;
    const float tone = nco_.frequency();
    const float far = tone + static_cast<float>(mode.side) * span;
    const float low = min_of(tone, far);
    const float high = max_of(tone, far);
    return low >= static_cast<float>(k_min_tone_hz) - k_band_margin_hz && high <= static_cast<float>(k_max_tone_hz) + k_band_margin_hz;
}

void Decoder::commit_header(const HeaderScore& score) {
    Mode mode;
    LostReason refusal = LostReason::none;
    if (!mode_from_header(score.decision, mode, refusal)) {
        ban_alias(slot_blocks_);
        lose(refusal);
        return;
    }
    enter_track(header_start(score.start), mode, 0, &score.decision, true);
    weak_header_ = score.decision.agreement <= k_header_agree;
}

// The header's fields against the caps and the profile; T snapped to the whole ms of the header's residue
// nearest the train's T (spec 3.8 step 5).
bool Decoder::mode_from_header(const dsp::HeaderDecision& decision, Mode& mode, LostReason& refusal) const {
    refusal = LostReason::unsupported_mode;
    const HeaderFields fields = header_fields(decision.word);
    if (fields.data_slots == 0 || fields.bits_per_peak > UNLIMITED_MAX_BITS_PER_PEAK) return false;
    if (frame_bytes(fields.bits_per_peak, fields.data_slots) > UNLIMITED_MAX_FRAME_BYTES) return false;
    const float train_ms = slot_blocks_ * static_cast<float>(block_samples_) / k_samples_per_ms;
    const float residue = static_cast<float>(fields.slot_ms_residue);
    const float modulo = static_cast<float>(k_header_slot_ms_modulo);
    int32_t slot_ms = round_to_int(floorf((train_ms - residue) / modulo + 0.5f) * modulo + residue);
    const int32_t min_ms = config_.min_slot_ms > k_min_slot_us / k_slot_quantum_us ? config_.min_slot_ms
                                                                                    : k_min_slot_us / k_slot_quantum_us;
    const int32_t max_ms = config_.max_slot_ms();
    // At an end of the range the train's T may be nearer the value just outside it: the one inside is the sender's.
    const int32_t step = k_header_slot_ms_modulo;
    if (slot_ms > max_ms && slot_ms - step >= min_ms && train_ms - static_cast<float>(slot_ms - step) <= k_snap_reach_ms) slot_ms -= step;
    if (slot_ms < min_ms && slot_ms + step <= max_ms && static_cast<float>(slot_ms + step) - train_ms <= k_snap_reach_ms) slot_ms += step;
    if (slot_ms < min_ms || slot_ms > max_ms) return false;
    const uint32_t slot_us = static_cast<uint32_t>(slot_ms) * k_slot_quantum_us;
    if ((fields.data_slots + 1u) * slot_us > k_max_frame_us) return false;
    mode.slot_us = slot_us;
    mode.bits_per_peak = fields.bits_per_peak;
    mode.data_slots = fields.data_slots;
    mode.spacing = fields.spacing;
    mode.side = decision.side;
    return true;
}

Decoder::Marker Decoder::header_start(int32_t g) const {
    Marker start;
    start.position = grid_centre(g);
    start.detected = flip_at(g);
    start.amplitude = measure_flip(start.position, k_marker_half * slot_blocks_).amplitude;
    return start;
}

// A header-less lock may take the remembered mode (a faded header of a station heard in the last minute): same
// f_ref within 10 Hz and T of the train within 3 %.
bool Decoder::memory_fits() const {
    return memory_usable() && fabsf(slot_blocks_ / mode_slot_blocks(memory_.mode) - 1.0f) <= k_mode_memory_t;
}

// No header: lost(no_header), then ACQUIRE on the same tone with its candidates. A tone inside the remembered grid
// but away from its f_ref was a peak (one repeated on a tone can look steady to the search): the remembered f_ref
// is tried instead.
void Decoder::header_timeout() {
    const float tone = nco_.frequency();
    lose(LostReason::no_header);
    if (state_ == DecoderState::acquire && memory_holds(tone) && fabsf(tone - memory_.tone_hz) > k_memory_search_margin_hz) {
        lock_tone(memory_.tone_hz);
        watch_ = false;
        tone_steady_ = false;
        afc_looked_ = true;
        noise_frozen_ = false;
    }
}

// ---------------------------------------------------------------------------
// TRACK (spec 3.9): history clock
// ---------------------------------------------------------------------------

// Frame e: audit and short-EOT evidence -> early step (its STOP) -> late step (END, alias, LOST, release) -> audit
// of frame e + 1 -> ... A short final frame ends as soon as its STOP and EOT triple is measured.
void Decoder::run_track() {
    const uint8_t n = mode_.data_slots;
    while (state_ == DecoderState::track) {
        uint8_t stop = 0;
        if (late_.pending) {
            while (eot_centres_ < n + k_short_eot_flips && measure_eot(static_cast<uint8_t>(eot_centres_ + 1u))) {
                if (!short_end(stop)) continue;
                end_short(late_.slots, late_.first_strong, late_.start, stop);
                return;
            }
            if (!late_ready()) return;
            track_late();
            continue;
        }
        if (track_frame_ >= 0) {
            while (audit_step()) {
            }
            if (state_ != DecoderState::track) return;
            if (audit_next_ > audit_positions() && eot_centres_ < n + 1u) measure_eot(static_cast<uint8_t>(n + 1u));
            if (short_end(stop)) {
                end_short(bank_frame_ == static_cast<uint32_t>(track_frame_) ? bank_slots_ : finished_slots_, first_strong_,
                          track_start_, stop);
                return;
            }
            if (eot_centres_ < n + 1u) return;
        }
        if (!early_ready()) return;
        track_early();
    }
}

float Decoder::exact_slot_blocks() const {
    return mode_slot_blocks(mode_) * (1.0f + drift_);
}

uint8_t Decoder::audit_positions() const {
    return static_cast<uint8_t>(2u * mode_.data_slots + 1u);
}

// The START's crest, else the running reference.
float Decoder::frame_reference(const Marker& start) const {
    return start.detected && start.amplitude > 0.0f ? start.amplitude : reference_average_;
}

// Audit position j of frame track_frame_ at S + j T / 2 (slot centres and boundaries), once its history is in
// and the peaks under its half windows are decided. The purity gate: a data peak leaks into the f_ref windows,
// so a flip counts only when its crest reaches k_audit_purity of theirs (spec 3.11).
bool Decoder::audit_step() {
    if (track_frame_ < 0 || audit_next_ > audit_positions()) return false;
    const uint8_t j = audit_next_;
    const float slot = slot_blocks_;
    const float half = k_marker_half * slot;
    const float position = track_start_.position + k_audit_step * static_cast<float>(j) * slot;
    if (position + half + 1.0f > end_position()) return false;
    const uint8_t first = static_cast<uint8_t>(j / 2u);              // slot under the first half (0 = START)
    const uint8_t last = static_cast<uint8_t>((j + 1u) / 2u);        // slot under the second half
    const uint8_t latest = last > mode_.data_slots ? mode_.data_slots : last;
    if (!slot_decided(track_frame_, latest)) return false;
    float purity = 0.0f;
    for (uint8_t i = first; i <= latest; ++i) {
        if (i >= 1) purity = max_of(purity, static_cast<float>(slot_crest_[i - 1]));
    }

    const FlipMeasure m = measure_flip(position, half);
    const float reference = frame_reference(track_start_);
    const bool steady = m.valid && m.kappa <= k_tune_kappa && m.q / m.kappa >= k_tune_energy &&
                        m.steady >= k_tune_crest * reference;
    tune_run_ = steady ? static_cast<uint8_t>(tune_run_ + 1u) : 0;
    if (tune_run_ >= k_tune_positions) {
        lose(LostReason::signal_gone);
        return false;
    }
    float evidence = 0.0f;
    if (m.valid) {
        const bool marker_like = m.kappa > 0.0f && m.amplitude >= k_flip_amplitude_ratio * reference &&
                                 m.amplitude >= k_audit_purity * purity;
        evidence = marker_like ? clamp(m.q_balanced, k_audit_low, k_audit_high) : clamp(m.q_balanced, k_audit_low, 0.0f);
    }
    audit_.set(static_cast<uint8_t>(j - 1u), evidence);
    if (evidence >= k_audit_high && first_strong_ == 0) first_strong_ = j;
    // A slot centre with a marker's or a carrier's level on f_ref holds no peak of this frame (f_ref is quiet under the
    // peaks, whatever the data): a short final frame's STOP, or the next transmission's tune.
    const bool active = m.valid && m.energy >= k_audit_high &&
                        max_of(m.amplitude, m.steady) >= k_audit_purity * reference;
    if ((j & 1u) == 0 && active && first_active_ == 0) first_active_ = static_cast<uint8_t>(j / 2u);
    if ((j & 1u) == 0) {
        eot_evidence_[j / 2u] = eot_evidence(m, reference);  // EOT evidence at the centre of slot j / 2
        eot_centres_ = static_cast<uint8_t>(j / 2u);
    }
    ++audit_next_;
    return true;
}

// EOT evidence at slot centre `centre` beyond the peaks of the frame (N + 1..N + 3), once the history holds it.
bool Decoder::measure_eot(uint8_t centre) {
    const float slot = slot_blocks_;
    const float half = k_marker_half * slot;
    const Marker& start = late_.pending ? late_.start : track_start_;
    const float position = start.position + static_cast<float>(centre) * slot;
    if (position + half + 1.0f > end_position()) return false;
    eot_evidence_[centre] = eot_evidence(measure_flip(position, half), frame_reference(start));
    eot_centres_ = centre;
    return true;
}

// Flip evidence of an EOT triple, clamped to +-8 and stored in 1/k_audit_scale units: a marker-like flip counts
// its q, anything else at most 0.
int8_t Decoder::eot_evidence(const FlipMeasure& m, float reference) const {
    float eot = 0.0f;
    if (m.valid) {
        const bool flip = m.kappa >= k_kappa_track && m.amplitude >= k_flip_amplitude_ratio * reference;
        eot = flip ? clamp(m.q, -k_evidence_clip, k_evidence_clip) : clamp(min_of(m.q, 0.0f), -k_evidence_clip, 0.0f);
    }
    return static_cast<int8_t>(round_to_int(eot * static_cast<float>(k_audit_scale)));
}

bool Decoder::early_ready() const {
    const bool header = track_frame_ < 0;
    if (!header && audit_next_ <= audit_positions()) return false;
    const float slot = slot_blocks_;
    const uint8_t peaks = header ? k_header_slots : mode_.data_slots;
    const float predicted = track_start_.position + static_cast<float>(peaks + 1u) * slot;
    const float search = header ? k_first_stop_search : (stop_missed_ ? k_track_search_miss : k_track_search);
    return predicted + (search + k_marker_half) * slot + 1.0f <= end_position();
}

// The STOP of frame track_frame_: frame-phase loop (gain 0.5 on the measured STOP), T drift, START/STOP phase
// AFC; the frame then waits for its late step and the next frame's START is set.
void Decoder::track_early() {
    const bool header = track_frame_ < 0;
    const float slot = slot_blocks_;
    const float half = k_marker_half * slot;
    const uint8_t peaks = header ? k_header_slots : mode_.data_slots;
    const float slots = static_cast<float>(peaks + 1u);
    const float predicted = track_start_.position + slots * slot;
    const float search = header ? k_first_stop_search : (stop_missed_ ? k_track_search_miss : k_track_search);
    const float kappa_min = stop_missed_ ? k_kappa_after_miss : k_kappa_track;
    FlipMeasure m = search_flip(predicted, search * slot, half, kappa_min);
    // Within 2 % of T per slot of the frame, but never further than for an 8-peak frame: a longer frame (N = 16, 32)
    // would take a noise flip half a slot from the prediction after a miss for its STOP.
    const float tolerance = k_frame_t_tolerance * min_of(slots, k_frame_t_slots) * slot;
    const bool on_time = fabsf(m.position - predicted) <= tolerance;
    const bool detected = is_flip(m, kappa_min) && m.amplitude >= k_flip_amplitude_ratio * reference_average_ && on_time;
    if (!detected) m = measure_flip(predicted, half);
    const bool marker_like = m.amplitude >= k_flip_amplitude_ratio * reference_average_;
    stop_evidence_ += marker_like ? clipped_evidence(m) : min_of(clipped_evidence(m), 0.0f);

    Marker stop;
    stop.position = detected ? m.position : predicted;
    stop.amplitude = m.amplitude;
    stop.detected = detected;
    float next_start = predicted;
    if (detected) {
        const float error = m.position - predicted;
        next_start = predicted + k_phase_gain * error;
        drift_ = clamp(drift_ + k_drift_gain * error / (slots * slot), -k_max_drift, k_max_drift);
        slot_blocks_ = exact_slot_blocks();
        update_reference(m.amplitude);
        if (!confirmed_) add_marker_edges(m.position, slot, m.amplitude);
    }
    stop_afc(m, detected, half);
    rotation_afc(m, detected);

    if (!header) {
        late_.start = track_start_;
        late_.stop = stop;
        late_.stop_q = m.valid ? m.q : 0.0f;
        late_.slots = finished_slots_;
        late_.first_strong = first_strong_;
        late_.first_active = first_active_;
        late_.pending = true;
        audit_.next_frame();
        if (guard_markers_ < k_count_limit_u8 && detected) ++guard_markers_;
    }
    start_eot_ = eot_evidence(m, reference_average_);
    stop_missed_ = !detected;
    track_start_.position = next_start;
    track_start_.amplitude = stop.amplitude;
    track_start_.detected = detected;
    ++track_frame_;
    if (header) begin_audit();  // a data frame's audit starts after its predecessor's late step
    if (bank_frame_ == static_cast<uint32_t>(track_frame_)) {
        bank_start_ = next_start;  // slot 1 was opened on the prediction; slots 2..N use the measured START
        bank_reference_ = frame_reference(track_start_);
    }
}

// START/STOP phase AFC on a strong detected STOP, whose halves are then cached as the next START's (the START
// may have left the history by the time a long frame's STOP comes).
void Decoder::stop_afc(const FlipMeasure& m, bool detected, float half) {
    Complex stop_before;
    Complex stop_after;
    const bool halves = detected && history_.window(origin_block_, m.position - half, m.position, stop_before) &&
                        history_.window(origin_block_, m.position, m.position + half, stop_after);
    if (halves && m.q >= k_afc_min_q && start_halves_valid_) track_afc(stop_before, stop_after, m.position);
    start_halves_valid_ = halves;
    if (!halves) return;
    start_before_ = stop_before;
    start_after_ = stop_after;
    start_halves_position_ = m.position;
}

void Decoder::rotation_afc(const FlipMeasure& m, bool detected) {
    const float slot_s = slot_blocks_ * static_cast<float>(block_samples_) / static_cast<float>(k_decoder_rate_hz);
    const float level = sqrtf(m.amplitude * m.amplitude + m.steady * m.steady);
    // q - q_balanced = |E_b - E_a| / (2 noise), energy = (E_b + E_a) / (2 noise)
    const bool balanced = m.valid && m.q - m.q_balanced <= k_rotation_balance * m.energy;
    const bool rotated = !detected && balanced && slot_s * k_ms_per_s >= k_rotation_min_slot_ms &&
                         m.energy >= k_rotation_energy && level >= k_train_amplitude_ratio * reference_average_ &&
                         level <= k_rotation_max_level * reference_average_ && fabsf(m.phase_step) >= k_rotation_min_rad;
    if (!rotated) {
        rotation_hz_ = 0.0f;
        return;
    }
    const float offset = m.phase_step / (k_two_pi * k_rotation_span * slot_s);
    const float larger = max_of(fabsf(offset), fabsf(rotation_hz_));
    if (offset * rotation_hz_ > 0.0f && fabsf(offset - rotation_hz_) <= k_rotation_agreement * larger) {
        nco_.adjust_frequency(k_rotation_gain * 0.5f * (offset + rotation_hz_));
        rotation_hz_ = 0.0f;
        return;
    }
    rotation_hz_ = offset;
}

void Decoder::cache_start_halves(float position) {
    const float half = k_marker_half * slot_blocks_;
    start_halves_valid_ = history_.window(origin_block_, position - half, position, start_before_) &&
                          history_.window(origin_block_, position, position + half, start_after_);
    start_halves_position_ = position;
}

bool Decoder::late_ready() const {
    const float reach = k_end_reach + (confirmed_ ? 0.0f : 1.0f);
    if (late_.stop.position + reach * slot_blocks_ + 1.0f > end_position()) return false;
    // An unconfirmed END also looks at the peak slot after the EOT markers (slot 3 of the next frame).
    return confirmed_ || slot_decided(track_frame_, k_eot_clean_slot);
}

void Decoder::track_late() {
    late_.pending = false;
    late_step();
    if (state_ == DecoderState::track) begin_audit();
}

// Late step of a frame (spec 3.9), in order: END, alias, LOST, confirmation, release (EOT triples were checked as
// their markers came in, in run_track()).
void Decoder::late_step() {
    const uint8_t buffer = late_.slots.buffer;
    frames_[buffer].frame_flags = 0;
    if (!late_.start.detected) frames_[buffer].frame_flags |= event_flag_flywheel_start;
    if (!late_.stop.detected) frames_[buffer].frame_flags |= event_flag_flywheel_stop;

    if (end_of_transmission(late_.stop.position)) {
        const bool clean = confirmed_ || (!weak_header_ && frame_present(late_.slots, mode_.data_slots, k_presence) &&
                                          late_.first_strong == 0 && decodable() && stops_found() &&
                                          tones_vary(late_.slots) && eot_clean(late_.stop.position));
        if (clean && !confirmed_) confirm(buffer);
        finish(buffer, clean ? frame_bytes(mode_.bits_per_peak, mode_.data_slots) : 0);
        return;
    }

    const uint8_t window_mask = static_cast<uint8_t>((1u << k_presence_window) - 1u);
    const bool strong = late_.first_strong != 0;
    strong_bits_ = static_cast<uint8_t>(((strong_bits_ << 1) | (strong ? 1u : 0u)) & window_mask);
    // Markers inside the frames: the lock is off the frames' phase. Its T is only in doubt when it came from the mode
    // memory (another station's T may be an alias of this one's): a header's grid decodes at the sender's T alone, so
    // after a header lock T stays open to the relock of this station.
    if (audit_.max_evidence() >= k_audit_threshold || count_bits(strong_bits_) >= k_strong_frames) {
        if ((lock_flags_ & event_flag_mode_memory) != 0) ban_alias(slot_blocks_);
        lose(LostReason::alias);
        return;
    }

    // A frame whose STOP was missed and whose slot centre j <= N shows a marker or a carrier on f_ref: a short final
    // frame whose EOT (or STOP) the next transmission's tune masked, or a lock off the frames' phase. Its data ends
    // before j: the slots from j on are not this frame's peaks.
    if (!late_.stop.detected && late_.first_active != 0) {
        const uint8_t bytes = frame_bytes(mode_.bits_per_peak, static_cast<uint8_t>(late_.first_active - 1u));
        if (frames_[buffer].count > bytes) frames_[buffer].count = bytes;
    }

    const bool present = frame_present(late_.slots, mode_.data_slots, k_presence);
    const bool alive = present || late_.stop_q >= k_q_present;
    presence_bits_ = static_cast<uint8_t>(((presence_bits_ << 1) | (alive ? 1u : 0u)) & window_mask);
    if (k_presence_window - count_bits(presence_bits_) >= k_lost_absent) {
        lose(LostReason::signal_gone);
        return;
    }

    if (!confirmed_) {
        guard_step(buffer, present, strong, alive);
        return;
    }
    // Released: a frame whose STOP was found, or one without it whose peaks are clearly there (3/4 confident; half
    // is often reached by noise at M = 8, e.g. the frame after an EOT that was missed).
    if (late_.stop.detected || frame_present(late_.slots, mode_.data_slots, k_strong_presence)) {
        release_held();
        release_frame(buffer, frames_[buffer].count);
        return;
    }
    // Held: a frame whose STOP and peaks are weak but not gone, released if the next frames show the signal. An
    // absent frame (STOP at noise level, peaks not present) holds noise: it is dropped. A third weak frame in a row
    // drops the oldest held one.
    if (!alive) return;
    if (held_count_ == k_max_held_frames) {
        held_[0] = held_[1];
        --held_count_;
    }
    hold_frame(buffer);
}

// An unconfirmed lock. Fresh (header or header-less with memory): confirmed by frame 0 or 1 when present, with no
// strong inner flip, quiet marker edges, a decodable SNR, its STOPs found where the mode puts them (stops_found:
// interference that passed for a train and a header seldom flips there) and varying tones (tones_vary). Late join:
// the v0.1 guard over 4 frames, whose frames 2 and 3 are held and released.
void Decoder::guard_step(uint8_t buffer, bool present, bool strong, bool alive) {
    if (guard_frames_ < k_count_limit_u8) ++guard_frames_;
    if (present && guard_present_ < k_count_limit_u8) ++guard_present_;
    if ((lock_flags_ & event_flag_late_join) != 0) {
        if (guard_frames_ > k_guard_frames - k_guard_released && alive) hold_frame(buffer);
        if (guard_frames_ < k_guard_frames) return;
        if (guard_confirms()) {
            confirm(buffer);
        } else if (strong_bits_ != 0) {
            ban_alias(slot_blocks_);
            lose(LostReason::alias);
        } else {
            lose(LostReason::signal_gone);
        }
        return;
    }
    if (alive) hold_frame(buffer);
    if (present && !strong && marker_edges_quiet() && decodable() && stops_found() && tones_vary(late_.slots) &&
        (!weak_header_ || weak_header_confirms())) {
        confirm(buffer);
    } else if (guard_frames_ >= k_confirm_frames) {
        lose(LostReason::signal_gone);
    }
}

// Audit of frame track_frame_ from its first position.
void Decoder::begin_audit() {
    audit_next_ = 1;
    first_strong_ = 0;
    first_active_ = 0;
    eot_centres_ = 0;
    memset(eot_evidence_, 0, sizeof(eot_evidence_));
    eot_evidence_[0] = start_eot_;
}

// EOT triples (spec 2.4): a STOP and the two EOT markers flip at slot centres j, j + 1, j + 2 of a frame, which
// then holds j - 1 peaks. j = 2..N: a short final frame; j = N + 1: a full one (besides the END rule); j = 0: the
// frame's START was the last STOP, whose END was missed (a confirmed lock only: nothing of this frame is data).
// Checked on the centres measured so far; `stop` receives j.
bool Decoder::short_end(uint8_t& stop) const {
    const int16_t threshold = static_cast<int16_t>(k_short_eot_evidence * k_audit_scale);
    const int16_t minimum = static_cast<int16_t>(k_short_eot_min * k_audit_scale);
    const uint8_t first = confirmed_ ? 0 : k_short_eot_first;
    for (uint8_t j = first; j <= mode_.data_slots + 1u && j + k_short_eot_flips - 1u <= eot_centres_; ++j) {
        if (j > 0 && j < k_short_eot_first) continue;
        int16_t sum = 0;
        bool flips = true;
        for (uint8_t i = 0; i < k_short_eot_flips; ++i) {
            sum = static_cast<int16_t>(sum + eot_evidence_[j + i]);
            flips = flips && eot_evidence_[j + i] >= minimum;
        }
        if (sum >= threshold && flips) {
            stop = j;
            return true;
        }
    }
    return false;
}

// END by an EOT triple whose STOP is at slot centre `stop` of the frame. A new lock is confirmed by it when its
// peaks are present, its markers' edges quiet, its SNR decodable, its STOPs found (stops_found: the header's) and no
// full-strength flip lies among its peaks. A lock on a weak header waits for a full frame.
void Decoder::end_short(const SlotSummary& slots, uint8_t first_strong, const Marker& start, uint8_t stop) {
    const uint8_t buffer = slots.buffer;
    const uint8_t peaks = stop > 0 ? static_cast<uint8_t>(stop - 1u) : 0;
    frames_[buffer].frame_flags = start.detected ? 0 : event_flag_flywheel_start;
    const bool clean = confirmed_ || (!weak_header_ && frame_present(slots, peaks, k_presence) && marker_edges_quiet() &&
                                      decodable() && stops_found() &&
                                      (first_strong == 0 || first_strong > 2u * peaks + 1u));
    if (clean && !confirmed_) confirm(buffer);
    finish(buffer, clean ? frame_bytes(mode_.bits_per_peak, peaks) : 0);
}

// v0.1 rule: flips at +T and +2T after the STOP.
bool Decoder::end_of_transmission(float stop) const {
    const float half = k_marker_half * slot_blocks_;
    float evidence = 0.0f;
    for (uint8_t j = 1; j <= k_eot_markers; ++j) {
        const FlipMeasure m = measure_flip(stop + static_cast<float>(j) * slot_blocks_, half);
        if (m.valid && m.kappa >= k_kappa_track && m.amplitude >= k_flip_amplitude_ratio * reference_average_) {
            evidence += clamp(m.q, -k_evidence_clip, k_evidence_clip);
        }
    }
    return evidence >= k_end_evidence;
}

// An EOT confirms a new lock only when the EOT markers have quiet edges, no marker follows them and the slot
// after them holds no peak (a frame chain read at the wrong grid has "EOT" flips too).
bool Decoder::eot_clean(float stop) {
    const float slot = slot_blocks_;
    for (uint8_t j = 1; j <= k_eot_markers; ++j) add_marker_edges(stop + static_cast<float>(j) * slot, slot, reference_average_);
    if (!marker_edges_quiet()) return false;
    const FlipMeasure after = measure_flip(stop + static_cast<float>(k_eot_clean_slot) * slot, k_marker_half * slot);
    if (is_flip(after, k_kappa_track)) return false;
    return ((bank_slots_.confident_bits >> (k_eot_clean_slot - 1u)) & 1u) == 0;
}

// A frame is present when at least `fraction` (half, spec 3.9) of its first `peaks` slots hold a confident peak.
bool Decoder::frame_present(const SlotSummary& slots, uint8_t peaks, float fraction) const {
    if (peaks == 0) return false;
    const uint32_t mask = peaks >= k_max_data_slots ? k_all_slots : ((1u << peaks) - 1u);
    return static_cast<float>(count_bits(slots.confident_bits & mask)) >= fraction * static_cast<float>(peaks);
}

// A lock whose header matched only 6 of 8 slots (or none, a faded header with the mode memory): every false lock left
// in F3/F4 had one, against 1 of 2000 true headers in the channel suites. It confirms on a full frame only, with three
// detected STOPs' worth of evidence and at least 8 tones (tones_vary() judges them).
bool Decoder::weak_header_confirms() const {
    return stop_evidence_ >= k_weak_stop_factor * k_q_track && mode_.bits_per_peak >= k_bg_min_bits;
}

// Data puts consecutive peaks on the same tone with probability 1 / M (the rotation spreads repeated bytes), so at
// least half of a frame's confident slots change tone from the previous one; a carrier, keyed CW or speech that passed
// for a header keeps its tone. Below 8 tones repeats are too common to tell.
bool Decoder::tones_vary(const SlotSummary& slots) const {
    if (mode_.bits_per_peak < k_bg_min_bits || slots.confident < k_min_tone_changes) return true;
    return 2u * slots.changes + 1u >= slots.confident;
}

// Late join: the v0.1 guard over 4 frames (spec 3.12).
bool Decoder::guard_confirms() {
    return guard_markers_ >= k_guard_min_detected && guard_present_ >= k_guard_min_present &&
           audit_.max_evidence() < k_audit_threshold && strong_bits_ == 0 && marker_edges_quiet() && decodable();
}

// Carrier phase advance from START to STOP, comparing the same half of both markers: the receiver
// filter shifts both alike, so the estimate is unbiased (the phase step inside one marker is not: a
// filter edge distorts the reversal). Unambiguous for |offset| < 1 / (2 (N + 1) T), which the fine AFC ensures.
void Decoder::track_afc(const Complex& stop_before, const Complex& stop_after, float stop_position) {
    // The carrier sign flips once per marker, so each half of the STOP is minus the matching START half.
    const float re = -(stop_before.re * start_before_.re + stop_before.im * start_before_.im) -
                     (stop_after.re * start_after_.re + stop_after.im * start_after_.im);
    const float im = -(stop_before.im * start_before_.re - stop_before.re * start_before_.im) -
                     (stop_after.im * start_after_.re - stop_after.re * start_after_.im);
    if (re == 0.0f && im == 0.0f) return;
    const float block_seconds = static_cast<float>(block_samples_) / static_cast<float>(k_decoder_rate_hz);
    const float span_seconds = (stop_position - start_halves_position_) * block_seconds;
    if (span_seconds <= 0.0f) return;
    const float offset = atan2f(im, re) / (k_two_pi * span_seconds);
    const float limit = k_afc_clamp / (slot_blocks_ * block_seconds);
    nco_.adjust_frequency(clamp(k_afc_gain * offset, -limit, limit));
}

void Decoder::add_marker_edges(float marker, float slot_blocks, float crest) {
    for (int8_t side = -1; side <= 1; side += 2) {
        float excess = 0.0f;
        float noise_ratio = 0.0f;
        if (!boundary_excess(marker + static_cast<float>(side) * k_slot_centre * slot_blocks, slot_blocks, crest, excess,
                             noise_ratio)) {
            continue;
        }
        guard_edge_excess_ += excess;
        guard_edge_noise_ += noise_ratio;
        if (guard_edges_ < k_count_limit_u8) ++guard_edges_;
    }
}

bool Decoder::marker_edges_quiet() const {
    if (guard_edges_ == 0) return true;
    const float count = static_cast<float>(guard_edges_);
    const float allowance = k_marker_edge_sigmas * (guard_edge_noise_ / count) / sqrtf(count);
    return guard_edge_excess_ / count <= k_marker_edge_max + allowance;
}

// The lock's STOPs so far (the header's and the data frames'), each its clipped q if it is marker-like: one detected
// STOP, or two weak ones near the gate (q 3-4 at T = 8 ms).
bool Decoder::stops_found() const {
    return stop_evidence_ >= k_q_track;
}

bool Decoder::decodable() const {
    const float model_db = k_model_snr_db - k_db_per_decade * log10f(slot_ms() / k_model_slot_ms);
    return snr_db() >= model_db - k_confirm_margin_db;
}

// `buffer`: the frame that confirmed the lock; the locked event names the first frame released.
void Decoder::confirm(uint8_t buffer) {
    confirmed_ = true;
    tone_confirmed_ = true;
    emit_locked(lock_flags_, frames_[held_count_ > 0 ? held_[0] : buffer].frame_index);
    release_held();
}

void Decoder::release_frame(uint8_t buffer, uint8_t bytes) {
    const FrameBuffer& frame = frames_[buffer];
    const uint8_t count = bytes < frame.count ? bytes : frame.count;
    for (uint8_t n = 0; n < count; ++n) {
        Event event = make_event(EventType::byte);
        event.value = frame.bytes[n];
        event.index = n;
        event.flags = static_cast<uint8_t>(frame.flags[n] | frame.frame_flags | lock_flags_);
        for (uint8_t b = 0; b < k_bits_per_byte; ++b) {
            event.soft[b] = static_cast<int8_t>(k_soft_scale * soft_nibble(frame.soft, static_cast<uint16_t>(n * k_bits_per_byte + b)));
        }
        event.frame_index = frame.frame_index;
        emit(event);
    }
}

void Decoder::release_held() {
    for (uint8_t i = 0; i < held_count_; ++i) release_frame(held_[i], frames_[held_[i]].count);
    held_count_ = 0;
}

void Decoder::hold_frame(uint8_t buffer) {
    if (held_count_ < k_max_held_frames) held_[held_count_++] = buffer;
}

// A buffer that holds neither a held frame, the frame awaiting its late step nor the frame just finished.
uint8_t Decoder::free_buffer() const {
    uint8_t fallback = 0;
    for (uint8_t b = 0; b < k_frame_buffers; ++b) {
        bool held = false;
        for (uint8_t i = 0; i < held_count_; ++i) held = held || held_[i] == b;
        if (held) continue;
        fallback = b;
        if ((late_.pending && late_.slots.buffer == b) || finished_slots_.buffer == b) continue;
        return b;
    }
    return fallback;
}

// ---------------------------------------------------------------------------
// TRACK: slot decisions (spec 3.10), on the sample clock
// ---------------------------------------------------------------------------

void Decoder::begin_bank_frame() {
    const uint8_t buffer = free_buffer();
    FrameBuffer& frame = frames_[buffer];
    memset(&frame, 0, sizeof(frame));
    frame.frame_index = bank_frame_;
    memset(&bank_slots_, 0, sizeof(bank_slots_));
    bank_slots_.buffer = buffer;
    bank_decided_ = 0;
    byte_bits_ = 0;
    byte_flags_ = 0;
    bank_reference_ = reference_average_;
}

void Decoder::schedule_data_window(uint8_t slot) {
    schedule_window(bank_start_ + static_cast<float>(slot) * slot_blocks_, slot_blocks_, slot);
}

// Grid tones f_t = f_ref + side (5 + t c) / T at the exact T (1 + epsilon); coefficients follow the AFC.
void Decoder::open_data_window() {
    const float slot_s = static_cast<float>(mode_.slot_us) / (k_us_per_ms * k_ms_per_s) * (1.0f + drift_);
    const float spacing = mode_.spacing == Spacing::standard
                              ? static_cast<float>(k_standard_spacing_num) / static_cast<float>(k_standard_spacing_den)
                              : 1.0f;
    const float tone = nco_.frequency();
    const float side = static_cast<float>(mode_.side);
    const uint16_t bins = grid_.bank.bins();
    for (uint16_t t = 0; t < bins; ++t) {
        const float offset = (static_cast<float>(k_grid_guard) + static_cast<float>(t) * spacing) / slot_s;
        grid_.bank.set_frequency(t, tone + side * offset);
    }
}

// Window 0 is the STOP slot: no peak, only noise (and a marker leaking below -40 dB into the grid). It measures
// the noise for the marker path and the SNR report once per frame (spec 3.10 "marker noise"; the peaks' own
// windows leak -29..-37 dB into every bin, which N_bin carries at high SNR).
void Decoder::close_data_window() {
    const uint8_t slot = static_cast<uint8_t>(window_index_);
    const float square_sum = grid_.bank.window_square_sum();
    if (slot == 0) {
        if (square_sum > 0.0f && !window_blanked_) track_noise_.push(dsp::window_noise(grid_.bank) / square_sum);
        schedule_data_window(1);
        return;
    }
    const dsp::SlotDecision decision =
        dsp::decide_slot(grid_.bank, grid_.background, mode_.bits_per_peak, static_cast<uint8_t>(slot - 1u));
    const uint16_t bins = grid_.bank.bins();
    for (uint16_t t = 0; t < bins; ++t) grid_.background.push(t, grid_.bank.energy(t));
    store_slot(decision, slot, window_blanked_);
    if (state_ != DecoderState::track) return;
    if (slot < mode_.data_slots) {
        schedule_data_window(static_cast<uint8_t>(slot + 1u));
        return;
    }
    finished_slots_ = bank_slots_;
    ++bank_frame_;
    bank_start_ += static_cast<float>(mode_.data_slots + 1u) * slot_blocks_;
    begin_bank_frame();
    schedule_data_window(0);
}

// Appends the slot's k bits (MSB first) with their LLRs to the frame buffer; each byte keeps the OR of the
// flags of the slots that carried its bits.
void Decoder::store_slot(const dsp::SlotDecision& decision, uint8_t slot, bool blanked) {
    uint8_t flags = 0;
    if (decision.erasure) flags |= event_flag_erasure;
    if (blanked) flags |= event_flag_blanked;
    emit_slot(decision, slot, flags);
    if (decision.confident) {
        bank_slots_.confident_bits |= 1u << (slot - 1u);
        if (bank_slots_.confident > 0 && decision.tone != bank_slots_.last_tone) ++bank_slots_.changes;
        bank_slots_.last_tone = decision.tone;
        ++bank_slots_.confident;
    }
    const float crest = decision.crest < static_cast<float>(k_crest_limit) ? decision.crest : static_cast<float>(k_crest_limit);
    slot_crest_[slot - 1u] = static_cast<uint16_t>(round_to_int(crest));
    bank_decided_ = slot;

    FrameBuffer& frame = frames_[bank_slots_.buffer];
    const uint8_t k = mode_.bits_per_peak;
    for (uint8_t b = 0; b < k && frame.count < UNLIMITED_MAX_FRAME_BYTES; ++b) {
        const uint8_t bit = static_cast<uint8_t>((decision.symbol >> (k - 1u - b)) & 1u);
        frame.bytes[frame.count] = static_cast<uint8_t>((frame.bytes[frame.count] << 1) | bit);
        set_soft_nibble(frame.soft, static_cast<uint16_t>(frame.count * k_bits_per_byte + byte_bits_), decision.soft[b]);
        byte_flags_ = static_cast<uint8_t>(byte_flags_ | flags);
        if (++byte_bits_ < k_bits_per_byte) continue;
        frame.flags[frame.count] = byte_flags_;
        ++frame.count;
        byte_bits_ = 0;
        byte_flags_ = 0;
    }
}

bool Decoder::slot_decided(int32_t frame, uint8_t slot) const {
    if (frame < 0) return true;
    const uint32_t f = static_cast<uint32_t>(frame);
    return bank_frame_ > f || (bank_frame_ == f && bank_decided_ >= slot);
}

// ---------------------------------------------------------------------------
// Mode memory (spec 3.12)
// ---------------------------------------------------------------------------

void Decoder::remember_mode() {
    if (!confirmed_ || mode_.slot_us == 0) return;
    memory_.mode = mode_;
    memory_.tone_hz = nco_.frequency();
    memory_.crest = reference_average_;
    memory_.expires_block = history_.end_block() +
                            static_cast<uint32_t>(k_mode_memory_ms * k_samples_per_ms / static_cast<float>(block_samples_));
    memory_.valid = true;
}

bool Decoder::memory_valid() const {
    return memory_.valid && static_cast<int32_t>(memory_.expires_block - history_.end_block()) > 0;
}

bool Decoder::memory_usable() const {
    return memory_valid() && fabsf(nco_.frequency() - memory_.tone_hz) <= k_mode_memory_hz;
}

bool Decoder::stream_relock() const {
    return state_ == DecoderState::acquire && !tone_steady_ && memory_usable();
}

// A tune on the held f_ref (a new transmission, not the stream), over the widest candidate scale or k_stream_tune_half
// of the remembered T if longer, just before `end`.
bool Decoder::stream_tune(float end, float widest_half) const {
    const float half = max_of(widest_half, k_stream_tune_half * mode_slot_blocks(memory_.mode));
    const FlipMeasure m = measure_flip(end - half - 1.0f, half);
    return m.valid && m.kappa <= k_tune_kappa && m.energy >= k_tune_energy && m.steady >= k_tune_crest * memory_.crest;
}

// A tone inside the remembered grid (f_ref to its farthest tone, plus a search bin).
bool Decoder::memory_holds(float tone_hz) const {
    if (!memory_valid()) return false;
    const Mode& mode = memory_.mode;
    const float slot_s = static_cast<float>(mode.slot_us) / (k_us_per_ms * k_ms_per_s);
    const float spacing = mode.spacing == Spacing::standard
                              ? static_cast<float>(k_standard_spacing_num) / static_cast<float>(k_standard_spacing_den)
                              : 1.0f;
    const float span = (static_cast<float>(k_grid_guard) + static_cast<float>(grid_tones(mode.bits_per_peak) - 1u) * spacing) / slot_s;
    const float offset = (tone_hz - memory_.tone_hz) * static_cast<float>(mode.side);
    return offset >= -k_memory_search_margin_hz && offset <= span + k_memory_search_margin_hz;
}

float Decoder::mode_slot_blocks(const Mode& mode) const {
    return static_cast<float>(mode.slot_us) / k_us_per_ms * k_samples_per_ms / static_cast<float>(block_samples_);
}

// ---------------------------------------------------------------------------
// Measurement (spec 3.4)
// ---------------------------------------------------------------------------

FlipMeasure Decoder::measure_flip(float centre, float half) const {
    Complex before;
    Complex after;
    if (!history_.window(origin_block_, centre - half, centre, before) ||
        !history_.window(origin_block_, centre, centre + half, after)) {
        FlipMeasure invalid;
        memset(&invalid, 0, sizeof(invalid));
        invalid.kappa = k_no_value;
        invalid.position = centre;
        invalid.valid = false;
        return invalid;
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

// Expected square of a marker crest measured on noise alone: N_m.
float Decoder::marker_noise(float sigma2, float slot_blocks) const {
    const float half = k_marker_half * slot_blocks;
    const float gain = 2.0f * half * static_cast<float>(block_samples_) * k_g_marker;
    return k_noise_amplitude * sigma2 * 2.0f * dsp::noise_samples(half, block_samples_) / (gain * gain);
}

float Decoder::noise_variance() const {
    const float estimate = state_ == DecoderState::track ? track_noise_.mean_estimate() : noise_.mean_estimate();
    return max_of(estimate, k_min_noise_variance);
}

float Decoder::end_position() const {
    return static_cast<float>(history_.end_block() - origin_block_);
}

float Decoder::candidate_position(const Candidate& candidate) const {
    return static_cast<float>(static_cast<int32_t>(candidate.block - origin_block_)) + candidate.fraction;
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
    ban_until_block_ = history_.end_block() +
                       static_cast<uint32_t>(k_alias_ban_ms * k_samples_per_ms / static_cast<float>(block_samples_));
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
    event.frame_index = bank_frame_;
    event.tone_hz = tone_hz();
    event.slot_ms = slot_ms();
    event.snr_db = snr_db();
    if (mode_.slot_us != 0) {
        event.bits_per_peak = mode_.bits_per_peak;
        event.data_slots = mode_.data_slots;
        event.spacing = mode_.spacing;
        event.side = mode_.side;
    }
    return event;
}

void Decoder::emit(const Event& event) {
    if (handler_ != 0) handler_(event, context_);
}

void Decoder::emit_slot(const dsp::SlotDecision& decision, uint8_t slot, uint8_t flags) {
    Event event = make_event(EventType::slot);
    event.value = decision.symbol;
    event.index = slot;
    event.tone = decision.tone;
    event.level_pct = percent_of(decision.crest, bank_reference_);
    event.confidence = decision.confidence;
    event.flags = flags;
    for (uint8_t b = 0; b < mode_.bits_per_peak; ++b) event.soft[b] = static_cast<int8_t>(k_soft_scale * decision.soft[b]);
    emit(event);
}

void Decoder::emit_locked(uint8_t flags, uint32_t frame_index) {
    Event event = make_event(EventType::locked);
    event.flags = flags;
    event.frame_index = frame_index;
    emit(event);
}

}  // namespace unlimited
