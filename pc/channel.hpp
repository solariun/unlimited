#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace unlimited {
namespace sim {

// clean: output = input * output_gain, nothing else (control).
// usb/lsb: SSB transmitter + HF path + SSB receiver, simulated on the analytic signal at the audio rate.
// am/fm: full complex-baseband transmitter, path and receiver at an internal rate >= 48 kHz.
enum class Mode { clean, usb, lsb, am, fm };

// A key-down tone is a sine of amplitude signal_level at the channel input.
//
// SNR (WSJT / digital-mode convention):
//     snr = P_ref / (N0 * 2500 Hz)
// P_ref is the RF power of the key-down tone: the tone's own power for usb/lsb,
// the unmodulated carrier power for am/fm. N0 is the noise power spectral
// density, so snr_db is the SNR in a 2500 Hz bandwidth whatever the receiver
// filters are. Fading has unit average power gain, so snr_db is the average SNR.
//
// Impulses (QRN) are Poisson arrivals with random RF phase, each a rectangular
// spike 1/48000 s long whose peak is impulse_level_db above the key-down RF
// envelope peak (usb/lsb: the tone; am: the carrier at the modulation crest,
// carrier * (1 + am_modulation_index); fm: the carrier). At other simulation
// rates the spike keeps the same area (amplitude x duration), i.e. the same
// spectral density inside the receiver band, so its filtered effect does not
// depend on the rate.
//
// FM transmitter audio chain, as in a real rig: pre-emphasis (unity gain at
// 1 kHz), deviation limiter (clipper at fm_max_deviation_hz), splatter filter
// (flat to fm_audio_high_hz), modulator. fm_deviation_hz is the peak deviation
// of a 1 kHz key-down tone; higher tones deviate more (750 us: +5.8 dB at
// 2 kHz) until the limiter clips them. The defaults are the TIA-603 standard
// test modulation: 1 kHz at 60 % of a 5 kHz rated deviation. After clipping,
// the splatter filter lets the fundamental of an overdriven tone exceed the
// limit by up to 4 / pi, as in real transmitters.
struct ChannelConfig {
    Mode mode = Mode::usb;
    double sample_rate = 8000;          // audio rate in and out (Hz)
    double signal_level = 0.5;          // key-down tone amplitude at the input; SNR reference
    bool noise = true;
    double snr_db = 20;
    double freq_offset_hz = 0;          // usb/lsb: receiver mistuning (audio shift); am/fm: carrier offset
    double lsb_pivot_hz = 3000;         // lsb: audio f -> lsb_pivot_hz - f + freq_offset_hz
    double rx_low_hz = 300;             // receiver audio passband low edge (usb/lsb/am/fm, -6 dB point)
    double rx_high_hz = 2700;           // receiver audio passband high edge (usb/lsb/am, -6 dB point)

    // Watterson 2-path model (ITU-R F.1487 / CCIR 520): each path an
    // independent complex Gaussian (Rayleigh) tap with a Gaussian Doppler
    // spectrum whose 2-sigma width is doppler_spread_hz. Total average power
    // gain is 1. path_delay_ms = 0 gives single-path flat Rayleigh fading;
    // doppler_spread_hz = 0 freezes the taps at one random draw.
    bool fading = false;
    double doppler_spread_hz = 0.5;
    double path_delay_ms = 1.0;         // rounded to whole samples of the simulation rate
    double path2_gain_db = 0;           // second (delayed) path power relative to the first

    double impulse_rate_hz = 0;         // QRN: mean impulses per second (Poisson)
    double impulse_level_db = 20;       // impulse peak over the key-down RF envelope peak

    // Receiver AGC (usb/lsb/am): fast attack, slow exponential decay, gain
    // capped at +40 dB and starting at unity. agc_target is the output peak of
    // a key-down tone. usb/lsb: the detector follows the envelope of the
    // receiver output, so noise between bursts gets amplified as the gain
    // recovers. am: it follows the received carrier level (smoothed below the
    // audio band), so it tracks fading but not the modulation, like an AM rig.
    bool agc = false;
    double agc_attack_ms = 2;
    double agc_decay_ms = 500;
    double agc_target = 0.5;

    double am_modulation_index = 0.8;   // modulation index of a key-down tone (envelope clipped at 0)
    double am_if_bandwidth_hz = 6000;

    double fm_deviation_hz = 3000;      // peak deviation of a 1 kHz key-down tone
    double fm_max_deviation_hz = 5000;  // transmitter deviation limit; 0 removes limiter and splatter filter
    double fm_if_bandwidth_hz = 12500;
    double fm_emphasis_us = 750;        // 6 dB/octave time constant; 0 disables pre- and de-emphasis
    bool fm_tx_preemphasis = true;
    bool fm_rx_deemphasis = true;       // false models a flat receiver data port
    double fm_audio_high_hz = 3000;     // fm audio passband is rx_low_hz..fm_audio_high_hz

    // Interferers from other stations, specified where they land: frequency in the receiver audio, key-down
    // level relative to the key-down tone at the receiver output. Neither fades, and the AGC (usb/lsb/am) acts
    // on them as on the signal. usb/lsb: added to the RF before the sideband filter; am/fm: added to the
    // detected audio before the audio filter (a heterodyne). A frequency of 0 disables the interferer.
    double carrier_hz = 0;              // steady carrier
    double carrier_db = -200;
    double cw_hz = 0;                   // keyed CW: random Morse, PARIS timing, 5 ms raised-cosine edges
    double cw_db = -200;
    double cw_wpm = 20;

    // QSB: slow fading of the wanted signal, gain dB = -qsb_depth_db * (0.5 - 0.5 cos(2 pi qsb_rate_hz t)).
    // It starts at the crest, where snr_db applies; noise and interferers do not fade.
    double qsb_depth_db = 0;
    double qsb_rate_hz = 0.2;

    // Transmitter sample-clock error, |clock_ppm| <= 1e5: the input is played at sample_rate * (1 + clock_ppm /
    // 1e6) and the channel runs at sample_rate, so tones move up and durations shrink for clock_ppm > 0. The
    // input is resampled (pc::Resampler) before the channel, so the output length differs from the input
    // length and trails it by the resampler's half-length: use the vector process() (the pointer process()
    // throws std::logic_error).
    double clock_ppm = 0;

    double output_gain = 1.0;
    // Output repeats per seed on every system: the draws are portable_random.hpp's, not the standard library's.
    std::uint32_t seed = 1;
};

double fm_cnr_db(const ChannelConfig& config);

// Fading presets (ITU-R F.1487 / CCIR 520, two equal paths): good 0.5 ms / 0.1 Hz, moderate 1 ms / 0.5 Hz,
// poor 2 ms / 1 Hz, flutter 0.5 ms / 10 Hz; flat is single-path Rayleigh at 1 Hz; none turns fading off.
// Doppler is the 2-sigma spread, as doppler_spread_hz.
enum class FadingPreset { none, flat, ccir_good, ccir_moderate, ccir_poor, flutter };

void apply_preset(ChannelConfig& config, FadingPreset preset);

class Channel {
public:
    explicit Channel(const ChannelConfig& config);  // throws std::invalid_argument for an unusable config
    ~Channel();
    Channel(Channel&& other);
    Channel& operator=(Channel&& other);

    void process(const float* in, float* out, std::size_t count);  // one output per input; clock_ppm must be 0
    std::vector<float> process(const std::vector<float>& in);      // same length unless clock_ppm != 0
    void reset();
    const ChannelConfig& config() const;

private:
    class Impl;
    ChannelConfig config_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sim
}  // namespace unlimited
