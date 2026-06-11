/**
 * espnow_lcd.cpp — on-device ESPnow settings pane (LVGL).
 *
 * Settings → Reticulum → Transports → ESPnow, mirroring the web EspnowPanel.
 * This whole file lives under conditional/spangap-lcd/, compiled only when the
 * lcd straddle is staged, so no #if guard is needed. The registration runs via
 * the when:-gated espnowLcdRegister init: hook (spangap/spangap-lcd).
 */
#include "lcd.h"

/* Settings → Reticulum → Transports → ESPnow. Mirrors the web EspnowPanel. */
static void espnowSettingsPane(void* arg) {
    lv_obj_t* p = static_cast<lv_obj_t*>(arg);
    lcdSettingSection (p, "ESPnow");
    lcdSettingSwitch  (p, "Enable", "s.espnow.enable");
    lcdSettingDropdown(p, "WiFi channel", "s.espnow.channel",
                       "1,2,3,4,5,6,7,8,9,10,11,12,13");
    lcdSettingDropdown(p, "Rate", "s.espnow.rate", "250k,500k");
    lcdSettingDropdown(p, "On conflict", "s.espnow.conflict_policy", "disable,stay");
    lcdSettingSection (p, "Status");
    lcdSettingValue   (p, "State", "espnow.state");
    lcdSettingValue   (p, "Channel", "espnow.channel_eff");
}

/* Register the ESPnow settings pane — a when:-gated init: hook
 * (spangap/spangap-lcd). Plain C++ linkage to match the generated dispatcher's
 * forward decl. */
void espnowLcdRegister(void) {
    lcdRegisterSettings("Mesh Network/RNS Interfaces/ESPnow", "ESPnow", espnowSettingsPane);
}
