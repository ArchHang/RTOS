"""Compile the actual diagnostic C with valid and invalid capacity settings.

Ten ARM compilation cases verify exact rejection diagnostics, legal boundaries,
and an inactive OFF configuration. No linking, serial or hardware operations.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess

from check_disabled_equivalence import allocated_object
from strict.run_strict import INCLUDES, TOOLS

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SOURCE = ROOT / "RTOS/Services/rtos_diag.c"
EXPECTED_ERROR = "Diagnostic capacity must be a power of two in [8, 1024]"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot():
    paths = {SOURCE, Path(__file__).resolve(), HERE / "check_disabled_equivalence.py",
             HERE / "strict/run_strict.py"}
    for include in INCLUDES:
        paths.update((ROOT / include).rglob("*.h"))
    return {path.relative_to(ROOT).as_posix(): digest(path) for path in sorted(paths)}


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--tools", type=Path, default=TOOLS)
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    out = args.out_dir.resolve() if args.out_dir else HERE / "config_bounds/evidence" / stamp
    if HERE not in out.parents or out.exists():
        raise SystemExit("Output must be new and inside this batch")
    out.mkdir(parents=True)
    build = HERE / ".build" / ("config_bounds_" + stamp)
    build.mkdir(parents=True)
    before = snapshot()
    (out / "sources.json").write_text(json.dumps(before, indent=2) + "\n", encoding="utf-8")
    compiler = args.tools / "arm-none-eabi-gcc.exe"
    version = subprocess.run([str(compiler), "--version"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                             timeout=30)
    (out / "compiler.txt").write_text(version.stdout, encoding="utf-8")
    if version.returncode:
        raise SystemExit("Compiler version query failed")
    matrix = [(1, capacity, False) for capacity in (0, 1, 7, 12, 1025, 2048)]
    matrix += [(1, capacity, True) for capacity in (8, 64, 1024)]
    matrix += [(0, 12, True)]
    results = []
    for enabled, capacity, should_compile in matrix:
        tag = f"diagnostics{enabled}_capacity{capacity}"
        target = build / (tag + ".o")
        command = [str(compiler), "-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16",
                   "-mfloat-abi=hard", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-DDEBUG", "-DUSE_HAL_DRIVER", "-DSTM32F446xx",
                   "-DRTOS_CONFIG_ASSERT_ENABLED=1", "-DRTOS_CONFIG_USE_CHANNEL=1",
                   f"-DRTOS_CONFIG_USE_DIAGNOSTICS={enabled}",
                   f"-DRTOS_CONFIG_DIAGNOSTIC_CAPACITY={capacity}",
                   "-ffunction-sections", "-fdata-sections",
                   *["-I" + str(ROOT / directory) for directory in INCLUDES],
                   "-c", str(SOURCE), "-o", str(target)]
        process = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, encoding="utf-8", errors="replace",
                                 timeout=120)
        (out / (tag + ".txt")).write_text(process.stdout, encoding="utf-8")
        result = {"tag": tag, "enabled": enabled, "capacity": capacity,
                  "expected_compile": should_compile, "command": command,
                  "exit_code": process.returncode, "diagnostics": process.stdout}
        if should_compile:
            result["passed"] = process.returncode == 0 and target.is_file() and not re.search(
                r"\b(?:warning|error):", process.stdout)
            if target.is_file():
                result["object_sha256"] = digest(target)
                sections = allocated_object(target)
                result["allocated_sections"] = sections
                if not enabled:
                    no_state = not any(section["bytes"] for name, section in sections.items()
                                       if name.startswith((".bss", ".data")))
                    result["disabled_has_no_static_state"] = no_state
                    result["passed"] = result["passed"] and no_state
        else:
            matches = re.findall(r'^.*error: #error "([^"]+)"\s*$', process.stdout, re.MULTILINE)
            result["exact_capacity_error"] = matches == [EXPECTED_ERROR]
            result["passed"] = (process.returncode != 0 and result["exact_capacity_error"]
                                 and not target.exists())
        if target.is_file():
            if build.resolve() not in target.resolve().parents:
                raise RuntimeError("Generated object escaped the isolated build directory")
            target.unlink()
        results.append(result)
        print(tag + ": " + ("PASS" if result["passed"] else "FAIL"), flush=True)
    after = snapshot()
    (out / "sources_after.json").write_text(json.dumps(after, indent=2) + "\n", encoding="utf-8")
    passed = all(result["passed"] for result in results) and before == after
    report = {"status": "PASS" if passed else "FAIL", "completed_utc": datetime.now(timezone.utc).isoformat(),
              "checks": len(results), "passed": sum(bool(result["passed"]) for result in results),
              "source_identity_stable": before == after, "expected_error": EXPECTED_ERROR,
              "results": results,
              "limits": ["Compilation only; not a linked application RAM-fit or runtime result",
                         "Maximum legal capacity may exceed an application's remaining SRAM"]}
    (out / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    build.rmdir()
    print("Evidence: " + str(out))
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
