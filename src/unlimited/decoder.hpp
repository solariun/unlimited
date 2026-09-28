#pragma once

#include "unlimited/dsp.hpp"

namespace unlimited {

// Decision line (spec 3.5, V14): the fixed line sits at threshold_percent of the window's reference line.
static const uint8_t k_default_threshold_percent = 70;
static const uint8_t k_min_threshold_percent = 50;
static const uint8_t k_max_threshold_percent = 90;

enum class DecisionMode : uint8_t {
    fixed,    // threshold_percent of the reference line (70 %: Gustavo's rule, the default)
    adaptive  // v0.3's smart line: 50..75 % of the reference line (about 70 % on weak signals), never below the noise
};

enum class DecoderState : uint8_t { search, acquire, track };

enum class EventType : uint8_t { state, locked, slot, byte, end, lost };

enum class LostReason : uint8_t {
    none,
    framing,  // k_max_framing_errors windows in a row lost their START or STOP while a signal was present
    reset     // reset() was called while tracking
};

enum EventFlag : uint8_t {
    event_flag_weak = 0x01,     // a bit within 12.5 % of its decision line
    event_flag_blanked = 0x02,  // the impulse blanker cut part of the window
    event_flag_framing = 0x04   // slot: the window's START or STOP is missing; its byte is dropped (no byte event)
};

struct Event {
    EventType type;
    LostReason reason;             // lost
    DecoderState state;            // new state for EventType::state, current state otherwise
    uint8_t flags;                 // slot, byte: EventFlag bits
    uint8_t value;                 // byte: the byte; slot: the bit (0 or 1)
    uint8_t slot;                  // slot: 1..8, the data slot in its window (bit 8 - slot)
    uint8_t level_pct;             // slot: level, % of the reference line at this slot (<= 255)
    uint8_t threshold_pct;         // slot: the decision line, same units
    uint8_t start_pct;             // slot, byte: the window's START level, % of the running reference (<= 255)
    uint8_t stop_pct;              // slot, byte: its STOP level, same units
    int8_t soft[k_bits_per_byte];  // byte: bits MSB first; slot: soft[0]; > 0 means 1, 64 = one line of margin
    uint32_t byte_index;           // slot, byte: the window's position, 0 = the first byte after the anchor
    float tone_hz;                 // locked, slot, byte: the pitch heard
    float slot_ms;                 // locked, slot, byte: T as measured (the sender's clock)
    float snr_db;                  // locked, slot, byte: key-down tone over the noise in 2500 Hz
};

typedef void (*EventHandler)(const Event& event, void* context);

// The receiver is told the speed (the same B as the sender) and its audio passband; it finds the pitch itself.
struct DecoderConfig {
    uint32_t slot_us;            // T = 1 / (10 B), k_min_slot_us..k_max_slot_us: slot_us_for_speed(B)
    Passband passband;           // the radio's audio passband; the pitch search stays inside it (spec 3.2)
    uint8_t threshold_percent;   // DecisionMode::fixed: k_min_threshold_percent..k_max_threshold_percent (70)
    DecisionMode decision_mode;  // fixed (the default) or adaptive
    bool impulse_blanker;

    DecoderConfig();  // 6 bytes/s, 300..2700 Hz, the fixed 70 % line, blanker on
    ConfigError check() const;
    bool valid() const;  // check() == ConfigError::none
    // Pitches the search looks at: search_range(passband, slot_us) (spec 3.2).
    Passband search_range() const;
};

class Decoder {
public:
    Decoder(const DecoderConfig& config, EventHandler handler, void* context);

    void reset();
    void process(const int16_t* samples, size_t count);
    void process_sample(int16_t sample);

    DecoderState state() const;
    bool dcd() const;                   // carrier detect: state() != DecoderState::search
    float tone_hz() const;              // the pitch held; 0 in SEARCH
    float slot_ms() const;              // T as measured while tracking, else the configured T
    float snr_db() const;               // while tracking, else 0
    uint32_t framing_errors() const;    // windows dropped since the last lock
    uint16_t lookahead_samples() const; // the look-ahead delay: events come this much after the audio (spec 3.1)
    uint8_t acquire_windows() const;    // windows checked before `locked` (spec 3.3): 2, and 4 at 25 bytes/s
    const DecoderConfig& config() const;

private:
    // One window measured on a grid (spec 3.3-3.5): the coherent sums of its 10 slots over their central 0.75 T, the
    // levels (crest units, noise removed) and energies over the noise, and the tone slots' early/late balance.
    struct Window {
        dsp::Complex sum[k_window_slots];
        float level[k_window_slots];
        float snr[k_window_slots];      // |S|^2 / noise of the window
        bool valid;                     // the history held it
    };

    // How a candidate START checked out (spec 3.3): a steady tone (loud slot edges: a VOX lead, a carrier, CW) is no
    // data, so the scan may take the next onset after it; missing markers mean data on a wrong grid.
    enum class Check : uint8_t { ok, steady, markers };

    // A decided window: bits MSB first, soft values, the lines in % and flags.
    struct Decision {
        uint8_t value;
        uint8_t flags;
        uint8_t level_pct[k_bits_per_byte];
        uint8_t threshold_pct[k_bits_per_byte];
        int8_t soft[k_bits_per_byte];
        uint8_t start_pct;
        uint8_t stop_pct;
        bool start_present;
        bool stop_present;
    };

    // The anchor scan (spec 3.3): slot-long windows of block energies, one start position per block, oldest first.
    struct Scan {
        uint32_t next;          // absolute block of the next slot start to look at
        uint32_t origin;        // where the scan (re)started
        uint32_t loud_until;    // one past the last loud position (the origin before any)
        uint32_t watch_until;   // watching: the candidate is found once the scan passes this block
        bool loud;              // a loud position was seen since the origin
        bool in_data;           // beeps seen without a valid anchor: wait for a silent window
        bool trusted;           // nothing before the origin can be a transmission already running
        bool watching;          // `anchor` holds a candidate START; a much louder onset right after it replaces it
        bool found;             // `anchor` holds a candidate START to verify
        float anchor;           // its position, history blocks from origin_block_
        float excess;           // watching: the candidate's slot energy over the noise
    };

    void initialize();
    void on_search_block();
    void on_block(int32_t re, int32_t im, uint32_t energy);
    void rebase();

    // State changes.
    void set_state(DecoderState state);
    void enter_search();
    void enter_acquire();
    void retune(float tone_hz, uint8_t mixed_samples, uint32_t keep_from);
    void forget_history(bool trusted);

    // Acquisition (spec 3.2, 3.3).
    void steer(bool holding);
    void restart_scan(uint32_t origin, bool trusted);
    void scan_step_all();
    bool scan_step(Scan& scan, uint32_t limit);
    void watch(Scan& scan, uint32_t position, float excess) const;
    float group_energy(float from, float blocks, float& noise, uint32_t& groups) const;
    static float loud_ratio(uint32_t groups);
    bool locate_onset(uint32_t loud_from, bool walk_back, float& position) const;
    bool loud_slot(float start, float& excess) const;
    bool loud_before(float position) const;
    bool quiet_before(float start, float excess) const;
    float refine_anchor(float anchor) const;
    void run_acquire();
    void reject(Check check);
    bool refine_pitch(float anchor);
    float refine_timing(float anchor, uint8_t windows) const;
    bool steady_start(float anchor) const;
    float energy_density(float from, float blocks) const;
    Check windows_check(float anchor, float& reference) const;

    // Tracking (spec 3.4-3.7).
    void run_track();
    bool track_step();
    bool window_ready(float start) const;
    Window measure(float start) const;
    Decision decide(const Window& window) const;
    void emit_window(const Decision& decision, float start);
    void update_loops(const Window& window, const Decision& decision, float start);
    float timing_error(float start, const Decision& decision) const;
    bool quiet_window(const Window& window, float reference) const;
    bool silent_window(const Window& window) const;
    bool tone_in(const Window& window, uint8_t slot, float reference) const;
    bool markers_present(const Window& window, float reference) const;
    bool resolve_pending();
    void continue_after_pending(float next);
    void finish_end(float next_start);
    void lose(LostReason reason, float from);
    void lock(float anchor, float reference);

    // Measurement.
    float slot_level(const dsp::Complex& sum) const;
    float window_noise(float slots) const;  // expected |S|^2 of noise over `slots` of T
    float edge_ratio(float start, uint8_t windows) const;
    float slot_density(const dsp::Complex& sum) const;
    float edge_density(float boundary) const;
    float noise_variance() const;
    float live_end() const;
    float blocks_to_ms(float blocks) const;
    uint32_t absolute(float position) const;
    float relative(uint32_t block) const;
    bool marker_present(float level, float snr, float reference) const;

    Event make_event(EventType type) const;
    void emit(const Event& event);

    DecoderConfig config_;
    EventHandler handler_;
    void* context_;
    DecoderState state_;
    uint8_t block_samples_;  // 0 = invalid configuration, the decoder stays idle
    uint8_t block_fill_;
    uint8_t settle_blocks_;
    bool settle_trusted_;    // the scan restarts trusted after the settle (the tone came up fresh)
    uint16_t fresh_blocks_;  // search blocks since a tone came up within which its START is still in the history
    uint16_t protect_blocks_;  // ... within which the scan may not have reached it yet
    float band_reach_hz_;      // half the occupied band and a search bin: a candidate's own sidebands lie within
    float refine_reach_hz_;    // the largest pitch correction of a candidate (a quarter of the band, the re-mix reach)
    uint8_t acquire_windows_;  // windows of the check before lock (spec 3.3)
    uint32_t block_energy_;

    dsp::Lookahead lookahead_;
    dsp::Nco nco_;
    dsp::Cic2 cic_;
    dsp::ImpulseBlanker blanker_;
    int32_t blank_delay_re_[dsp::ImpulseBlanker::k_latency];
    int32_t blank_delay_im_[dsp::ImpulseBlanker::k_latency];
    dsp::PrefixHistory history_;
    dsp::ToneSearch search_;
    dsp::NoiseTracker noise_;       // TRACK: the central half of decided zeros
    float search_noise_;            // sigma^2 per sample from the tone search's floor (SEARCH, ACQUIRE)

    uint32_t origin_block_;         // positions are float history blocks from here (rebased)
    float slot_blocks_;             // T in history blocks (nominal)
    float scan_group_;              // blocks summed coherently by the scan (k_scan_coherent_ms)
    Scan scan_;
    Scan pending_scan_;             // TRACK: the new transmission a silent START may begin (spec 3.7)
    uint32_t acquire_block_;        // history block when ACQUIRE began (timeout)
    bool start_checked_;            // the candidate's first slots passed the early steady test
    bool pitch_refined_;            // the anchor's pitch was refined at the last block; its windows are checked next
    float verify_hz_;               // the pitch before that refinement (restored when the candidate fails)
    bool locked_tone_;              // the tone search locked (ACQUIRE by the search, not by an anchor)
    float locked_hz_;

    // TRACK.
    float window_start_;            // the next window's START, history blocks from origin_block_
    uint32_t byte_index_;           // of the next window
    float drift_;                   // timing loop integral: blocks per window beyond 10 T
    float reference_;               // running START/STOP level (crest units)
    dsp::Complex last_stop_;        // the previous window's STOP sum (pitch loop)
    bool has_last_stop_;
    uint8_t framing_run_;
    uint32_t framing_errors_;
    // A silent START (spec 3.7): the old transmission ended and a new one may begin inside that window (back to back),
    // or the START faded and the old one goes on. Both are weighed on the next windows before anything is released.
    bool pending_;
    bool pending_new_;              // a new START inside the window checked out
    uint8_t pending_stage_;         // both hypotheses held on this many more windows (0: not weighed yet)
    float pending_start_;           // the silent START's window
    float pending_anchor_;          // the new START
    float pending_reference_;       // its reference
    Window pending_window_;         // the window with the silent START
};

}  // namespace unlimited
