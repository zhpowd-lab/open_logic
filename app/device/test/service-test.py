"""通过公开 HTTP 接口核对服务合同，不把软件图案当硬件采集。"""
import importlib.util
import json
from pathlib import Path
import struct
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from http.server import HTTPServer

spec = importlib.util.spec_from_file_location("logic16_service", Path(__file__).resolve().parents[1] / "linux/service.py")
service = importlib.util.module_from_spec(spec)
spec.loader.exec_module(service)


class ServiceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.backend = service.TestPattern()
        cls.server = HTTPServer(("127.0.0.1", 0), service.make_handler(cls.backend))
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.url = "http://127.0.0.1:%d" % cls.server.server_port

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def request(self, route, method="GET", authorized=False):
        request = Request(self.url + route, method=method)
        if authorized:
            request.add_header("X-Logic16-Arm", "1")
        return urlopen(request, timeout=3)

    def test_contract(self):
        with self.request("/api/capture") as response:
            info = json.load(response)
        self.assertTrue(info["softwareServiceTest"])
        self.assertEqual(info["sampleCount"], 1000000)
        self.assertEqual(info["samplePeriodTicks"], 24)
        route = "/api/samples?capture=%d&epoch=%s&count=1000000" % (info["captureId"], info["sourceEpoch"])
        with self.request(route) as response:
            raw = response.read()
        self.assertEqual(len(raw), 2000000)
        self.assertEqual(struct.unpack_from("<H", raw, 500248 * 2)[0], 0x510C)
        self.assertEqual(struct.unpack_from("<H", raw, 500252 * 2)[0], 0x510F)
        self.assertEqual(info["firstSampleTick"], None)

    def test_unknown_before_arm(self):
        empty = dict.fromkeys(service.KEYS, 0)
        info = service.metadata(empty)
        self.assertIsNone(info["samplePeriodTicks"])
        self.assertIsNone(info["sequenceStart"])
        self.assertIsNone(info["droppedSamples"])
        self.assertIsNone(info["liveWord"])

    def test_live_monitor_independent_of_capture(self):
        state = dict.fromkeys(service.KEYS, 0)
        state.update(live_status=service.LIVE_ENABLED | service.LIVE_VALID | 0xA55A, heartbeat=123)
        info = service.metadata(state)
        self.assertEqual(info["liveWord"], 0xA55A)
        self.assertEqual(info["liveReadSequence"], 123)
        self.assertTrue(info["liveMonitorEnabled"])
        self.assertEqual(info["sampleCount"], 0)
        self.assertIsNone(info["samplePeriodTicks"])
        state.update(live_status=5 << service.LIVE_ERROR_SHIFT)
        info = service.metadata(state)
        self.assertIsNone(info["liveWord"])
        self.assertEqual(info["liveErrorCode"], 5)
        self.assertFalse(info["liveMonitorEnabled"])
        state.update(reserved2=service.LIVE_VALID | 0xA55A)
        self.assertEqual(service.metadata(state)["startupGpioProbeWord"], 0xA55A)
        # PSCI 返回负值时同样可能置有效位，不能把关核错误显示成一次输入测量。
        state.update(error=service.CPU_OFF_RETURNED_ERROR, reserved2=0xFFFFFFFF)
        info = service.metadata(state)
        self.assertIsNone(info["startupGpioProbeWord"])
        self.assertEqual(info["shutdownResult"], 0xFFFFFFFF)

    def test_commands_and_stale_capture(self):
        old = self.backend.status()["capture_id"]
        with self.assertRaises(HTTPError) as failure:
            self.request("/api/arm?period=24&count=1000000&pattern=1", "POST")
        self.assertEqual(failure.exception.code, 403)
        with self.request("/api/arm?period=24&count=1000000&pattern=1", "POST", True) as response:
            self.assertEqual(response.status, 202)
        with self.assertRaises(HTTPError) as failure:
            self.request("/api/samples?capture=%d&epoch=0000000000000001&count=10" % old)
        self.assertEqual(failure.exception.code, 409)

    def test_reject_bad_requests(self):
        for query in ("period=0&count=1&pattern=1", "period=24&count=1000001&pattern=1",
                      "period=240&count=100001&pattern=1", "period=24&period=24&count=1&pattern=1",
                      "period=24&count=1&pattern=0", "period=24&count=-1&pattern=1"):
            with self.assertRaises(HTTPError) as failure:
                self.request("/api/arm?" + query, "POST", True)
            self.assertEqual(failure.exception.code, 409)


if __name__ == "__main__":
    unittest.main(verbosity=2)
