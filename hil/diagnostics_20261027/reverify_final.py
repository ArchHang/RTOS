"""Recheck final captures with both archived and corrected fault-context parsers.

Read-only for captures. The original identity/source/oracle versions remain
untouched; this independent report explains the parser-only correction.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path

from verify_results import read, require, verify_capture

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
ORIGINAL = HERE / "oracles/before_fault_context_fix/verify_results.py"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--directory", type=Path, default=HERE / "evidence/final")
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--expected-count", type=int, default=32)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("diagnostic_oracle_before_fault_fix", ORIGINAL)
    original = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(original)
    original_hash = digest(ORIGINAL)
    corrected_hash = digest(HERE / "verify_results.py")
    require(digest(ORIGINAL.with_name("contracts.json")) == digest(HERE / "contracts.json"),
            "ordinary fixture contract unchanged")
    rows = []
    outside_scope = []
    current_hashes = {}
    original_identities = {}
    for path in sorted(args.directory.rglob("identity.json")):
        meta = json.loads(read(path))
        if meta.get("variant") == "diagnostic_fault" or meta.get("oracle_sha256") != original_hash:
            outside_scope.append({"tag": meta.get("tag"), "stage": meta.get("stage"),
                "reason": "intentional fault or not captured under the archived ordinary parser"})
            continue
        require(meta.get("passed") and meta.get("stage") == "complete",
                "ordinary capture must be complete: " + str(path))
        original_identities[path] = digest(path)
        for name, expected in meta["artifacts"].items():
            require(digest(path.parent / name) == expected, "changed original artifact: " + name)
        require(meta["oracle_sha256"] == original_hash, "capture records the archived parser version")
        sources = json.loads(read(path.parent / "sources.json"))
        changed_inputs = []
        for relative, identity in sources.items():
            if relative not in current_hashes:
                current_hashes[relative] = digest(ROOT / relative)
            if current_hashes[relative] != identity["sha256"]:
                changed_inputs.append(relative)
        require(changed_inputs == ["hil/diagnostics_20261027/verify_results.py"],
                "only the parser source changed since these captures: " + str(changed_inputs))
        before = original.verify_capture(path.parent)
        after = verify_capture(path.parent)
        require(before == after, "ordinary acceptance result unchanged: " + meta["tag"])
        rows.append({"tag": meta["tag"], "variant": meta["variant"],
            "directory": path.parent.relative_to(HERE).as_posix(),
            "original_identity_sha256": original_identities[path],
            "original_oracle_sha256": meta["oracle_sha256"], "original_artifacts_unchanged": True,
            "archived_parser_result": before, "corrected_parser_result": after,
            "ordinary_acceptance_unchanged": True,
            "changed_inputs": changed_inputs, "passed": True})
    require(len(rows) == args.expected_count, "exact expected ordinary capture count")
    require(all(digest(path) == value for path, value in original_identities.items()),
            "no original identity modified during re-verification")
    require(digest(ORIGINAL) == original_hash and digest(HERE / "verify_results.py") == corrected_hash,
            "both parser versions stable during re-verification")
    out = args.out_dir.resolve() if args.out_dir else HERE / "reverification" / datetime.now().strftime(
        "%Y%m%d_%H%M%S_%f")
    require(HERE in out.parents and not out.exists(), "new report directory inside this batch required")
    out.mkdir(parents=True)
    report = {"status": "PASS", "created_utc": datetime.now(timezone.utc).isoformat(),
        "captures": len(rows), "scope": "32 final non-fault captures; artifacts and identities read-only",
        "original_oracle_path": ORIGINAL.relative_to(HERE).as_posix(),
        "original_oracle_sha256": original_hash, "corrected_oracle_sha256": corrected_hash,
        "parser_only_change": True, "production_or_fixture_source_changed": False,
        "ordinary_acceptance_equal_under_both_versions": True,
        "outside_scope": outside_scope,
        "explanation": "Fault oracle now matches the decoded UNDEFINSTR line inside the fault-status "
            "section and requires CFSR=0x00010000. The startup menu may also contain UNDEFINSTR. "
            "Ordinary acceptance rules, compiled inputs and original captured evidence are unchanged.",
        "results": rows}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (out / "report.md").write_text(
        "# 最终普通捕获的双版本复核\n\n"
        f"共 {len(rows)} 个已完成捕获，在原接受器与修正后的接受器下结果完全一致，全部通过。\n\n"
        "修正仅针对故障文本上下文：启动菜单也会出现 UNDEFINSTR，故障证据必须取自故障状态"
        "解码段并核对 CFSR=0x00010000。生产代码、夹具、构建参数和原始证据未改变。\n\n"
        "原 identity.json 保留原接受器版本；本报告另行记录新版本的复核结果，没有覆盖旧记录。"
        "故意故障捕获由修正后的接受器单独验证，不计入这 32 项。\n", encoding="utf-8")
    print(json.dumps({"status": "PASS", "captures": len(rows), "output": str(out)}))


if __name__ == "__main__":
    main()
