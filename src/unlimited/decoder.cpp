#include "unlimited/decoder.hpp"

#include <math.h>
#include <string.h>

namespace unlimited {

// Spec 3.9: the history (k_history_cells, 8.3 KB) and the look-ahead (k_lookahead_max_samples, 6.4 KB) are most of
// the decoder; the tone search is 1.5 KB and the rest about 1 KB. The budget leaves 1 KB of room for the 64-bit host.
static const uint32_t k_decoder_budget_bytes = 18432;
static_assert(sizeof(Decoder) <= k_decoder_budget_bytes, "Decoder must fit its 18 KB budget (spec 3.9)");

namespace {

using dsp::Complex;

// Units.
const float k_us_per_sample = 125.0f;  // 10^6 / 8000
const float k_samples_per_ms = static_cast<float>(k_decoder_rate_hz) / 1000.0f;
const float k_pi = 3.14159265f;
const float k_two_pi = 2.0f * k_pi;
const float k_reference_bandwidth_hz = 2500.0f;
const float k_snr_floor_db = -99.0f;
const float k_db_per_decade = 10.0f;
const float k_noise_amplitude = 4.0f;  // E|2 S / (n g)|^2 = 4 sigma^2 n_eff / (n g)^2
const float k_percent = 100.0f;
const float k_percent_limit = 255.0f;
const uint8_t k_top_bit = 0x80;
const float k_half = 0.5f;

// Front end (spec 3.1).
const uint8_t k_energy_shift = 4;
const float k_energy_scale = 256.0f;                                                 // (1 << k_energy_shift)^2
const float k_in_bin_scale = 2.0f / (k_mixer_gain * k_mixer_gain * k_energy_scale);  // e_k = p_k for a steady tone
const uint8_t k_settle_blocks = dsp::ImpulseBlanker::k_latency + 1;  // blocks mixed before the NCO moved far
// The history is re-mixed when the NCO moves this little (a neighbour search bin: the leading bin of a tone between two
// bins); a farther move is another signal and forgets it. A block mixed 75 Hz off loses at most 1.3 dB (32 samples).
const float k_remix_reach_hz = 75.0f;
const float k_steer_min_hz = 2.0f;   // ACQUIRE follows the search's estimate when it moves at least this much
const float k_search_step_hz = 50.0f;  // the tone search's bins
// A fresh tone that is not the strongest takes the NCO over when its slot-long average is at least this part of the
// tone held's: a transmission starting beside a steady tone or keyed CW, not a sideband of a stronger signal.
const float k_takeover_ratio = 0.25f;
// ... and it stands at least this far over the leading threshold: a noise bin that crosses it by chance as a
// transmission starts on the tone held does not take the receiver away (1 byte/s at gate + 3 dB lost 1 in 40 so).
const float k_takeover_excess = 2.0f;

// Geometry, in slots (spec 3.3-3.5).
const float k_slot_window = 0.75f;   // levels: the central 75 % of a slot
const float k_noise_window = 0.5f;   // noise: the central half of a decided zero, clear of its neighbours' ramps
const float k_timing_half = 0.5f;    // early/late half windows
const float k_edge_half = 0.1f;      // slot-edge windows, +-0.1 T around a boundary
const float k_scan_lead = 0.125f;    // the scan's slot window starts 1/8 slot in (the central 75 %)
const float k_onset_back = 0.25f;    // locate_onset() looks from 1/4 slot before the first loud position ...
const float k_onset_reach = 1.0f;    // ... to one slot after it
const float k_onset_rise = 0.5f;     // a rising edge: the later half holds 3/4 of the energy of a slot-long window
const uint8_t k_onset_walk_slots = 2;  // locate_onset() moves back at most this many slots onto an earlier tone
const float k_onset_gap = 0.5f;      // slots without a loud window before a new onset
const float k_onset_half = 0.25f;    // the walk-back's half slot ends a quarter slot before the onset
const uint8_t k_margin_blocks = 2;   // a CIC-2 block holds samples up to one block after its start
const float k_margin_slots = 0.5f;   // history held past a window before it is read: the early/late windows
const float k_keep_slots = 4.0f;     // a re-mix keeps 4 slots behind the scan or the anchor (what they read)

// Acquisition (spec 3.3).
const uint8_t k_onset_silent_slots = 2;
const float k_onset_tolerance = 0.25f;     // slots: the 2 silent slots may start this much late (block rounding)
const uint8_t k_proven_silent_slots = 10;  // a whole window: longer than any silent run inside a transmission (8)
// Silence before the START is relative to the START (spec 3.3): a candidate stays tentative for 3 slots, and a tone
// 16 times its energy there (a quarter of the level, k_silence_ratio) whose 2 slots before it are quiet next to it
// replaces it: the candidate was a weak precursor (a linear-phase filter's pre-echo, a squelch burst, a click).
const float k_watch_slots = 3.0f;
const float k_precursor_rise = 16.0f;      // 1 / k_silence_ratio^2
// The scan's energy (spec 3.3): coherent sums over sub-windows of at most k_scan_coherent_ms (before the lock the pitch
// is known within a search bin: 25 Hz turns 0.25 cycles in 10 ms, a 0.9 dB loss), their energies added. A window is
// loud when noise alone would give that energy with probability 1e-6 (the Gamma tail, Wilson-Hilferty: z 4.75).
const float k_scan_coherent_ms = 10.0f;
const float k_loud_z = 4.75f;
const float k_wilson_third = 1.0f / 9.0f;
const uint8_t k_acquire_blocks_per_block = 8;
// The check before lock covers k_min_acquire_windows windows, and at least k_acquire_min_ms of signal (4 windows at 25
// bytes/s): a short burst of noise, CW or speech seldom shows START and STOP where 4 windows need them.
const uint8_t k_min_acquire_windows = 2;
const uint8_t k_max_acquire_windows = 4;
const float k_acquire_min_ms = 160.0f;
const uint8_t k_refine_iterations = 4;      // the early/late balance moves at most 0.19 slot per step
const float k_refine_settled = 0.01f;       // slots: a step this small ends the refinement
const uint8_t k_pitch_gap_blocks = 2;      // CIC-2 blocks two apart carry independent noise
// |sum of products| over the blocks' energies: a tone (0.7 and more on the tone slots at the gates), not noise (about
// 1 / sqrt(products)).
const float k_pitch_coherence = 0.4f;
const float k_refine_band_part = 0.5f;     // the largest correction: this part of the occupied band
const float k_marker_ratio = 0.5f;         // START/STOP present: level >= half the running reference ...
const float k_marker_snr = 4.0f;           // ... and |S|^2 >= 4 times the noise of its window
const float k_edge_ratio_max = 0.3f;       // energy density at the slot edges / at the markers (a steady tone: 1.1)
// ... at the two boundaries after the first START, the slots after it loud too: a steady tone reads 0.8-1.0 there, a
// boundary between two beeps 0.05-0.25 (one edge alone is noisy near the gate: both must be above).
const float k_start_edge_max = 0.6f;
const uint8_t k_steady_slots = 2;
// The early test (before the anchor is refined): each boundary's edge is the quietest of those k_edge_step apart
// within k_edge_search_steps steps on either side (a beep's null lies within 0.2 T of an anchor that far off).
const float k_edge_step = 0.1f;
const uint8_t k_edge_search_steps = 2;
const float k_acquire_timeout_ms = 1500.0f;
const uint8_t k_acquire_timeout_windows = 3;
const float k_search_ban_ms = 10000.0f;

// Timing and pitch (spec 3.4).
const float k_timing_slope = 5.33f;        // (late - early) / (late + early) per slot of offset, Tukey beep
const float k_timing_gain = 0.2f;
const float k_timing_integral = 0.02f;
const float k_timing_clamp = 0.25f;        // slots per window
const float k_pitch_gain = 0.5f;
const float k_pitch_clamp = 0.1f;          // cycles per slot per window
const uint8_t k_stop_gap_slots = 9;        // START to STOP

// Decisions (spec 3.5).
const float k_floor_sigma = 2.6f;          // the line never below 2.6 sigma of an empty slot (v0.3)
const float k_soft_scale = 64.0f;
const float k_soft_limit = 127.0f;
const float k_weak_margin = 0.125f;
const float k_reference_alpha = 0.25f;
const uint8_t k_noise_zero_pct = 35;       // a zero feeds the noise estimate when under 35 % of the reference line

// Framing and end (spec 3.6, 3.7).
const uint8_t k_max_framing_errors = 2;
// After a silent START both hypotheses (a new transmission, the old one going on) are weighed on at most this many
// more windows each while both hold (spec 3.7); then the signal is lost.
const uint8_t k_pending_windows = 4;
const float k_loud_snr = 16.0f;            // a slot with |S|^2 >= 16 times its noise holds a tone (noise: p = 1e-7)
const float k_silence_ratio = 0.25f;       // a slot below a quarter of the tone's level is silent
// Before a START (spec 3.3): a slot holds something when |S|^2 >= 8 times its noise (noise alone: 3e-4) and it is as
// loud as a quarter of the START: a weak carrier or keyed tone under it is no silence.
const float k_before_snr = 8.0f;
// A window with nothing in it: its 10 slots' |S|^2 average under twice their noise (noise alone: over it with
// probability 0.005, the Gamma(10) tail).
const float k_silent_mean_snr = 2.0f;

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

uint32_t whole_blocks(float blocks) {
    const int32_t count = round_to_int(blocks);
    return count > 0 ? static_cast<uint32_t>(count) : 1u;
}

uint8_t percent_of(float value, float reference) {
    if (reference <= 0.0f) return static_cast<uint8_t>(k_percent_limit);
    return static_cast<uint8_t>(round_to_int(clamp(k_percent * value / reference, 0.0f, k_percent_limit)));
}

// Phase of a times conj(b), in -pi..pi.
float phase_between(const Complex& a, const Complex& b) {
    return atan2f(a.im * b.re - a.re * b.im, a.re * b.re + a.im * b.im);
}

}  // namespace

// ---------------------------------------------------------------------------
// DecoderConfig
// ---------------------------------------------------------------------------

DecoderConfig::DecoderConfig()
    : slot_us(slot_us_for_centi_speed(k_default_centi_bytes_per_second)),
      passband(),
      threshold_percent(k_default_threshold_percent),
      decision_mode(DecisionMode::fixed),
      impulse_blanker(true) {
    passband.low_hz = k_ssb_passband_low_hz;
    passband.high_hz = k_ssb_passband_high_hz;
}

ConfigError DecoderConfig::check() const {
    if (!slot_valid(slot_us)) return ConfigError::slot;
    if (!passband_valid(passband)) return ConfigError::passband;
    const Passband range = search_range();
    if (range.low_hz > range.high_hz) return ConfigError::passband;
    if (threshold_percent < k_min_threshold_percent || threshold_percent > k_max_threshold_percent) {
        return ConfigError::threshold;
    }
    if (decision_mode != DecisionMode::fixed && decision_mode != DecisionMode::adaptive) {
        return ConfigError::decision_mode;
    }
    return ConfigError::none;
}

bool DecoderConfig::valid() const {
    return check() == ConfigError::none;
}

Passband DecoderConfig::search_range() const {
    return unlimited::search_range(passband, slot_us);
}

// ---------------------------------------------------------------------------
// Construction and public API
// ---------------------------------------------------------------------------

Decoder::Decoder(const DecoderConfig& config, EventHandler handler, void* context)
    : config_(config), handler_(handler), context_(context), state_(DecoderState::search) {
    initialize();
}

void Decoder::initialize() {
    const float slot_samples = static_cast<float>(config_.slot_us) / k_us_per_sample;
    block_samples_ = 0;
    if (config_.valid()) {
        const int32_t samples = round_to_int(slot_samples / static_cast<float>(k_blocks_per_slot));
        block_samples_ = static_cast<uint8_t>(samples < k_min_block_samples
                                                  ? k_min_block_samples
                                                  : (samples > k_max_block_samples ? k_max_block_samples : samples));
    }
    const uint8_t samples = block_samples_ > 0 ? block_samples_ : k_min_block_samples;
    slot_blocks_ = slot_samples / static_cast<float>(samples);
    scan_group_ = max_of(1.0f, k_scan_coherent_ms * k_samples_per_ms / static_cast<float>(samples));
    const float lead = 2.0f * slot_samples + static_cast<float>(k_lead_latency_samples);
    lookahead_.configure(static_cast<uint16_t>(min_of(lead, static_cast<float>(k_lookahead_max_samples))));
    // A tone that came up at most fresh_blocks_ search blocks ago began at most a search block before (the leading
    // average's lag); a far move now restarts the history (after the settle) the look-ahead behind the search, so it
    // still holds 2 slots of silence before the START: R - 2 T - settle - lag, less a block for the block grid.
    const float search_block = static_cast<float>(dsp::ToneSearch::k_block_samples);
    const float settle = static_cast<float>(k_settle_blocks * samples);
    const float reach = static_cast<float>(lookahead_.delay()) - 2.0f * slot_samples - settle - search_block;
    fresh_blocks_ = static_cast<uint16_t>(max_of(floorf(reach / search_block) - 1.0f, 1.0f));
    // A tone held that came up at most protect_blocks_ ago may be a transmission whose START the scan has not reached
    // yet (it hears the look-ahead later, and knows an onset a slot after it).
    protect_blocks_ = static_cast<uint16_t>(
        ceilf((static_cast<float>(lookahead_.delay()) + 2.0f * slot_samples) / search_block) + 1.0f);
    const float half_band = k_half * static_cast<float>(occupied_band(k_default_tone_hz, config_.slot_us).width_hz);
    band_reach_hz_ = half_band + k_search_step_hz;
    refine_reach_hz_ = max_of(k_refine_band_part * 2.0f * half_band, k_remix_reach_hz);
    const float window_ms = static_cast<float>(k_window_slots) * slot_samples / k_samples_per_ms;
    acquire_windows_ = static_cast<uint8_t>(
        min_of(max_of(ceilf(k_acquire_min_ms / window_ms), static_cast<float>(k_min_acquire_windows)),
               static_cast<float>(k_max_acquire_windows)));
    block_fill_ = 0;
    settle_blocks_ = 0;
    settle_trusted_ = false;
    block_energy_ = 0;

    nco_ = dsp::Nco();
    // An invalid configuration leaves the decoder idle (block_samples_ 0) on the widest search, a valid pitch.
    Passband range;
    range.low_hz = k_min_tone_hz;
    range.high_hz = k_max_tone_hz;
    if (block_samples_ > 0) range = config_.search_range();
    const uint16_t high = range.high_hz >= range.low_hz ? range.high_hz : range.low_hz;
    const uint16_t start_hz = k_default_tone_hz < range.low_hz ? range.low_hz : (k_default_tone_hz > high ? high
                                                                                                          : k_default_tone_hz);
    nco_.set_frequency(static_cast<float>(start_hz));
    cic_.reset();
    blanker_.reset();
    memset(blank_delay_re_, 0, sizeof(blank_delay_re_));
    memset(blank_delay_im_, 0, sizeof(blank_delay_im_));
    history_.reset();
    search_.configure(range.low_hz, high, config_.slot_us);
    noise_.reset(0.0f);
    search_noise_ = k_min_noise_variance;

    origin_block_ = history_.end_block();
    memset(&scan_, 0, sizeof(scan_));
    memset(&pending_scan_, 0, sizeof(pending_scan_));
    // What came before the first sample is unknown (V6: a receiver that starts in the middle of a transmission waits
    // for the next one): the scan starts where the look-ahead's initial zeros end, untrusted.
    const uint32_t zeros = (lookahead_.delay() + samples - 1u) / samples;
    restart_scan(history_.end_block() + zeros, false);
    acquire_block_ = history_.end_block();
    locked_tone_ = false;
    locked_hz_ = 0.0f;

    window_start_ = 0.0f;
    byte_index_ = 0;
    drift_ = 0.0f;
    reference_ = 0.0f;
    last_stop_.re = 0.0f;
    last_stop_.im = 0.0f;
    has_last_stop_ = false;
    framing_run_ = 0;
    framing_errors_ = 0;
    pending_ = false;
    pending_new_ = false;
    pending_stage_ = 0;
    pending_start_ = 0.0f;
    pending_anchor_ = 0.0f;
    pending_reference_ = 0.0f;
    memset(&pending_window_, 0, sizeof(pending_window_));
    pitch_refined_ = false;
    start_checked_ = false;
    verify_hz_ = 0.0f;
    state_ = DecoderState::search;
}

void Decoder::reset() {
    const DecoderState old = state_;
    if (old == DecoderState::track) {
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

// The tone search hears the sample now; the mixer hears it the look-ahead later (spec 3.1).
void Decoder::process_sample(int16_t sample) {
    if (block_samples_ == 0) return;
    if (search_.push(sample)) on_search_block();
    const int32_t x = lookahead_.push(sample);
    const int32_t scaled = x >> k_energy_shift;
    block_energy_ += static_cast<uint32_t>(scaled * scaled);
    int16_t cos_q15;
    int16_t sin_q15;
    nco_.next(cos_q15, sin_q15);
    cic_.push((x * cos_q15) >> k_mix_shift, -((x * sin_q15) >> k_mix_shift));
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
    const float slot_blocks = state_ == DecoderState::track
                                  ? slot_blocks_ + drift_ / static_cast<float>(k_window_slots)
                                  : slot_blocks_;
    return blocks_to_ms(slot_blocks);
}

// Key-down tone power over the noise in 2500 Hz (spec 1.3's convention): the reference is the crest A of a beep, the
// tone's power A^2 / 2 against sigma^2 * 2500 / 4000.
float Decoder::snr_db() const {
    if (state_ != DecoderState::track || reference_ <= 0.0f) return 0.0f;
    const float power = reference_ * reference_ * static_cast<float>(k_decoder_rate_hz);
    const float noise = k_noise_amplitude * k_reference_bandwidth_hz * noise_variance();
    if (power <= 0.0f) return k_snr_floor_db;
    return k_db_per_decade * log10f(power / noise);
}

uint32_t Decoder::framing_errors() const {
    return framing_errors_;
}

uint16_t Decoder::lookahead_samples() const {
    return lookahead_.delay();
}

uint8_t Decoder::acquire_windows() const {
    return acquire_windows_;
}

const DecoderConfig& Decoder::config() const {
    return config_;
}

// ---------------------------------------------------------------------------
// Front end (spec 3.1)
// ---------------------------------------------------------------------------

void Decoder::on_search_block() {
    search_noise_ = max_of(search_.floor() / static_cast<float>(dsp::ToneSearch::k_block_samples),
                           k_min_noise_variance);
    float tone = 0.0f;
    switch (state_) {
        case DecoderState::search:
            steer(false);
            if (search_.candidate(tone)) {
                locked_tone_ = true;
                locked_hz_ = tone;
                enter_acquire();
            }
            break;
        case DecoderState::acquire: {
            if (search_.candidate(tone)) {
                locked_tone_ = true;
                locked_hz_ = tone;
            }
            // A candidate START holds the pitch and the history it needs until it is verified (a stronger tone that
            // has just come up aside).
            const bool candidate = scan_.found || scan_.watching;
            steer(candidate);
            // A steady carrier on the held pitch (the search masks it after 2.5 s) is no transmission.
            if (!candidate && search_.masked(nco_.frequency())) enter_search();
            break;
        }
        case DecoderState::track:
            break;
    }
}

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
        history_.reset();
        if (--settle_blocks_ == 0) restart_scan(history_.end_block(), settle_trusted_);
    }
    rebase();

    switch (state_) {
        case DecoderState::search:
        case DecoderState::acquire:
            run_acquire();
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
    window_start_ -= delta;
    pending_start_ -= delta;
    pending_anchor_ -= delta;
    scan_.anchor -= delta;
    pending_scan_.anchor -= delta;
}

// Moves the NCO to tone_hz and re-mixes what was mixed already (the history from keep_from on and the blanker's delay
// line) as if it had been there all along (spec 3.1, v0.3's re-mix): h_i sums samples around the first sample of block
// i, and its samples turn by 2 pi df per sample from there to the first sample mixed at the new tone. mixed_samples:
// those of the block being summed that the old tone mixed.
void Decoder::retune(float tone_hz, uint8_t mixed_samples, uint32_t keep_from) {
    const float before = nco_.frequency();
    nco_.set_frequency(tone_hz);
    const float per_sample = k_two_pi * (nco_.frequency() - before) / static_cast<float>(k_decoder_rate_hz);
    const float per_block = per_sample * static_cast<float>(block_samples_);
    const uint8_t latency = dsp::ImpulseBlanker::k_latency;
    const float newest = per_sample * static_cast<float>(mixed_samples) + per_block;  // the delay line's newest
    for (uint8_t i = 0; i < latency; ++i) {
        const float angle = newest + per_block * static_cast<float>(latency - 1u - i);
        const float c = cosf(angle);
        const float s = sinf(angle);
        const float re = static_cast<float>(blank_delay_re_[i]);
        const float im = static_cast<float>(blank_delay_im_[i]);
        blank_delay_re_[i] = round_to_int(re * c - im * s);
        blank_delay_im_[i] = round_to_int(re * s + im * c);
    }
    history_.rotate(keep_from, newest + per_block * static_cast<float>(latency), per_block);
}

void Decoder::forget_history(bool trusted) {
    history_.reset();
    origin_block_ = history_.end_block();
    settle_blocks_ = k_settle_blocks;
    settle_trusted_ = trusted;
    restart_scan(history_.end_block(), trusted);
}

// ---------------------------------------------------------------------------
// State changes
// ---------------------------------------------------------------------------

void Decoder::set_state(DecoderState state) {
    if (state == state_) return;
    state_ = state;
    emit(make_event(EventType::state));
}

void Decoder::enter_search() {
    locked_tone_ = false;
    set_state(DecoderState::search);
}

void Decoder::enter_acquire() {
    acquire_block_ = history_.end_block();
    set_state(DecoderState::acquire);
}

// ---------------------------------------------------------------------------
// Acquisition (spec 3.2, 3.3)
// ---------------------------------------------------------------------------

// SEARCH and ACQUIRE before an anchor: the NCO follows the leading bin (the provisional tune), or in ACQUIRE the
// search's estimate of the locked tone (a few Hz, where the bin is within 25 Hz). A move within the re-mix reach
// re-mixes the history the scan still needs; a farther one (another signal) forgets it:
// - a takeover, in SEARCH and ACQUIRE: a tone that has just come up from quiet (fresh: a transmission starting while
//   another tone is held): the strongest tone (the leading bin, or the lock), even with a candidate START held
//   (`holding`); or the strongest fresh tone when nothing is held (the tone held is then a steady tone, keyed CW, or a
//   transmission joined late that V6 does not decode);
// - in SEARCH, nothing held: when the tone held is gone (a keyed signal's sidebands take the lead at times) or is a
//   steady carrier the search has masked.
// The scan then starts afresh, on a trusted origin when the tone came up fresh (the look-ahead holds the silence
// before it), else untrusted: a transmission already running is not joined (V6).
void Decoder::steer(bool holding) {
    const float held = nco_.frequency();
    float tone = 0.0f;
    bool target = false;
    if (state_ == DecoderState::acquire && locked_tone_) {
        tone = locked_hz_;
        target = fabsf(tone - held) >= k_steer_min_hz;
    } else {
        target = search_.leading(tone) && tone != held;
    }
    const bool near = target && fabsf(tone - held) <= k_remix_reach_hz;
    if (near && holding) return;
    if (near && history_.first_block() != history_.end_block() && settle_blocks_ == 0) {
        const uint32_t behind = whole_blocks(k_keep_slots * slot_blocks_);
        const uint32_t keep_from =
            scan_.next - history_.first_block() > behind ? scan_.next - behind : history_.first_block();
        retune(tone, block_fill_, keep_from);
        return;
    }
    // A candidate held, or a tone held that came up within protect_blocks_ (young), brings its own sidebands up with
    // it: the strongest tone replaces it only from outside its occupied band.
    const bool young = search_.fresh(held, protect_blocks_);
    const float reach = holding || young ? band_reach_hz_ : k_remix_reach_hz;
    bool takeover = target && fabsf(tone - held) > reach && search_.fresh(tone, fresh_blocks_);
    // Else the strongest fresh tone, when nothing is held, the tone held is not a young leader (a transmission whose
    // START the scan has yet to reach; the key clicks of CW come up again and again but do not lead) and the fresh
    // tone is at least k_takeover_ratio of it and k_takeover_excess over the leading threshold.
    float lead_tone = 0.0f;
    const bool leads = search_.leading(lead_tone) && fabsf(lead_tone - held) <= k_search_step_hz;
    float fresh_tone = 0.0f;
    if (!takeover && !holding && !(young && leads) && search_.fresh_leading(fresh_tone, fresh_blocks_) &&
        fabsf(fresh_tone - held) > k_remix_reach_hz && search_.lead_excess(fresh_tone) >= k_takeover_excess &&
        search_.lead_power(fresh_tone) >= k_takeover_ratio * search_.lead_power(held)) {
        tone = fresh_tone;
        takeover = true;
    }
    if (!takeover) {
        if (!target || holding || state_ != DecoderState::search) return;
        if (search_.following(held) && !search_.masked(held)) return;
    }
    const bool fresh = search_.fresh(tone, fresh_blocks_);
    nco_.set_frequency(tone);
    forget_history(fresh);
    acquire_block_ = history_.end_block();
}

void Decoder::restart_scan(uint32_t origin, bool trusted) {
    start_checked_ = false;
    scan_.next = origin;
    scan_.loud_until = origin;
    scan_.origin = origin;
    scan_.watch_until = origin;
    scan_.loud = false;
    scan_.in_data = false;
    scan_.trusted = trusted;
    scan_.watching = false;
    scan_.found = false;
    scan_.anchor = 0.0f;
    scan_.excess = 0.0f;
}

// Energy of [from, from + blocks) (history blocks): the coherent sums of sub-windows of about scan_group_ blocks, their
// energies added. `noise` gets what noise alone gives there on average; `groups` how many sub-windows.
float Decoder::group_energy(float from, float blocks, float& noise, uint32_t& groups) const {
    groups = whole_blocks(blocks / scan_group_);
    const float size = blocks / static_cast<float>(groups);
    const float group_noise = noise_variance() * dsp::noise_samples(size, block_samples_);
    float energy = 0.0f;
    noise = 0.0f;
    for (uint32_t g = 0; g < groups; ++g) {
        const float at = from + static_cast<float>(g) * size;
        Complex sum;
        if (!history_.window(origin_block_, at, at + size, sum)) continue;
        energy += magnitude_squared(sum);
        noise += group_noise;
    }
    return energy;
}

// Energy over noise, as a multiple of the noise, above which `groups` sub-windows hold a tone (false alarm 1e-6).
float Decoder::loud_ratio(uint32_t groups) {
    const float m = static_cast<float>(groups);
    const float root = 1.0f - k_wilson_third / m + k_loud_z * sqrtf(k_wilson_third / m);
    return root * root * root;
}

// The START of a tone whose first loud slot window is at loud_from: where the energy of its first half (from the start)
// equals that of its second half (a beep is symmetric), found by stepping one block at a time from a clear rising
// edge (a loud slot-long window, most of its energy in the later half) and interpolating. walk_back: a tone just
// before it moves it back (a weak START before a 1).
bool Decoder::locate_onset(uint32_t loud_from, bool walk_back, float& position) const {
    const float half = k_timing_half * slot_blocks_;
    const float start = relative(loud_from) - k_onset_back * slot_blocks_;
    const uint32_t steps = whole_blocks((k_onset_back + k_onset_reach) * slot_blocks_);
    float previous = 0.0f;
    bool rising = false;
    for (uint32_t step = 0; step <= steps; ++step) {
        const float q = start + static_cast<float>(step);
        float early_noise = 0.0f;
        float late_noise = 0.0f;
        uint32_t early_groups = 0;
        uint32_t late_groups = 0;
        const float early = group_energy(q, half, early_noise, early_groups) - early_noise;
        const float late = group_energy(q + half, half, late_noise, late_groups) - late_noise;
        const float balance = late - early;
        if (!rising) {
            const float noise = early_noise + late_noise;
            rising = late + early + noise >= loud_ratio(early_groups + late_groups) * noise &&
                     balance >= k_onset_rise * (late + early);
        } else if (balance <= 0.0f) {
            position = q - 1.0f + previous / (previous - balance);
            // The START is the first tone after silence: a slot before it that holds a tone means the crossing was
            // taken a slot late (a weak START followed by a 1).
            for (uint8_t back = 0; walk_back && back < k_onset_walk_slots && loud_before(position); ++back) {
                position -= slot_blocks_;
            }
            return true;
        }
        previous = balance;
    }
    return false;
}

// The half slot just before `position` (centred half a slot before it) holds a tone.
bool Decoder::loud_before(float position) const {
    float noise = 0.0f;
    uint32_t groups = 0;
    const float energy = group_energy(position - (k_half + k_onset_half) * slot_blocks_, k_half * slot_blocks_, noise,
                                      groups);
    return noise > 0.0f && energy >= loud_ratio(groups) * noise;
}

// The scan's test on one slot window from `start`: loud against the Gamma tail of its noise. `excess`: the energy over
// the noise.
bool Decoder::loud_slot(float start, float& excess) const {
    float noise = 0.0f;
    uint32_t groups = 0;
    const float energy = group_energy(start + k_scan_lead * slot_blocks_, k_slot_window * slot_blocks_, noise, groups);
    excess = energy - noise;
    return noise > 0.0f && energy >= loud_ratio(groups) * noise;
}

// The k_onset_silent_slots slots before `start` hold no tone next to one of slot energy `excess` over the noise: each
// is quiet (below the Gamma tail) or under 1 / k_precursor_rise of its energy (spec 3.3).
bool Decoder::quiet_before(float start, float excess) const {
    for (uint8_t slot = 1; slot <= k_onset_silent_slots; ++slot) {
        float before = 0.0f;
        if (loud_slot(start - static_cast<float>(slot) * slot_blocks_, before) &&
            k_precursor_rise * before >= excess) {
            return false;
        }
    }
    return true;
}

// A watched candidate (spec 3.3): a loud window at `position` k_precursor_rise times its energy is located; when its 2
// slots before are quiet next to it, it is the START and the candidate was a precursor.
void Decoder::watch(Scan& scan, uint32_t position, float excess) const {
    if (excess < k_precursor_rise * scan.excess) return;
    float start = 0.0f;
    if (!locate_onset(position, false, start) || start <= scan.anchor) return;
    float start_excess = 0.0f;
    loud_slot(start, start_excess);
    if (!quiet_before(start, start_excess)) return;
    scan.anchor = start;
    scan.excess = start_excess;
    scan.watch_until = absolute(start + k_watch_slots * slot_blocks_);
}

// One start position of the scan (spec 3.3): the slot window from `next` holds a tone (loud) or not. The first loud
// window after silence is judged at once: its START is located, then it is the anchor when the last loud window ended
// at least 2 slots before it and no beep came since the origin without a whole silent window. False when the history
// does not hold the position yet or `limit` is reached.
bool Decoder::scan_step(Scan& scan, uint32_t limit) {
    if (scan.found) return false;
    if (scan.next >= limit) {
        // The limit ends the watch: the candidate is taken as it is.
        if (!scan.watching) return false;
        scan.watching = false;
        scan.found = true;
        return true;
    }
    // What locate_onset() reads past the position (the scan's origin may lie ahead of the history: the decoder's start).
    const uint32_t reach = whole_blocks((k_onset_reach + 2.0f) * slot_blocks_);
    if (static_cast<int32_t>(scan.next + reach + k_margin_blocks - history_.end_block()) > 0) return false;
    if (!history_.holds(scan.next)) {
        // The history no longer reaches back to the scan: nothing before its oldest block is known.
        scan.next = history_.first_block();
        scan.loud_until = scan.next;
        scan.origin = scan.next;
        scan.loud = false;
        scan.trusted = false;
        scan.watching = false;
        return true;
    }
    const uint32_t p = scan.next;
    scan.next = p + 1u;
    float excess = 0.0f;
    const bool loud = loud_slot(relative(p), excess);
    if (scan.watching) {
        if (loud) watch(scan, p, excess);
        if (p >= scan.watch_until) {
            scan.watching = false;
            scan.found = true;
        }
    }
    if (!loud) {
        if (scan.in_data && p - scan.loud_until >= static_cast<uint32_t>(k_proven_silent_slots * slot_blocks_)) {
            scan.in_data = false;
            scan.trusted = true;  // a whole silent window: whatever ran before it has ended
        }
        return true;
    }
    // A loud window within half a slot of the last one belongs to the same tone (the test flickers on a weak edge).
    const bool onset = !scan.loud || static_cast<float>(p - scan.loud_until) >= k_onset_gap * slot_blocks_;
    const uint32_t quiet_from = scan.loud ? scan.loud_until : scan.origin;
    scan.loud = true;
    scan.loud_until = p + 1u;
    if (!onset || scan.found) return true;
    float start = 0.0f;
    if (!locate_onset(p, true, start)) {
        // An untrusted scan's first tone without a clear onset, with no whole silent window before it (at the
        // history's start): a transmission may be running.
        const bool unproven = static_cast<float>(p - scan.origin) < k_proven_silent_slots * slot_blocks_;
        if (!scan.trusted && quiet_from == scan.origin && unproven) scan.in_data = true;
        return true;
    }
    const float silence = start - relative(quiet_from);
    const bool silent = silence >= (static_cast<float>(k_onset_silent_slots) - k_onset_tolerance) * slot_blocks_;
    const bool proven = scan.trusted || quiet_from != scan.origin ||
                        silence >= static_cast<float>(k_proven_silent_slots) * slot_blocks_;
    if (!silent || !proven || scan.in_data) {
        scan.in_data = true;
        return true;
    }
    // A clean onset while a candidate is watched belongs to its transmission (a louder START is taken above).
    if (scan.watching) return true;
    scan.watching = true;
    scan.anchor = start;
    loud_slot(start, scan.excess);
    scan.watch_until = absolute(start + k_watch_slots * slot_blocks_);
    return true;
}

void Decoder::scan_step_all() {
    for (uint8_t i = 0; i < k_acquire_blocks_per_block; ++i) {
        if (!scan_step(scan_, history_.end_block())) break;
    }
}

// SEARCH and ACQUIRE, once per history block: the scan catches up (bounded), an anchor is verified once the history
// holds its first windows, and ACQUIRE gives up when no transmission start comes.
void Decoder::run_acquire() {
    scan_step_all();
    if ((scan_.found || scan_.watching) && state_ == DecoderState::search) enter_acquire();
    if (scan_.found) {
        float anchor = scan_.anchor;
        // A steady tone (CW, a carrier, the VOX lead) frees the pitch as soon as the history holds the first slots.
        if (!start_checked_) {
            const float first = anchor + (static_cast<float>(k_steady_slots + 1u) + k_margin_slots) * slot_blocks_;
            if (first + static_cast<float>(k_margin_blocks) > live_end()) return;
            start_checked_ = true;
            if (steady_start(anchor)) {
                verify_hz_ = nco_.frequency();
                reject(Check::steady);
            }
            return;
        }
        const float last = anchor + static_cast<float>((acquire_windows_ - 1u) * k_window_slots) * slot_blocks_;
        if (!window_ready(last)) return;
        // The check before lock, one step per block (spec 3.9): the pitch first, the windows at the next block.
        if (!pitch_refined_) {
            verify_hz_ = nco_.frequency();
            if (refine_pitch(anchor)) {
                pitch_refined_ = true;
            } else {
                reject(Check::markers);
            }
            return;
        }
        pitch_refined_ = false;
        anchor = refine_anchor(anchor);
        float reference = 0.0f;
        const Check check = windows_check(anchor, reference);
        if (check == Check::ok) {
            lock(anchor, reference);
        } else {
            reject(check);
        }
        return;
    }
    if (state_ != DecoderState::acquire) return;
    // Beeps are there that no anchor explains (a transmission already running), or the tone held has just come up
    // from quiet (a transmission starting; the scan hears it a look-ahead later): DCD stays on. A steady tone does not
    // hold ACQUIRE: it times out and is banned below.
    if (scan_.in_data || search_.fresh(nco_.frequency(), fresh_blocks_)) acquire_block_ = history_.end_block();
    const float window_ms = blocks_to_ms(static_cast<float>(k_window_slots) * slot_blocks_);
    const float timeout_ms = max_of(k_acquire_timeout_ms, static_cast<float>(k_acquire_timeout_windows) * window_ms);
    const float waited_ms = blocks_to_ms(static_cast<float>(history_.end_block() - acquire_block_));
    if (waited_ms < timeout_ms) return;
    // A tone that never began a transmission (a carrier, CW, speech) is banned while it is still there.
    if (locked_tone_ && search_.present(locked_hz_)) {
        const float blocks = k_search_ban_ms * k_samples_per_ms / static_cast<float>(dsp::ToneSearch::k_block_samples);
        search_.ban(locked_hz_, static_cast<uint16_t>(blocks));
    }
    enter_search();
}

// A candidate that did not check out (spec 3.3): the pitch goes back and the scan goes on past it, as on a steady tone
// (a VOX lead, a carrier: loud slot edges; the next onset after 2 silent slots may be the START) or inside data.
void Decoder::reject(Check check) {
    if (nco_.frequency() != verify_hz_) retune(verify_hz_, block_fill_, history_.first_block());
    scan_.found = false;
    start_checked_ = false;
    if (check == Check::markers) scan_.in_data = true;
    if (!locked_tone_) enter_search();
    acquire_block_ = history_.end_block();
}

// Spec 3.2: tone slots have no phase reversals, so blocks two apart turn by 2 pi df 2 B / fs; the sum of their
// products over the first windows (noise blocks add nothing on average) gives df modulo fs / (2 B), and the products of
// neighbouring blocks (whose noise is slightly correlated through the CIC-2, a small pull towards 0) pick the alias:
// the provisional tune may be off by more than the 62.5 Hz that 2 blocks of 32 samples leave. The history is then
// re-mixed from k_keep_slots before the anchor.
bool Decoder::refine_pitch(float anchor) {
    // The slots that hold the tone: the markers, and the data slots with at least a quarter of their energy (a 1),
    // on sub-windows (the pitch is not refined yet). An interferer the CIC-2 lets through then biases nothing in the
    // silent slots between them.
    const uint8_t slots = static_cast<uint8_t>(acquire_windows_ * k_window_slots);
    const float window = k_slot_window * slot_blocks_;
    const float lead = k_half * (1.0f - k_slot_window) * slot_blocks_;
    float density[k_max_acquire_windows * k_window_slots];
    float markers = 0.0f;
    for (uint8_t slot = 0; slot < slots; ++slot) {
        density[slot] = energy_density(anchor + static_cast<float>(slot) * slot_blocks_ + lead, window);
        const uint8_t in_window = static_cast<uint8_t>(slot % k_window_slots);
        if (in_window == k_start_slot || in_window == k_stop_slot) markers += density[slot];
    }
    const float tone = k_marker_ratio * k_marker_ratio * markers / static_cast<float>(2u * acquire_windows_);
    Complex sum;
    sum.re = 0.0f;
    sum.im = 0.0f;
    Complex near;
    near.re = 0.0f;
    near.im = 0.0f;
    float older_energy = 0.0f;
    float newer_energy = 0.0f;
    for (uint8_t slot = 0; slot < slots; ++slot) {
        const uint8_t in_window = static_cast<uint8_t>(slot % k_window_slots);
        const bool marker = in_window == k_start_slot || in_window == k_stop_slot;
        if (!marker && density[slot] < tone) continue;
        const float from = anchor + static_cast<float>(slot) * slot_blocks_ + lead;
        const uint32_t first = absolute(ceilf(from));        // the first and last blocks wholly inside
        const uint32_t last = absolute(from + window - 1.0f);
        for (uint32_t i = first; i + k_pitch_gap_blocks <= last; ++i) {
            Complex older;
            Complex middle;
            Complex newer;
            if (!history_.block(i, older) || !history_.block(i + 1u, middle) ||
                !history_.block(i + k_pitch_gap_blocks, newer)) {
                return false;
            }
            sum.re += newer.re * older.re + newer.im * older.im;
            sum.im += newer.im * older.re - newer.re * older.im;
            near.re += middle.re * older.re + middle.im * older.im;
            near.im += middle.im * older.re - middle.re * older.im;
            older_energy += magnitude_squared(older);
            newer_energy += magnitude_squared(newer);
        }
    }
    // Coherence |sum| / sqrt(E_older E_newer) (1 for a tone alone, about 1 / sqrt(count) for noise), squared: no root.
    const float coherence = k_pitch_coherence * k_pitch_coherence * older_energy * newer_energy;
    if (older_energy <= 0.0f || magnitude_squared(sum) < coherence) return false;
    const float block_s = static_cast<float>(block_samples_) / static_cast<float>(k_decoder_rate_hz);
    const float gap_s = static_cast<float>(k_pitch_gap_blocks) * block_s;
    const float coarse_hz = atan2f(near.im, near.re) / (k_two_pi * block_s);
    const float alias_hz = 1.0f / gap_s;
    float offset_hz = atan2f(sum.im, sum.re) / (k_two_pi * gap_s);
    offset_hz += alias_hz * floorf((coarse_hz - offset_hz) / alias_hz + k_half);
    // The search's estimate of a keyed tone is within its occupied band (within the re-mix reach at the slow speeds):
    // a tone farther off is another signal the CIC-2 lets through (a transmission beside an interferer held) or noise,
    // not the one the scan heard begin.
    if (fabsf(offset_hz) > refine_reach_hz_) return false;
    const float keep = anchor - k_keep_slots * slot_blocks_;
    retune(nco_.frequency() + offset_hz, block_fill_, absolute(keep > relative(history_.first_block())
                                                                   ? keep
                                                                   : relative(history_.first_block())));
    return true;
}

// The anchor moved onto the balance of the first windows' START and STOP (spec 3.3), a few steps; then back a slot at a
// time while the slot before it holds a tone as loud as half the START (the START is the first tone after silence: a
// weak START missed by the scan leaves the anchor on a later tone).
float Decoder::refine_anchor(float anchor) const {
    for (uint8_t walk = 0; walk <= k_onset_walk_slots; ++walk) {
        for (uint8_t i = 0; i < k_refine_iterations; ++i) {
            const float step = refine_timing(anchor, acquire_windows_);
            anchor += step * slot_blocks_;
            if (fabsf(step) < k_refine_settled) break;
        }
        if (walk == k_onset_walk_slots) break;
        const Window window = measure(anchor - slot_blocks_);
        if (!window.valid || window.level[k_start_slot] < k_marker_ratio * window.level[k_first_data_slot]) break;
        anchor -= slot_blocks_;
    }
    return anchor;
}

// Early/late balance of the START and STOP of `windows` windows from `anchor` (spec 3.4), in slots (late: > 0).
float Decoder::refine_timing(float anchor, uint8_t windows) const {
    float balance = 0.0f;
    float total = 0.0f;
    const float half = k_timing_half * slot_blocks_;
    for (uint8_t w = 0; w < windows; ++w) {
        for (uint8_t marker = 0; marker < 2; ++marker) {
            const uint8_t slot = marker == 0 ? k_start_slot : k_stop_slot;
            const float centre = anchor + (static_cast<float>(w * k_window_slots + slot) + k_half) * slot_blocks_;
            Complex early;
            Complex late;
            if (!history_.window(origin_block_, centre - half, centre, early) ||
                !history_.window(origin_block_, centre, centre + half, late)) {
                continue;
            }
            balance += magnitude_squared(late) - magnitude_squared(early);
            total += magnitude_squared(late) + magnitude_squared(early);
        }
    }
    if (total <= 0.0f) return 0.0f;
    return clamp(balance / total / k_timing_slope, -k_half, k_half);
}

// Windows 0 .. acquire_windows_ - 1 from `anchor` (fewer when the last ones are silent: a shorter transmission): the
// first START a beep (its trailing edge quiet: a steady tone's is not), every START and STOP present against their
// mean level, and the slot edges quiet. `reference` gets that mean.
// The early test of a candidate (spec 3.3): a steady tone goes on through the first two slot boundaries after the
// START, where beeps leave a null. Neither the pitch nor the anchor is refined yet, so the energies are those of short
// sub-windows and each boundary's edge is the quietest near it.
bool Decoder::steady_start(float anchor) const {
    const float window = k_slot_window * slot_blocks_;
    const float lead = k_half * (1.0f - k_slot_window) * slot_blocks_;  // the central 0.75 T begins 0.125 T in
    const float edge = 2.0f * k_edge_half * slot_blocks_;
    const float start = energy_density(anchor + lead, window);
    if (start <= 0.0f) return false;
    for (uint8_t slot = k_first_data_slot; slot <= k_steady_slots; ++slot) {
        const float boundary = anchor + static_cast<float>(slot) * slot_blocks_;
        if (energy_density(boundary + lead, window) < k_marker_ratio * k_marker_ratio * start) return false;
        const float first = boundary - k_edge_half * slot_blocks_;
        float quietest = energy_density(first, edge);
        for (uint8_t step = 1; step <= k_edge_search_steps; ++step) {
            const float offset = static_cast<float>(step) * k_edge_step * slot_blocks_;
            quietest = min_of(quietest, min_of(energy_density(first - offset, edge), energy_density(first + offset, edge)));
        }
        if (quietest <= k_start_edge_max * start) return false;
    }
    return true;
}

// Energy over the noise of [from, from + blocks) from its sub-windows of about scan_group_ blocks, per block and per
// block of sub-window: a steady tone gives the same value whatever the lengths. The sub-windows (10 ms) keep a tone
// that far off the pitch out (an interferer beside the tone held) and cost little for a pitch error of a few Hz.
float Decoder::energy_density(float from, float blocks) const {
    float noise = 0.0f;
    uint32_t groups = 0;
    const float energy = group_energy(from, blocks, noise, groups);
    const float size = blocks / static_cast<float>(groups);
    return max_of(energy - noise, 0.0f) / (size * blocks);
}

Decoder::Check Decoder::windows_check(float anchor, float& reference) const {
    Window windows[k_max_acquire_windows];
    for (uint8_t w = 0; w < acquire_windows_; ++w) {
        windows[w] = measure(anchor + static_cast<float>(w * k_window_slots) * slot_blocks_);
        if (!windows[w].valid) {
            return Check::markers;
        }
    }
    // A steady tone (the VOX lead is at least 3 slots) goes on through the first two slot boundaries after the START;
    // beeps leave a null at each of them. First: on a steady tone the anchor's refinement means nothing.
    const float start_density = slot_density(windows[0].sum[k_start_slot]);
    bool steady = true;
    for (uint8_t slot = k_first_data_slot; slot <= k_steady_slots; ++slot) {
        steady = steady && windows[0].level[slot] >= k_marker_ratio * windows[0].level[k_start_slot] &&
                 edge_density(anchor + static_cast<float>(slot) * slot_blocks_) > k_start_edge_max * start_density;
    }
    if (steady) return Check::steady;
    // The START is the first tone after at least 2 silent slots (spec 3.3): a tone just before it means another grid.
    const Window before = measure(anchor - static_cast<float>(k_onset_silent_slots) * slot_blocks_);
    if (!before.valid) {
        return Check::markers;
    }
    for (uint8_t slot = 0; slot < k_onset_silent_slots; ++slot) {
        if (before.snr[slot] >= k_before_snr && before.level[slot] >= k_silence_ratio * windows[0].level[k_start_slot]) {
            return Check::markers;
        }
    }
    // A transmission shorter than the check: the windows after its last byte are silent (a key pressed alone), with
    // nothing in them at all: their mean slot energy is that of noise (a weak carrier or noise burst is not silence).
    uint8_t count = acquire_windows_;
    while (count > 1 && silent_window(windows[count - 1])) --count;
    float levels = 0.0f;
    for (uint8_t w = 0; w < count; ++w) levels += windows[w].level[k_start_slot] + windows[w].level[k_stop_slot];
    reference = levels / static_cast<float>(2u * count);
    if (reference <= 0.0f) return Check::markers;
    for (uint8_t w = 0; w < count; ++w) {
        if (!marker_present(windows[w].level[k_start_slot], windows[w].snr[k_start_slot], reference) ||
            !marker_present(windows[w].level[k_stop_slot], windows[w].snr[k_stop_slot], reference)) {
            return Check::markers;
        }
    }
    return edge_ratio(anchor, count) <= k_edge_ratio_max ? Check::ok : Check::steady;
}

// Energy density (per sample squared, noise removed) of a slot's central 0.75 T sum and of the +-0.1 T edge window at
// a slot boundary: the same scale for a steady tone.
float Decoder::slot_density(const Complex& sum) const {
    const float samples = k_slot_window * slot_blocks_ * static_cast<float>(block_samples_);
    return max_of(magnitude_squared(sum) - window_noise(k_slot_window), 0.0f) / (samples * samples);
}

float Decoder::edge_density(float boundary) const {
    const float blocks = 2.0f * k_edge_half * slot_blocks_;
    const float samples = blocks * static_cast<float>(block_samples_);
    Complex sum;
    if (!history_.window(origin_block_, boundary - k_edge_half * slot_blocks_, boundary + k_edge_half * slot_blocks_,
                         sum)) {
        return 0.0f;
    }
    const float noise = noise_variance() * dsp::noise_samples(blocks, block_samples_);
    return max_of(magnitude_squared(sum) - noise, 0.0f) / (samples * samples);
}

// Energy density at the slot edges of `windows` windows (their leading edge included) over the density at their
// markers: about 1.1 for a steady tone, near 0 for beeps (each ends in its own slot).
float Decoder::edge_ratio(float start, uint8_t windows) const {
    float edges = 0.0f;
    float markers = 0.0f;
    const uint8_t boundaries = static_cast<uint8_t>(windows * k_window_slots);
    for (uint8_t b = 0; b <= boundaries; ++b) edges += edge_density(start + static_cast<float>(b) * slot_blocks_);
    const float half = k_half * k_slot_window * slot_blocks_;
    for (uint8_t w = 0; w < windows; ++w) {
        for (uint8_t marker = 0; marker < 2; ++marker) {
            const uint8_t slot = marker == 0 ? k_start_slot : k_stop_slot;
            const float centre = start + (static_cast<float>(w * k_window_slots + slot) + k_half) * slot_blocks_;
            Complex sum;
            if (history_.window(origin_block_, centre - half, centre + half, sum)) markers += slot_density(sum);
        }
    }
    const float edge_mean = edges / static_cast<float>(boundaries + 1u);
    const float marker_mean = markers / static_cast<float>(2u * windows);
    return marker_mean > 0.0f ? edge_mean / marker_mean : 1.0f;
}

// TRACK from window 0 at `anchor` (spec 3.3): `locked`, then the windows the history holds are read at once.
void Decoder::lock(float anchor, float reference) {
    window_start_ = anchor;
    byte_index_ = 0;
    drift_ = 0.0f;
    reference_ = reference;
    noise_.reset(noise_variance());
    has_last_stop_ = false;
    framing_run_ = 0;
    framing_errors_ = 0;
    pending_ = false;
    pitch_refined_ = false;
    start_checked_ = false;
    scan_.found = false;
    set_state(DecoderState::track);
    emit(make_event(EventType::locked));
}

// ---------------------------------------------------------------------------
// Tracking (spec 3.4-3.7)
// ---------------------------------------------------------------------------

// One window per block at most (spec 3.9): windows come every 10 slots, so the decoder never falls behind.
void Decoder::run_track() {
    if (state_ == DecoderState::track) track_step();
}

bool Decoder::window_ready(float start) const {
    const float end = start + (static_cast<float>(k_window_slots) + k_margin_slots) * slot_blocks_;
    return end + static_cast<float>(k_margin_blocks) <= live_end();
}

// One window when the history holds it: a silent START may end the transmission or begin a new one (resolved over the
// next windows); otherwise its byte is decided, released (or dropped as a framing error) and the loops follow it.
bool Decoder::track_step() {
    if (pending_) return resolve_pending();
    const float start = window_start_;
    if (!window_ready(start)) return false;
    const Window window = measure(start);
    if (!window.valid) {
        lose(LostReason::framing, start);
        return false;
    }
    const Decision decision = decide(window);
    if (!decision.start_present && !tone_in(window, k_start_slot, reference_)) {
        if (quiet_window(window, reference_)) {
            finish_end(start);
            return false;
        }
        pending_ = true;
        pending_new_ = false;
        pending_stage_ = 0;
        pending_start_ = start;
        pending_window_ = window;
        // The scan starts half a slot into the window (clear of the old STOP's tail), its silence from the STOP's end
        // (the last loud position).
        pending_scan_ = scan_;
        pending_scan_.next = absolute(start + k_half * slot_blocks_);
        pending_scan_.loud_until = absolute(start);
        pending_scan_.origin = pending_scan_.next;
        pending_scan_.loud = true;
        pending_scan_.in_data = false;
        pending_scan_.trusted = true;
        pending_scan_.watching = false;
        pending_scan_.found = false;
        return true;
    }
    emit_window(decision, start);
    if (!decision.start_present || !decision.stop_present) {
        ++framing_errors_;
        if (++framing_run_ >= k_max_framing_errors) {
            lose(LostReason::framing, start + static_cast<float>(k_window_slots) * slot_blocks_);
            return false;
        }
    } else {
        framing_run_ = 0;
    }
    update_loops(window, decision, start);
    ++byte_index_;
    return true;
}

// A silent START at pending_start_ (spec 3.7): either the transmission ended and a new one begins inside that window
// (back to back), or its START faded and the old one goes on. The new hypothesis: the first onset after 2 silent slots
// in the window whose windows check out. The old one: the window's STOP and the next window's START and STOP are
// there, and its slot edges are quiet (a grid not on the slots of what is heard reads loud edges). Exactly one must
// hold; while both do, one more window of each is weighed per step, up to k_pending_windows; when neither does, or
// both still do, the signal is lost: nothing is released on a grid that might be wrong.
bool Decoder::resolve_pending() {
    const float start = pending_start_;
    const float window = static_cast<float>(k_window_slots) * slot_blocks_;
    if (!pending_new_) {
        const uint32_t limit = absolute(start + window + k_half * slot_blocks_);
        if (pending_scan_.found) {
            // One candidate checked per block (spec 3.9), once the history holds the windows of the check.
            if (!window_ready(pending_scan_.anchor + static_cast<float>(acquire_windows_ - 1u) * window)) return false;
            const float anchor = refine_anchor(pending_scan_.anchor);
            float reference = 0.0f;
            pending_scan_.found = false;  // not a start: look on
            if (windows_check(anchor, reference) == Check::ok) {
                pending_new_ = true;
                pending_anchor_ = anchor;
                pending_reference_ = reference;
            }
            return false;
        }
        for (uint8_t step = 0; step < k_acquire_blocks_per_block && !pending_scan_.found; ++step) {
            if (!scan_step(pending_scan_, limit)) break;
        }
        if (pending_scan_.found || pending_scan_.next < limit) return false;
    }

    const float next = start + window + drift_;
    bool old_holds = false;
    bool new_holds = !pending_new_;
    if (pending_stage_ == 0) {
        if (!window_ready(next)) return false;
        const Window old_next = measure(next);
        old_holds = old_next.valid &&
                    marker_present(pending_window_.level[k_stop_slot], pending_window_.snr[k_stop_slot], reference_) &&
                    markers_present(old_next, reference_) && edge_ratio(next, 1) <= k_edge_ratio_max;
        new_holds = pending_new_;
    } else {
        // Stage k: the old grid's window k after `next` against the new transmission's window k + 1 (or its silence:
        // a short transmission), once the history holds both and still holds the new START.
        const float old_at = next + static_cast<float>(pending_stage_) * window;
        const float new_at = pending_anchor_ + static_cast<float>(pending_stage_ + 1u) * window;
        if (!window_ready(old_at > new_at ? old_at : new_at)) return false;
        const Window old_window = measure(old_at);
        const Window new_window = measure(new_at);
        old_holds = old_window.valid && markers_present(old_window, reference_) &&
                    edge_ratio(old_at, 1) <= k_edge_ratio_max;
        new_holds = new_window.valid && history_.holds(absolute(pending_anchor_)) &&
                    (markers_present(new_window, pending_reference_) || quiet_window(new_window, pending_reference_));
    }
    if (new_holds && old_holds && pending_stage_ < k_pending_windows) {
        ++pending_stage_;
        return false;
    }
    pending_ = false;
    if (new_holds && !old_holds) {
        finish_end(start);
        lock(pending_anchor_, pending_reference_);
    } else if (old_holds && !new_holds) {
        continue_after_pending(next);
    } else {
        lose(LostReason::framing, start);
    }
    return true;
}

// The START faded: the pending window is a framing error and the old grid goes on at `next`.
void Decoder::continue_after_pending(float next) {
    const Decision decision = decide(pending_window_);
    emit_window(decision, pending_start_);
    ++framing_errors_;
    framing_run_ = 1;
    window_start_ = next;
    has_last_stop_ = false;
    ++byte_index_;
}

// `end` (spec 3.7); the scan looks for the next transmission from the silent window on.
void Decoder::finish_end(float next_start) {
    search_.forget();
    emit(make_event(EventType::end));
    pending_ = false;
    enter_search();
    restart_scan(absolute(next_start), true);
}

// `lost` (spec 3.6): a signal is still there, so the scan waits for a silent window before the next anchor.
void Decoder::lose(LostReason reason, float from) {
    Event event = make_event(EventType::lost);
    event.reason = reason;
    emit(event);
    pending_ = false;
    enter_search();
    restart_scan(absolute(from), false);
    scan_.in_data = true;
}

// Spec 3.3-3.5: the coherent sum of each slot's central 0.75 T; its level in crest units with the noise removed.
Decoder::Window Decoder::measure(float start) const {
    Window window;
    window.valid = true;
    const float half = k_half * k_slot_window * slot_blocks_;
    const float noise = window_noise(k_slot_window);
    for (uint8_t j = 0; j < k_window_slots; ++j) {
        const float centre = start + (static_cast<float>(j) + k_half) * slot_blocks_;
        Complex sum;
        if (!history_.window(origin_block_, centre - half, centre + half, sum)) {
            memset(&window, 0, sizeof(window));
            window.valid = false;
            return window;
        }
        window.sum[j] = sum;
        window.snr[j] = magnitude_squared(sum) / noise;
        window.level[j] = slot_level(sum);
    }
    return window;
}

// Spec 3.5: the reference line from the START level to the STOP level; each data slot against the decision line.
Decoder::Decision Decoder::decide(const Window& window) const {
    Decision d;
    memset(&d, 0, sizeof(d));
    d.start_present = marker_present(window.level[k_start_slot], window.snr[k_start_slot], reference_);
    d.stop_present = marker_present(window.level[k_stop_slot], window.snr[k_stop_slot], reference_);
    d.start_pct = percent_of(window.level[k_start_slot], reference_);
    d.stop_pct = percent_of(window.level[k_stop_slot], reference_);
    const float slot_noise = k_noise_amplitude * window_noise(k_slot_window) /
                             ((k_slot_window * slot_blocks_ * static_cast<float>(block_samples_) * k_g_slot) *
                              (k_slot_window * slot_blocks_ * static_cast<float>(block_samples_) * k_g_slot));
    // The raw floor of v0.3 (2.6 sigma of an empty slot), in the noise-free level's terms.
    const float floor = sqrtf(max_of(k_floor_sigma * k_floor_sigma - 1.0f, 0.0f) * slot_noise);
    const float start = window.level[k_start_slot];
    const float stop = window.level[k_stop_slot];
    bool weak = false;
    for (uint8_t i = 0; i < k_bits_per_byte; ++i) {
        const uint8_t slot = static_cast<uint8_t>(k_first_data_slot + i);
        const float reference = start + (stop - start) * static_cast<float>(slot) / static_cast<float>(k_stop_slot);
        float threshold = 0.0f;
        if (config_.decision_mode == DecisionMode::adaptive) {
            // v0.3's smart line compares the raw level |S| with rho * ref; here the level has the noise removed.
            const float rho = dsp::equal_likelihood_ratio(2.0f * reference * reference / max_of(slot_noise, 1e-20f));
            threshold = sqrtf(max_of(rho * rho * reference * reference - slot_noise, 0.0f));
        } else {
            threshold = static_cast<float>(config_.threshold_percent) / k_percent * reference;
        }
        threshold = max_of(threshold, floor);
        const float level = window.level[slot];
        if (level >= threshold) d.value = static_cast<uint8_t>(d.value | (k_top_bit >> i));
        const float margin = threshold > 0.0f ? (level - threshold) / threshold : 0.0f;
        d.soft[i] = static_cast<int8_t>(round_to_int(clamp(k_soft_scale * margin, -k_soft_limit, k_soft_limit)));
        if (fabsf(margin) < k_weak_margin) weak = true;
        d.level_pct[i] = percent_of(level, reference);
        d.threshold_pct[i] = percent_of(threshold, reference);
    }
    if (weak) d.flags |= event_flag_weak;
    if (!d.start_present || !d.stop_present) d.flags |= event_flag_framing;
    return d;
}

// Slot events for the 8 data slots, then the byte (not for a framing error: its byte is dropped).
void Decoder::emit_window(const Decision& decision, float start) {
    uint8_t flags = decision.flags;
    const float end = start + static_cast<float>(k_window_slots) * slot_blocks_;
    if (history_.any_blanked(origin_block_, start, end)) flags |= event_flag_blanked;
    for (uint8_t i = 0; i < k_bits_per_byte; ++i) {
        Event event = make_event(EventType::slot);
        event.value = static_cast<uint8_t>((decision.value >> (k_bits_per_byte - 1u - i)) & 1u);
        event.slot = static_cast<uint8_t>(k_first_data_slot + i);
        event.level_pct = decision.level_pct[i];
        event.threshold_pct = decision.threshold_pct[i];
        event.start_pct = decision.start_pct;
        event.stop_pct = decision.stop_pct;
        event.soft[0] = decision.soft[i];
        event.flags = flags;
        emit(event);
    }
    if ((flags & event_flag_framing) != 0) return;
    Event event = make_event(EventType::byte);
    event.value = decision.value;
    event.flags = flags;
    event.start_pct = decision.start_pct;
    event.stop_pct = decision.stop_pct;
    memcpy(event.soft, decision.soft, sizeof(event.soft));
    emit(event);
}

// After a window (spec 3.4): the next window's start from the timing loop (a PI loop on the early/late balance of the
// tone slots), the pitch from the carrier's turn between markers, the reference from the markers, the noise from the
// central half of the clear zeros.
void Decoder::update_loops(const Window& window, const Decision& decision, float start) {
    const float error = clamp(timing_error(start, decision), -k_timing_clamp, k_timing_clamp);
    drift_ += k_timing_integral * error * slot_blocks_;
    window_start_ = start + static_cast<float>(k_window_slots) * slot_blocks_ + drift_ +
                    k_timing_gain * error * slot_blocks_;

    const float slot_s = slot_blocks_ * static_cast<float>(block_samples_) / static_cast<float>(k_decoder_rate_hz);
    if (decision.start_present && decision.stop_present) {
        // START to STOP turns by 2 pi df 9 T (fine, ambiguous beyond 1 / (18 T)); the previous STOP to this START by
        // 2 pi df T resolves it.
        const float fine = phase_between(window.sum[k_stop_slot], window.sum[k_start_slot]);
        if (has_last_stop_) {
            const float coarse_hz = phase_between(window.sum[k_start_slot], last_stop_) / (k_two_pi * slot_s);
            const float gap_s = static_cast<float>(k_stop_gap_slots) * slot_s;
            const float turns = floorf(coarse_hz * gap_s - fine / k_two_pi + k_half);
            const float offset_hz = (fine + k_two_pi * turns) / (k_two_pi * gap_s);
            const float limit_hz = k_pitch_clamp / slot_s;
            nco_.adjust_frequency(clamp(k_pitch_gain * offset_hz, -limit_hz, limit_hz));
        }
    }
    if (decision.stop_present) {
        last_stop_ = window.sum[k_stop_slot];
        has_last_stop_ = true;
    } else {
        has_last_stop_ = false;
    }
    if (decision.start_present) reference_ += k_reference_alpha * (window.level[k_start_slot] - reference_);
    if (decision.stop_present) reference_ += k_reference_alpha * (window.level[k_stop_slot] - reference_);

    const float half = k_half * k_noise_window * slot_blocks_;
    const float samples = dsp::noise_samples(k_noise_window * slot_blocks_, block_samples_);
    for (uint8_t i = 0; i < k_bits_per_byte; ++i) {
        // Only clear zeros: a one decided 0 (a high line, a weak signal) would feed its tone to the noise estimate,
        // whose rise then lowers every slot's SNR (a false end).
        if ((decision.value & (k_top_bit >> i)) != 0 || decision.level_pct[i] >= k_noise_zero_pct) continue;
        const float centre = start + (static_cast<float>(k_first_data_slot + i) + k_half) * slot_blocks_;
        Complex sum;
        if (!history_.window(origin_block_, centre - half, centre + half, sum) ||
            history_.any_blanked(origin_block_, centre - half, centre + half)) {
            continue;
        }
        noise_.push(magnitude_squared(sum) / samples);
    }
}

// Early/late balance of the window's tone slots (the markers present and the decided ones), in slots (late: > 0).
float Decoder::timing_error(float start, const Decision& decision) const {
    float balance = 0.0f;
    float total = 0.0f;
    const float half = k_timing_half * slot_blocks_;
    for (uint8_t slot = 0; slot < k_window_slots; ++slot) {
        bool tone = false;
        if (slot == k_start_slot) {
            tone = decision.start_present;
        } else if (slot == k_stop_slot) {
            tone = decision.stop_present;
        } else {
            tone = (decision.value & (k_top_bit >> (slot - k_first_data_slot))) != 0;
        }
        if (!tone) continue;
        const float centre = start + (static_cast<float>(slot) + k_half) * slot_blocks_;
        Complex early;
        Complex late;
        if (!history_.window(origin_block_, centre - half, centre, early) ||
            !history_.window(origin_block_, centre, centre + half, late)) {
            continue;
        }
        balance += magnitude_squared(late) - magnitude_squared(early);
        total += magnitude_squared(late) + magnitude_squared(early);
    }
    if (total <= 0.0f) return 0.0f;
    return balance / total / k_timing_slope;
}

bool Decoder::quiet_window(const Window& window, float reference) const {
    for (uint8_t j = 0; j < k_window_slots; ++j) {
        if (tone_in(window, j, reference)) return false;
    }
    return true;
}

bool Decoder::silent_window(const Window& window) const {
    float snr = 0.0f;
    for (uint8_t j = 0; j < k_window_slots; ++j) snr += window.snr[j];
    return snr < k_silent_mean_snr * static_cast<float>(k_window_slots);
}

bool Decoder::markers_present(const Window& window, float reference) const {
    return marker_present(window.level[k_start_slot], window.snr[k_start_slot], reference) &&
           marker_present(window.level[k_stop_slot], window.snr[k_stop_slot], reference);
}

// A slot holds a tone (spec 3.3, 3.7): above the noise (|S|^2 >= 16 times its noise) and above k_silence_ratio of the
// tone's level (a strong beep's tail leaks a little into the next slot).
bool Decoder::tone_in(const Window& window, uint8_t slot, float reference) const {
    return window.snr[slot] >= k_loud_snr && window.level[slot] >= k_silence_ratio * reference;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

float Decoder::slot_level(const Complex& sum) const {
    const float samples = k_slot_window * slot_blocks_ * static_cast<float>(block_samples_);
    const float energy = magnitude_squared(sum) - window_noise(k_slot_window);
    return 2.0f * sqrtf(max_of(energy, 0.0f)) / (samples * k_g_slot);
}

float Decoder::window_noise(float slots) const {
    return noise_variance() * dsp::noise_samples(slots * slot_blocks_, block_samples_);
}

float Decoder::noise_variance() const {
    const float estimate = state_ == DecoderState::track ? noise_.mean_estimate() : search_noise_;
    return max_of(estimate, k_min_noise_variance);
}

float Decoder::live_end() const {
    return static_cast<float>(history_.end_block() - origin_block_);
}

float Decoder::blocks_to_ms(float blocks) const {
    const uint8_t samples = block_samples_ > 0 ? block_samples_ : k_min_block_samples;
    return blocks * static_cast<float>(samples) / k_samples_per_ms;
}

uint32_t Decoder::absolute(float position) const {
    return origin_block_ + static_cast<uint32_t>(static_cast<int32_t>(floorf(position)));
}

float Decoder::relative(uint32_t block) const {
    return static_cast<float>(static_cast<int32_t>(block - origin_block_));
}

bool Decoder::marker_present(float level, float snr, float reference) const {
    return level >= k_marker_ratio * reference && snr >= k_marker_snr;
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
    event.byte_index = byte_index_;
    event.tone_hz = tone_hz();
    event.slot_ms = slot_ms();
    event.snr_db = snr_db();
    return event;
}

void Decoder::emit(const Event& event) {
    if (handler_ != 0) handler_(event, context_);
}

}  // namespace unlimited
