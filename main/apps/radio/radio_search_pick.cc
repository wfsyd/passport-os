#include "radio_search_pick.h"

#include "cJSON.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace {

bool starts_with(const char *text, const char *prefix) {
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

bool contains_ci(const char *text, const char *needle) {
    if (!needle[0]) return true;
    for (const char *p = text; *p; ++p) {
        std::size_t i = 0;
        while (needle[i] && p[i] &&
               std::tolower(static_cast<unsigned char>(p[i])) ==
                   std::tolower(static_cast<unsigned char>(needle[i]))) {
            ++i;
        }
        if (!needle[i]) return true;
    }
    return false;
}

const char *string_field(cJSON *object, const char *name) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

bool flag_field(cJSON *object, const char *name, bool fallback) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return cJSON_IsNumber(item) ? item->valueint != 0 : fallback;
}

long votes_field(cJSON *object) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "votes");
    return cJSON_IsNumber(item) ? static_cast<long>(item->valuedouble) : 0;
}

} // namespace

bool radio_search_consider(const char *station_json, const char *wanted, RadioSearchBest *best) {
    if (!station_json || !wanted || !best) return false;
    cJSON *entry = cJSON_Parse(station_json);
    if (!entry || !cJSON_IsObject(entry)) {
        cJSON_Delete(entry);
        return false;
    }
    const char *name = string_field(entry, "name");
    const char *url = string_field(entry, "url_resolved");
    if (!url[0]) url = string_field(entry, "url");
    const char *codec = string_field(entry, "codec");
    const bool hls = flag_field(entry, "hls", false) || std::strstr(url, ".m3u8");
    const bool aac = !strncasecmp(codec, "AAC", 3);
    // MP3 without a declared codec is accepted only for an .mp3 URL, as in the directory catalog.
    const bool mp3 = !strcasecmp(codec, "MP3") || (!codec[0] && std::strstr(url, ".mp3") && !hls);
    int tier = -1;
    if (hls && aac) tier = 2;
    else if (!hls && mp3) tier = 0;
    else if (!hls && aac) tier = 1;
    const cJSON *bitrate = cJSON_GetObjectItemCaseSensitive(entry, "bitrate");
    const bool bitrate_ok = !cJSON_IsNumber(bitrate) || bitrate->valuedouble <= 0 ||
                            bitrate->valuedouble <= 128;
    const long votes = votes_field(entry);
    const bool qualifies = tier >= 0 && bitrate_ok && name[0] && contains_ci(name, wanted) &&
                           flag_field(entry, "lastcheckok", true) &&
                           (starts_with(url, "http://") || starts_with(url, "https://")) &&
                           std::strlen(url) < sizeof(best->url);
    bool changed = false;
    if (qualifies && (!best->found || tier < best->tier ||
                      (tier == best->tier && votes > best->votes))) {
        std::snprintf(best->url, sizeof(best->url), "%s", url);
        best->votes = votes;
        best->tier = tier;
        best->found = true;
        changed = true;
    }
    cJSON_Delete(entry);
    return changed;
}
