// The ESP32 KISS TNC's glue on a PC (spec 12.7): examples/arduino/kiss_tnc_esp32/kiss_tnc.h -- the receive audio's
// decimator, the transmit audio's 1-bit modulator and the computer's port with back-pressure -- run on the same Modem
// core as the board, in the sketch's call sequence: loop() (one 10 ms ADC frame through the decimator into
// audio_input(), the computer's bytes through ComputerPort into host_input(), tick()) and the output task
// (audio_output(), the modulator, the DMA's queue of buffers). The other station is unlimited_modem's core, the radios
// are sim::Channel, and the wires are modelled as built: the ADC's 12-bit codes around mid-scale with its own noise,
// the 1-bit stream through the wiring's two RC poles, heard by the other radio's sound card.
#include "../examples/arduino/kiss_tnc_esp32/kiss_tnc.h"
#include "channel.hpp"
#include "resampler.hpp"
#include "test_harness.hpp"
#include "unlimited/modem.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <random>
#include <vector>

using unlimited::KissDecoder;
using unlimited::KissStep;
using unlimited::Modem;
using unlimited::ModemConfig;
using unlimited::ModemCounters;

namespace sim = unlimited::sim;

namespace {

using std::int16_t;
using std::int64_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

typedef std::vector<uint8_t> Bytes;

const double k_pi = 3.14159265358979323846;
const double k_full_scale = 32768.0;
const double k_audio_rate_hz = unlimited::k_modem_rate_hz;
const double k_ms_per_s = 1000.0;
const double k_power_db = 10.0;
const double k_amplitude_db = 20.0;

// One pass of the sketch: an ADC frame of 240 codes, one output chunk of 80 samples, 10 ms.
const uint32_t k_step_ms = kiss_tnc::k_chunk_ms;
const size_t k_step_samples = kiss_tnc::k_chunk_samples;
const size_t k_step_codes = kiss_tnc::k_frame_conversions;
const size_t k_step_words = k_step_samples * kiss_tnc::k_words_per_sample;
static_assert(k_step_codes == k_step_samples * kiss_tnc::k_decimation, "one ADC frame per output chunk");
const uint32_t k_bits_per_sample = kiss_tnc::k_bits_per_word * kiss_tnc::k_words_per_sample;

// The ADC: 12 bits, the bias at mid-scale (the wiring's 10k/10k), the radio's audio at about half the range, and the
// converter's own noise, white up to 12 kHz (the decimator must keep it from folding into the band).
const double k_adc_mid = 2048.0;
const double k_adc_max = 4095.0;
const double k_adc_swing = 1024.0;  // codes per unit of the receiver's audio (a key-down tone reaches about 0.7)
const double k_adc_noise = 3.0;     // codes rms

// The transmit wiring: two RC poles (1k with 47 nF, 10k with 4.7 nF: 3386 Hz), then the other radio and its sound card,
// modelled as a third-order CIC down to 8 kHz. Levels in units of half the supply around its middle: the pin's
// average moves by x / (2 k_one) of it for a sample x.
const double k_rc_corner_hz = 3386.0;
const int k_cic_order = 3;
const double k_fixed_scale = 65536.0;  // the CIC runs on integers: the RC output times this

// The radios: USB with a 2.4 kHz filter, sim::Channel's SNR convention (key-down tone over the noise in 2500 Hz).
const double k_reference_bandwidth_hz = 2500.0;
const double k_receiver_bandwidth_hz = 2400.0;
const double k_noise_peak_sigmas = 5.0;
const double k_headroom = 0.9;
const double k_strong_snr_db = 20.0;  // exactness at a strong signal (V22)
const double k_offset_hz = 40.0;      // the radios mistuned, one way and the other

// The measurement band of the 1-bit output and the tone used there.
const double k_band_low_hz = 300.0;
const double k_band_high_hz = 2700.0;
const double k_test_tone_hz = 1500.0;
const size_t k_settle_samples = 8000;  // 1 s before measuring: the filters and the loop settle
const size_t k_measure_samples = 8000; // 1 s measured: 1 Hz bins

// A beep at the pin: louder than this share of a full-scale crest's level there.
const double k_loud_share = 0.1;
const uint32_t k_max_run_ms = 90000;
const uint32_t k_seed = 2026;

// ---------------------------------------------------------------------------
// KISS
// ---------------------------------------------------------------------------

Bytes kiss_frame(const Bytes& data) {
    Bytes out(1, unlimited::k_kiss_fend);
    out.push_back(unlimited::k_kiss_data);
    for (size_t i = 0; i < data.size(); ++i) {
        uint8_t escaped[unlimited::k_kiss_escaped_max];
        const uint8_t size = unlimited::kiss_escape(data[i], escaped);
        out.insert(out.end(), escaped, escaped + size);
    }
    out.push_back(unlimited::k_kiss_fend);
    return out;
}

std::vector<Bytes> kiss_frames(const Bytes& stream) {
    KissDecoder decoder;
    std::vector<Bytes> frames;
    Bytes frame;
    for (size_t i = 0; i < stream.size(); ++i) {
        uint8_t value = 0;
        const KissStep step = decoder.feed(stream[i], value);
        if (step == KissStep::data) frame.push_back(value);
        if (step == KissStep::end) {
            frames.push_back(frame);
            frame.clear();
        }
        if (!decoder.in_data()) frame.clear();
    }
    return frames;
}

// A frame of random bytes with KISS's special bytes inside.
Bytes frame_bytes(size_t length, uint32_t seed) {
    std::mt19937 generator(seed);
    Bytes bytes(length);
    for (size_t i = 0; i < length; ++i) bytes[i] = static_cast<uint8_t>(generator());
    bytes[length / 3] = unlimited::k_kiss_fend;
    bytes[length / 3 + 1] = unlimited::k_kiss_fesc;
    return bytes;
}

// ---------------------------------------------------------------------------
// The wires
// ---------------------------------------------------------------------------

// The sketch's Serial as its computer drives it; read() insists on the back-pressure rule: the port is read only when
// everything read before has been taken by the modem.
struct ComputerLine {
    Bytes from_computer;
    size_t read_at = 0;
    Bytes to_computer;
    const kiss_tnc::ComputerPort* port = nullptr;
    size_t early_reads = 0;  // reads while bytes read before still waited

    int available() const { return static_cast<int>(from_computer.size() - read_at); }

    size_t read(uint8_t* buffer, size_t size) {
        if (port != nullptr && port->waiting() != 0) ++early_reads;
        const size_t count = std::min(size, from_computer.size() - read_at);
        std::copy(from_computer.begin() + static_cast<std::ptrdiff_t>(read_at),
                  from_computer.begin() + static_cast<std::ptrdiff_t>(read_at + count), buffer);
        read_at += count;
        return count;
    }
};

// The pin's 1-bit stream as the other radio hears it (see k_rc_corner_hz): out[i] in units of half the supply.
class PinListener {
public:
    PinListener()
        : alpha_(1.0 - std::exp(-2.0 * k_pi * k_rc_corner_hz / kiss_tnc::k_bit_rate_hz)),
          cic_gain_(std::pow(static_cast<double>(k_bits_per_sample), k_cic_order)),
          first_(0.0),
          second_(0.0),
          integrators_(),
          combs_() {}

    void listen(const uint32_t* words, size_t samples, double* out) {
        for (size_t i = 0; i < samples; ++i) {
            for (uint8_t w = 0; w < kiss_tnc::k_words_per_sample; ++w) {
                const uint32_t word = words[i * kiss_tnc::k_words_per_sample + w];
                for (int bit = kiss_tnc::k_bits_per_word - 1; bit >= 0; --bit) {
                    const double level = ((word >> bit) & 1u) != 0 ? 1.0 : -1.0;
                    first_ += alpha_ * (level - first_);
                    second_ += alpha_ * (first_ - second_);
                    uint64_t value = static_cast<uint64_t>(static_cast<int64_t>(std::llround(second_ * k_fixed_scale)));
                    for (int s = 0; s < k_cic_order; ++s) {
                        integrators_[s] += value;
                        value = integrators_[s];
                    }
                }
            }
            uint64_t value = integrators_[k_cic_order - 1];
            for (int s = 0; s < k_cic_order; ++s) {
                const uint64_t difference = value - combs_[s];
                combs_[s] = value;
                value = difference;
            }
            out[i] = static_cast<double>(static_cast<int64_t>(value)) / (k_fixed_scale * cic_gain_);
        }
    }

private:
    double alpha_;
    double cic_gain_;
    double first_;
    double second_;
    uint64_t integrators_[k_cic_order];  // wrap around on purpose: the combs take the differences back
    uint64_t combs_[k_cic_order];
};

// A tone of amplitude 1 in the int16 scale, as PinListener hears it at hz: the modulator holds each sample for its 64
// bits (a boxcar, like one more CIC stage), then the RC poles and the CIC.
double pin_gain(double hz) {
    const double ratio = hz / k_rc_corner_hz;
    const double rc = 1.0 / (1.0 + ratio * ratio);
    const double x = k_pi * hz / kiss_tnc::k_bit_rate_hz;
    const double boxcar = std::sin(x * k_bits_per_sample) / (k_bits_per_sample * std::sin(x));
    return rc * std::pow(boxcar, k_cic_order + 1) / (kiss_tnc::k_input_divisor * k_full_scale);
}

// The tone at hz fitted over x by least squares (8 kHz samples); with `band`, the power of what is left between
// k_band_low_hz and k_band_high_hz too.
struct ToneFit {
    double amplitude;
    double noise_power;
};

ToneFit fit_tone(const std::vector<double>& x, double hz, bool band = true) {
    double ss = 0.0, cc = 0.0, sc = 0.0, xs = 0.0, xc = 0.0;
    for (size_t n = 0; n < x.size(); ++n) {
        const double s = std::sin(2.0 * k_pi * hz * n / k_audio_rate_hz);
        const double c = std::cos(2.0 * k_pi * hz * n / k_audio_rate_hz);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        xs += x[n] * s;
        xc += x[n] * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = (xs * cc - xc * sc) / det;
    const double b = (xc * ss - xs * sc) / det;
    ToneFit fit;
    fit.amplitude = std::sqrt(a * a + b * b);
    fit.noise_power = 0.0;
    if (!band) return fit;
    std::vector<double> rest(x.size());
    for (size_t n = 0; n < x.size(); ++n)
        rest[n] = x[n] - a * std::sin(2.0 * k_pi * hz * n / k_audio_rate_hz) -
                  b * std::cos(2.0 * k_pi * hz * n / k_audio_rate_hz);
    const size_t count = x.size();
    const double bin_hz = k_audio_rate_hz / count;
    for (size_t k = static_cast<size_t>(std::ceil(k_band_low_hz / bin_hz)); k * bin_hz <= k_band_high_hz; ++k) {
        std::complex<double> sum(0.0, 0.0);
        const std::complex<double> step = std::polar(1.0, -2.0 * k_pi * k / count);
        std::complex<double> phasor(1.0, 0.0);
        for (size_t n = 0; n < count; ++n) {
            sum += rest[n] * phasor;
            phasor *= step;
        }
        fit.noise_power += 2.0 * std::norm(sum) / (static_cast<double>(count) * count);
    }
    return fit;
}

// The modulator's output for `samples`, heard at the pin; `swap` sends the two slots of each frame the other way round.
std::vector<double> through_the_pin(const std::vector<int16_t>& samples, bool swap = false) {
    kiss_tnc::OneBitModulator modulator;
    PinListener listener;
    std::vector<uint32_t> words(samples.size() * kiss_tnc::k_words_per_sample);
    modulator.render(samples.data(), samples.size(), words.data());
    if (swap)
        for (size_t i = 0; i + 1 < words.size(); i += kiss_tnc::k_words_per_sample) std::swap(words[i], words[i + 1]);
    std::vector<double> heard(samples.size());
    listener.listen(words.data(), samples.size(), heard.data());
    return heard;
}

std::vector<int16_t> tone(double hz, double crest, size_t count) {
    std::vector<int16_t> samples(count);
    for (size_t n = 0; n < count; ++n)
        samples[n] = static_cast<int16_t>(std::lround(crest * std::sin(2.0 * k_pi * hz * n / k_audio_rate_hz)));
    return samples;
}

std::vector<double> tail(const std::vector<double>& x, size_t count) {
    return std::vector<double>(x.end() - static_cast<std::ptrdiff_t>(count), x.end());
}

// ---------------------------------------------------------------------------
// The stations
// ---------------------------------------------------------------------------

// The sketch's settings (all the core's defaults but the crest: full scale) at another speed; a fixed seed.
ModemConfig board_config(uint16_t centi, uint16_t tail_ms = unlimited::k_default_tail_ms) {
    ModemConfig config;
    config.signal.slot_us = unlimited::slot_us_for_centi_speed(centi);
    config.signal.amplitude = INT16_MAX;
    config.signal.tail_ms = tail_ms;
    config.receiver.slot_us = config.signal.slot_us;
    config.access.output_latency_ms = kiss_tnc::k_output_latency_ms;
    config.access.seed = k_seed;
    return config;
}

// unlimited_modem's core with its defaults at that speed.
ModemConfig pc_config(uint16_t centi) {
    ModemConfig config;
    config.signal.slot_us = unlimited::slot_us_for_centi_speed(centi);
    config.receiver.slot_us = config.signal.slot_us;
    config.access.seed = k_seed + 1;
    return config;
}

struct Key {
    uint32_t on_ms;
    uint32_t off_ms;  // 0 while keyed
};

// The ESP32: the sketch's loop() and output task around its Modem, with a DMA queue of k_dma_buffers chunks that
// held silence at the start (the sketch preloads it).
class Board {
public:
    explicit Board(const ModemConfig& config)
        : modem(config, &Board::on_host, &Board::on_ptt, this), keyed(false), dcd_steps(0), now_ms_(0) {
        decimator_.begin();
        line.port = &computer_;
        const std::vector<int16_t> silence(k_step_samples, 0);
        for (uint8_t i = 0; i < kiss_tnc::k_dma_buffers; ++i) {
            std::vector<uint32_t> words(k_step_words);
            modulator_.render(silence.data(), k_step_samples, words.data());
            dma_.push_back(words);
        }
    }

    // loop(): the ADC frame, the computer, the timers, the LED.
    void loop(const float* codes, uint32_t now_ms) {
        now_ms_ = now_ms;
        size_t samples = 0;
        for (size_t i = 0; i < k_step_codes; ++i)
            if (decimator_.push(codes[i], heard_[samples])) ++samples;
        modem.audio_input(heard_, samples);
        computer_.service(modem, line);
        modem.tick(now_ms);
        if (modem.dcd()) ++dcd_steps;
    }

    // The output task: the modem's next 10 ms into the DMA's queue; the chunk leaving the queue plays at the pin now.
    void output(uint32_t* pin_words) {
        int16_t played[k_step_samples];
        modem.audio_output(played, k_step_samples);
        std::vector<uint32_t> words(k_step_words);
        modulator_.render(played, k_step_samples, words.data());
        dma_.push_back(words);
        std::copy(dma_.front().begin(), dma_.front().end(), pin_words);
        dma_.pop_front();
    }

    const kiss_tnc::ComputerPort& computer() const { return computer_; }

    Modem modem;
    ComputerLine line;
    bool keyed;
    std::vector<Key> keys;
    size_t dcd_steps;

private:
    static void on_host(const uint8_t* data, size_t size, void* context) {
        Board& board = *static_cast<Board*>(context);
        board.line.to_computer.insert(board.line.to_computer.end(), data, data + size);
    }

    static void on_ptt(bool on, void* context) {
        Board& board = *static_cast<Board*>(context);
        board.keyed = on;
        if (on) {
            const Key key = {board.now_ms_, 0};
            board.keys.push_back(key);
        } else if (!board.keys.empty()) {
            board.keys.back().off_ms = board.now_ms_;
        }
    }

    kiss_tnc::Decimator decimator_;
    kiss_tnc::OneBitModulator modulator_;
    kiss_tnc::ComputerPort computer_;
    int16_t heard_[kiss_tnc::k_frame_samples];
    std::deque<std::vector<uint32_t> > dma_;
    uint32_t now_ms_;
};

// unlimited_modem's core on the other radio.
class PcStation {
public:
    explicit PcStation(const ModemConfig& config) : modem(config, &PcStation::on_host, &PcStation::on_ptt, this),
                                                   keyed(false), pending_from(0), keys(0) {}

    void computer() {
        if (pending_from < pending.size())
            pending_from += modem.host_input(&pending[pending_from], pending.size() - pending_from);
    }

    Modem modem;
    bool keyed;
    Bytes pending;  // what its computer wrote, not taken yet
    size_t pending_from;
    Bytes to_computer;
    size_t keys;

private:
    static void on_host(const uint8_t* data, size_t size, void* context) {
        PcStation& station = *static_cast<PcStation*>(context);
        station.to_computer.insert(station.to_computer.end(), data, data + size);
    }

    static void on_ptt(bool on, void* context) {
        PcStation& station = *static_cast<PcStation*>(context);
        station.keyed = on;
        if (on) ++station.keys;
    }
};

sim::ChannelConfig radio(double signal_level, double snr_db, double offset_hz, uint32_t seed) {
    sim::ChannelConfig config;
    config.mode = sim::Mode::usb;
    config.sample_rate = k_audio_rate_hz;
    config.signal_level = signal_level;
    config.snr_db = snr_db;
    config.freq_offset_hz = offset_hz;
    config.rx_low_hz = unlimited::k_ssb_passband_low_hz;
    config.rx_high_hz = unlimited::k_ssb_passband_high_hz;
    config.seed = seed;
    const double snr = std::pow(10.0, snr_db / k_power_db);
    const double tone_power = 0.5 * signal_level * signal_level;
    const double sigma = std::sqrt(tone_power / (snr * k_reference_bandwidth_hz) * k_receiver_bandwidth_hz);
    config.output_gain = std::min(1.0, k_headroom / (signal_level + k_noise_peak_sigmas * sigma));
    return config;
}

int16_t to_int16(double x) {
    const double value = std::round(x * k_full_scale);
    return static_cast<int16_t>(std::max(-k_full_scale, std::min(k_full_scale - 1.0, value)));
}

// The ESP32 TNC and a PC modem on two radios, stepped 10 ms at a time: the PC's computer, timers and audio; the
// board's output task and the air (only while PTT is keyed); the PC's receiver; the board's loop().
class Bench {
public:
    Bench(uint16_t centi, double snr_db, uint32_t seed, uint16_t board_tail_ms = unlimited::k_default_tail_ms)
        : board(new Board(board_config(centi, board_tail_ms))),
          pc(new PcStation(pc_config(centi))),
          now_ms(0),
          loud_while_unkeyed(0),
          adc_min(k_adc_max),
          adc_max(0.0),
          to_board_(radio(pc->modem.signal().amplitude / k_full_scale, snr_db, k_offset_hz, seed)),
          to_pc_(radio(pin_gain(board->modem.signal().tone_hz) * board->modem.signal().amplitude, snr_db,
                       -k_offset_hz, seed + 1)),
          up_(k_audio_rate_hz, kiss_tnc::k_adc_rate_hz),
          generator_(seed),
          adc_noise_(0.0, k_adc_noise),
          loud_level_(k_loud_share * pin_gain(board->modem.signal().tone_hz) * board->modem.signal().amplitude) {
        adc_audio_.assign(2 * k_step_codes, 0.0f);  // the resampler holds its first outputs back
    }

    void run_until(uint32_t max_ms, size_t board_frames, size_t pc_frames) {
        while (now_ms < max_ms && (frames_at_board().size() < board_frames || frames_at_pc().size() < pc_frames ||
                                   board->modem.channel_state() != unlimited::ChannelState::idle ||
                                   pc->modem.channel_state() != unlimited::ChannelState::idle))
            step();
    }

    std::vector<Bytes> frames_at_board() const { return kiss_frames(board->line.to_computer); }
    std::vector<Bytes> frames_at_pc() const { return kiss_frames(pc->to_computer); }

    std::unique_ptr<Board> board;
    std::unique_ptr<PcStation> pc;
    uint32_t now_ms;
    std::vector<uint32_t> loud_ms;  // when the pin carried a beep (each 8 kHz sample, in ms)
    size_t loud_while_unkeyed;
    double adc_min;
    double adc_max;

private:
    void step() {
        // The PC station: its computer, its timers, its audio; the board's radio hears it (and the band's noise).
        pc->computer();
        pc->modem.tick(now_ms);
        int16_t pc_out[k_step_samples];
        pc->modem.audio_output(pc_out, k_step_samples);
        float sent[k_step_samples];
        float heard[k_step_samples];
        for (size_t i = 0; i < k_step_samples; ++i) sent[i] = pc->keyed ? static_cast<float>(pc_out[i] / k_full_scale) : 0.0f;
        to_board_.process(sent, heard, k_step_samples);
        std::vector<float> up;
        up_.process(heard, k_step_samples, up);
        adc_audio_.insert(adc_audio_.end(), up.begin(), up.end());

        // The board's output task, and its radio on the air while PTT is keyed; the PC's receiver.
        const bool on_air = board->keyed;
        uint32_t pin_words[k_step_words];
        board->output(pin_words);
        double pin[k_step_samples];
        listener_.listen(pin_words, k_step_samples, pin);
        float to_pc[k_step_samples];
        float pc_heard[k_step_samples];
        for (size_t i = 0; i < k_step_samples; ++i) {
            if (std::fabs(pin[i]) > loud_level_) {
                loud_ms.push_back(now_ms + static_cast<uint32_t>(i * k_ms_per_s / k_audio_rate_hz));
                if (!on_air) ++loud_while_unkeyed;
            }
            to_pc[i] = on_air ? static_cast<float>(pin[i]) : 0.0f;
        }
        to_pc_.process(to_pc, pc_heard, k_step_samples);
        int16_t pc_in[k_step_samples];
        for (size_t i = 0; i < k_step_samples; ++i) pc_in[i] = to_int16(pc_heard[i]);
        pc->modem.audio_input(pc_in, k_step_samples);

        // The board's loop(): one ADC frame of what its radio heard.
        float codes[k_step_codes];
        for (size_t i = 0; i < k_step_codes; ++i) {
            double audio = 0.0;
            if (!adc_audio_.empty()) {
                audio = adc_audio_.front();
                adc_audio_.pop_front();
            }
            const double code = std::round(k_adc_mid + audio * k_adc_swing + adc_noise_(generator_));
            codes[i] = static_cast<float>(std::max(0.0, std::min(k_adc_max, code)));
            adc_min = std::min(adc_min, static_cast<double>(codes[i]));
            adc_max = std::max(adc_max, static_cast<double>(codes[i]));
        }
        board->loop(codes, now_ms);
        now_ms += k_step_ms;
    }

    sim::Channel to_board_;
    sim::Channel to_pc_;
    unlimited::pc::Resampler up_;
    std::deque<float> adc_audio_;
    PinListener listener_;
    std::mt19937 generator_;
    std::normal_distribution<double> adc_noise_;
    double loud_level_;
};

// The HF default and the fastest speed (the widest band through the decimator and the RC poles), in centi-bytes/s.
const uint16_t k_speeds[] = {600, 2500};

unsigned whole_bytes_per_second(uint16_t centi) {
    return static_cast<unsigned>(centi / unlimited::k_centi_per_unit);
}

unsigned hundredths(uint16_t centi) {
    return static_cast<unsigned>(centi % unlimited::k_centi_per_unit);
}

}  // namespace

// rx_esp32's decimator, as the TNC runs it: flat through the band, and what would fold onto it from above 4 kHz (the
// ADC's own noise included) at least 50 dB down.
TEST(kiss_tnc_decimator_is_flat_to_3_khz_and_stops_aliases) {
    const double k_amplitude_codes = 500.0;
    // In the band, and above 4 kHz where they would land on 3000, 1000, 1000 and 3000 Hz.
    const double frequencies[] = {300.0, 1500.0, 2700.0, 3000.0, 5000.0, 7000.0, 9000.0, 11000.0};
    const double k_flat_db = 0.2;
    const double k_stop_db = -50.0;
    const size_t k_inputs = 2 * kiss_tnc::k_adc_rate_hz;
    for (size_t f = 0; f < test::count_of(frequencies); ++f) {
        const double hz = frequencies[f];
        const bool in_band = hz < k_audio_rate_hz / 2;
        double lands_hz = std::fmod(hz, k_audio_rate_hz);
        if (lands_hz > k_audio_rate_hz / 2) lands_hz = k_audio_rate_hz - lands_hz;
        kiss_tnc::Decimator decimator;
        decimator.begin();
        std::vector<double> out;
        for (size_t n = 0; n < k_inputs; ++n) {
            const double code = k_adc_mid + k_amplitude_codes * std::sin(2.0 * k_pi * hz * n / kiss_tnc::k_adc_rate_hz);
            int16_t sample = 0;
            if (decimator.push(static_cast<float>(code), sample)) out.push_back(sample);
        }
        const ToneFit fit = fit_tone(tail(out, k_measure_samples), lands_hz, false);
        const double gain_db =
            k_amplitude_db * std::log10(fit.amplitude / (k_amplitude_codes * kiss_tnc::k_adc_to_int16));
        NOTE("%.0f Hz (heard at %.0f Hz): %.2f dB", hz, lands_hz, gain_db);
        if (in_band) {
            CHECK(std::fabs(gain_db) <= k_flat_db);
        } else {
            CHECK(gain_db <= k_stop_db);
        }
    }
    // A step of the bias is gone after half a second (the DC blocker's corner is 4 Hz).
    kiss_tnc::Decimator decimator;
    decimator.begin();
    const double k_step_codes_dc = 300.0;
    const size_t k_half_second = kiss_tnc::k_adc_rate_hz / 2;
    int16_t sample = 0;
    int16_t last = 0;
    for (size_t n = 0; n < 2 * k_half_second; ++n)
        if (decimator.push(static_cast<float>(k_adc_mid + (n >= k_half_second ? k_step_codes_dc : 0.0)), sample))
            last = sample;
    const int16_t k_rest = 2;
    CHECK(std::abs(last) <= k_rest);
}

// The 1-bit output: the audio comes back from the pin's stream through the wiring's RC poles with the gain the
// comments give (a full-scale crest moves the pin's average by a quarter of the supply before the filters), the
// quantization noise at least 70 dB under a full-scale crest in 300..2700 Hz, the loop stable at full scale, silence
// exactly half ones.
TEST(kiss_tnc_one_bit_output_carries_the_audio) {
    const double k_min_snr_db = 70.0;
    const double k_gain_tolerance = 0.01;
    const size_t k_count = k_settle_samples + k_measure_samples;
    const double full_crest = INT16_MAX;
    const double full_level = pin_gain(k_test_tone_hz) * full_crest;
    const double full_power = 0.5 * full_level * full_level;

    const ToneFit full = fit_tone(tail(through_the_pin(tone(k_test_tone_hz, full_crest, k_count)), k_measure_samples),
                                  k_test_tone_hz);
    const double full_snr_db = k_power_db * std::log10(full_power / full.noise_power);
    NOTE("full scale: level %.4f of half the supply (expected %.4f), SNR %.1f dB in 300..2700 Hz", full.amplitude,
         full_level, full_snr_db);
    CHECK(std::fabs(full.amplitude / full_level - 1.0) <= k_gain_tolerance);
    CHECK(full_snr_db >= k_min_snr_db);

    const double k_quiet_db = -20.0;
    const double quiet_crest = full_crest * std::pow(10.0, k_quiet_db / k_amplitude_db);
    const ToneFit quiet = fit_tone(tail(through_the_pin(tone(k_test_tone_hz, quiet_crest, k_count)), k_measure_samples),
                                   k_test_tone_hz);
    const double quiet_floor_db = k_power_db * std::log10(full_power / quiet.noise_power);
    NOTE("-20 dBFS: noise %.1f dB under a full-scale crest", quiet_floor_db);
    CHECK(quiet_floor_db >= k_min_snr_db);

    // Silence: half ones in every sample's 64 bits (the pin's average at mid-supply); the most negative int16 held:
    // a quarter; then a full-scale tone again, as clean as before (the loop did not run away).
    kiss_tnc::OneBitModulator modulator;
    std::vector<int16_t> samples(k_measure_samples, 0);
    const size_t k_held = 1000;
    std::fill(samples.begin() + static_cast<std::ptrdiff_t>(k_held), samples.begin() + static_cast<std::ptrdiff_t>(2 * k_held),
              INT16_MIN);
    const std::vector<int16_t> after = tone(k_test_tone_hz, full_crest, k_count);
    samples.insert(samples.end(), after.begin(), after.end());
    std::vector<uint32_t> words(samples.size() * kiss_tnc::k_words_per_sample);
    modulator.render(samples.data(), samples.size(), words.data());
    const size_t k_start = 2;  // the loop's first samples leave its resting state
    size_t off_half = 0;
    uint32_t held_ones = 0;
    for (size_t i = k_start; i < k_held; ++i) {
        uint32_t ones = 0;
        for (uint8_t w = 0; w < kiss_tnc::k_words_per_sample; ++w) {
            uint32_t word = words[i * kiss_tnc::k_words_per_sample + w];
            for (; word != 0; word &= word - 1) ++ones;
        }
        if (ones != k_bits_per_sample / 2) ++off_half;
    }
    for (size_t i = k_held; i < 2 * k_held; ++i)
        for (uint8_t w = 0; w < kiss_tnc::k_words_per_sample; ++w)
            for (uint32_t word = words[i * kiss_tnc::k_words_per_sample + w]; word != 0; word &= word - 1) ++held_ones;
    const double held_share = static_cast<double>(held_ones) / (k_held * k_bits_per_sample);
    const double k_quarter = 0.25;
    const double k_share_tolerance = 1.0 / k_bits_per_sample;
    NOTE("silence: %zu of %zu samples off half ones; INT16_MIN held: %.4f ones", off_half, k_held - k_start, held_share);
    CHECK_EQ(off_half, static_cast<size_t>(0));
    CHECK(std::fabs(held_share - k_quarter) <= k_share_tolerance);
    PinListener listener;
    std::vector<double> heard(samples.size());
    listener.listen(words.data(), samples.size(), heard.data());
    const ToneFit recovered = fit_tone(tail(heard, k_measure_samples), k_test_tone_hz);
    CHECK(k_power_db * std::log10(full_power / recovered.noise_power) >= k_min_snr_db);

    // For the record (spec 12.7, open issue): the two slots of a frame sent the other way round.
    const ToneFit swapped = fit_tone(
        tail(through_the_pin(tone(k_test_tone_hz, full_crest, k_count), true), k_measure_samples), k_test_tone_hz);
    NOTE("slots swapped: SNR %.1f dB", k_power_db * std::log10(full_power / swapped.noise_power));
}

// The computer's port: when the modem cannot take more (its 64 frame slots are full), what was read waits in the
// port and the serial port is not read again until it is taken; everything arrives, in order, nothing twice.
TEST(kiss_tnc_computer_port_holds_what_the_modem_cannot_take) {
    const size_t k_frames = 200;
    const uint16_t k_fast_centi = 2500;
    ModemConfig config = board_config(k_fast_centi);
    config.access.full_duplex = true;  // no channel check: the frames go as fast as the air takes them
    Board board(config);
    for (size_t i = 0; i < k_frames; ++i) {
        const Bytes frame = kiss_frame(Bytes(1, static_cast<uint8_t>(i)));
        board.line.from_computer.insert(board.line.from_computer.end(), frame.begin(), frame.end());
    }
    const std::vector<float> quiet(k_step_codes, static_cast<float>(k_adc_mid));
    uint32_t now = 0;
    board.loop(quiet.data(), now);
    const ModemCounters first = board.modem.counters();
    NOTE("after the first pass: %u frames queued, %zu bytes read, %zu waiting, %u refusals",
         static_cast<unsigned>(first.queued_frames), board.line.read_at, board.computer().waiting(),
         static_cast<unsigned>(first.host_refusals));
    CHECK_EQ(first.queued_frames, static_cast<uint32_t>(unlimited::ModemTransmitter::k_frame_slots));
    CHECK(board.computer().waiting() > 0);
    CHECK(board.line.read_at < board.line.from_computer.size());
    const size_t read_before = board.line.read_at;
    board.loop(quiet.data(), now);
    CHECK_EQ(board.line.read_at, read_before);  // still full: the port was not read

    uint32_t words[k_step_words];
    while (now < k_max_run_ms && board.modem.counters().transmissions < k_frames) {
        board.output(words);
        now += k_step_ms;
        board.loop(quiet.data(), now);
    }
    const ModemCounters last = board.modem.counters();
    CHECK_EQ(last.transmissions, static_cast<uint32_t>(k_frames));
    CHECK_EQ(last.bytes_sent, static_cast<uint32_t>(k_frames));
    CHECK_EQ(last.kiss.frames, static_cast<uint32_t>(k_frames));
    CHECK_EQ(last.kiss.bad_escapes, 0u);
    CHECK_EQ(board.line.read_at, board.line.from_computer.size());
    CHECK_EQ(board.computer().waiting(), static_cast<size_t>(0));
    CHECK_EQ(board.line.early_reads, static_cast<size_t>(0));
}

// Radio -> board -> computer: frames the PC modem sends reach the board's computer as KISS, byte for byte, in order,
// through the ADC's codes and the decimator; a reception shorter than the minimum frame (14 bytes) does not (V23).
TEST(kiss_tnc_frames_from_the_air_reach_the_computer) {
    const size_t k_lengths[] = {14, 15, 32, 64};
    for (size_t s = 0; s < test::count_of(k_speeds); ++s) {
        Bench bench(k_speeds[s], k_strong_snr_db, k_seed + static_cast<uint32_t>(s));
        std::vector<Bytes> passed;
        for (size_t f = 0; f < test::count_of(k_lengths); ++f) {
            const Bytes frame = frame_bytes(k_lengths[f], k_seed + static_cast<uint32_t>(f));
            const Bytes kiss = kiss_frame(frame);
            bench.pc->pending.insert(bench.pc->pending.end(), kiss.begin(), kiss.end());
            if (frame.size() >= unlimited::k_default_min_frame_bytes) passed.push_back(frame);
        }
        bench.run_until(k_max_run_ms, passed.size(), 0);
        const std::vector<Bytes> got = bench.frames_at_board();
        const ModemCounters counters = bench.board->modem.counters();
        NOTE("%u.%02u bytes/s: %zu frames sent, %zu at the board's computer after %.1f s, %u short dropped; DCD on "
             "%.1f s; ADC codes %.0f..%.0f",
             whole_bytes_per_second(k_speeds[s]), hundredths(k_speeds[s]), test::count_of(k_lengths), got.size(),
             bench.now_ms / k_ms_per_s, static_cast<unsigned>(counters.short_frames),
             bench.board->dcd_steps * k_step_ms / k_ms_per_s,
             bench.adc_min, bench.adc_max);
        CHECK(got == passed);
        CHECK_EQ(counters.short_frames, 1u);
        CHECK_EQ(bench.pc->keys, test::count_of(k_lengths));
        CHECK(bench.board->keys.empty());  // the board only listened
        CHECK(bench.board->dcd_steps > 0);
        CHECK(!bench.board->modem.dcd());
        CHECK(bench.adc_min > 0.0 && bench.adc_max < k_adc_max);  // the level model never clipped
    }
}

// Computer -> board -> radio: frames the board's computer writes go on the air through the 1-bit output and reach the
// PC modem's computer byte for byte; PTT is keyed once per frame, the pin carries no beep while it is released, the
// TX delay comes first and the last beep leaves before the release. With the shortest tail (2 slots, 8 ms at
// 25 bytes/s) only k_output_latency_ms keeps the last STOP from being cut: it must cover the DMA's queue.
TEST(kiss_tnc_frames_from_the_computer_go_on_the_air) {
    const size_t k_lengths[] = {15, 32, 64};
    struct Case {
        uint16_t centi;
        uint16_t tail_ms;
    };
    const uint16_t k_shortest_tail_ms = 0;  // the encoder keeps 2 silent slots
    const Case cases[] = {{k_speeds[0], unlimited::k_default_tail_ms},
                          {k_speeds[1], unlimited::k_default_tail_ms},
                          {k_speeds[1], k_shortest_tail_ms}};
    for (size_t c = 0; c < test::count_of(cases); ++c) {
        Bench bench(cases[c].centi, k_strong_snr_db, k_seed + static_cast<uint32_t>(c), cases[c].tail_ms);
        std::vector<Bytes> sent;
        for (size_t f = 0; f < test::count_of(k_lengths); ++f) {
            sent.push_back(frame_bytes(k_lengths[f], k_seed + static_cast<uint32_t>(f)));
            const Bytes kiss = kiss_frame(sent.back());
            bench.board->line.from_computer.insert(bench.board->line.from_computer.end(), kiss.begin(), kiss.end());
        }
        bench.run_until(k_max_run_ms, 0, sent.size());
        const std::vector<Bytes> got = bench.frames_at_pc();
        const std::vector<Key>& keys = bench.board->keys;
        NOTE("%u.%02u bytes/s, tail %u ms: %zu frames at the PC's computer after %.1f s, %zu keys, %zu beep samples "
             "while released",
             whole_bytes_per_second(cases[c].centi), hundredths(cases[c].centi), static_cast<unsigned>(cases[c].tail_ms),
             got.size(), bench.now_ms / k_ms_per_s, keys.size(), bench.loud_while_unkeyed);
        CHECK(got == sent);
        CHECK_EQ(bench.loud_while_unkeyed, static_cast<size_t>(0));
        REQUIRE(keys.size() == sent.size());
        const uint32_t txdelay_ms = bench.board->modem.signal().lead_in_ms;
        for (size_t k = 0; k < keys.size(); ++k) {
            CHECK(keys[k].off_ms > keys[k].on_ms);
            uint32_t first = 0;
            uint32_t last = 0;
            bool any = false;
            for (size_t i = 0; i < bench.loud_ms.size(); ++i) {
                const uint32_t t = bench.loud_ms[i];
                if (t < keys[k].on_ms || t > keys[k].off_ms) continue;
                if (!any) first = t;
                last = t;
                any = true;
            }
            CHECK(any);
            NOTE("  key %zu: on %u ms, first beep +%u ms, last beep %u ms before the release", k,
                 static_cast<unsigned>(keys[k].on_ms), static_cast<unsigned>(first - keys[k].on_ms),
                 static_cast<unsigned>(keys[k].off_ms - last));
            CHECK(first >= keys[k].on_ms + txdelay_ms);
            CHECK(last < keys[k].off_ms);
        }
        CHECK_EQ(bench.board->line.read_at, bench.board->line.from_computer.size());
        CHECK(bench.frames_at_board().empty());  // half duplex: the board did not hear itself
    }
}
