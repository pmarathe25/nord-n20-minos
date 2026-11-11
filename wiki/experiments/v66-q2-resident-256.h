#ifndef N20_V66_Q2_RESIDENT_256_H
#define N20_V66_Q2_RESIDENT_256_H

#include "v66-q2-256.h"

/* Opt-in extension to the existing single-matrix RPC. A session starts at slot
 * zero, so existing RPC_UPLOAD/PROJECT callers retain their old behavior.
 * One host owner must serialize calls on a handle, including select/project. */
enum {
    RPC_RESIDENT_SELECT = 0x07010000,
    RPC_RESIDENT_EVICT = 0x08010000,
    RPC_RESIDENT_STATS = 0x09000100,
    N20_RESIDENT_SLOTS = 96,
};

#define N20_RESIDENT_LIMIT_BYTES ((uint64_t)256 * 1024 * 1024)

struct n20_resident_slot { uint32_t slot; };
struct n20_resident_stats {
    uint64_t allocated_bytes;
    uint64_t limit_bytes;
    uint32_t selected_slot;
    uint32_t allocated_slots;
    uint32_t complete_slots;
    uint32_t reserved;
};

#endif
