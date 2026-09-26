// Unlimited transmitter for an Arduino Uno / Nano (ATmega328P, 16 MHz).
//
// Every line typed on the serial port (9600 baud) is sent as one Unlimited packet (sync word, length,
// payload, CRC-16), so rx_esp32 or `unlimited_decode --packet` print it back. The PTT pin is keyed while
// the encoder is busy; a line typed during a transmission is appended to it or sent right after.
//
// The signal (preset hf): short beeps on one 1500 Hz pitch, one slot of 16 ms per bit (a beep is a 1, silence
// a 0), 8 bits per package between START/STOP markers: 55.6 bit/s, about 7 bytes/s. It occupies 1362-1638 Hz,
// so it fits a 2.4 kHz SSB filter (300-2700 Hz) with +-1062 Hz of tuning room; the sketch prints this at start.
// There is no flow control: while a packet waits for the encoder queue the sketch holds one more line plus the
// 64-byte serial buffer, so pace pasted text. Random data averages about 0.37 of the tune tone's power.
//
// Audio path: Timer2 ticks at 8 kHz and its ISR loads Encoder::next_sample() into the Timer1 PWM on
// pin 9 (OC1A). Timer1 runs at 64 kHz with 250 steps (about 8 bits), exactly 8 PWM periods per sample,
// and both timers start in a fixed phase: every sample reaches the pin with the same latency, so the PWM
// adds no timing-jitter spurs. Timer1 and Timer2 are taken (no Servo, no tone(), no PWM on pins 3, 10, 11).
// The encoder is integer-only (no float, no division in the ISR). The ISR takes about 500..630 CPU cycles per
// sample on average and at most about 1,010 of the 2,000 per tick, for every preset: no tick is lost and loop()
// keeps about 70% of the CPU. `make check_embedded` measures this ISR on a cycle-counting ATmega328P model
// (tests/avr) and fails above 1,600 cycles.
//
// Wiring:
//   pin 9 --[1k]--+--[10k]--+--||--[47k]--+-- radio mic / data input
//                 |         |  1uF        |
//               47nF      4.7nF         [470R]
//                 |         |             |
//                GND       GND           GND
//     Two RC poles at 3.4 kHz: -1.6 dB at 1500 Hz and about -50 dB at the 64 kHz carrier. The 1 uF blocks
//     the 2.5 V bias; 47k/470R brings the 3.5 V p-p output down to mic level (35 mV p-p). A 10k trimmer
//     instead of the divider lets you set the level with the tune tone: full power, ALC not moving.
//   pin 8 --[1k]-- base of an NPN (2N2222), emitter to GND, collector to the radio PTT line (HIGH = TX).
//   The on-board LED follows PTT.
#include <unlimited.h>

namespace {

const uint32_t k_serial_baud = 9600;
const uint8_t k_audio_pin = 9;                             // OC1A
const uint8_t k_ptt_pin = 8;
const uint8_t k_ptt_led_pin = LED_BUILTIN;
const unlimited::Preset k_preset = unlimited::Preset::hf;  // T 16 ms, N 8, 1500 Hz; every receiver profile hears it
const uint16_t k_ptt_settle_ms = 100;                      // silent lead-in after keying (relay, TX delay)
const uint8_t k_max_payload = 96;                          // longest line sent as one packet
static_assert(k_max_payload <= unlimited::k_packet_max_payload, "a line must fit one packet");

const uint32_t k_sample_rate_hz = 8000;
const uint8_t k_pwm_periods_per_sample = 8;
const uint8_t k_tick_prescaler = 8;
// Timer1: fast PWM, TOP = ICR1, no prescaler: 16 MHz / 250 = 64 kHz.
const uint16_t k_pwm_top = F_CPU / (k_sample_rate_hz * k_pwm_periods_per_sample) - 1;
// Timer2: CTC, clk/8, 250 counts: 8 kHz, i.e. the same 2000 CPU cycles as 8 PWM periods.
const uint8_t k_tick_top = F_CPU / k_tick_prescaler / k_sample_rate_hz - 1;
static_assert(k_tick_top == k_pwm_top, "one tick must span exactly k_pwm_periods_per_sample PWM periods");
const uint8_t k_pwm_middle = (k_pwm_top + 1) / 2;          // output level of a zero sample
const uint8_t k_sample_high_byte = 8;                      // int16 -> signed 8 bits
const uint8_t k_level_shift = 7;                           // (s8 * 125) >> 7 spans -125..124
// Timer1 count when Timer2 starts: every tick then comes just after a Timer1 BOTTOM, and the OCR1A write
// (about 80 cycles into the ISR) keeps about 150 cycles of slack, enough to sit behind a millis() or UART
// interrupt, before the double-buffered update at the next BOTTOM.
const uint16_t k_pwm_start_phase = 16;

unlimited::EncoderConfig transmitter_config() {
    unlimited::EncoderConfig config = unlimited::EncoderConfig::from_preset(k_preset, k_sample_rate_hz);
    if (config.lead_in_ms < k_ptt_settle_ms) config.lead_in_ms = k_ptt_settle_ms;
    return config;
}

unlimited::Encoder g_encoder(transmitter_config());
volatile uint8_t g_next_level = k_pwm_middle;

uint8_t g_line[k_max_payload];
uint8_t g_line_size = 0;
bool g_line_ready = false;
uint8_t g_packet[k_max_payload + unlimited::k_packet_overhead];
uint8_t g_packet_size = 0;
uint8_t g_packet_sent = 0;

uint8_t pwm_level(int16_t sample) {
    const int16_t high = static_cast<int8_t>(sample >> k_sample_high_byte);
    return static_cast<uint8_t>(k_pwm_middle + ((high * k_pwm_middle) >> k_level_shift));
}

void start_audio() {
    pinMode(k_audio_pin, OUTPUT);
    noInterrupts();
    GTCCR = _BV(TSM) | _BV(PSRASY) | _BV(PSRSYNC);  // hold the prescalers while both timers are set up
    TCCR1A = _BV(COM1A1) | _BV(WGM11);              // mode 14, non-inverting PWM on OC1A
    TCCR1B = _BV(WGM13) | _BV(WGM12) | _BV(CS10);
    ICR1 = k_pwm_top;
    OCR1A = k_pwm_middle;
    TCCR2A = _BV(WGM21);                            // CTC on OCR2A
    TCCR2B = _BV(CS21);                             // clk / 8
    OCR2A = k_tick_top;
    TIMSK2 = _BV(OCIE2A);
    TCNT2 = 0;
    TCNT1 = k_pwm_start_phase;
    GTCCR = 0;
    interrupts();
}

// "occupied bandwidth 276 Hz (1362-1638 Hz); passband 300-2700 Hz: fits; shift tolerance -1062/+1062 Hz"
// (the demos' bandwidth line, integer only: no float code on the AVR). The tolerance stops where the band would
// leave the passband or the pitch the search of the receiver that hears this sender (passband_fit(config), spec 1.5).
void print_bandwidth(const unlimited::EncoderConfig& config) {
    const unlimited::Band band = unlimited::occupied_band(config);
    const unlimited::PassbandFit fit = unlimited::passband_fit(config);
    Serial.print(F("occupied bandwidth "));
    Serial.print(band.width_hz);
    Serial.print(F(" Hz ("));
    Serial.print(band.low_hz);
    Serial.print('-');
    Serial.print(band.high_hz);
    Serial.print(F(" Hz); passband "));
    Serial.print(config.passband.low_hz);
    Serial.print('-');
    Serial.print(config.passband.high_hz);
    if (!fit.fits) {
        Serial.println(F(" Hz: does not fit"));
        return;
    }
    Serial.print(F(" Hz: fits; shift tolerance -"));
    Serial.print(fit.margin_low_hz);
    Serial.print(F("/+"));
    Serial.print(fit.margin_high_hz);
    Serial.println(F(" Hz"));
}

// Collects one line; it stays in g_line until the previous packet is in the encoder queue.
void read_serial() {
    while (!g_line_ready && Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r' || c == '\n') {
            g_line_ready = g_line_size > 0;
        } else {
            g_line[g_line_size++] = static_cast<uint8_t>(c);
            g_line_ready = g_line_size == k_max_payload;
        }
    }
    if (g_line_ready && g_packet_sent == g_packet_size) {
        g_packet_size = static_cast<uint8_t>(unlimited::packet_build(g_line, g_line_size, g_packet, sizeof(g_packet)));
        g_packet_sent = 0;
        g_line_size = 0;
        g_line_ready = false;
    }
}

// Keeps the encoder queue topped up; an empty queue at a package boundary ends the transmission.
void feed_encoder() {
    while (g_packet_sent < g_packet_size && g_encoder.write(g_packet[g_packet_sent])) ++g_packet_sent;
}

void set_ptt(bool on) {
    digitalWrite(k_ptt_pin, on ? HIGH : LOW);
    digitalWrite(k_ptt_led_pin, on ? HIGH : LOW);
}

void update_transmitter() {
    if (g_encoder.busy()) return;
    const bool queued = g_encoder.queued() > 0;
    set_ptt(queued);
    if (queued && g_encoder.start()) Serial.println(F("TX"));
}

}  // namespace

// Writes the sample computed on the previous tick first, so the latency to the pin is constant.
ISR(TIMER2_COMPA_vect) {
    OCR1A = g_next_level;
    g_next_level = pwm_level(g_encoder.next_sample());
}

void setup() {
    pinMode(k_ptt_pin, OUTPUT);
    pinMode(k_ptt_led_pin, OUTPUT);
    set_ptt(false);
    Serial.begin(k_serial_baud);
    start_audio();
    Serial.println(F("Unlimited tx_uno: preset hf, 1500 Hz, T 16 ms, 8 bits per package, 55.6 bit/s"));
    print_bandwidth(g_encoder.config());
    if (!g_encoder.config().valid()) Serial.println(F("tx_uno: the encoder refuses this configuration"));
    Serial.println(F("type a line to send it"));
}

void loop() {
    read_serial();
    feed_encoder();
    update_transmitter();
}
