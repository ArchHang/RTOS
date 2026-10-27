"""Build and execute native tests against the actual production channel C.

The host port scripts waits; it does not emulate Cortex-M exception timing.
No sanitizer result is claimed unless a separate sanitizer build really runs.
"""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
if not (ROOT / "RTOS/IPC/rtos_channel.c").is_file():
    raise RuntimeError("Host suite must remain under <project>/hil/<batch>/host")
DEFAULT_COMPILER = Path(r"C:\Users\23350\.conda\envs\mingw\Scripts\gcc.exe")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def execute(command, timeout):
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=timeout)
    return result.returncode, result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER)
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    if not args.compiler.is_file():
        raise SystemExit("Native compiler is unavailable: " + str(args.compiler))
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    output = args.out_dir.resolve() if args.out_dir else HERE / "evidence" / stamp
    if output.exists():
        raise SystemExit("Refusing to overwrite evidence: " + str(output))
    output.mkdir(parents=True)
    build = HERE / ".build" / stamp
    build.mkdir(parents=True)
    sources = [ROOT / "RTOS/IPC/rtos_channel.c", HERE / "host_port.c", HERE / "channel_model.c"]
    if not sources[0].is_file():
        raise SystemExit("Production source has not been created: " + str(sources[0]))
    identity_paths = sorted(set(sources + list(HERE.glob("*.c")) + list(HERE.glob("*.h"))
                                + list((ROOT / "RTOS/Core/Inc").glob("*.h"))
                                + list((ROOT / "RTOS/Core/Private").glob("*.h"))
                                + [Path(__file__).resolve()]))
    identities = {path.relative_to(ROOT).as_posix(): digest(path) for path in identity_paths}
    (output / "sources.json").write_text(json.dumps(identities, indent=2) + "\n", encoding="utf-8")
    version_code, version = execute([str(args.compiler), "--version"], 30)
    if version_code:
        raise SystemExit(version)
    (output / "compiler.txt").write_text(version, encoding="utf-8")
    results = []
    matrix = [(optimization, suite) for optimization in ("O0", "O3")
              for suite in ("model", "boundaries", "regions", "disabled")]
    for optimization, suite in matrix:
        tag = optimization + "_" + suite
        executable = build / ("channel_" + tag + (".exe" if os.name == "nt" else ""))
        enabled = "0" if suite == "disabled" else "1"
        command = [str(args.compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-" + optimization, "-DRTOS_CONFIG_USE_CHANNEL=" + enabled,
                   "-DRTOS_CONFIG_ASSERT_ENABLED=0", "-include", str(HERE / "host_port.h"),
                   "-I" + str(HERE), "-I" + str(ROOT / "RTOS/Core/Inc"),
                   "-I" + str(ROOT / "RTOS/Core/Private")]
        chosen = sources
        if suite in ("boundaries", "regions"):
            chosen = [HERE / "host_port.c", HERE / ("channel_" + suite + ".c")]
        elif suite == "disabled":
            chosen = [sources[0], HERE / "channel_disabled.c"]
        command += [str(path) for path in chosen] + ["-o", str(executable)]
        (output / ("command_" + tag + ".json")).write_text(
            json.dumps(command, indent=2) + "\n", encoding="utf-8")
        code, diagnostics = execute(command, 120)
        (output / ("compile_" + tag + ".txt")).write_text(diagnostics, encoding="utf-8")
        if code or re.search(r"\b(?:warning|error):", diagnostics, re.I):
            raise SystemExit("Native strict compilation failed: " + tag + "\n" + diagnostics)
        code, transcript = execute([str(executable)], 120)
        (output / ("log_" + tag + ".txt")).write_text(transcript, encoding="utf-8")
        prefix = {"model": "HOST", "boundaries": "BOUNDARY", "regions": "REGION",
                  "disabled": "DISABLED"}[suite]
        if code or "|FAIL|" in transcript or prefix + "|DONE|status=0" not in transcript:
            raise SystemExit("Native execution failed: " + tag + "\n" + transcript)
        if suite != "model":
            if suite == "boundaries":
                found = re.search(r"BOUNDARY\|SUMMARY\|checks=(\d+) failures=0", transcript)
            elif suite == "regions":
                found = re.search(r"REGION\|SUMMARY\|checks=(\d+) failures=0", transcript)
                for marker in ("REGION|REORDER|steps=1000 status=PASS",
                               "REGION|EDGES|adjacent=PASS one_byte_overlap=PASS "
                               "crossing_query=PASS",
                               "REGION|PENDING|acquire_reindex=PASS receive_reindex=PASS"):
                    if marker not in transcript:
                        raise SystemExit("Missing independent region coverage: " + marker)
                if (os.name == "nt" and "REGION|PROTECTED|removed_owner_noaccess=PASS"
                        not in transcript):
                    raise SystemExit("Missing compacted index protected owner test")
            else:
                found = re.search(r"DISABLED\|SUMMARY\|pass=(\d+) fail=0", transcript)
            if not found:
                raise SystemExit("Missing independent boundary/disabled summary")
            results.append({"optimization": optimization, "suite": suite, "passed": True,
                            "checks": int(found[1]), "executable_sha256": digest(executable),
                            "production_source_mode": "direct include" if suite in (
                                "boundaries", "regions") else "separate translation unit",
                            "sanitizer": "not used"})
            print(f"{tag} PASS: {found[1]} checks", flush=True)
            continue
        summary = re.search(r"HOST\|SUMMARY\|checks=(\d+) api_calls=(\d+) "
                            r"model_operations=(\d+) failures=0 sanitizer=none", transcript)
        if not summary or int(summary[3]) < 200000:
            raise SystemExit("Native model coverage is incomplete: " + optimization)
        for capacity in (1, 3, 16):
            if f"HOST|MODEL|capacity={capacity} operations=100000 status=PASS" not in transcript:
                raise SystemExit("Missing capacity coverage")
        if "HOST|NEGATIVE|payload_detector=PASS injected=1 detected=1" not in transcript:
            raise SystemExit("Missing negative corruption detector control")
        if os.name == "nt" and "HOST|PROTECTED|old_control_noaccess=PASS" not in transcript:
            raise SystemExit("Missing protected-page lifetime check")
        if ("HOST|DEADLINE|early_wake_late_run=PASS late_arrival=PASS exact_edge=PASS"
                not in transcript):
            raise SystemExit("Missing finite grant deadline checks")
        if "HOST|MAXIMUM|capacity=256 lifecycle=PASS" not in transcript:
            raise SystemExit("Missing maximum legal capacity lifecycle")
        if "HOST|REGISTRY|last_active_hot_api=PASS external_tail_alias=PASS" not in transcript:
            raise SystemExit("Missing last registry entry and cross-channel alias checks")
        if ("HOST|CONTRACT|active_descriptor_overwrite=DEMONSTRATED backups_reclaimed=PASS"
                not in transcript):
            raise SystemExit("Missing application descriptor overwrite contract control")
        results.append({"optimization": optimization, "suite": suite, "passed": True,
                        "checks": int(summary[1]), "api_calls": int(summary[2]),
                        "model_operations": int(summary[3]), "executable_sha256": digest(executable),
                        "sanitizer": "not used", "runtime_scope": "native scripted scheduler"})
        print(f"{tag} PASS: {summary[3]} model operations; {summary[1]} checks", flush=True)
    after = {path.relative_to(ROOT).as_posix(): digest(path) for path in identity_paths}
    if identities != after:
        raise SystemExit("Inputs changed during compilation/execution; rerun frozen inputs")
    report = {"status": "PASS", "compiler": str(args.compiler), "results": results,
              "source_identity_stable": True, "production_source": "RTOS/IPC/rtos_channel.c",
              "limitations": ["No Cortex-M timing or real IRQ/concurrency validation",
                              "No ASan/UBSan instrumentation in these native builds",
                              "Canary tests cannot prevent direct misuse of expired naked pointers"]}
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Evidence: " + str(output), flush=True)


if __name__ == "__main__":
    main()
