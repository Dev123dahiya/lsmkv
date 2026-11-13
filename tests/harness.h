// A small test harness. The project has no third-party dependencies, so the
// runner lives here: tests self-register, failures report file and line and
// keep going, and the process exit code is the number of failed tests.
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lsmkv::test {

struct Case {
  std::string suite;
  std::string name;
  std::function<void()> fn;
};

inline std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Registrar {
  Registrar(std::string suite, std::string name, std::function<void()> fn) {
    Registry().push_back({std::move(suite), std::move(name), std::move(fn)});
  }
};

// Thrown by a failed assertion so the rest of the case is skipped but the
// runner carries on with the next one.
struct Failure {
  std::string message;
};

inline int& CheckCount() {
  static int n = 0;
  return n;
}

inline void Fail(const char* file, int line, const std::string& what) {
  throw Failure{std::string(file) + ":" + std::to_string(line) + ": " + what};
}

template <typename A, typename B>
std::string Describe(const A& a, const B& b) {
  std::string out;
  if constexpr (std::is_convertible_v<A, std::string_view>) {
    out += "\n    left:  \"" + std::string(std::string_view(a)) + "\"";
  } else if constexpr (std::is_arithmetic_v<A>) {
    out += "\n    left:  " + std::to_string(a);
  }
  if constexpr (std::is_convertible_v<B, std::string_view>) {
    out += "\n    right: \"" + std::string(std::string_view(b)) + "\"";
  } else if constexpr (std::is_arithmetic_v<B>) {
    out += "\n    right: " + std::to_string(b);
  }
  return out;
}

inline int RunAll(std::string_view filter) {
  int failed = 0, ran = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (const auto& c : Registry()) {
    const std::string full = c.suite + "." + c.name;
    if (!filter.empty() && full.find(filter) == std::string_view::npos) continue;
    ++ran;
    const int before = CheckCount();
    const auto start = std::chrono::steady_clock::now();
    try {
      c.fn();
      const auto ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
      std::printf("  ok   %-52s %5d checks  %6.1f ms\n", full.c_str(), CheckCount() - before, ms);
    } catch (const Failure& f) {
      std::printf("  FAIL %-52s\n       %s\n", full.c_str(), f.message.c_str());
      ++failed;
    } catch (const std::exception& e) {
      std::printf("  FAIL %-52s\n       threw: %s\n", full.c_str(), e.what());
      ++failed;
    }
  }
  const auto ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("\n%d test%s, %d check%s, %d failed  (%.0f ms)\n", ran, ran == 1 ? "" : "s",
              CheckCount(), CheckCount() == 1 ? "" : "s", failed, ms);
  return failed;
}

}  // namespace lsmkv::test

#define LSMKV_CAT_(a, b) a##b
#define LSMKV_CAT(a, b) LSMKV_CAT_(a, b)

#define TEST(suite, name)                                                              \
  static void LSMKV_CAT(suite##_##name##_body_, __LINE__)();                           \
  static const ::lsmkv::test::Registrar LSMKV_CAT(suite##_##name##_reg_, __LINE__)(    \
      #suite, #name, LSMKV_CAT(suite##_##name##_body_, __LINE__));                     \
  static void LSMKV_CAT(suite##_##name##_body_, __LINE__)()

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++::lsmkv::test::CheckCount();                                                  \
    if (!(cond)) ::lsmkv::test::Fail(__FILE__, __LINE__, "CHECK failed: " #cond);   \
  } while (0)

#define CHECK_EQ(a, b)                                                                        \
  do {                                                                                        \
    ++::lsmkv::test::CheckCount();                                                            \
    const auto& _a = (a);                                                                     \
    const auto& _b = (b);                                                                     \
    if (!(_a == _b))                                                                          \
      ::lsmkv::test::Fail(__FILE__, __LINE__,                                                 \
                          "CHECK_EQ failed: " #a " == " #b + ::lsmkv::test::Describe(_a, _b)); \
  } while (0)

#define CHECK_NE(a, b)                                                                        \
  do {                                                                                        \
    ++::lsmkv::test::CheckCount();                                                            \
    const auto& _a = (a);                                                                     \
    const auto& _b = (b);                                                                     \
    if (_a == _b)                                                                             \
      ::lsmkv::test::Fail(__FILE__, __LINE__,                                                 \
                          "CHECK_NE failed: " #a " != " #b + ::lsmkv::test::Describe(_a, _b)); \
  } while (0)

#define CHECK_LT(a, b)                                                                        \
  do {                                                                                        \
    ++::lsmkv::test::CheckCount();                                                            \
    const auto& _a = (a);                                                                     \
    const auto& _b = (b);                                                                     \
    if (!(_a < _b))                                                                           \
      ::lsmkv::test::Fail(__FILE__, __LINE__,                                                 \
                          "CHECK_LT failed: " #a " < " #b + ::lsmkv::test::Describe(_a, _b));  \
  } while (0)

// Asserts a Status is OK, printing its message when it is not.
#define CHECK_OK(expr)                                                                   \
  do {                                                                                   \
    ++::lsmkv::test::CheckCount();                                                       \
    ::lsmkv::Status _s = (expr);                                                         \
    if (!_s.ok())                                                                        \
      ::lsmkv::test::Fail(__FILE__, __LINE__,                                            \
                          "expected OK from " #expr ", got " + _s.ToString());           \
  } while (0)
