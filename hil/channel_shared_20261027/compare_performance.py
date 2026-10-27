"""Compare verified, unchanged-fixture baseline/optimized captures without hardware.

The baseline is a newly replayed previously verified image, not a fresh build.
Every copy/pool/loan result is retained, including regressions. Raw profile cycles
include instrumentation and IRQ entry overhead; none is represented as WCET.
"""
import argparse
from collections import defaultdict
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import statistics
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
OLD_ORACLE = ROOT / "hil/channel_20261027/verify_results.py"
CHANNEL_FILES = {"RTOS/IPC/rtos_channel.c", "RTOS/Core/Inc/rtos_channel.h"}
sys.dont_write_bytecode = True


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def load_oracle():
    spec = importlib.util.spec_from_file_location("channel_previous_oracle", OLD_ORACLE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fixture_inputs(variant, manifest):
    relevant = {"hil/fw/Src/" + variant + ".c", "hil/fw/Src/channel_fixture.c",
                "hil/fw/Inc/channel_fixture.h", "hil/fw/Inc/audit_regressions.h"}
    return {item["source"]: item["sha256"] for item in manifest["fixed_fixtures"]
            if item["source"] in relevant}


def read_capture(directory, variant, phase, manifest, oracle):
    directory = directory.resolve()
    identity = load_json(directory / "identity.json")
    require(identity["variant"] == variant, f"wrong variant: {directory}")
    require(identity["passed"] and identity["stage"] == "complete",
            f"capture has not completed verification: {directory}")
    expected_provenance = "replayed_verified_image" if phase == "baseline" else "fresh_build"
    require(identity.get("build_provenance") == expected_provenance,
            f"missing/wrong build provenance: {directory}")
    require(identity["mask"] == "basepri" and identity["assertions"] == 1
            and identity["optimization"] == "O2", f"unexpected comparison config: {directory}")
    for name, expected in identity["artifacts"].items():
        require(sha(directory / name) == expected, f"changed capture artifact: {directory / name}")
    if phase == "baseline":
        original_path = directory / "original_identity.json"
        require(sha(original_path) == identity["original_identity_sha256"],
                "changed historical replay identity")
        original = load_json(original_path)
        require(original["passed"] and original["stage"] == "complete"
                and original["built_elf_sha256"] == identity["built_elf_sha256"],
                "replay is not the original verified image")
        for name in ("sources.json", "build.txt", "build_detail.txt"):
            require(sha(directory / name) == original["artifacts"][name],
                    f"changed historical build/source record: {name}")
    oracle_result = oracle.verify_capture(directory)
    sources = load_json(directory / "sources.json")
    for relative, expected in fixture_inputs(variant, manifest).items():
        require(relative in sources and sources[relative]["sha256"] == expected,
                f"changed compiled fixture/header: {directory} {relative}")
    for item in manifest["build_inputs"]:
        relative = item["source"]
        require(relative in sources and sources[relative]["sha256"] == item["sha256"],
                f"different fixed build input: {directory} {relative}")
    for item in manifest["sources"]:
        relative = item["source"]
        if phase == "baseline" or relative not in CHANNEL_FILES:
            require(relative in sources and sources[relative]["sha256"] == item["sha256"],
                    f"source is not aligned to baseline: {directory} {relative}")
    text = (directory / f'log_{identity["tag"]}.txt').read_text(encoding="utf-8-sig")
    meta = oracle.rows(text, "CH|META|")[0]
    row_prefix = "CH|BENCH|" if variant == "channel_bench" else "CH|PROFILE|"
    rows = oracle.rows(text, row_prefix)
    ignored = []
    compiled_fixtures = fixture_inputs(variant, manifest)
    for item in manifest["fixed_fixtures"]:
        if item["source"] not in compiled_fixtures and item["source"] in sources:
            if sources[item["source"]]["sha256"] != item["sha256"]:
                ignored.append(item["source"])
    return {"directory": str(directory), "identity": identity, "sources": sources,
            "meta": meta, "rows": rows, "verification": oracle_result,
            "uncompiled_fixture_differences_from_current": ignored}


def validate_pair(baseline, optimized):
    for key in ("variant", "mask", "assertions", "optimization"):
        require(baseline["identity"][key] == optimized["identity"][key],
                f"different comparison field: {key}")
    require(baseline["meta"] == optimized["meta"], "different fixture work/timing metadata")
    changed = []
    for relative in baseline["sources"].keys() & optimized["sources"].keys():
        if not relative.startswith(("RTOS/", "Core/", "Drivers/", "AI/")):
            continue
        if baseline["sources"][relative] != optimized["sources"][relative]:
            changed.append(relative)
            require(relative in CHANNEL_FILES, f"unrelated compiled-source change: {relative}")
    require("RTOS/IPC/rtos_channel.c" in changed, "no channel algorithm change was measured")
    return sorted(changed)


def summary(values):
    return {"n": len(values), "median": statistics.median(values),
            "minimum": min(values), "maximum": max(values)}


def change(baseline, optimized):
    return {"baseline": summary(baseline), "optimized": summary(optimized),
            "optimized_minus_baseline_percent":
                (statistics.median(optimized) / statistics.median(baseline) - 1.0) * 100.0,
            "baseline_over_optimized": statistics.median(baseline) / statistics.median(optimized)}


def benchmark_cells(capture):
    result = defaultdict(list)
    counts = {}
    for row in capture["rows"]:
        key = (int(row["bytes"]), int(row["capacity"]), row["mode"], row["path"])
        messages = int(row["messages"])
        counts[key] = messages
        result[key].append(int(row["cycles"]) / messages)
    return result, counts


def compare_benchmark(baseline, optimized):
    before, before_counts = benchmark_cells(baseline)
    after, after_counts = benchmark_cells(optimized)
    require(before.keys() == after.keys() and before_counts == after_counts,
            "different benchmark shape/message counts")
    cells = []
    for key in sorted(before):
        require(len(before[key]) == len(after[key]) == 3, "three repeats per benchmark cell")
        size, capacity, work, path = key
        cells.append({"bytes": size, "capacity": capacity, "mode": work, "path": path,
                      "messages": before_counts[key], **change(before[key], after[key])})
    relative = []
    shapes = sorted({key[:3] for key in before})
    for size, capacity, work in shapes:
        prefix = (size, capacity, work)
        medians = {phase: {path: statistics.median(data[prefix + (path,)])
                           for path in ("copy", "pool", "loan")}
                   for phase, data in (("baseline", before), ("optimized", after))}
        row = {"bytes": size, "capacity": capacity, "mode": work, "median_cycles": medians}
        for control in ("copy", "pool"):
            old_ratio = medians["baseline"]["loan"] / medians["baseline"][control]
            new_ratio = medians["optimized"]["loan"] / medians["optimized"][control]
            row["loan_relative_to_" + control] = {
                "baseline_loan_over_control": old_ratio,
                "optimized_loan_over_control": new_ratio,
                "normalized_change_percent": (new_ratio / old_ratio - 1.0) * 100.0,
                "baseline_control_over_loan": 1.0 / old_ratio,
                "optimized_control_over_loan": 1.0 / new_ratio,
            }
        relative.append(row)
    return cells, relative


def profile_cells(capture):
    result = defaultdict(list)
    for row in capture["rows"]:
        key = (row["case"], row["operation"], int(row["bytes"]), int(row["waiters"]))
        result[key].append(int(row["cycles"]))
    return result


def compare_profile(baseline, optimized):
    before, after = profile_cells(baseline), profile_cells(optimized)
    require(before.keys() == after.keys(), "different profile shape/API/waiter matrix")
    cells = []
    for key in sorted(before):
        require(len(before[key]) == len(after[key]) == 8, "eight repeats per profile cell")
        scenario, operation, size, waiters = key
        cells.append({"case": scenario, "operation": operation, "bytes": size,
                      "waiters": waiters, **change(before[key], after[key])})
    return cells


def capture_description(capture, phase):
    identity = capture["identity"]
    return {"directory": capture["directory"], "tag": identity["tag"],
            "build_provenance": identity["build_provenance"],
            "fresh_build_in_this_batch": phase != "baseline",
            "elf_sha256": identity["built_elf_sha256"],
            "uncompiled_fixture_differences_from_current":
                capture["uncompiled_fixture_differences_from_current"],
            "verification": capture["verification"]}


def write_csv(path, rows, keys):
    columns = keys + ("baseline_n", "baseline_median", "baseline_min", "baseline_max",
                      "optimized_n", "optimized_median", "optimized_min", "optimized_max",
                      "optimized_minus_baseline_percent", "baseline_over_optimized")
    with path.open("x", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(columns)
        for row in rows:
            cells = [row[key] for key in keys]
            for phase in ("baseline", "optimized"):
                cells += [row[phase][key] for key in ("n", "median", "minimum", "maximum")]
            cells += [row["optimized_minus_baseline_percent"], row["baseline_over_optimized"]]
            writer.writerow(cells)


def main():
    parser = argparse.ArgumentParser(__doc__)
    for phase in ("baseline", "optimized"):
        for kind in ("bench", "profile"):
            parser.add_argument(f"--{phase}-{kind}", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), "output evidence directory already exists")
    manifest = load_json(HERE / "baseline/manifest.json")
    oracle = load_oracle()
    captures = {}
    for phase in ("baseline", "optimized"):
        for kind in ("bench", "profile"):
            path = getattr(args, phase + "_" + kind)
            captures[(phase, kind)] = read_capture(path, "channel_" + kind, phase, manifest, oracle)
    changes = {kind: validate_pair(captures[("baseline", kind)], captures[("optimized", kind)])
               for kind in ("bench", "profile")}
    bench, relative = compare_benchmark(captures[("baseline", "bench")],
                                        captures[("optimized", "bench")])
    profile = compare_profile(captures[("baseline", "profile")],
                              captures[("optimized", "profile")])
    report = {"schema_version": 1, "passed": True, "oracle_sha256": sha(OLD_ORACLE),
              "source_changes": changes,
              "captures": {phase: {kind: capture_description(captures[(phase, kind)], phase)
                                   for kind in ("bench", "profile")}
                           for phase in ("baseline", "optimized")},
              "benchmark_units": "raw DWT cycles per message; identical messages/work per cell",
              "profile_units": "observed instrumented IRQ latency cycles, not WCET",
              "benchmark_cells": bench, "loan_relative_to_controls": relative,
              "profile_cells": profile,
              "regressions": [row for row in bench if row["optimized_minus_baseline_percent"] > 0],
              "limits": ["Baseline is replayed_verified_image; its historical build is retained.",
                         "One active channel was measured; no multichannel worst-case claim.",
                         "All copy, pool and loan changes are shown; layout/IRQ noise can affect all paths.",
                         "Three benchmark repeats and eight IRQ repeats are observations, not confidence bounds.",
                         "No DMA test or DMA transfer participates in this batch."]}
    args.output.mkdir(parents=True, exist_ok=False)
    write_csv(args.output / "benchmark_changes.csv", bench,
              ("bytes", "capacity", "mode", "path", "messages"))
    write_csv(args.output / "profile_changes.csv", profile,
              ("case", "operation", "bytes", "waiters"))
    (args.output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "bench_cells": len(bench), "profile_cells": len(profile),
                      "regressions_preserved": len(report["regressions"]),
                      "output": str(args.output)}, indent=2))


if __name__ == "__main__":
    main()
