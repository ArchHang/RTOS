"""Verify only bytes that are actually candidates for Git publication.

--git-index reads staged blobs with git show :path. Workspace mode is restricted
to tracked/untracked non-ignored candidates, so an ignored local ELF cannot make
this publication check appear to verify a binary unavailable in a fresh clone.
Historical source manifests retain their own generation and are not compared
with today's source. Only the 35 final diagnostic captures compare current
compiled inputs, distinguishing exact bytes from LF/CRLF equivalence.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SPECS = {
    "channel_20261027": {"groups": ("matrix", "regression", "default_off"),
                         "captures": 48, "owned_c_units": 17},
    "channel_shared_20261027": {"groups": ("optimized", "regression", "default_off", "baseline_replay"),
                                "captures": 46, "owned_c_units": 17},
    "diagnostics_20261027": {"groups": ("final",), "captures": 35, "owned_c_units": 18},
}
BINARY_SUFFIXES = {".elf", ".bin", ".hex", ".o", ".obj", ".a", ".lib", ".exe", ".dll", ".so"}


def sha(data):
    return hashlib.sha256(data).hexdigest()


class PublishedReader:
    def __init__(self, index):
        self.index = index
        command = ["git", "ls-files", "--cached"]
        if not index:
            command += ["--others", "--exclude-standard"]
        command.append("-z")
        raw = subprocess.check_output(command, cwd=ROOT)
        self.candidates = {item.decode("utf-8") for item in raw.split(b"\0") if item}
        self.cache = {}

    def read(self, path):
        path = str(path)
        if path not in self.candidates:
            raise FileNotFoundError("Not a published candidate: " + path)
        if path not in self.cache:
            if self.index:
                self.cache[path] = subprocess.check_output(["git", "show", ":" + path], cwd=ROOT)
            else:
                self.cache[path] = (ROOT / path).read_bytes()
        return self.cache[path]

    def json(self, path):
        return json.loads(self.read(path).decode("utf-8-sig"))


def compiled_input(path):
    suffix = PurePosixPath(path).suffix.lower()
    return (suffix in {".c", ".h", ".s", ".ld"}
            or path.startswith("hil/templates/")
            or path in {"hil/build_fw.ps1", "hil/build_layout.ps1", "hil/run_hil.ps1"})


def current_source_match(reader, path, expected):
    data = reader.read(path)
    actual = sha(data)
    if actual == expected:
        status = "RAW_BYTES_IDENTICAL"
    else:
        lf = data.replace(b"\r\n", b"\n")
        variants = {sha(lf), sha(lf.replace(b"\n", b"\r\n"))}
        status = "LF_CRLF_EQUIVALENT_NOT_RAW_IDENTICAL" if expected in variants else "MISMATCH"
    return {"path": path, "captured_sha256": expected, "published_sha256": actual, "status": status}


def artifact_path(identity_path, name):
    name_path = PurePosixPath(name)
    if name_path.is_absolute() or len(name_path.parts) != 1 or "\\" in name or name in {".", ".."}:
        raise ValueError("Artifact must be a direct capture file: " + name)
    return str(PurePosixPath(identity_path).parent / name_path)


def verify_capture_text(reader, identity_path, batch, spec, source_cache):
    identity = reader.json(identity_path)
    errors = []
    if not identity.get("passed") or identity.get("stage") != "complete":
        errors.append("Capture identity is not an accepted completed capture")
    text_count = 0
    skipped = []
    text_artifacts = []
    for name, expected in identity.get("artifacts", {}).items():
        path = artifact_path(identity_path, name)
        if PurePosixPath(name).suffix.lower() in BINARY_SUFFIXES:
            skipped.append({"path": path, "recorded_sha256": expected,
                            "status": "NOT_VERIFIED_BINARY_EXCLUDED_FROM_PUBLICATION"})
            if path in reader.candidates:
                errors.append("Build binary unexpectedly included in publication: " + path)
            continue
        try:
            data = reader.read(path)
            actual = sha(data)
            text_artifacts.append({"path": path, "captured_sha256": expected,
                                   "published_sha256": actual, "matches": actual == expected})
            if b"\0" in data:
                errors.append("Unclassified binary artifact cannot be treated as text: " + path)
            if actual != expected:
                errors.append("Published artifact bytes do not match original identity: " + path)
            text_count += 1
        except (FileNotFoundError, subprocess.CalledProcessError) as error:
            errors.append(str(error))
    if not text_count:
        errors.append("No published text artifacts were checked")
    sources_path = str(PurePosixPath(identity_path).parent / "sources.json")
    sources = reader.json(sources_path)
    own_units = [path for path in sources if path.startswith("RTOS/")
                 and not path.startswith("RTOS/lib/") and path.lower().endswith(".c")]
    if len(own_units) != spec["owned_c_units"]:
        errors.append(f"Historical source generation expected {spec['owned_c_units']} owned C units, "
                      f"found {len(own_units)}")
    source_counts = Counter()
    if batch == "diagnostics_20261027":
        for path, metadata in sources.items():
            if not compiled_input(path):
                continue
            key = (path, metadata["sha256"])
            if key not in source_cache:
                try:
                    source_cache[key] = current_source_match(reader, path, metadata["sha256"])
                except (FileNotFoundError, subprocess.CalledProcessError) as error:
                    source_cache[key] = {"path": path, "captured_sha256": metadata["sha256"],
                                         "status": "MISSING_FROM_PUBLICATION", "error": str(error)}
            match = source_cache[key]
            source_counts[match["status"]] += 1
            if match["status"] in {"MISMATCH", "MISSING_FROM_PUBLICATION"}:
                errors.append("Current diagnostic compiled input does not match: " + path)
    return {"identity_path": identity_path, "published_identity_sha256": sha(reader.read(identity_path)),
            "tag": identity.get("tag"), "variant": identity.get("variant"),
            "text_only_status": "PASS" if not errors else "FAIL",
            "reported_hil_passed": identity.get("passed"), "text_artifacts_checked": text_count,
            "text_artifacts": text_artifacts, "binary_artifacts_not_verified": skipped,
            "historical_owned_c_units": len(own_units),
            "historical_sources_compared_to_current": batch == "diagnostics_20261027",
            "current_compiled_input_comparisons": dict(source_counts),
            "capture_reported_verification": identity.get("verification"),
            "errors": errors}


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--git-index", action="store_true")
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    reader = PublishedReader(args.git_index)
    batches = []
    source_cache = {}
    errors = []
    for batch, spec in SPECS.items():
        roots = [f"hil/{batch}/evidence/{group}/" for group in spec["groups"]]
        identities = sorted(path for path in reader.candidates if path.endswith("/identity.json")
                            and any(path.startswith(root) for root in roots))
        if len(identities) != spec["captures"]:
            errors.append(f"{batch}: expected {spec['captures']} formal captures, found {len(identities)}")
        captures = []
        for path in identities:
            try:
                result = verify_capture_text(reader, path, batch, spec, source_cache)
            except (ValueError, KeyError, FileNotFoundError, subprocess.CalledProcessError) as error:
                result = {"identity_path": path, "text_only_status": "FAIL", "errors": [str(error)]}
            captures.append(result)
            errors.extend(result["errors"])
        batches.append({"batch": batch, "formal_captures": len(captures),
                        "expected_captures": spec["captures"],
                        "expected_historical_owned_c_units": spec["owned_c_units"],
                        "text_only_passed": sum(item["text_only_status"] == "PASS" for item in captures),
                        "captures": captures})
    text_count = sum(capture.get("text_artifacts_checked", 0)
                     for batch in batches for capture in batch["captures"])
    skipped = sum(len(capture.get("binary_artifacts_not_verified", []))
                  for batch in batches for capture in batch["captures"])
    report = {"status": "PASS" if not errors else "FAIL", "scope": "PUBLISHED_TEXT_BYTES_ONLY",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "read_mode": "GIT_INDEX_ACTUAL_BLOBS" if args.git_index else "WORKSPACE_GIT_CANDIDATES_ONLY",
        "published_candidate_count": len(reader.candidates), "formal_captures": sum(
            batch["formal_captures"] for batch in batches), "text_artifacts_checked": text_count,
        "excluded_binary_artifacts_not_verified": skipped,
        "full_binary_or_hardware_identity_verified": False,
        "historical_source_generations": {batch: spec["owned_c_units"] for batch, spec in SPECS.items()},
        "diagnostic_current_source_comparisons": sorted(source_cache.values(),
            key=lambda item: (item["path"], item["captured_sha256"])),
        "batches": batches, "errors": errors,
        "limitations": ["PASS verifies published text bytes, not excluded ELF/bin or hardware identity",
            "Ignored local files are never used to fill missing published artifacts",
            "Historical channel sources are not equated with the current 18-unit implementation",
            "LF/CRLF equivalence of current source is reported separately from raw byte identity",
            "This does not rerun the historical HIL, old-baseline build or full local finalizer"]}
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    out = args.out_dir.resolve() if args.out_dir else HERE / "evidence" / stamp
    if HERE not in out.parents or out.exists():
        raise SystemExit("Output must be new and inside publish_20261027")
    out.mkdir(parents=True)
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "mode": report["read_mode"],
        "formal_captures": report["formal_captures"], "text_artifacts_checked": text_count,
        "excluded_binaries_not_verified": skipped, "errors": errors, "output": str(out)}))
    raise SystemExit(0 if not errors else 1)


if __name__ == "__main__":
    main()
