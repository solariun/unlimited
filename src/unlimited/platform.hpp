#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__AVR__)
#include <avr/pgmspace.h>
#define UNLIMITED_ROM PROGMEM
#else
#define UNLIMITED_ROM
#endif

namespace unlimited {

inline uint16_t rom_read_u16(const uint16_t* address) {
#if defined(__AVR__)
    return static_cast<uint16_t>(pgm_read_word(address));
#else
    return *address;
#endif
}

// Keeps the compiler from moving memory accesses across a hand-off to an ISR (single core).
inline void compiler_barrier() {
#if defined(__GNUC__)
    __asm__ __volatile__("" ::: "memory");
#endif
}

// Hand-off between a producer and a consumer that may run on different cores (a PC audio thread, a dual-core
// ESP32): stores before a release fence are seen by the other side before the store after it, and loads after an
// acquire fence see what was published before the load in front of it. On a single core (AVR) they emit nothing and
// act as compiler_barrier().
inline void release_fence() {
#if defined(__GNUC__)
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

inline void acquire_fence() {
#if defined(__GNUC__)
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}

}  // namespace unlimited
