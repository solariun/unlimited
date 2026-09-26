#pragma once

#include "unlimited/protocol.hpp"

// Queue size; like UNLIMITED_PACKET_MAX, only ever defined for the whole build (encoder.cpp must agree).
#ifndef UNLIMITED_ENCODER_QUEUE
#define UNLIMITED_ENCODER_QUEUE 64
#endif

namespace unlimited {

static const uint32_t k_min_sample_rate_hz = 8000;  // EncoderConfig::sample_rate_hz
static const uint32_t k_max_sample_rate_hz = 192000;

// Presets of spec 1.4, all on 1500 Hz. Net rate = N / ((N + 1) T).
enum class Preset : uint8_t {
    hf_slow,  // T 32 ms, N 8,  27.8 bit/s, SSB 300..2700 Hz
    hf,       // T 16 ms, N 8,  55.6 bit/s, SSB 300..2700 Hz (the default)
    hf_fast,  // T 8 ms,  N 8,  111.1 bit/s, SSB 300..2700 Hz (good HF paths only)
    am,       // T 8 ms,  N 16, 117.6 bit/s, AM 100..3000 Hz
    fm        // T 4 ms,  N 16, 235.3 bit/s, FM 300..3000 Hz, 300 ms lead-in (receiver min_slot_ms 4)
};

struct EncoderConfig {
    uint32_t sample_rate_hz;
    uint32_t slot_us;           // T, k_min_slot_us..k_max_slot_us, any value
    uint16_t tone_hz;           // the one pitch: tune tone, markers and data
    int16_t amplitude;          // crest A (key-down peak), output units
    Passband passband;          // the receiving radio's audio passband the occupied band must fit
    uint16_t lead_in_ms;
    uint16_t tune_ms;
    uint16_t tail_ms;
    uint8_t bits_per_package;   // N, k_min_bits_per_package..k_max_bits_per_package
    uint8_t sync_markers;

    EncoderConfig();  // Preset::hf at 8000 Hz
    static EncoderConfig from_preset(Preset preset, uint32_t sample_rate_hz);
    // Integer only; the first rule broken, in ConfigError order (the rate and T ranges also keep the tone 500 Hz
    // below rate / 2 and T >= 32 samples).
    ConfigError check() const;
    bool valid() const;  // check() == ConfigError::none
};

Band occupied_band(const EncoderConfig& config);  // occupied_band(tone_hz, slot_us)
// The pitches the receiver that hears this sender by default searches (spec 1.5): search_range(passband, T_min) with
// the window of the fm profile (T_min 4 ms) below k_fast_slot_us, of ssb and am (8 ms) up to 64 ms, else the
// smallest whole ms whose window holds T.
Passband search_range(const EncoderConfig& config);
// The shift tolerance (spec 1.5): passband_fit(tone_hz, slot_us, passband, search_range(config)): the filter fit of
// occupied_band(config), each margin limited to the pitches that receiver searches; fits: the band fits the passband.
PassbandFit passband_fit(const EncoderConfig& config);

enum class EncoderSegment : uint8_t { idle, lead_in, tune, sync, package, end, tail };

enum class SlotKind : uint8_t { silent, tone, one, zero, marker };

// Read-only telemetry (TUI, full application). The START of the first package is the last sync marker; each STOP
// is the last slot of its package segment.
struct EncoderStatus {
    EncoderSegment segment;
    SlotKind kind;               // kind of the slot being rendered
    uint8_t slot;                // package segment: 1..d data slot, d + 1 the STOP; 0 elsewhere
    uint8_t package_bits;        // package segment: d, the bits of this package (N, fewer in a short final one)
    uint8_t byte;                // package segment, data slot: the byte holding the bit being sent; else 0
    uint8_t bit_index;           // same: 0..7, MSB first
    uint32_t package_index;      // package segment: 0 = the package after the sync train
    uint32_t byte_index;         // package segment, data slot: position of `byte` in the transmission
    uint32_t slot_index;         // slot being rendered, counted from 0 at start(), lead-in included; +1 per slot
    uint32_t samples_rendered;   // samples rendered since start()
};

// Threads and ISRs: one producer calls write(), queue_free(), queued(), busy() and, while idle, start(); one
// consumer (an ISR, or the audio thread) calls next_sample() or render(). The queue and start() hand over with
// release/acquire fences (platform.hpp), so the two may run on different cores. abort() and status() touch the
// consumer's state: call them from the consumer, or with it stopped (interrupts masked around the call).
class Encoder {
public:
    static const uint16_t k_queue_size = UNLIMITED_ENCODER_QUEUE;
    static_assert(k_queue_size >= 16 && k_queue_size <= 128 && (k_queue_size & (k_queue_size - 1)) == 0,
                  "queue size must be a power of two in 16..128");
    // Two consecutive packages span at most ceil(2 N / 8) + 1 bytes: the producer can keep the next one queued.
    static_assert((2u * k_max_bits_per_package + k_bits_per_byte - 1) / k_bits_per_byte + 1 <= k_queue_size,
                  "the queue must hold two packages of k_max_bits_per_package bits");

    explicit Encoder(const EncoderConfig& config);

    bool start();
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
    void begin_tune();
    void begin_sync();
    void begin_package_or_end();  // at a START: a full package, a short final package, or the END markers
    void begin_end();
    void begin_tail();
    void go_idle();
    void flip_sign();
    void load_bit();              // kind_ of the data slot about to start, from the byte at queue_tail_
    int16_t envelope_q15(uint32_t slot_phase) const;

    EncoderConfig config_;
    // Position in the slot as a 64-bit fraction (2^64 = one slot; a slot ends on overflow), kept as two words:
    // 32-bit adds only (a 64-bit add, compare or shift is a library call on AVR). The high word is u.
    uint32_t slot_phase_high_;
    uint32_t slot_phase_low_;
    uint32_t slot_step_high_;
    uint32_t slot_step_low_;
    uint32_t tone_phase_;         // the one NCO: advances on every sample; += 2^31 after a marker (the sign flip)
    uint32_t tone_step_;
    uint32_t countdown_;          // slots left in lead-in/tune/sync/END, samples left in the tail
    uint32_t tail_samples_;       // computed by start(): no division at a slot edge
    uint32_t slot_index_;
    uint32_t samples_rendered_;
    uint32_t package_index_;
    uint32_t byte_index_;         // position of the byte at queue_tail_ in the transmission
    uint16_t tune_slots_;
    volatile EncoderSegment segment_;
    SlotKind kind_;
    uint8_t slot_;                // as EncoderStatus::slot; tune: first/middle/last
    uint8_t package_bits_;        // d of the package being sent
    uint8_t bit_offset_;          // bits of the byte at queue_tail_ already sent, 0..7
    // Free-running indices (mod 256): head - tail bytes are queued, up to k_queue_size.
    volatile uint8_t queue_head_;
    volatile uint8_t queue_tail_;
    uint8_t queue_[k_queue_size];  // a byte stays at queue_tail_ until the slot of its last bit ends
};

}  // namespace unlimited
