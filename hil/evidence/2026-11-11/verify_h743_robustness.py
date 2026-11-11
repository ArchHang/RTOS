"""Recheck H743 scheduling, lifecycle and production-stress UART ledgers."""

import json
from pathlib import Path

from verify import integers, need, one, pressure, rows


ARCHIVE = Path(__file__).resolve().parent / "raw" / "h743-robustness"


def load(tag):
    return (ARCHIVE / f"{tag}.txt").read_text(encoding="utf-8")


def check_regressions():
    for tag, created, checks in (("base_reap_exact", 40, (7, 1)), ("fix_reap_exact", 256, (8, 0)),
                                  ("fix_reap_O0_clean", 256, (8, 0))):
        text = load(tag)
        load_row = one(text, "H7REAP|LOAD|")
        need(integers(load_row, "created", "completed", "idle_switches") == (created, created, 0), tag)
        need(integers(one(text, "H7REAP|DONE|"), "pass", "fail") == checks, tag)

    for tag, expected in (("base_resume_exact", 0), ("fix_resume_O2", 32), ("fix_resume_O3", 32)):
        text = load(tag)
        result = one(text, "H7RESUME|RESULT|")
        need(integers(result, "rounds", "irq", "valid", "selected_high", "high_first", "low_first", "clean") ==
             (32, 32, 32, expected, expected, 32 - expected, 32), tag)
        need(integers(one(text, "H7RESUME|DONE|"), "pass", "fail") ==
             ((4, 0) if expected == 32 else (2, 2)), tag)

    text = load("base_noslice_exact")
    need("H7B|CHECK|lowest_priority_forever|FAIL|round=0" in text, "Lowest priority baseline")
    need(integers(one(text, "H7B|DONE|"), "status", "rounds") == (1, 0), "Lowest priority baseline")
    for tag, count in (("fix_noslice_O2", 32), ("fix_boundary_coop", 32), ("fix_boundary_O0_nocache", 128)):
        done = one(load(tag), "H7B|DONE|")
        need(integers(done, "status", "failures", "rounds", "irq", "accepted", "full", "task_timeouts") ==
             (0, 0, count, 4 * count, 16 * count, 4 * count, count), tag)

    for tag, config in (("fix_fifo_O2", (1, 1)), ("fix_fifo_coop", (0, 0))):
        text = load(tag)
        need(integers(one(text, "H7FIFO|BEGIN|"), "preempt", "time_slice") == config, tag)
        done = one(text, "H7FIFO|DONE|")
        need(integers(done, "status", "created", "completed", "fifo", "removed", "solo", "clean",
                      "entries", "solo_ticks", "solo_switches", "idle_switches") ==
             (0, 3, 1, 1, 1, 1, 1, 12, 4, 0, 0), tag)


def check_lifecycle(tag, duration, seed):
    text = load(tag)
    start = one(text, "H7L|START|")
    need(integers(start, "seconds", "seed") == (duration, seed), tag)
    phases = rows(text, "H7L|PHASE|")
    need([int(phase["phase"]) for phase in phases] == [0, 1, 2], tag)
    for phase in phases:
        ticks, irq, gap = integers(phase, "ticks", "irq", "max_gap_cycles")
        need(ticks > 0 and irq >= 2 * ticks and 0 < gap <= 4_800_000, tag)
    need(sum(int(phase["ticks"]) for phase in phases) >= duration * 1_000, tag)
    total = one(text, "H7L|TOTAL|")
    round_counts = integers(total, "deliver", "reopen", "timeout", "drop_rounds")
    need(min(round_counts) > 0 and sum(round_counts) == int(total["rounds"]), tag)
    need(int(total["tasks"]) == sum(integers(total, "self_delete", "external_delete")) and
         min(integers(total, "self_delete", "blocked_delete", "overload_refused", "nested")) > 0, tag)
    need(integers(total, "heap_live", "heap_max_alloc") == (0, int(start["heap"])), tag)
    sources = rows(text, "H7L|SOURCE|")
    need([int(src["id"]) for src in sources] == [0, 1, 2, 3], tag)
    for source in sources:
        accepted, received = integers(source, "accepted", "received")
        need(accepted > 0 and accepted == received, tag)
    need(int(total["irq"]) == sum(integers(sources[3], "accepted", "refused")), tag)
    need(one(text, "H7L|DONE|")["status"] == "0" and "|FAIL|" not in text, tag)
    return {"rounds": int(total["rounds"]), "irq": int(total["irq"]),
            "packets": sum(int(source["received"]) for source in sources)}


def main():
    check_regressions()
    results = {}
    metadata = json.loads((ARCHIVE / "runs.json").read_text(encoding="utf-8"))
    need(metadata["restoration"] == {"restored": True, "readback_bytes": 2_097_152}, "Original flash readback")
    for tag in ("base_pressure180", "fix_pressure180_real"):
        text = load(tag)
        need(integers(one(text, "STRESS|BEGIN|"), "seconds", "seed", "queue_capacity") ==
             (180, 3735928559, 8), tag)
        results[tag] = {"packets": pressure(text, metadata[tag])}
    for tag, duration, seed in (("fix_lifecycle60", 60, 3735928559),
                                ("fix_lifecycle45_O3", 45, 1592594996)):
        results[tag] = check_lifecycle(tag, duration, seed)
    print(json.dumps({"status": "PASS", **results}, indent=2))


if __name__ == "__main__":
    main()
