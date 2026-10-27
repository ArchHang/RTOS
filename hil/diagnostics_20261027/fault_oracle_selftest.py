"""Read-only positive and ten negative controls for actual SWD evidence parsing.

Mutations operate on temporary copies of debugger text/identity only. The real
UART capture and its ELF are referenced in place and are never copied or edited.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import tempfile

from verify_fault_snapshot import verify

HERE = Path(__file__).resolve().parent
DEFAULT_SNAPSHOT = HERE / "fault_debug/20261027_165907_9971"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_one(text, old, new):
    if old not in text:
        raise AssertionError("Negative-control injection did not find: " + old)
    return text.replace(old, new, 1)


def replace_pattern(text, pattern, replacement):
    changed, count = re.subn(pattern, replacement, text, count=1, flags=re.MULTILINE)
    if count != 1:
        raise AssertionError("Negative-control injection did not match: " + pattern)
    return changed


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--snapshot", type=Path, default=DEFAULT_SNAPSHOT)
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    original = args.snapshot.resolve()
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    out = args.out_dir.resolve() if args.out_dir else HERE / "fault_oracle_evidence" / stamp
    if HERE not in out.parents or out.exists():
        raise SystemExit("Output must be new and inside this batch")
    out.mkdir(parents=True)
    filenames = ("identity.json", "snapshot.txt", "server.txt", "server_error.txt")
    copied_inputs = {name: (original / name).read_bytes() for name in filenames}
    identity = json.loads(copied_inputs["identity.json"].decode("utf-8-sig"))
    capture = Path(identity["capture"])
    capture_identity = json.loads((capture / "identity.json").read_text(encoding="utf-8-sig"))
    protected = [original / name for name in filenames]
    protected += [capture / "identity.json"] + [capture / name for name in capture_identity["artifacts"]]
    source_paths = [Path(__file__).resolve(), HERE / "verify_fault_snapshot.py",
                    HERE / "verify_results.py", HERE / "fault_snapshot.gdb", HERE / "contracts.json"]
    protected += source_paths
    before = {str(path): digest(path) for path in protected}
    positive = verify(original)
    if positive["status"] != "PASS":
        raise SystemExit("Actual unmodified SWD evidence was rejected")
    text = copied_inputs["snapshot.txt"].decode("utf-8-sig")
    server = copied_inputs["server.txt"].decode("utf-8-sig")
    first_slot = re.search(r"^DGDBG\|SLOT\|index=(\d+) commit=(\d+) seq=(\d+)", text, re.MULTILINE)
    if not first_slot:
        raise SystemExit("Unmodified snapshot has no slot for injection")
    marker_task = re.search(r"^DGDBG\|SLOT\|[^\r\n]* task=(\d+) event=9", text, re.MULTILINE)[1]
    cases = []

    def add_text(name, content, description, expected):
        cases.append({"name": name, "file": "snapshot.txt", "content": content.encode("utf-8"),
                      "description": description, "expected_rejection": expected})

    add_text("frozen_zero", replace_one(text, "DGDBG|STATE|frozen=1", "DGDBG|STATE|frozen=0"),
             "Clear only frozen flag in copied snapshot", "terminal fault froze committed history")
    add_text("commit_mismatch", replace_one(text, first_slot[0],
        f"DGDBG|SLOT|index={first_slot[1]} commit={int(first_slot[2]) + 1} seq={first_slot[3]}"),
        "Change one commit without changing sequence/index", "every slot commit and physical index agree")
    add_text("missing_marker", replace_pattern(text, r"^(DGDBG\|SLOT\|[^\r\n]*?)event=9",
        r"\g<1>event=8"), "Change first marker to another event while retaining commit/sequence",
        "both original marker payloads survived actual fault")
    add_text("raw_opcode_wrong_address", replace_pattern(text,
        r"^(0x[0-9a-f]+)([^\r\n]*:\s+0xde00[ \t]*\r?$)",
        lambda match: f"0x{int(match[1], 16) + 2:x}" + match[2]),
        "Keep UDF opcode but move the raw target read address by two bytes",
        "raw target UDF opcode read at the actual fault PC")
    add_text("wrong_delay_budget", replace_one(text, "event=1 op=1 value=0x2",
        "event=1 op=1 value=0x3"), "Change retained delay budget from two to three ticks",
        "actual successful two-tick delay episode")
    add_text("wrong_task", text.replace(" task=" + marker_task + " ",
        " task=" + str(int(marker_task) + 100) + " "),
        "Retain the whole six-event timeline but attribute it to a different task",
        "markers identify the actual faulting task")
    add_text("huge_sequence_range", replace_pattern(text, r"(DGDBG\|STATE\|[^\r\n]* next=)\d+",
        r"\g<1>4294967295"), "Increase next sequence before any range allocation",
        "nonempty bounded logical range")
    wrong_elf = json.loads(copied_inputs["identity.json"].decode("utf-8-sig"))
    wrong_elf["debugger_arguments"][-1] = str(original / "wrong_symbols.elf")
    cases.append({"name": "wrong_elf_argument", "file": "identity.json",
        "content": (json.dumps(wrong_elf, indent=2) + "\n").encode("utf-8"),
        "description": "Change actual debugger symbol argument while keeping firmware hash field",
        "expected_rejection": "actual debugger ELF and script arguments"})
    wrong_probe = json.loads(copied_inputs["identity.json"].decode("utf-8-sig"))
    arguments = wrong_probe["server_arguments"]
    arguments[arguments.index("-i") + 1] = "000000000000000000000000"
    cases.append({"name": "wrong_stlink_serial", "file": "identity.json",
        "content": (json.dumps(wrong_probe, indent=2) + "\n").encode("utf-8"),
        "description": "Select a different ST-LINK in copied server arguments",
        "expected_rejection": "same physical ST-LINK selected"})
    cases.append({"name": "missing_attach_confirmation", "file": "server.txt",
        "content": replace_one(server, "Target connection mode: Attach",
            "Target connection mode: Under Reset").encode("utf-8"),
        "description": "Remove Attach confirmation from the copied server log",
        "expected_rejection": "server confirmed attach mode"})
    scratch = HERE / ".build"
    scratch.mkdir(exist_ok=True)
    results = []
    with tempfile.TemporaryDirectory(prefix="fault_oracle_", dir=scratch) as temporary:
        temporary_root = Path(temporary).resolve()
        if scratch.resolve() not in temporary_root.parents:
            raise RuntimeError("Temporary copy directory escaped its dedicated root")
        for case in cases:
            directory = temporary_root / case["name"]
            directory.mkdir()
            for name, content in copied_inputs.items():
                (directory / name).write_bytes(content)
            original_hash = digest(directory / case["file"])
            (directory / case["file"]).write_bytes(case["content"])
            changed_hash = digest(directory / case["file"])
            if original_hash == changed_hash:
                raise AssertionError("Mutation did not change input: " + case["name"])
            rejection = ""
            try:
                verify(directory)
            except AssertionError as error:
                rejection = str(error)
            if rejection != case["expected_rejection"]:
                raise AssertionError("Wrong acceptance/rejection for " + case["name"] + ": " + rejection)
            results.append({"name": case["name"], "description": case["description"],
                "mutated_file": case["file"], "original_sha256": original_hash,
                "mutated_sha256": changed_hash, "rejected": True, "rejection": rejection})
    after = {str(path): digest(path) for path in protected}
    if before != after:
        raise AssertionError("Original capture, debugger input or verifier source changed")
    if temporary_root.exists():
        raise AssertionError("Temporary mutation copies were not removed")
    report = {"status": "PASS", "created_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "Parser controls using temporary mutations of actual SWD evidence; no hardware actions",
        "positive_control": positive, "negative_controls": len(results), "results": results,
        "original_inputs_unchanged": True, "temporary_copies_removed": True,
        "original_snapshot_directory": str(original), "capture_referenced_in_place": str(capture),
        "protected_input_sha256": before}
    (out / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "PASS", "negative_controls": len(results), "output": str(out)}))


if __name__ == "__main__":
    main()
