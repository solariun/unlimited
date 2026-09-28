#pragma once

#include "unlimited/encoder.hpp"

// The transmissions of the AVR ISR gate (spec 3.9, 8): the five speeds of spec 1.3 on 1500 Hz, 1 byte/s at 2700 Hz in
// the AM passband (the highest pitch), and 6 and 25 bytes/s with a VOX lead, each with tx_uno's 100 ms lead-in. Shared
// by the AVR harness and the host program that checks it.
namespace unlimited {
namespace avr_isr {

static const uint8_t k_cases = 8;
static const uint32_t k_rate_hz = 8000;
static const uint16_t k_lead_in_ms = 100;
static const uint8_t k_bytes = 40;
static const uint8_t k_byte_step = 37;
static const uint8_t k_byte_offset = 11;
static const uint8_t k_speed_cases = 5;
static const uint16_t k_speeds_centi[k_speed_cases] = {100, 300, 600, 1200, 2500};
static const uint8_t k_high_case = 5;
static const uint16_t k_high_tone_hz = 2700;
static const uint16_t k_wide_low_hz = 100;
static const uint16_t k_wide_high_hz = 3000;
static const uint8_t k_vox_case = 6;
static const uint8_t k_fast_vox_case = 7;
static const uint16_t k_vox_lead_ms = 150;
static const uint16_t k_fast_centi = 2500;

inline EncoderConfig case_config(uint8_t index) {
    EncoderConfig config;
    config.sample_rate_hz = k_rate_hz;
    config.lead_in_ms = k_lead_in_ms;
    if (index < k_speed_cases) {
        config.slot_us = slot_us_for_centi_speed(k_speeds_centi[index]);
    } else if (index == k_high_case) {
        config.slot_us = slot_us_for_centi_speed(k_speeds_centi[0]);
        config.tone_hz = k_high_tone_hz;
        config.passband.low_hz = k_wide_low_hz;
        config.passband.high_hz = k_wide_high_hz;
    } else {
        config.vox_lead_ms = k_vox_lead_ms;
        if (index == k_fast_vox_case) config.slot_us = slot_us_for_centi_speed(k_fast_centi);
    }
    return config;
}

inline uint8_t case_byte(uint8_t index) {
    return static_cast<uint8_t>(index * k_byte_step + k_byte_offset);
}

}  // namespace avr_isr
}  // namespace unlimited
