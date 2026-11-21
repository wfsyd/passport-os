// Host test: NetEase failure logs show only the endpoint path, never the query.
#include "apps/netease/netease_endpoint.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using netease::endpoint_path;

int main() {
    char path[96];
    endpoint_path("https://music.163.com/api/nuser/account/get", path, sizeof(path));
    assert(!std::strcmp(path, "/api/nuser/account/get"));
    // Query values (uid, offset) and cookie-like tokens are dropped.
    endpoint_path("http://music.163.com/api/user/playlist?uid=42&limit=101&offset=0", path, sizeof(path));
    assert(!std::strcmp(path, "/api/user/playlist"));
    endpoint_path("http://music.163.com/api/song/enhance/player/url?ids=%5B1%5D&br=128000", path, sizeof(path));
    assert(!std::strcmp(path, "/api/song/enhance/player/url"));
    endpoint_path("https://interfacepc.music.163.com/eapi/song/enhance/player/url/v1", path, sizeof(path));
    assert(!std::strcmp(path, "/eapi/song/enhance/player/url/v1"));
    endpoint_path("http://music.163.com/api/v6/playlist/detail?id=7&n=0#frag", path, sizeof(path));
    assert(!std::strcmp(path, "/api/v6/playlist/detail"));
    // No path, null url, and truncation.
    endpoint_path("http://music.163.com", path, sizeof(path));
    assert(path[0] == 0);
    endpoint_path(nullptr, path, sizeof(path));
    assert(path[0] == 0);
    char small[6];
    endpoint_path("http://h/api/abcdef", small, sizeof(small));
    assert(!std::strcmp(small, "/api/"));
    std::puts("netease endpoint path: PASS");
    return 0;
}
