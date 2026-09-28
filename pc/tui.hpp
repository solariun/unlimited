#pragma once

#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace unlimited {
namespace pc {

const double k_default_refresh_hz = 20.0;
const double k_level_window_ms = 300.0;  // the level meter's integration time, a VU meter's

enum class TuiMode { encoder, decoder };

// Visible cells of a rendered line: UTF-8 code points outside ANSI escape sequences.
std::size_t display_width(const std::string& line);

// A level measured over some samples (spec 12.6): the peak and the RMS in dBFS, full scale 32768 (a full-scale sine
// reads a peak of 0 dBFS and an RMS of -3.0 dBFS; digital silence reads -infinity), and the clips: samples at -32768
// or +32767, where the converter ran out of range.
struct Level {
    std::uint64_t samples;  // how many samples it covers; 0: nothing measured
    double peak_dbfs;
    double rms_dbfs;
    std::uint64_t clips;
};

// "peak -6.0 dBFS, RMS -9.0 dBFS, 0 clips" (the clips only when with_clips), "digital silence" or "no audio".
std::string level_text(const Level& level, bool with_clips);

// The programs' level meter (spec 12.6): the recent level (the last whole window of window_ms; the window being
// filled until the first one is whole), the level since the start, and how long the audio has stayed at digital
// silence (every sample 0: on macOS, an input the terminal may not record). Another sample rate is another stream:
// the recent level and the silence start over. push() is arithmetic only (no lock, no allocation), so a real-time
// callback may call it; one thread uses a meter.
class LevelMeter {
public:
    explicit LevelMeter(double window_ms = k_level_window_ms);

    void push(const std::int16_t* samples, std::size_t count, std::uint32_t sample_rate_hz);
    Level recent() const;
    Level total() const;
    double silent_seconds() const;  // how long every sample has been 0, up to the newest one

private:
    struct Sums {
        std::uint64_t samples;
        std::uint32_t peak;  // the largest |sample|, 0..32768
        double squares;
        std::uint64_t clips;
    };

    static void add(Sums& sums, std::int16_t sample);
    static Level level_of(const Sums& sums);

    double window_ms_;
    std::uint32_t sample_rate_hz_;
    std::uint64_t window_samples_;
    Sums block_;  // the window being filled
    Sums last_;   // the last whole window
    Sums total_;
    std::uint64_t zero_run_;
};

// Hands encoder statuses from the thread that renders the encoder (a live output's real-time callback, the consumer of
// spec 2.5: only it may call Encoder::status()) to the thread that draws the view: one producer, one consumer,
// lock-free (release/acquire on two counters), fixed capacity allocated at construction. push() never blocks or
// allocates: when the view falls a whole ring behind, the status is dropped and counted.
class StatusRing {
public:
    explicit StatusRing(std::size_t min_capacity);  // rounded up to a power of two
    StatusRing(const StatusRing&) = delete;
    StatusRing& operator=(const StatusRing&) = delete;

    bool push(const EncoderStatus& status);  // producer; false when the ring is full (dropped)
    bool pop(EncoderStatus& status);         // consumer; false when empty
    std::size_t capacity() const;
    std::uint32_t dropped() const;

private:
    std::vector<EncoderStatus> slots_;
    std::size_t mask_;
    std::atomic<std::size_t> written_;
    std::atomic<std::size_t> read_;
    std::atomic<std::uint32_t> dropped_;
};

// One data slot of a window as the windows panel draws it. Decoder: from its slot event, the level and the decision
// line in % of the reference line at that slot (spec 3.5). Encoder: as sent, 100 for a beep and 0 for a silence, no
// decision line.
struct SlotBar {
    uint8_t level_pct;
    uint8_t threshold_pct;
    uint8_t bit;
    uint8_t flags;
};

// Terminal view of a transmission (spec 6): status, scope, windows panel, spectrum strip and text. The windows panel is
// the picture of spec 3.5: each byte's window with its START and STOP bars, the dashed reference line from the START
// level to the STOP level, the amber decision line, one bar per data slot with its bit under it and the byte under the
// bits (decoder: the last windows that fit, a window dropped as a framing error marked; encoder: the slots being sent,
// the window being sent completed ahead of the caret). The Tui only observes: audio samples, EncoderStatus (encoder)
// and decoder Events. Single-threaded: on live audio it runs on the input's worker thread or the main thread, never
// in the audio callback (spec 12.6).
class Tui {
public:
    explicit Tui(TuiMode mode);
    ~Tui();

    Tui(const Tui&) = delete;
    Tui& operator=(const Tui&) = delete;

    void set_color(bool enabled);                                       // default on
    void set_label(const std::string& label);                           // after RX/TX, e.g. a device name
    void set_speed(float bytes_per_second);                             // the speed both sides were given
    void set_tone_hz(float tone_hz);                                    // decoder events override it
    void set_slot_ms(float slot_ms);                                    // decoder events override it
    void set_passband(const Passband& passband);                        // encoder: the sender's; decoder: its own
    // The pitches the receiver searches (encoder: search_range(config); decoder: config.search_range()): the status
    // shift tolerance stops at their edges (spec 1.3). Default 300..2700 Hz, the widest any receiver searches.
    void set_search_range(const Passband& search);
    void set_field(const std::string& key, const std::string& value);  // extra status item; "" removes it
    // An extra status item right after the state, before the rest (a modem's own items, spec 12.6: they stay on screen
    // in a small terminal); "" removes it.
    void set_front_field(const std::string& key, const std::string& value);
    // The level meter item, after the state (spec 12.6): decoder "in peak -12.3 dBFS, RMS -28.4 dBFS, 0 clips" (the
    // input), encoder "out peak -3.0 dBFS, RMS -9.1 dBFS" (the output).
    void set_level(const Level& level);
    // A warning item right after the level (e.g. an input at digital silence); "" removes it.
    void set_warning(const std::string& warning);
    // Decoder view: DCD as the receiver has it (Decoder::dcd() or Modem::dcd(), spec 3.10), shown after the state;
    // off until set.
    void set_dcd(bool on);

    void push_audio(const int16_t* samples, std::size_t count, uint32_t sample_rate_hz);
    // At least once per slot: every new slot is kept, and a window's byte joins the sent text at its STOP.
    void on_encoder_status(const EncoderStatus& status);
    void on_event(const Event& event);

    // Exactly `rows` lines, each ended by '\n' and at most `columns` cells wide.
    std::string render(int columns, int rows) const;
    // The bytes draw() writes for a terminal of that size: cursor home, lines, clear-to-end-of-line.
    std::string frame(int columns, int rows, bool clear) const;
    // The next frame for a terminal of that size: the screen is cleared first when the size differs from the last
    // frame's (a resize is followed at the next frame).
    std::string next_frame(int columns, int rows);

    bool open(std::FILE* out = stdout);  // false when out is not a terminal; hides the cursor
    void draw();                         // one frame at the current terminal size, one fwrite
    void close();                        // final frame, cursor back; the frame stays on screen

    // A decided window (decoder): its slot events, then its byte event (or none: a framing error).
    struct WindowView {
        uint32_t generation;  // lock it belongs to
        uint32_t index;       // byte_index
        uint8_t count;        // data slots seen
        uint8_t start_pct;    // START and STOP levels, % of the running reference
        uint8_t stop_pct;
        uint8_t flags;
        uint8_t value;        // the byte, once complete
        bool complete;        // its byte event came
        bool dropped;         // a framing error: no byte
        SlotBar slots[k_bits_per_byte];
    };

    struct SentSlot {  // encoder: one slot as sent
        EncoderStatus status;
        uint32_t generation;
    };

private:
    void recent_audio(std::size_t count, std::vector<float>& out) const;
    void append_text(uint8_t byte);
    void new_generation();
    WindowView& open_window(const Event& event);
    std::vector<std::string> status_items() const;

    TuiMode mode_;
    bool color_;
    std::string label_;
    std::vector<std::pair<std::string, std::string> > fields_;
    std::vector<std::pair<std::string, std::string> > front_fields_;
    Level level_;
    bool has_level_;
    std::string warning_;
    float bytes_per_second_;  // 0 = not set
    float tone_hz_;
    float slot_ms_;
    float snr_db_;
    bool has_snr_;
    Passband passband_;
    bool has_passband_;
    Passband search_;

    std::vector<int16_t> audio_;
    std::size_t audio_head_;
    std::size_t audio_fill_;
    uint32_t audio_rate_hz_;
    double audio_seconds_;

    uint32_t generation_;
    std::deque<WindowView> windows_;  // decoder, oldest first
    std::deque<SentSlot> sent_;       // encoder: the recent slots, oldest first

    EncoderStatus status_;
    bool has_status_;

    DecoderState state_;
    bool dcd_;
    uint8_t last_flags_;      // decoder: flags of the last byte
    uint32_t bytes_;
    uint32_t locks_;
    uint32_t losses_;
    uint32_t ends_;
    uint32_t dropped_;        // decoder: windows dropped as framing errors
    std::string last_event_;
    std::string text_;

    std::FILE* out_;
    bool open_;
    int drawn_columns_;
    int drawn_rows_;
};

// Limits screen refreshes: due() never blocks, wait() sleeps until the next refresh; next() is when the next one is
// due, for a caller that waits on its own events until then.
class RefreshPacer {
public:
    typedef std::chrono::steady_clock Clock;

    explicit RefreshPacer(double rate_hz = k_default_refresh_hz);
    bool due();
    void wait();
    Clock::time_point next() const;

private:
    void consume(Clock::time_point now);

    Clock::duration period_;
    Clock::time_point next_;
};

// Paces file audio to real time (--realtime): advance() sleeps until the audio time of every
// sample handed over so far has elapsed since restart().
class RealtimePacer {
public:
    explicit RealtimePacer(uint32_t sample_rate_hz);
    void restart();
    void advance(std::size_t samples);

private:
    typedef std::chrono::steady_clock Clock;

    Clock::time_point start_;
    uint64_t samples_;
    uint32_t sample_rate_hz_;
};

}  // namespace pc
}  // namespace unlimited
