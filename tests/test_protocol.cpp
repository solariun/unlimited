#include "test_harness.hpp"
#include "unlimited/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

using unlimited::HeaderFields;
using unlimited::Spacing;
using unlimited::gray_decode;
using unlimited::gray_encode;
using unlimited::header_fields;
using unlimited::header_symbol;
using unlimited::header_word;
using unlimited::k_header_slots;
using unlimited::k_header_words;
using unlimited::peak_symbol;
using unlimited::peak_tone;
using unlimited::tone_rotation;

namespace {

using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using test::count_of;

const unsigned header_tones = 8;
const unsigned min_distance = 6;
const unsigned max_repeats = 3;
const unsigned max_bits = 8;
const unsigned max_slots = 32;
const unsigned bits_per_byte = 8;
const uint32_t us_per_ms = 1000;
const unsigned n_code_shift = 7;
const unsigned n_code_reserved = 3;
const unsigned slot_ms_modulo = 8;

struct HeaderVector {
    const char* name;
    uint8_t bits_per_peak;
    uint8_t data_slots;
    uint32_t slot_ms;
    Spacing spacing;
    uint16_t word;
    uint8_t tones[header_tones];
};

// Spec 2.2 (design 3.2).
const HeaderVector header_vectors[] = {
    {"fm_fast T6 k3 N8", 3, 8, 6, Spacing::standard, 0x032, {2, 5, 6, 2, 7, 7, 4, 7}},
    {"fm T8 k3 N8", 3, 8, 8, Spacing::standard, 0x002, {2, 3, 1, 7, 6, 5, 0, 4}},
    {"hf_fast T16 k4 N8", 4, 8, 16, Spacing::standard, 0x003, {3, 2, 0, 6, 7, 4, 1, 5}},
    {"hf T32 k5 N8", 5, 8, 32, Spacing::standard, 0x004, {4, 5, 7, 1, 0, 3, 6, 2}},
    {"hf_robust T64 k6 N8", 6, 8, 64, Spacing::standard, 0x005, {5, 4, 6, 0, 1, 2, 7, 3}},
    {"hf_weak T128 k7 N8", 7, 8, 128, Spacing::standard, 0x006, {6, 7, 5, 3, 2, 1, 4, 0}},
    {"T32 k5 N16", 5, 16, 32, Spacing::standard, 0x084, {4, 7, 4, 6, 1, 7, 0, 7}},
    {"T16 k4 N32", 4, 32, 16, Spacing::standard, 0x103, {3, 6, 6, 3, 5, 7, 6, 4}},
    {"T128 k8 dense", 8, 8, 128, Spacing::dense, 0x047, {7, 7, 0, 4, 6, 2, 6, 6}},
};

typedef std::vector<std::vector<uint8_t> > Codebook;

Codebook all_headers() {
    Codebook book(k_header_words, std::vector<uint8_t>(header_tones));
    for (unsigned w = 0; w < k_header_words; ++w)
        for (unsigned j = 0; j < header_tones; ++j)
            book[w][j] = header_symbol(static_cast<uint16_t>(w), static_cast<uint8_t>(j));
    return book;
}

// Smallest distance between the window of a codeword sent `shift` slots off (window slot j holds sent slot
// j + shift) and any codeword, over the slots the window shares with the sent word.
unsigned misaligned_distance(const Codebook& book, int shift, unsigned& shared) {
    unsigned best = header_tones;
    shared = 0;
    for (unsigned sent = 0; sent < k_header_words; ++sent) {
        for (unsigned read = 0; read < k_header_words; ++read) {
            unsigned slots = 0;
            unsigned agree = 0;
            for (int j = 0; j < static_cast<int>(header_tones); ++j) {
                const int source = j + shift;
                if (source < 0 || source >= static_cast<int>(header_tones)) continue;
                ++slots;
                if (book[sent][source] == book[read][j]) ++agree;
            }
            shared = slots;
            if (slots - agree < best) best = slots - agree;
        }
    }
    return best;
}

unsigned popcount(unsigned value) {
    unsigned count = 0;
    for (; value != 0; value &= value - 1) ++count;
    return count;
}

unsigned reference_gray(unsigned value) {
    return value ^ (value >> 1);
}

// The n whose Gray code is `code`: bit i of n is the XOR of the code's bits i and above.
unsigned reference_gray_inverse(unsigned code) {
    unsigned value = 0;
    for (unsigned bits = code; bits != 0; bits >>= 1) value ^= bits;
    return value;
}

}  // namespace

TEST(protocol_header_vectors) {
    for (size_t v = 0; v < count_of(header_vectors); ++v) {
        const HeaderVector& vector = header_vectors[v];
        const uint16_t word =
            header_word(vector.bits_per_peak, vector.data_slots, vector.slot_ms * us_per_ms, vector.spacing);
        if (!CHECK_EQ(word, vector.word)) NOTE("%s", vector.name);
        for (uint8_t j = 0; j < k_header_slots; ++j)
            if (!CHECK_EQ(unsigned(header_symbol(word, j)), unsigned(vector.tones[j])))
                NOTE("%s slot %u", vector.name, j);
    }
}

TEST(protocol_header_code_distance_and_repeats) {
    const Codebook book = all_headers();
    unsigned distance = header_tones;
    for (unsigned a = 0; a < k_header_words; ++a) {
        for (unsigned b = a + 1; b < k_header_words; ++b) {
            unsigned d = 0;
            for (unsigned j = 0; j < header_tones; ++j) d += book[a][j] != book[b][j] ? 1u : 0u;
            if (d < distance) distance = d;
        }
    }
    unsigned repeats = 0;
    for (unsigned w = 0; w < k_header_words; ++w) {
        unsigned count[header_tones] = {0};
        for (unsigned j = 0; j < header_tones; ++j) {
            REQUIRE(book[w][j] < header_tones);
            if (++count[book[w][j]] > repeats) repeats = count[book[w][j]];
        }
    }
    NOTE("512 words: minimum distance %u, most repeats of one tone %u", distance, repeats);
    CHECK_EQ(distance, min_distance);
    CHECK_EQ(repeats, max_repeats);
}

TEST(protocol_header_code_misaligned_windows) {
    const Codebook book = all_headers();
    struct Case {
        int shift;
        unsigned slots;
        unsigned distance;
    };
    const Case cases[] = {{-1, 7, 3}, {1, 7, 3}, {-2, 6, 2}, {2, 6, 2}};
    for (size_t c = 0; c < count_of(cases); ++c) {
        unsigned shared = 0;
        const unsigned distance = misaligned_distance(book, cases[c].shift, shared);
        NOTE("window %+d slots: >= %u of %u symbols from every codeword", cases[c].shift, distance, shared);
        CHECK_EQ(shared, cases[c].slots);
        CHECK(distance >= cases[c].distance);
    }
}

TEST(protocol_header_fields_round_trip) {
    const uint8_t slot_counts[] = {8, 16, 32};
    const Spacing spacings[] = {Spacing::standard, Spacing::dense};
    for (uint8_t k = 1; k <= max_bits; ++k) {
        for (size_t n = 0; n < count_of(slot_counts); ++n) {
            for (uint32_t ms = 6; ms <= 128; ++ms) {
                for (size_t s = 0; s < count_of(spacings); ++s) {
                    const uint16_t word = header_word(k, slot_counts[n], ms * us_per_ms, spacings[s]);
                    REQUIRE(word < k_header_words);
                    const HeaderFields fields = header_fields(word);
                    CHECK_EQ(unsigned(fields.bits_per_peak), unsigned(k));
                    CHECK_EQ(unsigned(fields.data_slots), unsigned(slot_counts[n]));
                    CHECK_EQ(unsigned(fields.slot_ms_residue), unsigned(ms % slot_ms_modulo));
                    CHECK(fields.spacing == spacings[s]);
                }
            }
        }
    }
    // Every word with N code 0..2 comes back from its fields; N code 3 is reserved (data_slots 0).
    unsigned reserved = 0;
    for (unsigned w = 0; w < k_header_words; ++w) {
        const HeaderFields fields = header_fields(static_cast<uint16_t>(w));
        if ((w >> n_code_shift) == n_code_reserved) {
            CHECK_EQ(unsigned(fields.data_slots), 0u);
            ++reserved;
            continue;
        }
        const uint32_t slot_us = (fields.slot_ms_residue + slot_ms_modulo) * us_per_ms;
        CHECK_EQ(unsigned(header_word(fields.bits_per_peak, fields.data_slots, slot_us, fields.spacing)), w);
    }
    CHECK_EQ(reserved, unsigned(k_header_words) / 4);
    // A slot count the header cannot carry maps to the reserved code, which decoders reject.
    CHECK_EQ(unsigned(header_fields(header_word(5, 12, 32000, Spacing::standard)).data_slots), 0u);
}

TEST(protocol_gray_code) {
    for (unsigned v = 0; v < 256; ++v) {
        const uint8_t value = static_cast<uint8_t>(v);
        CHECK_EQ(unsigned(gray_encode(value)), reference_gray(v));
        CHECK_EQ(unsigned(gray_decode(gray_encode(value))), v);
        CHECK_EQ(unsigned(gray_encode(gray_decode(value))), v);
        if (v > 0) CHECK_EQ(popcount(gray_encode(value) ^ gray_encode(static_cast<uint8_t>(v - 1))), 1u);
    }
}

TEST(protocol_rotation) {
    const uint8_t expected[] = {1, 1, 1, 3, 5, 9, 17, 33};
    for (uint8_t k = 1; k <= max_bits; ++k) CHECK_EQ(unsigned(tone_rotation(k)), unsigned(expected[k - 1]));
}

// Spec 1.5 (design 2.3): k = 5, N = 8, bytes 48 69 21 00 FF; tone = gray^-1(s) + slot r (D33: the label of tone n is
// gray(n)).
TEST(protocol_mapping_vector) {
    const uint8_t k = 5;
    const uint8_t bytes[] = {0x48, 0x69, 0x21, 0x00, 0xFF};
    const uint8_t symbols[] = {9, 1, 20, 18, 2, 0, 7, 31};
    const uint8_t tones[] = {14, 6, 2, 11, 23, 25, 3, 24};
    uint64_t bits = 0;
    for (size_t i = 0; i < count_of(bytes); ++i) bits = (bits << bits_per_byte) | bytes[i];
    for (uint8_t slot = 0; slot < count_of(symbols); ++slot) {
        const unsigned shift = (count_of(symbols) - 1 - slot) * k;
        CHECK_EQ(unsigned((bits >> shift) & ((1u << k) - 1)), unsigned(symbols[slot]));
        CHECK_EQ(unsigned(peak_tone(symbols[slot], slot, k)), unsigned(tones[slot]));
        CHECK_EQ(unsigned(peak_symbol(tones[slot], slot, k)), unsigned(symbols[slot]));
    }
}

TEST(protocol_mapping_round_trip_and_neighbours) {
    for (uint8_t k = 1; k <= max_bits; ++k) {
        const unsigned tones = 1u << k;
        const unsigned rotation = tone_rotation(k);
        unsigned worst = 0;
        unsigned total = 0;
        unsigned pairs = 0;
        for (uint8_t slot = 0; slot < max_slots; ++slot) {
            std::vector<unsigned> used(tones, 0);
            for (unsigned s = 0; s < tones; ++s) {
                const uint8_t tone = peak_tone(static_cast<uint8_t>(s), slot, k);
                REQUIRE(tone < tones);
                ++used[tone];
                CHECK_EQ(unsigned(tone), (reference_gray_inverse(s) + slot * rotation) % tones);
                CHECK_EQ(unsigned(peak_symbol(tone, slot, k)), s);
            }
            for (unsigned t = 0; t < tones; ++t) CHECK_EQ(used[t], 1u);
            if (tones < 2) continue;
            for (unsigned t = 0; t < tones; ++t) {
                const unsigned next = (t + 1) % tones;
                const unsigned d = popcount(peak_symbol(static_cast<uint8_t>(t), slot, k) ^
                                            peak_symbol(static_cast<uint8_t>(next), slot, k));
                if (d > worst) worst = d;
                total += d;
                ++pairs;
            }
        }
        // U23: the label of the de-rotated tone n is gray(n), so neighbouring tones (cyclically) differ in 1 bit.
        NOTE("k %u: neighbouring tones differ in %.3f label bits on average, %u at most", unsigned(k),
             double(total) / pairs, worst);
        CHECK_EQ(worst, 1u);
    }
}

TEST(protocol_short_frame_packing) {
    const unsigned slot_counts[] = {8, 16, 32};
    for (size_t n = 0; n < count_of(slot_counts); ++n) {
        for (unsigned k = 1; k <= max_bits; ++k) {
            const unsigned frame_bytes = slot_counts[n] * k / bits_per_byte;
            CHECK_EQ(frame_bytes * bits_per_byte, slot_counts[n] * k);
            for (unsigned q = 1; q < frame_bytes; ++q) {
                const unsigned peaks = (bits_per_byte * q + k - 1) / k;
                CHECK(peaks < slot_counts[n]);
                CHECK_EQ(peaks * k / bits_per_byte, q);
            }
        }
    }
}
