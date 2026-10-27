#!/usr/bin/env python3
"""Read-only acceptance of the reorganized F446 RTOS's retained HIL evidence.

Default: require every planned capture and compilation. --in-progress reports
missing work and never writes a result. --output creates a new report exclusively.
No compiler, subprocess, network, serial port, debug probe or Git mutation is used.
Saved ELF bytes are mandatory. Offline Python oracles are identified separately
from the firmware/build source manifests because they do not build the firmware.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import importlib.util
import itertools
import json
from pathlib import Path, PurePosixPath
import re
import sys

sys.dont_write_bytecode = True
BATCH = Path(__file__).resolve().parent
PROJECT = BATCH.parents[1]
RAW_HASH = "9AFE41ACC36726438E64F5AC7C8408AD5EBF077F4E974872964AAB62DE7959A2"
ADV_HASH = "3447F08BF7FA6FF6CA13D77260AD2D583268077A61D77B974DF902FB31FB4897"
MAIN_HASH = "32C1E6F3D50C754A8E09DB169982551AE1CB4828760662AB106BDEE482012874"
PREFLIGHT_FIXTURE_HASH = "92ED9861E035B4A33320EAE227AFDFF1748B2CAB9E784E2277A305DC76B5E10F"
OPTS = ("O0", "O2", "O3")
CONFIGS = tuple(itertools.product(("basepri", "primask"), (0, 1), OPTS))
VARIANTS = {
    "am": ("AM", "adversarial_mutex", 910),
    "aw": ("AW", "adversarial_wait", 1600),
    "of": ("OF", "adversarial_lifetime", 80),
    "amb": ("AMB", "adversarial_mutex_boundary", 87),
}
SOURCE_DIRS = ("RTOS/Core/Src", "RTOS/IPC", "RTOS/Memory", "RTOS/Services",
               "RTOS/USB", "RTOS/Port/GCC/ARM_CM4F_CM7F")
WARNING = re.compile(r"^(?:make(?:\.exe)? : )?\.\./Core/Src/main\.c:\d+:\d+: warning: "
                     r".+ \[-Wunused-(?:function|variable)\]\s*$")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1048576), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def read_text(path):
    return path.read_text(encoding="utf-8-sig", errors="replace")


def load_module(name, path, expected=None):
    if expected is not None and sha256(path) != expected:
        raise ValueError("Historical oracle changed: " + str(path))
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RAW = load_module("refactor_raw_oracle", PROJECT / "hil/basepri_20261025/verify_evidence.py", RAW_HASH)
ADV = load_module("refactor_adversarial_oracle",
                  PROJECT / "hil/adversarial_20261025/verify_evidence.py", ADV_HASH)
# Only the reviewed module basename changes; the historical file remains intact.
RAW.EXPECTED_MODULES = tuple("rtos_usb_device.c" if index == 13 else
                             "rtos_usb_osal.c" if index == 14 else name
                             for index, name in enumerate(RAW.EXPECTED_MODULES))


class CaptureDateTime(datetime):
    @classmethod
    def fromisoformat(cls, value):
        return datetime.fromisoformat(re.sub(r"(\.\d{6})\d+(?=[+-]\d{2}:\d{2}$)", r"\1", value))


RAW.datetime = CaptureDateTime


class Results:
    def __init__(self, evidence, in_progress=False):
        self.evidence = evidence
        self.project = PROJECT
        self.raw = RAW
        self.in_progress = in_progress
        self.errors, self.pending, self.details = [], [], {}
        self.validators, self.captures, self.identities = {}, {}, {}
        self.manifest_identity = None
        self.sources = sorted(path for relative in SOURCE_DIRS
                              for path in (PROJECT / relative).glob("*.c"))
        self.required = self.source_required()

    def require(self, condition, message):
        RAW.require(condition, message)

    def file(self, directory, name):
        path = directory / name
        if not path.is_file():
            raise RAW.PendingEvidence(str(path))
        return path

    def json(self, directory, name):
        return json.loads(read_text(self.file(directory, name)))

    def check(self, name, callback):
        try:
            self.details[name] = callback()
        except RAW.PendingEvidence as error:
            self.pending.append({"check": name, "missing": str(error)})
        except (RAW.EvidenceError, OSError, ValueError, KeyError, TypeError,
                IndexError, AttributeError, RAW.struct.error) as error:
            self.errors.append({"check": name, "error": str(error)})

    def source_required(self):
        paths = set()
        for relative in ("RTOS", "Core", "hil/fw"):
            paths.update(path for path in (PROJECT / relative).rglob("*")
                         if path.is_file() and path.suffix.lower() in (".c", ".h", ".s"))
        paths.update(path for path in (PROJECT / "hil/templates").iterdir()
                     if path.is_file() and path.suffix in (".mk", ".template"))
        paths.update(PROJECT / relative for relative in
                     ("hil/build_fw.ps1", "hil/build_layout.ps1", "hil/run_hil.ps1",
                      "hil/restore_project.ps1", "hil/repro_all.ps1", ".cproject"))
        paths.update(BATCH.glob("*.ps1"))
        paths.update(PROJECT.glob("*.ld"))
        return {path.relative_to(PROJECT).as_posix() for path in paths}

    def source_manifest(self, path):
        if not path.is_file():
            raise RAW.PendingEvidence(str(path))
        entries = json.loads(read_text(path))
        self.require(isinstance(entries, list), "Source manifest must be an array")
        identity, seen = {}, set()
        for entry in entries:
            relative = entry["path"]
            self.require(isinstance(relative, str) and relative == relative.replace("\\", "/")
                         and ":" not in relative and not relative.startswith("/")
                         and all(part not in ("", ".", "..") for part in relative.split("/")),
                         "Noncanonical source path: " + str(relative))
            self.require(PurePosixPath(relative).as_posix() == relative
                         and relative.casefold() not in seen, "Duplicate source path: " + relative)
            seen.add(relative.casefold())
            target = (PROJECT / relative).resolve()
            self.require(target.is_relative_to(PROJECT), "Escaping source path: " + relative)
            recorded = entry["sha256"]
            self.require(isinstance(recorded, str) and re.fullmatch(r"[0-9A-Fa-f]{64}", recorded)
                         and target.is_file() and sha256(target) == recorded.upper(),
                         "Tested source differs from retained source: " + relative)
            identity[relative] = recorded.upper()
        self.require(set(identity) == self.required,
                     "Source inventory differs; missing=" + str(sorted(self.required - set(identity)))
                     + " extra=" + str(sorted(set(identity) - self.required)))
        if self.manifest_identity is None:
            self.manifest_identity = identity
        self.require(identity == self.manifest_identity, "Source snapshots differ between captures")
        return {"files": len(identity), "sha256": sha256(path)}

    def build_proof(self, directory, tag, fixture=None, configuration=None):
        stdout = read_text(self.file(directory, "build_" + tag + ".txt"))
        detail = read_text(self.file(directory, "build_detail_" + tag + ".txt"))
        self.require(not re.search(r"(?:fatal )?error:|undefined reference|No rule to make target",
                                   stdout + "\n" + detail, re.I), "Build failed: " + tag)
        warnings = [line for line in detail.splitlines() if "warning:" in line]
        self.require(all(WARNING.fullmatch(line) for line in warnings),
                     "Unexpected build warnings: " + tag)
        if tag != "final_daily":
            self.require(re.search(r"built '[a-z_]+' OK", stdout) is not None
                         and re.search(r"\bwarnings=" + str(len(warnings)) + r"\b", stdout),
                         "Missing successful build/diagnostic count: " + tag)
        commands = [line for line in detail.splitlines()
                    if "arm-none-eabi-gcc " in line and " -c " in line]
        if fixture:
            matching = [line for line in commands
                        if re.search(r'hil/fwsrc/' + re.escape(fixture) + r'\.c[" ]', line)]
            self.require(len(matching) == 1 and all(flag in matching[0].split()
                         for flag in ("-Wextra", "-Werror")), "Missing strict fixture command: " + tag)
        if configuration:
            mask, assertions, optimization = configuration
            self.require(len(warnings) == 25, "Expected 25 pre-existing unused demo warnings: " + tag)
            matching = []
            for source in self.sources:
                relative = source.relative_to(PROJECT).as_posix()
                found = [line for line in commands if ('../' + relative + '"') in line]
                self.require(len(found) == 1, "Missing production compile command: " + relative)
                matching.extend(found)
            if fixture:
                matching.extend(line for line in commands if "hil/fwsrc/" + fixture + '.c"' in line)
            for command in matching:
                flags = command.split()
                self.require("-" + optimization in flags
                             and ("-DRTOS_CONFIG_USE_BASEPRI=0" in flags) == (mask == "primask")
                             and ("-DRTOS_CONFIG_ASSERT_ENABLED=0" in flags) == (assertions == 0),
                             "Compile configuration differs from tag: " + tag)
                self.require(("-DRTOS_HIL_OBJECT_LIFETIME=1" in flags) == tag.startswith("of_"),
                             "Lifetime hook configuration differs: " + tag)
        return {"warnings": len(warnings), "commands": len(commands)}

    def pre_capture(self, directory, tag):
        key = str(directory / tag)
        if key in self.identities:
            return
        source = directory / ("sources_" + tag + ".json")
        if tag == "final_daily" and not source.exists():
            source = BATCH / "sources_final.json"
        source_identity = self.source_manifest(source)
        self.file(directory, "firmware_" + tag + ".elf")
        self.build_proof(directory, tag)
        self.identities[key] = source_identity

    def validator(self, directory):
        key = str(directory)
        if key not in self.validators:
            owner = self

            class RetainedVerifier(RAW.Verifier):
                def capture(self, tag, require_exit_isb=False):
                    owner.pre_capture(self.evidence, tag)
                    text = super().capture(tag, require_exit_isb=require_exit_isb)
                    owner.captures[str(self.evidence / tag)] = self.captures[tag]
                    return text

            self.validators[key] = RetainedVerifier(directory, PROJECT, self.in_progress)
        return self.validators[key]

    def capture(self, directory, tag, exit_isb=False):
        return self.validator(directory).capture(tag, require_exit_isb=exit_isb)

    def index(self, directory, filename, wanted, callback):
        entries = self.json(directory, filename)
        self.require(isinstance(entries, list), "Results index must be an array: " + filename)
        tags = [entry["tag"] for entry in entries]
        self.require(len(tags) == len(set(tags)) and set(tags) <= set(wanted),
                     "Duplicate/unexpected result tags: " + filename)
        if set(tags) != set(wanted):
            self.pending.append({"check": filename, "missing_tags": sorted(set(wanted) - set(tags))})
        for entry in entries:
            tag = entry["tag"]
            if self.in_progress and entry.get("passed") is not True and "error" not in entry:
                self.pending.append({"check": tag, "stage": entry.get("stage", "running")})
                continue
            self.check(directory.name + ":" + tag, lambda entry=entry: callback(entry))
        return {"expected": len(wanted), "indexed": len(entries)}

    def matrix_image(self, directory, entry, suite):
        tag = entry["tag"]
        prefix, variant, unused = VARIANTS[suite]
        self.require(entry.get("passed") is True and entry.get("stage") == "complete"
                     and entry.get("suite") == suite and entry.get("prefix") == prefix
                     and entry.get("variant") == variant, "Incomplete matrix runner: " + tag)
        config = (entry["critical_mask"], entry["assertions"], entry["optimization"])
        self.require(tag == "%s_%s_a%d_%s" % ((suite,) + config), "Matrix identity differs: " + tag)
        path = self.file(directory, "sources_" + tag + ".json")
        self.require(entry.get("source_manifest") == path.name
                     and entry.get("source_manifest_sha256") == sha256(path),
                     "Matrix source-manifest identity differs: " + tag)
        self.build_proof(directory, tag, variant, config)
        result = self.adversarial(directory, tag, suite)
        self.require(entry.get("summary") == result["summary"], "Matrix/raw summary differs: " + tag)
        return result

    def adversarial(self, directory, tag, suite, baseline=False):
        result = ADV.BatchVerifier.adversarial(self, directory, tag, suite, baseline)
        metadata = {
            "am": "AM|META|public_api_only=1 private_tcb_writes=0 depths=2,3,6 owners_base=15..20 high=4",
            "aw": "AW|META|rounds=8 workers=4 syscall_irq=TIM7 priority=5 highirq=TIM2 priority=0 finite_waits=1",
            "of": "OF|META|rounds=8 cases_per_round=10 real_irq=TIM7 priority=5 static_storage=1 timeout_ticks=2 hypothesis_requires_measurement=1",
            "amb": "AMB|META|recursion_boundary_injection=1 owner_handoff_public_api=1 finite_cycle_public_api=1",
        }
        text = self.capture(directory, tag)
        self.require(RAW.one(text, VARIANTS[suite][0] + "|META|") == metadata[suite],
                     "Adversarial setup/provenance metadata differs: " + tag)
        return result

    def registry_image(self, directory, entry):
        tag = entry["tag"]
        config = (entry["critical_mask"], entry["assertions"], entry["optimization"])
        self.require(tag == "registry_%s_a%d_%s" % config
                     and entry.get("passed") is True and entry.get("stage") == "complete"
                     and entry.get("variant") == "adversarial_registry"
                     and entry.get("expected_checks") == 259, "Registry identity/count differs")
        source = self.file(directory, "sources_" + tag + ".json")
        self.require(entry.get("source_manifest") == source.name
                     and entry.get("source_manifest_sha256") == sha256(source),
                     "Registry manifest identity differs")
        self.build_proof(directory, tag, "adversarial_registry", config)
        text = self.capture(directory, tag)
        self.require(not RAW.FAIL_PATTERN.search(text), "Registry reported failure")
        summary_line = RAW.one(text, "AR|SUMMARY|")
        summary = RAW.fields(summary_line)
        self.require(summary.get("pass") == summary.get("checks") == 259
                     and summary.get("fail") == 0 and summary.get("cases") == 2
                     and entry.get("summary") == summary_line, "Registry summary differs")
        self.require(RAW.fields(RAW.one(text, "AR|DONE|")).get("status") == 0,
                     "Registry terminal result failed")
        meta = RAW.fields(RAW.one(text, "AR|META|"))
        self.require(meta.get("owners") == meta.get("capacity") == 8
                     and meta.get("worker_limit") == 9 and meta.get("public_api_only") == 1
                     and meta.get("private_tcb_writes") == 0 and meta.get("finite_wait_ticks", 0) > 0,
                     "Registry public API/capacity provenance differs")
        cases = [RAW.fields(line) for line in RAW.rows(text, "AR|CASE|")]
        self.require(len(cases) == 2 and {(row.get("id"), row.get("name")) for row in cases}
                     == {(1, "saturated_handoff"), (2, "deleted_multi_owner")}, "Registry cases differ")
        checks = [RAW.fields(line) for line in RAW.rows(text, "AR|PASS|")]
        self.require(len(checks) == 259 and [row.get("id") for row in checks] == list(range(1, 260))
                     and {row.get("case") for row in checks} == {1, 2}
                     and all(type(row.get("actual")) is int and row.get("actual") == row.get("expected")
                             for row in checks), "Registry check identities/values differ")
        graph = RAW.fields(RAW.one(text, "AR|GRAPH|"))
        expected = {"phase": "m_plus_one", "registered": 8, "distinct_current_owners": 8,
                    "detached_owner": 0, "old_base": 15, "old_effective": 15,
                    "new_owner": 8, "new_effective": 4, "old_owns": 0}
        self.require(all(graph.get(key) == value for key, value in expected.items()),
                     "Registry did not observe the M+1 owner graph")
        handoffs = [RAW.fields(line) for line in RAW.rows(text, "AR|HANDOFF|")]
        self.require([(row.get("phase"), row.get("lock")) for row in handoffs]
                     == [("deleted", 0), ("deleted", 1), ("released", 0),
                         ("other_chain_retained", 1), ("released", 1)],
                     "Registry handoff snapshots differ")
        for row in handoffs:
            lock = row["lock"]
            owner = lock + (3 if row["phase"] == "released" else 1)
            self.require(row.get("owner") == row.get("expected_owner") == owner
                         and row.get("effective") == row.get("expected_effective") == 6 + lock
                         and row.get("waiters") == int(row["phase"] != "released"),
                         "Registry ownership/inheritance changed during transfer")
        return {"pass": 259, "fail": 0, "cases": 2, "summary": summary_line}

    def compile_checks(self):
        directory = self.evidence / "compile"
        entries = self.json(directory, "compile_results.json")
        sources = {path.stem: path.relative_to(PROJECT).as_posix() for path in self.sources}
        self.require(len(sources) == 16, "Unexpected production source count")
        wanted = {"m%d_a%d_%s_%s" % (mask, assertions, opt, source)
                  for mask, assertions, opt, source in itertools.product((0, 1), (0, 1), OPTS, sources)}
        wanted.add("cm7_port_compile_only")
        self.require(isinstance(entries, list) and len(entries) == 193
                     and Counter(entry["tag"] for entry in entries) == Counter(wanted),
                     "Strict compilation coverage differs from 192 CM4 + one CM7")
        for entry in entries:
            tag = entry["tag"]
            self.require(entry.get("passed") is True and entry.get("warnings") == 0,
                         "Strict compilation failed: " + tag)
            if tag == "cm7_port_compile_only":
                self.require(entry.get("hardware_test") is False, "CM7 is compile-only")
                supplement = self.json(directory, "cm7_supplement.json")
                self.require(supplement.get("tag") == tag
                             and supplement.get("passed") is True,
                             "Missing successful CM7 rerun identity")
                data = supplement
                source = "RTOS/Port/GCC/ARM_CM4F_CM7F/port.c"
                log_name = supplement.get("log", "compile_cm7_supplement.txt")
                flags = ("-mcpu=cortex-m7", "-mfpu=fpv5-d16", "-O2")
            else:
                match = re.fullmatch(r"m([01])_a([01])_(O[023])_(\w+)", tag)
                self.require(match is not None, "Malformed compile tag")
                mask, assertions, opt, stem = match.groups()
                source = sources[stem]
                data = entry
                log_name = "compile_" + tag + ".txt"
                flags = ("-mcpu=cortex-m4", "-mfpu=fpv4-sp-d16", "-" + opt,
                         "-DRTOS_CONFIG_USE_BASEPRI=" + mask,
                         "-DRTOS_CONFIG_ASSERT_ENABLED=" + assertions, "-fstack-usage")
                stack = read_text(self.file(directory, tag + ".su"))
                self.require("\t" in stack, "Missing retained stack-usage rows: " + tag)
            self.require(data.get("source") == source
                         and str(data.get("sha256", "")).upper() == sha256(PROJECT / source),
                         "Strict compile source identity differs: " + tag)
            arguments = data.get("arguments")
            self.require(isinstance(arguments, list)
                         and all(flag in arguments for flag in
                                 ("-std=gnu11", "-mthumb", "-mfloat-abi=hard",
                                  "-Wall", "-Wextra", "-Werror", "-c", "-o") + flags),
                         "Strict compiler options differ: " + tag)
            operand = arguments[arguments.index("-c") + 1]
            self.require(Path(operand).resolve() == (PROJECT / source).resolve(),
                         "Compile source operand differs: " + tag)
            log = read_text(self.file(directory, log_name))
            self.require(not re.search(r"\b(?:warning|error|fatal)\s*:", log, re.I),
                         "Strict compile log contains diagnostics: " + tag)
            if tag == "cm7_port_compile_only":
                original = read_text(self.file(directory, "compile_cm7_port_only.txt"))
                self.require(not re.search(r"\b(?:warning|error|fatal)\s*:", original, re.I),
                             "Original CM7 compile contains diagnostics")
        return {"compilations": 193, "cm4_configurations": 12,
                "production_sources": 16, "cm7_compile_only": 1}

    def benchmark(self, directory, tag):
        text = self.capture(directory, tag)
        self.require(not RAW.FAIL_PATTERN.search(text), "Benchmark failed")
        found = {}
        for line in RAW.rows(text, "BM|"):
            match = re.match(r"BM\|(\d+)\|([^|]+)\|", line)
            if not match:
                continue
            index = int(match.group(1))
            self.require(index not in found, "Duplicate benchmark")
            row = RAW.fields(line)
            self.require(RAW.number(row, "N") > 0
                         and 0 < RAW.number(row, "min") <= RAW.number(row, "avg")
                         <= RAW.number(row, "max"), "Invalid benchmark statistics")
            found[index] = {"name": match.group(2), **row}
        self.require(set(found) == set(range(8)), "BM0..BM7 incomplete")
        RAW.one(text, "BM|DONE|")
        return {"benchmarks": found, "scope": "Observed cycles; no fixed WCET or speedup claim"}

    def regression(self, directory, entry, specification):
        tag = entry["tag"]
        self.require(entry.get("passed") is True
                     and bool(entry.get("negative")) == bool(specification and specification[-1]),
                     "Regression runner classification differs: " + tag)
        if tag in ("final_audit", "final_selftest", "final_boundary", "final_lowlevel"):
            return self.validator(directory).functional(tag)
        if tag == "final_nohook_bench":
            return self.benchmark(directory, tag)
        return self.validator(directory).production(tag, *specification)

    def preflight_fixture_failure(self):
        """Classify one retained failed oracle attempt, outside final acceptance.

        This permits no exception in source_manifest(). Only this fixed archive
        may contain the old fixture bytes; every other recorded build input must
        still match the retained source. The failed capture is never added to
        self.captures and therefore contributes no successful hardware image.
        """
        archive = self.evidence / "preflight/before_guard_mapping_fix"
        directory = archive / "port"
        tag = "guard_c13_p0"
        recorded = self.file(archive, "basepri_guard.c.recorded")
        self.require(sha256(recorded) == PREFLIGHT_FIXTURE_HASH,
                     "Unexpected preflight fixture identity")
        old_fixture = read_text(recorded).replace("\r\n", "\n")
        current_path = PROJECT / "hil/fw/Src/basepri_guard.c"
        current_fixture = read_text(current_path).replace("\r\n", "\n")
        corrected_mapping = '"rtos_usb_device.c", "rtos_usb_osal.c", "port.c", "port.c"'
        old_mapping = '"rtos_usb_osal.c", "rtos_usb_osal.c", "port.c", "port.c"'
        self.require(current_fixture.count(corrected_mapping) == 1
                     and old_fixture == current_fixture.replace(corrected_mapping, old_mapping, 1),
                     "Fixture change exceeds the reviewed case-13 module mapping")
        manifest = self.json(directory, "sources_" + tag + ".json")
        self.require(isinstance(manifest, list), "Preflight source manifest must be an array")
        seen = set()
        for entry in manifest:
            relative = entry["path"]
            self.require(isinstance(relative, str) and "\\" not in relative
                         and ":" not in relative and not relative.startswith("/")
                         and all(part not in ("", ".", "..") for part in relative.split("/"))
                         and relative in self.required and relative.casefold() not in seen,
                         "Invalid/duplicate preflight source path")
            seen.add(relative.casefold())
            target = (PROJECT / relative).resolve()
            self.require(target.is_relative_to(PROJECT), "Escaping preflight source path")
            expected = (PREFLIGHT_FIXTURE_HASH if relative == "hil/fw/Src/basepri_guard.c"
                        else sha256(target))
            self.require(str(entry["sha256"]).upper() == expected,
                         "Unexplained preflight source difference: " + relative)
        self.require(seen == {relative.casefold() for relative in self.required},
                     "Preflight source inventory differs")
        self.file(directory, "firmware_" + tag + ".elf")
        verifier = RAW.Verifier(directory, PROJECT, False)
        text = verifier.capture(tag)
        self.build_proof(directory, tag)
        arm = RAW.fields(RAW.one(text, "BP|GUARD_ARM|"))
        assertion = RAW.fields(RAW.one(text, "BP|ASSERT|"))
        self.require(arm.get("case") == assertion.get("case") == 13 and arm.get("prio") == 0
                     and assertion.get("ipsr") == 71 and assertion.get("line", 0) > 0
                     and assertion.get("file") == "../RTOS/USB/rtos_usb_device.c",
                     "Preflight assertion did not come from the real device API/TIM7")
        line = RAW.one(text, "BP|ASSERT|")
        self.require("rtos_port_isr_priority() >= RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY" in line
                     and "((rtos_port_in_isr()) == RTOS_FALSE)" in line,
                     "Preflight assertion is not the expected priority-contract guard")
        device_lines = read_text(PROJECT / "RTOS/USB/rtos_usb_device.c").splitlines()
        line_number = assertion["line"]
        self.require(line_number <= len(device_lines)
                     and device_lines[line_number - 1].strip() == "RTOS_ASSERT_ISR_OK();",
                     "Preflight assertion source line differs from retained production code")
        failures = [line for line in text.splitlines() if RAW.FAIL_PATTERN.search(line)]
        expected_failure = "BP|GUARD|FAIL|case=13 prio=0 reason=assertion_source_and_condition_checked"
        self.require(failures == [expected_failure]
                     and RAW.one(text, "BP|GUARD|") == expected_failure
                     and RAW.fields(RAW.one(text, "BP|DONE|")).get("status") == 1,
                     "Preflight contains an unrelated failure or successful terminal status")
        detail = read_text(self.file(directory, "build_detail_" + tag + ".txt"))
        fixture_commands = [line for line in detail.splitlines()
                            if "arm-none-eabi-gcc " in line and "hil/fwsrc/basepri_guard.c" in line]
        self.require(len(fixture_commands) == 1
                     and "-DBASEPRI_GUARD_CASE=13U" in fixture_commands[0].split()
                     and "-DBASEPRI_GUARD_PRIORITY=0U" in fixture_commands[0].split()
                     and "-Wl,--wrap=rtos_assert_fail" in detail,
                     "Preflight build does not prove the intended wrapped case-13 test")
        return {"classification": "fixture_failure", "accepted_as_final_pass": False,
                "tag": tag, "fixture_sha256": PREFLIGHT_FIXTURE_HASH,
                "reason": "case 13 expected OSAL filename while CDC availability correctly asserted in device module",
                "capture": verifier.captures[tag], "source_files": len(manifest)}

    def restoration(self):
        directory = self.evidence / "daily"
        text = self.capture(directory, "final_daily")
        self.require("System init done" in text and "task 0 running" in text
                     and "task 1 running" in text,
                     "Daily firmware startup/progress missing")
        self.require(not RAW.FAIL_PATTERN.search(text), "Daily firmware emitted FAIL")
        self.require(sha256(PROJECT / "Core/Src/main.c") == MAIN_HASH, "main.c changed")
        for relative in ("hil/fwsrc", "Debug/hil", "Debug/.hil_build_stamp"):
            self.require(not (PROJECT / relative).exists(), "Variant artifacts remain: " + relative)
        mapping = {"makefile.template": "makefile", "sources.mk.template": "sources.mk",
                   "objects.mk.template": "objects.mk", "objects.list.template": "objects.list"}
        for path in (PROJECT / "hil/templates").glob("*.mk"):
            relative = path.stem.replace("_", "/")
            # Directory names containing underscores need the authoritative list.
            explicit = {
                "AI_output_int8": "AI/output/int8", "Core_Startup": "Core/Startup",
                "Drivers_STM32F4xx_HAL_Driver_Src": "Drivers/STM32F4xx_HAL_Driver/Src",
                "RTOS_Port_GCC_ARM_CM4F_CM7F": "RTOS/Port/GCC/ARM_CM4F_CM7F",
            }
            mapping[path.name] = explicit.get(path.stem, relative) + "/subdir.mk"
        mapping["subdir.mk.template"] = "Core/Src/subdir.mk"
        for template, relative in mapping.items():
            wanted = read_text(PROJECT / "hil/templates" / template).replace("\r\n", "\n")
            retained = read_text(self.file(PROJECT / "Debug", relative)).replace("\r\n", "\n")
            self.require(wanted == retained, "Daily build rule is not restored: " + relative)
        return {"daily_boot": True, "restored_build_files": len(mapping), "main_unchanged": True}

    def run(self):
        self.check("compile", self.compile_checks)
        self.check("preflight_fixture_failure", self.preflight_fixture_failure)
        matrix = self.evidence / "matrix"
        wanted = {"%s_%s_a%d_%s" % ((suite,) + config)
                  for suite, config in itertools.product(VARIANTS, CONFIGS)}
        self.check("matrix_index", lambda: self.index(
            matrix, "matrix_results.json", wanted,
            lambda entry: self.matrix_image(matrix, entry, entry["suite"])))
        registry = self.evidence / "registry"
        wanted = {"registry_%s_a%d_%s" % config for config in CONFIGS}
        self.check("registry_index", lambda: self.index(
            registry, "registry_results.json", wanted, lambda entry: self.registry_image(registry, entry)))
        port = self.evidence / "port"
        validator = self.validator(port)
        port_suites = (
            ("matrix_results.json", {"matrix_%s_a%d_%s" % config for config in CONFIGS}, validator.matrix),
            ("blocking_results.json", {"blocking_%s_a%d_%s" % config for config in CONFIGS}, validator.blocking),
            ("nesting_results.json", {"nesting_%s_%s" % config
             for config in itertools.product(("basepri", "primask"), OPTS)}, validator.nesting),
            ("guard_results.json", {"guard_c%d_p%d" % pair
             for pair in itertools.product(range(1, 15), (0, 4, 5))}
             | {"guard_c15_p0", "guard_c16_p0"}, validator.guard),
        )
        for filename, wanted, callback in port_suites:
            def image(entry, callback=callback):
                self.require(entry.get("passed") is True, "Port runner did not pass")
                return callback(entry["tag"])
            self.check("port:" + filename, lambda filename=filename, wanted=wanted, image=image:
                       self.index(port, filename, wanted, image))
        regression = self.evidence / "regression"
        functional = {tag: None for tag in ("final_audit", "final_selftest", "final_boundary",
                                            "final_lowlevel", "final_nohook_bench")}
        production = {
            "final_production_cap1": (1, 30, 1, False),
            "final_production_cap8": (8, 30, 305419896, False),
            "final_production_cap32": (32, 30, 2779096485, False),
            "final_production_negative": (8, 6, 1, True),
        }
        soak = {"final_production_soak120": (8, 120, 3735928559, False)}
        for directory, filename, wanted in (
                (regression, "regressions_functional.json", functional),
                (regression, "regressions_production.json", production),
                (self.evidence / "soak", "regressions_soak.json", soak)):
            self.check(filename, lambda directory=directory, filename=filename, wanted=wanted:
                       self.index(directory, filename, wanted,
                                  lambda entry: self.regression(directory, entry, wanted[entry["tag"]])))
        self.check("restoration", self.restoration)
        status = "FAIL" if self.errors else ("IN_PROGRESS" if self.pending else "PASS")
        return {
            "status": status, "checked_utc": datetime.now(timezone.utc).isoformat(),
            "evidence": str(self.evidence),
            "coverage": {"planned_hardware_images": 145, "verified_captures": len(self.captures),
                         "adversarial_images": 60, "port_images": 74,
                         "regression_images": 10, "daily_images": 1,
                         "strict_compilations": 193, "soak_seconds": 120},
            "source_manifest_files": len(self.manifest_identity or {}),
            "offline_oracles": {
                "verify_results_sha256": sha256(Path(__file__)),
                "verify_layout_sha256": sha256(BATCH / "verify_layout.py"),
                "historical_raw_sha256": RAW_HASH, "historical_adversarial_sha256": ADV_HASH,
                "reviewed_adaptation": "USB guard case 13 uses device and case 14 uses OSAL; benchmark has no dated fixed-cycle requirement",
            },
            "limitations": [
                "Finite synthetic HIL coverage does not prove absence of every bug.",
                "USB transport audit uses endpoint wrappers; physical USB host interoperability is not demonstrated.",
                "Cortex-M7 port is compiled only; real-board captures use F446 Cortex-M4.",
                "Offline Python oracles are separately identified, not firmware build inputs.",
                "Retained ELF bytes are required locally for identity checks and are ignored by Git.",
            ],
            "errors": self.errors, "pending": self.pending,
            "details": self.details, "captures": self.captures,
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, default=BATCH / "evidence")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--in-progress", action="store_true")
    arguments = parser.parse_args()
    if arguments.output and arguments.in_progress:
        parser.error("--in-progress never writes a result file")
    if arguments.output and arguments.output.exists():
        parser.error("Refusing to overwrite " + str(arguments.output))
    result = Results(arguments.evidence.resolve(), arguments.in_progress).run()
    print(json.dumps({key: value for key, value in result.items()
                      if key not in ("details", "captures")}, ensure_ascii=False, indent=2))
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        with arguments.output.open("x", encoding="utf-8") as handle:
            json.dump(result, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
    raise SystemExit(0 if result["status"] == "PASS" else 1)


if __name__ == "__main__":
    main()

