// Unlimited receiver for an ESP32: radio audio -> ADC -> Decoder -> Serial (115200 baud).
//
// The decoder needs 8 kHz int16 audio. The ESP32 ADC DMA cannot sample below 20 kHz, so it runs at
// 24 kHz without any CPU timing (no ISR, no jitter) and the sketch decimates by 3: DC blocker, then a
// 47-tap low-pass (flat to 3 kHz, below -50 dB from 5 kHz, which is what folds back onto 300..3000 Hz).
// The decoder tracks a sample-clock error of up to +-1000 ppm (spec test L5) and the ADC clock is derived from
// the crystal, so no calibration is expected.
//
// The receiver is told two things (below): the speed in bytes per second, the same as the sender's (tx_uno sends
// 6 bytes/s), and the audio passband of the radio. It finds the pitch by itself; USB or LSB and mistuning only move
// the pitch. It decodes a transmission from its first byte (the first tone after silence is the START of byte 0):
// start it before the sender transmits.
//
// loop() blocks in adc_continuous_read() until the next 10 ms DMA frame arrives; the decoder runs there,
// in task context, as the core requires. Prints "locked" with the pitch, T, the SNR and the received band, every
// byte as soon as its window's STOP is heard, "end" and "lost". The DCD pin is high while a transmission is
// being decoded (Decoder::dcd(), spec 3.10: a modem waits for it to drop before it transmits).
//
// Wiring:
//   radio speaker / data out --||--+-- GPIO36 (VP, ADC1 channel 0, classic ESP32; fine with Wi-Fi on)
//                            1uF   |
//                  3V3 --[10k]-----+-----[10k]-- GND     (bias at mid-scale)
//   Keep the audio under about 1.5 V peak-to-peak (ADC range with 12 dB attenuation: 0.15..2.45 V).
//   Optional: 1k in series and 47 nF to GND at the pin keeps strong hiss above 12 kHz from aliasing.
//   GPIO2 (the on-board LED of most ESP32 DevKit boards) shows DCD.
#include <unlimited.h>
#include <esp_adc/adc_continuous.h>
#include <math.h>

namespace {

const uint32_t k_serial_baud = 115200;
const int k_audio_pin = 36;
const uint8_t k_dcd_pin = 2;
// The sender's speed in hundredths of a byte per second (6.00: the HF default) and the radio's passband.
const uint16_t k_centi_bytes_per_second = unlimited::k_default_centi_bytes_per_second;
const uint16_t k_passband_low_hz = unlimited::k_ssb_passband_low_hz;
const uint16_t k_passband_high_hz = unlimited::k_ssb_passband_high_hz;
const uint16_t k_centi = unlimited::k_centi_per_unit;

const uint32_t k_adc_rate_hz = 24000;
const uint8_t k_decimation = 3;
static_assert(k_adc_rate_hz == k_decimation * unlimited::k_decoder_rate_hz, "the ADC runs at 3x the decoder rate");
const uint32_t k_frame_conversions = 240;  // 10 ms per DMA frame
const uint32_t k_frame_bytes = k_frame_conversions * SOC_ADC_DIGI_RESULT_BYTES;
static_assert(k_frame_bytes % SOC_ADC_DIGI_DATA_BYTES_PER_CONV == 0, "DMA frames hold whole conversions");
const uint32_t k_pool_frames = 8;          // 80 ms of slack while Serial prints
const uint32_t k_read_timeout_ms = 1000;
const uint32_t k_output_samples = k_frame_conversions / k_decimation;

const uint8_t k_fir_taps = 47;
const float k_fir_cutoff_hz = 4000.0f;     // -6 dB point between the 3 kHz pass edge and the 5 kHz stop edge
const float k_hamming_a0 = 0.54f;
const float k_hamming_a1 = 0.46f;
const float k_dc_pole = 0.999f;            // DC blocker corner at about 4 Hz
const float k_adc_to_int16 = 16.0f;        // 12-bit ADC -> int16 full scale
const float k_int16_max = 32767.0f;
const float k_int16_min = -32768.0f;
const float k_two_pi = 6.28318531f;
const float k_us_per_ms = 1000.0f;

// 24 kHz ADC codes -> 8 kHz int16: DC blocker, then a Hamming-windowed sinc evaluated at every 3rd input.
class Decimator {
public:
    void begin() {
        const float centre = 0.5f * (k_fir_taps - 1);
        const float cutoff = k_fir_cutoff_hz / k_adc_rate_hz;
        float sum = 0.0f;
        for (uint8_t n = 0; n < k_fir_taps; ++n) {
            const float t = n - centre;
            const float sinc = t == 0.0f ? 2.0f * cutoff : sinf(k_two_pi * cutoff * t) / (0.5f * k_two_pi * t);
            taps_[n] = sinc * (k_hamming_a0 - k_hamming_a1 * cosf(k_two_pi * n / (k_fir_taps - 1)));
            sum += taps_[n];
        }
        for (uint8_t n = 0; n < k_fir_taps; ++n) taps_[n] /= sum;  // unity gain at DC and in the pass band
    }

    bool push(float code, int16_t& out) {
        const float x = code - last_code_ + k_dc_pole * last_output_;
        last_code_ = code;
        last_output_ = x;
        // Every sample is stored twice, so the last k_fir_taps inputs are always contiguous.
        history_[head_] = x;
        history_[head_ + k_fir_taps] = x;
        head_ = head_ + 1 == k_fir_taps ? 0 : head_ + 1;
        if (++phase_ < k_decimation) return false;
        phase_ = 0;
        const float* window = history_ + head_;
        float y = 0.0f;
        for (uint8_t n = 0; n < k_fir_taps; ++n) y += taps_[n] * window[n];
        y *= k_adc_to_int16;
        out = static_cast<int16_t>(y > k_int16_max ? k_int16_max : (y < k_int16_min ? k_int16_min : y));
        return true;
    }

private:
    float taps_[k_fir_taps] = {};
    float history_[2 * k_fir_taps] = {};
    float last_code_ = 0.0f;
    float last_output_ = 0.0f;
    uint8_t head_ = 0;
    uint8_t phase_ = 0;
};

const char* lost_reason(unlimited::LostReason reason) {
    switch (reason) {
        case unlimited::LostReason::framing:
            return "framing errors (the signal faded or was not Unlimited)";
        case unlimited::LostReason::reset:
            return "reset";
        case unlimited::LostReason::none:
            break;
    }
    return "none";
}

unlimited::DecoderConfig receiver_config() {
    unlimited::DecoderConfig config;
    config.slot_us = unlimited::slot_us_for_centi_speed(k_centi_bytes_per_second);
    config.passband.low_hz = k_passband_low_hz;
    config.passband.high_hz = k_passband_high_hz;
    return config;
}

const unlimited::DecoderConfig g_config = receiver_config();

// Pitch, T, SNR, and the received band against the passband (how far the radio may still drift).
void print_lock(const unlimited::Event& event) {
    Serial.printf("locked: pitch %.1f Hz, T %.2f ms, SNR %.1f dB\n", event.tone_hz, event.slot_ms, event.snr_db);
    const uint16_t tone_hz = static_cast<uint16_t>(lroundf(event.tone_hz));
    const uint32_t slot_us = static_cast<uint32_t>(lroundf(event.slot_ms * k_us_per_ms));
    const unlimited::Band band = unlimited::occupied_band(tone_hz, slot_us);
    // The shift stops where the band leaves the passband or the pitch leaves this receiver's search (spec 1.3).
    const unlimited::PassbandFit fit =
        unlimited::passband_fit(tone_hz, slot_us, g_config.passband, g_config.search_range());
    Serial.printf("  occupied bandwidth %u Hz (%u-%u Hz); passband %u-%u Hz: ", band.width_hz, band.low_hz,
                  band.high_hz, g_config.passband.low_hz, g_config.passband.high_hz);
    if (fit.fits)
        Serial.printf("fits; shift tolerance -%d/+%d Hz\n", fit.margin_low_hz, fit.margin_high_hz);
    else
        Serial.println("does not fit");
}

void on_event(const unlimited::Event& event, void*) {
    switch (event.type) {
        case unlimited::EventType::state:
            digitalWrite(k_dcd_pin, event.state != unlimited::DecoderState::search ? HIGH : LOW);
            break;
        case unlimited::EventType::locked:
            print_lock(event);
            break;
        case unlimited::EventType::byte:
            Serial.write(event.value);  // as soon as its window is read (spec 3.6)
            break;
        case unlimited::EventType::end:
            Serial.println("\nend");
            break;
        case unlimited::EventType::lost:
            Serial.printf("\nlost: %s\n", lost_reason(event.reason));
            break;
        case unlimited::EventType::slot:
            break;
    }
}

unlimited::Decoder g_decoder(g_config, &on_event, nullptr);
Decimator g_decimator;
adc_continuous_handle_t g_adc = nullptr;
uint8_t g_raw[k_frame_bytes];
adc_continuous_data_t g_parsed[k_frame_conversions];
int16_t g_audio[k_output_samples];  // a frame of k_decimation * k_output_samples inputs yields at most this

bool start_adc() {
    adc_unit_t unit;
    adc_channel_t channel;
    if (adc_continuous_io_to_channel(k_audio_pin, &unit, &channel) != ESP_OK || unit != ADC_UNIT_1) return false;
    adc_continuous_handle_cfg_t handle_config = {};
    handle_config.max_store_buf_size = k_pool_frames * k_frame_bytes;
    handle_config.conv_frame_size = k_frame_bytes;
    if (adc_continuous_new_handle(&handle_config, &g_adc) != ESP_OK) return false;
    adc_digi_pattern_config_t pattern = {};
    pattern.atten = ADC_ATTEN_DB_12;
    pattern.channel = channel;
    pattern.unit = unit;
    pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
    adc_continuous_config_t config = {};
    config.pattern_num = 1;
    config.adc_pattern = &pattern;
    config.sample_freq_hz = k_adc_rate_hz;
    config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
    return adc_continuous_config(g_adc, &config) == ESP_OK && adc_continuous_start(g_adc) == ESP_OK;
}

}  // namespace

void setup() {
    Serial.begin(k_serial_baud);
    pinMode(k_dcd_pin, OUTPUT);
    digitalWrite(k_dcd_pin, LOW);
    g_decimator.begin();
    if (!start_adc()) {
        Serial.println("rx_esp32: ADC setup failed");
        vTaskDelete(nullptr);
    }
    const unlimited::Passband search = g_config.search_range();
    Serial.printf("Unlimited rx_esp32: GPIO%d, speed %u.%02u bytes/s (T %lu us), passband %u-%u Hz, pitch search "
                  "%u-%u Hz\n",
                  k_audio_pin, k_centi_bytes_per_second / k_centi, k_centi_bytes_per_second % k_centi,
                  static_cast<unsigned long>(g_config.slot_us), g_config.passband.low_hz, g_config.passband.high_hz,
                  search.low_hz, search.high_hz);
    if (!g_config.valid()) Serial.println("rx_esp32: the decoder refuses this configuration");
}

void loop() {
    uint32_t bytes = 0;
    if (adc_continuous_read(g_adc, g_raw, sizeof(g_raw), &bytes, k_read_timeout_ms) != ESP_OK) {
        Serial.println("rx_esp32: no ADC data");
        return;
    }
    uint32_t count = 0;
    if (adc_continuous_parse_data(g_adc, g_raw, bytes, g_parsed, &count) != ESP_OK) return;
    size_t samples = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!g_parsed[i].valid) continue;
        if (g_decimator.push(static_cast<float>(g_parsed[i].raw_data), g_audio[samples])) ++samples;
    }
    g_decoder.process(g_audio, samples);
}
