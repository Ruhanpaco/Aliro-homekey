import importlib.util
import sys
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("analyze_tap_log.py")
SPEC = importlib.util.spec_from_file_location("analyze_tap_log", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class AnalyzeTapLogTests(unittest.TestCase):
    def test_summarizes_transactions_retries_and_pn532_errors(self):
        summary = MODULE.summarize(
            [
                "I (100) aliro/reader: transaction ok (fast) in 816 ms\n",
                "I (200) aliro/reader: transaction ok (standard) in 2360 ms\n",
                "W (300) aliro/reader: APDU exchange attempt 1/3 failed: ESP_ERR_TIMEOUT\n",
                "I (310) aliro/reader: APDU exchange recovered on attempt 2/3\n",
                "W (400) aliro/reader: transaction failed after 1200 ms: ESP_ERR_TIMEOUT\n",
                "E (410) nfc/pn532: pn532_command(345): 0x4A: no ACK\n",
            ]
        )

        self.assertEqual(summary.transactions, 3)
        self.assertEqual(summary.successful, 2)
        self.assertEqual(summary.failed, 1)
        self.assertAlmostEqual(summary.success_rate, 66.6666666667)
        self.assertEqual(summary.fast, 1)
        self.assertEqual(summary.standard, 1)
        self.assertEqual(summary.median_duration_ms, 1200)
        self.assertEqual(summary.p95_duration_ms, 2360)
        self.assertEqual(summary.failure_reasons, {"ESP_ERR_TIMEOUT": 1})
        self.assertEqual(summary.apdu_failed_attempts, 1)
        self.assertEqual(summary.apdu_recoveries, 1)
        self.assertEqual(summary.apdu_failure_reasons, {"ESP_ERR_TIMEOUT": 1})
        self.assertEqual(summary.pn532_error_lines, 1)

    def test_empty_log_reports_no_transactions(self):
        summary = MODULE.summarize([])

        self.assertEqual(summary.transactions, 0)
        self.assertIsNone(summary.success_rate)
        self.assertIsNone(summary.median_duration_ms)
        self.assertIsNone(summary.p95_duration_ms)

    def test_counts_warning_level_pn532_lines(self):
        summary = MODULE.summarize(
            [
                "I (10) nfc/pn532: reader ready, mode W is available\n",
                "W (20) nfc/pn532: transient radio warning\n",
                "E (30) nfc/pn532: 0x4A: no ACK\n",
                "W (40) aliro/reader: message mentions nfc/pn532\n",
            ]
        )
        self.assertEqual(summary.pn532_error_lines, 2)


if __name__ == "__main__":
    unittest.main()
