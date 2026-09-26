// AVR side of the ISR gate (spec 8.6 B5): the Timer2 ISR body of examples/arduino/tx_uno, called once per sample from
// main() the way the timer would call it, for the transmission of case UNLIMITED_ISR_CASE (isr_cases.hpp). The
// sample is also stored in avr_isr_sample for the host program (tests/avr/isr_cycles.cpp), which runs this image on
// its ATmega328P interpreter. Built like a sketch: avr-g++ -Os -flto -mmcu=atmega328p.
#include <avr/interrupt.h>
#include <avr/io.h>

#include "isr_cases.hpp"

#ifndef UNLIMITED_ISR_CASE
#define UNLIMITED_ISR_CASE 3
#endif

extern "C" {
volatile int16_t avr_isr_sample;
}

namespace {

const uint8_t k_done = 0xAA;    // GPIOR0 when the transmission has ended
const uint8_t k_failed = 0xEE;  // GPIOR0 when start() refused the configuration
const uint8_t k_pwm_middle = 125;
const uint8_t k_sample_high_byte = 8;
const uint8_t k_level_shift = 7;

unlimited::Encoder g_encoder(unlimited::avr_isr::case_config(UNLIMITED_ISR_CASE));
volatile uint8_t g_next_level = k_pwm_middle;

uint8_t pwm_level(int16_t sample) {
    const int16_t high = static_cast<int8_t>(sample >> k_sample_high_byte);
    return static_cast<uint8_t>(k_pwm_middle + ((high * k_pwm_middle) >> k_level_shift));
}

void refill(uint8_t& sent) {
    while (sent < unlimited::avr_isr::k_bytes && g_encoder.write(unlimited::avr_isr::case_byte(sent))) ++sent;
}

}  // namespace

// tx_uno's ISR, plus the store of the sample.
ISR(TIMER2_COMPA_vect) {
    OCR1A = g_next_level;
    const int16_t sample = g_encoder.next_sample();
    avr_isr_sample = sample;
    g_next_level = pwm_level(sample);
}

int main() {
    uint8_t sent = 0;
    refill(sent);
    if (!g_encoder.start()) {
        GPIOR0 = k_failed;
        for (;;) {
        }
    }
    while (g_encoder.busy()) {
        refill(sent);
        __asm__ __volatile__("call __vector_7" ::: "memory");  // TIMER2_COMPA
    }
    GPIOR0 = k_done;
    for (;;) {
    }
}
