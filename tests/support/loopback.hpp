#pragma once

#include "channel.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// Shared test helpers: encode bytes to 8 kHz int16, optionally through the channel simulator, run a Decoder, collect
// its events and score them against what was sent. v1.0 layout (spec 2.1): lead-in, the optional VOX lead and gap, one
// window of 10 slots per byte (START, 8 data slots MSB first, STOP), the tail.
namespace unlimited {
namespace loopback {

// One transmission placed in a longer recording.
struct Transmission {
    EncoderConfig config;
    std::vector<std::uint8_t> data;
    std::size_t start_sample;  // first sample of the transmission in the recording
    std::size_t length;        // samples
};

struct Recording {
    std::vector<std::int16_t> samples;
    std::vector<Transmission> transmissions;
};

std::vector<std::uint8_t> random_bytes(std::size_t count, std::uint32_t seed);

// Renders a whole transmission; the encoder queue is refilled while it renders.
std::vector<std::int16_t> encode(const std::vector<std::uint8_t>& data, const EncoderConfig& config);

// 8000 Hz, B bytes per second, the pitch; the passband widened to 100..3000 Hz when the band needs it.
EncoderConfig speed_config(float bytes_per_second, std::uint16_t tone_hz = k_default_tone_hz);
// The receiver of that sender: the same slot, the sender's passband.
DecoderConfig receiver_for(const EncoderConfig& config);

void append_silence(Recording& recording, double ms, std::uint32_t rate_hz = k_decoder_rate_hz);
void append_transmission(Recording& recording, const std::vector<std::uint8_t>& data, const EncoderConfig& config);
// The silence a recording starts with before its first transmission: at least `ms` and 15 slots. The decoder takes
// nothing before its first sample for silence (V6): it needs a whole silent window before its first START.
double leading_silence_ms(const EncoderConfig& config, double ms = 500.0);
// Silence around one transmission: leading_silence_ms() before it, at least 3 windows after it (the end comes 2 silent
// windows after the last STOP, plus the look-ahead).
Recording single(const std::vector<std::uint8_t>& data, const EncoderConfig& config, double silence_ms = 500.0);

// Slot layout of a transmission, in samples from the recording's start.
double slot_samples(const EncoderConfig& config);
std::size_t vox_slots(const EncoderConfig& config);  // VOX lead slots and the gap, 0 without a VOX lead
double first_start_sample(const Transmission& transmission);  // the START of byte 0
double slot_start_sample(const Transmission& transmission, std::size_t byte, std::size_t slot);
double end_sample(const Transmission& transmission);          // end of the last STOP

// Scales one slot of a transmission in place, e.g. 0 to erase a START.
void scale_slot(Recording& recording, std::size_t transmission, std::size_t byte, std::size_t slot, double gain);

// int16 -> float -> sim::Channel -> int16. signal_level comes from the encoder amplitude; the output is scaled down
// when needed so that noise peaks do not clip.
std::vector<std::int16_t> through_channel(const std::vector<std::int16_t>& samples, sim::ChannelConfig config,
                                          std::int16_t amplitude);
// The channel's filter delay (samples, energy centroid of a 1500 Hz beep): event times minus it are the decoder's own.
double channel_delay_samples(sim::Mode mode = sim::Mode::usb);
// The usb channel with AWGN at snr_db (key-down in 2500 Hz) and a tuning offset.
std::vector<std::int16_t> usb(const std::vector<std::int16_t>& samples, double snr_db, std::uint32_t seed,
                              std::int16_t amplitude, double offset_hz = 0.0);

struct Capture {
    std::vector<Event> events;
    std::vector<std::size_t> event_sample;  // input samples consumed when the event arrived
    std::size_t lookahead = 0;              // the decoder's look-ahead, samples
};

// chunk 0: one sample at a time (exact event positions); otherwise process() in chunks of that size.
Capture run_decoder(const std::vector<std::int16_t>& samples, const DecoderConfig& config, std::size_t chunk = 0);

struct Score {
    std::size_t bytes_sent = 0;
    std::size_t bytes_released = 0;
    std::size_t matched = 0;       // released bytes mapped to a sent byte
    std::size_t wrong_bytes = 0;
    std::size_t bit_errors = 0;
    std::size_t lost_bytes = 0;    // sent bytes never released
    std::size_t extra_bytes = 0;   // released bytes that map to nothing (or a duplicate)
    std::size_t locks = 0;
    std::size_t lost_events = 0;
    std::size_t ends = 0;
    std::size_t framing_windows = 0;  // windows dropped as framing errors (slot events flagged)
    std::size_t locked_transmissions = 0;  // transmissions with a lock whose first byte was byte 0
    double ber() const;
    double loss() const;
};

// Maps every byte event to a sent byte: the byte events between a locked and its end or lost belong to the latest
// transmission whose first START lies between 9.5 slots and 10 windows before the lock (the look-ahead and the channel
// delay removed; the lock comes once the first window is in: spec 3.3, V20), at their byte_index.
struct Mapping {
    Score score;
    std::vector<std::vector<int> > received;        // per transmission, per byte: value or -1
    std::vector<std::vector<Event> > byte_events;   // per transmission, per byte: the event (if received)
};

Mapping map_events(const Recording& recording, const Capture& capture, double delay_samples = 0.0);
Score score(const Recording& recording, const Capture& capture, double delay_samples = 0.0);

std::size_t count_events(const Capture& capture, EventType type);

}  // namespace loopback
}  // namespace unlimited
