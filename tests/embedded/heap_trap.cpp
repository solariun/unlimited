// Heap trap (spec 8): the whole core decodes the five speeds of spec 1.3, a transmission with a VOX lead, two back to
// back and a WAV round trip while every C++ allocation function aborts. Built by 'make check_embedded' from this file
// and src/ only, with -fno-exceptions -fno-rtti; malloc and friends are covered by its nm check.
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
const size_t k_chunk_samples = 37;              // odd on purpose; shorter than any slot, so the queue never runs dry
const uint32_t k_lead_silence_ms = 500;
const uint32_t k_lead_silence_slots = 15;       // the decoder needs a whole silent window before its first START (V6)
const uint32_t k_us_per_ms = 1000;
const uint32_t k_max_trailing_ms = 3000;        // the end comes one silent window after the last STOP, plus the look-ahead
const uint32_t k_ms_per_s = 1000;
const float k_us_per_ms_f = 1000.0f;
const float k_slot_tolerance = 0.005f;          // measured slot_ms within 0.5 %
const float k_percent = 100.0f;
const uint32_t k_random_seed = 0x2545F491u;
const uint32_t k_wav_buffer_bytes = 64 * 1024;
const uint16_t k_vox_lead_ms = 150;

struct Case {
    const char* name;
    uint16_t centi_bytes_per_second;
    uint16_t vox_lead_ms;
    size_t bytes;
    uint8_t transmissions;  // back to back, each preceded by nothing but the previous one's tail
};

// The five speeds of spec 1.3, a VOX lead, and two transmissions back to back (spec 8).
const Case k_cases[] = {
    {"1 B/s", 100, 0, 12, 1},     {"3 B/s", 300, 0, 30, 1},   {"6 B/s", 600, 0, 60, 1},
    {"12 B/s", 1200, 0, 120, 1},  {"25 B/s", 2500, 0, 250, 1}, {"6 B/s VOX", 600, k_vox_lead_ms, 30, 1},
    {"6 B/s x2", 600, 0, 20, 2},
};
const size_t k_case_count = sizeof(k_cases) / sizeof(k_cases[0]);

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
};

uint8_t g_data[k_max_bytes];
int16_t g_silence[k_chunk_samples];
uint8_t g_wav[k_wav_buffer_bytes];

uint32_t next_random(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float absolute(float value) {
    return value < 0.0f ? -value : value;
}

void reset_tally(Tally& tally, const uint8_t* expected, size_t size, float slot_ms) {
    memset(&tally, 0, sizeof(tally));
    tally.expected = expected;
    tally.size = size;
    tally.slot_ms = slot_ms;
}

void on_event(const Event& event, void* context) {
    Tally& tally = *static_cast<Tally*>(context);
    switch (event.type) {
        case EventType::locked:
            ++tally.locks;
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

// The silence before a first transmission: k_lead_silence_ms, and at least k_lead_silence_slots of the slot.
uint32_t lead_silence_ms(uint32_t slot_us) {
    const uint32_t slots_ms = k_lead_silence_slots * slot_us / k_us_per_ms;
    return slots_ms > k_lead_silence_ms ? slots_ms : k_lead_silence_ms;
}

void feed_silence(Decoder& decoder, uint32_t ms) {
    const uint32_t samples = ms * (k_decoder_rate_hz / k_ms_per_s);
    for (uint32_t done = 0; done < samples; done += k_chunk_samples) decoder.process(g_silence, k_chunk_samples);
}

// Silence until the last end, bounded.
void drain(Decoder& decoder, const Tally& tally, size_t ends) {
    const uint32_t samples = k_max_trailing_ms * (k_decoder_rate_hz / k_ms_per_s);
    for (uint32_t done = 0; done < samples && tally.ends < ends; done += k_chunk_samples)
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

bool clean_run(const Tally& tally, size_t transmissions) {
    return tally.received == tally.size && tally.wrong == 0 && tally.extra == 0 && tally.locks == transmissions &&
           tally.ends == transmissions && tally.losts == 0 && tally.worst_slot_error <= k_slot_tolerance;
}

void print_tally(const char* name, const Tally& tally, bool pass) {
    printf("  %-10s %4zu bytes: received %zu wrong %zu extra %zu locks %zu ends %zu lost %zu, "
           "slot error %.3f %%  %s\n",
           name, tally.size, tally.received, tally.wrong, tally.extra, tally.locks, tally.ends, tally.losts,
           static_cast<double>(tally.worst_slot_error * k_percent), pass ? "ok" : "FAIL");
}

EncoderConfig case_config(const Case& test_case) {
    EncoderConfig config;
    config.sample_rate_hz = k_decoder_rate_hz;
    config.slot_us = slot_us_for_centi_speed(test_case.centi_bytes_per_second);
    config.vox_lead_ms = test_case.vox_lead_ms;
    return config;
}

bool run_case(const Case& test_case, uint32_t& random_state) {
    const size_t total = test_case.bytes * test_case.transmissions;
    for (size_t i = 0; i < total; ++i) g_data[i] = static_cast<uint8_t>(next_random(random_state));
    const EncoderConfig config = case_config(test_case);
    if (!config.valid()) {
        printf("  %-10s invalid encoder configuration  FAIL\n", test_case.name);
        return false;
    }
    DecoderConfig receiver;
    receiver.slot_us = config.slot_us;
    Tally tally;
    reset_tally(tally, g_data, total, static_cast<float>(config.slot_us) / k_us_per_ms_f);
    Encoder encoder(config);
    Decoder decoder(receiver, &on_event, &tally);
    feed_silence(decoder, lead_silence_ms(config.slot_us));
    bool streamed = true;
    for (uint8_t t = 0; t < test_case.transmissions; ++t) {
        streamed = stream(encoder, decoder, g_data + t * test_case.bytes, test_case.bytes) && streamed;
    }
    drain(decoder, tally, test_case.transmissions);
    const bool pass = streamed && clean_run(tally, test_case.transmissions);
    print_tally(test_case.name, tally, pass);
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
    EncoderConfig config;
    config.slot_us = slot_us_for_centi_speed(k_max_centi_bytes_per_second / 2);
    Encoder encoder(config);
    const bool queued = encoder.write(data, size) == size && encoder.start();
    const uint32_t samples = encoder.duration_samples(size);
    MemoryByteSink sink(g_wav, k_wav_buffer_bytes);
    WavOutput output(sink, samples);
    EncoderSource source(encoder);
    const bool written = queued && output.start(source, config.sample_rate_hz) && output.wait();

    Tally tally;
    reset_tally(tally, data, size, static_cast<float>(config.slot_us) / k_us_per_ms_f);
    DecoderConfig receiver;
    receiver.slot_us = config.slot_us;
    Decoder decoder(receiver, &on_event, &tally);
    DecoderSink decoder_sink(decoder);
    MemoryByteSource bytes(g_wav, sink.size());
    WavReader reader;
    const bool opened = written && reader.open(bytes) && reader.format().sample_rate_hz == k_decoder_rate_hz;
    uint32_t read = 0;
    int16_t chunk[k_chunk_samples];
    feed_silence(decoder, lead_silence_ms(config.slot_us));
    while (opened) {
        const size_t got = reader.read(chunk, k_chunk_samples);
        if (got == 0) break;
        decoder_sink.write(chunk, got);
        read += static_cast<uint32_t>(got);
    }
    drain(decoder, tally, 1);
    const bool pass = opened && read == samples && clean_run(tally, 1);
    print_tally("wav", tally, pass);
    return pass;
}

}  // namespace

int main() {
    printf("heap_trap: sizeof Encoder %zu, Decoder %zu bytes\n", sizeof(Encoder), sizeof(Decoder));
    uint32_t random_state = k_random_seed;
    size_t failures = 0;
    for (size_t i = 0; i < k_case_count; ++i) {
        if (!run_case(k_cases[i], random_state)) ++failures;
    }
    if (!wav_round_trip()) ++failures;
    printf("heap_trap: %s (%zu failure%s)\n", failures == 0 ? "PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
