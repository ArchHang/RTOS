"""Check the functional H743 UART and USB host regression records."""

import json
from pathlib import Path

from verify import integers, need, one, pressure, rows


ARCHIVE = Path(__file__).resolve().parent / "raw" / "h743-functional"


def read(tag):
    return (ARCHIVE / f"{tag}.txt").read_text(encoding="utf-8-sig")


def check_priority():
    for tag, status, order in (("priority_base", 1, ("2310", "1234")),
                               ("priority_fix_O2", 0, ("1230", "1423")),
                               ("priority_fix_O3", 0, ("1230", "1423"))):
        text = read(tag)
        trace = rows(text, "H7PRIO|TRACE|")
        need([integers(row, "running", "length") for row in trace] == [(0, 3), (1, 4)], tag)
        need(tuple(row["order"] for row in trace) == order, tag)
        need(integers(one(text, "H7PRIO|DONE|"), "status", "ready", "running") ==
             (status, 1 - status, 1 - status), tag)


def check_services():
    for tag, status, values in (("services_base", 1, (1_500_000, 0, 0)),
                                ("fix_services_O2", 0, (1_000_000, 1_000_000, 1_431_655_764)),
                                ("fix_services_O3", 0, (1_000_000, 1_000_000, 1_431_655_764))):
        text = read(tag)
        result = one(text, "H7S|CYCLES|")
        need(integers(result, "native", "zero") == (1_000_000, 0), tag)
        need(integers(result, "fractional", "sub_mhz", "wrap") == values, tag)
        need(integers(one(text, "H7S|DONE|"), "status", "checks", "failures") == (status, 12, status), tag)


def check_heap():
    for tag, passed in (("heap_lto_base", False), ("heap_lto_release", True)):
        text = read(tag)
        records = rows(text, "H7MP|CASE|")
        need([integers(row, "kind", "irq", "expected") for row in records] ==
             [(0, 1, 1), (1, 1, 1), (2, 1, 32760)], tag)
        for row in records:
            before, observed, expected, cycles, success = integers(
                row, "before", "observed", "expected", "cycles", "pass")
            need(before != expected and success == int(passed), tag)
            need((observed == expected and cycles < 24_000_000) if passed else
                 (observed == before and cycles >= 24_000_000), tag)
        need(integers(one(text, "H7MP|DONE|"), "pass", "fail") == ((4, 0) if passed else (1, 3)), tag)

    text = read("heap_min_host")
    capacities = rows(text, "heap_configured=")
    need([integers(row, "heap_configured", "usable") for row in capacities] ==
         [(8, 0), (23, 0), (24, 16), (25, 16), (31, 16), (32, 24), (33, 24), (39, 24), (40, 32), (41, 32)],
         "Minimum heap capacities")
    need([row["init"] for row in capacities] == ["PARAM"] * 2 + ["OK"] * 8, "Minimum heap init")
    need([int(row["smallest_tail"]) for row in capacities[2:]] == [0] * 6 + [16, 16], "Minimum split")


def check_modules():
    checks = (
        ("startup_final_O0", "STARTUP|DONE|", 11), ("boundary_O2", "BT|SUMMARY|", 103),
        ("robustness_fix_O2", "RB|DONE|", 52), ("mutex_final_O2", "AM|SUMMARY|", 910),
        ("mutex_boundary_run", "AMB|SUMMARY|", 87), ("mutex_registry_run", "AR|SUMMARY|", 259),
        ("ipc_wait_final_O3", "AW|SUMMARY|", 1600), ("ipc_lifetime_run", "OF|SUMMARY|", 80),
        ("queue_copy_O0", "QCOPY|DONE|", 6), ("port_compat_O0", "H7C|DONE|", 16),
        ("compat_lto_final", "H7C|DONE|", 16),
    )
    for tag, prefix, expected in checks:
        need(integers(one(read(tag), prefix), "pass", "fail") == (expected, 0), tag)


def check_usb():
    cases = ["queue-full", "thread-full", "controller-fail", "media", "transport"]
    for tag, passes, failures in (("usb_base", 101, [12, 12, 1, 0, 0]),
                                  ("usb_O0", 128, [0] * 5), ("usb_O2", 128, [0] * 5),
                                  ("usb_O3", 128, [0] * 5), ("usb_readonly", 129, [0] * 5)):
        text = read(tag)
        done = rows(text, "USBHOST|DONE|")
        need([row["case"] for row in done] == cases, tag)
        need([int(row["fail"]) for row in done] == failures, tag)
        checks = [line.rsplit("|", 1)[-1] for line in text.splitlines() if line.startswith("USBHOST|CHECK|")]
        need(checks.count("PASS") == passes and checks.count("FAIL") == sum(failures), tag)


def main():
    check_priority()
    check_services()
    check_heap()
    check_modules()
    check_usb()
    record = json.loads((ARCHIVE / "runs.json").read_text(encoding="utf-8"))
    need(record["restoration"] == {"restored": True, "readback_bytes": 2_097_152}, "Original flash readback")
    text = read("integration_pressure120")
    need(integers(one(text, "STRESS|BEGIN|"), "seconds", "seed", "queue_capacity") ==
         (120, 1592594996, 1), "Pressure configuration")
    packets = pressure(text, record["integration_pressure120"])
    print(json.dumps({"status": "PASS", "production_packets": packets}, indent=2))


if __name__ == "__main__":
    main()
