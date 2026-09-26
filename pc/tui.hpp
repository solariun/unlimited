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

// One data slot of a package as the packages panel draws it. Decoder: from its slot event, the level and the
// decision line in % of the reference line at that slot (spec 3.10). Encoder: as sent, 100 for a beep and 0 for a
// silence, no decision line.
struct SlotBar {
    uint8_t level_pct;
    uint8_t threshold_pct;
    uint8_t bit;
    uint8_t flags;
};

// Terminal view of a transmission (D24, spec 6.5): status, scope, packages panel, spectrum strip and text. The
// packages panel is the picture of spec 3.10: START and STOP bars, the dashed reference line from the START crest to
// the STOP crest, the amber decision line, one bar per data slot with its bit under it and the bytes under the bits
// (decoder: the last packages that fit; encoder: the slots being sent, the package being sent completed ahead of the
// caret). The Tui only observes: audio samples, EncoderStatus (encoder) and decoder Events. Single-threaded.
class Tui {
public:
    explicit Tui(TuiMode mode);
    ~Tui();

    Tui(const Tui&) = delete;
    Tui& operator=(const Tui&) = delete;

    void set_color(bool enabled);                                       // default on
    void set_profile(const std::string& profile);                       // e.g. "hf", "ssb 8-64 ms"
    void set_tone_hz(float tone_hz);                                    // decoder events override it
    void set_slot_ms(float slot_ms);                                    // decoder events override it
    void set_package(uint8_t bits_per_package);                         // encoder: N as sent
    void set_passband(const Passband& passband);                        // encoder: the sender's; decoder: its own
    // The pitches the receiver searches (encoder: search_range(config); decoder: config.search_range()): the status
    // shift tolerance stops at their edges (spec 1.5). Default 300..2700 Hz, the widest any receiver searches.
    void set_search_range(const Passband& search);
    void set_field(const std::string& key, const std::string& value);  // extra status item; "" removes it

    void push_audio(const int16_t* samples, std::size_t count, uint32_t sample_rate_hz);
    // At least once per slot, after set_package(): the sent text is rebuilt from the bits of every package, so
    // short final packages are exact.
    void on_encoder_status(const EncoderStatus& status);
    void on_event(const Event& event);

    // Exactly `rows` lines, each ended by '\n' and at most `columns` cells wide.
    std::string render(int columns, int rows) const;
    // The bytes draw() writes for a terminal of that size: cursor home, lines, clear-to-end-of-line.
    std::string frame(int columns, int rows, bool clear) const;

    bool open(std::FILE* out = stdout);  // false when out is not a terminal; hides the cursor
    void draw();                         // one frame at the current terminal size, one fwrite
    void close();                        // final frame, cursor back; the frame stays on screen

    // A decided (decoder) or sent (encoder) package.
    struct PackageView {
        uint32_t generation;       // lock (decoder) or transmission (encoder) it belongs to
        uint32_t index;            // package_index
        uint8_t bits_per_package;  // N: the stream bit of data slot i is index * N + i - 1
        uint8_t count;             // d, its data slots
        uint8_t start_pct;         // START and STOP crests, % of the running marker reference
        uint8_t stop_pct;
        uint8_t flags;
        bool complete;             // decoder: its package event came
        float slot_ms;             // its own T
        SlotBar slots[k_max_bits_per_package];
    };

    struct ByteView {
        uint32_t generation;
        uint32_t byte_index;
        uint8_t value;
    };

    struct SentSlot {  // encoder: one slot as sent
        EncoderStatus status;
        uint32_t generation;
    };

private:
    void recent_audio(std::size_t count, std::vector<float>& out) const;
    void append_text(uint8_t byte);
    void new_generation();
    void add_slot(const Event& event);
    void finish_package(const Event& event);
    PackageView& open_package(const Event& event);
    std::vector<std::string> status_items() const;

    TuiMode mode_;
    bool color_;
    std::string profile_;
    std::vector<std::pair<std::string, std::string> > fields_;
    float tone_hz_;
    float slot_ms_;
    float snr_db_;
    bool has_snr_;
    uint8_t bits_per_package_;  // 0 = not known yet
    Passband passband_;
    bool has_passband_;
    Passband search_;

    std::vector<int16_t> audio_;
    std::size_t audio_head_;
    std::size_t audio_fill_;
    uint32_t audio_rate_hz_;
    double audio_seconds_;

    uint32_t generation_;
    std::deque<PackageView> packages_;  // decoder, oldest first
    std::deque<ByteView> byte_views_;   // decoder: bytes of the recent packages, for the labels under the bits
    std::deque<SentSlot> sent_;         // encoder: the recent slots, oldest first

    EncoderStatus status_;
    bool has_status_;
    uint8_t sent_bits_;       // encoder: bits of the byte being rebuilt
    uint8_t sent_bit_count_;

    DecoderState state_;
    uint8_t last_flags_;      // decoder: flags of the last byte
    bool late_join_;
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
