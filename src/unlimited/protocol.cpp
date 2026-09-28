#include "unlimited/protocol.hpp"

namespace unlimited {

namespace {

const uint32_t k_half_band_scale = 500;   // half of the band: k_band_99_milli * 1000 / 2
const uint32_t k_width_scale = 1000;      // milli-cycles per slot to Hz at slot_us
const uint32_t k_max_hz = 0xFFFF;
const int32_t k_int16_min = -32768;
const int32_t k_int16_max = 32767;
const float k_centi_scale = 100.0f;
const float k_max_centi = 65535.0f;
const float k_us_per_byte = 100000.0f;    // 10 slots per byte: T_us = 100000 / B

// ceil(numerator / denominator), clipped to 0..65535 Hz; a zero slot is infinitely wide.
uint16_t ceil_hz(uint32_t numerator, uint32_t denominator) {
    if (denominator == 0) return static_cast<uint16_t>(k_max_hz);
    uint32_t quotient = numerator / denominator;
    if (quotient * denominator < numerator) ++quotient;
    return static_cast<uint16_t>(quotient > k_max_hz ? k_max_hz : quotient);
}

int16_t clip_int16(int32_t value) {
    return static_cast<int16_t>(value < k_int16_min ? k_int16_min : (value > k_int16_max ? k_int16_max : value));
}

int16_t smaller_of(int16_t a, int16_t b) {
    return a < b ? a : b;
}

// A filter margin that also stops at the edge of the search: the smaller of the two, never below 0 (a tone the
// search took a few Hz beyond its edge has no room left on that side).
int16_t room(int16_t filter_margin, int32_t search_margin) {
    const int32_t margin = search_margin < filter_margin ? search_margin : filter_margin;
    return clip_int16(margin > 0 ? margin : 0);
}

}  // namespace

uint16_t centi_bytes_per_second(float bytes_per_second) {
    const float centi = bytes_per_second * k_centi_scale + 0.5f;
    if (!(centi > 0.0f)) return 0;
    return static_cast<uint16_t>(centi > k_max_centi ? k_max_centi : centi);
}

uint32_t slot_us_for_speed(float bytes_per_second) {
    return slot_us_for_centi_speed(centi_bytes_per_second(bytes_per_second));
}

uint32_t slot_us_for_centi_speed(uint16_t centi) {
    if (centi == 0) return 0;
    return (k_centi_slot_numerator_us + centi / 2u) / centi;
}

float bytes_per_second(uint32_t slot_us) {
    return slot_us == 0 ? 0.0f : k_us_per_byte / static_cast<float>(slot_us);
}

bool slot_valid(uint32_t slot_us) {
    return slot_us >= k_min_slot_us && slot_us <= k_max_slot_us;
}

Band occupied_band(uint16_t tone_hz, uint32_t slot_us) {
    const uint32_t half = ceil_hz(k_band_99_milli * k_half_band_scale, slot_us);
    Band band;
    band.low_hz = static_cast<uint16_t>(tone_hz > half ? tone_hz - half : 0);
    const uint32_t high = static_cast<uint32_t>(tone_hz) + half;
    band.high_hz = static_cast<uint16_t>(high > k_max_hz ? k_max_hz : high);
    band.width_hz = static_cast<uint16_t>(band.high_hz - band.low_hz);
    return band;
}

uint16_t width_26db_hz(uint32_t slot_us) {
    return ceil_hz(k_band_26db_milli * k_width_scale, slot_us);
}

uint16_t width_40db_hz(uint32_t slot_us) {
    return ceil_hz(k_band_40db_milli * k_width_scale, slot_us);
}

bool passband_valid(const Passband& passband) {
    return passband.low_hz < passband.high_hz && passband.high_hz <= k_max_passband_hz;
}

PassbandFit passband_fit(const Band& band, const Passband& passband) {
    PassbandFit fit;
    fit.margin_low_hz = clip_int16(static_cast<int32_t>(band.low_hz) - static_cast<int32_t>(passband.low_hz));
    fit.margin_high_hz = clip_int16(static_cast<int32_t>(passband.high_hz) - static_cast<int32_t>(band.high_hz));
    fit.fits = fit.margin_low_hz >= 0 && fit.margin_high_hz >= 0;
    fit.tolerance_hz = static_cast<uint16_t>(fit.fits ? smaller_of(fit.margin_low_hz, fit.margin_high_hz) : 0);
    return fit;
}

Passband search_range(const Passband& passband, uint32_t slot_us) {
    const int32_t half = ceil_hz(k_band_99_milli * k_half_band_scale, slot_us);
    int32_t low = static_cast<int32_t>(passband.low_hz) + half;
    if (low < k_min_tone_hz) low = k_min_tone_hz;
    int32_t high = static_cast<int32_t>(passband.high_hz) - half;
    if (high > k_max_tone_hz) high = k_max_tone_hz;
    if (high < 0) high = 0;
    Passband range;
    const int32_t highest = static_cast<int32_t>(k_max_hz);
    range.low_hz = static_cast<uint16_t>(low > highest ? highest : low);
    range.high_hz = static_cast<uint16_t>(high);
    return range;
}

PassbandFit passband_fit(uint16_t tone_hz, uint32_t slot_us, const Passband& passband, const Passband& search) {
    PassbandFit fit = passband_fit(occupied_band(tone_hz, slot_us), passband);
    if (!fit.fits) return fit;
    fit.margin_low_hz = room(fit.margin_low_hz, static_cast<int32_t>(tone_hz) - static_cast<int32_t>(search.low_hz));
    fit.margin_high_hz = room(fit.margin_high_hz, static_cast<int32_t>(search.high_hz) - static_cast<int32_t>(tone_hz));
    fit.tolerance_hz = static_cast<uint16_t>(smaller_of(fit.margin_low_hz, fit.margin_high_hz));
    return fit;
}

}  // namespace unlimited
