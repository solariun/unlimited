#include "test_harness.hpp"
#include "unlimited/encoder.hpp"
#include "unlimited/packet.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using unlimited::Band;
using unlimited::Encoder;
using unlimited::EncoderConfig;
using unlimited::EncoderSegment;
using unlimited::EncoderStatus;
using unlimited::Passband;
using unlimited::PassbandFit;
using unlimited::Preset;
using unlimited::SlotKind;

namespace {

using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using test::count_of;

typedef std::complex<double> Complex;

const double two_pi = 6.28318530717958647692;
const uint32_t half_band_numerator = 2200000;  // k_band_99_milli * 500
const uint32_t width_26_numerator = 7000000;
const uint32_t width_40_numerator = 9900000;
const uint32_t base_rate = 8000;

uint32_t ceil_div(uint32_t a, uint32_t b) {
    return (a + b - 1) / b;
}

Passband passband(uint16_t low, uint16_t high) {
    Passband p;
    p.low_hz = low;
    p.high_hz = high;
    return p;
}

// Samples of a whole transmission and the status before each sample.
struct Rendered {
    std::vector<double> samples;
    std::vector<EncoderStatus> status;
};

Rendered render(const EncoderConfig& config, const std::vector<uint8_t>& data) {
    Encoder encoder(config);
    size_t fed = encoder.write(data.data(), data.size());
    Rendered out;
    if (!encoder.start()) return out;
    while (encoder.busy()) {
        if (fed < data.size()) fed += encoder.write(&data[fed], data.size() - fed);
        out.status.push_back(encoder.status());
        out.samples.push_back(encoder.next_sample());
    }
    return out;
}

// The packages of a transmission as bit strings, from the encoder's slot kinds.
std::vector<std::string> packages_of(const EncoderConfig& config, const std::vector<uint8_t>& data) {
    const Rendered rendered = render(config, data);
    std::vector<std::string> packages;
    uint32_t last_slot = 0xFFFFFFFFu;
    for (size_t n = 0; n < rendered.status.size(); ++n) {
        const EncoderStatus& s = rendered.status[n];
        if (s.slot_index == last_slot || s.segment != EncoderSegment::package) continue;
        last_slot = s.slot_index;
        if (s.slot == 1) packages.push_back(std::string());
        if (s.kind == SlotKind::one) packages.back() += '1';
        if (s.kind == SlotKind::zero) packages.back() += '0';
    }
    return packages;
}

// Energy spectrum of samples [begin, end) at tone + d for d = -span .. span (Hz), from the upper side of the real
// signal's spectrum mirrored (the envelope is real, so its spectrum is symmetric): the image at -tone stays 2 tone away.
std::vector<double> baseband_spectrum(const EncoderConfig& config, const Rendered& r, size_t begin, size_t end,
                                      double span_hz, double step_hz, std::vector<double>& frequencies) {
    std::vector<double> upper;
    for (double d = 0.0; d <= span_hz + 1e-9; d += step_hz) {
        Complex sum(0.0, 0.0);
        const Complex rotation = std::polar(1.0, -two_pi * (config.tone_hz + d) / config.sample_rate_hz);
        Complex phasor(1.0, 0.0);
        for (size_t n = begin; n < end; ++n) {
            sum += r.samples[n] * phasor;
            phasor *= rotation;
        }
        upper.push_back(std::norm(sum));
    }
    std::vector<double> power;
    frequencies.clear();
    for (size_t i = upper.size(); i-- > 1;) {
        power.push_back(upper[i]);
        frequencies.push_back(-static_cast<double>(i) * step_hz);
    }
    for (size_t i = 0; i < upper.size(); ++i) {
        power.push_back(upper[i]);
        frequencies.push_back(static_cast<double>(i) * step_hz);
    }
    return power;
}

struct Widths {
    double w99;
    double w26;
    double w40;
};

// 99 % energy width (symmetric about 0), -26 dB and -40 dB widths (the outermost frequencies at that level).
Widths widths_of(const std::vector<double>& power, const std::vector<double>& frequencies) {
    double total = 0.0;
    double peak = 0.0;
    for (size_t i = 0; i < power.size(); ++i) {
        total += power[i];
        peak = std::max(peak, power[i]);
    }
    const size_t middle = power.size() / 2;
    Widths w = {0.0, 0.0, 0.0};
    double inside = power[middle];
    for (size_t k = 1; middle + k < power.size() && k <= middle; ++k) {
        inside += power[middle + k] + power[middle - k];
        if (inside >= 0.99 * total) {
            w.w99 = 2.0 * frequencies[middle + k];
            break;
        }
    }
    for (size_t i = 0; i < power.size(); ++i) {
        const double level = power[i] / peak;
        if (level >= std::pow(10.0, -2.6)) w.w26 = std::max(w.w26, 2.0 * std::fabs(frequencies[i]));
        if (level >= std::pow(10.0, -4.0)) w.w40 = std::max(w.w40, 2.0 * std::fabs(frequencies[i]));
    }
    return w;
}

// In-place radix-2 FFT (size a power of two).
void fft(std::vector<Complex>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t length = 2; length <= n; length <<= 1) {
        const Complex w = std::polar(1.0, -two_pi / static_cast<double>(length));
        for (size_t i = 0; i < n; i += length) {
            Complex wn(1.0, 0.0);
            for (size_t k = 0; k < length / 2; ++k) {
                const Complex u = a[i + k];
                const Complex v = a[i + k + length / 2] * wn;
                a[i + k] = u + v;
                a[i + k + length / 2] = u - v;
                wn *= w;
            }
        }
    }
}

// Share of a real signal's power (0..rate/2) inside [low_hz, high_hz].
double power_inside(const std::vector<double>& samples, double rate, double low_hz, double high_hz) {
    size_t size = 1;
    while (size < samples.size()) size <<= 1;
    std::vector<Complex> a(size, Complex(0.0, 0.0));
    for (size_t i = 0; i < samples.size(); ++i) a[i] = samples[i];
    fft(a);
    double total = 0.0;
    double inside = 0.0;
    for (size_t k = 0; k <= size / 2; ++k) {
        const double f = static_cast<double>(k) * rate / static_cast<double>(size);
        const double p = std::norm(a[k]);
        total += p;
        if (f >= low_hz && f <= high_hz) inside += p;
    }
    return inside / total;
}

}  // namespace

// U29: the integer formulas for every T from 4 to 128 ms in 1 ms steps and every tone in 300..2700 Hz.
TEST(protocol_band_functions_exact) {
    size_t checked = 0;
    for (uint32_t slot_us = unlimited::k_min_slot_us; slot_us <= unlimited::k_max_slot_us; slot_us += 1000) {
        const uint32_t half = ceil_div(half_band_numerator, slot_us);
        CHECK_EQ(unsigned(unlimited::width_26db_hz(slot_us)), unsigned(ceil_div(width_26_numerator, slot_us)));
        CHECK_EQ(unsigned(unlimited::width_40db_hz(slot_us)), unsigned(ceil_div(width_40_numerator, slot_us)));
        for (uint32_t tone = unlimited::k_min_tone_hz; tone <= unlimited::k_max_tone_hz; ++tone) {
            const Band band = unlimited::occupied_band(static_cast<uint16_t>(tone), slot_us);
            const uint32_t low = tone > half ? tone - half : 0;  // clipped at 0 Hz
            if (!CHECK_EQ(unsigned(band.low_hz), unsigned(low)) ||
                !CHECK_EQ(unsigned(band.high_hz), unsigned(tone + half)) ||
                !CHECK_EQ(unsigned(band.width_hz), unsigned(tone + half - low))) {
                NOTE("T %u us, tone %u", slot_us, tone);
                return;
            }
            ++checked;
        }
    }
    NOTE("%zu (tone, T) pairs", checked);
    // Clipping: a band below 0 Hz or above 65535 Hz, and a zero slot, stay in range.
    const Band low = unlimited::occupied_band(10, 4000);
    CHECK_EQ(low.low_hz, 0);
    CHECK_EQ(low.high_hz, 560);
    const Band wide = unlimited::occupied_band(1500, 0);
    CHECK_EQ(wide.low_hz, 0);
    CHECK_EQ(wide.high_hz, 65535);
}

// Spec 1.5: the widths per slot length.
TEST(protocol_width_table) {
    struct Row {
        uint32_t slot_ms;
        uint16_t band;
        uint16_t w26;
        uint16_t w40;
    };
    const Row rows[] = {{4, 1100, 1750, 2475}, {5, 880, 1400, 1980}, {8, 550, 875, 1238}, {12, 368, 584, 825},
                        {16, 276, 438, 619},   {20, 220, 350, 495},  {32, 138, 219, 310}, {64, 70, 110, 155},
                        {128, 36, 55, 78}};
    for (size_t i = 0; i < count_of(rows); ++i) {
        const uint32_t slot_us = rows[i].slot_ms * 1000;
        CHECK_EQ(unsigned(unlimited::occupied_band(1500, slot_us).width_hz), unsigned(rows[i].band));
        CHECK_EQ(unsigned(unlimited::width_26db_hz(slot_us)), unsigned(rows[i].w26));
        CHECK_EQ(unsigned(unlimited::width_40db_hz(slot_us)), unsigned(rows[i].w40));
    }
}

TEST(protocol_passband_valid_and_fit) {
    CHECK(unlimited::passband_valid(passband(300, 2700)));
    CHECK(unlimited::passband_valid(passband(0, 4000)));
    CHECK(!unlimited::passband_valid(passband(300, 300)));
    CHECK(!unlimited::passband_valid(passband(2700, 300)));
    CHECK(!unlimited::passband_valid(passband(300, 4001)));
    // The 16 ms example of spec 1.5.
    const Band hf = unlimited::occupied_band(1500, 16000);
    CHECK_EQ(hf.low_hz, 1362);
    CHECK_EQ(hf.high_hz, 1638);
    const PassbandFit fit = unlimited::passband_fit(hf, passband(300, 2700));
    CHECK(fit.fits);
    CHECK_EQ(fit.margin_low_hz, 1062);
    CHECK_EQ(fit.margin_high_hz, 1062);
    CHECK_EQ(fit.tolerance_hz, 1062);
    // Outside: negative margins, no tolerance.
    const PassbandFit below = unlimited::passband_fit(unlimited::occupied_band(400, 16000), passband(300, 2700));
    CHECK(!below.fits);
    CHECK_EQ(below.margin_low_hz, -38);
    CHECK_EQ(below.tolerance_hz, 0);
    const PassbandFit above = unlimited::passband_fit(unlimited::occupied_band(2600, 16000), passband(300, 2700));
    CHECK(!above.fits);
    CHECK_EQ(above.margin_high_hz, -38);
    // Margins beyond int16 are clipped.
    const PassbandFit far = unlimited::passband_fit(unlimited::occupied_band(1500, 0), passband(300, 2700));
    CHECK(!far.fits);
    CHECK_EQ(far.margin_high_hz, -32768);
}

// Spec 1.5 "Presets in typical filters": the pure filter room below / above each slot length's band in each filter
// (passband_fit(band, passband)), and the shift tolerance a sender prints (passband_fit(config)): the same room, each
// side also ending where the pitch would leave the search of the receiver that hears it (300..2700 Hz; from 1000 Hz
// below 8 ms).
TEST(protocol_presets_in_typical_filters) {
    const Passband filters[] = {passband(300, 2100), passband(300, 2700), passband(200, 2900), passband(100, 3000),
                                passband(300, 3000)};
    struct Row {
        uint32_t slot_ms;
        int16_t low[5];
        int16_t high[5];
    };
    const Row rooms[] = {{32, {1131, 1131, 1231, 1331, 1131}, {531, 1131, 1331, 1431, 1431}},
                         {16, {1062, 1062, 1162, 1262, 1062}, {462, 1062, 1262, 1362, 1362}},
                         {8, {925, 925, 1025, 1125, 925}, {325, 925, 1125, 1225, 1225}},
                         {4, {650, 650, 750, 850, 650}, {50, 650, 850, 950, 950}}};
    const Row shifts[] = {{32, {1131, 1131, 1200, 1200, 1131}, {531, 1131, 1200, 1200, 1200}},
                          {16, {1062, 1062, 1162, 1200, 1062}, {462, 1062, 1200, 1200, 1200}},
                          {8, {925, 925, 1025, 1125, 925}, {325, 925, 1125, 1200, 1200}},
                          {4, {500, 500, 500, 500, 500}, {50, 650, 850, 950, 950}}};
    for (size_t r = 0; r < count_of(rooms); ++r) {
        const Band band = unlimited::occupied_band(1500, rooms[r].slot_ms * 1000);
        for (size_t f = 0; f < count_of(filters); ++f) {
            const PassbandFit fit = unlimited::passband_fit(band, filters[f]);
            CHECK(fit.fits);
            CHECK_EQ(fit.margin_low_hz, rooms[r].low[f]);
            CHECK_EQ(fit.margin_high_hz, rooms[r].high[f]);
            CHECK_EQ(unsigned(fit.tolerance_hz), unsigned(std::min(rooms[r].low[f], rooms[r].high[f])));
            EncoderConfig config = EncoderConfig::from_preset(Preset::hf, base_rate);
            config.slot_us = shifts[r].slot_ms * 1000;
            config.passband = filters[f];
            const PassbandFit shift = unlimited::passband_fit(config);
            if (!CHECK(shift.fits) || !CHECK_EQ(shift.margin_low_hz, shifts[r].low[f]) ||
                !CHECK_EQ(shift.margin_high_hz, shifts[r].high[f]) ||
                !CHECK_EQ(unsigned(shift.tolerance_hz), unsigned(std::min(shifts[r].low[f], shifts[r].high[f])))) {
                NOTE("T %u ms in %u-%u Hz", unsigned(shifts[r].slot_ms), filters[f].low_hz, filters[f].high_hz);
            }
        }
    }
    // In a 1.8 kHz filter with the pitch at its centre: the room is hf +-762 Hz, fm +-350 Hz; fm's receiver searches
    // from 1000 Hz, so its shift is -200/+350 Hz.
    CHECK_EQ(unsigned(unlimited::passband_fit(unlimited::occupied_band(1200, 16000), filters[0]).tolerance_hz), 762u);
    CHECK_EQ(unsigned(unlimited::passband_fit(unlimited::occupied_band(1200, 4000), filters[0]).tolerance_hz), 350u);
    const Preset centred[] = {Preset::hf, Preset::fm};
    const int16_t centred_low[] = {762, 200};
    const int16_t centred_high[] = {762, 350};
    for (size_t c = 0; c < count_of(centred); ++c) {
        EncoderConfig config = EncoderConfig::from_preset(centred[c], base_rate);
        config.tone_hz = 1200;
        config.passband = filters[0];
        const PassbandFit shift = unlimited::passband_fit(config);
        CHECK(config.valid());
        CHECK_EQ(shift.margin_low_hz, centred_low[c]);
        CHECK_EQ(shift.margin_high_hz, centred_high[c]);
    }
    // Every preset fits its own passband: the shift tolerance in its own passband (spec 1.7).
    const Preset presets[] = {Preset::hf_slow, Preset::hf, Preset::hf_fast, Preset::am, Preset::fm};
    const int16_t own_low[] = {1131, 1062, 925, 1125, 500};
    const int16_t own_high[] = {1131, 1062, 925, 1200, 950};
    for (size_t p = 0; p < count_of(presets); ++p) {
        const PassbandFit shift = unlimited::passband_fit(EncoderConfig::from_preset(presets[p], base_rate));
        CHECK(shift.fits);
        CHECK_EQ(shift.margin_low_hz, own_low[p]);
        CHECK_EQ(shift.margin_high_hz, own_high[p]);
    }
}

// Spec 1.5, V14: one search rule for receivers and senders. search_range(passband, min_slot_us) is
// DecoderConfig::search_range(); a sender's shift tolerance never moves its pitch outside the search of a receiver
// whose window holds its T, never exceeds the filter room, and keeps the filter's fits; the receiver-side fit stops
// at its own search and never goes below 0.
TEST(protocol_shift_tolerance_follows_the_search) {
    const uint16_t k_edge_step_hz = 50;
    const uint8_t k_fastest_window_ms = 4;       // DecoderConfig::min_slot_ms 4..32; the fm profile's
    const uint8_t k_slowest_window_ms = 32;
    const uint8_t k_ssb_window_ms = 8;           // the ssb and am profiles'
    const uint32_t k_ssb_slowest_us = 64000;     // 8 x 8 ms
    const uint32_t k_us_per_ms = 1000;
    const uint32_t k_window_us_per_ms = 8000;    // a window starting at m ms holds T up to 8 m ms
    const uint32_t k_slot_step_us = 500;
    const uint16_t k_tone_step_hz = 11;
    size_t ranges = 0;
    for (uint8_t min_slot = k_fastest_window_ms; min_slot <= k_slowest_window_ms; ++min_slot) {
        for (uint16_t low = 0; low <= unlimited::k_max_passband_hz; low += k_edge_step_hz) {
            for (uint16_t high = low + k_edge_step_hz; high <= unlimited::k_max_passband_hz; high += k_edge_step_hz) {
                unlimited::DecoderConfig receiver;
                receiver.min_slot_ms = min_slot;
                receiver.passband = passband(low, high);
                const Passband a = receiver.search_range();
                const Passband b = unlimited::search_range(receiver.passband, uint32_t(min_slot) * k_us_per_ms);
                if (!CHECK(a.low_hz == b.low_hz && a.high_hz == b.high_hz)) {
                    NOTE("min_slot %u ms, passband %u-%u Hz", unsigned(min_slot), low, high);
                    return;
                }
                ++ranges;
            }
        }
    }
    const Passband filters[] = {passband(300, 2100), passband(300, 2700), passband(200, 2900), passband(100, 3000),
                                passband(300, 3000), passband(1000, 2000)};
    size_t senders = 0;
    for (uint32_t slot_us = unlimited::k_min_slot_us; slot_us <= unlimited::k_max_slot_us; slot_us += k_slot_step_us) {
        for (uint16_t tone = unlimited::k_min_tone_hz; tone <= unlimited::k_max_tone_hz; tone += k_tone_step_hz) {
            for (size_t f = 0; f < count_of(filters); ++f) {
                EncoderConfig config = EncoderConfig::from_preset(Preset::hf, base_rate);
                config.slot_us = slot_us;
                config.tone_hz = tone;
                config.bits_per_package = 1;
                config.passband = filters[f];
                if (!config.valid()) continue;
                // The default receiver: the fm profile's window below 8 ms, ssb to 64 ms, else the smallest whole ms.
                unlimited::DecoderConfig receiver;
                receiver.passband = config.passband;
                receiver.min_slot_ms = k_ssb_window_ms;
                if (slot_us < unlimited::k_fast_slot_us) receiver.min_slot_ms = k_fastest_window_ms;
                if (slot_us > k_ssb_slowest_us) {
                    receiver.min_slot_ms = uint8_t((slot_us + k_window_us_per_ms - 1) / k_window_us_per_ms);
                }
                REQUIRE(slot_us >= receiver.min_slot_ms * k_us_per_ms);
                REQUIRE(slot_us <= receiver.max_slot_ms() * k_us_per_ms);
                const Passband search = receiver.search_range();
                const Passband sender_search = unlimited::search_range(config);
                const PassbandFit room = unlimited::passband_fit(unlimited::occupied_band(config), config.passband);
                const PassbandFit shift = unlimited::passband_fit(config);
                if (!CHECK(sender_search.low_hz == search.low_hz && sender_search.high_hz == search.high_hz) ||
                    !CHECK(shift.fits == room.fits) || !CHECK(shift.margin_low_hz >= 0 && shift.margin_high_hz >= 0) ||
                    !CHECK(shift.margin_low_hz <= room.margin_low_hz && shift.margin_high_hz <= room.margin_high_hz) ||
                    !CHECK(tone - shift.margin_low_hz >= search.low_hz) ||
                    !CHECK(tone + shift.margin_high_hz <= search.high_hz)) {
                    NOTE("T %u us, tone %u Hz, passband %u-%u Hz", unsigned(slot_us), tone, filters[f].low_hz,
                         filters[f].high_hz);
                    return;
                }
                ++senders;
            }
        }
    }
    NOTE("%zu search ranges, %zu senders", ranges, senders);
    // A sender that does not fit keeps the filter's negative margins and no tolerance.
    EncoderConfig low = EncoderConfig::from_preset(Preset::hf, base_rate);
    low.tone_hz = 400;
    const PassbandFit outside = unlimited::passband_fit(low);
    CHECK(!outside.fits);
    CHECK_EQ(outside.margin_low_hz, -38);
    CHECK_EQ(outside.tolerance_hz, 0);
    // The receiver's own fit: an fm-profile receiver hearing 1100 Hz at 4 ms follows it down to its 1000 Hz search
    // edge (the filter would allow 250 Hz); a tone the search took 3 Hz below its edge has no room left below.
    const unlimited::DecoderConfig fm = unlimited::DecoderConfig::for_profile(unlimited::Profile::fm);
    const PassbandFit heard = unlimited::passband_fit(1100, 4000, fm.passband, fm.search_range());
    CHECK(heard.fits);
    CHECK_EQ(heard.margin_low_hz, 100);
    CHECK_EQ(heard.margin_high_hz, 1350);
    CHECK_EQ(heard.tolerance_hz, 100);
    const PassbandFit edge = unlimited::passband_fit(997, 4000, fm.passband, fm.search_range());
    CHECK(edge.fits);
    CHECK_EQ(edge.margin_low_hz, 0);
    CHECK_EQ(edge.tolerance_hz, 0);
}

// Spec 2.2: "Hi" (0x48 0x69) cut into packages of N = 8, 4 and 3 bits.
TEST(protocol_hi_packages_bit_exact) {
    const std::vector<uint8_t> hi = {0x48, 0x69};
    struct Case {
        uint8_t bits;
        const char* packages[6];
        size_t count;
        size_t slots;  // data slots + STOPs
    };
    const Case cases[] = {{8, {"01001000", "01101001"}, 2, 18},
                          {4, {"0100", "1000", "0110", "1001"}, 4, 20},
                          {3, {"010", "010", "000", "110", "100", "1"}, 6, 22}};
    for (size_t c = 0; c < count_of(cases); ++c) {
        EncoderConfig config;
        config.bits_per_package = cases[c].bits;
        const std::vector<std::string> packages = packages_of(config, hi);
        REQUIRE(packages.size() == cases[c].count);
        size_t slots = 0;
        for (size_t p = 0; p < packages.size(); ++p) {
            CHECK_EQ(packages[p], std::string(cases[c].packages[p]));
            slots += packages[p].size() + 1;
        }
        CHECK_EQ(slots, cases[c].slots);
    }
    // As a packet: 2D D4 00 02 48 69 93 4A; at N = 16 four packages; with hf 98 slots and the tail, 13344 samples.
    uint8_t packet[unlimited::k_packet_overhead + 2];
    REQUIRE(unlimited::packet_build(hi.data(), 2, packet, sizeof(packet)) == 8u);
    const uint8_t expected[] = {0x2D, 0xD4, 0x00, 0x02, 0x48, 0x69, 0x93, 0x4A};
    for (size_t i = 0; i < count_of(expected); ++i) CHECK_EQ(unsigned(packet[i]), unsigned(expected[i]));
    EncoderConfig wide;
    wide.bits_per_package = 16;
    const std::vector<std::string> packages = packages_of(wide, std::vector<uint8_t>(packet, packet + 8));
    REQUIRE(packages.size() == 4u);
    CHECK_EQ(packages[0], std::string("0010110111010100"));
    CHECK_EQ(packages[1], std::string("0000000000000010"));
    CHECK_EQ(packages[2], std::string("0100100001101001"));
    CHECK_EQ(packages[3], std::string("1001001101001010"));
    CHECK_EQ(Encoder(EncoderConfig()).duration_samples(8), 13344u);
    CHECK_EQ(render(EncoderConfig(), std::vector<uint8_t>(packet, packet + 8)).samples.size(), 13344u);
}

// U6 (a), (c): one isolated data slot (a lone 1 between zeros) and one marker of the real encoder at T = 4, 16 and 32 ms:
// 99 % width 4.34 / T, -26 dB 7.0 / T, -40 dB 9.87 / T (+-2 %); the constants cover them; the widths scale as 1 / T.
TEST(protocol_bandwidth_constants_against_encoder_spectrum) {
    const uint32_t slots_us[] = {4000, 16000, 32000};
    double scaled[3][3];
    for (size_t t = 0; t < count_of(slots_us); ++t) {
        EncoderConfig config;
        config.slot_us = slots_us[t];
        config.passband = passband(100, 3000);
        REQUIRE(config.valid());
        const Rendered r = render(config, std::vector<uint8_t>(1, 0x10));  // 0001 0000: one lone 1
        size_t one_begin = 0, one_end = 0, marker_begin = 0, marker_end = 0;
        uint32_t first_sync = 0;
        for (size_t n = 0; n < r.status.size(); ++n) {
            const EncoderStatus& s = r.status[n];
            if (s.kind == SlotKind::one) {
                if (one_end == 0) one_begin = n;
                one_end = n + 1;
            }
            if (s.segment == EncoderSegment::sync && first_sync == 0) first_sync = s.slot_index;
            if (s.segment == EncoderSegment::sync && s.slot_index == first_sync + 3) {  // a marker between markers
                if (marker_end == 0) marker_begin = n;
                marker_end = n + 1;
            }
        }
        REQUIRE(one_end > one_begin);
        const double slot_s = slots_us[t] * 1e-6;
        // +-6 / T holds the -40 dB width (+-4.94 / T).
        const double span = 6.0 / slot_s;
        const double step = 0.01 / slot_s;
        std::vector<double> frequencies;
        const std::vector<double> power = baseband_spectrum(config, r, one_begin, one_end, span, step, frequencies);
        const Widths w = widths_of(power, frequencies);
        scaled[t][0] = w.w99 * slot_s;
        scaled[t][1] = w.w26 * slot_s;
        scaled[t][2] = w.w40 * slot_s;
        NOTE("T %u ms: 99 %% %.3f/T, -26 dB %.3f/T, -40 dB %.3f/T", slots_us[t] / 1000, scaled[t][0], scaled[t][1],
             scaled[t][2]);
        CHECK_NEAR(scaled[t][0], 4.34, 4.34 * 0.02);
        CHECK_NEAR(scaled[t][1], 7.00, 7.00 * 0.02);
        CHECK_NEAR(scaled[t][2], 9.87, 9.87 * 0.02);
        CHECK(unlimited::k_band_99_milli / 1000.0 >= scaled[t][0]);
        CHECK_NEAR(unlimited::k_band_26db_milli / 1000.0, scaled[t][1], scaled[t][1] * 0.02);
        CHECK_NEAR(unlimited::k_band_40db_milli / 1000.0, scaled[t][2], scaled[t][2] * 0.02);
        // A marker is no wider than a data slot at -40 dB (spec 1.1: 9.76 / T against 9.87 / T).
        REQUIRE(marker_end > marker_begin);
        const std::vector<double> marker = baseband_spectrum(config, r, marker_begin, marker_end, span, step, frequencies);
        const Widths wm = widths_of(marker, frequencies);
        NOTE("   marker: 99 %% %.3f/T, -40 dB %.3f/T", wm.w99 * slot_s, wm.w40 * slot_s);
        CHECK(wm.w40 * slot_s <= 1.1 * scaled[t][2]);
    }
    for (size_t k = 0; k < 3; ++k) {
        CHECK_NEAR(scaled[1][k], scaled[2][k], 0.01 * scaled[2][k]);
        CHECK_NEAR(scaled[0][k], scaled[2][k], 0.01 * scaled[2][k]);
    }
}

// U6 (b): whole transmissions of 64 random bytes, N = 1, 4, 8, 16, 32 at 16 ms: >= 98.5 % of the power inside
// occupied_band(), >= 99.0 % inside the -26 dB width.
TEST(protocol_transmission_power_inside_band) {
    const uint8_t bits[] = {1, 4, 8, 16, 32};
    std::mt19937 generator(64);
    for (size_t b = 0; b < count_of(bits); ++b) {
        if (bits[b] > unlimited::k_max_bits_per_package) continue;
        EncoderConfig config;
        config.bits_per_package = bits[b];
        std::vector<uint8_t> data(64);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(generator());
        const Rendered r = render(config, data);
        const Band band = unlimited::occupied_band(config);
        const double half_26 = 0.5 * unlimited::width_26db_hz(config.slot_us);
        const double in_band = power_inside(r.samples, base_rate, band.low_hz, band.high_hz);
        const double in_26 = power_inside(r.samples, base_rate, config.tone_hz - half_26, config.tone_hz + half_26);
        NOTE("N %u: %.3f %% inside %u..%u Hz, %.3f %% inside the -26 dB width", unsigned(bits[b]), 100.0 * in_band,
             unsigned(band.low_hz), unsigned(band.high_hz), 100.0 * in_26);
        CHECK(in_band >= 0.985);
        CHECK(in_26 >= 0.990);
    }
}
