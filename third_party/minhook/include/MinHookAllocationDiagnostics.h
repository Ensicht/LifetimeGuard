#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MinHookAllocationDiagnostics {
    uint64_t origin;
    uint64_t minimum;
    uint64_t maximum;
    uint64_t queries;
    uint64_t free_candidates;
    uint64_t attempts;
    uint64_t allocation_failures;
    uint64_t reused;
    uint64_t last_candidate;
    uint64_t last_region_base;
    uint64_t last_region_size;
    uint32_t last_region_state;
    uint32_t query_error;
    uint32_t allocation_error;
    uint32_t reserved;
} MinHookAllocationDiagnostics;

// Available only to consumers built with MINHOOK_ENABLE_ALLOC_DIAGNOSTICS.
void MinHookGetAllocationDiagnostics(MinHookAllocationDiagnostics* output);

#ifdef __cplusplus
}
#endif
