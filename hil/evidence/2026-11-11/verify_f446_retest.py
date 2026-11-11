"""Check the F446 replay of the native and shared Cortex-M fixtures."""

import json
from pathlib import Path

from verify import integers, need, one, pressure, rows


ARCHIVE = Path(__file__).resolve().parent / "raw" / "f446-retest"


def read(tag):
    return (ARCHIVE / f"{tag}.txt").read_text(encoding="utf-8-sig")


def check_native():
    checks = (
        ("boundary_O2", "BT|SUMMARY|", 103), ("selftest_O2", "ST|SUMMARY|", 432),
        ("audit_O2", "AT|SCHEDTASK|SUMMARY|", 25), ("audit_O2", "AT|IPC|SUMMARY|", 15),
        ("audit_O2", "AT|MEMTIMER|SUMMARY|", 23), ("audit_O2", "AT|USBPORT|SUMMARY|", 62),
        ("startup_O0", "STARTUP|DONE|", 11), ("queue_copy_O0", "QCOPY|DONE|", 6),
        ("robustness_coop_O2", "RB|DONE|", 56), ("mutex_chain_O2", "AM|SUMMARY|", 910),
        ("mutex_boundary_O3", "AMB|SUMMARY|", 87), ("mutex_registry_O2", "AR|SUMMARY|", 259),
        ("wait_O3", "AW|SUMMARY|", 1600), ("lifetime_O2", "OF|SUMMARY|", 80),
        ("wrap_O3", "WRAP|DONE|", 16), ("usb_cdc768_O2", "CDCWR|DONE|", 4),
        ("usb_cdc512_O3", "CDCWR|DONE|", 4), ("usb_priority_O2", "UPR|DONE|", 9),
        ("adc_O2", "ADC|DONE|", 2), ("timer_supervisor_O2", "SUP|DONE|", 23),
        ("bus_recovery_O2", "BUSREC|DONE|", 3), ("compat_O0", "H7C|DONE|", 16),
    )
    for tag, prefix, expected in checks:
        need(integers(one(read(tag), prefix), "pass", "fail") == (expected, 0), (tag, prefix))
    text = read("msp_O0")
    entry, sample = one(text, "MSP|ENTRY|"), one(text, "MSP|SAMPLE|")
    pointers = [int(address, 16) for address in (entry["svc"], entry["one_pendsv"],
                                                sample["initial"], sample["final"])]
    need(len(set(pointers)) == 1 and pointers[0] % 8 == 0 and sample["ticks"] == "32", "MSP balance")
    need(sum(line.endswith("|PASS") for line in text.splitlines() if line.startswith("MSP|")) == 4, "MSP checks")
    fp = one(read("compat_O0"), "H7C|FP|")
    need(integers(fp, "rounds_a", "rounds_b", "errors_a", "errors_b") == (64, 64, 0, 0), "FPU")


def check_scheduler():
    for tag, created, status, checks in (("base_reap", 35, -2, (7, 1)), ("reap_O0", 256, 0, (8, 0))):
        text = read(tag)
        need(integers(one(text, "H7REAP|LOAD|"), "created", "completed", "status", "idle_switches") ==
             (created, created, status, 0), tag)
        need(integers(one(text, "H7REAP|DONE|"), "pass", "fail") == checks, tag)

    for tag, high in (("base_resume", 0), ("resume_O2", 32)):
        text = read(tag)
        result = one(text, "H7RESUME|RESULT|")
        need(integers(result, "rounds", "irq", "valid", "selected_high", "high_first", "low_first", "clean") ==
             (32, 32, 32, high, high, 32 - high, 32), tag)
        need(integers(one(text, "H7RESUME|DONE|"), "pass", "fail") == ((4, 0) if high else (2, 2)), tag)

    text = read("base_noslice")
    need("H7B|CHECK|lowest_priority_forever|FAIL|round=0" in text, "Idle baseline")
    need(integers(one(text, "H7B|DONE|"), "status", "rounds") == (1, 0), "Idle baseline verdict")
    boundary = one(read("boundary_noslice"), "H7B|DONE|")
    need(integers(boundary, "status", "checks", "failures", "rounds", "irq", "accepted", "full", "task_timeouts") ==
         (0, 487, 0, 32, 128, 512, 128, 32), "Timer queue and priority boundaries")
    fifo = one(read("idle_fifo_coop"), "H7FIFO|DONE|")
    need(integers(fifo, "status", "entries", "solo_ticks", "idle_switches", "solo_switches") == (0, 12, 4, 0, 0),
         "Cooperative Idle FIFO")

    for tag, status, order in (("base_priority", 1, ("2310", "1234")),
                               ("priority_O2", 0, ("1230", "1423")),
                               ("priority_O3", 0, ("1230", "1423"))):
        text = read(tag)
        need(tuple(row["order"] for row in rows(text, "H7PRIO|TRACE|")) == order, tag)
        need(integers(one(text, "H7PRIO|DONE|"), "status", "ready", "running") ==
             (status, 1 - status, 1 - status), tag)


def check_services_and_heap():
    for tag, status, values in (("base_services", 1, (1_500_000, 0, 0)),
                                ("services_O3", 0, (1_000_000, 1_000_000, 1_431_655_764))):
        text = read(tag)
        value = one(text, "H7S|CYCLES|")
        need(integers(value, "clock", "native", "zero") == (168_000_000, 1_000_000, 0), tag)
        need(integers(value, "fractional", "sub_mhz", "wrap") == values, tag)
        need(integers(one(text, "H7S|DONE|"), "status", "checks", "failures") == (status, 12, status), tag)

    for tag, passed in (("base_heap_lto", False), ("heap_lto", True)):
        text = read(tag)
        records = rows(text, "F4MP|CASE|")
        need([integers(row, "kind", "irq") for row in records] == [(0, 1), (1, 1), (2, 1)], tag)
        for row in records:
            before, observed, expected, cycles, success = integers(
                row, "before", "observed", "expected", "cycles", "pass")
            need(before != expected and success == int(passed), tag)
            need((observed == expected and cycles < 8_400_000) if passed else
                 (observed == before and cycles >= 8_400_000), tag)
        need(integers(one(text, "F4MP|DONE|"), "pass", "fail") == ((4, 0) if passed else (1, 3)), tag)


def check_lifecycle():
    text = read("lifecycle60")
    start = one(text, "H7L|START|")
    need(integers(start, "seconds", "seed") == (60, 3735928559), "Lifecycle configuration")
    phases = rows(text, "H7L|PHASE|")
    need([int(phase["phase"]) for phase in phases] == [0, 1, 2], "Lifecycle phases")
    for phase in phases:
        ticks, irq, gap = integers(phase, "ticks", "irq", "max_gap_cycles")
        need(ticks > 0 and irq >= 2 * ticks and 0 < gap <= 1_680_000, "Lifecycle IRQ service")
    total = one(text, "H7L|TOTAL|")
    modes = integers(total, "deliver", "reopen", "timeout", "drop_rounds")
    need(min(modes) > 0 and sum(modes) == int(total["rounds"]), "Lifecycle modes")
    need(int(total["tasks"]) == sum(integers(total, "self_delete", "external_delete")), "Task conservation")
    need(integers(total, "heap_live", "heap_max_alloc") == (2, int(start["heap"])), "Heap recovery")
    sources = rows(text, "H7L|SOURCE|")
    need([int(source["id"]) for source in sources] == [0, 1, 2, 3], "Lifecycle sources")
    for source in sources:
        accepted, received = integers(source, "accepted", "received")
        need(accepted > 0 and accepted == received, "Lifecycle packet conservation")
    need(int(total["irq"]) == sum(integers(sources[3], "accepted", "refused")), "Lifecycle IRQ ledger")
    need(one(text, "H7L|DONE|")["status"] == "0", "Lifecycle verdict")
    return {"rounds": int(total["rounds"]), "packets": sum(int(source["received"]) for source in sources)}


def check_usb_and_uart():
    old = read("usb_msc_base")
    need(integers(one(old, "UMSC|DONE|"), "pass", "fail") == (3, 4), "MSC baseline")
    for case in ("queue_full_returns_no_memory", "queue_full_controller_untouched",
                 "queue_full_creates_no_worker", "queue_failure_releases_sync"):
        need(f"UMSC|CHECK|{case}|FAIL" in old, case)
    current = read("usb_msc_current")
    need(integers(one(current, "UMSC|DONE|"), "pass", "fail") == (24, 0), "MSC current")
    for case in ("queue_contents_unchanged", "thread_failure_releases_queue", "controller_failure_removes_worker",
                 "retry_succeeds", "worker_blocks_on_real_queue"):
        need(f"UMSC|CHECK|{case}|PASS" in current, case)

    uart = json.loads((ARCHIVE / "uart_summary.json").read_text(encoding="utf-8"))
    need(uart["status"] == "pass" and uart["windows"] == [1, 4], "UART configuration")
    need(uart["business_valid_responses"] == 72 and uart["unique_valid_responses"] == 74 and
         uart["crc_recovery"] and uart["header_recovery"], "UART transaction and recovery")
    return uart["business_valid_responses"]


def main():
    check_native()
    check_scheduler()
    check_services_and_heap()
    lifecycle = check_lifecycle()
    uart_responses = check_usb_and_uart()
    metadata = json.loads((ARCHIVE / "runs.json").read_text(encoding="utf-8"))
    need(metadata["restoration"] == {"restored": True, "readback_bytes": 524_288}, "Original F446 flash")
    text = read("production120")
    need(integers(one(text, "STRESS|BEGIN|"), "seconds", "seed", "queue_capacity") == (120, 1592594996, 1),
         "Production configuration")
    packets = pressure(text, metadata["production120"])
    print(json.dumps({"status": "PASS", "production_packets": packets, "lifecycle": lifecycle,
                      "uart_inference_responses": uart_responses}, indent=2))


if __name__ == "__main__":
    main()
