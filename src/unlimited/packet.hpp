#pragma once

#include "unlimited/decoder.hpp"

// Largest payload a packet may carry and PacketReader accepts. To change it, define it for the whole build
// (-DUNLIMITED_PACKET_MAX=N, on Arduino a build property), never in one source file: packet.cpp and every
// user of PacketReader must see the same value.
#ifndef UNLIMITED_PACKET_MAX
#if defined(__AVR__)
#define UNLIMITED_PACKET_MAX 256
#else
#define UNLIMITED_PACKET_MAX 1024
#endif
#endif

namespace unlimited {

// [0x2D 0xD4][LEN hi][LEN lo][payload, LEN bytes][CRC hi][CRC lo], LEN 1..k_packet_max_payload,
// CRC-16/CCITT-FALSE over the two LEN bytes and the payload.
static const uint8_t k_packet_sync_0 = 0x2D;
static const uint8_t k_packet_sync_1 = 0xD4;
static const uint8_t k_packet_header = 4;
static const uint8_t k_packet_crc = 2;
static const uint8_t k_packet_overhead = k_packet_header + k_packet_crc;
static_assert(UNLIMITED_PACKET_MAX >= 1 && UNLIMITED_PACKET_MAX <= 0xFFFF - k_packet_overhead,
              "UNLIMITED_PACKET_MAX must be 1..65529");
static const uint16_t k_packet_max_payload = UNLIMITED_PACKET_MAX;
static const uint16_t k_crc16_init = 0xFFFF;
static const uint16_t k_crc16_poly = 0x1021;
static const uint16_t k_crc16_check = 0x29B1;

uint16_t crc16_ccitt(const uint8_t* data, size_t size, uint16_t crc = k_crc16_init);

// Returns the packet size (size + k_packet_overhead), or 0 when size is 0, above k_packet_max_payload or
// out_size is too small. payload may overlap out (e.g. already sit at out + k_packet_header).
size_t packet_build(const uint8_t* payload, uint16_t size, uint8_t* out, size_t out_size);

// flags: OR of the event flags of the packet's bytes (after a rescan, also of the discarded bytes before it).
typedef void (*PacketHandler)(const uint8_t* payload, uint16_t size, uint8_t flags, void* context);

// Hunts for 0x2D 0xD4; LEN 0 or above k_packet_max_payload is rejected at once. After a CRC failure the
// buffered bytes are rescanned from the byte after the failed 0x2D, so no packet behind it is lost. end and lost
// events, and a byte event whose byte_index is not the one after the previous byte's (bytes of a lost package are
// missing), rescan the bytes behind a candidate still incomplete (e.g. a corrupted LEN), then reset it. slot and
// package events are ignored.
class PacketReader {
public:
    PacketReader(PacketHandler handler, void* context);

    void push(uint8_t byte, uint8_t flags);
    void on_event(const Event& event);
    void reset();
    uint32_t crc_errors() const;

private:
    void parse();
    void resync();
    void flush();
    void drop(uint16_t count);

    PacketHandler handler_;
    void* context_;
    uint32_t crc_errors_;
    uint32_t next_index_;  // byte_index the next byte event should carry
    uint16_t length_;      // LEN of the candidate, once its header is complete
    uint16_t fill_;        // bytes buffered, starting at a candidate 0x2D
    uint16_t parsed_;      // bytes of the candidate examined so far
    uint8_t flags_;        // OR of the event flags of the buffered bytes
    bool indexed_;         // next_index_ is known (a byte event arrived since the last reset)
    uint8_t buffer_[k_packet_max_payload + k_packet_overhead];
};

}  // namespace unlimited
