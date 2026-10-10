"""Offline regression checks; no GitHub writes or paid model requests."""

import json
import unittest
from datetime import datetime, timezone
from unittest.mock import Mock, patch

import repo_status


class ReportTests(unittest.TestCase):
    def setUp(self):
        writer = patch("repo_status.Path.write_text")
        self.response_file = writer.start()
        self.addCleanup(writer.stop)

    @patch("repo_status.request_json")
    def test_rejected_response_is_saved_with_specific_diagnostics(
        self, request
    ):
        for content, reason, expected in (
            ("中文" * 100, "length", "output incomplete.*length"),
            ("An English report", "stop", "language check failed.*Chinese=0"),
        ):
            with self.subTest(reason=reason):
                request.reset_mock()
                request.return_value = {
                    "choices": [
                        {
                            "finish_reason": reason,
                            "message": {"content": content},
                        }
                    ],
                    "usage": {"total_tokens": 100},
                }
                with self.assertRaisesRegex(RuntimeError, expected):
                    repo_status.summarize({}, "secret-test-key")
                request.assert_called_once()
                saved = self.response_file.call_args.args[0]
                self.assertNotIn("secret-test-key", saved)
                diagnostic = json.loads(saved)
                self.assertEqual(diagnostic["content"], content)
                self.assertEqual(diagnostic["finish_reason"], reason)
                self.assertEqual(diagnostic["usage"], {"total_tokens": 100})

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

    @patch("repo_status.request_json")
    def test_mixed_language_report_must_be_predominantly_chinese(
        self, request
    ):
        request.return_value = {
            "choices": [
                {
                    "finish_reason": "stop",
                    "message": {
                        "content": "中文" * 10
                        + " This is an English report." * 20
                    },
                }
            ]
        }
        with self.assertRaises(RuntimeError):
            repo_status.summarize({}, "test")
        request.assert_called_once()

    @patch("repo_status.request_json")
    def test_chinese_report_allows_code_and_links(self, request):
        content = (
            "今天仓库的重要变更已经合并，仍需核对最新提交的测试结果。"
            + " `"
            + "TechnicalIdentifier" * 20
            + "` "
            + "https://github.com/organization/repository/pull/123"
        )
        request.return_value = {
            "choices": [
                {"finish_reason": "stop", "message": {"content": content}}
            ]
        }
        self.assertEqual(repo_status.summarize({}, "test"), content)

    def test_render_lists_releases_and_neutralizes_all_copied_mentions(self):
        now = datetime(2026, 10, 10, 1, tzinfo=timezone.utc)
        data = {
            "window_start": "2026-10-09T01:00:00Z",
            "counts": dict.fromkeys(
                [
                    "merged_prs",
                    "new_prs",
                    "updated_prs",
                    "new_issues",
                    "updated_issues",
                ],
                0,
            ),
            "pull_requests": [
                {
                    "number": 1,
                    "title": "Ask @reviewer",
                    "state": "open",
                    "html_url": "https://github.com/o/r/pull/1",
                }
            ],
            "issues": [],
            "releases": [
                {
                    "tag_name": "v1",
                    "name": "v1 [stable] @org/team",
                    "html_url": "https://github.com/o/r/releases/tag/v1",
                }
            ],
            "coverage_gaps": [],
        }
        _, body = repo_status.render(
            data, "请@codex review；联系 user@example.com", now
        )
        self.assertIn("请@\u200bcodex", body)
        self.assertIn("@\u200breviewer", body)
        self.assertIn("@\u200borg/team", body)
        self.assertIn("user@example.com", body)
        self.assertIn("release [v1 \\[stable\\]", body)
        self.assertIn("https://github.com/o/r/releases/tag/v1", body)

    def test_publish_neutralizes_mentions_on_create_and_update(self):
        today = {
            "number": 3,
            "title": "[repo-status] 仓库日报：2026-10-10",
            "user": {"login": "github-actions[bot]"},
            "html_url": "https://github.com/o/r/issues/3",
        }
        for existing in ([], [today]):
            with self.subTest(existing=bool(existing)):
                github = Mock()
                github.pages.return_value = existing
                github.call.return_value = today
                repo_status.publish(
                    github, today["title"], "请@user 和 @org/team 查看"
                )
                body = github.call.call_args.args[1]["body"]
                self.assertEqual(body, "请@\u200buser 和 @\u200borg/team 查看")
                self.assertEqual(repo_status.neutralize_mentions(body), body)
                github.call.assert_called_once()

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

    def test_cleanup_preserves_future_same_day_and_undated_reports(self):
        today = {
            "number": 3,
            "title": "[repo-status] 仓库日报：2026-10-10",
            "user": {"login": "github-actions[bot]"},
        }
        yesterday = dict(
            today, number=2, title="[repo-status] 仓库日报：2026-10-09"
        )
        future = dict(
            today, number=4, title="[repo-status] 仓库日报：2026-10-11"
        )
        duplicate = dict(today, number=5)
        legacy = dict(today, number=6, title="[repo-status] legacy")
        invalid = dict(
            today, number=7, title="[repo-status] 仓库日报：2026-02-30"
        )
        github = Mock()
        github.pages.return_value = [
            future,
            today,
            yesterday,
            duplicate,
            legacy,
            invalid,
        ]
        github.call.side_effect = [
            dict(today, html_url="https://github.com/o/r/issues/3"),
            {},
        ]
        repo_status.publish(github, today["title"], "中文日报")
        self.assertEqual(
            [call.args[0] for call in github.call.call_args_list],
            ["issues/3", "issues/2"],
        )


if __name__ == "__main__":
    unittest.main()
