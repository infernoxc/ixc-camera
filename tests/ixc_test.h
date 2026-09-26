#pragma once

// Tiny dependency-free test harness. Each IXC_TEST registers itself at static-init time;
// test_main.cpp runs them (optionally filtered by a substring) and returns nonzero on failure.

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace ixc::test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& Registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& FailureCount() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

inline void ReportFailure(const char* file, int line, const std::string& what) {
    ++FailureCount();
    std::fprintf(stderr, "    FAILED %s(%d): %s\n", file, line, what.c_str());
}

template <typename A, typename B>
std::string DescribeNe(const A& a, const B& b) {
    std::ostringstream os;
    os << "expected equal: [" << a << "] vs [" << b << "]";
    return os.str();
}

}  // namespace ixc::test

#define IXC_TEST_CONCAT2(a, b) a##b
#define IXC_TEST_CONCAT(a, b) IXC_TEST_CONCAT2(a, b)

#define IXC_TEST(name)                                                                              \
    static void name();                                                                             \
    static ::ixc::test::Registrar IXC_TEST_CONCAT(ixc_reg_, name)(#name, &name);                    \
    static void name()

#define IXC_CHECK(cond)                                                                             \
    do {                                                                                            \
        if (!(cond)) ::ixc::test::ReportFailure(__FILE__, __LINE__, "check failed: " #cond);       \
    } while (0)

#define IXC_CHECK_EQ(a, b)                                                                          \
    do {                                                                                            \
        const auto& ixc_a_ = (a);                                                                   \
        const auto& ixc_b_ = (b);                                                                   \
        if (!(ixc_a_ == ixc_b_))                                                                    \
            ::ixc::test::ReportFailure(__FILE__, __LINE__, #a " == " #b ": " +                     \
                                                               ::ixc::test::DescribeNe(ixc_a_, ixc_b_)); \
    } while (0)

// Stops the current test when a precondition fails (avoids cascades / null dereferences).
#define IXC_REQUIRE(cond)                                                                           \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            ::ixc::test::ReportFailure(__FILE__, __LINE__, "requirement failed: " #cond);          \
            return;                                                                                 \
        }                                                                                           \
    } while (0)
