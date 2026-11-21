// Host test: IANA -> POSIX TZ mapping and DST offsets (glibc parses the same strings the device uses).
#include "apps/voice/tz_zone.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { const char *iana, *posix; } expect_t;

static int offset_for(const char *posix, int64_t utc) {
    assert(setenv("TZ", posix, 1) == 0);
    tzset();
    return tz_zone_offset_sec(utc);
}

int main(void) {
    const expect_t table[] = {
        {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Australia/Hobart", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Australia/Canberra", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Australia/ACT", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Australia/Brisbane", "AEST-10"},
        {"Australia/Lindeman", "AEST-10"},
        {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
        {"Australia/Broken_Hill", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
        {"Australia/Darwin", "ACST-9:30"},
        {"Australia/Perth", "AWST-8"},
        {"Australia/Lord_Howe", "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0"},
        {"Asia/Shanghai", "CST-8"},
        {"Asia/Hong_Kong", "CST-8"},
        {"Asia/Taipei", "CST-8"},
        {"Asia/Macau", "CST-8"},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        const char *got = tz_zone_posix(table[i].iana);
        assert(got && strcmp(got, table[i].posix) == 0);
    }

    // Unknown, empty, NULL and over-long names keep the fixed-offset path (NULL).
    assert(tz_zone_posix("Europe/London") == NULL);
    assert(tz_zone_posix("australia/sydney") == NULL);
    assert(tz_zone_posix("") == NULL && tz_zone_posix(NULL) == NULL);
    char longname[256];
    memset(longname, 'A', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    assert(tz_zone_posix(longname) == NULL);

    // DST offsets: mid-January (summer) and mid-July (winter), 2027.
    const int64_t jan = 1800000000LL;   /* 2027-01-15 08:00 UTC */
    const int64_t jul = 1816000000LL;   /* 2027-07-19 12:26 UTC */
    assert(offset_for(tz_zone_posix("Australia/Sydney"), jan) == 11 * 3600);
    assert(offset_for(tz_zone_posix("Australia/Sydney"), jul) == 10 * 3600);
    assert(offset_for(tz_zone_posix("Australia/Adelaide"), jan) == 10 * 3600 + 1800);
    assert(offset_for(tz_zone_posix("Australia/Adelaide"), jul) == 9 * 3600 + 1800);
    assert(offset_for(tz_zone_posix("Australia/Brisbane"), jan) == 10 * 3600);
    assert(offset_for(tz_zone_posix("Australia/Brisbane"), jul) == 10 * 3600);
    assert(offset_for(tz_zone_posix("Australia/Darwin"), jul) == 9 * 3600 + 1800);
    assert(offset_for(tz_zone_posix("Australia/Perth"), jan) == 8 * 3600);
    assert(offset_for(tz_zone_posix("Australia/Lord_Howe"), jan) == 11 * 3600);
    assert(offset_for(tz_zone_posix("Australia/Lord_Howe"), jul) == 10 * 3600 + 1800);
    assert(offset_for(tz_zone_posix("Asia/Shanghai"), jan) == 8 * 3600);
    assert(offset_for(tz_zone_posix("Asia/Shanghai"), jul) == 8 * 3600);

    unsetenv("TZ");
    tzset();
    puts("TZ zone map: IANA->POSIX table, unknown names and DST offsets PASS");
    return 0;
}
