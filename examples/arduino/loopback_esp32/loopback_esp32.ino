// Unlimited self-test on an ESP32, no radio and no wiring: a packet is encoded with every preset, fed
// straight into a Decoder in RAM, decoded, printed and compared. The decoder is told nothing about the speed or
// the bits per package: it learns T and N from the signal, and the test checks it learnt the sent ones. For each
// preset it prints the CPU time the encoder and the decoder spend per second of audio, i.e. the load of one core
// at real time. Results go to Serial (115200 baud); send any character to run the test again.
#include <unlimited.h>

namespace {

const uint32_t k_serial_baud = 115200;
const uint32_t k_sample_rate_hz = unlimited::k_decoder_rate_hz;
const size_t k_chunk_samples = 256;
const uint32_t k_lead_silence_ms = 200;
const uint32_t k_max_trailing_ms = 3000;  // END comes about 2.4 slots after the last STOP
const uint32_t k_ms_per_s = 1000;
const float k_us_per_s = 1e6f;
const float k_us_per_ms = 1000.0f;
const float k_ms_per_s_float = 1000.0f;
const float k_percent = 100.0f;
const float k_slot_tolerance = 0.005f;  // the learnt T within 0.5 % of the sent one

const char k_message[] = "CQ CQ DE UNLIMITED ESP32 LOOPBACK 0123456789";
const uint8_t k_message_size = sizeof(k_message) - 1;

struct Case {
    const char* name;
    unlimited::Preset preset;
    unlimited::Profile profile;  // a receiver profile whose window holds the preset's T
};

const Case k_cases[] = {
    {"hf_slow", unlimited::Preset::hf_slow, unlimited::Profile::ssb},
    {"hf", unlimited::Preset::hf, unlimited::Profile::ssb},
    {"hf_fast", unlimited::Preset::hf_fast, unlimited::Profile::ssb},
    {"am", unlimited::Preset::am, unlimited::Profile::am},
    {"fm", unlimited::Preset::fm, unlimited::Profile::fm},
};

struct Result {
    uint8_t payload[unlimited::k_packet_max_payload];
    uint16_t size;
    uint8_t packets;
    uint8_t locks;
    uint8_t ends;
    uint8_t losts;
    uint8_t bits_per_package;  // N of the locked event, learnt from the signal
    float tone_hz;
    float slot_ms;
    float snr_db;
};

Result g_result;
uint8_t g_packet[k_message_size + unlimited::k_packet_overhead];
int16_t g_chunk[k_chunk_samples];
int16_t g_silence[k_chunk_samples];
TaskHandle_t g_loop_task = nullptr;

void on_packet(const uint8_t* payload, uint16_t size, uint8_t, void*) {
    memcpy(g_result.payload, payload, size);
    g_result.size = size;
    ++g_result.packets;
}

unlimited::PacketReader g_packets(&on_packet, nullptr);

void on_event(const unlimited::Event& event, void*) {
    g_packets.on_event(event);
    switch (event.type) {
        case unlimited::EventType::locked:
            ++g_result.locks;
            g_result.bits_per_package = event.bits_per_package;
            g_result.tone_hz = event.tone_hz;
            g_result.slot_ms = event.slot_ms;
            g_result.snr_db = event.snr_db;
            break;
        case unlimited::EventType::end:
            ++g_result.ends;
            break;
        case unlimited::EventType::lost:
            ++g_result.losts;
            break;
        case unlimited::EventType::state:
        case unlimited::EventType::slot:
        case unlimited::EventType::package:
        case unlimited::EventType::byte:
            break;
    }
}

unlimited::Decoder g_ssb_decoder(unlimited::DecoderConfig::for_profile(unlimited::Profile::ssb), &on_event, nullptr);
unlimited::Decoder g_am_decoder(unlimited::DecoderConfig::for_profile(unlimited::Profile::am), &on_event, nullptr);
unlimited::Decoder g_fm_decoder(unlimited::DecoderConfig::for_profile(unlimited::Profile::fm), &on_event, nullptr);

unlimited::Decoder& decoder_for(unlimited::Profile profile) {
    switch (profile) {
        case unlimited::Profile::ssb:
            break;
        case unlimited::Profile::am:
            return g_am_decoder;
        case unlimited::Profile::fm:
            return g_fm_decoder;
    }
    return g_ssb_decoder;
}

const char* profile_name(unlimited::Profile profile) {
    switch (profile) {
        case unlimited::Profile::ssb:
            return "ssb";
        case unlimited::Profile::am:
            return "am";
        case unlimited::Profile::fm:
            return "fm";
    }
    return "?";
}

bool run_case(const Case& test_case, size_t packet_size) {
    const unlimited::EncoderConfig config = unlimited::EncoderConfig::from_preset(test_case.preset, k_sample_rate_hz);
    unlimited::Encoder encoder(config);
    unlimited::Decoder& decoder = decoder_for(test_case.profile);
    decoder.reset();
    g_packets.reset();
    memset(&g_result, 0, sizeof(g_result));

    uint32_t audio_samples = 0;
    for (uint32_t done = 0; done < k_lead_silence_ms * k_sample_rate_hz / k_ms_per_s; done += k_chunk_samples) {
        decoder.process(g_silence, k_chunk_samples);
    }
    size_t written = encoder.write(g_packet, packet_size);
    const bool started = encoder.start();
    uint32_t encode_us = 0;
    uint32_t decode_us = 0;
    while (true) {
        const uint32_t t0 = micros();
        written += encoder.write(g_packet + written, packet_size - written);
        const size_t rendered = encoder.render(g_chunk, k_chunk_samples);
        const uint32_t t1 = micros();
        if (rendered == 0) break;
        decoder.process(g_chunk, rendered);
        const uint32_t t2 = micros();
        encode_us += t1 - t0;
        decode_us += t2 - t1;
        audio_samples += rendered;
    }
    for (uint32_t done = 0; done < k_max_trailing_ms * k_sample_rate_hz / k_ms_per_s && g_result.ends == 0;
         done += k_chunk_samples) {
        decoder.process(g_silence, k_chunk_samples);
    }

    const float sent_slot_ms = config.slot_us / k_us_per_ms;
    const bool slot_learnt = fabsf(g_result.slot_ms / sent_slot_ms - 1.0f) <= k_slot_tolerance;
    const bool pass = started && g_result.packets == 1 && g_result.size == k_message_size &&
                      memcmp(g_result.payload, k_message, k_message_size) == 0 && g_result.locks == 1 &&
                      g_result.ends == 1 && g_result.losts == 0 && g_packets.crc_errors() == 0 &&
                      g_result.bits_per_package == config.bits_per_package && slot_learnt;
    const float audio_s = static_cast<float>(audio_samples) / k_sample_rate_hz;
    const float rate = config.bits_per_package * k_ms_per_s_float / ((config.bits_per_package + 1) * sent_slot_ms);
    Serial.printf("%-8s sent T %.0f ms, N %u (%.1f bit/s) at %u Hz; receiver profile %s: %s\n", test_case.name,
                  sent_slot_ms, config.bits_per_package, rate, config.tone_hz, profile_name(test_case.profile),
                  pass ? "PASS" : "FAIL");
    Serial.printf("  decoded \"%.*s\" (learnt pitch %.1f Hz, T %.2f ms, N %u, SNR %.1f dB)\n", g_result.size,
                  g_result.payload, g_result.tone_hz, g_result.slot_ms, g_result.bits_per_package, g_result.snr_db);
    Serial.printf("  %.1f s of audio: encoder %.0f us/s (%.2f %%), decoder %.0f us/s (%.2f %%)\n", audio_s,
                  encode_us / audio_s, k_percent * encode_us / (audio_s * k_us_per_s), decode_us / audio_s,
                  k_percent * decode_us / (audio_s * k_us_per_s));
    return pass;
}

void run_all() {
    const size_t packet_size = unlimited::packet_build(reinterpret_cast<const uint8_t*>(k_message), k_message_size,
                                                       g_packet, sizeof(g_packet));
    size_t failures = 0;
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); ++i) {
        if (!run_case(k_cases[i], packet_size)) ++failures;
    }
    Serial.printf("loopback_esp32: %s (%u failures), sizeof Encoder %u, Decoder %u, PacketReader %u bytes\n",
                  failures == 0 ? "PASSED" : "FAILED", static_cast<unsigned>(failures),
                  static_cast<unsigned>(sizeof(unlimited::Encoder)), static_cast<unsigned>(sizeof(unlimited::Decoder)),
                  static_cast<unsigned>(sizeof(unlimited::PacketReader)));
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
