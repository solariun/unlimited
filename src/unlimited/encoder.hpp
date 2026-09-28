#pragma once

#include "unlimited/protocol.hpp"

// Queue size; only ever defined for the whole build (encoder.cpp must agree).
#ifndef UNLIMITED_ENCODER_QUEUE
#define UNLIMITED_ENCODER_QUEUE 64
#endif

namespace unlimited {

static const uint32_t k_min_sample_rate_hz = 8000;  // EncoderConfig::sample_rate_hz
static const uint32_t k_max_sample_rate_hz = 192000;

struct EncoderConfig {
    uint32_t sample_rate_hz;
    uint32_t slot_us;       // T = 1 / (10 B), k_min_slot_us..k_max_slot_us: slot_us_for_speed(B)
    uint16_t tone_hz;       // the one pitch: every beep and the VOX lead
    int16_t amplitude;      // crest A (key-down peak), output units
    Passband passband;      // the receiving radio's audio passband the occupied band must fit
    uint16_t lead_in_ms;    // silence before the transmission (the radio's TX delay)
    uint16_t vox_lead_ms;   // 0: none; else a steady tone of max(ceil(ms / T), k_min_vox_lead_slots) slots, then
                            // k_vox_gap_slots of silence (spec 2.1)
    uint16_t tail_ms;       // silence after the last STOP: max(tail_ms, k_min_tail_slots slots)

    EncoderConfig();  // 6 bytes/s, 1500 Hz, -3 dBFS, passband 300..2700 Hz, 8000 Hz, no lead-in, no VOX lead, 100 ms
    // Integer only; the first rule broken, in ConfigError order (the rate and T ranges also keep the tone 500 Hz
    // below rate / 2 and T >= 32 samples).
    ConfigError check() const;
    bool valid() const;  // check() == ConfigError::none
};

Band occupied_band(const EncoderConfig& config);  // occupied_band(tone_hz, slot_us)
// The pitches the receiver of this speed searches in this passband (spec 3.2): search_range(passband, slot_us).
Passband search_range(const EncoderConfig& config);
// The shift tolerance (spec 1.3): passband_fit(tone_hz, slot_us, passband, search_range(config)).
PassbandFit passband_fit(const EncoderConfig& config);

enum class EncoderSegment : uint8_t { idle, lead_in, vox_lead, gap, window, tail };

enum class SlotKind : uint8_t { silent, lead, start, one, zero, stop };

// Read-only telemetry (TUI). In the window segment: slot 0 is the START, 1..8 the data slots (MSB first), 9 the
// STOP; byte and byte_index describe the window's byte in every one of its slots.
struct EncoderStatus {
    EncoderSegment segment;
    SlotKind kind;               // kind of the slot being rendered
    uint8_t slot;                // window: 0..9; 0 elsewhere
    uint8_t byte;                // window: the byte being sent; else 0
    uint8_t bit_index;           // window, data slot: 0..7, MSB first; else 0
    uint32_t byte_index;         // window: position of `byte` in the transmission; else 0
    uint32_t slot_index;         // slot being rendered, counted from 0 at start(), lead-in included; +1 per slot
    uint32_t samples_rendered;   // samples rendered since start()
};

// Threads and ISRs (spec 2.5): one producer calls write(), queue_free(), queued(), busy() and, while idle, start();
// one consumer (an ISR, or the audio thread) calls next_sample() or render(). The queue and start() hand over with
// release/acquire fences (platform.hpp), so the two may run on different cores. abort() and status() touch the
// consumer's state: call them from the consumer, or with it stopped (interrupts masked around the call).
class Encoder {
public:
    static const uint16_t k_queue_size = UNLIMITED_ENCODER_QUEUE;
    static_assert(k_queue_size >= 16 && k_queue_size <= 128 && (k_queue_size & (k_queue_size - 1)) == 0,
                  "queue size must be a power of two in 16..128");

    explicit Encoder(const EncoderConfig& config);

    bool start();                                   // false when busy, the queue is empty or the config invalid
    bool write(uint8_t byte);
    size_t write(const uint8_t* data, size_t size);
    void abort();

    int16_t next_sample();                          // ISR-safe; 0 when idle
    size_t render(int16_t* out, size_t count);      // stops at the end of the transmission; returns samples written

    bool busy() const;                              // true from start() until the last tail sample is rendered
    size_t queue_free() const;                      // k_queue_size - queued()
    size_t queued() const;                          // bytes waiting, the byte being sent included
    uint32_t duration_samples(size_t data_bytes) const;  // PTT hold time, spec 2.4; 0 when 0 bytes or invalid
    EncoderStatus status() const;
    const EncoderConfig& config() const;

private:
    void next_slot(EncoderSegment segment);
    EncoderSegment enter_vox_lead();
    EncoderSegment enter_gap();
    EncoderSegment enter_window_or_tail();  // at a window boundary: the next byte, or the tail when the queue is empty
    void go_idle();
    int16_t envelope_q15(uint32_t slot_phase) const;

    EncoderConfig config_;
    // Position in the slot as a 64-bit fraction (2^64 = one slot; a slot ends on overflow), kept as two words:
    // 32-bit adds only (a 64-bit add, compare or shift is a library call on AVR). The high word is u.
    uint32_t slot_phase_high_;
    uint32_t slot_phase_low_;
    uint32_t slot_step_high_;
    uint32_t slot_step_low_;
    uint32_t tone_phase_;         // the one NCO: advances on every sample, silent ones included
    uint32_t tone_step_;
    uint32_t countdown_;          // slots left in lead-in/VOX lead/gap, samples left in the tail
    uint32_t tail_samples_;       // computed by start(): no division at a slot edge
    uint32_t slot_index_;
    uint32_t samples_rendered_;
    uint32_t byte_index_;         // position of the byte at queue_tail_ in the transmission
    uint16_t vox_lead_slots_;
    volatile EncoderSegment segment_;
    SlotKind kind_;
    uint8_t slot_;                // window: 0..9; VOX lead: first/middle/last
    uint8_t byte_;                // the window's byte, read from the queue at its START
    uint8_t bits_;                // its bits not sent yet, the next one in bit 7
    // Free-running indices (mod 256): head - tail bytes are queued, up to k_queue_size.
    volatile uint8_t queue_head_;
    volatile uint8_t queue_tail_;
    uint8_t queue_[k_queue_size];  // a byte stays at queue_tail_ until its window's STOP ends
};

}  // namespace unlimited
