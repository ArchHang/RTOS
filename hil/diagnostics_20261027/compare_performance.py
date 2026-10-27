"""Compare four completed, independently accepted diagnostic ON/OFF captures.

Keeps every measured sample and every positive/negative delta. The measurements
describe complete exchanges, including the loop and scheduler/IRQ interference;
they are neither an individual API instruction count nor a WCET measurement.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import statistics
import struct
import subprocess

from strict.run_strict import TOOLS
from verify_results import read, require, rows, verify_capture

HERE = Path(__file__).resolve().parent
NAMES = ("sem_nowait", "queue_nowait", "notify_nowait", "sem_pingpong",
         "queue_pingpong", "notify_pingpong")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elf_details(path):
    data = path.read_bytes()
    require(data[:6] == b"\x7fELF\x01\x01", "ELF32 little-endian required")
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    sections = [struct.unpack_from("<10I", data, header[5] + index * header[10])
                for index in range(header[11])]
    names_section = sections[header[12]]
    names = data[names_section[4]:names_section[4] + names_section[5]]

    def string(table, offset):
        return table[offset:].split(b"\0", 1)[0].decode("utf-8")

    section_names = [string(names, section[0]) for section in sections]
    allocated = {section_names[index]: {"type": section[1], "flags": section[2],
        "address": section[3], "bytes": section[5], "alignment": section[8],
        "stored_in_file": section[1] != 8} for index, section in enumerate(sections)
        if section[2] & 2}
    symbols = []
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB, retained in the saved debug ELF
            continue
        strings_section = sections[section[6]]
        strings = data[strings_section[4]:strings_section[4] + strings_section[5]]
        for offset in range(section[4], section[4] + section[5], section[9]):
            symbol = struct.unpack_from("<IIIBBH", data, offset)
            name = string(strings, symbol[0])
            if not name.startswith(("rtos_diag_", "diag_", "rtos_internal_diag_")):
                continue
            symbols.append({"name": name, "address": symbol[1], "bytes": symbol[2],
                "type": symbol[3] & 15, "binding": symbol[3] >> 4,
                "section": section_names[symbol[5]] if symbol[5] < len(sections) else None})
    # Aliased symbols must not be counted twice. Literal pools, alignment and
    # observation call sites elsewhere are not a module's defined symbols.
    functions = {(symbol["section"], symbol["address"], symbol["bytes"])
                 for symbol in symbols if symbol["type"] == 2 and symbol["section"]}
    objects = {(symbol["section"], symbol["address"], symbol["bytes"])
               for symbol in symbols if symbol["type"] == 1 and symbol["section"]}
    return {"allocated_sections": allocated,
        "bss_data_bytes": sum(allocated.get(name, {}).get("bytes", 0) for name in (".bss", ".data")),
        "allocated_nobits_bytes": sum(section["bytes"] for section in allocated.values()
                                       if section["type"] == 8),
        "allocated_file_bytes": sum(section["bytes"] for section in allocated.values()
                                     if section["stored_in_file"]),
        "writable_allocated_bytes": sum(section["bytes"] for section in allocated.values()
                                         if section["flags"] & 1),
        "diagnostic_defined_function_bytes": sum(item[2] for item in functions),
        "diagnostic_defined_object_bytes": sum(item[2] for item in objects),
        "diagnostic_symbols": symbols}


def load_capture(directory, expected_tag):
    meta = json.loads(read(directory / "identity.json"))
    require(meta["passed"] and meta["stage"] == "complete", "capture must be complete: " + expected_tag)
    require(meta["tag"] == expected_tag and meta["variant"] == "diagnostic_bench",
            "expected benchmark capture identity")
    for name, expected in meta["artifacts"].items():
        require(digest(directory / name) == expected, "changed capture artifact: " + name)
    acceptance = verify_capture(directory)
    require(acceptance["pass"] == 72 and acceptance["expected_fail"] == 0,
            "all benchmark correctness contracts")
    text = read(directory / ("log_" + expected_tag + ".txt"))
    samples = rows(text, "DG|BENCH|")
    values = {}
    for name in NAMES:
        selected = sorted((sample for sample in samples if sample["name"] == name),
                          key=lambda sample: int(sample["round"]))
        require([int(sample["round"]) for sample in selected] == [0, 1, 2],
                "exact three rounds per path")
        raw = [{key: int(value) for key, value in sample.items() if key != "name"}
               for sample in selected]
        cycles = [sample["cycles"] / sample["operations"] for sample in raw]
        values[name] = {"raw_samples": raw, "cycles_per_exchange": cycles,
                        "min": min(cycles), "median": statistics.median(cycles), "max": max(cycles)}
    elf = directory / ("firmware_" + expected_tag + ".elf")
    return {"identity": meta, "acceptance": acceptance, "measurements": values,
            "sources": json.loads(read(directory / "sources.json")), "elf": elf,
            "elf_details": elf_details(elf), "directory": directory}


def tool_output(command, destination):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            encoding="utf-8", errors="replace", timeout=60)
    destination.write_text(result.stdout, encoding="utf-8")
    require(result.returncode == 0, "ARM ELF inspection failed: " + destination.name)
    return {"command": command, "exit_code": result.returncode,
            "output": destination.name, "output_sha256": digest(destination)}


def markdown(report):
    lines = ["# 诊断 ON/OFF 实测比较", "", "每格为三次等工作量测量的 cycles/完整交换；"
             "交换含 API、循环和实际调度/中断干扰。保留全部原始样本，不作为 WCET。", "",
             "| 优化 | 路径 | OFF min / median / max | ON min / median / max | 中位数增加 | 增幅 |",
             "| --- | --- | ---: | ---: | ---: | ---: |"]
    for comparison in report["comparisons"]:
        for name in NAMES:
            row = comparison["paths"][name]
            render = lambda item: " / ".join(f'{item[key]:.2f}' for key in ("min", "median", "max"))
            lines.append(f'| {comparison["optimization"]} | {name} | {render(row["off"])} | '
                         f'{render(row["on"])} | {row["median_added_cycles"]:+.2f} | '
                         f'{row["median_added_percent"]:+.2f}% |')
    lines.extend(["", "| 优化 | 指标 | OFF bytes | ON bytes | 差值 bytes |",
                  "| --- | --- | ---: | ---: | ---: |"])
    for comparison in report["comparisons"]:
        for metric, values in comparison["memory"].items():
            lines.append(f'| {comparison["optimization"]} | {metric} | {values["off"]} | '
                         f'{values["on"]} | {values["added"]:+d} |')
    lines.extend(["", "`.bss + .data` 为链接后静态数据，NOBITS 另含链接脚本预留区；"
                  "两者不能相加。defined symbol 数量只归属诊断模块自身，"
                  "不含其他内核函数新增调用点或所有对齐，不能当作完整 Flash 增量。", "",
                  "四个 ELF 的原始 `arm-none-eabi-size -A` 与 `nm -S` 输出保存在 tools/。"
                  "完整样本、配置及 ELF 节/符号信息见同目录 report.json。", ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--captures", "--directory", dest="captures", type=Path,
                        default=HERE / "evidence/performance")
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--tools", type=Path, default=TOOLS)
    args = parser.parse_args()
    captures = {}
    for identity_path in sorted(args.captures.rglob("identity.json")):
        meta = json.loads(read(identity_path))
        if (meta.get("variant") != "diagnostic_bench" or not meta.get("passed")
                or meta.get("stage") != "complete"):
            continue
        require(meta["optimization"] in ("O2", "O3") and meta["diagnostics"] in (0, 1),
                "only O2/O3 ON/OFF diagnostic benchmarks in this analysis directory")
        key = meta["optimization"] + ("_on" if meta["diagnostics"] else "_off")
        require(key not in captures, "one completed capture per configuration: " + key)
        captures[key] = load_capture(identity_path.parent, meta["tag"])
    require(set(captures) == {f"{optimization}_{mode}" for optimization in ("O2", "O3")
                             for mode in ("off", "on")}, "all four completed benchmark captures")
    for optimization in ("O2", "O3"):
        off, on = (captures[optimization + "_" + mode] for mode in ("off", "on"))
        require(off["sources"] == on["sources"], "same source closure in matched ON/OFF pair")
        for key in ("mask", "assertions", "optimization", "channels", "capacity", "port"):
            require(off["identity"][key] == on["identity"][key], "matched configuration: " + key)
        require(off["identity"]["diagnostics"] == 0 and on["identity"]["diagnostics"] == 1,
                "opposite diagnostic switches in each pair")
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    out = args.out_dir.resolve() if args.out_dir else HERE / "performance_analysis" / stamp
    require(HERE in out.parents and not out.exists(), "new output inside this batch required")
    (out / "tools").mkdir(parents=True)
    inspections = {}
    comparisons = []
    for key, capture in captures.items():
        inspections[key] = [tool_output([str(args.tools / tool), *options, str(capture["elf"])],
            out / "tools" / f"{key}_{label}.txt") for tool, options, label in (
                ("arm-none-eabi-size.exe", ["-A"], "size"),
                ("arm-none-eabi-nm.exe", ["-S", "--size-sort", "--radix=d"], "nm"))]
    for optimization in ("O2", "O3"):
        off, on = (captures[optimization + "_" + mode] for mode in ("off", "on"))
        comparison = {"optimization": optimization, "paths": {}, "memory": {}}
        for name in NAMES:
            old, new = off["measurements"][name], on["measurements"][name]
            delta = new["median"] - old["median"]
            comparison["paths"][name] = {"off": old, "on": new,
                "median_added_cycles": delta, "median_added_percent": delta / old["median"] * 100}
        for metric in ("bss_data_bytes", "allocated_nobits_bytes", "allocated_file_bytes",
                       "writable_allocated_bytes", "diagnostic_defined_function_bytes",
                       "diagnostic_defined_object_bytes"):
            old, new = off["elf_details"][metric], on["elf_details"][metric]
            comparison["memory"][metric] = {"off": old, "on": new, "added": new - old}
        comparisons.append(comparison)
    report = {"status": "PASS", "created_utc": datetime.now(timezone.utc).isoformat(),
        "measurements_per_configuration": 18, "exchanges_per_sample": 1024,
        "sources_matched_within_each_pair": True, "comparisons": comparisons,
        "captures": {key: {"path": str(capture["directory"]),
            "identity_sha256": digest(capture["directory"] / "identity.json"),
            "elf_sha256": digest(capture["elf"]), "elf_details": capture["elf_details"],
            "arm_tools": inspections[key]} for key, capture in captures.items()},
        "analysis_sources": {Path(__file__).name: digest(Path(__file__)),
                             "verify_results.py": digest(HERE / "verify_results.py")},
        "limits": ["Three samples per path; no statistical confidence or WCET guarantee",
                   "Samples include loop, scheduler and SysTick interference",
                   "Symbols do not fully attribute inline call-site or alignment cost",
                   "ON/OFF performance does not itself prove an absence of all bugs"]}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (out / "report.md").write_text(markdown(report), encoding="utf-8")
    print(markdown(report))
    print("Evidence: " + str(out))


if __name__ == "__main__":
    main()
