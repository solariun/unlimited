// The KISS modem's portable core (spec 12.1, 12.2): the KISS codec, the send queue and its back-pressure, frame
// boundaries, the channel check on a simulated clock, PTT sequencing, and the receiver's streaming to the computer.
#include "support/loopback.hpp"
#include "test_harness.hpp"
#include "unlimited/modem.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using unlimited::AccessConfig;
using unlimited::ChannelState;
using unlimited::Decoder;
using unlimited::DecoderConfig;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::Event;
using unlimited::EventType;
using unlimited::KissCounters;
using unlimited::KissDecoder;
using unlimited::KissStep;
using unlimited::Modem;
using unlimited::ModemConfig;
using unlimited::ModemCounters;
using unlimited::ModemTransmitter;
using unlimited::ModemWake;

namespace loopback = unlimited::loopback;

namespace {

using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using std::int16_t;

typedef std::vector<uint8_t> Bytes;

const uint8_t k_fend = unlimited::k_kiss_fend;
const uint8_t k_fesc = unlimited::k_kiss_fesc;
const uint8_t k_tfend = unlimited::k_kiss_tfend;
const uint8_t k_tfesc = unlimited::k_kiss_tfesc;
const uint32_t k_samples_per_ms = unlimited::k_modem_rate_hz / 1000;
const uint32_t k_ms_per_s = 1000;
const uint32_t k_us_per_ms = 1000;
const uint16_t k_fast_centi = 2500;    // 25 bytes/s: short transmissions for the timing tests
const uint16_t k_default_centi = 600;  // 6 bytes/s
const uint32_t k_seed = 12345;
const uint8_t k_draw_shift = 24;

// ---------------------------------------------------------------------------
// KISS helpers
// ---------------------------------------------------------------------------

Bytes kiss_frame(const Bytes& data) {
    Bytes out(1, k_fend);
    out.push_back(unlimited::k_kiss_data);
    for (size_t i = 0; i < data.size(); ++i) {
        uint8_t escaped[unlimited::k_kiss_escaped_max];
        const uint8_t size = unlimited::kiss_escape(data[i], escaped);
        out.insert(out.end(), escaped, escaped + size);
    }
    out.push_back(k_fend);
    return out;
}

Bytes text(const char* value) {
    Bytes bytes;
    for (const char* c = value; *c != '\0'; ++c) bytes.push_back(static_cast<uint8_t>(*c));
    return bytes;
}

// Every data frame the decoder gives for `input`.
std::vector<Bytes> decode_frames(KissDecoder& decoder, const Bytes& input) {
    std::vector<Bytes> frames;
    Bytes current;
    for (size_t i = 0; i < input.size(); ++i) {
        uint8_t value = 0;
        const KissStep step = decoder.feed(input[i], value);
        if (step == KissStep::data) current.push_back(value);
        if (step == KissStep::end) {
            frames.push_back(current);
            current.clear();
        }
        if (!decoder.in_data()) current.clear();
    }
    return frames;
}

std::vector<Bytes> decode_frames(const Bytes& input) {
    KissDecoder decoder;
    return decode_frames(decoder, input);
}

// The generator of the p-persistence draws (ModemTransmitter: xorshift32, the top 8 bits).
uint8_t next_draw(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<uint8_t>(state >> k_draw_shift);
}

// ---------------------------------------------------------------------------
// A bench on a simulated clock: the device plays 1 ms of audio per step
// ---------------------------------------------------------------------------

struct Bench {
    std::vector<std::pair<uint32_t, bool> > ptt;  // (ms, on)
    Bytes host;                                   // bytes to the computer
    std::vector<uint64_t> host_sample;            // input samples heard when each came
    uint32_t wakes_control = 0;
    uint32_t wakes_host = 0;
    bool wake_pending = true;
    bool record = true;  // keep what audio_output() played
    uint32_t now_ms = 0;
    uint64_t heard = 0;
    std::vector<int16_t> played;  // every sample of audio_output()

    static void on_ptt(bool on, void* context) {
        Bench& bench = *static_cast<Bench*>(context);
        bench.ptt.push_back(std::make_pair(bench.now_ms, on));
    }

    static void on_host(const uint8_t* data, size_t size, void* context) {
        Bench& bench = *static_cast<Bench*>(context);
        for (size_t i = 0; i < size; ++i) {
            bench.host.push_back(data[i]);
            bench.host_sample.push_back(bench.heard);
        }
    }

    static void on_wake(ModemWake what, void* context) {
        Bench& bench = *static_cast<Bench*>(context);
        if (what == ModemWake::control) {
            ++bench.wakes_control;
            bench.wake_pending = true;
        } else {
            ++bench.wakes_host;
        }
    }

    uint32_t keys() const {
        uint32_t count = 0;
        for (size_t i = 0; i < ptt.size(); ++i) count += ptt[i].second ? 1 : 0;
        return count;
    }
};

// Event-driven stepping: tick() only after a wake-up or at next_tick_ms(), as the PC's control thread does.
template <typename Core>
void run(Core& core, Bench& bench, uint32_t ms, bool every_ms = false) {
    int16_t buffer[k_samples_per_ms];
    for (uint32_t i = 0; i < ms; ++i) {
        const uint32_t next = core.next_tick_ms();
        const bool due = next != unlimited::k_no_tick && static_cast<int32_t>(bench.now_ms - next) >= 0;
        if (every_ms || bench.wake_pending || due) {
            bench.wake_pending = false;
            core.tick(bench.now_ms);
        }
        core.audio_output(buffer, k_samples_per_ms);
        if (bench.record) bench.played.insert(bench.played.end(), buffer, buffer + k_samples_per_ms);
        ++bench.now_ms;
    }
}

EncoderConfig signal_at(uint16_t centi) {
    EncoderConfig signal;
    signal.sample_rate_hz = unlimited::k_modem_rate_hz;
    signal.slot_us = unlimited::slot_us_for_centi_speed(centi);
    signal.lead_in_ms = unlimited::k_default_txdelay_ms;
    if (!unlimited::passband_fit(signal).fits) {
        signal.passband.low_hz = unlimited::k_am_passband_low_hz;
        signal.passband.high_hz = unlimited::k_am_passband_high_hz;
    }
    return signal;
}

// Channel access that keys at the first chance: no dwait, every draw wins.
AccessConfig eager_access() {
    AccessConfig access;
    access.dwait_ms = 0;
    access.persist = 255;
    access.seed = k_seed;
    return access;
}

ModemConfig modem_at(uint16_t centi) {
    ModemConfig config;
    config.signal = signal_at(centi);
    config.receiver.slot_us = config.signal.slot_us;
    config.receiver.passband = config.signal.passband;
    config.access = eager_access();
    return config;
}

// Transmissions in played audio: runs of samples between silences longer than `gap` samples.
struct Burst {
    size_t first;  // first non-zero sample
    size_t last;   // last non-zero sample
};

std::vector<Burst> bursts(const std::vector<int16_t>& audio, size_t gap) {
    std::vector<Burst> found;
    size_t zeros = gap;
    for (size_t i = 0; i < audio.size(); ++i) {
        if (audio[i] == 0) {
            ++zeros;
            continue;
        }
        if (zeros >= gap || found.empty()) {
            Burst burst = {i, i};
            found.push_back(burst);
        }
        found.back().last = i;
        zeros = 0;
    }
    return found;
}

// A decoder's byte events, per transmission (locked .. end/lost).
struct Heard {
    std::vector<Bytes> frames;
    size_t locks = 0;
    size_t ends = 0;
    size_t losses = 0;
};

void on_heard(const Event& event, void* context) {
    Heard& heard = *static_cast<Heard*>(context);
    if (event.type == EventType::locked) {
        ++heard.locks;
        heard.frames.push_back(Bytes());
    } else if (event.type == EventType::byte && !heard.frames.empty()) {
        heard.frames.back().push_back(event.value);
    } else if (event.type == EventType::end) {
        ++heard.ends;
    } else if (event.type == EventType::lost) {
        ++heard.losses;
    }
}

Heard decode_audio(const std::vector<int16_t>& audio, const EncoderConfig& signal) {
    Heard heard;
    DecoderConfig receiver;
    receiver.slot_us = signal.slot_us;
    receiver.passband = signal.passband;
    Decoder decoder(receiver, &on_heard, &heard);
    // The receiver takes nothing before its first sample for silence (V6): it hears some before the audio.
    const std::vector<int16_t> lead(static_cast<size_t>(loopback::leading_silence_ms(signal) * k_samples_per_ms), 0);
    decoder.process(lead.data(), lead.size());
    decoder.process(audio.data(), audio.size());
    const std::vector<int16_t> drain(static_cast<size_t>(3 * unlimited::k_window_slots * signal.slot_us / k_us_per_ms *
                                                         k_samples_per_ms) +
                                         decoder.lookahead_samples(),
                                     0);
    decoder.process(drain.data(), drain.size());
    return heard;
}

uint32_t transmission_ms(const EncoderConfig& signal, size_t bytes) {
    return Encoder(signal).duration_samples(bytes) / k_samples_per_ms;
}

}  // namespace

// ---------------------------------------------------------------------------
// KISS codec
// ---------------------------------------------------------------------------

TEST(kiss_frames_escapes_and_shared_fends) {
    const uint8_t escaped[] = {k_fend, 0x00, 0x41, k_fesc, k_tfend, 0x42, k_fesc, k_tfesc, k_fend};
    const std::vector<Bytes> one = decode_frames(Bytes(escaped, escaped + test::count_of(escaped)));
    REQUIRE(one.size() == 1);
    const uint8_t expected[] = {0x41, k_fend, 0x42, k_fesc};
    CHECK(one[0] == Bytes(expected, expected + test::count_of(expected)));

    // C0 00 A C0 00 B C0: the FEND between the frames closes A and opens B; neither is lost.
    const uint8_t shared[] = {k_fend, 0x00, 'A', k_fend, 0x00, 'B', k_fend};
    const std::vector<Bytes> two = decode_frames(Bytes(shared, shared + test::count_of(shared)));
    REQUIRE(two.size() == 2);
    CHECK(two[0] == text("A"));
    CHECK(two[1] == text("B"));

    // Runs of FENDs, a data frame on port 1 (0x10), bytes before the first FEND.
    const uint8_t mixed[] = {'x', 'y', k_fend, k_fend, k_fend, 0x10, 'C', k_fend, k_fend, 0x00, 'D', 'E', k_fend};
    KissDecoder decoder;
    const std::vector<Bytes> frames = decode_frames(decoder, Bytes(mixed, mixed + test::count_of(mixed)));
    REQUIRE(frames.size() == 2);
    CHECK(frames[0] == text("C"));
    CHECK(frames[1] == text("DE"));
    const KissCounters& counters = decoder.counters();
    CHECK_EQ(counters.frames, 2u);
    CHECK_EQ(counters.bytes, 3u);
    CHECK_EQ(counters.outside, 2u);
    CHECK_EQ(counters.parameters, 0u);
    CHECK_EQ(counters.unknown, 0u);
}

TEST(kiss_commands_are_ignored_and_counted) {
    // TXDELAY, P, SLOTTIME, TXTAIL, FULLDUPLEX (1..5): accepted, ignored; SETHARDWARE, RETURN and the rest: counted
    // as unknown. A parameter holding FEND or FESC escaped stays ignored. The data frame after them comes through.
    const uint8_t input[] = {k_fend, 0x01, 0x28, k_fend, 0x02, 0x3F, k_fend, 0x03, 0x0A, k_fend, 0x04, 0x05,
                             k_fend, 0x05, 0x00, k_fend, 0x21, k_fesc, k_tfend, k_fend, 0x06, 0x01, k_fend, 0xFF,
                             k_fend, 0x0E, 'z', k_fend, 0x00, 'O', 'K', k_fend};
    KissDecoder decoder;
    const std::vector<Bytes> frames = decode_frames(decoder, Bytes(input, input + test::count_of(input)));
    REQUIRE(frames.size() == 1);
    CHECK(frames[0] == text("OK"));
    CHECK_EQ(decoder.counters().parameters, 6u);
    CHECK_EQ(decoder.counters().unknown, 3u);
    CHECK_EQ(decoder.counters().frames, 1u);
}

TEST(kiss_bad_escapes_pass_the_byte) {
    KissDecoder decoder;
    const uint8_t input[] = {k_fend, 0x00, k_fesc, 0x41, k_fend,   // FESC 41: 41 passes
                             0x00, 0x42, k_fesc, k_fend,           // FESC FEND: the frame ends anyway
                             0x00, k_fesc, k_fend};                // an empty frame with a bad escape: nothing
    const std::vector<Bytes> frames = decode_frames(decoder, Bytes(input, input + test::count_of(input)));
    REQUIRE(frames.size() == 2);
    CHECK(frames[0] == text("A"));
    CHECK(frames[1] == text("B"));
    CHECK_EQ(decoder.counters().bad_escapes, 3u);
    CHECK_EQ(decoder.counters().frames, 2u);
}

TEST(kiss_empty_frames_give_nothing) {
    const uint8_t input[] = {k_fend, 0x00, k_fend, k_fend, k_fend, 0x00, k_fend};
    KissDecoder decoder;
    CHECK(decode_frames(decoder, Bytes(input, input + test::count_of(input))).empty());
    CHECK_EQ(decoder.counters().frames, 0u);
}

TEST(kiss_peek_tells_what_feed_does) {
    const uint8_t input[] = {'n', k_fend, 0x00, 'a', k_fesc, k_tfesc, k_fesc, 'q', k_fend, 0x03, 0x10, k_fend};
    KissDecoder decoder;
    for (size_t i = 0; i < test::count_of(input); ++i) {
        uint8_t peeked = 0;
        uint8_t fed = 0;
        const KissStep peek = decoder.peek(input[i], peeked);
        const KissStep again = decoder.peek(input[i], peeked);
        const KissStep feed = decoder.feed(input[i], fed);
        CHECK(peek == again);
        CHECK(peek == feed);
        if (feed == KissStep::data) CHECK_EQ(static_cast<unsigned>(peeked), static_cast<unsigned>(fed));
    }
}

TEST(kiss_escape_writes_one_or_two_bytes) {
    for (unsigned value = 0; value < 256; ++value) {
        uint8_t out[unlimited::k_kiss_escaped_max];
        const uint8_t size = unlimited::kiss_escape(static_cast<uint8_t>(value), out);
        if (value == k_fend) {
            CHECK(size == 2 && out[0] == k_fesc && out[1] == k_tfend);
        } else if (value == k_fesc) {
            CHECK(size == 2 && out[0] == k_fesc && out[1] == k_tfesc);
        } else {
            CHECK(size == 1 && out[0] == value);
        }
    }
    // Every byte value through kiss_frame() and back.
    Bytes all;
    for (unsigned value = 0; value < 256; ++value) all.push_back(static_cast<uint8_t>(value));
    const std::vector<Bytes> frames = decode_frames(kiss_frame(all));
    REQUIRE(frames.size() == 1);
    CHECK(frames[0] == all);
}

// ---------------------------------------------------------------------------
// The send side: queue, frames, channel check, PTT
// ---------------------------------------------------------------------------

TEST(transmitter_one_frame_is_one_transmission) {
    Bench bench;
    const EncoderConfig signal = signal_at(k_fast_centi);
    ModemTransmitter tx(signal, eager_access(), &Bench::on_ptt, &bench, &Bench::on_wake);
    REQUIRE(tx.valid());
    const Bytes data = text("Hello, Unlimited!");
    const Bytes kiss = kiss_frame(data);
    CHECK_EQ(tx.host_input(kiss.data(), kiss.size()), kiss.size());
    run(tx, bench, transmission_ms(signal, data.size()) + 200);
    REQUIRE(bench.ptt.size() == 2);
    CHECK(bench.ptt[0].second && !bench.ptt[1].second);
    const ModemCounters counters = tx.counters();
    CHECK_EQ(counters.transmissions, 1u);
    CHECK_EQ(counters.bytes_sent, static_cast<uint32_t>(data.size()));
    CHECK_EQ(counters.queued_bytes, 0u);
    CHECK_EQ(counters.queued_frames, 0u);
    CHECK_EQ(counters.kiss.frames, 1u);
    CHECK(tx.channel_state() == ChannelState::idle);
    CHECK(!tx.transmitting());

    const Heard heard = decode_audio(bench.played, signal);
    CHECK_EQ(heard.locks, 1u);
    REQUIRE(heard.frames.size() == 1);
    CHECK(heard.frames[0] == data);
}

// Two frames in one write, sharing a FEND: two transmissions, each exactly its frame. The first transmission's length
// is its own frame's (Encoder::duration_samples): no byte of the second frame entered it.
TEST(transmitter_two_frames_are_two_transmissions) {
    Bench bench;
    const EncoderConfig signal = signal_at(k_fast_centi);
    ModemTransmitter tx(signal, eager_access(), &Bench::on_ptt, &bench, &Bench::on_wake);
    const Bytes first = text("first frame");
    const Bytes second = text("the second one");
    Bytes kiss = kiss_frame(first);
    const Bytes more = kiss_frame(second);
    kiss.insert(kiss.end(), more.begin() + 1, more.end());  // C0 00 first C0 00 second C0
    CHECK_EQ(tx.host_input(kiss.data(), kiss.size()), kiss.size());
    CHECK_EQ(tx.counters().queued_frames, 2u);
    run(tx, bench, transmission_ms(signal, first.size()) + transmission_ms(signal, second.size()) + 500);
    CHECK_EQ(bench.keys(), 2u);
    CHECK_EQ(tx.counters().transmissions, 2u);

    // Each key's audio: the lead-in, then windows, then the tail; the key to key spacing shows the first length.
    REQUIRE(bench.ptt.size() == 4);
    const uint32_t first_ms = transmission_ms(signal, first.size());
    const uint32_t on_air = bench.ptt[1].first - bench.ptt[0].first;
    CHECK(on_air >= first_ms && on_air <= first_ms + 2);
    const Heard heard = decode_audio(bench.played, signal);
    CHECK_EQ(heard.locks, 2u);
    REQUIRE(heard.frames.size() == 2);
    CHECK(heard.frames[0] == first);
    CHECK(heard.frames[1] == second);
}

// A frame goes on the air once it is complete: an open frame waits for its FEND however long.
TEST(transmitter_waits_for_the_end_of_a_frame) {
    Bench bench;
    const EncoderConfig signal = signal_at(k_fast_centi);
    ModemTransmitter tx(signal, eager_access(), &Bench::on_ptt, &bench, &Bench::on_wake);
    Bytes kiss = kiss_frame(text("open for a while"));
    const uint8_t closing = kiss.back();
    kiss.pop_back();
    CHECK_EQ(tx.host_input(kiss.data(), kiss.size()), kiss.size());
    run(tx, bench, 3000);
    CHECK(bench.ptt.empty());
    CHECK_EQ(tx.counters().queued_frames, 0u);
    CHECK_EQ(tx.host_input(&closing, 1), 1u);
    run(tx, bench, transmission_ms(signal, 16) + 100);
    CHECK_EQ(bench.keys(), 1u);
    CHECK_EQ(tx.counters().bytes_sent, 16u);
}

// The frame slots: 64 complete frames at most; the FEND of the 65th waits (back-pressure) and the computer is woken
// once a frame has gone out.
TEST(transmitter_frame_slots_back_pressure) {
    Bench bench;
    const EncoderConfig signal = signal_at(k_fast_centi);
    ModemTransmitter tx(signal, eager_access(), &Bench::on_ptt, &bench, &Bench::on_wake);
    Bytes kiss;
    const size_t frames = ModemTransmitter::k_frame_slots + 1;
    for (size_t i = 0; i < frames; ++i) {
        const Bytes one = kiss_frame(Bytes(1, static_cast<uint8_t>('a' + i % 26)));
        kiss.insert(kiss.end(), one.begin(), one.end());
    }
    const size_t taken = tx.host_input(kiss.data(), kiss.size());
    CHECK_EQ(taken, kiss.size() - 1);  // all but the last FEND
    CHECK_EQ(tx.counters().queued_frames, static_cast<uint32_t>(ModemTransmitter::k_frame_slots));
    CHECK_EQ(tx.counters().host_refusals, 1u);
    CHECK_EQ(bench.wakes_host, 0u);
    run(tx, bench, transmission_ms(signal, 1) + 100);
    CHECK_EQ(tx.counters().transmissions, 1u);
    CHECK_EQ(bench.wakes_host, 1u);
    CHECK_EQ(tx.host_input(&kiss[taken], 1), 1u);
    CHECK_EQ(tx.counters().queued_frames, static_cast<uint32_t>(ModemTransmitter::k_frame_slots));
}

// A frame longer than the send queue: the queue takes what fits and refuses the rest; the full queue starts the
// frame, which streams as the computer tops it up; the computer is woken each time a quarter of the queue is free.
TEST(transmitter_a_frame_longer_than_the_queue_streams) {
    Bench bench;
    bench.record = false;
    const EncoderConfig signal = signal_at(k_fast_centi);
    ModemTransmitter tx(signal, eager_access(), &Bench::on_ptt, &bench, &Bench::on_wake);
    const size_t size = ModemTransmitter::k_queue_size + ModemTransmitter::k_queue_size / 2;
    Bytes data(size);
    for (size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>('A' + i % 26);  // nothing to escape
    const Bytes kiss = kiss_frame(data);
    size_t offered = tx.host_input(kiss.data(), kiss.size());
    CHECK_EQ(offered, 2u + ModemTransmitter::k_queue_size);  // FEND, command, a full queue
    CHECK_EQ(tx.counters().host_refusals, 1u);
    const uint32_t per_byte_ms = unlimited::k_window_slots * signal.slot_us / k_us_per_ms;
    const uint32_t total_ms = transmission_ms(signal, size) + 200;
    for (uint32_t elapsed = 0; elapsed < total_ms; elapsed += per_byte_ms) {
        const uint32_t wakes = bench.wakes_host;
        run(tx, bench, per_byte_ms);
        if (bench.wakes_host != wakes && offered < kiss.size())
            offered += tx.host_input(&kiss[offered], kiss.size() - offered);
    }
    CHECK_EQ(offered, kiss.size());
    CHECK_EQ(bench.keys(), 1u);
    const ModemCounters counters = tx.counters();
    CHECK_EQ(counters.transmissions, 1u);
    CHECK_EQ(counters.bytes_sent, static_cast<uint32_t>(size));
    CHECK_EQ(counters.underruns, 0u);
    CHECK(bench.wakes_host >= 2u);
    NOTE("%zu bytes in one transmission, %u wake-ups of the computer, %u refusals", size, bench.wakes_host,
         counters.host_refusals);
}

// A streaming frame whose computer stops before its end: the transmission ends when the encoder runs dry, and the
// rest of that frame is dropped as it arrives (never a second transmission); the next frame is untouched.
TEST(transmitter_never_splits_a_frame) {
    Bench bench;
    bench.record = false;
    const EncoderConfig signal = signal_at(k_fast_centi);
    ModemTransmitter tx(signal, eager_access(), &Bench::on_ptt, &bench, &Bench::on_wake);
    const size_t size = ModemTransmitter::k_queue_size + 10;
    Bytes data(size, 'x');
    const Bytes kiss = kiss_frame(data);
    const size_t first = tx.host_input(kiss.data(), kiss.size());
    REQUIRE(first < kiss.size());
    run(tx, bench, transmission_ms(signal, ModemTransmitter::k_queue_size) + 500);
    CHECK_EQ(bench.keys(), 1u);
    CHECK_EQ(tx.counters().underruns, 1u);
    CHECK_EQ(tx.counters().bytes_sent, static_cast<uint32_t>(ModemTransmitter::k_queue_size));
    // The rest of the frame, then another frame.
    CHECK_EQ(tx.host_input(&kiss[first], kiss.size() - first), kiss.size() - first);
    const Bytes next = kiss_frame(text("next"));
    CHECK_EQ(tx.host_input(next.data(), next.size()), next.size());
    run(tx, bench, transmission_ms(signal, 4) + 500);
    CHECK_EQ(bench.keys(), 2u);
    const ModemCounters counters = tx.counters();
    CHECK_EQ(counters.transmissions, 2u);
    CHECK_EQ(counters.bytes_sent, static_cast<uint32_t>(ModemTransmitter::k_queue_size + 4));
    CHECK_EQ(counters.bytes_skipped, 10u);
}

// The channel check on a simulated clock (spec 12.1): dwait counts from the start, then one draw per slot time with a
// fixed seed: the key comes exactly when the generator first gives a number <= persist.
TEST(transmitter_channel_check_dwait_and_persistence) {
    Bench bench;
    const EncoderConfig signal = signal_at(k_fast_centi);
    AccessConfig access;
    access.seed = k_seed;
    ModemTransmitter tx(signal, access, &Bench::on_ptt, &bench, &Bench::on_wake);
    const Bytes kiss = kiss_frame(text("persist"));
    tx.host_input(kiss.data(), kiss.size());

    uint32_t state = k_seed;
    uint32_t failures = 0;
    while (next_draw(state) > access.persist) ++failures;
    const uint32_t expected = access.dwait_ms + failures * access.slot_time_ms;
    run(tx, bench, expected + 1);
    REQUIRE(!bench.ptt.empty());
    CHECK_EQ(bench.ptt[0].first, expected);
    CHECK_EQ(tx.counters().draws, failures + 1);
    CHECK_EQ(tx.counters().deferrals, failures);
    NOTE("seed %u: %u draws above persist %u, key at %u ms", k_seed, failures, access.persist, expected);
}

// DCD holds the transmitter: no key while the receiver hears a signal; dwait counts again from DCD off, and the
// draws start over.
TEST(transmitter_waits_while_dcd_is_on) {
    Bench bench;
    const EncoderConfig signal = signal_at(k_fast_centi);
    AccessConfig access;
    access.seed = k_seed;
    access.persist = 255;
    ModemTransmitter tx(signal, access, &Bench::on_ptt, &bench, &Bench::on_wake);
    const Bytes kiss = kiss_frame(text("busy"));
    tx.host_input(kiss.data(), kiss.size());
    const uint32_t busy_from = 700;
    const uint32_t busy_until = 4000;
    run(tx, bench, busy_from);
    tx.set_dcd(true);
    CHECK(bench.wakes_control > 0u);
    run(tx, bench, busy_until - busy_from);
    CHECK(bench.ptt.empty());
    CHECK(tx.channel_state() == ChannelState::waiting);
    tx.set_dcd(false);
    run(tx, bench, access.dwait_ms + 5);
    REQUIRE(!bench.ptt.empty());
    CHECK_EQ(bench.ptt[0].first, busy_until + access.dwait_ms);

    // DCD during the draws: the next draw waits for dwait after DCD off again.
    Bench second;
    AccessConfig slow = access;
    slow.persist = 0;
    slow.dwait_ms = 200;
    ModemTransmitter held(signal, slow, &Bench::on_ptt, &second, &Bench::on_wake);
    held.host_input(kiss.data(), kiss.size());
    run(held, second, 1000);
    const uint32_t draws = held.counters().draws;
    CHECK(draws >= 1u);
    held.set_dcd(true);
    run(held, second, 1000);
    CHECK_EQ(held.counters().draws, draws);
    held.set_dcd(false);
    run(held, second, slow.dwait_ms);
    CHECK_EQ(held.counters().draws, draws);
    run(held, second, 1);
    CHECK_EQ(held.counters().draws, draws + 1);
}

// Full duplex: no channel check, the frame keys at the first tick, DCD or not.
TEST(transmitter_full_duplex_keys_at_once) {
    Bench bench;
    const EncoderConfig signal = signal_at(k_fast_centi);
    AccessConfig access;
    access.full_duplex = true;
    access.persist = 0;
    ModemTransmitter tx(signal, access, &Bench::on_ptt, &bench, &Bench::on_wake);
    tx.set_dcd(true);
    run(tx, bench, 50);
    const Bytes kiss = kiss_frame(text("duplex"));
    tx.host_input(kiss.data(), kiss.size());
    run(tx, bench, 2);
    REQUIRE(!bench.ptt.empty());
    CHECK_EQ(bench.ptt[0].first, 50u);
    CHECK_EQ(tx.counters().draws, 0u);
}

// PTT sequencing: key, then the TX delay of silence, the windows, the tail; PTT off the output latency after the last
// sample of the tail. The same timeline with a tick every millisecond and event-driven.
TEST(transmitter_ptt_follows_the_audio) {
    const EncoderConfig signal = signal_at(k_default_centi);
    AccessConfig access = eager_access();
    access.output_latency_ms = 37;
    const Bytes data = text("PTT");
    for (int every_ms = 0; every_ms < 2; ++every_ms) {
        Bench bench;
        ModemTransmitter tx(signal, access, &Bench::on_ptt, &bench, &Bench::on_wake);
        const uint32_t start = 20;
        run(tx, bench, start, every_ms != 0);
        const Bytes kiss = kiss_frame(data);
        tx.host_input(kiss.data(), kiss.size());
        const uint32_t duration = Encoder(signal).duration_samples(data.size());
        run(tx, bench, duration / k_samples_per_ms + access.output_latency_ms + 50, every_ms != 0);
        REQUIRE(bench.ptt.size() == 2);
        const uint32_t key = bench.ptt[0].first;
        CHECK(key == start || key == start + 1);
        const std::vector<Burst> found = bursts(bench.played, k_samples_per_ms * k_ms_per_s);
        REQUIRE(found.size() == 1);
        // The first START comes exactly the TX delay after the audio began at the key.
        const size_t first_expected = static_cast<size_t>(key) * k_samples_per_ms + signal.lead_in_ms * k_samples_per_ms;
        CHECK(found[0].first >= first_expected && found[0].first < first_expected + k_samples_per_ms);
        // PTT off: the transmission (duration samples from the key) plus the latency, within a tick.
        const uint32_t end_ms = (key * k_samples_per_ms + duration + k_samples_per_ms - 1) / k_samples_per_ms;
        const uint32_t off = bench.ptt[1].first;
        CHECK(off >= end_ms + access.output_latency_ms && off <= end_ms + access.output_latency_ms + 2);
        NOTE("%s: key %u ms, first START %.1f ms, PTT off %u ms (audio end %u ms + %u ms latency)",
             every_ms ? "tick every ms" : "event-driven", key, found[0].first / static_cast<double>(k_samples_per_ms),
             off, end_ms, access.output_latency_ms);
    }
}

// With VOX the lead tone starts at the key: the audio is not silent in the first millisecond.
TEST(transmitter_vox_lead_keys_the_radio) {
    Bench bench;
    EncoderConfig signal = signal_at(k_default_centi);
    signal.lead_in_ms = 0;
    signal.vox_lead_ms = unlimited::k_default_vox_lead_ms;
    ModemTransmitter tx(signal, eager_access(), &Bench::on_ptt, &bench, &Bench::on_wake);
    const Bytes kiss = kiss_frame(text("vox"));
    tx.host_input(kiss.data(), kiss.size());
    run(tx, bench, transmission_ms(signal, 3) + 100);
    REQUIRE(!bench.ptt.empty());
    const size_t key = static_cast<size_t>(bench.ptt[0].first) * k_samples_per_ms;
    bool sound = false;
    for (size_t i = key; i < key + k_samples_per_ms; ++i) sound = sound || bench.played[i] != 0;
    CHECK(sound);
    const Heard heard = decode_audio(bench.played, signal);
    REQUIRE(heard.frames.size() == 1);
    CHECK(heard.frames[0] == text("vox"));
}

// Between two of its own transmissions the modem leaves the silence a receiver needs to end the first: a whole window
// and 2 slots after the last STOP (2 windows with the fade bridge's receivers), the tail and a silent lead-in counted,
// a VOX lead not. Without it a VOX lead right after a 100 ms tail reads as the old transmission going on (spec 12.1).
TEST(transmitter_spaces_its_own_transmissions) {
    const uint8_t windows[] = {1, 2};
    for (size_t w = 0; w < test::count_of(windows); ++w) {
        Bench bench;
        EncoderConfig signal = signal_at(k_default_centi);
        signal.lead_in_ms = 0;
        signal.vox_lead_ms = unlimited::k_default_vox_lead_ms;
        AccessConfig access = eager_access();
        access.end_windows = windows[w];
        ModemTransmitter tx(signal, access, &Bench::on_ptt, &bench, &Bench::on_wake);
        Bytes kiss = kiss_frame(text("one"));
        const Bytes second = kiss_frame(text("two"));
        kiss.insert(kiss.end(), second.begin(), second.end());
        tx.host_input(kiss.data(), kiss.size());
        run(tx, bench, 2 * transmission_ms(signal, 3) + 2000);
        CHECK_EQ(bench.keys(), 2u);
        const std::vector<Burst> found = bursts(bench.played, k_samples_per_ms * unlimited::k_default_tail_ms);
        REQUIRE(found.size() == 2);
        const double slot_samples = signal.slot_us / static_cast<double>(k_us_per_ms) * k_samples_per_ms;
        const double gap_slots = (found[1].first - found[0].last) / slot_samples;
        const double needed = windows[w] * unlimited::k_window_slots + unlimited::k_end_margin_slots;
        CHECK(gap_slots >= needed - 0.1);
        CHECK(gap_slots <= needed + 1.0);
        NOTE("VOX, %u end window(s): %.1f slots of silence between the last STOP and the next lead tone", windows[w],
             gap_slots);
    }
    // A TX delay longer than the silence needed adds nothing: the second key follows the first release at once.
    Bench bench;
    const EncoderConfig signal = signal_at(k_fast_centi);
    ModemTransmitter tx(signal, eager_access(), &Bench::on_ptt, &bench, &Bench::on_wake);
    Bytes kiss = kiss_frame(text("a"));
    const Bytes second = kiss_frame(text("b"));
    kiss.insert(kiss.end(), second.begin(), second.end());
    tx.host_input(kiss.data(), kiss.size());
    run(tx, bench, 2 * transmission_ms(signal, 1) + 200);
    REQUIRE(bench.ptt.size() == 4);
    CHECK(bench.ptt[2].first - bench.ptt[1].first <= 1u);
}

// The frame on the air, for display (the --tui view): its size once its end is known, and its windows sent so far,
// from 0 during the TX delay to its size; nothing once its audio has ended.
TEST(transmitter_reports_the_frame_on_the_air) {
    Bench bench;
    const EncoderConfig signal = signal_at(k_fast_centi);
    ModemTransmitter tx(signal, eager_access(), &Bench::on_ptt, &bench, &Bench::on_wake);
    const Bytes data(10, 'p');
    const Bytes kiss = kiss_frame(data);
    tx.host_input(kiss.data(), kiss.size());
    run(tx, bench, signal.lead_in_ms / 2);
    ModemCounters counters = tx.counters();
    CHECK(tx.channel_state() == ChannelState::keyed);
    CHECK_EQ(counters.on_air_size, 10u);
    CHECK_EQ(counters.on_air_sent, 0u);
    const uint32_t window_ms = unlimited::k_window_slots * signal.slot_us / k_us_per_ms;
    run(tx, bench, signal.lead_in_ms / 2 + 4 * window_ms + 2);
    counters = tx.counters();
    CHECK_EQ(counters.on_air_sent, 4u);
    run(tx, bench, transmission_ms(signal, data.size()));
    counters = tx.counters();
    CHECK_EQ(counters.on_air_size, 0u);
    CHECK_EQ(counters.on_air_sent, 0u);
    CHECK_EQ(counters.transmissions, 1u);
}

TEST(transmitter_invalid_config_is_inert) {
    Bench bench;
    EncoderConfig signal = signal_at(k_default_centi);
    signal.sample_rate_hz = 48000;  // the modem runs at 8000 Hz
    ModemTransmitter tx(signal, eager_access(), &Bench::on_ptt, &bench, &Bench::on_wake);
    CHECK(!tx.valid());
    const Bytes kiss = kiss_frame(text("nothing"));
    CHECK_EQ(tx.host_input(kiss.data(), kiss.size()), 0u);
    run(tx, bench, 500);
    CHECK(bench.ptt.empty());
    CHECK(std::count(bench.played.begin(), bench.played.end(), 0) == static_cast<std::ptrdiff_t>(bench.played.size()));
    AccessConfig access = eager_access();
    access.slot_time_ms = 0;
    CHECK(!ModemTransmitter(signal_at(k_default_centi), access, nullptr, nullptr).valid());
}

// ---------------------------------------------------------------------------
// The modem: receiver streaming, half duplex, DCD
// ---------------------------------------------------------------------------

namespace {

// Feeds a modem audio in 1 ms steps while it plays, from sample `from` of `input` (zeros past its end).
void listen(Modem& modem, Bench& bench, const std::vector<int16_t>& input, size_t& from, uint32_t ms) {
    int16_t out[k_samples_per_ms];
    int16_t in[k_samples_per_ms];
    for (uint32_t i = 0; i < ms; ++i) {
        const uint32_t next = modem.next_tick_ms();
        const bool due = next != unlimited::k_no_tick && static_cast<int32_t>(bench.now_ms - next) >= 0;
        if (bench.wake_pending || due) {
            bench.wake_pending = false;
            modem.tick(bench.now_ms);
        }
        for (uint32_t s = 0; s < k_samples_per_ms; ++s) in[s] = from + s < input.size() ? input[from + s] : 0;
        from += k_samples_per_ms;
        bench.heard += k_samples_per_ms;
        modem.audio_input(in, k_samples_per_ms);
        modem.audio_output(out, k_samples_per_ms);
        bench.played.insert(bench.played.end(), out, out + k_samples_per_ms);
        ++bench.now_ms;
    }
}

}  // namespace

// Received bytes go to the computer as they are decoded: C0 00 at the lock, each byte escaped, C0 at the end. The
// first data bytes arrive while the transmission is still on the air (nothing is held for the frame's end). With the
// minimum frame off (V23: 0), from the first byte on.
TEST(modem_streams_what_it_hears_to_the_computer) {
    Bench bench;
    ModemConfig config = modem_at(k_default_centi);
    config.min_frame_bytes = 0;
    Modem modem(config, &Bench::on_host, &Bench::on_ptt, &bench, &Bench::on_wake);
    REQUIRE(modem.valid());
    Bytes data = text("KISS C0 and DB inside: ");
    data.push_back(k_fend);
    data.push_back(k_fesc);
    data.push_back('.');
    const loopback::Recording recording = loopback::single(data, config.signal);
    size_t from = 0;
    listen(modem, bench, recording.samples, from, static_cast<uint32_t>(recording.samples.size() / k_samples_per_ms) + 1000);
    const std::vector<Bytes> frames = decode_frames(bench.host);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0] == data);
    REQUIRE(bench.host.size() > 4);
    CHECK(bench.host[0] == k_fend && bench.host[1] == unlimited::k_kiss_data);
    CHECK(bench.host.back() == k_fend);
    // The byte for the frame's 5th byte reached the computer before the transmission's audio had ended.
    const loopback::Transmission& tx = recording.transmissions[0];
    const size_t fifth = 2 + 4;  // C0 00 and four plain bytes before it
    CHECK(bench.host_sample[fifth] < tx.start_sample + tx.length);
    const ModemCounters counters = modem.counters();
    CHECK_EQ(counters.frames_received, 1u);
    CHECK_EQ(counters.bytes_received, static_cast<uint32_t>(data.size()));
    CHECK_EQ(counters.ends, 1u);
    CHECK_EQ(counters.losses, 0u);
    NOTE("byte 4 reached the computer %.0f ms before the end of the transmission's audio",
         (static_cast<double>(tx.start_sample + tx.length) - static_cast<double>(bench.host_sample[fifth])) /
             k_samples_per_ms);
}

namespace {

// The host bytes of a frame heard alone: where its data byte `index` (plain text: no KISS escapes) came, in samples.
uint64_t data_byte_sample(const Bench& bench, size_t index) {
    return bench.host_sample[2 + index];  // after C0 00
}

// Transmissions of these texts, 1.5 s apart, heard by a modem of `config`: its bench.
Bench hear(const ModemConfig& config, const std::vector<Bytes>& frames, Modem*& kept) {
    Bench bench;
    kept = new Modem(config, &Bench::on_host, &Bench::on_ptt, &bench, &Bench::on_wake);
    loopback::Recording recording;
    loopback::append_silence(recording, loopback::leading_silence_ms(config.signal));
    for (size_t i = 0; i < frames.size(); ++i) {
        loopback::append_transmission(recording, frames[i], config.signal);
        loopback::append_silence(recording, 1500.0);
    }
    size_t from = 0;
    listen(*kept, bench, recording.samples, from, static_cast<uint32_t>(recording.samples.size() / k_samples_per_ms) + 1000);
    return bench;
}

}  // namespace

// V23, the default minimum frame (15 bytes, the shortest AX.25 frame): a reception's first 14 bytes wait; C0 00 and
// the first 15 bytes reach the computer together when the 15th is decoded, in order, and the rest streams as decoded.
// The first byte comes 14 windows later than without the minimum (2.33 s at 6 bytes/s).
TEST(modem_min_frame_holds_then_streams) {
    const ModemConfig config = modem_at(k_default_centi);
    CHECK_EQ(static_cast<unsigned>(config.min_frame_bytes), static_cast<unsigned>(unlimited::k_default_min_frame_bytes));
    CHECK_EQ(static_cast<unsigned>(ModemConfig().min_frame_bytes), 15u);
    const std::vector<Bytes> frames(1, text("a frame of AX.25 size, and more bytes after its 15th"));
    Modem* held = nullptr;
    const Bench bench = hear(config, frames, held);
    ModemConfig streaming_config = config;
    streaming_config.min_frame_bytes = 0;
    Modem* streaming = nullptr;
    const Bench plain = hear(streaming_config, frames, streaming);
    const std::vector<Bytes> got = decode_frames(bench.host);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == frames[0]);
    CHECK(decode_frames(plain.host) == frames);
    const size_t minimum = unlimited::k_default_min_frame_bytes;
    // C0 00 and the first 15 bytes in one piece, when the 15th was decoded: exactly when the streaming modem gave it.
    for (size_t i = 0; i < 2 + minimum; ++i) CHECK_EQ(bench.host_sample[i], data_byte_sample(plain, minimum - 1));
    // The 16th and later: as decoded, as the streaming modem gave them.
    for (size_t i = minimum; i < frames[0].size(); ++i) CHECK_EQ(data_byte_sample(bench, i), data_byte_sample(plain, i));
    const double window = unlimited::k_window_slots * config.signal.slot_us / 1000.0 * k_samples_per_ms;
    const double waited = static_cast<double>(data_byte_sample(bench, 0) - data_byte_sample(plain, 0));
    NOTE("the first byte reached the computer %.2f windows (%.2f s) later than without the minimum frame",
         waited / window, waited / unlimited::k_modem_rate_hz);
    CHECK(std::fabs(waited / window - static_cast<double>(minimum - 1)) < 0.5);
    const ModemCounters counters = held->counters();
    CHECK_EQ(counters.frames_received, 1u);
    CHECK_EQ(counters.bytes_received, static_cast<uint32_t>(frames[0].size()));
    CHECK_EQ(counters.short_frames, 0u);
    delete held;
    delete streaming;
}

// V23: receptions shorter than the minimum frame never reach the computer, not even C0 00 or C0 (a stray from speech
// or CW is a byte or two, V20): 1 and 14 bytes dropped, the 15-byte one byte for byte; with the minimum off (0), all
// three stream, the 1-byte one included.
TEST(modem_min_frame_drops_short_receptions) {
    std::vector<Bytes> frames;
    frames.push_back(text("k"));
    frames.push_back(text("14 bytes here."));
    frames.push_back(text("15 bytes, here."));
    REQUIRE(frames[1].size() == 14 && frames[2].size() == 15);
    ModemConfig config = modem_at(k_default_centi);
    Modem* modem = nullptr;
    const Bench bench = hear(config, frames, modem);
    const std::vector<Bytes> got = decode_frames(bench.host);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == frames[2]);
    CHECK_EQ(bench.host.size(), 2u + 15u + 1u);  // C0 00, the 15 bytes, C0: nothing of the short ones
    ModemCounters counters = modem->counters();
    CHECK_EQ(counters.frames_received, 1u);
    CHECK_EQ(counters.bytes_received, 15u);
    CHECK_EQ(counters.ends, 1u);
    CHECK_EQ(counters.short_frames, 2u);
    CHECK_EQ(counters.short_bytes, 15u);
    delete modem;

    config.min_frame_bytes = 0;
    const Bench open = hear(config, frames, modem);
    CHECK(decode_frames(open.host) == frames);
    counters = modem->counters();
    CHECK_EQ(counters.frames_received, 3u);
    CHECK_EQ(counters.short_frames, 0u);
    // C0 00 and byte 0 in one piece: byte 0 comes with the lock (V20), nothing waits.
    CHECK_EQ(open.host_sample[0], open.host_sample[2]);
    delete modem;
    ModemConfig too_long = modem_at(k_default_centi);
    too_long.min_frame_bytes = unlimited::k_max_min_frame_bytes + 1;
    CHECK(!too_long.valid());
}

// Half duplex: while PTT is keyed the receiver hears silence, so another station's transmission heard meanwhile never
// reaches the computer; full duplex hears it.
TEST(modem_half_duplex_drops_what_is_heard_while_sending) {
    for (int full = 0; full < 2; ++full) {
        Bench bench;
        ModemConfig config = modem_at(k_default_centi);
        config.access.full_duplex = full != 0;
        Modem modem(config, &Bench::on_host, &Bench::on_ptt, &bench, &Bench::on_wake);
        // Our frame is long enough to cover the other station's whole transmission.
        const Bytes ours(40, 'o');
        const Bytes kiss = kiss_frame(ours);
        modem.host_input(kiss.data(), kiss.size());
        const Bytes theirs = text("the other station");
        const loopback::Recording recording = loopback::single(theirs, config.signal);
        size_t from = 0;
        listen(modem, bench, recording.samples, from, transmission_ms(config.signal, ours.size()) + 3000);
        CHECK_EQ(bench.keys(), 1u);
        const std::vector<Bytes> frames = decode_frames(bench.host);
        if (full != 0) {
            REQUIRE(frames.size() == 1);
            CHECK(frames[0] == theirs);
            CHECK_EQ(modem.counters().muted_samples, 0u);
        } else {
            CHECK(frames.empty());
            CHECK(modem.counters().muted_samples > 0u);
        }
    }
}

// A frame waits while the receiver decodes a transmission, and goes out dwait after it ended.
TEST(modem_waits_for_the_channel) {
    Bench bench;
    ModemConfig config = modem_at(k_default_centi);
    config.access.dwait_ms = 500;
    Modem modem(config, &Bench::on_host, &Bench::on_ptt, &bench, &Bench::on_wake);
    const Bytes theirs = text("a transmission already on the air");
    const loopback::Recording recording = loopback::single(theirs, config.signal);
    const loopback::Transmission& tx = recording.transmissions[0];
    size_t from = 0;
    // The other station is sending; our frame arrives in the middle of its transmission.
    const uint32_t middle_ms = static_cast<uint32_t>((tx.start_sample + tx.length / 2) / k_samples_per_ms);
    listen(modem, bench, recording.samples, from, middle_ms);
    CHECK(modem.dcd());
    const Bytes kiss = kiss_frame(text("reply"));
    modem.host_input(kiss.data(), kiss.size());
    listen(modem, bench, recording.samples, from, 6000);
    REQUIRE(!bench.ptt.empty());
    const std::vector<Bytes> frames = decode_frames(bench.host);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0] == theirs);
    // The key came after the reception closed (its C0) plus dwait.
    const uint32_t closed_ms = static_cast<uint32_t>(bench.host_sample.back() / k_samples_per_ms);
    CHECK(bench.ptt[0].first >= closed_ms + config.access.dwait_ms);
    CHECK(bench.ptt[0].first <= closed_ms + config.access.dwait_ms + 2);
    NOTE("their audio ended at %zu ms, the reception closed at %u ms, our key at %u ms",
         (tx.start_sample + tx.length) / k_samples_per_ms, closed_ms, bench.ptt[0].first);
}

// The fade bridge (spec 12.1): the TX delay becomes at least 300 ms of silence, unless a VOX lead precedes the START.
TEST(modem_fade_bridge_lengthens_a_silent_lead_in) {
    ModemConfig config = modem_at(k_default_centi);
    config.signal.lead_in_ms = 50;
    CHECK_EQ(config.sent().lead_in_ms, 50u);
    config.fade_bridge = true;
    CHECK_EQ(config.sent().lead_in_ms, unlimited::k_fade_bridge_silence_ms);
    config.signal.lead_in_ms = 500;
    CHECK_EQ(config.sent().lead_in_ms, 500u);
    config.signal.lead_in_ms = 0;
    config.signal.vox_lead_ms = unlimited::k_default_vox_lead_ms;
    CHECK_EQ(config.sent().lead_in_ms, 0u);
    CHECK(!unlimited::k_default_fade_bridge);
    CHECK(!ModemConfig().fade_bridge);

    Bench bench;
    ModemConfig bridged = modem_at(k_fast_centi);
    bridged.signal.lead_in_ms = 0;
    bridged.fade_bridge = true;
    Modem modem(bridged, &Bench::on_host, &Bench::on_ptt, &bench, &Bench::on_wake);
    CHECK_EQ(modem.signal().lead_in_ms, unlimited::k_fade_bridge_silence_ms);
    const Bytes kiss = kiss_frame(text("bridge"));
    modem.host_input(kiss.data(), kiss.size());
    run(modem, bench, 1000);
    REQUIRE(!bench.ptt.empty());
    const std::vector<Burst> found = bursts(bench.played, k_samples_per_ms * k_ms_per_s);
    REQUIRE(!found.empty());
    CHECK(found[0].first >= (bench.ptt[0].first + unlimited::k_fade_bridge_silence_ms) * k_samples_per_ms);
}

TEST(modem_config_checks_both_halves) {
    ModemConfig config;
    CHECK(config.valid());
    CHECK(config.receiver.decision_mode == unlimited::DecisionMode::adaptive);
    CHECK_EQ(config.signal.lead_in_ms, unlimited::k_default_txdelay_ms);
    CHECK_EQ(config.receiver.slot_us, config.signal.slot_us);
    ModemConfig mismatch = config;
    mismatch.receiver.slot_us = unlimited::slot_us_for_centi_speed(k_fast_centi);  // two speeds
    CHECK(!mismatch.valid());
    Bench bench;
    Modem inert(mismatch, &Bench::on_host, &Bench::on_ptt, &bench, &Bench::on_wake);
    CHECK(!inert.valid());
    const Bytes kiss = kiss_frame(text("x"));
    CHECK_EQ(inert.host_input(kiss.data(), kiss.size()), 0u);
    ModemConfig rate = config;
    rate.signal.sample_rate_hz = 48000;
    CHECK(!rate.valid());
}

TEST(modem_sizes) {
    NOTE("sizeof Modem %zu B (send queue %u B, decoder %zu B), ModemTransmitter %zu B, KissDecoder %zu B, Encoder %zu B",
         sizeof(Modem), static_cast<unsigned>(ModemTransmitter::k_queue_size), sizeof(Decoder),
         sizeof(ModemTransmitter), sizeof(KissDecoder), sizeof(Encoder));
    CHECK(sizeof(ModemTransmitter) < ModemTransmitter::k_queue_size + 1024u);
}
