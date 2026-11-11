"""Exercise the F446 UART pipeline and retain binary request and response captures."""

import argparse
import binascii
import csv
import ctypes
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import sys
import time


SYNC = b"\xa5\x5a"
TRANSFORM = 0x01
INFERENCE = 0x02
STATUS = 0x03
ERROR = 0xE0
RESPONSE_BIT = 0x80
TRANSFORM_LIMIT = 64
MODEL_INPUT_SIZE = 2044
HEADER = struct.Struct("<BHI")
CRC = struct.Struct("<H")
TRANSFORM_RESULT = struct.Struct("<II")
INFERENCE_RESULT = struct.Struct("<I4f")
STATUS_LEGACY = struct.Struct("<12I")
STATUS_RESULT = struct.Struct("<15I")
STATUS_FIELDS = (
    "rx_bytes", "rx_irq_cycles", "rx_irq_peak", "rx_uart_errors", "rx_queue_drops",
    "bad_crc", "rejected", "transform_done", "infer_done", "model_cycles", "parser_cycles",
    "tx_failures",
)
CYCLE_FIELDS = ("rx_irq_cycles", "model_cycles", "parser_cycles")
FRAME_OVERHEAD = len(SYNC) + HEADER.size + CRC.size


class PipelineError(Exception):
    """A UART transaction or protocol assertion failed."""


@dataclass(frozen=True)
class Frame:
    opcode: int
    request_id: int
    payload: bytes


def checksum(data):
    return binascii.crc_hqx(data, 0xffff)


def encode(frame):
    body = HEADER.pack(frame.opcode, len(frame.payload), frame.request_id) + frame.payload
    return SYNC + body + CRC.pack(checksum(body))


def transformed(payload):
    fnv = 2166136261
    for value in payload:
        fnv = ((fnv ^ value) * 16777619) & 0xffffffff
    return TRANSFORM_RESULT.pack(sum(payload), fnv)


class NativeModel:
    def __init__(self, library_path):
        self.library = ctypes.CDLL(str(library_path))
        try:
            self.forward = self.library.ai_forward_u8
        except AttributeError as error:
            raise PipelineError("The native model does not export ai_forward_u8") from error
        self.forward.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self.forward.restype = ctypes.c_int

    def expected(self, payload):
        source = (ctypes.c_uint8 * MODEL_INPUT_SIZE).from_buffer_copy(payload)
        logits = (ctypes.c_float * 4)()
        predicted = ctypes.c_uint32()
        status = self.forward(source, logits, ctypes.byref(predicted))
        if status != 0 or predicted.value > 3 or not all(math.isfinite(value) for value in logits):
            raise PipelineError(f"Host model failed or returned invalid logits (status {status})")
        return INFERENCE_RESULT.pack(predicted.value, *logits)


class ResponseDecoder:
    def __init__(self):
        self.buffer = bytearray()
        self.synchronized = False
        self.startup_bytes = 0

    def feed(self, data):
        self.buffer.extend(data)
        if not self.synchronized:
            position = self.buffer.find(SYNC)
            if position < 0:
                keep = int(self.buffer.endswith(SYNC[:1]))
                drop = len(self.buffer) - keep
                self.startup_bytes += drop
                del self.buffer[:drop]
                return
            self.startup_bytes += position
            del self.buffer[:position]
            self.synchronized = True
        while self.buffer:
            prefix_size = min(len(self.buffer), len(SYNC))
            if self.buffer[:prefix_size] != SYNC[:prefix_size]:
                raise PipelineError("Unexpected bytes at response frame boundary")
            if len(self.buffer) < FRAME_OVERHEAD:
                return
            opcode, size, request_id = HEADER.unpack_from(self.buffer, len(SYNC))
            if size > STATUS_RESULT.size:
                raise PipelineError(f"Oversized response for request {request_id}")
            frame_size = FRAME_OVERHEAD + size
            if len(self.buffer) < frame_size:
                return
            body = bytes(self.buffer[len(SYNC):frame_size - CRC.size])
            received_crc, = CRC.unpack_from(self.buffer, frame_size - CRC.size)
            if checksum(body) != received_crc:
                raise PipelineError(f"Invalid response CRC for request {request_id}")
            del self.buffer[:frame_size]
            yield Frame(opcode, request_id, body[HEADER.size:])


@dataclass(frozen=True)
class PendingRequest:
    request_id: int
    opcode: int
    payload_size: int
    expected: bytes
    sent_ns: int


class Capture:
    def __init__(self, port, transmit, receive, transactions):
        self.port = port
        self.transmit = transmit
        self.receive = receive
        self.transactions = transactions
        self.decoder = ResponseDecoder()

    def read(self, output):
        data = self.port.read(max(1, min(self.port.in_waiting, 4096)))
        now = time.perf_counter_ns()
        if data:
            output.write(data)
        return data, now

    def receive_frames(self):
        data, now = self.read(self.receive)
        return self.decoder.feed(data), now

    def write(self, data):
        started = time.perf_counter_ns()
        # Preserve attempted writes, including one that the driver fails partway through.
        self.transmit.write(data)
        count = self.port.write(data)
        if count != len(data):
            raise PipelineError(f"Short serial write ({count} of {len(data)} bytes)")
        return started


def await_start(capture, startup, args):
    deadline = time.perf_counter_ns() + int(
        (args.timeout if args.ready_text else args.startup_delay) * 1e9
    )
    marker = args.ready_text.encode() if args.ready_text else b""
    tail = b""
    while time.perf_counter_ns() < deadline:
        data, _ = capture.read(startup)
        if marker:
            tail += data
            if marker in tail:
                return
            tail = tail[-len(marker):]
    if marker:
        raise PipelineError(f"Startup marker absent: {args.ready_text!r}")


def assert_payload(frame, request, args):
    if frame.opcode == ERROR:
        code = int.from_bytes(frame.payload, "little")
        raise PipelineError(f"Target error {code:#x} for request {frame.request_id}")
    if frame.opcode != (request.opcode | RESPONSE_BIT):
        raise PipelineError(f"Wrong opcode {frame.opcode:#x} for request {frame.request_id}")
    if request.opcode == INFERENCE:
        if len(frame.payload) != INFERENCE_RESULT.size:
            raise PipelineError(f"Wrong inference result size for request {frame.request_id}")
        class_id, *logits = INFERENCE_RESULT.unpack(frame.payload)
        expected_class, *expected_logits = INFERENCE_RESULT.unpack(request.expected)
        error = max(abs(actual - wanted) for actual, wanted in zip(logits, expected_logits))
        matches = all(
            math.isfinite(actual) and abs(actual - wanted) <= args.atol + args.rtol * abs(wanted)
            for actual, wanted in zip(logits, expected_logits)
        )
        if class_id != expected_class or not matches:
            raise PipelineError(
                f"Inference mismatch for request {frame.request_id}: "
                f"expected class {expected_class} logits {expected_logits}, "
                f"actual class {class_id} logits {logits}"
            )
        return error
    if frame.payload != request.expected:
        raise PipelineError(
            f"Incorrect response for request {frame.request_id}: "
            f"expected={request.expected.hex()}, actual={frame.payload.hex()}"
        )
    return None


def accept_response(frame, pending, seen, observed_ns, deadline_ns, capture, scenario, args):
    request_id = frame.request_id
    if request_id in seen:
        raise PipelineError(f"Duplicate response for request {request_id}")
    request = pending.get(request_id)
    if request is None:
        raise PipelineError(f"Unexpected response for request {request_id}")
    max_error = assert_payload(frame, request, args)
    elapsed = observed_ns - request.sent_ns
    if elapsed > deadline_ns:
        raise PipelineError(f"Late response for request {request_id}")
    seen.add(request_id)
    del pending[request_id]
    capture.transactions.writerow((scenario, request_id, request.payload_size, elapsed, max_error))
    return elapsed, max_error


def check_timeout(pending, deadline_ns, now):
    if pending:
        oldest = next(iter(pending.values()))
        if now - oldest.sent_ns > deadline_ns:
            raise PipelineError(f"No response for request {oldest.request_id}")


def verify_recovery(capture, seen, args, timeout_ns, *, invalid_header=False):
    data = b"\xa5\x5a\x00\xff"
    if invalid_header:
        short_frame = encode(Frame(TRANSFORM, 4, b"\x11\x22\x33"))
        length = len(SYNC) + HEADER.size
        # Keep the actual body and old CRC short so a parser waiting for 2044 bytes stalls.
        malformed = SYNC + HEADER.pack(TRANSFORM, MODEL_INPUT_SIZE, 4) + short_frame[length:]
        if SYNC in malformed[len(SYNC):]:
            raise PipelineError("Invalid header fixture contains an extra synchronization marker")
        valid_id = 5
        scenario = "header_recovery"
    else:
        malformed = bytearray(encode(Frame(TRANSFORM, 1, data)))
        malformed[-1] ^= 1
        valid_id = 2
        scenario = "recovery"
    sent = capture.write(malformed + encode(Frame(TRANSFORM, valid_id, data)))
    pending = {valid_id: PendingRequest(valid_id, TRANSFORM, len(data), transformed(data), sent)}
    while pending:
        replies, now = capture.receive_frames()
        for frame in replies:
            accept_response(frame, pending, seen, now, timeout_ns, capture, scenario, args)
        check_timeout(pending, timeout_ns, now)


def payloads(count, seed, mode):
    if mode == "transform":
        fixed = (b"", b"\xff", bytes(range(63)), b"\xa5\x5a" * 32)
    else:
        fixed = (
            bytes(MODEL_INPUT_SIZE),
            bytes(254 * (i % 110) // 109 for i in range(MODEL_INPUT_SIZE)),
            bytes([255]) * MODEL_INPUT_SIZE,
            b"\xa5\x5a" * (MODEL_INPUT_SIZE // 2),
            bytes(int(127 + 100 * math.sin(i / 16)) for i in range(MODEL_INPUT_SIZE)),
        )
    rng = random.Random(seed)
    for index in range(count):
        if index < len(fixed):
            yield fixed[index]
        else:
            size = rng.randrange(TRANSFORM_LIMIT + 1) if mode == "transform" else MODEL_INPUT_SIZE
            yield bytes(rng.getrandbits(8) for _ in range(size))


def latency_summary(values):
    ordered = sorted(values)
    count = len(ordered)
    percentiles = {f"p{rank}": ordered[(count * rank + 99) // 100 - 1] for rank in (50, 95, 99)}
    return {"min": ordered[0], "mean": sum(ordered) // count, **percentiles, "max": ordered[-1]}


def run_window(capture, seen, first_id, window, args, timeout_ns, workload):
    scenario = f"window_{window}"
    opcode = TRANSFORM if args.mode == "transform" else INFERENCE
    pending = {}
    samples = []
    worst_error = 0.0
    sent_count = 0
    payload_size = 0
    started = None
    finished = None
    while len(samples) < args.requests:
        while len(pending) < window and sent_count < args.requests:
            payload, expected = workload[sent_count]
            request_id = first_id + sent_count
            sent = capture.write(encode(Frame(opcode, request_id, payload)))
            if started is None:
                started = sent
            pending[request_id] = PendingRequest(request_id, opcode, len(payload), expected, sent)
            payload_size += len(payload)
            sent_count += 1
        replies, observed = capture.receive_frames()
        for frame in replies:
            elapsed, max_error = accept_response(frame, pending, seen, observed, timeout_ns,
                                                 capture, scenario, args)
            samples.append(elapsed)
            if max_error is not None:
                worst_error = max(worst_error, max_error)
            finished = observed
        check_timeout(pending, timeout_ns, observed)
    duration = finished - started
    result = {
        "window": window,
        "requests": sent_count,
        "responses": len(samples),
        "duration_ns": duration,
        "requests_per_second": sent_count * 1e9 / duration,
        "request_payload_bytes": payload_size,
        "request_wire_bytes": payload_size + sent_count * FRAME_OVERHEAD,
        "response_wire_bytes": sent_count * (
            FRAME_OVERHEAD + (TRANSFORM_RESULT if opcode == TRANSFORM else INFERENCE_RESULT).size
        ),
        "latency_ns": latency_summary(samples),
    }
    if opcode == INFERENCE:
        result["worst_logit_abs_error"] = worst_error
        class_counts = [0] * 4
        for _, expected in workload:
            class_id, *_ = INFERENCE_RESULT.unpack(expected)
            class_counts[class_id] += 1
        result["class_counts"] = class_counts
    return result


def read_status(capture, request_id, timeout_ns):
    started = capture.write(encode(Frame(STATUS, request_id, b"")))
    status = None
    while status is None:
        replies, observed = capture.receive_frames()
        for frame in replies:
            if (frame.opcode != (STATUS | RESPONSE_BIT) or frame.request_id != request_id
                    or len(frame.payload) not in (STATUS_RESULT.size, STATUS_LEGACY.size)
                    or status is not None):
                raise PipelineError(f"Unexpected frame while awaiting status {request_id}: {frame}")
            if observed - started > timeout_ns:
                raise PipelineError(f"Late status response for request {request_id}")
            if len(frame.payload) == STATUS_RESULT.size:
                words = STATUS_RESULT.unpack(frame.payload)
                status = dict(zip(STATUS_FIELDS, words))
                for field, upper in zip(CYCLE_FIELDS, words[len(STATUS_FIELDS):]):
                    status[field] |= upper << 32
                status["cycle_counter_bits"] = 64
            else:
                status = dict(zip(STATUS_FIELDS, STATUS_LEGACY.unpack(frame.payload)))
                status["cycle_counter_bits"] = 32
        if observed - started > timeout_ns:
            raise PipelineError(f"No status response for request {request_id}")
    return status


def status_delta(before, after):
    cycle_bits = before["cycle_counter_bits"]
    if cycle_bits != after["cycle_counter_bits"]:
        raise PipelineError("Status counter format changed during a run")
    return {
        field: after[field] if field == "rx_irq_peak" else
        (after[field] - before[field]) % (1 << (cycle_bits if field in CYCLE_FIELDS else 32))
        for field in STATUS_FIELDS
    }


def verify_device_counters(delta, transforms, inferences, bad_crc, *, rejected=0):
    expected = {
        "transform_done": transforms, "infer_done": inferences, "bad_crc": bad_crc,
        "rx_uart_errors": 0, "rx_queue_drops": 0, "rejected": rejected, "tx_failures": 0,
    }
    for field, count in expected.items():
        if delta[field] != count:
            raise PipelineError(f"Device counter {field} expected {count}, observed {delta[field]}")


def verify_quiet(capture, seconds):
    deadline = time.perf_counter_ns() + int(seconds * 1e9)
    while time.perf_counter_ns() < deadline:
        replies, _ = capture.receive_frames()
        for frame in replies:
            raise PipelineError(f"Extra response after test for request {frame.request_id}")
    if capture.decoder.buffer:
        raise PipelineError("Trailing incomplete response")


def run(port, output, args, summary, workload):
    timeout_ns = int(args.timeout * 1e9)
    with (
        (output / "startup.bin").open("wb") as startup,
        (output / "tx.bin").open("wb") as tx,
        (output / "rx.bin").open("wb") as rx,
        (output / "transactions.csv").open("w", encoding="utf-8", newline="") as csv_output,
    ):
        transactions = csv.writer(csv_output)
        transactions.writerow(("scenario", "request_id", "request_payload_bytes", "latency_ns",
                               "logit_max_abs_error"))
        capture = Capture(port, tx, rx, transactions)
        await_start(capture, startup, args)
        seen = set()
        summary["status_initial"] = before = read_status(capture, 0, timeout_ns)
        verify_recovery(capture, seen, args, timeout_ns)
        after = read_status(capture, 3, timeout_ns)
        summary["recovery_status_after"] = after
        delta = status_delta(before, after)
        summary["recovery_status_delta"] = delta
        verify_device_counters(delta, transforms=1, inferences=0, bad_crc=1)
        summary["crc_recovery"] = True
        first_id = 4
        if args.verify_header_recovery:
            summary["header_recovery"] = False
            before = after
            verify_recovery(capture, seen, args, timeout_ns, invalid_header=True)
            after = read_status(capture, 6, timeout_ns)
            summary["header_recovery_status_after"] = after
            delta = status_delta(before, after)
            summary["header_recovery_status_delta"] = delta
            verify_device_counters(delta, transforms=1, inferences=0, bad_crc=0, rejected=1)
            summary["header_recovery"] = True
            first_id = 7
        for window in args.windows:
            outcome = run_window(capture, seen, first_id, window, args, timeout_ns, workload)
            summary["scenarios"].append(outcome)
            first_id += args.requests
            outcome["device_before"] = after
            after = read_status(capture, first_id, timeout_ns)
            outcome["device_after"] = after
            delta = status_delta(outcome["device_before"], after)
            outcome["device_delta"] = delta
            verify_device_counters(
                delta, transforms=args.requests if args.mode == "transform" else 0,
                inferences=args.requests if args.mode == "inference" else 0, bad_crc=0
            )
            first_id += 1
        verify_quiet(capture, args.quiet)
        summary["unique_valid_responses"] = len(seen)
        summary["business_valid_responses"] = sum(item["responses"] for item in summary["scenarios"])
        summary["startup_bytes_before_first_frame"] = capture.decoder.startup_bytes


def windows_argument(value):
    try:
        windows = [int(item) for item in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("Expected comma-separated window sizes") from error
    if len(set(windows)) != len(windows) or any(not 1 <= item <= 64 for item in windows):
        raise argparse.ArgumentTypeError("Window sizes must be distinct integers in 1..64")
    return windows


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--requests", type=int, help="Business requests per window")
    parser.add_argument("--windows", type=windows_argument, default="1,8")
    parser.add_argument("--seed", type=int, default=3735928559)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--elf", type=Path, help="Record the identity of the supplied ELF")
    parser.add_argument("--mode", choices=("transform", "inference"), default="transform")
    parser.add_argument("--model", type=Path, help="Native DLL exporting ai_forward_u8")
    parser.add_argument("--atol", type=float, default=0.02, help="Absolute logit tolerance")
    parser.add_argument("--rtol", type=float, default=0.001, help="Relative logit tolerance")
    parser.add_argument("--timeout", type=float, default=3, help="Per-request deadline in seconds")
    parser.add_argument("--quiet", type=float, default=0.2, help="Trailing observation in seconds")
    parser.add_argument("--verify-header-recovery", action="store_true",
                        help="Test resynchronization after an impossible transform length")
    startup = parser.add_mutually_exclusive_group()
    startup.add_argument("--startup-delay", type=float, default=0.1, help="Drain startup text")
    startup.add_argument("--ready-text", help="Wait for a UTF-8 startup marker")
    args = parser.parse_args()
    if args.requests is None:
        args.requests = 600 if args.mode == "transform" else 36
    if not 1 <= args.requests <= 100000 or args.baud <= 0:
        parser.error("Expected a positive baud rate and requests in 1..100000")
    if args.mode == "inference" and (args.model is None or args.requests > 10000):
        parser.error("Inference requires --model and supports up to 10000 requests per window")
    if not all(math.isfinite(value) and value >= 0 for value in (args.atol, args.rtol)):
        parser.error("Expected finite nonnegative logit tolerances")
    if not (0 < args.timeout <= 60 and 0 < args.quiet <= 60 and 0 <= args.startup_delay <= 60):
        parser.error("Expected a positive bounded timeout and a quiet duration in (0, 60]")
    return args


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def main():
    args = parse_args()
    try:
        import serial
    except ImportError:
        print("pyserial is required to open the target UART", file=sys.stderr)
        return 1

    output = args.output_dir.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print(f"Cannot create evidence directory: {error}", file=sys.stderr)
        return 1

    summary = {
        "status": "fail",
        "started_utc": utc_now(),
        "argv": sys.argv,
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "python": sys.version.split()[0],
        "pyserial": serial.__version__,
        "port": args.port,
        "baud": args.baud,
        "mode": args.mode,
        "seed": args.seed,
        "requested_per_window": args.requests,
        "windows": args.windows,
        "scenarios": [],
        "crc_recovery": False,
    }
    try:
        if args.elf is not None:
            elf = args.elf.resolve()
            summary["elf"] = str(elf)
            summary["elf_sha256"] = hashlib.sha256(elf.read_bytes()).hexdigest()
        if args.mode == "inference":
            library = args.model.resolve()
            summary.update(
                model=str(library), model_sha256=hashlib.sha256(library.read_bytes()).hexdigest(),
                logit_atol=args.atol, logit_rtol=args.rtol
            )
            expected = NativeModel(library).expected
        else:
            expected = transformed
        workload = [(payload, expected(payload))
                    for payload in payloads(args.requests, args.seed, args.mode)]
        with serial.Serial(args.port, args.baud, timeout=0.010, write_timeout=args.timeout) as port:
            run(port, output, args, summary, workload)
        summary["status"] = "pass"
    except (OSError, ValueError, PipelineError) as error:
        summary["error"] = str(error)
    finally:
        summary["finished_utc"] = utc_now()
        (output / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8"
        )
    print(json.dumps(summary))
    return 0 if summary["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
