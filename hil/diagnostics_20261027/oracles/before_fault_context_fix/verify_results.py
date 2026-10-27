"""Independent, read-only acceptance of UART, complete builds and flashed ELF.

This batch owns its acceptance rules. It does not change historical verifiers
when the production source count or feature configuration changes.
"""
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


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fields(line):
    return dict(re.findall(r"(?:^|[ |])([a-z_]+)=([^\s|]+)", line))


def rows(text, prefix):
    return [fields(line) for line in text.splitlines() if line.startswith(prefix)]


def numbers(row, names):
    return tuple(int(row[name], 0) for name in names.split())


def verify_diagnostics(text, identity):
    variant = identity["variant"]
    require(not re.search(r"(?:^|[| ])FAIL(?:[| ]|$)", text, re.MULTILINE),
            "no failed verdict anywhere in diagnostic UART")
    mode = {"diagnostic_checks": "functional", "diagnostic_bench": "bench",
            "diagnostic_ring": "ring", "diagnostic_fault": "fault"}.get(variant)
    if variant == "diagnostic_checks" and not identity["diagnostics"]:
        mode = "disabled"
    require(mode is not None, "supported diagnostic fixture")
    meta = rows(text, "DG|META|")
    require(len(meta) == 1 and meta[0]["mode"] == mode, "one correct runtime mode")
    require(int(meta[0]["diagnostics"]) == identity["diagnostics"], "runtime feature matches build")
    require(int(meta[0]["capacity"]) == identity["capacity"], "runtime configured capacity")
    if mode in ("functional", "ring", "disabled"):
        require(numbers(meta[0], "assertions mask")
                == (identity["assertions"], int(identity["mask"] == "basepri")),
                "runtime assertions and exclusion mode match the label")
    if mode == "fault":
        armed = rows(text, "DG|FAULT_ARMED|")
        require(identity["fault"] and len(armed) == 1 and armed[0]["code"] == "udf"
                and int(armed[0]["task"]) > 0, "one explicitly armed terminal UDF")
        require(text.count("FAULT REPORT") == 1 and text.count("UNDEFINSTR") == 1
                and text.count("DIAG history frozen in SRAM; inspect diag_slots with the debugger.") == 1
                and text.count("Locate source:") == 1 and "System halted." in text,
                "complete intended fault decoding and diagnostic integration banner")
        require("NESTED FAULT" not in text and "ASSERT FAILED" not in text,
                "no nested failure in the intentional fault handler")
        require(not rows(text, "DG|SUMMARY|") and not rows(text, "DG|DONE|"),
                "terminal fault cannot report a normal functional result")
        return {"mode": "fault", "uart_terminal_fault_path": True,
                "sram_freeze_proven_by_uart": False}
    checks = re.findall(r"^DG\|CHECK\|(PASS|FAIL)\|id=(\d+) name=([^\s]+)\s*$",
                        text, re.MULTILINE)
    raw_checks = [line for line in text.splitlines() if line.startswith("DG|CHECK|")]
    require(len(raw_checks) == len(checks), "no malformed diagnostic verdicts")
    require([int(item[1]) for item in checks] == list(range(1, len(checks) + 1)),
            "complete ordered unique verdict identities")
    require(all(item[0] == "PASS" for item in checks), "all raw contracts passed")
    summary = rows(text, "DG|SUMMARY|")
    require(len(summary) == 1 and summary[0]["mode"] == mode,
            "one diagnostic summary of the right mode")
    require(numbers(summary[0], "pass fail") == (len(checks), 0), "summary agrees with raw checks")
    require(rows(text, "DG|DONE|") == [{"status": "0"}], "one successful final terminator")
    if mode == "bench":
        bench = rows(text, "DG|BENCH|")
        names = ("sem_nowait", "queue_nowait", "notify_nowait", "sem_pingpong",
                 "queue_pingpong", "notify_pingpong")
        require(Counter((row["name"], int(row["round"])) for row in bench)
                == Counter(itertools.product(names, range(3))), "complete 18 benchmark samples")
        require(all(int(row["operations"]) == 1024 and int(row["cycles"]) > 0
                    and 0 <= int(row["ticks"]) < 30000 and int(row["errors"]) == 0
                    and int(row["checksum"]) == 524800 for row in bench),
                "same work, real cycles and no benchmark errors")
        expected_checks = ["bench_prepared", "bench_exact_work", "bench_worker_accounted",
                           "bench_cleanup"] * 18
        require([item[2] for item in checks] == expected_checks,
                "all 72 benchmark preparation/data/worker/cleanup contracts")
    elif mode == "disabled":
        contract = json.loads(read(Path(__file__).with_name("contracts.json")))
        require([item[2] for item in checks] == contract["disabled"],
                "complete disabled output-preservation contracts")
        require(not rows(text, "DG|CASE|") and not rows(text, "DG|IRQ|"),
                "disabled fixture executes no enabled diagnostic cases")
    elif mode == "ring":
        contract = json.loads(read(Path(__file__).with_name("contracts.json")))
        require([item[2] for item in checks] == contract["ring"], "complete minimal-ring contracts")
        cases = rows(text, "DG|CASE|")
        require(len(cases) == 1 and cases[0]["name"] == "ring"
                and numbers(cases[0], "count overwritten") == (identity["capacity"], 5),
                "minimum ring bounds exercised")
        require([numbers(row, 'priority completed markers preserved') for row in rows(text, 'DG|IRQ|')]
                == [(0, 1, 0, 1), (4, 1, 0, 1), (5, 1, 1, 1)],
                "minimum ring real IRQ contracts")
    else:
        # The fixed case contract is deliberately independent of UART's counts.
        contract = json.loads(read(Path(__file__).with_name("contracts.json")))
        expected = contract["functional"][str(identity["diagnostics"])]
        require([item[2] for item in checks] == expected, "complete ordered functional contracts")
        cases = rows(text, "DG|CASE|")
        expected_cases = ([('phase', '0', '1'), ('phase', '0', '2')]
            + [('returned', str(operation), '4') for operation in range(10)]
            + [('timeout', str(operation), '4') for operation in (0, 5, 6)])
        require([(row['name'], row['operation'], row['phase']) for row in cases[:15]]
                == expected_cases, "actual waiting/ready/returned/timeout operation matrix")
        require(all(int(row['episode']) > 0 for row in cases[:12])
                and all(int(row['episode']) > 0 for row in cases[21:23]),
                "all complete cases identify an actual retained WAIT sequence")
        require(all(row['wake'] == '0' and row['result'] == '0'
                    for row in cases[2:12]), "all ten APIs really returned successfully")
        require([int(row['cause']) for row in cases[:12]]
                == [0, 1, 1, 1, 1, 1, 1, 1, 1, 3, 2, 2],
                "resource grants, elapsed delay and channel notifications have distinct causes")
        require(all(row['wake'] == '-3' and row['result'] == '-3'
                    for row in cases[12:15]) and cases[13]['value'] == '1',
                "partial event bits do not hide timeout")
        require([(row['suspended'], row['deleted'], row['phase'])
                 for row in cases[15:18]] == [('0', '0', '4'), ('1', '0', '5'), ('0', '1', '6')],
                "deinit, suspend and deletion have distinct phases")
        require([(row['name'], row['timeout'], row['phase'], row['cause'], row['result'])
                 for row in cases[18:20]] == [('delay_suspend', '150', '4', '7', '0'),
                    ('delay_suspend', '4294967295', '4', '7', '0')],
                "finite and indefinite delay cancellation differ from elapsed delay")
        require(cases[20] == {'name': 'ready_suspend', 'phase': '5', 'cause': '1', 'wake': '0'},
                "suspending a READY task preserves the earlier resource grant")
        require([(row['name'], row['recreate'], row['wake'], row['result'])
                 for row in cases[21:23]] == [('channel_close', '0', '-11', '-11'),
                    ('channel_close', '1', '-11', '-8')], "same-address close/rebuild return matrix")
        require([(row['name'], row['operation'], row['wake'], row['result'])
                 for row in cases[23:25]] == [('channel_late', '8', '0', '-3'),
                    ('channel_late', '9', '0', '-3')]
                and all(int(row['elapsed']) >= 45 for row in cases[23:25]),
                "resource notification can precede actual late timeout")
        require(cases[25]['name'] == 'ring'
                and numbers(cases[25], 'count overwritten') == (identity['capacity'], 5)
                and int(cases[25]['last']) - int(cases[25]['first']) + 1 == identity['capacity'],
                "bounded ring really overwritten five oldest records")
        require([(row['name'], row['reset'], row['phase'], row['incomplete'])
                 for row in cases[26:]] == [('missing_wait', '0', '0', '1'),
                    ('missing_wait', '1', '0', '1')], "both lost-history paths remain unknown")
        require([numbers(row, 'priority completed markers preserved') for row in rows(text, 'DG|IRQ|')]
                == [(0, 1, 0, 1), (4, 1, 0, 1), (5, 1, 1, 1)],
                "real IRQ rejection/acceptance and exact mask restoration")
    return {"mode": mode, "pass": len(checks), "expected_fail": 0}


def verify_build(detail, identity):
    warnings = [line for line in detail.splitlines() if "warning:" in line]
    expected_warnings = 2 if identity["variant"] == "daily" else 25
    require(len(warnings) == expected_warnings and all(re.search(
        r"Core/Src/main\.c:\d+:\d+: warning: .+ \[-Wunused-(?:function|variable)\]", line)
        for line in warnings), "only known unused demo-main warnings")
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
        "Services/rtos_diag.c", "USB/rtos_usb_device.c", "USB/rtos_usb_osal.c",
        "USB/rtos_usb_msc.c", "Port/GCC/ARM_CM4F_CM7F/port.c")}
    kernel = [(source, line) for source, line in units
              if source.startswith("../RTOS/") and not source.startswith("../RTOS/lib/")]
    require(len(kernel) == 18 and {source for source, _ in kernel} == expected_units,
            "fresh compilation of all 18 RTOS translation units")
    compiles = [line for source, line in units if source.startswith(
        ("../RTOS/", "../Core/Src/", "../hil/fwsrc/"))]
    channels = identity["variant"].startswith("channel_") or identity["channels"]
    require(all(("-DRTOS_CONFIG_USE_CHANNEL=1" in line) == channels for line in compiles),
            "uniform channel configuration")
    require(all(("-DRTOS_CONFIG_USE_DIAGNOSTICS=1" in line) == bool(identity["diagnostics"])
                for line in compiles), "uniform diagnostic enable configuration")
    require(all(("-DRTOS_CONFIG_USE_BASEPRI=0" in line) == (identity["mask"] == "primask")
                for line in compiles), "uniform interrupt mask configuration")
    require(all(("-DRTOS_CONFIG_ASSERT_ENABLED=0" in line) == (identity["assertions"] == 0)
                for line in compiles), "uniform assertion configuration")
    require(all(re.findall(r"(?:^| )-(O[0-3sg])(?: |$)", line) == [identity["optimization"]]
                for line in compiles), "uniform labeled optimization")
    if identity["diagnostics"]:
        capacity = f'-DRTOS_CONFIG_DIAGNOSTIC_CAPACITY={identity["capacity"]}'
        require(all(capacity in line for line in compiles), "uniform diagnostic capacity")
    if identity["variant"].startswith("diagnostic_"):
        fixture = [(source, line) for source, line in units if source.startswith("../hil/fwsrc/")]
        require(fixture and all("-Wextra" in line and "-Werror" in line for _, line in fixture),
                "strict diagnostic fixture compilation")


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
    require(text.count("System init done") == 1, "one actual boot per capture")
    if not identity["fault"]:
        require(not re.search(r"ASSERT FAILED|HardFault_Handler|FAULT\||FAULT REPORT|"
                              r"\*\*\* NESTED FAULT|System halted\.", text), "no fatal fault")
    run = json.loads(read(directory / f"run_{tag}.json"))
    require(run["tag"] == tag and run["terminator"] == identity["terminator"]
            and run["flashed"] is True and run["terminator_seen"] is True
            and run["port"] == identity["port"] and run["baud"] == 115200,
            "actual serial capture identity and successful terminator")
    require(run["failure_lines"] == [], "capture contains no failed verdict")
    elf = directory / f"firmware_{tag}.elf"
    require(digest(elf).upper() == run["sha256"]
            and run["sha256"].lower() == identity["built_elf_sha256"], "flashed ELF identity")
    flash = read(directory / f"flash_{tag}.txt")
    require("066DFF555775514867092907" in flash and "Download verified successfully" in flash,
            "physical expected ST-LINK and successful write verification")
    verify_build(read(directory / "build_detail.txt"), identity)
    if variant.startswith("diagnostic_"):
        return verify_diagnostics(text, identity)
    return verify_legacy(text, variant, identity["mask"])


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    captures = []
    for path in sorted(args.directory.rglob("identity.json")):
        obj = json.loads(read(path))
        if "variant" not in obj:
            continue
        require(obj["passed"] and obj["stage"] == "complete", f"incomplete: {path}")
        for name, expected in obj["artifacts"].items():
            require(digest(path.parent / name) == expected, f"changed artifact: {name}")
        captures.append({"tag": obj["tag"], **verify_capture(path.parent)})
    require(captures, "no accepted captures")
    print(json.dumps({"captures": len(captures), "passed": True, "results": captures}, indent=2))


if __name__ == "__main__":
    main()
