// Name-based AU presets: pick the highest-voted working MP3 from a mocked
// radio-browser response, fed through the same streamed JSON splitter the
// device uses.
#include "apps/radio/radio_json_stream.h"
#include "apps/radio/radio_search_pick.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Mocked response for countrycode=AU&name=triple j (already sorted by votes, as the API does).
const std::string kTripleJ = R"([
 {"name":"triple j Sydney","url_resolved":"http://a.example/aac","codec":"AAC","votes":9000,"lastcheckok":1,"hls":0},
 {"name":"triple j HLS","url_resolved":"https://a.example/hls.m3u8","codec":"MP3","votes":8000,"lastcheckok":1,"hls":1},
 {"name":"triple j broken","url_resolved":"http://a.example/broken.mp3","codec":"MP3","votes":7000,"lastcheckok":0,"hls":0},
 {"name":"Other station","url_resolved":"http://a.example/other.mp3","codec":"MP3","votes":6000,"lastcheckok":1,"hls":0},
 {"name":"Triple J Melbourne","url":"http://a.example/mel","codec":"MP3","votes":1200,"lastcheckok":1,"hls":0},
 {"name":"triple j Hobart","url_resolved":"https://a.example/hobart.mp3","codec":"MP3","votes":3100,"lastcheckok":1,"hls":false},
 {"name":"triple j Perth","url_resolved":"http://a.example/perth","codec":"","votes":5000,"lastcheckok":1,"hls":0},
 {"name":"triple j tie","url_resolved":"http://a.example/tie.mp3","codec":"MP3","votes":3100,"lastcheckok":1,"hls":0}
])";

// Mocked response for ABC Jazz: nothing matches the name, so the preset is absent.
const std::string kNoMatch = R"([
 {"name":"ABC Classic","url_resolved":"http://a.example/classic.mp3","codec":"MP3","votes":500,"lastcheckok":1,"hls":0}
])";

// Mocked response with a codec-less .mp3 URL that is the only qualifying entry.
const std::string kCodecless = R"([
 {"name":"ABC Jazz","url_resolved":"http://a.example/jazz.mp3","codec":"","votes":40,"lastcheckok":1,"hls":0}
])";

RadioSearchBest pick(const std::string &body, const char *wanted, std::size_t chunk) {
    RadioSearchBest best = {};
    char object[8192];
    RadioJsonStream parser(object, sizeof(object));
    for (std::size_t off = 0; off < body.size(); off += chunk) {
        const std::size_t n = std::min(chunk, body.size() - off);
        parser.Feed(body.data() + off, n, [&](const char *json) {
            radio_search_consider(json, wanted, &best);
        });
    }
    assert(parser.Complete());
    return best;
}

} // namespace

int main() {
    // Highest-voted MP3 that is working, not HLS, has a usable URL and matches the name:
    // Hobart (3100) comes first in the response; the equal-vote "tie" entry must not replace it.
    for (std::size_t chunk : {1u, 7u, 512u}) {
        const RadioSearchBest best = pick(kTripleJ, "triple j", chunk);
        assert(best.found);
        assert(std::strcmp(best.url, "https://a.example/hobart.mp3") == 0);
        assert(best.votes == 3100);
    }

    // Name matching is case-insensitive and substring-based, but not fuzzy.
    assert(!pick(kNoMatch, "ABC Jazz", 64).found);
    assert(pick(kNoMatch, "abc classic", 64).found);

    // No codec field: accepted only when the URL ends in .mp3.
    const RadioSearchBest jazz = pick(kCodecless, "ABC Jazz", 3);
    assert(jazz.found && std::strcmp(jazz.url, "http://a.example/jazz.mp3") == 0);

    // Codec order: MP3 beats direct AAC even with fewer votes; HLS AAC is last resort.
    const std::string kCodecOrder = R"([
     {"name":"ABC Jazz AAC hls","url_resolved":"https://a.example/jazz.m3u8","codec":"AAC","votes":9000,"lastcheckok":1,"hls":1},
     {"name":"ABC Jazz AAC","url_resolved":"http://a.example/jazz.aac","codec":"AAC","votes":4000,"lastcheckok":1,"hls":0},
     {"name":"ABC Jazz MP3 low","url_resolved":"http://a.example/jazz-low.mp3","codec":"MP3","votes":10,"lastcheckok":1,"hls":0}
    ])";
    assert(std::strcmp(pick(kCodecOrder, "ABC Jazz", 40).url, "http://a.example/jazz-low.mp3") == 0);
    const std::string kAacThenHls = R"([
     {"name":"ABC Radio National HLS","url_resolved":"https://a.example/rn.m3u8","codec":"AAC","votes":9000,"lastcheckok":1,"hls":1},
     {"name":"ABC Radio National","url_resolved":"http://a.example/rn.aac","codec":"AAC","votes":4000,"lastcheckok":1,"hls":0}
    ])";
    const RadioSearchBest rn = pick(kAacThenHls, "ABC Radio National", 33);
    assert(rn.found && rn.tier == 1 && std::strcmp(rn.url, "http://a.example/rn.aac") == 0);
    const std::string kHlsOnly = R"([
     {"name":"ABC Radio National HLS","url_resolved":"https://a.example/rn.m3u8","codec":"AAC","votes":9000,"lastcheckok":1,"hls":1},
     {"name":"ABC Radio National HLS MP3","url_resolved":"https://a.example/rn2.m3u8","codec":"MP3","votes":8000,"lastcheckok":1,"hls":1}
    ])";
    const RadioSearchBest rn_hls = pick(kHlsOnly, "ABC Radio National", 50);
    assert(rn_hls.found && rn_hls.tier == 2 && std::strcmp(rn_hls.url, "https://a.example/rn.m3u8") == 0);

    // Bitrate above 128 kbps is excluded; unknown (0 or absent) is accepted.
    const std::string kBitrate = R"([
     {"name":"triple j loud","url_resolved":"http://a.example/loud.mp3","codec":"MP3","bitrate":192,"votes":9999,"lastcheckok":1,"hls":0},
     {"name":"triple j aac hi","url_resolved":"http://a.example/hi.aac","codec":"AAC","bitrate":256,"votes":9998,"lastcheckok":1,"hls":0},
     {"name":"triple j ok","url_resolved":"http://a.example/ok.mp3","codec":"MP3","bitrate":128,"votes":5,"lastcheckok":1,"hls":0}
    ])";
    assert(std::strcmp(pick(kBitrate, "triple j", 25).url, "http://a.example/ok.mp3") == 0);
    const std::string kUnknownBitrate = R"([
     {"name":"triple j unknown","url_resolved":"http://a.example/u.mp3","codec":"MP3","bitrate":0,"votes":1,"lastcheckok":1,"hls":0}
    ])";
    assert(pick(kUnknownBitrate, "triple j", 25).found);

    // Invalid input and empty results do not select anything.
    RadioSearchBest empty = {};
    assert(!radio_search_consider("[]", "triple j", &empty) && !empty.found);
    assert(!radio_search_consider("{not json", "triple j", &empty) && !empty.found);
    assert(!radio_search_consider(nullptr, "triple j", &empty) && !empty.found);

    std::puts("Radio search presets: codec tiers, highest votes, bitrate cap, name match, filters: PASS");
    return 0;
}
