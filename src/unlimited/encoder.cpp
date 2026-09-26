#include "unlimited/encoder.hpp"

namespace unlimited {

const uint16_t Encoder::k_queue_size;

#if defined(__AVR__)
// B5: 145 bytes with the default 64-byte queue; the queue itself may be set anywhere in 16..128.
static_assert(sizeof(Encoder) - Encoder::k_queue_size <= 96, "B5: the AVR encoder must fit in 96 bytes plus its queue");
#endif

namespace {

const uint32_t k_default_rate_hz = 8000;
const uint16_t k_tone_nyquist_margin_hz = 500;
const uint8_t k_min_slot_samples = 32;
const int16_t k_default_amplitude = 23197;  // -3 dBFS
const uint32_t k_us_per_s = 1000000;
const uint32_t k_us_per_ms = 1000;
const uint32_t k_ms_per_s = 1000;
const uint8_t k_queue_mask = static_cast<uint8_t>(Encoder::k_queue_size - 1);
const uint8_t k_byte_shift = 3;  // bits = bytes << 3
const uint8_t k_msb = k_bits_per_byte - 1;

// Guaranteed by the ranges alone, so check() needs no test: every tone stays 500 Hz below Nyquist and a slot is
// never shorter than 32 samples.
static_assert(k_max_tone_hz + k_tone_nyquist_margin_hz < k_min_sample_rate_hz / 2, "tones below rate / 2 - 500 Hz");
static_assert(k_min_slot_us / k_us_per_ms * (k_min_sample_rate_hz / k_ms_per_s) >= k_min_slot_samples, "T >= 32 samples");

// Presets (spec 1.7): slot length and bits per package; all on 1500 Hz.
const uint16_t k_hf_slow_slot_ms = 32;
const uint16_t k_hf_slot_ms = 16;
const uint16_t k_hf_fast_slot_ms = 8;
const uint16_t k_am_slot_ms = 8;
const uint16_t k_fm_slot_ms = 4;

// The shortest slot of the receiver window that hears a sender by default (spec 1.5): the fastest window, the fm
// profile's, starts at k_min_slot_us; the ssb and am profiles' at k_fast_slot_us holds T up to k_speed_span times
// that; a slower T needs the smallest whole ms whose window holds it (the demos' "heard by" suggestion).
uint32_t receiver_min_slot_us(uint32_t slot_us) {
    if (slot_us < k_fast_slot_us) return k_min_slot_us;
    const uint32_t us_per_window_ms = k_speed_span * k_us_per_ms;
    const uint32_t needed_us = (slot_us + us_per_window_ms - 1) / us_per_window_ms * k_us_per_ms;
    return needed_us > k_fast_slot_us ? needed_us : k_fast_slot_us;
}

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
const uint32_t k_half_turn = 0x80000000u;

const int16_t k_q15_one = 32767;
const uint8_t k_q15_shift = 15;
const int32_t k_q15_half = static_cast<int32_t>(1) << (k_q15_shift - 1);
const uint8_t k_word_shift = 16;

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

// max(ceil(tune_ms / T), k_min_tune_slots); at most ceil(65535 ms / 4 ms) = 16384: fits 16 bits.
uint16_t tune_slots(const EncoderConfig& config) {
    const uint32_t tune_us = static_cast<uint32_t>(config.tune_ms) * k_us_per_ms;
    const uint32_t slots = (tune_us + config.slot_us - 1) / config.slot_us;
    return static_cast<uint16_t>(slots < k_min_tune_slots ? k_min_tune_slots : slots);
}

// floor(ms * rate / 1000) without 64-bit arithmetic (rate <= 192000, ms <= 65535).
uint32_t ms_to_samples_floor(uint16_t ms, uint32_t rate_hz) {
    return ms * (rate_hz / k_ms_per_s) + ms * (rate_hz % k_ms_per_s) / k_ms_per_s;
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

// w(u): Tukey alpha 0.5, ramps of T / 4 and a flat top of T / 2.
int16_t tukey_q15(uint32_t slot_phase) {
    return (slot_phase < k_ramp_end || slot_phase > k_ramp_begin) ? ramp_q15(slot_phase) : k_q15_one;
}

// w(u) * r(u): w is flat over the reversal, so the product is w, then r, then -w.
int16_t marker_q15(uint32_t slot_phase) {
    if (slot_phase <= k_reversal_begin) return tukey_q15(slot_phase);
    if (slot_phase < k_reversal_end) return cosine_q15((slot_phase - k_reversal_begin) << 1);
    return static_cast<int16_t>(-tukey_q15(slot_phase));
}

void set_mode(EncoderConfig& config, uint16_t slot_ms, uint8_t bits_per_package, uint16_t passband_low_hz,
              uint16_t passband_high_hz, uint16_t lead_in_ms) {
    config.slot_us = static_cast<uint32_t>(slot_ms) * k_us_per_ms;
    config.bits_per_package = bits_per_package;
    config.passband.low_hz = passband_low_hz;
    config.passband.high_hz = passband_high_hz;
    config.lead_in_ms = lead_in_ms;
}

// A switch, not a table: no RAM on AVR.
void set_preset(EncoderConfig& config, Preset preset) {
    switch (preset) {
        case Preset::hf_slow:
            set_mode(config, k_hf_slow_slot_ms, k_hf_bits_per_package, k_ssb_passband_low_hz, k_ssb_passband_high_hz, 0);
            break;
        case Preset::hf:
            set_mode(config, k_hf_slot_ms, k_hf_bits_per_package, k_ssb_passband_low_hz, k_ssb_passband_high_hz, 0);
            break;
        case Preset::hf_fast:
            set_mode(config, k_hf_fast_slot_ms, k_hf_bits_per_package, k_ssb_passband_low_hz, k_ssb_passband_high_hz, 0);
            break;
        case Preset::am:
            set_mode(config, k_am_slot_ms, k_wide_bits_per_package, k_am_passband_low_hz, k_am_passband_high_hz, 0);
            break;
        case Preset::fm:
            set_mode(config, k_fm_slot_ms, k_wide_bits_per_package, k_fm_passband_low_hz, k_fm_passband_high_hz,
                     k_default_fm_lead_in_ms);
            break;
    }
}

}  // namespace

EncoderConfig::EncoderConfig()
    : sample_rate_hz(k_default_rate_hz),
      slot_us(static_cast<uint32_t>(k_hf_slot_ms) * k_us_per_ms),
      tone_hz(k_default_tone_hz),
      amplitude(k_default_amplitude),
      passband(),
      lead_in_ms(0),
      tune_ms(k_default_tune_ms),
      tail_ms(k_default_tail_ms),
      bits_per_package(k_hf_bits_per_package),
      sync_markers(k_default_sync_markers) {
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
    if (slot_us < k_min_slot_us || slot_us > k_max_slot_us) return ConfigError::slot;
    if (slot_us < k_fast_slot_us && tone_hz < k_min_fast_tone_hz) return ConfigError::fast_tone;
    if (bits_per_package < k_min_bits_per_package || bits_per_package > k_max_bits_per_package) {
        return ConfigError::bits_per_package;
    }
    if ((static_cast<uint32_t>(bits_per_package) + 1u) * slot_us > k_max_package_us) return ConfigError::package_length;
    if (!passband_valid(passband)) return ConfigError::passband;
    if (!passband_fit(occupied_band(*this), passband).fits) return ConfigError::outside_passband;
    if (sync_markers < k_min_sync_markers || sync_markers > k_max_sync_markers) return ConfigError::sync_markers;
    return amplitude > 0 ? ConfigError::none : ConfigError::amplitude;
}

bool EncoderConfig::valid() const {
    return check() == ConfigError::none;
}

Band occupied_band(const EncoderConfig& config) {
    return occupied_band(config.tone_hz, config.slot_us);
}

Passband search_range(const EncoderConfig& config) {
    return search_range(config.passband, receiver_min_slot_us(config.slot_us));
}

PassbandFit passband_fit(const EncoderConfig& config) {
    return passband_fit(config.tone_hz, config.slot_us, config.passband, search_range(config));
}

Encoder::Encoder(const EncoderConfig& config)
    : config_(config),
      slot_phase_high_(0),
      slot_phase_low_(0),
      slot_step_high_(0),
      slot_step_low_(0),
      tone_phase_(0),
      tone_step_(0),
      countdown_(0),
      tail_samples_(0),
      slot_index_(0),
      samples_rendered_(0),
      package_index_(0),
      byte_index_(0),
      tune_slots_(0),
      segment_(EncoderSegment::idle),
      kind_(SlotKind::silent),
      slot_(0),
      package_bits_(0),
      bit_offset_(0),
      queue_head_(0),
      queue_tail_(0),
      queue_() {}

// Producer side: everything the consumer reads is set up first, then published with segment_.
bool Encoder::start() {
    if (busy() || queued() == 0 || !config_.valid()) return false;
    const uint32_t rate = config_.sample_rate_hz;
    tone_phase_ = 0;  // the carrier sign starts at +1
    tone_step_ = round_fraction_2_32(config_.tone_hz, rate);
    tune_slots_ = tune_slots(config_);
    tail_samples_ = ms_to_samples_floor(config_.tail_ms, rate);
    // One slot is L = rate * slot_us / 10^6 samples; the step is rounded up so slot j starts exactly at sample
    // ceil(j L) (the rounding error stays below one exact-rational gap for 7.5e8 samples).
    const uint64_t slot_step = ceil_fraction_2_64(k_us_per_s, static_cast<uint64_t>(rate) * config_.slot_us);
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
    package_index_ = 0;
    byte_index_ = 0;
    package_bits_ = 0;
    bit_offset_ = 0;
    slot_ = 0;
    kind_ = SlotKind::silent;
    EncoderSegment first = EncoderSegment::lead_in;
    if (countdown_ == 0) {
        countdown_ = tune_slots_;
        slot_ = k_tune_first;
        kind_ = SlotKind::tone;
        first = EncoderSegment::tune;
    }
    release_fence();
    segment_ = first;
    return true;
}

// Producer side: the byte is stored before the new head is published; the freed room was read before the tail moved.
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
    bit_offset_ = 0;
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
    const int16_t envelope = envelope_q15(slot_phase_high_);
    if (envelope != 0) out = multiply_q15(multiply_q15(config_.amplitude, envelope), sine_q15(tone_phase_));
    tone_phase_ += tone_step_;
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

// lead + (N_tune + N_sync + B + P + 2) T + tail, B = 8 n, P = ceil(B / N) (spec 2.4).
uint32_t Encoder::duration_samples(size_t data_bytes) const {
    const uint32_t k_saturated = 0xFFFFFFFFu;
    if (data_bytes == 0 || !config_.valid()) return 0;
    // Every byte takes at least 8 slots of at least 32 samples; below that bound the slot count fits 32 bits.
    if (data_bytes > k_saturated / (k_bits_per_byte * k_min_slot_samples)) return k_saturated;
    const uint32_t bits = static_cast<uint32_t>(data_bytes) * k_bits_per_byte;
    const uint32_t packages = (bits + config_.bits_per_package - 1u) / config_.bits_per_package;
    const uint32_t slots = tune_slots(config_) + config_.sync_markers + bits + packages + k_end_markers;
    const uint64_t slotted_us =
        static_cast<uint64_t>(config_.lead_in_ms) * k_us_per_ms + static_cast<uint64_t>(slots) * config_.slot_us;
    if (slotted_us > static_cast<uint64_t>(k_saturated) * k_us_per_s / config_.sample_rate_hz) return k_saturated;
    const uint64_t slotted = (slotted_us * config_.sample_rate_hz + k_us_per_s - 1) / k_us_per_s;
    const uint64_t total = slotted + ms_to_samples_floor(config_.tail_ms, config_.sample_rate_hz);
    return total > k_saturated ? k_saturated : static_cast<uint32_t>(total);
}

// Consumer side (see encoder.hpp): segment_ is read once, so every field describes the same slot.
EncoderStatus Encoder::status() const {
    EncoderStatus status;
    status.segment = segment_;
    status.kind = status.segment == EncoderSegment::idle ? SlotKind::silent : kind_;
    status.slot = 0;
    status.package_bits = 0;
    status.byte = 0;
    status.bit_index = 0;
    status.package_index = 0;
    status.byte_index = 0;
    if (status.segment == EncoderSegment::package) {
        status.slot = slot_;
        status.package_bits = package_bits_;
        status.package_index = package_index_;
        if (slot_ <= package_bits_) {
            status.byte = queue_[queue_tail_ & k_queue_mask];
            status.bit_index = bit_offset_;
            status.byte_index = byte_index_;
        }
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
                begin_sync();
            } else {
                slot_ = countdown_ == 1 ? k_tune_last : k_tune_middle;
            }
            break;
        case EncoderSegment::sync:
            flip_sign();
            if (--countdown_ == 0) begin_package_or_end();
            break;
        case EncoderSegment::package:
            if (slot_ <= package_bits_) {
                // A data slot ended: its byte is handed back after its last bit.
                if (++bit_offset_ == k_bits_per_byte) {
                    bit_offset_ = 0;
                    ++byte_index_;
                    release_fence();  // the byte was read before its room is handed back
                    queue_tail_ = static_cast<uint8_t>(queue_tail_ + 1u);
                }
                ++slot_;
                if (slot_ <= package_bits_) {
                    load_bit();
                } else {
                    kind_ = SlotKind::marker;  // the STOP
                }
                break;
            }
            flip_sign();
            ++package_index_;
            // A short final package always ends the transmission: its STOP and the two END markers are three flips
            // in a row where data slots should be, which only an end can give.
            if (package_bits_ < config_.bits_per_package) {
                begin_end();
            } else {
                begin_package_or_end();
            }
            break;
        case EncoderSegment::end:
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
    kind_ = SlotKind::tone;
    segment_ = EncoderSegment::tune;
}

void Encoder::begin_sync() {
    countdown_ = config_.sync_markers;
    slot_ = 0;
    kind_ = SlotKind::marker;
    segment_ = EncoderSegment::sync;
}

// At a START: R = 8 queued - bits already sent of the byte at the tail. R >= N: a full package; 0 < R < N: a short
// final package of R bits; R = 0: END (spec 2.3). No division.
void Encoder::begin_package_or_end() {
    const uint8_t queued = static_cast<uint8_t>(queue_head_ - queue_tail_);
    acquire_fence();  // the bytes written before that head
    const uint16_t bits = static_cast<uint16_t>((static_cast<uint16_t>(queued) << k_byte_shift) - bit_offset_);
    if (bits == 0) {
        begin_end();
        return;
    }
    package_bits_ = bits < config_.bits_per_package ? static_cast<uint8_t>(bits) : config_.bits_per_package;
    slot_ = 1;
    load_bit();
    segment_ = EncoderSegment::package;
}

void Encoder::begin_end() {
    countdown_ = k_end_markers;
    slot_ = 0;
    package_bits_ = 0;
    kind_ = SlotKind::marker;
    segment_ = EncoderSegment::end;
}

void Encoder::begin_tail() {
    countdown_ = tail_samples_;
    kind_ = SlotKind::silent;
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

// Bit bit_offset_ (MSB first) of the byte at the queue tail.
void Encoder::load_bit() {
    const uint8_t byte = queue_[queue_tail_ & k_queue_mask];
    kind_ = ((byte >> (k_msb - bit_offset_)) & 1u) != 0 ? SlotKind::one : SlotKind::zero;
}

int16_t Encoder::envelope_q15(uint32_t slot_phase) const {
    switch (kind_) {
        case SlotKind::tone:
            if (slot_ == k_tune_first && slot_phase < k_ramp_end) return ramp_q15(slot_phase);
            if (slot_ == k_tune_last && slot_phase > k_ramp_begin) return ramp_q15(slot_phase);
            return k_q15_one;
        case SlotKind::one:
            return tukey_q15(slot_phase);
        case SlotKind::marker:
            return marker_q15(slot_phase);
        case SlotKind::silent:
        case SlotKind::zero:
            break;
    }
    return 0;
}

}  // namespace unlimited
