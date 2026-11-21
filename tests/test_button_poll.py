"""Run production ADC polling against short taps at every sample phase."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/main.cc').read_text()
start = source.index('static int button_from_mv(')
end = source.index('static void input_task(', start)
polling = source[start:end]
pins = (root / 'components/bsp/include/bsp_pins.h').read_text()
ranges = next(line for line in pins.splitlines() if line.startswith('#define BSP_BTN_MV_TABLE'))
code = ranges + '\n#define BSP_BTN_COUNT 3\n' + r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include "launcher/ok_gesture.h"
struct KeyMessage { uint8_t button; app_btn_event_t event; uint16_t mv; };
using TickType_t = unsigned;
static constexpr uint8_t kPowerCommand = 0xfe;
static unsigned now, tap_start, tap_end, second_start, second_end, finish_at=1000;
static int voltage;
static bool screen_on;
static std::vector<KeyMessage> messages;
static std::vector<unsigned> emitted_at;
struct Done {};
#define pdMS_TO_TICKS(n) (n)
#define ESP_LOGI(...) ((void)0)
static unsigned xTaskGetTickCount() { return now; }
static int64_t esp_timer_get_time() { return int64_t(now)*1000; }
static passport_app_id_t app_registry_get_active() { return APP_ID_HOME; }
static void vTaskDelay(unsigned delay) { now += delay; if (now >= finish_at) throw Done{}; }
static int bsp_button_read_mv() { return (now >= tap_start && now < tap_end) ||
    (second_start && now >= second_start && now < second_end) ? voltage : 2900; }
[[maybe_unused]] static bool app_registry_screen_is_on() { return screen_on; }
static void key_post(KeyMessage message) { messages.push_back(message);emitted_at.push_back(now); }
[[maybe_unused]] static bool radio_app_wants_triple_click() { return false; }
''' + polling + r'''
// OK gesture timing with a triple-capable foreground app (Radio table mode) and without one.
static bool radio_triple(passport_app_id_t app) { return app == APP_ID_RADIO; }
static auto collect(std::vector<KeyMessage> &out) {
    return [&out](uint8_t b, app_btn_event_t e, uint16_t mv) { out.push_back({b, e, mv}); };
}
static void ok(OkGesture &g, std::vector<KeyMessage> &out, app_btn_event_t e, int64_t t, passport_app_id_t app) {
    g.event(BSP_BTN_OK, e, 603, t, app, collect(out));
}
static void tick(OkGesture &g, std::vector<KeyMessage> &out, int64_t t, passport_app_id_t app) {
    g.poll(t, app, collect(out));
}
// One short press: press at t, click and release 80 ms later.
static void tap(OkGesture &g, std::vector<KeyMessage> &out, int64_t t, passport_app_id_t app) {
    ok(g, out, BTN_EVT_PRESS, t, app);
    ok(g, out, BTN_EVT_CLICK, t + 80000, app);
    ok(g, out, BTN_EVT_RELEASE, t + 80000, app);
}
static unsigned ok_count(const std::vector<KeyMessage> &out, app_btn_event_t e) {
    unsigned n = 0;
    for (const auto &m : out) n += m.button == BSP_BTN_OK && m.event == e;
    return n;
}
static void triple_checks() {
    const passport_app_id_t radio = APP_ID_RADIO, home = APP_ID_HOME;
    // Single: unchanged timing, emitted 180 ms after its release, never as a double.
    {
        OkGesture g; g.set_triple_query(radio_triple); std::vector<KeyMessage> out;
        tap(g, out, 0, radio);
        tick(g, out, 259999, radio);
        assert(out.size() == 1);
        tick(g, out, 260000, radio);
        assert(ok_count(out, BTN_EVT_CLICK) == 1 && ok_count(out, BTN_EVT_RELEASE) == 1);
        assert(ok_count(out, BTN_EVT_DOUBLE) == 0 && ok_count(out, BTN_EVT_TRIPLE) == 0);
    }
    // Double in a triple-capable app: held for one window, then emitted once.
    {
        OkGesture g; g.set_triple_query(radio_triple); std::vector<KeyMessage> out;
        tap(g, out, 0, radio);
        tap(g, out, 150000, radio);
        assert(ok_count(out, BTN_EVT_DOUBLE) == 0 && ok_count(out, BTN_EVT_CLICK) == 0);
        tick(g, out, 409999, radio);
        assert(ok_count(out, BTN_EVT_DOUBLE) == 0);
        tick(g, out, 410000, radio);
        assert(ok_count(out, BTN_EVT_DOUBLE) == 1 && ok_count(out, BTN_EVT_CLICK) == 0);
        tick(g, out, 2000000, radio);
        assert(ok_count(out, BTN_EVT_DOUBLE) == 1);
    }
    // Double in an app without the query: unchanged, emitted at the second click.
    {
        OkGesture g; g.set_triple_query(radio_triple); std::vector<KeyMessage> out;
        tap(g, out, 0, home);
        tap(g, out, 150000, home);
        assert(ok_count(out, BTN_EVT_DOUBLE) == 1);
        OkGesture none; std::vector<KeyMessage> plain;
        tap(none, plain, 0, radio);
        tap(none, plain, 150000, radio);
        assert(ok_count(plain, BTN_EVT_DOUBLE) == 1);
    }
    // Triple: third click inside the window yields TRIPLE at once, and no double or click.
    {
        OkGesture g; g.set_triple_query(radio_triple); std::vector<KeyMessage> out;
        tap(g, out, 0, radio);
        tap(g, out, 150000, radio);
        tap(g, out, 300000, radio);
        assert(ok_count(out, BTN_EVT_TRIPLE) == 1 && ok_count(out, BTN_EVT_DOUBLE) == 0);
        tick(g, out, 3000000, radio);
        assert(ok_count(out, BTN_EVT_CLICK) == 0 && ok_count(out, BTN_EVT_DOUBLE) == 0);
        assert(ok_count(out, BTN_EVT_TRIPLE) == 1);
    }
    // Third press after the window: the held double goes out first, the third is a single.
    {
        OkGesture g; g.set_triple_query(radio_triple); std::vector<KeyMessage> out;
        tap(g, out, 0, radio);
        tap(g, out, 150000, radio);
        ok(g, out, BTN_EVT_PRESS, 420000, radio);
        assert(ok_count(out, BTN_EVT_DOUBLE) == 1);
        ok(g, out, BTN_EVT_CLICK, 500000, radio);
        ok(g, out, BTN_EVT_RELEASE, 500000, radio);
        assert(ok_count(out, BTN_EVT_TRIPLE) == 0);
        tick(g, out, 680000, radio);
        assert(ok_count(out, BTN_EVT_CLICK) == 1 && ok_count(out, BTN_EVT_DOUBLE) == 1);
    }
    // Hold after a double: LONG only, and the held double is dropped (same as today).
    {
        OkGesture g; g.set_triple_query(radio_triple); std::vector<KeyMessage> out;
        tap(g, out, 0, radio);
        tap(g, out, 150000, radio);
        ok(g, out, BTN_EVT_PRESS, 300000, radio);
        ok(g, out, BTN_EVT_LONG, 800000, radio);
        tick(g, out, 2000000, radio);
        assert(ok_count(out, BTN_EVT_LONG) == 1 && ok_count(out, BTN_EVT_DOUBLE) == 0);
        assert(ok_count(out, BTN_EVT_TRIPLE) == 0 && ok_count(out, BTN_EVT_CLICK) == 0);
    }
    // Single then hold: LONG immediately, no click later.
    {
        OkGesture g; g.set_triple_query(radio_triple); std::vector<KeyMessage> out;
        ok(g, out, BTN_EVT_PRESS, 0, radio);
        ok(g, out, BTN_EVT_LONG, 500000, radio);
        tick(g, out, 2000000, radio);
        assert(ok_count(out, BTN_EVT_LONG) == 1 && ok_count(out, BTN_EVT_CLICK) == 0);
    }
    // Another key during a held double: the double goes out first, then the key.
    {
        OkGesture g; g.set_triple_query(radio_triple); std::vector<KeyMessage> out;
        tap(g, out, 0, radio);
        tap(g, out, 150000, radio);
        g.event(BSP_BTN_DOWN, BTN_EVT_PRESS, 302, 260000, radio, collect(out));
        assert(out.size() >= 2);
        assert(out[out.size() - 2].button == BSP_BTN_OK && out[out.size() - 2].event == BTN_EVT_DOUBLE);
        assert(out.back().button == BSP_BTN_DOWN && out.back().event == BTN_EVT_PRESS);
    }
    // Foreground change while a double is held: dropped, never sent to the new app.
    {
        OkGesture g; g.set_triple_query(radio_triple); std::vector<KeyMessage> out;
        tap(g, out, 0, radio);
        tap(g, out, 150000, radio);
        tick(g, out, 200000, home);
        tick(g, out, 2000000, home);
        assert(ok_count(out, BTN_EVT_DOUBLE) == 0 && ok_count(out, BTN_EVT_TRIPLE) == 0);
    }
}

int main() {
    assert(button_from_mv(-1) == -1);
    for (int mv : {0, 5, 10, 149}) assert(button_from_mv(mv) == BSP_BTN_UP);
    for (int mv : {150, 280, 302, 320, 449}) assert(button_from_mv(mv) == BSP_BTN_DOWN);
    for (int mv : {450, 580, 603, 630, 999}) assert(button_from_mv(mv) == BSP_BTN_OK);
    for (int mv : {1000, 2500, 2900, 3300}) assert(button_from_mv(mv) == -1);
    for (bool lit : {false, true}) for (int button = 0; button < 3; ++button) {
        for (unsigned phase = 0; phase < 50; ++phase) {
            now = 0; tap_start = 1 + phase; tap_end = tap_start + 80;second_start=second_end=0;
            voltage = button == BSP_BTN_UP ? 5 : button == BSP_BTN_DOWN ? 302 : 603;
            screen_on = lit; messages.clear();emitted_at.clear();
            try { button_task(nullptr); } catch (Done &) {}
            assert(messages.size() == 3);
            assert(messages[0].event == BTN_EVT_PRESS);
            assert(messages[1].event == BTN_EVT_CLICK);
            assert(messages[2].event == BTN_EVT_RELEASE);
            for (const auto &message : messages) assert(message.button == button);
            // Measure from the physical release, including the real 3-sample
            // ADC debounce, rather than timing only the gesture class.
            const unsigned latency=emitted_at[1]-tap_end;
            if(button==BSP_BTN_OK)assert(latency>=220&&latency<240);
            else assert(latency>=40&&latency<60);
        }
    }
    // Real debouncer + gesture resolver, every 20 ms sampling phase. Even
    // when the consumer is stalled, the queue contains only one DOUBLE.
    voltage=603;
    for(unsigned phase=0;phase<20;++phase)for(unsigned gap:{80u,120u,160u}){
        now=0;tap_start=1+phase;tap_end=tap_start+80;
        second_start=tap_end+gap;second_end=second_start+80;messages.clear();
        try{button_task(nullptr);}catch(Done&){}
        assert(messages.size()==3);
        assert(messages[0].event==BTN_EVT_PRESS&&messages[1].event==BTN_EVT_PRESS);
        assert(messages[2].event==BTN_EVT_DOUBLE);
    }
    // Two taps outside the double window retain two ordinary clicks.
    now=0;tap_start=1;tap_end=81;second_start=481;second_end=561;messages.clear();
    try{button_task(nullptr);}catch(Done&){}
    assert(messages.size()==6&&messages[1].event==BTN_EVT_CLICK&&messages[4].event==BTN_EVT_CLICK);
    // A short tap followed by a long hold must not click/open anything before
    // Back or the existing 2 s / 5 s power gestures.
    now=0;tap_start=1;tap_end=81;second_start=181;second_end=5500;finish_at=6000;messages.clear();
    try{button_task(nullptr);}catch(Done&){}
    unsigned long_count=0,power_count=0;
    for(const auto &m:messages){
        if(m.button==kPowerCommand){++power_count;continue;}
        assert(m.event!=BTN_EVT_CLICK&&m.event!=BTN_EVT_DOUBLE);
        if(m.event==BTN_EVT_LONG)++long_count;
    }
    assert(long_count==1&&power_count==3);

    // Pending OK must be committed before a different navigation key; an
    // async foreground change must discard it instead of clicking a new app.
    OkGesture gesture;messages.clear();
    const auto emit=[](uint8_t b,app_btn_event_t e,uint16_t mv){key_post({b,e,mv});};
    gesture.event(BSP_BTN_OK,BTN_EVT_PRESS,603,0,APP_ID_HOME,emit);
    gesture.event(BSP_BTN_OK,BTN_EVT_CLICK,603,80000,APP_ID_HOME,emit);
    gesture.event(BSP_BTN_OK,BTN_EVT_RELEASE,603,80000,APP_ID_HOME,emit);
    gesture.event(BSP_BTN_DOWN,BTN_EVT_PRESS,302,100000,APP_ID_HOME,emit);
    assert(messages.size()==4&&messages[1].event==BTN_EVT_CLICK&&messages.back().button==BSP_BTN_DOWN);
    messages.clear();
    gesture.event(BSP_BTN_OK,BTN_EVT_CLICK,603,200000,APP_ID_HOME,emit);
    gesture.poll(600000,APP_ID_REMINDERS,emit);assert(messages.empty());
    gesture.event(BSP_BTN_OK,BTN_EVT_PRESS,603,700000,APP_ID_HOME,emit);
    messages.clear();
    gesture.event(BSP_BTN_OK,BTN_EVT_CLICK,603,780000,APP_ID_REMINDERS,emit);
    gesture.event(BSP_BTN_OK,BTN_EVT_RELEASE,603,780000,APP_ID_REMINDERS,emit);
    gesture.poll(1200000,APP_ID_REMINDERS,emit);assert(messages.empty());
    triple_checks();
}
'''
with tempfile.TemporaryDirectory() as tmp:
    (Path(tmp)/'bsp_button.h').write_text('#pragma once\nenum { BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK };\n')
    path = Path(tmp) / 'button.cpp'
    path.write_text(code)
    executable = str(path.with_suffix(''))
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+tmp, '-I'+str(root/'main'), str(path), '-o', executable], check=True)
    subprocess.run([executable], check=True)
print('ADC polling: short taps, single/double/triple/long OK, power hold, sampling phases, mixed keys and stale gestures PASS')
