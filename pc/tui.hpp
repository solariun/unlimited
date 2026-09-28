#pragma once

#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

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

enum class TuiMode { encoder, decoder };

// Visible cells of a rendered line: UTF-8 code points outside ANSI escape sequences.
std::size_t display_width(const std::string& line);

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
// and decoder Events. Single-threaded.
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

    void push_audio(const int16_t* samples, std::size_t count, uint32_t sample_rate_hz);
    // At least once per slot: every new slot is kept, and a window's byte joins the sent text at its STOP.
    void on_encoder_status(const EncoderStatus& status);
    void on_event(const Event& event);

    // Exactly `rows` lines, each ended by '\n' and at most `columns` cells wide.
    std::string render(int columns, int rows) const;
    // The bytes draw() writes for a terminal of that size: cursor home, lines, clear-to-end-of-line.
    std::string frame(int columns, int rows, bool clear) const;

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

// Limits screen refreshes: due() never blocks, wait() sleeps until the next refresh.
class RefreshPacer {
public:
    explicit RefreshPacer(double rate_hz = k_default_refresh_hz);
    bool due();
    void wait();

private:
    typedef std::chrono::steady_clock Clock;

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
