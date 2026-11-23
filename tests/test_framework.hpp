// Minimal dependency-free test harness (GoogleTest-style registration).
// Kept tiny on purpose so the project builds anywhere with just a compiler.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tftest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline int run_all(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int ran = 0, failed_cases = 0;
    for (const auto& c : registry()) {
        if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
        const int before = failures();
        c.fn();
        ++ran;
        const bool ok = failures() == before;
        if (!ok) ++failed_cases;
        std::printf("[%s] %s\n", ok ? "  OK  " : " FAIL ", c.name);
    }
    std::printf("\n%d test(s), %d failed, %d failed assertion(s)\n", ran, failed_cases, failures());
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace tftest

#define TF_CAT2(a, b) a##b
#define TF_CAT(a, b) TF_CAT2(a, b)
#define TEST(name)                                                  \
    static void name();                                             \
    static ::tftest::Registrar TF_CAT(tf_reg_, name)(#name, &name); \
    static void name()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ++::tftest::failures();                                                  \
            std::printf("    %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        const auto& tf_a_ = (a);                                                                          \
        const auto& tf_b_ = (b);                                                                          \
        if (!(tf_a_ == tf_b_)) {                                                                          \
            ++::tftest::failures();                                                                       \
            std::printf("    %s:%d: CHECK_EQ(%s, %s) failed: %lld vs %lld\n", __FILE__, __LINE__, #a, #b, \
                        static_cast<long long>(tf_a_), static_cast<long long>(tf_b_));                    \
        }                                                                                                 \
    } while (0)

// Aborts the current test case on failure.
#define REQUIRE(cond)                                                                  \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            ++::tftest::failures();                                                    \
            std::printf("    %s:%d: REQUIRE(%s) failed\n", __FILE__, __LINE__, #cond); \
            return;                                                                    \
        }                                                                              \
    } while (0)

#define TF_TEST_MAIN                          \
    int main(int argc, char** argv) {         \
        return ::tftest::run_all(argc, argv); \
    }
