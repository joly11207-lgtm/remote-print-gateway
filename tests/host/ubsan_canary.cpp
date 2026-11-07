// UBSAN canary: proves undefined-behaviour instrumentation is active.
// Intentionally contains a signed integer overflow. Only built by
// run_mock_tests.ps1 with -fsanitize=undefined; never part of the normal suite.

#include <stdio.h>

int main() {
  volatile int x = 2147483647;
  volatile int y = x + 1;  // deliberate signed overflow
  printf("%d\n", y);
  return 0;
}
