#pragma once

#include "unlimited/platform.hpp"

namespace unlimited {

// Transmission (spec 2.1): tune tone, sync train, an 8-slot mode header, then frames of a START marker and N
// data peaks; the STOP of a frame is the START of the next. N = k_min_data_slots << (header N code).
static const uint8_t k_min_sync_markers = 8;
static const uint8_t k_max_sync_markers = 32;
static const uint8_t k_eot_markers = 2;
static const uint8_t k_header_slots = 8;
static const uint8_t k_min_data_slots = 8;
static const uint8_t k_max_data_slots = 32;
// Defaults of EncoderConfig() and the presets.
static const uint8_t k_default_sync_markers = 8;
static const uint8_t k_default_data_slots = 8;
static const uint8_t k_max_bits_per_peak = 8;
static const uint8_t k_bits_per_byte = 8;

// Tone grid (spec 1.3): data tone n at f_ref + side * (k_grid_guard + n * c) / T, c = 8/7 (standard) or
// 1 (dense); header tone h at f_ref + side * (k_grid_guard + h * 8/7) / T in every mode.
static const uint8_t k_grid_guard = 5;
static const uint8_t k_standard_spacing_num = 8;  // 8 / (7 T): spectral zero of Tukey alpha 0.25
static const uint8_t k_standard_spacing_den = 7;

// Waveform, as fractions of the slot T; energies in T * A^2 / 2 units.
static const float k_tukey_ramp = 0.25f;  // markers and tune: Tukey alpha 0.5
static const float k_reversal_start = 0.375f;
static const float k_reversal_width = 0.25f;
static const float k_marker_energy = 0.5625f;
static const float k_data_ramp = 0.125f;  // data and header peaks: Tukey alpha 0.25
static const float k_peak_energy = 0.84375f;

// Speed is chosen by the sender only: T is a whole number of ms and (N + 1) T <= k_max_frame_us. A decoder
// accepts T in [T_min, k_speed_span * T_min].
static const uint32_t k_min_slot_us = 6000;  // the header needs 13 / T <= 2400 Hz
static const uint32_t k_max_slot_us = 128000;
static const uint32_t k_slot_quantum_us = 1000;
static const uint32_t k_max_frame_us = 1152000;
static const uint32_t k_min_dense_slot_us = 32000;  // G2: at T16 k5 dense the span leaves 150 Hz of SSB passband
static const uint8_t k_speed_span = 8;

// f_ref and every data and header tone lie in [k_min_tone_hz, k_max_tone_hz].
static const uint16_t k_min_tone_hz = 300;
static const uint16_t k_max_tone_hz = 2700;
static const uint16_t k_band_centre_hz = 1500;  // HF presets centre the occupied band here
static const uint16_t k_fm_tone_hz = 2650;

static const uint16_t k_default_tune_ms = 250;
static const uint16_t k_default_fm_lead_in_ms = 300;
static const uint16_t k_default_tail_ms = 100;

// Quarter-wave sine, 256 steps plus the end point, scaled to 65534 (twice the Q15 full scale): linear interpolation
// with one 16 x 16 multiply stays within 1 LSB of 32767 sin; phase is a full turn over 2^32.
static const uint8_t k_quarter_table_bits = 8;
static const uint16_t k_quarter_table_size = (1u << k_quarter_table_bits) + 1;
extern const uint16_t k_quarter_sine[k_quarter_table_size] UNLIMITED_ROM;

int16_t sine_q15(uint32_t phase);
int16_t cosine_q15(uint32_t phase);

enum class Spacing : uint8_t { standard, dense };  // 8 / (7 T) | 1 / T
enum class GridSide : uint8_t { above, below };    // as sent; USB/LSB inversion flips it at the receiver

// Mode header (spec 2.2): word = (k - 1) | (T_ms mod 8) << 3 | spacing << 6 | N code << 7, N code 0/1/2 =
// 8/16/32 data slots, 3 reserved. Sent as 8 tones, one per slot, of an RS(8,3) + x^3 coset code over GF(8).
static const uint16_t k_header_words = 512;
static const uint8_t k_header_slot_ms_modulo = 8;

struct HeaderFields {
    uint8_t bits_per_peak;   // 1..8
    uint8_t data_slots;      // 8, 16, 32; 0 = reserved N code
    uint8_t slot_ms_residue; // T_ms mod 8
    Spacing spacing;
};

uint16_t header_word(uint8_t bits_per_peak, uint8_t data_slots, uint32_t slot_us, Spacing spacing);
HeaderFields header_fields(uint16_t word);
uint8_t header_symbol(uint16_t word, uint8_t slot);  // tone 0..7 of header slot 0..7

// Data mapping (spec 1.5): slot i = 1..N of a frame carries symbol s on tone (gray^-1(s) + (i - 1) r) mod M,
// M = 2^k, r = tone_rotation(k), so de-rotated neighbouring tones differ in one bit. `slot` below is i - 1.
uint8_t gray_encode(uint8_t value);
uint8_t gray_decode(uint8_t value);
uint8_t tone_rotation(uint8_t bits_per_peak);  // (M / 8) | 1
uint8_t peak_tone(uint8_t symbol, uint8_t slot, uint8_t bits_per_peak);
uint8_t peak_symbol(uint8_t tone, uint8_t slot, uint8_t bits_per_peak);

}  // namespace unlimited
