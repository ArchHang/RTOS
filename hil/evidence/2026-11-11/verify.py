"""Verify archived UART bytes and recompute RTOS delivery acceptance metrics."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ARCHIVE = Path(__file__).resolve().parent
ROOT = ARCHIVE.parents[2]
MANIFEST = json.loads((ARCHIVE / "manifest.json").read_text(encoding="utf-8"))
FAILURE = re.compile(r"\|FAIL(?:\||$)|^(?:FAULT\||H743\|FAULT\|)", re.MULTILINE)
EXPECTED_NEGATIVES = {
    "usb_priority_baseline": {
        "UPR|base_after_yield|FAIL|actual=4|expected=9",
        "UPR|effective_after_release|FAIL|actual=4|expected=9",
    },
    "usb_cdc_baseline": {"CDCWR|uint32_wrap|FAIL"},
    "checksum_negative": {
        "STRESS|FAIL|packet_integrity checksum_errors=1",
        "STRESS|FAIL|first=PACKET_CHECKSUM actual=0 expected=6 errors=1",
        "STRESS|FAIL|check=every_successful_identity_validated_exactly_once",
        "STRESS|FAIL|check=all_runtime_contracts",
    },
}


def need(condition, label):
    if not condition:
        raise ValueError(label)


def digest(contents):
    return hashlib.sha256(contents).hexdigest()


def transcript(record):
    path = (ARCHIVE / record["uart"]).resolve()
    need(path.is_relative_to(ARCHIVE / "raw"), f"Invalid UART path: {path}")
    contents = path.read_bytes()
    need(len(contents) == record["uart_bytes"] and digest(contents) == record["uart_sha256"],
         f"UART identity mismatch: {path}")
    return "\n".join(contents.decode("utf-8-sig").splitlines())


def fields(line):
    return dict(re.findall(r"(?:^|[| ])(\w+)=([^| ]+)", line))


def rows(text, prefix):
    return [fields(line) for line in text.splitlines() if line.startswith(prefix)]


def one(text, prefix):
    matches = rows(text, prefix)
    need(len(matches) == 1, f"Expected a unique row: {prefix}")
    return matches[0]


def integers(data, *keys):
    return tuple(int(data[key]) for key in keys)


def pressure(text, record):
    negative = record["outcome"] == "checksum_negative"
    begin = one(text, "STRESS|BEGIN|")
    seconds, workers = integers(begin, "seconds", "workers")
    need(workers == 16 and begin["priorities"] == "5..20", "Stress task configuration")
    need(record["elapsed_seconds"] >= seconds, "Stress completed before configured duration")
    phases = rows(text, "STRESS|PHASE|")
    need([integers(p, "phase", "ticks") for p in phases] ==
         [(phase, seconds * 1000 // 3) for phase in (1, 2, 3)], "Stress phase coverage")
    sources = rows(text, "STRESS|SOURCE|")
    need([int(src["source"]) for src in sources] == list(range(6)), "Stress source ledger")
    for src in sources:
        sent, delivered, prefix, pending = integers(src, "sent", "validated", "prefix", "pending")
        if negative and src["source"] == "0":
            need(sent == delivered + 1 and prefix == 3, "Corrupted packet identity")
        else:
            need(sent == delivered == prefix and pending == 0, "Message conservation")
    samples = rows(text, "STRESS|WORKER|")
    need(len(samples) == 16 and all(int(w["heartbeat"]) > 0 for w in samples), "Task progress")
    high = one(text, "STRESS|HIGHIRQ|")
    entries, natural, forced, triple = integers(high, "entries", "natural", "forced_observed", "triple")
    overlap, spurious = integers(one(text, "STRESS|HIGHIRQ_ACCOUNT|"), "coincident", "spurious")
    need(triple > 0 and entries + overlap == natural + forced + spurious and
         int(high["forced_requests"]) == forced and high["restored"] == "1" and
         high["errors"] == "0", "Hardware IRQ ledger")
    mask = one(text, "STRESS|MASK|")
    control = one(text, "STRESS|CONTROL|")
    need(int(mask["safe_checks"]) > 0 and mask["leaks"] == "0" and
         control["misses"] == control["watchdog_misses"] == "0", "Mask and deadline")
    coverage = one(text, "STRESS|COVERAGE|")
    need(int(coverage["high_full"]) > 0 and int(coverage["recovery_empty"]) > 0,
         "Overload and recovery coverage")
    need(integers(one(text, "STRESS|SUMMARY|"), "pass", "fail", "worker_errors") ==
         ((163, 3, 1) if negative else (165, 0, 0)), "Stress summary")
    need(one(text, "STRESS|DONE|")["status"] == ("-1" if negative else "0"), "Stress verdict")
    return sum(int(src["validated"]) for src in sources)


def check_board(record):
    text = transcript(record)
    tag = record["tag"]
    outcome = record["outcome"]
    failures = {line for line in text.splitlines() if FAILURE.search(line)}
    if outcome == "h743_startup_diagnostic":
        need(len(failures) == 1 and "cfsr=00000400" in text and "H743|BOOT|" not in text,
             "H743 startup failure evidence")
        return text, None
    need(failures == EXPECTED_NEGATIVES.get(outcome, set()), f"Unexpected failure lines in {tag}")
    pattern = record["end_pattern"]
    need(re.search(pattern, text, re.MULTILINE) if pattern else
         ("task 0 running" in text and "task 1 running" in text), f"Missing terminator: {tag}")
    if outcome == "usb_priority_baseline":
        need("UPR|DONE|pass=7|fail=2" in text, "USB priority baseline")
    if outcome == "usb_cdc_baseline":
        need("CDCWR|DONE|pass=3|fail=1" in text and
             "CDCWR|BYTES|uint32_wrap|second=00000000" in text, "USB CDC baseline")
    if "cdc_cursor_fixed" in tag or tag == "integrated_usb_cdc_O2":
        need("CDCWR|DONE|pass=4|fail=0" in text and
             "CDCWR|BYTES|uint32_wrap|second=8596A7B8" in text, "USB CDC fixed")
    if "usb_priority_fixed" in tag or tag == "integrated_usb_priority_O2":
        need("UPR|DONE|pass=9|fail=0" in text, "USB priority fixed")
    if "wrap_ipc" in tag or tag.startswith(("integrated_wrap", "wrap_final_")):
        need(len(re.findall(r"^WRAP\|ROUND\|PASS\|.*\|systick=3\|irq=1\|irq_tick=0$",
                            text, re.MULTILINE)) == 16, "Real SysTick/EXTI wrap rounds")
    if record["board"] == "STM32H743":
        boot = one(text, "H743|BOOT|")
        need(integers(boot, "cpu_hz", "hclk_hz", "pclk1_hz", "pclk2_hz") ==
             (480000000, 240000000, 120000000, 120000000), "H743 clock")
        ready = one(text, "H743|READY|")
        layout = one(text, "H743|LAYOUT|")
        profile = record["profile"]
        need(ready["entry"] == profile["entry"] and ready["systick_load"] == "479999" and
             ready["basepri"] == str(int(profile["critical_mask"] == "basepri")) and
             ready["assert"] == str(int(not profile["disable_assertions"])) and
             all(layout[key] == str(int(profile[key] == "on")) for key in ("icache", "dcache")),
             "H743 runtime profile")
        if tag.startswith("compat_final"):
            need(integers(one(text, "H7C|FP|"), "rounds_a", "rounds_b", "errors_a", "errors_b") ==
                 (64, 64, 0, 0), "H743 FPU rounds")
        if tag.startswith("dma_final"):
            need(integers(one(text, "H7DMA|RESULT|"), "rounds", "irq", "launch_failed") ==
                 (32, 32, 0), "H743 DMA IRQ ledger")
    return text, pressure(text, record) if "STRESS|BEGIN|" in text else None


def check_host():
    steps = {"memory_random": 0, "sem_order_model": 0}
    for record in MANIFEST["host"]:
        text = transcript(record)
        fixture = record["fixture"]
        counter, count = ("operations", 8) if fixture == "memory_random" else ("steps", 4)
        parsed = re.findall(rf"^PASS [^\n]+\b{counter}=(\d+)\b", text, re.MULTILINE)
        need(parsed == ["200000"] * count, f"Host model count: {record['uart']}")
        steps[fixture] += sum(map(int, parsed))
    return steps


def key_results(transcripts):
    def sample(batch, tag, prefix, field="avg"):
        return int(one(transcripts[batch, tag], prefix)[field])

    sem = "SWB|GIVE|waiters=16|high_enrolled=last|"
    tick = "STB|CASE|PASS|path=empty|"
    timeout = "TTB|CASE|PASS|kind=queue|mutexes=8|waiters=16|"
    notify = "TRB|NOTIFY|active=32|path=sweep|"
    f446 = "f446-architecture"
    table = {
        "F446 semaphore": (sample(f446, "base_sem_O2_a", sem),
                           sample(f446, "integrated_sem_O2", sem)),
        "F446 queue timeout": (sample(f446, "base_tick_O2_a", timeout),
                               sample(f446, "integrated_due_O2", timeout)),
        "H743 semaphore": (sample("h743", "sem_wake_bench_base", sem),
                           sample("h743", "sem_wake_bench_final", sem)),
        "H743 queue timeout": (sample("h743", "tick_timeout_bench_base", timeout),
                               sample("h743", "tick_timeout_bench_final", timeout)),
        "H743 round-robin notification": (sample("h743", "task_registry_bench_base", notify),
                                          sample("h743", "task_registry_bench_final", notify)),
        "H743 empty SysTick": (sample("h743", "systick_empty_bench_base", tick, "p50"),
                               sample("h743", "systick_empty_bench_final", tick, "p50")),
    }
    need(tuple(table.values()) == ((491, 278), (3108, 1656), (362, 210), (1668, 1035),
                                  (155, 130), (36, 24)), "Reported A/B cycle values")
    return table


def check_sources():
    for key, paths in MANIFEST["kernel_source_sha256"].items():
        revision = MANIFEST["kernel_revisions"][key]
        need(len(paths) == 64, f"Kernel source inventory: {key}")
        for name, expected in paths.items():
            source = subprocess.check_output(["git", "show", revision + ":" + name], cwd=ROOT)
            need(digest(source) == expected, f"Source provenance mismatch: {key} {name}")


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--check-sources", action="store_true", help="also verify both Git snapshots")
    cli.add_argument("--h743-backup-dir", type=Path, help="optional local three-image restore proof")
    args = cli.parse_args()
    need(MANIFEST["schema"] == 1 and len(MANIFEST["runs"]) == 115 and
         len(MANIFEST["host"]) == 6, "Release evidence inventory")
    transcripts, ledgers = {}, {}
    for record in MANIFEST["runs"]:
        text, count = check_board(record)
        key = (record["batch"], record["tag"])
        need(key not in transcripts, f"Duplicate capture: {key}")
        transcripts[key] = text
        if count is not None:
            ledgers[record["tag"]] = count
    host = check_host()
    table = key_results(transcripts)
    need(ledgers == {"final_production_soak900": 1973927,
                     "integrated_soak180_O3_primask_noassert": 394891,
                     "final_production_cap1": 65298, "final_production_cap8": 65942,
                     "final_production_cap32": 65960, "final_production_negative": 13385,
                     "integrated_pressure120_O2": 263311,
                     "integrated_pressure30_O3_primask": 65935,
                     "pressure_final120_AXI_O2": 263186,
                     "pressure_final30_DTCM_O3_PRIMASK": 65803}, "Reported pressure message counts")
    if args.check_sources:
        check_sources()
    if args.h743_backup_dir:
        images = [(args.h743_backup_dir / name).read_bytes() for name in
                  ("original_flash.bin", "original_flash_second_read.bin", "restored_readback.bin")]
        need(len(images[0]) == 2097152 and images[0] == images[1] == images[2], "H743 restore proof")
    print(json.dumps({"board_captures": len(transcripts), "host_steps": host,
                      "accepted_cycles": table, "validated_pressure_messages": ledgers}, indent=2))


if __name__ == "__main__":
    main()
