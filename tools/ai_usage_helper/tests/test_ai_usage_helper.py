"""Tests for ai_usage_helper using fake fixture logs (no real user data)."""
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import ai_usage_helper as helper  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / "fixtures"
NOW = datetime(2026, 11, 20, 0, 30, tzinfo=timezone.utc)  # 10:30 in +10:00
TZ = timezone(timedelta(hours=10))
EXPECTED = {
    "v": 1,
    "t": int(NOW.timestamp()),
    "claude": {"tok_today": 2385, "tok_5h": 935, "reset": "11:00", "last": "09:00"},
    "codex": {"p5h": 18, "pwk": 42, "reset": "13:00", "reset_wk": "Mon 09:30",
              "tok_today": 4500},
}


def stage_fixtures(test):
    """Copy fixtures to a temp dir and give files mtimes relative to NOW."""
    tmp = tempfile.mkdtemp(prefix="ai-usage-test-")
    test.addCleanup(shutil.rmtree, tmp, True)
    dst = Path(tmp) / "fixtures"
    shutil.copytree(str(FIXTURES), str(dst))
    recent = NOW.timestamp() - 86400
    old = NOW.timestamp() - 10 * 86400
    for path in dst.rglob("*"):
        if path.is_file():
            os.utime(str(path), (recent, recent))
    stale = dst / "codex" / "sessions" / "2026" / "11" / "10"
    for path in stale.rglob("*.jsonl"):
        os.utime(str(path), (old, old))
    return dst


class ParseTests(unittest.TestCase):
    def test_timestamp_variants(self):
        want = datetime(2026, 11, 19, 1, 0, tzinfo=timezone.utc)
        self.assertEqual(helper.parse_ts("2026-11-19T01:00:00.000Z"), want)
        self.assertEqual(helper.parse_ts("2026-11-19T01:00:00Z"), want)
        self.assertEqual(helper.parse_ts("2026-11-19T01:00:00.123456+00:00"),
                         want.replace(microsecond=123456))
        self.assertEqual(helper.parse_ts("2026-11-19T11:00:00.1234567+10:00"),
                         want.replace(microsecond=123456))
        self.assertIsNotNone(helper.parse_ts("2026-11-19T01:00:00").tzinfo)

    def test_bad_timestamps(self):
        for bad in (None, "", "not-a-time", 12345):
            self.assertIsNone(helper.parse_ts(bad))


class ClaudeTests(unittest.TestCase):
    def setUp(self):
        self.root = stage_fixtures(self)
        self.paths = helper.recent_files(
            [self.root / "claude" / "projects"], "*.jsonl",
            NOW.timestamp() - helper.SCAN_DAYS * 86400)

    def test_summary_dedupes_and_blocks(self):
        out = helper.summarize_claude(self.paths, NOW, TZ)
        self.assertEqual(out, EXPECTED["claude"])

    def test_block_inactive_has_no_reset(self):
        later = datetime(2026, 11, 22, tzinfo=timezone.utc)
        out = helper.summarize_claude(self.paths, later, TZ)
        self.assertEqual(out["tok_5h"], 0)
        self.assertEqual(out["tok_today"], 0)
        self.assertNotIn("reset", out)
        self.assertEqual(out["last"], "09:00")

    def test_no_entries_gives_empty(self):
        self.assertEqual(helper.summarize_claude([], NOW, TZ), {})

    def test_default_roots_dedupe(self):
        tmp = Path(tempfile.mkdtemp(prefix="ai-usage-roots-"))
        self.addCleanup(shutil.rmtree, str(tmp), True)
        home = tmp / "home"
        (home / ".claude" / "projects").mkdir(parents=True)
        cfg = tmp / "cfg"
        (cfg / "projects").mkdir(parents=True)
        roots = helper.default_claude_roots({"CLAUDE_CONFIG_DIR": str(cfg)}, home)
        self.assertEqual(len(roots), 2)
        roots = helper.default_claude_roots({"CLAUDE_CONFIG_DIR": str(home / ".claude")}, home)
        self.assertEqual(len(roots), 1)


class CodexTests(unittest.TestCase):
    def setUp(self):
        self.root = stage_fixtures(self)
        self.paths = helper.recent_files(
            [self.root / "codex" / "sessions"], "rollout-*.jsonl",
            NOW.timestamp() - helper.SCAN_DAYS * 86400)

    def test_summary(self):
        self.assertEqual(helper.summarize_codex(self.paths, NOW, TZ), EXPECTED["codex"])

    def test_stale_file_excluded(self):
        names = [p.name for p in self.paths]
        self.assertFalse(any("ccc" in n for n in names))
        self.assertEqual(len(names), 2)

    def test_expired_windows_report_zero(self):
        later = datetime(2026, 11, 23, tzinfo=timezone.utc)
        out = helper.summarize_codex(self.paths, later, TZ)
        self.assertEqual(out, {"p5h": 0, "pwk": 0, "tok_today": 0})

    def test_default_codex_roots_honours_env(self):
        roots = helper.default_codex_roots({"CODEX_HOME": str(self.root / "codex")},
                                           self.root / "nohome")
        self.assertEqual(roots, [self.root / "codex" / "sessions"])


class PayloadTests(unittest.TestCase):
    def test_full_payload_matches_fixtures(self):
        root = stage_fixtures(self)
        payload = helper.compute(
            now=NOW, tz=TZ,
            claude_roots=[root / "claude" / "projects"],
            codex_roots=[root / "codex" / "sessions"])
        self.assertEqual(payload, EXPECTED)

    def test_payload_is_small_compact_json(self):
        body = helper.encode(EXPECTED)
        self.assertLess(len(body), 512)
        self.assertNotIn(b": ", body)
        self.assertNotIn(b", ", body)
        self.assertEqual(json.loads(body.decode("ascii")), EXPECTED)

    def test_no_sources_gives_minimal_payload(self):
        payload = helper.compute(now=NOW, tz=TZ,
                                 claude_roots=[Path("/nonexistent-a")],
                                 codex_roots=[Path("/nonexistent-b")])
        self.assertEqual(payload, {"v": 1, "t": int(NOW.timestamp())})


class CacheTests(unittest.TestCase):
    def test_producer_runs_at_most_once_per_ttl(self):
        clock = [0.0]
        calls = [0]

        def producer():
            calls[0] += 1
            return {"v": 1, "t": calls[0]}

        cache = helper.UsageCache(producer, ttl=60, clock=lambda: clock[0])
        cache.get()
        cache.get()
        self.assertEqual(calls[0], 1)
        clock[0] = 60
        self.assertIn(b'"t":2', cache.get())
        self.assertEqual(calls[0], 2)

    def test_failure_keeps_minimal_payload(self):
        def producer():
            raise RuntimeError("boom")

        cache = helper.UsageCache(producer, clock=lambda: 0.0)
        body = json.loads(cache.get().decode("ascii"))
        self.assertEqual(body["v"], 1)
        self.assertNotIn("claude", body)


class HttpTests(unittest.TestCase):
    def _serve(self, token):
        server = helper.make_server("127.0.0.1", 0, token,
                                    helper.UsageCache(lambda: EXPECTED))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return "http://127.0.0.1:%d" % server.server_address[1]

    @staticmethod
    def _get(url):
        try:
            with urllib.request.urlopen(url, timeout=5) as resp:
                return resp.status, resp.headers.get("Content-Type"), resp.read()
        except urllib.error.HTTPError as exc:
            return exc.code, None, exc.read()

    def test_token_required(self):
        base = self._serve("s3cret")
        status, ctype, body = self._get(base + "/usage.json?t=s3cret")
        self.assertEqual(status, 200)
        self.assertEqual(ctype, "application/json")
        self.assertEqual(json.loads(body.decode("ascii")), EXPECTED)
        self.assertEqual(self._get(base + "/usage.json")[0], 403)
        self.assertEqual(self._get(base + "/usage.json?t=wrong")[0], 403)
        self.assertEqual(self._get(base + "/other?t=s3cret")[0], 404)

    def test_no_token_mode(self):
        base = self._serve(None)
        self.assertEqual(self._get(base + "/usage.json")[0], 200)


if __name__ == "__main__":
    unittest.main()
