// ASAN canary: proves the ASAN harness is actually active.
// This file INTENTIONALLY contains a stack-use-after-scope. It must never be
// compiled into the normal test suite. run_mock_tests.ps1 builds it with
// -fsanitize=address and requires it to fail with an ASAN report; if it exits
// cleanly, sanitizer instrumentation/linking is broken and the run is invalid.

#include <stdio.h>

int main() {
  int* escaped = nullptr;
  {
    int stack_local = 42;
    escaped = &stack_local;
  }
  printf("%d\n", *escaped);  // deliberate stack-use-after-scope
  return 0;
}
