#!/usr/bin/env python3
"""Verify the adversarial F446 batch from saved evidence, without hardware access.

The historical verifier is imported only for its unchanged raw-log validators;
its historical source/restoration checks are never executed for this batch.
Strict success requires three independently detected failing baselines, the
mixed-wait control, 48 fixed images, 12 registry-boundary images, 74 port images, 97 strict compilations,
functional/production regressions, a 900-second soak and daily restoration.
--in-progress validates available completed work and never writes a PASS file.
Python 3.9+, standard library only; no subprocess, network, serial or SWD access.
"""

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import importlib.util
import itertools
import json
import os
from pathlib import Path
import re
import struct
import sys


MAIN_SHA256 = "32C1E6F3D50C754A8E09DB169982551AE1CB4828760662AB106BDEE482012874"
RAW_VALIDATORS_SHA256 = "9AFE41ACC36726438E64F5AC7C8408AD5EBF077F4E974872964AAB62DE7959A2"
OPTIMIZATIONS = ("O0", "O2", "O3")
VARIANTS = {
    "am": ("AM", "adversarial_mutex", 910),
    "aw": ("AW", "adversarial_wait", 1600),
    "of": ("OF", "adversarial_lifetime", 80),
    "amb": ("AMB", "adversarial_mutex_boundary", 87),
}
OF_NAMES = ("sem_give", "sem_take", "queue_send", "queue_recv", "event_set",
            "event_clear", "event_wait_ready", "mutex_take", "mutex_give", "event_wait_empty")
WARNING = re.compile(r"^(?:make(?:\.exe)? : )?\.\./Core/Src/main\.c:\d+:\d+: warning: "
                     r".+ \[-Wunused-(?:function|variable)\]\s*$")


class CaptureDateTime(datetime):
    """Accept .NET's seven fractional digits on Python 3.9/3.10 as well."""

    @classmethod
    def fromisoformat(cls, value):
        normalized = re.sub(r"(\.\d{6})\d+(?=[+-]\d{2}:\d{2}$)", r"\1", value)
        return datetime.fromisoformat(normalized)


def read_text(path):
    return path.read_text(encoding="utf-8-sig", errors="replace")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1048576), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def load_raw_validators(project):
    path = project / "hil/basepri_20261025/verify_evidence.py"
    if sha256(path) != RAW_VALIDATORS_SHA256:
        raise ValueError("historical raw validator changed; review before accepting new evidence")
    spec = importlib.util.spec_from_file_location("historical_raw_validators", path)
    module = importlib.util.module_from_spec(spec)
    # Do not create __pycache__ artifacts while checking evidence.
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    module.datetime = CaptureDateTime
    return module


class BatchVerifier:
    def __init__(self, batch, evidence, matrix, project, in_progress):
        self.batch, self.evidence, self.matrix, self.project = batch, evidence, matrix, project
        self.in_progress = in_progress
        self.raw = load_raw_validators(project)
        self.errors, self.pending, self.details, self.captures = [], [], {}, {}
        self.raw_instances = {}

    def require(self, condition, message):
        self.raw.require(condition, message)

    def file(self, directory, name):
        path = directory / name
        if not path.is_file():
            raise self.raw.PendingEvidence(str(path))
        return path

    def json(self, directory, name):
        return json.loads(read_text(self.file(directory, name)))

    def check(self, name, callback):
        try:
            self.details[name] = callback()
        except self.raw.PendingEvidence as error:
            self.pending.append({"check": name, "missing": str(error)})
        except (self.raw.EvidenceError, OSError, ValueError, KeyError, TypeError,
                IndexError, AttributeError, self.raw.struct.error) as error:
            self.errors.append({"check": name, "error": str(error)})

    def validator(self, directory):
        key = str(directory)
        if key not in self.raw_instances:
            self.raw_instances[key] = self.raw.Verifier(directory, self.project, self.in_progress)
        return self.raw_instances[key]

    def capture(self, directory, tag, exit_isb=False):
        # This new batch requires archived ELF bytes, not only a recorded hash.
        self.file(directory, "firmware_%s.elf" % tag)
        verifier = self.validator(directory)
        text = verifier.capture(tag, require_exit_isb=exit_isb)
        self.captures[str(directory / tag)] = verifier.captures[tag]
        return text

    def source_manifest(self, path, required=None):
        manifest = json.loads(read_text(path))
        self.require(isinstance(manifest, list) and manifest, "empty source manifest")
        seen = set()
        for entry in manifest:
            relative = str(entry["path"]).replace("\\", "/")
            target = (self.project / relative).resolve()
            self.require(target.is_relative_to(self.project) and relative not in seen,
                         "duplicate or escaping source path: " + relative)
            seen.add(relative)
            self.require(target.is_file() and sha256(target) == str(entry["sha256"]).upper(),
                         "tested source differs from retained source: " + relative)
        if required is not None:
            self.require(required <= seen, "source manifest omits: " + ", ".join(sorted(required - seen)))
        return len(manifest)

    def source_required(self):
        result = {"Core/Src/main.c", "hil/build_fw.ps1", "hil/run_hil.ps1",
                  "hil/restore_project.ps1"}
        for relative in ("RTOS", "hil/fw"):
            result.update(str(path.relative_to(self.project)).replace("\\", "/")
                          for path in (self.project / relative).rglob("*")
                          if path.is_file() and path.suffix.lower() in (".c", ".h", ".s"))
        return result

    def adversarial(self, directory, tag, suite, baseline=False):
        prefix, unused, total = VARIANTS[suite]
        text = self.capture(directory, tag)
        summary_line = self.raw.one(text, prefix + "|SUMMARY|")
        summary = self.raw.fields(summary_line)
        passed = self.raw.number(summary, "pass")
        failures = self.raw.number(summary, "fail")
        expected_failures = {"am": 109, "of": 72, "amb": 3, "aw": 0}[suite] if baseline else 0
        self.require((passed, failures) == (total - expected_failures, expected_failures),
                     "unexpected adversarial pass/fail totals")
        fail_lines = [line for line in text.splitlines() if self.raw.FAIL_PATTERN.search(line)]
        self.require(len(fail_lines) == failures, "summary differs from raw failure count")
        self.require(self.raw.number(self.raw.fields(self.raw.one(text, prefix + "|DONE|")),
                                     "status") == int(failures != 0), "incorrect DONE status")
        self.raw.one(text, prefix + "|META|")
        if suite == "am":
            cases = [self.raw.fields(line) for line in self.raw.rows(text, "AM|CASE|")]
            self.require(len(cases) == 15 and {row.get("id") for row in cases} == set(range(1, 16))
                         and {(row.get("kind"), row.get("depth")) for row in cases}
                         == set(itertools.product(range(5), (2, 3, 6)))
                         and summary.get("cases") == 15, "missing mutex-chain cases")
            self.require(len(self.raw.rows(text, "AM|PASS|")) == passed,
                         "mutex pass records disagree with summary")
            chain = [self.raw.fields(line) for line in self.raw.rows(text, "AM|CHAIN|")]
            self.require(chain and sum(row["effective"] != row["expected"] for row in chain)
                         == failures, "mutex propagation failure signature differs")
            for line in fail_lines:
                row = self.raw.fields(line)
                self.require(row.get("actual") != row.get("expected") and row.get("name") in
                             {"initial_chain", "high_inheritance", "timeout_withdrawal",
                              "suspend_withdrawal", "delete_withdrawal", "changed_waiter"},
                             "unexpected mutex baseline failure")
        elif suite == "aw":
            self.require(summary.get("rounds") == 8 and summary.get("phases") == 73
                         and summary.get("irq_requests") == 120, "mixed-wait summary incomplete")
            phases = [self.raw.fields(line) for line in self.raw.rows(text, "AW|PHASE|")]
            expected = set(itertools.product(range(8),
                ("queue_receivers", "queue_senders", "semaphore_waiters", "event_consumption",
                 "notify_increment", "event_notify_independence", "queue_object_delete",
                 "queue_timeout_late_send", "mutex_timeout_inheritance")))
            self.require(len(phases) == 73 and
                         {(row.get("round"), row.get("name")) for row in phases[:-1]} == expected
                         and phases[-1].get("name") == "final_cleanup"
                         and all(row.get("fail") == 0 for row in phases), "missing mixed-wait phases")
            high = self.raw.fields(self.raw.one(text, "AW|HIGHIRQ|"))
            self.require(high.get("entries", 0) > 0 and high.get("errors") == 0
                         and high.get("restored") == 1, "real high-priority IRQ was not restored")
        elif suite == "of":
            cases = self.raw.rows(text, "OF|CASE|")
            self.require(summary.get("cases") == len(cases) == 80, "object lifetime case count")
            seen = set()
            for line in cases:
                row = self.raw.fields(line)
                key = (row.get("round"), row.get("name"))
                self.require(key not in seen and row.get("id") == len(seen) + 1,
                             "duplicate or reordered lifetime case")
                seen.add(key)
                self.require(row.get("prep") == 0 and row.get("hook") == 1 and row.get("irq") == 1
                             and row.get("inside") == 1 and row.get("irq_status") == 0
                             and row.get("dead_init") == 0 and row.get("ipsr") == 71
                             and row.get("priority") == 5, "missing real legal ISR deinitialization")
                if not baseline:
                    self.require(line.startswith("OF|CASE|PASS|") and row.get("mismatch") == 0
                                 and row.get("status") == row.get("expected")
                                 and row.get("elapsed") in (0, 1),
                                 "deinitialized object was used or consumed its 2-tick timeout")
            self.require(seen == set(itertools.product(range(8), OF_NAMES)),
                         "missing object operations/rounds")
            if baseline:
                self.require(Counter(self.raw.fields(line).get("name") for line in fail_lines)
                             == Counter({name: 8 for name in OF_NAMES if name != "mutex_give"}),
                             "lifetime baseline failure signature differs")
        else:
            self.require(len(self.raw.rows(text, "AMB|PASS|")) == passed,
                         "boundary pass records differ from summary")
            boundary = self.raw.fields(self.raw.one(text, "AMB|BOUNDARY|"))
            self.require(boundary.get("owner_retained") == 1
                         and boundary.get("repair") == "fixture_only", "missing boundary provenance")
            if baseline:
                names = Counter(self.raw.fields(line).get("name") for line in fail_lines)
                self.require(names == Counter({"recursive_overflow_status": 1,
                             "recursive_overflow_count": 1, "cycle_inherits_high": 1})
                             and boundary.get("count") == 0 and boundary.get("status") == 0,
                             "overflow/cycle baseline failure signature differs")
            else:
                self.require(boundary.get("count") == 0xFFFFFFFF and boundary.get("status") == -7,
                             "recursive overflow was not rejected before state mutation")
        return {"pass": passed, "fail": failures, "summary": summary_line}

    def matrix_image(self, entry):
        tag, suite = entry["tag"], entry["suite"]
        prefix, variant, unused = VARIANTS[suite]
        self.require(entry.get("passed") is True and entry.get("stage") == "complete"
                     and entry.get("prefix") == prefix and entry.get("variant") == variant,
                     "runner did not complete this image")
        wanted = "%s_%s_a%d_%s" % (suite, entry["critical_mask"], entry["assertions"],
                                   entry["optimization"])
        self.require(tag == wanted, "configuration identity differs from tag")
        source = self.file(self.matrix, "sources_%s.json" % tag)
        self.require(entry.get("source_manifest") == source.name
                     and entry.get("source_manifest_sha256") == sha256(source),
                     "source manifest identity differs from runner index")
        source_count = self.source_manifest(source, self.source_required())
        stdout = read_text(self.file(self.matrix, "build_%s.txt" % tag))
        detail = read_text(self.file(self.matrix, "build_detail_%s.txt" % tag))
        self.file(self.matrix, "capture_stdout_%s.txt" % tag)
        warnings = [line for line in detail.splitlines() if "warning:" in line]
        self.require(len(warnings) == 25 and all(WARNING.fullmatch(line) for line in warnings)
                     and not re.search(r"(?:fatal )?error:", detail)
                     and re.search(r"warnings=25\b", stdout)
                     and entry.get("warning_count") == 25
                     and entry.get("allowed_existing_main_unused_warnings") == 25,
                     "diagnostics exceed the 25 existing unused main.c demo declarations")
        commands = [line for line in detail.splitlines() if "arm-none-eabi-gcc " in line
                    and " -c " in line]
        fixture = [line for line in commands if "hil/fwsrc/%s.c\"" % variant in line]
        self.require(len(fixture) == 1 and all(flag in fixture[0].split()
                     for flag in ("-Wextra", "-Werror")), "fixture lacks strict compilation")
        for source_name in ("rtos_mutex", "rtos_sem", "rtos_queue", "rtos_event"):
            matching = [line for line in commands if "RTOS/Core/Src/%s.c\"" % source_name in line]
            self.require(len(matching) == 1, "missing kernel compilation: " + source_name)
            for command in matching + fixture:
                self.require("-" + entry["optimization"] in command.split(), "wrong optimization")
                self.require(("-DRTOS_CONFIG_USE_BASEPRI=0" in command.split())
                             == (entry["critical_mask"] == "primask"), "wrong interrupt mask mode")
                self.require(("-DRTOS_CONFIG_ASSERT_ENABLED=0" in command.split())
                             == (entry["assertions"] == 0), "wrong assertion configuration")
                self.require(("-DRTOS_HIL_OBJECT_LIFETIME=1" in command.split()) == (suite == "of"),
                             "object-lifetime hook configuration differs from variant")
        result = self.adversarial(self.matrix, tag, suite)
        self.require(entry.get("summary") == result["summary"], "index/raw summary mismatch")
        result["source_files"] = source_count
        return result

    def matrix_index(self):
        entries = self.json(self.matrix, "matrix_results.json")
        expected = {"%s_%s_a%d_%s" % item for item in itertools.product(
            VARIANTS, ("basepri", "primask"), (0, 1), OPTIMIZATIONS)}
        self.require(isinstance(entries, list), "matrix index must be an array")
        tags = [entry["tag"] for entry in entries]
        self.require(len(tags) == len(set(tags)) and set(tags) <= expected, "invalid matrix tags")
        if set(tags) != expected:
            self.pending.append({"check": "matrix_coverage", "missing_tags": sorted(expected - set(tags))})
        for entry in entries:
            if entry.get("stage") != "complete" and self.in_progress and "error" not in entry:
                self.pending.append({"check": entry["tag"], "stage": entry.get("stage")})
            else:
                self.check("matrix:" + entry["tag"], lambda entry=entry: self.matrix_image(entry))
        return {"expected": 48, "indexed": len(entries)}

    def registry_image(self, directory, entry):
        """Check capacity states independently of the fixture's PASS decisions."""
        tag = entry["tag"]
        self.require(entry.get("passed") is True
                     and entry.get("variant") == "adversarial_registry",
                     "registry runner did not complete this image")
        wanted = "registry_%s_a%d_%s" % (entry["critical_mask"], entry["assertions"],
                                       entry["optimization"])
        self.require(tag == wanted, "registry configuration identity differs from tag")
        expected_checks = entry.get("expected_checks")
        self.require(type(expected_checks) is int and expected_checks > 0,
                     "registry index requires a positive expected_checks")
        source = self.file(directory, "sources_%s.json" % tag)
        self.source_manifest(source, self.source_required())
        if "source_manifest_sha256" in entry:
            self.require(entry["source_manifest_sha256"] == sha256(source),
                         "registry source manifest hash differs from index")
        stdout = read_text(self.file(directory, "build_%s.txt" % tag))
        detail = read_text(self.file(directory, "build_detail_%s.txt" % tag))
        warnings = [line for line in detail.splitlines() if "warning:" in line]
        self.require(len(warnings) == 25 and all(WARNING.fullmatch(line) for line in warnings)
                     and not re.search(r"(?:fatal )?error:", detail)
                     and re.search(r"warnings=25\b", stdout),
                     "registry diagnostics exceed existing unused main.c declarations")
        commands = [line for line in detail.splitlines() if "arm-none-eabi-gcc " in line
                    and " -c " in line]
        fixture = [line for line in commands if "hil/fwsrc/adversarial_registry.c\"" in line]
        self.require(len(fixture) == 1 and all(flag in fixture[0].split()
                     for flag in ("-Wextra", "-Werror")), "registry fixture lacks strict compilation")
        for source_name in ("rtos_mutex", "rtos_sem", "rtos_queue", "rtos_event"):
            matching = [line for line in commands if "RTOS/Core/Src/%s.c\"" % source_name in line]
            self.require(len(matching) == 1, "missing registry kernel compilation: " + source_name)
            for command in matching + fixture:
                self.require("-" + entry["optimization"] in command.split(),
                             "registry optimization differs")
                self.require(("-DRTOS_CONFIG_USE_BASEPRI=0" in command.split())
                             == (entry["critical_mask"] == "primask"), "registry mask mode differs")
                self.require(("-DRTOS_CONFIG_ASSERT_ENABLED=0" in command.split())
                             == (entry["assertions"] == 0), "registry assertion mode differs")
                self.require("-DRTOS_HIL_OBJECT_LIFETIME=1" not in command.split(),
                             "registry build unexpectedly enables the lifetime hook")
        text = self.capture(directory, tag)
        self.require(not self.raw.FAIL_PATTERN.search(text), "registry fixture emitted FAIL")
        summary_line = self.raw.one(text, "AR|SUMMARY|")
        summary = self.raw.fields(summary_line)
        self.require(summary.get("pass") == summary.get("checks") == expected_checks
                     and summary.get("fail") == 0 and summary.get("cases") == 2,
                     "registry summary differs from expected checks/two cases")
        self.require(entry.get("summary") == summary_line, "registry index/raw summary differs")
        self.require(self.raw.fields(self.raw.one(text, "AR|DONE|")).get("status") == 0,
                     "registry fixture did not finish successfully")
        meta = self.raw.fields(self.raw.one(text, "AR|META|"))
        self.require(meta.get("owners") == meta.get("capacity") == 8
                     and meta.get("worker_limit") == 9 and meta.get("public_api_only") == 1
                     and meta.get("private_tcb_writes") == 0
                     and meta.get("finite_wait_ticks", 0) > 0, "registry MAX8/public-API provenance differs")
        cases = [self.raw.fields(line) for line in self.raw.rows(text, "AR|CASE|")]
        self.require(len(cases) == 2 and {(row.get("id"), row.get("name")) for row in cases}
                     == {(1, "saturated_handoff"), (2, "deleted_multi_owner")},
                     "registry capacity cases are missing or duplicated")
        checks = [self.raw.fields(line) for line in self.raw.rows(text, "AR|PASS|")]
        self.require(len(checks) == expected_checks
                     and [row.get("id") for row in checks] == list(range(1, expected_checks + 1))
                     and {row.get("case") for row in checks} == {1, 2}
                     and all(type(row.get("actual")) is int and
                             row.get("actual") == row.get("expected") for row in checks),
                     "registry raw PASS count/identity/value evidence is inconsistent")
        graph = self.raw.fields(self.raw.one(text, "AR|GRAPH|"))
        graph_expected = {"phase": "m_plus_one", "registered": 8,
                          "distinct_current_owners": 8, "detached_owner": 0,
                          "old_base": 15, "old_effective": 15, "new_owner": 8,
                          "new_effective": 4, "old_owns": 0}
        self.require(all(graph.get(key) == value for key, value in graph_expected.items()),
                     "MAX8 plus detached owner state was not actually observed")
        handoffs = [self.raw.fields(line) for line in self.raw.rows(text, "AR|HANDOFF|")]
        self.require([(row.get("phase"), row.get("lock")) for row in handoffs]
                     == [("deleted", 0), ("deleted", 1), ("released", 0),
                         ("other_chain_retained", 1), ("released", 1)],
                     "multi-lock owner deletion/handoff snapshots are incomplete")
        for row in handoffs:
            lock = row["lock"]
            owner = lock + (3 if row["phase"] == "released" else 1)
            self.require(row.get("owner") == row.get("expected_owner") == owner
                         and row.get("effective") == row.get("expected_effective") == 6 + lock
                         and row.get("waiters") == int(row["phase"] != "released"),
                         "remaining waiters or inherited priorities differ during owner transfer")
        return {"pass": expected_checks, "fail": 0, "cases": 2, "summary": summary_line,
                "max_registered_mutexes": 8, "max_scratch_owners_observed": 9}

    def registry_index(self):
        directory = self.evidence / "registry"
        entries = self.json(directory, "registry_results.json")
        expected = {"registry_%s_a%d_%s" % item for item in itertools.product(
            ("basepri", "primask"), (0, 1), OPTIMIZATIONS)}
        self.require(isinstance(entries, list), "registry index must be an array")
        tags = [row["tag"] for row in entries]
        self.require(len(tags) == len(set(tags)) and set(tags) <= expected, "invalid registry tags")
        if set(tags) != expected:
            self.pending.append({"check": "registry_coverage", "missing_tags": sorted(expected - set(tags))})
        counts = {row.get("expected_checks") for row in entries if row.get("passed") is True}
        self.require(len(counts) <= 1, "registry expected_checks differs across configurations")
        for entry in entries:
            if entry.get("passed") is not True and self.in_progress and "error" not in entry:
                self.pending.append({"check": entry["tag"], "stage": entry.get("stage", "running")})
            else:
                self.check("registry:" + entry["tag"], lambda entry=entry:
                           self.registry_image(directory, entry))
        return {"expected": 12, "indexed": len(entries),
                "expected_checks_per_image": next(iter(counts)) if counts else None}

    def port_matrix(self):
        directory = self.evidence / "port"
        validator = self.validator(directory)
        configured = list(itertools.product(("basepri", "primask"), (0, 1), OPTIMIZATIONS))
        suites = (
            ("matrix_results.json", {"matrix_%s_a%d_%s" % item for item in configured}, validator.matrix),
            ("blocking_results.json", {"blocking_%s_a%d_%s" % item for item in configured}, validator.blocking),
            ("nesting_results.json", {"nesting_%s_%s" % item for item in
                itertools.product(("basepri", "primask"), OPTIMIZATIONS)}, validator.nesting),
            ("guard_results.json", {"guard_c%d_p%d" % item for item in
                itertools.product(range(1, 15), (0, 4, 5))} | {"guard_c15_p0", "guard_c16_p0"}, validator.guard))
        total = 0
        for filename, expected, callback in suites:
            try:
                entries = self.json(directory, filename)
            except self.raw.PendingEvidence as error:
                self.pending.append({"check": "port:" + filename, "missing": str(error)})
                continue
            self.require(isinstance(entries, list), "port index must be an array")
            tags = [entry["tag"] for entry in entries]
            self.require(len(tags) == len(set(tags)) and set(tags) <= expected, "invalid port index")
            if set(tags) != expected:
                self.pending.append({"check": "port:" + filename,
                                     "missing_tags": sorted(expected - set(tags))})
            for entry in entries:
                tag = entry["tag"]
                def verify(tag=tag, entry=entry, callback=callback):
                    self.require(entry.get("passed") is True, "port runner reported failure")
                    self.capture(directory, tag)
                    return callback(tag)
                self.check("port:" + tag, verify)
                total += 1
        return {"expected": 74, "indexed": total}

    def compile_checks(self):
        directory = self.evidence / "compile"
        entries = self.json(directory, "compile_results.json")
        sources = {"rtos_heap": "RTOS/Core/Src/rtos_heap.c",
                   "rtos_perf": "RTOS/Core/Src/rtos_perf.c", "rtos_usb": "RTOS/Core/Src/rtos_usb.c",
                   "port": "RTOS/Port/GCC/ARM_CM7/port.c", "rtos_mutex": "RTOS/Core/Src/rtos_mutex.c",
                   "rtos_sem": "RTOS/Core/Src/rtos_sem.c", "rtos_queue": "RTOS/Core/Src/rtos_queue.c",
                   "rtos_event": "RTOS/Core/Src/rtos_event.c"}
        expected = {"m%d_a%d_%s_%s" % item for item in itertools.product(
            (0, 1), (0, 1), OPTIMIZATIONS, sources)} | {"cm7_port_compile_only"}
        self.require(isinstance(entries, list) and len(entries) == 97
                     and {row["tag"] for row in entries} == expected, "strict compile coverage incomplete")
        stacks = []
        for row in entries:
            tag = row["tag"]
            self.require(row.get("passed") is True and row.get("warnings") == 0,
                         "strict compile result failed")
            name = "compile_cm7_port_only.txt" if tag == "cm7_port_compile_only" else "compile_%s.txt" % tag
            self.require(not read_text(self.file(directory, name)).strip(), "strict compiler emitted diagnostics")
            if tag == "cm7_port_compile_only":
                self.require(row.get("hardware_test") is False, "M7 must remain compilation only")
                continue
            source_name = tag.split("_", 3)[3]
            self.require(row.get("source") == sources[source_name]
                         and row.get("sha256") == sha256(self.project / sources[source_name]),
                         "strict compile source identity differs from current source")
            usage = read_text(self.file(directory, tag + ".su"))
            self.require(usage.strip(), "empty stack-usage evidence")
            for line in usage.splitlines():
                if "mutex_refresh_priority_graph" in line:
                    parts = line.split("\t")
                    self.require(len(parts) == 3 and parts[1].isdigit() and parts[2] == "static",
                                 "PI graph has unbounded compiler stack usage")
                    stacks.append({"tag": tag, "frame_bytes": int(parts[1])})
        return {"compilations": 97, "stack_usage_files": 96, "pi_graph_frames": stacks,
                "cm7_hardware_test": False}

    def regressions(self, directory, filename, wanted):
        entries = self.json(directory, filename)
        # The historical PowerShell runner serializes a one-image suite as a
        # single object. Normalize its container; retain exact tag coverage and
        # every source, firmware, raw-log and acceptance check below.
        if isinstance(entries, dict) and len(wanted) == 1:
            entries = [entries]
        self.require(isinstance(entries, list), "regression index must be an array")
        tags = [row["tag"] for row in entries]
        self.require(len(tags) == len(set(tags)) and set(tags) <= set(wanted), "regression tags differ")
        if set(tags) != set(wanted):
            self.pending.append({"check": filename, "missing_tags": sorted(set(wanted) - set(tags))})
        validator = self.validator(directory)
        for entry in entries:
            tag = entry["tag"]
            def verify(tag=tag, entry=entry):
                self.require(entry.get("passed") is True, "regression runner failed")
                self.capture(directory, tag)
                source = self.file(directory, "sources_%s.json" % tag)
                self.source_manifest(source)
                if tag in ("final_audit", "final_selftest", "final_boundary", "final_lowlevel"):
                    return validator.functional(tag)
                if tag == "final_nohook_bench":
                    return validator.benchmark(tag)
                capacity, seconds, seed, negative = wanted[tag]
                return validator.production(tag, capacity, seconds, seed, negative)
            self.check("regression:" + tag, verify)
        return {"captures": len(entries)}

    def final_sources(self):
        path = self.file(self.batch, "sources_final.json")
        count = self.source_manifest(path, self.source_required())
        self.require(sha256(self.project / "Core/Src/main.c") == MAIN_SHA256,
                     "main.c differs from the fixed preserved daily source")
        return {"files": count, "main_sha256": MAIN_SHA256}

    def tracked_paths(self):
        """Read Git's ordinary v2/v3 index; never spawn git or alter its index."""
        git_directory = self.project / ".git"
        if git_directory.is_file():
            pointer = read_text(git_directory).strip()
            self.require(pointer.startswith("gitdir: "), "invalid Git directory pointer")
            git_directory = (self.project / pointer[8:]).resolve()
        data = self.file(git_directory, "index").read_bytes()
        self.require(len(data) >= 32 and data[:4] == b"DIRC", "invalid Git index header")
        version, count = struct.unpack_from(">II", data, 4)
        self.require(version in (2, 3), "cleanup requires a reviewed Git index v2/v3")
        self.require(hashlib.sha1(data[:-20]).digest() == data[-20:], "Git index checksum differs")
        offset, tracked = 12, set()
        for unused in range(count):
            start = offset
            self.require(start + 62 <= len(data) - 20, "truncated Git index entry")
            mode = struct.unpack_from(">I", data, start + 24)[0]
            flags = struct.unpack_from(">H", data, start + 60)[0]
            offset += 62
            if flags & 0x4000:
                self.require(version == 3 and offset + 2 <= len(data) - 20,
                             "invalid extended Git index entry")
                offset += 2
            end = data.find(b"\0", offset, len(data) - 20)
            self.require(end >= offset, "unterminated Git index path")
            encoded = data[offset:end]
            self.require((flags & 0xFFF) in (len(encoded), 0xFFF), "Git index path length differs")
            # A sparse-directory or split-index entry would need an expanded
            # tracked-file oracle. Refuse it instead of silently missing paths.
            self.require(mode & 0o170000 != 0o040000 and encoded,
                         "sparse/split Git index is unsupported for cleanup verification")
            tracked.add(encoded.decode("utf-8", "surrogateescape").replace("\\", "/").casefold())
            offset = start + ((end + 1 - start + 7) // 8) * 8
        while offset < len(data) - 20:
            self.require(offset + 8 <= len(data) - 20, "truncated Git index extension")
            signature, size = struct.unpack_from(">4sI", data, offset)
            self.require(signature != b"link", "split Git index requires an expanded tracked oracle")
            offset += 8 + size
            self.require(offset <= len(data) - 20, "Git index extension exceeds file")
        self.require(offset == len(data) - 20, "invalid Git index end offset")
        return tracked, version

    def cleanup_path(self, relative):
        self.require(isinstance(relative, str) and relative, "cleanup path is empty")
        relative = relative.replace("\\", "/")
        pieces = relative.split("/")
        self.require(all(piece and piece not in (".", "..") and ":" not in piece
                         and "\0" not in piece and piece == piece.rstrip(" .") for piece in pieces)
                     and pieces[0].casefold() != ".git", "unsafe cleanup path: " + relative)
        lexical = self.project / relative
        target = lexical.resolve()
        self.require(target != self.project and target.is_relative_to(self.project),
                     "cleanup path escapes project: " + relative)
        return relative, lexical, target

    def retained_cleanup_copy(self, relative, expected_hash, expected_bytes):
        unused, lexical, retained = self.cleanup_path(relative)
        self.require(lexical.is_file() and retained.is_file()
                     and retained.stat().st_size == expected_bytes
                     and sha256(retained) == expected_hash,
                     "retained cleanup evidence is absent or differs: " + relative)

    def cleanup_record(self, manifest, expected_count, tracked, seen, historical=False):
        self.require(isinstance(manifest, dict), "cleanup manifest must be an object")
        if manifest.get("status") != "complete":
            raise self.raw.PendingEvidence("cleanup manifest is not complete")
        entries = manifest.get("entries")
        self.require(isinstance(entries, list) and len(entries) == expected_count
                     and manifest.get("deleted_files") == expected_count,
                     "cleanup deleted count differs from its reviewed entries")
        total_bytes = 0
        for entry in entries:
            relative, lexical, target = self.cleanup_path(entry["path"])
            identity = relative.casefold()
            self.require(identity not in seen, "duplicate cleanup path across manifests: " + relative)
            seen.add(identity)
            self.require(identity not in tracked, "cleanup targeted a tracked file: " + relative)
            self.require(not os.path.lexists(str(lexical)) and not target.exists(),
                         "recorded cleanup target still exists: " + relative)
            size = entry.get("bytes")
            self.require(type(size) is int and size >= 0, "invalid cleanup byte count")
            digest = entry.get("sha256", "")
            self.require(isinstance(digest, str) and re.fullmatch(r"[0-9A-Fa-f]{64}", digest),
                         "invalid cleanup target SHA256")
            total_bytes += size
            if historical:
                self.require(entry.get("tracked") is False and entry.get("deleted") is True,
                             "historical cleanup did not confirm an untracked deletion")
                self.require(isinstance(entry.get("absolute_path"), str)
                             and Path(entry["absolute_path"]).resolve() == target,
                             "historical absolute and relative cleanup paths disagree")
        self.require(manifest.get("deleted_bytes") == total_bytes,
                     "cleanup deleted bytes differ from the sum of recorded file sizes")
        if historical:
            self.require(Path(manifest.get("repository", "")).resolve() == self.project
                         and manifest.get("planned_files") == expected_count
                         and manifest.get("failures") == []
                         and manifest.get("protected_files_missing") == [],
                         "historical cleanup repository/count/failure fields disagree")
            retention = manifest.get("retention_verification", {})
            self.require(all(retention.get(key) == 0 for key in
                             ("recorded_targets_remaining", "disallowed_recorded_targets",
                              "tracked_deletions")) and retention.get("known_protected_paths_missing") == [],
                         "historical cleanup retention assertions failed")
        else:
            self.require(manifest.get("recorded_targets_remaining") == 0,
                         "final cleanup reports remaining targets")
        return total_bytes

    def cleanup_checks(self):
        old = self.json(self.batch, "cleanup_manifest.json")
        final = self.json(self.batch, "cleanup_final_manifest.json")
        # A missing/planned final manifest is pending (including an explicit
        # KeepCompileArtifacts run), and can never produce strict PASS.
        if final.get("status") != "complete":
            raise self.raw.PendingEvidence("final cleanup has not completed")
        tracked, index_version = self.tracked_paths()
        seen = set()
        old_bytes = self.cleanup_record(old, 277, tracked, seen, historical=True)
        final_bytes = self.cleanup_record(final, 200, tracked, seen)
        sources = ("rtos_heap", "rtos_perf", "rtos_usb", "port", "rtos_mutex",
                   "rtos_sem", "rtos_queue", "rtos_event")
        names = {"m%d_a%d_%s_%s" % item for item in itertools.product(
            (0, 1), (0, 1), OPTIMIZATIONS, sources)}
        expected = {"Debug/adversarial_compile_checks/" + name + suffix
                    for name, suffix in itertools.product(names, (".o", ".su"))}
        expected |= {"Debug/adversarial_compile_checks/cm7_port_compile_only.o",
                     "Debug/adversarial_lifetime_agent_check.o",
                     "Debug/adversarial_wait_agent_check.o"}
        retained_logs = {}
        for variant, prefix, directory in (
                ("adversarial_mutex", "am", "matrix_final"),
                ("adversarial_wait", "aw", "matrix_final"),
                ("adversarial_lifetime", "of", "matrix_final"),
                ("adversarial_mutex_boundary", "amb", "matrix_final"),
                ("adversarial_registry", "registry", "registry")):
            target = "hil/logs/build_%s_1.txt" % variant
            expected.add(target)
            retained_logs[target] = str((self.evidence / directory /
                ("build_detail_%s_primask_a0_O3.txt" % prefix)).relative_to(self.project)).replace("\\", "/")
        self.require({row["path"].replace("\\", "/") for row in final["entries"]} == expected,
                     "final cleanup differs from the exact 200 reviewed compiler/log targets")
        stack_count = 0
        duplicate_count = 0
        for row in final["entries"]:
            relative = row["path"].replace("\\", "/")
            digest = row["sha256"].upper()
            if relative in retained_logs:
                retained = row.get("retained_copy", "").replace("\\", "/")
                self.require(row.get("reason") == "duplicate_build_log_with_identical_retained_evidence"
                             and retained == retained_logs[relative], "duplicate log retention path differs")
                self.retained_cleanup_copy(retained, digest, row["bytes"])
                duplicate_count += 1
            else:
                self.require(row.get("reason") == "regenerable_compiler_artifact"
                             and row.get("retained_copy") is None,
                             "compiler cleanup reason/retained-copy fields differ")
                if relative.endswith(".su"):
                    retained = str((self.evidence / "compile" / Path(relative).name
                                    ).relative_to(self.project)).replace("\\", "/")
                    self.retained_cleanup_copy(retained, digest, row["bytes"])
                    stack_count += 1
        self.require(duplicate_count == 5 and stack_count == 96
                     and len(list((self.evidence / "compile").glob("*.su"))) == 96,
                     "retained duplicate log/stack-usage counts differ")
        return {"historical_deleted_files": 277, "historical_deleted_bytes": old_bytes,
                "historical_planned_bytes_legacy_field": old.get("planned_bytes"),
                "final_deleted_files": 200, "final_deleted_bytes": final_bytes,
                "unique_targets_absent": len(seen), "tracked_paths_deleted": 0,
                "git_index_version": index_version, "retained_identical_build_logs": duplicate_count,
                "retained_identical_stack_records": stack_count}

    def restoration(self):
        directory = self.evidence / "daily"
        state = self.json(directory, "restore_status.json")
        for key in ("restored", "main_unchanged", "kernel_preserved", "build_files_restored",
                    "new_test_symbols_absent", "daily_flashed"):
            self.require(state.get(key) is True, "restoration assertion failed: " + key)
        self.require(state.get("daily_tag") == "final_daily", "daily tag mismatch")
        text = self.capture(directory, "final_daily")
        self.require("System init done" in text and not self.raw.FAIL_PATTERN.search(text),
                     "daily firmware did not boot cleanly")
        self.require(not any(re.match(r"(?:AM|AMB|AW|OF|BP|BLOCK|NEST|AT|STRESS|BT|LC)\|", line)
                             for line in text.splitlines()), "dedicated test firmware still running")
        symbols = {name for name, kind, code in self.raw.elf_symbols(
            self.file(directory, "firmware_final_daily.elf"))}
        forbidden = {"rtos_hil_object_access_hook", "rtos_port_critical_test_hook",
                     "__wrap_rtos_assert_fail", "audit_pre_start", "audit_regressions_task",
                     "production_stress_task", "production_irq_start", "production_highirq_start"}
        self.require(not symbols & forbidden, "test hooks remain in daily image")
        self.require("rtos_port_critical_nesting_error" in symbols, "retained port hardening is absent")
        self.require(sha256(self.project / "Core/Src/main.c") == MAIN_SHA256, "daily main.c changed")
        pairs = [("Core/Src/subdir.mk", "subdir.mk.template"),
                 ("makefile", "makefile.template"), ("objects.list", "objects.list.template")]
        for relative in ("RTOS/Core/Src", "RTOS/Port/GCC/ARM_CM7",
                         "RTOS/lib/third/CherryUSB/class/msc", "RTOS/lib/third/CherryUSB/core",
                         "RTOS/lib/third/CherryUSB/port/dwc2"):
            pairs.append((relative + "/subdir.mk", relative.replace("/", "_") + ".mk"))
        for actual, template in pairs:
            self.require(read_text(self.project / "Debug" / actual).replace("\r\n", "\n") ==
                         read_text(self.project / "hil/templates" / template).replace("\r\n", "\n"),
                         "daily build file not restored: " + actual)
        for relative in ("hil/fwsrc", "Debug/hil", "Debug/.hil_build_stamp"):
            self.require(not (self.project / relative).exists(), "variant artifact retained: " + relative)
        return {"restored": True, "main_sha256": MAIN_SHA256, "template_pairs": len(pairs)}

    def run(self):
        for tag, suite in (("mutex_chain_baseline", "am"), ("object_lifetime_baseline", "of"),
                           ("mutex_boundary_baseline", "amb"), ("wait_mix_baseline", "aw")):
            self.check("baseline:" + tag, lambda tag=tag, suite=suite:
                       self.adversarial(self.batch / "baseline", tag, suite, baseline=True))
        self.check("matrix_index", self.matrix_index)
        self.check("registry_index", self.registry_index)
        self.check("port_matrix", self.port_matrix)
        self.check("strict_compile", self.compile_checks)
        functional = {tag: None for tag in ("final_audit", "final_selftest", "final_boundary",
                                           "final_lowlevel", "final_nohook_bench")}
        production = {"final_production_cap1": (1, 30, 1, False),
                      "final_production_cap8": (8, 30, 305419896, False),
                      "final_production_cap32": (32, 30, 2779096485, False),
                      "final_production_negative": (8, 6, 1, True)}
        self.check("functional_index", lambda: self.regressions(self.evidence / "regression",
                   "regressions_functional.json", functional))
        self.check("production_index", lambda: self.regressions(self.evidence / "regression",
                   "regressions_production.json", production))
        self.check("soak_index", lambda: self.regressions(self.evidence / "soak",
                   "regressions_soak.json", {"final_production_soak900": (8, 900, 3735928559, False)}))
        self.check("final_sources", self.final_sources)
        self.check("restoration", self.restoration)
        self.check("cleanup", self.cleanup_checks)
        status = "ERROR" if self.errors else ("IN_PROGRESS" if self.in_progress else
                  ("INCOMPLETE" if self.pending else "PASS"))
        matrix_details = [value for key, value in self.details.items() if key.startswith("matrix:")]
        registry_details = [value for key, value in self.details.items() if key.startswith("registry:")]
        return {"schema_version": 1, "status": status,
                "generated_utc": datetime.now(timezone.utc).isoformat(),
                "hardware_accessed_by_verifier": False, "mode": "in_progress" if self.in_progress else "strict",
                "batch_directory": str(self.batch), "matrix_directory": str(self.matrix),
                "raw_validator_sha256": sha256(self.project / "hil/basepri_20261025/verify_evidence.py"),
                "coverage": {"expected_adversarial_images": 48,
                             "validated_adversarial_images": len(matrix_details),
                             "adversarial_pass_checks": sum(row["pass"] for row in matrix_details),
                             "expected_registry_images": 12,
                             "validated_registry_images": len(registry_details),
                             "registry_pass_checks": sum(row["pass"] for row in registry_details),
                             "expected_port_images": 74, "expected_strict_compilations": 97,
                             "real_failing_baselines": sum(
                                 "baseline:" + tag in self.details for tag in
                                 ("mutex_chain_baseline", "object_lifetime_baseline", "mutex_boundary_baseline")),
                             "mixed_wait_control_checks": self.details.get(
                                 "baseline:wait_mix_baseline", {}).get("pass", 0)},
                "errors": self.errors, "pending": self.pending,
                "captures": self.captures, "details": self.details,
                "limits": ["Finite tests do not prove absence of all bugs.",
                           "F446 Cortex-M4 hardware; Cortex-M7 compilation only.",
                           "USB contracts do not establish physical USB bus functionality.",
                           "Recursion overflow uses explicit boundary injection; cycle and handoff use public APIs.",
                           "25 existing unused main.c demo declarations are allowed per audit image; fixtures compile with -Werror.",
                           "Compiler stack frames do not establish total call-chain or interrupt WCET bounds."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--batch-dir", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--evidence-dir", type=Path)
    parser.add_argument("--matrix-dir", type=Path)
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--in-progress", action="store_true")
    args = parser.parse_args()
    batch = args.batch_dir.resolve()
    evidence = (args.evidence_dir or batch / "evidence").resolve()
    matrix = (args.matrix_dir or evidence / "matrix_final").resolve()
    verifier = BatchVerifier(batch, evidence, matrix, args.project_root.resolve(), args.in_progress)
    result = verifier.run()
    if result["status"] == "PASS":
        (batch / "verification_summary.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("status", "mode", "coverage", "errors", "pending")},
                     ensure_ascii=False, indent=2))
    return int(bool(result["errors"]) or (bool(result["pending"]) and not args.in_progress))


if __name__ == "__main__":
    sys.exit(main())
