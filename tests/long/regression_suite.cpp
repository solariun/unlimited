#include "regression.hpp"

#include "test_harness.hpp"

// Registration order is the run order: F5 and F6 check what the tests before them recorded.
using namespace unlimited::regression;

TEST(L5_clock_error_10_min) {
    test_l5_clock();
}

TEST(A1_awgn_smart_line) {
    test_a1_smart_line();
}

TEST(A1_n_sweep_at_the_hf_gate) {
    test_a1_n_sweep();
}

TEST(A2_awgn_fixed_line) {
    test_a2_fixed_line();
}

TEST(A3_acquisition) {
    test_a3_acquisition();
}

TEST(A4_snr_report) {
    test_a4_snr_report();
}

TEST(C1_ccir_good) {
    test_c1_ccir_good();
}

TEST(C2_ccir_moderate) {
    test_c2_ccir_moderate();
}

TEST(C3_ccir_poor) {
    test_c3_ccir_poor();
}

TEST(C4_flat_rayleigh) {
    test_c4_flat_rayleigh();
}

TEST(C5_qsb) {
    test_c5_qsb();
}

TEST(C6_qrn_blanker) {
    test_c6_qrn();
}

TEST(C7_agc) {
    test_c7_agc();
}

TEST(C8_carrier_qrm) {
    test_c8_carrier();
}

TEST(C9_keyed_cw_qrm) {
    test_c9_cw();
}

TEST(C10_fm) {
    test_c10_fm();
}

TEST(C11_am) {
    test_c11_am();
}

TEST(C12_flutter) {
    test_c12_flutter();
}

TEST(C13_agc_fading) {
    test_c13_agc_fading();
}

TEST(C14_sideband_shift_fading) {
    test_c14_sideband_shift_fading();
}

TEST(C15_fm_emphasis_mismatch) {
    test_c15_fm_emphasis_mismatch();
}

TEST(L19_passband) {
    test_l19_passband();
}

TEST(L20_cold_late_join) {
    test_l20_cold_late_join();
}

TEST(F1_noise_false_lock) {
    test_f1_noise();
}

TEST(F2_carrier_false_lock) {
    test_f2_carrier();
}

TEST(F3_cw_false_lock) {
    test_f3_cw();
}

TEST(F4_speech_false_lock) {
    test_f4_speech();
}

TEST(F7_package_learning) {
    test_f7_package_learning();
}

TEST(F5_crc_valid_wrong_packets) {
    test_f5_packets();
}

TEST(F6_wrong_byte_runs) {
    test_f6_runs();
}

TEST(Z_summary) {
    print_summary();
}
