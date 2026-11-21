#include "apps/radio/app_radio.h"
#include "apps/radio/radio_ui.h"
#include "apps/radio/radio_player.h"
#include "apps/radio/radio_catalog.h"
#include "apps/radio/au_radio_table.h"
#include "apps/home/home_profile.h"
#include "voice_intent.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>

static const char *TAG = "app_radio";

namespace {

std::atomic<bool> s_radio_active{false};
std::atomic<bool> s_wifi_connected{false};
std::atomic<bool> s_ui_alive{false};
std::atomic<bool> s_player_ready{false};
// Reserve before task creation so concurrent Wi-Fi/start paths own one worker.
std::atomic<bool> s_catalog_running{false};
std::atomic<bool> s_catalog_cancel{false};
// One successful city search per Radio session. Outdoor Wi-Fi drops and
// returns often; re-running the search on every reconnect stopped playback
// for several seconds each time.
std::atomic<bool> s_catalog_installed{false};
SemaphoreHandle_t s_catalog_commit = nullptr;
// Xiaozhi voice request for this session (heap; the catalog worker takes it).
std::atomic<voice_intent::Request *> s_voice{nullptr};
// Voice "stop after N minutes": pauses (never powers off, unlike the settings
// sleep timer). The task waits for app stop after firing, so its handle stays
// valid for the notification that ends it.
TaskHandle_t s_stop_task = nullptr;
std::atomic<bool> s_stop_running{false};

void stop_worker(void *arg) {
    const uint32_t minutes = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(minutes * 60000u)) && s_radio_active.load()) {
        ESP_LOGI(TAG, "Voice stop timer: pausing after %u min", (unsigned)minutes);
        radio_player_set_playing(false);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    s_stop_running.store(false);
    vTaskDelete(nullptr);
}

void end_stop_timer() {
    if (!s_stop_running.load()) return;
    xTaskNotifyGive(s_stop_task);
    for (int waited = 0; s_stop_running.load() && waited < 500; waited += 10) vTaskDelay(pdMS_TO_TICKS(10));
}

bool catalog_cancelled() { return s_catalog_cancel.load() || !s_radio_active.load(); }

// Table mode applies to an AU profile that the bundled station table covers. Other
// profiles, and voice city requests, keep directory discovery.
bool table_area_for_profile(std::size_t *area) {
    home_profile_t profile;
    home_profile_get(&profile);
    const char *country = profile.country_code[0] ? profile.country_code : PASSPORT_DEFAULT_COUNTRY;
    if (std::strcmp(country, "AU") != 0) return false;
    // Order: user-picked area (radio_city, "Name, ST"), then profile city/region/GPS.
    // IP lookup is only reached when no table area matches (directory discovery).
    return au_radio::area_for_selection(profile.radio_city, profile.city, profile.region,
                                        profile.lat, profile.lon, area);
}

// First station whose name contains the spoken keywords, else the top one.
std::size_t voice_station(const RadioStation *stations, std::size_t count, const char *keywords) {
    if (keywords && keywords[0])
        for (std::size_t i = 0; i < count; ++i) if (std::strstr(stations[i].name, keywords)) return i;
    return 0;
}

void catalog_worker(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Starting shared city station catalog search...");
    constexpr std::size_t kDiscoveryCapacity = 8;
    // Release the live decoder/TLS working set before directory HTTP/cJSON.
    radio_player_set_playing(false);
    if (!radio_player_wait_idle(2000) || catalog_cancelled()) {
        // A timed-out decoder/TLS worker still owns its memory. Do not overlap
        // it with another HTTP connection, and leave normal playback retryable.
        xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
        if (!catalog_cancelled()) radio_player_set_playing(true);
        s_catalog_running.store(false);
        xSemaphoreGive(s_catalog_commit);
        ESP_LOGW(TAG, "Catalog deferred: player has not released stream resources");
        vTaskDelete(nullptr);
        return;
    }
    std::size_t table_area = 0;
    if (!s_voice.load(std::memory_order_acquire) && table_area_for_profile(&table_area)) {
        // Table mode: the rows come from flash, so no directory HTTP runs here.
        xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
        const bool alive = !catalog_cancelled() && s_ui_alive.load();
        if (alive && radio_player_replace_table(table_area)) {
            ESP_LOGI(TAG, "Station table: %s", au_radio::area_name(table_area));
            radio_ui_set_location(au_radio::area_name(table_area));
            s_catalog_installed.store(true);
        }
        if (alive) radio_player_set_playing(true);
        xSemaphoreGive(s_catalog_commit);
        s_catalog_running.store(false);
        vTaskDelete(nullptr);
        return;
    }
    auto *stations = static_cast<RadioStation *>(
        std::calloc(kDiscoveryCapacity, sizeof(RadioStation)));
    if (!stations) {
        xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
        if (!catalog_cancelled()) radio_player_set_playing(true);
        s_catalog_running.store(false);
        xSemaphoreGive(s_catalog_commit);
        ESP_LOGE(TAG, "Unable to allocate station catalog");
        vTaskDelete(nullptr);
        return;
    }
    voice_intent::Request *voice = s_voice.exchange(nullptr);
    RadioLocation preferred = {}, loc = {};
    home_profile_t profile;
    home_profile_get(&profile);
    if (voice) {
        // A spoken city replaces the profile city for this session only.
        // The tool limits the name to this field; preferred is zero-filled.
        // Country: the profile's country when known, else the build default (same as radio_catalog).
        std::memcpy(preferred.city, voice->name,
                    std::min(std::strlen(voice->name), sizeof(preferred.city) - 1));
        std::snprintf(preferred.country_code, sizeof(preferred.country_code), "%s",
                      profile.country_code[0] ? profile.country_code : PASSPORT_DEFAULT_COUNTRY);
    } else {
        std::snprintf(preferred.city, sizeof(preferred.city), "%s", profile.city);
        std::snprintf(preferred.region, sizeof(preferred.region), "%s", profile.region);
        std::snprintf(preferred.search_city, sizeof(preferred.search_city), "%s", profile.radio_city);
        std::snprintf(preferred.country_code, sizeof(preferred.country_code), "%s", profile.country_code);
    }
    std::size_t found = radio_catalog_discover_location(preferred, stations, kDiscoveryCapacity,
                                                       &loc, catalog_cancelled, voice ? voice->detail : nullptr);
    // Serialize installation/start with stop; no late playback in the next app.
    xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    const bool alive = !catalog_cancelled() && s_ui_alive.load();
    if (alive && found > 0) {
        ESP_LOGI(TAG, "Location detected: %s (%s), found %u stations", loc.city, loc.region, (unsigned)found);
        radio_ui_set_location(loc.city);
        radio_player_replace_stations_resuming(stations, found);
        if (voice) radio_player_set_station(voice_station(stations, found, voice->detail));
        s_catalog_installed.store(true);
    }
    delete voice;
    std::free(stations);
    if (alive) {
        // Start the MP3 decoder only after directory HTTP/cJSON is released.
        radio_player_set_playing(true);
    }
    xSemaphoreGive(s_catalog_commit);
    s_catalog_running.store(false);
    vTaskDelete(nullptr);
}

void start_catalog() {
#ifdef PASSPORT_RADIO_HLS_TEST
    // Keep the known test matrix visible; city discovery would replace it.
    radio_ui_set_location("全球电台测试");
    radio_player_set_playing(true);
    return;
#endif
    if (!s_catalog_commit) return;
    xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    bool idle = false;
    // Reserve before task creation; a fast worker cannot leave a stale handle.
    if (!s_radio_active.load() || !s_player_ready.load() || s_catalog_installed.load() ||
        !s_catalog_running.compare_exchange_strong(idle, true)) {
        xSemaphoreGive(s_catalog_commit);
        return;
    }
    s_catalog_cancel.store(false);
    if (xTaskCreate(catalog_worker, "radio_cat", 6144, nullptr, 5, nullptr) != pdPASS) {
        s_catalog_running.store(false);
        ESP_LOGE(TAG, "Failed to create catalog task");
        radio_player_set_playing(true);
    }
    xSemaphoreGive(s_catalog_commit);
}

// Area page: while open, the keys drive the picker instead of the station list.
// Long OK closes one level (on_back), so the global back gesture still works.
au_radio::AreaPicker s_picker;
std::atomic<bool> s_picker_open{false};
std::atomic<bool> s_menu_open{false};

void show_picker_rows() {
    static const char *rows[au_radio::AreaPicker::kMaxAreas + 1];
    const std::size_t count = s_picker.count();
    for (std::size_t i = 0; i < count; ++i) rows[i] = s_picker.label(i);
    radio_ui_show_area(s_picker.title(), rows, count, s_picker.selected());
}

void close_picker() {
    s_picker_open.store(false);
    s_menu_open.store(false);
    radio_ui_show_main();
}

// Saves the choice ("" = automatic location) and re-runs table selection for it.
void apply_area_choice(const char *choice) {
    home_profile_t profile;
    home_profile_get(&profile);
    std::snprintf(profile.radio_city, sizeof(profile.radio_city), "%s", choice ? choice : "");
    if (home_profile_set_text(&profile) != ESP_OK) {
        ESP_LOGW(TAG, "Area choice not saved");
        return;
    }
    s_catalog_installed.store(false);
    start_catalog();
}

// Radio menu, opened by a triple press of OK in table mode: "Band" cycles FM/AM/DAB+,
// "Area" opens the picker. Long OK closes it (on_back), like the other sub-pages.
std::size_t s_menu_selected;

void show_menu_rows() {
    static char band[24];
    static const char *rows[2];
    std::snprintf(band, sizeof(band), "Band: %s", au_radio::band_label(radio_player_table_band()));
    rows[0] = band;
    rows[1] = "Area";
    radio_ui_show_area("Radio", rows, 2, s_menu_selected);
}

void close_menu() {
    s_menu_open.store(false);
    radio_ui_show_main();
}

void menu_key(uint8_t btn, app_btn_event_t event) {
    if (event != BTN_EVT_CLICK) return;
    if (btn == BSP_BTN_UP) {
        s_menu_selected = s_menu_selected ? s_menu_selected - 1 : 0;
        show_menu_rows();
    } else if (btn == BSP_BTN_DOWN) {
        s_menu_selected = s_menu_selected < 1 ? s_menu_selected + 1 : s_menu_selected;
        show_menu_rows();
    } else if (btn == BSP_BTN_OK) {
        if (s_menu_selected == 0) {
            radio_app_cycle_band();
            close_menu();
        } else {
            close_menu();
            radio_app_open_area_picker();
        }
    }
}

void picker_key(uint8_t btn, app_btn_event_t event) {
    if (event != BTN_EVT_CLICK) return;
    if (btn == BSP_BTN_UP) {
        s_picker.move(-1);
        show_picker_rows();
    } else if (btn == BSP_BTN_DOWN) {
        s_picker.move(1);
        show_picker_rows();
    } else if (btn == BSP_BTN_OK) {
        switch (s_picker.select()) {
            case au_radio::AreaPicker::Outcome::Stay:
                show_picker_rows();
                break;
            case au_radio::AreaPicker::Outcome::Auto:
                apply_area_choice("");
                close_picker();
                break;
            case au_radio::AreaPicker::Outcome::Area: {
                char choice[48] = {};
                if (au_radio::format_area_choice(s_picker.chosen_area(), choice, sizeof(choice)))
                    apply_area_choice(choice);
                close_picker();
                break;
            }
            case au_radio::AreaPicker::Outcome::Close:
                close_picker();
                break;
        }
    }
}

void on_wifi_event(WifiEvent event, const std::string &data) {
    if (!s_radio_active.load()) return;

    if (event == WifiEvent::Connected) {
        ESP_LOGI(TAG, "Wi-Fi connected: %s", data.c_str());
        s_wifi_connected.store(true);
        radio_ui_set_network(true, ("已连接: " + data).c_str());
        if (!s_player_ready.load()) return;
        radio_player_set_network(true);

        start_catalog();
    } else if (event == WifiEvent::Disconnected) {
        ESP_LOGI(TAG, "Wi-Fi disconnected: %s", data.c_str());
        s_wifi_connected.store(false);
        radio_ui_set_network(false, "Wi-Fi已断开");
        if (s_player_ready.load()) radio_player_set_network(false);
    }
}

} // namespace

void radio_app_init(void) {
    ESP_LOGI(TAG, "Initializing Radio App subsystems...");
    radio_ui_init();
    static StaticSemaphore_t catalog_commit_storage;
    s_catalog_commit = xSemaphoreCreateMutexStatic(&catalog_commit_storage);
}

void radio_app_start(void) {
    ESP_LOGI(TAG, "Starting Radio App...");
    if (s_radio_active.load()) {
        ESP_LOGW(TAG, "Radio App is already active");
        return;
    }
    const voice_intent::Request request = voice_intent::take(APP_ID_RADIO);
    if (request.source == voice_intent::Source::Radio) {
        delete s_voice.exchange(new (std::nothrow) voice_intent::Request(request));
        if (request.stop_minutes) {
            end_stop_timer();
            s_stop_running.store(true);
            if (xTaskCreate(stop_worker, "radio_stop", 2048,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(request.stop_minutes)), 4,
                            &s_stop_task) != pdPASS) s_stop_running.store(false);
        }
    }
    // 1. 显示 Radio UI 界面
    s_picker_open.store(false);
    s_menu_open.store(false);
    radio_ui_show_main();
    radio_ui_set_volume(radio_player_volume());
    if (!s_catalog_commit || s_catalog_running.load()) {
        s_ui_alive.store(true);
        radio_ui_set_playback(RadioPlaybackState::Error, "目录仍在退出，请稍后重试");
        return;
    }

    // A previous bounded stop may have retained a slow stream worker to avoid
    // freeing decoder/I2S resources underneath it.  Finish that cleanup before
    // creating the next session; never allow two player owners to coexist.
    if (s_player_ready.load()) {
        if (!radio_player_deinit(2000)) {
            s_radio_active.store(false);
            s_ui_alive.store(true);
            radio_ui_set_playback(RadioPlaybackState::Error,
                                  "上次音频仍在退出，请稍后重试");
            ESP_LOGE(TAG, "Previous player worker is still alive; refusing a second owner");
            return;
        }
        s_player_ready.store(false);
    }

    if (!radio_player_init()) {
        s_player_ready.store(false);
        s_radio_active.store(false);
        s_ui_alive.store(true);
        radio_ui_set_playback(RadioPlaybackState::Error, "内存不足，音频启动失败");
        return;
    }
    s_player_ready.store(true);
    s_catalog_installed.store(false); // stations were reset to presets above
    s_radio_active.store(true);
    s_ui_alive.store(true);
    radio_player_set_playing(false);

    // 2. 挂接全局 Wi-Fi 事件回调
    WifiManager::GetInstance().SetEventCallback(on_wifi_event);
    if (!WifiManager::GetInstance().IsConnected()) {
        ESP_LOGI(TAG, "Wi-Fi not connected yet, checking station...");
        if (!SsidManager::GetInstance().GetSsidList().empty()) {
            radio_ui_set_network(false, "正在连接网络...");
            WifiManager::GetInstance().StartStation();
        } else {
            radio_ui_set_playback(RadioPlaybackState::Error, "请在手机设置中配网");
        }
    } else {
        radio_ui_set_network(true, "Wi-Fi已连接");
        radio_player_set_network(true);
        start_catalog();
    }

    // 3. 目录查询释放 HTTP/cJSON 内存后再启动播放。
}

bool radio_app_prepare_stop(void) {
    // Match catalog installation's lock: it must not resume playback after
    // this cancellation has stopped the player.
    if (s_catalog_commit) xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    s_catalog_cancel.store(true);
    if (s_radio_active.exchange(false) && s_player_ready.load())
        radio_player_set_playing(false);
    if (s_catalog_commit) xSemaphoreGive(s_catalog_commit);
    if (s_catalog_running.load()) return false;
    if (s_player_ready.load()) {
        if (!radio_player_deinit(0)) return false;
        s_player_ready.store(false);
    }
    return true;
}

void radio_app_stop(void) {
    ESP_LOGI(TAG, "Stopping Radio App (keeping global Wi-Fi active)...");
    s_picker_open.store(false);
    if (s_catalog_commit) xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    s_catalog_cancel.store(true);
    s_radio_active.store(false);
    if (s_catalog_commit) xSemaphoreGive(s_catalog_commit);

    // 1. 停止播放并等待拉流线程退出
    if (s_player_ready.load()) {
        radio_player_set_playing(false);
        radio_player_wait_idle(3500);
    }

    // 2. 2026-09-13 修复(C2):目录任务**必须 join**。原来只置标志就返回,
    //    任务还占着 6144B 栈 + 24KB 响应缓冲(单次 HTTP 最长 5s),紧接着进
    //    小智/语音时极易分配失败。这里等 5s 上界,超时也不 vTaskDelete
    //    (它可能正持堆锁),只记日志由它自行退出。
    s_ui_alive.store(false);
    end_stop_timer();
    int waited_ms = 0;
    while (s_catalog_running.load() && waited_ms < 5000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited_ms += 50;
    }
    if (s_catalog_running.load()) {
        ESP_LOGW(TAG, "catalog task still running after 5s; left to exit by itself");
    }

    // An unused voice request (no catalog ran) must not apply to a later visit.
    if (!s_catalog_running.load()) delete s_voice.exchange(nullptr);

    // 3. 移除事件回调，保留全局 Wi-Fi 连接
    WifiManager::GetInstance().SetEventCallback(nullptr);

    // 4. Radio is a session, not a boot-lifetime service. Release the 7KB
    // stream task, decoder registry, codec objects and I2S DMA before the next
    // app (especially Xiaozhi) tries to allocate a large contiguous block.
    if (s_player_ready.load()) {
        if (radio_player_deinit(5000)) s_player_ready.store(false);
    }

    // 5. 隐藏 Radio UI 并暂停高频定时器
    radio_ui_hide();

    ESP_LOGI(TAG, "Radio App stopped. free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// Opens the Area page. Nothing lists when the profile is not an AU table area.
void radio_app_open_area_picker(void) {
    if (!s_player_ready.load() || !s_radio_active.load()) return;
    s_picker.open();
    if (s_picker.count() == 0) return;
    s_picker_open.store(true);
    show_picker_rows();
}

// Moves to the next band of the current table area. False outside table mode.
bool radio_app_cycle_band(void) {
    return s_player_ready.load() && radio_player_cycle_table_band();
}

bool radio_app_wants_triple_click(void) {
    return s_player_ready.load() && radio_player_table_mode();
}

bool radio_app_on_back(void) {
    if (s_menu_open.load()) {
        close_menu();
        return true;
    }
    if (!s_picker_open.load()) return false;
    if (s_picker.back() == au_radio::AreaPicker::Outcome::Close) close_picker();
    else show_picker_rows();
    return true;
}

void radio_app_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (!s_player_ready.load()) return;
    if (s_picker_open.load()) {
        picker_key(btn, event);
        return;
    }
    if (s_menu_open.load()) {
        menu_key(btn, event);
        return;
    }
    if (event == BTN_EVT_TRIPLE) {
        // Only OK sends a triple, and only when radio_app_wants_triple_click() said yes.
        if (btn == BSP_BTN_OK && radio_player_table_mode()) {
            s_menu_selected = 0;
            s_menu_open.store(true);
            show_menu_rows();
        }
        return;
    }

    if (event == BTN_EVT_CLICK) {
        if (btn == BSP_BTN_UP) {
            // "+" 短按：切换下一电台
            radio_player_select_relative(1);
        } else if (btn == BSP_BTN_DOWN) {
            // "-" 短按：切换上一电台
            radio_player_select_relative(-1);
        } else if (btn == BSP_BTN_OK) {
            // "OK" 短按：播放 / 暂停切换
            radio_player_toggle();
        }
    } else if (event == BTN_EVT_LONG) {
        if (btn == BSP_BTN_UP) {
            // "+" 长按: 音量增加
            uint8_t cur = radio_player_volume();
            uint8_t next = cur >= 90 ? 100 : cur + 10;
            radio_player_set_volume(next);
            ESP_LOGI(TAG, "Radio volume up: %u -> %u", cur, next);
        } else if (btn == BSP_BTN_DOWN) {
            // "-" 长按: 音量减小
            uint8_t cur = radio_player_volume();
            uint8_t next = cur <= 10 ? 0 : cur - 10;
            radio_player_set_volume(next);
            ESP_LOGI(TAG, "Radio volume down: %u -> %u", cur, next);
        }
    }
}

const passport_app_t g_radio_app = {
    .id = APP_ID_RADIO,
    .name = "城市收音机",
    .en_name = "RADIO",
    .desc = "收听城市网络电台\n上下换台，确定播放",
    .tag = "直连",
    .theme_color = 0xFFB74D,
    .init = radio_app_init,
    .start = radio_app_start,
    .stop = radio_app_stop,
    .on_key = radio_app_on_key,
    .prepare_stop = radio_app_prepare_stop,
    .on_back = radio_app_on_back,
};
