#include "unlimited/packet.hpp"

#include <string.h>

namespace unlimited {

namespace {

const uint16_t k_crc16_top_bit = 0x8000;
const uint8_t k_byte_mask = 0xFF;
const uint8_t k_sync_bytes = 2;
const uint8_t k_length_offset = k_sync_bytes;
const uint8_t k_length_bytes = 2;

uint16_t big_endian(const uint8_t* bytes) {
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << k_bits_per_byte) | bytes[1]);
}

void put_big_endian(uint16_t value, uint8_t* bytes) {
    bytes[0] = static_cast<uint8_t>(value >> k_bits_per_byte);
    bytes[1] = static_cast<uint8_t>(value & k_byte_mask);
}

}  // namespace

uint16_t crc16_ccitt(const uint8_t* data, size_t size, uint16_t crc) {
    for (size_t i = 0; i < size; ++i) {
        crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(data[i]) << k_bits_per_byte));
        for (uint8_t bit = 0; bit < k_bits_per_byte; ++bit) {
            const bool top = (crc & k_crc16_top_bit) != 0;
            crc = static_cast<uint16_t>(crc << 1);
            if (top) crc = static_cast<uint16_t>(crc ^ k_crc16_poly);
        }
    }
    return crc;
}

size_t packet_build(const uint8_t* payload, uint16_t size, uint8_t* out, size_t out_size) {
    const size_t total = static_cast<size_t>(size) + k_packet_overhead;
    if (size == 0 || size > k_packet_max_payload || out_size < total) return 0;
    memmove(out + k_packet_header, payload, size);
    out[0] = k_packet_sync_0;
    out[1] = k_packet_sync_1;
    put_big_endian(size, out + k_length_offset);
    put_big_endian(crc16_ccitt(out + k_length_offset, static_cast<size_t>(size) + k_length_bytes),
                   out + k_packet_header + size);
    return total;
}

PacketReader::PacketReader(PacketHandler handler, void* context)
    : handler_(handler),
      context_(context),
      crc_errors_(0),
      next_index_(0),
      length_(0),
      fill_(0),
      parsed_(0),
      flags_(0),
      indexed_(false),
      buffer_() {}

// Only a 0x2D can start the buffer, and a candidate never outgrows it: a complete one is consumed at once.
void PacketReader::push(uint8_t byte, uint8_t flags) {
    if (fill_ == 0 && byte != k_packet_sync_0) return;
    buffer_[fill_++] = byte;
    flags_ = static_cast<uint8_t>(flags_ | flags);
    parse();
}

// A byte_index that does not follow the previous byte's: the bytes of a lost package are missing, which ends the
// candidate as an end would (spec 2.6).
void PacketReader::on_event(const Event& event) {
    switch (event.type) {
        case EventType::byte:
            if (indexed_ && event.byte_index != next_index_) flush();
            push(event.value, event.flags);
            next_index_ = event.byte_index + 1u;
            indexed_ = true;
            break;
        case EventType::end:
        case EventType::lost:
            flush();
            break;
        case EventType::state:
        case EventType::locked:
        case EventType::slot:
        case EventType::package:
            break;
    }
}

void PacketReader::reset() {
    next_index_ = 0;
    length_ = 0;
    fill_ = 0;
    parsed_ = 0;
    flags_ = 0;
    indexed_ = false;
}

uint32_t PacketReader::crc_errors() const {
    return crc_errors_;
}

// buffer_[0] is the candidate's 0x2D; each new byte is checked at its position in the packet.
void PacketReader::parse() {
    while (parsed_ < fill_) {
        ++parsed_;
        if (parsed_ == k_sync_bytes) {
            if (buffer_[1] != k_packet_sync_1) resync();
        } else if (parsed_ == k_packet_header) {
            length_ = big_endian(buffer_ + k_length_offset);
            if (length_ == 0 || length_ > k_packet_max_payload) resync();
        } else if (parsed_ > k_packet_header && parsed_ == length_ + k_packet_overhead) {
            const uint16_t crc = crc16_ccitt(buffer_ + k_length_offset, static_cast<size_t>(length_) + k_length_bytes);
            if (crc == big_endian(buffer_ + parsed_ - k_packet_crc)) {
                if (handler_ != nullptr) handler_(buffer_ + k_packet_header, length_, flags_, context_);
                drop(parsed_);
            } else {
                ++crc_errors_;
                resync();
            }
        }
    }
}

// The candidate at buffer_[0] failed: rescan what is buffered, starting after its 0x2D.
void PacketReader::resync() {
    drop(1);
}

// The transmission ended: a candidate still waiting never completes (a corrupted LEN may claim more bytes than
// followed it), so the bytes buffered behind it are rescanned before the reader starts clean.
void PacketReader::flush() {
    while (fill_ > 0) {
        drop(1);
        parse();
    }
    reset();
}

// Discards count bytes, then everything before the next 0x2D, and restarts parsing at the buffer start.
void PacketReader::drop(uint16_t count) {
    while (count < fill_ && buffer_[count] != k_packet_sync_0) ++count;
    fill_ = static_cast<uint16_t>(fill_ - count);
    memmove(buffer_, buffer_ + count, fill_);
    parsed_ = 0;
    if (fill_ == 0) flags_ = 0;
}

}  // namespace unlimited
