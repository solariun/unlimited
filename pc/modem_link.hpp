#pragma once

#include "channel.hpp"
#include "unlimited/modem.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Two modem cores on one simulated radio channel, in memory and on a simulated clock (spec 8, 12.3): what station A's
// computer sends as KISS comes out of station B's computer, and back. The integration tests and
// `unlimited_modem --loopback` run it. Every 10 ms each station ticks, plays 10 ms of audio (on the air only while its
// PTT is keyed, or always with a VOX lead) and hears the other one's through sim::Channel (or as sent, when clean).
namespace unlimited {
namespace pc {

struct LinkConfig {
    ModemConfig stations[2];      // A and B
    bool noisy;                   // false: each hears the other's audio as sent; true: through sim::Channel
    sim::ChannelConfig channel;   // A -> B; B -> A has the negative offset and seed + 1
    LinkConfig();                 // two ModemConfig(), clean
};

// One reception at a station, from `locked` to `end` or `lost`, as its receiver's events told it.
struct LinkReception {
    std::uint32_t locked_ms;
    std::uint32_t closed_ms;  // 0 while open
    float tone_hz;
    float slot_ms;
    float snr_db;             // the last byte's
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint32_t> indexes;  // byte_index of each byte: its place in the transmission
    std::uint32_t dropped;               // windows dropped as framing errors
    bool closed;
    bool lost;
};

struct LinkKey {             // one PTT key of a station: one transmission
    std::uint32_t on_ms;
    std::uint32_t off_ms;    // 0 while keyed
};

class ModemLink {
public:
    static const std::uint32_t k_step_ms = 10;

    // A frame a station's computer received (KISS decoded): the handler may send() replies.
    typedef void (*FrameHandler)(ModemLink& link, std::size_t station, const std::vector<std::uint8_t>& frame,
                                 void* context);

    explicit ModemLink(const LinkConfig& config);  // throws std::invalid_argument for a channel the simulator refuses
    ~ModemLink();
    ModemLink(const ModemLink&) = delete;
    ModemLink& operator=(const ModemLink&) = delete;

    void send(std::size_t station, const std::vector<std::uint8_t>& data);  // one KISS data frame from its computer
    void run(std::uint32_t ms);
    // Until nothing is waiting or on the air and no reception is open, the receivers' end latency included; false
    // when max_ms passed first.
    bool run_until_idle(std::uint32_t max_ms);
    void set_frame_handler(FrameHandler handler, void* context);

    std::uint32_t now_ms() const;
    const std::vector<std::vector<std::uint8_t> >& frames(std::size_t station) const;  // what its computer received
    const std::vector<LinkReception>& receptions(std::size_t station) const;
    const std::vector<LinkKey>& keys(std::size_t station) const;
    const Modem& modem(std::size_t station) const;
    std::uint32_t settle_ms() const;  // how long after the last PTT release the receivers may still be busy

private:
    struct Station;

    void step();
    bool idle() const;

    LinkConfig config_;
    std::unique_ptr<Station> stations_[2];
    std::unique_ptr<sim::Channel> channels_[2];  // [0]: A -> B, [1]: B -> A
    std::uint32_t now_ms_;
    std::uint32_t last_release_ms_;
    FrameHandler handler_;
    void* handler_context_;
};

}  // namespace pc
}  // namespace unlimited
