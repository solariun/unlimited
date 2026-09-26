// Unlimited transmitter for an Arduino Uno / Nano (ATmega328P, 16 MHz).
//
// Every line typed on the serial port (9600 baud) is sent as one Unlimited packet (sync word, length,
// payload, CRC-16), so rx_esp32 or `unlimited_decode --packet` print it back. The PTT pin is keyed while
// the encoder is busy; a line typed during a transmission is appended to it or sent right after.
// There is no flow control: the hf preset sends about 17 bytes/s (139 bit/s), and while a packet waits for the
// encoder queue the sketch holds one more line plus the 64-byte serial buffer, so pace pasted text.
// Every slot carries a peak (100% duty, average power 0.81 of the tune tone): on a duty-limited rig reduce
// the drive for long transmissions.
//
// Audio path: Timer2 ticks at 8 kHz and its ISR loads Encoder::next_sample() into the Timer1 PWM on
// pin 9 (OC1A). Timer1 runs at 64 kHz with 250 steps (about 8 bits), exactly 8 PWM periods per sample,
// and both timers start in a fixed phase: every sample reaches the pin with the same latency, so the PWM
// adds no timing-jitter spurs. Timer1 and Timer2 are taken (no Servo, no tone(), no PWM on pins 3, 10, 11).
// The ISR takes 600..720 CPU cycles per sample on average and at most about 1,500 of the 2,000 per tick, for every
// preset: no tick is lost and loop() keeps about 65% of the CPU. `make check_embedded` measures this ISR on a
// cycle-counting ATmega328P model (tests/avr) and fails above 1,600 cycles.
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
// T 32 ms, 5 bits per peak on 32 tones below f_ref 2132 Hz (band 869..2132 Hz); decoded by every profile.
const unlimited::Preset k_preset = unlimited::Preset::hf;
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

// Keeps the encoder queue topped up; a frame boundary with an empty queue ends the transmission.
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
    Serial.println(F("Unlimited tx_uno: type a line to send it"));
}

void loop() {
    read_serial();
    feed_encoder();
    update_transmitter();
}
