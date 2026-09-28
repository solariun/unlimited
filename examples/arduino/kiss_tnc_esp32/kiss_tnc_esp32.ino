// Unlimited KISS TNC for an ESP32: a program on a computer (any AX.25 or KISS program) talks KISS to this board over
// its USB serial port, and the board is the modem. Every KISS frame the computer sends goes on the air as one
// Unlimited transmission; every transmission heard comes back to the computer as a KISS frame (spec 12.7). It runs the
// portable core of the PC's unlimited_modem (unlimited::Modem, spec 12.2) with the same rules (spec 12.1): the channel
// check (DCD off, dwait, p-persistence), PTT with the TX delay, and receptions shorter than the minimum frame
// (15 bytes) never passed to the computer.
//
//   computer                              ESP32                                                  radio
//   KISS, 115200 baud --> Serial --> host_input() --> send queue --> audio_output() --> GPIO25 --> mic / data in
//                     <-- Serial <-- KISS frames  <-- receiver   <-- audio_input()  <-- GPIO36 <-- speaker / data out
//                                    tick(): DCD, dwait, p-persistence, TX delay ----> GPIO4 ----> PTT
//
// Audio in: the ADC's DMA at 24 kHz, decimated to 8 kHz exactly as rx_esp32 does (a DC blocker and a 47-tap low-pass,
// kiss_tnc.h). loop() blocks in adc_continuous_read() until the next 10 ms frame arrives and runs the receiver there,
// in task context; the same pass serves the computer's bytes and the control side's timers, so nothing polls.
//
// Audio out: on the classic ESP32 the ADC's DMA and the DAC's DMA both need the I2S0 peripheral, which the ADC holds,
// so the DAC cannot play by DMA while the receiver listens. The audio goes out on the second I2S peripheral instead,
// as a stream of bits on GPIO25: 64 bits per 8 kHz sample, 512 kbit/s, clocked by the crystal through DMA. The share
// of ones follows the audio (a second-order sigma-delta modulator, kiss_tnc.h) and two RC poles turn the stream back
// into the audio, like tx_uno's PWM. A task on core 0 feeds the DMA: it takes 10 ms of audio from audio_output(),
// turns it into bits and blocks in i2s_channel_write() until a DMA buffer is free. Idle, it plays silence (half ones:
// a steady 1.65 V after the filter).
//
// Contexts (spec 12.2): loop() on core 1 calls audio_input() (receive), host_input() (computer) and tick() (control);
// the output task on core 0 calls audio_output() (audio); the core hands over between them without locks. CPU, an
// estimate from the PC's measurements (spec 12.7): the receiver and the decimator about 1..6 % of core 1 at any speed,
// the 1-bit output about 3.5 % of core 0; loopback_esp32 measures the receiver on a board.
//
// Serial is the computer's KISS port: after the start-up lines, which end with a FEND so a KISS program that read
// them starts in step, nothing but KISS is written to it. There is no flow control toward the computer (a USB-serial
// bridge has no handshake lines to the ESP32): the send queue holds 2048 bytes (341 s of air at 6 bytes/s) and the
// serial driver 1024 more, so keep an AX.25 program's window times its frame size (MAXFRAME x PACLEN) below that.
// KISS parameter frames (TXDELAY, P, SLOTTIME, TXTAIL, FULLDUPLEX) are ignored: the settings below rule, as in
// unlimited_modem.
//
// Wiring (classic ESP32 DevKit; all grounds joined, the radio's audio ground included):
//
//   Receive audio, as rx_esp32 (radio speaker or data out -> GPIO36):
//
//     radio audio out --||--+------------ GPIO36 (VP, ADC1 channel 0)
//                      1uF  |
//             3V3 --[10k]---+---[10k]-- GND          the bias: mid-scale, 1.65 V
//
//     Keep the audio under about 1.5 V p-p (the ADC's range with 12 dB attenuation is 0.15..2.45 V) and loud enough
//     that the band's hiss is plainly there with no signal (tens of mV): the ADC's own noise must stay below it.
//     Optional: 1k in series and 47 nF to GND at the pin keeps strong hiss above 12 kHz from aliasing.
//
//   Transmit audio (GPIO25 -> radio mic or data in): two RC poles at 3.4 kHz turn the bit stream into the audio (the
//   stream's noise lies far above it, up to 256 kHz, where the poles take 75 dB off), the 1 uF blocks the 1.65 V
//   average, and the divider brings the beeps' full-scale crests, about 1.1 V p-p after the poles, to mic level:
//   about 10 mV p-p at 1500 Hz.
//
//     GPIO25 --[1k]--+--[10k]--+--||--[47k]--+-- radio mic / data input
//                    |         |  1uF        |
//                  47nF      4.7nF        [470R]
//                    |         |             |
//                   GND       GND           GND
//
//     A data (line) input wants about 100..300 mV p-p: 4.7k instead of the 470R (about 100 mV p-p). Or a 10k trimmer
//     in place of the 470R with the output on its wiper (0..190 mV p-p): set full power on the beeps' crests with the
//     ALC not moving.
//     Put the 1k and the 47 nF right at the pin (the stream switches at up to 512 kHz) and keep the board away from
//     the antenna and its feed line, as any computer near an HF receiver.
//
//   PTT, k_ptt_active_level HIGH:
//
//     GPIO4 --[1k]--+-- base of an NPN (2N2222, BC547); emitter to GND, collector to the radio's PTT line
//                   |   (the radio pulls the line up; the transistor grounds it: transmit)
//                 [10k] keeps the transistor off while the ESP32 boots (the pin floats until setup())
//                   |
//                  GND
//
//     Or an optocoupler (PC817), with no ground shared with the PTT line: GPIO4 --[330R]-- LED anode, cathode to GND;
//     the phototransistor's collector to the PTT line, its emitter to the radio's PTT ground.
//
//   DCD: GPIO2, the on-board LED of most DevKit boards (or an LED and 330R to GND). It is on while a transmission is
//   being decoded (Modem::dcd(), spec 3.10); the TNC waits for it to go off before it transmits.
//
// Flashing: install the ESP32 core (Arduino IDE: Boards Manager, "esp32" by Espressif, 3.x) and this library; open
// this sketch, choose the board "ESP32 Dev Module" and upload. With arduino-cli, from the library's folder:
//     arduino-cli compile --fqbn esp32:esp32:esp32 --library . examples/arduino/kiss_tnc_esp32
//     arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/cu.usbserial-0001 examples/arduino/kiss_tnc_esp32
// (Linux: -p /dev/ttyUSB0). After a reset the board prints its settings once, then speaks only KISS. Opening the port
// may reset a DevKit (its auto-reset circuit): it is ready again about a second later.
//
// First test, with no radio: a direct audio cable to a computer's sound card, both ways (GPIO25's network to the
// card's input: a line input takes the audio after the 1 uF, 1.1 V p-p; the card's output to GPIO36's network, its
// volume set so the audio stays under 1.5 V p-p), and unlimited_modem on that computer at the same speed. Any KISS
// program can drive the board; with AX25Toolkit's ax25tnc, which keeps the port open:
//   1. unlimited_modem --list-devices, then with the card's number N:  unlimited_modem -d coreaudio:N --bps 6 --monitor
//      (Linux: -d alsa:N). Its default PTT is VOX: a 150 ms lead tone before each frame, which the board lets go.
//   2. The board's side:  ax25tnc -m unproto -M -b 115200 -c N0CALL-1 /dev/cu.usbserial-0001
//      The computer's side, in another terminal:  ax25tnc -m unproto -M -c N0CALL-2 /tmp/unlimited
//   3. Board -> computer: type "hello from the ESP32" on the board's side. The board keys GPIO4 for the frame (its
//      16 bytes of AX.25 header and the text: about 6.3 s at 6 bytes/s with the TX delay and the tail);
//      unlimited_modem's monitor prints it with its pitch and SNR, and the computer's ax25tnc shows N0CALL-1>CQ.
//   4. Computer -> board: type a line on the computer's side. The DCD LED lights while the board decodes it, and the
//      board's ax25tnc shows N0CALL-2>CQ once the frame's 15th byte is in (its first byte waits 2.3 s at 6 bytes/s).
// Through radios: the same with the board on one radio and the computer on another, both at the same speed; the
// computer keys its radio with unlimited_modem's --ptt (rts, dtr, icom, ...). Set each transmitter's audio for full
// power on the beeps with the ALC not moving, and each receiver's audio as above.
#include <unlimited.h>

#include "kiss_tnc.h"

#include <driver/i2s_std.h>
#include <esp_adc/adc_continuous.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <new>

#if !CONFIG_IDF_TARGET_ESP32
#error "kiss_tnc_esp32 is for the classic ESP32: its ADC DMA and the 1-bit output on the second I2S peripheral"
#endif

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Settings. The speed must be the same at both stations; everything else belongs to this station alone.
// ---------------------------------------------------------------------------------------------------------------------

// Speed, in hundredths of a byte per second: 600 = 6.00 bytes/s, the HF default (slots of 16.7 ms, a signal 264 Hz
// wide). Any 100..2500 (1.00..25.00 bytes/s): slower is narrower and holds on under weaker signals, faster needs more
// signal (spec 1.3: 1 byte/s for very weak HF paths, 6 for HF, 12 for good HF and AM, 25 for FM).
const uint16_t k_centi_bytes_per_second = unlimited::k_default_centi_bytes_per_second;  // 600
// The pitch of the beeps this station sends, 300..2700 Hz. The receiver finds the other station's pitch by itself, so
// mistuning on USB or LSB does not matter.
const uint16_t k_tone_hz = unlimited::k_default_tone_hz;  // 1500
// The radio's audio passband: what its filters let through. The signal must fit inside (the TNC refuses to start
// when it does not) and the pitch search stays inside. SSB 300..2700 Hz (a 2.4 kHz filter), AM 100..3000, FM 300..3000.
const uint16_t k_passband_low_hz = unlimited::k_ssb_passband_low_hz;    // 300
const uint16_t k_passband_high_hz = unlimited::k_ssb_passband_high_hz;  // 2700
// The receiver's decision line between a 0 and a 1: k_threshold_auto, the adaptive line (the default: half of the
// START and STOP tones' level on a clean signal, about 70 % of it on a weak one), or a fixed line at 50..90 % of it.
const uint8_t k_threshold_auto = 0;
const uint8_t k_threshold_percent = k_threshold_auto;
// Receptions shorter than this many bytes never reach the computer: speech, CW or noise makes a byte or two readable
// now and then, and an AX.25 frame is at least 15 bytes. A frame's first byte then waits 14 windows (2.3 s at
// 6 bytes/s). 0 hands every reception over from its first byte (for a protocol with frames shorter than 15 bytes).
const uint8_t k_min_frame_bytes = unlimited::k_default_min_frame_bytes;  // 15
// Channel access, as any KISS TNC: after the channel went quiet (DCD off: no transmission being decoded) wait
// k_dwait_ms, then transmit when a random 0..255 is at most k_persist, else wait k_slot_time_ms and draw again.
const uint16_t k_dwait_ms = unlimited::k_default_dwait_ms;          // 1500
const uint8_t k_persist = unlimited::k_default_persist;             // 63: about one chance in four per slot time
const uint16_t k_slot_time_ms = unlimited::k_default_slot_time_ms;  // 100
// HF fades (off by default; both stations must agree): on, a transmission ends only after 2 silent windows, so a fade
// of one window costs a byte instead of the frame, and this station leaves 300 ms of silence before each START.
const bool k_fade_bridge = unlimited::k_default_fade_bridge;  // false
// PTT: this pin keys the transmitter through a transistor or an optocoupler (wiring above); k_ptt_active_level is its
// level while transmitting. The TX delay is silence after keying and before the first byte, while the radio switches
// to transmit (radios need about 20..100 ms); the tail is silence after the last byte, before PTT is released.
const uint8_t k_ptt_pin = 4;
const uint8_t k_ptt_active_level = HIGH;
const uint16_t k_txdelay_ms = unlimited::k_default_txdelay_ms;  // 100
const uint16_t k_txtail_ms = unlimited::k_default_tail_ms;      // 100
// VOX: true when the radio is keyed by the sound itself (no PTT wire). Each transmission then starts with a plain lead
// tone of k_vox_lead_ms and 2 silent slots (the receiver lets the lead go), the TX delay is not used, and the PTT pin
// is never driven.
const bool k_vox = false;
const uint16_t k_vox_lead_ms = unlimited::k_default_vox_lead_ms;  // 150
// The beeps' crests: full scale moves the pin's average by 1.65 V p-p (about 1.1 V p-p after the RC poles), the 1-bit
// stream's largest clean swing. Set the radio's level with the divider or the trimmer (wiring above), not here.
const int16_t k_output_crest = INT16_MAX;

static_assert(k_centi_bytes_per_second >= unlimited::k_min_centi_bytes_per_second &&
                  k_centi_bytes_per_second <= unlimited::k_max_centi_bytes_per_second,
              "k_centi_bytes_per_second: 100..2500 (1.00..25.00 bytes/s)");
static_assert(k_tone_hz >= unlimited::k_min_tone_hz && k_tone_hz <= unlimited::k_max_tone_hz, "k_tone_hz: 300..2700");
static_assert(k_threshold_percent == k_threshold_auto || (k_threshold_percent >= unlimited::k_min_threshold_percent &&
                                                          k_threshold_percent <= unlimited::k_max_threshold_percent),
              "k_threshold_percent: k_threshold_auto or 50..90");
static_assert(k_min_frame_bytes <= unlimited::k_max_min_frame_bytes, "k_min_frame_bytes: 0..64");
static_assert(k_slot_time_ms > 0, "k_slot_time_ms: at least 1");
static_assert(k_output_crest > 0, "k_output_crest: above 0");

// ---------------------------------------------------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------------------------------------------------

const int k_audio_in_pin = 36;    // VP, ADC1 channel 0 (fine with Wi-Fi on)
const int k_audio_out_pin = 25;   // the data line of I2S1: the 1-bit audio
const uint8_t k_dcd_led_pin = 2;  // the on-board LED of most DevKit boards

const uint32_t k_serial_baud = 115200;  // the computer's KISS port (USB serial)
const size_t k_serial_rx_bytes = 1024;  // the serial driver's receive buffer: 89 ms at 115200 baud

const uint32_t k_frame_bytes = kiss_tnc::k_frame_conversions * SOC_ADC_DIGI_RESULT_BYTES;
static_assert(k_frame_bytes % SOC_ADC_DIGI_DATA_BYTES_PER_CONV == 0, "DMA frames hold whole conversions");
const uint32_t k_pool_frames = 8;         // 80 ms of slack for the receiver's slowest pass
const uint32_t k_read_timeout_ms = 100;   // a frame comes every 10 ms; this only bounds a stall of the ADC

const uint32_t k_output_stack_bytes = 4096;
const UBaseType_t k_output_priority = 5;  // above loop()'s 1, below the system's tasks
const BaseType_t k_output_core = 0;       // loop() runs on core 1: the output never waits for the receiver

const uint8_t k_mac_bytes = 6;
const uint32_t k_seed_multiplier = 31;
const uint16_t k_centi = unlimited::k_centi_per_unit;

// ---------------------------------------------------------------------------------------------------------------------
// The modem
// ---------------------------------------------------------------------------------------------------------------------

// Two TNCs on one channel must not draw the same p-persistence numbers: the chip's MAC address differs per board, the
// hardware random number generator per start.
uint32_t persistence_seed() {
    uint8_t mac[k_mac_bytes] = {};
    esp_efuse_mac_get_default(mac);
    uint32_t seed = esp_random();
    for (uint8_t i = 0; i < k_mac_bytes; ++i) seed = seed * k_seed_multiplier + mac[i];
    return seed;
}

unlimited::ModemConfig modem_config() {
    unlimited::Passband passband;
    passband.low_hz = k_passband_low_hz;
    passband.high_hz = k_passband_high_hz;
    unlimited::ModemConfig config;
    config.signal.slot_us = unlimited::slot_us_for_centi_speed(k_centi_bytes_per_second);
    config.signal.tone_hz = k_tone_hz;
    config.signal.amplitude = k_output_crest;
    config.signal.passband = passband;
    config.signal.lead_in_ms = k_vox ? 0 : k_txdelay_ms;
    config.signal.vox_lead_ms = k_vox ? k_vox_lead_ms : 0;
    config.signal.tail_ms = k_txtail_ms;
    config.receiver.slot_us = config.signal.slot_us;
    config.receiver.passband = passband;
    if (k_threshold_percent == k_threshold_auto) {
        config.receiver.decision_mode = unlimited::DecisionMode::adaptive;
    } else {
        config.receiver.decision_mode = unlimited::DecisionMode::fixed;
        config.receiver.threshold_percent = k_threshold_percent;
    }
    config.access.dwait_ms = k_dwait_ms;
    config.access.persist = k_persist;
    config.access.slot_time_ms = k_slot_time_ms;
    config.access.output_latency_ms = kiss_tnc::k_output_latency_ms;
    config.access.seed = persistence_seed();
    config.fade_bridge = k_fade_bridge;
    config.min_frame_bytes = k_min_frame_bytes;
    return config;
}

// Why the settings are refused (the ranges are checked when compiling; this is what only the numbers together tell).
const char* settings_problem(const unlimited::ModemConfig& config) {
    unlimited::ConfigError error = config.sent().check();
    if (error == unlimited::ConfigError::none) error = config.heard().check();
    switch (error) {
        case unlimited::ConfigError::outside_passband:
            return "the signal does not fit the passband at this speed and pitch";
        case unlimited::ConfigError::passband:
            return "the passband leaves no pitch to search at this speed";
        case unlimited::ConfigError::none:
        case unlimited::ConfigError::sample_rate:
        case unlimited::ConfigError::tone:
        case unlimited::ConfigError::slot:
        case unlimited::ConfigError::amplitude:
        case unlimited::ConfigError::threshold:
        case unlimited::ConfigError::decision_mode:
            break;
    }
    return "the modem refuses these settings";
}

// Receive context: the modem's bytes for the computer, already KISS.
void on_host(const uint8_t* data, size_t size, void*) {
    Serial.write(data, size);
}

// Control context: PTT around each transmission (VOX: the sound keys the radio, nothing is driven).
void on_ptt(bool on, void*) {
    if (k_vox) return;
    digitalWrite(k_ptt_pin, on ? k_ptt_active_level : (k_ptt_active_level == HIGH ? LOW : HIGH));
}

// The modem (about 20 KB, static) is built in setup() once its settings are checked.
alignas(unlimited::Modem) uint8_t g_modem_storage[sizeof(unlimited::Modem)];
unlimited::Modem* g_modem = nullptr;
kiss_tnc::Decimator g_decimator;
kiss_tnc::OneBitModulator g_modulator;
kiss_tnc::ComputerPort g_computer;

adc_continuous_handle_t g_adc = nullptr;
uint8_t g_raw[k_frame_bytes];
adc_continuous_data_t g_parsed[kiss_tnc::k_frame_conversions];
int16_t g_heard[kiss_tnc::k_frame_samples];

i2s_chan_handle_t g_i2s = nullptr;
int16_t g_played[kiss_tnc::k_chunk_samples];
uint32_t g_words[kiss_tnc::k_chunk_samples * kiss_tnc::k_words_per_sample];

// ---------------------------------------------------------------------------------------------------------------------
// Audio in: as rx_esp32
// ---------------------------------------------------------------------------------------------------------------------

bool start_adc() {
    adc_unit_t unit;
    adc_channel_t channel;
    if (adc_continuous_io_to_channel(k_audio_in_pin, &unit, &channel) != ESP_OK || unit != ADC_UNIT_1) return false;
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
    config.sample_freq_hz = kiss_tnc::k_adc_rate_hz;
    config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
    return adc_continuous_config(g_adc, &config) == ESP_OK && adc_continuous_start(g_adc) == ESP_OK;
}

// ---------------------------------------------------------------------------------------------------------------------
// Audio out: the 1-bit stream on I2S1
// ---------------------------------------------------------------------------------------------------------------------

// One I2S frame per 8 kHz sample: two 32-bit slots, MSB first, back to back on the data pin (512 kbit/s); the bit
// clock and the word select stay inside (no pin). Every DMA buffer holds silence before the start, so the pin never
// rests low.
bool start_output() {
    i2s_chan_config_t channel = {};
    channel.id = I2S_NUM_1;
    channel.role = I2S_ROLE_MASTER;
    channel.dma_desc_num = kiss_tnc::k_dma_buffers;
    channel.dma_frame_num = kiss_tnc::k_chunk_samples;
    if (i2s_new_channel(&channel, &g_i2s, nullptr) != ESP_OK) return false;
    i2s_std_config_t config = {};
    config.clk_cfg.sample_rate_hz = unlimited::k_modem_rate_hz;
    config.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    config.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    config.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    config.slot_cfg.ws_width = kiss_tnc::k_bits_per_word;
    config.slot_cfg.ws_pol = false;
    config.slot_cfg.bit_shift = false;
    config.slot_cfg.msb_right = false;
    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.ws = I2S_GPIO_UNUSED;
    config.gpio_cfg.dout = static_cast<gpio_num_t>(k_audio_out_pin);
    config.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(g_i2s, &config) != ESP_OK) return false;
    for (size_t i = 0; i < kiss_tnc::k_chunk_samples; ++i) g_played[i] = 0;
    for (uint8_t i = 0; i < kiss_tnc::k_dma_buffers; ++i) {
        g_modulator.render(g_played, kiss_tnc::k_chunk_samples, g_words);
        size_t loaded = 0;
        if (i2s_channel_preload_data(g_i2s, g_words, sizeof(g_words), &loaded) != ESP_OK) return false;
    }
    return i2s_channel_enable(g_i2s) == ESP_OK;
}

// Audio context: the modem's next 10 ms, as bits, into the DMA. i2s_channel_write() blocks until a DMA buffer is
// free, so the crystal paces this task; audio_output() itself never blocks (silence when nothing is sent).
void output_task(void*) {
    for (;;) {
        g_modem->audio_output(g_played, kiss_tnc::k_chunk_samples);
        g_modulator.render(g_played, kiss_tnc::k_chunk_samples, g_words);
        size_t written = 0;
        i2s_channel_write(g_i2s, g_words, sizeof(g_words), &written, portMAX_DELAY);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Start-up lines (then only KISS)
// ---------------------------------------------------------------------------------------------------------------------

void print_settings() {
    const unlimited::EncoderConfig& signal = g_modem->signal();
    const unlimited::Band band = unlimited::occupied_band(signal);
    const unlimited::PassbandFit fit = unlimited::passband_fit(signal);
    Serial.printf("Unlimited KISS TNC: KISS on this port at %lu baud; speed %u.%02u bytes/s (T %lu us), pitch %u Hz\n",
                  static_cast<unsigned long>(k_serial_baud), k_centi_bytes_per_second / k_centi,
                  k_centi_bytes_per_second % k_centi, static_cast<unsigned long>(signal.slot_us), signal.tone_hz);
    Serial.printf("  occupied %u-%u Hz in the passband %u-%u Hz: shift tolerance -%d/+%d Hz\n", band.low_hz,
                  band.high_hz, signal.passband.low_hz, signal.passband.high_hz, fit.margin_low_hz, fit.margin_high_hz);
    if (k_vox)
        Serial.printf("  PTT by VOX: a %u ms lead tone and 2 silent slots before each transmission; tail %u ms\n",
                      k_vox_lead_ms, k_txtail_ms);
    else
        Serial.printf("  PTT on GPIO%u (%s to transmit), TX delay %u ms, tail %u ms\n", k_ptt_pin,
                      k_ptt_active_level == HIGH ? "high" : "low", signal.lead_in_ms, k_txtail_ms);
    Serial.printf("  channel: dwait %u ms, persist %u, slot time %u ms; receiver: %s decision line, minimum frame %u "
                  "bytes, fade bridge %s\n",
                  k_dwait_ms, k_persist, k_slot_time_ms, k_threshold_percent == k_threshold_auto ? "adaptive" : "fixed",
                  k_min_frame_bytes, k_fade_bridge ? "on" : "off");
    Serial.printf("  audio in GPIO%d (ADC, 24 kHz), audio out GPIO%d (1-bit, %lu kbit/s), DCD LED GPIO%u\n",
                  k_audio_in_pin, k_audio_out_pin,
                  static_cast<unsigned long>(kiss_tnc::k_bit_rate_hz / kiss_tnc::k_ms_per_s), k_dcd_led_pin);
}

void halt(const char* why) {
    Serial.printf("kiss_tnc_esp32: %s\n", why);
    vTaskDelete(nullptr);
}

}  // namespace

void setup() {
    pinMode(k_ptt_pin, OUTPUT);
    digitalWrite(k_ptt_pin, k_ptt_active_level == HIGH ? LOW : HIGH);
    pinMode(k_dcd_led_pin, OUTPUT);
    digitalWrite(k_dcd_led_pin, LOW);
    Serial.setRxBufferSize(k_serial_rx_bytes);
    Serial.begin(k_serial_baud);
    g_decimator.begin();
    if (!start_adc()) halt("ADC setup failed");
    const unlimited::ModemConfig config = modem_config();
    if (!config.valid()) halt(settings_problem(config));
    g_modem = new (g_modem_storage) unlimited::Modem(config, &on_host, &on_ptt, nullptr);
    if (!start_output()) halt("I2S output setup failed");
    if (xTaskCreatePinnedToCore(output_task, "tnc_audio_out", k_output_stack_bytes, nullptr, k_output_priority,
                                nullptr, k_output_core) != pdPASS)
        halt("the output task did not start");
    print_settings();
    Serial.write(unlimited::k_kiss_fend);  // a KISS program that read the lines above starts in step
}

// Receive, computer, control: one pass per 10 ms ADC frame.
void loop() {
    uint32_t bytes = 0;
    if (adc_continuous_read(g_adc, g_raw, sizeof(g_raw), &bytes, k_read_timeout_ms) == ESP_OK) {
        uint32_t count = 0;
        if (adc_continuous_parse_data(g_adc, g_raw, bytes, g_parsed, &count) == ESP_OK) {
            size_t samples = 0;
            for (uint32_t i = 0; i < count; ++i) {
                if (g_parsed[i].valid && g_decimator.push(static_cast<float>(g_parsed[i].raw_data), g_heard[samples]))
                    ++samples;
            }
            g_modem->audio_input(g_heard, samples);
        }
    }
    g_computer.service(*g_modem, Serial);
    g_modem->tick(millis());
    digitalWrite(k_dcd_led_pin, g_modem->dcd() ? HIGH : LOW);
}
