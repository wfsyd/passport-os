#!/usr/bin/env python3
"""Probe stream candidates for the AU radio table and write third_party/au-radio/streams.csv.

Inputs:
  --candidates  CSV with columns name,url (several rows per name allowed, tried in order).
                Header only is fine. third_party/au-radio/stream_candidates.csv is the usual file.
  --stations    stations.csv (default third_party/au-radio/stations.csv). Names come from it.
  --area        optional area_id such as nsw-sydney. Default: every area.
  --radio-browser
                also look up each name on radio-browser.info (AU, most clicked, working only)
                and try the url_resolved values after the candidates.

For each station name in scope the candidates are tried in order. A candidate passes when:
  - the request (redirects followed, at most 5; Icy-MetaData: 0) returns Content-Type
    audio/mpeg, audio/aac or audio/aacp, or an HLS type (application/vnd.apple.mpegurl or
    application/x-mpegurl); and
  - the first 32 KB (for HLS, the first segment of the first media playlist) contains an
    MP3 frame sync, an ADTS sync or a TS sync (0x47 every 188 bytes).
  Each request must finish within 10 s. Text playlists (.pls, .m3u, or an HLS master) are
  never a pass. Their first stream URL is resolved and tested instead. For https URLs the
  http:// form is tried first and preferred when it passes.

Output: rows for passing names go to --out (default third_party/au-radio/streams.csv) with
the URL that passed, its codec (mp3, aac or hls) and today's date. Rows for names outside
--area are kept as they were. An in-scope name with no passing candidate has no row.
A summary table is printed: name | result | URL | reasons for failures.

Stdlib only. Exit status is 0 whatever the probe results; only bad command-line flags
(argparse) exit with 2.

Usage: python3 tools/probe_au_streams.py --candidates third_party/au-radio/stream_candidates.csv \\
           --area nsw-sydney --radio-browser
"""
import argparse
import csv
import datetime
import http.client
import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_STATIONS = ROOT / 'third_party/au-radio/stations.csv'
DEFAULT_OUT = ROOT / 'third_party/au-radio/streams.csv'
CANDIDATE_COLUMNS = ['name', 'url']
STREAM_COLUMNS = ['name', 'url', 'codec', 'checked']
READ_BYTES = 32 * 1024
TIMEOUT_S = 10.0
MAX_REDIRECTS = 5
MAX_PLAYLIST_DEPTH = 3
AUDIO_TYPES = ('audio/mpeg', 'audio/aac', 'audio/aacp')
HLS_TYPES = ('application/vnd.apple.mpegurl', 'application/x-mpegurl')
PLS_TYPES = ('audio/x-scpls', 'audio/scpls', 'audio/x-pls')
RADIO_BROWSER_URL = 'http://all.api.radio-browser.info/json/stations/search'
USER_AGENT = 'au-radio-table-probe/1.0'

MP3_SYNC = re.compile(rb'\xff[\xe0-\xff]')          # 11 set bits: 0xFFE? and 0xFFF?
ADTS_SYNC = re.compile(rb'\xff[\xf0\xf1\xf8\xf9]')  # 0xFFF with layer 00


class ProbeError(Exception):
    """A request or parse failure, with the reason shown in the summary."""


def sniff_audio(data):
    """Return (ok, kind) for the bytes of an audio response. kind is adts, mp3 or ts."""
    if ADTS_SYNC.search(data):
        return True, 'adts'
    if MP3_SYNC.search(data):
        return True, 'mp3'
    if has_ts_sync(data):
        return True, 'ts'
    return False, None


def has_ts_sync(data):
    """True when three or more consecutive 188-byte TS packets start with 0x47."""
    for start in range(min(188, len(data))):
        if data[start] != 0x47:
            continue
        count, pos = 0, start
        while pos < len(data) and data[pos] == 0x47:
            count += 1
            pos += 188
        if count >= 3:
            return True
    return False


def parse_playlist(text, base_url):
    """Return (kind, url) for a text playlist. kind is pls, m3u, hls-master or hls-media.

    pls and m3u give the first stream URL. hls-master gives its first variant, and
    hls-media gives its first segment. (None, None) when the text has no URL.
    """
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if any(line.lower() == '[playlist]' for line in lines):
        for line in lines:
            match = re.match(r'(?i)file\d+=(.+)$', line)
            if match:
                return 'pls', urllib.parse.urljoin(base_url, match.group(1).strip())
        return None, None
    uris = [line for line in lines if not line.startswith('#')]
    if not uris:
        return None, None
    target = urllib.parse.urljoin(base_url, uris[0])
    if any(line.startswith('#EXT-X-STREAM-INF') for line in lines):
        return 'hls-master', target
    if any(line.startswith('#EXT-X-') for line in lines):
        return 'hls-media', target
    return 'm3u', target


def looks_like_playlist(url, ctype, body):
    """True for a text playlist: by its first bytes, or by its type or extension."""
    if b'\0' in body[:1024]:
        return False
    head = body[:1024].decode('utf-8', 'replace').lstrip().lower()
    if head.startswith('#extm3u') or head.startswith('[playlist]'):
        return True
    path = urllib.parse.urlsplit(url).path.lower()
    return ctype in HLS_TYPES or ctype in PLS_TYPES or path.endswith(('.pls', '.m3u', '.m3u8'))


class _Redirects(urllib.request.HTTPRedirectHandler):
    max_redirections = MAX_REDIRECTS
    max_repeats = MAX_REDIRECTS


_OPENER = urllib.request.build_opener(_Redirects)


def fetch(url):
    """GET url and read up to READ_BYTES within TIMEOUT_S. Returns (final_url, content_type, body)."""
    deadline = time.monotonic() + TIMEOUT_S
    request = urllib.request.Request(url, headers={
        'Icy-MetaData': '0', 'User-Agent': USER_AGENT, 'Accept': '*/*'})
    try:
        response = _OPENER.open(request, timeout=TIMEOUT_S)
    except urllib.error.HTTPError as error:
        raise ProbeError(f'HTTP {error.code} {error.msg}'.strip())
    except urllib.error.URLError as error:
        raise ProbeError(f'connect failed: {error.reason}')
    except (OSError, ValueError, http.client.HTTPException) as error:
        raise ProbeError(f'request failed: {error}')
    with response:
        ctype = response.headers.get('Content-Type', '').split(';')[0].strip().lower()
        body = bytearray()
        try:
            while len(body) < READ_BYTES and time.monotonic() < deadline:
                chunk = response.read(min(4096, READ_BYTES - len(body)))
                if not chunk:
                    break
                body += chunk
        except (OSError, http.client.HTTPException) as error:
            if not body:
                raise ProbeError(f'read failed: {error}')
        return response.geturl(), ctype, bytes(body)


def probe(url, depth=0):
    """Return (ok, codec, stream_url, reason) for one URL, following playlists."""
    if depth > MAX_PLAYLIST_DEPTH:
        return False, None, url, 'too many nested playlists'
    try:
        final, ctype, body = fetch(url)
        if looks_like_playlist(final, ctype, body):
            kind, target = parse_playlist(body.decode('utf-8', 'replace'), final)
            if target is None:
                return False, None, url, f'{ctype or "playlist"} has no stream URL'
            if kind in ('pls', 'm3u', 'hls-master'):
                ok, codec, stream, reason = probe(target, depth + 1)
                return ok, codec, stream, reason if ok else f'playlist -> {reason}'
            # hls-media: the first segment must carry audio bytes.
            _, _, segment = fetch(target)
            ok, _ = sniff_audio(segment)
            if not ok:
                return False, None, url, 'HLS first segment has no MP3, ADTS or TS sync'
            return True, 'hls', url, 'ok'
        if ctype not in AUDIO_TYPES:
            return False, None, url, f'content-type {ctype or "(none)"} is not audio/mpeg, audio/aac, audio/aacp or HLS'
        ok, kind = sniff_audio(body)
        if not ok:
            return False, None, url, 'no MP3, ADTS or TS sync in the first 32 KB'
        if kind == 'ts':
            return True, 'hls', url, 'ok'
        return True, 'aac' if kind == 'adts' else 'mp3', url, 'ok'
    except ProbeError as error:
        return False, None, url, str(error)


def probe_candidate(url):
    """probe(), but an https URL is tried in its http:// form first and kept if that passes."""
    if not url.startswith('https://'):
        return probe(url)
    plain = 'http://' + url[len('https://'):]
    ok, codec, stream, reason = probe(plain)
    if ok:
        return ok, codec, stream, 'http form'
    ok, codec, stream, reason_https = probe(url)
    if ok:
        return ok, codec, stream, f'https only (http: {reason})'
    return False, None, url, f'http: {reason}; https: {reason_https}'


def radio_browser_urls(name):
    """Return (urls, error) for the AU stations named `name` on radio-browser.info."""
    query = urllib.parse.urlencode({'name': name, 'countrycode': 'AU', 'hidebroken': 'true',
                                    'order': 'clickcount', 'reverse': 'true', 'limit': 5})
    request = urllib.request.Request(f'{RADIO_BROWSER_URL}?{query}', headers={'User-Agent': USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_S) as response:
            entries = json.loads(response.read().decode('utf-8', 'replace'))
    except (OSError, ValueError, http.client.HTTPException) as error:
        return [], str(error)
    urls = []
    for entry in entries if isinstance(entries, list) else []:
        url = (entry.get('url_resolved') or '').strip()
        if url.startswith(('http://', 'https://')) and url not in urls:
            urls.append(url)
    return urls, None


def read_station_names(path, area):
    """Station names in table order, limited to one area_id when given."""
    with path.open(encoding='utf-8', newline='') as source:
        rows = list(csv.DictReader(source))
    if area is not None and not any(row['area_id'] == area for row in rows):
        raise ValueError(f'area {area} is not in {path}')
    names = []
    for row in rows:
        if (area is None or row['area_id'] == area) and row['name'] not in names:
            names.append(row['name'])
    return names, {row['name'] for row in rows}


def read_candidates(path):
    candidates = {}
    with path.open(encoding='utf-8', newline='') as source:
        reader = csv.DictReader(source)
        if reader.fieldnames != CANDIDATE_COLUMNS:
            raise ValueError(f'{path}: header must be {",".join(CANDIDATE_COLUMNS)}')
        for row in reader:
            name, url = row['name'].strip(), row['url'].strip()
            if name and url:
                urls = candidates.setdefault(name, [])
                if url not in urls:
                    urls.append(url)
    return candidates


def read_existing_streams(path):
    if not path.exists():
        return []
    with path.open(encoding='utf-8', newline='') as source:
        reader = csv.DictReader(source)
        if reader.fieldnames != STREAM_COLUMNS:
            return []
        return [row for row in reader]


def write_streams(path, names, scope, results, existing):
    """Write one row per name: fresh results for names in scope, existing rows elsewhere."""
    old = {row['name']: row for row in existing}
    today = datetime.date.today().isoformat()
    rows = []
    for name in names:
        if name in scope:
            if name in results and results[name][0]:
                _, codec, url = results[name]
                rows.append([name, url, codec, today])
        elif name in old:
            rows.append([old[name][c] for c in STREAM_COLUMNS])
    with path.open('w', encoding='utf-8', newline='') as target:
        writer = csv.writer(target, lineterminator='\n')
        writer.writerow(STREAM_COLUMNS)
        writer.writerows(rows)
    return len(rows)


def print_summary(rows):
    headers = ['name', 'result', 'URL', 'reasons for failures']
    widths = [max(len(str(r[i])) for r in rows + [headers]) for i in range(3)]
    line = '{:<%d}  {:<%d}  {:<%d}  {}' % tuple(widths)
    print(line.format(*headers))
    for row in rows:
        print(line.format(*row))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--candidates', type=Path, required=True, help='CSV with name,url rows')
    parser.add_argument('--stations', type=Path, default=DEFAULT_STATIONS)
    parser.add_argument('--area', default=None, help='area_id to probe, for example nsw-sydney')
    parser.add_argument('--radio-browser', action='store_true',
                        help='also try radio-browser.info url_resolved values for each name')
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = parser.parse_args(argv)
    try:
        names, all_names = read_station_names(args.stations, args.area)
        candidates = read_candidates(args.candidates)
        existing = read_existing_streams(args.out)
    except (OSError, ValueError, KeyError) as error:
        print(f'probe_au_streams: {error}', file=sys.stderr)
        return 0
    scope = set(names)
    results, table = {}, []
    for name in names:
        urls = list(candidates.get(name, []))
        notes = []
        if args.radio_browser:
            found, error = radio_browser_urls(name)
            urls += [url for url in found if url not in urls]
            if error:
                notes.append(f'radio-browser: {error}')
        if not urls:
            table.append([name, 'NO CANDIDATES', '-', '; '.join(notes) or 'no candidate URL'])
            continue
        failures, passed = [], None
        for url in urls:
            ok, codec, stream, reason = probe_candidate(url)
            if ok:
                passed = (stream, codec, reason)
                break
            failures.append(f'{url} ({reason})')
        if passed:
            results[name] = (True, passed[1], passed[0])
            table.append([name, f'PASS {passed[1]}', passed[0], '; '.join(notes + failures)])
        else:
            results[name] = (False, None, None)
            table.append([name, 'FAIL', '-', '; '.join(notes + failures)])
        print(f'{name}: {table[-1][1]}', file=sys.stderr)
    count = write_streams(args.out, all_names, scope, results, existing)
    print_summary(table)
    print(f'wrote {count} stream rows to {args.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
