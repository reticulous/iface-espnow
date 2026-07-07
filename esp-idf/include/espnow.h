/**
 * espnow — ESPnow interface task.
 *
 * Bridges RNS packets over Espressif's connectionless ESP-NOW link
 * using the chip's long-range PHY (250 kbps or 500 kbps). One broadcast
 * peer (FF:FF:FF:FF:FF:FF) — every node on the configured 2.4 GHz
 * channel hears every packet, exactly the broadcast medium RNS expects
 * (same shape as the LoRa interface, no framing bytes on the wire).
 *
 * Self-registers with rnsd as the `espnow` interface. Requires WiFi
 * hardware to be up (net owns the radio); active while netIsUp().
 */
#pragma once

#include "service.h"

/** Boot-registered ESPnow service: onInit() registers the `espnow` CLI verb
 *  and spawns the interface task (self-registers with rnsd; active while
 *  netIsUp()). */
class EspnowService : public Service {
public:
    void onInit() override;
};
