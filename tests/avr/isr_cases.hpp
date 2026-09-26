#pragma once

#include "unlimited/encoder.hpp"

// The transmissions of the AVR ISR gate (spec 8.6 B5): the five presets, T 128 ms (the longest slot) at 2700 Hz in
// the AM passband, and N = 1 and N = 32 at 16 ms, each with tx_uno's 100 ms lead-in. Shared by the AVR harness and
// the host program that checks it.
namespace unlimited {
namespace avr_isr {

static const uint8_t k_cases = 8;
static const uint32_t k_rate_hz = 8000;
static const uint16_t k_lead_in_ms = 100;
static const uint8_t k_bytes = 40;
static const uint8_t k_byte_step = 37;
static const uint8_t k_byte_offset = 11;
static const uint8_t k_preset_cases = 5;
static const uint8_t k_slow_case = 5;
static const uint32_t k_slow_slot_us = 128000;
static const uint16_t k_slow_tone_hz = 2700;
static const uint16_t k_wide_low_hz = 100;
static const uint16_t k_wide_high_hz = 3000;
static const uint8_t k_single_case = 6;
static const uint8_t k_long_case = 7;
static const uint32_t k_package_slot_us = 16000;
static const uint8_t k_single_bits = 1;
static const uint8_t k_long_bits = 32;

inline EncoderConfig case_config(uint8_t index) {
    const Preset presets[k_preset_cases] = {Preset::hf_slow, Preset::hf, Preset::hf_fast, Preset::am, Preset::fm};
    EncoderConfig config = EncoderConfig::from_preset(index < k_preset_cases ? presets[index] : Preset::hf, k_rate_hz);
    if (index == k_slow_case) {
        config.slot_us = k_slow_slot_us;
        config.tone_hz = k_slow_tone_hz;
        config.passband.low_hz = k_wide_low_hz;
        config.passband.high_hz = k_wide_high_hz;
    } else if (index == k_single_case || index == k_long_case) {
        config.slot_us = k_package_slot_us;
        config.bits_per_package = index == k_single_case ? k_single_bits : k_long_bits;
    }
    if (config.lead_in_ms < k_lead_in_ms) config.lead_in_ms = k_lead_in_ms;
    return config;
}

inline uint8_t case_byte(uint8_t index) {
    return static_cast<uint8_t>(index * k_byte_step + k_byte_offset);
}

}  // namespace avr_isr
}  // namespace unlimited
