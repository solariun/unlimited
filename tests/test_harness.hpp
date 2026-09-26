#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct TestCase {
    const char* name;
    void (*function)();
    const char* file;
    int line;
};

// Thrown by REQUIRE to abandon the current test.
struct AbortTest {};

const int report_precision = 9;

template <typename T, std::size_t N>
constexpr std::size_t count_of(const T (&)[N]) {
    return N;
}

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& current_failures() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, void (*function)(), const char* file, int line) {
        const TestCase test_case = {name, function, file, line};
        registry().push_back(test_case);
    }
};

inline void fail(const char* file, int line, const std::string& message) {
    std::printf("    %s:%d: %s\n", file, line, message.c_str());
    ++current_failures();
}

inline bool check(bool ok, const char* macro, const char* expression, const char* file, int line) {
    if (!ok) fail(file, line, std::string(macro) + "(" + expression + ") failed");
    return ok;
}

template <typename A, typename B>
bool check_eq(const A& a, const B& b, const char* expr_a, const char* expr_b, const char* file, int line) {
    if (a == b) return true;
    std::ostringstream message;
    message.precision(report_precision);
    message << "CHECK_EQ(" << expr_a << ", " << expr_b << ") failed: " << a << " != " << b;
    fail(file, line, message.str());
    return false;
}

inline bool check_near(double a, double b, double tolerance, const char* expr_a, const char* expr_b, const char* file,
                       int line) {
    const double difference = std::fabs(a - b);
    if (difference <= tolerance) return true;  // also rejects NaN
    std::ostringstream message;
    message.precision(report_precision);
    message << "CHECK_NEAR(" << expr_a << ", " << expr_b << ") failed: " << a << " vs " << b << ", |diff| "
            << difference << " > " << tolerance;
    fail(file, line, message.str());
    return false;
}

inline bool matches_filter(const std::string& name, int argc, char** argv) {
    if (argc <= 1) return true;
    for (int i = 1; i < argc; ++i)
        if (name.find(argv[i]) != std::string::npos) return true;
    return false;
}

inline int run_all(int argc, char** argv) {
    int run = 0;
    std::vector<std::string> failed;
    for (std::size_t i = 0; i < registry().size(); ++i) {
        const TestCase& test_case = registry()[i];
        if (!matches_filter(test_case.name, argc, argv)) continue;
        ++run;
        current_failures() = 0;
        std::printf("[ RUN  ] %s\n", test_case.name);
        std::fflush(stdout);
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        try {
            test_case.function();
        } catch (const AbortTest&) {
        } catch (const std::exception& error) {
            fail(test_case.file, test_case.line, std::string("unexpected exception: ") + error.what());
        } catch (...) {
            fail(test_case.file, test_case.line, "unexpected exception");
        }
        const long long elapsed_ms = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        const bool passed = current_failures() == 0;
        std::printf("[ %s ] %s (%lld ms)\n", passed ? "PASS" : "FAIL", test_case.name, elapsed_ms);
        if (!passed) failed.push_back(test_case.name);
    }
    if (run == 0) {
        std::printf("no test matches the filter\n");
        return 1;
    }
    std::printf("\n%d test(s) run, %d passed, %d failed\n", run, run - static_cast<int>(failed.size()),
                static_cast<int>(failed.size()));
    for (std::size_t i = 0; i < failed.size(); ++i) std::printf("  FAILED: %s\n", failed[i].c_str());
    return failed.empty() ? 0 : 1;
}

}  // namespace test

#define TEST(name)                                                                                              \
    static void test_body_##name();                                                                             \
    static const ::test::Registrar test_registrar_##name(#name, &test_body_##name, __FILE__, __LINE__);         \
    static void test_body_##name()

#define CHECK(condition) ::test::check(static_cast<bool>(condition), "CHECK", #condition, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tolerance) ::test::check_near((a), (b), (tolerance), #a, #b, __FILE__, __LINE__)
#define REQUIRE(condition)                                                                                      \
    do {                                                                                                        \
        if (!::test::check(static_cast<bool>(condition), "REQUIRE", #condition, __FILE__, __LINE__))           \
            throw ::test::AbortTest();                                                                          \
    } while (0)

// Prints a measured value under the running test.
#define NOTE(...)                                                                                               \
    do {                                                                                                        \
        std::printf("    note: ");                                                                              \
        std::printf(__VA_ARGS__);                                                                               \
        std::printf("\n");                                                                                      \
    } while (0)
