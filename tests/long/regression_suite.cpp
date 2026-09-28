#include "regression.hpp"

#include "test_harness.hpp"

// Registration order is the run order: the integrity rows add up what the tests before them recorded (a report since
// V22).
using namespace unlimited::regression;

TEST(A1_A2_awgn_per_speed) {
    test_a1_awgn();
}

TEST(A3_acquisition_from_byte_0) {
    test_a3_acquisition();
}

TEST(S1_short_transmissions) {
    test_s1_short();
}

TEST(L5_clock_error_10_min) {
    test_l5_clock();
}

TEST(L19_passband_and_shift) {
    test_l19_passband();
}

TEST(C_channels) {
    test_c_channels();
}

TEST(F1_noise_stray_bytes) {
    test_f1_noise();
}

TEST(F2_carrier_stray_bytes) {
    test_f2_carrier();
}

TEST(F3_cw_stray_bytes) {
    test_f3_cw();
}

TEST(F4_speech_stray_bytes) {
    test_f4_speech();
}

TEST(Integrity_extra_and_shifted_bytes) {
    test_integrity();
}

TEST(Z_summary) {
    print_summary();
}
