"""Negative controls for the UART/build oracle; no firmware or hardware runs.

Synthetic known-good inputs establish the parser acceptance contract. Each
mutation then removes or corrupts one required fact and must be rejected.
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path

from verify_results import verify_build, verify_diagnostics

HERE = Path(__file__).resolve().parent


def identity():
    return {"variant": "diagnostic_bench", "diagnostics": 1, "capacity": 64,
            "mask": "basepri", "channels": False, "assertions": 1,
            "optimization": "O2"}


def bench_text():
    lines = ["DG|META|mode=bench diagnostics=1 capacity=64 operations=1024 rounds=3"]
    count = 0
    for repetition in range(3):
        for suffix in ("nowait", "pingpong"):
            for operation in ("sem", "queue", "notify"):
                lines.append(f"DG|BENCH|name={operation}_{suffix} round={repetition} "
                             "operations=1024 cycles=10000 ticks=2 checksum=524800 errors=0")
                for check in ("bench_prepared", "bench_exact_work", "bench_worker_accounted",
                              "bench_cleanup"):
                    count += 1
                    lines.append(f"DG|CHECK|PASS|id={count} name={check}")
    lines.extend(("DG|SUMMARY|mode=bench pass=72 fail=0", "DG|DONE|status=0"))
    return "\n".join(lines) + "\n"


def build_text():
    sources = (
        "Core/Src/rtos_kernel.c", "Core/Src/rtos_task.c", "Core/Src/rtos_sched.c",
        "Core/Src/rtos_wait.c", "IPC/rtos_sem.c", "IPC/rtos_mutex.c", "IPC/rtos_queue.c",
        "IPC/rtos_event.c", "IPC/rtos_channel.c", "Memory/rtos_heap.c",
        "Memory/rtos_mempool.c", "Services/rtos_timer.c", "Services/rtos_perf.c",
        "Services/rtos_diag.c", "USB/rtos_usb_device.c", "USB/rtos_usb_osal.c",
        "USB/rtos_usb_msc.c", "Port/GCC/ARM_CM4F_CM7F/port.c")
    lines = [f'arm-none-eabi-gcc "../RTOS/{source}" -mcpu=cortex-m4 -c '
             '-DRTOS_CONFIG_USE_DIAGNOSTICS=1 -DRTOS_CONFIG_DIAGNOSTIC_CAPACITY=64 -O2'
             for source in sources]
    lines.append('arm-none-eabi-gcc "../hil/fwsrc/diagnostic_bench.c" -mcpu=cortex-m4 -c '
                 '-DRTOS_CONFIG_USE_DIAGNOSTICS=1 -DRTOS_CONFIG_DIAGNOSTIC_CAPACITY=64 '
                 '-O2 -Wall -Wextra -Werror')
    lines.extend(f"../Core/Src/main.c:{index + 1}:1: warning: unused variable "
                 "'fixture_unused' [-Wunused-variable]" for index in range(25))
    return "\n".join(lines) + "\n"


def fault_text():
    return ("System init done\n"
        "[FAULT-TEST] send '1'..'9' via USART2:\n"
        "  5: UNDEFINSTR  execute UDF halfword\n"
        "DG|META|mode=fault diagnostics=1 capacity=64\n"
        "DG|FAULT_ARMED|task=3 code=udf\n"
        "==================== FAULT REPORT ====================\n"
        "DIAG history frozen in SRAM; inspect diag_slots with the debugger.\n"
        "--- Exception context ---\n"
        "Mode       : Thread (task code)\n"
        "--- Fault status registers ---\n"
        "CFSR = 0x00010000\n"
        "  UsageFaults:\n"
        "    UNDEFINSTR  undefined instruction executed\n"
        "MMFAR = not valid\n"
        "BFAR  = not valid\n"
        "SHCSR = 0x00070008\n"
        "--- Action ---\n"
        "System halted. Fault context also mirrored in g_fault (debugger access).\n"
        "Locate source: arm-none-eabi-addr2line -e f446_rtos.elf -f -C <PC>\n"
        "======================================================\n")


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    out = args.out_dir or HERE / "oracle_evidence" / datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    out.mkdir(parents=True, exist_ok=False)
    good, build = bench_text(), build_text()
    config = identity()
    verify_diagnostics(good, config)
    verify_build(build, config)
    fault = fault_text()
    fault_config = config | {"variant": "diagnostic_fault", "fault": True}
    verify_diagnostics(fault, fault_config)
    for name, content in (("positive_bench.txt", good), ("positive_build.txt", build),
                          ("positive_fault_with_menu.txt", fault)):
        (out / name).write_text(content, encoding="utf-8")
    mutations = []

    def add(name, changed, verifier=verify_diagnostics, case_config=None):
        mutations.append((name, changed, verifier, config if case_config is None else case_config))

    add("lost_first_check", good.replace("DG|CHECK|PASS|id=1 name=bench_prepared\n", "", 1))
    add("duplicate_check", good.replace("DG|CHECK|PASS|id=1", "DG|CHECK|PASS|id=2", 1))
    add("wrong_check_name", good.replace("name=bench_cleanup", "name=unexpected", 1))
    add("failed_contract", good.replace("DG|CHECK|PASS|id=1", "DG|CHECK|FAIL|id=1", 1))
    add("malformed_contract", good.replace("DG|CHECK|PASS|id=1", "DG|CHECK|PASS|id=x", 1))
    add("extra_failure", good + "DG|OTHER|FAIL|reason=injected\n")
    add("missing_summary", good.replace("DG|SUMMARY|mode=bench pass=72 fail=0\n", ""))
    add("incorrect_summary", good.replace("pass=72 fail=0", "pass=73 fail=0"))
    add("duplicate_summary", good + "DG|SUMMARY|mode=bench pass=72 fail=0\n")
    add("lost_done", good.replace("DG|DONE|status=0\n", ""))
    add("failed_done", good.replace("DG|DONE|status=0", "DG|DONE|status=1"))
    add("duplicate_done", good + "DG|DONE|status=0\n")
    add("wrong_mode", good.replace("mode=bench", "mode=functional", 1))
    add("wrong_runtime_switch", good.replace("diagnostics=1", "diagnostics=0", 1))
    add("wrong_runtime_capacity", good.replace("capacity=64", "capacity=32", 1))
    add("missing_measurement", "\n".join(line for line in good.splitlines()
                                         if not line.startswith("DG|BENCH|name=sem_nowait round=0")))
    add("duplicate_measurement", good.replace("name=sem_nowait round=0", "name=sem_nowait round=1", 1))
    add("less_work", good.replace("operations=1024 cycles=10000", "operations=1023 cycles=10000", 1))
    add("zero_cycles", good.replace("cycles=10000", "cycles=0", 1))
    add("possible_dwt_wrap", good.replace("ticks=2", "ticks=30000", 1))
    add("negative_ticks", good.replace("ticks=2", "ticks=-1", 1))
    add("bad_payload_checksum", good.replace("checksum=524800", "checksum=524801", 1))
    add("worker_error", good.replace("errors=0", "errors=1", 1))
    add("missing_kernel_unit", build.replace(next(line + "\n" for line in build.splitlines()
                                                  if 'Services/rtos_diag.c"' in line), ""), verify_build)
    add("duplicate_kernel_unit", build + build.splitlines()[0] + "\n", verify_build)
    add("mixed_diagnostic_switch", build.replace("-DRTOS_CONFIG_USE_DIAGNOSTICS=1", "", 1), verify_build)
    add("mixed_capacity", build.replace("-DRTOS_CONFIG_DIAGNOSTIC_CAPACITY=64", "", 1), verify_build)
    add("mixed_assertions", build.replace("-c ", "-c -DRTOS_CONFIG_ASSERT_ENABLED=0 ", 1), verify_build)
    add("mixed_mask", build.replace("-c ", "-c -DRTOS_CONFIG_USE_BASEPRI=0 ", 1), verify_build)
    add("mixed_optimization", build.replace("-O2", "-O3", 1), verify_build)
    add("missing_strict_fixture", build.replace("-Wextra -Werror", ""), verify_build)
    add("new_warning", build + "../RTOS/Services/rtos_diag.c:1:1: warning: injected\n", verify_build)
    add("compiler_error", build + "../RTOS/Services/rtos_diag.c:1:1: error: injected\n", verify_build)
    decode_line = "    UNDEFINSTR  undefined instruction executed\n"

    def add_fault(name, changed):
        add(name, changed, case_config=fault_config)

    add_fault("fault_missing_armed", fault.replace("DG|FAULT_ARMED|task=3 code=udf\n", ""))
    add_fault("fault_missing_decode", fault.replace(decode_line, ""))
    add_fault("fault_only_menu_undefinstr", fault.replace(decode_line,
        "    DIVBYZERO   integer division by zero\n"))
    add_fault("fault_wrong_cfsr", fault.replace("CFSR = 0x00010000", "CFSR = 0x02000000"))
    add_fault("fault_missing_cfsr", fault.replace("CFSR = 0x00010000\n", ""))
    add_fault("fault_duplicate_cfsr", fault.replace("CFSR = 0x00010000\n",
        "CFSR = 0x00010000\nCFSR = 0x00010000\n"))
    add_fault("fault_decode_outside_status", fault.replace(decode_line, "").replace(
        "--- Fault status registers ---", decode_line + "--- Fault status registers ---"))
    add_fault("fault_nested", fault + "*** NESTED FAULT: injected\n")
    add_fault("fault_normal_done", fault + "DG|DONE|status=0\n")
    add_fault("fault_normal_summary", fault + "DG|SUMMARY|mode=fault pass=1 fail=0\n")
    add_fault("fault_missing_report", fault.replace("FAULT REPORT", "OTHER REPORT"))
    add_fault("fault_missing_diag_banner", fault.replace(
        "DIAG history frozen in SRAM; inspect diag_slots with the debugger.\n", ""))
    add_fault("fault_wrong_armed_opcode", fault.replace("code=udf", "code=divide"))
    add_fault("fault_zero_armed_task", fault.replace("task=3 code=udf", "task=0 code=udf"))
    add_fault("fault_armed_after_report", fault.replace("DG|FAULT_ARMED|task=3 code=udf\n", "")
        + "DG|FAULT_ARMED|task=3 code=udf\n")
    results = []
    for name, changed, verifier, case_config in mutations:
        rejected = False
        try:
            verifier(changed, case_config)
        except (AssertionError, KeyError, ValueError):
            rejected = True
        (out / (name + ".txt")).write_text(changed, encoding="utf-8")
        results.append({"name": name, "injected": True, "rejected": rejected,
                        "variant": case_config["variant"]})
        if not rejected:
            raise SystemExit("Oracle accepted broken evidence: " + name)
    report = {"status": "PASS", "scope": "synthetic parser negative controls only",
              "good_controls": 3, "rejected_mutations": len(results), "results": results,
              "oracle_sha256": hashlib.sha256((HERE / "verify_results.py").read_bytes()).hexdigest(),
              "driver_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    (out / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
