#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const passport_app_t g_radio_app;

void radio_app_init(void);
void radio_app_start(void);
void radio_app_stop(void);
void radio_app_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv);
// Not bound to a key yet (see the device-check notes): opens the Area page, cycles FM/AM/DAB+.
void radio_app_open_area_picker(void);
bool radio_app_cycle_band(void);
// Whether triple-press OK opens the Radio menu now (table mode). Read by the OK gesture detector.
bool radio_app_wants_triple_click(void);

#ifdef __cplusplus
}
#endif
