#!/usr/bin/env python3
"""Run the target memory fixture with a single-threaded GCC host port stub."""

import argparse
import hashlib
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "robustness.h"
#include "robustness_memory.h"
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

int main(int argc, char **argv)
{
    if (rtos_heap_init() != RTOS_OK) {
        return 2;
    }
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", default=ROOT, type=Path,
                        help="tree supplying RTOS/Memory and RTOS/Core/Inc")
    parser.add_argument("--cc", default="gcc", help="native GCC executable")
    parser.add_argument("--opt", choices=("O0", "O2", "O3"), default="O2")
    args = parser.parse_args()

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
        return int(any(results))


if __name__ == "__main__":
    raise SystemExit(main())
