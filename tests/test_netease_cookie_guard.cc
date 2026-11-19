// Host test: the login cookie must never be sent over plain http://.
#include "apps/netease/netease_cookie_guard.h"

#include <cassert>
#include <cstdio>

using netease::cookie_forbidden_on_http;

int main() {
    const char *cookie = "os=pc; MUSIC_U=secret123; channel=netease";
    // Plain HTTP with the login token: forbidden.
    assert(cookie_forbidden_on_http("http://music.163.com/api/user/playlist?uid=1", cookie));
    assert(cookie_forbidden_on_http("http://music.163.com/api/v6/playlist/detail?id=1&n=0", "MUSIC_U=x"));
    // HTTPS with the login token: allowed.
    assert(!cookie_forbidden_on_http("https://music.163.com/api/user/playlist?uid=1", cookie));
    assert(!cookie_forbidden_on_http("https://interfacepc.music.163.com/eapi/v3/discovery", cookie));
    // Plain HTTP without the login token (anonymous metadata): allowed.
    assert(!cookie_forbidden_on_http("http://music.163.com/api/song/lyric?id=1&lv=1", nullptr));
    assert(!cookie_forbidden_on_http("http://music.163.com/api/cloudsearch/pc", ""));
    assert(!cookie_forbidden_on_http("http://music.163.com/api/x", "os=pc; channel=netease"));
    // Null url guard.
    assert(!cookie_forbidden_on_http(nullptr, cookie));
    std::puts("netease cookie guard: PASS");
    return 0;
}
