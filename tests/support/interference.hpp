#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

// Interference for the stray-byte scenes (spec 4 F, 8; V20): a drifting steady carrier, keyed CW (up to two
// stations) and speech-shaped bursts, one sample at a time at 8000 Hz. Shared by the long suite's F scenes and the unit
// tests, so both hear exactly the same audio for the same seed.
namespace unlimited {
namespace interference {

// The key-down reference amplitude the levels are quoted against (full scale 1).
const double k_reference = 0.05;

class Uniform {
public:
    explicit Uniform(std::uint32_t seed);
    double operator()(double low, double high);
    int integer(int low, int high);

private:
    std::mt19937 generator_;
};

class Interferer {
public:
    virtual ~Interferer() {}
    virtual double next() = 0;
};

class NoInterferer : public Interferer {
public:
    double next() override;
};

// Steady carrier +20 dB over the noise in 2500 Hz with slow drift, a new frequency every 5 minutes, 1 s off between
// them.
class DriftingCarrier : public Interferer {
public:
    DriftingCarrier(std::uint32_t seed, double low_hz, double high_hz);
    double next() override;

private:
    void start_segment();

    Uniform uniform_;
    double low_;
    double high_;
    double amplitude_;
    double base_ = 0.0;
    double drift_ = 0.0;
    double phase_ = 0.0;
    std::size_t sample_ = 0;
};

// Keyed CW stations one after the other: random text, PARIS timing (12-30 WPM), 5 ms raised-cosine edges, -6..+30 dB.
class CwTrack : public Interferer {
public:
    CwTrack(std::uint32_t seed, double low_hz, double high_hz, double gap_min_s, double gap_max_s);
    double next() override;

private:
    void start_station();

    Uniform uniform_;
    double low_;
    double high_;
    double gap_min_;
    double gap_max_;
    std::vector<std::uint8_t> keying_;
    std::size_t unit_samples_ = 1;
    std::size_t sample_ = 0;
    std::size_t gap_left_ = 0;
    int ramp_ = 0;
    int edge_samples_;
    double tone_ = 0.0;
    double amplitude_ = 0.0;
    double phase_ = 0.0;
};

// Two CW tracks: the second, sparser station overlaps the first about a third of the time.
class TwoCwTracks : public Interferer {
public:
    TwoCwTracks(std::uint32_t seed, double low_hz, double high_hz);
    double next() override;

private:
    CwTrack first_;
    CwTrack second_;
};

// Voiced syllables (harmonics of a gliding f0 of 90-250 Hz shaped by three formants) at about 4 per second, optional
// fricatives, phrases separated by pauses, +10..+25 dB.
class Speech : public Interferer {
public:
    explicit Speech(std::uint32_t seed);
    double next() override;

    static const int k_max_harmonics = 40;

private:
    enum class Part { pause, fricative, voiced, gap };

    void set_length(double seconds);
    void start_pause();
    void start_phrase();
    void start_syllable();
    void advance();

    Uniform uniform_;
    Part part_ = Part::pause;
    std::size_t length_ = 1;
    std::size_t left_ = 1;
    int syllables_left_ = 0;
    int syllables_ = 1;
    double phrase_f0_ = 0.0;
    double f0_start_ = 0.0;
    double f0_end_ = 0.0;
    double level_ = 0.0;
    double phase_ = 0.0;
    double previous_noise_ = 0.0;
    int harmonics_ = 0;
    double gains_[k_max_harmonics] = {};
};

}  // namespace interference
}  // namespace unlimited
