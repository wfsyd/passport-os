// main/apps/voice/tz_zone.h —— IANA 时区名 -> POSIX TZ 字符串(纯 C,宿主可测)。
// 只覆盖澳大利亚与中国大陆/港澳台;未知时区返回 NULL,调用方保持固定分钟偏移。
#ifndef TZ_ZONE_H
#define TZ_ZONE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TZ_ZONE_NAME_MAX 40   /* 最长 IANA 名(含结尾 NUL 之前) */
#define TZ_POSIX_MAX     48   /* POSIX TZ 字符串容量(含 NUL) */

// 返回与 iana 对应的 POSIX TZ 字符串(静态存储);NULL/空串/未知名 -> NULL。
const char *tz_zone_posix(const char *iana);

// 给定 UTC 秒,返回当前 TZ 环境(setenv/tzset 之后)下的偏移秒数。
int tz_zone_offset_sec(int64_t utc);

#ifdef __cplusplus
}
#endif

#endif /* TZ_ZONE_H */
