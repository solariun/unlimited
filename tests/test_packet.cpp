#include "support/loopback.hpp"
#include "test_harness.hpp"
#include "unlimited/packet.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using unlimited::Event;
using unlimited::EventType;
using unlimited::PacketReader;
using unlimited::crc16_ccitt;
using unlimited::k_packet_header;
using unlimited::k_packet_max_payload;
using unlimited::k_packet_overhead;
using unlimited::packet_build;

namespace {

using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint8_t;

typedef std::vector<uint8_t> Bytes;

const uint8_t sync_0 = 0x2D;
const uint8_t sync_1 = 0xD4;
const size_t length_at = 2;          // LEN hi, LEN lo
const size_t host_max_payload = 1024;
const size_t ax25_payload = 330;     // an AX.25 frame as KISS carries it
const unsigned bits_per_byte = 8;
const uint8_t byte_mask = 0xFF;
const size_t reader_fields_max = 40;  // handler, context, counters and padding next to the buffer

static_assert(k_packet_header == 4 && k_packet_overhead == 6, "[2D D4][LEN hi][LEN lo] ... [CRC hi][CRC lo]");
static_assert(UNLIMITED_PACKET_MAX == host_max_payload && k_packet_max_payload == host_max_payload,
              "host default UNLIMITED_PACKET_MAX is 1024");

struct Received {
    Bytes payload;
    uint8_t flags;
};

struct Collector {
    std::vector<Received> packets;

    static void on_packet(const uint8_t* payload, uint16_t size, uint8_t flags, void* context) {
        Collector* self = static_cast<Collector*>(context);
        const Received received = {Bytes(payload, payload + size), flags};
        self->packets.push_back(received);
    }
};

// Deterministic payload without 0x2D, so no packet can hide inside it.
Bytes payload_of(size_t size, uint32_t seed) {
    Bytes bytes;
    uint32_t state = seed;
    while (bytes.size() < size) {
        state = state * 1664525u + 1013904223u;
        const uint8_t byte = static_cast<uint8_t>(state >> 24);
        if (byte != sync_0) bytes.push_back(byte);
    }
    return bytes;
}

Bytes build(const Bytes& payload) {
    Bytes out(payload.size() + k_packet_overhead);
    const size_t size = packet_build(payload.data(), static_cast<uint16_t>(payload.size()), out.data(), out.size());
    REQUIRE(size == out.size());
    return out;
}

// A header only: sync word and a LEN.
Bytes header_of(uint16_t length) {
    Bytes bytes;
    bytes.push_back(sync_0);
    bytes.push_back(sync_1);
    bytes.push_back(static_cast<uint8_t>(length >> bits_per_byte));
    bytes.push_back(static_cast<uint8_t>(length & byte_mask));
    return bytes;
}

void feed(PacketReader& reader, const Bytes& bytes, uint8_t flags = 0) {
    for (size_t i = 0; i < bytes.size(); ++i) reader.push(bytes[i], flags);
}

Bytes concat(const Bytes& a, const Bytes& b) {
    Bytes out(a);
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

Event event_of(EventType type, uint8_t value = 0, uint8_t flags = 0, uint32_t byte_index = 0) {
    Event event;
    std::memset(&event, 0, sizeof(event));
    event.type = type;
    event.value = value;
    event.flags = flags;
    event.byte_index = byte_index;
    return event;
}

// Byte events for `bytes`, byte_index counting from `first`.
void send(PacketReader& reader, const Bytes& bytes, uint32_t first, uint8_t flags = 0) {
    for (size_t i = 0; i < bytes.size(); ++i) {
        reader.on_event(event_of(EventType::byte, bytes[i], flags, first + static_cast<uint32_t>(i)));
    }
}

uint16_t stored_crc(const Bytes& packet) {
    return static_cast<uint16_t>((packet[packet.size() - 2] << bits_per_byte) | packet[packet.size() - 1]);
}

}  // namespace

TEST(packet_crc_check_value) {
    const char* check = "123456789";
    CHECK_EQ(crc16_ccitt(reinterpret_cast<const uint8_t*>(check), std::strlen(check)), 0x29B1);
    CHECK_EQ(unlimited::k_crc16_check, 0x29B1);
    CHECK_EQ(crc16_ccitt(nullptr, 0), 0xFFFF);
    // Chaining: the CRC of a+b equals the CRC of b started from the CRC of a.
    const Bytes data = payload_of(40, 3);
    const uint16_t whole = crc16_ccitt(data.data(), data.size());
    CHECK_EQ(crc16_ccitt(data.data() + 17, data.size() - 17, crc16_ccitt(data.data(), 17)), whole);
}

TEST(packet_build_layout_and_limits) {
    const size_t sizes[] = {5, ax25_payload, host_max_payload};
    for (size_t i = 0; i < test::count_of(sizes); ++i) {
        const Bytes payload = payload_of(sizes[i], 1);
        const Bytes packet = build(payload);
        CHECK_EQ(packet[0], sync_0);
        CHECK_EQ(packet[1], sync_1);
        CHECK_EQ(static_cast<size_t>(packet[length_at]), sizes[i] >> bits_per_byte);  // big-endian LEN
        CHECK_EQ(static_cast<size_t>(packet[length_at + 1]), sizes[i] & byte_mask);
        CHECK(Bytes(packet.begin() + k_packet_header, packet.end() - 2) == payload);
        CHECK_EQ(stored_crc(packet), crc16_ccitt(packet.data() + length_at, payload.size() + 2));  // LEN + payload
    }

    const Bytes payload = payload_of(host_max_payload + 1, 2);
    std::vector<uint8_t> out(host_max_payload + 1 + k_packet_overhead);
    CHECK_EQ(packet_build(payload.data(), 0, out.data(), out.size()), 0u);
    CHECK_EQ(packet_build(payload.data(), 5, out.data(), 5 + k_packet_overhead - 1), 0u);
    CHECK_EQ(packet_build(payload.data(), static_cast<uint16_t>(host_max_payload + 1), out.data(), out.size()), 0u);
    CHECK_EQ(packet_build(payload.data(), static_cast<uint16_t>(host_max_payload), out.data(), out.size()),
             host_max_payload + k_packet_overhead);

    // In place: the payload already sits after the header, or at the very start of the buffer.
    const Bytes expected = build(payload_of(ax25_payload, 3));
    Bytes shifted(expected.size());
    const Bytes body = payload_of(ax25_payload, 3);
    std::memcpy(&shifted[k_packet_header], body.data(), body.size());
    CHECK_EQ(packet_build(&shifted[k_packet_header], static_cast<uint16_t>(body.size()), shifted.data(),
                          shifted.size()),
             expected.size());
    CHECK(shifted == expected);
    Bytes front(expected.size());
    std::memcpy(&front[0], body.data(), body.size());
    CHECK_EQ(packet_build(front.data(), static_cast<uint16_t>(body.size()), front.data(), front.size()),
             expected.size());
    CHECK(front == expected);
}

TEST(packet_reader_size) {
    CHECK(sizeof(PacketReader) >= k_packet_max_payload + k_packet_overhead);
    CHECK(sizeof(PacketReader) <= k_packet_max_payload + k_packet_overhead + reader_fields_max);
    NOTE("sizeof(PacketReader) %zu bytes for UNLIMITED_PACKET_MAX %u", sizeof(PacketReader),
         static_cast<unsigned>(UNLIMITED_PACKET_MAX));
}

TEST(packet_every_single_bit_flip_is_detected) {
    const size_t sizes[] = {64, ax25_payload};
    const Bytes trailer(host_max_payload + k_packet_overhead, 0x00);
    for (size_t s = 0; s < test::count_of(sizes); ++s) {
        const Bytes payload = payload_of(sizes[s], 9);
        const Bytes packet = build(payload);
        size_t flips = 0;
        for (size_t byte = 0; byte < packet.size(); ++byte) {
            for (unsigned bit = 0; bit < bits_per_byte; ++bit) {
                Bytes corrupted = packet;
                corrupted[byte] = static_cast<uint8_t>(corrupted[byte] ^ (1u << bit));
                if (byte >= k_packet_header) {  // payload or CRC: the CRC itself must disagree
                    const uint16_t crc = crc16_ccitt(corrupted.data() + length_at, payload.size() + 2);
                    CHECK(crc != stored_crc(corrupted));
                }
                Collector collector;
                PacketReader reader(&Collector::on_packet, &collector);
                feed(reader, concat(corrupted, trailer));
                if (!CHECK(collector.packets.empty())) NOTE("flip byte %zu bit %u delivered a packet", byte, bit);
                ++flips;
            }
        }
        CHECK_EQ(flips, packet.size() * bits_per_byte);
    }
}

TEST(packet_round_trip_and_back_to_back) {
    Collector collector;
    PacketReader reader(&Collector::on_packet, &collector);
    const size_t sizes[] = {1, 17, ax25_payload, host_max_payload, 255, 256, 3};
    Bytes stream = payload_of(7, 4);  // leading noise
    for (size_t i = 0; i < test::count_of(sizes); ++i) stream = concat(stream, build(payload_of(sizes[i], 10 + i)));
    feed(reader, stream);
    REQUIRE(collector.packets.size() == test::count_of(sizes));
    for (size_t i = 0; i < test::count_of(sizes); ++i) CHECK(collector.packets[i].payload == payload_of(sizes[i], 10 + i));
    CHECK_EQ(reader.crc_errors(), 0u);
}

TEST(packet_sync_word_inside_payload) {
    Collector collector;
    PacketReader reader(&Collector::on_packet, &collector);
    Bytes payload = header_of(3);
    payload.push_back(sync_0);
    payload.push_back(sync_0);
    const Bytes first = build(payload);
    const Bytes second = build(payload_of(9, 5));
    feed(reader, concat(first, second));
    REQUIRE(collector.packets.size() == 2u);
    CHECK(collector.packets[0].payload == payload);
    CHECK(collector.packets[1].payload == payload_of(9, 5));
}

TEST(packet_crc_failure_resyncs_without_loss) {
    // A truncated packet whose LEN swallows the start of the next packet: after the CRC failure the
    // buffered bytes are rescanned and the next packet is still delivered.
    const Bytes good = build(payload_of(20, 6));
    Bytes broken = header_of(16);
    broken.push_back(0x11);
    broken.push_back(0x22);
    broken.push_back(0x33);
    Collector collector;
    PacketReader reader(&Collector::on_packet, &collector);
    feed(reader, concat(broken, good));
    REQUIRE(collector.packets.size() == 1u);
    CHECK(collector.packets[0].payload == payload_of(20, 6));
    CHECK_EQ(reader.crc_errors(), 1u);

    // A corrupted packet directly followed by two good ones.
    Bytes bad = build(payload_of(30, 7));
    bad[10] = static_cast<uint8_t>(bad[10] ^ 0x40);
    Collector second;
    PacketReader again(&Collector::on_packet, &second);
    feed(again, concat(concat(bad, good), build(payload_of(2, 8))));
    REQUIRE(second.packets.size() == 2u);
    CHECK(second.packets[0].payload == payload_of(20, 6));
    CHECK(second.packets[1].payload == payload_of(2, 8));
    CHECK_EQ(again.crc_errors(), 1u);

    // LEN 40 swallows two whole packets and part of a third: all three come out of the rescan.
    Bytes greedy(broken);
    greedy[length_at + 1] = 40;
    const Bytes small = build(payload_of(2, 14));
    const Bytes third = build(payload_of(12, 15));
    Collector third_collector;
    PacketReader greedy_reader(&Collector::on_packet, &third_collector);
    feed(greedy_reader, concat(concat(concat(greedy, small), good), third));
    REQUIRE(third_collector.packets.size() == 3u);
    CHECK(third_collector.packets[0].payload == payload_of(2, 14));
    CHECK(third_collector.packets[1].payload == payload_of(20, 6));
    CHECK(third_collector.packets[2].payload == payload_of(12, 15));
    CHECK_EQ(greedy_reader.crc_errors(), 1u);

    // AX.25 size: a corrupted 330-byte packet whose tail hides a whole 330-byte packet and the start of a
    // maximum-size one (LEN claims the full buffer): both good packets survive the rescan.
    Bytes large_bad = build(payload_of(ax25_payload, 16));
    large_bad[100] = static_cast<uint8_t>(large_bad[100] ^ 0x01);
    Bytes truncated(large_bad.begin(), large_bad.begin() + 50);
    truncated[length_at] = static_cast<uint8_t>(host_max_payload >> bits_per_byte);
    truncated[length_at + 1] = static_cast<uint8_t>(host_max_payload & byte_mask);
    const Bytes ax25 = build(payload_of(ax25_payload, 17));
    const Bytes biggest = build(payload_of(host_max_payload, 18));
    Collector large;
    PacketReader large_reader(&Collector::on_packet, &large);
    feed(large_reader, concat(concat(concat(truncated, ax25), biggest), concat(large_bad, ax25)));
    REQUIRE(large.packets.size() == 3u);
    CHECK(large.packets[0].payload == payload_of(ax25_payload, 17));
    CHECK(large.packets[1].payload == payload_of(host_max_payload, 18));
    CHECK(large.packets[2].payload == payload_of(ax25_payload, 17));
    CHECK_EQ(large_reader.crc_errors(), 2u);
}

// A bit error that raises LEN (hi byte 100 -> 612, lo byte 100 -> 228) keeps the packet waiting for bytes that never
// come; the intact packets behind it come out when the transmission ends (end or lost), and nothing else does.
TEST(packet_end_rescans_behind_a_corrupted_length) {
    const Bytes a = build(payload_of(100, 20));
    const Bytes b = build(payload_of(21, 21));
    const Bytes c = build(payload_of(20, 22));
    const size_t flips[] = {length_at, length_at + 1};
    const uint8_t masks[] = {0x02, 0x80};
    const EventType ends[] = {EventType::end, EventType::lost};
    for (size_t f = 0; f < test::count_of(flips); ++f) {
        for (size_t e = 0; e < test::count_of(ends); ++e) {
            Bytes corrupted(a);
            corrupted[flips[f]] = static_cast<uint8_t>(corrupted[flips[f]] ^ masks[f]);
            Collector collector;
            PacketReader reader(&Collector::on_packet, &collector);
            feed(reader, concat(concat(corrupted, b), c));
            CHECK(collector.packets.empty());
            reader.on_event(event_of(ends[e]));
            REQUIRE(collector.packets.size() == 2u);
            CHECK(collector.packets[0].payload == payload_of(21, 21));
            CHECK(collector.packets[1].payload == payload_of(20, 22));
            // The reader starts clean after the rescan.
            feed(reader, b);
            CHECK_EQ(collector.packets.size(), 3u);
        }
    }
}

TEST(packet_rejects_bad_length_and_false_sync) {
    Collector collector;
    PacketReader reader(&Collector::on_packet, &collector);
    Bytes stream = header_of(0);  // LEN 0 is invalid
    stream.push_back(sync_0);
    stream.push_back(sync_0);  // 2D 2D D4: the second 0x2D starts the packet
    const Bytes good = build(payload_of(4, 11));
    stream.insert(stream.end(), good.begin() + 1, good.end());
    feed(reader, stream);
    REQUIRE(collector.packets.size() == 1u);
    CHECK(collector.packets[0].payload == payload_of(4, 11));
    CHECK_EQ(reader.crc_errors(), 0u);

    // LEN above UNLIMITED_PACKET_MAX is rejected at once: the reader does not wait for that many bytes.
    const uint16_t too_long[] = {static_cast<uint16_t>(host_max_payload + 1), 0x0500, 0xFFFF};
    for (size_t i = 0; i < test::count_of(too_long); ++i) {
        Collector next;
        PacketReader fresh(&Collector::on_packet, &next);
        feed(fresh, concat(header_of(too_long[i]), good));
        CHECK_EQ(next.packets.size(), 1u);
        CHECK_EQ(fresh.crc_errors(), 0u);
    }
}

TEST(packet_events_forward_flags_and_reset) {
    Collector collector;
    PacketReader reader(&Collector::on_packet, &collector);
    const Bytes packet = build(payload_of(6, 12));
    const uint8_t weak = unlimited::event_flag_weak;
    const uint8_t late = unlimited::event_flag_late_join;
    const uint8_t flywheel = unlimited::event_flag_flywheel_stop;

    // Half a packet, then the transmission ends: nothing is delivered and the parser starts clean.
    uint32_t index = 0;
    for (size_t i = 0; i < packet.size() / 2; ++i) reader.on_event(event_of(EventType::byte, packet[i], weak, index++));
    reader.on_event(event_of(EventType::end));
    for (size_t i = packet.size() / 2; i < packet.size(); ++i) reader.on_event(event_of(EventType::byte, packet[i], 0, index++));
    CHECK(collector.packets.empty());

    index = 0;
    for (size_t i = 0; i < packet.size() / 2; ++i) reader.on_event(event_of(EventType::byte, packet[i], 0, index++));
    reader.on_event(event_of(EventType::lost));
    CHECK(collector.packets.empty());

    // slot and package events (telemetry) interleaved with the bytes change nothing.
    reader.on_event(event_of(EventType::locked, 0, late));
    reader.on_event(event_of(EventType::state));
    index = 40;
    for (size_t i = 0; i < packet.size(); ++i) {
        reader.on_event(event_of(EventType::slot, sync_0, weak));
        reader.on_event(event_of(EventType::byte, packet[i], i == 4 ? weak : (i == 7 ? flywheel | late : 0), index++));
        reader.on_event(event_of(EventType::package, 8));
        reader.on_event(event_of(EventType::slot, sync_1));
    }
    REQUIRE(collector.packets.size() == 1u);
    CHECK(collector.packets[0].payload == payload_of(6, 12));
    CHECK_EQ(collector.packets[0].flags, weak | flywheel | late);
    CHECK_EQ(reader.crc_errors(), 0u);

    // Flags do not leak into the next packet.
    feed(reader, packet);
    REQUIRE(collector.packets.size() == 2u);
    CHECK_EQ(collector.packets[1].flags, 0);
}

// Spec 2.6: a byte_index gap (the bytes of a lost package are missing) ends the candidate as an end would: the packet
// that lost bytes is not delivered, the one behind it is, and nothing is mixed across the gap.
TEST(packet_byte_index_gap_rescans_like_end) {
    const Bytes a = build(payload_of(10, 30));
    const Bytes b = build(payload_of(12, 31));
    const Bytes c = build(payload_of(9, 32));
    {
        // a whole, b with 3 bytes missing in its middle, c whole: a and c come out.
        Collector collector;
        PacketReader reader(&Collector::on_packet, &collector);
        uint32_t index = 0;
        send(reader, a, index);
        index += static_cast<uint32_t>(a.size());
        const Bytes b_head(b.begin(), b.begin() + 6);
        const Bytes b_tail(b.begin() + 9, b.end());
        send(reader, b_head, index);
        send(reader, b_tail, index + 9);  // bytes 6..8 of b missing
        index += static_cast<uint32_t>(b.size());
        send(reader, c, index);
        REQUIRE(collector.packets.size() == 2u);
        CHECK(collector.packets[0].payload == payload_of(10, 30));
        CHECK(collector.packets[1].payload == payload_of(9, 32));
    }
    {
        // A corrupted LEN waits for bytes; a gap behind it rescans what was buffered: the intact packet behind the
        // corrupted one comes out, then the packet after the gap.
        Bytes corrupted(a);
        corrupted[length_at] = static_cast<uint8_t>(corrupted[length_at] ^ 0x02);
        Collector collector;
        PacketReader reader(&Collector::on_packet, &collector);
        send(reader, concat(corrupted, b), 0);
        CHECK(collector.packets.empty());
        send(reader, c, static_cast<uint32_t>(corrupted.size() + b.size() + 5));
        REQUIRE(collector.packets.size() == 2u);
        CHECK(collector.packets[0].payload == payload_of(12, 31));
        CHECK(collector.packets[1].payload == payload_of(9, 32));
    }
    {
        // After a reset (end, lost) the next byte_index is free: a new transmission starts at 0 again.
        Collector collector;
        PacketReader reader(&Collector::on_packet, &collector);
        send(reader, a, 100);
        reader.on_event(event_of(EventType::end));
        send(reader, b, 0);
        CHECK_EQ(collector.packets.size(), 2u);
    }
}

// U3: "Hi" -> 2D D4 00 02 48 69 93 4A.
TEST(packet_hi_example) {
    const Bytes hi = {0x48, 0x69};
    const Bytes packet = build(hi);
    const Bytes expected = {0x2D, 0xD4, 0x00, 0x02, 0x48, 0x69, 0x93, 0x4A};
    CHECK(packet == expected);
    CHECK_EQ(crc16_ccitt(packet.data() + length_at, 4), 0x934Au);
}

TEST(packet_reader_without_handler) {
    PacketReader reader(nullptr, nullptr);
    feed(reader, build(payload_of(10, 13)));
    Bytes bad = build(payload_of(10, 13));
    bad[5] = static_cast<uint8_t>(bad[5] ^ 1);
    feed(reader, bad);
    CHECK_EQ(reader.crc_errors(), 1u);
}

// AX.25-size and maximum-size packets in one transmission: Encoder -> Decoder -> PacketReader.
TEST(packet_modem_round_trip_ax25_and_max) {
    const Bytes ax25 = unlimited::loopback::random_bytes(ax25_payload, 21);
    const Bytes biggest = unlimited::loopback::random_bytes(host_max_payload, 22);
    const Bytes stream = concat(build(ax25), build(biggest));
    const unlimited::loopback::Recording recording =
        unlimited::loopback::single(stream, unlimited::loopback::preset_config(unlimited::Preset::hf));
    const unlimited::DecoderConfig config = unlimited::DecoderConfig::for_profile(unlimited::Profile::ssb);
    const unlimited::loopback::Capture capture =
        unlimited::loopback::run_decoder(recording.samples, config, recording.samples.size());
    Collector collector;
    PacketReader reader(&Collector::on_packet, &collector);
    for (size_t i = 0; i < capture.events.size(); ++i) reader.on_event(capture.events[i]);
    REQUIRE(collector.packets.size() == 2u);
    CHECK(collector.packets[0].payload == ax25);
    CHECK(collector.packets[1].payload == biggest);
    CHECK_EQ(collector.packets[0].flags, 0);
    CHECK_EQ(collector.packets[1].flags, 0);
    CHECK_EQ(reader.crc_errors(), 0u);
    NOTE("%zu framed bytes, %.1f s of audio", stream.size(),
         static_cast<double>(recording.samples.size()) / unlimited::k_decoder_rate_hz);
}
