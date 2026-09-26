// Heap trap (spec 8.7 B2): the whole core runs the L1' clean loopback, a packet round trip (a short, an
// AX.25-size and a maximum-size packet) and a WAV round trip while every C++ allocation function aborts.
// Built by 'make check_embedded' from this file and src/ only, with -fno-exceptions -fno-rtti; malloc and
// friends are covered by its nm check.
#include "unlimited.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

[[noreturn]] void trap(const char* what) {
    fputs("heap_trap: ", stderr);
    fputs(what, stderr);
    fputs(" called\n", stderr);
    abort();
}

}  // namespace

void* operator new(size_t) {
    trap("operator new");
}

void* operator new[](size_t) {
    trap("operator new[]");
}

void* operator new(size_t, const std::nothrow_t&) noexcept {
    trap("operator new(nothrow)");
}

void* operator new[](size_t, const std::nothrow_t&) noexcept {
    trap("operator new[](nothrow)");
}

void operator delete(void*) noexcept {
    trap("operator delete");
}

void operator delete[](void*) noexcept {
    trap("operator delete[]");
}

void operator delete(void*, const std::nothrow_t&) noexcept {
    trap("operator delete(nothrow)");
}

void operator delete[](void*, const std::nothrow_t&) noexcept {
    trap("operator delete[](nothrow)");
}

namespace {

using namespace unlimited;

const size_t k_max_bytes = 1000;
const size_t k_chunk_samples = 37;              // odd on purpose; shorter than any frame, so the queue never runs dry
const uint32_t k_lead_silence_ms = 200;
const uint32_t k_max_trailing_ms = 3000;        // END arrives about 2.4 T after the last STOP
const uint32_t k_ms_per_s = 1000;
const float k_us_per_ms_f = 1000.0f;
const float k_slot_tolerance = 0.005f;          // L1: measured slot_ms within 0.5 %
const float k_percent = 100.0f;
const uint32_t k_random_seed = 0x2545F491u;
const uint32_t k_wav_buffer_bytes = 64 * 1024;

struct Case {
    const char* name;
    bool preset;
    Preset preset_value;
    // Custom mode (preset false): T, k, N, spacing, side and f_ref as the sender sets them.
    uint32_t slot_us;
    uint8_t bits_per_peak;
    uint8_t data_slots;
    Spacing spacing;
    GridSide side;
    uint16_t tone_hz;
    Profile profile;
    size_t bytes;
};

// L1': every preset, then T = 6, 12, 20, 37, 100 and 128 ms with other (k, N), both spacings and both sides.
const Case k_cases[] = {
    {"fm_fast", true, Preset::fm_fast, 0, 0, 0, Spacing::standard, GridSide::below, 0, Profile::fm, 1000},
    {"fm", true, Preset::fm, 0, 0, 0, Spacing::standard, GridSide::below, 0, Profile::am, 1000},
    {"hf_fast", true, Preset::hf_fast, 0, 0, 0, Spacing::standard, GridSide::below, 0, Profile::ssb, 1000},
    {"hf", true, Preset::hf, 0, 0, 0, Spacing::standard, GridSide::below, 0, Profile::ssb, 400},
    {"hf_robust", true, Preset::hf_robust, 0, 0, 0, Spacing::standard, GridSide::below, 0, Profile::ssb, 120},
    {"hf_weak", true, Preset::hf_weak, 0, 0, 0, Spacing::standard, GridSide::below, 0, Profile::ssb, 120},
    {"T6 k2 N16", false, Preset::hf, 6000, 2, 16, Spacing::standard, GridSide::below, 2650, Profile::fm, 300},
    {"T12 k4 N32", false, Preset::hf, 12000, 4, 32, Spacing::standard, GridSide::above, 600, Profile::am, 600},
    {"T20 k1 N32", false, Preset::hf, 20000, 1, 32, Spacing::standard, GridSide::below, 1500, Profile::ssb, 60},
    {"T20 k5 N16", false, Preset::hf, 20000, 5, 16, Spacing::standard, GridSide::below, 2400, Profile::ssb, 400},
    {"T37 k6 N16", false, Preset::hf, 37000, 6, 16, Spacing::standard, GridSide::above, 450, Profile::ssb, 300},
    {"T100 k7 N8", false, Preset::hf, 100000, 7, 8, Spacing::dense, GridSide::below, 2200, Profile::ssb, 100},
    {"T128 k8 N8", false, Preset::hf, 128000, 8, 8, Spacing::dense, GridSide::above, 400, Profile::ssb, 100},
};
const size_t k_case_count = sizeof(k_cases) / sizeof(k_cases[0]);

const char k_packet_text[] = "CQ CQ DE UNLIMITED HEAP TRAP 0123456789";
const size_t k_text_size = sizeof(k_packet_text) - 1;
const size_t k_ax25_payload = 330;  // an AX.25 frame as KISS carries it
const size_t k_packet_sizes[] = {k_text_size, k_ax25_payload, k_packet_max_payload};
const size_t k_packet_count = sizeof(k_packet_sizes) / sizeof(k_packet_sizes[0]);
const size_t k_payload_bytes = k_text_size + k_ax25_payload + k_packet_max_payload;
const char k_wav_text[] = "WAV ROUND TRIP";

struct Tally {
    const uint8_t* expected;
    size_t size;
    size_t received;
    size_t wrong;
    size_t extra;
    size_t locks;
    size_t ends;
    size_t losts;
    float slot_ms;
    float worst_slot_error;
    uint8_t bits_per_peak;  // mode of the locked event
    uint8_t data_slots;
    PacketReader* packets;
};

struct PacketResult {
    size_t count;
    size_t matched;
    size_t offset;  // of the next expected payload in g_payloads
};

uint8_t g_data[k_max_bytes];
int16_t g_silence[k_chunk_samples];
uint8_t g_wav[k_wav_buffer_bytes];
uint8_t g_payloads[k_payload_bytes];
uint8_t g_packets[k_payload_bytes + k_packet_count * k_packet_overhead];

uint32_t next_random(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float absolute(float value) {
    return value < 0.0f ? -value : value;
}

void reset_tally(Tally& tally, const uint8_t* expected, size_t size, float slot_ms, PacketReader* packets) {
    memset(&tally, 0, sizeof(tally));
    tally.expected = expected;
    tally.size = size;
    tally.slot_ms = slot_ms;
    tally.packets = packets;
}

void on_event(const Event& event, void* context) {
    Tally& tally = *static_cast<Tally*>(context);
    if (tally.packets != nullptr) tally.packets->on_event(event);
    switch (event.type) {
        case EventType::locked:
            ++tally.locks;
            tally.bits_per_peak = event.bits_per_peak;
            tally.data_slots = event.data_slots;
            break;
        case EventType::end:
            ++tally.ends;
            break;
        case EventType::lost:
            ++tally.losts;
            break;
        case EventType::byte: {
            if (tally.received >= tally.size) {
                ++tally.extra;
                break;
            }
            if (event.value != tally.expected[tally.received]) ++tally.wrong;
            ++tally.received;
            const float error = absolute(event.slot_ms / tally.slot_ms - 1.0f);
            if (error > tally.worst_slot_error) tally.worst_slot_error = error;
            break;
        }
        case EventType::state:
        case EventType::slot:
            break;
    }
}

void on_packet(const uint8_t* payload, uint16_t size, uint8_t, void* context) {
    PacketResult& result = *static_cast<PacketResult*>(context);
    if (result.count < k_packet_count && size == k_packet_sizes[result.count] &&
        memcmp(payload, g_payloads + result.offset, size) == 0)
        ++result.matched;
    result.offset += size;
    ++result.count;
}

void feed_silence(Decoder& decoder, uint32_t ms) {
    const uint32_t samples = ms * (k_decoder_rate_hz / k_ms_per_s);
    for (uint32_t done = 0; done < samples; done += k_chunk_samples) decoder.process(g_silence, k_chunk_samples);
}

// Silence until END, bounded.
void drain(Decoder& decoder, const Tally& tally) {
    const uint32_t samples = k_max_trailing_ms * (k_decoder_rate_hz / k_ms_per_s);
    for (uint32_t done = 0; done < samples && tally.ends == 0; done += k_chunk_samples)
        decoder.process(g_silence, k_chunk_samples);
}

// The MCU pattern: the encoder queue is topped up while it renders, each chunk goes straight to the decoder.
bool stream(Encoder& encoder, Decoder& decoder, const uint8_t* data, size_t size) {
    size_t written = encoder.write(data, size);
    if (!encoder.start()) return false;
    int16_t chunk[k_chunk_samples];
    while (true) {
        written += encoder.write(data + written, size - written);
        const size_t rendered = encoder.render(chunk, k_chunk_samples);
        if (rendered == 0) break;
        decoder.process(chunk, rendered);
    }
    return written == size;
}

bool clean_run(const Tally& tally) {
    return tally.received == tally.size && tally.wrong == 0 && tally.extra == 0 && tally.locks == 1 &&
           tally.ends == 1 && tally.losts == 0 && tally.worst_slot_error <= k_slot_tolerance;
}

void print_tally(const char* name, const Tally& tally, bool pass) {
    printf("  %-10s %4zu bytes: received %zu wrong %zu extra %zu locks %zu ends %zu lost %zu, "
           "slot error %.3f %%  %s\n",
           name, tally.size, tally.received, tally.wrong, tally.extra, tally.locks, tally.ends, tally.losts,
           static_cast<double>(tally.worst_slot_error * k_percent), pass ? "ok" : "FAIL");
}

EncoderConfig case_config(const Case& test_case) {
    if (test_case.preset) return EncoderConfig::from_preset(test_case.preset_value, k_decoder_rate_hz);
    EncoderConfig config = EncoderConfig::from_preset(Preset::hf, k_decoder_rate_hz);
    config.slot_us = test_case.slot_us;
    config.bits_per_peak = test_case.bits_per_peak;
    config.data_slots = test_case.data_slots;
    config.spacing = test_case.spacing;
    config.side = test_case.side;
    config.tone_hz = test_case.tone_hz;
    return config;
}

// A build with smaller caps (UNLIMITED_MAX_BITS_PER_PEAK, UNLIMITED_MAX_FRAME_BYTES) refuses larger modes.
bool within_caps(const EncoderConfig& config) {
    return config.bits_per_peak <= UNLIMITED_MAX_BITS_PER_PEAK && config.frame_bytes() <= UNLIMITED_MAX_FRAME_BYTES;
}

bool run_case(const Case& test_case, uint32_t& random_state) {
    for (size_t i = 0; i < test_case.bytes; ++i) g_data[i] = static_cast<uint8_t>(next_random(random_state));
    const EncoderConfig config = case_config(test_case);
    if (!config.valid()) {
        printf("  %-10s invalid encoder configuration  FAIL\n", test_case.name);
        return false;
    }
    if (!within_caps(config)) {
        printf("  %-10s over the decoder caps (%u, %u): skipped\n", test_case.name,
               static_cast<unsigned>(UNLIMITED_MAX_BITS_PER_PEAK), static_cast<unsigned>(UNLIMITED_MAX_FRAME_BYTES));
        return true;
    }
    Tally tally;
    reset_tally(tally, g_data, test_case.bytes, static_cast<float>(config.slot_us) / k_us_per_ms_f, nullptr);
    Encoder encoder(config);
    Decoder decoder(DecoderConfig::for_profile(test_case.profile), &on_event, &tally);
    feed_silence(decoder, k_lead_silence_ms);
    const bool streamed = stream(encoder, decoder, g_data, test_case.bytes);
    drain(decoder, tally);
    const bool pass = streamed && clean_run(tally) && tally.bits_per_peak == config.bits_per_peak &&
                      tally.data_slots == config.data_slots;
    print_tally(test_case.name, tally, pass);
    return pass;
}

// The three packets back to back in one transmission.
bool packet_round_trip(uint32_t& random_state) {
    memcpy(g_payloads, k_packet_text, k_text_size);
    for (size_t i = k_text_size; i < k_payload_bytes; ++i)
        g_payloads[i] = static_cast<uint8_t>(next_random(random_state));
    size_t size = 0;
    size_t offset = 0;
    for (size_t i = 0; i < k_packet_count; ++i) {
        size += packet_build(g_payloads + offset, static_cast<uint16_t>(k_packet_sizes[i]), g_packets + size,
                             sizeof(g_packets) - size);
        offset += k_packet_sizes[i];
    }
    PacketResult result = {0, 0, 0};
    PacketReader reader(&on_packet, &result);
    const EncoderConfig config = EncoderConfig::from_preset(Preset::hf_fast, k_decoder_rate_hz);
    Tally tally;
    reset_tally(tally, g_packets, size, static_cast<float>(config.slot_us) / k_us_per_ms_f, &reader);
    Encoder encoder(config);
    Decoder decoder(DecoderConfig(), &on_event, &tally);
    feed_silence(decoder, k_lead_silence_ms);
    const bool streamed = size == sizeof(g_packets) && stream(encoder, decoder, g_packets, size);
    drain(decoder, tally);
    const bool pass = streamed && clean_run(tally) && result.count == k_packet_count &&
                      result.matched == k_packet_count && reader.crc_errors() == 0;
    print_tally("packet", tally, pass);
    return pass;
}

class MemoryByteSink : public ByteSink {
public:
    MemoryByteSink(uint8_t* buffer, uint32_t capacity) : buffer_(buffer), capacity_(capacity), position_(0), size_(0) {}

    bool write(const uint8_t* data, size_t size) override {
        if (size > capacity_ - position_) return false;
        memcpy(buffer_ + position_, data, size);
        position_ += static_cast<uint32_t>(size);
        if (position_ > size_) size_ = position_;
        return true;
    }

    bool seek(uint32_t position) override {
        if (position > size_) return false;
        position_ = position;
        return true;
    }

    uint32_t size() const { return size_; }

private:
    uint8_t* buffer_;
    uint32_t capacity_;
    uint32_t position_;
    uint32_t size_;
};

class MemoryByteSource : public ByteSource {
public:
    MemoryByteSource(const uint8_t* buffer, uint32_t size) : buffer_(buffer), size_(size), position_(0) {}

    size_t read(uint8_t* data, size_t size) override {
        const uint32_t left = size_ - position_;
        const uint32_t count = size < left ? static_cast<uint32_t>(size) : left;
        memcpy(data, buffer_ + position_, count);
        position_ += count;
        return count;
    }

    bool seek(uint32_t position) override {
        if (position > size_) return false;
        position_ = position;
        return true;
    }

private:
    const uint8_t* buffer_;
    uint32_t size_;
    uint32_t position_;
};

// Encoder -> EncoderSource -> WavOutput -> memory, then memory -> WavReader -> DecoderSink -> Decoder.
bool wav_round_trip() {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(k_wav_text);
    const size_t size = sizeof(k_wav_text) - 1;
    const EncoderConfig config = EncoderConfig::from_preset(Preset::hf_fast, k_decoder_rate_hz);
    Encoder encoder(config);
    const bool queued = encoder.write(data, size) == size && encoder.start();
    const uint32_t samples = encoder.duration_samples(size);
    MemoryByteSink sink(g_wav, k_wav_buffer_bytes);
    WavOutput output(sink, samples);
    EncoderSource source(encoder);
    const bool written = queued && output.start(source, config.sample_rate_hz) && output.wait();

    Tally tally;
    reset_tally(tally, data, size, static_cast<float>(config.slot_us) / k_us_per_ms_f, nullptr);
    Decoder decoder(DecoderConfig(), &on_event, &tally);
    DecoderSink decoder_sink(decoder);
    MemoryByteSource bytes(g_wav, sink.size());
    WavReader reader;
    const bool opened = written && reader.open(bytes) && reader.format().sample_rate_hz == k_decoder_rate_hz;
    uint32_t read = 0;
    int16_t chunk[k_chunk_samples];
    feed_silence(decoder, k_lead_silence_ms);
    while (opened) {
        const size_t got = reader.read(chunk, k_chunk_samples);
        if (got == 0) break;
        decoder_sink.write(chunk, got);
        read += static_cast<uint32_t>(got);
    }
    drain(decoder, tally);
    const bool pass = opened && read == samples && clean_run(tally);
    print_tally("wav", tally, pass);
    return pass;
}

}  // namespace

int main() {
    printf("heap_trap: sizeof Encoder %zu, Decoder %zu, PacketReader %zu bytes (UNLIMITED_PACKET_MAX %u)\n",
           sizeof(Encoder), sizeof(Decoder), sizeof(PacketReader), static_cast<unsigned>(k_packet_max_payload));
    uint32_t random_state = k_random_seed;
    size_t failures = 0;
    for (size_t i = 0; i < k_case_count; ++i) {
        if (!run_case(k_cases[i], random_state)) ++failures;
    }
    if (!packet_round_trip(random_state)) ++failures;
    if (!wav_round_trip()) ++failures;
    printf("heap_trap: %s (%zu failure%s)\n", failures == 0 ? "PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
