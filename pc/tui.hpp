#pragma once

#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace unlimited {
namespace pc {

const double k_default_refresh_hz = 20.0;

enum class TuiMode { encoder, decoder };

// Visible cells of a rendered line: UTF-8 code points outside ANSI escape sequences.
std::size_t display_width(const std::string& line);

// One peak slot of the header or frame a Tui shows: as sent (encoder) or as decided (decoder slot event).
struct PeakSlot {
    uint8_t tone;
    uint8_t symbol;
    uint8_t level_pct;   // decoder: % of the START/STOP crest
    uint8_t confidence;  // decoder: 0.5 dB steps
    uint8_t flags;       // decoder: event_flag_erasure, event_flag_blanked
    bool present;
};

// Terminal view of a transmission (D24, spec 6.5): scope, peaks panel (a tone-by-slot grid of the header or
// frame, and on the decoder the slot levels against the START/STOP crest), spectrum strip with the tone grid,
// status and text. It only observes: audio samples, EncoderStatus (encoder) and decoder Events.
// Single-threaded.
class Tui {
public:
    explicit Tui(TuiMode mode);
    ~Tui();

    Tui(const Tui&) = delete;
    Tui& operator=(const Tui&) = delete;

    void set_color(bool enabled);                                       // default on
    void set_profile(const std::string& profile);                       // e.g. "ssb", "hf"
    void set_tone_hz(float tone_hz);                                    // decoder events override it
    void set_slot_ms(float slot_ms);                                    // decoder events override it
    void set_mode(uint8_t bits_per_peak, uint8_t data_slots, Spacing spacing, GridSide side);  // as sent
    void set_field(const std::string& key, const std::string& value);  // extra status item; "" removes it

    void push_audio(const int16_t* samples, std::size_t count, uint32_t sample_rate_hz);
    void on_encoder_status(const EncoderStatus& status);  // at least once per slot: the text is rebuilt from
                                                          // the symbols of every peak (set_mode() first)
    void on_event(const Event& event);

    // Exactly `rows` lines, each ended by '\n' and at most `columns` cells wide.
    std::string render(int columns, int rows) const;
    // The bytes draw() writes for a terminal of that size: cursor home, lines, clear-to-end-of-line.
    std::string frame(int columns, int rows, bool clear) const;

    bool open(std::FILE* out = stdout);  // false when out is not a terminal; hides the cursor
    void draw();                         // one frame at the current terminal size, one fwrite
    void close();                        // final frame, cursor back; the frame stays on screen

private:
    void recent_audio(std::size_t count, std::vector<float>& out) const;
    void clear_peaks();
    void flush_sent_frame();
    void append_text(uint8_t byte);
    void set_mode_from(const Event& event);

    TuiMode mode_;
    bool color_;
    std::string profile_;
    std::vector<std::pair<std::string, std::string> > fields_;
    float tone_hz_;
    float slot_ms_;
    float snr_db_;
    bool has_snr_;

    uint8_t bits_per_peak_;  // 0 = no mode yet
    uint8_t data_slots_;
    Spacing spacing_;
    int8_t side_;            // +1 grid above f_ref, -1 below (decoder: as received)
    bool mode_memory_;

    std::vector<int16_t> audio_;
    std::size_t audio_head_;
    std::size_t audio_fill_;
    uint32_t audio_rate_hz_;
    double audio_seconds_;

    PeakSlot peaks_[k_max_data_slots];
    uint32_t peaks_frame_;       // decoder: frame_index of peaks_; encoder: slot_index of their first peak

    EncoderStatus status_;
    bool has_status_;
    EncoderSegment peaks_segment_;  // encoder: header or frame held in peaks_
    bool has_peaks_;
    bool sent_flushed_;             // the frame in peaks_ is already in the text

    DecoderState state_;
    Event last_byte_;
    Event last_slot_;
    bool has_byte_;
    bool has_slot_;
    uint32_t bytes_;
    uint32_t locks_;
    uint32_t losses_;
    uint32_t ends_;
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
