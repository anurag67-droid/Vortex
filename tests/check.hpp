#pragma once
// Minimal test helpers. Unlike assert(), CHECK still runs in Release builds
// (where NDEBUG is defined), and a failure is reported without stopping the
// rest of the suite.
#include <iostream>

inline int g_failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        const auto& check_a_ = (a);                                                 \
        const auto& check_b_ = (b);                                                 \
        if (!(check_a_ == check_b_)) {                                              \
            ++g_failures;                                                           \
            std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << ": " #a " == " #b \
                      << "\n    left:  " << check_a_ << "\n    right: " << check_b_ << "\n"; \
        }                                                                           \
    } while (0)

inline int finish_tests(const char* suite) {
    if (g_failures == 0) {
        std::cout << suite << ": all checks passed\n";
        return 0;
    }
    std::cerr << suite << ": " << g_failures << " check(s) failed\n";
    return 1;
}
