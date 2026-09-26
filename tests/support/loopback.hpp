#pragma once

#include "channel.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// Shared test helpers: encode bytes to 8 kHz int16, optionally through the channel simulator, run a
// Decoder, collect its events and score them against what was sent. v0.2 layout (spec 2.1): tune, sync, an
// 8-peak header, frames of N peaks + STOP (the last one possibly short), EOT.
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
// Any mode at 8000 Hz. tone_hz 0 centres the band on 1500 Hz with the grid on `side` (as the HF presets).
EncoderConfig mode_config(std::uint32_t slot_ms, std::uint8_t bits_per_peak, std::uint8_t data_slots = 8,
                          Spacing spacing = Spacing::standard, GridSide side = GridSide::below,
                          std::uint16_t tone_hz = 0);
// A valid mode at T = slot_ms (whole ms) with f_ref = tone_hz: bits per peak as the presets use at that T,
// fewer when the band does not fit; the grid on the side of 1500 Hz.
EncoderConfig slot_config(double slot_ms, std::uint16_t tone_hz);

void append_silence(Recording& recording, double ms, std::uint32_t rate_hz = k_decoder_rate_hz);
void append_transmission(Recording& recording, const std::vector<std::uint8_t>& data, const EncoderConfig& config);
// Trailing silence long enough for the decoder's END at any T.
Recording single(const std::vector<std::uint8_t>& data, const EncoderConfig& config, double silence_ms = 200.0);

// Slot layout of a transmission, in samples from its first sample (slot 0 = first tune slot).
double slot_samples(const EncoderConfig& config);
double slot_start_sample(const Transmission& transmission, std::size_t slot);
std::size_t header_start_slot(const EncoderConfig& config);   // the last sync marker, START of the header
std::size_t first_frame_slot(const EncoderConfig& config);    // START of data frame 0 (the header STOP)
std::size_t frame_count(const Transmission& transmission);
std::size_t frame_peaks(const Transmission& transmission, std::size_t frame);  // N, or d for a short last frame
std::size_t frame_start_slot(const Transmission& transmission, std::size_t frame);
std::size_t frame_of_byte(const Transmission& transmission, std::size_t byte_index);
double stop_centre_sample(const Transmission& transmission, std::size_t frame);

// Scales one slot (index as in slot_start_sample) of a transmission in place, e.g. 0 to erase a marker.
void scale_slot(Recording& recording, std::size_t transmission, std::size_t slot, double gain);

// int16 -> float -> sim::Channel -> int16. signal_level comes from the encoder amplitude; the output is
// scaled down when needed so that noise peaks do not clip.
std::vector<std::int16_t> through_channel(const std::vector<std::int16_t>& samples, sim::ChannelConfig config,
                                          std::int16_t amplitude);
// Delay of the channel (no noise, fading or impulses) in samples, from the envelope of a transmission.
long channel_delay(const std::vector<std::int16_t>& samples, sim::ChannelConfig config, std::int16_t amplitude);

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
    std::size_t frames_sent = 0;
    std::size_t frames_delivered = 0;  // every byte of the frame released
    std::size_t locks = 0;
    std::size_t late_joins = 0;
    std::size_t lost_events = 0;
    std::size_t alias_losts = 0;
    std::size_t ends = 0;
    std::size_t flywheel_bytes = 0;
    double ber() const;
    double loss() const;
};

// Maps every byte event to a sent byte: the events of one lock share one frame offset, found from the release
// times (a frame is released at least 2 T after its STOP, held frames later); byte = frame * B + index.
struct Mapping {
    Score score;
    std::vector<std::vector<int> > received;  // per transmission, per byte: value or -1
    std::vector<std::vector<Event> > byte_events;  // per transmission, per byte: the event (if received)
};

Mapping map_events(const Recording& recording, const Capture& capture);
Score score(const Recording& recording, const Capture& capture);

std::size_t count_events(const Capture& capture, EventType type);

// Genie slot decoder (A5): the transmission's peaks decided on the known grid (exact slot positions shifted by
// `delay` samples, f_ref as received, grid side as received) with the decoder's own slot bank and background.
// Returns the data bytes (short last frame included).
std::vector<std::uint8_t> genie_bytes(const std::vector<std::int16_t>& received, const Transmission& transmission,
                                      double tone_hz, int side, long delay);

}  // namespace loopback
}  // namespace unlimited
