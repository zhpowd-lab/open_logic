#!/usr/bin/env python3
"""装置独立采集服务：原码只读，显式新建采集与网页显示启停分离。"""
import argparse
import errno
from enum import IntEnum
import fcntl
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import mmap
import os
import signal
import struct
import sys
from urllib.parse import parse_qs, urlsplit

MAGIC = 0x3631474C
CLOCK = 24000000
MAX_SAMPLES = 1000000
MIN_PERIOD_TICKS = 24  # 24 MHz 计数器的一微秒档。
CHANNELS = 16  # 原码每个 uint16 对应一组十六路。
WORD_BYTES = 2  # 原码不含共享状态头。
PAGE_BYTES = 4096  # 与 Linux 驱动的 mmap 对齐一致。
STATUS_VERSION = 3  # ABI3 使用独立当前值，不改原码格式。
STATUS_WORDS = 36  # 末尾四字段包含准备统计和诊断保留位。
STATUS = struct.Struct("<%dI" % STATUS_WORDS)
DEFAULT_PORT = 8102  # 与模拟波形和诊断服务保持独立。
RESERVED_PORTS = (8098, 8099, 6011)  # 拒绝占用既有链路。
SOCKET_TIMEOUT_SECONDS = 3  # 限制单连接阻塞服务的时间。
SERVER_POLL_SECONDS = 0.1  # 只影响服务退出，不控制采样周期。
CPU_INDEX = 3  # 与当前 RemoteProc 固件约定一致。
UINT_HIGH_SHIFT = 32  # 序列化的高半字位数。
CPU_OFF_RETURNED_ERROR = 9  # PSCI CPU_OFF 返回即为异常。
UNKNOWN_DROP_ERRORS = (3, 4, 5)  # 总线/超时故障不能推断未完成批次的实际丢点数。
LIVE_PERIOD_TICKS = 24000  # 空闲期 1 kHz 监视，不写入波形缓冲区。
LIVE_WORD_MASK = 0xFFFF
LIVE_VALID = 1 << 16
LIVE_ENABLED = 1 << 17
LIVE_ERROR_SHIFT = 24
PATTERN_PHASE_MASK = 1023  # 固定图案的重复长度减一。
PATTERN_PULSE_WINDOWS_SAMPLES = ((128, 384), (512, 576), (768, 784), (896, 900))  # 左闭右开。
PATTERN_NOTCH_BEGIN_SAMPLE = 500248  # 全段只出现一次的缺口。
PATTERN_NOTCH_END_SAMPLE = 500252  # 缺口终点，不包含。
PATTERN_COUNTER_MASK = 0x7FFE  # 辅助通道计数位，保留 D0 和 D15。
KEYS = ("seq magic version bytes cpu state capture_id period_ticks limit count dropped error "
        "first_lo first_hi last_lo last_hi min_interval max_interval max_late max_read "
        "last_word heartbeat clock_hz active_pattern command_gen command_ack command request_period "
        "request_limit request_pattern epoch_lo epoch_hi priming_reads priming_max_ticks reserved2 live_status").split()
class CaptureState(IntEnum):
    BOOT = 0
    IDLE = 1
    RUNNING = 2
    FULL = 3
    STOPPED = 4
    FAULT = 5
    OFF = 6


STATES = tuple(state.name.lower() for state in CaptureState)
CLAIM, RELEASE, START = 0x4C01, 0x4C02, 0x40104C03
ENABLE_MONITOR = 0x4C04


def capacity(period):
    return min(MAX_SAMPLES, CLOCK // period)


def validate_request(period, count, pattern):
    if not MIN_PERIOD_TICKS <= period <= CLOCK or not 1 <= count <= capacity(period) or pattern not in (0, 1):
        raise ValueError("周期、深度或源模式越界；最大 1 秒、1,000,000 点")


class Device:
    def __init__(self, release=False):
        self.fd = os.open("/dev/logic16", os.O_RDWR | os.O_CLOEXEC)
        self.samples = None
        try:
            if release:
                fcntl.ioctl(self.fd, RELEASE)
            else:
                fcntl.ioctl(self.fd, CLAIM)
                aligned_bytes = (MAX_SAMPLES * WORD_BYTES + PAGE_BYTES - 1) // PAGE_BYTES * PAGE_BYTES
                self.samples = mmap.mmap(self.fd, aligned_bytes,
                                         flags=mmap.MAP_SHARED, prot=mmap.PROT_READ, offset=PAGE_BYTES)
                fcntl.ioctl(self.fd, ENABLE_MONITOR)
        except BaseException:
            if self.samples is not None:
                self.samples.close()
            os.close(self.fd)
            raise

    def status(self):
        data = os.read(self.fd, STATUS.size)
        if len(data) != STATUS.size:
            raise RuntimeError("状态长度不匹配")
        state = dict(zip(KEYS, STATUS.unpack(data)))
        if (state["magic"] != MAGIC or state["version"] != STATUS_VERSION or state["bytes"] != STATUS.size or
                state["cpu"] != CPU_INDEX or state["clock_hz"] != CLOCK or state["count"] > MAX_SAMPLES or
                state["state"] >= len(STATES)):
            raise RuntimeError("CPU3 状态合同不匹配")
        return state

    def arm(self, period, count, pattern):
        validate_request(period, count, pattern)
        fcntl.ioctl(self.fd, START, struct.pack("<4I", period, count, pattern, 0))

    def data(self, capture_id, epoch, count):
        before = self.status()
        if capture_id != before["capture_id"] or epoch != epoch_of(before) or count > before["count"]:
            raise ValueError("请求的数据段已变化或尚未提交；保留原显示并重新读取元数据")
        result = self.samples[:count * WORD_BYTES]
        after = self.status()
        if after["capture_id"] != capture_id or epoch_of(after) != epoch:
            raise ValueError("读取期间数据段发生变化")
        return result

    def close(self):
        if self.samples is not None:
            self.samples.close()
        os.close(self.fd)


def pattern_word(index):
    phase = index & PATTERN_PHASE_MASK
    pulse = any(begin <= phase < end for begin, end in PATTERN_PULSE_WINDOWS_SAMPLES)
    if PATTERN_NOTCH_BEGIN_SAMPLE <= index < PATTERN_NOTCH_END_SAMPLE:
        pulse = False
    return ((index >> 1) & PATTERN_COUNTER_MASK) | int(pulse)


class TestPattern:
    """只验证 HTTP 和回放合同，明确标为软件测试，不能代替 CPU3 或总线实测。"""
    def __init__(self):
        self.current = dict.fromkeys(KEYS, 0)
        self.current.update(magic=MAGIC, version=STATUS_VERSION, bytes=STATUS.size, cpu=CPU_INDEX, clock_hz=CLOCK,
                            active_pattern=1, epoch_lo=1)
        self.buffer = b"".join(struct.pack("<H", pattern_word(i)) for i in range(MAX_SAMPLES))
        self.arm(MIN_PERIOD_TICKS, MAX_SAMPLES, 1)

    def status(self):
        return dict(self.current)

    def arm(self, period, count, pattern):
        validate_request(period, count, pattern)
        if pattern != 1:
            raise ValueError("软件服务测试不能请求硬件输入")
        self.current.update(state=CaptureState.FULL, capture_id=self.current["capture_id"] + 1,
                            period_ticks=period, limit=count, count=count,
                            last_word=pattern_word(count - 1))

    def data(self, capture_id, epoch, count):
        if capture_id != self.current["capture_id"] or epoch != epoch_of(self.current) or count > self.current["count"]:
            raise ValueError("测试段已变化")
        return self.buffer[:count * WORD_BYTES]

    def close(self):
        pass


def epoch_of(state):
    return "%08x%08x" % (state["epoch_hi"], state["epoch_lo"])


def metadata(state, software_service=False):
    captured = bool(state["capture_id"])
    final = captured and state["state"] != CaptureState.RUNNING
    pattern = bool(state["active_pattern"])
    first_tick = (state["first_hi"] << UINT_HIGH_SHIFT) | state["first_lo"]
    last_tick = (state["last_hi"] << UINT_HIGH_SHIFT) | state["last_lo"]
    return {
        "format": "logic16-le-v1", "channels": CHANNELS, "byteOrder": "little",
        "sampleCount": state["count"], "samplePeriodTicks": state["period_ticks"] if captured else None,
        "clockHz": CLOCK, "sequenceStart": 0 if captured else None,
        "droppedSamples": state["dropped"] if captured and state["error"] not in UNKNOWN_DROP_ERRORS else None,
        "source": "服务软件测试图案 · 非装置采集" if software_service else
                  ("CPU3 固定测试图案 · 非实物输入" if pattern else "CPU3 16 位总线遥信 · CS 常开"),
        "mode": "device", "deviceConnected": True, "demo": False,
        "captureId": state["capture_id"], "sourceEpoch": epoch_of(state),
        "captureState": STATES[state["state"]], "capacity": state["limit"] if captured else None,
        "errorCode": state["error"], "pendingCommand": state["command_gen"] != state["command_ack"],
        "pattern": pattern, "softwareServiceTest": software_service,
        "nominalTime": True, "sampleTimeBasis": "software-read-entry", "droppedAtTail": True,
        "firstSampleTick": str(first_tick) if final and state["count"] and not software_service else None,
        "lastSampleTick": str(last_tick) if final and state["count"] and not software_service else None,
        "minIntervalTicks": state["min_interval"] if final and state["count"] > 1 and not software_service else None,
        "maxIntervalTicks": state["max_interval"] if final and state["count"] > 1 and not software_service else None,
        "maxLateTicks": state["max_late"] if final and not software_service else None,
        "maxReadTicks": state["max_read"] if final and not software_service else None,
        "lastWord": state["last_word"] if captured and final else None,
        "primingReads": state["priming_reads"] if captured and not software_service else None,
        "primingMaxTicks": state["priming_max_ticks"] if captured and not software_service else None,
        "shutdownResult": state["reserved2"] if state["error"] == CPU_OFF_RETURNED_ERROR else None,
        "startupGpioProbeWord": state["reserved2"] & LIVE_WORD_MASK if state["reserved2"] & LIVE_VALID else None,
        "liveMonitorSupported": not software_service,
        "liveWord": state["live_status"] & LIVE_WORD_MASK if state["live_status"] & LIVE_VALID else None,
        "liveReadSequence": state["heartbeat"] if not software_service else None,
        "livePeriodTicks": LIVE_PERIOD_TICKS if not software_service else None,
        "liveErrorCode": state["live_status"] >> LIVE_ERROR_SHIFT if not software_service else None,
        "liveMonitorEnabled": bool(state["live_status"] & LIVE_ENABLED) and not software_service,
    }


def make_handler(backend):
    class Handler(BaseHTTPRequestHandler):
        server_version = "Logic16/1"
        sys_version = ""

        def setup(self):
            super().setup()
            self.connection.settimeout(SOCKET_TIMEOUT_SECONDS)

        def log_message(self, format_text, *args):
            pass

        def respond(self, status, body, content_type="application/json; charset=utf-8"):
            if not isinstance(body, bytes):
                body = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode()
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def query(self, expected):
            values = parse_qs(urlsplit(self.path).query, keep_blank_values=True, strict_parsing=True)
            if set(values) != set(expected) or any(len(value) != 1 for value in values.values()):
                raise ValueError("请求参数缺失、重复或包含未知字段")
            result = {key: value[0] for key, value in values.items()}
            for key in expected:
                if key != "epoch":
                    if not result[key] or not all('0' <= char <= '9' for char in result[key]) or len(result[key]) > 10:
                        raise ValueError("参数必须为范围内整数")
                    result[key] = int(result[key])
            return result

        def dispatch(self):
            try:
                route = urlsplit(self.path).path
                if self.command in ("GET", "HEAD") and route == "/api/capture":
                    self.respond(200, metadata(backend.status(), isinstance(backend, TestPattern)))
                elif self.command in ("GET", "HEAD") and route == "/api/samples":
                    query = self.query(("capture", "epoch", "count"))
                    if not 0 <= query["count"] <= MAX_SAMPLES:
                        raise ValueError("样本范围无效")
                    data = backend.data(query["capture"], query["epoch"], query["count"])
                    self.respond(200, data, "application/octet-stream")
                elif self.command == "POST" and route == "/api/arm":
                    if self.headers.get("X-Logic16-Arm") != "1" or self.headers.get("Content-Length", "0") != "0":
                        self.respond(403, {"error": "仅接受明确的无正文采集命令"})
                        return
                    query = self.query(("period", "count", "pattern"))
                    backend.arm(query["period"], query["count"], query["pattern"])
                    self.respond(202, {"accepted": True})
                else:
                    self.respond(404, {"error": "无此独立采集接口"})
            except (ValueError, OverflowError) as error:
                self.respond(409, {"error": str(error)})
            except OSError as error:
                self.respond(409 if error.errno in (errno.EBUSY, errno.EAGAIN) else 503,
                             {"error": "装置资源忙或采集接口不可用", "errno": error.errno})
            except RuntimeError as error:
                self.respond(503, {"error": str(error)})

        do_GET = dispatch
        do_HEAD = dispatch
        do_POST = dispatch
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--test-pattern", action="store_true")
    parser.add_argument("--release", action="store_true")
    options = parser.parse_args()
    if not 1024 <= options.port <= 65535 or options.port in RESERVED_PORTS:
        parser.error("端口无效或属于现有服务")
    backend = TestPattern() if options.test_pattern else Device(release=options.release)
    try:
        if options.release:
            print("采集资源已释放")
            return
        server = HTTPServer((options.bind, options.port), make_handler(backend))
        print("logic16 服务就绪：空闲期监视当前开入，波形段需明确新建采集", flush=True)
        def terminate(signum, frame):
            raise KeyboardInterrupt()
        signal.signal(signal.SIGTERM, terminate)
        try:
            server.serve_forever(poll_interval=SERVER_POLL_SECONDS)
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()
    finally:
        backend.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as failure:
        print("logic16 服务启动失败：" + str(failure), file=sys.stderr)
        sys.exit(1)
