#pragma once

#include "unlimited/decoder.hpp"
#include "unlimited/transmitter.hpp"

// The KISS modem's portable core (spec 12.1, 12.2): the send side of transmitter.hpp plus the receiver. What the
// computer sends as one KISS data frame goes on the air as one transmission; what the receiver decodes goes back to
// the computer as it comes: C0 00 when a transmission is found from its start (`locked`), each byte escaped at once,
// C0 at its `end` or `lost`, once the reception is a frame's minimum long (V23, below; 0: from its first byte). No
// heap, no threads, no exceptions; float only in the decoder.
namespace unlimited {

// HF fades (spec 12.1): with the fade bridge the receiver ends a transmission only after 2 silent windows (a fade of
// one window costs a framing error, not a split frame) and needs 300 ms of silence, or a VOX lead and its gap, before
// a START; this modem then leaves at least that much silence before each START (the TX delay, unless a VOX lead
// precedes it) and 2 silent windows after each transmission before its next one. Both stations must use the same
// setting. The modem's default is one constant, to flip once decided.
static const bool k_default_fade_bridge = false;
static const uint16_t k_fade_bridge_silence_ms = k_min_onset_silence_ms;  // protocol.hpp: 300 ms
static const uint8_t k_fade_bridge_end_windows = 2;

// Short receptions (spec 12.1, V23, Gustavo 2026-09-28): what the receiver makes of speech, CW or noise is a byte or
// two (stray bytes, V20); an AX.25 frame is at least 15 bytes (two addresses and a control byte). A reception goes to
// the computer only once it holds min_frame_bytes bytes: its first bytes wait, then C0 00 and all of them go at once
// and the rest streams as decoded; a reception that ends shorter sends nothing, not even C0 00 or C0. 0 turns this off
// (every reception streams from its first byte). The first byte then reaches the computer min_frame_bytes - 1 windows
// later: 2.3 s at 6 bytes/s with 15.
static const uint8_t k_default_min_frame_bytes = 15;
static const uint8_t k_max_min_frame_bytes = 64;

struct ModemConfig {
    EncoderConfig signal;    // what this station sends: sample_rate_hz 8000; lead_in_ms is the TX delay (PTT by a line
                             // or CAT), vox_lead_ms the VOX lead (VOX), tail_ms the TX tail
    DecoderConfig receiver;  // what it listens for: the same speed (slot_us) as signal
    AccessConfig access;
    bool fade_bridge;
    uint8_t min_frame_bytes;  // receptions shorter than this never reach the computer (V23); 0: off; at most
                              // k_max_min_frame_bytes

    // 6 bytes/s both ways, 1500 Hz at -3 dBFS, 300..2700 Hz, TX delay 100 ms, TX tail 100 ms, the adaptive decision
    // line, the channel defaults of AccessConfig, the fade bridge k_default_fade_bridge, the minimum frame
    // k_default_min_frame_bytes.
    ModemConfig();
    bool valid() const;               // both halves valid at 8000 Hz, the same speed, a slot time, the minimum frame
    EncoderConfig sent() const;       // signal as sent: the fade bridge lengthens a silent lead-in to 300 ms
    DecoderConfig heard() const;      // receiver as run: the fade bridge set
};

// Contexts as in ModemTransmitter; audio_input() is the receive context: it runs the Decoder and calls the host
// handler (the bytes for the computer) and the event tap from inside. In half duplex the receiver hears silence while
// PTT is keyed (like a radio's muted receiver): nothing of one's own transmission reaches the computer.
class Modem {
public:
    static const uint8_t k_host_chunk = 32;  // bytes for the computer are handed over in pieces of at most this

    // An invalid configuration leaves the modem inert (host_input() takes nothing, the receiver stays idle).
    Modem(const ModemConfig& config, HostHandler host, PttHandler ptt, void* context, WakeHandler wake = nullptr);

    size_t host_input(const uint8_t* data, size_t size);       // computer
    void audio_input(const int16_t* samples, size_t count);    // receive: 8 kHz from the radio
    void audio_output(int16_t* out, size_t count);             // audio: 8 kHz to the radio
    void tick(uint32_t now_ms);                                // control
    uint32_t next_tick_ms() const;                             // control

    // Every decoder event, after the modem handled it (monitors, a TUI); set before the audio starts.
    void set_event_tap(EventHandler tap, void* context);

    bool valid() const;
    bool dcd() const;            // the receiver decodes a transmission (Decoder::dcd())
    bool transmitting() const;   // PTT keyed
    ChannelState channel_state() const;
    ModemCounters counters() const;
    uint16_t lookahead_samples() const;  // events come this long after the audio that caused them (spec 3.1)
    const ModemConfig& config() const;
    const EncoderConfig& signal() const;   // as sent
    const DecoderConfig& receiver() const; // as run

private:
    static void on_event(const Event& event, void* context);
    void open();
    void close(bool lost);
    void release_held();
    void drop_held();
    void put_escaped(uint8_t value);
    void put(uint8_t byte);
    void flush();

    ModemConfig config_;
    bool valid_;
    DecoderConfig receiver_;
    HostHandler host_;
    void* context_;
    EventHandler tap_;
    void* tap_context_;
    ModemTransmitter transmitter_;
    Decoder decoder_;
    uint8_t out_[k_host_chunk];
    uint8_t out_fill_;
    bool open_;     // C0 00 went to the computer: a frame is open
    bool holding_;  // a reception shorter than min_frame_bytes so far: its bytes wait in held_ (V23)
    uint8_t held_[k_max_min_frame_bytes];
    uint8_t held_count_;
    uint32_t frames_received_;  // these counters: published for counters()
    uint32_t bytes_received_;
    uint32_t ends_;
    uint32_t losses_;
    uint32_t short_frames_;
    uint32_t short_bytes_;
    uint32_t muted_samples_;
};

}  // namespace unlimited
