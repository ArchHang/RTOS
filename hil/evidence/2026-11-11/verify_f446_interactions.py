"""Verify the F446 interaction regressions and final integration UART evidence.

Use --root to select an archive containing {tag}.txt (or log_{tag}.txt) and
runs.json. Original UART prefixes remain intact. Checks select the last READY
or fixture start marker after validating the complete raw-byte digest.
"""

import argparse
from collections import Counter
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re

from verify import EXPECTED_NEGATIVES, FAILURE, integers, need, one, pressure, rows


DEFAULT_ROOT = Path(__file__).resolve().parent / "raw" / "f446-interactions"


def capture_time(value):
    # PowerShell emits seven fractional digits. Python 3.9 accepts at most six.
    return datetime.fromisoformat(re.sub(r"(\.\d{6})\d+(?=[+-]\d\d:\d\d$)", r"\1", value))


class Archive:
    def __init__(self, root):
        self.root = root.resolve(strict=True)
        metadata = self.root / "runs.json"
        self.runs = json.loads(metadata.read_text(encoding="utf-8-sig"))
        self.ignored_prefix_lines = {}

    def raw(self, tag):
        need(tag.isidentifier(), f"Invalid capture tag: {tag}")
        paths = [self.root / f"{tag}.txt", self.root / f"log_{tag}.txt"]
        existing = [path for path in paths if path.is_file()]
        need(existing, f"Missing UART log: {tag}")
        contents = existing[0].read_bytes()
        need(all(path.read_bytes() == contents for path in existing), f"Ambiguous UART logs: {tag}")
        return contents

    def read(self, tag, marker):
        contents = self.raw(tag)
        lines = contents.decode("utf-8-sig").splitlines()
        starts = [index for index, line in enumerate(lines) if line.startswith(marker)]
        need(starts, f"Missing start marker {marker} in {tag}")
        self.ignored_prefix_lines[tag] = starts[-1]
        return "\n".join(lines[starts[-1]:])

    def portable(self, tag, entry, priority=4):
        text = self.read(tag, "F4HIL|READY|")
        ready = one(text, "F4HIL|READY|")
        need(ready["entry"] == entry and int(ready["priority"]) == priority, f"Fixture entry: {tag}")
        return text

    def run(self, tag, outcome):
        record = self.runs[tag]
        need(record["outcome"] == outcome, f"Capture outcome: {tag}")
        return record

    def identities(self):
        board_count = 0
        host_count = 0
        for tag, record in self.runs.items():
            if not isinstance(record, dict) or "raw_sha256" not in record:
                continue
            need(hashlib.sha256(self.raw(tag)).hexdigest() == record["raw_sha256"],
                 f"Raw UART identity: {tag}")
            if record.get("execution") == "host":
                host_count += 1
                continue
            board_count += 1
            elapsed = (capture_time(record["finished"]) - capture_time(record["started"])).total_seconds()
            need(elapsed > 0 and abs(elapsed - record["elapsed_seconds"]) < 0.001,
                 f"Capture elapsed time: {tag}")
            need(record["flashed"] and record["terminator_seen"] and
                 record["build"]["exit_code"] == 0, f"Incomplete board capture: {tag}")
        need((board_count, host_count) == (32, 5), "Board and host evidence count")
        patch = self.root / "baseline_to_tested_final.patch"
        need(hashlib.sha256(patch.read_bytes()).hexdigest() == self.runs["patch_sha256"],
             "Archived baseline-to-final patch identity")

    def restoration(self):
        record = self.runs["restoration"]
        need(record == {"restored": True, "readback_bytes": 524_288}, "Original F446 flash readback")
        need(self.runs["option_bytes_match"] is True, "Original F446 option bytes")


def no_failures(text, tag):
    need(FAILURE.search(text) is None, f"Unexpected failure output: {tag}")


def check_reap_and_stack(archive):
    for tag, status, checks, capacity in (
        ("base_reap_irq", (-2, -9), (2, 1), 56),
        ("fix_reap_only_O2", (0, 0), (3, 0), 800),
        ("fix_reap_only_O3", (0, 0), (3, 0), 800),
    ):
        text = archive.portable(tag, "f446_reap_irq_pressure_run", priority=9)
        result = one(text, "REAPIRQ|RESULT|")
        need(integers(result, "created", "exited", "irq", "max_before", "setup", "clean") ==
             (36, 36, 1, capacity, 1, 1), tag)
        need(integers(result, "alloc", "delete") == status, tag)
        need(integers(one(text, "REAPIRQ|DONE|"), "pass", "fail") == checks, tag)
        no_failures(text, tag)

    for tag, hooks, stale, checks in (("probe_stack_hook", 2, 1, (0, 1)),
                                     ("fix_stack_O2", 1, 0, (1, 0))):
        text = archive.portable(tag, "f446_stack_hook_irq_run")
        need(integers(one(text, "SGIR|RESULT|"), "hooks", "stale", "irq", "irq_status", "wait",
                      "delete", "recreate", "new_guard", "clean") ==
             (hooks, stale, 1, 0, 0, 0, 0, 1, 1), tag)
        need(integers(one(text, "SGIR|DONE|"), "pass", "fail") == checks, tag)
        no_failures(text, tag)


def check_cooperative(archive):
    for tag, winners, checks in (("base_coop_yield", (1, 0), (0, 2)),
                                 ("fix_coop_O2", (3, 2), (2, 0)),
                                 ("fix_coop_O0", (3, 2), (2, 0))):
        text = archive.portable(tag, "f446_cooperative_yield_irq_run")
        cases = rows(text, "COY|CASE|")
        need([integers(row, "kind", "expected") for row in cases] == [(0, 3), (1, 2)], tag)
        need(tuple(int(row["first"]) for row in cases) == winners, tag)
        for row in cases:
            need(integers(row, "irq", "window", "irq_status", "wait", "clean") == (1, 1, 0, 0, 1), tag)
        need(integers(one(text, "COY|DONE|"), "pass", "fail") == checks, tag)
        no_failures(text, tag)


def check_timer_order(archive):
    for tag, extended, fixed in (("base_timer_order", False, False),
                                  ("fix_timer_O2", False, True),
                                  ("fix_timer_O3", False, True),
                                  ("timer_delete_base_O2", True, False),
                                  ("timer_delete_fixed_O2", True, True)):
        text = archive.portable(tag, "f446_timer_command_order_run")
        expected = Counter()
        if not fixed:
            expected.update({"F4TI|CHECK|older_start_before_callback_stop|FAIL": 16,
                             "F4TI|CHECK|older_stop_before_callback_start|FAIL": 16})
            if extended:
                expected["F4TI|CHECK|callback_start_rejects_earlier_delete|FAIL"] = 1
        failures = Counter(line for line in text.splitlines() if FAILURE.search(line))
        need(failures == expected, f"Timer failure identities: {tag}")
        need(integers(one(text, "F4TI|DONE|"), "status", "checks", "failures", "rounds",
                      "stop_calls", "start_calls") ==
             (int(not fixed), 234 if extended else 227, sum(expected.values()), 16,
              16 if fixed else 32, 32 if fixed else 16), tag)
        if extended:
            need(integers(one(text, "F4TI|DELETE|"), "calls", "command_status") ==
                 (1, -1 if fixed else 0), tag)


def check_timer_masks(archive):
    measurements = {}
    for old_tag, new_tag, has_sender in (("base_burst_O2", "fix_burst_O2", False),
                                        ("timer_burst_waiter_base_O2", "timer_burst_waiter_fixed_O2", True)):
        pair = []
        for tag in (old_tag, new_tag):
            text = archive.portable(tag, "f446_timer_irq_burst_run")
            samples = rows(text, "F4TB|LOAD|")
            need([int(row["active"]) for row in samples] == [32, 96, 128], tag)
            for row in samples:
                need(integers(row, "irq", "accepted", "full") == (8, 136, 8), tag)
                need(min(integers(row, "sections", "max_mask_cycles")) > 0, tag)
                if has_sender:
                    need(int(row["task_sends"]) == 8, tag)
            need(integers(one(text, "F4TB|DONE|"), "status", "checks", "failures", "clock_hz") ==
                 (0, 33, 0, 168_000_000), tag)
            no_failures(text, tag)
            pair.append(samples)
        for before, after in zip(*pair):
            need(int(after["max_mask_cycles"]) < int(before["max_mask_cycles"]) and
                 int(after["sections"]) > int(before["sections"]), f"Timer mask improvement: {new_tag}")
        measurements[new_tag] = {
            "active": [int(row["active"]) for row in pair[0]],
            "before_max_mask_cycles": [int(row["max_mask_cycles"]) for row in pair[0]],
            "after_max_mask_cycles": [int(row["max_mask_cycles"]) for row in pair[1]],
        }

    tag = "timer_dispatch_fixed_O2"
    text = archive.read(tag, "TDB|SETUP|")
    no_failures(text, tag)
    need(integers(one(text, "TDB|DONE|"), "pass", "fail") == (6, 0), tag)
    dispatch = rows(text, "TDB|DISPATCH|PASS|")
    need([row["mode"] for row in dispatch] == ["forever", "deadline"], tag)
    for row in dispatch:
        average, minimum, maximum = integers(row, "avg", "min", "max")
        need(integers(row, "N", "skipped", "invalid") == (64, 0, 0) and
             0 < minimum <= average <= maximum, tag)
    need(sum(line.endswith("|PASS") for line in text.splitlines()) == 6, tag)
    return measurements


def check_usb(archive):
    for tag, fixed in (("base_usb_session", False), ("fix_usb_O2", True),
                        ("fix_usb_O3", True), ("final_usb_session", True)):
        text = archive.portable(tag, "usb_rx_session_run")
        cases = rows(text, "URX|CASE|")
        need([int(row["action"]) for row in cases] == [2, 4, 5, 3], tag)
        for row in cases:
            action = int(row["action"])
            readers = 1 if action == 3 else 4
            need(integers(row, "created", "blocked", "irq") ==
                 (readers, 0 if action == 3 else readers, 2), tag)
            if fixed:
                expected = (readers, readers, 0, 2 if action == 4 else 0, 1)
            else:
                expected = {2: (2, 2, 0, 0, 0), 4: (2, 0, 2, 0, 0),
                            5: (2, 2, 0, 0, 0), 3: (0, 0, 0, 0, 0)}[action]
            need(integers(row, "done", "cancelled", "stolen", "fresh", "pass") == expected, tag)
        need(integers(one(text, "URX|DONE|"), "status", "failures", "setup") ==
             ((0, 0, 1) if fixed else (1, 4, 1)), tag)
        no_failures(text, tag)

    for tag, capacity in (("final_cdc768", 768), ("final_cdc512", 512)):
        text = archive.read(tag, "CDCWR|BEGIN|")
        need(int(one(text, "CDCWR|BEGIN|")["capacity"]) == capacity, tag)
        need(integers(one(text, "CDCWR|DONE|"), "pass", "fail") == (4, 0), tag)
        need([row["second"] for row in rows(text, "CDCWR|BYTES|")] == ["8596A7B8"] * 2, tag)
        need(sum(line.endswith("|PASS") for line in text.splitlines()) == 4, tag)
        no_failures(text, tag)


def check_integration(archive):
    text = archive.read("final_robustness_coop", "RB|BEGIN|")
    need(integers(one(text, "RB|BEGIN|"), "basepri", "assert", "preempt", "hotpath") == (1, 1, 0, 1),
         "Cooperative robustness profile")
    need(integers(one(text, "RB|DONE|"), "pass", "fail") == (56, 0), "Cooperative robustness")
    need(sum(line.endswith("|PASS") for line in text.splitlines() if line.startswith("RB|CHECK|")) == 56,
         "Cooperative robustness check count")
    no_failures(text, "final_robustness_coop")

    text = archive.read("final_wait_O3", "AW|META|")
    need(integers(one(text, "AW|SUMMARY|"), "pass", "fail", "rounds", "phases", "irq_requests") ==
         (1600, 0, 8, 73, 120), "IPC wait coverage")
    need(len(rows(text, "AW|PHASE|")) == 73 and one(text, "AW|DONE|")["status"] == "0", "IPC phases")
    high = one(text, "AW|HIGHIRQ|")
    need(int(high["entries"]) > 0 and integers(high, "nonzero_samples", "errors", "restored") == (0, 0, 1),
         "IPC high-priority IRQ ledger")
    no_failures(text, "final_wait_O3")

    text = archive.read("final_lifetime", "OF|META|")
    need(integers(one(text, "OF|META|"), "rounds", "cases_per_round", "static_storage") == (8, 10, 1),
         "Object lifetime configuration")
    cases = rows(text, "OF|CASE|PASS|")
    need([int(row["id"]) for row in cases] == list(range(1, 81)), "Object lifetime case identities")
    for row in cases:
        need(int(row["status"]) == int(row["expected"]) and int(row["mismatch"], 0) == 0 and
             integers(row, "prep", "hook", "irq", "inside", "irq_status", "dead_init", "priority") ==
             (0, 1, 1, 1, 0, 0, 5), "Object lifetime IRQ result")
    need(integers(one(text, "OF|SUMMARY|"), "pass", "fail", "cases") == (80, 0, 80) and
         one(text, "OF|DONE|")["status"] == "0", "Object lifetime summary")
    no_failures(text, "final_lifetime")

    text = archive.read("final_wrap_O3", "WRAP|BEGIN|")
    need(integers(one(text, "WRAP|BEGIN|"), "rounds", "workers") == (16, 12), "Wrap configuration")
    need([integers(row, "n", "systick", "irq", "irq_tick") for row in rows(text, "WRAP|ROUND|PASS|")] ==
         [(number, 3, 1, 0) for number in range(1, 17)], "Real SysTick wrap rounds")
    need(integers(one(text, "WRAP|DONE|"), "pass", "fail") == (16, 0), "Wrap summary")
    no_failures(text, "final_wrap_O3")


def check_lifecycle(archive):
    tag = "final_lifecycle60"
    text = archive.portable(tag, "h743_lifecycle_stress_run")
    start = one(text, "H7L|START|")
    need(integers(start, "seconds", "seed") == (60, 2_779_096_485), "Lifecycle configuration")
    phases = rows(text, "H7L|PHASE|")
    need([int(row["phase"]) for row in phases] == [0, 1, 2], "Lifecycle phases")
    for row in phases:
        ticks, irq, gap = integers(row, "ticks", "irq", "max_gap_cycles")
        need(ticks > 0 and irq >= 2 * ticks and 0 < gap <= 1_680_000, "Lifecycle IRQ service")
    # Phase snapshots exclude the UART printing between phases. The capture
    # interval independently checks the requested wall-clock duration.
    need(archive.run(tag, "pass")["elapsed_seconds"] >= 60, "Lifecycle capture duration")
    total = one(text, "H7L|TOTAL|")
    modes = integers(total, "deliver", "reopen", "timeout", "drop_rounds")
    need(min(modes) > 0 and sum(modes) == int(total["rounds"]), "Lifecycle operation modes")
    need(int(total["tasks"]) == sum(integers(total, "self_delete", "external_delete")) and
         min(integers(total, "self_delete", "blocked_delete", "overload_refused", "nested")) > 0,
         "Lifecycle task conservation and coverage")
    need(integers(total, "heap_live", "heap_max_alloc") == (2, int(start["heap"])), "Lifecycle heap recovery")
    sources = rows(text, "H7L|SOURCE|")
    need([int(row["id"]) for row in sources] == [0, 1, 2, 3], "Lifecycle source identities")
    for row in sources:
        accepted, received = integers(row, "accepted", "received")
        need(accepted > 0 and accepted == received, "Lifecycle packet conservation")
    need(int(total["irq"]) == sum(integers(sources[3], "accepted", "refused")), "Lifecycle IRQ accounting")
    need(one(text, "H7L|DONE|")["status"] == "0", "Lifecycle verdict")
    no_failures(text, tag)
    return {"rounds": int(total["rounds"]), "packets": sum(int(row["received"]) for row in sources)}


def check_host(archive):
    counts = {}
    cases = ["queue-full", "thread-full", "controller-fail", "media", "transport"]
    for tag, failed, passed in (("host_tx_baseline", True, 130), ("host_tx_O0", False, 132),
                                ("host_tx_O2", False, 132), ("host_tx_O3", False, 132),
                                ("host_tx_readonly", False, 133)):
        text = archive.read(tag, "USBHOST|BUILD|")
        archive.run(tag, "expected_failure" if failed else "pass")
        done = rows(text, "USBHOST|DONE|")
        need([row["case"] for row in done] == cases, f"USB host case identities: {tag}")
        need([int(row["fail"]) for row in done] == [0, 0, 0, 0, 2 if failed else 0], tag)
        expected = {f"USBHOST|CHECK|{kind}_stale_writer_cannot_use_new_session|FAIL"
                    for kind in ("cdc", "hid")} if failed else set()
        need({line for line in text.splitlines() if FAILURE.search(line)} == expected,
             f"USB host failure identities: {tag}")
        need(sum(line.endswith("|PASS") for line in text.splitlines()) == passed, tag)
        for kind in ("cdc", "hid"):
            need(f"USBHOST|CHECK|{kind}_new_writer_uses_new_session|PASS" in text, tag)
        if not failed:
            record = archive.runs[tag]["build"]
            need(record["case_exits"] == dict.fromkeys(cases, 0), f"USB host exit codes: {tag}")
            for kind in ("cdc", "hid"):
                need(f"USBHOST|CHECK|{kind}_stale_writer_cannot_use_new_session|PASS" in text, tag)
        counts[tag] = {"pass": passed, "expected_fail": 2 if failed else 0}
    return counts


def check_pressure(archive):
    packets = {}
    for tag, seconds, seed, capacity, negative in (
        ("base_pressure120", 120, 1_592_594_996, 1, False),
        ("final_pressure180", 180, 1_592_594_996, 1, False),
        ("final_negative6", 6, 1, 8, True),
    ):
        text = archive.read(tag, "STRESS|BEGIN|")
        need(integers(one(text, "STRESS|BEGIN|"), "seconds", "seed", "queue_capacity", "inject") ==
             (seconds, seed, capacity, int(negative)), tag)
        failures = {line for line in text.splitlines() if FAILURE.search(line)}
        need(failures == (EXPECTED_NEGATIVES["checksum_negative"] if negative else set()),
             f"Pressure failure identities: {tag}")
        packets[tag] = pressure(text, archive.run(tag, "checksum_negative" if negative else "pass"))
    return packets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT,
                        help="Evidence archive directory containing runs.json")
    args = parser.parse_args()
    archive = Archive(args.root)
    archive.identities()
    check_reap_and_stack(archive)
    check_cooperative(archive)
    check_timer_order(archive)
    masks = check_timer_masks(archive)
    check_usb(archive)
    check_integration(archive)
    lifecycle = check_lifecycle(archive)
    packets = check_pressure(archive)
    host = check_host(archive)
    archive.restoration()
    print(json.dumps({"status": "PASS", "timer_masks": masks, "lifecycle": lifecycle,
                      "pressure_packets": packets, "host": host,
                      "ignored_prefix_lines": archive.ignored_prefix_lines}, indent=2))


if __name__ == "__main__":
    main()
