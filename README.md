# iface-espnow

## What is this?

**iface-espnow** is the RNS-over-ESPnow transport for
[rns](../rns): it bridges RNS packets over
Espressif's connectionless ESP-NOW link using the chip's long-range
PHY. Single broadcast peer; one RNS packet = one ESP-NOW v2 frame, no
on-air framing. Brings the WiFi radio up itself — **no IP stack
required**.

## What this straddle owns

```
iface-espnow/
├── esp-idf/
│   ├── include/espnow.h
│   └── src/espnow.cpp
└── browser/
    └── src/
        ├── modules/espnow.ts
        └── panels/EspnowPanel.vue
```

## How others use it

```cpp
espnowInit();   // after rnsdInit
```

Configuration:

- `s.espnow.enable` — on/off
- `s.espnow.channel` — WiFi channel to lock to
- `s.espnow.lr_rate` — long-range PHY rate (250 kbps / 500 kbps)

Same lifecycle / registration model as the other transports
(`s.espnow.enable`, `RNSD_PORT_REGISTER`).

## Dependencies

- [rns](../rns)
- [spangap-net](../../s/spangap-net) — *current* hard dep; the plan
  says espnow needs no IP stack but today's code still pulls in
  `net.h` for `netIsUp()` and `NET_EV_UP/DOWN` subscription. The
  `straddle.yaml` carries the dep until the espnow task is reworked to
  own the radio independently.

## Read next

- [INTERNALS.md](INTERNALS.md) — radio ownership, long-range PHY,
  broadcast peer rationale.
