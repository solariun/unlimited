#pragma once

#include "unlimited/dsp.hpp"

namespace unlimited {

enum class Profile : uint8_t {
    ssb,  // HF USB or LSB: T = 16..128 ms, f_ref 300..2700 Hz
    am,   // AM: T = 8..64 ms, f_ref 300..2700 Hz
    fm    // VHF/UHF FM: T = 4..32 ms (a sender uses >= 6), f_ref 1000..2700 Hz
};

enum class DecoderState : uint8_t { search, acquire, preamble, track };

enum class EventType : uint8_t { state, locked, slot, byte, end, lost };

enum class LostReason : uint8_t { none, signal_gone, alias, preamble_timeout, reset, no_header, unsupported_mode };

enum EventFlag : uint8_t {
    event_flag_late_join = 0x01,
    event_flag_flywheel_start = 0x02,
    event_flag_flywheel_stop = 0x04,
    event_flag_blanked = 0x08,
    event_flag_erasure = 0x10,      // a slot of this byte had E_best < 2 E_second
    event_flag_mode_memory = 0x20,  // mode inherited from an earlier header
    event_flag_blind_mode = 0x40    // v0.2b: mode estimated from the data
};

struct Event {
    EventType type;
    LostReason reason;
    DecoderState state;           // new state for EventType::state, current state otherwise
    uint8_t flags;
    uint8_t value;                // byte: the byte; slot: the k-bit symbol
    uint8_t index;                // byte: 0..frame_bytes-1 in its frame; slot: 1..N
    uint8_t tone;                 // slot: grid tone index as received
    uint8_t level_pct;            // slot: winner amplitude, % of the frame's START crest (<= 255)
    uint8_t confidence;           // slot: 10 log10(E_best / E_second), 0.5 dB steps (<= 255)
    int8_t soft[k_bits_per_byte]; // byte: bit LLRs MSB first; slot: soft[0..k-1]; > 0 means 1; 16 per nat, |soft| <= 112
    uint8_t bits_per_peak;        // mode, valid from locked to end/lost, else 0
    uint8_t data_slots;
    Spacing spacing;
    int8_t side;                  // +1 grid above f_ref as received, -1 below
    uint32_t frame_index;         // 0 = first data frame after the header
    float tone_hz;                // f_ref
    float slot_ms;                // exact T once the mode is known
    float snr_db;
};

static_assert(k_max_bits_per_peak <= k_bits_per_byte, "Event::soft holds the LLRs of one peak");

typedef void (*EventHandler)(const Event& event, void* context);

// The receiver chooses one thing: the T range it accepts (the tone search follows the radio path). A sender's mode is
// learnt from its header; T below 16 ms over SSB needs the am profile.
struct DecoderConfig {
    uint8_t min_slot_ms;   // accepted T = min_slot_ms .. 8 * min_slot_ms; block = min_slot_ms samples
    uint16_t min_tone_hz;  // f_ref search range
    uint16_t max_tone_hz;
    bool impulse_blanker;  // also the slot-path blanker

    DecoderConfig();  // for_profile(Profile::ssb)
    static DecoderConfig for_profile(Profile profile);  // fills the fields; the profile itself is not kept
    bool valid() const;
    uint16_t max_slot_ms() const;
};

class Decoder {
public:
    Decoder(const DecoderConfig& config, EventHandler handler, void* context);

    void reset();
    void process(const int16_t* samples, size_t count);
    void process_sample(int16_t sample);

    DecoderState state() const;
    float tone_hz() const;
    float slot_ms() const;
    float snr_db() const;
    uint8_t bits_per_peak() const;  // 0 until a mode is known
    uint8_t data_slots() const;

private:
    struct Marker {
        float position;
        float amplitude;
        bool detected;
    };

    struct Mode {
        uint32_t slot_us;       // exact T; 0 = no mode
        uint8_t bits_per_peak;
        uint8_t data_slots;
        Spacing spacing;
        int8_t side;            // +1 grid above f_ref as received, -1 below
    };

    struct ModeMemory {
        Mode mode;
        float tone_hz;          // f_ref after AFC
        float crest;            // marker crest (reference_average_)
        uint32_t expires_block;
        bool valid;
    };

    // A data frame being decoded or held: bytes, their flags and 4-bit q4 LLRs (two per byte of soft).
    struct FrameBuffer {
        uint8_t bytes[UNLIMITED_MAX_FRAME_BYTES];
        uint8_t flags[UNLIMITED_MAX_FRAME_BYTES];
        uint8_t soft[UNLIMITED_MAX_FRAME_BYTES * k_bits_per_byte / 2];
        uint32_t frame_index;
        uint8_t count;
        uint8_t frame_flags;    // flywheel_start / flywheel_stop, set by its late step
    };

    struct HeaderScore {
        dsp::HeaderDecision decision;
        int32_t start;          // grid index of the header START
    };

    // TRACK only; shares storage with the fine AFC, which works in ACQUIRE and PREAMBLE (spec 3.15).
    struct GridPath {
        dsp::GridBank bank;
        dsp::BinBackground background;
    };

    // What the slots of a finished frame found (sample clock), kept for its early and late steps.
    struct SlotSummary {
        uint32_t confident_bits;  // bit i - 1: slot i was confident
        uint8_t confident;
        uint8_t changes;          // confident slots whose tone differs from the previous confident slot's
        uint8_t last_tone;        // tone of the latest confident slot
        uint8_t buffer;
    };

    // The frame whose late step is due (spec 3.9).
    struct LateFrame {
        Marker start;
        Marker stop;
        float stop_q;
        SlotSummary slots;
        uint8_t first_strong;     // first audit position (1..2N+1) with full-strength evidence, 0 = none
        uint8_t first_active;     // first slot centre 1..N with a marker or a carrier on f_ref, 0 = none
        bool pending;
    };

    struct SyncScore {
        float evidence;
        float amplitude;      // mean crest of the hits
        float boundary;       // boundary_energy() of the markers
        uint8_t hit_bits;     // bit i: a hit at centre - i T
        uint8_t marker_bits;  // the hits and the weak markers
        uint8_t hits;
        uint8_t positions;    // of the prefix scored
    };

    static const uint8_t k_frame_buffers = 4;
    static const uint8_t k_max_held_frames = 2;
    static const uint8_t k_presence_window = 4;
    static const uint8_t k_header_ring = k_header_slots + 1;

    void initialize();
    void on_block(int32_t re, int32_t im, uint32_t energy);
    void rebase();

    // Slot path (sample clock, spec 3.2): one window at a time, placed ahead of its first sample.
    void run_slot_path(int16_t sample);
    void schedule_window(float centre, float length_blocks, int32_t index);
    void open_window(uint32_t index);
    bool window_push(int16_t sample);
    void on_window_closed();

    void set_state(DecoderState state);
    void enter_search(bool keep_statistics = false);
    void lock_tone(float tone_hz);
    void enter_acquire();
    void enter_preamble(float anchor, float slot_blocks, float amplitude, uint8_t hit_bits);
    void enter_track(const Marker& start, const Mode& mode, uint8_t flags, const dsp::HeaderDecision* header,
                     bool header_frame);
    void lose(LostReason reason);
    void finish(uint8_t buffer, uint8_t bytes);

    void run_search();
    bool watching() const;
    bool train_line(float tone_hz) const;
    bool harmonic_image(float tone_hz) const;
    void run_acquire();
    void run_afc(int32_t re, int32_t im);
    void forget_history();
    void push_block_noise();
    void run_candidates(bool tuned);
    bool try_sync(const dsp::Candidate& candidate);
    SyncScore sync_score(float centre, float slot_blocks) const;
    static bool enough_hits(const SyncScore& score);
    static bool strong_hits(const SyncScore& score);
    bool boundary_excess(float boundary, float slot_blocks, float crest, float& excess, float& noise_ratio) const;
    float boundary_energy(const dsp::FlipMeasure* measures, const bool* marker, float slot_blocks, float crest,
                          float& allowance) const;
    void refine_sync(float& centre, float& slot_blocks) const;
    bool try_late_join(const dsp::Candidate& candidate);
    bool has_candidate_near(float position, float tolerance) const;

    // PREAMBLE: the sync train (history clock) and the mode header (sample clock).
    void run_preamble();
    bool preamble_step();
    void continue_train(int32_t g, const dsp::FlipMeasure& m);
    void seed_train_fit(float anchor, float slot_blocks);
    void add_train_point(float index, const dsp::FlipMeasure& m);
    bool train_fit(float index, float& slot_blocks, float& position) const;
    uint8_t sub_rate_train(int32_t gap);
    bool flip_at(int32_t g) const;
    float grid_centre(int32_t g) const;
    void schedule_header_window(int32_t g);
    void open_header_window();
    void close_header_window(int32_t g);
    void header_step(int32_t g);
    bool header_candidate(int32_t start) const;
    bool header_energy(int32_t start, float (&energy)[2][k_header_slots][k_header_slots]) const;
    bool train_ended(int32_t last) const;
    bool try_header(int32_t start, const float (&energy)[2][k_header_slots][k_header_slots], HeaderScore& score);
    bool header_plausible(const dsp::HeaderDecision& decision) const;
    void commit_header(const HeaderScore& score);
    bool mode_from_header(const dsp::HeaderDecision& decision, Mode& mode, LostReason& refusal) const;
    Marker header_start(int32_t g) const;
    void header_timeout();

    // TRACK: early and late steps and the audit on the history clock, slot decisions on the sample clock.
    void run_track();
    float exact_slot_blocks() const;
    void schedule_data_window(uint8_t slot);
    void open_data_window();
    void close_data_window();
    void begin_bank_frame();
    void store_slot(const dsp::SlotDecision& decision, uint8_t slot, bool blanked);
    bool slot_decided(int32_t frame, uint8_t slot) const;
    bool audit_step();
    uint8_t audit_positions() const;
    float frame_reference(const Marker& start) const;
    bool early_ready() const;
    void track_early();
    void stop_afc(const dsp::FlipMeasure& m, bool detected, float half);
    void rotation_afc(const dsp::FlipMeasure& m, bool detected);
    void cache_start_halves(float position);
    bool late_ready() const;
    void track_late();
    void late_step();
    void guard_step(uint8_t buffer, bool present, bool strong, bool alive);
    void begin_audit();
    bool measure_eot(uint8_t centre);
    int8_t eot_evidence(const dsp::FlipMeasure& m, float reference) const;
    bool short_end(uint8_t& stop) const;
    void end_short(const SlotSummary& slots, uint8_t first_strong, const Marker& start, uint8_t stop);
    bool end_of_transmission(float stop) const;
    bool eot_clean(float stop);
    bool frame_present(const SlotSummary& slots, uint8_t peaks, float fraction) const;
    bool tones_vary(const SlotSummary& slots) const;
    bool weak_header_confirms() const;
    bool guard_confirms();
    void track_afc(const dsp::Complex& stop_before, const dsp::Complex& stop_after, float stop_position);
    void add_marker_edges(float marker, float slot_blocks, float crest);
    bool marker_edges_quiet() const;
    bool stops_found() const;
    bool decodable() const;
    void confirm(uint8_t buffer);
    void release_frame(uint8_t buffer, uint8_t bytes);
    void release_held();
    void hold_frame(uint8_t buffer);
    uint8_t free_buffer() const;

    void remember_mode();
    bool memory_valid() const;
    bool memory_usable() const;
    bool stream_relock() const;  // ACQUIRE on the remembered f_ref with no tune heard: the station's running stream
    bool stream_tune(float end, float widest_half) const;
    bool memory_holds(float tone_hz) const;
    bool memory_fits() const;
    float mode_slot_blocks(const Mode& mode) const;

    dsp::FlipMeasure measure_flip(float centre, float half) const;
    dsp::FlipMeasure search_flip(float centre, float span, float half, float kappa_min) const;
    float noise_variance() const;
    float marker_noise(float sigma2, float slot_blocks) const;
    float end_position() const;
    float candidate_position(const dsp::Candidate& candidate) const;
    bool in_range(float slot_blocks) const;
    bool banned(float slot_blocks) const;
    void ban_alias(float slot_blocks);
    void update_reference(float amplitude);
    void reset_scales();

    Event make_event(EventType type) const;
    void emit(const Event& event);
    void emit_slot(const dsp::SlotDecision& decision, uint8_t slot, uint8_t flags);
    void emit_locked(uint8_t flags, uint32_t frame_index);

    DecoderConfig config_;
    EventHandler handler_;
    void* context_;
    DecoderState state_;
    uint8_t block_samples_;  // 0 = invalid configuration, the decoder stays idle
    uint8_t block_fill_;
    uint8_t afc_decimation_;
    uint8_t settle_blocks_;
    uint32_t block_energy_;

    dsp::Nco nco_;
    dsp::Cic2 cic_;
    dsp::ImpulseBlanker blanker_;
    int32_t blank_delay_re_[dsp::ImpulseBlanker::k_latency];
    int32_t blank_delay_im_[dsp::ImpulseBlanker::k_latency];
    dsp::PrefixHistory history_;
    dsp::ToneSearch search_;
    union {
        dsp::FineAfc afc_;  // ACQUIRE, PREAMBLE: configure() on entry
        GridPath grid_;     // TRACK: bank.reset() and background.reset() on entry
    };
    dsp::Complex afc_sum_;
    uint8_t afc_fill_;
    dsp::QuantileTracker noise_;     // SEARCH..PREAMBLE: robust to the signal in its windows
    dsp::NoiseTracker track_noise_;  // TRACK: fed from N_bin of the grid bins
    dsp::CandidateList candidates_;
    dsp::AuditRing audit_;
    float scale_q_[k_candidate_scales][2];
    float scale_kappa_[k_candidate_scales];  // kappa of the newer measure of scale_q_

    // Slot path: blanker, then the header bank (PREAMBLE) or grid_.bank (TRACK).
    dsp::SlotBlanker slot_blanker_;
    dsp::HeaderBank header_bank_;
    uint32_t sample_index_;          // index of the input sample being processed
    uint32_t end_sample_;            // first sample after the newest history block
    uint32_t window_start_;          // first sample of the scheduled window
    float window_offset_;            // its start within that sample, samples (0..1)
    float window_length_;            // samples
    int32_t window_index_;           // PREAMBLE: grid index; TRACK: data slot 1..N
    bool window_pending_;
    bool window_open_;
    bool window_blanked_;

    // PREAMBLE: train flips and the header.
    int16_t header_log_[k_header_ring][k_header_bins];  // log2 Q8.8 energies of the last 9 grid slots
    int32_t header_row_[k_header_ring];                 // grid index held by each row, -1 = none
    HeaderScore header_pending_;     // accepted H(s) waiting for the look-ahead H(s + 1)
    float header_noise_;             // N_h of the latest evaluated hypothesis
    uint32_t flip_bits_;             // bit i: grid index grid_index_ - i was a detected flip
    int32_t last_flip_;              // grid index of the newest detected flip
    bool header_has_pending_;
    bool header_seen_;               // an H(s) on the remembered side held peaks: the header is there

    // Mode and timing.
    Mode mode_;
    ModeMemory memory_;
    float drift_;                    // epsilon: exact T x (1 + epsilon)
    float rotation_hz_;              // offset read on the last STOP missed for its rotation, 0 = none

    // TRACK, history clock: frame track_frame_ (-1 = the header) starts at track_start_.
    Marker track_start_;
    int32_t track_frame_;
    dsp::Complex start_before_;      // START halves cached for track_afc
    dsp::Complex start_after_;
    float start_halves_position_;
    bool start_halves_valid_;
    bool stop_missed_;
    uint8_t audit_next_;             // next audit position of track_frame_, 1..2N+1
    uint8_t first_strong_;
    uint8_t first_active_;
    uint8_t eot_centres_;            // slot centres of track_frame_ with short EOT evidence so far
    int8_t eot_evidence_[k_max_data_slots + 4];  // EOT triples: flip evidence at slot centres 0..N+3, 1/k_audit_scale
    int8_t start_eot_;               // the same at the START of track_frame_ (the STOP just measured)
    LateFrame late_;

    // TRACK, sample clock: frame bank_frame_ starts at bank_start_.
    FrameBuffer frames_[k_frame_buffers];
    float bank_start_;
    float bank_reference_;           // crest behind level_pct: the frame's START, else the running reference
    uint32_t bank_frame_;
    SlotSummary bank_slots_;
    SlotSummary finished_slots_;     // the frame whose slots all closed, until its early step
    uint8_t bank_decided_;           // slots decided in bank_frame_
    uint8_t byte_bits_;              // bits of the byte being assembled
    uint8_t byte_flags_;
    uint16_t slot_crest_[k_max_data_slots];  // winner crest of each slot, input units (audit purity)
    uint8_t held_[k_max_held_frames];
    uint8_t held_count_;

    uint32_t origin_block_;
    float slot_blocks_;              // PREAMBLE: T of the train; TRACK: exact T (1 + epsilon)
    float grid_position_;
    // PREAMBLE: weighted least-squares sums of the train's markers, position (history blocks) over grid index.
    float fit_weight_;
    float fit_x_;
    float fit_y_;
    float fit_xx_;
    float fit_xy_;
    int32_t grid_last_flip_;         // L: the newest train flip
    int32_t grid_index_;
    uint8_t train_ones_;
    uint8_t train_gap_;
    uint8_t train_gap_count_;
    float reference_average_;
    float guard_edge_excess_;  // boundary_excess() at the edges of the guard's detected markers, summed
    float guard_edge_noise_;
    uint8_t guard_edges_;
    uint8_t presence_bits_;  // frames with the STOP or the data present, last k_presence_window
    uint8_t strong_bits_;    // frames holding a full-strength flip at an audit position
    uint8_t tune_run_;       // audit positions in a row on a steady carrier: a new transmission's tune tone
    uint8_t guard_markers_;  // late-join guard: detected START and STOPs
    uint8_t guard_present_;  // late-join guard: frames present
    uint8_t guard_frames_;
    uint8_t lock_flags_;     // late_join, mode_memory, blind_mode of this lock
    bool confirmed_;         // locked was emitted and frames flow
    float stop_evidence_;    // clipped flip evidence of this lock's STOPs, summed (stops_found)
    bool weak_header_;       // its header matched 6 of 8 slots, or it has none (weak_header_confirms)
    bool tone_confirmed_;
    bool afc_looked_;
    bool tone_steady_;
    bool noise_frozen_;      // ACQUIRE keeps the noise it started from (push_block_noise)
    bool watch_;  // a lock on a tone from the search: the search keeps listening for a train on another tone (watching)
    bool searching_;  // the search took the last sample
    uint32_t state_blocks_;
    uint32_t tone_blocks_;   // since lock_tone() or the end of a confirmed lock: ACQUIRE's time on this tone
    float watch_left_hz_;    // the tone the watch last moved away from, 0 = none (since SEARCH)
    float ban_slot_blocks_;
    uint32_t ban_until_block_;
};

}  // namespace unlimited
