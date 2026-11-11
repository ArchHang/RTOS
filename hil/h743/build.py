"""Build an isolated STM32H743 RTOS test image using GNU Arm Embedded GCC."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess


HIL = Path(__file__).resolve().parent
ROOT = HIL.parents[1]
TOOL_ROOT = Path("E:/ST/STM32CubeIDE_1.13.2/STM32CubeIDE/plugins")
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
RTOS_DIRS = ("Core/Src", "IPC", "Memory", "Services", "Port/GCC/ARM_CM4F_CM7F")
HAL_DRIVERS = ("", "_cortex", "_dma", "_flash", "_gpio", "_pwr", "_pwr_ex", "_rcc",
               "_rcc_ex", "_uart", "_uart_ex")


def identifier(value):
    if not IDENTIFIER.fullmatch(value):
        raise argparse.ArgumentTypeError(f"Invalid C identifier {value!r}")
    return value


def definition(value):
    identifier(value.partition("=")[0])
    if "\n" in value or "\r" in value:
        raise argparse.ArgumentTypeError("A definition must be a single line")
    return value


def priority(value):
    number = int(value)
    if not 0 <= number < 31:
        raise argparse.ArgumentTypeError("Entry priority must be in 0..30")
    return number


def compiler_path(requested):
    if requested:
        return requested.expanduser().resolve(strict=True)
    if os.environ.get("ARM_NONE_EABI_GCC"):
        return Path(os.environ["ARM_NONE_EABI_GCC"]).expanduser().resolve(strict=True)
    for pattern in ("*gnu-tools-for-stm32.11*/tools/bin/arm-none-eabi-gcc.exe",
                    "*gnu-tools-for-stm32.*/tools/bin/arm-none-eabi-gcc.exe"):
        candidates = sorted(TOOL_ROOT.glob(pattern))
        if candidates:
            return candidates[-1]
    found = shutil.which("arm-none-eabi-gcc")
    if found:
        return Path(found).resolve(strict=True)
    raise ValueError("Provide --compiler pointing to arm-none-eabi-gcc")


def run(arguments, log):
    command = [str(item) for item in arguments]
    completed = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding="utf-8", errors="replace", check=False)
    log.write(json.dumps(command, ensure_ascii=False) + "\n" + completed.stdout)
    if completed.returncode:
        log.flush()
        raise RuntimeError(f"Command failed ({completed.returncode}): {command!r}\n{completed.stdout}")
    return completed.stdout


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def generate_startup(source, output, vector):
    code = source.read_text(encoding="utf-8")
    if vector == "direct":
        for label, target in (("SVC_Handler", "rtos_port_svc_handler"),
                              ("PendSV_Handler", "rtos_port_pend_sv_handler")):
            code, count = re.subn(rf"(\.word[ \t]+){label}\b", rf"\g<1>{target}", code)
            if count != 1:
                raise ValueError(f"Unexpected BSP vector format for {label}: found {count}")
    output.write_text(code, encoding="utf-8")


def check_vectors(image, symbols, vector):
    raw = image.read_bytes()
    if not 64 <= len(raw) <= 128 * 1024:
        raise ValueError(f"Flash image length outside first sector: {len(raw)}")
    resolved = {name: int(address, 16) for address, name in
                re.findall(r"^([0-9a-fA-F]+)\s+\w\s+(\w+)$", symbols, re.MULTILINE)}
    names = {1: "Reset_Handler", 15: "SysTick_Handler"}
    names[11] = "rtos_port_svc_handler" if vector == "direct" else "SVC_Handler"
    names[14] = "rtos_port_pend_sv_handler" if vector == "direct" else "PendSV_Handler"
    vectors = struct.unpack_from("<16I", raw)
    if vectors[0] != 0x20020000:
        raise ValueError(f"Unexpected MSP: {vectors[0]:08x}")
    for index, name in names.items():
        if name not in resolved or vectors[index] != resolved[name] | 1:
            raise ValueError(f"Vector {index} does not resolve to Thumb symbol {name}")
    return {"MSP": hex(vectors[0]), **{name: hex(vectors[index]) for index, name in names.items()}}


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--rtos-root", type=Path, default=ROOT / "RTOS")
    bsp = parser.add_mutually_exclusive_group()
    bsp.add_argument("--bsp-root", type=Path,
                     help="historical h743_microarch_lab BSP (default)")
    bsp.add_argument("--cubeh7-root", type=Path,
                     help="official STM32CubeH7 package with CMSIS and HAL drivers")
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--entry-source", type=Path, default=HIL / "h743_bench.c")
    parser.add_argument("--entry-name", type=identifier, default="h743_bench_run")
    parser.add_argument("--entry-priority", type=priority, default=9)
    parser.add_argument("--extra-source", type=Path, action="append", default=[])
    parser.add_argument("--define", type=definition, action="append", default=[])
    parser.add_argument("--wrap", type=identifier, action="append", default=[])
    parser.add_argument("--opt", choices=("O0", "O2", "O3"), default="O2")
    parser.add_argument("--critical-mask", choices=("basepri", "primask"), default="basepri")
    parser.add_argument("--disable-assertions", action="store_true")
    parser.add_argument("--data-memory", choices=("axi", "dtcm"), default="axi")
    parser.add_argument("--icache", choices=("on", "off"), default="on")
    parser.add_argument("--dcache", choices=("on", "off"), default="on")
    parser.add_argument("--vector", choices=("direct", "wrapper"), default="direct")
    args = parser.parse_args()
    if args.bsp_root is None and args.cubeh7_root is None:
        args.bsp_root = ROOT.parent / "h743_microarch_lab"
    return args


def board_inputs(args):
    if args.cubeh7_root is not None:
        package = args.cubeh7_root.resolve(strict=True) / "Drivers"
        cmsis = package / "CMSIS"
        device = cmsis / "Device/ST/STM32H7xx"
        templates = device / "Source/Templates"
        hal = package / "STM32H7xx_HAL_Driver"
        glue = HIL / "cubeh7"
        startup = templates / "gcc/startup_stm32h743xx.s"
        sources = [templates / "system_stm32h7xx.c", glue / "clock_uart.c", glue / "heap.c"]
        includes = [glue, cmsis / "Include", device / "Include", hal / "Inc",
                    hal / "Inc/Legacy"]
        hal_source = hal / "Src"
    else:
        bsp = args.bsp_root.resolve(strict=True)
        startup = bsp / "bsp/startup.s"
        sources = [bsp / "bsp/system_stm32h7xx.c", bsp / "bsp/clock.c",
                   bsp / "bsp/usart.c", bsp / "src/syscalls.c"]
        includes = [bsp / "include", bsp / "vendor/cmsis", bsp / "vendor/device",
                    bsp / "vendor/hal/include", bsp / "vendor/hal/include/Legacy"]
        hal_source = bsp / "vendor/hal/src"
    sources.extend(hal_source / f"stm32h7xx_hal{name}.c" for name in HAL_DRIVERS)
    for path in [startup, *sources]:
        if not path.is_file():
            raise FileNotFoundError(f"Missing board source: {path}")
    for path in includes:
        if not path.is_dir():
            raise FileNotFoundError(f"Missing board include directory: {path}")
    return startup, sources, includes


def build(args):
    compiler = compiler_path(args.compiler)
    def tool(name):
        return compiler.with_name(f"arm-none-eabi-{name}{compiler.suffix}")

    rtos = args.rtos_root.resolve(strict=True)
    startup_source, board_sources, board_includes = board_inputs(args)
    output = args.out_dir.resolve()
    if (output / "build.json").exists():
        raise ValueError(f"Refusing to overwrite completed build evidence: {output}")
    objects_dir = output / "objects"
    objects_dir.mkdir(parents=True, exist_ok=True)

    core_sources = []
    for directory in RTOS_DIRS:
        members = sorted((rtos / directory).glob("*.c"))
        if not members:
            raise ValueError(f"No RTOS source files found in {rtos / directory}")
        core_sources.extend(members)
    test_sources = [HIL / "h743_main.c", args.entry_source.resolve(strict=True),
                    *(source.resolve(strict=True) for source in args.extra_source)]
    if args.vector == "wrapper":
        test_sources.append(HIL / "h743_vectors.S")
    startup = output / "startup.s"
    generate_startup(startup_source, startup, args.vector)
    sources = [*test_sources, *core_sources, startup, *board_sources]
    if len(sources) != len(set(sources)):
        raise ValueError("A source file was listed more than once")

    includes = [HIL, ROOT / "hil/fw/Inc", rtos / "Core/Inc", rtos / "Core/Private",
                *board_includes]
    cpu = ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard",
           "-mno-unaligned-access"]
    compile_flags = [*cpu, f"-{args.opt}", "-g3", "-ffunction-sections", "-fdata-sections",
                     "-fno-lto", "-ffp-contract=off", "-DUSE_HAL_DRIVER", "-DSTM32H743xx",
                     f"-DH743_TEST_ENTRY={args.entry_name}",
                     f"-DH743_TEST_ENTRY_PRIORITY={args.entry_priority}U",
                     f"-DH743_TEST_ICACHE={int(args.icache == 'on')}",
                     f"-DH743_TEST_DCACHE={int(args.dcache == 'on')}",
                     f"-DH743_TEST_DATA_DTCM={int(args.data_memory == 'dtcm')}",
                     f"-DRTOS_CONFIG_USE_BASEPRI={int(args.critical_mask == 'basepri')}",
                     f"-DRTOS_CONFIG_ASSERT_ENABLED={int(not args.disable_assertions)}",
                     *(f"-D{value}" for value in args.define), *(f"-I{path}" for path in includes)]

    linker_source = HIL / "linker.ld"
    linker = output / "linker.ld"
    elf = output / "firmware.elf"
    image = output / "firmware.bin"
    symbol_file = output / "symbols.txt"
    with (output / "build.log").open("w", encoding="utf-8") as log:
        compiler_version = run([compiler, "--version"], log).splitlines()[0]
        run([compiler, "-E", "-P", "-x", "c", *compile_flags, linker_source, "-o", linker], log)
        objects = []
        for index, source in enumerate(sources):
            destination = objects_dir / f"{index:03d}_{source.stem}.o"
            flags = ["-std=gnu11", "-Wall"] if source.suffix == ".c" else []
            if source.suffix == ".c" and (source in test_sources or source in core_sources
                                          or source.parent == HIL / "cubeh7"):
                flags += ["-Wextra", "-Werror"]
            run([compiler, *compile_flags, *flags, "-c", source, "-o", destination], log)
            objects.append(destination)

        run([compiler, *cpu, *objects, "-T", linker, "--specs=nano.specs", "--specs=nosys.specs",
             "-Wl,--gc-sections", f"-Wl,-Map={output / 'firmware.map'}",
             *(f"-Wl,--wrap={name}" for name in args.wrap), "-o", elf], log)
        run([tool("objcopy"), "-O", "binary", elf, image], log)
        symbols = run([tool("nm"), "-n", elf], log)
        symbol_file.write_text(symbols, encoding="utf-8")
        size = run([tool("size"), elf], log)

    headers = {path for directory in includes for path in directory.glob("*.h")}
    inputs = sorted({*sources, *headers, startup_source, linker_source, Path(__file__).resolve()})
    report = {"compiler": compiler_version, "arguments": vars(args), "compile_flags": compile_flags,
              "vectors": check_vectors(image, symbols, args.vector), "size": size,
              "sources": {str(path): digest(path) for path in inputs},
              "elf_sha256": digest(elf), "bin_sha256": digest(image),
              "bin_bytes": image.stat().st_size}
    (output / "build.json").write_text(json.dumps(report, indent=2, default=str) + "\n",
                                       encoding="utf-8")
    print(size, end="")
    print(f"Validated H743 vectors and first-sector image: {elf}")


if __name__ == "__main__":
    build(arguments())
