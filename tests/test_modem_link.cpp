// Two KISS modem cores through the channel simulator (spec 8, 12.8): KISS frames written into station A come out of
// station B byte for byte, one frame per transmission, never merged with the next nor split in two.
#include "modem_link.hpp"
#include "support/loopback.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using unlimited::ModemConfig;
using unlimited::pc::LinkConfig;
using unlimited::pc::LinkKey;
using unlimited::pc::LinkReception;
using unlimited::pc::ModemLink;

namespace sim = unlimited::sim;

namespace {

using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint8_t;

typedef std::vector<uint8_t> Bytes;

const size_t k_a = 0;
const size_t k_b = 1;
const uint32_t k_max_run_ms = 600000;
const double k_good_snr_db = 20.0;
const double k_gate_margin_db = 3.0;
const double k_offset_hz = 40.0;  // A -> B heard 40 Hz high, B -> A 40 Hz low (mistuned radios)
const uint32_t k_seed = 2026;
// The lengths of the frames sent: a single byte, short and AX.25-sized.
const size_t k_lengths[] = {1, 2, 5, 8, 12, 16, 20, 24};
const size_t k_rounds = 4;  // of the lengths above, per speed and SNR

struct Speed {
    uint16_t centi;     // bytes/s x 100
    double gate_db;     // spec 4: the gate SNR of the speed
};

const Speed k_speeds[] = {{600, 1.3}, {1200, 4.3}, {2500, 8.0}};
// A lock comes a little after its first window: half a slot and a few blocks (spec 3.3, V20); a margin for them.
const uint32_t k_lock_margin_ms = 60;

Bytes frame_bytes(size_t length, std::mt19937& generator) {
    Bytes bytes(length);
    for (size_t i = 0; i < length; ++i) bytes[i] = static_cast<uint8_t>(generator());
    if (length > 2) {
        bytes[length / 2] = unlimited::k_kiss_fend;  // KISS's special bytes in every frame long enough
        bytes[length / 2 + 1] = unlimited::k_kiss_fesc;
    }
    return bytes;
}

ModemConfig station(uint16_t centi) {
    ModemConfig config;
    config.signal.slot_us = unlimited::slot_us_for_centi_speed(centi);
    if (!unlimited::passband_fit(config.signal).fits) {
        config.signal.passband.low_hz = unlimited::k_am_passband_low_hz;
        config.signal.passband.high_hz = unlimited::k_am_passband_high_hz;
    }
    config.receiver.slot_us = config.signal.slot_us;
    config.receiver.passband = config.signal.passband;
    config.min_frame_bytes = 0;  // these tests send frames of 1 byte and up: every reception streams (V23 off)
    return config;
}

LinkConfig link_config(uint16_t centi, double snr_db) {
    LinkConfig config;
    config.stations[k_a] = station(centi);
    config.stations[k_b] = station(centi);
    config.stations[k_a].access.seed = k_seed;
    config.stations[k_b].access.seed = k_seed + 1;
    config.noisy = true;
    config.channel.mode = sim::Mode::usb;
    config.channel.snr_db = snr_db;
    config.channel.freq_offset_hz = k_offset_hz;
    config.channel.rx_low_hz = config.stations[k_a].signal.passband.low_hz;
    config.channel.rx_high_hz = config.stations[k_a].signal.passband.high_hz;
    config.channel.seed = k_seed;
    return config;
}

int popcount(unsigned value) {
    int count = 0;
    for (; value != 0; value &= value - 1) ++count;
    return count;
}

// What one station heard of the other's transmissions: each reception belongs to the last transmission keyed before
// its lock; a transmission with two receptions was split, a reception with a byte beyond its frame ran into the next
// transmission (merged).
struct Tally {
    size_t frames = 0;
    size_t transmissions = 0;
    size_t found = 0;        // transmissions with a reception
    size_t split = 0;
    size_t merged = 0;
    size_t bytes = 0;        // bytes of the frames found
    size_t received = 0;
    size_t dropped = 0;      // windows dropped as framing errors
    size_t bit_errors = 0;
    size_t strays = 0;         // receptions that match no transmission's frame
    size_t kiss_mismatch = 0;  // the computer's KISS frames differ from the receptions' bytes
};

// The START of byte 0 comes the lead-in (and a VOX lead with its gap) after the key.
uint32_t start_offset_ms(const unlimited::EncoderConfig& signal) {
    const double slot_ms = signal.slot_us / 1000.0;
    return signal.lead_in_ms + static_cast<uint32_t>(unlimited::loopback::vox_slots(signal) * slot_ms);
}

Tally tally(const ModemLink& link, size_t sender, const std::vector<Bytes>& sent) {
    Tally t;
    const size_t receiver = 1 - sender;
    const std::vector<LinkKey>& keys = link.keys(sender);
    const std::vector<LinkReception>& receptions = link.receptions(receiver);
    const uint32_t start_ms = start_offset_ms(link.modem(sender).signal());
    const uint32_t lookahead_ms = link.modem(receiver).lookahead_samples() * 1000u / unlimited::k_modem_rate_hz;
    // A reception's START was heard its first window and a margin before the lock, never after it (spec 3.3, V20: the
    // lock comes with the first window): among the transmissions that started then, the one whose frame its bytes match.
    const uint32_t window_ms = unlimited::k_window_slots * link.modem(receiver).receiver().slot_us / 1000u;
    const uint32_t reach_ms = lookahead_ms + 2u * window_ms + k_lock_margin_ms;
    t.frames = sent.size();
    t.transmissions = keys.size();
    std::vector<size_t> per_key(keys.size(), 0);
    for (size_t r = 0; r < receptions.size(); ++r) {
        const LinkReception& reception = receptions[r];
        size_t key = keys.size();
        size_t best = 0;
        for (size_t k = 0; k < keys.size() && k < sent.size(); ++k) {
            const uint32_t start = keys[k].on_ms + start_ms;
            if (start + lookahead_ms > reception.locked_ms || start + reach_ms < reception.locked_ms) continue;
            size_t matches = 0;
            for (size_t i = 0; i < reception.bytes.size(); ++i)
                matches += reception.indexes[i] < sent[k].size() && sent[k][reception.indexes[i]] == reception.bytes[i];
            if (matches > best) {
                best = matches;
                key = k;
            }
        }
        if (key == keys.size()) {
            ++t.strays;
            continue;
        }
        if (++per_key[key] > 1) {
            ++t.split;
            continue;
        }
        const Bytes& frame = sent[key];
        t.bytes += frame.size();
        t.dropped += reception.dropped;
        for (size_t i = 0; i < reception.bytes.size(); ++i) {
            const uint32_t index = reception.indexes[i];
            if (index >= frame.size()) {
                ++t.merged;
                break;
            }
            ++t.received;
            t.bit_errors += static_cast<size_t>(popcount(static_cast<unsigned>(frame[index] ^ reception.bytes[i])));
        }
    }
    for (size_t k = 0; k < per_key.size(); ++k) t.found += per_key[k] > 0 ? 1 : 0;
    // Each non-empty reception is one KISS frame to the computer, with the same bytes.
    std::vector<Bytes> expected;
    for (size_t r = 0; r < receptions.size(); ++r)
        if (!receptions[r].bytes.empty()) expected.push_back(receptions[r].bytes);
    const std::vector<Bytes>& kiss = link.frames(receiver);
    if (kiss.size() != expected.size()) {
        t.kiss_mismatch = std::max(kiss.size(), expected.size());
    } else {
        for (size_t i = 0; i < kiss.size(); ++i) t.kiss_mismatch += kiss[i] != expected[i] ? 1 : 0;
    }
    return t;
}

}  // namespace

// Spec 8: at 6, 12 and 25 bytes/s, at 20 dB and at the gate + 3 dB (key-down SNR in 2500 Hz, mistuned 40 Hz): one
// transmission per frame; at 20 dB every frame byte for byte; at the gate + 3 dB the bit errors are reported and no
// frame is ever merged with the next or split in two, nor a reception that belongs to no frame. (Until V20 the check
// before lock, 8 windows at 25 bytes/s, locked short frames after the next frame's START: 3 of 32 frames split.)
TEST(modem_link_frames_come_out_byte_for_byte) {
    for (size_t s = 0; s < test::count_of(k_speeds); ++s) {
        const double snrs[] = {k_good_snr_db, k_speeds[s].gate_db + k_gate_margin_db};
        for (size_t n = 0; n < test::count_of(snrs); ++n) {
            ModemLink link(link_config(k_speeds[s].centi, snrs[n]));
            std::mt19937 generator(k_seed + static_cast<uint32_t>(s * 10 + n));
            std::vector<Bytes> sent;
            for (size_t round = 0; round < k_rounds; ++round) {
                for (size_t i = 0; i < test::count_of(k_lengths); ++i) {
                    sent.push_back(frame_bytes(k_lengths[i], generator));
                    link.send(k_a, sent.back());
                }
            }
            CHECK(link.run_until_idle(k_max_run_ms));
            const Tally t = tally(link, k_a, sent);
            CHECK_EQ(t.transmissions, sent.size());
            CHECK_EQ(t.split, 0u);
            CHECK_EQ(t.merged, 0u);
            CHECK_EQ(t.strays, 0u);
            CHECK_EQ(t.kiss_mismatch, 0u);
            CHECK_EQ(link.keys(k_b).size(), 0u);
            CHECK_EQ(link.modem(k_a).counters().transmissions, static_cast<uint32_t>(sent.size()));
            if (n == 0) {
                CHECK(link.frames(k_b) == sent);
                CHECK_EQ(t.bit_errors, 0u);
                CHECK_EQ(t.dropped, 0u);
            } else {
                CHECK(t.found * 2 >= sent.size());
            }
            const double bits = 8.0 * static_cast<double>(t.received);
            NOTE("%.2f bytes/s at %4.1f dB: %zu frames, %zu transmissions, %zu found, %zu/%zu bytes, %zu windows "
                 "dropped, %zu bit errors (BER %.1e), split %zu, merged %zu, strays %zu, %.0f s simulated",
                 k_speeds[s].centi / 100.0, snrs[n], t.frames, t.transmissions, t.found, t.received, t.bytes,
                 t.dropped, t.bit_errors, bits > 0 ? t.bit_errors / bits : 0.0, t.split, t.merged, t.strays,
                 link.now_ms() / 1000.0);
        }
    }
}

namespace {

// B answers each frame with "ACK " and the frame; A sends the next frame on each answer, `rounds` in all.
struct Conversation {
    std::vector<Bytes> from_a;
    std::vector<Bytes> from_b;
    size_t rounds;
};

void converse(ModemLink& link, size_t station, const Bytes& frame, void* context) {
    Conversation& talk = *static_cast<Conversation*>(context);
    if (station == k_b) {
        Bytes answer;
        const char prefix[] = "ACK ";
        answer.insert(answer.end(), prefix, prefix + sizeof(prefix) - 1);
        answer.insert(answer.end(), frame.begin(), frame.end());
        talk.from_b.push_back(answer);
        link.send(k_b, answer);
    } else if (talk.from_a.size() < talk.rounds) {
        const std::string text = "frame " + std::to_string(talk.from_a.size() + 1) + " from A";
        talk.from_a.push_back(Bytes(text.begin(), text.end()));
        link.send(k_a, talk.from_a.back());
    }
}

}  // namespace

// A conversation, as AX.25 has one: each frame answered by the other station after its channel check (DCD off, dwait,
// p-persistence), half duplex both ways: every frame byte for byte, and never both on the air at once.
TEST(modem_link_conversation_both_ways) {
    ModemLink link(link_config(1200, k_good_snr_db));
    Conversation talk;
    talk.rounds = 3;
    link.set_frame_handler(&converse, &talk);
    talk.from_a.push_back(Bytes(1, 'Q'));
    link.send(k_a, talk.from_a.back());
    CHECK(link.run_until_idle(k_max_run_ms));
    CHECK_EQ(talk.from_a.size(), talk.rounds);
    CHECK(link.frames(k_b) == talk.from_a);
    CHECK(link.frames(k_a) == talk.from_b);
    const std::vector<LinkKey>& a = link.keys(k_a);
    const std::vector<LinkKey>& b = link.keys(k_b);
    CHECK_EQ(a.size(), talk.rounds);
    CHECK_EQ(b.size(), talk.rounds);
    size_t overlaps = 0;
    for (size_t i = 0; i < a.size(); ++i)
        for (size_t j = 0; j < b.size(); ++j)
            overlaps += a[i].on_ms < b[j].off_ms && b[j].on_ms < a[i].off_ms ? 1 : 0;
    CHECK_EQ(overlaps, 0u);
    // Each answer keyed at least dwait after the frame it answers left the air (and the receiver let go of it).
    for (size_t i = 0; i < b.size() && i < a.size(); ++i)
        CHECK(b[i].on_ms >= a[i].off_ms + unlimited::k_default_dwait_ms);
    NOTE("%zu frames each way in %.1f s; the first answer keyed %u ms after the question's PTT release",
         talk.rounds, link.now_ms() / 1000.0, b.empty() || a.empty() ? 0u : b[0].on_ms - a[0].off_ms);
}

// Frames that follow each other at once (every p-persistence draw wins) with a VOX lead, on a clean channel, short ones
// among them, at 6, 12 and 25 bytes/s: each arrives, because the modem leaves a receiver's end silence (a window and
// 2 slots) before the next lead tone, and every first window is decided alone (V20). Found while building: without the
// spacing (end_windows 0, reported) VOX leads came 100 ms after the previous STOP and frames were lost; with the check
// before lock (4 windows at 12 bytes/s, V18) 3 of these 8 frames arrived at 12 bytes/s.
TEST(modem_link_back_to_back_vox_frames) {
    const uint16_t speeds[] = {600, 1200, 2500};
    const size_t lengths[] = {1, 2, 1, 3, 5, 1, 2, 8};
    for (size_t v = 0; v < test::count_of(speeds); ++v) {
        const uint8_t spacings[] = {unlimited::k_default_end_windows, 0};
        for (size_t k = 0; k < test::count_of(spacings); ++k) {
            if (k > 0 && speeds[v] != 600) continue;  // the report without spacing, once
            LinkConfig config = link_config(speeds[v], k_good_snr_db);
            config.noisy = false;
            for (size_t s = 0; s < 2; ++s) {
                config.stations[s].signal.lead_in_ms = 0;
                config.stations[s].signal.vox_lead_ms = unlimited::k_default_vox_lead_ms;
                config.stations[s].access.persist = 255;
                config.stations[s].access.end_windows = spacings[k];
            }
            ModemLink link(config);
            std::mt19937 generator(k_seed + static_cast<uint32_t>(v));
            std::vector<Bytes> sent;
            for (size_t i = 0; i < test::count_of(lengths); ++i) {
                sent.push_back(frame_bytes(lengths[i], generator));
                link.send(k_a, sent.back());
            }
            CHECK(link.run_until_idle(k_max_run_ms));
            CHECK_EQ(link.keys(k_a).size(), sent.size());
            if (k == 0) CHECK(link.frames(k_b) == sent);
            NOTE("%.0f bytes/s, end windows %u: %zu of %zu frames arrived (every draw wins, frames of 1..8 bytes)",
                 speeds[v] / 100.0, spacings[k], link.frames(k_b).size(), sent.size());
        }
    }
}

// VOX both ways: the lead tone and its gap before each START; frames byte for byte.
TEST(modem_link_vox_lead) {
    LinkConfig config = link_config(600, k_good_snr_db);
    for (size_t s = 0; s < 2; ++s) {
        config.stations[s].signal.lead_in_ms = 0;
        config.stations[s].signal.vox_lead_ms = unlimited::k_default_vox_lead_ms;
    }
    ModemLink link(config);
    std::mt19937 generator(k_seed);
    std::vector<Bytes> sent;
    for (size_t i = 0; i < 3; ++i) {
        sent.push_back(frame_bytes(6 + i, generator));
        link.send(k_a, sent.back());
    }
    CHECK(link.run_until_idle(k_max_run_ms));
    CHECK(link.frames(k_b) == sent);
    CHECK_EQ(link.keys(k_a).size(), sent.size());
}
