// The send side without the receiver (spec 12.2): the KISS codec, the send queue, the channel check and the encoder,
// built from kiss.cpp, transmitter.cpp and the encoder's sources only (no decoder.cpp, no dsp.cpp): the link itself
// is the proof. On the host it sends a frame and checks the transmission's length; built for the ATmega328P (an
// Arduino Nano) with a small UNLIMITED_MODEM_QUEUE, 'make check_embedded' reports its flash and RAM.
#include "unlimited/transmitter.hpp"

#if !defined(__AVR__)
#include <stdio.h>
#include <stdlib.h>
#endif

namespace {

using namespace unlimited;

const size_t k_step_samples = 8;  // 1 ms at 8 kHz
const uint32_t k_max_ms = 60000;
const uint8_t k_frame[] = {k_kiss_fend, k_kiss_data, 'N', 'A', 'N', 'O', k_kiss_fesc, k_kiss_tfend, k_kiss_fend};
const size_t k_data_bytes = 5;  // N A N O and the escaped FEND

uint32_t g_keys = 0;
bool g_keyed = false;

void on_ptt(bool on, void*) {
    if (on) ++g_keys;
    g_keyed = on;
}

EncoderConfig signal_config() {
    EncoderConfig signal;
    signal.sample_rate_hz = k_modem_rate_hz;
    signal.lead_in_ms = k_default_txdelay_ms;
    return signal;
}

AccessConfig access_config() {
    AccessConfig access;
    access.dwait_ms = 0;
    access.persist = 255;
    return access;
}

ModemTransmitter g_transmitter(signal_config(), access_config(), &on_ptt, nullptr);
int16_t g_audio[k_step_samples];

}  // namespace

int main() {
    const size_t taken = g_transmitter.host_input(k_frame, sizeof(k_frame));
    uint32_t keyed_samples = 0;
    for (uint32_t ms = 0; ms < k_max_ms; ++ms) {
        g_transmitter.tick(ms);
        g_transmitter.audio_output(g_audio, k_step_samples);
        if (g_keyed) keyed_samples += k_step_samples;
        if (g_keys == 1 && !g_keyed) break;
    }
#if defined(__AVR__)
    return taken == sizeof(k_frame) && g_keys == 1 && g_transmitter.counters().bytes_sent == k_data_bytes &&
                   keyed_samples > 0
               ? 0
               : 1;
#else
    const uint32_t expected = Encoder(signal_config()).duration_samples(k_data_bytes);
    const ModemCounters counters = g_transmitter.counters();
    const bool pass = taken == sizeof(k_frame) && g_keys == 1 && counters.transmissions == 1 &&
                      counters.bytes_sent == k_data_bytes && keyed_samples >= expected &&
                      keyed_samples <= expected + 2 * k_step_samples;
    printf("send_only: no decoder linked; sizeof ModemTransmitter %zu (queue %u, %u frame slots), KissDecoder %zu, "
           "Encoder %zu bytes; one frame of %zu bytes keyed %u samples for a %u-sample transmission  %s\n",
           sizeof(ModemTransmitter), static_cast<unsigned>(ModemTransmitter::k_queue_size),
           static_cast<unsigned>(ModemTransmitter::k_frame_slots), sizeof(KissDecoder), sizeof(Encoder), k_data_bytes,
           keyed_samples, expected, pass ? "ok" : "FAIL");
    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}
