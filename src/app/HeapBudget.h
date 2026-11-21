#pragma once

#include <stddef.h>

#if defined(ARDUINO)
#include <esp_heap_caps.h>
#endif

namespace rtb {

// Runtime memory budget for the HTTP print endpoints.
//
// RTB_MAX_JOB_BITMAP_BYTES is a LOGICAL protocol limit (64 KB); it says nothing
// about how much contiguous heap exists right now. On a WROOM-32E with Wi-Fi and
// BLE both up, the largest free block is a fraction of that, so a "logically
// legal" bitmap can still fail to allocate. These helpers answer the second
// question separately:
//
//   payload > configured maximum          -> 413 payload_too_large
//   payload allowed but no heap for it   -> 503 insufficient_memory
//
// 0 always means "unknown, do not check" so the host tests stay deterministic.

// Largest contiguous block the allocator can hand out right now (0 = unknown).
inline size_t largestHeapBlockBytes() {
#if defined(ARDUINO)
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#else
  return 0;
#endif
}

// Budget for ONE bitmap allocation: keep 1/reserve_divisor of the largest block
// for the response buffer, queue bookkeeping and the network stack.
inline size_t heapBudgetBytes(size_t largest_block, size_t reserve_divisor = 4) {
  if (largest_block == 0) return 0;
  if (reserve_divisor == 0) return largest_block;
  return largest_block - largest_block / reserve_divisor;
}

// Budget used by the print endpoints, sampled from the live heap.
inline size_t httpPrintHeapBudget() {
  return heapBudgetBytes(largestHeapBlockBytes());
}

}  // namespace rtb
