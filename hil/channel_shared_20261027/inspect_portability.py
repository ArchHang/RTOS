"""Check the shared-memory channel's direct dependencies without touching hardware.

This is a static boundary audit, not proof that the ARM port works on another
architecture. Comments about DMA are documentation, not executable dependencies.
The Cortex-M port deliberately remains architecture-specific.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BASELINE = HERE / "baseline"
IMPLEMENTATION = "RTOS/IPC/rtos_channel.c"
PUBLIC_HEADER = "RTOS/Core/Inc/rtos_channel.h"
LEXICAL_PARTS = re.compile(
    r'("(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\')|(/\*.*?\*/|//[^\r\n]*)',
    re.DOTALL,
)
HARDWARE_TOKEN = re.compile(
    r"\b(?:DMA\w*|HAL_\w*|LL_\w*|SCB|NVIC|DWT|SysTick|RCC|TIM\d+|"
    r"GPIO\w*|__asm\w*|__get_\w*|__set_\w*)\b"
)
SDK_INCLUDE = re.compile(r"stm32|cmsis|hal_driver|stm32cube|board", re.IGNORECASE)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def comments_removed(text, remove_literals=False):
    def replace(match):
        literal, comment = match.groups()
        if literal is not None and not remove_literals:
            return literal
        return "".join("\n" if char == "\n" else " " for char in match.group())

    return LEXICAL_PARTS.sub(replace, text)


def audit_file(path):
    data = path.read_bytes()
    text = data.decode("utf-8-sig")
    no_comments = comments_removed(text)
    code = comments_removed(text, remove_literals=True)
    includes = re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', no_comments, re.MULTILINE)
    hardware = []
    for match in HARDWARE_TOKEN.finditer(code):
        hardware.append({"token": match.group(), "line": code.count("\n", 0, match.start()) + 1})
    dependencies = sorted(set(re.findall(
        r"\b(?:RTOS_PORT_[A-Z0-9_]+|rtos_port_[a-z0-9_]+|"
        r"rtos_internal_[a-z0-9_]+|rtos_kernel)\b", code
    )))
    return {"bytes": len(data), "sha256": sha(data), "includes": includes,
            "sdk_includes": [name for name in includes if SDK_INCLUDE.search(name)],
            "direct_hardware_tokens": hardware, "rtos_port_kernel_dependencies": dependencies,
            "memory_calls": sorted(set(re.findall(r"\b(memcpy|memmove|memset)\s*\(", code)))}


def validate_fixed_inputs(manifest):
    checks = []
    for item in manifest["sources"]:
        snapshot = HERE / item["snapshot"]
        checks.append({"kind": "immutable_baseline", "path": item["snapshot"],
                       "matches": sha(snapshot.read_bytes()) == item["sha256"]})
    for kind in ("fixed_fixtures", "build_inputs"):
        for item in manifest[kind]:
            checks.append({"kind": kind, "path": item["source"],
                           "matches": sha((ROOT / item["source"]).read_bytes()) == item["sha256"]})
    for item in manifest["sources"]:
        if item["source"].endswith(("rtos_config.h", "rtos_port.h")):
            checks.append({"kind": "unchanged_configuration_and_port", "path": item["source"],
                           "matches": sha((ROOT / item["source"]).read_bytes()) == item["sha256"]})
    return checks


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--implementation", choices=("baseline", "current"), default="current")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    manifest = json.loads((BASELINE / "manifest.json").read_text(encoding="utf-8"))
    base = BASELINE if args.implementation == "baseline" else ROOT
    reports = {relative: audit_file(base / relative) for relative in (IMPLEMENTATION, PUBLIC_HEADER)}
    fixed_inputs = validate_fixed_inputs(manifest)
    passed = all(not row["sdk_includes"] and not row["direct_hardware_tokens"]
                 for row in reports.values()) and all(row["matches"] for row in fixed_inputs)
    report = {"schema_version": 1, "implementation": args.implementation, "passed": passed,
              "scope": "direct channel C/public header; comments and string literals excluded",
              "files": reports, "fixed_input_checks": fixed_inputs,
              "limits": ["No DMA or peripheral driver is invoked by this script.",
                         "The RTOS port still uses ARM PRIMASK/BASEPRI/IPSR and NVIC priorities.",
                         "Static scanning is not a cross-architecture execution or SMP proof."]}
    text = json.dumps(report, indent=2) + "\n"
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(text)
    print(text, end="")
    if not passed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
