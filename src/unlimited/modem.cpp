#include "unlimited/modem.hpp"

namespace unlimited {

const uint8_t Modem::k_host_chunk;

static_assert(k_modem_rate_hz == k_decoder_rate_hz, "the modem's audio rate is the decoder's");

namespace {

// What an invalid configuration runs with: nothing (ModemTransmitter and Decoder stay inert).
EncoderConfig inert_signal() {
    EncoderConfig signal;
    signal.sample_rate_hz = 0;
    return signal;
}

// The fade bridge's receivers end a transmission after 2 silent windows: the modem spaces its transmissions so.
AccessConfig spaced(const ModemConfig& config) {
    AccessConfig access = config.access;
    if (config.fade_bridge && access.end_windows < k_fade_bridge_end_windows) access.end_windows = k_fade_bridge_end_windows;
    return access;
}

DecoderConfig inert_receiver() {
    DecoderConfig receiver;
    receiver.slot_us = 0;
    return receiver;
}

}  // namespace

ModemConfig::ModemConfig()
    : signal(), receiver(), access(), fade_bridge(k_default_fade_bridge), min_frame_bytes(k_default_min_frame_bytes) {
    signal.sample_rate_hz = k_modem_rate_hz;
    signal.lead_in_ms = k_default_txdelay_ms;
    receiver.slot_us = signal.slot_us;
    receiver.passband = signal.passband;
    receiver.decision_mode = DecisionMode::adaptive;
}

bool ModemConfig::valid() const {
    return signal.sample_rate_hz == k_modem_rate_hz && sent().valid() && receiver.valid() &&
           receiver.slot_us == signal.slot_us && access.slot_time_ms > 0 && min_frame_bytes <= k_max_min_frame_bytes;
}

// A VOX lead and its gap already stand before the START; otherwise the lead-in is silence, at least 300 ms with the
// fade bridge.
EncoderConfig ModemConfig::sent() const {
    EncoderConfig sent_signal = signal;
    if (fade_bridge && sent_signal.vox_lead_ms == 0 && sent_signal.lead_in_ms < k_fade_bridge_silence_ms)
        sent_signal.lead_in_ms = k_fade_bridge_silence_ms;
    return sent_signal;
}

DecoderConfig ModemConfig::heard() const {
    DecoderConfig heard_receiver = receiver;
    heard_receiver.fade_bridge = fade_bridge;
    return heard_receiver;
}

Modem::Modem(const ModemConfig& config, HostHandler host, PttHandler ptt, void* context, WakeHandler wake)
    : config_(config),
      valid_(config.valid()),
      receiver_(valid_ ? config.heard() : inert_receiver()),
      host_(host),
      context_(context),
      tap_(nullptr),
      tap_context_(nullptr),
      transmitter_(valid_ ? config.sent() : inert_signal(), spaced(config), ptt, context, wake),
      decoder_(receiver_, &Modem::on_event, this),
      out_(),
      out_fill_(0),
      open_(false),
      holding_(false),
      held_(),
      held_count_(0),
      frames_received_(0),
      bytes_received_(0),
      ends_(0),
      losses_(0),
      short_frames_(0),
      short_bytes_(0),
      muted_samples_(0) {}

size_t Modem::host_input(const uint8_t* data, size_t size) {
    return transmitter_.host_input(data, size);
}

void Modem::audio_input(const int16_t* samples, size_t count) {
    if (!valid_) return;
    if (!config_.access.full_duplex && transmitter_.transmitting()) {
        for (size_t i = 0; i < count; ++i) decoder_.process_sample(0);
        store_release(muted_samples_, muted_samples_ + static_cast<uint32_t>(count));
    } else {
        decoder_.process(samples, count);
    }
    flush();
    transmitter_.set_dcd(decoder_.dcd());
}

void Modem::audio_output(int16_t* out, size_t count) {
    transmitter_.audio_output(out, count);
}

void Modem::tick(uint32_t now_ms) {
    transmitter_.tick(now_ms);
}

uint32_t Modem::next_tick_ms() const {
    return transmitter_.next_tick_ms();
}

void Modem::set_event_tap(EventHandler tap, void* context) {
    tap_ = tap;
    tap_context_ = context;
}

// Streaming to the computer (spec 12.1): C0 00 at `locked`, each byte escaped as it is decoded, C0 at `end` or `lost`;
// with a minimum frame (V23) the reception's first bytes wait until there are min_frame_bytes of them, and a reception
// that ends shorter sends nothing.
void Modem::on_event(const Event& event, void* context) {
    Modem& modem = *static_cast<Modem*>(context);
    switch (event.type) {
        case EventType::locked:
            if (modem.open_) {  // a new reception while one is open (not seen: end or lost comes first)
                modem.put(k_kiss_fend);
                modem.open_ = false;
            }
            modem.drop_held();
            if (modem.config_.min_frame_bytes == 0) {
                modem.open();
            } else {
                modem.holding_ = true;
            }
            break;
        case EventType::byte:
            if (modem.open_) {
                modem.put_escaped(event.value);
                store_release(modem.bytes_received_, modem.bytes_received_ + 1u);
            } else if (modem.holding_) {
                modem.held_[modem.held_count_++] = event.value;
                if (modem.held_count_ >= modem.config_.min_frame_bytes) modem.release_held();
            }
            break;
        case EventType::end:
        case EventType::lost:
            if (modem.open_) modem.close(event.type == EventType::lost);
            modem.drop_held();
            break;
        case EventType::state:
        case EventType::slot:
            break;
    }
    if (modem.tap_ != nullptr) modem.tap_(event, modem.tap_context_);
}

// C0 00: a frame opens at the computer.
void Modem::open() {
    put(k_kiss_fend);
    put(k_kiss_data);
    open_ = true;
    store_release(frames_received_, frames_received_ + 1u);
}

// C0: the frame closes (a reception's `end` or `lost`).
void Modem::close(bool lost) {
    put(k_kiss_fend);
    open_ = false;
    if (lost) {
        store_release(losses_, losses_ + 1u);
    } else {
        store_release(ends_, ends_ + 1u);
    }
}

void Modem::put_escaped(uint8_t value) {
    uint8_t escaped[k_kiss_escaped_max];
    const uint8_t size = kiss_escape(value, escaped);
    for (uint8_t i = 0; i < size; ++i) put(escaped[i]);
}

// The reception reached the minimum frame (V23): C0 00 and every byte held, in order; the rest streams.
void Modem::release_held() {
    holding_ = false;
    open();
    for (uint8_t i = 0; i < held_count_; ++i) put_escaped(held_[i]);
    store_release(bytes_received_, bytes_received_ + held_count_);
    held_count_ = 0;
}

// A reception that ended (or gave way to a new one) shorter than the minimum frame: nothing of it goes to the computer.
void Modem::drop_held() {
    if (!holding_) return;
    holding_ = false;
    store_release(short_frames_, short_frames_ + 1u);
    store_release(short_bytes_, short_bytes_ + held_count_);
    held_count_ = 0;
}

void Modem::put(uint8_t byte) {
    out_[out_fill_++] = byte;
    if (out_fill_ == k_host_chunk) flush();
}

void Modem::flush() {
    if (out_fill_ == 0) return;
    if (host_ != nullptr) host_(out_, out_fill_, context_);
    out_fill_ = 0;
}

bool Modem::valid() const {
    return valid_;
}

bool Modem::dcd() const {
    return transmitter_.dcd();
}

bool Modem::transmitting() const {
    return transmitter_.transmitting();
}

ChannelState Modem::channel_state() const {
    return transmitter_.channel_state();
}

ModemCounters Modem::counters() const {
    ModemCounters counters = transmitter_.counters();
    counters.frames_received = load_acquire(frames_received_);
    counters.bytes_received = load_acquire(bytes_received_);
    counters.ends = load_acquire(ends_);
    counters.losses = load_acquire(losses_);
    counters.short_frames = load_acquire(short_frames_);
    counters.short_bytes = load_acquire(short_bytes_);
    counters.muted_samples = load_acquire(muted_samples_);
    return counters;
}

uint16_t Modem::lookahead_samples() const {
    return decoder_.lookahead_samples();
}

const ModemConfig& Modem::config() const {
    return config_;
}

const EncoderConfig& Modem::signal() const {
    return transmitter_.signal();
}

const DecoderConfig& Modem::receiver() const {
    return receiver_;
}

}  // namespace unlimited
