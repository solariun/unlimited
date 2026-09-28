#pragma once

#include "unlimited/encoder.hpp"
#include "unlimited/kiss.hpp"

// The send side of the KISS modem (spec 12.1, 12.2): KISS frames from the computer into a send queue, the channel
// check, PTT, and the Encoder, one transmission per frame. It needs no receiver (a send-only or a future Arduino Nano
// build links this, the KISS codec and the encoder only); the Modem (modem.hpp) adds the receiver.

// Send queue, in bytes: a power of two, 16..32768; only ever defined for the whole build (transmitter.cpp must agree).
#ifndef UNLIMITED_MODEM_QUEUE
#if defined(__AVR__) || defined(ARDUINO) || defined(ESP_PLATFORM) || defined(__XTENSA__)
#define UNLIMITED_MODEM_QUEUE 2048
#else
#define UNLIMITED_MODEM_QUEUE 16384
#endif
#endif

namespace unlimited {

static const uint32_t k_modem_rate_hz = 8000;  // the modem's audio, in and out (the decoder's rate)

// The TX delay (Gustavo, 2026-09-28): silence before the first START with a line or CAT PTT, only at the beginning:
// radios need about 20..100 ms to switch to transmit, and nothing is ever added once the transmission has started.
// With VOX the lead tone and its gap take its place (spec 2.1).
static const uint16_t k_default_txdelay_ms = 100;

// Channel access (spec 12.1), with kiss_modem's defaults.
static const uint16_t k_default_dwait_ms = 1500;
static const uint8_t k_default_persist = 63;
static const uint16_t k_default_slot_time_ms = 100;
static const uint32_t k_no_tick = 0xFFFFFFFFu;  // next_tick_ms(): nothing is timed, tick() waits for an event
// A receiver ends a transmission after a whole silent window (2 with the fade bridge, modem.hpp) following its last
// STOP; a tone before that (the next transmission's VOX lead) reads as the old one going on. So between two of this
// modem's transmissions the silence after the last STOP lasts that many windows and k_end_margin_slots more.
static const uint8_t k_default_end_windows = 1;
static const uint8_t k_end_margin_slots = 2;

// Who needs to run because of what another context did (WakeHandler).
enum class ModemWake : uint8_t {
    control,  // tick(): a frame is ready, DCD changed, or a transmission's audio ended
    host      // host_input(): room came back in the send queue after it refused bytes
};

// Called from the context that calls the method named; each must return at once.
typedef void (*PttHandler)(bool on, void* context);                                // tick()
typedef void (*HostHandler)(const uint8_t* data, size_t size, void* context);      // Modem::audio_input()
typedef void (*WakeHandler)(ModemWake what, void* context);  // any context, the audio callback included: post a
                                                               // wake-up (a byte on a pipe, a task notification)

struct AccessConfig {
    uint16_t dwait_ms;           // after the channel went quiet (DCD off), wait this long before contending
    uint8_t persist;             // p-persistence: transmit when a random 0..255 is <= persist
    uint16_t slot_time_ms;       // between p-persistence draws, >= 1
    uint16_t output_latency_ms;  // from audio_output() to the device's output: PTT is released this long after
                                 // the transmission's last sample
    bool full_duplex;            // no channel check, and the receiver hears while sending
    uint32_t seed;               // of the p-persistence draws (0 takes a fixed seed)
    uint8_t end_windows;         // silent windows after a transmission before the next one (k_default_end_windows)

    AccessConfig();  // dwait 1500 ms, persist 63, slot time 100 ms, no latency, half duplex, fixed seed, 1 window
};

// A snapshot for display and tests; each field is written by one context. The receive fields stay 0 without a
// receiver (ModemTransmitter).
struct ModemCounters {
    KissCounters kiss;         // computer -> modem, as the KISS decoder saw it
    uint32_t host_refusals;    // host_input() calls that left bytes for later (the send queue was full)
    uint32_t queued_bytes;     // in the send queue now
    uint32_t queued_frames;    // complete frames in the send queue now
    uint32_t transmissions;    // frames sent, one transmission each
    uint32_t bytes_sent;
    uint32_t underruns;        // transmissions that ended before their frame did (the computer was slower than the air)
    uint32_t bytes_skipped;    // the rest of those frames, dropped: a frame is never split into two transmissions
    uint32_t draws;            // p-persistence draws
    uint32_t deferrals;        // draws that waited a slot time
    uint32_t on_air_sent;      // the frame on the air: its bytes whose windows are complete
    uint32_t on_air_size;      // and its size (0 while its end has not arrived: a frame longer than the queue)
    uint32_t frames_received;  // receptions passed to the computer (C0 00 to it): found from their start, and at least
                               // the minimum frame long (Modem, V23)
    uint32_t bytes_received;   // their bytes
    uint32_t ends;             // of those, closed by `end`
    uint32_t losses;           // of those, closed by `lost`
    uint32_t short_frames;     // receptions shorter than the minimum frame: nothing of them went to the computer
    uint32_t short_bytes;      // their bytes
    uint32_t muted_samples;    // heard while transmitting and replaced by silence (half duplex)
};

// The control side's state (tick()).
enum class ChannelState : uint8_t {
    idle,       // nothing to send
    waiting,    // a frame is ready: DCD off, dwait, p-persistence
    keyed,      // PTT on, the frame on the air (lead-in or VOX lead, windows, tail)
    releasing   // its audio ended: PTT off once the output latency has passed
};

// Four calling contexts, one per method group; between them only lock-free single-producer rings and counters, each
// written by one context with store_release() and read by the others with load_acquire() (platform.hpp), so each
// context may run on its own thread or core:
//   computer: host_input()                 control: tick(), next_tick_ms()
//   audio:    audio_output() (an ISR or a device callback: never blocks, never locks)
//   receive:  set_dcd() (Modem::audio_input())
// dcd(), transmitting(), channel_state() and counters() read snapshots from any context. No heap, no threads.
class ModemTransmitter {
public:
    static const uint16_t k_queue_size = UNLIMITED_MODEM_QUEUE;
    // Complete frames the send queue holds at most: one per 32 bytes of queue, 4..64 (a power of two).
    static const uint8_t k_frame_slots =
        static_cast<uint8_t>(k_queue_size >= 2048 ? 64 : (k_queue_size >= 128 ? k_queue_size / 32 : 4));
    static const uint16_t k_budget_bytes = 512;  // the transmitter beyond its queue and frame slots
    static_assert(k_queue_size >= 16 && k_queue_size <= 32768 && (k_queue_size & (k_queue_size - 1)) == 0,
                  "UNLIMITED_MODEM_QUEUE must be a power of two in 16..32768");

    // signal.sample_rate_hz must be k_modem_rate_hz (8000): the modem's audio is 8 kHz. An invalid configuration
    // leaves the transmitter inert: host_input() takes nothing, audio_output() is silence.
    ModemTransmitter(const EncoderConfig& signal, const AccessConfig& access, PttHandler ptt, void* context,
                     WakeHandler wake = nullptr);

    // Computer: KISS bytes; returns how many were taken. Bytes that do not fit (the send queue or its frame slots are
    // full) are left for a later call: WakeHandler(host) says when room came back.
    size_t host_input(const uint8_t* data, size_t size);

    // Audio: fills count samples at 8 kHz, silence when no transmission is on. Starts the encoder when the control side
    // keyed, tops its queue up from the send queue within the frame (never a byte of the next frame), and ends the
    // transmission when the frame is sent.
    void audio_output(int16_t* out, size_t count);

    // Control: the channel check and PTT at now_ms (any millisecond clock, wrapping). next_tick_ms(): when tick() must
    // run next, or k_no_tick when only an event (WakeHandler(control)) can change anything.
    void tick(uint32_t now_ms);
    uint32_t next_tick_ms() const;

    // Receive: whether the receiver hears a signal (a transmission is being decoded). Without a receiver, never called:
    // the channel is taken as clear.
    void set_dcd(bool busy);

    bool valid() const;
    bool dcd() const;
    bool transmitting() const;  // PTT keyed (from the key to the release)
    ChannelState channel_state() const;
    ModemCounters counters() const;
    const EncoderConfig& signal() const;  // as sent: the lead-in may be longer than asked (Modem: fade bridge)
    const AccessConfig& access() const;

private:
    static const uint16_t k_queue_mask = k_queue_size - 1;
    static const uint8_t k_slot_mask = k_frame_slots - 1;

    bool frame_ready() const;
    bool may_key(uint32_t now_ms, bool busy);
    void key();
    void set_timer(uint32_t at_ms);
    uint8_t draw();
    void begin();
    void top_up();
    void finish();
    void skip();
    void offer_room();
    void publish_progress();

    EncoderConfig signal_;
    AccessConfig access_;
    PttHandler ptt_;
    WakeHandler wake_;
    void* context_;
    bool valid_;
    Encoder encoder_;

    // Computer side.
    KissDecoder kiss_;
    KissCounters kiss_counters_;           // kiss_'s counters, published after each host_input()
    uint32_t refusals_total_;
    uint16_t written_;                     // bytes written into queue_, published as queue_head_
    uint8_t queue_[k_queue_size];
    uint16_t frame_ends_[k_frame_slots];   // where each complete frame ends (a queue position)
    volatile uint16_t queue_head_;         // computer: free-running byte count
    volatile uint16_t queue_tail_;         // audio
    volatile uint8_t ends_head_;           // computer: complete frames, free-running
    volatile uint8_t ends_tail_;           // audio
    volatile uint8_t refusals_;            // computer: bumped when host_input() leaves bytes

    // Audio side.
    uint8_t refusals_seen_;
    uint8_t started_;                      // go_ of the transmission on the air
    bool sending_;
    uint32_t fed_;                         // bytes of this transmission given to the encoder
    uint16_t frame_start_;                 // the queue position of its first byte
    volatile uint32_t on_air_sent_;        // progress snapshots of the frame on the air (counters())
    volatile uint32_t on_air_size_;
    volatile bool skipping_;               // the rest of a frame whose transmission ended early is being dropped
    volatile uint8_t done_;                // bumped when a transmission's audio ended
    uint32_t transmissions_;               // these and the other plain counters: published for counters()
    uint32_t bytes_sent_;
    uint32_t underruns_;
    uint32_t bytes_skipped_;

    // Receive side.
    volatile bool dcd_;
    volatile uint8_t dcd_changes_;

    // Control side.
    volatile ChannelState state_;
    volatile bool keyed_;
    volatile uint8_t go_;                  // bumped when PTT is keyed: the audio side starts the next frame
    uint32_t spacing_ms_;                  // after a transmission's end, before the next key (end_windows)
    uint32_t spaced_until_ms_;
    bool clock_started_;
    uint8_t dcd_changes_seen_;
    uint32_t quiet_since_ms_;              // the channel has been quiet since (DCD off)
    bool draw_waiting_;                    // a draw failed: the next one comes at draw_at_ms_
    uint32_t draw_at_ms_;
    uint32_t release_at_ms_;
    bool timer_set_;
    uint32_t timer_ms_;
    uint32_t random_;
    uint32_t draws_;
    uint32_t deferrals_;
};

}  // namespace unlimited
