#!/usr/bin/env python3
"""Compile documentation examples using real ARM GCC; never build/flash daily FW."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
EXAMPLES = ROOT / "docs/ai/examples"
EXAMPLE_FILES = {"handbook.c", "ipc.c", "memory.c", "observation.c", "task_notify.c",
                 "timer.c", "usb.c"}
CORE_HEADERS = ROOT / "RTOS/Core/Inc"
SOURCE_DIRS = ("Core/Src", "IPC", "Memory", "Services", "USB", "Port/GCC/ARM_CM4F_CM7F")
INCLUDES = ("RTOS/Core/Inc", "RTOS/Core/Private", "Core/Inc",
            "Drivers/STM32F4xx_HAL_Driver/Inc", "Drivers/STM32F4xx_HAL_Driver/Inc/Legacy",
            "Drivers/CMSIS/Device/ST/STM32F4xx/Include", "Drivers/CMSIS/Include",
            "RTOS/lib/third/CherryUSB/core", "RTOS/lib/third/CherryUSB/common",
            "RTOS/lib/third/CherryUSB/class/cdc", "RTOS/lib/third/CherryUSB/class/hid",
            "RTOS/lib/third/CherryUSB/class/msc", "RTOS/lib/third/CherryUSB/port/dwc2")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def save(path, data):
    if path.exists():
        raise ValueError("Refusing to overwrite evidence: " + str(path))
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def parse_defines(text):
    result = {}
    for line in text.splitlines():
        match = re.fullmatch(r"#define[ \t]+([A-Za-z_]\w*)(?:[ \t]+(.*))?", line)
        if match:
            result[match.group(1)] = match.group(2) or ""
    return result


def inputs():
    paths = set()
    for name in ("RTOS", "Core", "Drivers", "AI"):
        paths.update(path for path in (ROOT / name).rglob("*")
                     if path.is_file() and path.suffix.lower() in (".c", ".h", ".s", ".a", ".ld"))
    paths.update(path for path in EXAMPLES.iterdir() if path.suffix in (".c", ".h"))
    paths.update(ROOT.glob("*.ld"))
    paths.update((ROOT / name) for name in (".project", ".cproject"))
    paths.add(Path(__file__).resolve())
    return {str(path.relative_to(ROOT)).replace("\\", "/"): digest(path)
            for path in sorted(paths)}


def identity(expected):
    actual = inputs()
    if actual != expected:
        raise ValueError("Compilation input identity changed during verification")


def profiles():
    for mask in (0, 1):
        for assertions in (0, 1):
            for optimization in ("O0", "O2", "O3"):
                yield {"tag": "base_m%d_a%d_%s" % (mask, assertions, optimization),
                       "mask": mask, "assertions": assertions, "optimization": optimization,
                       "overrides": {}}
    overrides = {
        "usb_off": {"RTOS_CONFIG_USE_USB": "0"},
        "perf_off": {"RTOS_CONFIG_USE_PERF_MONITOR": "0"},
        "optional_off": {"RTOS_CONFIG_USE_USB": "0", "RTOS_CONFIG_USE_PERF_MONITOR": "0",
                         "RTOS_CONFIG_USE_TIMERS": "0"},
        "hot_stats": {"RTOS_CONFIG_PERF_HOTPATH_STATS": "1"},
        "dynamic_tasks": {"RTOS_CONFIG_USE_STATIC_ALLOCATION": "0"},
        "tick_100hz": {"RTOS_CONFIG_TICK_RATE_HZ": "100U"},
        "usb_device_off": {"RTOS_CONFIG_USB_DEVICE_MODE": "0"},
    }
    for name, values in overrides.items():
        yield {"tag": name, "mask": 1, "assertions": 1,
               "optimization": "O2", "overrides": values}


def make_overlay(directory, overrides):
    directory.mkdir()
    for path in CORE_HEADERS.glob("*.h"):
        shutil.copyfile(str(path), str(directory / path.name))
    config = directory / "rtos_config.h"
    data = config.read_bytes().decode("utf-8")
    for name, value in overrides.items():
        pattern = r"(?m)^(#define\s+" + re.escape(name) + r"\s+)[^\r\n]+"
        data, count = re.subn(pattern, lambda match: match.group(1) + "(" + value + ")", data)
        if count != 1:
            raise ValueError("Expected exactly one configuration definition: " + name)
    config.write_bytes(data.encode("utf-8"))
    return {path.name: digest(path) for path in sorted(directory.glob("*.h"))}


def run(command, directory, tag, expected_pattern=None):
    completed = subprocess.run([str(item) for item in command], cwd=str(ROOT),
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout = completed.stdout.decode("utf-8", errors="replace")
    stderr = completed.stderr.decode("utf-8", errors="replace")
    save(directory / (tag + ".json"), {"command": [str(item) for item in command],
         "cwd": str(ROOT), "returncode": completed.returncode, "stdout": stdout, "stderr": stderr})
    if expected_pattern is None:
        if completed.returncode != 0:
            raise ValueError("Command failed: " + tag + ": " + stderr[-2000:])
    elif completed.returncode == 0 or not re.search(expected_pattern, stderr, re.IGNORECASE):
        raise ValueError("Expected a specific compiler rejection: " + tag)
    return stdout


def flags(profile, overlay=None):
    result = ["-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard",
              "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fno-common", "-fno-lto",
              "-ffunction-sections", "-fdata-sections", "-" + profile["optimization"],
              "-DSTM32F446xx", "-DUSE_HAL_DRIVER",
              "-DRTOS_CONFIG_USE_BASEPRI=" + str(profile["mask"]),
              "-DRTOS_CONFIG_ASSERT_ENABLED=" + str(profile["assertions"])]
    if overlay:
        result += ["-I" + str(overlay)]
    result += ["-I" + str(ROOT / path) for path in INCLUDES]
    result += ["-I" + str(EXAMPLES)]
    return result


def verify_example_names():
    pattern = r"\b(ai_example_[a-z0-9_]+)\s*\("
    declarations = set(re.findall(pattern, (EXAMPLES / "ai_examples.h").read_text(encoding="utf-8")))
    definitions = set()
    for path in EXAMPLES.glob("*.c"):
        definitions.update(re.findall(r"\b(ai_example_[a-z0-9_]+)\s*\([^;{}]*\)\s*\{",
                                      path.read_text(encoding="utf-8")))
    if declarations != definitions:
        raise ValueError("Example declaration/definition inventory mismatch")
    return len(declarations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    if not compiler.is_file():
        raise SystemExit("Compiler does not exist: " + str(compiler))
    nm = compiler.with_name("arm-none-eabi-nm.exe" if compiler.suffix == ".exe" else "arm-none-eabi-nm")
    if not nm.is_file():
        raise SystemExit("Matching ARM nm does not exist: " + str(nm))
    output = args.output or HERE / "evidence" / ("compile_" + datetime.now().strftime("%Y%m%d_%H%M%S_%f"))
    output = output.resolve()
    if output == ROOT or ROOT not in output.parents:
        raise SystemExit("Evidence output must be a new directory inside this project")
    if output.exists():
        raise SystemExit("Refusing to overwrite " + str(output))
    output.mkdir(parents=True)
    work = output / ".objects"
    work.mkdir()
    expected = inputs()
    save(output / "inputs.json", expected)
    examples = sorted(EXAMPLES.glob("*.c"))
    kernel = sorted(path for directory in SOURCE_DIRS for path in (ROOT / "RTOS" / directory).glob("*.c"))
    result = {"status": "RUNNING", "started_utc": datetime.now(timezone.utc).isoformat(),
              "baseline": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=str(ROOT)).decode().strip(),
              "compiler": str(compiler), "profiles": [], "example_objects": 0,
              "kernel_support_objects": 0, "header_probes": 0, "expected_rejections": 0,
              "partial_links": 0, "production_inputs_stable": False, "error": None,
              "limits": ["Compile and relocatable-link checks only; no executable firmware, no execution or hardware.",
                         "Partial links leave BSP, runtime and third-party symbols unresolved and do not prove final integration.",
                         "Overlay configurations are temporary copies, not production header changes."]}
    try:
        result["compiler_sha256"] = digest(compiler)
        result["nm_sha256"] = digest(nm)
        result["example_functions"] = verify_example_names()
        result["compiler_version"] = run([compiler, "--version"], output, "compiler_version")
        if {path.name for path in examples} != EXAMPLE_FILES or len(kernel) != 16:
            raise ValueError("Unexpected example/kernel inventory")
        for profile in profiles():
            tag = profile["tag"]
            directory = output / tag
            directory.mkdir()
            objects = work / tag
            objects.mkdir()
            overlay = None
            if profile["overrides"]:
                overlay = objects / "headers"
                profile["overlay_headers_sha256"] = make_overlay(overlay, profile["overrides"])
            common = flags(profile, overlay)
            probe = objects / "effective_config.c"
            probe.write_text('#include "rtos.h"\n', encoding="utf-8")
            defines = run([compiler, *common, "-dM", "-E", probe], directory, "effective_config")
            macros = parse_defines(defines)
            selected = dict(profile["overrides"])
            selected.update(RTOS_CONFIG_USE_BASEPRI=str(profile["mask"]),
                            RTOS_CONFIG_ASSERT_ENABLED=str(profile["assertions"]))
            for name, requested in selected.items():
                actual = macros.get(name, "").strip().strip("()").rstrip("uUlL")
                wanted = requested.strip().strip("()").rstrip("uUlL")
                if actual != wanted:
                    raise ValueError("Actual preprocessor configuration mismatch: " + name +
                                     " requested=" + repr(wanted) + " actual=" + repr(actual))
            profile["effective_config"] = {name: macros[name] for name in sorted(macros)
                                           if name.startswith("RTOS_CONFIG_")}
            built = []
            for category, paths in (("example", examples), ("kernel", kernel)):
                for index, source in enumerate(paths):
                    name = category + "_%02d_" % index + source.stem
                    target = objects / (name + ".o")
                    extra = ["-Wconversion", "-Wshadow", "-Wmissing-prototypes"] if category == "example" else []
                    run([compiler, *common, *extra, "-c", source, "-o", target], directory, name)
                    built.append(target)
                    result["example_objects" if category == "example" else "kernel_support_objects"] += 1
            partial = objects / "examples_with_kernel.o"
            run([compiler, "-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard",
                 "-nostdlib", "-r", *built, "-o", partial], directory, "partial_link")
            undefined_text = run([nm, "-u", partial], directory, "undefined_symbols")
            undefined = [line.split()[-1] for line in undefined_text.splitlines() if line.split()]
            if any(name.startswith(("rtos_", "ai_example_")) for name in undefined):
                raise ValueError("Unresolved own API/example symbols: " + ", ".join(undefined))
            result["partial_links"] += 1
            profile.update(status="PASS", example_objects=len(examples), kernel_support_objects=16,
                           partial_object_sha256=digest(partial), external_undefined_symbols=undefined)
            result["profiles"].append(profile)
            if tag == "base_m1_a1_O2":
                for header in sorted(CORE_HEADERS.glob("*.h")):
                    source = objects / ("include_" + header.stem + ".c")
                    source.write_text('#include "' + header.name + '"\n', encoding="utf-8")
                    run([compiler, *common, "-c", source, "-o", source.with_suffix(".o")],
                        directory, "header_" + header.stem)
                    result["header_probes"] += 1
                negative = objects / "incorrect_timer_arity.c"
                negative.write_text('#include "rtos_timer.h"\nvoid invalid(void) { '
                                    'rtos_timer_create((rtos_timer_t *)0); }\n', encoding="utf-8")
                run([compiler, *common, "-c", negative, "-o", negative.with_suffix(".o")],
                    directory, "negative_timer_arity", r"too few arguments.*rtos_timer_create")
                result["expected_rejections"] += 1
            if tag == "usb_off":
                negative = objects / "usb_off_missing_types.c"
                negative.write_text('#include "rtos_usb.h"\nvoid invalid(void) { '
                                    '(void)rtos_usb_init(); }\n', encoding="utf-8")
                run([compiler, *common, "-c", negative, "-o", negative.with_suffix(".o")],
                    directory, "known_usb_off_header_dependency", r"rtos_status_t.*undeclared")
                result["expected_rejections"] += 1
                positive = objects / "usb_off_with_types.c"
                positive.write_text('#include <stdbool.h>\n#include "rtos_types.h"\n'
                                    '#include "rtos_usb.h"\nvoid valid(void) { '
                                    '(void)rtos_usb_init(); (void)rtos_usb_cdc_is_connected(0U); }\n',
                                    encoding="utf-8")
                run([compiler, *common, "-c", positive, "-o", positive.with_suffix(".o")],
                    directory, "usb_off_with_required_types")
                result["header_probes"] += 1
            identity(expected)
            print("COMPILE PASS " + tag, flush=True)
        result["production_inputs_stable"] = True
        result["status"] = "PASS"
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        result["status"] = "FAIL"
        result["error"] = str(error)
        raise
    finally:
        result["finished_utc"] = datetime.now(timezone.utc).isoformat()
        # Remove only this invocation's generated object/header directory.
        if work.parent != output or output == ROOT or ROOT not in output.parents:
            result["cleanup_error"] = "Object directory is outside project-owned evidence"
        else:
            shutil.rmtree(str(work))
        save(output / "summary.json", result)
        print("Retained compilation evidence: " + str(output), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
