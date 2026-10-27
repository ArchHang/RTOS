"""Compile the actual 17 owned RTOS units and the channel public examples.

This produces compile evidence only. It never links, flashes, uses a serial port,
or edits production sources. The M7 rows retain the F446 BSP and prove only code
generation, not STM32H7 integration or cache/DMA correctness.
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
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_TOOLS = Path(
    r"E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins"
    r"\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32"
    r"_1.1.100.202309141235\tools\bin")
INCLUDE_DIRS = (
    "Core/Inc", "AI/output/int8", "RTOS/Core/Inc", "RTOS/Core/Private",
    "Drivers/STM32F4xx_HAL_Driver/Inc", "Drivers/STM32F4xx_HAL_Driver/Inc/Legacy",
    "Drivers/CMSIS/Device/ST/STM32F4xx/Include", "Drivers/CMSIS/Include",
    "RTOS/lib/third/CherryUSB/core", "RTOS/lib/third/CherryUSB/common",
    "RTOS/lib/third/CherryUSB/class/cdc", "RTOS/lib/third/CherryUSB/class/hid",
    "RTOS/lib/third/CherryUSB/class/msc", "RTOS/lib/third/CherryUSB/port/dwc2",
)
PUBLIC_PROBE = """#include "rtos_channel.h"
int channel_public_probe(void)
{
    rtos_channel_t channel;
    rtos_channel_slot_t slots[1];
    rtos_channel_writer_t writer;
    rtos_channel_reader_t reader;
    rtos_channel_stats_t stats;
    unsigned char storage[8];
    rtos_channel_config_t config;
    config.storage = storage;
    config.storage_size = sizeof(storage);
    config.slots = slots;
    config.slots_size = sizeof(slots);
    config.block_size = 8U;
    config.block_count = 1U;
    (void)rtos_channel_init(&channel, &config);
    (void)rtos_channel_acquire(&channel, &writer, RTOS_NO_WAIT);
    (void)rtos_channel_publish(&channel, &writer, 0U, 0U);
    (void)rtos_channel_cancel(&channel, &writer);
    (void)rtos_channel_receive(&channel, &reader, RTOS_NO_WAIT);
    (void)rtos_channel_release(&channel, &reader);
    (void)rtos_channel_get_stats(&channel, &stats);
    (void)rtos_channel_close(&channel);
    return (int)rtos_channel_deinit(&channel);
}
"""


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def execute(command):
    started = time.monotonic()
    try:
        process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=120)
        return process.returncode, process.stdout + process.stderr, time.monotonic() - started
    except (OSError, subprocess.TimeoutExpired) as error:
        return -1, repr(error), time.monotonic() - started


def flags(cpu, enabled, basepri, assertions, optimization, standard="gnu11"):
    fpu = "fpv4-sp-d16" if cpu == "m4" else "fpv5-d16"
    return ["-mcpu=cortex-" + cpu, "-mthumb", "-mfpu=" + fpu, "-mfloat-abi=hard",
            "-std=" + standard, "-Wall", "-Wextra", "-Werror", "-" + optimization,
            "-DDEBUG", "-DUSE_HAL_DRIVER", "-DSTM32F446xx",
            "-DRTOS_CONFIG_USE_CHANNEL=" + str(enabled),
            "-DRTOS_CONFIG_USE_BASEPRI=" + str(basepri),
            "-DRTOS_CONFIG_ASSERT_ENABLED=" + str(assertions),
            *["-I" + str(ROOT / path) for path in INCLUDE_DIRS]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools", type=Path, default=DEFAULT_TOOLS)
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--jobs", type=int, choices=range(1, 9), default=4)
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--skip-documentation", action="store_true",
                       help="Run 429 checks without reading or hashing the mutable Markdown file")
    scope.add_argument("--documentation-only", action="store_true",
                       help="Run just the frozen Markdown C11 example as a separate one-check batch")
    options = parser.parse_args()
    suffix = ".exe" if os.name == "nt" else ""
    compiler = options.tools / ("arm-none-eabi-gcc" + suffix)
    size_tool = options.tools / ("arm-none-eabi-size" + suffix)
    if not compiler.is_file() or not size_tool.is_file():
        raise SystemExit("Missing ARM compiler or size tool: " + str(options.tools))

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    output = options.out_dir.resolve() if options.out_dir else HERE / "evidence" / stamp
    if HERE not in output.parents or output.exists():
        raise SystemExit("Output must be new and inside the strict directory: " + str(output))
    output.mkdir(parents=True)
    build = HERE / ".build" / stamp
    build.mkdir(parents=True)
    probes = output / "probes"
    probes.mkdir()

    sources = sorted(path for path in (ROOT / "RTOS").rglob("*.c")
                     if "lib" not in path.relative_to(ROOT / "RTOS").parts)
    if len(sources) != 17:
        raise SystemExit("Expected the explicit 17 owned RTOS units; found " + str(len(sources)))
    document = ROOT / "docs/rtos_channel_design.md"
    example_file = probes / "documentation_example.c"
    public_file = probes / "public_header.c"
    if not options.skip_documentation:
        blocks = re.findall(r"```c\n(.*?)\n```", document.read_text(encoding="utf-8"), re.S)
        example = next(block for block in blocks if '#include "rtos_channel.h"' in block)
        example_file.write_text(example + "\n", encoding="utf-8")
    if not options.documentation_only:
        public_file.write_text(PUBLIC_PROBE, encoding="utf-8")

    inputs = set(sources + [Path(__file__).resolve()])
    if not options.skip_documentation:
        inputs.update((document, example_file))
    if not options.documentation_only:
        inputs.add(public_file)
    for directory in INCLUDE_DIRS:
        inputs.update((ROOT / directory).rglob("*.h"))
    identities = {path.relative_to(ROOT).as_posix(): digest(path) for path in sorted(inputs)}
    write_json(output / "sources.json", identities)
    code, version, _ = execute([str(compiler), "--version"])
    (output / "compiler.txt").write_text(version, encoding="utf-8")
    if code:
        write_json(output / "summary.json", {"status": "FAIL", "preflight": version})
        raise SystemExit("Compiler launch failed; see " + str(output))

    plans = []
    configurations = [] if options.documentation_only else itertools.product(
        (0, 1), (0, 1), (0, 1), ("O0", "O2", "O3"))
    for enabled, basepri, assertions, optimization in configurations:
        for source in sources:
            tag = f"m4_e{enabled}_b{basepri}_a{assertions}_{optimization}_{source.stem}"
            target = build / (tag + ".o")
            command = [str(compiler), *flags("m4", enabled, basepri, assertions, optimization),
                       "-ffunction-sections", "-fdata-sections", "-fstack-usage",
                       "-c", str(source), "-o", str(target)]
            plans.append({"tag": tag, "suite": "m4_matrix", "command": command,
                          "source": source.relative_to(ROOT).as_posix(), "object": str(target)})
    for source in ([] if options.documentation_only else sources):
        tag = "m7_e1_b1_a1_O2_" + source.stem
        target = build / (tag + ".o")
        command = [str(compiler), *flags("m7", 1, 1, 1, "O2"),
                   "-ffunction-sections", "-fdata-sections", "-fstack-usage",
                   "-c", str(source), "-o", str(target)]
        plans.append({"tag": tag, "suite": "m7_codegen_only", "command": command,
                      "source": source.relative_to(ROOT).as_posix(), "object": str(target)})
    if not options.skip_documentation:
        plans.append({"tag": "documentation_c11", "suite": "documentation",
                      "command": [str(compiler), *flags("m4", 1, 1, 1, "O2", "c11"),
                                  "-fsyntax-only", "-x", "c", str(example_file)]})
    standards = [] if options.documentation_only else itertools.product(("c11", "c++17"), (0, 1))
    for standard, enabled in standards:
        # A C11 example may use C-only syntax; only the public header is C++17 tested.
        plans.append({"tag": f"public_{standard}_e{enabled}", "suite": "public_header",
                      "command": [str(compiler), *flags("m4", enabled, 1, 1, "O2", standard),
                                  "-fsyntax-only", "-x", "c++" if standard == "c++17" else "c",
                                  str(public_file)]})
    write_json(output / "commands.json", plans)

    def run_plan(plan):
        result = dict(plan)
        code, diagnostics, elapsed = execute(plan["command"])
        result.update(exit_code=code, seconds=round(elapsed, 4), diagnostics=diagnostics)
        result["passed"] = code == 0 and not re.search(r"\b(?:warning|error):", diagnostics, re.I)
        result["stack_usage"] = ""
        if "object" in plan:
            target = Path(plan["object"])
            if target.is_file():
                result["object_sha256"] = digest(target)
                size_code, size_output, _ = execute([str(size_tool), str(target)])
                result["size_command"] = [str(size_tool), str(target)]
                result["size_output"] = size_output
                found = re.search(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+", size_output, re.M)
                if size_code or not found:
                    result["passed"] = False
                else:
                    result["size"] = dict(zip(("text", "data", "bss", "decimal"),
                                              map(int, found.groups())))
            elif result["passed"]:
                result["passed"] = False
                result["diagnostics"] += "\nMissing compiled object.\n"
            stack = target.with_suffix(".su")
            if stack.is_file():
                result["stack_usage"] = stack.read_text(encoding="utf-8", errors="replace")
            elif result["passed"]:
                result["passed"] = False
                result["diagnostics"] += "\nMissing GCC stack-usage output.\n"
            # Delete only generated direct files after collecting hashes/sizes/SU.
            for generated in (target, stack):
                if build.resolve() not in generated.resolve().parents:
                    raise RuntimeError("Generated path escaped private build directory")
                if generated.is_file():
                    generated.unlink()
        return result

    results = []
    with ThreadPoolExecutor(max_workers=options.jobs) as executor:
        for index, result in enumerate(executor.map(run_plan, plans), 1):
            results.append(result)
            if not result["passed"] or index % 34 == 0 or index == len(plans):
                print(f"STRICT {index}/{len(plans)} {result['tag']} "
                      + ("PASS" if result["passed"] else "FAIL"), flush=True)
    (output / "compile.log").write_text("\n".join(
        f"[{row['tag']}] exit={row['exit_code']} passed={row['passed']}\n{row['diagnostics']}"
        for row in results), encoding="utf-8")
    (output / "stack_usage.su").write_text("\n".join(
        "# " + row["tag"] + "\n" + row["stack_usage"] for row in results if "object" in row),
        encoding="utf-8")
    (output / "sizes.tsv").write_text("case\ttext\tdata\tbss\tdecimal\n" + "".join(
        row["tag"] + "\t" + "\t".join(str(row["size"][key]) for key in
        ("text", "data", "bss", "decimal")) + "\n" for row in results if "size" in row),
        encoding="utf-8")
    for row in results:
        row.pop("stack_usage")
    write_json(output / "results.json", results)
    after = {path.relative_to(ROOT).as_posix(): digest(path) for path in sorted(inputs)}
    changed = sorted(key for key in identities if identities[key] != after[key])
    write_json(output / "sources_after.json", after)
    passed = sum(bool(row["passed"]) for row in results)
    expected = 1 if options.documentation_only else (429 if options.skip_documentation else 430)
    summary = {"status": "PASS" if passed == expected and not changed else "FAIL",
               "completed_utc": datetime.now(timezone.utc).isoformat(),
               "compiler": str(compiler), "translation_units": len(sources),
               "scope": "documentation_only" if options.documentation_only else
                        ("without_documentation" if options.skip_documentation else "full"),
               "expected_checks": expected,
               "checks": len(plans), "passed": passed, "failed": len(plans) - passed,
               "suites": {suite: sum(row["suite"] == suite for row in plans) for suite in
                          ("m4_matrix", "m7_codegen_only", "documentation", "public_header")},
               "source_identity_stable": not changed, "changed_inputs": changed,
               "limitations": ["Compilation only; no link or runtime validation",
                               "M7 code generation retains F446 headers; no H7 BSP proof",
                               "No M7 DMA/cache/MPU or hardware evidence",
                               "Size totals describe separate objects, not linked firmware"]}
    write_json(output / "summary.json", summary)
    print("Evidence: " + str(output), flush=True)
    print(f"STRICT SUMMARY {summary['status']} pass={passed} fail={len(plans) - passed}", flush=True)
    raise SystemExit(0 if summary["status"] == "PASS" else 1)


if __name__ == "__main__":
    main()
