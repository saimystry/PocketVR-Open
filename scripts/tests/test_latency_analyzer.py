import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("latency_analyzer", Path(__file__).parents[1] / "latency_analyzer.py")
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)
HEADER = "frame_id,bytes,encode_ms,before_send_ms,send_ms,network_ms,queue_ms,decode_ms,display_ms,total_ms\n"


class LatencyAnalyzer(unittest.TestCase):
    def write(self, text):
        folder = tempfile.TemporaryDirectory(prefix="PocketVR-latency-")
        self.addCleanup(folder.cleanup)
        path = Path(folder.name) / "latency.csv"
        path.write_text(text, encoding="utf-8")
        return path

    def test_reports_percentiles_for_each_stage(self):
        rows = "".join(f"{i},1000,{i},1,1,1,1,1,1,{i}\n" for i in range(1, 101))
        summary = analyzer.summarize(self.write(HEADER + rows))
        self.assertEqual(summary["frames"], 100)
        self.assertEqual(summary["average_kilobits"], 8)
        self.assertEqual(summary["total_ms"][1], 50)
        self.assertEqual(summary["total_ms"][2], 95)

    def test_repeated_header_from_a_new_session_is_ignored(self):
        text = HEADER + "1,1000,1,1,1,1,1,1,1,1\n" + HEADER + "2,1000,1,1,1,1,1,1,1,3\n"
        self.assertEqual(analyzer.summarize(self.write(text))["frames"], 2)

    def test_empty_log_is_explained(self):
        with self.assertRaises(SystemExit):
            analyzer.summarize(self.write(HEADER))


if __name__ == "__main__":
    unittest.main()
