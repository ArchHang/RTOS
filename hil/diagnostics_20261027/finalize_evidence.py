"""Reconcile final-code HIL, native/compile tests, fault SRAM and daily restore."""
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess

from verify_fault_snapshot import verify as verify_fault
from verify_results import require, verify_capture

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
MAIN_SHA = "32c1e6f3d50c754a8e09db169982551ae1cb4828760662ab106bdee482012874"
TOOLS = Path(r"E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins"
             r"\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32"
             r"_1.1.100.202309141235\tools\bin")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def relative(path):
    return path.relative_to(HERE).as_posix()


def latest_summary(directory):
    summaries = sorted((HERE / directory).glob("*/summary.json"))
    require(bool(summaries), "missing evidence: " + directory)
    path = summaries[-1]
    value = read_json(path)
    require(value["status"] == "PASS", "latest result failed: " + str(path))
    return {"path": relative(path), "sha256": digest(path), "result": value}


def verify_final_captures():
    records = []
    archived_oracle = HERE / "oracles/before_fault_context_fix/verify_results.py"
    allowed_oracles = {digest(HERE / "verify_results.py"), digest(archived_oracle)}
    for path in sorted((HERE / "evidence/final").glob("*/identity.json")):
        identity = read_json(path)
        require(identity["passed"] and identity["stage"] == "complete", "incomplete capture")
        for name, expected in identity["artifacts"].items():
            require(digest(path.parent / name) == expected, "changed capture: " + name)
        require(identity["oracle_sha256"] in allowed_oracles, "unknown capture oracle")
        inputs = read_json(path.parent / "sources.json")
        for name, info in inputs.items():
            if name == "hil/diagnostics_20261027/verify_results.py":
                require(info["sha256"] in allowed_oracles, "unrecognized oracle input")
            else:
                require(digest(ROOT / name) == info["sha256"], "changed build input: " + name)
        verification = verify_capture(path.parent)
        records.append({"tag": identity["tag"], "variant": identity["variant"],
                        "identity_sha256": digest(path), "verification": verification})
    expected = Counter({"diagnostic_checks": 13, "diagnostic_ring": 2,
        "diagnostic_bench": 4, "diagnostic_fault": 1, "daily": 2, "audit": 1,
        "selftest": 1, "boundary": 1, "lowlevel": 1, "bench": 1,
        "adversarial_mutex": 1, "adversarial_wait": 1, "adversarial_lifetime": 1,
        "adversarial_mutex_boundary": 1, "adversarial_registry": 1,
        "production_stress": 1, "basepri": 2})
    require(Counter(item["variant"] for item in records) == expected, "complete 35-capture batch")
    require(sum(item["verification"].get("pass", 0) for item in records) == 6067,
            "complete raw new-fixture check total")
    return records


def verify_restored():
    restored = HERE / "restored"
    metadata = read_json(restored / "run_daily_restored.json")
    firmware = restored / "firmware_daily_restored.elf"
    require(metadata["flashed"] and metadata["terminator_seen"]
            and not metadata["failure_lines"], "daily restore capture")
    flash = (restored / "flash_daily_restored.txt").read_text(encoding="utf-8-sig")
    require("066DFF555775514867092907" in flash
            and "Download verified successfully" in flash, "restored physical flash verified")
    require(digest(firmware).upper() == metadata["sha256"]
            and digest(firmware) == digest(ROOT / "Debug/f446_rtos.elf"), "restored ELF identity")
    log = (restored / "log_daily_restored.txt").read_text(encoding="utf-8-sig")
    require(log.count("System init done") == 1, "restored one actual boot")
    boot = log.split("System init done", 1)[1]
    require("task 0 running" in boot and "task 1 running" in boot, "both tasks after restore boot")
    require(read_json(restored / "equivalence.json")["allocated_image_identical"],
            "canonical allocated image equals verified daily OFF image")
    require(not (ROOT / "hil/fwsrc").exists() and not (ROOT / "Debug/hil").exists()
            and not (ROOT / "Debug/.hil_build_stamp").exists(), "variant artifacts removed")
    nm = subprocess.run([str(TOOLS / "arm-none-eabi-nm.exe"), "--defined-only",
                         str(firmware)], capture_output=True, text=True, check=True).stdout
    (restored / "defined_symbols.txt").write_text(nm, encoding="utf-8")
    require(not re.search(r"rtos_diag|rtos_internal_diag|diag_slots|diag_frozen", nm),
            "no linked diagnostic functions or record state in daily OFF")
    require(digest(ROOT / "Core/Src/main.c") == MAIN_SHA, "main preserved")
    return {"status": "PASS", "main_unchanged": True, "canonical_image_identical": True,
            "no_diagnostic_symbols": True, "task_0_and_1_after_boot": True,
            "firmware_sha256": digest(firmware)}


def main():
    captures = verify_final_captures()
    host = latest_summary("host/evidence")
    strict = latest_summary("strict/evidence")
    equivalence = latest_summary("equivalence")
    oracle = latest_summary("oracle_evidence")
    configuration = latest_summary("config_bounds/evidence")
    fault_oracle = latest_summary("fault_oracle_evidence")
    require(host["result"]["source_identity_stable"]
            and strict["result"]["source_identity_stable"]
            and equivalence["result"]["source_identity_stable"], "stable non-HIL source inputs")
    fault_directory = HERE / "fault_debug/20261027_165907_9971"
    fault = verify_fault(fault_directory)
    restored = verify_restored()
    performance_path = HERE / "performance_analysis/20261027_165203_421154/report.json"
    performance = read_json(performance_path)
    summary = {"status": "PASS", "completed_utc": datetime.now(timezone.utc).isoformat(),
        "captures": len(captures), "results": captures,
        "functional_configurations": 12, "functional_checks_per_configuration": 472,
        "new_fixture_checks": 6067, "legacy_integration_captures": 13,
        "host": host, "strict": strict, "disabled_equivalence": equivalence,
        "oracle": oracle, "configuration_bounds": configuration,
        "fault_oracle": fault_oracle,
        "fault_sram": {"path": relative(fault_directory), "result": fault},
        "restored": restored, "performance": {"path": relative(performance_path),
            "sha256": digest(performance_path), "result": performance},
        "limits": ["Native model is not an ARM interrupt emulator or sanitizer",
            "M7 checks are code generation only, not hardware validation",
            "Frozen bounded history can lose an old WAIT and must report UNKNOWN",
            "Terminal fault SRAM is not persistent across reset/power loss",
            "Performance samples are complete-work measurements, not WCET",
            "Historical intermediate/failed attempts are not counted as final passes"]}
    (HERE / "final_summary.json").write_text(json.dumps(summary, indent=2) + "\n",
                                             encoding="utf-8")
    print(json.dumps({"status": "PASS", "final_captures": len(captures),
                      "main_preserved": True, "daily_restored": True}))


if __name__ == "__main__":
    main()
