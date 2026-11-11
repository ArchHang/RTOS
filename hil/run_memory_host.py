#!/usr/bin/env python3
"""Run the target memory fixture with a single-threaded GCC host port stub."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "hil/fw/Src/robustness_memory.c"

HOST_PORT = r"""
#define RTOS_PORT_H_
#include <stdint.h>

typedef uint32_t rtos_stack_t;

#define RTOS_PORT_ENTER_CRITICAL() ((void)0)
#define RTOS_PORT_EXIT_CRITICAL() ((void)0)
#define RTOS_PORT_IN_ISR() 0U

static inline uint32_t rtos_port_isr_priority(void)
{
    return 0U;
}
"""

HOST_MAIN = r"""
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef RTOS_HOST_LTO_POLL
#include <windows.h>
#endif
#include "robustness.h"
#include "robustness_memory.h"
#include "rtos_config.h"
#include "rtos_heap.h"
#include "rtos_perf.h"
#include "rtos_task.h"

static uint32_t passed;
static uint32_t failed;

void robustness_check(const char *name, bool is_ok)
{
    printf("%s %s\n", is_ok ? "PASS" : "FAIL", name);
    if (is_ok) {
        passed++;
    } else {
        failed++;
    }
}

void rtos_perf_register_pool(rtos_mempool_t *pool)
{
    (void)pool;
}

void rtos_assert_fail(const char *condition, const char *file, int line)
{
    fprintf(stderr, "%s:%d: assertion %s\n", file, line, condition);
    abort();
}

static int check_heap_configuration(void)
{
    const uint32_t rounded = RTOS_CONFIG_HEAP_SIZE & ~UINT32_C(7);
    const bool clean = rtos_heap_get_free() == 0U && rtos_heap_get_max_alloc() == 0U &&
                       rtos_heap_get_alloc_count() == 0U && rtos_heap_get_free_count() == 0U &&
                       rtos_heap_alloc(1U) == NULL;
    const rtos_status_t status = rtos_heap_init();
    if (!clean || (rounded < 24U && status != RTOS_ERR_PARAM)) {
        return 1;
    }
    if (rounded < 24U) {
        (void)printf("heap_configured=%" PRIu32 " usable=0 init=PARAM\n",
                     (uint32_t)RTOS_CONFIG_HEAP_SIZE);
        return rtos_heap_get_max_alloc() == 0U && rtos_heap_alloc(1U) == NULL ? 0 : 2;
    }
    const uint32_t capacity = rounded - 8U;
    if ((status != RTOS_OK) || (rtos_heap_get_max_alloc() != capacity) ||
        (rtos_heap_get_free() != capacity)) {
        return 3;
    }
    uint8_t *const buffer = rtos_heap_alloc(capacity);
    if (buffer == NULL) {
        return 4;
    }
    buffer[capacity - 1U] = 0xA5U;
    const bool repeated = rtos_heap_init() == RTOS_OK && rtos_heap_get_free() == 0U &&
                          rtos_heap_get_alloc_count() == 1U && buffer[capacity - 1U] == 0xA5U;
    rtos_heap_free(buffer);
    if (!repeated || rtos_heap_get_max_alloc() != capacity || rtos_heap_get_free_count() != 1U) {
        return 5;
    }
    void *const minimum = rtos_heap_alloc(1U);
    const uint32_t tail = rtos_heap_get_max_alloc();
    rtos_heap_free(minimum);
    (void)printf("heap_configured=%" PRIu32 " usable=%" PRIu32 " smallest_tail=%" PRIu32
                 " init=OK\n", (uint32_t)RTOS_CONFIG_HEAP_SIZE, capacity, tail);
    const uint32_t expected_tail = rounded >= 40U ? capacity - 16U : 0U;
    return minimum != NULL && tail == expected_tail &&
           rtos_heap_get_max_alloc() == capacity ? 0 : 6;
}

#ifdef RTOS_HOST_LTO_POLL
static DWORD WINAPI release_block(void *block)
{
    Sleep(60U);
    rtos_heap_free(block);
    (void)printf("writer_free=%" PRIu32 " writer_releases=%" PRIu32 "\n",
                 rtos_heap_get_free(), rtos_heap_get_free_count());
    (void)fflush(stdout);
    return 0U;
}

static int check_lto_observation(void)
{
    const uint32_t capacity = rtos_heap_get_max_alloc();
    void *const block = rtos_heap_alloc(capacity);
    if ((block == NULL) || (rtos_heap_get_free() != 0U)) {
        return 1;
    }
    (void)printf("capacity=%" PRIu32 "\n", capacity);
    (void)fflush(stdout);
    HANDLE worker = CreateThread(NULL, 0U, release_block, block, 0U, NULL);
    if (worker == NULL) {
        rtos_heap_free(block);
        return 2;
    }
    while (rtos_heap_get_free() == 0U) {
        /* Repeated getter calls are the only observation in this loop. */
    }
    const DWORD waited = WaitForSingleObject(worker, 1000U);
    DWORD worker_status = 1U;
    const bool completed = waited == WAIT_OBJECT_0 && GetExitCodeThread(worker, &worker_status);
    const bool closed = CloseHandle(worker) != 0;
    (void)printf("reader_free=%" PRIu32 " reader_releases=%" PRIu32 "\n",
                 rtos_heap_get_free(), rtos_heap_get_free_count());
    return completed && closed && worker_status == 0U &&
           rtos_heap_get_free() == capacity && rtos_heap_get_free_count() == 1U ? 0 : 3;
}
#endif

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "heap_configuration") == 0) {
        return check_heap_configuration();
    }
    if (rtos_heap_init() != RTOS_OK) {
        return 2;
    }
#ifdef RTOS_HOST_LTO_POLL
    if (argc == 2 && strcmp(argv[1], "lto_poll") == 0) {
        return check_lto_observation();
    }
#endif
    if (argc == 1) {
        printf("pointer_bytes=%zu pointer_alignment=%zu\n", sizeof(void *), _Alignof(void *));
        robustness_memory_run();
        printf("summary passed=%lu failed=%lu\n", (unsigned long)passed, (unsigned long)failed);
        return failed == 0U ? 0 : 1;
    }
    if (strcmp(argv[1], "heap_alignment") == 0) {
        uint8_t *payload = rtos_heap_alloc(64U);
        if (payload == NULL) {
            return 2;
        }
        rtos_heap_free(payload + 1U);
        rtos_heap_free(payload);
        return 0;
    }
    if (strcmp(argv[1], "pool_alignment") == 0) {
        static rtos_mempool_t pool;
        static _Alignas(void *) uint8_t storage[64];
        return rtos_mempool_init(&pool, storage, 9U, 2U) == RTOS_OK ? 0 : 2;
    }
    return 2;
}
"""


def check_lto_poll(compile_args, executable, expected):
    subprocess.run(compile_args + ["-flto", "-DRTOS_HOST_LTO_POLL=1"], check=True)
    try:
        run = subprocess.run([str(executable), "lto_poll"], check=False, timeout=3,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = run.stdout
        observed = "visible" if run.returncode == 0 else "error"
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b""
        observed = "stale"
    lines = output.decode("ascii", errors="replace").splitlines()
    print("\n".join(lines))
    capacity = lines[0].removeprefix("capacity=") if lines else ""
    writer_updated = capacity.isdecimal() and f"writer_free={capacity} writer_releases=1" in lines
    print(f"lto_poll_observed={observed} expected={expected} writer_updated={writer_updated}")
    return int(not writer_updated or observed != expected)


def check_heap_configurations(compile_args, executable, scratch):
    overlay = scratch / "heap_config.h"
    outcomes = []
    for size in (8, 23, 24, 25, 31, 32, 33, 39, 40, 41):
        overlay.write_text(f'#include "rtos_config.h"\n#undef RTOS_CONFIG_HEAP_SIZE\n'
                           f'#define RTOS_CONFIG_HEAP_SIZE {size}U\n', encoding="ascii")
        subprocess.run(compile_args + ["-include", str(overlay)], check=True)
        outcomes.append(subprocess.run([str(executable), "heap_configuration"],
                                       check=False, timeout=3).returncode)
    return int(any(outcomes))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", default=ROOT, type=Path,
                        help="tree supplying RTOS/Memory and RTOS/Core/Inc")
    parser.add_argument("--cc", default="gcc", help="native GCC executable")
    parser.add_argument("--opt", choices=("O0", "O2", "O3"), default="O2")
    parser.add_argument("--lto-poll", choices=("visible", "stale"),
                        help="replay heap visibility on Windows and require this observation")
    parser.add_argument("--heap-configurations", action="store_true",
                        help="check initialization at ten small heap capacities")
    args = parser.parse_args()
    if args.lto_poll and os.name != "nt":
        parser.error("--lto-poll requires a Windows host compiler")

    source_root = args.source_root.resolve()
    includes = source_root / "RTOS/Core/Inc"
    sources = [source_root / "RTOS/Memory" / name for name in
               ("rtos_heap.c", "rtos_mempool.c")]
    for path in [FIXTURE, includes / "rtos_config.h", *sources]:
        print(f"sha256={hashlib.sha256(path.read_bytes()).hexdigest()} {path}", flush=True)
    print(f"opt={args.opt} source_root={source_root}", flush=True)
    subprocess.run([args.cc, "--version"], check=True)
    subprocess.run([args.cc, "-dumpmachine"], check=True)

    with tempfile.TemporaryDirectory(prefix="rtos-memory-host-") as temporary:
        scratch = Path(temporary)
        port = scratch / "host_port.h"
        driver = scratch / "host_main.c"
        port.write_text(HOST_PORT, encoding="ascii")
        driver.write_text(HOST_MAIN, encoding="ascii")
        executable = scratch / "memory.exe"
        compile_args = [args.cc, "-std=c11", f"-{args.opt}", "-Wall", "-Wextra", "-Werror",
                        "-I", str(includes), "-I", str(ROOT / "hil/fw/Inc"),
                        "-include", str(port), str(driver), str(FIXTURE),
                        *(str(path) for path in sources), "-o", str(executable)]
        subprocess.run(compile_args, check=True)
        fixture_exit = subprocess.run([str(executable)], check=False).returncode
        print(f"fixture_exit={fixture_exit}", flush=True)

        subprocess.run(compile_args + ["-fsanitize=alignment",
                                       "-fsanitize-undefined-trap-on-error"], check=True)
        results = [fixture_exit]
        for case in ("heap_alignment", "pool_alignment"):
            status = subprocess.run([str(executable), case], check=False).returncode
            print(f"{case}_exit={status}", flush=True)
            results.append(status)
        if args.heap_configurations:
            results.append(check_heap_configurations(compile_args, executable, scratch))
        if args.lto_poll:
            results.append(check_lto_poll(compile_args, executable, args.lto_poll))
        return int(any(results))


if __name__ == "__main__":
    raise SystemExit(main())
