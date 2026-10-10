"""Offline regression checks; no GitHub writes or paid model requests."""

import unittest
from datetime import datetime, timezone
from unittest.mock import Mock, patch

import repo_status


class ReportTests(unittest.TestCase):
    def test_pagination_advances_and_stops_at_cutoff(self):
        github = repo_status.GitHub("owner/repo", "test")
        github.call = Mock(
            side_effect=[
                [{"updated_at": "2026-10-10T01:00:00Z"}] * 100,
                [{"updated_at": "2026-10-09T00:00:00Z"}],
            ]
        )
        self.assertEqual(
            len(list(github.pages("pulls", cutoff="2026-10-09T01:00:00Z"))),
            100,
        )
        self.assertIn("page=1", github.call.call_args_list[0].args[0])
        self.assertIn("page=2", github.call.call_args_list[1].args[0])

    def test_pagination_limit_fails_closed(self):
        github = repo_status.GitHub("owner/repo", "test")
        github.call = Mock(return_value=[{}] * 100)
        with self.assertRaisesRegex(RuntimeError, "Pagination limit"):
            list(github.pages("issues"))
        self.assertEqual(github.call.call_count, repo_status.MAX_PAGES)

    @patch("repo_status.request_json")
    def test_single_model_call_without_tools(self, request):
        request.return_value = {
            "choices": [
                {
                    "finish_reason": "stop",
                    "message": {
                        "content": "今天仓库的重要变更已经合并，仍需核对最新提交的测试结果。"
                    },
                }
            ]
        }
        repo_status.summarize({}, "test")
        request.assert_called_once()
        self.assertNotIn("tools", request.call_args.args[2])

    @patch("repo_status.request_json", side_effect=RuntimeError("HTTP 429"))
    def test_model_failure_is_not_retried(self, request):
        with self.assertRaises(RuntimeError):
            repo_status.summarize({}, "test")
        request.assert_called_once()

    @patch("repo_status.request_json")
    def test_incomplete_or_english_response_is_rejected(self, request):
        for content, reason in (
            ("", "stop"),
            ("An English report", "stop"),
            ("中文" * 100, "length"),
        ):
            request.return_value = {
                "choices": [
                    {"finish_reason": reason, "message": {"content": content}}
                ]
            }
            with self.assertRaises(RuntimeError):
                repo_status.summarize({}, "test")

    def test_collect_filters_reports_and_deduplicates_prs(self):
        now = datetime(2026, 10, 10, 1, tzinfo=timezone.utc)
        item = {
            "number": 1,
            "title": "Fix CPU",
            "body": "details",
            "state": "closed",
            "html_url": "https://github.com/o/r/pull/1",
            "created_at": "2026-10-09T02:00:00Z",
            "updated_at": "2026-10-09T03:00:00Z",
            "merged_at": "2026-10-09T03:00:00Z",
            "user": {"login": "developer"},
            "head": {"sha": "exact-head"},
        }
        github = Mock()
        github.pages.side_effect = [
            [item],
            [
                dict(item, pull_request={}),
                dict(item, number=2, title="[repo-status] 仓库日报"),
            ],
            [],
            [],
        ]
        github.call.side_effect = [
            {
                "check_runs": [
                    {
                        "name": "CI",
                        "conclusion": "success",
                        "head_sha": "exact-head",
                    }
                ]
            },
            [],
        ]
        data = repo_status.collect(github, now)
        self.assertEqual(data["counts"]["merged_prs"], 1)
        self.assertEqual(data["issues"], [])
        self.assertEqual(
            data["pull_requests"][0]["head_checks"][0]["head_sha"],
            "exact-head",
        )

    def test_publish_updates_today_before_closing_old_bot_reports(self):
        github = Mock()
        today = {
            "number": 3,
            "title": "[repo-status] 仓库日报：2026-10-10",
            "user": {"login": "github-actions[bot]"},
        }
        yesterday = dict(
            today, number=2, title="[repo-status] 仓库日报：2026-10-09"
        )
        human = dict(yesterday, number=1, user={"login": "human"})
        github.pages.return_value = [today, yesterday, human]
        github.call.side_effect = [
            dict(today, html_url="https://github.com/o/r/issues/3"),
            {},
        ]
        repo_status.publish(github, today["title"], "中文日报")
        self.assertEqual(
            [call.args[0] for call in github.call.call_args_list],
            ["issues/3", "issues/2"],
        )
        self.assertEqual(
            github.call.call_args_list[1].args[1], {"state": "closed"}
        )

    def test_failed_creation_does_not_close_existing_reports(self):
        github = Mock()
        github.pages.return_value = [
            {
                "number": 2,
                "title": "[repo-status] old",
                "user": {"login": "github-actions[bot]"},
            }
        ]
        github.call.side_effect = RuntimeError("HTTP 503")
        with self.assertRaises(RuntimeError):
            repo_status.publish(github, "[repo-status] new", "中文日报")
        github.call.assert_called_once()


if __name__ == "__main__":
    unittest.main()
