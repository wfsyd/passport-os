// main/apps/voice/tz_zone.c —— IANA -> POSIX TZ 映射与偏移计算(纯 C,无 ESP 依赖)。
#include "tz_zone.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *iana;
    const char *posix;
} tz_zone_entry_t;

static const tz_zone_entry_t kZones[] = {
    {"Australia/Sydney",     "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Melbourne",  "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Hobart",     "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Canberra",   "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/ACT",        "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Brisbane",   "AEST-10"},
    {"Australia/Lindeman",   "AEST-10"},
    {"Australia/Adelaide",   "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Broken_Hill","ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Darwin",     "ACST-9:30"},
    {"Australia/Perth",      "AWST-8"},
    {"Australia/Lord_Howe",  "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0"},
    {"Asia/Shanghai",        "CST-8"},
    {"Asia/Hong_Kong",       "CST-8"},
    {"Asia/Taipei",          "CST-8"},
    {"Asia/Macau",           "CST-8"},
};

const char *tz_zone_posix(const char *iana) {
    if (!iana || !iana[0]) return NULL;
    // Bounded: a name longer than any known zone can never match.
    size_t len = 0;
    while (len <= TZ_ZONE_NAME_MAX && iana[len]) ++len;
    if (len > TZ_ZONE_NAME_MAX) return NULL;
    for (size_t i = 0; i < sizeof(kZones) / sizeof(kZones[0]); ++i) {
        if (strcmp(kZones[i].iana, iana) == 0) return kZones[i].posix;
    }
    return NULL;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned mp = m > 2 ? m - 3 : m + 9;   // March-based month index
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int tz_zone_offset_sec(int64_t utc) {
    const time_t t = (time_t)utc;
    struct tm local;
    if (!localtime_r(&t, &local)) return 0;
    // Wall-clock fields read as if they were UTC; the difference is the offset.
    const int64_t days = days_from_civil((int64_t)local.tm_year + 1900,
                                         (unsigned)local.tm_mon + 1, (unsigned)local.tm_mday);
    const int64_t wall = days * 86400 + local.tm_hour * 3600LL + local.tm_min * 60LL + local.tm_sec;
    return (int)(wall - utc);
}
