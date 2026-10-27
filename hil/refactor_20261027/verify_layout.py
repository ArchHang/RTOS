"""Read-only structural equivalence check against the pre-refactor commit.

This checks source tokens, not runtime equivalence. Hardware regression and
compiler checks are recorded separately. No historical evidence is modified.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASELINE = "6602f8fa1b9acb9b12ba27e17f2ffc4fbcb8b03f"
TOKEN = re.compile(
    r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|'
    r'\s+|[A-Za-z_]\w*|[0-9][A-Za-z0-9_.]*|.', re.S
)
WRAPPERS = {"usb_ms_to_ticks", "usb_osal_ms_to_ticks"}
EXPECTED_DIRECTORIES = {
    "RTOS/Core/Src": {"rtos_kernel.c", "rtos_task.c", "rtos_sched.c", "rtos_wait.c"},
    "RTOS/IPC": {"rtos_event.c", "rtos_mutex.c", "rtos_queue.c", "rtos_sem.c"},
    "RTOS/Memory": {"rtos_heap.c", "rtos_mempool.c"},
    "RTOS/Services": {"rtos_perf.c", "rtos_timer.c"},
    "RTOS/USB": {"rtos_usb_device.c", "rtos_usb_msc.c", "rtos_usb_osal.c"},
    "RTOS/Port/GCC/ARM_CM4F_CM7F": {"port.c"},
}


def git(*arguments):
    return subprocess.check_output(["git", *arguments], cwd=ROOT)


def tokens(source):
    return [item for item in TOKEN.findall(source)
            if not item.isspace() and not item.startswith(("/*", "//"))]


def functions(source):
    """Extract top-level definitions, retaining #if branches inside bodies."""
    items = tokens(source)
    depth = 0
    begin = None
    result = []
    for index, item in enumerate(items):
        if item == "{":
            if depth == 0 and index > 0 and items[index - 1] == ")":
                nesting = 1
                cursor = index - 2
                while cursor >= 0 and nesting:
                    nesting += (items[cursor] == ")") - (items[cursor] == "(")
                    cursor -= 1
                if cursor >= 0 and re.fullmatch(r"[A-Za-z_]\w*", items[cursor]):
                    begin = (items[cursor], cursor)
            depth += 1
        elif item == "}":
            depth -= 1
            if depth == 0 and begin is not None:
                name, start = begin
                body = ["rtos_usb_timeout_ticks" if part in WRAPPERS else part
                        for part in items[start:index + 1]]
                result.append((name, body))
                begin = None
    if depth != 0:
        raise ValueError("Unbalanced top-level braces")
    return result


def top_level_declarations(source):
    """Collect storage/type/prototype declarations independently of file order.

    Preprocessor directives are excluded here; this check does not establish
    equivalence of their conditions. Function bodies are excluded, while array
    initializers, typedef fields, alignment attributes and assertions remain.
    The scanner targets this project's C subset rather than arbitrary C syntax.
    """
    without_directives = re.sub(
        r"^[ \t]*#(?:[^\n\\]|\\\n)*\n", "\n", source, flags=re.M
    )
    items = tokens(without_directives)
    result = []
    depth = 0
    start = 0
    is_function = False
    for index, item in enumerate(items):
        if item == "{":
            if depth == 0:
                # An initializer ending with a cast/call is still a declaration.
                is_function = (index > 0 and items[index - 1] == ")" and
                               "=" not in items[start:index])
            depth += 1
        elif item == "}":
            depth -= 1
            if depth == 0 and is_function:
                start = index + 1
                is_function = False
        elif item == ";" and depth == 0:
            declaration = tuple(items[start:index + 1])
            start = index + 1
            if declaration:
                result.append(declaration)
    if depth != 0:
        raise ValueError("Unbalanced top-level declaration braces")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    tracked = git("ls-tree", "-r", "--name-only", BASELINE).decode().splitlines()
    old_files = [name for name in tracked
                 if name.startswith(("RTOS/Core/Src/", "RTOS/Port/")) and name.endswith(".c")]
    current_files = sorted(path for directory in EXPECTED_DIRECTORIES
                           for path in (ROOT / directory).glob("*.c"))
    errors = []
    old_functions = collections.defaultdict(list)
    new_functions = collections.defaultdict(list)
    old_declarations = collections.Counter()
    new_declarations = collections.Counter()
    definitions = []
    for name in old_files:
        source = git("show", f"{BASELINE}:{name}").decode("utf-8-sig")
        old_declarations.update(top_level_declarations(source))
        for function, body in functions(source):
            if function not in WRAPPERS:
                old_functions[function].append(body)
    for path in current_files:
        source = path.read_text(encoding="utf-8-sig")
        new_declarations.update(top_level_declarations(source))
        for function, body in functions(source):
            new_functions[function].append(body)
            definitions.append({"name": function, "path": path.relative_to(ROOT).as_posix()})
    for name in sorted(old_functions.keys() | new_functions.keys()):
        before = sorted(old_functions[name])
        after = sorted(new_functions[name])
        if before != after:
            errors.append(f"Function tokens differ: {name}")
    if len(definitions) < 200:
        errors.append("Unexpectedly small function coverage; review parser")

    # The sole declaration removed from C is now centralized in the private
    # header. All remaining declarations must retain their exact token counts.
    relocated_prototype = tuple(tokens("extern uint32_t rtos_internal_alloc_task_id(void);"))
    declaration_baseline_count = sum(old_declarations.values())
    if old_declarations[relocated_prototype] != 1 or new_declarations[relocated_prototype] != 0:
        errors.append("Unexpected allocation-ID prototype relocation")
    old_declarations.subtract({relocated_prototype: 1})
    for label, difference in (("Removed", old_declarations - new_declarations),
                              ("Added", new_declarations - old_declarations)):
        for declaration, count in difference.items():
            errors.append(f"{label} top-level declaration ({count}): " + " ".join(declaration))

    # Public header declarations, macros and layouts must remain token-identical.
    headers = [name for name in tracked if name.startswith("RTOS/Core/Inc/")
               and name.endswith(".h") and not name.endswith("/rtos_internal.h")]
    for name in headers:
        before = git("show", f"{BASELINE}:{name}").decode("utf-8-sig")
        if tokens(before) != tokens((ROOT / name).read_text(encoding="utf-8-sig")):
            errors.append(f"Public header tokens differ: {name}")
    untouched = [name for name in tracked if name.startswith("RTOS/lib/third/")]
    untouched += ["Core/Src/main.c"]
    for name in untouched:
        before = git("show", f"{BASELINE}:{name}")
        after = (ROOT / name).read_bytes()
        # Git's configured checkout conversion may use CRLF for production C.
        if before.replace(b"\r\n", b"\n") != after.replace(b"\r\n", b"\n"):
            errors.append(f"Protected source changed: {name}")
    for directory, expected in EXPECTED_DIRECTORIES.items():
        actual = {path.name for path in (ROOT / directory).glob("*.c")}
        if actual != expected:
            errors.append(f"Unexpected source inventory: {directory}: {actual}")
    if not (ROOT / "RTOS/Core/Private/rtos_internal.h").is_file():
        errors.append("Missing private kernel header")
    else:
        private_tokens = tokens((ROOT / "RTOS/Core/Private/rtos_internal.h").read_text(
            encoding="utf-8-sig"))
        expected = list(relocated_prototype[1:])  # A header declaration needs no explicit extern.
        matches = sum(private_tokens[index:index + len(expected)] == expected
                      for index in range(len(private_tokens) - len(expected) + 1))
        if matches != 1:
            errors.append("Private header must declare allocation-ID interface exactly once")
    if (ROOT / "RTOS/Core/Inc/rtos_internal.h").exists():
        errors.append("Private header still present in public include directory")
    object_lines = (ROOT / "hil/templates/objects.list.template").read_text().splitlines()
    objects = [line.strip().strip('"').removeprefix("./") for line in object_lines if line.strip()]
    counts = collections.Counter(objects)
    for source in current_files:
        object_name = source.relative_to(ROOT).with_suffix(".o").as_posix()
        if counts[object_name] != 1:
            errors.append(f"Source must link exactly once: {object_name}")
    for name, count in counts.items():
        if count != 1:
            errors.append(f"Duplicate link input: {name}")
        if name.endswith(".o"):
            stem = (ROOT / name).with_suffix("")
            if not any(stem.with_suffix(suffix).is_file() for suffix in (".c", ".s", ".S")):
                errors.append(f"Stale link input: {name}")
    report = {
        "status": "PASS" if not errors else "FAIL", "baseline": BASELINE,
        "production_translation_units": len(current_files),
        "function_definitions_compared": len(definitions),
        "top_level_declarations_before": declaration_baseline_count,
        "top_level_declarations_compared": sum(new_declarations.values()),
        "internal_prototypes_relocated": 1,
        "public_headers_compared": len(headers), "protected_files_compared": len(untouched),
        "allowed_change": "Two identical USB millisecond wrappers replaced by existing inline",
        "limitations": "Token comparison does not prove preprocessor, runtime or timing equivalence",
        "errors": errors, "definitions": definitions,
        "main_sha256": hashlib.sha256((ROOT / "Core/Src/main.c").read_bytes()).hexdigest(),
    }
    if args.output:
        if args.output.exists():
            raise SystemExit(f"Refusing to overwrite {args.output}")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "definitions"},
                     ensure_ascii=False, indent=2))
    raise SystemExit(bool(errors))


if __name__ == "__main__":
    main()
