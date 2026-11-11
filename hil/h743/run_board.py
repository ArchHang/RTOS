"""Flash isolated H743 HIL images and preserve the previously installed image."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import threading
import time

import serial


FLASH_ADDRESS = "0x08000000"
FLASH_SIZE = 0x200000
SECTOR_SIZE = 0x20000
UID_ADDRESS = 0x1FF1E800
UID_SIZE = 12
DEFAULT_CLI = Path(
    "E:/ST/STM32CubeIDE_1.13.2/STM32CubeIDE/plugins/"
    "com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.500.202603051304/"
    "tools/bin/STM32_Programmer_CLI.exe"
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def programmer(args, operations, log):
    command = [str(args.cli), "-c", "port=SWD", f"sn={args.probe}", "mode=UR",
               "freq=8000", *map(str, operations)]
    result = subprocess.run(command, capture_output=True, timeout=150, check=False)
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    log.write_text(output, encoding="utf-8")
    if (result.returncode or re.search(r"\bError:", output, re.IGNORECASE)
            or not re.search(r"Device ID\s*:\s*0x450\b", output)):
        raise RuntimeError(f"H743 programmer operation failed; see {log}")
    return output


def read_flash(args, target, log):
    programmer(args, ["-u", FLASH_ADDRESS, hex(FLASH_SIZE), target, "-rst"], log)
    contents = target.read_bytes()
    if len(contents) != FLASH_SIZE:
        raise RuntimeError(f"Incomplete H743 flash read: {target}")
    return contents


def read_target_identity(args, log, *, reset=False):
    operations = ["-r32", hex(UID_ADDRESS), str(UID_SIZE)]
    if reset:
        operations.append("-rst")
    output = programmer(args, operations, log)
    revision = re.search(r"^\s*Revision ID\s*:\s*Rev ([VX])\b", output, re.MULTILINE)
    uid_words = {}
    for line in output.splitlines():
        row = re.fullmatch(r"\s*0x([\da-f]{8})\s*:\s*((?:[\da-f]{8}[ \t]*)+)\s*",
                           line, re.IGNORECASE)
        if row:
            address = int(row[1], 16)
            for index, word in enumerate(row[2].split()):
                uid_words[address + 4 * index] = word.lower()
    uid = "".join(uid_words.get(address, "")
                  for address in range(UID_ADDRESS, UID_ADDRESS + UID_SIZE, 4))
    if revision is None or len(uid) != 24 or uid in ("0" * 24, "f" * 24):
        raise RuntimeError(f"Could not identify a supported H743 with a complete UID; see {log}")
    return {"revision": revision[1], "uid": uid}


def require_target_identity(args, expected, log):
    if read_target_identity(args, log) != expected:
        raise RuntimeError(f"Connected H743 does not match the verified backup; see {log}")


def verified_backup(args):
    files = [args.batch / name for name in ("original_flash.bin", "original_flash_second_read.bin")]
    contents = files[0].read_bytes()
    if (len(contents) != FLASH_SIZE or contents != files[1].read_bytes()
            or not (args.batch / "backup_verified.json").is_file()):
        raise RuntimeError("Two identical full original flash reads are required")
    expected = json.loads((args.batch / "backup_verified.json").read_text(encoding="utf-8-sig"))
    if (expected.get("hash", "").lower() != digest(contents)
            or expected.get("probe_serial") != args.probe):
        raise RuntimeError("Original flash reads do not match the verified backup")
    uid = expected.get("uid")
    revision = expected.get("revision")
    if (revision not in ("V", "X") or not isinstance(uid, str)
            or not re.fullmatch(r"[0-9a-f]{24}", uid)):
        raise RuntimeError("Backup has no verified H743 UID. Legacy captures are evidence only.")
    return contents, {"revision": revision, "uid": uid}


def backup(args):
    args.batch.mkdir(parents=True, exist_ok=True)
    first = args.batch / "original_flash.bin"
    second = args.batch / "original_flash_second_read.bin"
    manifest = args.batch / "backup_verified.json"
    if any(path.exists() for path in (first, second, manifest)):
        raise RuntimeError("Select a new batch directory for a fresh board backup")
    options = programmer(args, ["-ob", "displ"], args.batch / "original_options.txt")
    if (not re.search(r"\bSWAP_BANK\s*:\s*0x0\b", options)
            or not re.search(r"\bnWRP0\s*:\s*0x1\b", options)):
        raise RuntimeError("Sector 0 must be writable and the flash banks must be unswapped")
    identity = read_target_identity(args, args.batch / "original_target.txt")
    original = read_flash(args, first, args.batch / "original_flash.log")
    repeated = read_flash(args, second, args.batch / "original_flash_second_read.log")
    repeated_identity = read_target_identity(args, args.batch / "original_target_second.txt",
                                             reset=True)
    if original != repeated or identity != repeated_identity:
        raise RuntimeError("Original H743 flash or target identity reads disagree")
    metadata = {"target": "STM32H743", "device_id": "0x450", "probe_serial": args.probe,
                **identity,
                "address": FLASH_ADDRESS, "size": FLASH_SIZE, "reads": 2,
                "hash": digest(original)}
    manifest.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"BACKED UP {len(original)} bytes by two reads", flush=True)


def restore(args):
    original, identity = verified_backup(args)
    target = args.batch / "restored_readback.bin"
    sector = args.batch / "original_sector0.bin"
    sector.write_bytes(original[:SECTOR_SIZE])
    require_target_identity(args, identity, args.batch / "restore_target.txt")
    programmer(args, ["-d", sector, FLASH_ADDRESS, "-v", "-rst"],
               args.batch / "restore_sector0.txt")
    readback = read_flash(args, target, args.batch / "restore_readback.txt")
    used_full_restore = readback != original
    if used_full_restore:
        require_target_identity(args, identity, args.batch / "restore_target_full.txt")
        programmer(args, ["-d", args.batch / "original_flash.bin", FLASH_ADDRESS, "-v", "-rst"],
                   args.batch / "restore_full.txt")
        readback = read_flash(args, target, args.batch / "restore_readback_full.txt")
    result = {"restored": readback == original, "readback_bytes": len(readback),
              "original_sha256": digest(original), "readback_sha256": digest(readback),
              "used_full_restore": used_full_restore}
    (args.batch / "restoration.json").write_text(json.dumps(result, indent=2) + "\n")
    if not result["restored"]:
        raise RuntimeError("Original flash restoration did not match the complete readback")
    print(f"RESTORED {len(readback)} bytes by readback", flush=True)


def uart_window(snapshot, ready):
    if ready is None:
        return snapshot, None
    startups = list(ready.finditer(snapshot))
    return (snapshot[startups[0].start():] if startups else b""), len(startups)


def uart_failures(verdict):
    return [line for line in verdict.splitlines()
            if line.startswith(("FAULT|", "H743|FAULT|"))
            or re.search(r"\|FAIL(?:\||$)", line)
            or re.search(r"\|DONE\|.*\b(?:status|fail)=-?[1-9][0-9]*(?:[ |]|$)", line)]


def capture(args):
    _, identity = verified_backup(args)
    if not re.fullmatch(r"[A-Za-z0-9_-]+", args.tag):
        raise ValueError("Use an alphanumeric capture tag with optional underscores or dashes")
    if args.entry is not None and not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", args.entry):
        raise ValueError("The expected H743 entry must be a C identifier")
    ready = None if args.entry is None else re.compile(
        rb"^H743\|READY\|entry=" + args.entry.encode("ascii") + rb"\|[^\n]*\n", re.MULTILINE)
    image = args.image.resolve()
    contents = image.read_bytes()
    if image.suffix.lower() != ".bin" or not 16 < len(contents) <= SECTOR_SIZE:
        raise RuntimeError("The test must be a binary wholly within flash sector 0")
    msp, reset = struct.unpack_from("<II", contents)
    if msp != 0x20020000 or not 0x08000000 < (reset & ~1) < 0x08020000 or (reset & 1) == 0:
        raise RuntimeError("The test does not contain the expected DTCM MSP and Thumb reset vector")
    folder = args.batch / args.tag
    folder.mkdir(exist_ok=False)
    require_target_identity(args, identity, folder / "target.txt")
    started = datetime.now(timezone.utc).isoformat()
    observed = bytearray()
    stop = threading.Event()
    capture_errors = []
    terminated = False

    with serial.Serial(args.port, args.baud, timeout=0.05) as port:
        port.reset_input_buffer()

        def collect():
            try:
                with (folder / "uart.bin").open("wb", buffering=0) as stream:
                    while not stop.is_set():
                        chunk = port.read(min(max(1, port.in_waiting), 16384))
                        if chunk:
                            stream.write(chunk)
                            observed.extend(chunk)
            except serial.SerialException as error:
                capture_errors.append(str(error))

        reader = threading.Thread(target=collect, daemon=True)
        reader.start()
        try:
            programmer(args, ["-d", image, FLASH_ADDRESS, "-v", "-rst"], folder / "flash.txt")
            end = re.compile(args.terminator.encode("ascii"), re.MULTILINE)
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline and not capture_errors:
                # A stable snapshot lets the capture thread continue servicing the UART.
                snapshot = bytes(observed).replace(b"\r\n", b"\n")
                verdict, _ = uart_window(snapshot, ready)
                if end.search(verdict):
                    terminated = True
                    break
                if re.search(rb"^H743\|(?:FAULT|FAIL)\|[^\n]+\n", verdict, re.MULTILINE):
                    break
                time.sleep(0.05)
            time.sleep(0.15)
        finally:
            stop.set()
            reader.join(timeout=1.0)
            snapshot = bytes(observed).replace(b"\r\n", b"\n")
            (folder / "uart.txt").write_bytes(snapshot.decode("utf-8", errors="replace").encode("utf-8"))
            verdict, startups = uart_window(snapshot, ready)
            verdict_text = verdict.decode("utf-8", errors="replace")
            if ready is not None:
                (folder / "current_uart.txt").write_bytes(verdict_text.encode("utf-8"))
            failures = uart_failures(verdict_text)
            if ready is not None and startups != 1:
                failures.append(f"Expected one READY line for {args.entry}; observed {startups}")
            result = {"tag": args.tag, "started": started,
                      "finished": datetime.now(timezone.utc).isoformat(), "image": str(image),
                      "sha256": digest(contents), "bytes": len(contents), "port": args.port,
                      "baud": args.baud, "terminator": args.terminator,
                      "expected_entry": args.entry, "matching_ready_lines": startups,
                      "terminated": terminated, "capture_errors": capture_errors,
                      "failures": failures}
            (folder / "run.json").write_text(json.dumps(result, indent=2) + "\n")
    if not terminated or failures or capture_errors:
        raise RuntimeError(f"H743 capture did not pass; see {folder / 'uart.txt'}")
    print(f"PASS {args.tag}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=DEFAULT_CLI)
    parser.add_argument("--batch", type=Path, required=True)
    parser.add_argument("--probe", required=True)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("backup")
    run = sub.add_parser("capture")
    run.add_argument("--image", type=Path, required=True)
    run.add_argument("--entry", help="Scope the verdict to the READY line of this H743 fixture")
    run.add_argument("--tag", required=True)
    run.add_argument("--port", default="COM4")
    run.add_argument("--baud", type=int, default=921600)
    run.add_argument("--timeout", type=float, default=25.0)
    run.add_argument("--terminator", required=True)
    sub.add_parser("restore")
    args = parser.parse_args()
    args.batch = args.batch.resolve()
    if args.action == "backup":
        backup(args)
    elif args.action == "capture":
        capture(args)
    else:
        restore(args)


if __name__ == "__main__":
    main()
