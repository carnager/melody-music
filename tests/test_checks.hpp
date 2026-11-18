// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// How a plain test program (not a QtTest one) checks: CHECK notes a failure
// and goes on, the program ending `return failures == 0 ? 0 : 1`; require
// stops at the first, for what the rest depends on.

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace checks {

inline int failures = 0;

inline void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

inline void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

} // namespace checks

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): it names the line and the expression.
#define CHECK(expression) ::checks::check((expression), #expression, __LINE__)
