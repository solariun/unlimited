// The parts of kiss_tnc_esp32 that touch no hardware (spec 12.7): the receive audio's decimator (the ADC's 24 kHz to
// the modem's 8 kHz, rx_esp32's), the transmit audio's 1-bit modulator (the modem's 8 kHz to the I2S data pin), and
// the computer's KISS input with back-pressure. The sketch runs them on the board; tests/test_kiss_tnc.cpp runs the
// same code on a PC, with the channel simulator in place of the radio.
#pragma once

#include "unlimited/modem.hpp"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace kiss_tnc {

// ---------------------------------------------------------------------------
// Receive audio: the ADC's DMA at 24 kHz, decimated by 3 (rx_esp32's decimator, unchanged)
// ---------------------------------------------------------------------------

// The ESP32 ADC DMA cannot sample below 20 kHz, so it runs at 24 kHz without any CPU timing (no ISR, no jitter) and
// the decimator takes every third sample after a DC blocker and a 47-tap low-pass (flat to 3 kHz, below -50 dB from
// 5 kHz, which is what folds back onto 300..3000 Hz).
const uint32_t k_adc_rate_hz = 24000;
const uint8_t k_decimation = 3;
static_assert(k_adc_rate_hz == k_decimation * unlimited::k_modem_rate_hz, "the ADC runs at 3x the modem's rate");
const uint32_t k_frame_conversions = 240;                          // 10 ms per ADC DMA frame
const size_t k_frame_samples = k_frame_conversions / k_decimation;  // what one frame gives at 8 kHz

const uint8_t k_fir_taps = 47;
const float k_fir_cutoff_hz = 4000.0f;  // -6 dB point between the 3 kHz pass edge and the 5 kHz stop edge
const float k_hamming_a0 = 0.54f;
const float k_hamming_a1 = 0.46f;
const float k_dc_pole = 0.999f;         // DC blocker corner at about 4 Hz
const float k_adc_to_int16 = 16.0f;     // 12-bit ADC -> int16 full scale
const float k_int16_max = 32767.0f;
const float k_int16_min = -32768.0f;
const float k_two_pi = 6.28318531f;

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

// ---------------------------------------------------------------------------
// Transmit audio: a 1-bit stream on the I2S data pin
// ---------------------------------------------------------------------------

// Why not the DAC: on the classic ESP32 the ADC's DMA and the DAC's DMA both run through the I2S0 peripheral, and the
// receive side holds it. The second I2S peripheral (I2S1) plays the audio instead, as a stream of bits on one pin:
// each 8 kHz sample becomes one I2S frame of two 32-bit slots, 64 bits, 512 kbit/s in all, clocked by the crystal
// through DMA. The share of ones follows the audio (half of them in silence); two RC poles on the pin turn the stream
// back into the audio (spec 12.7).
const uint8_t k_bits_per_word = 32;
const uint8_t k_words_per_sample = 2;  // the left and the right slot of one I2S frame
const uint32_t k_bit_rate_hz = unlimited::k_modem_rate_hz * k_bits_per_word * k_words_per_sample;
const uint32_t k_ms_per_s = 1000;
const uint32_t k_chunk_ms = 10;        // one audio_output() call, one DMA buffer
const size_t k_chunk_samples = k_chunk_ms * unlimited::k_modem_rate_hz / k_ms_per_s;
const uint8_t k_dma_buffers = 4;       // I2S DMA descriptors: 40 ms of audio queued ahead of the pin
// From audio_output() to the pin: the chunk waits for a free DMA buffer (one chunk), then the buffers ahead of it
// play. PTT is released this long after the transmission's last sample was made (AccessConfig::output_latency_ms).
const uint16_t k_output_latency_ms = (k_dma_buffers + 1) * k_chunk_ms;

// Second-order noise shaping (error feedback, noise transfer (1 - z^-1)^2 = 1 - 2 z^-1 + z^-2): the quantization noise
// of the single bit is pushed far above the audio band, where the RC filter removes it: through the wiring's two RC
// poles, about 80 dB of signal to noise in 300..2700 Hz for a full-scale crest (tests/test_kiss_tnc.cpp). The input
// is halved: the loop is then stable for any int16 (a second-order loop fed at full scale is not), and a full-scale
// crest moves the pin's average by +-0.25 of the supply: 1.65 V +- 0.83 V.
// The stream is continuous only if the two slots of a frame leave in the buffer's order, the left one first: the
// standard driver's order on the ESP32 (it leaves the peripheral's "right first" bit clear). Sent the other way round
// the noise shaping would break down to about 33 dB, still far above what the receiver needs.
const int32_t k_one = 32768;           // the quantizer's two levels, +-1 in the int16 scale
const int32_t k_input_divisor = 2;
const int32_t k_error_tap = 2;         // the 2 z^-1 of the noise transfer; the z^-2 tap is 1

class OneBitModulator {
public:
    // count samples -> count * k_words_per_sample words, each sent most significant bit first; a 1 drives the pin high.
    void render(const int16_t* in, size_t count, uint32_t* out) {
        for (size_t i = 0; i < count; ++i) {
            const int32_t x = in[i] / k_input_divisor;
            for (uint32_t w = 0; w < k_words_per_sample; ++w) {
                uint32_t word = 0;
                for (uint32_t b = 0; b < k_bits_per_word; ++b) {  // 14 instructions per bit on the ESP32 (-Os)
                    const int32_t wanted = x - k_error_tap * error_ + error_before_;
                    const bool high = wanted >= 0;
                    word = (word << 1) | (high ? 1u : 0u);
                    error_before_ = error_;
                    error_ = (high ? k_one : -k_one) - wanted;
                }
                *out++ = word;
            }
        }
    }

private:
    int32_t error_ = 0;         // the quantizer's error one bit ago
    int32_t error_before_ = 0;  // and two bits ago
};

// ---------------------------------------------------------------------------
// The computer: KISS in, with back-pressure
// ---------------------------------------------------------------------------

const size_t k_host_read_bytes = 256;  // read from the serial port at a time

// KISS bytes from the computer into the modem (spec 12.1, 12.2). What host_input() does not take (the send queue or
// its frame slots are full) waits here, and the port is not read again until it is taken: the serial driver's buffer
// holds what follows. Port: Arduino's HardwareSerial on the board (available(), read(buffer, size)), a stand-in in the
// tests.
class ComputerPort {
public:
    template <typename Port>
    void service(unlimited::Modem& modem, Port& port) {
        for (;;) {
            if (from_ == to_) {
                const int available = port.available();
                if (available <= 0) return;
                const size_t wanted = static_cast<size_t>(available) < k_host_read_bytes
                                          ? static_cast<size_t>(available)
                                          : k_host_read_bytes;
                to_ = port.read(pending_, wanted);
                from_ = 0;
                if (to_ == 0) return;
            }
            from_ += modem.host_input(pending_ + from_, to_ - from_);
            if (from_ < to_) return;  // the modem is full: the rest waits here, the port waits in its buffer
        }
    }

    size_t waiting() const { return to_ - from_; }  // read from the port, not taken by the modem yet

private:
    uint8_t pending_[k_host_read_bytes] = {};
    size_t from_ = 0;
    size_t to_ = 0;
};

}  // namespace kiss_tnc
