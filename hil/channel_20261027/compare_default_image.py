"""Compare ELF32 allocated contents/addresses and load mappings, ignoring DWARF."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def image(path):
    data = path.read_bytes()
    assert data[:6] == b"\x7fELF\x01\x01", "ELF32 little endian required"
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    sections = [struct.unpack_from("<10I", data, header[5] + i * header[10])
                for i in range(header[11])]
    names = sections[header[12]]
    strings = data[names[4]:names[4] + names[5]]
    allocated = {}
    for section in sections:
        name = strings[section[0]:].split(b"\0", 1)[0].decode()
        if section[2] & 2:  # SHF_ALLOC, including zero-filled SHT_NOBITS
            content = None if section[1] == 8 else hashlib.sha256(
                data[section[4]:section[4] + section[5]]).hexdigest()
            allocated[name] = {"type": section[1], "flags": section[2],
                               "address": section[3], "bytes": section[5], "sha256": content}
    mappings = []
    for index in range(header[9]):
        segment = struct.unpack_from("<8I", data, header[4] + index * header[8])
        if segment[0] == 1:
            mappings.append({"virtual": segment[2], "physical": segment[3],
                             "filesz": segment[4], "memsz": segment[5],
                             "flags": segment[6], "alignment": segment[7]})
    return {"file_sha256": hashlib.sha256(data).hexdigest(), "entry": header[3],
            "allocated": allocated, "load_mappings": mappings}


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    before, after = image(args.before), image(args.after)
    identical = all(before[k] == after[k] for k in ("entry", "allocated", "load_mappings"))
    result = {"allocated_image_identical": identical, "before": before, "after": after,
              "scope": "ELF32 load mappings, entry, SHF_ALLOC section bytes/size/address; DWARF ignored"}
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"allocated_image_identical": identical}))
    if not identical:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
