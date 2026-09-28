#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__AVR__)
#include <avr/interrupt.h>
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

template <typename T>
struct Identity {
    typedef T type;
};

// A value one context writes and others read (the KISS modem's contexts, spec 12.2), whole and in order:
// store_release() makes it visible after everything its writer did before, load_acquire() reads it before everything
// its reader does after. With GCC or Clang these are the compiler's atomic accesses (the fences above tied to the
// value, which ThreadSanitizer follows). On an AVR, one core with an ISR as the other context, a value wider than a
// byte is read or written with interrupts off: an interrupt between its instructions would see half of it.
template <typename T>
inline T load_acquire(const volatile T& from) {
#if defined(__AVR__)
    if (sizeof(T) == 1) {
        const T value = from;
        compiler_barrier();
        return value;
    }
    const uint8_t sreg = SREG;
    cli();
    const T value = from;
    SREG = sreg;
    compiler_barrier();
    return value;
#elif defined(__GNUC__)
    T value;
    __atomic_load(&from, &value, __ATOMIC_ACQUIRE);
    return value;
#else
    return from;
#endif
}

template <typename T>
inline void store_release(volatile T& to, typename Identity<T>::type value) {
#if defined(__AVR__)
    compiler_barrier();
    if (sizeof(T) == 1) {
        to = value;
        return;
    }
    const uint8_t sreg = SREG;
    cli();
    to = value;
    SREG = sreg;
#elif defined(__GNUC__)
    __atomic_store(&to, &value, __ATOMIC_RELEASE);
#else
    to = value;
#endif
}

}  // namespace unlimited
