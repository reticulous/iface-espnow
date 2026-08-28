# iface-espnow

**iface-espnow** is an RNS **interface** for [rns](../rns): it carries Reticulum
packets over Espressif's **ESP-NOW** link, using the chip's long-range PHY. Every
node sitting on the same 2.4 GHz channel hears every packet, so to Reticulum it
looks like one shared broadcast medium — the same shape the LoRa interface
presents. One RNS packet maps 1:1 to one ESP-NOW frame.

## Origins

ESP-NOW is not a separate PHY or MAC: every ESP-NOW packet is an ordinary
**802.11 vendor-specific action frame** carried over the chip's normal Wi-Fi
MAC/PHY. It is connectionless — no association, no handshake — and goes through
the same CSMA/CA (listen-before-talk) as any Wi-Fi frame. The native on-air
payload unit is ≤250 B (`ESP_NOW_MAX_DATA_LEN`); the ESP-NOW **v2** API raises
the cap with driver-level fragmentation, so a full 500-byte RNS packet rides one
`esp_now_send`. See [INTERNALS.md](INTERNALS.md) for the long-range PHY details.

## What it does

The interface runs as one FreeRTOS task. It registers a single broadcast peer
(`FF:FF:FF:FF:FF:FF`) and self-registers with `rnsd` as the interface named
`espnow`. From then on, each RNS packet `rnsd` sends out goes on the air as one
ESP-NOW frame, and each ESP-NOW frame carrying our magic prefix is handed back to
`rnsd` as one inbound packet. `rnsd` does all routing; this interface only moves
bytes.

It registers in **gateway mode** with MTU **500** and a bitrate of 250000 or
500000 bits/s (matching the selected long-range rate). Because ESP-NOW has no
app-level port or group field, a 4-byte magic prefix (`"RNS"` + a framing-version
byte) tags our frames; any other ESP-NOW traffic on the channel is dropped before
it reaches `rnsd`.

### Radio ownership

The Wi-Fi radio is owned by [spangap-net](../spangap-net) — a **hard
dependency**. ESP-NOW only needs the radio started, so this interface asks net to
bring Wi-Fi up (`netUp()`) — but **only while enabled**. The request is issued
from the enable-driven `applyConfig` reconcile (run now-at-boot and on every
`s.espnow` change), never as an unconditional boot-time call, so a disabled
interface never powers the radio; and while disabled the task parks rather than
spinning its 1 Hz stats loop. It gates on `netIsUp()`, and (de)inits its ESP-NOW
endpoint on net's `NET_EV_UP`/`NET_EV_DOWN` events. It never owns the radio
itself.

The channel is shared by Wi-Fi and ESP-NOW alike (station and access-point
modes both). When the station is associated to an upstream AP, that AP fixes the
channel — all ESP-NOW peers must then sit on the AP's channel. The
channel-conflict policy below governs what happens when the configured ESP-NOW
channel and the AP's channel disagree.

### Starts automatically

When `iface-espnow` is in the build it starts on its own — the generated init
dispatcher launches the task, which waits for `rns.ready` (the rnsd boot barrier)
before doing anything. There is no init call for a consumer to make. `rnsd` is
ahead of it in dependency order, so its interface port is up first.

## Channel-conflict policy

If the device is associated to a Wi-Fi AP on a channel other than the configured
ESP-NOW channel, `s.espnow.conflict_policy` decides the outcome:

- **`disable`** (default) — don't touch the channel. ESP-NOW refuses to start (or
  stops if it was running) and publishes a channel conflict so the user can move
  their router or change the ESP-NOW channel. It restarts automatically once the
  conflict clears (Wi-Fi gone, or moved onto the ESP-NOW channel).
- **`stay`** — the ESP-NOW channel wins. The interface leaves the wrong-channel
  network (`netDown`) and forces the radio onto its channel.

The live Wi-Fi channel is re-checked on net upstream up/down and on a throttled
30 s poll — that is the only way to catch an AP that *moves* channel mid-session
(the driver follows the switch silently, with no event).

## Storage variables

### Settings (read)

| Key | Default | Meaning |
|---|---|---|
| `s.espnow.enable` | `0` | Master switch. `1` brings the interface up; `0` stops it. |
| `s.espnow.channel` | `1` | 2.4 GHz channel (1–13; 12/13 are region-restricted, 14 is unusable for the long-range PHY). |
| `s.espnow.rate` | `"250k"` | Long-range PHY rate: `"250k"` or `"500k"`. |
| `s.espnow.conflict_policy` | `"disable"` | Behaviour on a Wi-Fi channel conflict: `"disable"` or `"stay"` (above). |
| `s.espnow.ifac_netname` | `""` | IFAC network name. Empty = open (non-IFAC) interface. |
| `s.espnow.ifac_size` | `0` | IFAC access-code length in bytes (0 = rnsd default). |
| `s.espnow.announce_interval` | `30` | How often this node says who it is: rnsd replays every hosted destination's announce onto `espnow` on this beat, jittered ±10 %. Minutes. `0` = never on this interface's own account — the node then says who it is only when an application changes what it advertises, when a peer arrives, or on the pane's **Announce now**. Live. See [rns/README.md](../rns/README.md), "The announce beat". |
| `s.espnow.mode` | `"access_point"` | interface mode: `full`, `gateway`, `access_point`, `roaming`, `boundary` |
| `s.espnow.community_radius` | `3` | Community Radius: nodes within this many hops on this radio are served — their announces kept and answered for, searches run on their behalf, and only their announces are re-broadcast onto the air. `0` = endpoint: on-demand only. Default 3: this is an edge whose neighbours nobody else can answer for. See `rns/README.md`. |

`s.espnow.enable`, `s.espnow.channel`, `s.espnow.rate`, and
`s.espnow.conflict_policy` are declared in this straddle's `straddle.yaml`
`settings:` block, which generates their defaults and the LCD pane.

### Secrets (read)

| Key | Meaning |
|---|---|
| `s.espnow.ifac_netkey` | IFAC passphrase. Empty = open interface. Write-only from the browser. |

IFAC (Interface Access Codes) is RNS access control: a network name + passphrase
that must match on every peer of the interface, or traffic is dropped. All three
IFAC fields (`ifac_netname`, `ifac_netkey`, `ifac_size`) are passed straight
through to `rnsd` in the registration payload; `rnsd` derives the IFAC identity
and enforces it.

### Runtime state & telemetry (written)

| Key | Meaning |
|---|---|
| `espnow.state` | `down` / `waiting_wifi` / `channel_conflict` / `up` / `error` / `rnsd_unavailable`. |
| `espnow.up` | `1` while the ESP-NOW endpoint is inited and the peer is added. |
| `espnow.channel_eff` | The channel actually in effect (may differ from the request when an AP fixes it). |
| `espnow.rate_eff` | Effective rate in bits/s (250000 or 500000). |
| `espnow.conflict_ch` | The conflicting Wi-Fi channel during a conflict; `0` when there is none. |
| `espnow.stats.{tx_bytes,rx_bytes,tx_frames,rx_frames,tx_fail,rx_drop}` | Traffic counters. |
| `espnow.announce_now` | Command sentinel, self-clearing: ask rnsd to replay every hosted destination's announce onto `espnow` now — the pane's **Announce now** button. |
| `rns.pill.espnow.*` | The top status line's ESPnow pill (light green `9be89b`, order 5), written through rnsd: `E` and how many nodes are on the channel. This medium is pure broadcast and keeps no peer table of its own, so the count comes from rnsd's shared neighbourhood — the same list `espnow n` prints. Shown while the switch is on, at 0 as readily as at 3. See [rns/README](../rns/README.md#status-line-pills). |

## CLI

```
espnow            interface status: state, channel, rate, conflict policy, counters
espnow up         enable (sets s.espnow.enable = 1)
espnow down       disable (sets s.espnow.enable = 0)
espnow n[eighbors] [-v]  direct RNS peers on the channel
```

ESP-NOW is pure broadcast and keeps no peer table of its own, so `espnow n` reads
rnsd's shared neighbourhood — which destinations were heard one hop away here —
in the format every interface prints. It is also where the status-line pill's
count comes from. See
[rns/README](../rns/README.md#the-neighbourhood--who-is-one-hop-away).

Run on-device via `spangap cli "<command>"`.

## Browser

A Settings panel (`browser/src/panels/EspnowPanel.vue`, registered by
`modules/espnow.ts` under Settings → Reticulum Mesh → ESPnow)
exposes the enable switch, channel and rate, the conflict policy as a radio group,
the IFAC network name and (write-only) passphrase, and a live status/counters
block with the conflict banner.

## Dependencies

- [rns](../rns) — the Reticulum stack this interface plugs into over ITS.
- [spangap-net](../spangap-net) — owns the Wi-Fi radio; ESP-NOW gates on it.

## Read next

- [INTERNALS.md](INTERNALS.md) — task model, wire framing, bring-up/teardown,
  the channel-conflict machinery, and maintainer pitfalls.
