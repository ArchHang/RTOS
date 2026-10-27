"""Verify actual post-UDF SRAM plus the corrected stack-use arithmetic.

This accepts a separate read-only SWD snapshot only when it identifies the same
already verified, flashed ELF and capture. It does not accept the UART banner as
proof of recorder state. No hardware actions are performed by this verifier.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

from verify_results import require, rows, verify_capture

HERE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return path.read_text(encoding="utf-8-sig", errors="strict")


def verify(directory):
    identity = json.loads(read(directory / "identity.json"))
    capture = Path(identity["capture"])
    require(identity["attach_only"] and identity["debugger_exit"] == 0, "read-only attach")
    require("-g" in identity["server_arguments"], "server attached without target reset")
    server_arguments = identity["server_arguments"]
    require("-i" in server_arguments and server_arguments[server_arguments.index("-i") + 1]
            == "066DFF555775514867092907", "same physical ST-LINK selected")
    require("Target connection mode: Attach" in read(directory / "server.txt"),
            "server confirmed attach mode")
    require(digest(capture / "identity.json") == identity["capture_identity_sha256"],
            "same immutable UART capture identity")
    current = json.loads(read(capture / "identity.json"))
    require(current["passed"] and current["stage"] == "complete", "verified UART capture")
    require(current["variant"] == "diagnostic_fault" and current["diagnostics"] == 1,
            "explicit fatal test, feature enabled")
    require(current["capacity"] == 64, "64-slot debugger script matches firmware")
    for name, expected in current["artifacts"].items():
        require(digest(capture / name) == expected, "unchanged captured artifact: " + name)
    verify_capture(capture)
    firmware = capture / ("firmware_" + current["tag"] + ".elf")
    arguments = identity["debugger_arguments"]
    require(Path(arguments[-1]).resolve() == firmware.resolve() and "-x" in arguments
            and Path(arguments[arguments.index("-x") + 1]).resolve()
            == (HERE / "fault_snapshot.gdb").resolve(), "actual debugger ELF and script arguments")
    require(digest(firmware) == identity["firmware_sha256"] == current["built_elf_sha256"],
            "same flashed ELF used for debugger symbols")
    require(digest(HERE / "fault_snapshot.gdb") == identity["script_sha256"], "debugger script")
    text = read(directory / "snapshot.txt")
    require(not re.search(r"No such file|Error|Cannot|error:", text), "no debugger failure")
    state = rows(text, "DGDBG|STATE|")
    fault = rows(text, "DGDBG|FAULT|")
    require(len(state) == len(fault) == 1, "one actual frozen state and fault")
    state, fault = state[0], fault[0]
    require(tuple(int(state[key]) for key in ("frozen", "trigger", "fault_valid", "writing"))
            == (1, 2, 1, 0), "terminal fault froze committed history")
    require(int(fault["code"], 0) == int(fault["cfsr"], 0) == 0x10000
            and fault["frame_valid"] == "1", "actual UNDEFINSTR context")
    require(re.search(r"0x0*" + fault["frame_pc"][2:] + r".*:\s+udf\s+#0", text),
            "fault PC really contains UDF on target")
    raw_opcode = re.findall(r"^(0x[0-9a-f]+)[^\r\n]*:\s+0xde00\s*$", text, re.MULTILINE)
    require(len(raw_opcode) == 1 and int(raw_opcode[0], 0) == int(fault["frame_pc"], 0),
            "raw target UDF opcode read at the actual fault PC")
    slots = rows(text, "DGDBG|SLOT|")
    slots.sort(key=lambda row: int(row["seq"]))
    first, next_sequence = int(state["first"]), int(state["next"])
    require(0 < first < next_sequence and next_sequence - first <= 64,
            "nonempty bounded logical range")
    require([int(row["seq"]) for row in slots] == list(range(first, next_sequence)),
            "all committed records in logical session retained")
    require(all(row["commit"] == row["seq"] and
                int(row["index"]) == (int(row["seq"]) - 1) % 64 for row in slots),
            "every slot commit and physical index agree")
    markers = [row for row in slots if row["event"] == "9"]
    require([(int(row["value"], 0), int(row["detail"], 0)) for row in markers]
            == [(0xA55A0001, 0x12345678), (0xA55A0002, 0x87654321)],
            "both original marker payloads survived actual fault")
    task = markers[0]["task"]
    timeline = [int(row["event"]) for row in slots if row["task"] == task]
    require(timeline == [9, 1, 2, 3, 4, 9], "actual wait/wake/run/return before fault")
    uart = read(capture / ("log_" + current["tag"] + ".txt"))
    armed = rows(uart, "DG|FAULT_ARMED|")
    task_in_report = re.search(r"^Task\s+:.*id (\d+)\)", uart, re.MULTILINE)
    require(len(armed) == 1 and task == markers[1]["task"] == armed[0]["task"]
            == task_in_report[1], "markers identify the actual faulting task")
    uart_pc = re.search(r"^PC\s+= (0x[0-9A-F]+)", uart, re.MULTILINE)
    require(int(uart_pc[1], 0) == int(fault["frame_pc"], 0), "UART and SRAM fault PC agree")
    task_rows = [row for row in slots if row["task"] == task]
    wait, wake, run, returned = task_rows[1:5]
    require(wait["op"] == returned["op"] == "1" and int(wait["value"], 0) == 2
            and wait["result"] == wake["result"] == returned["result"] == "0",
            "actual successful two-tick delay episode")
    require(int(wake["tick"]) - int(wait["tick"]) >= 2
            and int(wait["tick"]) <= int(wake["tick"]) <= int(run["tick"])
            <= int(returned["tick"]), "delay elapsed before wake/run/return")
    sp = int(re.search(r"Frame SP\s*= (0x[0-9A-F]+)", uart)[1], 0)
    stack = re.search(r"Task stack : base (0x[0-9A-F]+), size (0x[0-9A-F]+)", uart)
    base, size = int(stack[1], 0), int(stack[2], 0)
    used = int(re.search(r"Stack usage: (0x[0-9A-F]+) bytes in use", uart)[1], 0)
    require(base <= sp <= base + size and used == size - (sp - base),
            "downward stack usage is capacity minus remaining space")
    return {"status": "PASS", "committed_records": len(slots), "task_id": int(task),
            "markers": 2, "cfsr": "0x10000", "stack_bytes": size,
            "remaining_bytes": sp - base, "used_bytes": used,
            "scope": "Physical F446 SWD SRAM and target opcode, after verified UART fault capture"}


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    result = verify(args.directory)
    (args.directory / "verification.json").write_text(json.dumps(result, indent=2) + "\n",
                                                       encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
