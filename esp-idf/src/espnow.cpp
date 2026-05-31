/**
 * espnow — ESPnow transport task.
 *
 * RNS-over-ESP-NOW. Single broadcast peer (FF:FF:FF:FF:FF:FF): every
 * node on the configured 2.4 GHz channel hears every packet, the same
 * broadcast medium the LoRa transport presents. No on-air framing — an
 * RNS packet (≤500 B) maps 1:1 to one ESP-NOW v2 frame (cap 1470 B).
 *
 * PHY: the chip's Espressif long-range mode only — 250 kbps or 500 kbps
 * (WIFI_PHY_MODE_LR). The slower-but-far modes; we deliberately do not
 * expose the normal 11b/g/n rates.
 *
 * WiFi ownership stays with net. ESP-NOW only needs the radio started,
 * so we gate on netIsUp() and (de)init esp_now on NET_EV_UP / NET_EV_DOWN.
 * The channel is applied best-effort: when the STA is associated to an
 * AP the channel is fixed by that AP and our set_channel is a no-op —
 * all ESP-NOW peers must then sit on the AP's channel anyway.
 *
 * RX: esp_now recv cb runs on the WiFi task — it only copies the frame
 * into a FreeRTOS queue and notifies us; the espnow task forwards to
 * rnsd from its own context. TX: rnsd → onRnsdRecv → esp_now_send.
 *
 * See docs/component-plan.md §5 / §11.
 */
#include "espnow.h"
#include "spangap.h"
#include "net.h"
#include "ports.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char* TAG = "espnow";

#define ESPNOW_VERSION   1
#define RNS_MTU          500
#define ESPNOW_RX_QDEPTH 16

/* Channels the ESP32-S3 2.4 GHz radio can actually drive for ESP-NOW.
 * 1–11 are worldwide; 12 & 13 are region-restricted (not permitted in
 * the US). Channel 14 is 802.11b-only (Japan) and not usable for the
 * long-range PHY, so it is intentionally absent. */
#define ESPNOW_CHAN_MIN  1
#define ESPNOW_CHAN_MAX  13

static const uint8_t BCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

/* ESP-NOW has no app-level port/group field — the recv cb sees every
 * frame on the channel from any ESP-NOW device. A 4-byte magic ("RNS"
 * + framing version) tags ours: prepended on TX, required on RX. A
 * mismatch drops foreign ESP-NOW traffic before it reaches rnsd. */
static const uint8_t ESPNOW_MAGIC[4] = { 'R','N','S', 0x01 };
#define ESPNOW_MAGIC_LEN 4

/* ─────────────── globals (single-task ownership) ─────────────── */

static TaskHandle_t  s_task     = nullptr;
static QueueHandle_t s_rxQueue  = nullptr;

static int           s_rnsdHandle = -1;

static bool          s_netUp   = false;   /* WiFi radio available */
static bool          s_enabled = false;   /* s.espnow.enable applied */
static bool          s_running = false;   /* esp_now inited + peer added */

static uint8_t       s_channel = 1;
static bool          s_rate500 = false;   /* false = 250 kbps, true = 500 kbps */
static bool          s_policyStay = false;/* false = disable-until-disconnect,
                                           * true  = keep channel, leave WiFi */
static uint8_t       s_selfMac[6] = {0};

/* Set by net upstream-up / throttled poll callbacks; the task re-checks
 * the live WiFi channel against ours and, under the "stay" policy,
 * leaves a network that sits on (or moved to) a different channel. */
static volatile bool s_recheckChan = false;
static TickType_t    s_lastChanPoll = 0;
#define ESPNOW_CHAN_POLL_MS 30000

static uint64_t      s_txBytes = 0, s_rxBytes = 0;
static uint64_t      s_txFrames = 0, s_rxFrames = 0;
static uint64_t      s_txFail = 0, s_rxDrop = 0;

static volatile bool s_configDirty = true;

/* Fixed-size RX slot — copied out of the WiFi-task recv cb. */
typedef struct {
    uint16_t len;
    uint8_t  data[RNS_MTU];
} espnow_rx_t;

/* ─────────────── publish ─────────────── */

static void publishState(const char* state) {
    storageSet("espnow.state", state);
    storageSet("espnow.up", s_running ? 1 : 0);
}

static void publishStats(void) {
    storageSet("espnow.stats.tx_bytes",  (int)(s_txBytes  & 0x7fffffff));
    storageSet("espnow.stats.rx_bytes",  (int)(s_rxBytes  & 0x7fffffff));
    storageSet("espnow.stats.tx_frames", (int)(s_txFrames & 0x7fffffff));
    storageSet("espnow.stats.rx_frames", (int)(s_rxFrames & 0x7fffffff));
    storageSet("espnow.stats.tx_fail",   (int)(s_txFail   & 0x7fffffff));
    storageSet("espnow.stats.rx_drop",   (int)(s_rxDrop   & 0x7fffffff));
    storageSet("espnow.channel_eff",     (int)s_channel);
    storageSet("espnow.rate_eff",        s_rate500 ? 500000 : 250000);
}

/* ─────────────── rnsd registration ─────────────── */

static void onRnsdRecv(int handle, size_t bytesAvail);
static void onRnsdDisconnect(int ref);

static void deregisterFromRnsd(void) {
    if (s_rnsdHandle >= 0) {
        itsDisconnect(s_rnsdHandle);
        s_rnsdHandle = -1;
    }
}

static bool registerWithRnsd(void) {
    deregisterFromRnsd();
    rnsd_transport_t reg = {};
    safeStrncpy(reg.name, "espnow", sizeof(reg.name));
    reg.mtu     = RNS_MTU;
    reg.bitrate = s_rate500 ? 500000 : 250000;
    reg.mode    = RNS_IFACE_MODE_GATEWAY;
    reg.in = reg.out = 1;
    reg.fwd = 1;     /* gateway forwards */
    reg.rpt = 0;
    s_rnsdHandle = itsConnect("rnsd", RNSD_PORT_TRANSPORT, &reg, sizeof(reg),
                              pdMS_TO_TICKS(500), 1, onRnsdRecv, onRnsdDisconnect);
    if (s_rnsdHandle < 0) { warn("rnsd register failed"); return false; }
    info("registered as iface espnow (mtu=%u bitrate=%u ch=%u)",
         (unsigned)RNS_MTU, (unsigned)(s_rate500 ? 500000 : 250000),
         (unsigned)s_channel);
    return true;
}

static void onRnsdDisconnect(int /*ref*/) {
    s_rnsdHandle = -1;   /* task loop re-registers while running */
}

/* ─────────────── ESP-NOW callbacks (WiFi-task context) ─────────────── */

static void espnowRecvCb(const esp_now_recv_info_t* info,
                          const uint8_t* data, int len)
{
    if (!s_rxQueue) return;
    /* Drop our own broadcasts (esp_now normally won't echo, belt-and-braces). */
    if (info && info->src_addr &&
        std::memcmp(info->src_addr, s_selfMac, 6) == 0) return;
    /* Require our magic; silently ignore any other ESP-NOW traffic. */
    if (len <= ESPNOW_MAGIC_LEN ||
        std::memcmp(data, ESPNOW_MAGIC, ESPNOW_MAGIC_LEN) != 0) return;
    int plen = len - ESPNOW_MAGIC_LEN;
    if (plen > RNS_MTU) return;

    espnow_rx_t slot;
    slot.len = (uint16_t)plen;
    std::memcpy(slot.data, data + ESPNOW_MAGIC_LEN, plen);
    if (xQueueSend(s_rxQueue, &slot, 0) != pdTRUE) {
        s_rxDrop++;            /* consumer behind — newest dropped */
        return;
    }
    if (s_task) xTaskNotifyGive(s_task);
}

static void espnowSendCb(const esp_now_send_info_t* /*tx*/,
                         esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) s_txFail++;
}

/* ─────────────── bring-up / tear-down ─────────────── */

static void espnowStop(void) {
    if (!s_running) return;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_del_peer(BCAST);
    esp_now_deinit();
    s_running = false;
    deregisterFromRnsd();
    storageSet("espnow.conflict_ch", 0);
    publishState("down");
    info("stopped");
}

static bool espnowStart(void) {
    if (s_running) return true;
    if (!s_netUp) { publishState("waiting_wifi"); return false; }

    esp_wifi_get_mac(WIFI_IF_STA, s_selfMac);

    /* Long-range PHY must be in the STA protocol bitmap for the LR rate
     * to be selectable. Keep 11b/g/n so net's normal WiFi still works. */
    esp_err_t e = esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    if (e != ESP_OK) warn("set_protocol(LR): %s", esp_err_to_name(e));

    /* Setting the channel sets the WiFi channel — STA, AP and AP+STA
     * alike. Two policies for the case where we're associated to an
     * upstream AP whose channel differs from ours:
     *
     *  - disable: don't touch the channel, refuse to start, surface a
     *    "channel conflict" so the user can move their router or us.
     *  - stay:    our channel wins. Leave the wrong-channel network
     *    (netDown) and own the channel; net's reconnect + our recheck
     *    keep us off non-matching networks. */
    uint8_t primary = 0; wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&primary, &sec);
    bool mismatch = netIsStaConnected() && primary && primary != s_channel;

    if (mismatch && !s_policyStay) {
        warn("channel %u conflicts with WiFi (STA on channel %u) — "
             "not starting (policy: disable)",
             (unsigned)s_channel, (unsigned)primary);
        storageSet("espnow.conflict_ch", (int)primary);
        publishState("channel_conflict");
        return false;
    }

    if (s_policyStay) {
        if (mismatch) {
            warn("leaving WiFi (STA on channel %u) to keep ESPnow on "
                 "channel %u (policy: stay)",
                 (unsigned)primary, (unsigned)s_channel);
            netDown(true);
        }
        e = esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        if (e != ESP_OK)
            warn("set_channel(%u): %s", (unsigned)s_channel, esp_err_to_name(e));
    } else if (!netIsStaConnected()) {
        /* No upstream link to protect. Own the channel; also the AP
         * channel when soft-AP is (or comes) up. */
        e = esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        if (e != ESP_OK)
            warn("set_channel(%u): %s", (unsigned)s_channel, esp_err_to_name(e));
    }
    /* else: associated and already on our channel — nothing to set. */

    if (esp_wifi_get_channel(&primary, &sec) == ESP_OK && primary)
        s_channel = primary;
    storageSet("espnow.conflict_ch", 0);

    e = esp_now_init();
    if (e != ESP_OK) { err("esp_now_init: %s", esp_err_to_name(e));
                       publishState("error"); return false; }

    esp_now_register_recv_cb(espnowRecvCb);
    esp_now_register_send_cb(espnowSendCb);

    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, BCAST, 6);
    peer.channel = 0;                 /* 0 = use current WiFi channel */
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    e = esp_now_add_peer(&peer);
    if (e != ESP_OK) { err("add_peer: %s", esp_err_to_name(e));
                       esp_now_deinit(); publishState("error"); return false; }

    esp_now_rate_config_t rc = {};
    rc.phymode = WIFI_PHY_MODE_LR;
    rc.rate    = s_rate500 ? WIFI_PHY_RATE_LORA_500K : WIFI_PHY_RATE_LORA_250K;
    rc.ersu    = false;
    rc.dcm     = false;
    e = esp_now_set_peer_rate_config(BCAST, &rc);
    if (e != ESP_OK) warn("set_peer_rate_config: %s", esp_err_to_name(e));

    s_running = true;
    publishState("up");
    info("up: ch=%u rate=%s kbps", (unsigned)s_channel,
         s_rate500 ? "500" : "250");

    if (!registerWithRnsd()) publishState("rnsd_unavailable");
    return true;
}

/* ─────────────── outbound (rnsd → ESP-NOW) ─────────────── */

static void drainOutbound(void) {
    if (!s_running || s_rnsdHandle < 0) return;
    static uint8_t frame[ESPNOW_MAGIC_LEN + RNS_MTU + 16];
    std::memcpy(frame, ESPNOW_MAGIC, ESPNOW_MAGIC_LEN);
    uint8_t* pkt = frame + ESPNOW_MAGIC_LEN;
    while (itsBytesAvailable(s_rnsdHandle) > 0) {
        size_t n = itsRecv(s_rnsdHandle, pkt, RNS_MTU + 16, 0);
        if (n == 0) break;
        if (n > RNS_MTU) { s_txFail++; continue; }
        esp_err_t e = esp_now_send(BCAST, frame, ESPNOW_MAGIC_LEN + n);
        if (e != ESP_OK) { s_txFail++; continue; }
        s_txFrames++;
        s_txBytes += n;
    }
}

static void onRnsdRecv(int /*handle*/, size_t /*bytesAvail*/) {
    drainOutbound();
}

/* ─────────────── inbound (ESP-NOW → rnsd) ─────────────── */

static void drainInbound(void) {
    if (!s_rxQueue) return;
    espnow_rx_t slot;
    while (xQueueReceive(s_rxQueue, &slot, 0) == pdTRUE) {
        s_rxFrames++;
        s_rxBytes += slot.len;
        if (s_rnsdHandle < 0) continue;
        size_t s = itsSend(s_rnsdHandle, slot.data, slot.len, pdMS_TO_TICKS(100));
        if (s == 0) warn("rnsd ITS send dropped (%u B)", (unsigned)slot.len);
    }
}

/* ─────────────── config / net events ─────────────── */

static void applyConfig(void) {
    s_enabled = storageGetInt("s.espnow.enable", 0) != 0;

    int ch = storageGetInt("s.espnow.channel", 1);
    if (ch < ESPNOW_CHAN_MIN || ch > ESPNOW_CHAN_MAX) ch = 1;

    char rate[8] = "250k";
    storageGetStr("s.espnow.rate", rate, sizeof(rate), "250k");
    bool r500 = strcmp(rate, "500k") == 0;

    char pol[16] = "disable";
    storageGetStr("s.espnow.conflict_policy", pol, sizeof(pol), "disable");
    bool stay = strcmp(pol, "stay") == 0;

    bool changed = ((uint8_t)ch != s_channel) || (r500 != s_rate500)
                   || (stay != s_policyStay);
    s_channel = (uint8_t)ch;
    s_rate500 = r500;
    s_policyStay = stay;

    if (!s_enabled) {
        espnowStop();
        storageSet("espnow.conflict_ch", 0);
        publishState("down");
        return;
    }
    if (s_running && changed) espnowStop();   /* re-apply ch / rate */
    if (!s_running) espnowStart();
}

static void onCfgChange(const char* /*key*/, const char* /*val*/) {
    s_configDirty = true;
    if (s_task) xTaskNotifyGive(s_task);
}

/* Reconcile the live WiFi channel with ours. Runs on the espnow task
 * (flagged by net upstream-up / throttled poll). The only way to catch
 * an AP that *moved* channel mid-session — esp_wifi follows the switch
 * silently, there is no event for it. */
static void checkStaChannel(void) {
    if (!s_enabled) return;
    uint8_t p = 0; wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&p, &sec);
    bool mismatch = netIsStaConnected() && p && p != s_channel;

    if (s_policyStay) {
        if (s_running && mismatch) {
            warn("WiFi on channel %u ≠ ESPnow channel %u — leaving WiFi "
                 "(policy: stay)", (unsigned)p, (unsigned)s_channel);
            netDown(true);
            esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        }
        return;
    }
    /* policy: disable */
    if (s_running && mismatch) {
        warn("WiFi moved to channel %u — disabling ESPnow until WiFi "
             "disconnects (policy: disable)", (unsigned)p);
        espnowStop();
        storageSet("espnow.conflict_ch", (int)p);
        publishState("channel_conflict");
    } else if (!s_running && !mismatch) {
        espnowStart();          /* conflict cleared (WiFi gone / matched) */
    }
}

static void onNetUp(const char*) {
    s_netUp = true;
    s_configDirty = true;
    if (s_task) xTaskNotifyGive(s_task);
}

static void onNetDown(const char*) {
    s_netUp = false;
    espnowStop();
    publishState("waiting_wifi");
}

static void onUpstreamChange(const char*) {     /* UP or DOWN */
    s_recheckChan = true;
    if (s_task) xTaskNotifyGive(s_task);
}

/* net ticks NET_EV_POLL ~every 10 ms while connected. We piggyback on
 * that existing tick (no self-spun timer) but only act every 30 s. */
static void onNetPoll(const char*) {
    TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - s_lastChanPoll) < pdMS_TO_TICKS(ESPNOW_CHAN_POLL_MS))
        return;
    s_lastChanPoll = now;
    s_recheckChan = true;
    if (s_task) xTaskNotifyGive(s_task);
}

/* ─────────────── CLI ─────────────── */

static void cliEspnow(const char* args) {
    if (args && strcmp(args, "help") == 0) { cliPrintf("%-*s ESPnow transport status; up/down\n", CLI_HELP_COL, "espnow [up|down]"); return; }
    if (args && cliWantsHelp(args)) {
        cliPrintf("%-*s ESPnow transport status\n", CLI_HELP_COL, "espnow");
        cliPrintf("%-*s enable/disable ESPnow\n",    CLI_HELP_COL, "espnow up|down");
        return;
    }
    if (args && strcmp(args, "up") == 0)   { storageSet("s.espnow.enable", 1); cliPrintf("enabled\n");  return; }
    if (args && strcmp(args, "down") == 0) { storageSet("s.espnow.enable", 0); cliPrintf("disabled\n"); return; }

    int conflict = storageGetInt("espnow.conflict_ch", 0);
    cliPrintf("state:    %s\n",
              s_running              ? "up"
              : !s_enabled           ? "down"
              : conflict             ? "channel conflict"
              : !s_netUp             ? "waiting wifi"
                                     : "starting");
    if (conflict)
        cliPrintf("conflict: WiFi is on channel %d — move your router to "
                  "channel %u or change ESPnow's channel\n",
                  conflict, (unsigned)s_channel);
    cliPrintf("channel:  %u\n",          (unsigned)s_channel);
    cliPrintf("rate:     %s kbps (long range)\n", s_rate500 ? "500" : "250");
    cliPrintf("conflict: %s\n", s_policyStay
              ? "stay on channel (leave WiFi networks on other channels)"
              : "disable ESPnow until WiFi disconnects");
    cliPrintf("rx:       %u frames, %u bytes (drop %u)\n",
              (unsigned)s_rxFrames, (unsigned)s_rxBytes, (unsigned)s_rxDrop);
    cliPrintf("tx:       %u frames, %u bytes (fail %u)\n",
              (unsigned)s_txFrames, (unsigned)s_txBytes, (unsigned)s_txFail);
}

/* ─────────────── task ─────────────── */

static void espnowTaskMain(void*) {
    info("[%s] task up", TAG);

    itsClientInit(2);
    s_rxQueue = xQueueCreate(ESPNOW_RX_QDEPTH, sizeof(espnow_rx_t));

    s_netUp = netIsUp();
    netRegister(NET_EV_UP,            onNetUp);
    netRegister(NET_EV_DOWN,          onNetDown);
    netRegister(NET_EV_UPSTREAM_UP,   onUpstreamChange);
    netRegister(NET_EV_UPSTREAM_DOWN, onUpstreamChange);
    netRegister(NET_EV_POLL,          onNetPoll);
    storageSubscribeChanges("s.espnow", onCfgChange);

    /* ESP-NOW needs the radio started; ask net to bring WiFi up. */
    netUp();

    for (;;) {
        if (s_configDirty) { s_configDirty = false; applyConfig(); }
        if (s_recheckChan) { s_recheckChan = false; checkStaChannel(); }

        drainInbound();

        /* Re-register with rnsd if it dropped while we're running. */
        if (s_running && s_rnsdHandle < 0 && s_enabled) registerWithRnsd();

        drainOutbound();
        publishStats();

        itsPoll(pdMS_TO_TICKS(1000));   /* cap so stats publish; woken by
                                         * rx-queue notify / cfg / net evt */
    }
}

#if CONFIG_SPANGAP_LCD
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
#endif

void espnowInit(void) {
    if (storageGetInt("s.espnow.version", 0) < ESPNOW_VERSION) {
        storageDefault("s.espnow.enable", 0);
        storageDefault("s.espnow.channel", 1);
        storageDefault("s.espnow.rate", "250k");
        storageDefault("s.espnow.conflict_policy", "disable");
        storageSet("s.espnow.version", ESPNOW_VERSION);
    }

#if CONFIG_SPANGAP_LCD
    lcdRegisterSettings("Reticulum/Transports/ESPnow", "ESPnow", espnowSettingsPane);
#endif

    cliRegisterCmd("espnow", cliEspnow);

    /* Core 0 alongside net + rnsd, prio 2, PSRAM stack. */
    s_task = spawnTask(espnowTaskMain, TAG, 6144, nullptr, 2, 0, STACK_PSRAM);
}
