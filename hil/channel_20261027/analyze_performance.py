"""Aggregate verified raw DWT observations; no measurements are synthesized."""
import csv
import json
from collections import defaultdict
from pathlib import Path
import statistics

from verify_results import fields, verify_capture

HERE = Path(__file__).resolve().parent


def main():
    samples = defaultdict(list)
    profile = defaultdict(list)
    capture_names = []
    for identity in sorted((HERE / "evidence/matrix").rglob("identity.json")):
        obj = json.loads(identity.read_text())
        if obj["variant"] not in ("channel_bench", "channel_profile"):
            continue
        verify_capture(identity.parent)
        capture_names.append(obj["tag"])
        for line in (identity.parent / f'log_{obj["tag"]}.txt').read_text().splitlines():
            row = fields(line)
            if line.startswith("CH|BENCH|"):
                key = (obj["optimization"], int(row["bytes"]), int(row["capacity"]),
                       row["mode"], row["path"])
                samples[key].append(int(row["cycles"]) / int(row["messages"]))
            elif line.startswith("CH|PROFILE|"):
                key = (obj["mask"], row["case"], row["operation"], int(row["bytes"]),
                       int(row["waiters"]))
                profile[key].append(int(row["cycles"]))
    output = HERE / "evidence/analysis"
    output.mkdir(exist_ok=True)
    with (output / "throughput.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("optimization", "bytes", "capacity", "work", "path", "n",
                         "median_cycles_per_message", "min_cycles_per_message",
                         "max_cycles_per_message"))
        for key, values in sorted(samples.items()):
            writer.writerow((*key, len(values), statistics.median(values), min(values), max(values)))
    with (output / "irq_observations.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("mask", "case", "operation", "bytes", "waiters", "n",
                         "median_cycles", "min_cycles", "max_cycles", "max_us_at_168mhz"))
        for key, values in sorted(profile.items()):
            writer.writerow((*key, len(values), statistics.median(values), min(values), max(values),
                             max(values) / 168.0))
    report = {"captures": capture_names, "method": "median of raw cycles/messages, same shape/work",
              "comparisons": []}
    for optimization, size, count, work, path in sorted(samples):
        if path != "loan":
            continue
        loan = statistics.median(samples[(optimization, size, count, work, "loan")])
        copy = statistics.median(samples[(optimization, size, count, work, "copy")])
        pool = statistics.median(samples[(optimization, size, count, work, "pool")])
        report["comparisons"].append({"optimization": optimization, "bytes": size,
            "capacity": count, "work": work, "copy_cycles": copy, "pool_cycles": pool,
            "loan_cycles": loan, "copy_over_loan": copy / loan,
            "pool_over_loan": pool / loan, "loan_change_vs_copy_percent": (loan / copy - 1) * 100})
    (output / "performance.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"captures": len(capture_names), "bench_cells": len(samples),
                      "irq_cells": len(profile)}, indent=2))


if __name__ == "__main__":
    main()
