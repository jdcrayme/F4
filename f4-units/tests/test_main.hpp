#pragma once

#include <iostream>
#include <string_view>
#include <cstring>
#include <cmath>
#include <cstdlib>

namespace test {

static int g_pass = 0;
static int g_fail = 0;
static int g_section_fail = 0;

struct WithinAbsMatcher {
    double target;
    double margin;
    bool match(double val) const { return std::abs(val - target) <= margin; }
};

inline bool check(bool cond, const char* expr, const char* file, int line) {
    if (cond) { g_pass++; return true; }
    g_fail++;
    std::cerr << "  FAIL: " << file << ":" << line << ": " << expr << "\n";
    return false;
}

inline bool check_approx(double actual, double expected, double margin,
                           const char* file, int line) {
    bool ok = std::abs(actual - expected) <= margin;
    if (ok) { g_pass++; return true; }
    g_fail++;
    std::cerr << "  FAIL: " << file << ":" << line << ": got " << actual
              << ", expected ~" << expected << " (margin " << margin << ")\n";
    return false;
}

struct Section {
    const char* name;
    bool active;
    Section(const char* n) : name(n), active(true) {
        std::cout << "  [ " << n << " ]\n";
        g_section_fail = g_fail;
    }
    ~Section() {
        if (g_fail == g_section_fail) {
            // section passed
        }
    }
};

struct TestRunner {
    const char* name;
    TestRunner(const char* n) : name(n) {
        g_pass = 0; g_fail = 0;
        std::cout << "\n=== " << name << " ===\n";
    }
    ~TestRunner() {
        if (g_fail == 0) {
            std::cout << "  PASSED (" << g_pass << " checks)\n";
        } else {
            std::cout << "  FAILED (" << g_fail << " of " << (g_pass + g_fail) << " checks)\n";
        }
    }
    bool ok() const { return g_fail == 0; }
};

} // namespace test

#define TEST_CHECK(expr) test::check((expr), #expr, __FILE__, __LINE__)
#define TEST_APPROX(actual, expected, margin) test::check_approx((actual), (expected), (margin), __FILE__, __LINE__)
#define TEST_SECTION(name) test::Section _section(name)
#define TEST_RUNNER(name) test::TestRunner _runner(name)
#define TEST_PASS() return _runner.ok() ? 0 : 1

int run_all_tests();

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int result = run_all_tests();
    std::cout << "\n==============================\n";
    if (result == 0) std::cout << "All tests passed.\n";
    else std::cout << "Some tests failed.\n";
    std::cout << "==============================\n";
    return result;
}
