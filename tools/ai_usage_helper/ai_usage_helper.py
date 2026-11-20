#!/usr/bin/env python3
"""Serve local Claude Code and OpenAI Codex CLI usage as a tiny JSON document.

Reads only local log files (Claude Code JSONL transcripts and Codex CLI
rollout files). It makes no network calls to any provider and never reads
credential files. Standard library only, Python 3.9+.

Usage:
  python ai_usage_helper.py once
  python ai_usage_helper.py serve --port 8787 [--bind 0.0.0.0] [--token SECRET]
"""
from __future__ import annotations

import argparse
import hmac
import json
import math
import os
import re
import stat
import sys
import threading
import time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

VERSION = 1
SCAN_DAYS = 8            # only files modified in this many days are read
BLOCK_HOURS = 5          # Claude Code usage block length
CACHE_SECONDS = 60       # recompute at most this often
WEEKDAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")  # fixed, locale-free
_FRACTION_RE = re.compile(r"^(.*T\d{2}:\d{2}:\d{2})\.(\d+)(.*)$")


# ---------------------------------------------------------------- helpers

def parse_ts(value):
    """Parse an ISO-8601 timestamp into an aware datetime, or None."""
    if not isinstance(value, str) or not value.strip():
        return None
    text = value.strip()
    if text[-1] in "zZ":
        text = text[:-1] + "+00:00"
    match = _FRACTION_RE.match(text)
    if match:  # Python 3.9 only accepts 3 or 6 fractional digits
        frac = (match.group(2) + "000000")[:6]
        text = "%s.%s%s" % (match.group(1), frac, match.group(3))
    try:
        dt = datetime.fromisoformat(text)
    except ValueError:
        return None
    if dt.tzinfo is None:
        dt = dt.astimezone()  # naive stamps are treated as local time
    return dt


def _is_num(value):
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value))


def _count(value):
    return int(value) if _is_num(value) and value > 0 else 0


def _hhmm(dt, tz):
    local = dt.astimezone(tz)
    return "%02d:%02d" % (local.hour, local.minute)


def read_jsonl(path):
    """Yield each line that parses as a JSON object; skip everything else."""
    try:
        with open(str(path), "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except ValueError:
                    continue
                if isinstance(obj, dict):
                    yield obj
    except OSError:
        return


def _unique_dirs(candidates):
    out, seen = [], set()
    for cand in candidates:
        try:
            if not cand.is_dir():
                continue
            key = str(cand.resolve())
        except OSError:
            continue
        if key not in seen:
            seen.add(key)
            out.append(cand)
    return out


def default_claude_roots(env=None, home=None):
    env = os.environ if env is None else env
    home = Path(home) if home else Path.home()
    cands = []
    cfg = (env.get("CLAUDE_CONFIG_DIR") or "").strip()
    if cfg:
        cands.append(Path(cfg).expanduser() / "projects")
    cands.append(home / ".claude" / "projects")
    cands.append(home / ".config" / "claude" / "projects")
    return _unique_dirs(cands)


def default_codex_roots(env=None, home=None):
    env = os.environ if env is None else env
    home = Path(home) if home else Path.home()
    cands = []
    codex_home = (env.get("CODEX_HOME") or "").strip()
    if codex_home:
        cands.append(Path(codex_home).expanduser() / "sessions")
    cands.append(home / ".codex" / "sessions")
    return _unique_dirs(cands)


def recent_files(roots, pattern, cutoff_ts):
    """Matching regular files under roots, modified at or after cutoff_ts."""
    found = set()
    for root in roots:
        try:
            for path in root.rglob(pattern):
                try:
                    st = path.stat()
                except OSError:
                    continue
                if stat.S_ISREG(st.st_mode) and st.st_mtime >= cutoff_ts:
                    found.add(path)
        except OSError:
            continue
    return sorted(found)


# ---------------------------------------------------------------- Claude Code

def claude_entries(paths):
    """Return (utc datetime, tokens) for each unique assistant usage record."""
    entries, seen = [], set()
    for path in paths:
        for obj in read_jsonl(path):
            msg = obj.get("message")
            if not isinstance(msg, dict):
                continue
            usage = msg.get("usage")
            if not isinstance(usage, dict):
                continue
            dt = parse_ts(obj.get("timestamp"))
            if dt is None:
                continue
            msg_id = msg.get("id")
            if isinstance(msg_id, str) and msg_id:
                req_id = obj.get("requestId")
                key = (msg_id, req_id if isinstance(req_id, str) else "")
                if key in seen:  # streamed duplicates repeat the same message
                    continue
                seen.add(key)
            tokens = sum(_count(usage.get(k)) for k in (
                "input_tokens", "output_tokens",
                "cache_creation_input_tokens", "cache_read_input_tokens"))
            entries.append((dt, tokens))
    return entries


def summarize_claude(paths, now, tz=None):
    entries = sorted(claude_entries(paths), key=lambda e: e[0])
    if not entries:
        return {}
    today = now.astimezone(tz).date()
    tok_today = 0
    tok_block = 0
    block_start = block_end = None
    prev = None
    for dt, tokens in entries:
        if dt.astimezone(tz).date() == today:
            tok_today += tokens
        # A new block starts at the first message after a gap of 5 h or more,
        # or at the first message after the current block has ended.
        if block_end is None or dt >= block_end or dt - prev >= timedelta(hours=BLOCK_HOURS):
            block_start = dt.astimezone(tz).replace(minute=0, second=0, microsecond=0)
            block_end = block_start + timedelta(hours=BLOCK_HOURS)
            tok_block = 0
        tok_block += tokens
        prev = dt
    active = block_start <= now < block_end
    out = {
        "tok_today": tok_today,
        "tok_5h": tok_block if active else 0,
        "last": _hhmm(entries[-1][0], tz),
    }
    if active:
        out["reset"] = _hhmm(block_end, tz)
    return out


# ---------------------------------------------------------------- Codex CLI

def _token_node(obj):
    """Return the event dict if obj is a token_count or rate-limit event."""
    payload = obj.get("payload")
    node = payload if isinstance(payload, dict) else obj
    if node.get("type") == "token_count" or obj.get("type") == "token_count":
        return node
    if isinstance(node.get("rate_limits"), dict):
        return node
    return None


def _total_tokens(usage):
    if _is_num(usage.get("total_tokens")):
        return _count(usage.get("total_tokens"))
    return _count(usage.get("input_tokens")) + _count(usage.get("output_tokens"))


def _window(win, base):
    """Return (used_percent or None, reset datetime or None) for one window."""
    if not isinstance(win, dict):
        return None, None
    pct = round(win["used_percent"]) if _is_num(win.get("used_percent")) else None
    reset = None
    try:
        at = win.get("resets_at")
        if _is_num(at):
            reset = datetime.fromtimestamp((at / 1000.0) if at > 1e11 else at, timezone.utc)
        elif isinstance(at, str):
            reset = parse_ts(at)
        elif _is_num(win.get("resets_in_seconds")):
            reset = base + timedelta(seconds=win["resets_in_seconds"])
    except (OverflowError, OSError, ValueError):
        reset = None
    return pct, reset


def _file_mtime(path):
    try:
        return datetime.fromtimestamp(path.stat().st_mtime, timezone.utc)
    except (OSError, OverflowError, ValueError):
        return None


def summarize_codex(paths, now, tz=None):
    today = now.astimezone(tz).date()
    tok_today = 0
    seen_tokens = False
    rl_best = None  # (datetime, rate_limits dict) of the latest event
    for path in paths:
        fallback = _file_mtime(path)
        prev_total = None
        for obj in read_jsonl(path):
            node = _token_node(obj)
            if node is None:
                continue
            dt = parse_ts(obj.get("timestamp")) or parse_ts(node.get("timestamp")) or fallback
            info = node.get("info") if isinstance(node.get("info"), dict) else {}
            last_use = info.get("last_token_usage")
            total_use = info.get("total_token_usage")
            tokens = None
            if isinstance(last_use, dict):
                tokens = _total_tokens(last_use)
            if isinstance(total_use, dict):
                cur = _total_tokens(total_use)
                if tokens is None:  # no per-event usage: use the running total
                    if prev_total is None or cur < prev_total:
                        tokens = cur
                    else:
                        tokens = cur - prev_total
                prev_total = cur
            if tokens is not None:
                seen_tokens = True
                if dt is not None and dt.astimezone(tz).date() == today:
                    tok_today += tokens
            rl = node.get("rate_limits")
            if dt is not None and isinstance(rl, dict):
                if rl_best is None or dt >= rl_best[0]:
                    rl_best = (dt, rl)
    if rl_best is None and not seen_tokens:
        return {}
    out = {}
    if rl_best is not None:
        base, rl = rl_best
        pct, reset = _window(rl.get("primary"), base)
        if reset is not None and reset <= now:  # the 5 h window has already reset
            pct, reset = 0, None
        if pct is not None:
            out["p5h"] = pct
        if reset is not None:
            out["reset"] = _hhmm(reset, tz)
        pct, reset = _window(rl.get("secondary"), base)
        if reset is not None and reset <= now:
            pct, reset = 0, None
        if pct is not None:
            out["pwk"] = pct
        if reset is not None:
            local = reset.astimezone(tz)
            out["reset_wk"] = "%s %s" % (WEEKDAYS[local.weekday()], _hhmm(reset, tz))
    out["tok_today"] = tok_today
    return out


# ---------------------------------------------------------------- payload

def compute(now=None, tz=None, claude_roots=None, codex_roots=None):
    """Build the usage payload. tz=None means the PC's local time zone."""
    now = datetime.now(timezone.utc) if now is None else now
    cutoff = now.timestamp() - SCAN_DAYS * 86400
    if claude_roots is None:
        claude_roots = default_claude_roots()
    if codex_roots is None:
        codex_roots = default_codex_roots()
    payload = {"v": VERSION, "t": int(now.timestamp())}
    claude = summarize_claude(recent_files(claude_roots, "*.jsonl", cutoff), now, tz)
    codex = summarize_codex(recent_files(codex_roots, "rollout-*.jsonl", cutoff), now, tz)
    if claude:
        payload["claude"] = claude
    if codex:
        payload["codex"] = codex
    return payload


def encode(payload):
    return json.dumps(payload, separators=(",", ":")).encode("ascii")


class UsageCache:
    """Runs the producer at most once per ttl; keeps the last good body on failure."""

    def __init__(self, producer, ttl=CACHE_SECONDS, clock=time.monotonic):
        self._producer = producer
        self._ttl = ttl
        self._clock = clock
        self._lock = threading.Lock()
        self._body = None
        self._at = 0.0

    def get(self):
        with self._lock:
            now = self._clock()
            if self._body is None or now - self._at >= self._ttl:
                try:
                    self._body = encode(self._producer())
                except Exception as exc:  # never leak paths or data to the client
                    print("usage scan failed: %s" % type(exc).__name__, file=sys.stderr)
                    if self._body is None:
                        self._body = encode({"v": VERSION, "t": int(time.time())})
                self._at = now
            return self._body


# ---------------------------------------------------------------- HTTP

def make_server(bind, port, token, cache):
    class Handler(BaseHTTPRequestHandler):
        server_version = "AIUsageHelper"
        sys_version = ""
        timeout = 10

        def do_GET(self):
            parts = urlsplit(self.path)
            if parts.path != "/usage.json":
                self._send(404, b"not found\n", "text/plain; charset=utf-8")
                return
            if token is not None:
                given = parse_qs(parts.query).get("t", [""])[0]
                if not hmac.compare_digest(given.encode("utf-8"), token.encode("utf-8")):
                    self._send(403, b"forbidden\n", "text/plain; charset=utf-8")
                    return
            self._send(200, cache.get(), "application/json")

        def _send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):  # the request line may carry ?t=SECRET
            return

    return ThreadingHTTPServer((bind, port), Handler)


# ---------------------------------------------------------------- CLI

def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("once", help="print the usage JSON once and exit")
    serve = sub.add_parser("serve", help="serve GET /usage.json over HTTP")
    serve.add_argument("--port", type=int, default=8787)
    serve.add_argument("--bind", default="127.0.0.1",
                       help="address to listen on (use 0.0.0.0 for the LAN)")
    serve.add_argument("--token", default=None,
                       help="require ?t=TOKEN on /usage.json")
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.cmd == "once":
        print(encode(compute()).decode("ascii"))
        return 0
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.token is not None and not args.token:
        parser.error("--token must not be empty")
    server = make_server(args.bind, args.port, args.token, UsageCache(compute))
    host, port = server.server_address[0], server.server_address[1]
    print("Serving http://%s:%d/usage.json" % (host, port), file=sys.stderr)
    if args.token is not None:
        print("Token required: append ?t=YOUR_TOKEN", file=sys.stderr)
    elif host not in ("127.0.0.1", "::1", "localhost"):
        print("WARNING: no --token; anyone who can reach this port can read the counts",
              file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
