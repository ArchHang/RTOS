"""Recheck the archived F446 heap captures without a board or vendor tools."""
import argparse
from collections import Counter, defaultdict
from hashlib import sha256
import json
from pathlib import Path
import re


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
METRICS = {
    "irq": "cold P6 response (TIM5 ticks), mask (CPU cycles)",
    "irq_direct": "cold P6 response (TIM5 ticks), mask (CPU cycles)",
    "periodic": "fragment P6 response (TIM5 ticks), mask (CPU cycles)",
    "periodic_direct": "fragment P6 response (TIM5 ticks), mask (CPU cycles)",
    "industry": "fragment miss maximum (CPU cycles)",
    "failure": "first miss, first max query (CPU cycles)",
    "weighted": "normal, burst, diagnosed (CPU cycles per API call)",
    "boundary": "requests, minimum-sized blocks",
    "stress": "timer IRQs, nested IRQs",
}
IRQ_CASES = ("idle", "empty", "known", "cold", "cached", "telemetry")
BUSINESS = {
    "fixed": (384, "21896bc5"),
    "telemetry": (2304, "48324885"),
    "burst": (2304, "48324885"),
    "sessions": (1536, "7d04edc5"),
    "fragment": (1, "9e4aaf0d"),
}
WEIGHTED = {
    "normal": (6240, 1416058069),
    "burst": (7487, 3756184500),
    "diagnosed": (9061, 700988257),
}
FIRST_MISS = {"current": 7388, "accepted": 6855, "candidate": 8416, "isolated_max": 105}


def require(condition, explanation):
    if not condition:
        raise ValueError(explanation)


def values(record):
    result = {}
    for token in record.split("|")[2:]:
        key, separator, value = token.partition("=")
        require(key not in result, f"duplicate field {key}")
        result[key] = int(value) if separator and key != "input" and value.isdecimal() else value or True
    return result


def segment(raw, prefix, checks):
    lines = raw.decode("ascii").splitlines()
    starts = [index for index, line in enumerate(lines) if line.startswith(f"{prefix}|START|")]
    ends = [index for index, line in enumerate(lines) if line.startswith(f"{prefix}|DONE|")]
    require(len(starts) == len(ends) == 1 and starts[0] < ends[0], "expected one complete test run")
    start, end = starts[0], ends[0]
    terminator = {"status": 0, "checks": checks, "failures": 0}
    if prefix == "HBIRQ":
        terminator = {"status": 0, "failure": 0}
    require(values(lines[end]) == terminator, "unexpected terminator")
    result = defaultdict(list)
    for line in lines[start + 1:end]:
        require("|FAIL" not in line, line)
        if line.startswith((f"{prefix}|", "HILIRQ|BG|")):
            result[line.split("|")[1]].append(values(line))
    return values(lines[start]), result


def keyed(records, *fields):
    index = {tuple(record[field] for field in fields): record for record in records}
    require(len(index) == len(records), "duplicate record")
    return index


def check_stats(stats, count=None, total=None):
    n, lower, upper = (stats[field] for field in ("n", "min", "max"))
    total = stats["sum"] if total is None else total
    require(count is None or n == count, f"wrong sample count {n}")
    require(0 <= lower <= upper and n * lower <= total <= n * upper, "invalid timing statistics")
    require(n != 0 or lower == upper == 0, "nonzero timing with no samples")


def check_cases(records, expected):
    require([case["case"] for case in records] == list(expected), "unexpected cases")
    require(all(case.get("PASS") and case.get("line", 0) == 0 for case in records), "failed case")


def check_irq(start, rows, backend, instrumented):
    require(start == {"backend": backend, "instrumented": int(instrumented),
                      "cpu_hz": 168_000_000, "tim_hz": 84_000_000}, "unexpected IRQ header")
    response, timing, tests = (keyed(rows[kind], "case", "p") for kind in ("RSP", "TIME", "CASE"))
    keys = {(name, priority) for name in IRQ_CASES for priority in (4, 6)}
    require(response.keys() == timing.keys() == tests.keys() == keys, "unexpected IRQ cases")
    for key in keys:
        name, priority = key
        count = 384 if name == "telemetry" else 96
        check_stats(response[key], count)
        test, time = tests[key], timing[key]
        require(test.get("PASS") and test["line"] == 0, f"failed IRQ case {key}")
        require(0 < test["in_api"] <= count and test["in_api_max"] <= response[key]["max"],
                f"invalid in-call interrupt count {key}")
        require(priority != 6 or test["isr_masked"] == 0, f"priority-6 ISR ran while masked {key}")
        samples = count if instrumented and name != "idle" else 0
        require(time["mask_n"] == samples and bool(time["mask_max"]) == bool(samples)
                and time["mask_max"] <= time["api_max"], f"invalid mask statistics {key}")
    require(response["known", 6]["max"] >= 300 > response["known", 4]["max"], "IRQ control failed")
    return response["cold", 6]["max"], timing["cold", 6]["mask_max"]


def check_business(start, rows, phase, backend):
    require(start == {"backend": backend, "hz": 168_000_000, "budget": 32768, "cases": 5},
            "unexpected business fixture header")
    check_cases(rows["CASE"], BUSINESS)
    stats = keyed(rows["STAT"], "case", "op")
    require(len(stats) == 15, "wrong business stats count")
    for case in rows["CASE"]:
        name = case["case"]
        require((case["completed"], case["input"]) == BUSINESS[name], f"wrong input trace {name}")
        capacity = 32672 if backend == "zephyr" else 32760
        require(case["initial"] == case["restored"] == capacity, f"capacity did not recover {name}")
        for op in ("alloc", "free", "miss"):
            check_stats(stats[name, op])
    if phase == "industry":
        require(not rows["BG"], "unexpected background IRQ")
        return stats["fragment", "miss"]["max"]
    check_cases(rows["BG"], BUSINESS)
    for irq in rows["BG"]:
        name = irq["case"]
        check_stats(irq)
        require(irq["p"] == 6 and irq["n"] > 0, f"wrong periodic IRQ priority or count {name}")
        expected_loss = int(name == "fragment" and backend != "zephyr")
        require(irq["lost"] == expected_loss, f"unexpected missed rearms {name}")
        direct = phase == "periodic_direct"
        require(bool(irq["mask_n"]) == bool(irq["mask_max"]) == direct,
                f"wrong periodic mask sampling mode {name}")
    fragment = rows["BG"][-1]
    return fragment["max"], fragment["mask_max"]


def check_failure(start, rows, backend):
    require(start == {"capacity": 28496, "holes": 128, "raw_fragmented": 4608, "max_fragmented": 48},
            "unexpected fragmentation layout")
    check_cases(rows["CASE"], ("layout", "exhausted", "fragmented", "recovery", "capacity"))
    stats = keyed(rows["STAT"], "op")
    ops = ("exhausted", "first_miss", "same_miss", "larger_miss", "max_after_miss", "max_after_query")
    require(len(stats) == len(ops), "wrong failure stats count")
    for op in ops:
        check_stats(stats[op,], 32 if op in ("same_miss", "larger_miss") else 1)
    require(stats["first_miss",]["sum"] == FIRST_MISS[backend], "unexpected first-miss cycle count")
    return stats["first_miss",]["sum"], stats["max_after_miss",]["sum"]


def check_weighted(start, rows):
    require(start["capacity"] == 28496 and start["basepri"] == 1, "unexpected weighted fixture header")
    check_cases(rows["CASE"], WEIGHTED)
    stats = keyed(rows["STAT"], "case", "op")
    require(len(stats) == 12, "wrong weighted operation stats count")
    require([record["case"] for record in rows["TOTAL"]] == list(WEIGHTED), "unexpected weighted totals")
    averages = []
    for record in rows["TOTAL"]:
        name = record["case"]
        counts = cycles = 0
        for op in ("alloc_ok", "alloc_fail", "free", "query"):
            stat = stats[name, op]
            total = (stat["sum_hi"] << 32) + stat["sum_lo"]
            check_stats(stat, 3120 if op in ("alloc_ok", "free") else None, total)
            require(stat["invalid"] == 0, f"invalid timing samples {name}")
            counts += stat["n"]
            cycles += total
        require((counts, record["hash"]) == WEIGHTED[name] and record["calls"] == counts
                and record["frames"] == 3072, f"weighted trace identity mismatch {name}")
        require(record["max_alloc"] == record["restored_free"] == 28496, f"capacity lost {name}")
        require(cycles == (record["cycles_hi"] << 32) + record["cycles_lo"], f"cycle ledger mismatch {name}")
        averages.append(round(cycles / counts, 2))
    return tuple(averages)


def check_stress(start, rows):
    require(start["seconds"] == 20 and start["basepri"] == 1 and start["irq_period_us"] == 1000,
            "unexpected stress fixture header")
    require([source["id"] for source in rows["SOURCE"]] == list(range(4)), "missing stress sources")
    total, = rows["TOTAL"]
    for source in rows["SOURCE"]:
        require(source["attempts"] == source["accepted"] + source["no_memory"] + source["queue_full"]
                and source["accepted"] == source["received"], "unbalanced source accounting")
    for field in ("accepted", "no_memory", "queue_full"):
        require(total[field] == sum(source[field] for source in rows["SOURCE"]), f"wrong stress total {field}")
    require(total["alloc_delta"] == total["free_delta"]
            and total["max_alloc"] == total["final_free"] == start["capacity"], "stress heap not restored")
    require(total["irq"] == rows["SOURCE"][2]["attempts"]
            and total["nested"] == rows["SOURCE"][3]["attempts"] > 0, "missing interrupt workload")
    return total["irq"], total["nested"]


def check_capture(raw, phase, backend, prefix):
    checks = {"HILIRQ": 12, "HIB": 5, "HFL": 5, "HWB": 3, "HBN": 7, "HCPM": 3, "HBIRQ": None}
    start, rows = segment(raw, prefix, checks[prefix])
    if prefix == "HILIRQ":
        return check_irq(start, rows, backend, phase == "irq_direct")
    if prefix == "HIB":
        return check_business(start, rows, phase, backend)
    if prefix == "HFL":
        return check_failure(start, rows, backend)
    if prefix == "HWB":
        return check_weighted(start, rows)
    if prefix == "HBIRQ":
        return check_stress(start, rows)
    if prefix == "HBN":
        check_cases(rows["CASE"], ("request_bounds", "rounding", "split_threshold", "exhaustion",
                                   "corruption_repair", "invalid_release", "masked_failures"))
        total, = rows["TOTAL"]
        require(total["requests"] == 92 and total["min_blocks"] == 1781
                and total["free"] == total["max"] == start["capacity"] == 28496
                and total["live"] == start["live"] == 2, "boundary heap accounting mismatch")
        return total["requests"], total["min_blocks"]
    check_cases(rows["CASE"], ("setup", "few", "mixed"))
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-source", action="store_true", help="check normalized local source hashes")
    args = parser.parse_args()
    manifest = json.loads((HERE / "manifest.json").read_bytes())
    require(manifest["schema_version"] == 1, "unsupported manifest")
    if args.check_source:
        sources = [manifest["current_heap"]]
        for fixtures in manifest["fixture_sources"].values():
            sources.extend(fixtures)
        for source in sources:
            content = (ROOT / source["file"]).read_bytes().replace(b"\r\n", b"\n")
            require(sha256(content).hexdigest() == source["lf_sha256"], f"source changed {source['file']}")
    counts = Counter()
    files = set()
    for label, evidence in manifest["runs_by_firmware"].items():
        phase, backend, prefix = (evidence[key] for key in ("phase", "backend", "prefix"))
        runs = evidence["runs"]
        identities = {run["recorded_firmware_sha256"] for run in runs}
        require(len(runs) == 2 and len(identities) == 1 and re.fullmatch("[0-9a-f]{64}", next(iter(identities))),
                f"inconsistent recorded firmware identity {label}")
        observations = []
        for repeat, run in enumerate(runs, 1):
            filename = run["file"]
            require(filename == f"log_{label}_r{repeat}.txt", "unexpected capture filename")
            raw = (HERE / "uart" / filename).read_bytes()
            try:
                require(sha256(raw).hexdigest() == run["uart_sha256"], "UART hash mismatch")
                observations.append(check_capture(raw, phase, backend, prefix))
            except (ValueError, KeyError) as error:
                raise ValueError(f"{filename}: {error}") from error
            files.add(filename)
            counts[backend] += 1
        metrics = METRICS.get(phase)
        print(f"{label}: {metrics} = {observations}" if metrics else f"{label}: all cases passed twice")
    require(files == {file.name for file in (HERE / "uart").glob("*.txt")}, "unexpected UART archive contents")
    require(len(files) == 50 and counts["accepted"] == 16, "incomplete evidence bundle")
    print(f"PASS: {len(files)} archived UART captures ({counts['accepted']} final RTOS)")


if __name__ == "__main__":
    main()
