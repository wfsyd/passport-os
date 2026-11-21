"""Offline tests for tools/probe_au_streams.py: byte sniffing, playlist parsing and the https order."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import probe_au_streams as probe  # noqa: E402

MP3 = b'\x00' * 20 + b'\xff\xfb\x90\x64' + b'\x00' * 50
ADTS = b'\x00' * 10 + b'\xff\xf1\x50\x80' + b'\x00' * 50
TS = (b'\x47' + b'\x00' * 187) * 4

# Byte sniffing.
assert probe.sniff_audio(MP3) == (True, 'mp3')
assert probe.sniff_audio(ADTS) == (True, 'adts')
assert probe.sniff_audio(TS) == (True, 'ts')
assert probe.sniff_audio(b'') == (False, None)
assert probe.sniff_audio(b'#EXTM3U\n#EXTINF:6,\nseg.ts\n') == (False, None)
assert probe.sniff_audio(b'<html>no stream</html>') == (False, None)
assert probe.sniff_audio((b'\x47' + b'\x00' * 187) * 2) == (False, None), 'two TS packets are not enough'
assert probe.sniff_audio(b'\x47' + b'\x00' * 100 + b'\x47') == (False, None), 'TS packets must be 188 apart'
assert probe.has_ts_sync(b'\x00' * 5 + TS), 'packets may start at any offset below 188'

# Playlist parsing: the first stream URL, resolved against the playlist URL.
assert probe.parse_playlist('[playlist]\nNumberOfEntries=2\nFile1=http://a.example/x.mp3\nFile2=http://b/y\n',
                            'http://p.example/s.pls') == ('pls', 'http://a.example/x.mp3')
assert probe.parse_playlist('[playlist]\nFile1=stream.aac\n', 'http://p.example/dir/s.pls') == \
    ('pls', 'http://p.example/dir/stream.aac')
assert probe.parse_playlist('#EXTM3U\n#EXTINF:-1,Name\nhttp://a.example/live\n', 'http://p.example/s.m3u') == \
    ('m3u', 'http://a.example/live')
assert probe.parse_playlist('#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=64000\nlo/index.m3u8\n',
                            'http://p.example/master.m3u8') == ('hls-master', 'http://p.example/lo/index.m3u8')
assert probe.parse_playlist('#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nseg0.ts\n',
                            'http://p.example/lo/index.m3u8') == ('hls-media', 'http://p.example/lo/seg0.ts')
assert probe.parse_playlist('#EXTM3U\n', 'http://p.example/empty.m3u') == (None, None)
assert probe.parse_playlist('[playlist]\nTitle=none\n', 'http://p.example/x.pls') == (None, None)

# Which bodies are playlists rather than audio.
assert probe.looks_like_playlist('http://p.example/s.pls', 'text/plain', b'[playlist]\nFile1=x\n')
assert probe.looks_like_playlist('http://p.example/s', 'application/vnd.apple.mpegurl', b'')
assert probe.looks_like_playlist('http://p.example/s.m3u', 'text/plain', b'http://a/x\n')
assert not probe.looks_like_playlist('http://p.example/s', 'audio/mpeg', MP3)
assert not probe.looks_like_playlist('http://p.example/s', 'audio/aac', ADTS)


# https URLs: the http:// form is tried first and kept when it passes.
calls = []


def fake_probe(url, depth=0):
    calls.append(url)
    if url.startswith('http://') and 'plain-ok' in url:
        return True, 'mp3', url, 'ok'
    if url.startswith('https://'):
        return True, 'aac', url, 'ok'
    return False, None, url, 'no'


real_probe = probe.probe
probe.probe = fake_probe
try:
    assert probe.probe_candidate('https://s.example/plain-ok') == (True, 'mp3', 'http://s.example/plain-ok', 'http form')
    assert calls == ['http://s.example/plain-ok']
    calls.clear()
    ok, codec, url, reason = probe.probe_candidate('https://s.example/only-tls')
    assert ok and codec == 'aac' and url == 'https://s.example/only-tls', (ok, codec, url)
    assert calls == ['http://s.example/only-tls', 'https://s.example/only-tls']
finally:
    probe.probe = real_probe

print('probe_au_streams: sniffing, playlist parsing, https order: PASS')
