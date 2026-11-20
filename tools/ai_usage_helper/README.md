# AI usage helper (PC side, for the ESP32 badge)

A small Python script that reads your **local** Claude Code and OpenAI Codex CLI
logs and serves a tiny JSON summary over HTTP on your home network. The badge
polls it. Standard library only; Python 3.9+ on Windows, macOS or Linux.

## What it reads

- Claude Code transcripts: `~/.claude/projects/**/*.jsonl` (also `CLAUDE_CONFIG_DIR`
  and `~/.config/claude/projects`).
- Codex CLI rollout files: `~/.codex/sessions/**/rollout-*.jsonl` (also `CODEX_HOME`).
- Only files modified in the last 8 days are read.

It never reads credential files, and it makes no network calls to Anthropic or OpenAI.

## Privacy

Only counts leave the PC: token totals, percentages, reset times and the last
activity time. No prompts, file names, project names or model replies are sent or
stored.

## Run it

```
python3 ai_usage_helper.py once                       # print the JSON once
python3 ai_usage_helper.py serve --port 8787 --bind 0.0.0.0 --token SECRET
```

The badge (or a browser on the LAN) fetches `http://PC-IP:8787/usage.json?t=SECRET`.
Without `--token` the endpoint is open to anyone who can reach the port, so use a
token when binding to `0.0.0.0`. Without `--bind 0.0.0.0` it only listens on
localhost. Responses are recomputed at most every 60 seconds.

Example output (fake data):

```
{"v":1,"t":1795134600,"claude":{"tok_today":885,"tok_5h":435,"reset":"11:00","last":"09:00"},"codex":{"p5h":18,"pwk":42,"reset":"13:00","reset_wk":"Mon 09:30","tok_today":4500}}
```

Keys: `claude.tok_today` / `tok_5h` (tokens in the current 5-hour block; input,
output and cache-creation tokens only, cache reads are left out because they
repeat the whole context on every turn), `reset`
(block end, local HH:MM), `last` (last activity). `codex.p5h` / `pwk` (used %
of the 5-hour and weekly windows), `reset` / `reset_wk` (local time), `tok_today`.
Missing values are left out.

## Run at login

**Windows (Task Scheduler):** Create task, trigger "At log on", action
`pythonw` with arguments `"C:\path\ai_usage_helper.py" serve --bind 0.0.0.0 --token SECRET`.

**macOS (launchd):** save as `~/Library/LaunchAgents/local.ai-usage-helper.plist`
with `ProgramArguments` = `python3`, the script path, `serve`, `--bind`, `0.0.0.0`,
`--token`, `SECRET`, and `RunAtLoad` = true. Then run
`launchctl load ~/Library/LaunchAgents/local.ai-usage-helper.plist`.

**Linux (systemd --user):** create `~/.config/systemd/user/ai-usage-helper.service`:

```
[Service]
ExecStart=/usr/bin/python3 /path/to/ai_usage_helper.py serve --bind 0.0.0.0 --token SECRET
Restart=on-failure
[Install]
WantedBy=default.target
```

Then run `systemctl --user daemon-reload && systemctl --user enable --now ai-usage-helper`.

## Firewall

Allow inbound TCP on the chosen port (8787 by default) for the private network only.
On Windows, the first run may prompt; choose "Private networks". Do not forward the
port on your router.

## Tests

```
python3 -m unittest discover -s tools/ai_usage_helper/tests
```

Tests use fake fixture logs in `tests/fixtures/`.
