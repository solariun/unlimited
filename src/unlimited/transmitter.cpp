#include "unlimited/transmitter.hpp"

namespace unlimited {

const uint16_t ModemTransmitter::k_queue_size;
const uint8_t ModemTransmitter::k_frame_slots;
const uint16_t ModemTransmitter::k_budget_bytes;

// Spec 12.2: the send side on any target, the queue aside (a Nano build chooses a small UNLIMITED_MODEM_QUEUE).
static_assert(sizeof(ModemTransmitter) <= ModemTransmitter::k_queue_size +
                                              ModemTransmitter::k_frame_slots * sizeof(uint16_t) +
                                              ModemTransmitter::k_budget_bytes,
              "the send side must fit its queue, its frame slots and 512 bytes");

namespace {

// Samples per Encoder::render() call: a slot has at least 32 samples, so the encoder queue is topped up at least once
// a slot and never runs dry while the frame has bytes.
const size_t k_render_step = 32;
const uint32_t k_default_seed = 0x2545F491u;  // xorshift32 needs a state other than 0
const uint8_t k_draw_shift = 24;              // a draw is the top 8 bits of the generator
const uint8_t k_xorshift_a = 13;
const uint8_t k_xorshift_b = 17;
const uint8_t k_xorshift_c = 5;
const uint16_t k_resume_divisor = 4;          // the computer is woken when a quarter of the queue is free again

const uint32_t k_us_per_ms = 1000;

// Wrap-safe: a is before b on a millisecond clock that wraps every 49.7 days.
bool before(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) < 0;
}

uint32_t ceil_ms(uint32_t slots, uint32_t slot_us) {
    return (slots * slot_us + k_us_per_ms - 1) / k_us_per_ms;
}

// What the silence after a transmission's last STOP lacks when the next transmission follows at once: its tail
// (at least k_min_tail_slots) and its silent lead-in count; a VOX lead is a tone and does not.
uint32_t spacing_ms(const EncoderConfig& signal, const AccessConfig& access) {
    if (!slot_valid(signal.slot_us)) return 0;
    const uint32_t needed = ceil_ms(static_cast<uint32_t>(access.end_windows) * k_window_slots + k_end_margin_slots,
                                    signal.slot_us);
    const uint32_t min_tail = ceil_ms(k_min_tail_slots, signal.slot_us);
    const uint32_t tail = signal.tail_ms > min_tail ? signal.tail_ms : min_tail;
    const uint32_t lead = signal.vox_lead_ms > 0 ? 0 : signal.lead_in_ms;
    return needed > tail + lead ? needed - tail - lead : 0;
}

void publish(KissCounters& to, const KissCounters& from) {
    store_release(to.frames, from.frames);
    store_release(to.bytes, from.bytes);
    store_release(to.parameters, from.parameters);
    store_release(to.unknown, from.unknown);
    store_release(to.bad_escapes, from.bad_escapes);
    store_release(to.outside, from.outside);
}

KissCounters snapshot(const KissCounters& from) {
    KissCounters counters = KissCounters();
    counters.frames = load_acquire(from.frames);
    counters.bytes = load_acquire(from.bytes);
    counters.parameters = load_acquire(from.parameters);
    counters.unknown = load_acquire(from.unknown);
    counters.bad_escapes = load_acquire(from.bad_escapes);
    counters.outside = load_acquire(from.outside);
    return counters;
}

}  // namespace

AccessConfig::AccessConfig()
    : dwait_ms(k_default_dwait_ms),
      persist(k_default_persist),
      slot_time_ms(k_default_slot_time_ms),
      output_latency_ms(0),
      full_duplex(false),
      seed(0),
      end_windows(k_default_end_windows) {}

ModemTransmitter::ModemTransmitter(const EncoderConfig& signal, const AccessConfig& access, PttHandler ptt,
                                   void* context, WakeHandler wake)
    : signal_(signal),
      access_(access),
      ptt_(ptt),
      wake_(wake),
      context_(context),
      valid_(signal.sample_rate_hz == k_modem_rate_hz && signal.valid() && access.slot_time_ms > 0),
      encoder_(signal),
      kiss_(),
      kiss_counters_(),
      refusals_total_(0),
      written_(0),
      queue_(),
      frame_ends_(),
      queue_head_(0),
      queue_tail_(0),
      ends_head_(0),
      ends_tail_(0),
      refusals_(0),
      refusals_seen_(0),
      started_(0),
      sending_(false),
      fed_(0),
      frame_start_(0),
      on_air_sent_(0),
      on_air_size_(0),
      skipping_(false),
      done_(0),
      transmissions_(0),
      bytes_sent_(0),
      underruns_(0),
      bytes_skipped_(0),
      dcd_(false),
      dcd_changes_(0),
      state_(ChannelState::idle),
      keyed_(false),
      go_(0),
      spacing_ms_(spacing_ms(signal, access)),
      spaced_until_ms_(0),
      clock_started_(false),
      dcd_changes_seen_(0),
      quiet_since_ms_(0),
      draw_waiting_(false),
      draw_at_ms_(0),
      release_at_ms_(0),
      timer_set_(false),
      timer_ms_(0),
      random_(access.seed != 0 ? access.seed : k_default_seed),
      draws_(0),
      deferrals_(0) {}

// ---------------------------------------------------------------------------
// Computer side
// ---------------------------------------------------------------------------

// Each byte is looked at first and taken only when what it gives fits: a data byte needs room in the queue, the end of
// a frame a frame slot. A frame's end is published after its bytes and before any byte of the next frame, so the
// audio side, reading the head before the ends, never mistakes the next frame's bytes for this one's.
size_t ModemTransmitter::host_input(const uint8_t* data, size_t size) {
    if (!valid_) return 0;
    const uint16_t tail = load_acquire(queue_tail_);  // the audio side read the bytes and ends it handed back
    const uint8_t ends_tail = load_acquire(ends_tail_);
    uint16_t written = written_;
    uint8_t ends_head = ends_head_;
    bool ended = false;
    bool refused = false;
    size_t taken = 0;
    for (; taken < size; ++taken) {
        uint8_t value = 0;
        const KissStep step = kiss_.peek(data[taken], value);
        if (step == KissStep::data && static_cast<uint16_t>(written - tail) >= k_queue_size) {
            refused = true;
            break;
        }
        if (step == KissStep::end && static_cast<uint8_t>(ends_head - ends_tail) >= k_frame_slots) {
            refused = true;
            break;
        }
        kiss_.feed(data[taken], value);
        if (step == KissStep::data) {
            queue_[written & k_queue_mask] = value;
            written = static_cast<uint16_t>(written + 1u);
        } else if (step == KissStep::end) {
            frame_ends_[ends_head & k_slot_mask] = written;
            store_release(queue_head_, written);
            ends_head = static_cast<uint8_t>(ends_head + 1u);
            store_release(ends_head_, ends_head);
            ended = true;
        }
    }
    store_release(queue_head_, written);
    written_ = written;
    publish(kiss_counters_, kiss_.counters());
    if (refused) {
        store_release(refusals_total_, refusals_total_ + 1u);
        store_release(refusals_, static_cast<uint8_t>(refusals_ + 1u));
    }
    const bool full = static_cast<uint16_t>(written - tail) >= k_queue_size;  // a frame too long for the queue streams
    if ((ended || full) && wake_ != nullptr) wake_(ModemWake::control, context_);
    return taken;
}

// ---------------------------------------------------------------------------
// Audio side
// ---------------------------------------------------------------------------

void ModemTransmitter::audio_output(int16_t* out, size_t count) {
    size_t done = 0;
    if (valid_) {
        if (skipping_) skip();
        while (done < count) {
            if (!sending_) {
                const uint8_t go = load_acquire(go_);
                if (go == started_) break;
                started_ = go;
                begin();
                if (!sending_) continue;
            }
            top_up();
            const size_t step = count - done < k_render_step ? count - done : k_render_step;
            done += encoder_.render(out + done, step);
            if (!encoder_.busy()) finish();
        }
        if (sending_) publish_progress();
        offer_room();
    }
    for (; done < count; ++done) out[done] = 0;
}

// The control side keyed: the frame at the head of the queue goes to the encoder (lead-in or VOX lead first).
void ModemTransmitter::begin() {
    fed_ = 0;
    frame_start_ = queue_tail_;
    store_release(on_air_size_, 0);
    top_up();
    sending_ = encoder_.start();
    if (!sending_) finish();
}

// The encoder's queue is topped up with bytes of the frame on the air only: up to its end when that is known, else
// everything present (a frame longer than the send queue, still arriving).
void ModemTransmitter::top_up() {
    const uint16_t head = load_acquire(queue_head_);
    const uint8_t ends_head = load_acquire(ends_head_);
    const uint16_t tail = queue_tail_;
    uint16_t available = static_cast<uint16_t>(head - tail);
    if (ends_head != ends_tail_) {
        const uint16_t to_end = static_cast<uint16_t>(frame_ends_[ends_tail_ & k_slot_mask] - tail);
        if (to_end < available) available = to_end;
    }
    const size_t room = encoder_.queue_free();
    const uint16_t count = room < available ? static_cast<uint16_t>(room) : available;
    if (count == 0) return;
    for (uint16_t i = 0; i < count; ++i) encoder_.write(queue_[static_cast<uint16_t>(tail + i) & k_queue_mask]);
    fed_ += count;
    store_release(queue_tail_, static_cast<uint16_t>(tail + count));  // the bytes were read before their room goes back
}

// The transmission's audio ended (the tail is out). Its frame leaves the queue; a frame whose end had not arrived yet
// (the computer was slower than the air) is not split: the rest of it is dropped as it comes (skip()). done_ comes
// last: the control side, seeing it, sees the queue and skipping_ as they are now.
void ModemTransmitter::finish() {
    sending_ = false;
    const uint16_t head = load_acquire(queue_head_);
    const uint8_t ends_head = load_acquire(ends_head_);
    const uint16_t tail = queue_tail_;
    if (ends_head != ends_tail_) {
        const uint16_t end = frame_ends_[ends_tail_ & k_slot_mask];
        const uint16_t left = static_cast<uint16_t>(end - tail);
        if (left != 0) {
            store_release(underruns_, underruns_ + 1u);
            store_release(bytes_skipped_, bytes_skipped_ + left);
        }
        store_release(queue_tail_, end);
        store_release(ends_tail_, static_cast<uint8_t>(ends_tail_ + 1u));
    } else {
        store_release(underruns_, underruns_ + 1u);
        store_release(bytes_skipped_, bytes_skipped_ + static_cast<uint16_t>(head - tail));
        store_release(queue_tail_, head);
        store_release(skipping_, true);
    }
    if (fed_ > 0) store_release(transmissions_, transmissions_ + 1u);
    store_release(bytes_sent_, bytes_sent_ + fed_);
    store_release(on_air_sent_, 0);
    store_release(on_air_size_, 0);
    store_release(done_, started_);
    if (wake_ != nullptr) wake_(ModemWake::control, context_);
}

// skipping_ clears only after the frame has left the queue: the control side, seeing it clear, sees the frame gone.
void ModemTransmitter::skip() {
    const uint16_t head = load_acquire(queue_head_);
    const uint8_t ends_head = load_acquire(ends_head_);
    const uint16_t tail = queue_tail_;
    if (ends_head == ends_tail_) {
        store_release(bytes_skipped_, bytes_skipped_ + static_cast<uint16_t>(head - tail));
        store_release(queue_tail_, head);
        return;
    }
    const uint16_t end = frame_ends_[ends_tail_ & k_slot_mask];
    store_release(bytes_skipped_, bytes_skipped_ + static_cast<uint16_t>(end - tail));
    store_release(queue_tail_, end);
    store_release(ends_tail_, static_cast<uint8_t>(ends_tail_ + 1u));
    store_release(skipping_, false);
    if (wake_ != nullptr) wake_(ModemWake::control, context_);  // the next frame may be ready
}

// For display: the frame's windows sent so far (the bytes given to the encoder less those still in its queue, the one
// on the air included) and its size once its end is known.
void ModemTransmitter::publish_progress() {
    store_release(on_air_sent_, static_cast<uint32_t>(fed_ - encoder_.queued()));
    if (on_air_size_ != 0) return;
    const uint8_t ends_head = load_acquire(ends_head_);
    if (ends_head != ends_tail_)
        store_release(on_air_size_, static_cast<uint16_t>(frame_ends_[ends_tail_ & k_slot_mask] - frame_start_));
}

// The computer was refused: wake it once a quarter of the queue and a frame slot are free again.
void ModemTransmitter::offer_room() {
    const uint8_t refusals = load_acquire(refusals_);
    if (refusals == refusals_seen_) return;
    const uint16_t head = load_acquire(queue_head_);
    const uint8_t ends_head = load_acquire(ends_head_);
    const uint16_t used = static_cast<uint16_t>(head - queue_tail_);
    const uint8_t frames = static_cast<uint8_t>(ends_head - ends_tail_);
    if (k_queue_size - used < k_queue_size / k_resume_divisor || frames >= k_frame_slots) return;
    refusals_seen_ = refusals;
    if (wake_ != nullptr) wake_(ModemWake::host, context_);
}

// ---------------------------------------------------------------------------
// Control side
// ---------------------------------------------------------------------------

// DCD, or a change of it since the last tick, restarts the quiet time; with a frame ready and the channel checked,
// PTT is keyed and the audio side starts; when its audio has ended and the output latency has passed, PTT is released
// and the next frame goes through the same check.
void ModemTransmitter::tick(uint32_t now_ms) {
    timer_set_ = false;
    if (!valid_) return;
    if (!clock_started_) {
        clock_started_ = true;
        quiet_since_ms_ = now_ms;  // what was on the air before the start is not known: dwait from the start
        spaced_until_ms_ = now_ms;
    }
    const uint8_t changes = load_acquire(dcd_changes_);
    const bool busy = load_acquire(dcd_);  // after the count: a change counted is a change seen
    if (busy || changes != dcd_changes_seen_) {
        dcd_changes_seen_ = changes;
        quiet_since_ms_ = now_ms;
        draw_waiting_ = false;  // a channel that clears again starts with a draw
    }
    for (;;) {
        switch (state_) {
            case ChannelState::idle:
                if (!frame_ready()) return;
                store_release(state_, ChannelState::waiting);
                break;
            case ChannelState::waiting:
                if (may_key(now_ms, busy)) key();
                return;
            case ChannelState::keyed:
                // The audio side wakes the control side when the transmission ends.
                if (load_acquire(done_) != go_) return;
                store_release(state_, ChannelState::releasing);
                release_at_ms_ = now_ms + access_.output_latency_ms;
                spaced_until_ms_ = now_ms + spacing_ms_;
                break;
            case ChannelState::releasing:
                if (before(now_ms, release_at_ms_)) {
                    set_timer(release_at_ms_);
                    return;
                }
                if (ptt_ != nullptr) ptt_(false, context_);
                store_release(keyed_, false);  // after PTT: the receiver hears again once the transmitter is off
                store_release(state_, ChannelState::idle);
                break;
        }
    }
}

uint32_t ModemTransmitter::next_tick_ms() const {
    return timer_set_ ? timer_ms_ : k_no_tick;
}

// skipping_ first: once it reads clear, the skipped frame has left the queue (skip()), and while this side is idle the
// audio side moves no tail.
bool ModemTransmitter::frame_ready() const {
    if (load_acquire(skipping_)) return false;
    const uint8_t ends_head = load_acquire(ends_head_);
    const uint8_t ends_tail = load_acquire(ends_tail_);
    if (ends_head != ends_tail) return true;
    const uint16_t head = load_acquire(queue_head_);
    const uint16_t tail = load_acquire(queue_tail_);
    return static_cast<uint16_t>(head - tail) >= k_queue_size;
}

// The channel check (spec 12.1): DCD off, dwait since the channel went quiet, then p-persistence once per slot time.
// Full duplex keys at once. Either way this modem's previous transmission has had its silence (spacing_ms()).
bool ModemTransmitter::may_key(uint32_t now_ms, bool busy) {
    if (before(now_ms, spaced_until_ms_)) {
        set_timer(spaced_until_ms_);
        return false;
    }
    if (access_.full_duplex) return true;
    if (busy) return false;  // DCD going off wakes the control side
    const uint32_t clear_at = quiet_since_ms_ + access_.dwait_ms;
    if (before(now_ms, clear_at)) {
        set_timer(clear_at);
        return false;
    }
    if (draw_waiting_ && before(now_ms, draw_at_ms_)) {
        set_timer(draw_at_ms_);
        return false;
    }
    store_release(draws_, draws_ + 1u);
    if (draw() <= access_.persist) return true;
    store_release(deferrals_, deferrals_ + 1u);
    draw_waiting_ = true;
    draw_at_ms_ = now_ms + access_.slot_time_ms;
    set_timer(draw_at_ms_);
    return false;
}

// The receiver goes quiet first (half duplex), then PTT, then the audio side starts the frame.
void ModemTransmitter::key() {
    store_release(keyed_, true);
    if (ptt_ != nullptr) ptt_(true, context_);
    store_release(state_, ChannelState::keyed);
    draw_waiting_ = false;
    store_release(go_, static_cast<uint8_t>(go_ + 1u));
}

void ModemTransmitter::set_timer(uint32_t at_ms) {
    timer_set_ = true;
    timer_ms_ = at_ms == k_no_tick ? at_ms + 1u : at_ms;  // k_no_tick means none: a deadline there moves 1 ms on
}

uint8_t ModemTransmitter::draw() {
    random_ ^= random_ << k_xorshift_a;
    random_ ^= random_ >> k_xorshift_b;
    random_ ^= random_ << k_xorshift_c;
    return static_cast<uint8_t>(random_ >> k_draw_shift);
}

// ---------------------------------------------------------------------------
// Receive side and snapshots
// ---------------------------------------------------------------------------

void ModemTransmitter::set_dcd(bool busy) {
    if (busy == dcd_) return;
    store_release(dcd_, busy);
    store_release(dcd_changes_, static_cast<uint8_t>(dcd_changes_ + 1u));
    if (wake_ != nullptr) wake_(ModemWake::control, context_);
}

bool ModemTransmitter::valid() const {
    return valid_;
}

bool ModemTransmitter::dcd() const {
    return load_acquire(dcd_);
}

bool ModemTransmitter::transmitting() const {
    return load_acquire(keyed_);
}

ChannelState ModemTransmitter::channel_state() const {
    return load_acquire(state_);
}

ModemCounters ModemTransmitter::counters() const {
    ModemCounters counters = ModemCounters();
    counters.kiss = snapshot(kiss_counters_);
    counters.host_refusals = load_acquire(refusals_total_);
    const uint16_t head = load_acquire(queue_head_);
    const uint16_t tail = load_acquire(queue_tail_);
    const uint8_t ends_head = load_acquire(ends_head_);
    const uint8_t ends_tail = load_acquire(ends_tail_);
    counters.queued_bytes = static_cast<uint16_t>(head - tail);
    counters.queued_frames = static_cast<uint8_t>(ends_head - ends_tail);
    counters.transmissions = load_acquire(transmissions_);
    counters.bytes_sent = load_acquire(bytes_sent_);
    counters.underruns = load_acquire(underruns_);
    counters.bytes_skipped = load_acquire(bytes_skipped_);
    counters.draws = load_acquire(draws_);
    counters.deferrals = load_acquire(deferrals_);
    counters.on_air_sent = load_acquire(on_air_sent_);
    counters.on_air_size = load_acquire(on_air_size_);
    return counters;
}

const EncoderConfig& ModemTransmitter::signal() const {
    return signal_;
}

const AccessConfig& ModemTransmitter::access() const {
    return access_;
}

}  // namespace unlimited
