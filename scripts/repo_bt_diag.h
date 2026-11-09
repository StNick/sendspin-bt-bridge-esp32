#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Relaxed atomic, cumulative transport diagnostics. Not a playback-delivery guarantee.
typedef struct {
    uint32_t source_enqueued;
    uint32_t source_dequeued;
    uint32_t source_rejected;
    uint32_t source_flushed;
    uint32_t source_depth;
    uint32_t source_high_water;
    uint32_t source_gap_us_max;
    uint32_t media_writes;
    uint32_t media_bytes;
    uint32_t media_write_failures;
    uint32_t l2cap_depth;
    uint32_t l2cap_high_water;
    uint32_t path_congested;
    uint32_t path_dropped;
    uint32_t guard_deferrals;
    uint32_t controller_completed;
    uint32_t controller_gap_us_max;
    uint32_t controller_outstanding_max;
} repo_bt_diag_t;
void repo_bt_diag_get(repo_bt_diag_t* out);
void repo_bt_diag_reset(void);
void repo_bt_diag_enqueue(unsigned depth, int accepted);
void repo_bt_diag_read(unsigned depth, int present);
void repo_bt_diag_flush(void);
void repo_bt_diag_path(unsigned depth, int congested, int dropped);
void repo_bt_diag_write(unsigned bytes, int accepted);
void repo_bt_diag_guard(void);
void repo_bt_diag_completed(unsigned count, unsigned outstanding);
#ifdef __cplusplus
}
#endif
