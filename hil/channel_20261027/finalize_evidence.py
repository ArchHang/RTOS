"""Recheck all final captures and current production inputs; write a compact index."""
import hashlib
import json
from pathlib import Path

from verify_results import require, verify_capture

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    groups = {}
    for batch, expected in (("matrix", 33), ("regression", 13), ("default_off", 2)):
        captures = sorted((HERE / "evidence" / batch).rglob("identity.json"))
        require(len(captures) == expected, f"exact final count {batch}")
        records = []
        for identity in captures:
            obj = json.loads(identity.read_text())
            require(obj["passed"] and obj["stage"] == "complete", "completed identity")
            for name, expected_hash in obj["artifacts"].items():
                require(sha(identity.parent / name) == expected_hash, f"changed artifact: {name}")
            sources = json.loads((identity.parent / "sources.json").read_text())
            for name, meta in sources.items():
                if name.startswith(("RTOS/", "Core/", "Drivers/", "AI/")) \
                        or name in ("hil/build_fw.ps1", "hil/build_layout.ps1", "hil/run_hil.ps1") \
                        or name.startswith("hil/templates/"):
                    require(sha(ROOT / name) == meta["sha256"], f"changed compiled input: {name}")
            if obj["variant"].startswith("channel_"):
                variant_source = "channel_stress" if obj["variant"] == "channel_stress_negative" \
                    else obj["variant"]
                for unit in (variant_source, "channel_fixture"):
                    name = f"hil/fw/Src/{unit}.c"
                    require(sha(ROOT / name) == sources[name]["sha256"], f"changed fixture {name}")
            records.append({"tag": obj["tag"], "identity": str(identity.relative_to(HERE)),
                            "identity_sha256": sha(identity), **verify_capture(identity.parent)})
        groups[batch] = records
    strict = sorted((HERE / "strict/evidence").glob("*/summary.json"))
    host = sorted((HERE / "host/evidence").glob("*/summary.json"))
    oracle = sorted((HERE / "evidence").glob("oracle_tests_*/summary.json"))
    require(len(strict) == len(host) == len(oracle) == 1, "one final report per auxiliary suite")
    require(json.loads(strict[0].read_text())["passed"] == 430, "430 strict checks")
    require(json.loads(host[0].read_text())["status"] == "PASS", "host model pass")
    require(json.loads(oracle[0].read_text())["status"] == "PASS", "oracle controls pass")
    for name in ("default_off_equivalence.json", "restored_daily_equivalence.json"):
        require(json.loads((HERE / "evidence/baseline" / name).read_text())[
            "allocated_image_identical"], "unchanged default allocated image")
    readback = HERE / "evidence/preflight/original_stress_failure/loaded_sections_verified.json"
    require(json.loads(readback.read_text())["all_loaded_sections_match"], "debug image verified")
    report = {"status": "PASS", "hardware_captures": 48,
              "current_oracle_sha256": sha(HERE / "verify_results.py"),
              "production_channel_sha256": sha(ROOT / "RTOS/IPC/rtos_channel.c"),
              "main_sha256": sha(ROOT / "Core/Src/main.c"),
              "groups": groups,
              "auxiliary": [{"path": str(p.relative_to(HERE)), "sha256": sha(p)}
                            for p in (*strict, *host, *oracle, readback)],
              "limits": ["Finite tested scenarios, not proof of absence of unknown defects",
                         "Expected corruption failures accepted only by the exact negative oracle",
                         "Original stress error localized to a consumer receive timeout",
                         "M7 compile only; no ADC/USB bus or M7 cache HIL claim"]}
    (HERE / "evidence/final_summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print("48 captures + host + 430 compile checks + oracle + default equivalence: PASS")


if __name__ == "__main__":
    main()
