#pragma once

#include "unlimited/encoder.hpp"

// The transmissions of the AVR ISR gate (spec 8.7 B5): the presets, and T 128 ms k 8 dense (the widest peak), each
// with tx_uno's 100 ms lead-in. Shared by the AVR harness and the host program that checks it.
namespace unlimited {
namespace avr_isr {

static const uint8_t k_cases = 7;
static const uint32_t k_rate_hz = 8000;
static const uint16_t k_lead_in_ms = 100;
static const uint8_t k_bytes = 40;
static const uint8_t k_byte_step = 37;
static const uint8_t k_byte_offset = 11;
static const uint32_t k_dense_slot_us = 128000;
static const uint8_t k_dense_bits = 8;
static const uint16_t k_dense_tone_hz = 2700;

inline EncoderConfig case_config(uint8_t index) {
    const Preset presets[] = {Preset::fm_fast, Preset::fm, Preset::hf_fast, Preset::hf, Preset::hf_robust,
                              Preset::hf_weak, Preset::hf};
    EncoderConfig config = EncoderConfig::from_preset(presets[index < k_cases ? index : 0], k_rate_hz);
    if (index + 1 == k_cases) {
        config.slot_us = k_dense_slot_us;
        config.bits_per_peak = k_dense_bits;
        config.tone_hz = k_dense_tone_hz;
        config.spacing = Spacing::dense;
    }
    if (config.lead_in_ms < k_lead_in_ms) config.lead_in_ms = k_lead_in_ms;
    return config;
}

inline uint8_t case_byte(uint8_t index) {
    return static_cast<uint8_t>(index * k_byte_step + k_byte_offset);
}

}  // namespace avr_isr
}  // namespace unlimited
