// AI usage app: shows Claude Code and Codex usage from the PC helper
// (tools/ai_usage_helper). It polls only while in the foreground and the
// screen is on. The HTTP work runs in a short-lived task; the LVGL side only
// reads a small snapshot, so leaving the app never waits on the network.
// TODO(bilingual): the 20 px and 16 px CJK fonts only hold pre-listed glyphs.
// Add Chinese labels once the font is regenerated.
#include "apps/app_base.h"
#include "apps/ai_usage/ai_usage_core.h"
#include "apps/home/home_profile.h"
#include "launcher/app_registry.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "wifi_manager.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

namespace {

constexpr char kTag[] = "ai_usage";
constexpr int64_t kRefreshUs = 60LL * 1000 * 1000;   // poll interval
constexpr int64_t kDeadlineUs = 4LL * 1000 * 1000;   // whole request, not per socket
constexpr int64_t kWifiRetryUs = 5LL * 1000 * 1000;  // retry soon once Wi-Fi returns
constexpr uint32_t kFetchStack = 6144;               // HTTP client plus 1 KB body on the stack
constexpr int kBarWidth = 204;
constexpr size_t kMinLargestBlock = 16 * 1024;     // HTTP client plus task stack

// Snapshot shared by the fetch task and the LVGL tick. Plain data only: the
// task never touches LVGL, and the UI never waits for the network.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
ai_usage_t s_good{};
bool s_have_good = false;
bool s_last_failed = false;
bool s_wifi_down = false;
bool s_busy = false;            // a fetch task is alive (guarded by s_mux)
int64_t s_good_us = 0;          // time of the last good parse

// LVGL-side state; only touched with the LVGL lock held.
int64_t s_next_fetch_us = 0;    // 0 means fetch now
lv_obj_t *s_screen = nullptr;
lv_timer_t *s_timer = nullptr;
lv_obj_t *s_status = nullptr;
lv_obj_t *s_claude_5h = nullptr;
lv_obj_t *s_claude_today = nullptr;
lv_obj_t *s_claude_reset = nullptr;
lv_obj_t *s_codex_5h = nullptr;
lv_obj_t *s_codex_week = nullptr;
lv_obj_t *s_codex_reset = nullptr;
lv_obj_t *s_codex_bar_5h = nullptr;
lv_obj_t *s_codex_fill_5h = nullptr;
lv_obj_t *s_codex_bar_week = nullptr;
lv_obj_t *s_codex_fill_week = nullptr;
lv_obj_t *s_wifi_bars[3] = {nullptr, nullptr, nullptr};
ui_pixel_battery_t s_battery{};
int64_t s_battery_updated_us = 0;

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                uint32_t color, int x, int y, int w) {
    lv_obj_t *o = ui_pixel_label(parent, text, font, color);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, 20);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    return o;
}

lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

// Key on the left, value on the right, both inside a panel.
lv_obj_t *row(lv_obj_t *panel, int y, const char *key, lv_obj_t **value) {
    label(panel, key, &lv_font_montserrat_14, UI_TEXT_DIM, 0, y, 72);
    *value = label(panel, "--", &lv_font_montserrat_14, UI_TEXT, 72, y, 132);
    return *value;
}

void set_text(lv_obj_t *obj, const char *text) {
    if (obj) ui_pixel_label_set_text(obj, text);
}

void format_percent(int32_t value, char *out, size_t cap) {
    if (value < 0) std::snprintf(out, cap, "--");
    else std::snprintf(out, cap, "%ld%%", (long)value);
}

void set_bar(lv_obj_t *fill, int32_t percent) {
    if (!fill) return;
    int32_t p = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    lv_obj_set_width(fill, kBarWidth * p / 100);
}

// Refreshes the status line and every value from the snapshot. Caller holds
// the LVGL lock.
void refresh(int64_t now) {
    if (!s_screen) return;
    ai_usage_t good{};
    bool have_good, failed, wifi_down, busy;
    int64_t good_us;
    portENTER_CRITICAL(&s_mux);
    good = s_good;
    have_good = s_have_good;
    failed = s_last_failed;
    wifi_down = s_wifi_down;
    busy = s_busy;
    good_us = s_good_us;
    portEXIT_CRITICAL(&s_mux);

    char age[24] = "";
    if (have_good) {
        const int64_t secs = (now - good_us) / 1000000;
        ai_usage_format_age(secs < 0 ? 0 : (uint32_t)secs, age, sizeof(age));
    }
    char url[AI_USAGE_URL_MAX];
    home_profile_get_ai_usage_url(url, sizeof(url));
    char status[64];
    if (!url[0]) {
        std::snprintf(status, sizeof(status), "Set helper URL in phone setup");
    } else if (wifi_down || failed) {
        const char *what = wifi_down ? "Wi-Fi offline" : "PC offline";
        if (have_good) std::snprintf(status, sizeof(status), "%s - %s", what, age);
        else std::snprintf(status, sizeof(status), "%s", what);
    } else if (have_good) {
        std::snprintf(status, sizeof(status), "updated %s", age);
    } else {
        std::snprintf(status, sizeof(status), "%s", busy ? "Fetching usage..." : "Waiting for first update");
    }
    set_text(s_status, status);
    ui_pixel_text_color(s_status, failed || wifi_down ? UI_WARNING : UI_TEXT_DIM);

    char buf[AI_USAGE_TIME_MAX + AI_USAGE_WEEK_MAX + 4];  // fits "<reset> / <reset_wk>"
    const bool claude = have_good && good.has_claude;
    ai_usage_format_tokens(claude ? good.claude.tok_5h : AI_USAGE_UNSET, buf, sizeof(buf));
    set_text(s_claude_5h, claude && good.claude.tok_5h >= 0 ? buf : "--");
    ai_usage_format_tokens(claude ? good.claude.tok_today : AI_USAGE_UNSET, buf, sizeof(buf));
    set_text(s_claude_today, claude && good.claude.tok_today >= 0 ? buf : "--");
    set_text(s_claude_reset, claude && good.claude.reset[0] ? good.claude.reset : "--");

    const bool codex = have_good && good.has_codex;
    format_percent(codex ? good.codex.p5h : AI_USAGE_UNSET, buf, sizeof(buf));
    set_text(s_codex_5h, buf);
    format_percent(codex ? good.codex.pwk : AI_USAGE_UNSET, buf, sizeof(buf));
    set_text(s_codex_week, buf);
    set_bar(s_codex_fill_5h, codex ? good.codex.p5h : AI_USAGE_UNSET);
    set_bar(s_codex_fill_week, codex ? good.codex.pwk : AI_USAGE_UNSET);
    if (codex && good.codex.reset[0] && good.codex.reset_wk[0]) {
        std::snprintf(buf, sizeof(buf), "%s / %s", good.codex.reset, good.codex.reset_wk);
        set_text(s_codex_reset, buf);
    } else if (codex && good.codex.reset[0]) {
        set_text(s_codex_reset, good.codex.reset);
    } else if (codex && good.codex.reset_wk[0]) {
        set_text(s_codex_reset, good.codex.reset_wk);
    } else {
        set_text(s_codex_reset, "--");
    }

    if (!s_battery_updated_us || now - s_battery_updated_us >= 5000000) {
        ui_pixel_battery_set(&s_battery, bsp_battery_soc());
        const bool wifi_ok = WifiManager::GetInstance().IsConnected();
        int bars = 0;
        uint32_t wifi_col = UI_TEXT_DIM;
        if (wifi_ok) {
            const int rssi = WifiManager::GetInstance().GetRssi();
            bars = rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
            wifi_col = UI_TEXT;
        }
        ui_pixel_wifi_bars_set(s_wifi_bars, bars, wifi_col);
        s_battery_updated_us = now;
    }
}

// ---- network (task context, never LVGL) ----

// Bounded GET: status 200 only, body at most AI_USAGE_BODY_MAX bytes, whole
// request within kDeadlineUs. Returns the parsed snapshot through out.
bool fetch_usage(const char *url, ai_usage_t *out) {
    char body[AI_USAGE_BODY_MAX + 1];  // one spare byte detects oversize bodies
    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.timeout_ms = 4000;
    cfg.buffer_size = 512;
    cfg.buffer_size_tx = 256;
    cfg.disable_auto_redirect = true;
    cfg.keep_alive_enable = false;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    const int64_t deadline = esp_timer_get_time() + kDeadlineUs;
    bool ok = false;
    size_t total = 0;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK) break;
        if (esp_http_client_fetch_headers(client) < 0) break;
        if (esp_http_client_get_status_code(client) != 200) break;
        const int64_t declared = esp_http_client_get_content_length(client);
        if (declared > AI_USAGE_BODY_MAX) break;
        for (;;) {
            if (esp_timer_get_time() > deadline) break;
            const size_t room = AI_USAGE_BODY_MAX + 1 - total;
            const int n = esp_http_client_read(client, body + total, room);
            if (n < 0) break;
            if (n == 0) {
                ok = total > 0 && ai_usage_parse(body, total, out);
                break;
            }
            total += static_cast<size_t>(n);
            if (total > AI_USAGE_BODY_MAX) break;  // oversize: reject, do not parse
        }
    } while (false);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!ok) ESP_LOGW(kTag, "usage fetch failed (received %u bytes)", (unsigned)total);
    return ok;
}

void fetch_task(void *) {
    char url[AI_USAGE_URL_MAX];
    home_profile_get_ai_usage_url(url, sizeof(url));
    ai_usage_t parsed{};
    const bool ok = url[0] != '\0' && fetch_usage(url, &parsed);
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (ok) {
        s_good = parsed;
        s_have_good = true;
        s_good_us = now;
        s_last_failed = false;
    } else {
        s_last_failed = true;
    }
    s_busy = false;
    portEXIT_CRITICAL(&s_mux);
    vTaskDelete(nullptr);
}

// Starts at most one fetch task. Caller holds the LVGL lock.
void maybe_fetch(int64_t now) {
    char url[AI_USAGE_URL_MAX];
    home_profile_get_ai_usage_url(url, sizeof(url));
    if (!url[0]) return;  // no helper configured: nothing to poll
    if (now < s_next_fetch_us) return;
    const bool wifi = WifiManager::GetInstance().IsConnected();
    if (!wifi) {
        portENTER_CRITICAL(&s_mux);
        s_wifi_down = true;
        portEXIT_CRITICAL(&s_mux);
        s_next_fetch_us = now + kWifiRetryUs;
        return;
    }
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_busy;
    s_wifi_down = false;
    if (!busy) s_busy = true;
    portEXIT_CRITICAL(&s_mux);
    if (busy) return;
    s_next_fetch_us = now + kRefreshUs;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kMinLargestBlock) {
        ESP_LOGW(kTag, "skip usage fetch: heap too fragmented");
        portENTER_CRITICAL(&s_mux);
        s_busy = false;
        s_last_failed = true;
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    if (xTaskCreate(fetch_task, "ai_usage", kFetchStack, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGW(kTag, "no memory for usage fetch task");
        portENTER_CRITICAL(&s_mux);
        s_busy = false;
        s_last_failed = true;
        portEXIT_CRITICAL(&s_mux);
    }
}

void tick(lv_timer_t *) {
    if (!app_registry_screen_is_on()) return;  // no polling with the screen off
    const int64_t now = esp_timer_get_time();
    maybe_fetch(now);
    refresh(now);
}

void init() {}

void start() {
    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen) {
        bsp_lvgl_unlock();
        return;
    }
    s_screen = lv_obj_create(nullptr);
    ui_pixel_background(s_screen);
    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(top, UI_STATUS_WIFI_X, 18, s_wifi_bars);
    lv_obj_t *name = ui_pixel_label(top, "AI Usage", &lv_font_montserrat_14, UI_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 42, 0);
    s_battery = ui_pixel_battery_create(top, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);

    s_status = label(s_screen, "", &lv_font_montserrat_14, UI_TEXT_DIM, 12, 32, 216);

    // Claude Code panel: 5 h block tokens, tokens today, block reset time.
    lv_obj_t *claude = ui_pixel_panel_create(s_screen, 12, 56, 216, 96, UI_SURFACE);
    label(claude, "Claude Code", &lv_font_montserrat_14, UI_ACCENT, 0, 0, 216);
    row(claude, 24, "5h tokens", &s_claude_5h);
    row(claude, 44, "today", &s_claude_today);
    row(claude, 64, "resets", &s_claude_reset);

    // Codex panel: 5 h and weekly percentages with bars, then reset times.
    lv_obj_t *codex = ui_pixel_panel_create(s_screen, 12, 158, 216, 110, UI_SURFACE);
    label(codex, "Codex", &lv_font_montserrat_14, UI_ACCENT, 0, 0, 216);
    row(codex, 22, "5h limit", &s_codex_5h);
    s_codex_bar_5h = box(codex, 0, 40, kBarWidth, 8, UI_SURFACE_ALT);
    s_codex_fill_5h = box(s_codex_bar_5h, 0, 0, 0, 8, UI_ACCENT);
    row(codex, 52, "week", &s_codex_week);
    s_codex_bar_week = box(codex, 0, 70, kBarWidth, 8, UI_SURFACE_ALT);
    s_codex_fill_week = box(s_codex_bar_week, 0, 0, 0, 8, UI_ACCENT);
    row(codex, 84, "resets", &s_codex_reset);

    ui_pixel_footer(s_screen, "OK refresh", "hold OK back", &lv_font_montserrat_14);

    // First fetch happens on enter. Polling then repeats every 60 s.
    s_next_fetch_us = 0;
    s_battery_updated_us = 0;
    refresh(esp_timer_get_time());
    lv_screen_load(s_screen);
    s_timer = lv_timer_create(tick, 1000, nullptr);
    ESP_LOGI(kTag, "started");
    bsp_lvgl_unlock();
}

// Deletes everything the screen owns. A fetch task still in flight only writes
// the shared snapshot, never LVGL objects, so it may finish after this returns.
void stop() {
    if (!bsp_lvgl_lock(-1)) return;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
    s_status = s_claude_5h = s_claude_today = s_claude_reset = nullptr;
    s_codex_5h = s_codex_week = s_codex_reset = nullptr;
    s_codex_bar_5h = s_codex_fill_5h = s_codex_bar_week = s_codex_fill_week = nullptr;
    s_wifi_bars[0] = s_wifi_bars[1] = s_wifi_bars[2] = nullptr;
    s_battery = {};
    bsp_lvgl_unlock();
}

void on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    if (btn != BSP_BTN_OK || event != BTN_EVT_CLICK || !bsp_lvgl_lock(500)) return;
    if (s_screen) {
        const int64_t now = esp_timer_get_time();
        s_next_fetch_us = 0;  // OK refreshes now; a fetch already in flight is kept
        maybe_fetch(now);
        refresh(now);
    }
    bsp_lvgl_unlock();
}

}  // namespace

extern const passport_app_t g_ai_usage_app = {
    .id = APP_ID_AI_USAGE,
    .name = "AI Usage",
    .en_name = "AI USAGE",
    .desc = "Claude Code and Codex usage\nfrom the PC helper",
    .tag = "Wi-Fi",
    .theme_color = 0x6CC4DD,
    .init = init,
    .start = start,
    .stop = stop,
    .on_key = on_key,
};
