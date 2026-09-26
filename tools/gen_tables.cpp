// Regenerates k_quarter_sine for src/unlimited/tables.cpp.
// Build: c++ -std=c++11 -O2 tools/gen_tables.cpp -o bin/gen_tables && bin/gen_tables

#include <cmath>
#include <cstdio>

namespace {

const int k_table_bits = 8;
const int k_steps = 1 << k_table_bits;
const int k_entries = k_steps + 1;
const double k_full_scale = 65534.0;  // twice the Q15 full scale 32767
const double k_half_pi = 1.57079632679489661923;
const int k_per_line = 12;

}  // namespace

int main() {
    std::printf("const uint16_t k_quarter_sine[k_quarter_table_size] UNLIMITED_ROM = {\n");
    for (int i = 0; i < k_entries; ++i) {
        const long value = std::lround(k_full_scale * std::sin(k_half_pi * i / k_steps));
        if (i % k_per_line == 0) std::printf("    ");
        std::printf("%ld%s", value, i + 1 < k_entries ? "," : "");
        std::printf((i % k_per_line == k_per_line - 1 || i + 1 == k_entries) ? "\n" : " ");
    }
    std::printf("};\n");
    return 0;
}
