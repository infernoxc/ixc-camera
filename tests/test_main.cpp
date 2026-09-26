#include "ixc_test.h"

#include <cstring>

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    int failedTests = 0;
    for (const auto& c : ixc::test::Registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = ixc::test::FailureCount();
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        const bool ok = ixc::test::FailureCount() == before;
        std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", c.name);
        ++ran;
        if (!ok) ++failedTests;
    }
    std::printf("\n%d test(s) run, %d failed, %d check failure(s)\n", ran, failedTests, ixc::test::FailureCount());
    if (ran == 0) {
        std::fprintf(stderr, "no tests matched\n");
        return 2;
    }
    return failedTests == 0 ? 0 : 1;
}
