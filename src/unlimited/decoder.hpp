#pragma once

#include "unlimited/dsp.hpp"

namespace unlimited {

// Receiver presets (spec 1.7): a profile only fills DecoderConfig's fields.
enum class Profile : uint8_t {
    ssb,  // HF SSB, USB or LSB: T 8..64 ms, passband 300..2700 Hz (the default)
    am,   // AM receivers: T 8..64 ms, passband 100..3000 Hz
    fm    // VHF/UHF FM: T 4..32 ms, passband 300..3000 Hz, tones from 1000 Hz
};

enum class DecisionMode : uint8_t {
    adaptive,     // the smart line: 50..75 % of the START-STOP reference line (about 70 % when weak), noise floor
    fixed_ratio   // DecoderConfig::fixed_ratio of the reference line (0.70: the original rule)
};

enum class DecoderState : uint8_t { search, acquire, preamble, track };

enum class EventType : uint8_t { state, locked, slot, package, byte, end, lost };

enum class LostReason : uint8_t { none, signal_gone, alias, preamble_timeout, reset, unsupported };

enum EventFlag : uint8_t {
    event_flag_late_join = 0x01,        // joined a running transmission: relock after a fade or cold join (spec 3.12)
    event_flag_flywheel_start = 0x02,   // the package's START was not detected: measured where predicted
    event_flag_flywheel_stop = 0x04,    // the same for its STOP
    event_flag_blanked = 0x08,          // the impulse blanker cut part of the package
    event_flag_weak = 0x10              // a bit within 12.5 % of its decision line
};

struct Event {
    EventType type;
    LostReason reason;             // lost
    DecoderState state;            // new state for EventType::state, current state otherwise
    uint8_t flags;                 // EventFlag bits
    uint8_t value;                 // byte: the byte; slot: the bit (0 or 1); package: d, its bit count
    uint8_t slot;                  // slot: 1..d, the data slot in its package
    uint8_t bits_per_package;      // N, from locked to end or lost; 0 before
    uint8_t level_pct;             // slot: amplitude, % of the reference line at this slot (<= 255)
    uint8_t threshold_pct;         // slot: the decision line, same units
    uint8_t start_pct;             // slot, package: START crest, % of the running marker reference (<= 255)
    uint8_t stop_pct;              // slot, package: STOP crest, same units
    int8_t soft[k_bits_per_byte];  // byte: bits MSB first; slot: soft[0]; > 0 means 1, 64 = one line of margin
    uint32_t package_index;        // slot, package, byte, locked: package number, 0 = the first after the train
    uint32_t byte_index;           // byte: position in the transmission, 0 = the first byte
    float tone_hz;
    float slot_ms;                 // measured T (package: its own (STOP - START) / (d + 1))
    float snr_db;
};

typedef void (*EventHandler)(const Event& event, void* context);

// The receiver chooses its T range and its audio passband; it learns the tone, T and N from the signal.
struct DecoderConfig {
    uint8_t min_slot_ms;           // k_min_window_slot_ms..k_max_window_slot_ms: accepted T = min_slot_ms ..
                                   // 8 * min_slot_ms; block = min_slot_ms samples
    Passband passband;             // the radio's audio passband; the tone search stays inside it (spec 3.6)
    DecisionMode decision_mode;
    float fixed_ratio;             // DecisionMode::fixed_ratio: fraction of the reference line
    bool impulse_blanker;

    DecoderConfig();  // for_profile(Profile::ssb)
    static DecoderConfig for_profile(Profile profile);  // fills the fields; the profile itself is not kept
    ConfigError check() const;
    bool valid() const;  // check() == ConfigError::none
    uint16_t max_slot_ms() const;
    // Tones the search looks at: the passband less half the occupied band at max_slot_ms(), within
    // [k_min_tone_hz, k_max_tone_hz], from k_min_fast_tone_hz when min_slot_ms < 8.
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
    float tone_hz() const;
    float slot_ms() const;
    float snr_db() const;
    uint8_t bits_per_package() const;   // N once learnt (TRACK), else 0
    const DecoderConfig& config() const;

private:
    struct Marker {
        float position;   // history blocks from origin_block_
        float amplitude;  // crest A_mk
        float balanced;   // q_bal of its flip
        bool detected;
    };

    // A decided package (spec 3.10): bits MSB first as sent, their soft values and flags.
    struct Package {
        uint32_t index;   // package number from the first START
        uint8_t bits[(k_max_bits_per_package + k_bits_per_byte - 1) / k_bits_per_byte];
        int8_t soft[k_max_bits_per_package];
        uint8_t count;    // d
        uint8_t flags;
        bool faded;       // decided on noise: an erasure, never released as bytes
        bool stop_gone;   // its STOP read at the noise floor
    };

    // What deciding a package gives besides its bits: the guard's tallies and the quiet-gap noise (spec 3.10, 3.11).
    struct PackageStats {
        float gap_noise;       // quiet-gap noise inputs, summed
        float threshold_sum;   // decision lines, % of the reference line, summed over the bits
        float ones_level_sum;  // levels of the decided ones, % of the reference line
        float strongest;       // largest audit evidence
        float edge_excess;     // boundary excess at the edges of its detected markers, summed
        float edge_noise;
        float edge_energy;     // at every slot edge, less the noise, summed (spec 1.1: nulls)
        float one_energy;      // at the centres of the decided ones, same windows
        uint8_t edges;
        uint8_t gaps;
        uint8_t ones;
        uint8_t zeros;
        uint8_t loud_zeros;
        uint8_t twisted;       // data slot centres that flip at full strength (a marker, no data)
    };

    // PREAMBLE: a reading of a candidate package, measured and decided when its STOP was found (spec 3.8), with what
    // the guard and the telemetry need once it is confirmed (the history may no longer hold it then).
    struct Reading {
        Package package;
        PackageStats stats;
        uint8_t level_pct[k_max_bits_per_package];
        uint8_t threshold_pct[k_max_bits_per_package];
        int8_t audit[dsp::AuditRing::k_max_positions];  // 1/k_audit_scale units
        Marker start;
        Marker stop;
        float reference;  // running marker crest when it was decided
        bool first;       // package 0: its STOP is the first marker after the train
        bool flipped;     // a marker the walk took for noise lies among its data slots
        bool valid;
    };

    // Station memory (spec 3.12): what a confirmed lock knew when it was lost or ended.
    struct StationMemory {
        float tone_hz;
        float slot_blocks;
        float crest;
        uint32_t marker_block;      // absolute block of its last detected marker
        float marker_fraction;
        uint32_t marker_package;    // package index whose START that marker is
        uint32_t expires_block;
        uint8_t bits_per_package;
        bool ended;                 // END was seen: the next transmission brings its own preamble
        bool valid;
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

    // Expected squares on noise alone of a data slot's and a marker's amplitude, and the decision floor.
    struct SlotNoise {
        float slot;    // N_a
        float marker;  // N_m
        float floor;   // k_floor_sigma sqrt(N_a)
    };

    static const uint8_t k_held_packages = 22;    // the full guard of N = 1 (36 slots), 4 packages recovered before
    static const uint8_t k_readings = 3;          // a candidate's two readings, or the rejected one's and the new one
    static const uint8_t k_cold_hypotheses = 4;   // N = 8, 16, 24, 32
    static const uint8_t k_fold_grids = 4;        // a hypothesis's grid and its 3rd, 5th and 7th sub-grids

    // Cold late join (spec 3.12): a chain of equal marker intervals with no preamble, folded on the slot grids of
    // N = 8, 16, 24, 32.
    struct ColdJoin {
        float anchor;                                         // START of the first package to fold
        float period;                                         // P, blocks
        float edge[k_cold_hypotheses][k_fold_grids];          // noise-free edge energy, summed
        float centre[k_cold_hypotheses][k_fold_grids];        // the same at the slot centres
        uint8_t folded;
        bool pending;
    };

    void initialize();
    void on_block(int32_t re, int32_t im, uint32_t energy);
    void rebase();

    void set_state(DecoderState state);
    void enter_search(bool keep_statistics = false);
    void lock_tone(float tone_hz);
    void enter_acquire();
    void enter_preamble(float anchor, float slot_blocks, float amplitude, uint8_t marker_bits, int32_t min_start);
    void enter_track(const Marker& start, uint32_t package_index, uint8_t bits, bool late_join);
    void lose(LostReason reason, bool keep_tone = true);
    void finish(uint32_t last_package);

    // SEARCH and ACQUIRE (spec 3.6, 3.7).
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
    bool boundary_excess(float boundary, float slot_blocks, float crest, float& excess, float& noise_ratio) const;
    float boundary_energy(const dsp::FlipMeasure* measures, const bool* marker, float slot_blocks, float crest,
                          float& allowance) const;
    void refine_sync(float& centre, float& slot_blocks) const;
    bool hidden_midpoints(float centre, float slot_blocks, uint8_t hit_bits, uint8_t& hidden_bits) const;
    bool stream_tune(float end, float widest_half) const;
    bool try_late_join(const dsp::Candidate& candidate);
    bool chain_start(float newest, float period, float slot_blocks, uint8_t intervals, float& start,
                     uint8_t& back) const;
    bool train_behind(const dsp::Candidate& candidate, float slot_blocks) const;
    bool has_candidate_near(float position, float tolerance, float q_min) const;
    float candidate_q_near(float position, float tolerance) const;
    bool try_cold_join(const dsp::Candidate& candidate);
    void cold_join_step();
    float chain_marker(uint8_t package) const;
    bool fold_ratio(uint8_t bits, float& ratio) const;
    bool fold_unrivalled(uint8_t bits) const;
    void fold_package(float start, float stop);
    uint8_t cold_bits(uint8_t hypothesis) const;
    bool fold_passes(uint8_t hypothesis, uint8_t grid) const;

    // PREAMBLE: the rest of the train, then the package length (spec 3.8).
    void run_preamble();
    bool preamble_step();
    void on_marker(int32_t g, const dsp::FlipMeasure& m);
    void continue_train(int32_t g, const dsp::FlipMeasure& m);
    void add_train_point(float index, const dsp::FlipMeasure& m);
    void seed_train_fit(float anchor, float slot_blocks);
    bool train_fit(float index, float& slot_blocks, float& position) const;
    int32_t train_end_bound(float anchor, float slot_blocks, float crest, bool& silent) const;
    uint8_t sync_train_ones(float anchor, float slot_blocks, uint8_t marker_bits) const;
    bool start_certain(const Reading& first, uint8_t bits) const;
    uint8_t sub_rate_train(int32_t gap);
    bool sync_in_phase(int32_t g, uint8_t multiple) const;
    void restart_grid(float slot_blocks, float anchor);
    void measure_reading(Reading& reading, const Marker& start, const Marker& stop, uint8_t bits, bool first);
    bool end_before_confirmation();
    bool whole_bytes(const Reading& first, const Reading* second) const;
    bool readings_clean(const Reading& first, const Reading* second, float end_stop);
    void emit_reading(const Reading& reading, uint32_t index);
    bool confirmation_step();
    void recover_packages(const Reading& first, uint32_t count, uint8_t bits);
    uint8_t sub_chain(float first, float middle, float last, uint8_t bits) const;
    bool sub_package_zeros(const Package& first, const Package& second, uint8_t bits) const;
    bool train_follows_tune(int32_t min_start, bool silent, float slot_blocks) const;
    bool train_closed() const;
    float grid_position(int32_t g) const;
    float preamble_span(int32_t gap) const;

    // TRACK (spec 3.9 - 3.11).
    void run_track();
    bool track_step();
    void track_afc(const Marker& start, const Marker& stop);
    bool rotated_marker(const dsp::FlipMeasure& m) const;
    void rotation_afc(const dsp::FlipMeasure& m, bool detected);
    uint8_t scan_short_end();
    void end_short_package(uint8_t stop_slot);
    float end_evidence(float position) const;
    bool end_of_transmission(float stop) const;
    bool end_clean(float stop);
    uint8_t tune_edges(const Marker& start, const Marker& stop, uint8_t bits, uint8_t& truncate);
    void decide_package(const Marker& start, const Marker& stop, uint8_t bits, Package& package, PackageStats& stats,
                        uint8_t* level_pct, uint8_t* threshold_pct, int8_t* audit);
    bool marker_gone(const Marker& marker) const;
    bool faded_package(const Package& package, const Marker& start, const Marker& stop) const;
    void emit_package(const Package& package, const Marker& start, const Marker& stop, float reference,
                      const uint8_t* level_pct, const uint8_t* threshold_pct);
    void push_audit(const int8_t* audit, uint8_t bits);
    void guard_package(const Package& package, const PackageStats& stats, const Marker& stop);
    void release(const Package& package, bool detected);
    void check_guard();
    bool guard_refused() const;
    bool guard_clean() const;
    bool decodable() const;
    bool zeros_quiet() const;
    bool beep_shaped(float edge_energy, uint16_t edges, float one_energy, uint16_t ones) const;
    bool slot_edges_quiet() const;
    void add_marker_edges(float marker, float slot_blocks, float crest, PackageStats& stats) const;
    void measure_shape(const Marker& start, float slot_blocks, uint8_t bits, const Package& package,
                       PackageStats& stats) const;
    void confirm();
    void release_held();
    void hold(const Package& package);
    void assemble(const Package& package);  // bits into bytes by package index; byte events

    // Station memory (spec 3.12).
    void remember_station(bool ended);
    bool memory_valid() const;
    bool memory_usable() const;
    bool stream_relock() const;
    float memory_position() const;  // the remembered marker, history blocks from origin_block_

    // Measurement and decision (spec 3.4, 3.10).
    dsp::FlipMeasure measure_flip(float centre, float half) const;
    dsp::FlipMeasure search_flip(float centre, float span, float half, float kappa_min) const;
    bool peak_beyond(const dsp::FlipMeasure& m, float predicted, float span, float half) const;
    bool bridges_trusted() const;
    dsp::FlipMeasure bridge(float older, float newer, float slot_blocks) const;
    Marker measure_marker(float position, float slot_blocks) const;
    float slot_amplitude(float centre, float slot_blocks) const;
    float window_energy(float centre, float half) const;
    SlotNoise slot_noise(float slot_blocks) const;
    float decision_threshold(const SlotNoise& noise, float reference) const;
    float noise_variance() const;
    float marker_noise(float sigma2, float slot_blocks) const;
    float end_position() const;
    float candidate_position(const dsp::Candidate& candidate) const;
    float blocks_to_ms(float blocks) const;
    float ms_to_blocks(float ms) const;
    bool in_range(float slot_blocks) const;
    bool banned(float slot_blocks) const;
    void ban_alias(float slot_blocks);
    void update_reference(float amplitude);
    void reset_scales();

    Event make_event(EventType type) const;
    void emit(const Event& event);
    void emit_locked(uint32_t package_index);

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
    dsp::FineAfc afc_;
    dsp::Complex afc_sum_;
    uint8_t afc_fill_;
    dsp::QuantileTracker noise_;     // SEARCH..PREAMBLE: robust to the signal in its windows
    dsp::NoiseTracker track_noise_;  // TRACK: the quiet gaps of decided zeros
    dsp::CandidateList candidates_;
    dsp::AuditRing audit_;
    dsp::PackageLearner learner_;
    float scale_q_[k_candidate_scales][2];
    float scale_kappa_[k_candidate_scales];  // kappa of the newer measure of scale_q_

    // Grid (PREAMBLE, history clock): slot index g sits at grid_position_ + (g - grid_last_) T.
    uint32_t origin_block_;
    float slot_blocks_;              // T in history blocks
    float grid_position_;
    int32_t grid_last_;              // grid index of the newest marker
    int32_t grid_index_;             // grid index evaluated last
    int32_t end_stop_;               // PREAMBLE: the marker whose END is being checked, -1 = none
    float fit_weight_;               // weighted least-squares sums of the train's markers
    float fit_x_;
    float fit_y_;
    float fit_xx_;
    float fit_xy_;
    Marker last_marker_;             // PREAMBLE: the newest marker (START of the next reading)
    Marker train_last_;              // PREAMBLE: the train's newest marker L
    dsp::FlipMeasure end_flip_;      // PREAMBLE: the marker after end_stop_, kept until the END check
    uint8_t train_gap_;
    uint8_t train_gap_count_;
    uint8_t train_markers_;
    uint8_t sync_hits_;              // the sync's markers, bit i at grid index -i
    int32_t min_start_;              // the earliest START of package 0 by the train's length, dsp::k_no_start unknown
    bool tuned_train_;               // the train follows a tune: a transmission's start, its package 0 can be placed
    float first_after_;              // position of the first marker after the train's newest one (g1)
    uint8_t candidate_reading_;      // readings_ of the current candidate (A; B at + 1)
    uint8_t previous_reading_;       // readings_ of the candidate rejected at the newest marker, k_readings = none
    bool confirm_pending_;           // N confirmed: the END rule of the confirming marker is waited for
    bool data_flip_;                 // a marker taken for noise after the newest marker (in the next reading's data)
    Reading readings_[k_readings];

    // TRACK.
    Marker start_;
    Marker last_detected_;           // newest detected STOP (station memory)
    uint32_t last_detected_package_;
    float reference_average_;        // running marker crest
    uint32_t package_index_;         // of the package starting at start_
    float short_evidence_[3];        // the short-END triple's newest three slot centres
    uint8_t short_cursor_;           // the next slot centre of the package to scan for it
    uint8_t short_run_;              // centres scanned, up to three
    uint8_t short_found_;            // its STOP's slot centre, 0 = none
    bool start_rotated_;             // the current package's START was a STOP turned by a frequency step
    uint8_t bits_per_package_;       // N, 0 until learnt
    Package held_[k_held_packages];
    uint8_t held_count_;
    uint8_t presence_window_;        // STOPs in the LOST window: max(4, ceil(36 / (N + 1)))
    uint32_t presence_bits_;         // STOPs present (q >= 1), newest in bit 0
    uint32_t detected_bits_;
    uint8_t strong_bits_;            // packages holding a full-strength flip at an audit position, last 4
    uint8_t anti_bits_;              // STOPs that read as a steady carrier, last 8
    uint8_t tune_run_;               // slot edges in a row on a steady carrier (the next tune)
    float rotation_hz_;
    uint16_t guard_slots_;           // slots of the packages held by the guard
    uint8_t guard_packages_;
    uint8_t guard_markers_;          // markers of the guard (its first START and every STOP)
    uint8_t guard_detected_;
    uint8_t guard_balanced_;         // detected STOPs with balanced halves
    uint8_t guard_inner_;            // guard packages holding a flip at an audit position
    uint8_t guard_edges_;
    uint8_t guard_zeros_;
    uint8_t guard_loud_zeros_;
    uint16_t guard_ones_;
    uint16_t guard_bits_;
    float guard_edge_excess_;
    float guard_edge_noise_;
    float guard_edge_energy_;        // slot edges and ones of the guard's packages (PackageStats)
    float guard_one_energy_;
    float guard_stop_q_;             // weakest balanced STOP of the guard
    float guard_threshold_sum_;      // decision lines of the guard, % of the reference, summed
    float guard_ones_level_sum_;
    bool guard_started_;             // the first guard package's START was detected
    bool late_join_;
    bool confirmed_;                 // locked was emitted and packages flow
    bool tone_confirmed_;
    bool tone_steady_;
    bool afc_looked_;
    bool noise_frozen_;
    bool watch_;
    bool searching_;

    // Bytes: bits are placed by package index, so a lost package leaves a gap, never a shift (spec 3.13).
    uint32_t byte_index_;
    uint32_t next_bit_;              // stream position of the bit that continues the byte being assembled
    uint8_t byte_value_;
    uint8_t byte_bits_;
    uint8_t byte_skip_;              // bits to drop before the next byte starts (after a gap)
    uint8_t byte_flags_;
    bool assembling_;
    int8_t byte_soft_[k_bits_per_byte];

    StationMemory memory_;
    ColdJoin cold_;
    uint32_t state_blocks_;
    uint32_t tone_blocks_;
    float watch_left_hz_;
    float ban_slot_blocks_;
    uint32_t ban_until_block_;
};

}  // namespace unlimited
