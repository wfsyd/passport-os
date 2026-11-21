#pragma once
#include "apps/app_base.h"

// Lives on the existing ADC task's stack. Resolve gestures at sample time,
// before the input queue can be delayed by network/audio teardown. Only OK
// clicks wait; UP/DOWN press/release (including push-to-talk) stay immediate.
class OkGesture {
public:
    // 300 ms made every confirmation feel stalled. A 180 ms inter-press
    // window retains exclusive double-clicks while shortening single clicks.
    static constexpr int64_t kDoubleUs = 180000;
    // True when the app handles BTN_EVT_TRIPLE. It is asked at the second click
    // of an OK sequence. Without a query (or a false answer) the detector behaves
    // exactly as before: a double is emitted at its second click, with no waiting.
    using TripleQuery = bool (*)(passport_app_id_t app);
    void set_triple_query(TripleQuery query) { triple_query_ = query; }

    template<class Emit>
    void poll(int64_t now, passport_app_id_t app, Emit emit) {
        if (pending_ && app != owner_) {
            pending_ = false;
            discard_tail_ = second_down_;
            second_down_ = false;
        }
        if (triple_hold_ && app != owner_) {
            triple_hold_ = false;
            discard_tail_ = third_down_;
            third_down_ = false;
        }
        if (pending_ && !second_down_ && now - released_at_ >= kDoubleUs) flush(emit);
        // A held double is emitted only once no third press can still arrive.
        if (triple_hold_ && !third_down_ && now - released_at_ >= kDoubleUs) flush_double(emit);
    }

    template<class Emit>
    void event(uint8_t button, app_btn_event_t event, uint16_t mv,
               int64_t now, passport_app_id_t app, Emit emit) {
        poll(now, app, emit);
        if (button != BSP_BTN_OK) {
            // A different key ends the double-click opportunity. Preserve
            // input order instead of applying the pending OK to a later row.
            if (event == BTN_EVT_PRESS) {
                flush(emit);
                if (triple_hold_ && third_down_) {
                    // A key pressed while the third OK press is down: drop the sequence.
                    triple_hold_ = third_down_ = false;
                    discard_tail_ = true;
                } else {
                    flush_double(emit);
                }
            }
            emit(button, event, mv);
            return;
        }
        if (event == BTN_EVT_PRESS) {
            discard_tail_ = false;
            second_down_ = pending_;
            third_down_ = triple_hold_;
            owner_ = app;
            emit(button, event, mv); // wake screen immediately, without a click
        } else if (discard_tail_) {
            if (event == BTN_EVT_RELEASE) discard_tail_ = false;
        } else if (event == BTN_EVT_CLICK) {
            if (app != owner_) {
                pending_ = second_down_ = false;
                triple_hold_ = third_down_ = false;
                discard_tail_ = true;
                return; // foreground changed while the button was held
            }
            if (third_down_) {
                triple_hold_ = third_down_ = false;
                discard_tail_ = true;
                emit(button, BTN_EVT_TRIPLE, mv);
            } else if (second_down_) {
                pending_ = second_down_ = false;
                discard_tail_ = true;
                if (triple_query_ && triple_query_(app)) {
                    // Hold the double for one window: a third press makes a triple.
                    triple_hold_ = true;
                    owner_ = app;
                    released_at_ = now;
                    mv_ = mv;
                } else {
                    emit(button, BTN_EVT_DOUBLE, mv);
                }
            } else {
                pending_ = true;
                owner_ = app;
                released_at_ = now;
                mv_ = mv;
            }
        } else if (event == BTN_EVT_LONG) {
            // Single then hold means Back/power, never a delayed click on the
            // parent page. LONG_UP/RELEASE keep their existing semantics.
            pending_ = second_down_ = false;
            triple_hold_ = third_down_ = false;
            emit(button, event, mv);
        } else if (event != BTN_EVT_RELEASE || !pending_) {
            emit(button, event, mv);
        }
    }

private:
    template<class Emit> void flush(Emit emit) {
        if (!pending_) return;
        pending_ = second_down_ = false;
        emit(BSP_BTN_OK, BTN_EVT_CLICK, mv_);
        emit(BSP_BTN_OK, BTN_EVT_RELEASE, mv_);
    }
    template<class Emit> void flush_double(Emit emit) {
        if (!triple_hold_) return;
        triple_hold_ = third_down_ = false;
        emit(BSP_BTN_OK, BTN_EVT_DOUBLE, mv_);
    }
    int64_t released_at_ = 0;
    passport_app_id_t owner_ = APP_ID_COUNT;
    uint16_t mv_ = 0;
    TripleQuery triple_query_ = nullptr;
    bool pending_ = false, second_down_ = false, discard_tail_ = false;
    bool triple_hold_ = false, third_down_ = false;
};
