#include "test_harness.hpp"
#include "unlimited/decoder.hpp"
#include "unlimited/encoder.hpp"

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
const uint32_t us_per_ms = 1000;

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

// The windows of a transmission as slot strings, from the encoder's slot kinds: T a tone, . a silence.
std::vector<std::string> windows_of(const EncoderConfig& config, const std::vector<uint8_t>& data) {
    const Rendered rendered = render(config, data);
    std::vector<std::string> windows;
    uint32_t last_slot = 0xFFFFFFFFu;
    for (size_t n = 0; n < rendered.status.size(); ++n) {
        const EncoderStatus& s = rendered.status[n];
        if (s.slot_index == last_slot || s.segment != EncoderSegment::window) continue;
        last_slot = s.slot_index;
        if (s.slot == unlimited::k_start_slot) windows.push_back(std::string());
        windows.back() += s.kind == SlotKind::zero ? '.' : 'T';
    }
    return windows;
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

// Spec 1.3: B bytes/s in steps of 0.01, T = 1 / (10 B) rounded to the us; every step of 0.01 comes back from its T.
TEST(protocol_speed_arithmetic) {
    struct Row {
        float speed;
        uint16_t centi;
        uint32_t slot_us;
    };
    const Row rows[] = {{1.0f, 100, 100000}, {3.0f, 300, 33333}, {6.0f, 600, 16667}, {12.0f, 1200, 8333},
                        {25.0f, 2500, 4000}, {3.33f, 333, 30030}, {6.005f, 601, 16639}, {24.994f, 2499, 4002}};
    for (size_t i = 0; i < count_of(rows); ++i) {
        CHECK_EQ(unsigned(unlimited::centi_bytes_per_second(rows[i].speed)), unsigned(rows[i].centi));
        CHECK_EQ(unlimited::slot_us_for_speed(rows[i].speed), rows[i].slot_us);
        CHECK_EQ(unlimited::slot_us_for_centi_speed(rows[i].centi), rows[i].slot_us);
        CHECK(unlimited::slot_valid(rows[i].slot_us));
    }
    CHECK_EQ(unlimited::slot_us_for_speed(unlimited::k_default_bytes_per_second),
             unlimited::slot_us_for_centi_speed(unlimited::k_default_centi_bytes_per_second));
    CHECK_EQ(unlimited::slot_us_for_speed(unlimited::k_min_bytes_per_second), unlimited::k_max_slot_us);
    CHECK_EQ(unlimited::slot_us_for_speed(unlimited::k_max_bytes_per_second), unlimited::k_min_slot_us);
    size_t steps = 0;
    for (uint16_t centi = unlimited::k_min_centi_bytes_per_second; centi <= unlimited::k_max_centi_bytes_per_second;
         ++centi) {
        const uint32_t slot_us = unlimited::slot_us_for_centi_speed(centi);
        const uint32_t exact = (unlimited::k_centi_slot_numerator_us + centi / 2u) / centi;
        const float speed = unlimited::bytes_per_second(slot_us);
        if (!CHECK_EQ(slot_us, exact) || !CHECK(unlimited::slot_valid(slot_us)) ||
            !CHECK_EQ(unsigned(unlimited::centi_bytes_per_second(speed)), unsigned(centi)) ||
            !CHECK_EQ(unlimited::slot_us_for_speed(centi / 100.0f), slot_us)) {
            NOTE("%u centi-bytes/s", unsigned(centi));
            return;
        }
        ++steps;
    }
    NOTE("%zu speeds from 1.00 to 25.00 bytes/s", steps);
    // Outside 1..25 bytes/s the slot leaves the range (check() refuses it); nothing divides by zero.
    CHECK(!unlimited::slot_valid(unlimited::slot_us_for_speed(0.99f)));
    CHECK(!unlimited::slot_valid(unlimited::slot_us_for_speed(25.01f)));
    CHECK_EQ(unlimited::slot_us_for_speed(0.0f), 0u);
    CHECK_EQ(unlimited::slot_us_for_speed(-3.0f), 0u);
    CHECK_EQ(unlimited::slot_us_for_speed(0.004f), 0u);
    CHECK_EQ(unlimited::slot_us_for_centi_speed(0), 0u);
    CHECK_EQ(unsigned(unlimited::centi_bytes_per_second(1e9f)), 65535u);
    CHECK_EQ(unlimited::bytes_per_second(0), 0.0f);
    CHECK(!unlimited::slot_valid(0));
    CHECK(!unlimited::slot_valid(unlimited::k_min_slot_us - 1));
    CHECK(!unlimited::slot_valid(unlimited::k_max_slot_us + 1));
}

// U29: the integer formulas for every T from 4 to 100 ms in 1 ms steps and every tone in 300..2700 Hz.
TEST(protocol_band_functions_exact) {
    size_t checked = 0;
    for (uint32_t slot_us = unlimited::k_min_slot_us; slot_us <= unlimited::k_max_slot_us; slot_us += us_per_ms) {
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

// Spec 1.3: the widths at the five speeds of the table (the integer formulas round each half up).
TEST(protocol_width_table) {
    struct Row {
        float speed;
        uint16_t band;
        uint16_t w26;
        uint16_t w40;
    };
    const Row rows[] = {{1.0f, 44, 70, 99}, {3.0f, 134, 211, 298}, {6.0f, 264, 420, 594}, {12.0f, 530, 841, 1189},
                        {25.0f, 1100, 1750, 2475}};
    for (size_t i = 0; i < count_of(rows); ++i) {
        const uint32_t slot_us = unlimited::slot_us_for_speed(rows[i].speed);
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
    // 6 bytes/s at 1500 Hz in the 2.4 kHz SSB filter.
    const uint32_t six = unlimited::slot_us_for_speed(6.0f);
    const Band hf = unlimited::occupied_band(1500, six);
    CHECK_EQ(hf.low_hz, 1368);
    CHECK_EQ(hf.high_hz, 1632);
    const PassbandFit fit = unlimited::passband_fit(hf, passband(300, 2700));
    CHECK(fit.fits);
    CHECK_EQ(fit.margin_low_hz, 1068);
    CHECK_EQ(fit.margin_high_hz, 1068);
    CHECK_EQ(fit.tolerance_hz, 1068);
    // Outside: negative margins, no tolerance.
    const PassbandFit below = unlimited::passband_fit(unlimited::occupied_band(400, six), passband(300, 2700));
    CHECK(!below.fits);
    CHECK_EQ(below.margin_low_hz, -32);
    CHECK_EQ(below.tolerance_hz, 0);
    const PassbandFit above = unlimited::passband_fit(unlimited::occupied_band(2600, six), passband(300, 2700));
    CHECK(!above.fits);
    CHECK_EQ(above.margin_high_hz, -32);
    // Margins beyond int16 are clipped.
    const PassbandFit far = unlimited::passband_fit(unlimited::occupied_band(1500, 0), passband(300, 2700));
    CHECK(!far.fits);
    CHECK_EQ(far.margin_high_hz, -32768);
}

// Spec 1.3: the pure filter room below / above the band of each speed at 1500 Hz in typical filters
// (passband_fit(band, passband)), and the shift tolerance a sender prints (passband_fit(config)): the same room, each
// side also ending where the pitch would leave the receiver's search (never below 300 Hz or above 2700 Hz, so never
// more than 1200 Hz from 1500 Hz).
TEST(protocol_speeds_in_typical_filters) {
    const Passband filters[] = {passband(300, 2100), passband(300, 2700), passband(200, 2900), passband(100, 3000),
                                passband(300, 3000)};
    const float speeds[] = {1.0f, 3.0f, 6.0f, 12.0f, 25.0f};
    // [filter][speed]
    const int16_t room_low[5][5] = {{1178, 1133, 1068, 935, 650}, {1178, 1133, 1068, 935, 650},
                                    {1278, 1233, 1168, 1035, 750}, {1378, 1333, 1268, 1135, 850},
                                    {1178, 1133, 1068, 935, 650}};
    const int16_t room_high[5][5] = {{578, 533, 468, 335, 50},     {1178, 1133, 1068, 935, 650},
                                     {1378, 1333, 1268, 1135, 850}, {1478, 1433, 1368, 1235, 950},
                                     {1478, 1433, 1368, 1235, 950}};
    const int16_t shift_low[5][5] = {{1178, 1133, 1068, 935, 650}, {1178, 1133, 1068, 935, 650},
                                     {1200, 1200, 1168, 1035, 750}, {1200, 1200, 1200, 1135, 850},
                                     {1178, 1133, 1068, 935, 650}};
    const int16_t shift_high[5][5] = {{578, 533, 468, 335, 50},      {1178, 1133, 1068, 935, 650},
                                      {1200, 1200, 1200, 1135, 850}, {1200, 1200, 1200, 1200, 950},
                                      {1200, 1200, 1200, 1200, 950}};
    for (size_t f = 0; f < count_of(filters); ++f) {
        for (size_t v = 0; v < count_of(speeds); ++v) {
            EncoderConfig config;
            config.slot_us = unlimited::slot_us_for_speed(speeds[v]);
            config.passband = filters[f];
            const PassbandFit room = unlimited::passband_fit(unlimited::occupied_band(config), filters[f]);
            const PassbandFit shift = unlimited::passband_fit(config);
            if (!CHECK(config.valid()) || !CHECK(room.fits) || !CHECK_EQ(room.margin_low_hz, room_low[f][v]) ||
                !CHECK_EQ(room.margin_high_hz, room_high[f][v]) || !CHECK(shift.fits) ||
                !CHECK_EQ(shift.margin_low_hz, shift_low[f][v]) || !CHECK_EQ(shift.margin_high_hz, shift_high[f][v]) ||
                !CHECK_EQ(unsigned(shift.tolerance_hz), unsigned(std::min(shift_low[f][v], shift_high[f][v])))) {
                NOTE("%.0f bytes/s in %u-%u Hz", static_cast<double>(speeds[v]), filters[f].low_hz, filters[f].high_hz);
            }
        }
    }
    // 25 bytes/s (1100 Hz) does not fit a 500 Hz passband; in a 1.8 kHz filter it fits only with the pitch near its
    // centre.
    EncoderConfig narrow;
    narrow.slot_us = unlimited::slot_us_for_speed(25.0f);
    narrow.passband = passband(1250, 1750);
    CHECK(narrow.check() == unlimited::ConfigError::outside_passband);
    narrow.passband = filters[0];
    narrow.tone_hz = 1200;
    CHECK(narrow.valid());
    CHECK_EQ(unsigned(unlimited::passband_fit(narrow).tolerance_hz), 350u);
}

// Spec 1.3, 3.2: one search rule for receivers and senders. DecoderConfig::search_range() is search_range(passband,
// slot_us); a sender's shift tolerance never moves its pitch outside the search of the receiver of its speed, never
// exceeds the filter room, and keeps the filter's fit; the receiver-side fit stops at its own search and never goes
// below 0.
TEST(protocol_shift_tolerance_follows_the_search) {
    const uint16_t k_edge_step_hz = 50;
    const uint16_t k_centi_step = 7;
    const uint16_t k_tone_step_hz = 11;
    size_t ranges = 0;
    for (uint16_t centi = unlimited::k_min_centi_bytes_per_second; centi <= unlimited::k_max_centi_bytes_per_second;
         centi += k_centi_step * 10) {
        for (uint16_t low = 0; low <= unlimited::k_max_passband_hz; low += k_edge_step_hz) {
            for (uint16_t high = low + k_edge_step_hz; high <= unlimited::k_max_passband_hz; high += k_edge_step_hz) {
                unlimited::DecoderConfig receiver;
                receiver.slot_us = unlimited::slot_us_for_centi_speed(centi);
                receiver.passband = passband(low, high);
                const Passband a = receiver.search_range();
                const Passband b = unlimited::search_range(receiver.passband, receiver.slot_us);
                if (!CHECK(a.low_hz == b.low_hz && a.high_hz == b.high_hz)) {
                    NOTE("%u centi-bytes/s, passband %u-%u Hz", unsigned(centi), low, high);
                    return;
                }
                ++ranges;
            }
        }
    }
    const Passband filters[] = {passband(300, 2100), passband(300, 2700), passband(200, 2900), passband(100, 3000),
                                passband(300, 3000), passband(1000, 2000)};
    size_t senders = 0;
    for (uint16_t centi = unlimited::k_min_centi_bytes_per_second; centi <= unlimited::k_max_centi_bytes_per_second;
         centi += k_centi_step) {
        for (uint16_t tone = unlimited::k_min_tone_hz; tone <= unlimited::k_max_tone_hz; tone += k_tone_step_hz) {
            for (size_t f = 0; f < count_of(filters); ++f) {
                EncoderConfig config;
                config.slot_us = unlimited::slot_us_for_centi_speed(centi);
                config.tone_hz = tone;
                config.passband = filters[f];
                if (!config.valid()) continue;
                unlimited::DecoderConfig receiver;
                receiver.slot_us = config.slot_us;
                receiver.passband = config.passband;
                const Passband search = receiver.search_range();
                const Passband sender_search = unlimited::search_range(config);
                const PassbandFit room = unlimited::passband_fit(unlimited::occupied_band(config), config.passband);
                const PassbandFit shift = unlimited::passband_fit(config);
                if (!CHECK(receiver.valid()) ||
                    !CHECK(sender_search.low_hz == search.low_hz && sender_search.high_hz == search.high_hz) ||
                    !CHECK(shift.fits == room.fits) || !CHECK(shift.margin_low_hz >= 0 && shift.margin_high_hz >= 0) ||
                    !CHECK(shift.margin_low_hz <= room.margin_low_hz && shift.margin_high_hz <= room.margin_high_hz) ||
                    !CHECK(tone - shift.margin_low_hz >= search.low_hz) ||
                    !CHECK(tone + shift.margin_high_hz <= search.high_hz)) {
                    NOTE("%u centi-bytes/s, tone %u Hz, passband %u-%u Hz", unsigned(centi), tone, filters[f].low_hz,
                         filters[f].high_hz);
                    return;
                }
                ++senders;
            }
        }
    }
    NOTE("%zu search ranges, %zu senders", ranges, senders);
    // A sender that does not fit keeps the filter's negative margins and no tolerance.
    EncoderConfig low;
    low.tone_hz = 400;
    const PassbandFit outside = unlimited::passband_fit(low);
    CHECK(!outside.fits);
    CHECK_EQ(outside.margin_low_hz, -32);
    CHECK_EQ(outside.tolerance_hz, 0);
    // The receiver's own fit: an AM receiver at 1 byte/s hearing 1100 Hz follows it down to its 300 Hz search edge
    // (the filter would allow 978 Hz); a tone the search took 3 Hz below its edge has no room left below.
    unlimited::DecoderConfig am;
    am.slot_us = unlimited::slot_us_for_speed(1.0f);
    am.passband = passband(100, 3000);
    const Passband am_search = am.search_range();
    CHECK_EQ(am_search.low_hz, 300);
    CHECK_EQ(am_search.high_hz, 2700);
    const PassbandFit heard = unlimited::passband_fit(1100, am.slot_us, am.passband, am_search);
    CHECK(heard.fits);
    CHECK_EQ(heard.margin_low_hz, 800);
    CHECK_EQ(heard.margin_high_hz, 1600);
    CHECK_EQ(heard.tolerance_hz, 800);
    const PassbandFit edge = unlimited::passband_fit(297, am.slot_us, am.passband, am_search);
    CHECK(edge.fits);
    CHECK_EQ(edge.margin_low_hz, 0);
    CHECK_EQ(edge.tolerance_hz, 0);
    // FM at 25 bytes/s: the search keeps half the 1100 Hz band from each edge.
    unlimited::DecoderConfig fm;
    fm.slot_us = unlimited::slot_us_for_speed(25.0f);
    fm.passband = passband(300, 3000);
    CHECK_EQ(fm.search_range().low_hz, 850);
    CHECK_EQ(fm.search_range().high_hz, 2450);
}

// Spec 1.2, 2.2: "Hi" (0x48 0x69) as two windows: START, the bits MSB first (a tone is 1), STOP.
TEST(protocol_hi_windows_bit_exact) {
    const std::vector<uint8_t> hi = {0x48, 0x69};
    const std::vector<std::string> windows = windows_of(EncoderConfig(), hi);
    REQUIRE(windows.size() == 2u);
    CHECK_EQ(windows[0], std::string("T.T..T...T"));
    CHECK_EQ(windows[1], std::string("T.TT.T..TT"));
    // Every byte value: 10 slots, the tones where its bits are 1, START and STOP always tones.
    std::vector<uint8_t> all(256);
    for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<uint8_t>(i);
    const std::vector<std::string> every = windows_of(EncoderConfig(), all);
    REQUIRE(every.size() == all.size());
    for (size_t b = 0; b < all.size(); ++b) {
        std::string expected = "T";
        for (int bit = 7; bit >= 0; --bit) expected += ((b >> bit) & 1u) ? 'T' : '.';
        expected += 'T';
        if (!CHECK_EQ(every[b], expected)) {
            NOTE("byte 0x%02X", unsigned(b));
            return;
        }
    }
}

// U6 (a): one isolated data slot (a lone 1 between zeros) and one START of the real encoder at 25, 6 and 3 bytes/s:
// 99 % width 4.34 / T, -26 dB 7.0 / T, -40 dB 9.87 / T (+-2 %); the constants cover them; the widths scale as 1 / T.
TEST(protocol_bandwidth_constants_against_encoder_spectrum) {
    const float speeds[] = {25.0f, 6.0f, 3.0f};
    double scaled[3][3];
    for (size_t t = 0; t < count_of(speeds); ++t) {
        EncoderConfig config;
        config.slot_us = unlimited::slot_us_for_speed(speeds[t]);
        config.passband = passband(100, 3000);
        REQUIRE(config.valid());
        const Rendered r = render(config, std::vector<uint8_t>(1, 0x10));  // START 0001 0000 STOP: one lone 1
        size_t one_begin = 0, one_end = 0, start_begin = 0, start_end = 0;
        for (size_t n = 0; n < r.status.size(); ++n) {
            const EncoderStatus& s = r.status[n];
            if (s.kind == SlotKind::one) {
                if (one_end == 0) one_begin = n;
                one_end = n + 1;
            }
            if (s.kind == SlotKind::start) {
                if (start_end == 0) start_begin = n;
                start_end = n + 1;
            }
        }
        REQUIRE(one_end > one_begin);
        REQUIRE(start_end > start_begin);
        const double slot_s = config.slot_us * 1e-6;
        // +-6 / T holds the -40 dB width (+-4.94 / T).
        const double span = 6.0 / slot_s;
        const double step = 0.01 / slot_s;
        std::vector<double> frequencies;
        const std::vector<double> power = baseband_spectrum(config, r, one_begin, one_end, span, step, frequencies);
        const Widths w = widths_of(power, frequencies);
        scaled[t][0] = w.w99 * slot_s;
        scaled[t][1] = w.w26 * slot_s;
        scaled[t][2] = w.w40 * slot_s;
        NOTE("%.0f bytes/s: 99 %% %.3f/T, -26 dB %.3f/T, -40 dB %.3f/T", static_cast<double>(speeds[t]), scaled[t][0],
             scaled[t][1], scaled[t][2]);
        CHECK_NEAR(scaled[t][0], 4.34, 4.34 * 0.02);
        CHECK_NEAR(scaled[t][1], 7.00, 7.00 * 0.02);
        CHECK_NEAR(scaled[t][2], 9.87, 9.87 * 0.02);
        CHECK(unlimited::k_band_99_milli / 1000.0 >= scaled[t][0]);
        CHECK_NEAR(unlimited::k_band_26db_milli / 1000.0, scaled[t][1], scaled[t][1] * 0.02);
        CHECK_NEAR(unlimited::k_band_40db_milli / 1000.0, scaled[t][2], scaled[t][2] * 0.02);
        // The START is the same beep (spec 1.1): the same widths.
        const std::vector<double> start = baseband_spectrum(config, r, start_begin, start_end, span, step, frequencies);
        const Widths ws = widths_of(start, frequencies);
        CHECK_NEAR(ws.w99 * slot_s, scaled[t][0], 0.01 * scaled[t][0]);
        CHECK_NEAR(ws.w40 * slot_s, scaled[t][2], 0.01 * scaled[t][2]);
    }
    for (size_t k = 0; k < 3; ++k) {
        CHECK_NEAR(scaled[1][k], scaled[2][k], 0.01 * scaled[2][k]);
        CHECK_NEAR(scaled[0][k], scaled[2][k], 0.01 * scaled[2][k]);
    }
}

// U6 (b): whole transmissions of 64 random bytes at 1, 6 and 25 bytes/s: >= 98.5 % of the power inside
// occupied_band(), >= 99.0 % inside the -26 dB width.
TEST(protocol_transmission_power_inside_band) {
    const float speeds[] = {1.0f, 6.0f, 25.0f};
    std::mt19937 generator(64);
    for (size_t v = 0; v < count_of(speeds); ++v) {
        EncoderConfig config;
        config.slot_us = unlimited::slot_us_for_speed(speeds[v]);
        config.passband = passband(100, 3000);
        std::vector<uint8_t> data(64);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(generator());
        const Rendered r = render(config, data);
        const Band band = unlimited::occupied_band(config);
        const double half_26 = 0.5 * unlimited::width_26db_hz(config.slot_us);
        const double in_band = power_inside(r.samples, base_rate, band.low_hz, band.high_hz);
        const double in_26 = power_inside(r.samples, base_rate, config.tone_hz - half_26, config.tone_hz + half_26);
        NOTE("%.0f bytes/s: %.3f %% inside %u..%u Hz, %.3f %% inside the -26 dB width",
             static_cast<double>(speeds[v]), 100.0 * in_band, unsigned(band.low_hz), unsigned(band.high_hz),
             100.0 * in_26);
        CHECK(in_band >= 0.985);
        CHECK(in_26 >= 0.990);
    }
}
