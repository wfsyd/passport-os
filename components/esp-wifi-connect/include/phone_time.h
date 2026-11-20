#pragma once
#include <stdint.h>
#include <math.h>
#include <cJSON.h>

#include <string.h>

// Optional main-owned clock bridge; the Wi-Fi component has no main dependency.
extern "C" void passport_phone_time(int64_t epoch, int tz_minutes) __attribute__((weak));
extern "C" void passport_phone_zone(const char *iana) __attribute__((weak));
inline bool wifi_phone_time_parse(const cJSON *json, int64_t *epoch, int *minutes) {
    const auto *t = cJSON_GetObjectItemCaseSensitive(json, "epoch");
    const auto *z = cJSON_GetObjectItemCaseSensitive(json, "tz_minutes");
    if (!cJSON_IsNumber(t) || !cJSON_IsNumber(z)) return false;
    const double sec = t->valuedouble, tz = z->valuedouble;
    if (!isfinite(sec) || sec < 1700000000 || sec > 4102444800 || floor(sec) != sec ||
        !isfinite(tz) || tz < -720 || tz > 840 || floor(tz) != tz || (int)tz % 15) return false;
    *epoch = (int64_t)sec; *minutes = (int)tz;
    return true;
}
// Optional IANA zone name ("tz_zone", e.g. "Australia/Sydney"). Bounded copy:
// anything missing, non-string or too long becomes "" (fixed-offset mode).
inline void wifi_phone_zone_parse(const cJSON *json, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    const auto *z = cJSON_GetObjectItemCaseSensitive(json, "tz_zone");
    if (!cJSON_IsString(z) || !z->valuestring) return;
    const size_t n = strnlen(z->valuestring, cap);
    if (n == 0 || n >= cap) return;
    memcpy(out, z->valuestring, n);
    out[n] = '\0';
}
inline void wifi_phone_time_apply(const cJSON *json) {
    int64_t epoch; int minutes;
    if (passport_phone_time && wifi_phone_time_parse(json, &epoch, &minutes)) {
        passport_phone_time(epoch, minutes);
        if (passport_phone_zone) {
            char zone[64];
            wifi_phone_zone_parse(json, zone, sizeof(zone));
            passport_phone_zone(zone);
        }
    }
}
