"""Host checks for transport counters, reset, maxima, and timer wraparound."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "repo_bt_diag.h"
static int64_t clock_us;
int64_t esp_timer_get_time(void) { return clock_us; }
int main(void) {
    repo_bt_diag_t d;
    repo_bt_diag_reset();
    repo_bt_diag_enqueue(1, 1); repo_bt_diag_enqueue(2, 1); repo_bt_diag_enqueue(28, 0);
    clock_us = 1000; repo_bt_diag_read(1, 1);
    clock_us = 91000; repo_bt_diag_read(0, 1);
    clock_us = 92000; repo_bt_diag_read(0, 0);
    repo_bt_diag_path(6, 0, 0); repo_bt_diag_path(3, 1, 1);
    repo_bt_diag_write(661, 1); repo_bt_diag_write(661, 0);
    repo_bt_diag_guard(); repo_bt_diag_guard();
    clock_us = 100000; repo_bt_diag_completed(4, 4);
    clock_us = 180000; repo_bt_diag_completed(2, 2);
    repo_bt_diag_get(&d);
    assert(d.source_enqueued == 2 && d.source_rejected == 1 && d.source_dequeued == 2);
    assert(d.source_depth == 0 && d.source_high_water == 28 && d.source_gap_us_max == 90000);
    assert(d.l2cap_depth == 3 && d.l2cap_high_water == 6 && d.path_congested == 1 && d.path_dropped == 1);
    assert(d.media_writes == 1 && d.media_bytes == 661 && d.media_write_failures == 1);
    assert(d.controller_completed == 6 && d.controller_gap_us_max == 80000 && d.controller_outstanding_max == 4);
    assert(d.guard_deferrals == 2);
    repo_bt_diag_flush(); repo_bt_diag_get(&d); assert(d.source_flushed == 1);
    repo_bt_diag_reset(); repo_bt_diag_get(&d);
    assert(d.source_rejected == 0 && d.source_high_water == 0 && d.controller_gap_us_max == 0);
    clock_us = UINT32_MAX - 15; repo_bt_diag_read(1, 1);
    clock_us = (int64_t)UINT32_MAX + 101; repo_bt_diag_read(0, 1);
    repo_bt_diag_get(&d); assert(d.source_gap_us_max == 116);
    puts("PASS: transport counts, maxima, resets and 32-bit timer wraparound");
}
'''
with tempfile.TemporaryDirectory(prefix='bt-diag-') as temp:
    work = Path(temp)
    (work / 'esp_timer.h').write_text('#include <stdint.h>\nint64_t esp_timer_get_time(void);\n')
    (work / 'test.c').write_text(HARNESS)
    binary = work / ('test.exe' if os.name == 'nt' else 'test')
    subprocess.run([sys.executable, '-m', 'ziglang', 'cc', '-std=c11', '-O2',
                    '-I', str(work), '-I', str(ROOT / 'scripts'),
                    str(ROOT / 'scripts/repo_bt_diag.c'), str(work / 'test.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
