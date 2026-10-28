#!/usr/bin/env python3
"""Validate the baseline AI manuals without compiling or accessing hardware.

The scanner inventories every conditional callable declaration, header inline,
and function-like macro in Core/Inc. It checks coverage and declaration tokens,
not the semantic correctness of natural-language contracts. Those require the
separate source review recorded in the manuals.
"""
import argparse
import copy
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlparse


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BASELINE = "87bfbe4a6efc50f9b828128c713a1b5af2900ef8"
METADATA = tuple(ROOT / "docs/ai" / ("metadata_" + name + ".json")
                 for name in ("kernel", "ipc_memory", "services_usb"))
INDEX = ROOT / "docs/ai/api_index.json"
# Retired experiment runners were removed with their complete historical batches.
# Active firmware, canonical build inputs and retained HIL tools remain protected.
RETIRED_HIL_BATCHES = (
    "hil/archive_docs/", "hil/archive_tests/", "hil/audit_20261012/",
    "hil/perf_20261012/", "hil/lowlevel_20261012/", "hil/production_20261012/",
)
RETIRED_HIL_TOOLS = {
    "hil/basepri_20261025/run_matrix.ps1",
    "hil/basepri_20261025/compile_checks.ps1",
    "hil/basepri_20261025/run_regressions.ps1",
    "hil/adversarial_20261025/run_matrix.ps1",
    "hil/adversarial_20261025/compile_checks.ps1",
    "hil/adversarial_20261025/finalize_batch.ps1",
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def read(path):
    return path.read_text(encoding="utf-8-sig")


def git(*arguments):
    return subprocess.check_output(["git", *arguments], cwd=str(ROOT))


def tokens(text):
    text = re.sub(r"\\\r?\n", " ", text)
    return re.findall(r"[A-Za-z_]\w*|\d+|\.\.\.|[^\s]", strip_comments(text))


def token_sequence(needle, haystack):
    return any(haystack[index:index + len(needle)] == needle
               for index in range(len(haystack) - len(needle) + 1))


def strip_comments(text):
    pattern = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*'

    def replace(match):
        value = match.group(0)
        if value.startswith(("/*", "//")):
            return re.sub(r"[^\n]", " ", value)
        return value

    return re.sub(pattern, replace, text, flags=re.S)


def balanced_end(text, start, opening="(", closing=")"):
    depth = 0
    quote = None
    escaped = False
    for index in range(start, len(text)):
        char = text[index]
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            continue
        if char in ("\"", "'"):
            quote = char
        elif char == opening:
            depth += 1
        elif char == closing:
            depth -= 1
            if depth == 0:
                return index
    raise ValueError("Unbalanced source delimiter at offset " + str(start))


def function_form(fragment, inline=False):
    value = fragment.strip()
    if not value or re.match(r"^typedef\b", value):
        return None
    opening = value.find("(")
    if opening < 0:
        return None
    prefix = value[:opening].strip()
    match = re.fullmatch(r"(?P<return>[A-Za-z_][\w\s*]*?)\b(?P<name>[A-Za-z_]\w*)", prefix)
    if not match or "=" in prefix:
        return None
    end = balanced_end(value, opening)
    tail = value[end + 1:].strip().rstrip(";").strip()
    if tail and not re.fullmatch(r"(?:__attribute__\s*\(.*\)\s*)+", tail, flags=re.S):
        return None
    signature = " ".join(value.rstrip(";").split())
    if not inline:
        signature += ";"
    return {"name": match.group("name"), "kind": "inline" if inline else "function",
            "signature": signature, "parameters": value[opening + 1:end]}


def variable_form(fragment):
    value = fragment.strip()
    match = re.fullmatch(r"extern\s+([A-Za-z_][\w\s*]*?)\b([A-Za-z_]\w*)\s*;", value)
    if match is None:
        return None
    return {"name": match.group(2), "kind": "variable",
            "signature": " ".join(value.split()), "parameters": ""}


def signature_matches(provided, form):
    expected = tokens(form["signature"])
    actual = tokens(provided)
    if form["kind"] == "inline":
        return actual[:-1] == expected if actual and actual[-1] == ";" else actual == expected
    if form["kind"] == "macro" and not provided.lstrip().startswith("#"):
        return actual == tokens(form["name"] + "(" + form["parameters"] + ")")
    return actual == expected


def scan_header(path):
    text = strip_comments(read(path))
    logical = re.sub(r"\\\r?\n", " ", text)
    forms = []
    for match in re.finditer(r"(?m)^\s*#\s*define[ \t]+([A-Za-z_]\w*)\(([^)]*)\)([^\n]*)", logical):
        name, parameters, replacement = match.groups()
        forms.append({"name": name, "kind": "macro",
                      "signature": "#define " + name + "(" + parameters.strip() + ") " + replacement.strip(),
                      "parameters": parameters.strip()})
    # Blank complete logical preprocessor directives, preserving line counts.
    physical = text.splitlines(keepends=True)
    continued = False
    cleaned = []
    for line in physical:
        directive = continued or bool(re.match(r"\s*#", line))
        continued = directive and line.rstrip("\r\n").endswith("\\")
        cleaned.append(re.sub(r"[^\r\n]", " ", line) if directive else line)
    source = "".join(cleaned)
    source = re.sub(r'extern\s+"C"\s*\{', lambda m: " " * len(m.group(0)), source)
    start = 0
    index = 0
    unparsed = []
    while index < len(source):
        char = source[index]
        if char == ";":
            fragment = source[start:index + 1]
            form = function_form(fragment) or variable_form(fragment)
            if form:
                forms.append(form)
            elif "(" in fragment and not re.match(r"\s*(typedef|struct|enum|union)\b", fragment):
                # Initializers and arrays are not callable declarations.
                if "=" not in fragment and "[" not in fragment:
                    unparsed.append(" ".join(fragment.split())[:240])
            start = index + 1
        elif char == "{":
            fragment = source[start:index]
            form = function_form(fragment, inline=True)
            end = balanced_end(source, index, "{", "}")
            if form:
                forms.append(form)
                start = end + 1
            index = end
        elif char == "}":
            # The closing extern-C wrapper is not a declaration.
            start = index + 1
        index += 1
    unique = {}
    for form in forms:
        key = (form["name"], form["kind"], tuple(tokens(form["signature"])))
        unique[key] = form
    return list(unique.values()), unparsed


def anchors(text):
    result = set(re.findall(r'<a\s+(?:id|name)=["\']([^"\']+)["\']\s*>', text, flags=re.I))
    counts = {}
    fenced = False
    for line in text.splitlines():
        if re.match(r"\s*(```|~~~)", line):
            fenced = not fenced
            continue
        match = re.match(r"^ {0,3}#{1,6}\s+(.+?)\s*#*\s*$", line)
        if fenced or not match:
            continue
        label = re.sub(r"<[^>]*>", "", match.group(1)).lower()
        label = "".join(char for char in label if char.isalnum() or char in "_- ")
        slug = label.replace(" ", "-")
        index = counts.get(slug, 0)
        counts[slug] = index + 1
        result.add(slug if index == 0 else slug + "-" + str(index))
    return result


def section(text, anchor):
    marker = re.search(r'<a\s+(?:id|name)=["\']' + re.escape(anchor) + r'["\']\s*>', text)
    if marker is None:
        return ""
    tail = text[marker.end():]
    following = re.search(r'<a\s+(?:id|name)=["\']api-', tail)
    return tail if following is None else tail[:following.start()]


def parameter_names(parameters):
    names = []
    for item in parameters.split(","):
        item = item.strip()
        if item in ("", "void", "..."):
            continue
        pointer = re.search(r"\(\s*\*\s*(\w+)", item)
        words = re.findall(r"[A-Za-z_]\w*", item)
        if pointer:
            names.append(pointer.group(1))
        elif len(words) > 1 or (len(words) == 1 and " " not in item):
            names.append(words[-1])
    return names


def load_records():
    records = []
    documents = {}
    for metadata in METADATA:
        value = json.loads(read(metadata))
        if value.get("baseline") != BASELINE:
            raise ValueError("Metadata baseline differs: " + str(metadata))
        entries = value.get("symbols", value.get("records"))
        if not isinstance(entries, list):
            raise ValueError("Metadata has no record list: " + str(metadata))
        document = value.get("document")
        if not document:
            document = "docs/ai/contracts_" + metadata.stem[len("metadata_"):] + ".md"
        documents[document] = read(ROOT / document)
        for original in entries:
            row = dict(original)
            row["document"] = original.get("document", document)
            row["metadata"] = metadata.relative_to(ROOT).as_posix()
            row["anchor"] = row.get("anchor", "api-" + row["name"]).lstrip("#")
            records.append(row)
    return records, documents


def audit_records(records, documents, catalog, primary_keys, anchor_override=None):
    errors = []
    seen = set()
    parameter_checks = []
    for row in records:
        key = (row.get("header"), row.get("name"))
        if key in seen:
            errors.append("Duplicate contract: " + repr(key))
        seen.add(key)
        if key not in catalog:
            errors.append("Contract symbol absent from declared source: " + repr(key))
            continue
        if not row.get("classification"):
            errors.append("Missing usage classification: " + row["name"])
        implementation = ROOT / row.get("implementation", "")
        if not implementation.is_file():
            errors.append("Missing implementation path: " + row["name"])
        forms = catalog[key]
        function_forms = [form for form in forms if form["kind"] == "function"]
        inline_forms = [form for form in forms if form["kind"] == "inline"]
        primary = function_forms or inline_forms or forms
        provided = row.get("signature", row.get("prototype", ""))
        if not any(signature_matches(provided, form) for form in primary):
            errors.append("Signature differs from primary source declaration: " + row["name"])
        for variant in row.get("variants", []):
            signature = variant.get("signature", "") if isinstance(variant, dict) else variant
            if not any(signature_matches(signature, form) for form in forms):
                errors.append("Documented conditional form differs from source: " + row["name"])
        document = row["document"]
        if document not in documents:
            errors.append("Missing contract document: " + document)
            continue
        available = anchors(documents[document])
        if anchor_override and document in anchor_override:
            available = anchor_override[document]
        if row["anchor"] not in available:
            errors.append("Missing API anchor: " + row["name"])
        contract_section = section(documents[document], row["anchor"])
        fence_pattern = r"(?P<mark>```|~~~)[^\n]*\n(?P<body>.*?)(?P=mark)"
        blocks = [match.group("body") for match in re.finditer(fence_pattern, contract_section, flags=re.S)]
        if not any(token_sequence(tokens(provided), tokens(block)) for block in blocks):
            errors.append("Markdown prototype differs or is missing: " + row["name"])
        body = re.sub(fence_pattern, "", contract_section, flags=re.S)
        primary_form = primary[0]
        parameters = parameter_names(primary_form["parameters"])
        missing = [name for name in parameters if not re.search(
            r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])", body)]
        parameter_checks.append({"name": row["name"], "header": row["header"],
                                 "parameters": parameters, "not_found_in_prose": missing})
    for key in sorted(primary_keys - seen):
        errors.append("Missing callable contract: " + repr(key))
    return errors, parameter_checks


def negative_controls(records, documents, catalog, primary_keys):
    target = next(row for row in records if row["name"] == "rtos_queue_send")
    results = []
    missing = [row for row in records if row is not target]
    wrong = copy.deepcopy(records)
    next(row for row in wrong if row["name"] == target["name"])["signature"] += " bogus"
    absent_anchor = anchors(documents[target["document"]]) - {target["anchor"]}
    baseline_errors, _ = audit_records(records, documents, catalog, primary_keys)
    controls = (("missing_record", missing, None, "Missing callable contract"),
                ("wrong_signature", wrong, None, "Signature differs"),
                ("missing_anchor", records, {target["document"]: absent_anchor}, "Missing API anchor"))
    for name, variant, override, expected in controls:
        if baseline_errors:
            results.append({"case": name, "status": "NOT_RUN",
                            "reason": "Valid baseline contracts are required before negative controls"})
            continue
        errors, _ = audit_records(variant, documents, catalog, primary_keys, override)
        rejected = bool(errors) and any(expected in error and target["name"] in error for error in errors)
        rejected = rejected and all(target["name"] in error for error in errors)
        results.append({"case": name, "status": "PASS" if rejected else "FAIL",
                        "scope": "in-memory documentation-validator negative control",
                        "expected_rejections": errors})
    return results


def markdown_files():
    paths = set((ROOT / "docs").rglob("*.md")) | set((ROOT / "hil").rglob("*.md"))
    paths.update(ROOT / name for name in ("AGENTS.md", "README.md", "CONTRIBUTING.md",
                                         "RTOS/README.md"))
    return sorted(path for path in paths if path.is_file())


def check_links(paths):
    errors = []
    local = 0
    external = 0
    cache = {}
    for path in paths:
        fenced = False
        for line_number, line in enumerate(read(path).splitlines(), 1):
            if re.match(r"\s*(```|~~~)", line):
                fenced = not fenced
                continue
            if fenced:
                continue
            for match in re.finditer(r"!?\[[^\]]*\]\((?:<([^>]+)>|([^\s)]+))(?:\s+[^)]*)?\)", line):
                target = match.group(1) or match.group(2)
                parsed = urlparse(target)
                if parsed.scheme or target.startswith("//"):
                    external += 1
                    continue
                local += 1
                name = unquote(parsed.path)
                destination = ((ROOT / name.lstrip("/")) if name.startswith("/") else path.parent / name).resolve() if name else path
                reason = None
                if not destination.exists():
                    reason = "Missing local target"
                elif parsed.fragment and destination.suffix.lower() == ".md":
                    if destination not in cache:
                        cache[destination] = anchors(read(destination))
                    if unquote(parsed.fragment) not in cache[destination]:
                        reason = "Missing Markdown anchor"
                if reason:
                    errors.append({"source": path.relative_to(ROOT).as_posix(),
                                   "line": line_number, "target": target, "reason": reason})
    master = ROOT / "docs/rtos_design.md"
    previous = anchors(git("show", BASELINE + ":docs/rtos_design.md").decode("utf-8-sig"))
    missing = sorted(previous - anchors(read(master)))
    return {"documents": len(paths), "local_links": local, "external_links_not_fetched": external,
            "errors": errors, "legacy_master_anchors": len(previous), "missing_legacy_anchors": missing}


def protected(path):
    if path.startswith(RETIRED_HIL_BATCHES) or path in RETIRED_HIL_TOOLS:
        return False
    if path == "hil/templates/subdir.mk.last_variant":
        # restore_project writes this ignored diagnostic copy; no canonical
        # source/build inventory consumes it.
        return False
    suffix = Path(path).suffix.lower()
    if path.startswith(("RTOS/", "Core/", "Drivers/", "AI/")) and suffix in (".c", ".h", ".s", ".a", ".ld"):
        return True
    if path.startswith("hil/fw/") and suffix in (".c", ".h", ".s"):
        return True
    if path.startswith("hil/templates/"):
        return True
    if path.startswith("hil/") and suffix in (".py", ".ps1") and not path.startswith("hil/ai_docs/"):
        return True
    return path in (".project", ".cproject") or ("/" not in path and suffix in (".ld", ".ioc"))


def check_production():
    entries = git("ls-tree", "-r", "-z", BASELINE).split(b"\0")
    references = []
    for entry in entries:
        if not entry:
            continue
        metadata, name = entry.split(b"\t", 1)
        relative = name.decode("utf-8")
        if protected(relative):
            references.append((relative, metadata.split()[2]))
    process = subprocess.Popen(["git", "cat-file", "--batch"], cwd=str(ROOT),
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    errors = []
    identities = {}
    try:
        for relative, identity in references:
            process.stdin.write(identity + b"\n")
            process.stdin.flush()
            header = process.stdout.readline().split()
            size = int(header[2])
            original = process.stdout.read(size)
            if len(original) != size or process.stdout.read(1) != b"\n":
                raise ValueError("Truncated Git blob stream")
            path = ROOT / relative
            expected = hashlib.sha256(original).hexdigest().upper()
            identities[relative] = expected
            if not path.is_file() or sha(path) != expected:
                errors.append(relative)
    finally:
        process.stdin.close()
        process.wait()
    if process.returncode:
        raise ValueError("Git blob reader failed")
    # Existing files must be unchanged; new firmware/build files are also forbidden.
    actual = set()
    for directory in ("RTOS", "Core", "Drivers", "AI", "hil"):
        actual.update(path.relative_to(ROOT).as_posix() for path in (ROOT / directory).rglob("*")
                      if path.is_file() and protected(path.relative_to(ROOT).as_posix()))
    extra = sorted(actual - set(identities))
    retired_present = sorted(path for path in (*RETIRED_HIL_BATCHES, *RETIRED_HIL_TOOLS)
                             if (ROOT / path).exists())
    return {"status": "PASS" if not errors and not extra and not retired_present else "FAIL",
            "protected_files": len(references), "changed_or_missing": errors, "new_production_files": extra,
            "excluded_generated_paths": ["hil/templates/subdir.mk.last_variant"],
            "excluded_retired_hil_batches": list(RETIRED_HIL_BATCHES),
            "excluded_retired_hil_tools": sorted(RETIRED_HIL_TOOLS),
            "retired_hil_paths_still_present": retired_present,
            "sha256": identities}


def make_index(records, catalog, primary_keys):
    entries = []
    for row in sorted(records, key=lambda value: (value["header"], value["name"])):
        item = dict(row)
        item["scope"] = "primary-header" if (row["header"], row["name"]) in primary_keys else "supplemental-bridge"
        item["source_forms"] = catalog[(row["header"], row["name"])]
        entries.append(item)
    return {"schema_version": 1, "baseline": BASELINE,
            "primary_scope": "all callable declarations/header inlines/function-like macros and declared external variables in RTOS/Core/Inc/*.h; all conditional branches scanned",
            "supplemental_scope": "documented CherryUSB bridge/callback symbols only; not the complete third-party API",
            "semantic_review": "manual source review is required; declaration matching does not prove contract semantics",
            "entries": entries}


def inspect(write_index):
    watched = set(markdown_files()) | set(METADATA) | {Path(__file__).resolve()}
    before = {path.relative_to(ROOT).as_posix(): sha(path) for path in sorted(watched)}
    records, documents = load_records()
    headers = sorted((ROOT / "RTOS/Core/Inc").glob("*.h"))
    primary_headers = {path.relative_to(ROOT).as_posix() for path in headers}
    headers.extend(ROOT / row["header"] for row in records if row["header"] not in primary_headers)
    catalog = {}
    unparsed = []
    for path in sorted(set(headers)):
        forms, leftovers = scan_header(path)
        relative = path.relative_to(ROOT).as_posix()
        unparsed.extend({"header": relative, "declaration": text} for text in leftovers)
        for form in forms:
            catalog.setdefault((relative, form["name"]), []).append(form)
    primary_keys = {key for key in catalog if key[0] in primary_headers}
    errors, parameters = audit_records(records, documents, catalog, primary_keys)
    if unparsed:
        errors.append("Unparsed callable-like declarations require review")
    index = make_index(records, catalog, primary_keys) if not errors else None
    if index is not None and write_index:
        INDEX.write_bytes((json.dumps(index, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    elif index is not None:
        if not INDEX.is_file() or json.loads(read(INDEX)) != index:
            errors.append("Generated api_index.json is missing/stale; use --write-index explicitly")
    controls = negative_controls(records, documents, catalog, primary_keys)
    links = check_links(markdown_files())
    production = check_production()
    identities = {path.relative_to(ROOT).as_posix(): sha(path)
                  for path in sorted(set(markdown_files()) | set(METADATA) | {Path(__file__).resolve()})}
    changed = sorted(name for name in set(before) | set(identities)
                     if before.get(name) != identities.get(name))
    changed_production = [name for name, identity in production["sha256"].items()
                          if not (ROOT / name).is_file() or sha(ROOT / name) != identity]
    passed = not errors and not changed and not changed_production and not links["errors"] and not links["missing_legacy_anchors"] and production["status"] == "PASS" and all(row["status"] == "PASS" for row in controls)
    return {"status": "PASS" if passed else "FAIL", "baseline": BASELINE,
            "current_head": git("rev-parse", "HEAD").decode().strip(),
            "primary_headers": len(primary_headers),
            "primary_callables": sum(any(form["kind"] != "variable" for form in catalog[key]) for key in primary_keys),
            "primary_variables": sum(all(form["kind"] == "variable" for form in catalog[key]) for key in primary_keys),
            "documented_records": len(records), "supplemental_records": len(records) - len(primary_keys),
            "contract_errors": errors, "unparsed_declarations": unparsed,
            "parameter_name_observations": parameters,
            "parameter_observation_limit": "Lexical parameter-name presence in prose is reported, not used to infer semantic completeness or correctness.",
            "negative_controls": controls, "markdown": links, "production": production,
            "document_identity_stable": not changed, "changed_documents": changed,
            "production_identity_stable": not changed_production,
            "changed_during_production_check": changed_production,
            "document_sha256": identities,
            "limits": ["No compiler, firmware link, serial, debug probe or board access.",
                       "Source declaration coverage is independent of the curated metadata.",
                       "This checker does not prove natural-language semantics, all bugs, or runtime behavior."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write-index", action="store_true")
    parser.add_argument("--out-dir", type=Path)
    arguments = parser.parse_args()
    directory = arguments.out_dir or HERE / "evidence" / ("docs_" + datetime.now().strftime("%Y%m%d_%H%M%S_%f"))
    directory = directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    try:
        result = inspect(arguments.write_index)
    except (OSError, ValueError, KeyError, TypeError, IndexError, subprocess.SubprocessError) as error:
        result = {"status": "FAIL", "reason": str(error), "scope": "documentation validation; no hardware"}
    result["finished_local"] = datetime.now().astimezone().isoformat()
    result["python"] = {"version": sys.version, "executable": sys.executable}
    (directory / "summary.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    compact = {key: result.get(key) for key in ("status", "reason", "primary_headers", "primary_callables", "documented_records", "supplemental_records", "contract_errors", "unparsed_declarations")}
    compact["output"] = str(directory)
    print(json.dumps(compact, ensure_ascii=True, indent=2))
    raise SystemExit(0 if result["status"] == "PASS" else 1)


if __name__ == "__main__":
    main()
