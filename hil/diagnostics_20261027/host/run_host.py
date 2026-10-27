"""Compile the actual recorder with native mocks; not an ARM/IRQ emulation.

Strict O0/O3 builds exercise minimum/default/maximum static capacities. The
white-box mode injects terminal fault interruptions and non-wrapping limits.
No sanitizer coverage is claimed by this runner.
"""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
COMPILER = Path(r"C:\Users\23350\.conda\envs\mingw\Scripts\gcc.exe")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, output, timeout=120):
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, encoding="utf-8", errors="replace",
                            timeout=timeout)
    output.write_text(result.stdout, encoding="utf-8")
    return result.returncode, result.stdout


def snapshot():
    files = list(HERE.glob("*.c")) + list(HERE.glob("*.h")) + [Path(__file__).resolve(),
        ROOT / "RTOS/Services/rtos_diag.c"]
    files += list((ROOT / "RTOS/Core/Inc").glob("*.h"))
    files += list((ROOT / "RTOS/Core/Private").glob("*.h"))
    return {path.relative_to(ROOT).as_posix(): digest(path) for path in sorted(files)}


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--compiler", type=Path, default=COMPILER)
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    out = args.out_dir or HERE / "evidence" / stamp
    out.mkdir(parents=True, exist_ok=False)
    build = HERE / ".build" / stamp
    build.mkdir(parents=True)
    before = snapshot()
    (out / "sources.json").write_text(json.dumps(before, indent=2) + "\n", encoding="utf-8")
    code, _ = run([str(args.compiler), "--version"], out / "compiler.txt")
    if code:
        raise SystemExit("Compiler version query failed")
    matrix = [(optimization, capacity, 1) for optimization in ("O0", "O3")
              for capacity in (8, 64, 1024)]
    matrix += [(optimization, 64, 0) for optimization in ("O0", "O3")]
    results = []
    for optimization, capacity, enabled in matrix:
        tag = f"{optimization}_c{capacity}_on{enabled}"
        executable = build / (tag + (".exe" if os.name == "nt" else ""))
        source = HERE / ("diagnostic_model.c" if enabled else "diagnostic_disabled.c")
        command = [str(args.compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-" + optimization, f"-DRTOS_CONFIG_USE_DIAGNOSTICS={enabled}",
                   f"-DRTOS_CONFIG_DIAGNOSTIC_CAPACITY={capacity}",
                   "-DRTOS_CONFIG_ASSERT_ENABLED=0", "-DRTOS_HIL_DIAG_WRITE_STAGE=1",
                   "-include", str(HERE / "host_port.h"),
                   "-I" + str(ROOT / "RTOS/Core/Inc"),
                   "-I" + str(ROOT / "RTOS/Core/Private"),
                   str(source), "-o", str(executable)]
        (out / f"command_{tag}.json").write_text(json.dumps(command, indent=2) + "\n",
                                                encoding="utf-8")
        code, diagnostics = run(command, out / f"compile_{tag}.txt")
        if code or re.search(r"\b(?:warning|error):", diagnostics):
            raise SystemExit("Strict native compilation failed: " + tag + "\n" + diagnostics)
        code, text = run([str(executable)], out / f"log_{tag}.txt")
        prefix = "HOST" if enabled else "DISABLED"
        summary = re.search(prefix + r"\|SUMMARY\|checks=(\d+) failures=0 sanitizer=none", text)
        if code or not summary or "|FAIL|" in text or prefix + "|DONE|status=0" not in text:
            raise SystemExit("Native tests failed: " + tag + "\n" + text)
        if enabled:
            groups = ("parameters=PASS", "explanation=PASS operations=9 phases=7",
                      "irq_contract=PASS priorities=4", "terminal_publication=PASS stages=3 occupancy=2",
                      "counter_exhaustion=PASS")
            if any("HOST|GROUP|" + group not in text for group in groups):
                raise SystemExit("Incomplete independent boundary groups: " + tag)
            if not re.search(r"HOST\|MODEL\|rounds=160 operations=\d+ capacity="
                             + str(capacity) + " status=PASS", text):
                raise SystemExit("Missing independent bounded FIFO model: " + tag)
        results.append({"tag": tag, "optimization": optimization, "capacity": capacity,
                        "enabled": enabled, "checks": int(summary[1]),
                        "executable_sha256": digest(executable), "passed": True})
        print(f"{tag}: PASS ({summary[1]} checks)", flush=True)
    if before != snapshot():
        raise SystemExit("Source inputs changed during host run; rerun frozen inputs")
    report = {"status": "PASS", "source_identity_stable": True, "results": results,
              "scope": "native recorder + scripted single-thread mock exclusion",
              "sanitizer": "none", "limits": ["No Cortex-M timing or real interrupt concurrency",
                  "No ASan/UBSan instrumentation", "No proof of all possible RTOS bugs"]}
    (out / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Evidence: " + str(out))


if __name__ == "__main__":
    main()
