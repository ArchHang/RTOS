"""Compare default-OFF generated code/state with the pre-feature 17-unit baseline.

Assertions are disabled for this exact byte comparison: enabling assertions
embeds __LINE__ values, which necessarily change when observation points add
source lines. This check compares allocated ET_REL contents plus their relocation
targets, not DWARF/file identity, and never links or accesses hardware.
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from strict.run_strict import INCLUDES, TOOLS

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def allocated_object(path):
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("Expected ELF32 little-endian object")
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    sections = [struct.unpack_from("<10I", data, header[5] + index * header[10])
                for index in range(header[11])]
    names_section = sections[header[12]]
    names = data[names_section[4]:names_section[4] + names_section[5]]

    def string(table, offset):
        return table[offset:].split(b"\0", 1)[0].decode("utf-8")

    section_names = [string(names, section[0]) for section in sections]
    result = {}
    for index, section in enumerate(sections):
        if not section[2] & 2:
            continue
        content = None if section[1] == 8 else hashlib.sha256(
            data[section[4]:section[4] + section[5]]).hexdigest()
        result[section_names[index]] = {"type": section[1], "flags": section[2],
            "bytes": section[5], "alignment": section[8], "content": content,
            "relocations": []}
    for section in sections:
        if section[1] != 9 or section_names[section[7]] not in result:  # SHT_REL
            continue
        symbols = sections[section[6]]
        strings_section = sections[symbols[6]]
        strings = data[strings_section[4]:strings_section[4] + strings_section[5]]
        relocations = result[section_names[section[7]]]["relocations"]
        for offset in range(section[4], section[4] + section[5], section[9]):
            address, info = struct.unpack_from("<II", data, offset)
            symbol = struct.unpack_from("<IIIBBH", data,
                symbols[4] + (info >> 8) * symbols[9])
            name = string(strings, symbol[0])
            target_section = section_names[symbol[5]] if symbol[5] < len(sections) else symbol[5]
            relocations.append({"offset": address, "type": info & 255, "symbol": name,
                                "value": symbol[1], "target_section": target_section})
    return result


def run(command, output):
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, encoding="utf-8", errors="replace",
                            timeout=120)
    output.write_text(result.stdout, encoding="utf-8")
    if result.returncode or "warning:" in result.stdout or "error:" in result.stdout:
        raise RuntimeError("Strict compiler/tool failure: " + str(output))


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--tools", type=Path, default=TOOLS)
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    out = args.out_dir.resolve() if args.out_dir else HERE / "equivalence" / stamp
    if HERE not in out.parents or out.exists():
        raise SystemExit("Output must be new and inside this batch")
    out.mkdir(parents=True)
    build = HERE / ".build" / stamp
    build.mkdir(parents=True)
    baseline_root = HERE / "baseline"
    baseline = sorted(path for path in (baseline_root / "RTOS").rglob("*.c")
                      if "lib" not in path.relative_to(baseline_root / "RTOS").parts)
    if len(baseline) != 17:
        raise SystemExit("Expected original 17 owned baseline units")
    compiler = args.tools / "arm-none-eabi-gcc.exe"
    inputs = set(baseline + [ROOT / path.relative_to(baseline_root) for path in baseline])
    inputs.update((ROOT / "RTOS").rglob("*.h"))
    inputs.update((Path(__file__).resolve(), HERE / "strict/run_strict.py",
                   ROOT / "RTOS/Services/rtos_diag.c"))
    before = {path.relative_to(ROOT).as_posix(): digest(path) for path in sorted(inputs)}
    (out / "sources.json").write_text(json.dumps(before, indent=2) + "\n", encoding="utf-8")
    common = ["-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard",
              "-std=gnu11", "-O2", "-g3", "-Wall", "-Wextra", "-Werror", "-DDEBUG",
              "-DUSE_HAL_DRIVER", "-DSTM32F446xx", "-DRTOS_CONFIG_USE_DIAGNOSTICS=0",
              "-DRTOS_CONFIG_ASSERT_ENABLED=0", "-DRTOS_CONFIG_USE_CHANNEL=1",
              "-ffunction-sections", "-fdata-sections",
              *["-I" + str(ROOT / directory) for directory in INCLUDES]]
    rows, commands = [], []
    for old in baseline:
        relative = old.relative_to(baseline_root)
        objects = []
        for label, source in (("before", old), ("after", ROOT / relative)):
            target = build / (source.stem + "_" + label + ".o")
            command = [str(compiler), *common, "-c", str(source), "-o", str(target)]
            commands.append(command)
            run(command, out / (source.stem + "_" + label + ".txt"))
            objects.append(allocated_object(target))
        identical = objects[0] == objects[1]
        rows.append({"source": relative.as_posix(), "identical": identical,
                     "before": objects[0], "after": objects[1]})
        print(relative.as_posix() + ": " + ("IDENTICAL" if identical else "DIFFERENT"), flush=True)
    target = build / "rtos_diag_disabled.o"
    command = [str(compiler), *common, "-c", str(ROOT / "RTOS/Services/rtos_diag.c"),
               "-o", str(target)]
    commands.append(command)
    run(command, out / "diagnostic_disabled_compile.txt")
    nm = args.tools / "arm-none-eabi-nm.exe"
    run([str(nm), "-P", str(target)], out / "diagnostic_disabled_nm.txt")
    disabled_sections = allocated_object(target)
    disabled_no_state = not any(value["bytes"] != 0 and name.startswith((".bss", ".data"))
                                for name, value in disabled_sections.items())
    after = {path.relative_to(ROOT).as_posix(): digest(path) for path in sorted(inputs)}
    passed = all(row["identical"] for row in rows) and disabled_no_state and before == after
    report = {"status": "PASS" if passed else "FAIL", "translation_units": 17,
              "identical_units": sum(row["identical"] for row in rows),
              "source_identity_stable": before == after,
              "configuration": {"diagnostics": 0, "channel": 1, "assertions": 0,
                                "optimization": "O2", "cpu": "Cortex-M4F"},
              "disabled_module_no_static_state": disabled_no_state,
              "disabled_module_allocated_sections": disabled_sections, "results": rows,
              "scope": "All allocated ET_REL section contents/layout and relocation targets",
              "limits": ["Not a linked image or hardware timing result",
                         "Assertions disabled to exclude changed __LINE__ constants",
                         "Disabled API stubs can link if an application explicitly calls them"]}
    (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n", encoding="utf-8")
    (out / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Evidence: " + str(out))
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
