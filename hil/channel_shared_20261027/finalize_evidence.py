"""Recheck this CPU/shared-memory batch without touching firmware or hardware."""
import hashlib
import json
from pathlib import Path

from verify_results import require, verify_capture

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    manifest = load(HERE / "baseline/manifest.json")
    for item in manifest["sources"]:
        require(sha(HERE / item["snapshot"]) == item["sha256"], "unchanged archived baseline")
    groups = {}
    for batch, expected in (("baseline_replay", 2), ("optimized", 29),
                            ("regression", 13), ("default_off", 2)):
        identities = sorted((HERE / "evidence" / batch).rglob("identity.json"))
        require(len(identities) == expected, f"exact capture count: {batch}")
        records = []
        for identity in identities:
            meta = load(identity)
            require(meta["passed"] and meta["stage"] == "complete", "completed capture")
            require(meta["variant"] != "channel_dma", "no DMA workflow")
            for name, expected_hash in meta["artifacts"].items():
                require(sha(identity.parent / name) == expected_hash, f"changed artifact: {name}")
            sources = load(identity.parent / "sources.json")
            if batch == "baseline_replay":
                require(meta["build_provenance"] == "replayed_verified_image", "honest replay provenance")
                require(sha(identity.parent / "original_identity.json") ==
                        meta["original_identity_sha256"], "original replay identity")
                for item in manifest["sources"]:
                    require(sources[item["source"]]["sha256"] == item["sha256"], "replayed production")
            else:
                require(meta["build_provenance"] == "fresh_build", "fresh optimized build")
                for name, item in sources.items():
                    if name.startswith(("RTOS/", "Core/", "Drivers/", "AI/", "hil/templates/")) \
                            or name in ("hil/build_fw.ps1", "hil/build_layout.ps1", "hil/run_hil.ps1"):
                        require(sha(ROOT / name) == item["sha256"], f"compiled input changed: {name}")
                for item in manifest["fixed_fixtures"]:
                    require(sha(ROOT / item["source"]) == item["sha256"], "unchanged fixture")
            records.append({"tag": meta["tag"], "identity_sha256": sha(identity),
                            **verify_capture(identity.parent)})
        groups[batch] = records
    strict_reports = sorted((HERE / "strict/evidence").glob("*/summary.json"))
    require(len(strict_reports) == 2 and sum(load(p)["passed"] for p in strict_reports) == 430,
            "429 plus one strict compilation")
    require(all(load(p)["status"] == "PASS" and load(p)["source_identity_stable"]
                for p in strict_reports), "strict inputs stable")
    host_paths = [HERE / "host/evidence" / phase / "summary.json"
                  for phase in ("baseline_pre_optimization", "optimized_regions_v1")]
    for path, count in zip(host_paths, (6, 8)):
        data = load(path)
        require(data["status"] == "PASS" and len(data["results"]) == count
                and all(r["passed"] for r in data["results"]), "all host processes pass")
    oracles = sorted((HERE / "evidence").glob("oracle_tests_*/summary.json"))
    require(len(oracles) == 1 and load(oracles[0])["status"] == "PASS", "negative oracle controls")
    require(load(HERE / "evidence/portability.json")["passed"], "direct dependency audit")
    require(load(HERE / "evidence/comparison/comparison.json")["passed"], "performance comparison")
    for name in ("default_off.json", "restored.json"):
        require(load(HERE / "evidence/default_equivalence" / name)["allocated_image_identical"],
                "default load image unchanged")
    report = {"status": "PASS", "hardware_captures": 46,
              "fresh_builds": 44, "replayed_verified_images": 2, "dma_workflows": 0,
              "current_channel_sha256": sha(ROOT / "RTOS/IPC/rtos_channel.c"),
              "current_main_sha256": sha(ROOT / "Core/Src/main.c"),
              "oracle_sha256": sha(HERE / "verify_results.py"), "groups": groups,
              "auxiliary_reports": [{"path": str(p.relative_to(HERE)), "sha256": sha(p)}
                                    for p in (*strict_reports, *host_paths, *oracles)],
              "limits": ["Finite scenarios, not proof of all bugs absent",
                         "One active channel in hardware performance observations",
                         "Eight-owner index/lifecycle tests use the native scripted port",
                         "M7 code generation only; port adaptation still required for other CPUs"]}
    (HERE / "evidence/final_summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print("46 CPU/shared-memory captures, 430 compiles, host, oracle and default image: PASS")


if __name__ == "__main__":
    main()
