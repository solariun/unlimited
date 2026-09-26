#pragma once

#include "channel.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// Shared test helpers: encode bytes to 8 kHz int16, optionally through the channel simulator, run a Decoder, collect
// its events and score them against what was sent. v0.3 layout (spec 2.1): tune, sync (its last marker the first
// START), packages of N bits each closed by a STOP (the last one possibly short), END.
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

EncoderConfig preset_config(Preset preset);  // 8000 Hz
// Preset::hf with T = slot_ms, N = bits and the pitch; the passband widened to 100..3000 Hz when the band needs it.
EncoderConfig slot_config(double slot_ms, std::uint8_t bits, std::uint16_t tone_hz = k_default_tone_hz);

void append_silence(Recording& recording, double ms, std::uint32_t rate_hz = k_decoder_rate_hz);
void append_transmission(Recording& recording, const std::vector<std::uint8_t>& data, const EncoderConfig& config);
// Silence around one transmission, at least 4 T after it.
Recording single(const std::vector<std::uint8_t>& data, const EncoderConfig& config, double silence_ms = 200.0);

// Slot layout of a transmission, in samples from the recording's start (slot 0 = the first tune slot).
double slot_samples(const EncoderConfig& config);
double slot_start_sample(const Transmission& transmission, std::size_t slot);
std::size_t tune_slots(const EncoderConfig& config);
std::size_t first_start_slot(const EncoderConfig& config);  // the last sync marker, START of package 0
std::size_t package_count(const Transmission& transmission);
std::size_t package_bits(const Transmission& transmission, std::size_t package);  // N, or d for a short last one
std::size_t package_start_slot(const Transmission& transmission, std::size_t package);
std::size_t stop_slot(const Transmission& transmission, std::size_t package);
std::size_t end_slot(const Transmission& transmission);  // the first END marker

// Scales one slot (index as in slot_start_sample) of a transmission in place, e.g. 0 to erase a marker.
void scale_slot(Recording& recording, std::size_t transmission, std::size_t slot, double gain);

// int16 -> float -> sim::Channel -> int16. signal_level comes from the encoder amplitude; the output is scaled down
// when needed so that noise peaks do not clip.
std::vector<std::int16_t> through_channel(const std::vector<std::int16_t>& samples, sim::ChannelConfig config,
                                          std::int16_t amplitude);
// The usb channel with AWGN at snr_db (key-down in 2500 Hz) and a tuning offset.
// The channel's filter delay (samples, energy centroid of a 1500 Hz beep): event times minus it are the decoder's own.
double channel_delay_samples(sim::Mode mode = sim::Mode::usb);
std::vector<std::int16_t> usb(const std::vector<std::int16_t>& samples, double snr_db, std::uint32_t seed,
                              std::int16_t amplitude, double offset_hz = 0.0);

struct Capture {
    std::vector<Event> events;
    std::vector<std::size_t> event_sample;  // input samples consumed when the event arrived
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
    std::size_t late_joins = 0;
    std::size_t lost_events = 0;
    std::size_t alias_losts = 0;
    std::size_t ends = 0;
    std::size_t flywheel_bytes = 0;
    std::size_t shifted_segments = 0;  // late-join segments whose byte_index needed an offset (cold joins)
    double ber() const;
    double loss() const;
};

// Maps every byte event to a sent byte: the byte events between a locked and its end or lost belong to the
// transmission running when they came, at their byte_index. A late join whose byte_index starts at the join (a cold
// join) is placed at the offset where most of its bytes match.
struct Mapping {
    Score score;
    std::vector<std::vector<int> > received;        // per transmission, per byte: value or -1
    std::vector<std::vector<Event> > byte_events;   // per transmission, per byte: the event (if received)
    std::vector<long> offsets;                      // per lock: the byte offset applied
};

Mapping map_events(const Recording& recording, const Capture& capture);
Score score(const Recording& recording, const Capture& capture);

std::size_t count_events(const Capture& capture, EventType type);

}  // namespace loopback
}  // namespace unlimited
