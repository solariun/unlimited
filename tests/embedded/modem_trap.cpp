// Heap trap of the KISS modem core (spec 8, 12.2): two Modem cores joined in memory, A's audio into B's receiver and
// back, on a simulated clock, while every C++ allocation function aborts. KISS frames written into A come out of B byte
// for byte, B's answer out of A. Built by 'make check_embedded' from this file and src/ only, with -fno-exceptions
// -fno-rtti; malloc and friends are covered by its nm check.
#include "unlimited/modem.hpp"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

[[noreturn]] void trap(const char* what) {
    fputs("modem_trap: ", stderr);
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

const uint32_t k_step_samples = 8;  // 1 ms at 8 kHz
const uint32_t k_max_ms = 120000;
const size_t k_host_bytes = 1024;
const size_t k_kiss_bytes = 256;

struct Station {
    uint8_t host[k_host_bytes];  // what the modem sent its computer
    size_t host_size;
    uint32_t keys;
    bool keyed;
};

Station g_stations[2];
int16_t g_audio[2][k_step_samples];
uint8_t g_kiss[k_kiss_bytes];

void on_host(const uint8_t* data, size_t size, void* context) {
    Station& station = *static_cast<Station*>(context);
    for (size_t i = 0; i < size && station.host_size < k_host_bytes; ++i) station.host[station.host_size++] = data[i];
}

void on_ptt(bool on, void* context) {
    Station& station = *static_cast<Station*>(context);
    if (on) ++station.keys;
    station.keyed = on;
}

ModemConfig config_at(uint16_t centi, uint32_t seed) {
    ModemConfig config;
    config.signal.slot_us = slot_us_for_centi_speed(centi);
    config.receiver.slot_us = config.signal.slot_us;
    config.access.seed = seed;
    config.min_frame_bytes = 0;  // frames of 1..13 bytes: every reception streams (V23 off)
    return config;
}

// KISS for one data frame into g_kiss: its size.
size_t kiss(const uint8_t* data, size_t size) {
    size_t out = 0;
    g_kiss[out++] = k_kiss_fend;
    g_kiss[out++] = k_kiss_data;
    for (size_t i = 0; i < size; ++i) out += kiss_escape(data[i], g_kiss + out);
    g_kiss[out++] = k_kiss_fend;
    return out;
}

bool quiet(const Modem& modem) {
    const ModemCounters counters = modem.counters();
    return modem.channel_state() == ChannelState::idle && counters.queued_frames == 0 && !modem.dcd();
}

// Both modems run until each is quiet for a while (the receivers' end latency) or k_max_ms passes.
uint32_t run(Modem& a, Modem& b, uint32_t& now_ms) {
    const uint32_t settle_ms = 4000;
    uint32_t quiet_ms = 0;
    for (uint32_t elapsed = 0; elapsed < k_max_ms && quiet_ms < settle_ms; ++elapsed, ++now_ms) {
        a.tick(now_ms);
        b.tick(now_ms);
        a.audio_output(g_audio[0], k_step_samples);
        b.audio_output(g_audio[1], k_step_samples);
        b.audio_input(g_audio[0], k_step_samples);
        a.audio_input(g_audio[1], k_step_samples);
        quiet_ms = quiet(a) && quiet(b) ? quiet_ms + 1 : 0;
    }
    return now_ms;
}

bool same(const Station& station, const uint8_t* expected, size_t size) {
    return station.host_size == size && memcmp(station.host, expected, size) == 0;
}

bool run_case(const char* name, uint16_t centi) {
    memset(g_stations, 0, sizeof(g_stations));
    Modem a(config_at(centi, 1), &on_host, &on_ptt, &g_stations[0]);
    Modem b(config_at(centi, 2), &on_host, &on_ptt, &g_stations[1]);

    const uint8_t text[] = "KISS from A";
    const uint8_t escapes[] = {0xC0, 0xDB, 0xDC, 0xDD, 0x00, 0xFF};
    const uint8_t one[] = {'1'};
    const uint8_t answer[] = "answer from B";
    uint8_t expected_b[k_host_bytes];
    size_t expected_b_size = 0;
    const uint8_t* frames[] = {text, escapes, one};
    const size_t sizes[] = {sizeof(text) - 1, sizeof(escapes), sizeof(one)};
    for (size_t f = 0; f < 3; ++f) {
        const size_t size = kiss(frames[f], sizes[f]);
        if (a.host_input(g_kiss, size) != size) return false;
        memcpy(expected_b + expected_b_size, g_kiss, size);
        expected_b_size += size;
    }
    uint32_t now_ms = 0;
    run(a, b, now_ms);
    const size_t answer_size = kiss(answer, sizeof(answer) - 1);
    uint8_t expected_a[k_kiss_bytes];
    memcpy(expected_a, g_kiss, answer_size);
    if (b.host_input(g_kiss, answer_size) != answer_size) return false;
    run(a, b, now_ms);

    const bool pass = same(g_stations[1], expected_b, expected_b_size) && same(g_stations[0], expected_a, answer_size) &&
                      g_stations[0].keys == 3 && g_stations[1].keys == 1 && !g_stations[0].keyed &&
                      !g_stations[1].keyed;
    printf("  %-8s A -> B %zu KISS bytes (%s), B -> A %zu (%s), keys %u and %u, %.1f s simulated  %s\n", name,
           g_stations[1].host_size, same(g_stations[1], expected_b, expected_b_size) ? "exact" : "DIFFERENT",
           g_stations[0].host_size, same(g_stations[0], expected_a, answer_size) ? "exact" : "DIFFERENT",
           g_stations[0].keys, g_stations[1].keys, now_ms / 1000.0, pass ? "ok" : "FAIL");
    return pass;
}

}  // namespace

int main() {
    printf("modem_trap: sizeof Modem %zu, ModemTransmitter %zu (send queue %u, %u frame slots), KissDecoder %zu, "
           "Decoder %zu, Encoder %zu bytes\n",
           sizeof(Modem), sizeof(ModemTransmitter), static_cast<unsigned>(ModemTransmitter::k_queue_size),
           static_cast<unsigned>(ModemTransmitter::k_frame_slots), sizeof(KissDecoder), sizeof(Decoder),
           sizeof(Encoder));
    size_t failures = 0;
    if (!run_case("6 B/s", 600)) ++failures;
    if (!run_case("25 B/s", 2500)) ++failures;
    printf("modem_trap: %s (%zu failure%s)\n", failures == 0 ? "PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
