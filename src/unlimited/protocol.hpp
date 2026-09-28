#pragma once

#include "unlimited/platform.hpp"

namespace unlimited {

// The byte window (spec 1.2): slot 0 is the START tone, slots 1..8 carry the bits of one byte (most significant bit
// first; a beep is 1, silence is 0), slot 9 is the STOP tone. Windows follow each other with no gap.
static const uint8_t k_bits_per_byte = 8;
static const uint8_t k_window_slots = 10;
static const uint8_t k_start_slot = 0;
static const uint8_t k_first_data_slot = 1;
static const uint8_t k_stop_slot = 9;

// Speed (spec 1.3): B bytes per second, the same on both sides, 1.00..25.00 with a resolution of 0.01 (centi-bytes
// per second, "centi" below). Slot T = 1 / (10 B): T_us = round(10^7 / centi), 4,000..100,000 us.
static const uint16_t k_min_centi_bytes_per_second = 100;
static const uint16_t k_max_centi_bytes_per_second = 2500;
static const uint16_t k_default_centi_bytes_per_second = 600;  // 6 bytes/s: the HF default
static const uint16_t k_centi_per_unit = 100;
static const uint32_t k_centi_slot_numerator_us = 10000000;    // 10^6 us / (10 slots per byte) * 100 centi
static const float k_min_bytes_per_second = 1.0f;
static const float k_max_bytes_per_second = 25.0f;
static const float k_default_bytes_per_second = 6.0f;
static const uint32_t k_min_slot_us = 4000;    // 25 bytes/s
static const uint32_t k_max_slot_us = 100000;  // 1 byte/s

// Waveform (spec 1.1): every tone slot (START, STOP, a data 1, the VOX lead's inner slots aside) is a Tukey beep with
// alpha 0.5: cosine ramps over the first and last quarter of the slot, flat in between. Energy in T * A^2 / 2 units.
static const float k_tukey_ramp = 0.25f;
static const float k_beep_energy = 0.6875f;

// Pitch (spec 1.1).
static const uint16_t k_min_tone_hz = 300;
static const uint16_t k_max_tone_hz = 2700;
static const uint16_t k_default_tone_hz = 1500;

// Occupied band (spec 1.3, v0.3's formulas), in thousandths of a cycle per slot: width_hz = k * 1000 / slot_us.
// 99 %: 99 % of a beep's energy (4.34 / T measured, rounded up); -26 dB and -40 dB: the widths outside which a
// beep's spectrum stays that far below its centre.
static const uint32_t k_band_99_milli = 4400;
static const uint32_t k_band_26db_milli = 7000;
static const uint32_t k_band_40db_milli = 9900;

// Receiver audio passbands (spec 1.3).
static const uint16_t k_ssb_passband_low_hz = 300;  // 2.4 kHz SSB filter: the default
static const uint16_t k_ssb_passband_high_hz = 2700;
static const uint16_t k_am_passband_low_hz = 100;
static const uint16_t k_am_passband_high_hz = 3000;
static const uint16_t k_fm_passband_low_hz = 300;
static const uint16_t k_fm_passband_high_hz = 3000;
static const uint16_t k_max_passband_hz = 4000;  // half the decoder's 8 kHz input rate

// Transmission (spec 2.1): the VOX lead (only when configured) is a steady tone of at least k_min_vox_lead_slots,
// then k_vox_gap_slots of silence before the first START; the tail is max(tail_ms, k_min_tail_slots) of silence.
// Three slots: the receiver tells a steady tone from beeps by its first two slot boundaries (no null at either).
static const uint8_t k_vox_gap_slots = 2;
static const uint8_t k_min_vox_lead_slots = 3;
static const uint8_t k_min_tail_slots = 2;
static const uint16_t k_default_vox_lead_ms = 150;  // the programs' default with a VOX radio; the library's is 0
static const uint16_t k_default_tail_ms = 100;

// Quarter-wave sine, 256 steps plus the end point, scaled to 65534 (twice the Q15 full scale): linear
// interpolation with one 16 x 16 multiply stays within 1 LSB of 32767 sin; phase is a full turn over 2^32.
static const uint8_t k_quarter_table_bits = 8;
static const uint16_t k_quarter_table_size = (1u << k_quarter_table_bits) + 1;
extern const uint16_t k_quarter_sine[k_quarter_table_size] UNLIMITED_ROM;

int16_t sine_q15(uint32_t phase);
int16_t cosine_q15(uint32_t phase);

// Speed arithmetic (spec 1.3). The float forms round B to 0.01 first; the integer forms are for the AVR (no float).
// A speed outside 1.00..25.00 gives a slot outside k_min_slot_us..k_max_slot_us, which check() refuses.
uint16_t centi_bytes_per_second(float bytes_per_second);  // round(100 B), clipped to 0..65535
uint32_t slot_us_for_speed(float bytes_per_second);       // round(10^7 / round(100 B)); 0 when B rounds to 0
uint32_t slot_us_for_centi_speed(uint16_t centi);         // round(10^7 / centi); 0 when centi is 0
float bytes_per_second(uint32_t slot_us);                 // 100000 / slot_us; 0 when slot_us is 0
bool slot_valid(uint32_t slot_us);                        // k_min_slot_us <= slot_us <= k_max_slot_us

struct Band {
    uint16_t low_hz;
    uint16_t high_hz;
    uint16_t width_hz;  // high_hz - low_hz
};

struct Passband {
    uint16_t low_hz;
    uint16_t high_hz;
};

// How a signal sits in a receiver (spec 1.3): the radio may be mistuned so the tone moves down by margin_low_hz or
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
// The pure filter fit: margin_low_hz = band low - passband low, margin_high_hz = passband high - band high.
PassbandFit passband_fit(const Band& band, const Passband& passband);
// The pitches a receiver of slot slot_us searches (spec 3.2): its passband less half the occupied band at that slot,
// within [k_min_tone_hz, k_max_tone_hz]; empty (low_hz > high_hz) when nothing is left.
Passband search_range(const Passband& passband, uint32_t slot_us);
// The shift tolerance (spec 1.3): the filter fit of occupied_band(tone_hz, slot_us) in passband; when it fits, each
// margin also ends where the pitch would leave `search`, the pitches the receiver searches (never below 0).
PassbandFit passband_fit(uint16_t tone_hz, uint32_t slot_us, const Passband& passband, const Passband& search);

// Why EncoderConfig::check() or DecoderConfig::check() refuses a configuration: the first rule broken, each check
// testing its own rules in the order of spec 5.
enum class ConfigError : uint8_t {
    none,
    sample_rate,       // encoder: outside k_min_sample_rate_hz..k_max_sample_rate_hz
    tone,              // encoder: tone outside [k_min_tone_hz, k_max_tone_hz]
    slot,              // both: T outside k_min_slot_us..k_max_slot_us (the speed outside 1..25 bytes/s)
    passband,          // both: not passband_valid(); decoder: no tone left to search in it
    outside_passband,  // encoder: the occupied band does not fit the passband
    amplitude,         // encoder: not > 0
    threshold,         // decoder: threshold_percent outside k_min_threshold_percent..k_max_threshold_percent
    decision_mode      // decoder: not a DecisionMode value
};

}  // namespace unlimited
