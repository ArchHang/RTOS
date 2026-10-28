#!/usr/bin/env python3
"""Keep marked handbook examples identical to the actual compiled C sources.

Default is read-only. --sync explicitly replaces marked snippets in the handbook.
Only application examples are checked; unmarked mechanism summaries/assembly are
not executable examples and are not claimed to have been compiled by this tool.
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
HANDBOOK = ROOT / "docs/rtos_design.md"
EXAMPLES = ROOT / "docs/ai/examples"
BINDING = re.compile(
    r"(?P<marker><!-- example-(?P<kind>function|file): (?P<name>[a-z0-9_.]+) -->)"
    r"(?:\n[ \t]*\n```c\n(?P<code>.*?)\n```)?", re.S
)


def sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest().upper()


def hide_comments_literals(text):
    pattern = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda match: re.sub(r"[^\n]", " ", match.group()),
                  text, flags=re.S)


def extract_function(text, name):
    hidden = hide_comments_literals(text)
    pattern = r"(?m)^[^\n;{}#]*\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{"
    matches = list(re.finditer(pattern, hidden))
    if not matches:
        return None
    if len(matches) != 1:
        raise ValueError("Ambiguous function definition: " + name)
    match = matches[0]
    opening = hidden.rfind("{", match.start(), match.end())
    depth = 1
    cursor = opening + 1
    while cursor < len(hidden) and depth:
        depth += (hidden[cursor] == "{") - (hidden[cursor] == "}")
        cursor += 1
    if depth:
        raise ValueError("Unclosed C function: " + name)
    return text[match.start():cursor].strip("\n")


def inspect(document, sources, sync=False):
    entries = []
    errors = []

    def replace(match):
        name, kind = match.group("name"), match.group("kind")
        if kind == "file":
            candidates = [(path, text.rstrip("\n")) for path, text in sources.items()
                          if path.name == name]
        else:
            candidates = [(path, body) for path, text in sources.items()
                          for body in [extract_function(text, name)] if body is not None]
        if len(candidates) != 1:
            errors.append("Missing/ambiguous example source: " + name)
            return match.group()
        path, expected = candidates[0]
        actual = match.group("code")
        if not sync and actual != expected:
            errors.append("Missing/stale example block: " + name)
        entries.append({"kind": kind, "name": name,
                        "source": path.relative_to(ROOT).as_posix(),
                        "source_sha256": sha(sources[path]), "snippet_sha256": sha(expected),
                        "handbook_line": document.count("\n", 0, match.start()) + 1})
        if sync:
            return match.group("marker") + "\n\n```c\n" + expected + "\n```"
        return match.group()

    updated = BINDING.sub(replace, document)
    if not entries:
        errors.append("Handbook has no marked application examples")
    return updated, entries, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sync", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    sources = {path: path.read_text(encoding="utf-8-sig")
               for path in sorted(EXAMPLES.glob("*.c"))}
    original = HANDBOOK.read_text(encoding="utf-8-sig")
    updated, entries, errors = inspect(original, sources, args.sync)
    if args.sync and not errors:
        HANDBOOK.write_bytes(updated.encode("utf-8"))
        _, entries, errors = inspect(updated, sources)
    result = {"status": "FAIL" if errors else "PASS", "errors": errors,
              "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT)
                      .decode().strip(),
              "finished_local": datetime.now().astimezone().isoformat(),
              "validator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest().upper(),
              "blocks": len(entries),
              "unique_functions": len({row["name"] for row in entries if row["kind"] == "function"}),
              "whole_files": len({row["name"] for row in entries if row["kind"] == "file"}),
              "handbook_sha256": sha(updated if args.sync and not errors else original),
              "entries": entries,
              "limits": ["Marked C snippets are compared with the actual example sources.",
                         "This is not a compiler, firmware link, execution or hardware test.",
                         "Unmarked mechanism summaries, structures and assembly are outside this check."]}
    if args.output:
        output = args.output.resolve()
        if ROOT not in output.parents:
            raise ValueError("Evidence must stay inside the project")
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("x", encoding="utf-8") as handle:
            json.dump(result, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
    print(json.dumps({key: result[key] for key in
                     ("status", "blocks", "unique_functions", "whole_files", "errors")},
                     ensure_ascii=True, indent=2))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
