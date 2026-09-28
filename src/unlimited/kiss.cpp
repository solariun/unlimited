#include "unlimited/kiss.hpp"

namespace unlimited {

KissDecoder::KissDecoder() : state_(State::outside), frame_bytes_(0), counters_() {}

// One table of moves for peek() and feed(): FEND always closes whatever frame is open (a FEND after a FESC too: the
// escape was bad, but framing wins), and opens the next one.
KissStep KissDecoder::move(uint8_t byte, State& state, uint8_t& value, Count& count) const {
    count = Count::none;
    value = byte;
    if (byte == k_kiss_fend) {
        KissStep step = KissStep::none;
        if (state == State::data || state == State::data_escape) {
            if (state == State::data_escape) count = Count::bad_escape_end;
            else if (frame_bytes_ > 0) count = Count::frame;
            if (frame_bytes_ > 0) step = KissStep::end;
        }
        state = State::command;
        return step;
    }
    switch (state) {
        case State::outside:
            count = Count::outside;
            return KissStep::none;
        case State::command: {
            const uint8_t command = static_cast<uint8_t>(byte & k_kiss_command_mask);
            if (command == k_kiss_data) {
                state = State::data;
            } else {
                state = State::other;
                const bool parameter = command >= k_kiss_first_parameter && command <= k_kiss_last_parameter;
                count = parameter ? Count::parameter : Count::unknown;
            }
            return KissStep::none;
        }
        case State::data:
            if (byte == k_kiss_fesc) {
                state = State::data_escape;
                return KissStep::none;
            }
            count = Count::byte;
            return KissStep::data;
        case State::data_escape:
            state = State::data;
            if (byte == k_kiss_tfend) {
                value = k_kiss_fend;
            } else if (byte == k_kiss_tfesc) {
                value = k_kiss_fesc;
            } else {
                count = Count::bad_escape;  // the byte after FESC passes as it is
                return KissStep::data;
            }
            count = Count::byte;
            return KissStep::data;
        case State::other:
            if (byte == k_kiss_fesc) state = State::other_escape;
            return KissStep::none;
        case State::other_escape:
            state = State::other;
            return KissStep::none;
    }
    return KissStep::none;
}

KissStep KissDecoder::peek(uint8_t byte, uint8_t& value) const {
    State state = state_;
    Count count = Count::none;
    return move(byte, state, value, count);
}

KissStep KissDecoder::feed(uint8_t byte, uint8_t& value) {
    Count count = Count::none;
    const KissStep step = move(byte, state_, value, count);
    switch (count) {
        case Count::none:
            break;
        case Count::frame:
            ++counters_.frames;
            break;
        case Count::byte:
            ++counters_.bytes;
            break;
        case Count::parameter:
            ++counters_.parameters;
            break;
        case Count::unknown:
            ++counters_.unknown;
            break;
        case Count::bad_escape:
            ++counters_.bad_escapes;
            ++counters_.bytes;
            break;
        case Count::bad_escape_end:
            ++counters_.bad_escapes;
            if (frame_bytes_ > 0) ++counters_.frames;
            break;
        case Count::outside:
            ++counters_.outside;
            break;
    }
    if (step == KissStep::data) ++frame_bytes_;
    if (state_ != State::data && state_ != State::data_escape) frame_bytes_ = 0;
    return step;
}

void KissDecoder::reset() {
    state_ = State::outside;
    frame_bytes_ = 0;
}

bool KissDecoder::in_data() const {
    return state_ == State::data || state_ == State::data_escape;
}

const KissCounters& KissDecoder::counters() const {
    return counters_;
}

uint8_t kiss_escape(uint8_t byte, uint8_t out[k_kiss_escaped_max]) {
    if (byte == k_kiss_fend || byte == k_kiss_fesc) {
        out[0] = k_kiss_fesc;
        out[1] = byte == k_kiss_fend ? k_kiss_tfend : k_kiss_tfesc;
        return k_kiss_escaped_max;
    }
    out[0] = byte;
    return 1;
}

}  // namespace unlimited
