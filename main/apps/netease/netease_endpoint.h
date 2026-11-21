// Endpoint path of a NetEase request for log lines. Scheme, host and query are
// dropped, so the login cookie and request tokens never reach the log. Kept
// header-only and free of ESP-IDF includes so host tests can check it directly.
#pragma once

#include <cstddef>
#include <cstring>

namespace netease {

// Writes the path of url (e.g. "/api/user/playlist") into out, truncated to capacity.
inline void endpoint_path(const char *url, char *out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = 0;
    if (!url) return;
    const char *host = std::strstr(url, "://");
    host = host ? host + 3 : url;
    const char *path = std::strchr(host, '/');
    if (!path) return;
    size_t n = 0;
    while (path[n] && path[n] != '?' && path[n] != '#' && n + 1 < capacity) {
        out[n] = path[n];
        ++n;
    }
    out[n] = 0;
}

}  // namespace netease
