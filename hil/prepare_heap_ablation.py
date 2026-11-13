#!/usr/bin/env python3
"""Generate a benchmark-only heap source while preserving the checked-in heap API."""

import argparse
from pathlib import Path


# The interior offset and its bound are coupled. A zero-only bound retains the
# ability to resume after a proven all-live prefix and leaves the end cache intact.
MODES = {
    "current": (True, True, True, True),
    "no_live_prefix": (False, True, True, True),
    "no_interior_checkpoint": (True, False, False, True),
    "no_prefix_max": (True, True, False, True),
    "no_capacity_cache": (True, True, True, False),
    "live_prefix_only": (True, False, False, False),
    "linear": (False, False, False, False),
}


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"expected one source site starting with {old.splitlines()[0]!r}")
    return source.replace(old, new, 1)


def remove_live_prefix(source):
    source = replace_once(source, "static uint32_t s_live_prefix_end;", "")
    source = replace_once(
        source,
        "    if ((need <= prefix_max) || (offset < s_live_prefix_end)) {\n"
        "        offset = s_live_prefix_end;",
        "    if (need <= prefix_max) {\n"
        "        offset = 0U;",
    )
    source = replace_once(
        source,
        "            if (prefix_max == 0U) {\n"
        "                s_live_prefix_end = offset + used;\n"
        "            }\n",
        "",
    )
    source = replace_once(
        source,
        "    if (offset < s_live_prefix_end) {\n"
        "        s_live_prefix_end = offset;\n"
        "    }\n",
        "",
    )
    resets = ("            s_live_prefix_end = 0U;\n", "        s_live_prefix_end = 0U;\n")
    if source.count(resets[0]) != 2:
        raise ValueError("expected two nested live-prefix resets")
    source = source.replace(resets[0], "")
    return replace_once(source, resets[1], "")


def change_checkpoint_updates(source, interior, interior_bound, capacity):
    if not interior_bound:
        if interior:
            # A nonzero prefix is never attached to an internal checkpoint. A successful
            # allocation that resumed from an end sentinel may have changed the maximum.
            update = (
                "            if ((offset != checkpoint) && (prefix_max == 0U)) {\n"
                "                s_scan_offset = offset;\n"
                "                s_scan_prefix_max = 0U;\n"
                "            } else if (checkpoint == s_heap_total) {\n"
                "                s_scan_offset = 0U;\n"
                "            }\n"
            )
        elif capacity:
            update = "            s_scan_offset = 0U;\n"
        else:
            update = ""
        source = replace_once(
            source,
            "            if (offset != checkpoint) {\n"
            "                s_scan_offset = offset;\n"
            "                s_scan_prefix_max = prefix_max;\n"
            "            }\n",
            update,
        )
    if not capacity:
        source = replace_once(
            source,
            "    } else if ((checkpoint != offset) &&\n"
            "               ((checkpoint == 0U) || (prefix_max <= s_scan_prefix_max))) {\n"
            "        /* Keep a checkpoint that can still accelerate a successful allocation. */\n"
            "        s_scan_prefix_max = prefix_max;\n"
            "        s_scan_offset = offset;\n",
            "",
        )
        source = replace_once(
            source,
            "        } else if ((checkpoint == 0U) || (largest == prefix_max)) {\n"
            "            s_scan_prefix_max = largest;\n"
            "            s_scan_offset = offset;\n",
            "",
        )
    return source


def remove_checkpoint_storage(source, live):
    source = replace_once(source, "static uint32_t s_scan_offset;\n", "")
    source = replace_once(source, "static uint32_t s_scan_prefix_max;\n", "")
    source = replace_once(
        source,
        "    const uint32_t checkpoint = s_scan_offset;\n"
        "    uint32_t offset = checkpoint;\n"
        "    uint32_t prefix_max = (offset != 0U) ? s_scan_prefix_max : 0U;\n"
        + (
            "    if ((need <= prefix_max) || (offset < s_live_prefix_end)) {\n"
            "        offset = s_live_prefix_end;\n"
            "        prefix_max = 0U;\n"
            if live else
            "    if (need <= prefix_max) {\n"
            "        offset = 0U;\n"
            "        prefix_max = 0U;\n"
        ) + "    }\n",
        (
            "    uint32_t offset = s_live_prefix_end;\n"
            "    rtos_bool_t has_rejected_run = RTOS_FALSE;\n"
            if live else "    uint32_t offset = 0U;\n"
        ),
    )
    source = replace_once(
        source,
        "        if (run_size > prefix_max) {\n"
        "            prefix_max = run_size;\n"
        "        }\n",
        "        has_rejected_run = RTOS_TRUE;\n" if live else "",
    )
    if live:
        source = replace_once(source, "if (prefix_max == 0U)", "if (!has_rejected_run)")

    source = replace_once(
        source,
        "    if (offset < s_scan_offset) {\n"
        "        s_scan_offset = 0U;\n"
        "    }\n",
        "",
    )
    query_start = source.index("    const uint32_t checkpoint = s_scan_offset;",
                               source.index("uint32_t rtos_heap_get_max_alloc(void)"))
    query_end = source.index("\n    heap_unlock(mask);", query_start)
    query = source[query_start:query_end]
    first_line = query.index("        uint32_t run_size = 0U;")
    last_line = query.index("        /* Keep an interior checkpoint")
    loop = "\n".join(line[4:] if line else line
                     for line in query[first_line:last_line].splitlines())
    query = "    uint32_t largest = 0U;\n    uint32_t offset = 0U;\n" + loop + "\n"
    if live:
        query += (
            "    if (offset != s_heap_total) {\n"
            "        s_live_prefix_end = 0U;\n"
            "    }\n"
        )
    source = source[:query_start] + query + source[query_end:]
    source = replace_once(source, "            s_scan_offset = 0U;\n", "")
    if live:
        return replace_once(source, "        s_scan_offset = 0U;\n", "")
    return replace_once(
        source,
        "    if (offset != s_heap_total) {\n"
        "        s_scan_offset = 0U;\n"
        "    }\n",
        "",
    )


def generate(raw, mode):
    if mode == "current":
        return raw

    live, interior, interior_bound, capacity = MODES[mode]
    newline = "\r\n" if b"\r\n" in raw else "\n"
    source = raw.decode("utf-8").replace("\r\n", "\n")
    if not live:
        source = remove_live_prefix(source)
    source = change_checkpoint_updates(source, interior, interior_bound, capacity)
    if not interior and not capacity:
        source = remove_checkpoint_storage(source, live)
    source = f"/* Generated HIL heap ablation: {mode}. */\n" + source
    return source.replace("\n", newline).encode("utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=MODES)
    parser.add_argument("input", type=Path, help="production rtos_heap.c")
    parser.add_argument("output", type=Path, help="separate build-tree rtos_heap.c")
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error("input and output must refer to different files")
    try:
        source = generate(args.input.read_bytes(), args.mode)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(source)


if __name__ == "__main__":
    main()
