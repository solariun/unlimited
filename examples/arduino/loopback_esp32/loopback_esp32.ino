// Unlimited self-test on an ESP32, no radio and no wiring: a message is encoded at each of the five speeds of the
// spec (1, 3, 6, 12 and 25 bytes per second), fed straight into a Decoder told the same speed, decoded, printed and
// compared. The decoder finds the pitch and the windows by itself. For each speed it prints the CPU time the encoder
// and the decoder spend per second of audio (the load of one core at real time) and the longest single call of the
// decoder on one block of audio (the per-block worst case of spec 3.9). Results go to Serial (115200 baud); send any
// character to run the test again.
#include <unlimited.h>
#include <new>

namespace {

const uint32_t k_serial_baud = 115200;
const uint32_t k_sample_rate_hz = unlimited::k_decoder_rate_hz;
const size_t k_chunk_samples = 4;          // the smallest decoder block: each call ends at most one block
const uint32_t k_lead_silence_slots = 15;  // a whole silent window before the first START (V6), and a margin
const uint32_t k_max_trailing_ms = 3000;   // the end comes a silent window after the last STOP, plus the look-ahead
const uint32_t k_us_per_ms = 1000;
const uint32_t k_ms_per_s = 1000;
const float k_us_per_s = 1e6f;
const float k_us_per_ms_float = 1000.0f;
const float k_percent = 100.0f;
const float k_slot_tolerance = 0.005f;     // the measured T within 0.5 % of the sent one
const uint16_t k_centi = unlimited::k_centi_per_unit;
const uint8_t k_bits_per_byte = unlimited::k_bits_per_byte;

const char k_message[] = "CQ CQ DE UNLIMITED ESP32 LOOPBACK 0123456789";
const uint8_t k_message_size = sizeof(k_message) - 1;
const uint16_t k_speeds_centi[] = {100, 300, 600, 1200, 2500};

struct Result {
    uint8_t text[k_message_size];
    uint16_t size;
    uint8_t locks;
    uint8_t ends;
    uint8_t losts;
    float tone_hz;
    float slot_ms;
    float snr_db;
};

Result g_result;
int16_t g_chunk[k_chunk_samples];
int16_t g_silence[k_chunk_samples];
TaskHandle_t g_loop_task = nullptr;
// One decoder at a time, built in place for each speed (a Decoder is about 17 KB; the core itself never allocates).
alignas(unlimited::Decoder) uint8_t g_decoder_storage[sizeof(unlimited::Decoder)];

void on_event(const unlimited::Event& event, void*) {
    switch (event.type) {
        case unlimited::EventType::locked:
            ++g_result.locks;
            g_result.tone_hz = event.tone_hz;
            g_result.slot_ms = event.slot_ms;
            g_result.snr_db = event.snr_db;
            break;
        case unlimited::EventType::byte:
            if (g_result.size < k_message_size) g_result.text[g_result.size] = event.value;
            ++g_result.size;
            break;
        case unlimited::EventType::end:
            ++g_result.ends;
            break;
        case unlimited::EventType::lost:
            ++g_result.losts;
            break;
        case unlimited::EventType::state:
        case unlimited::EventType::slot:
            break;
    }
}

void feed_silence(unlimited::Decoder& decoder, uint32_t samples, uint32_t& worst_us, bool until_end) {
    for (uint32_t done = 0; done < samples && !(until_end && g_result.ends > 0); done += k_chunk_samples) {
        const uint32_t t0 = micros();
        decoder.process(g_silence, k_chunk_samples);
        const uint32_t spent = micros() - t0;
        if (spent > worst_us) worst_us = spent;
    }
}

bool run_speed(uint16_t centi) {
    unlimited::EncoderConfig config;
    config.sample_rate_hz = k_sample_rate_hz;
    config.slot_us = unlimited::slot_us_for_centi_speed(centi);
    config.passband.low_hz = unlimited::k_am_passband_low_hz;  // every speed fits 100..3000 Hz
    config.passband.high_hz = unlimited::k_am_passband_high_hz;
    unlimited::DecoderConfig receiver;
    receiver.slot_us = config.slot_us;
    receiver.passband = config.passband;
    unlimited::Encoder encoder(config);
    unlimited::Decoder* decoder = new (g_decoder_storage) unlimited::Decoder(receiver, &on_event, nullptr);
    memset(&g_result, 0, sizeof(g_result));

    uint32_t worst_us = 0;
    const uint32_t lead_samples = k_lead_silence_slots * config.slot_us / k_us_per_ms * (k_sample_rate_hz / k_ms_per_s);
    feed_silence(*decoder, lead_samples, worst_us, false);
    const uint8_t* message = reinterpret_cast<const uint8_t*>(k_message);
    size_t written = encoder.write(message, k_message_size);
    const bool started = encoder.start();
    uint32_t encode_us = 0;
    uint32_t decode_us = 0;
    uint32_t audio_samples = 0;
    while (true) {
        const uint32_t t0 = micros();
        written += encoder.write(message + written, k_message_size - written);
        const size_t rendered = encoder.render(g_chunk, k_chunk_samples);
        const uint32_t t1 = micros();
        if (rendered == 0) break;
        decoder->process(g_chunk, rendered);
        const uint32_t t2 = micros();
        encode_us += t1 - t0;
        decode_us += t2 - t1;
        if (t2 - t1 > worst_us) worst_us = t2 - t1;
        audio_samples += rendered;
    }
    feed_silence(*decoder, k_max_trailing_ms * (k_sample_rate_hz / k_ms_per_s), worst_us, true);

    const float sent_slot_ms = config.slot_us / k_us_per_ms_float;
    const bool slot_ok = fabsf(g_result.slot_ms / sent_slot_ms - 1.0f) <= k_slot_tolerance;
    const bool pass = started && g_result.size == k_message_size && memcmp(g_result.text, k_message, k_message_size) == 0 &&
                      g_result.locks == 1 && g_result.ends == 1 && g_result.losts == 0 && slot_ok;
    const float audio_s = static_cast<float>(audio_samples) / k_sample_rate_hz;
    Serial.printf("%u.%02u bytes/s (%lu bit/s, T %.3f ms) at %u Hz: %s\n", centi / k_centi, centi % k_centi,
                  static_cast<unsigned long>(static_cast<uint32_t>(centi) * k_bits_per_byte / k_centi), sent_slot_ms,
                  config.tone_hz, pass ? "PASS" : "FAIL");
    Serial.printf("  decoded \"%.*s\" (pitch %.1f Hz, T %.3f ms, SNR %.1f dB)\n",
                  g_result.size < k_message_size ? g_result.size : k_message_size, g_result.text, g_result.tone_hz,
                  g_result.slot_ms, g_result.snr_db);
    Serial.printf("  %.1f s of audio: encoder %.0f us/s (%.2f %%), decoder %.0f us/s (%.2f %%), longest decoder "
                  "call %lu us\n",
                  audio_s, encode_us / audio_s, k_percent * encode_us / (audio_s * k_us_per_s), decode_us / audio_s,
                  k_percent * decode_us / (audio_s * k_us_per_s), static_cast<unsigned long>(worst_us));
    decoder->~Decoder();
    return pass;
}

void run_all() {
    size_t failures = 0;
    for (size_t i = 0; i < sizeof(k_speeds_centi) / sizeof(k_speeds_centi[0]); ++i) {
        if (!run_speed(k_speeds_centi[i])) ++failures;
    }
    Serial.printf("loopback_esp32: %s (%u failures), sizeof Encoder %u, Decoder %u bytes\n",
                  failures == 0 ? "PASSED" : "FAILED", static_cast<unsigned>(failures),
                  static_cast<unsigned>(sizeof(unlimited::Encoder)), static_cast<unsigned>(sizeof(unlimited::Decoder)));
}

// Serial receive callback (UART event task): wakes loop() instead of having it poll.
void on_serial() {
    xTaskNotifyGive(g_loop_task);
}

}  // namespace

void setup() {
    Serial.begin(k_serial_baud);
    g_loop_task = xTaskGetCurrentTaskHandle();
    Serial.onReceive(on_serial);
    run_all();
}

void loop() {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (Serial.available() > 0) Serial.read();
    run_all();
}
