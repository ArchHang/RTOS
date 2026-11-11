#!/usr/bin/env python3
"""Run the real USB wrappers and CherryUSB classes with a native controller/RTOS stub."""

import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
HOST_PORT = """
#define RTOS_PORT_H_
#include <stdint.h>
typedef uint32_t rtos_stack_t;
#define RTOS_PORT_ENTER_CRITICAL() ((void)0)
#define RTOS_PORT_EXIT_CRITICAL() ((void)0)
#define RTOS_PORT_IN_ISR() 0U
"""
CASES = ("queue-full", "thread-full", "controller-fail", "media", "transport")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT, help="Root containing RTOS/")
    parser.add_argument("--cc", default="gcc", help="Native GCC executable")
    parser.add_argument("--opt", choices=("O0", "O2", "O3"), default="O2")
    parser.add_argument("--readonly", action="store_true", help="Compile with MSC media read-only")
    parser.add_argument("--evidence-dir", type=Path, help="Preserve this run's binary and build record")
    args = parser.parse_args()
    rtos = args.source_root.resolve() / "RTOS"
    cherry = rtos / "lib/third/CherryUSB"
    includes = [rtos / "Core/Inc", rtos / "Core/Private", *(cherry / folder for folder in (
        "common", "core", "class/cdc", "class/hid", "class/msc", "port/dwc2"))]
    sources = [*(rtos / "USB" / name for name in (
        "rtos_usb_device.c", "rtos_usb_msc.c", "rtos_usb_osal.c")),
        rtos / "Memory/rtos_mempool.c", *(cherry / name for name in (
            "core/usbd_core.c", "class/cdc/usbd_cdc_acm.c", "class/hid/usbd_hid.c",
            "class/msc/usbd_msc.c")), ROOT / "hil/fw/Src/usb_device_host.c"]
    print(f"USBHOST|BUILD|opt={args.opt}|rtos={rtos}", flush=True)
    with tempfile.TemporaryDirectory(prefix="rtos-usb-host-") as temporary:
        scratch = Path(temporary)
        port = scratch / "host_port.h"
        port.write_text(HOST_PORT, encoding="ascii")
        config = rtos / "Core/Inc/rtos_config.h"
        if args.readonly:
            includes[0] = shutil.copytree(includes[0], scratch / "include")
            config = includes[0] / config.name
            updated, count = re.subn(r"(?m)^#define RTOS_CONFIG_USB_MSC_READONLY .+$",
                                     "#define RTOS_CONFIG_USB_MSC_READONLY (1)",
                                     config.read_text(encoding="utf-8"))
            if count != 1:
                raise ValueError("Expected exactly one MSC read-only configuration value")
            config.write_text(updated, encoding="utf-8")
        executable = scratch / "usb_host.exe"
        # DWC2 headers declare 32-bit MMIO helpers that the host fixture never invokes.
        compile_args = [args.cc, "-std=gnu11", f"-{args.opt}", "-Wall", "-Wextra", "-Werror",
                        "-Wno-int-to-pointer-cast",
                        "-ffunction-sections", "-fdata-sections", "-DRTOS_CONFIG_ASSERT_ENABLED=0",
                        *(f"-I{include}" for include in includes), "-include", str(port),
                        *(str(source) for source in sources), "-Wl,--gc-sections", "-o", str(executable)]
        subprocess.run(compile_args, check=True, timeout=60)
        exits = [subprocess.run([str(executable), case], check=False, timeout=10).returncode
                 for case in CASES]
        if args.evidence_dir is not None:
            evidence = args.evidence_dir.resolve()
            evidence.mkdir(parents=True, exist_ok=False)
            shutil.copyfile(executable, evidence / executable.name)
            (evidence / "build.json").write_text(json.dumps({
                "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
                "arguments": compile_args,
                "source_digests": {str(path): sha256(path.read_bytes()).hexdigest()
                                   for path in [*sources, config, port]},
                "binary_digest": sha256(executable.read_bytes()).hexdigest(),
                "case_exits": dict(zip(CASES, exits)),
            }, indent=2) + "\n", encoding="utf-8")
    return int(any(exits))


if __name__ == "__main__":
    raise SystemExit(main())
