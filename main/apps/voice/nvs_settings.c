// main/apps/voice/nvs_settings.c —— 设置存储实现。
#include "nvs_settings.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "settings";
#define APP_NS "app"

static const char *K_TZ_HOUR = "tz_hour";
static const char *K_TZ_MIN = "tz_min";
static const char *K_TZ_POSIX = "tz_posix";
#define TZ_POSIX_STORE_MAX 48

esp_err_t nvs_settings_init(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) nvs_close(h);
    return e;
}

esp_err_t nvs_settings_get_tz_hour(int8_t *hour) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READONLY, &h);
    if (e != ESP_OK) { if (hour) *hour = 8; return ESP_OK; }
    int8_t v = 8;
    e = nvs_get_i8(h, K_TZ_HOUR, &v);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) v = 8;
    if (hour) *hour = (v >= -12 && v <= 12) ? v : 8;
    return ESP_OK;
}

esp_err_t nvs_settings_set_tz_hour(int8_t hour) {
    if (hour < -12 || hour > 12) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_i8(h, K_TZ_HOUR, hour);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "tz_hour = %d", (int)hour);
    return e;
}

esp_err_t nvs_settings_get_tz_min(int16_t *minutes) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READONLY, &h);
    if (e != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;
    int16_t v = 0;
    e = nvs_get_i16(h, K_TZ_MIN, &v);
    nvs_close(h);
    if (e != ESP_OK || v < -720 || v > 840) return ESP_ERR_NVS_NOT_FOUND;
    if (minutes) *minutes = v;
    return ESP_OK;
}

esp_err_t nvs_settings_set_tz_min(int16_t minutes) {
    if (minutes < -720 || minutes > 840) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_i16(h, K_TZ_MIN, minutes);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "tz_min = %d", (int)minutes);
    return e;
}

esp_err_t nvs_settings_get_tz_posix(char *buf, size_t cap) {
    if (!buf || cap == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READONLY, &h);
    if (e != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;
    size_t len = cap;
    e = nvs_get_str(h, K_TZ_POSIX, buf, &len);
    nvs_close(h);
    if (e != ESP_OK || buf[0] == '\0' || len > TZ_POSIX_STORE_MAX) {
        buf[0] = '\0';
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t nvs_settings_set_tz_posix(const char *posix) {
    if (!posix) posix = "";
    if (strlen(posix) >= TZ_POSIX_STORE_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = posix[0] ? nvs_set_str(h, K_TZ_POSIX, posix) : nvs_erase_key(h, K_TZ_POSIX);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;   // clearing an unset key is fine
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "tz_posix = %s", posix[0] ? posix : "(none)");
    return e;
}

void nvs_settings_factory_reset(void) {
    nvs_handle_t h;
    if (nvs_open(APP_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "app 命名空间已清空");
    }
}
