"""Compile 18 owned RTOS units across the diagnostic configuration matrix.

This script never links, flashes or accesses serial ports. M7 uses the F446 BSP
headers and proves code generation only, not H7 integration or cache correctness.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
TOOLS = Path(r"E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins"
             r"\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32"
             r"_1.1.100.202309141235\tools\bin")
INCLUDES = ("Core/Inc", "AI/output/int8", "RTOS/Core/Inc", "RTOS/Core/Private",
    "Drivers/STM32F4xx_HAL_Driver/Inc", "Drivers/STM32F4xx_HAL_Driver/Inc/Legacy",
    "Drivers/CMSIS/Device/ST/STM32F4xx/Include", "Drivers/CMSIS/Include",
    "RTOS/lib/third/CherryUSB/core", "RTOS/lib/third/CherryUSB/common",
    "RTOS/lib/third/CherryUSB/class/cdc", "RTOS/lib/third/CherryUSB/class/hid",
    "RTOS/lib/third/CherryUSB/class/msc", "RTOS/lib/third/CherryUSB/port/dwc2")
PUBLIC_PROBE = '''#include "rtos_diag.h"
int diagnostic_public_probe(void)
{
    rtos_diag_info_t info;
    rtos_diag_record_t record;
    rtos_diag_explanation_t explanation;
    (void)rtos_diag_reset();
    (void)rtos_diag_mark(1U, 2U);
    (void)rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER);
    (void)rtos_diag_get_info(&info);
    (void)rtos_diag_read(1U, 1U, &record);
    (void)rtos_diag_explain(1U, 1U, &explanation);
    rtos_diag_fault_freeze(1U, (uintptr_t)0U);
    return 0;
}
'''


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def execute(command):
    try:
        result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, encoding="utf-8", errors="replace",
                                timeout=120)
        return result.returncode, result.stdout
    except (OSError, subprocess.TimeoutExpired) as error:
        return -1, repr(error)


def flags(cpu, enabled, basepri, assertions, optimization, standard="gnu11"):
    return ["-mcpu=cortex-" + cpu, "-mthumb",
            "-mfpu=" + ("fpv4-sp-d16" if cpu == "m4" else "fpv5-d16"), "-mfloat-abi=hard",
            "-std=" + standard, "-Wall", "-Wextra", "-Werror", "-" + optimization,
            "-DDEBUG", "-DUSE_HAL_DRIVER", "-DSTM32F446xx", "-DRTOS_CONFIG_USE_CHANNEL=1",
            "-DRTOS_CONFIG_USE_DIAGNOSTICS=" + str(enabled),
            "-DRTOS_CONFIG_USE_BASEPRI=" + str(basepri),
            "-DRTOS_CONFIG_ASSERT_ENABLED=" + str(assertions),
            *["-I" + str(ROOT / path) for path in INCLUDES]]


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--tools", type=Path, default=TOOLS)
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--jobs", choices=range(1, 9), type=int, default=4)
    args = parser.parse_args()
    suffix = ".exe" if os.name == "nt" else ""
    compiler = args.tools / ("arm-none-eabi-gcc" + suffix)
    size_tool = args.tools / ("arm-none-eabi-size" + suffix)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    out = args.out_dir.resolve() if args.out_dir else HERE / "evidence" / stamp
    if out.exists() or HERE not in out.parents:
        raise SystemExit("Output must be new and inside strict directory")
    out.mkdir(parents=True)
    build = HERE / ".build" / stamp
    build.mkdir(parents=True)
    sources = sorted(path for path in (ROOT / "RTOS").rglob("*.c")
                     if "lib" not in path.relative_to(ROOT / "RTOS").parts)
    if len(sources) != 18:
        raise SystemExit("Expected 18 owned RTOS sources")
    probe = out / "public_header.c"
    probe.write_text(PUBLIC_PROBE, encoding="utf-8")
    inputs = set(sources + [Path(__file__).resolve(), probe])
    for directory in INCLUDES:
        inputs.update((ROOT / directory).rglob("*.h"))
    before = {path.relative_to(ROOT).as_posix(): digest(path) for path in sorted(inputs)}
    write_json(out / "sources.json", before)
    code, version = execute([str(compiler), "--version"])
    (out / "compiler.txt").write_text(version, encoding="utf-8")
    if code:
        raise SystemExit("Compiler launch failed: " + version)
    plans = []
    for enabled, basepri, assertions, optimization in itertools.product(
            (0, 1), (0, 1), (0, 1), ("O0", "O2", "O3")):
        for source in sources:
            tag = f"m4_d{enabled}_b{basepri}_a{assertions}_{optimization}_{source.stem}"
            target = build / (tag + ".o")
            command = [str(compiler), *flags("m4", enabled, basepri, assertions, optimization),
                       "-ffunction-sections", "-fdata-sections", "-fstack-usage",
                       "-c", str(source), "-o", str(target)]
            plans.append({"tag": tag, "suite": "m4_matrix", "command": command,
                          "source": source.relative_to(ROOT).as_posix(), "object": str(target)})
    for source in sources:
        tag = "m7_d1_b1_a1_O2_" + source.stem
        target = build / (tag + ".o")
        plans.append({"tag": tag, "suite": "m7_codegen_only",
                      "command": [str(compiler), *flags("m7", 1, 1, 1, "O2"),
                                  "-ffunction-sections", "-fdata-sections", "-fstack-usage",
                                  "-c", str(source), "-o", str(target)],
                      "source": source.relative_to(ROOT).as_posix(), "object": str(target)})
    for standard, enabled in itertools.product(("c11", "c++17"), (0, 1)):
        plans.append({"tag": f"public_{standard}_d{enabled}", "suite": "public_header",
                      "command": [str(compiler), *flags("m4", enabled, 1, 1, "O2", standard),
                                  "-fsyntax-only", "-x", "c++" if standard == "c++17" else "c",
                                  str(probe)]})
    write_json(out / "commands.json", plans)

    def run_plan(plan):
        code, diagnostic = execute(plan["command"])
        result = dict(plan, exit_code=code, diagnostics=diagnostic,
                      passed=code == 0 and not re.search(r"\b(?:warning|error):", diagnostic))
        if "object" in plan:
            target = Path(plan["object"])
            stack = target.with_suffix(".su")
            if not target.is_file() or not stack.is_file():
                result["passed"] = False
            else:
                result["object_sha256"] = digest(target)
                size_code, size = execute([str(size_tool), str(target)])
                result["size_output"] = size
                result["stack_usage"] = stack.read_text(encoding="utf-8")
                if size_code:
                    result["passed"] = False
            for generated in (target, stack):
                if build.resolve() not in generated.resolve().parents:
                    raise RuntimeError("Generated path escaped private build directory")
                if generated.is_file():
                    generated.unlink()
        return result

    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        for index, result in enumerate(executor.map(run_plan, plans), 1):
            results.append(result)
            if not result["passed"] or index % 36 == 0 or index == len(plans):
                print(f"STRICT {index}/{len(plans)} {result['tag']} "
                      + ("PASS" if result["passed"] else "FAIL"), flush=True)
    write_json(out / "results.json", results)
    (out / "compile.log").write_text("\n".join(
        f"[{row['tag']}] exit={row['exit_code']} passed={row['passed']}\n{row['diagnostics']}"
        for row in results), encoding="utf-8")
    after = {path.relative_to(ROOT).as_posix(): digest(path) for path in sorted(inputs)}
    write_json(out / "sources_after.json", after)
    changed = [name for name in before if before[name] != after[name]]
    passed = sum(bool(row["passed"]) for row in results)
    summary = {"status": "PASS" if passed == 454 and not changed else "FAIL",
               "completed_utc": datetime.now(timezone.utc).isoformat(),
               "translation_units": len(sources), "checks": len(plans),
               "passed": passed, "failed": len(plans) - passed,
               "suites": {suite: sum(row["suite"] == suite for row in plans)
                          for suite in ("m4_matrix", "m7_codegen_only", "public_header")},
               "source_identity_stable": not changed, "changed_inputs": changed,
               "limits": ["Compile only; no link/runtime proof",
                          "M7 retains F446 BSP headers; no H7 board/cache/DMA validation",
                          "Per-object sizes are not linked firmware sizes"]}
    write_json(out / "summary.json", summary)
    print(f"STRICT SUMMARY {summary['status']} pass={passed} fail={len(plans) - passed}")
    print("Evidence: " + str(out))
    raise SystemExit(0 if summary["status"] == "PASS" else 1)


if __name__ == "__main__":
    main()
