#include <cstdio>
#include <string>

#include "harness.h"

// Usage: lsmkv_tests [substring-filter]
// Exit code is the number of failed tests, so CI fails on any failure.
int main(int argc, char** argv) {
  const std::string filter = argc > 1 ? argv[1] : "";
  if (!filter.empty()) std::printf("filter: %s\n", filter.c_str());
  std::printf("\n");
  const int failed = lsmkv::test::RunAll(filter);
  return failed == 0 ? 0 : 1;
}
