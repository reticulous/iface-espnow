/**
 * espnow — ESPnow transport task.
 *
 * Bridges RNS packets over Espressif's connectionless ESP-NOW link
 * using the chip's long-range PHY (250 kbps or 500 kbps). One broadcast
 * peer (FF:FF:FF:FF:FF:FF) — every node on the configured 2.4 GHz
 * channel hears every packet, exactly the broadcast medium RNS expects
 * (same shape as the LoRa transport, no framing bytes on the wire).
 *
 * Self-registers with rnsd as the `espnow` interface. Requires WiFi
 * hardware to be up (net owns the radio); active while netIsUp().
 *
 * See docs/component-plan.md §5 / §11.
 */
#pragma once

void espnowInit(void);
