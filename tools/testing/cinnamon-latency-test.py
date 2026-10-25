#!/usr/bin/env python3
"""Run actual Cinnamon latency functions with minimal host-side kernel stubs.

This tests arithmetic/adaptation, not queue concurrency or on-device I/O.
Run with Python 3 before activating the kernel's Python 2 environment.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'block/cinnamon-iosched.c').read_text()


def function(name):
    match = re.search(r'^static [^\n]+\b' + name + r'\([^;]*?\)\n\{', source, re.M)
    if not match:
        raise RuntimeError('Cannot find function: ' + name)
    opening = source.index('{', match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
typedef uint64_t u64;
typedef uint64_t atomic64_t;
typedef int atomic_t;
#define atomic64_read(p) (*(p))
#define atomic_read(p) (*(p))
#define atomic64_set(p,v) (*(p)=(v))
#define atomic_set(p,v) (*(p)=(v))
#define div64_u64(a,b) ((a)/(b))
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define clamp(x,a,b) min(max(x,a),b)
#define time_before(a,b) ((long)((a)-(b))<0)
#define msecs_to_jiffies(x) (x)
enum {PRIO_CRITICAL, PRIO_READ_FG, PRIO_WRITE_FG, PRIO_READ_BG, PRIO_WRITE_BG, PRIO_TRIM, PRIO_COUNT};
struct cinnamon_data {
    atomic64_t total_latency_ns;
    atomic_t num_requests;
    unsigned long last_manual_param_change_jiffies, last_adjust_jiffies;
    int adjust_interval_ms, target_latency_ms;
    int queue_counts[PRIO_COUNT];
    int read_batch, write_batch, write_expire_ms, pressure_thres;
    int saved_read_batch, saved_write_batch, saved_write_expire_ms, emmc_mode;
};
struct elevator_queue { struct cinnamon_data *elevator_data; };
static unsigned long jiffies = 10000;
'''
constants = '\n'.join(re.findall(r'^#define EMMC_(?:MAX_BATCH|MIN_PRESSURE_THRES|MAX_PRESSURE_THRES)\s+[^\n]+', source, re.M))
functions = '\n'.join(function(name) for name in (
    'cinnamon_clamp_for_emmc', 'cinnamon_update_latency',
    'cinnamon_adjust_params', 'cinnamon_avg_latency_ns_show'))
tests = r'''
static struct cinnamon_data fresh(void) {
    struct cinnamon_data cd = {0};
    cd.target_latency_ms = 50;
    cd.adjust_interval_ms = 100;
    cd.read_batch = cd.write_batch = 8;
    cd.write_expire_ms = 100;
    cd.pressure_thres = 16;
    return cd;
}
static void check_report(struct cinnamon_data *cd, const char *expected) {
    char buf[64];
    struct elevator_queue e = {cd};
    cinnamon_avg_latency_ns_show(&e, buf);
    assert(strcmp(buf, expected) == 0);
}
int main(void) {
    struct cinnamon_data cd = fresh();
    int i;
    check_report(&cd, "0\n");
    cinnamon_adjust_params(&cd);
    assert(cd.read_batch == 8 && cd.write_expire_ms == 100);

    cinnamon_update_latency(&cd, 100000000ULL);
    check_report(&cd, "100000000\n");
    for (i = 0; i < 250; i++) {
        cinnamon_update_latency(&cd, 100000000ULL);
        /* Count rollover must never change the reported EWMA. */
        check_report(&cd, "100000000\n");
    }
    cinnamon_update_latency(&cd, 200000000ULL);
    check_report(&cd, "130000000\n");

    cd = fresh();
    for (i = 0; i < 100; i++) cinnamon_update_latency(&cd, 200000000ULL);
    cinnamon_adjust_params(&cd);
    assert(cd.read_batch == 4 && cd.write_batch == 4);
    assert(cd.write_expire_ms == 50 && cd.pressure_thres == 8);

    cd = fresh();
    for (i = 0; i < 100; i++) cinnamon_update_latency(&cd, 10000000ULL);
    cinnamon_adjust_params(&cd);
    assert(cd.read_batch == 16 && cd.write_batch == 16);
    assert(cd.write_expire_ms == 200 && cd.pressure_thres == 32);

    cd = fresh();
    for (i = 0; i < 100; i++) cinnamon_update_latency(&cd, 50000000ULL);
    cinnamon_adjust_params(&cd);
    assert(cd.read_batch == 8 && cd.write_expire_ms == 100);

    cd = fresh();
    cd.last_manual_param_change_jiffies = jiffies;
    cinnamon_update_latency(&cd, 200000000ULL);
    cinnamon_adjust_params(&cd);
    assert(cd.read_batch == 8); /* Keep manual-tuning grace period. */
    puts("PASS: empty, first sample, EWMA/count rollover, smoothing, high/low/neutral adaptation, manual grace");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='cinnamon-latency-') as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(prefix + constants + '\n' + functions + '\n' + tests)
    subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror', str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)
