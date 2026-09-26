#include "unlimited/encoder.hpp"

namespace unlimited {

const uint16_t Encoder::k_queue_size;

#if defined(__AVR__)
// B5: 160 bytes with the default 64-byte queue; the queue itself may be set anywhere in 16..128.
static_assert(sizeof(Encoder) - Encoder::k_queue_size <= 96, "B5: the AVR encoder must fit in 96 bytes plus its queue");
#endif

namespace {

const uint32_t k_default_rate_hz = 8000;
const uint16_t k_tone_nyquist_margin_hz = 500;
const uint8_t k_min_slot_samples = 32;
const uint8_t k_min_tune_slots = 6;
const int16_t k_default_amplitude = 23197;  // -3 dBFS
const uint32_t k_us_per_s = 1000000;
const uint32_t k_us_per_ms = 1000;
const uint32_t k_ms_per_s = 1000;
const uint8_t k_queue_mask = static_cast<uint8_t>(Encoder::k_queue_size - 1);

// Guaranteed by the ranges alone, so valid() needs no check: every tone stays 500 Hz below Nyquist and a slot
// is never shorter than 32 samples.
static_assert(k_max_tone_hz + k_tone_nyquist_margin_hz < k_min_sample_rate_hz / 2, "tones below rate / 2 - 500 Hz");
static_assert(k_min_slot_us / k_us_per_ms * (k_min_sample_rate_hz / k_ms_per_s) >= k_min_slot_samples, "T >= 32 samples");

const uint16_t k_long_tune_ms = 500;
const uint16_t k_weak_tune_ms = 1500;
const uint8_t k_long_sync_markers = 16;

// Tone offsets from f_ref in sevenths of 1/T: guard 35, standard spacing 8, dense spacing 7.
const uint16_t k_sevenths = k_standard_spacing_den;
const uint16_t k_header_top_tone = k_header_slots - 1;

// Tune slot position, kept in slot_ while in the tune segment.
const uint8_t k_tune_first = 0;
const uint8_t k_tune_middle = 1;
const uint8_t k_tune_last = 2;

// Slot position u = slot_phase / 2^32 (the high word of the 64-bit slot phase).
const uint8_t k_phase_high_shift = 32;
const uint8_t k_phase_fraction_bits = 64;
const uint32_t k_ramp_end = 0x40000000u;       // u = 0.25
const uint32_t k_reversal_begin = 0x60000000u; // u = 0.375
const uint32_t k_reversal_end = 0xA0000000u;   // u = 0.625
const uint32_t k_ramp_begin = 0xC0000000u;     // u = 0.75
const uint32_t k_peak_ramp_end = 0x20000000u;  // u = 1/8
const uint32_t k_peak_ramp_begin = 0xE0000000u; // u = 7/8
const uint32_t k_half_turn = 0x80000000u;

const int16_t k_q15_one = 32767;
const uint8_t k_q15_shift = 15;
const int32_t k_q15_half = static_cast<int32_t>(1) << (k_q15_shift - 1);
const uint8_t k_word_shift = 16;

// Header tones, 3 bits each (a GF(8) element), slot 0 in the lowest bits.
const uint8_t k_header_tone_bits = 3;
const uint8_t k_header_tone_mask = (1u << k_header_tone_bits) - 1u;

// ceil(numerator * 2^64 / denominator) for numerator < denominator < 2^48, by 16-bit long division
// (no 128-bit type on the MCUs).
const uint8_t k_division_digit_bits = 16;
const uint8_t k_division_digits = k_phase_fraction_bits / k_division_digit_bits;

uint64_t ceil_fraction_2_64(uint64_t numerator, uint64_t denominator) {
    uint64_t quotient = 0;
    uint64_t remainder = numerator;
    for (uint8_t digit = 0; digit < k_division_digits; ++digit) {
        remainder <<= k_division_digit_bits;
        quotient = (quotient << k_division_digit_bits) + remainder / denominator;
        remainder %= denominator;
    }
    return remainder != 0 ? quotient + 1 : quotient;
}

// round(numerator * 2^32 / denominator), numerator < 2^31.
uint32_t round_fraction_2_32(uint64_t numerator, uint64_t denominator) {
    return static_cast<uint32_t>(((numerator << k_phase_high_shift) + denominator / 2) / denominator);
}

// At most ceil(65535 ms / 6 ms) = 10923: fits 16 bits.
uint16_t tune_slots(const EncoderConfig& config) {
    const uint32_t tune_us = static_cast<uint32_t>(config.tune_ms) * k_us_per_ms;
    const uint32_t slots = (tune_us + config.slot_us - 1) / config.slot_us;
    return static_cast<uint16_t>(slots < k_min_tune_slots ? k_min_tune_slots : slots);
}

// floor(ms * rate / 1000) without 64-bit arithmetic (rate <= 192000, ms <= 65535).
uint32_t ms_to_samples_floor(uint16_t ms, uint32_t rate_hz) {
    return ms * (rate_hz / k_ms_per_s) + ms * (rate_hz % k_ms_per_s) / k_ms_per_s;
}

bool valid_data_slots(uint8_t data_slots) {
    for (uint8_t n = k_min_data_slots; n <= k_max_data_slots; n = static_cast<uint8_t>(n << 1))
        if (data_slots == n) return true;
    return false;
}

// Distance from f_ref to the farthest data or header tone, in sevenths of 1/T.
uint32_t span_sevenths(const EncoderConfig& config) {
    const uint32_t spacing = config.spacing == Spacing::standard ? k_standard_spacing_num : k_sevenths;
    const uint32_t top_tone = (1u << config.bits_per_peak) - 1u;
    const uint32_t data = top_tone * spacing;
    const uint32_t header = k_header_top_tone * k_standard_spacing_num;
    return k_grid_guard * k_sevenths + (data > header ? data : header);
}

// Data slots carrying q bytes: ceil(8 q / k).
uint32_t peak_slots(uint32_t bytes, uint8_t bits_per_peak) {
    return (bytes * k_bits_per_byte + bits_per_peak - 1u) / bits_per_peak;
}

// The same in the ISR, for a short final frame (q below the frame's N k / 8 bytes): fewer than N subtractions, where a
// division is a library call of several hundred cycles on AVR.
uint8_t short_frame_peaks(uint8_t bytes, uint8_t bits_per_peak) {
    uint16_t bits = static_cast<uint16_t>(bytes * k_bits_per_byte);
    uint8_t peaks = 0;
    for (; bits > 0; ++peaks) bits = bits > bits_per_peak ? static_cast<uint16_t>(bits - bits_per_peak) : 0;
    return peaks;
}

// (a b + 2^14) >> 15 for Q15 values: the high word of twice the sum (byte moves on AVR, where >> 15 is a loop).
int16_t multiply_q15(int16_t a, int16_t b) {
    const uint32_t sum = static_cast<uint32_t>(static_cast<int32_t>(a) * b + k_q15_half);
    return static_cast<int16_t>(static_cast<uint16_t>((sum << 1) >> k_word_shift));
}

// sin^2(2 pi u) = (1 - cos(4 pi u)) / 2: one table lookup and no multiply.
int16_t ramp_q15(uint32_t slot_phase) {
    const uint16_t cosine = static_cast<uint16_t>(cosine_q15(slot_phase << 1));
    const uint16_t twice = static_cast<uint16_t>(static_cast<uint16_t>(k_q15_one) - cosine);  // 0..65534
    return static_cast<int16_t>((twice + 1u) >> 1);
}

int16_t tukey_q15(uint32_t slot_phase) {
    return (slot_phase < k_ramp_end || slot_phase > k_ramp_begin) ? ramp_q15(slot_phase) : k_q15_one;
}

// w(u) * r(u): w is flat over the reversal, so the product is w, then r, then -w.
int16_t marker_q15(uint32_t slot_phase) {
    if (slot_phase < k_reversal_begin) return tukey_q15(slot_phase);
    if (slot_phase < k_reversal_end) return cosine_q15((slot_phase - k_reversal_begin) << 1);
    return static_cast<int16_t>(-tukey_q15(slot_phase));
}

// p(u), Tukey alpha 0.25: sin^2(4 pi u) = sin^2(4 pi (1 - u)), so one expression serves both ramps.
int16_t peak_q15(uint32_t slot_phase) {
    return (slot_phase < k_peak_ramp_end || slot_phase > k_peak_ramp_begin) ? ramp_q15(slot_phase << 1)
                                                                             : k_q15_one;
}

void set_mode(EncoderConfig& config, uint16_t slot_ms, uint8_t bits_per_peak, uint16_t tone_hz, uint16_t tune_ms,
              uint8_t sync_markers, uint16_t lead_in_ms) {
    config.slot_us = static_cast<uint32_t>(slot_ms) * k_us_per_ms;
    config.bits_per_peak = bits_per_peak;
    config.tone_hz = tone_hz;
    config.tune_ms = tune_ms;
    config.sync_markers = sync_markers;
    config.lead_in_ms = lead_in_ms;
}

// Modes of spec 1.4 (N = 8, standard spacing and the grid below f_ref come from EncoderConfig()). HF presets
// put f_ref at ceil(1500 + W / 2): the band is centred on 1500 Hz. A switch, not a table: no RAM on AVR.
void set_preset(EncoderConfig& config, Preset preset) {
    switch (preset) {
        case Preset::fm_fast:
            set_mode(config, 6, 3, k_fm_tone_hz, k_default_tune_ms, k_default_sync_markers, k_default_fm_lead_in_ms);
            break;
        case Preset::fm:
            set_mode(config, 8, 3, k_fm_tone_hz, k_default_tune_ms, k_default_sync_markers, k_default_fm_lead_in_ms);
            break;
        case Preset::hf_fast:
            set_mode(config, 16, 4, 2192, k_default_tune_ms, k_default_sync_markers, 0);
            break;
        case Preset::hf:
            set_mode(config, 32, 5, 2132, k_default_tune_ms, k_default_sync_markers, 0);
            break;
        case Preset::hf_robust:
            set_mode(config, 64, 6, 2102, k_long_tune_ms, k_long_sync_markers, 0);
            break;
        case Preset::hf_weak:
            set_mode(config, 128, 7, 2087, k_weak_tune_ms, k_long_sync_markers, 0);
            break;
    }
}

}  // namespace

EncoderConfig::EncoderConfig()
    : sample_rate_hz(k_default_rate_hz),
      tone_hz(0),
      slot_us(0),
      bits_per_peak(0),
      data_slots(k_default_data_slots),
      spacing(Spacing::standard),
      side(GridSide::below),
      amplitude(k_default_amplitude),
      lead_in_ms(0),
      tune_ms(0),
      sync_markers(0),
      tail_ms(k_default_tail_ms) {
    set_preset(*this, Preset::hf);
}

EncoderConfig EncoderConfig::from_preset(Preset preset, uint32_t sample_rate_hz) {
    EncoderConfig config;
    config.sample_rate_hz = sample_rate_hz;
    set_preset(config, preset);
    return config;
}

ConfigError EncoderConfig::check() const {
    if (sample_rate_hz < k_min_sample_rate_hz || sample_rate_hz > k_max_sample_rate_hz) return ConfigError::sample_rate;
    if (tone_hz < k_min_tone_hz || tone_hz > k_max_tone_hz) return ConfigError::tone;
    if (slot_us < k_min_slot_us || slot_us > k_max_slot_us || slot_us % k_slot_quantum_us != 0) return ConfigError::slot;
    if (bits_per_peak < 1 || bits_per_peak > k_max_bits_per_peak) return ConfigError::bits_per_peak;
    if (!valid_data_slots(data_slots)) return ConfigError::data_slots;
    if ((static_cast<uint32_t>(data_slots) + 1) * slot_us > k_max_frame_us) return ConfigError::frame_length;
    if (spacing != Spacing::standard && spacing != Spacing::dense) return ConfigError::spacing;
    if (spacing == Spacing::dense && slot_us < k_min_dense_slot_us) return ConfigError::dense_slot;
    if (side != GridSide::above && side != GridSide::below) return ConfigError::side;
    // Farthest tone f_ref -+ 1000 span / (7 T_ms) inside [k_min_tone_hz, k_max_tone_hz], exactly.
    const uint32_t room_hz = side == GridSide::below ? tone_hz - k_min_tone_hz : k_max_tone_hz - tone_hz;
    const uint32_t slot_ms = slot_us / k_us_per_ms;
    if (room_hz * k_sevenths * slot_ms < span_sevenths(*this) * k_ms_per_s) return ConfigError::band;
    if (frame_bytes() > Encoder::k_queue_size / 2) return ConfigError::queue;
    if (sync_markers < k_min_sync_markers || sync_markers > k_max_sync_markers) return ConfigError::sync_markers;
    return amplitude > 0 ? ConfigError::none : ConfigError::amplitude;
}

bool EncoderConfig::valid() const {
    return check() == ConfigError::none;
}

uint8_t EncoderConfig::frame_bytes() const {
    return static_cast<uint8_t>(static_cast<uint16_t>(data_slots) * bits_per_peak / k_bits_per_byte);
}

Encoder::Encoder(const EncoderConfig& config)
    : config_(config),
      slot_phase_high_(0),
      slot_phase_low_(0),
      slot_step_high_(0),
      slot_step_low_(0),
      tone_phase_(0),
      tone_step_(0),
      data_phase_(0),
      data_step_(0),
      unit_step_(0),
      std_step_(0),
      countdown_(0),
      tail_samples_(0),
      slot_index_(0),
      samples_rendered_(0),
      header_tones_(0),
      tune_slots_(0),
      segment_(EncoderSegment::idle),
      slot_(0),
      symbol_(0),
      tone_(0),
      frame_n_(0),
      frame_d_(0),
      queue_head_(0),
      queue_tail_(0),
      queue_() {}

// Producer side: everything the consumer reads is set up first, then published with segment_.
bool Encoder::start() {
    if (busy() || queued() == 0 || !config_.valid()) return false;
    const uint32_t rate = config_.sample_rate_hz;
    const uint64_t slot_rate = static_cast<uint64_t>(rate) * config_.slot_us;
    tone_phase_ = 0;
    tone_step_ = round_fraction_2_32(config_.tone_hz, rate);
    data_phase_ = 0;
    data_step_ = 0;
    unit_step_ = round_fraction_2_32(k_us_per_s, slot_rate);
    std_step_ = round_fraction_2_32(static_cast<uint64_t>(k_standard_spacing_num) * k_us_per_s,
                                    k_standard_spacing_den * slot_rate);
    const uint16_t word = header_word(config_.bits_per_peak, config_.data_slots, config_.slot_us, config_.spacing);
    header_tones_ = 0;
    for (uint8_t j = k_header_slots; j-- > 0;) {
        header_tones_ = (header_tones_ << k_header_tone_bits) | header_symbol(word, j);
    }
    tune_slots_ = tune_slots(config_);
    tail_samples_ = ms_to_samples_floor(config_.tail_ms, rate);
    // One slot is L = rate * slot_us / 10^6 samples; the step is rounded up so slot k starts exactly at
    // sample ceil(k * L) (the rounding error stays below one exact-rational gap for 7.5e8 samples).
    const uint64_t slot_step = ceil_fraction_2_64(k_us_per_s, slot_rate);
    slot_step_high_ = static_cast<uint32_t>(slot_step >> k_phase_high_shift);
    slot_step_low_ = static_cast<uint32_t>(slot_step);
    // The lead-in is on the slot grid: a partial first slot, then whole slots.
    const uint32_t lead_us = static_cast<uint32_t>(config_.lead_in_ms) * k_us_per_ms;
    const uint32_t lead_rest_us = lead_us % config_.slot_us;
    const uint64_t slot_phase =
        lead_rest_us != 0 ? ceil_fraction_2_64(config_.slot_us - lead_rest_us, config_.slot_us) : 0;
    slot_phase_high_ = static_cast<uint32_t>(slot_phase >> k_phase_high_shift);
    slot_phase_low_ = static_cast<uint32_t>(slot_phase);
    countdown_ = lead_us / config_.slot_us + (lead_rest_us != 0 ? 1 : 0);
    slot_index_ = 0;
    samples_rendered_ = 0;
    slot_ = 0;
    symbol_ = 0;
    tone_ = 0;
    frame_n_ = 0;
    frame_d_ = 0;
    EncoderSegment first = EncoderSegment::lead_in;
    if (countdown_ == 0) {
        countdown_ = tune_slots_;
        slot_ = k_tune_first;
        first = EncoderSegment::tune;
    }
    release_fence();
    segment_ = first;
    return true;
}

// Producer side: the byte is stored before the new head is published; the freed slot was read before the tail moved.
bool Encoder::write(uint8_t byte) {
    const uint8_t head = queue_head_;
    const uint8_t tail = queue_tail_;
    acquire_fence();
    if (static_cast<uint8_t>(head - tail) >= k_queue_size) return false;
    queue_[head & k_queue_mask] = byte;
    release_fence();
    queue_head_ = static_cast<uint8_t>(head + 1u);
    return true;
}

size_t Encoder::write(const uint8_t* data, size_t size) {
    size_t written = 0;
    while (written < size && write(data[written])) ++written;
    return written;
}

// Consumer side (see encoder.hpp). The queue is emptied before idle is published: the producer may start() again as
// soon as it sees idle.
void Encoder::abort() {
    queue_tail_ = queue_head_;
    go_idle();
}

int16_t Encoder::next_sample() {
    const EncoderSegment segment = segment_;
    if (segment == EncoderSegment::idle) return 0;
    acquire_fence();  // start() published the fields before segment_
    ++samples_rendered_;
    if (segment == EncoderSegment::tail) {
        if (--countdown_ == 0) go_idle();
        return 0;
    }
    int16_t out = 0;
    const uint32_t u = slot_phase_high_;
    const bool peak = slot_kind(segment) == SlotKind::peak;
    const int16_t envelope = peak ? peak_q15(u) : envelope_q15(segment, u);
    if (envelope != 0) {
        const int16_t crest = multiply_q15(config_.amplitude, envelope);
        out = multiply_q15(crest, sine_q15(peak ? data_phase_ : tone_phase_));
    }
    tone_phase_ += tone_step_;
    data_phase_ += data_step_;
    // 64-bit slot_phase += slot_step; the slot ends when the sum wraps (it is then below the step).
    const uint32_t low = slot_phase_low_ + slot_step_low_;
    const uint32_t high = slot_phase_high_ + slot_step_high_ + (low < slot_step_low_ ? 1u : 0u);
    slot_phase_low_ = low;
    slot_phase_high_ = high;
    if (high < slot_step_high_ || (high == slot_step_high_ && low < slot_step_low_)) next_slot(segment);
    return out;
}

size_t Encoder::render(int16_t* out, size_t count) {
    size_t written = 0;
    while (written < count && segment_ != EncoderSegment::idle) out[written++] = next_sample();
    return written;
}

bool Encoder::busy() const {
    const bool busy = segment_ != EncoderSegment::idle;
    acquire_fence();  // after idle, start() may reuse what the consumer read before it went idle
    return busy;
}

size_t Encoder::queue_free() const {
    return k_queue_size - queued();
}

size_t Encoder::queued() const {
    return static_cast<uint8_t>(queue_head_ - queue_tail_);
}

// lead + (N_tune + N_sync + 9 + D + ceil(D / N) + 2) T + tail, D = ceil(8 n / k) (spec 2.1).
uint32_t Encoder::duration_samples(size_t data_bytes) const {
    const uint32_t k_saturated = 0xFFFFFFFFu;
    if (!config_.valid()) return 0;
    // Every byte needs at least one slot of at least 32 samples; below that bound the slot count fits 32 bits.
    if (data_bytes > k_saturated / k_min_slot_samples) return k_saturated;
    const uint32_t peaks = peak_slots(static_cast<uint32_t>(data_bytes), config_.bits_per_peak);
    const uint32_t frames = (peaks + config_.data_slots - 1) / config_.data_slots;
    const uint32_t slots =
        tune_slots(config_) + config_.sync_markers + k_header_slots + 1 + peaks + frames + k_eot_markers;
    const uint64_t slotted_us =
        static_cast<uint64_t>(config_.lead_in_ms) * k_us_per_ms + static_cast<uint64_t>(slots) * config_.slot_us;
    if (slotted_us > static_cast<uint64_t>(k_saturated) * k_us_per_s / config_.sample_rate_hz) return k_saturated;
    const uint64_t slotted = (slotted_us * config_.sample_rate_hz + k_us_per_s - 1) / k_us_per_s;
    const uint64_t total = slotted + ms_to_samples_floor(config_.tail_ms, config_.sample_rate_hz);
    return total > k_saturated ? k_saturated : static_cast<uint32_t>(total);
}

// Consumer side (see encoder.hpp): segment_ is read once, so segment and kind describe the same slot.
EncoderStatus Encoder::status() const {
    EncoderStatus status;
    status.segment = segment_;
    status.kind = slot_kind(status.segment);
    status.byte = 0;
    status.slot = 0;
    status.symbol = 0;
    status.tone = 0;
    if (status.segment == EncoderSegment::header || status.segment == EncoderSegment::frame) {
        status.slot = slot_;
        if (status.kind == SlotKind::peak) {
            status.symbol = symbol_;
            status.tone = tone_;
        }
        // The bytes of a frame stay from queue_tail_ until its STOP ends.
        if (status.segment == EncoderSegment::frame) status.byte = queue_[queue_tail_ & k_queue_mask];
    }
    status.slot_index = slot_index_;
    status.samples_rendered = samples_rendered_;
    return status;
}

const EncoderConfig& Encoder::config() const {
    return config_;
}

void Encoder::next_slot(EncoderSegment segment) {
    ++slot_index_;
    switch (segment) {
        case EncoderSegment::lead_in:
            if (--countdown_ == 0) begin_tune();
            break;
        case EncoderSegment::tune:
            if (--countdown_ == 0) {
                countdown_ = config_.sync_markers;
                segment_ = EncoderSegment::sync;
            } else {
                slot_ = countdown_ == 1 ? k_tune_last : k_tune_middle;
            }
            break;
        case EncoderSegment::sync:
            flip_sign();
            if (--countdown_ == 0) begin_header();
            break;
        case EncoderSegment::header:
            if (slot_ < k_header_slots) {
                ++slot_;
                if (slot_ < k_header_slots) load_peak(segment);
                break;
            }
            flip_sign();
            begin_frame_or_eot();
            break;
        case EncoderSegment::frame:
            if (slot_ < frame_d_) {
                ++slot_;
                if (slot_ < frame_d_) load_peak(segment);
                break;
            }
            flip_sign();
            release_fence();  // the frame's bytes were read before their room is handed back
            queue_tail_ = static_cast<uint8_t>(queue_tail_ + frame_n_);
            // A short final frame is always followed by the EOT: its STOP and the two EOT markers form the
            // flip triple the decoder looks for inside the frame.
            if (frame_d_ < config_.data_slots) {
                begin_eot();
            } else {
                begin_frame_or_eot();
            }
            break;
        case EncoderSegment::eot:
            flip_sign();
            if (--countdown_ == 0) begin_tail();
            break;
        case EncoderSegment::idle:
        case EncoderSegment::tail:
            break;
    }
}

void Encoder::begin_tune() {
    countdown_ = tune_slots_;
    slot_ = k_tune_first;
    segment_ = EncoderSegment::tune;
}

void Encoder::begin_header() {
    slot_ = 0;
    segment_ = EncoderSegment::header;
    load_peak(EncoderSegment::header);
}

// q = 0: EOT; q >= B: a full frame; 0 < q < B: a short final frame of ceil(8 q / k) peaks (spec 2.4).
void Encoder::begin_frame_or_eot() {
    const uint8_t queued = static_cast<uint8_t>(queue_head_ - queue_tail_);
    acquire_fence();  // the bytes written before that head
    if (queued == 0) {
        begin_eot();
        return;
    }
    const uint8_t full = config_.frame_bytes();
    frame_n_ = queued < full ? queued : full;
    frame_d_ = queued < full ? short_frame_peaks(queued, config_.bits_per_peak) : config_.data_slots;
    slot_ = 0;
    segment_ = EncoderSegment::frame;
    load_peak(EncoderSegment::frame);
}

void Encoder::begin_eot() {
    countdown_ = k_eot_markers;
    segment_ = EncoderSegment::eot;
}

void Encoder::begin_tail() {
    countdown_ = tail_samples_;
    if (countdown_ != 0) {
        segment_ = EncoderSegment::tail;
    } else {
        go_idle();
    }
}

// The producer may start() again once it sees idle: what the consumer read comes first.
void Encoder::go_idle() {
    release_fence();
    segment_ = EncoderSegment::idle;
}

// s <- -s: half a turn on the NCO is an exact negation of sine_q15, applied at the slot edge where e = 0.
void Encoder::flip_sign() {
    tone_phase_ += k_half_turn;
}

// Header: tone h_j on the standard grid. Frame: bits [slot k, slot k + k) of the frame, MSB first, then
// Gray + rotation. Step = f_ref -+ (5 + n c) / T, as NCO steps (spec 1.7).
void Encoder::load_peak(EncoderSegment segment) {
    uint32_t spacing_step = std_step_;
    if (segment == EncoderSegment::header) {
        symbol_ = static_cast<uint8_t>(header_tones_ & k_header_tone_mask);  // no GF(8) arithmetic in the ISR
        header_tones_ >>= k_header_tone_bits;
        tone_ = symbol_;
    } else {
        const uint8_t k = config_.bits_per_peak;
        const uint16_t bit = static_cast<uint16_t>(slot_) * k;
        const uint8_t index = static_cast<uint8_t>(bit / k_bits_per_byte);
        const uint8_t offset = static_cast<uint8_t>(bit % k_bits_per_byte);
        uint16_t pair = 0;
        for (uint8_t i = 0; i < 2; ++i) {
            pair = static_cast<uint16_t>(pair << k_bits_per_byte);
            if (index + i < frame_n_) pair |= queue_[static_cast<uint8_t>(queue_tail_ + index + i) & k_queue_mask];
        }
        const uint8_t pair_bits = 2 * k_bits_per_byte;
        symbol_ = static_cast<uint8_t>((pair >> (pair_bits - offset - k)) & ((1u << k) - 1u));
        tone_ = peak_tone(symbol_, slot_, k);
        if (config_.spacing == Spacing::dense) spacing_step = unit_step_;
    }
    const uint32_t offset_step = k_grid_guard * unit_step_ + tone_ * spacing_step;
    data_step_ = config_.side == GridSide::above ? tone_step_ + offset_step : tone_step_ - offset_step;
}

int16_t Encoder::envelope_q15(EncoderSegment segment, uint32_t slot_phase) const {
    switch (segment) {
        case EncoderSegment::tune:
            if (slot_ == k_tune_first && slot_phase < k_ramp_end) return ramp_q15(slot_phase);
            if (slot_ == k_tune_last && slot_phase > k_ramp_begin) return ramp_q15(slot_phase);
            return k_q15_one;
        case EncoderSegment::sync:
        case EncoderSegment::eot:
        case EncoderSegment::header:
        case EncoderSegment::frame:
            return marker_q15(slot_phase);
        case EncoderSegment::idle:
        case EncoderSegment::lead_in:
        case EncoderSegment::tail:
            break;
    }
    return 0;
}

SlotKind Encoder::slot_kind(EncoderSegment segment) const {
    switch (segment) {
        case EncoderSegment::tune:
            return SlotKind::tone;
        case EncoderSegment::sync:
        case EncoderSegment::eot:
            return SlotKind::marker;
        case EncoderSegment::header:
            return slot_ < k_header_slots ? SlotKind::peak : SlotKind::marker;
        case EncoderSegment::frame:
            return slot_ < frame_d_ ? SlotKind::peak : SlotKind::marker;
        case EncoderSegment::idle:
        case EncoderSegment::lead_in:
        case EncoderSegment::tail:
            break;
    }
    return SlotKind::silent;
}

}  // namespace unlimited
