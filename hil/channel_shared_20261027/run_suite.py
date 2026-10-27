"""Build, flash and capture one independently verifiable F446 test image.

Uses the project's existing serial-before-flash runner; never shares COM3 with
another process. Evidence directories are append-only. The offline oracle is
verify_results.py, and intentional corruptions have a separate acceptance rule.
"""
import argparse
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(variant):
    files = set()
    for directory in ("RTOS", "Core", "Drivers", "AI"):
        for path in (ROOT / directory).rglob("*"):
            if path.is_file() and path.suffix.lower() in {".c", ".h", ".s"}:
                files.add(path)
    for path in (ROOT / "hil/templates").iterdir():
        if path.is_file() and path.name != "subdir.mk.last_variant":
            files.add(path)
    files.update(ROOT.glob("*.ld"))
    files.update((ROOT / "hil/fw/Inc").glob("*.h"))
    # Conservative closure: all maintained fixture C/ASM, excluding generated
    # fwsrc. A changed fixture during any capture invalidates that capture.
    files.update((ROOT / "hil/fw").rglob("*.c"))
    files.update((ROOT / "hil/fw").rglob("*.s"))
    for name in ("build_fw.ps1", "build_layout.ps1", "run_hil.ps1"):
        files.add(ROOT / "hil" / name)
    files.add(Path(__file__).resolve())
    return {p.relative_to(ROOT).as_posix(): {"sha256": sha(p), "bytes": p.stat().st_size}
            for p in sorted(files)}


def run(command, destination):
    environment = os.environ.copy()
    # The bundled Python runtime can inherit PowerShell 7 module paths. The
    # project's scripts run in Windows PowerShell 5.1; use its own system modules.
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
    parser.add_argument("--mask", choices=("basepri", "primask"), default="basepri")
    parser.add_argument("--assertions", choices=(0, 1), type=int, default=1)
    parser.add_argument("--optimization", choices=("O0", "O2", "O3"), default="O2")
    parser.add_argument("--batch", default="matrix")
    parser.add_argument("--tag")
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--seconds", type=int, default=90)
    parser.add_argument("--terminator", default=r"CH\|DONE\|status=")
    parser.add_argument("--reply-on", default="")
    parser.add_argument("--enable", action="store_true")
    parser.add_argument("--replay", type=Path,
                        help="Replay a previously verified ELF; do not rebuild current sources")
    args = parser.parse_args()
    tag = args.tag or f"{args.variant}_{args.mask}_a{args.assertions}_{args.optimization}"
    out = HERE / "evidence" / args.batch / tag
    out.mkdir(parents=True, exist_ok=False)
    meta = vars(args) | {"tag": tag, "started": datetime.now(timezone.utc).isoformat(),
                         "passed": False, "stage": "snapshot"}
    meta["replay"] = str(args.replay) if args.replay else None
    identity = out / "identity.json"

    def save():
        identity.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")

    save()
    try:
        base = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]
        if args.variant == "channel_dma":
            raise RuntimeError("DMA is outside this shared-memory-only batch")
        if args.replay:
            from verify_results import verify_capture
            old_directory = args.replay.resolve()
            old = json.loads((old_directory / "identity.json").read_text())
            require_replay = (args.variant in ("channel_bench", "channel_profile")
                and old["variant"] == args.variant and old["mask"] == args.mask
                and old["assertions"] == args.assertions
                and old["optimization"] == args.optimization and old["passed"])
            if not require_replay:
                raise RuntimeError("Replay configuration mismatch")
            verify_capture(old_directory)
            old_sources = json.loads((old_directory / "sources.json").read_text())
            baseline = json.loads((HERE / "baseline/manifest.json").read_text())
            for item in baseline["sources"]:
                if sha(HERE / item["snapshot"]) != item["sha256"] or \
                        old_sources[item["source"]]["sha256"] != item["sha256"]:
                    raise RuntimeError("Replay production baseline mismatch")
            for name, expected in old["artifacts"].items():
                if sha(old_directory / name) != expected:
                    raise RuntimeError("Original replay evidence changed: " + name)
            for name in ("sources.json", "build.txt", "build_detail.txt"):
                shutil.copyfile(old_directory / name, out / name)
            shutil.copyfile(old_directory / "identity.json", out / "original_identity.json")
            elf = old_directory / f'firmware_{old["tag"]}.elf'
            meta["built_elf_sha256"] = old["built_elf_sha256"]
            meta["build_command"] = old["build_command"]
            meta["build_provenance"] = "replayed_verified_image"
            meta["original_identity_sha256"] = sha(old_directory / "identity.json")
            meta["replayed_from"] = str(old_directory)
            before = sha(elf)
        else:
            before = snapshot(args.variant)
            (out / "sources.json").write_text(json.dumps(before, indent=2) + "\n", encoding="utf-8")
            command = base + [str(ROOT / "hil/build_fw.ps1"), "-Variant", args.variant,
                          "-CriticalMask", args.mask, "-Optimization", args.optimization,
                          "-Clean"]
            if args.assertions == 0:
                command.append("-DisableAssertions")
            if args.enable:
                command.append("-EnableChannels")
            meta["build_command"] = command
            meta["build_provenance"] = "fresh_build"
            meta["stage"] = "build"
            save()
            code = run(command, out / "build.txt")
            detail = ROOT / "hil/logs" / f"build_{args.variant}_1.txt"
            if detail.exists():
                (out / "build_detail.txt").write_bytes(detail.read_bytes())
            if code:
                raise RuntimeError(f"build exit {code}")
            elf = ROOT / "Debug/f446_rtos.elf"
            meta["built_elf_sha256"] = sha(elf)
        meta["stage"] = "capture"
        command = base + [str(ROOT / "hil/run_hil.ps1"), "-Elf",
                          str(elf), "-Tag", tag,
                          "-Seconds", str(args.seconds), "-Terminator", args.terminator,
                          "-Port", args.port, "-OutDir", str(out), "-LiveLog"]
        if args.variant == "channel_stress_negative":
            command.append("-AllowFailures")
        if args.reply_on:
            command += ["-ReplyOn", args.reply_on]
        meta["capture_command"] = command
        save()
        if run(command, out / "capture.txt"):
            raise RuntimeError("capture exit nonzero")
        if args.replay and before != sha(elf):
            raise RuntimeError("Replayed ELF changed during capture")
        if not args.replay and before != snapshot(args.variant):
            raise RuntimeError("compiled inputs changed during build/capture")
        meta["stage"] = "verify"
        save()
        from verify_results import verify_capture
        meta["verification"] = verify_capture(out)
        meta["oracle_sha256"] = sha(HERE / "verify_results.py")
        meta["artifacts"] = {p.name: sha(p) for p in sorted(out.iterdir())
                             if p.is_file() and p != identity}
        meta["passed"] = True
        meta["stage"] = "complete"
    except Exception as error:
        meta["error"] = str(error)
        raise
    finally:
        meta["finished"] = datetime.now(timezone.utc).isoformat()
        save()
    print(f"{tag}: PASS", flush=True)


if __name__ == "__main__":
    main()
