#include "unlimited/protocol.hpp"

namespace unlimited {

namespace {

const uint32_t k_half_band_scale = 500;   // half of the band: k_band_99_milli * 1000 / 2
const uint32_t k_width_scale = 1000;      // milli-cycles per slot to Hz at slot_us
const uint32_t k_max_hz = 0xFFFF;
const int32_t k_int16_min = -32768;
const int32_t k_int16_max = 32767;

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

Passband search_range(const Passband& passband, uint32_t min_slot_us) {
    const int32_t half = ceil_hz(k_band_99_milli * k_half_band_scale, k_speed_span * min_slot_us);
    int32_t low = static_cast<int32_t>(passband.low_hz) + half;
    if (low < k_min_tone_hz) low = k_min_tone_hz;
    if (min_slot_us < k_fast_slot_us && low < k_min_fast_tone_hz) low = k_min_fast_tone_hz;
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
