// Safety rule for the NetEase login cookie: MUSIC_U must never travel over
// plain http://. Kept header-only and free of ESP-IDF includes so host tests
// can check it directly.
#pragma once

#include <cstring>

namespace netease {

// True when a request to url must be sent without the given Cookie header value:
// the URL is plain HTTP and the cookie carries the login token.
inline bool cookie_forbidden_on_http(const char *url, const char *cookie) {
    if (!url || !cookie || !cookie[0]) return false;
    return std::strncmp(url, "http://", 7) == 0 && std::strstr(cookie, "MUSIC_U=") != nullptr;
}

}  // namespace netease
