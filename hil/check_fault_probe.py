#!/usr/bin/env python3
"""Check an intentional F446 UDF capture against the probe's hardware contract."""

import argparse
import json
from pathlib import Path
import re


HEX = r"(0x[0-9A-Fa-f]{8})"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def one(pattern, text):
    matches = re.findall(pattern, text, re.MULTILINE)
    require(len(matches) == 1, f"Expected one match for {pattern}, found {len(matches)}")
    return matches[0]


def verify(text, use_primask, legacy_stack):
    context = one(r"^FDIAG\|BEGIN\|(task|fpu|isr)$", text)
    one(r"^={54}$", text)
    require("FDIAG|FAIL" not in text, "Probe returned from the intentional fault")
    cfsr = int(one(r"^CFSR = " + HEX, text), 16)
    require(cfsr & (1 << 16), "The UDF did not set CFSR.UNDEFINSTR")
    if use_primask:
        hfsr = int(one(r"^HFSR[ ]+= " + HEX, text), 16)
        require(hfsr & (1 << 30), "PRIMASK probe did not escalate to HardFault")

    report = {"context": context, "cfsr": hex(cfsr)}
    if not legacy_stack:
        values = one(r"^FAULT\|MASK\|basepri=" + HEX + r"\|primask=" + HEX
                     + r"\|faultmask=" + HEX + r"\|crit_depth=" + HEX + "$", text)
        masks = tuple(int(value, 16) for value in values)
        require(masks == (0, 1, 0, 1) if use_primask else masks == (0x50, 0, 0, 1),
                "Fault entry masks or critical nesting differ from probe")
        report.update(zip(("basepri", "primask", "faultmask", "critical_depth"), masks))

    if context == "isr":
        one(r"^Stack frame: MSP, basic frame", text)
        require(re.search(r"IPSR = 22 \(Exception 22, IRQ 6\)", text), "EXTI0 origin missing")
        require("\nTask       :" not in text, "Handler/MSP wrongly attributed to a task")
        return report

    frame = "extended" if context == "fpu" else "basic"
    one(r"^Stack frame: PSP, " + frame + r" frame", text)
    sp = int(one(r"^Frame SP   = " + HEX + r" \(valid\)$", text), 16)
    base, size = (int(value, 16) for value in
                  one(r"^Task stack : base " + HEX + ", size " + HEX + " bytes$", text))
    require(base <= sp <= base + size, "Frame SP is outside the probe stack")
    used = int(one(r"^Stack usage: " + HEX, text), 16)
    remaining = sp - base
    if legacy_stack:
        require(used == remaining and remaining != size - remaining,
                "Old formatter did not exhibit the expected inverted stack reading")
    else:
        free = int(one(r"^Stack free : " + HEX, text), 16)
        require(used == size - remaining and free == remaining, "Stack distances are inconsistent")
    report.update(frame=frame, stack_bytes=size, reported_used=used, actual_remaining=remaining)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--primask", action="store_true")
    mode.add_argument("--legacy-stack", action="store_true")
    args = parser.parse_args()
    try:
        report = verify(args.capture.read_text(encoding="utf-8-sig"), args.primask, args.legacy_stack)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Fault capture rejected: {error}\n")
    print(json.dumps(report, ensure_ascii=True))


if __name__ == "__main__":
    main()
