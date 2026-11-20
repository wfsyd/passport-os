// main/time_sync.c —— wall-clock 校时(校时源仅电脑客户端)。
// 三通道下发统一入口:BLE 协议行 time.set、USB SYS 命令 time set、REPL 手动。
// 状态:重启后未校时(valid=false);set_epoch 置位后本会话有效。
// 时区:分钟偏移 s_tz_min。NVS 键 tz_min(天气接口/手机自动设置)优先,否则用老的
// tz_hour(int8,默认 +8,±12 校验)。若手机给出可识别的 IANA 时区(如 Australia/Sydney),
// 则保存 POSIX TZ 规则(NVS 键 tz_posix)并 setenv("TZ"),夏令时由 libc 计算。
#include "time_sync.h"
#include "nvs_settings.h"
#include "tz_zone.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>  // settimeofday
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "time_sync";

#define TZ_DEFAULT_HOUR 8
#define TZ_MAX_HOUR     12
#define TZ_MIN_MINUTES  (-720)
#define TZ_MAX_MINUTES  840   /* UTC+14(基里巴斯) */

// s_epoch 为校时基线(单写点:set_epoch);settimeofday 同步系统时钟并持续
// 走时 —— 设备端显示读 time(NULL)(见 format_local),否则永远冻结在校时
// 时刻(真机实测 13:46 不再走动)。宿主测试无系统时钟,按 s_epoch 直接格式化。
static int64_t s_epoch = 0;
static bool s_valid = false;
static int s_tz_min = TZ_DEFAULT_HOUR * 60;
static char s_tz_posix[TZ_POSIX_MAX];   // 空 = 固定偏移模式

// 应用(或清除)POSIX 规则到 libc 的 TZ 环境;不写 NVS。
static void apply_posix_env(const char *posix)
{
    if (posix && posix[0]) {
        snprintf(s_tz_posix, sizeof(s_tz_posix), "%s", posix);
        setenv("TZ", s_tz_posix, 1);
    } else {
        s_tz_posix[0] = '\0';
        unsetenv("TZ");
    }
    tzset();
}

esp_err_t time_sync_init(void)
{
    int16_t minutes = 0;
    if (nvs_settings_get_tz_min(&minutes) == ESP_OK) {
        s_tz_min = minutes;
    } else {
        int8_t tz = 0;
        nvs_settings_get_tz_hour(&tz);   // 内部已兜底:打不开/损坏/缺省 → 8
        s_tz_min = ((tz >= -TZ_MAX_HOUR && tz <= TZ_MAX_HOUR) ? tz : TZ_DEFAULT_HOUR) * 60;
    }
    char posix[TZ_POSIX_MAX];
    if (nvs_settings_get_tz_posix(posix, sizeof(posix)) == ESP_OK) {
        apply_posix_env(posix);
    } else {
        apply_posix_env(NULL);
    }
    s_valid = false;
    return ESP_OK;
}

void time_sync_set_epoch(int64_t epoch_sec)
{
    s_epoch = epoch_sec;
    s_valid = true;
#ifdef ESP_PLATFORM
    struct timeval tv = { .tv_sec = (time_t)epoch_sec, .tv_usec = 0 };
    settimeofday(&tv, NULL);   // 失败忽略:UI 走 s_epoch,系统时间仅日志时间戳备用
#endif
}

bool time_sync_valid(void) { return s_valid; }
int64_t time_sync_last_epoch(void) { return s_epoch; }

void passport_phone_time(int64_t epoch, int minutes) {
    if (epoch < 1700000000LL || epoch > 4102444800LL ||
        minutes < -720 || minutes > 840 || minutes % 15) return;
    if (time(NULL) < 1700000000LL) time_sync_set_epoch(epoch);
    time_sync_set_tz_offset_min(minutes);
}

esp_err_t time_sync_set_posix_tz(const char *posix)
{
    if (!posix || !posix[0]) {
        if (s_tz_posix[0] == '\0') return ESP_OK;
        ESP_LOGI(TAG, "时区规则清除,回到固定偏移 %+d 分钟", s_tz_min);
        apply_posix_env(NULL);
        return nvs_settings_set_tz_posix("");
    }
    if (strlen(posix) >= TZ_POSIX_MAX) return ESP_ERR_INVALID_ARG;
    if (strcmp(s_tz_posix, posix) == 0) return ESP_OK;   // 不重复写 flash
    ESP_LOGI(TAG, "时区规则 %s", posix);
    apply_posix_env(posix);
    return nvs_settings_set_tz_posix(posix);
}

void passport_phone_zone(const char *iana)
{
    // 未知/缺失的 IANA 名 -> 清除规则,保持固定偏移(与旧行为一致)。
    time_sync_set_posix_tz(tz_zone_posix(iana));
}

// 当前 UTC 秒:设备端取系统时钟(不早于校时基线),宿主端取校时基线。
static int64_t current_utc(void)
{
#ifdef ESP_PLATFORM
    time_t now = time(NULL);
    return ((int64_t)now < s_epoch) ? s_epoch : (int64_t)now;
#else
    return s_epoch;
#endif
}

int time_sync_tz_hour(void) { return s_tz_min / 60; }

int time_sync_tz_offset_sec(void)
{
    if (s_tz_posix[0] != '\0' && s_valid) return tz_zone_offset_sec(current_utc());
    return s_tz_min * 60;
}

esp_err_t time_sync_set_tz_offset_min(int minutes)
{
    if (minutes < TZ_MIN_MINUTES || minutes > TZ_MAX_MINUTES) return ESP_ERR_INVALID_ARG;
    if (minutes == s_tz_min) return ESP_OK;   // 每次天气刷新都会调,不重复写 flash
    ESP_LOGI(TAG, "时区自动更新 %+d -> %+d 分钟", s_tz_min, minutes);
    s_tz_min = minutes;
    return nvs_settings_set_tz_min((int16_t)minutes);
}

esp_err_t time_sync_set_tz(int hour)
{
    if (hour < -TZ_MAX_HOUR || hour > TZ_MAX_HOUR) {
        ESP_LOGW(TAG, "时区偏移越界 %d(±%d)", hour, TZ_MAX_HOUR);
        return ESP_ERR_INVALID_ARG;
    }
    s_tz_min = hour * 60;
    nvs_settings_set_tz_min((int16_t)s_tz_min);   // 手动设置覆盖自动值
    return nvs_settings_set_tz_hour((int8_t)hour);
}

// 本地时间 = 当前系统时钟 + tz*3600,按 UTC 读(gmtime_r 手动偏移,不依赖
// TZ env)。设备端系统时钟 set_epoch 时已 settimeofday,持续走时;宿主测试
// 无系统时钟,按 s_epoch 基线直接格式化(确定性基准)。返回写入长度
// (未校时固定 "--:--" 5 字符)。
int time_sync_format_local(char *buf, size_t cap)
{
    if (!s_valid) return snprintf(buf, cap, "--:--");
    // settimeofday 未生效时 current_utc() 冻结在校时基线。
    const time_t t = (time_t)(current_utc() + (int64_t)time_sync_tz_offset_sec());
    struct tm tm;
    gmtime_r(&t, &tm);
    return snprintf(buf, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}
