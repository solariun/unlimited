#pragma once

#include "unlimited/platform.hpp"

// KISS (spec 12.1): the framing between a computer and a TNC, and only that: KISS is the computer's door, not the air
// protocol. A frame is FEND, a command byte, the data, FEND; a FEND or FESC inside the data travels as FESC TFEND or
// FESC TFESC. Two frames may share the FEND between them (C0 00 A C0 00 B C0 gives A and B).
namespace unlimited {

static const uint8_t k_kiss_fend = 0xC0;
static const uint8_t k_kiss_fesc = 0xDB;
static const uint8_t k_kiss_tfend = 0xDC;
static const uint8_t k_kiss_tfesc = 0xDD;
static const uint8_t k_kiss_data = 0x00;          // command 0 on port 0: a data frame (the modem sends port 0)
static const uint8_t k_kiss_command_mask = 0x0F;  // the low nibble is the command, the high nibble the port
// TXDELAY, P, SLOTTIME, TXTAIL and FULLDUPLEX: accepted and ignored, the command line sets the timing (as kiss_modem).
static const uint8_t k_kiss_first_parameter = 1;
static const uint8_t k_kiss_last_parameter = 5;
static const uint8_t k_kiss_escaped_max = 2;      // bytes kiss_escape() writes at most

// What one byte from the computer gives: nothing (framing, a command, an ignored frame), a data byte of a data frame,
// or the end of a data frame that holds at least one byte (an empty data frame gives nothing).
enum class KissStep : uint8_t { none, data, end };

struct KissCounters {
    uint32_t frames;       // data frames ended (any port), each with at least one byte
    uint32_t bytes;        // data bytes taken
    uint32_t parameters;   // frames of commands 1..5, ignored
    uint32_t unknown;      // frames of any other command, ignored
    uint32_t bad_escapes;  // FESC followed by a byte other than TFEND or TFESC: that byte passes as it is
    uint32_t outside;      // bytes before the first FEND, ignored
};

// Streaming decoder of what the computer sends. peek() tells what feed() would do with a byte without taking it, so a
// caller with a full queue can leave the byte for later (back-pressure) and the decoder's state stays exact.
class KissDecoder {
public:
    KissDecoder();

    KissStep peek(uint8_t byte, uint8_t& value) const;
    KissStep feed(uint8_t byte, uint8_t& value);
    void reset();                       // back to waiting for a FEND; the counters stay
    bool in_data() const;               // inside a data frame (its bytes so far are data)
    const KissCounters& counters() const;

private:
    enum class State : uint8_t {
        outside,       // before the first FEND
        command,       // after a FEND: the next byte is the command (another FEND is a shared one)
        data,          // in a data frame
        data_escape,   // after a FESC in a data frame
        other,         // in a frame of another command: skipped up to the next FEND
        other_escape   // after a FESC in such a frame
    };

    // The decoder's move on `byte`: the next state, what it gives and which counter it bumps.
    enum class Count : uint8_t { none, frame, byte, parameter, unknown, bad_escape, bad_escape_end, outside };
    KissStep move(uint8_t byte, State& state, uint8_t& value, Count& count) const;

    State state_;
    uint32_t frame_bytes_;  // data bytes of the frame being read
    KissCounters counters_;
};

// A byte to the computer: FEND as FESC TFEND, FESC as FESC TFESC, any other byte as it is. Writes 1 or 2 bytes.
uint8_t kiss_escape(uint8_t byte, uint8_t out[k_kiss_escaped_max]);

}  // namespace unlimited
