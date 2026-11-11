"""Replay deterministic host models against the RTOS sources in this checkout."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
CORE_HEADERS = ROOT / "RTOS/Core/Inc"
PROFILES = {
    "o0": ("-O0",),
    "o2": ("-O2",),
    "o3": ("-O3",),
    "o2_ubsan": ("-O2", "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error"),
}
SEEDS = ("00000001", "12345678", "a5a5a5a5", "deadbeef")
STEPS = 200_000
MEMORY_CASE = re.compile(
    r"^PASS family=(heap|pool) seed=0x([0-9a-f]{8}) operations=(\d+) alloc=(\d+) "
    r"free=(\d+) exhausted=(\d+) complete_reuse=(\d+) rejection=(\d+) "
    r"peak_live=(\d+) checked_bytes=(\d+)$", re.MULTILINE
)


def check_memory(output):
    expected = {(family, seed) for family in ("heap", "pool") for seed in SEEDS}
    records = MEMORY_CASE.findall(output)
    if (len(records) != len(expected) or {(row[0], row[1]) for row in records} != expected
            or output.splitlines().count("PASS random_memory 8 seed/backend cases") != 1):
        raise ValueError("Missing, duplicate or unexpected allocator model results")
    for family, seed, *counts in records:
        operations, alloc, free, exhausted, reuse, _, peak, checked = map(int, counts)
        if operations != STEPS or alloc != free or reuse != 8 or min(exhausted, peak, checked) <= 0:
            raise ValueError(f"Invalid accounting for {family} seed={seed}")
    return sorted(records)


def check_ordered(output):
    expected = [f"PASS seed={seed} steps={STEPS}" for seed in SEEDS]
    if output.splitlines() != expected:
        raise ValueError("Ordered model did not pass all four seeds")
    return expected


def run(command, scratch):
    environment = {**os.environ, "TMP": str(scratch), "TEMP": str(scratch), "TMPDIR": str(scratch)}
    return subprocess.run(command, cwd=scratch, env=environment, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=180, check=False)


def checked_text(process):
    if process.returncode != 0:
        raise ValueError(f"Unexpected exit status {process.returncode}")
    return process.stdout.decode("utf-8", errors="replace").replace("\r\n", "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, required=True, help="new directory for logs and result")
    parser.add_argument("--cc", default="gcc", help="host GCC executable")
    parser.add_argument("--suite", choices=("all", "memory", "ordered"), default="all")
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if compiler is None:
        parser.error(f"compiler not found: {args.cc}")

    output = args.out_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    memory = HERE / "memory"
    ordered = HERE / "ordered"
    memory_sources = (ROOT / "RTOS/Memory/rtos_heap.c", ROOT / "RTOS/Memory/rtos_mempool.c")
    ordered_source = ROOT / "RTOS/Core/Src/rtos_wait.c"
    suites = {
        "memory": (
            ["-I", str(CORE_HEADERS), "-include", str(memory / "host_port.h"),
             str(memory / "memory_random.c"), *map(str, memory_sources)],
            [memory / "host_port.h", memory / "memory_random.c", *memory_sources,
             *sorted(CORE_HEADERS.glob("*.h"))], check_memory,
        ),
        "ordered": (
            ["-I", str(ordered), str(ordered / "sem_order_model.c"), str(ordered_source)],
            [ordered / "rtos_internal.h", ordered / "rtos_task.h",
             ordered / "sem_order_model.c", ordered_source], check_ordered,
        ),
    }
    report = {"scope": "single-threaded host models", "suites": {}}
    failed = False
    with tempfile.TemporaryDirectory(prefix="host_", dir=output) as temporary:
        scratch = Path(temporary)
        report["compiler"] = checked_text(run([compiler, "--version"], scratch)).splitlines()[0]
        report["target"] = checked_text(run([compiler, "-dumpmachine"], scratch)).strip()
        for suite, (inputs, sources, validate) in suites.items():
            if args.suite not in ("all", suite):
                continue
            suite_result = {"sha256": {source.relative_to(ROOT).as_posix():
                                      hashlib.sha256(source.read_bytes()).hexdigest()
                                      for source in sources}, "profiles": {}}
            report["suites"][suite] = suite_result
            reference = None
            for profile, flags in PROFILES.items():
                tag = f"{suite}_{profile}"
                image = scratch / (f"{tag}.exe" if os.name == "nt" else tag)
                command = [compiler, "-std=c11", *flags, "-Wall", "-Wextra", "-Werror",
                           *inputs, "-o", str(image)]
                result = {"command": command, "status": "FAIL"}
                suite_result["profiles"][profile] = result
                try:
                    built = run(command, scratch)
                    (output / f"{tag}_compile.txt").write_bytes(built.stdout)
                    result["compile_exit"] = built.returncode
                    checked_text(built)
                    tested = run([str(image)], scratch)
                    (output / f"{tag}_run.txt").write_bytes(tested.stdout)
                    result["run_exit"] = tested.returncode
                    records = validate(checked_text(tested))
                    if reference is not None and records != reference:
                        raise ValueError("Results differ between optimization profiles")
                    reference = records
                    result.update(status="PASS", records=records)
                except (ValueError, OSError, subprocess.TimeoutExpired) as error:
                    result["error"] = str(error)
                    failed = True
                print(f"{tag}: {result['status']}", flush=True)
    report["status"] = "FAIL" if failed else "PASS"
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
