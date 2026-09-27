import unittest
import pathlib
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parent.parent

class TelemetryInstrumentationTest(unittest.TestCase):
    def test_drawing_telemetry_injected(self):
        core_file = REPOSITORY / "relay12-d3d11" / "d3d11on12core.cpp"
        with open(core_file, "r") as f:
            content = f.read()
        
        self.assertIn("Proxy_pfnDraw(", content, "Proxy_pfnDraw interceptor missing!")
        self.assertIn("InterlockedIncrement64(&state->tele_draw_count);", content, "Missing draw count tracking")
        self.assertIn("if (state->deviceFuncs.pfnDraw) state->deviceFuncs.pfnDraw = Proxy_pfnDraw;", content, "Proxy not wired in CreateDevice")
        self.assertIn("d3d11on12core telemetry: [Draws:", content, "Missing telemetry report upon device destruction")

if __name__ == '__main__':
    unittest.main()
