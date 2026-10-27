"""Read-only independent acceptance of raw UART, build and firmware evidence."""
import argparse
from collections import Counter
import hashlib
import itertools
import json
from pathlib import Path
import re


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read(path):
    return path.read_text(encoding="utf-8-sig", errors="strict")


def fields(line):
    return dict(re.findall(r"(?:^|[ |])([a-z_]+)=([^\s|]+)", line))


def rows(text, prefix):
    return [fields(line) for line in text.splitlines() if line.startswith(prefix)]


def numbers(row, names):
    return tuple(int(row[name], 0) for name in names.split())


def verify_channel(text, variant):
    mode, good, bad = {
        "channel_checks": ("functional", 413, 0),
        "channel_stress": ("stress", 48, 0),
        "channel_stress_negative": ("stress", 45, 3),
        "channel_dma": ("dma", 36, 0),
        "channel_bench": ("bench", 1366, 0),
        "channel_profile": ("profile", 1040, 0),
    }.get(variant, ("profile", None, 0))
    summary = rows(text, "CH|SUMMARY|")
    require(len(summary) == 1 and summary[0]["mode"] == mode, "one correct summary")
    if good is None:
        good = int(summary[0]["pass"])
        require(good > 280, "complete profile checks")
    require(numbers(summary[0], "pass fail") == (good, bad), "exact summary counts")
    checks = re.findall(r"^CH\|CHECK\|(PASS|FAIL)\|id=(\d+) name=([^\s]+)\s*$",
                        text, re.MULTILINE)
    require(len(checks) == good + bad, "complete raw checks")
    check_lines = [line for line in text.splitlines() if line.startswith("CH|CHECK|")]
    require(len(check_lines) == len(checks), "no malformed check verdicts")
    require([int(c[1]) for c in checks] == list(range(1, len(checks) + 1)),
            "sequential unique check identities")
    require(Counter(c[0] for c in checks) == Counter({"PASS": good, "FAIL": bad}),
            "raw check counts agree")
    done = rows(text, "CH|DONE|")
    require(len(done) == 1 and done[0]["status"] == str(int(bad != 0)), "one expected DONE")
    meta = rows(text, "CH|META|")
    require(len(meta) == 1 and meta[0]["mode"] == mode, "one expected META")
    if bad:
        require(Counter(c[2] for c in checks if c[0] == "FAIL") == Counter({
            "stress_source_exact_once": 1, "stress_no_worker_or_irq_error": 1,
            "stress_unique_message_bitmap": 1}), "only specified injected failures")
    fail_lines = [line for line in text.splitlines() if re.search(
        r"(?:^|[| ])FAIL(?:[| ]|$)", line)]
    require(len(fail_lines) == bad and all(re.fullmatch(
        r"CH\|CHECK\|FAIL\|id=\d+ name=[^\s]+", line) for line in fail_lines),
        "no extra failed verdicts")
    if variant == "channel_checks":
        require([(r["priority"], r["legal"], r["completed"])
                 for r in rows(text, "CH|IRQ|")] == [("0", "0", "1"),
                 ("4", "0", "1"), ("5", "1", "1")], "real IRQ priority matrix")
        budget = rows(text, "CH|BUDGET|")
        require(len(budget) == 1 and int(budget[0]["requested"]) == 12
                and int(budget[0]["elapsed"]) >= 12, "retry total budget")
        deadlines = rows(text, "CH|DEADLINE|")
        require(Counter((r["operation"], r["early_wake"]) for r in deadlines) == Counter(
            itertools.product(("acquire", "receive"), ("0", "1"))), "late-grant matrix")
        require(all(int(r["elapsed"]) >= 50 and r["result"] == "-3" for r in deadlines),
                "late resources must not bypass elapsed budget")
    if mode == "stress":
        require(meta[0].get("consumer_period") == "1"
                and meta[0].get("consumer_wait") == "1000"
                and meta[0].get("diagnostics") == "1", "explicit business pacing and diagnostics")
        stress = rows(text, "CH|STRESS|")
        require(len(stress) == 1, "one stress record")
        expected = (4608, 4607, 4607, 1, 1, 512, 8, 8) if bad else (
            4608, 4608, 4608, 0, 0, 512, 8, 8)
        require(numbers(stress[0], "produced consumed unique errors checksum_errors "
                        "irq_accepted returned capacity") == expected, "exact message ledger")
        require(int(stress[0]["interrupts"]) >= 512, "actual ISR load")
        consumers = rows(text, "CH|CONSUMER|")
        require(Counter(r["index"] for r in consumers) == Counter("0123"),
                "four unique consumers")
        require(all(int(r["consumed"]) > 0 for r in consumers)
                and sum(int(r["consumed"]) for r in consumers) == expected[1],
                "each consumer participates and counts sum")
        tasks = rows(text, "CH|TASK|")
        expected_tasks = Counter((role, str(index)) for role in ("producer", "consumer")
                                 for index in range(4))
        expected_tasks[("irq", "0")] = 1
        require(Counter((r["role"], r["index"]) for r in tasks) == expected_tasks,
                "all eight tasks and the IRQ have diagnostics")
        require(all(r["errors"] == "0" and r["first_stage"] == "none"
                    and r["first_status"] == "0" and r["timeouts"] == "0"
                    and r["closed"] == ("1" if r["role"] == "consumer" else "0")
                    for r in tasks), "no unclassified API/timeout errors; all consumers close")
        inject = rows(text, "CH|INJECT|")
        require(inject == ([{"boundary": "payload_checksum", "source": "0",
                             "sequence": "7", "byte": "17", "count": "1"}] if bad else []),
                "one corruption iff negative control")
    if mode == "dma":
        dma = rows(text, "CH|DMA|")
        require(len(dma) == 2 and [r["case"] for r in dma] == ["normal", "close_in_flight"],
                "two DMA lifecycles")
        require(numbers(dma[0], "bytes tc remaining errors publish") == (16384, 1, 0, 0, 0),
                "actual successful DMA completion")
        require(numbers(dma[1], "bytes tc_before tc_after tc remaining publish cancel")
                == (16384, 0, 0, 1, 0, -11, 0), "completion after close")
        require(int(dma[1]["before"]) > 0 and int(dma[1]["after"]) > 0,
                "hardware transfer still in flight at close")
    if mode == "bench":
        shapes = list(itertools.product((16, 64, 256, 1024), (1, 3, 16)))
        shapes += [(4096, 1), (4096, 3), (16384, 1)]
        cases = rows(text, "CH|CASE|")
        require(Counter(numbers(r, "bytes capacity") for r in cases) == Counter(shapes),
                "complete size/capacity shape matrix")
        bench = rows(text, "CH|BENCH|")
        expected = Counter((size, count, work, path, rep) for size, count in shapes
                           for work in ("ipc", "e2e") for path in ("copy", "pool", "loan")
                           for rep in range(3))
        require(Counter((int(r["bytes"]), int(r["capacity"]), r["mode"], r["path"],
                         int(r["repetition"])) for r in bench) == expected,
                "270 distinct measurements, no missing/duplicate cell")
        require(all(int(r["messages"]) == int(r["capacity"]) * 128
                    and r["valid"] == "1" and int(r["cycles"]) > 0
                    and int(r["ticks"]) < 1000
                    and int(r["cycles_per_message"]) == int(r["cycles"]) // int(r["messages"])
                    for r in bench), "matched work, valid cycles, data and no DWT overflow")
    if mode == "profile":
        profile = rows(text, "CH|PROFILE|")
        expected = Counter(("ipc", operation, size, 0, rep)
                           for size in (16, 256, 1024, 4096, 16384)
                           for operation in ("copy_send", "copy_receive", "loan_acquire",
                                             "loan_publish", "loan_receive", "loan_release")
                           for rep in range(8))
        expected.update(("broadcast", operation, 256, count, rep)
                        for operation in ("loan_publish", "loan_release")
                        for count in (0, 4) for rep in range(8))
        expected.update(("baseline", "critical", 0, 0, rep) for rep in range(8))
        require(Counter((r["case"], r["operation"], int(r["bytes"]), int(r["waiters"]),
                         int(r["repetition"])) for r in profile) == expected,
                "280 distinct IRQ measurements, complete size/API/waiter matrix")
        require(all(numbers(r, "cycles hook_hits irq_count status valid")[1:]
                    == (1, 1, 0, 1) and int(r["cycles"]) > 0 for r in profile),
                "exactly one actual legal IRQ and successful operation per sample")
    return {"mode": mode, "pass": good, "expected_fail": bad}


def verify_legacy(text, variant, mask="basepri"):
    # Raw verdict counts, rather than trusting the firmware summary alone.
    expected = {"audit": ("AT", 117), "selftest": ("ST", 432),
                "boundary": ("BT", 103), "lowlevel": ("LC", 24),
                "adversarial_mutex": ("AM", 910),
                "adversarial_lifetime": ("OF", 80),
                "adversarial_mutex_boundary": ("AMB", 87),
                "adversarial_registry": ("AR", 259)}
    require(not re.search(r"\|FAIL(?:\||\s|$)", text), "no raw failed verdict")
    if variant in expected:
        prefix, count = expected[variant]
        raw = [line for line in text.splitlines() if line.startswith(prefix + "|")
               and re.search(r"\|PASS(?:\||$)", line)]
        require(len(raw) == count, f"{variant} complete raw PASS count {count}")
        if variant == "lowlevel":
            require(len(re.findall(r"^PC\|[^\r\n]+\|PASS\s*$", text, re.MULTILINE)) == 22,
                    "priority probes 22 PASS")
    elif variant == "adversarial_wait":
        phases = rows(text, "AW|PHASE|")
        names = ("queue_receivers", "queue_senders", "semaphore_waiters", "event_consumption",
                 "notify_increment", "event_notify_independence", "queue_object_delete",
                 "queue_timeout_late_send", "mutex_timeout_inheritance")
        increments = (26, 30, 33, 26, 11, 17, 12, 16, 28)
        expected_phases = []
        cumulative = 2  # high IRQ start and syscall IRQ ownership
        for round_number in range(8):
            for name, increment in zip(names, increments):
                cumulative += increment
                expected_phases.append((name, str(round_number), str(cumulative), "0"))
        expected_phases.append(("final_cleanup", "8", "1600", "0"))
        require([(r["name"], r["round"], r["pass"], r["fail"]) for r in phases]
                == expected_phases, "complete ordered 73 mixed-wait phases and exact deltas")
        summary = rows(text, "AW|SUMMARY|")
        require(len(summary) == 1 and numbers(summary[0], "pass fail rounds phases irq_requests")
                == (1600, 0, 8, 73, 120), "mixed-wait aggregate and real IRQ requests")
        irq = rows(text, "AW|HIGHIRQ|")
        require(len(irq) == 1 and int(irq[0]["entries"]) > 0
                and int(irq[0]["nonzero_samples"]) > 0
                and numbers(irq[0], "errors restored") == (0, 1), "real high-priority IRQ overlap")
    elif variant == "production_stress":
        summaries = rows(text, "STRESS|SUMMARY|")
        require(len(summaries) == 1 and numbers(summaries[0], "pass fail worker_errors")
                == (165, 0, 0), "production aggregate")
        require(len(re.findall(r"^STRESS\|CHECK\|.*status=PASS\s*$", text, re.MULTILINE)) == 165,
                "production raw contracts")
    elif variant == "daily":
        require("task 0 running" in text and "System init done" in text, "daily actual boot")
    elif variant == "bench":
        require("BM|DONE|benchmark complete" in text, "legacy benchmark done")
        measurements = re.findall(r"^BM\|([0-7])\|[^|]+\|N=(\d+)\|avg=(\d+)"
                                  r"\|min=(\d+)\|max=(\d+)\s*$", text, re.MULTILINE)
        require(Counter(r[0] for r in measurements) == Counter("01234567")
                and all(int(r[1]) > 0 and 0 < int(r[3]) <= int(r[2]) <= int(r[4])
                        for r in measurements), "all eight legacy microbenchmarks")
    elif variant == "basepri":
        require("BP|DONE|status=0" in text, "critical state regression done")
        meta = rows(text, "BP|META|")
        require(len(meta) == 1 and meta[0]["use_basepri"] == str(int(mask == "basepri")),
                "critical runtime mask configuration")
        summary = rows(text, "BP|SUMMARY|")
        require(len(summary) == 1 and int(summary[0]["pass"]) == (622 if mask == "basepri" else 620)
                and numbers(summary[0], "fail cases matrix_fail") == (0, 600, 0),
                "complete critical transition matrix")
        cases = rows(text, "BP|CASE|PASS|")
        require(len(cases) == 600 and [int(r["id"]) for r in cases] == list(range(1, 601))
                and all(r["nest"] == "0" and r["hook"] == "1"
                        and r["hook_pm"] == (r["in_pm"] if mask == "primask"
                                              and r["stage"] == "5" else "1")
                        and r["irq"] == "1" and r["irq_errors"] == "0"
                        and r["in_bp"] == r["out_bp"] and r["in_pm"] == r["out_pm"]
                        for r in cases), "critical matrix restores all caller masks")
        require(Counter((int(r["stage"]), int(r["prio"]), int(r["in_bp"], 0),
                         int(r["in_pm"]), int(r["depth"])) for r in cases) == Counter(
            itertools.product(range(1, 6), (0, 4, 5, 15), (0, 0x20, 0x50, 0x80, 0xF0),
                              (0, 1), (1, 2, 8))), "complete 600-cell critical configuration matrix")
        require(len(re.findall(r"^BP\|CHECK\|[^\r\n|]+\|PASS\s*$", text, re.MULTILINE))
                + len(cases) == int(summary[0]["pass"]), "critical raw verdict count")
    else:
        raise AssertionError(f"unsupported legacy variant {variant}")
    return {"mode": variant, "raw_acceptance": True}


def verify_capture(directory):
    identity = json.loads(read(directory / "identity.json"))
    tag, variant = identity["tag"], identity["variant"]
    text = read(directory / f"log_{tag}.txt")
    require(text.count("System init done") == 1, "one boot per capture")
    require(not re.search(r"ASSERT FAILED|HardFault_Handler|FAULT\||FAULT REPORT|"
                          r"\*\*\* NESTED FAULT|System halted\.", text), "no fatal fault")
    run = json.loads(read(directory / f"run_{tag}.json"))
    require(run["tag"] == tag and run["terminator"] == identity["terminator"]
            and run["flashed"] is True and run["terminator_seen"] is True
            and run["port"] == identity["port"] and run["baud"] == 115200, "real capture identity")
    elf = directory / f"firmware_{tag}.elf"
    require(hashlib.sha256(elf.read_bytes()).hexdigest().upper() == run["sha256"]
            and run["sha256"].lower() == identity["built_elf_sha256"],
            "flashed ELF identity")
    flash = read(directory / f"flash_{tag}.txt")
    require("066DFF555775514867092907" in flash and "Download verified successfully" in flash,
            "expected physical ST-LINK and successful verification")
    detail = read(directory / "build_detail.txt")
    warnings = [line for line in detail.splitlines() if "warning:" in line]
    expected_warnings = 2 if variant == "daily" else 25
    require(len(warnings) == expected_warnings and all(re.search(
        r"Core/Src/main\.c:\d+:\d+: warning: .+ \[-Wunused-(?:function|variable)\]", w)
        for w in warnings), "only expected pre-existing unused main demo warnings")
    require(not re.search(r"(?:fatal )?error:", detail), "no compiler errors")
    commands = [line for line in detail.splitlines()
                if re.match(r'arm-none-eabi-gcc "[^\"]+\.c" .* -c ', line)]
    units = [(re.match(r'arm-none-eabi-gcc "([^\"]+)"', line).group(1), line)
             for line in commands]
    expected_units = {"../RTOS/" + name for name in (
        "Core/Src/rtos_kernel.c", "Core/Src/rtos_task.c", "Core/Src/rtos_sched.c",
        "Core/Src/rtos_wait.c", "IPC/rtos_sem.c", "IPC/rtos_mutex.c", "IPC/rtos_queue.c",
        "IPC/rtos_event.c", "IPC/rtos_channel.c", "Memory/rtos_heap.c",
        "Memory/rtos_mempool.c", "Services/rtos_timer.c", "Services/rtos_perf.c",
        "USB/rtos_usb_device.c", "USB/rtos_usb_osal.c", "USB/rtos_usb_msc.c",
        "Port/GCC/ARM_CM4F_CM7F/port.c")}
    kernel = [(source, line) for source, line in units
              if source.startswith("../RTOS/") and not source.startswith("../RTOS/lib/")]
    require(len(kernel) == 17 and {source for source, _ in kernel} == expected_units,
            "fresh compile of all 17 kernel translation units")
    compiles = [line for source, line in units if source.startswith(
        ("../RTOS/", "../Core/Src/", "../hil/fwsrc/"))]
    enabled = variant.startswith("channel_") or identity["enable"]
    require(all(("-DRTOS_CONFIG_USE_CHANNEL=1" in line) == enabled for line in compiles),
            "uniform feature config")
    require(all(("-DRTOS_CONFIG_USE_BASEPRI=0" in line) == (identity["mask"] == "primask")
                for line in compiles), "uniform interrupt mask config")
    require(all(("-DRTOS_CONFIG_ASSERT_ENABLED=0" in line) == (identity["assertions"] == 0)
                for line in compiles), "uniform assertion config")
    require(all(re.findall(r"(?:^| )-(O[0-3sg])(?: |$)", line) == [identity["optimization"]]
                for line in compiles), "uniform labeled optimization")
    if variant.startswith("channel_"):
        fixture = [line for source, line in units if source.startswith("../hil/fwsrc/")]
        require(len(fixture) == 2 and all("-Wextra" in line and "-Werror" in line
                                         for line in fixture), "strict new HIL compilation")
        return verify_channel(text, variant)
    return verify_legacy(text, variant, identity["mask"])


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    identities = sorted(args.directory.rglob("identity.json"))
    captures = []
    for identity in identities:
        obj = json.loads(read(identity))
        if "variant" not in obj:
            continue
        require(obj["passed"] and obj["stage"] == "complete", f"incomplete: {identity}")
        for name, expected in obj["artifacts"].items():
            path = identity.parent / name
            require(hashlib.sha256(path.read_bytes()).hexdigest() == expected,
                    f"artifact changed: {path}")
        captures.append({"tag": obj["tag"], **verify_capture(identity.parent)})
    require(captures, "no captures")
    print(json.dumps({"captures": len(captures), "passed": True, "results": captures}, indent=2))


if __name__ == "__main__":
    main()
