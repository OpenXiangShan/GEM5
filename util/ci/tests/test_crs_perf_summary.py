#!/usr/bin/env python3

import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import unittest
from unittest import mock
import urllib.error


MODULE_PATH = Path(__file__).parents[1] / "crs_perf_summary.py"
SPEC = importlib.util.spec_from_file_location("crs_perf_summary", MODULE_PATH)
crs_summary = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = crs_summary
SPEC.loader.exec_module(crs_summary)


class CrsPerfSummaryTest(unittest.TestCase):
    def test_sanitizer_excludes_archive_and_internal_paths(self):
        analysis = {
            "schema_version": 1,
            "severity": "warning",
            "reasons": ["score moved"],
            "candidate": {
                "run_id": "12",
                "archive": "/nfs/home/share/secret/archive",
                "head_sha": "abc",
                "metadata": {
                    "commit": "abc",
                    "config_path": "configs/example/kmhv3.py",
                    "cluster_config": "/nfs/home/share/secret/weights.json",
                },
            },
            "baseline": None,
            "data_proc_warnings": {
                "candidate": ["warning: /nfs/home/share/secret/stats.txt"]
            },
        }
        sanitized = crs_summary.sanitize_analysis(analysis)
        rendered = str(sanitized)
        self.assertNotIn("/nfs/", rendered)
        self.assertNotIn("archive", sanitized["candidate"])
        self.assertNotIn("cluster_config", sanitized["candidate"]["metadata"])
        self.assertIn("[internal-path]", rendered)

    def test_extracts_responses_api_text(self):
        response = {
            "output": [
                {
                    "type": "message",
                    "content": [{"type": "output_text", "text": "summary"}],
                }
            ]
        }
        self.assertEqual(crs_summary.extract_output_text(response), "summary")

    def test_neutralizes_markdown_mentions(self):
        self.assertEqual(crs_summary.neutralize_mentions("@owner"), "@\u200bowner")

    def test_retries_transient_gateway_error(self):
        error = urllib.error.HTTPError(
            "http://example/responses", 502, "bad gateway", {}, io.BytesIO()
        )
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = json_bytes(
            {"output_text": "summary", "usage": {"input_tokens": 10}}
        )
        opener = mock.MagicMock()
        opener.open.side_effect = [error, response]
        with mock.patch.dict(
            os.environ,
            {
                "CRS_OPENAI_API_KEY": "test-key",
                "CRS_OPENAI_BASE_URL": "http://example",
            },
        ), mock.patch.object(
            crs_summary.urllib.request, "build_opener", return_value=opener
        ), mock.patch.object(crs_summary.time, "sleep") as sleep:
            text, usage = crs_summary.request_summary("prompt")
        self.assertEqual(text, "summary")
        self.assertEqual(usage["input_tokens"], 10)
        self.assertEqual(opener.open.call_count, 2)
        sleep.assert_called_once_with(1)


def json_bytes(value):
    return json.dumps(value).encode()


if __name__ == "__main__":
    unittest.main()
