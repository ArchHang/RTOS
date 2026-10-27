"""Mutation-test the CPU/shared-memory oracle using optimized capture copies.

No serial port, build tool, original capture or production file is modified.
Full-capture tests call verify_capture; UART-only tests call verify_channel
on this batch's optimized captures. No DMA variant is selected by this suite.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile
import types

HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_oracle():
    path = HERE / "verify_results.py"
    source = path.read_bytes()
    module = types.ModuleType("channel_oracle_under_test")
    module.__file__ = str(path)
    # Execute the exact recorded source bytes, avoiding a stale .pyc cache.
    exec(compile(source, str(path), "exec"), module.__dict__)
    return module, hashlib.sha256(source).hexdigest()


def read(path):
    return path.read_text(encoding="utf-8-sig")


def edit(path, transform):
    original = read(path)
    changed = transform(original)
    if changed == original:
        raise RuntimeError("mutation did not change input: " + str(path))
    path.write_text(changed, encoding="utf-8")


def replace_once(text, pattern, replacement):
    changed, count = re.subn(pattern, replacement, text, count=1, flags=re.MULTILINE)
    if count != 1:
        raise RuntimeError("missing mutation target: " + pattern)
    return changed


def passing_capture(oracle, evidence):
    preferred = evidence / "optimized/channel_checks_basepri_a1_O2/identity.json"
    candidates = [preferred] + sorted((evidence / "optimized").rglob("identity.json"))
    for path in candidates:
        if not path.is_file():
            continue
        identity = json.loads(read(path))
        if (identity.get("variant") != "channel_checks"
                or identity.get("passed") is not True or identity.get("stage") != "complete"):
            continue
        for name, expected in identity["artifacts"].items():
            if sha(path.parent / name) != expected:
                raise RuntimeError("original artifact changed: " + str(path.parent / name))
        oracle.verify_capture(path.parent)
        return path.parent
    return None


def passing_uart(oracle, evidence, variant):
    allowed = {"channel_bench", "channel_profile", "channel_stress", "channel_stress_negative"}
    if variant not in allowed:
        raise RuntimeError("Non-CPU/shared-memory baseline is outside this batch: " + variant)
    candidates = sorted((evidence / "optimized").rglob("identity.json"))
    for path in candidates:
        identity = json.loads(read(path))
        if identity.get("variant") == variant and identity.get("passed") is True:
            log = path.parent / ("log_" + identity["tag"] + ".txt")
            if log.is_file():
                oracle.verify_channel(read(log), variant)
                return log
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, default=HERE / "evidence")
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S_%f")
    output = args.out_dir.resolve() if args.out_dir else evidence / ("oracle_tests_" + stamp)
    output.mkdir(parents=True, exist_ok=False)
    oracle, oracle_hash = load_oracle()
    results = []
    transcript = []
    originals = {}
    missing = []

    def record(name, scope, action, rejection=None):
        try:
            action()
        except AssertionError as error:
            if rejection is None:
                raise
            if rejection not in str(error):
                raise RuntimeError(f"{name}: wrong rejection reason: {error}") from error
            detail = str(error)
        else:
            if rejection is not None:
                raise RuntimeError(name + ": mutated evidence was incorrectly accepted")
            detail = "unaltered real evidence accepted"
        results.append({"name": name, "scope": scope, "passed": True, "detail": detail})
        line = f"ORACLE|PASS|name={name} scope={scope} detail={detail}"
        transcript.append(line)
        print(line, flush=True)

    def remember(path):
        originals[str(path)] = sha(path)

    # Keep throwaway copies inside the requested writable evidence directory.
    with tempfile.TemporaryDirectory(prefix="temporary_copies_", dir=output) as temporary:
        temporary = Path(temporary)
        capture = passing_capture(oracle, evidence)
        if capture is None:
            missing.append("complete channel_checks capture with valid build and ELF identity")
        else:
            identity = json.loads(read(capture / "identity.json"))
            tag = identity["tag"]
            for path in capture.iterdir():
                if path.is_file():
                    remember(path)

            def full_case(name, mutation=None, rejection=None):
                target = temporary / name
                shutil.copytree(capture, target)
                if mutation:
                    mutation(target, tag)
                record(name, "complete_capture_copy", lambda: oracle.verify_capture(target),
                       rejection)

            full_case("real_capture_control")
            full_case("extra_failed_verdict", lambda p, t: edit(p / f"log_{t}.txt",
                lambda text: text + "\nOTHER|FAIL|unexpected_extra_error\n"),
                "no extra failed verdicts")
            full_case("malformed_check_verdict", lambda p, t: edit(p / f"log_{t}.txt",
                lambda text: text + "\nCH|CHECK|FAIL|id=414 unexpected\n"),
                "no malformed check verdicts")
            full_case("actual_fault_report", lambda p, t: edit(p / f"log_{t}.txt",
                lambda text: text + "\n==================== FAULT REPORT =====\n"),
                "no fatal fault")

            def wrong_optimization(path, tag):
                def mutate(text):
                    source = r'^(arm-none-eabi-gcc "../RTOS/Core/Src/rtos_kernel\.c".*)$'
                    destination = "-O0" if identity["optimization"] != "O0" else "-O3"
                    return replace_once(text, source, lambda match: replace_once(
                        match[1], r"-O[0-3sg]\b", destination))
                edit(path / "build_detail.txt", mutate)

            full_case("wrong_compiled_optimization", wrong_optimization,
                      "uniform labeled optimization")

            def wrong_elf(path, tag):
                elf = path / f"firmware_{tag}.elf"
                data = bytearray(elf.read_bytes())
                data[16] ^= 1
                elf.write_bytes(data)

            full_case("wrong_firmware_bytes", wrong_elf, "flashed ELF identity")

            def change_identity(path, key, value):
                target = path / "identity.json"
                meta = json.loads(read(target))
                meta[key] = value
                target.write_text(json.dumps(meta), encoding="utf-8")

            full_case("wrong_built_elf_binding", lambda p, t: change_identity(
                p, "built_elf_sha256", "0" * 64), "flashed ELF identity")
            full_case("missing_flash_verification", lambda p, t: edit(
                p / f"flash_{t}.txt", lambda text: text.replace(
                    "Download verified successfully", "Download verification omitted")),
                "expected physical ST-LINK and successful verification")

        def uart_cases(variant, mutations):
            original = passing_uart(oracle, evidence, variant)
            if original is None:
                missing.append("current valid UART for " + variant)
                return
            remember(original)
            prefix = variant.removeprefix("channel_")
            record(prefix + "_real_uart_control", "real_uart_only",
                   lambda: oracle.verify_channel(read(original), variant))
            for name, transform, rejection in mutations:
                copied = temporary / (prefix + "_" + name + ".txt")
                shutil.copy2(original, copied)
                edit(copied, transform)
                record(prefix + "_" + name, "real_uart_copy",
                       lambda copied=copied: oracle.verify_channel(read(copied), variant),
                       rejection)

        def drop_bench_cell(text):
            return replace_once(text, r"^CH\|BENCH\|[^\n]*\n?", "")

        def duplicate_bench_cell(text):
            lines = text.splitlines()
            indices = [i for i, line in enumerate(lines) if line.startswith("CH|BENCH|")]
            if len(indices) != 270:
                raise RuntimeError("baseline does not have exactly 270 bench cells")
            lines[indices[1]] = lines[indices[0]]
            return "\n".join(lines) + "\n"

        uart_cases("channel_bench", [
            ("missing_cell", drop_bench_cell, "270 distinct measurements"),
            ("duplicate_cell_same_row_count", duplicate_bench_cell, "270 distinct measurements"),
            ("incorrect_message_work", lambda text: replace_once(text,
                r"^(CH\|BENCH\|[^\n]* messages=)\d+", r"\g<1>1"), "matched work"),
        ])

        def drop_profile_cell(text):
            return replace_once(text, r"^CH\|PROFILE\|[^\n]*\n?", "")

        def duplicate_profile_cell(text):
            lines = text.splitlines()
            indices = [i for i, line in enumerate(lines) if line.startswith("CH|PROFILE|")]
            if len(indices) != 280:
                raise RuntimeError("baseline does not have exactly 280 profile cells")
            lines[indices[1]] = lines[indices[0]]
            return "\n".join(lines) + "\n"

        def profile_count(text, field, count):
            return replace_once(text, r"^(CH\|PROFILE\|[^\n]* " + field + r"=)1\b",
                                r"\g<1>" + str(count))

        uart_cases("channel_profile", [
            ("missing_cell", drop_profile_cell, "280 distinct IRQ measurements"),
            ("duplicate_cell_same_row_count", duplicate_profile_cell,
             "280 distinct IRQ measurements"),
            ("incorrect_hook_hits", lambda text: profile_count(text, "hook_hits", 0),
             "exactly one actual legal IRQ"),
            ("incorrect_irq_count", lambda text: profile_count(text, "irq_count", 2),
             "exactly one actual legal IRQ"),
        ])

        def zero_consumer_same_sum(text):
            lines = text.splitlines()
            indices = [i for i, line in enumerate(lines) if line.startswith("CH|CONSUMER|")]
            if len(indices) != 4:
                raise RuntimeError("baseline does not have all four consumers")
            first = int(oracle.fields(lines[indices[0]])["consumed"])
            second = int(oracle.fields(lines[indices[1]])["consumed"])
            lines[indices[0]] = re.sub(r"consumed=\d+", "consumed=0", lines[indices[0]])
            lines[indices[1]] = re.sub(r"consumed=\d+", "consumed=" + str(first + second),
                                       lines[indices[1]])
            return "\n".join(lines) + "\n"

        uart_cases("channel_stress", [
            ("zero_consumer_unchanged_total", zero_consumer_same_sum,
             "each consumer participates"),
            ("missing_task_diagnostic", lambda text: replace_once(
                text, r"^CH\|TASK\|[^\n]*\n?", ""),
             "all eight tasks and the IRQ have diagnostics"),
            ("unclassified_task_error", lambda text: replace_once(
                text, r"^(CH\|TASK\|[^\n]* errors=)0\b", r"\g<1>1"),
             "no unclassified API/timeout errors"),
            ("incorrect_business_pacing", lambda text: replace_once(
                text, r"consumer_period=1\b", "consumer_period=16"),
             "explicit business pacing and diagnostics"),
        ])
        uart_cases("channel_stress_negative", [
            ("additional_failure", lambda text: text + "\nOTHER|FAIL|extra_error\n",
             "no extra failed verdicts"),
            ("additional_malformed_failure", lambda text:
             text + "\nCH|CHECK|FAIL|id=49 unexpected\n", "no malformed check verdicts"),
            ("different_failure_name", lambda text: text.replace(
                "name=stress_unique_message_bitmap", "name=unrelated_real_bug"),
             "only specified injected failures"),
            ("second_corruption", lambda text: replace_once(
                text, r"checksum_errors=1\b", "checksum_errors=2"), "exact message ledger"),
        ])

    unchanged = all(sha(Path(path)) == expected for path, expected in originals.items())
    if not unchanged:
        raise RuntimeError("original evidence changed during oracle tests")
    if sha(HERE / "verify_results.py") != oracle_hash:
        raise RuntimeError("oracle changed during execution; rerun its final version")
    status = "INCOMPLETE" if missing else "PASS"
    report = {"status": status, "oracle_sha256": oracle_hash,
              "test_source_sha256": sha(Path(__file__).resolve()), "checks": len(results),
              "originals_unchanged": unchanged, "originals": originals,
              "missing": missing, "results": results,
              "limitations": ["Mutation tests validate the oracle, not another hardware run",
                              "UART-only baselines do not supply fresh-build or ELF proof"]}
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    transcript.append(f"ORACLE|SUMMARY|status={status} pass={len(results)} missing={len(missing)}")
    transcript.extend("ORACLE|MISSING|" + item for item in missing)
    (output / "run.txt").write_text("\n".join(transcript) + "\n", encoding="utf-8")
    print(transcript[-1] if not missing else transcript[-len(missing)-1], flush=True)
    print("Evidence: " + str(output), flush=True)
    if missing:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
