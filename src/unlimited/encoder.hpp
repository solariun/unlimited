#pragma once

#include "unlimited/protocol.hpp"

// Queue size; like UNLIMITED_PACKET_MAX, only ever defined for the whole build (encoder.cpp must agree).
#ifndef UNLIMITED_ENCODER_QUEUE
#define UNLIMITED_ENCODER_QUEUE 64
#endif

namespace unlimited {

static const uint32_t k_min_sample_rate_hz = 8000;  // EncoderConfig::sample_rate_hz
static const uint32_t k_max_sample_rate_hz = 192000;

// Modes of spec 1.4; every preset has N = 8, standard spacing and the grid below f_ref.
enum class Preset : uint8_t {
    fm_fast,    // T 6 ms,   k 3, 444 bit/s,  f_ref 2650 Hz
    fm,         // T 8 ms,   k 3, 333 bit/s,  f_ref 2650 Hz
    hf_fast,    // T 16 ms,  k 4, 222 bit/s,  f_ref 2192 Hz
    hf,         // T 32 ms,  k 5, 139 bit/s,  f_ref 2132 Hz
    hf_robust,  // T 64 ms,  k 6, 83.3 bit/s, f_ref 2102 Hz
    hf_weak     // T 128 ms, k 7, 48.6 bit/s, f_ref 2087 Hz
};

// Why EncoderConfig::check() refuses a configuration.
enum class ConfigError : uint8_t {
    none,
    sample_rate,    // outside k_min_sample_rate_hz..k_max_sample_rate_hz
    tone,           // f_ref outside [k_min_tone_hz, k_max_tone_hz]
    slot,           // T not a whole number of ms in k_min_slot_us..k_max_slot_us
    bits_per_peak,  // k outside 1..8
    data_slots,     // N not 8, 16 or 32
    frame_length,   // (N + 1) T > k_max_frame_us
    spacing,        // not a Spacing value
    dense_slot,     // dense spacing needs T >= k_min_dense_slot_us
    side,           // not a GridSide value
    band,           // a data or header tone outside [k_min_tone_hz, k_max_tone_hz]
    queue,          // frame_bytes() > Encoder::k_queue_size / 2
    sync_markers,   // outside k_min_sync_markers..k_max_sync_markers
    amplitude       // not > 0
};

struct EncoderConfig {
    uint32_t sample_rate_hz;
    uint16_t tone_hz;        // f_ref: tune tone and markers
    uint32_t slot_us;        // whole ms, 6000..128000
    uint8_t bits_per_peak;   // k = 1..8
    uint8_t data_slots;      // N = 8, 16, 32
    Spacing spacing;
    GridSide side;
    int16_t amplitude;
    uint16_t lead_in_ms;
    uint16_t tune_ms;
    uint8_t sync_markers;
    uint16_t tail_ms;

    EncoderConfig();  // Preset::hf at 8000 Hz
    static EncoderConfig from_preset(Preset preset, uint32_t sample_rate_hz);
    // Integer only; the first rule broken, in ConfigError order (rate and T ranges also keep every tone 500 Hz
    // below rate / 2 and T >= 32 samples).
    ConfigError check() const;
    bool valid() const;  // check() == ConfigError::none
    uint8_t frame_bytes() const;  // N * k / 8
};

enum class EncoderSegment : uint8_t { idle, lead_in, tune, sync, header, frame, eot, tail };

enum class SlotKind : uint8_t { silent, tone, peak, marker };

// Read-only telemetry (TUI, full application). byte, slot, symbol and tone are 0 outside the header and frame
// segments.
struct EncoderStatus {
    EncoderSegment segment;
    SlotKind kind;               // kind of the slot being rendered
    uint8_t byte;                // frame segment: first byte of the frame being sent, else 0
    uint8_t slot;                // header 0..7, frame 0..N-1 peak slot; the STOP gives 8 (header) or the frame's peak count
    uint8_t symbol;              // k-bit value of the peak being rendered (header: 0..7)
    uint8_t tone;                // grid tone index of the peak being rendered
    uint32_t slot_index;         // slot being rendered, counted from 0 at start(), lead-in included; +1 per slot
    uint32_t samples_rendered;   // samples rendered since start()
};

// Threads and ISRs: one producer calls write(), queue_free(), queued(), busy() and, while idle, start(); one consumer
// (an ISR, or the audio thread) calls next_sample() or render(). The queue and start() hand over with release/acquire
// fences (platform.hpp), so the two may run on different cores. abort() and status() touch the consumer's state:
// call them from the consumer, or with it stopped (interrupts masked around the call).
class Encoder {
public:
    static const uint16_t k_queue_size = UNLIMITED_ENCODER_QUEUE;
    static_assert(k_queue_size >= 16 && k_queue_size <= 128 && (k_queue_size & (k_queue_size - 1)) == 0,
                  "queue size must be a power of two in 16..128");

    explicit Encoder(const EncoderConfig& config);

    bool start();
    bool write(uint8_t byte);
    size_t write(const uint8_t* data, size_t size);
    void abort();

    int16_t next_sample();                          // ISR-safe; 0 when idle
    size_t render(int16_t* out, size_t count);      // stops at the end of the transmission; returns samples written

    bool busy() const;                              // true from start() until the last tail sample is rendered
    size_t queue_free() const;                      // k_queue_size - queued()
    size_t queued() const;                          // bytes waiting, the frame being sent included
    uint32_t duration_samples(size_t data_bytes) const;
    EncoderStatus status() const;
    const EncoderConfig& config() const;

private:
    void next_slot(EncoderSegment segment);
    void begin_tune();
    void begin_header();
    void begin_frame_or_eot();
    void begin_eot();
    void begin_tail();
    void go_idle();
    void flip_sign();
    void load_peak(EncoderSegment segment);  // symbol_, tone_ and data_step_ of the peak slot about to start
    int16_t envelope_q15(EncoderSegment segment, uint32_t slot_phase) const;
    SlotKind slot_kind(EncoderSegment segment) const;

    EncoderConfig config_;
    // Position in the slot as a 64-bit fraction (2^64 = one slot; a slot ends on overflow), kept as two words: 32-bit
    // adds only (a 64-bit add, compare or shift is a library call on AVR). The high word is u, the envelope phase.
    uint32_t slot_phase_high_;
    uint32_t slot_phase_low_;
    uint32_t slot_step_high_;
    uint32_t slot_step_low_;
    uint32_t tone_phase_;               // f_ref NCO: advances on every sample (marker sign, START/STOP phase AFC)
    uint32_t tone_step_;
    uint32_t data_phase_;               // data NCO: reset only by start(); its step changes only at slot edges
    uint32_t data_step_;
    uint32_t unit_step_;                // 1 / T: guard and dense spacing
    uint32_t std_step_;                 // 8 / (7 T): standard spacing and the header grid
    uint32_t countdown_;                // slots left in lead-in/tune/sync/EOT, samples left in the tail
    uint32_t tail_samples_;             // computed by start(): no division at a slot edge
    uint32_t slot_index_;
    uint32_t samples_rendered_;
    uint32_t header_tones_;             // the header's 8 tones, 3 bits each, computed by start(); consumed slot by slot
    uint16_t tune_slots_;
    volatile EncoderSegment segment_;
    uint8_t slot_;                      // header/frame slot as in EncoderStatus; tune: first/middle/last
    uint8_t symbol_;
    uint8_t tone_;
    uint8_t frame_n_;                   // bytes of this frame
    uint8_t frame_d_;                   // peak slots of this frame (N, or fewer in a short final frame)
    // Free-running indices (mod 256): head - tail bytes are queued, up to k_queue_size.
    volatile uint8_t queue_head_;
    volatile uint8_t queue_tail_;
    uint8_t queue_[k_queue_size];       // the bytes of a frame stay from queue_tail_ until its STOP ends
};

}  // namespace unlimited
