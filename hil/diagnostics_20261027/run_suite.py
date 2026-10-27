"""Build, flash and capture one diagnostic firmware with auditable provenance.

Only the caller runs this tool. It uses the maintained serial-before-flash HIL
runner; it never opens another serial connection or rewrites application main.
Every evidence directory is append-only, including failed attempts.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot():
    """Conservative build closure, excluding generated copies and documentation."""
    files = set()
    for directory in ("RTOS", "Core", "Drivers", "AI", "hil/fw"):
        files.update(path for path in (ROOT / directory).rglob("*")
                     if path.is_file() and path.suffix.lower() in {".c", ".h", ".s"})
    files.update(path for path in (ROOT / "hil/templates").iterdir()
                 if path.is_file() and path.name != "subdir.mk.last_variant")
    files.update(ROOT.glob("*.ld"))
    files.update(ROOT / "hil" / name for name in (
        "build_fw.ps1", "build_layout.ps1", "run_hil.ps1"))
    files.update((Path(__file__).resolve(), HERE / "verify_results.py", HERE / "contracts.json"))
    return {path.relative_to(ROOT).as_posix(): {
        "sha256": sha(path), "bytes": path.stat().st_size} for path in sorted(files)}


def run(command, destination):
    environment = os.environ.copy()
    windows_root = environment.get("SystemRoot", environment.get("SYSTEMROOT", r"C:\Windows"))
    environment["PSModulePath"] = str(Path(windows_root) /
        "System32/WindowsPowerShell/v1.0/Modules")
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, encoding="utf-8", errors="replace",
                            env=environment)
    destination.write_text(result.stdout, encoding="utf-8")
    return result.returncode


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("variant")
    parser.add_argument("--diag", choices=("on", "off"), default="on")
    parser.add_argument("--capacity", type=int, default=64)
    parser.add_argument("--channels", action="store_true")
    parser.add_argument("--mask", choices=("basepri", "primask"), default="basepri")
    parser.add_argument("--assertions", choices=(0, 1), type=int, default=1)
    parser.add_argument("--optimization", choices=("O0", "O2", "O3"), default="O2")
    parser.add_argument("--batch", default="matrix")
    parser.add_argument("--tag")
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--seconds", type=int, default=90)
    parser.add_argument("--terminator", default=r"DG\|DONE\|status=")
    parser.add_argument("--reply-on", default="")
    parser.add_argument("--fault", action="store_true",
                        help="Expect the explicitly armed fatal-fault fixture only")
    args = parser.parse_args()
    if args.fault != (args.variant == "diagnostic_fault"):
        parser.error("--fault is required exactly for the diagnostic_fault variant")
    if args.fault and args.diag != "on":
        parser.error("fault history validation requires diagnostics enabled")
    if args.fault and args.terminator == r"DG\|DONE\|status=":
        args.terminator = "Locate source:"
    if not 8 <= args.capacity <= 1024 or args.capacity & (args.capacity - 1):
        parser.error("capacity must be a power of two in 8..1024")
    if args.diag == "off" and args.capacity != 64:
        parser.error("OFF uses the default capacity value 64 in runtime metadata")
    tag = args.tag or (f"{args.variant}_{args.diag}_c{args.capacity}_{args.mask}"
                       f"_a{args.assertions}_{args.optimization}")
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", tag):
        parser.error("tag must contain only letters, digits, underscores and hyphens")
    batch = Path(args.batch)
    if batch.is_absolute() or ".." in batch.parts:
        parser.error("batch must be relative and remain inside evidence")
    out = HERE / "evidence" / batch / tag
    out.mkdir(parents=True, exist_ok=False)
    meta = vars(args) | {"tag": tag, "diagnostics": int(args.diag == "on"),
                         "started": datetime.now(timezone.utc).isoformat(),
                         "passed": False, "stage": "snapshot"}
    identity = out / "identity.json"

    def save():
        identity.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")

    save()
    try:
        before = snapshot()
        (out / "sources.json").write_text(json.dumps(before, indent=2) + "\n",
                                          encoding="utf-8")
        base = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]
        command = base + [str(ROOT / "hil/build_fw.ps1"), "-Variant", args.variant,
                          "-CriticalMask", args.mask, "-Optimization", args.optimization,
                          "-Clean"]
        if args.diag == "on":
            command += ["-EnableDiagnostics", "-DiagnosticCapacity", str(args.capacity)]
        if args.channels:
            command.append("-EnableChannels")
        if args.assertions == 0:
            command.append("-DisableAssertions")
        meta.update(build_command=command, build_provenance="fresh_build", stage="build")
        save()
        result = run(command, out / "build.txt")
        detail = ROOT / "hil/logs" / f"build_{args.variant}_1.txt"
        if detail.exists():
            (out / "build_detail.txt").write_bytes(detail.read_bytes())
        if result:
            raise RuntimeError(f"build exit {result}")
        elf = ROOT / "Debug/f446_rtos.elf"
        meta.update(built_elf_sha256=sha(elf), stage="capture")
        command = base + [str(ROOT / "hil/run_hil.ps1"), "-Elf", str(elf), "-Tag", tag,
                          "-Seconds", str(args.seconds), "-Terminator", args.terminator,
                          "-Port", args.port, "-OutDir", str(out), "-LiveLog"]
        if args.fault:
            command.append("-AllowFailures")
        if args.reply_on:
            command += ["-ReplyOn", args.reply_on]
        meta["capture_command"] = command
        save()
        if run(command, out / "capture.txt"):
            raise RuntimeError("capture exit nonzero")
        if before != snapshot():
            raise RuntimeError("build inputs changed during build/capture")
        meta["stage"] = "verify"
        save()
        from verify_results import verify_capture
        meta["verification"] = verify_capture(out)
        meta["oracle_sha256"] = sha(HERE / "verify_results.py")
        meta["artifacts"] = {path.name: sha(path) for path in sorted(out.iterdir())
                             if path.is_file() and path != identity}
        meta.update(passed=True, stage="complete")
    except Exception as error:
        meta["error"] = str(error)
        raise
    finally:
        meta["finished"] = datetime.now(timezone.utc).isoformat()
        save()
    print(f"{tag}: PASS", flush=True)


if __name__ == "__main__":
    main()
