"""Byte-check the original debugger experiment's explicitly loaded ELF sections.

GDB load writes sections, not ELF segment alignment padding. The dump is Flash
0x08000000..0x0800f990 from the archived original O0 image after its first run.
"""
import hashlib
import json
from pathlib import Path
import struct

HERE = Path(__file__).resolve().parent


def main():
    root = HERE / "evidence/preflight/original_stress_failure"
    elf_path = root / "firmware_channel_stress_basepri_a1_O0.elf"
    elf = elf_path.read_bytes()
    flash = (root / "original_flash_readback.bin").read_bytes()
    header = struct.unpack_from("<HHIIIIIHHHHHH", elf, 16)
    sections = [struct.unpack_from("<10I", elf, header[5] + i * header[10])
                for i in range(header[11])]
    programs = [struct.unpack_from("<8I", elf, header[4] + i * header[8])
                for i in range(header[9])]
    name_section = sections[header[12]]
    names = elf[name_section[4]:name_section[4] + name_section[5]]
    results = []
    for section in sections:
        if not (section[2] & 2) or section[1] == 8 or section[5] == 0:
            continue
        name = names[section[0]:].split(b"\0", 1)[0].decode()
        physical = None
        for program in programs:
            if program[0] == 1 and program[2] <= section[3] \
                    and section[3] + section[5] <= program[2] + program[4]:
                physical = program[3] + section[3] - program[2]
                break
        assert physical is not None and physical >= 0x08000000, name
        expected = elf[section[4]:section[4] + section[5]]
        actual = flash[physical - 0x08000000:physical - 0x08000000 + section[5]]
        results.append({"section": name, "physical_address": physical,
                        "bytes": len(expected), "match": actual == expected,
                        "expected_sha256": hashlib.sha256(expected).hexdigest(),
                        "readback_sha256": hashlib.sha256(actual).hexdigest()})
    report = {"elf_sha256": hashlib.sha256(elf).hexdigest(),
              "dump_sha256": hashlib.sha256(flash).hexdigest(),
              "all_loaded_sections_match": all(r["match"] for r in results),
              "sections": results,
              "note": "GDB compare-sections reported CRC mismatch. Direct byte readback is "
                      "equal in every loaded section; unloaded segment padding is excluded."}
    (root / "loaded_sections_verified.json").write_text(json.dumps(report, indent=2) + "\n")
    assert report["all_loaded_sections_match"]
    print(f"{len(results)} loaded sections: byte-for-byte PASS")


if __name__ == "__main__":
    main()
