#include "repo_bt_diag.h"
#include <stdatomic.h>
#include "esp_timer.h"
static _Atomic uint32_t source_enqueued;
static _Atomic uint32_t source_dequeued;
static _Atomic uint32_t source_rejected;
static _Atomic uint32_t source_flushed;
static _Atomic uint32_t source_depth;
static _Atomic uint32_t source_high_water;
static _Atomic uint32_t source_gap_us_max;
static _Atomic uint32_t media_writes;
static _Atomic uint32_t media_bytes;
static _Atomic uint32_t media_write_failures;
static _Atomic uint32_t l2cap_depth;
static _Atomic uint32_t l2cap_high_water;
static _Atomic uint32_t path_congested;
static _Atomic uint32_t path_dropped;
static _Atomic uint32_t guard_deferrals;
static _Atomic uint32_t controller_completed;
static _Atomic uint32_t controller_gap_us_max;
static _Atomic uint32_t controller_outstanding_max;

static _Atomic uint32_t last_read, last_completion;
#define LOAD(x) atomic_load_explicit(&(x), memory_order_relaxed)
#define STORE(x, v) atomic_store_explicit(&(x), (v), memory_order_relaxed)
#define ADD(x, v) atomic_fetch_add_explicit(&(x), (v), memory_order_relaxed)
static void maximum(_Atomic uint32_t* value, uint32_t candidate) {
    uint32_t old = atomic_load_explicit(value, memory_order_relaxed);
    while (old < candidate && !atomic_compare_exchange_weak_explicit(value, &old, candidate,
           memory_order_relaxed, memory_order_relaxed)) {}
}
static void gap(_Atomic uint32_t* previous, _Atomic uint32_t* max_gap) {
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint32_t before = atomic_exchange_explicit(previous, now, memory_order_relaxed);
    if (before) maximum(max_gap, now - before);
}
void repo_bt_diag_get(repo_bt_diag_t* out) {
    out->source_enqueued = LOAD(source_enqueued);
    out->source_dequeued = LOAD(source_dequeued);
    out->source_rejected = LOAD(source_rejected);
    out->source_flushed = LOAD(source_flushed);
    out->source_depth = LOAD(source_depth);
    out->source_high_water = LOAD(source_high_water);
    out->source_gap_us_max = LOAD(source_gap_us_max);
    out->media_writes = LOAD(media_writes);
    out->media_bytes = LOAD(media_bytes);
    out->media_write_failures = LOAD(media_write_failures);
    out->l2cap_depth = LOAD(l2cap_depth);
    out->l2cap_high_water = LOAD(l2cap_high_water);
    out->path_congested = LOAD(path_congested);
    out->path_dropped = LOAD(path_dropped);
    out->guard_deferrals = LOAD(guard_deferrals);
    out->controller_completed = LOAD(controller_completed);
    out->controller_gap_us_max = LOAD(controller_gap_us_max);
    out->controller_outstanding_max = LOAD(controller_outstanding_max);
}
void repo_bt_diag_reset(void) {
    STORE(source_enqueued, 0);
    STORE(source_dequeued, 0);
    STORE(source_rejected, 0);
    STORE(source_flushed, 0);
    STORE(source_depth, 0);
    STORE(source_high_water, 0);
    STORE(source_gap_us_max, 0);
    STORE(media_writes, 0);
    STORE(media_bytes, 0);
    STORE(media_write_failures, 0);
    STORE(l2cap_depth, 0);
    STORE(l2cap_high_water, 0);
    STORE(path_congested, 0);
    STORE(path_dropped, 0);
    STORE(guard_deferrals, 0);
    STORE(controller_completed, 0);
    STORE(controller_gap_us_max, 0);
    STORE(controller_outstanding_max, 0);
    STORE(last_read, 0);
    STORE(last_completion, 0);
}
void repo_bt_diag_enqueue(unsigned depth, int accepted) {
    STORE(source_depth, depth);
    maximum(&source_high_water, depth);
    if (accepted) ADD(source_enqueued, 1); else ADD(source_rejected, 1);
}
void repo_bt_diag_read(unsigned depth, int present) {
    STORE(source_depth, depth);
    if (present) { ADD(source_dequeued, 1); gap(&last_read, &source_gap_us_max); }
}
void repo_bt_diag_flush(void) { ADD(source_flushed, 1); STORE(source_depth, 0); }
void repo_bt_diag_path(unsigned depth, int congested, int dropped) {
    STORE(l2cap_depth, depth);
    maximum(&l2cap_high_water, depth);
    if (congested) ADD(path_congested, 1);
    if (dropped) ADD(path_dropped, 1);
}
void repo_bt_diag_write(unsigned bytes, int accepted) {
    if (accepted) { ADD(media_writes, 1); ADD(media_bytes, bytes); }
    else ADD(media_write_failures, 1);
}
void repo_bt_diag_guard(void) { ADD(guard_deferrals, 1); }
void repo_bt_diag_completed(unsigned count, unsigned outstanding) {
    ADD(controller_completed, count);
    maximum(&controller_outstanding_max, outstanding);
    if (count) gap(&last_completion, &controller_gap_us_max);
}
