#pragma once

#include "unlimited/platform.hpp"

// Largest N (bits per package) this build sends and decodes (spec 1.6, 3.14). The decoder grows with it (about
// 0.57 KB per bit). Define it for the whole build only (-DUNLIMITED_MAX_BITS_PER_PACKAGE=N, on Arduino a
// build property), never in one source file.
#ifndef UNLIMITED_MAX_BITS_PER_PACKAGE
#if defined(ARDUINO)
#define UNLIMITED_MAX_BITS_PER_PACKAGE 16
#else
#define UNLIMITED_MAX_BITS_PER_PACKAGE 32
#endif
#endif

namespace unlimited {

static_assert(UNLIMITED_MAX_BITS_PER_PACKAGE >= 16 && UNLIMITED_MAX_BITS_PER_PACKAGE <= 64,
              "UNLIMITED_MAX_BITS_PER_PACKAGE must be 16..64");

// Transmission (spec 2.1): lead-in, tune tone, sync train (its last marker is the first START), packages of
// N bits each closed by a STOP that is also the next START, END markers, tail. Everything on one pitch.
static const uint8_t k_min_sync_markers = 8;
static const uint8_t k_max_sync_markers = 32;
static const uint8_t k_default_sync_markers = 8;
static const uint8_t k_end_markers = 2;
static const uint8_t k_min_tune_slots = 6;
static const uint8_t k_min_bits_per_package = 1;
static const uint8_t k_max_bits_per_package = UNLIMITED_MAX_BITS_PER_PACKAGE;
static const uint8_t k_hf_bits_per_package = 8;     // HF presets and EncoderConfig()
static const uint8_t k_wide_bits_per_package = 16;  // AM and FM presets
static const uint8_t k_bits_per_byte = 8;

// Waveform (spec 1.1), as fractions of the slot T; energies in T * A^2 / 2 units. Data "1" and markers share the
// Tukey alpha 0.5 envelope; a marker adds the twist, a shaped 180 degree phase reversal in the middle of its slot.
static const float k_tukey_ramp = 0.25f;
static const float k_reversal_start = 0.375f;
static const float k_reversal_width = 0.25f;
static const float k_one_energy = 0.6875f;
static const float k_marker_energy = 0.5625f;

// Speed (spec 1.4): the sender picks T and N; a receiver accepts T in [T_min, k_speed_span * T_min] and learns
// both from the signal.
static const uint32_t k_min_slot_us = 4000;
static const uint32_t k_max_slot_us = 128000;
static const uint32_t k_max_package_us = 1152000;  // (N + 1) T: START to STOP
static const uint32_t k_fast_slot_us = 8000;       // below it: FM-like channels only, tone >= k_min_fast_tone_hz
static const uint8_t k_speed_span = 8;
// A receiver's T_min (DecoderConfig::min_slot_ms) lies in k_min_window_slot_ms..k_max_window_slot_ms: its window is
// min_slot_ms .. k_speed_span * min_slot_ms.
static const uint8_t k_min_window_slot_ms = 4;
static const uint8_t k_max_window_slot_ms = 32;

// Pitch (spec 1.3).
static const uint16_t k_min_tone_hz = 300;
static const uint16_t k_max_tone_hz = 2700;
static const uint16_t k_default_tone_hz = 1500;
static const uint16_t k_min_fast_tone_hz = 1000;

// Occupied band (spec 1.5), in thousandths of a cycle per slot: width_hz = k * 1000 / slot_us. 99 %: 99 % of a
// data slot's energy (4.34 / T measured, rounded up); -26 dB and -40 dB: the widths outside which a data slot's
// spectrum stays that far below its centre.
static const uint32_t k_band_99_milli = 4400;
static const uint32_t k_band_26db_milli = 7000;
static const uint32_t k_band_40db_milli = 9900;

// Receiver audio passbands (spec 1.5).
static const uint16_t k_ssb_passband_low_hz = 300;  // 2.4 kHz SSB filter: the default
static const uint16_t k_ssb_passband_high_hz = 2700;
static const uint16_t k_am_passband_low_hz = 100;
static const uint16_t k_am_passband_high_hz = 3000;
static const uint16_t k_fm_passband_low_hz = 300;
static const uint16_t k_fm_passband_high_hz = 3000;
static const uint16_t k_max_passband_hz = 4000;  // half the decoder's 8 kHz input rate

static const uint16_t k_default_tune_ms = 250;
static const uint16_t k_default_fm_lead_in_ms = 300;
static const uint16_t k_default_tail_ms = 100;

// Quarter-wave sine, 256 steps plus the end point, scaled to 65534 (twice the Q15 full scale): linear
// interpolation with one 16 x 16 multiply stays within 1 LSB of 32767 sin; phase is a full turn over 2^32.
static const uint8_t k_quarter_table_bits = 8;
static const uint16_t k_quarter_table_size = (1u << k_quarter_table_bits) + 1;
extern const uint16_t k_quarter_sine[k_quarter_table_size] UNLIMITED_ROM;

int16_t sine_q15(uint32_t phase);
int16_t cosine_q15(uint32_t phase);

struct Band {
    uint16_t low_hz;
    uint16_t high_hz;
    uint16_t width_hz;  // high_hz - low_hz
};

struct Passband {
    uint16_t low_hz;
    uint16_t high_hz;
};

// How a signal sits in a receiver (spec 1.5): the radio may be mistuned so the tone moves down by margin_low_hz or
// up by margin_high_hz; a negative margin is how far the band sticks out of the passband on that side.
struct PassbandFit {
    bool fits;              // the occupied band lies inside the passband
    int16_t margin_low_hz;
    int16_t margin_high_hz;
    uint16_t tolerance_hz;  // min(margin_low_hz, margin_high_hz) when it fits, else 0
};

// Integer only (AVR). A tone of tone_hz sent with slot slot_us occupies tone +- half, half = ceil(k_band_99_milli *
// 500 / slot_us), clipped to 0..65535 Hz.
Band occupied_band(uint16_t tone_hz, uint32_t slot_us);
uint16_t width_26db_hz(uint32_t slot_us);  // ceil(k_band_26db_milli * 1000 / slot_us)
uint16_t width_40db_hz(uint32_t slot_us);
bool passband_valid(const Passband& passband);  // low_hz < high_hz <= k_max_passband_hz
// The pure filter fit: margin_low_hz = band low - passband low, margin_high_hz = passband high - band high. It knows
// nothing of the pitches a receiver searches, so its margins may promise more mistuning than a receiver follows: the
// shift tolerance is the four-argument passband_fit() below.
PassbandFit passband_fit(const Band& band, const Passband& passband);
// The pitches a receiver whose shortest slot is min_slot_us searches (spec 1.5, V14): its passband less half the
// occupied band at its slowest slot (k_speed_span * min_slot_us), within [k_min_tone_hz, k_max_tone_hz], and from
// k_min_fast_tone_hz when min_slot_us < k_fast_slot_us; empty (low_hz > high_hz) when nothing is left.
Passband search_range(const Passband& passband, uint32_t min_slot_us);
// The shift tolerance (spec 1.5): the filter fit of occupied_band(tone_hz, slot_us) in passband; when it fits, each
// margin also ends where the pitch would leave `search`, the pitches the receiver searches (never below 0).
PassbandFit passband_fit(uint16_t tone_hz, uint32_t slot_us, const Passband& passband, const Passband& search);

// Why EncoderConfig::check() or DecoderConfig::check() refuses a configuration: the first rule broken, each check
// testing its own rules in the order of spec 5.1.
enum class ConfigError : uint8_t {
    none,
    sample_rate,       // encoder: outside k_min_sample_rate_hz..k_max_sample_rate_hz
    tone,              // encoder: tone outside [k_min_tone_hz, k_max_tone_hz]
    slot,              // encoder: T outside k_min_slot_us..k_max_slot_us
    fast_tone,         // encoder: T < k_fast_slot_us needs a tone >= k_min_fast_tone_hz
    bits_per_package,  // encoder: N outside k_min_bits_per_package..k_max_bits_per_package
    package_length,    // encoder: (N + 1) T > k_max_package_us
    passband,          // both: not passband_valid(); decoder: no tone left to search in it
    outside_passband,  // encoder: the occupied band does not fit the passband
    sync_markers,      // encoder: outside k_min_sync_markers..k_max_sync_markers
    amplitude,         // encoder: not > 0
    min_slot,          // decoder: min_slot_ms outside k_min_window_slot_ms..k_max_window_slot_ms
    decision_mode,     // decoder: not a DecisionMode value
    fixed_ratio        // decoder: fixed_ratio outside (0, 1)
};

}  // namespace unlimited
