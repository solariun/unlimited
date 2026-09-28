#include "unlimited/encoder.hpp"

namespace unlimited {

const uint16_t Encoder::k_queue_size;

#if defined(__AVR__)
// Spec 3.9: the AVR encoder without its queue; the queue itself may be set anywhere in 16..128.
static_assert(sizeof(Encoder) - Encoder::k_queue_size <= 96, "the AVR encoder must fit in 96 bytes plus its queue");
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
const uint8_t k_top_bit = 0x80;

// Guaranteed by the ranges alone, so check() needs no test: every tone stays 500 Hz below Nyquist and a slot is
// never shorter than 32 samples.
static_assert(k_max_tone_hz + k_tone_nyquist_margin_hz < k_min_sample_rate_hz / 2, "tones below rate / 2 - 500 Hz");
static_assert(k_min_slot_us / k_us_per_ms * (k_min_sample_rate_hz / k_ms_per_s) >= k_min_slot_samples, "T >= 32 samples");

// VOX lead slot position, kept in slot_ while in the vox_lead segment.
const uint8_t k_lead_first = 0;
const uint8_t k_lead_middle = 1;
const uint8_t k_lead_last = 2;

// Slot position u = slot_phase / 2^32 (the high word of the 64-bit slot phase).
const uint8_t k_phase_high_shift = 32;
const uint8_t k_phase_fraction_bits = 64;
const uint32_t k_ramp_end = 0x40000000u;       // u = 0.25
const uint32_t k_ramp_begin = 0xC0000000u;     // u = 0.75

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

// max(ceil(vox_lead_ms / T), k_min_vox_lead_slots) when a VOX lead is configured, else 0; at most
// ceil(65535 ms / 4 ms) = 16384: fits 16 bits.
uint16_t vox_lead_slots(const EncoderConfig& config) {
    if (config.vox_lead_ms == 0) return 0;
    const uint32_t lead_us = static_cast<uint32_t>(config.vox_lead_ms) * k_us_per_ms;
    const uint32_t slots = (lead_us + config.slot_us - 1) / config.slot_us;
    return static_cast<uint16_t>(slots < k_min_vox_lead_slots ? k_min_vox_lead_slots : slots);
}

// floor(ms * rate / 1000) without 64-bit arithmetic (rate <= 192000, ms <= 65535).
uint32_t ms_to_samples_floor(uint16_t ms, uint32_t rate_hz) {
    return ms * (rate_hz / k_ms_per_s) + ms * (rate_hz % k_ms_per_s) / k_ms_per_s;
}

// max(floor(tail_ms * rate / 1000), ceil(k_min_tail_slots * rate * T / 10^6)): the tail is never shorter than the
// silence a receiver needs before the next transmission's START (spec 2.1).
uint32_t tail_samples(const EncoderConfig& config) {
    const uint64_t slots_us = static_cast<uint64_t>(k_min_tail_slots) * config.slot_us * config.sample_rate_hz;
    const uint32_t minimum = static_cast<uint32_t>((slots_us + k_us_per_s - 1) / k_us_per_s);
    const uint32_t tail = ms_to_samples_floor(config.tail_ms, config.sample_rate_hz);
    return tail > minimum ? tail : minimum;
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

}  // namespace

EncoderConfig::EncoderConfig()
    : sample_rate_hz(k_default_rate_hz),
      slot_us(slot_us_for_centi_speed(k_default_centi_bytes_per_second)),
      tone_hz(k_default_tone_hz),
      amplitude(k_default_amplitude),
      passband(),
      lead_in_ms(0),
      vox_lead_ms(0),
      tail_ms(k_default_tail_ms) {
    passband.low_hz = k_ssb_passband_low_hz;
    passband.high_hz = k_ssb_passband_high_hz;
}

ConfigError EncoderConfig::check() const {
    if (sample_rate_hz < k_min_sample_rate_hz || sample_rate_hz > k_max_sample_rate_hz) return ConfigError::sample_rate;
    if (tone_hz < k_min_tone_hz || tone_hz > k_max_tone_hz) return ConfigError::tone;
    if (!slot_valid(slot_us)) return ConfigError::slot;
    if (!passband_valid(passband)) return ConfigError::passband;
    if (!passband_fit(occupied_band(*this), passband).fits) return ConfigError::outside_passband;
    return amplitude > 0 ? ConfigError::none : ConfigError::amplitude;
}

bool EncoderConfig::valid() const {
    return check() == ConfigError::none;
}

Band occupied_band(const EncoderConfig& config) {
    return occupied_band(config.tone_hz, config.slot_us);
}

Passband search_range(const EncoderConfig& config) {
    return search_range(config.passband, config.slot_us);
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
      byte_index_(0),
      vox_lead_slots_(0),
      segment_(EncoderSegment::idle),
      kind_(SlotKind::silent),
      slot_(0),
      byte_(0),
      bits_(0),
      queue_head_(0),
      queue_tail_(0),
      queue_() {}

// Producer side: everything the consumer reads is set up first, then published with segment_.
bool Encoder::start() {
    if (busy() || queued() == 0 || !config_.valid()) return false;
    const uint32_t rate = config_.sample_rate_hz;
    tone_phase_ = 0;  // the carrier sign starts at +1
    tone_step_ = round_fraction_2_32(config_.tone_hz, rate);
    vox_lead_slots_ = vox_lead_slots(config_);
    tail_samples_ = tail_samples(config_);
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
    byte_index_ = 0;
    slot_ = 0;
    kind_ = SlotKind::silent;
    EncoderSegment first = EncoderSegment::lead_in;
    if (countdown_ == 0) first = vox_lead_slots_ != 0 ? enter_vox_lead() : enter_window_or_tail();
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

// lead-in + (N_vox + gap + 10 n) T + tail (spec 2.4).
uint32_t Encoder::duration_samples(size_t data_bytes) const {
    const uint32_t k_saturated = 0xFFFFFFFFu;
    if (data_bytes == 0 || !config_.valid()) return 0;
    // Every byte takes 10 slots of at least 32 samples; below that bound the slot count fits 32 bits.
    if (data_bytes > k_saturated / (k_window_slots * k_min_slot_samples)) return k_saturated;
    const uint16_t vox = vox_lead_slots(config_);
    const uint32_t slots = static_cast<uint32_t>(data_bytes) * k_window_slots + vox + (vox != 0 ? k_vox_gap_slots : 0);
    const uint64_t slotted_us =
        static_cast<uint64_t>(config_.lead_in_ms) * k_us_per_ms + static_cast<uint64_t>(slots) * config_.slot_us;
    if (slotted_us > static_cast<uint64_t>(k_saturated) * k_us_per_s / config_.sample_rate_hz) return k_saturated;
    const uint64_t slotted = (slotted_us * config_.sample_rate_hz + k_us_per_s - 1) / k_us_per_s;
    const uint64_t total = slotted + tail_samples(config_);
    return total > k_saturated ? k_saturated : static_cast<uint32_t>(total);
}

// Consumer side (see encoder.hpp): segment_ is read once, so every field describes the same slot.
EncoderStatus Encoder::status() const {
    EncoderStatus status;
    status.segment = segment_;
    status.kind = status.segment == EncoderSegment::idle ? SlotKind::silent : kind_;
    status.slot = 0;
    status.byte = 0;
    status.bit_index = 0;
    status.byte_index = 0;
    if (status.segment == EncoderSegment::window) {
        status.slot = slot_;
        status.byte = byte_;
        status.byte_index = byte_index_;
        if (slot_ >= k_first_data_slot && slot_ < k_stop_slot) status.bit_index = static_cast<uint8_t>(slot_ - 1u);
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
            if (--countdown_ != 0) break;
            segment_ = vox_lead_slots_ != 0 ? enter_vox_lead() : enter_window_or_tail();
            break;
        case EncoderSegment::vox_lead:
            if (--countdown_ == 0) {
                segment_ = enter_gap();
            } else {
                slot_ = countdown_ == 1 ? k_lead_last : k_lead_middle;
            }
            break;
        case EncoderSegment::gap:
            if (--countdown_ == 0) segment_ = enter_window_or_tail();
            break;
        case EncoderSegment::window:
            if (slot_ < k_stop_slot - 1u) {
                // The next data slot: bits_ holds the bits not sent yet, the next one on top.
                ++slot_;
                kind_ = (bits_ & k_top_bit) != 0 ? SlotKind::one : SlotKind::zero;
                bits_ = static_cast<uint8_t>(bits_ << 1);
            } else if (slot_ == k_stop_slot - 1u) {
                slot_ = k_stop_slot;
                kind_ = SlotKind::stop;
            } else {
                // The STOP ended: the byte is handed back and the next window (or the tail) begins.
                ++byte_index_;
                release_fence();  // the byte was read before its room is handed back
                queue_tail_ = static_cast<uint8_t>(queue_tail_ + 1u);
                segment_ = enter_window_or_tail();
            }
            break;
        case EncoderSegment::idle:
        case EncoderSegment::tail:
            break;
    }
}

// The enter_ functions set up a segment's first slot and return the segment; the caller publishes it.
EncoderSegment Encoder::enter_vox_lead() {
    countdown_ = vox_lead_slots_;
    slot_ = k_lead_first;
    kind_ = SlotKind::lead;
    return EncoderSegment::vox_lead;
}

EncoderSegment Encoder::enter_gap() {
    countdown_ = k_vox_gap_slots;
    slot_ = 0;
    kind_ = SlotKind::silent;
    return EncoderSegment::gap;
}

// At a window boundary (spec 2.3): a queued byte starts its window with the START; an empty queue ends the
// transmission with the tail. No division.
EncoderSegment Encoder::enter_window_or_tail() {
    const uint8_t queued = static_cast<uint8_t>(queue_head_ - queue_tail_);
    acquire_fence();  // the bytes written before that head
    if (queued == 0) {
        countdown_ = tail_samples_;
        slot_ = 0;
        kind_ = SlotKind::silent;
        return EncoderSegment::tail;
    }
    byte_ = queue_[queue_tail_ & k_queue_mask];
    bits_ = byte_;
    slot_ = k_start_slot;
    kind_ = SlotKind::start;
    return EncoderSegment::window;
}

// The producer may start() again once it sees idle: what the consumer read comes first.
void Encoder::go_idle() {
    release_fence();
    segment_ = EncoderSegment::idle;
}

int16_t Encoder::envelope_q15(uint32_t slot_phase) const {
    switch (kind_) {
        case SlotKind::lead:
            if (slot_ == k_lead_first && slot_phase < k_ramp_end) return ramp_q15(slot_phase);
            if (slot_ == k_lead_last && slot_phase > k_ramp_begin) return ramp_q15(slot_phase);
            return k_q15_one;
        case SlotKind::start:
        case SlotKind::one:
        case SlotKind::stop:
            return tukey_q15(slot_phase);
        case SlotKind::silent:
        case SlotKind::zero:
            break;
    }
    return 0;
}

}  // namespace unlimited
