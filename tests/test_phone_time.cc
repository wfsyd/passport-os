#include <cassert>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include "phone_time.h"
int main() {
    for (int tz : {-720, 0, 345, 480, 840}) {
        char text[128]; snprintf(text,sizeof(text),"{\"epoch\":1790000000,\"tz_minutes\":%d}",tz);
        auto *j=cJSON_Parse(text);int64_t epoch;int minutes;
        assert(wifi_phone_time_parse(j,&epoch,&minutes) && epoch==1790000000 && minutes==tz);cJSON_Delete(j);
    }
    for (const char *text : {"{}", "{\"epoch\":1,\"tz_minutes\":0}",
         "{\"epoch\":1790000000,\"tz_minutes\":841}", "{\"epoch\":1790000000,\"tz_minutes\":-735}",
         "{\"epoch\":1790000000,\"tz_minutes\":1}", "{\"epoch\":1790000000.5,\"tz_minutes\":0}",
         "{\"epoch\":\"1790000000\",\"tz_minutes\":0}", "{\"epoch\":1e300,\"tz_minutes\":0}"}) {
        auto *j=cJSON_Parse(text);int64_t epoch;int minutes;
        assert(!wifi_phone_time_parse(j,&epoch,&minutes));cJSON_Delete(j);
    }
    // Optional IANA zone: bounded copy, anything odd becomes "" (fixed-offset mode).
    struct { const char *json; const char *zone; } zones[] = {
        {"{\"epoch\":1790000000,\"tz_minutes\":660,\"tz_zone\":\"Australia/Sydney\"}", "Australia/Sydney"},
        {"{\"epoch\":1790000000,\"tz_minutes\":480}", ""},
        {"{\"epoch\":1790000000,\"tz_minutes\":480,\"tz_zone\":42}", ""},
        {"{\"epoch\":1790000000,\"tz_minutes\":480,\"tz_zone\":\"\"}", ""},
        {"{\"epoch\":1790000000,\"tz_minutes\":480,\"tz_zone\":\"Australia/Sydney_very_long_name_that_exceeds_the_cap_for_sure\"}", ""},
    };
    for (const auto &z : zones) {
        auto *j=cJSON_Parse(z.json);char zone[32];
        wifi_phone_zone_parse(j,zone,sizeof(zone));
        assert(!std::strcmp(zone,z.zone));cJSON_Delete(j);
    }
    puts("Phone time: epoch/type/range, 15-minute timezone and IANA zone field PASS");
}
