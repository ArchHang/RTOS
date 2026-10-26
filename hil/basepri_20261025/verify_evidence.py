#!/usr/bin/env python3
"""Independently verify this dated F446 HIL evidence without opening hardware.

Python 3.9+, standard library only. The default is strict: every final batch,
source manifest and daily restoration must exist. Only complete strict success
writes verification_summary.json. --in-progress checks available evidence and
reports missing work, but never writes a success artifact. Historical failures
are accepted only with their specific, independently checked failure signatures.

ELFs are optional archival artifacts. If present their hashes must match capture
metadata; final mask/blocking ELFs also undergo an independent exit-ISB check.
Logs, flash traces and run metadata remain mandatory. No subprocess, network,
debug probe, serial port, package or external Python module is used.
"""

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import itertools
import json
from pathlib import Path
import re
import struct
import sys


MASKS = (0, 0x20, 0x50, 0x80, 0xF0)
OPTIMIZATIONS = ("O0", "O2", "O3")
BLOCK_KINDS = {"sem", "queue_recv", "queue_send", "event", "notify"}
ISB_OPCODE = bytes.fromhex("bf f3 6f 8f")
FAIL_PATTERN = re.compile(r"\|FAIL(?:\||\r?$)", re.MULTILINE)
EXPECTED_MODULES = (
    "", "rtos_sem.c", "rtos_queue.c", "rtos_event.c", "rtos_task.c",
    "rtos_heap.c", "rtos_heap.c", "rtos_heap.c", "rtos_perf.c",
    "rtos_perf.c", "rtos_perf.c", "rtos_perf.c", "rtos_perf.c",
    "rtos_usb.c", "rtos_usb.c", "port.c", "port.c",
)


class EvidenceError(Exception):
    """Existing evidence is malformed or contradicts an acceptance invariant."""


class PendingEvidence(Exception):
    """Required evidence has not been produced yet."""


def require(condition, message):
    if not condition:
        raise EvidenceError(message)


def read_text(path):
    # English machine fields survive replacement of legacy progress-bar bytes.
    return path.read_text(encoding="utf-8-sig", errors="replace")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def fields(line):
    result = {}
    for key, value in re.findall(r"\b([A-Za-z_][A-Za-z_0-9]*)=([^\s|]+)", line):
        require(key not in result, "duplicate field %s: %s" % (key, line))
        if re.fullmatch(r"-?\d+", value):
            value = int(value, 10)
        elif re.fullmatch(r"0[xX][0-9a-fA-F]+", value):
            value = int(value, 16)
        result[key] = value
    return result


def number(row, key):
    require(key in row and type(row[key]) is int, "missing integer field %s" % key)
    return row[key]


def rows(text, prefix):
    return [line.strip() for line in text.splitlines() if line.startswith(prefix)]


def one(text, prefix):
    found = rows(text, prefix)
    require(len(found) == 1, "expected one %s record, found %d" % (prefix, len(found)))
    return found[0]


def zero_fields(row, keys):
    for key in keys:
        require(number(row, key) == 0, "%s must be zero: %r" % (key, row))


def elf_symbols(path):
    """Return defined ELF32 little-endian ARM symbols and their section bytes."""
    data = path.read_bytes()
    require(len(data) >= 52 and data[:6] == b"\x7fELF\x01\x01", "not ELF32 LE: " + path.name)
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    require(header[2] == 40, "not an ARM ELF: " + path.name)
    shoff, shsize, shcount = header[6], header[11], header[12]
    require(shsize >= 40 and shcount > 0 and shoff + shsize * shcount <= len(data),
            "invalid section table: " + path.name)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + index * shsize)
                for index in range(shcount)]
    result = []
    for section in sections:
        if section[1] not in (2, 11):
            continue
        require(section[6] < len(sections) and section[9] >= 16, "invalid symbol table")
        strings_section = sections[section[6]]
        strings = data[strings_section[4]:strings_section[4] + strings_section[5]]
        for offset in range(section[4], section[4] + section[5], section[9]):
            name_offset, value, size, info, unused, section_index = struct.unpack_from(
                "<IIIBBH", data, offset)
            if section_index == 0 or name_offset >= len(strings):
                continue
            name = strings[name_offset:].split(b"\0", 1)[0].decode("ascii", "replace")
            code = b""
            if section_index < len(sections):
                target = sections[section_index]
                start = target[4] + (value & ~1) - target[3]
                if 0 <= start <= len(data) and size <= len(data) - start:
                    code = data[start:start + size]
            result.append((name, info & 15, code))
    require(result, "ELF has no defined symbols: " + path.name)
    return result


class Verifier:
    def __init__(self, evidence, project, in_progress):
        self.evidence = evidence
        self.project = project
        self.in_progress = in_progress
        self.errors = []
        self.pending = []
        self.validated = []
        self.captures = {}
        self.details = {}
        self.missing_elfs = []

    def file(self, name):
        path = self.evidence / name
        if not path.is_file():
            raise PendingEvidence(name)
        return path

    def json(self, name):
        return json.loads(read_text(self.file(name)))

    def check(self, name, callback):
        try:
            result = callback()
            self.validated.append(name)
            if result is not None:
                self.details[name] = result
        except PendingEvidence as error:
            self.pending.append({"check": name, "missing": str(error)})
        except (EvidenceError, OSError, ValueError, KeyError, TypeError, IndexError,
                struct.error) as error:
            self.errors.append({"check": name, "error": str(error)})

    def capture(self, tag, require_exit_isb=False):
        log = self.file("log_%s.txt" % tag)
        meta = self.json("run_%s.json" % tag)
        flash_path = self.file("flash_%s.txt" % tag)
        text = read_text(log)
        require(isinstance(meta, dict) and meta.get("tag") == tag, "capture tag mismatch")
        require(meta.get("port") == "COM3" and meta.get("baud") == 115200,
                "capture did not use the agreed COM3/115200 interface")
        require(meta.get("flashed") is True, "capture was not flashed in this invocation")
        require(meta.get("terminator_seen") is True, "capture did not see its terminator")
        terminator = meta.get("terminator")
        require(isinstance(terminator, str), "invalid terminator metadata")
        if tag != "final_daily":
            require(bool(terminator), "test capture requires a terminal marker")
        require(not terminator or re.search(terminator, text) is not None,
                "recorded terminator is absent from raw log")
        start = datetime.fromisoformat(meta["started"])
        end = datetime.fromisoformat(meta["finished"])
        require(start.tzinfo is not None and end >= start, "invalid capture timestamps")
        actual_failures = [line.strip() for line in text.splitlines()
                           if FAIL_PATTERN.search(line)]
        require(meta.get("failure_lines") == actual_failures,
                "run metadata failure_lines differs from raw log")
        flash = read_text(flash_path)
        for marker in ("NUCLEO-F446RE", "STM32F446", "Cortex-M4", "0x08000000",
                       "File download complete", "Download verified successfully"):
            require(marker in flash, "flash trace lacks target/download/verification: " + marker)
        require("ST-LINK SN" in flash and "Memory Programming" in flash,
                "missing real SWD programmer trace")
        expected_hash = meta.get("sha256", "")
        require(re.fullmatch(r"[A-Fa-f0-9]{64}", expected_hash) is not None,
                "invalid firmware hash")
        firmware = self.evidence / ("firmware_%s.elf" % tag)
        exit_isb_checked = False
        if firmware.is_file():
            require(sha256(firmware) == expected_hash.upper(), "firmware hash mismatch")
            if require_exit_isb:
                symbols = elf_symbols(firmware)
                exits = [code for name, kind, code in symbols
                         if name == "rtos_port_exit_critical" and kind == 2]
                if exits:
                    require(all(ISB_OPCODE in code for code in exits),
                            "final ELF contains a critical-exit function without ISB")
                else:
                    sem_code = [code for name, kind, code in symbols
                                if name == "rtos_sem_take" and kind == 2]
                    require(len(sem_code) == 1, "cannot locate sem_take for final ISB verification")
                    minimum = 1 if "primask" in tag else 2
                    require(sem_code[0].count(ISB_OPCODE) >= minimum,
                            "final inlined sem_take lacks exit ISB (old atomic build?)")
                exit_isb_checked = True
        else:
            self.missing_elfs.append(tag)
        self.captures[tag] = {"sha256": expected_hash.upper(), "port": "COM3",
                              "baud": 115200, "flashed": True,
                              "duration_seconds": round((end - start).total_seconds(), 3),
                              "elf_hash_checked": firmware.is_file(),
                              "exit_isb_checked": exit_isb_checked,
                              "log_sha256": sha256(log), "flash_sha256": sha256(flash_path)}
        return text

    def index(self, filename, expected_tags):
        entries = self.json(filename)
        require(isinstance(entries, list), "results index must be a JSON array")
        tags = [entry["tag"] for entry in entries]
        require(len(tags) == len(set(tags)), "duplicate result tag")
        require(set(tags) <= set(expected_tags), "unexpected tags in " + filename)
        missing = sorted(set(expected_tags) - set(tags))
        if missing:
            self.pending.append({"check": filename, "missing_tags": missing})
        for entry in entries:
            require(entry.get("passed") is True, "runner reported failure: " + entry["tag"])
        return entries

    def matrix(self, tag, baseline=False):
        text = self.capture(tag, require_exit_isb=not baseline)
        meta = fields(one(text, "BP|META|"))
        require(number(meta, "use_basepri") == int("primask" not in tag),
                "matrix tag and executing mask configuration disagree")
        if not baseline:
            require(number(meta, "asserts") == int("_a1_" in tag),
                    "matrix tag and executing assertion configuration disagree")
        require(number(meta, "threshold") == 0x50, "unexpected syscall threshold")
        cases = rows(text, "BP|CASE|")
        require(len(cases) == 600, "matrix must contain exactly 600 raw cases")
        expected = set(itertools.product(range(1, 6), (0, 4, 5, 15), MASKS, (0, 1), (1, 2, 8)))
        combinations = set()
        identifiers = set()
        failures = []
        for line in cases:
            row = fields(line)
            identity = number(row, "id")
            combo = tuple(number(row, key) for key in ("stage", "prio", "in_bp", "in_pm", "depth"))
            require(identity not in identifiers and combo not in combinations, "duplicate matrix case")
            identifiers.add(identity)
            combinations.add(combo)
            stage, priority, mask, primask, depth = combo
            expected_failure = baseline and stage in (2, 4) and priority in (0, 4) \
                and mask in (0, 0x80, 0xF0) and primask == 0
            require(line.startswith("BP|CASE|%s|" % ("FAIL" if expected_failure else "PASS")),
                    "matrix PASS/FAIL signature does not match expected transition")
            require(number(row, "out_pm") == primask and number(row, "nest") == 0,
                    "PRIMASK/nesting was not restored")
            require(number(row, "out_bp") == (0x50 if expected_failure else mask),
                    "BASEPRI result mismatch")
            for key in ("hook", "irq"):
                require(number(row, key) == 1, "real hook/IRQ occurrence not proven")
            require(number(row, "irq_errors") == 0, "IRQ pair damaged its incoming state")
            if expected_failure:
                require(number(row, "hook_pm") == 0 and number(row, "in_hook") == 1,
                        "baseline did not enter IRQ inside the vulnerable transaction")
                failures.append(combo)
            elif not baseline:
                if "basepri" in tag or stage < 5:
                    require(number(row, "hook_pm") == 1 and number(row, "in_hook") == 0,
                            "IRQ entered protected metadata transaction")
                held = primask == 1 or (mask != 0 and (priority << 4) >= mask)
                require(number(row, "before_repair") == int(not held),
                        "IRQ violated inherited mask or failed to run after release")
        require(identifiers == set(range(1, 601)) and combinations == expected,
                "matrix does not cover all unique input combinations")
        summary = fields(one(text, "BP|SUMMARY|"))
        matrix = fields(one(text, "BP|MATRIX|"))
        expected_fails = 36 if baseline else 0
        require(number(matrix, "cases") == 600 and number(matrix, "fail") == expected_fails,
                "matrix aggregate mismatch")
        require(number(summary, "cases") == 600 and number(summary, "matrix_fail") == expected_fails,
                "summary matrix count mismatch")
        checks = rows(text, "BP|CHECK|")
        passed_checks = sum(line.endswith("|PASS") for line in checks)
        failed_checks = [line for line in checks if line.endswith("|FAIL")]
        require(number(summary, "pass") == 600 - len(failures) + passed_checks,
                "matrix/check PASS aggregate is inconsistent")
        require(number(summary, "fail") == len(failures) + len(failed_checks),
                "matrix/check FAIL aggregate is inconsistent")
        if baseline:
            require(failed_checks == ["BP|CHECK|periodic_no_mask_leak|FAIL"],
                    "unexpected historical baseline failure")
        else:
            require(not failed_checks and not FAIL_PATTERN.search(text), "unexpected final matrix failure")
        stress = fields(one(text, "BP|STRESS|"))
        require(number(stress, "pairs") == number(stress, "requested") == 1000000,
                "asynchronous pair stress did not complete")
        require(number(stress, "irq") >= 1000 and number(stress, "ticks") > 0,
                "real asynchronous interrupt/tick load missing")
        zero_fields(stress, ("irq_errors",))
        require(number(stress, "thread_leaks") == (2680 if baseline else 0),
                "unexpected asynchronous mask leakage")
        progress = fields(one(text, "BP|PROGRESS|"))
        require(number(progress, "requests") == number(progress, "replies") == 1000,
                "peer IPC progress incomplete")
        require(number(progress, "peer_errors") == 0 and number(progress, "switches") > 0,
                "peer context switching failed")
        bodies = rows(text, "BP|BODY|")
        require(len(bodies) == 4, "missing body-preemption controls")
        for line in bodies:
            row = fields(line)
            want = int("primask" not in tag and number(row, "prio") < 5)
            require(number(row, "ran_inside") == number(row, "expected") == want,
                    "body IRQ delivery differs from mask model")
            require(number(row, "final_bp") == number(row, "nest") == 0
                    and number(row, "irq") == 1, "body did not restore/deliver IRQ")
            if want:
                require(number(row, "irq_bp") == 0x50, "missing expected nonzero ISR sample")
        require(number(fields(one(text, "BP|DONE|")), "status") == int(baseline),
                "matrix terminal status mismatch")
        return {"cases": 600, "matrix_failures": len(failures), "async_pairs": 1000000,
                "async_irqs": stress["irq"], "mask_leaks": stress["thread_leaks"],
                "summary": one(text, "BP|SUMMARY|")}

    def blocking(self, tag, historical=False):
        text = self.capture(tag, require_exit_isb=not historical)
        meta = fields(one(text, "BLOCK|META|"))
        require(number(meta, "rounds_per_kind") == 100 and number(meta, "timeout_ticks") == 2,
                "blocking fixture bounds changed")
        zero_fields(meta, ("givers", "setters", "notifiers", "port_hooks"))
        kinds = {}
        for line in rows(text, "BLOCK|KIND|"):
            kind = line.split("|")[2]
            require(kind not in kinds, "duplicate blocking kind")
            row = fields(line)
            fail = 100 if historical and kind == "sem" else 0
            require(number(row, "attempts") == 100 and number(row, "fail") == fail,
                    "blocking return count mismatch: " + kind)
            require(number(row, "false_ok") == number(row, "unexpected_ret") == fail,
                    "unexpected return signature: " + kind)
            zero_fields(row, ("early", "wait_errors", "resource_errors", "state_errors", "mask_errors"))
            require(number(row, "elapsed_max") >= number(row, "elapsed_min") >= 2,
                    "a blocking operation returned before its timeout")
            kinds[kind] = row
        require(set(kinds) == BLOCK_KINDS, "incomplete five-kind blocking matrix")
        if historical:
            samples = rows(text, "BLOCK|CASE|FAIL|")
            require(len(samples) == 10, "historical reproduction needs first ten concrete samples")
            for line in samples:
                row = fields(line)
                require(row.get("kind") == "sem" and number(row, "ret") == 0
                        and number(row, "expected") == number(row, "wait_result") == -3
                        and number(row, "timed_out") == 1 and number(row, "elapsed") >= 2,
                        "historical failure does not prove false OK after real timeout")
                zero_fields(row, ("basepri", "primask", "nest"))
            require(not rows(text, "BLOCK|CASE|PASS|"), "unexpected historical sample format")
        else:
            require(not FAIL_PATTERN.search(text), "final blocking fixture reported failure")
        summary = fields(one(text, "BLOCK|SUMMARY|"))
        failures = sum(row["fail"] for row in kinds.values())
        checks = rows(text, "BLOCK|CHECK|")
        require(all(line.endswith("|PASS") for line in checks), "blocking setup/cleanup failed")
        require(number(summary, "attempts") == 500 and number(summary, "fail") == failures
                and number(summary, "pass") == 500 - failures + len(checks),
                "blocking summary inconsistent with per-kind and check counters")
        high = fields(one(text, "BLOCK|HIGHIRQ|"))
        require(number(high, "entries") > 0 and number(high, "natural") > 0
                and number(high, "restored") == 1, "blocking high-IRQ load/restoration missing")
        zero_fields(high, ("errors", "mismatch", "pending_stop", "pending_restored"))
        if "reload_timeouts" in high:
            zero_fields(high, ("reload_timeouts", "sr"))
        require(number(fields(one(text, "BLOCK|DONE|")), "status") == int(historical),
                "blocking terminal status mismatch")
        return {"attempts": 500, "false_ok": failures,
                "kinds": sorted(kinds), "summary": one(text, "BLOCK|SUMMARY|")}

    def guard(self, tag):
        match = re.fullmatch(r"guard_c(\d+)_p(\d+)", tag)
        case, priority = map(int, match.groups())
        text = self.capture(tag)
        arm = fields(one(text, "BP|GUARD_ARM|"))
        result_line = one(text, "BP|GUARD|")
        result = fields(result_line)
        require(result_line.startswith("BP|GUARD|PASS|") and arm.get("case") == case
                and arm.get("prio") == priority and result.get("case") == case
                and result.get("prio") == priority, "guard identity/status mismatch")
        assertions = rows(text, "BP|ASSERT|")
        should_assert = priority < 5 or case > 14
        require(len(assertions) == int(should_assert), "wrong number of expected assertions")
        if should_assert:
            row = fields(assertions[0])
            require(number(row, "case") == case and number(row, "ipsr") == 71
                    and number(row, "line") > 0, "assertion did not come from real TIM7 case")
            require(str(row.get("file", "")).replace("\\", "/").endswith("/" + EXPECTED_MODULES[case]),
                    "assertion came from wrong production module")
            condition = assertions[0].split(" condition=", 1)[-1]
            if case <= 14:
                require("rtos_port_isr_priority()" in condition
                        and ">= RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY" in condition,
                        "arbitrary assertion is not a priority-contract success")
            else:
                require("nesting > 0U" in condition and "nesting < UINT32_MAX" in condition,
                        "arbitrary assertion is not an invalid-nesting success")
            require(result.get("reason") == "assertion_source_and_condition_checked",
                    "expected assertion was not independently classified")
        else:
            require(result.get("reason") == "legal_call_returned_or_missing_expected_assertion",
                    "legal priority-5 operation did not return")
        require(not FAIL_PATTERN.search(text)
                and number(fields(one(text, "BP|DONE|")), "status") == 0,
                "guard terminal result failed")
        return {"case": case, "priority": priority, "expected_assertion": should_assert}

    def nesting(self, tag):
        text = self.capture(tag)
        cases = rows(text, "NEST|CASE|")
        expected = set(itertools.product(("overflow_enter", "underflow_exit"), MASKS, (0, 1)))
        seen, identifiers = set(), set()
        for line in cases:
            require(line.startswith("NEST|CASE|PASS|"), "invalid nesting input changed state")
            row = fields(line)
            key = (row.get("operation"), number(row, "in_bp"), number(row, "in_pm"))
            require(key not in seen and number(row, "id") not in identifiers, "duplicate nesting case")
            seen.add(key)
            identifiers.add(row["id"])
            expected_count = 0xFFFFFFFF if key[0] == "overflow_enter" else 0
            require(number(row, "out_bp") == row["in_bp"] and number(row, "out_pm") == row["in_pm"]
                    and number(row, "count") == number(row, "expected_count") == expected_count
                    and number(row, "slot") == number(row, "expected_slot") == 0xA5A55A5A
                    and number(row, "returned") == 1 and row.get("repair") == "fixture_only",
                    "assert-off rejection did not preserve incoming state")
        require(seen == expected and identifiers == set(range(1, 21)), "missing nesting combinations")
        summary = fields(one(text, "NEST|SUMMARY|"))
        require(number(summary, "pass") == number(summary, "cases") == 20,
                "invalid nesting summary count")
        zero_fields(summary, ("fail", "asserts"))
        require(number(fields(one(text, "NEST|DONE|")), "status") == 0
                and not FAIL_PATTERN.search(text), "nesting capture did not complete")
        return {"cases": 20, "assertions_enabled": False}

    def functional(self, tag):
        text = self.capture(tag)
        require(not FAIL_PATTERN.search(text), "functional regression reported failure")
        if tag == "final_audit":
            expected = {"SCHEDTASK": 25, "IPC": 15, "MEMTIMER": 23, "USBPORT": 54}
            for group, count in expected.items():
                summary = fields(one(text, "AT|%s|SUMMARY|" % group))
                successes = [line for line in rows(text, "AT|%s|" % group) if "|PASS" in line]
                require(number(summary, "pass") == len(successes) == count
                        and number(summary, "fail") == 0, "audit subgroup count mismatch")
            one(text, "AT|DONE")
            return {"pass": 117, "fail": 0}
        if tag == "final_selftest":
            summary = fields(one(text, "ST|SUMMARY|"))
            actual_pass = sum("|PASS" in line for line in rows(text, "ST|"))
            actual_info = sum("|INFO" in line for line in rows(text, "ST|")
                              if not line.startswith("ST|META|"))
            require(number(summary, "pass") == actual_pass == 432
                    and number(summary, "info") == actual_info == 10
                    and number(summary, "total") == 442 and number(summary, "fail") == 0,
                    "selftest is not the final 432/0/10/442 result")
            require(one(text, "ST|SUMMARY|").endswith("|END"), "missing selftest END")
            return {"pass": 432, "fail": 0, "info": 10, "total": 442}
        if tag == "final_boundary":
            summary = fields(one(text, "BT|SUMMARY|"))
            require(number(summary, "pass") == 103 and number(summary, "fail") == 0
                    and sum("|PASS" in line for line in rows(text, "BT|")) == 103,
                    "boundary count mismatch")
            return {"pass": 103, "fail": 0}
        if tag == "final_lowlevel":
            for prefix, count in (("LC", 24), ("PC", 22)):
                summary = fields(one(text, prefix + "|SUMMARY|"))
                require(number(summary, "pass") == count and number(summary, "fail") == 0
                        and sum("|PASS" in line for line in rows(text, prefix + "|")) == count,
                        "low-level subgroup mismatch")
            detail = fields(one(text, "LC|DETAIL|"))
            require(all(number(detail, key) == 10000 for key in ("rounds_a", "rounds_b", "integer")),
                    "FPU/integer context exercise incomplete")
            zero_fields(detail, ("errors_a", "errors_b"))
            one(text, "BM|DONE|")
            return {"context_pass": 24, "perf_probe_pass": 22, "fail": 0}
        raise EvidenceError("unknown functional batch")

    def benchmark(self, tag):
        text = self.capture(tag)
        require(not FAIL_PATTERN.search(text), "benchmark capture failed")
        found = {}
        for line in rows(text, "BM|"):
            match = re.match(r"BM\|(\d+)\|([^|]+)\|", line)
            if match is None:
                continue
            index, name = int(match.group(1)), match.group(2)
            require(index not in found, "duplicate benchmark row")
            row = fields(line)
            require(number(row, "N") > 0 and 0 < number(row, "min")
                    <= number(row, "avg") <= number(row, "max"), "invalid cycle statistics")
            found[index] = {"name": name, **row}
        require(set(found) == set(range(8)), "BM0..BM7 incomplete")
        want = 24 if tag == "baseline_nohook_bench" else 40
        require(found[0]["name"] == "critical_pair" and found[0]["avg"] == want,
                "unexpected dated no-hook critical-pair measurement")
        one(text, "BM|DONE|")
        return {"critical_pair_cycles": want, "benchmarks": found,
                "scope": "same dated F446 O2 image measurement; not WCET"}

    def production(self, tag, capacity, seconds, seed, negative=False):
        text = self.capture(tag)
        begin = fields(one(text, "STRESS|BEGIN|"))
        require(number(begin, "seed") == seed and number(begin, "seconds") == seconds
                and number(begin, "queue_capacity") == capacity and number(begin, "workers") == 16
                and begin.get("priorities") == "5..20" and number(begin, "synthetic") == 1
                and number(begin, "inject") == int(negative), "wrong production configuration")
        require(self.captures[tag]["duration_seconds"] >= seconds, "capture shorter than requested stress")
        workers = {}
        for line in rows(text, "STRESS|WORKER|"):
            row = fields(line)
            index = number(row, "id")
            require(index not in workers and 0 <= index < 16, "duplicate/invalid worker ID")
            require(number(row, "priority") == index + 5 and number(row, "heartbeat") > 0
                    and number(row, "switches") > 0 and number(row, "stack_free_words") >= 32,
                    "worker did not make real progress or lacked stack margin")
            if index != 13:
                require(number(row, "completed") > 0, "worker role has no result")
            workers[index] = row
        require(set(workers) == set(range(16)), "not all sixteen workers reported")
        sources = {}
        for line in rows(text, "STRESS|SOURCE|"):
            row = fields(line)
            index = number(row, "source")
            require(index not in sources and 0 <= index < 6 and number(row, "sent") > 0,
                    "duplicate/missing/empty message source")
            if negative and index == 0:
                require(number(row, "sent") == number(row, "validated") + 1
                        and number(row, "prefix") == 3 and number(row, "pending") != 0,
                        "negative ledger is not exactly the one corrupted source-0 sequence")
            else:
                require(number(row, "sent") == number(row, "validated") == number(row, "prefix")
                        and number(row, "pending") == 0, "message identity conservation failed")
            sources[index] = row
        require(set(sources) == set(range(6)), "six-source message ledger incomplete")
        sent = sum(row["sent"] for row in sources.values())
        validated = sum(row["validated"] for row in sources.values())
        consumed = sum(workers[index]["completed"] for index in range(5, 9))
        require(consumed == sent and sent - validated == int(negative),
                "consumer/source message totals do not conserve")
        for index in range(4):
            require(workers[index + 1]["completed"] == sources[index]["sent"],
                    "task producer/ledger counters disagree")
        semaphore = fields(one(text, "STRESS|SEMAPHORE|"))
        require(number(semaphore, "accepted") == number(semaphore, "taken0")
                + number(semaphore, "taken1") + number(semaphore, "pending"),
                "semaphore token conservation failed")
        require(number(semaphore, "taken0") > 0 and number(semaphore, "taken1") > 0
                and workers[9]["completed"] == semaphore["taken0"]
                and workers[10]["completed"] == semaphore["taken1"],
                "shared semaphore consumers disagree with worker results")
        coverage = fields(one(text, "STRESS|COVERAGE|"))
        for key, minimum in (("queue_recv_waiters", 4), ("queue_send_waiters", 1),
                             ("sem_waiters", 2), ("lease_waiters", 1), ("event_waiters", 3),
                             ("mutex_waiters", 1), ("pi_observed", 1), ("high_full", 1),
                             ("recovery_empty", 1)):
            require(number(coverage, key) >= minimum, "coverage path was not exercised: " + key)
        control = fields(one(text, "STRESS|CONTROL|"))
        zero_fields(control, ("misses", "watchdog_misses"))
        require(number(control, "response_max") <= number(control, "deadline_ticks"),
                "synthetic deadline exceeded")
        irq = fields(one(text, "STRESS|IRQ|"))
        zero_fields(irq, ("errors",))
        require(number(irq, "depth_max") >= 2 and number(irq, "nested") > 0
                and number(irq, "forced") > 0, "real syscall-IRQ nesting missing")
        delivered = 0
        for index in (0, 1):
            count = number(irq, "sent%d" % index) + number(irq, "rejected%d" % index)
            require(number(irq, "entries%d" % index) >= count > 0,
                    "IRQ delivery count exceeds actual entries")
            require(number(irq, "sent%d" % index) == sources[index + 4]["sent"],
                    "IRQ source ledger mismatch")
            delivered += count
        require(delivered == semaphore["accepted"] + number(semaphore, "overflow"),
                "IRQ delivery/sem give classification counters disagree")
        high = fields(one(text, "STRESS|HIGHIRQ|"))
        require(number(high, "entries") > 0 and number(high, "natural") > 0
                and number(high, "triple") > 0 and number(high, "nested") >= high["triple"],
                "priority-zero real/nested/triple load missing")
        require(number(high, "forced_requests") == number(high, "forced_observed")
                == irq["nested"] and high["forced_requests"] > 0,
                "forced high-IRQ request/observation conservation failed")
        require(max(high["natural"], high["forced_observed"]) <= high["entries"]
                <= high["natural"] + high["forced_observed"],
                "high-IRQ entries cannot represent natural/forced event overlap")
        zero_fields(high, ("errors", "running", "mismatch", "restore_pending", "sr", "reload_timeouts"))
        require(number(high, "restored") == 1 and high.get("stop_pending") == "0/0",
                "high IRQ peripheral/mask state was not restored")
        require(re.fullmatch(r"[1-9]\d*/[1-9]\d*", str(high.get("reload_polls", ""))) is not None,
                "timer reload was not independently observed")
        masks = fields(one(text, "STRESS|MASK|"))
        require(number(masks, "safe_checks") > 0 and number(masks, "leaks") == 0,
                "thread mask leakage or no safe-point sampling")
        heap = fields(one(text, "STRESS|HEAP|"))
        require(number(heap, "before") == number(heap, "after")
                and number(heap, "max_before") == number(heap, "max_after"), "fixture changed heap")
        checks = rows(text, "STRESS|CHECK|")
        require(all(line.endswith(" status=PASS") for line in checks), "malformed production check")
        names = Counter(str(fields(line).get("name")) for line in checks)
        for name in ("worker_stack_margin", "worker_stack_magic_and_low_watermark",
                     "worker_is_suspended", "worker_priority_restored", "worker_role_has_real_result",
                     "static_worker_delete"):
            require(names[name] == 16, "worker lifecycle check coverage incomplete: " + name)
        require(names["all_sixteen_workers_progress_each_phase"] == 3,
                "not all three phases demonstrated worker progress")
        failures = [line for line in text.splitlines() if FAIL_PATTERN.search(line)]
        failed_checks = [line for line in failures if line.startswith("STRESS|FAIL|check=")]
        if negative:
            expected_checks = {"STRESS|FAIL|check=every_successful_identity_validated_exactly_once",
                               "STRESS|FAIL|check=all_runtime_contracts"}
            require(set(failed_checks) == expected_checks and len(failed_checks) == 2,
                    "negative control contains an unrelated failed check")
            require(len(rows(text, "STRESS|INJECT|checksum_corruption")) == 1,
                    "missing single corruption injection marker")
            for line in failures:
                require(line in expected_checks
                        or line == "STRESS|FAIL|packet_integrity checksum_errors=1"
                        or line == "STRESS|FAIL|first=PACKET_CHECKSUM actual=0 expected=6 errors=1",
                        "negative control includes another failure cause: " + line)
            require(any("packet_integrity checksum_errors=1" in line for line in failures)
                    and any("first=PACKET_CHECKSUM" in line for line in failures),
                    "negative payload corruption was not diagnosed")
        else:
            require(not failures and not rows(text, "STRESS|INJECT|"), "normal production run failed")
        summary = fields(one(text, "STRESS|SUMMARY|"))
        require(number(summary, "pass") == len(checks) == (163 if negative else 165)
                and number(summary, "worker_errors") == int(negative)
                and number(summary, "fail") == len(failed_checks) + int(negative),
                "production summary/check/error counters disagree")
        require(number(fields(one(text, "STRESS|DONE|")), "status") == (-1 if negative else 0),
                "production terminal status mismatch")
        phases = [fields(line) for line in rows(text, "STRESS|PHASE|")]
        require([row.get("phase") for row in phases] == [1, 2, 3]
                and sum(number(row, "ticks") for row in phases) == seconds * 1000,
                "phase time budgets do not sum to requested duration")
        for line in rows(text, "STRESS|PROGRESS|"):
            require(number(fields(line), "errors") == int(negative), "unexpected progress error count")
        return {"seconds": seconds, "queue_capacity": capacity, "seed": seed,
                "negative_control": negative, "successful_messages": validated,
                "consumed_messages": consumed, "semaphore": semaphore,
                "coverage": coverage, "high_irq": high, "safe_mask_checks": masks["safe_checks"],
                "non_delivery_irq_entries": irq["entries0"] + irq["entries1"] - delivered,
                "summary": one(text, "STRESS|SUMMARY|")}

    def compile_checks(self):
        sources = {"rtos_heap": "RTOS/Core/Src/rtos_heap.c", "rtos_perf": "RTOS/Core/Src/rtos_perf.c",
                   "rtos_usb": "RTOS/Core/Src/rtos_usb.c", "port": "RTOS/Port/GCC/ARM_CM7/port.c"}
        expected = {"m%d_a%d_%s_%s" % (mask, assertions, optimization, source)
                    for mask, assertions, optimization, source in itertools.product(
                        (0, 1), (0, 1), OPTIMIZATIONS, sources)} | {"cm7_port_compile_only"}
        entries = self.index("compile_results.json", expected)
        for entry in entries:
            require(entry.get("warnings") == 0, "strict compiler warnings were reported")
            tag = entry["tag"]
            filename = "compile_cm7_port_only.txt" if tag == "cm7_port_compile_only" else "compile_%s.txt" % tag
            output = read_text(self.file(filename)).strip()
            require(not output, "strict compilation has diagnostics: " + filename)
            if tag == "cm7_port_compile_only":
                require(entry.get("hardware_test") is False, "Cortex-M7 must be marked compile-only")
            else:
                source_name = tag.split("_", 3)[3]
                require(entry.get("source") == sources[source_name], "compile source/tag mismatch")
        return {"compilations": len(entries), "arm_cm7_hardware_test": False,
                "m0_m1_mean": "USE_BASEPRI=0/1, not Cortex-M0/M1 CPU targets"}

    def source_manifest(self):
        manifest = self.json("sources_final.json")
        require(isinstance(manifest, list) and manifest, "empty final source manifest")
        seen = set()
        for entry in manifest:
            relative = str(entry["path"]).replace("\\", "/")
            path = (self.project / relative).resolve()
            require(path.is_relative_to(self.project) and relative not in seen,
                    "duplicate or escaping manifest path")
            seen.add(relative)
            require(path.is_file() and sha256(path) == str(entry["sha256"]).upper(),
                    "final source hash mismatch: " + relative)
        required = {str(path.relative_to(self.project)).replace("\\", "/")
                    for directory in (self.project / "RTOS", self.project / "hil/fw")
                    for path in directory.rglob("*") if path.is_file() and path.suffix in (".c", ".h")}
        required |= {"Core/Src/main.c", "hil/build_fw.ps1", "hil/run_hil.ps1", "hil/restore_project.ps1",
                     "hil/repro_all.ps1", "hil/basepri_20261025/run_matrix.ps1",
                     "hil/basepri_20261025/run_regressions.ps1", "hil/basepri_20261025/verify_evidence.py"}
        require(required <= seen, "final manifest omits sources/scripts: " + ", ".join(sorted(required - seen)))
        port = read_text(self.project / "RTOS/Core/Inc/rtos_port.h")
        exit_body = re.search(r"static inline void rtos_port_exit_critical\(void\)\s*\{(.*?)\n\}", port, re.S)
        require(exit_body is not None and exit_body.group(1).rfind('" isb ')
                > exit_body.group(1).rfind('" msr primask,'), "final port is missing final exit ISB")
        snapshot_count = 0
        for snapshot in sorted(self.evidence.glob("sources_final_*.json")):
            entries = json.loads(read_text(snapshot))
            for entry in entries:
                relative = str(entry["path"]).replace("\\", "/")
                # All pre-capture production sources must still be exactly retained.
                if relative.startswith("RTOS/") or relative == "Core/Src/main.c":
                    require(sha256(self.project / relative) == entry["sha256"].upper(),
                            "tested kernel/main differs from retained source: " + snapshot.name + ":" + relative)
            snapshot_count += 1
        require(snapshot_count >= 10, "per-capture final source snapshots are incomplete")
        return {"files": len(manifest), "matched_kernel_capture_manifests": snapshot_count,
                "main_sha256": sha256(self.project / "Core/Src/main.c")}

    def restoration(self):
        state = self.json("restore_status.json")
        for key in ("restored", "main_unchanged", "build_files_restored", "daily_flashed",
                    "new_test_symbols_absent", "kernel_preserved"):
            require(state.get(key) is True, "restoration assertion is not true: " + key)
        require(state.get("daily_tag") == "final_daily", "restoration did not identify final_daily")
        text = self.capture("final_daily")
        require("System init done" in text and not FAIL_PATTERN.search(text), "daily firmware did not boot cleanly")
        for prefix in ("BP|", "BLOCK|", "NEST|", "AT|", "STRESS|", "BT|", "LC|"):
            require(not rows(text, prefix), "dedicated test firmware still running after restoration")
        pairs = [("Debug/Core/Src/subdir.mk", "hil/templates/subdir.mk.template"),
                 ("Debug/makefile", "hil/templates/makefile.template"),
                 ("Debug/objects.list", "hil/templates/objects.list.template")]
        for relative in ("RTOS/Core/Src", "RTOS/Port/GCC/ARM_CM7",
                         "RTOS/lib/third/CherryUSB/class/msc", "RTOS/lib/third/CherryUSB/core",
                         "RTOS/lib/third/CherryUSB/port/dwc2"):
            pairs.append(("Debug/" + relative + "/subdir.mk",
                          "hil/templates/" + relative.replace("/", "_") + ".mk"))
        for actual, template in pairs:
            require((self.project / actual).is_file() and (self.project / template).is_file(),
                    "restored build/template file absent: " + actual)
            require(read_text(self.project / actual).replace("\r\n", "\n")
                    == read_text(self.project / template).replace("\r\n", "\n"),
                    "build file was not restored: " + actual)
        for relative in ("hil/fwsrc", "Debug/hil", "Debug/.hil_build_stamp"):
            require(not (self.project / relative).exists(), "variant artifact retained: " + relative)
        archive = self.evidence / "firmware_final_daily.elf"
        if archive.is_file():
            symbols = {name for name, kind, code in elf_symbols(archive)}
            forbidden = {"rtos_port_critical_test_hook", "__wrap_rtos_assert_fail", "audit_pre_start",
                         "audit_regressions_task", "production_stress_task", "production_irq_start",
                         "production_highirq_start", "production_highirq_force_nested"}
            require(not (symbols & forbidden), "new test symbols retained in daily ELF: "
                    + ", ".join(sorted(symbols & forbidden)))
            require("rtos_port_critical_nesting_error" in symbols, "daily image lacks retained kernel hardening")
        return {"restored": True, "daily_tag": "final_daily", "template_pairs_checked": len(pairs)}

    def run_indexed(self, filename, tags, validator, suite, total_key=None, total_value=None):
        try:
            entries = self.index(filename, tags)
        except (PendingEvidence, EvidenceError, OSError, ValueError, KeyError, TypeError) as error:
            self.check(filename, lambda: (_ for _ in ()).throw(error))
            return
        for entry in entries:
            tag = entry["tag"]
            require(entry.get("suite") == suite or (filename == "guard_results.json"
                    and entry.get("suite") == "nesting" and entry.get("case") in (15, 16)),
                    "results index has unexpected suite: " + tag)
            if total_key is not None:
                require(entry.get(total_key) == total_value, "index case count mismatch: " + tag)
            if filename == "guard_results.json":
                identity = re.fullmatch(r"guard_c(\d+)_p(\d+)", tag)
                case, priority = map(int, identity.groups())
                require(entry.get("case") == case and entry.get("priority") == priority,
                        "guard index metadata does not match its tag")
            self.check(tag, lambda tag=tag: validator(tag))
            if "summary" in entry and tag in self.details:
                if self.details[tag].get("summary") != entry["summary"]:
                    self.errors.append({"check": tag, "error": "runner summary differs from raw log"})

    def run(self):
        variants = [(mask, assertions, optimization) for mask, assertions, optimization
                    in itertools.product(("basepri", "primask"), (0, 1), OPTIMIZATIONS)]
        matrix_tags = {"matrix_%s_a%d_%s" % item for item in variants}
        blocking_tags = {"blocking_%s_a%d_%s" % item for item in variants}
        guard_tags = {"guard_c%d_p%d" % (case, priority)
                      for case, priority in itertools.product(range(1, 15), (0, 4, 5))}
        guard_tags |= {"guard_c15_p0", "guard_c16_p0"}
        nesting_tags = {"nesting_%s_%s" % (mask, optimization)
                        for mask, optimization in itertools.product(("basepri", "primask"), OPTIMIZATIONS)}
        for filename, tags, validator, suite, count_key, count in (
                ("matrix_results.json", matrix_tags, self.matrix, "mask", "cases", 600),
                ("blocking_results.json", blocking_tags, self.blocking, "blocking", "attempts", 500),
                ("guard_results.json", guard_tags, self.guard, "guard", None, None),
                ("nesting_results.json", nesting_tags, self.nesting, "invalid_nesting", "cases", 20)):
            self.check(filename + ":batch", lambda filename=filename, tags=tags, validator=validator,
                       suite=suite, count_key=count_key, count=count: self.run_indexed(
                           filename, tags, validator, suite, count_key, count))
        self.check("compile_checks", self.compile_checks)
        self.check("baseline_injected_o2_final_fixture", lambda: self.matrix(
            "baseline_injected_o2_final_fixture", baseline=True))
        self.check("blocking_original_port", lambda: self.blocking("blocking_original_port", historical=True))
        self.check("blocking_final_isb", lambda: self.blocking("blocking_final_isb"))
        for tag in ("final_audit", "final_selftest", "final_boundary", "final_lowlevel"):
            self.check(tag, lambda tag=tag: self.functional(tag))
        for tag in ("baseline_nohook_bench", "final_nohook_bench"):
            self.check(tag, lambda tag=tag: self.benchmark(tag))
        for tag, capacity, seconds, seed, negative in (
                ("final_production_cap1", 1, 30, 1, False),
                ("final_production_cap8", 8, 30, 305419896, False),
                ("final_production_cap32", 32, 30, 2779096485, False),
                ("final_production_negative", 8, 6, 1, True),
                ("final_production_soak900", 8, 900, 3735928559, False)):
            self.check(tag, lambda tag=tag, capacity=capacity, seconds=seconds,
                       seed=seed, negative=negative: self.production(tag, capacity, seconds, seed, negative))
        partial = self.evidence / "log_final_production_soak900.txt.partial"
        if partial.is_file():
            progress = rows(read_text(partial), "STRESS|PROGRESS|")
            self.details["live_soak_observation"] = {"partial_only_not_acceptance": True,
                                                      "last_progress": progress[-1] if progress else None}
        self.check("sources_final", self.source_manifest)
        self.check("restoration", self.restoration)
        baseline = self.details.get("baseline_nohook_bench")
        final = self.details.get("final_nohook_bench")
        performance = None
        if baseline and final:
            delta = final["critical_pair_cycles"] - baseline["critical_pair_cycles"]
            performance = {"baseline_cycles": baseline["critical_pair_cycles"],
                           "final_cycles": final["critical_pair_cycles"], "additional_cycles": delta,
                           "additional_ns_at_168mhz": round(delta * 1000 / 168, 3),
                           "increase_percent": round(delta * 100 / baseline["critical_pair_cycles"], 3),
                           "scope": "no-hook O2 measured cost, not WCET or cross-target guarantee"}
        normal_production = [value for key, value in self.details.items()
                             if key.startswith("final_production_") and isinstance(value, dict)
                             and value.get("negative_control") is False]
        status = "ERROR" if self.errors else ("IN_PROGRESS" if self.in_progress else
                  ("INCOMPLETE" if self.pending else "PASS"))
        return {"schema_version": 1, "status": status,
                "generated_utc": datetime.now(timezone.utc).isoformat(),
                "mode": "in_progress_read_only" if self.in_progress else "strict",
                "hardware_accessed_by_verifier": False,
                "evidence_directory": str(self.evidence), "project_directory": str(self.project),
                "validated_groups": self.validated, "errors": self.errors, "pending": self.pending,
                "coverage": {"expected_configured_hardware_images": 74,
                             "final_mask_combinations": sum(self.details[tag]["cases"] for tag in matrix_tags
                                                             if tag in self.details),
                             "final_async_pairs": sum(self.details[tag]["async_pairs"] for tag in matrix_tags
                                                      if tag in self.details),
                             "final_blocking_attempts": sum(self.details[tag]["attempts"] for tag in blocking_tags
                                                            if tag in self.details),
                             "final_guard_images": len(guard_tags & self.details.keys()),
                             "assert_off_invalid_nesting_combinations": sum(self.details[tag]["cases"]
                                 for tag in nesting_tags if tag in self.details),
                             "normal_production_messages": sum(item["successful_messages"]
                                                               for item in normal_production)},
                "performance": performance, "captures": self.captures,
                "optional_elf_archives_absent": self.missing_elfs, "details": self.details,
                "limits": ["Finite executed paths do not prove absence of all RTOS bugs.",
                           "F446 Cortex-M4 was tested; Cortex-M7 is compilation only.",
                           "USB software contracts were exercised without a physical USB bus transfer test.",
                           "High-priority IRQ nonzero BASEPRI samples are expected; safe-point leaks must be zero.",
                           "Natural and forced IRQ events can coincide in one handler; IRQ entries can include non-delivery entries.",
                           "ELF hashes are independently checked only when the optional archived ELF is present.",
                           "Historical failing or pre-final success tags never replace the required final matrix tags."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--in-progress", action="store_true",
                        help="check existing artifacts; list pending work; do not write a summary")
    parser.add_argument("--evidence-dir", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    verifier = Verifier(args.evidence_dir.resolve(), args.project_root.resolve(), args.in_progress)
    result = verifier.run()
    if result["status"] == "PASS":
        output = verifier.evidence / "verification_summary.json"
        output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    # Compact console output; the complete strict artifact retains every check.
    print(json.dumps({key: result[key] for key in ("status", "mode", "coverage", "performance",
                     "errors", "pending")}, ensure_ascii=False, indent=2))
    if result["errors"] or (result["pending"] and not args.in_progress):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
