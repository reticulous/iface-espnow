# iface-espnow — internals

Maintainer reference. The [README](README.md) is the operator guide; this is for
changing the code without breaking it. It is self-authoritative.

## 1. What this straddle adds

Everything here is new on top of ESP-IDF's `esp_now`/`esp_wifi` API and `rnsd`'s
interface port — there is no upstream fork. The straddle is a single source file
(`esp-idf/src/espnow.cpp`) plus its browser panel. It contributes:

- **One FreeRTOS interface task** (`espnowTaskMain`) that owns the ESP-NOW
  endpoint and bridges it to `rnsd`.
- **Interface registration with `rnsd`** — connects to `RNSD_PORT_IFACE` with an
  `rnsd_iface_t` payload naming the interface `espnow`, MTU 500, gateway mode,
  in/out/forward enabled, and the IFAC fields passed through from storage.
- **A single broadcast peer** (`FF:FF:FF:FF:FF:FF`) on the long-range PHY, with
  the rate pinned per-peer (`esp_now_set_peer_rate_config`).
- **A 4-byte magic prefix** (`"RNS"` + framing-version byte `0x01`) for tagging
  our frames and filtering foreign ESP-NOW traffic.
- **A channel-conflict subsystem** that reconciles the configured ESP-NOW channel
  with the live Wi-Fi channel under a `disable`/`stay` policy.
- **Telemetry and a CLI** (`espnow`, `espnow up|down`).
- **Declarative settings** (`straddle.yaml` `settings:` block) and a browser
  Settings panel.

## 2. Task and threading model

One task: `spawnTask(espnowTaskMain, "espnow", 6144, …, prio 2, core 0,
STACK_PSRAM)` — core 0 alongside net and rnsd. All ESP-NOW endpoint state
(`s_running`, the rnsd handle, channel/rate/IFAC config, counters) is
single-task-owned and mutated only on this task.

**Two callbacks run off-task and must stay minimal:**

- **`espnowRecvCb` runs on the Wi-Fi task.** It only filters (self-MAC echo
  drop, magic check, length cap) and copies the payload into a fixed-size
  FreeRTOS queue slot (`espnow_rx_t`, depth 16), then `xTaskNotifyGive`s the
  espnow task. It does **not** touch rnsd or any espnow-task state. A full queue
  drops the newest frame and bumps `rx_drop`.
- **`espnowSendCb` runs on the Wi-Fi task.** It only bumps `tx_fail` on a
  non-success status.

The task's single wait point is `itsPoll(1000 ms)` — woken by the rx-queue
notify, a config-change notify, or a net-event notify. The 1 s cap exists so
stats publish on a steady cadence even when idle. Each pass: apply pending config,
re-check the Wi-Fi channel if flagged, drain inbound (queue → rnsd), re-register
with rnsd if the handle dropped, drain outbound (rnsd → air), publish stats.

## 3. Wire framing

```
[ "R" "N" "S" 0x01 ][ RNS packet bytes, ≤ 500 ]
   4-byte magic        one Reticulum packet
```

There is no other on-air framing — no HDLC, no length prefix. One RNS packet is
one ESP-NOW v2 frame.

- **TX (`drainOutbound`)** — pulls each available packet off the rnsd handle into
  a PSRAM scratch buffer positioned right after the magic, then
  `esp_now_send(BCAST, frame, 4 + n)`. A packet larger than MTU bumps `tx_fail`
  and is skipped.
- **RX (`espnowRecvCb`)** — requires the 4-byte magic and `len > 4`; strips the
  magic; rejects payloads over MTU. The remaining bytes are one RNS packet handed
  to rnsd by `drainInbound` via `itsSend(…, 100 ms)`.

The magic is the *only* thing separating our traffic from other ESP-NOW devices
on the channel — ESP-NOW has no port/group field, so the recv callback sees every
frame on the channel from any ESP-NOW sender.

Inbound `itsSend` to rnsd uses a 100 ms timeout; a drop logs `rnsd ITS send
dropped`. (rnsd staggers its 1 Hz tick precisely so it doesn't park past short
interface `itsSend` timeouts — see the rns internals.)

## 4. Bring-up and tear-down

`espnowStart` (called from `applyConfig` when enabled and the radio is up):

1. Reads self MAC; sets the STA protocol bitmap to
   `11B|11G|11N|LR` — the LR bit must be present for the long-range rate to be
   selectable, and the legacy bits stay so net's normal Wi-Fi keeps working.
2. Resolves the channel against the conflict policy (§5).
3. `esp_now_init`, registers both callbacks.
4. Adds the broadcast peer with `channel = 0` (use current Wi-Fi channel),
   `ifidx = WIFI_IF_STA`, `encrypt = false`.
5. Pins the peer rate: `WIFI_PHY_MODE_LR` at `WIFI_PHY_RATE_LORA_250K` or
   `…_500K`.
6. Publishes `up` and registers with rnsd.

`espnowStop` unregisters the callbacks, deletes the peer, deinits ESP-NOW,
deregisters from rnsd, clears `espnow.conflict_ch`, and publishes `down`.

**Reconfiguration is stop/restart.** `applyConfig` compares channel, rate,
policy, and the three IFAC fields against the running config; if any changed
while running it calls `espnowStop` then `espnowStart` to re-apply. Config arrives
via `storageSubscribeChanges` on both `s.espnow` and `secrets.espnow` (the IFAC
passphrase), each setting `s_configDirty` and notifying the task.

**Boot sequence** (`espnowTaskMain`): wait on `rns.ready` (120 s bounded;
`killSelf` if it never comes — no rnsd, no point), `itsClientInit(2)`, create the
rx queue, register the five net events (`UP`, `DOWN`, `UPSTREAM_UP`,
`UPSTREAM_DOWN`, `POLL`) and the two storage subscriptions, call `netUp()` to ask
net to start Wi-Fi, then `waitForTime(0)` so SNTP can sync before the interface
registers and announces, then enter the loop.

## 5. Channel-conflict machinery

The channel is a single shared resource: `esp_wifi_set_channel` moves STA, AP, and
AP+STA together. When the STA is associated to an AP on a different channel, that
AP owns the channel and our `set_channel` would either be a no-op or would drop
the upstream link.

`espnowStart` computes `mismatch = netIsStaConnected() && primary &&
primary != s_channel`:

- **`disable`** + mismatch → don't start; set `espnow.conflict_ch = primary`,
  publish `channel_conflict`, return false.
- **`stay`** + mismatch → `netDown(true)` to leave the network, then force
  `esp_wifi_set_channel(s_channel)`.
- **No upstream link** → own the channel outright (`set_channel`), which also
  sets the soft-AP channel.
- **Associated and already on our channel** → set nothing.

After bring-up, `s_channel` is reconciled to whatever the radio reports
(`esp_wifi_get_channel`), so `espnow.channel_eff` reflects the AP-imposed channel
when one applies.

**Mid-session re-check (`checkStaChannel`).** An AP can *move* channel after
association; the driver follows silently with no event. So the task re-checks on
`NET_EV_UPSTREAM_UP`/`DOWN` and on `NET_EV_POLL` throttled to every 30 s (it
piggybacks net's existing ~10 ms poll rather than spinning its own timer). On a
fresh mismatch: `stay` leaves the network and re-pins the channel; `disable` stops
ESP-NOW and republishes `channel_conflict`. When a `disable`-policy conflict
clears (no mismatch and not running), it restarts.

## 6. Maintainer pitfalls

- **The recv callback is on the Wi-Fi task — keep it to copy-and-notify.** It must
  not call into rnsd or mutate espnow-task state; the queue + notify is the
  hand-off. (The same constraint the auto interface lives under.)
- **The magic prefix is load-bearing.** ESP-NOW has no port/group field, so
  without the `"RNS"+0x01` tag the recv path would forward arbitrary ESP-NOW
  traffic from any device on the channel into rnsd. Both the TX prepend and the
  RX require/strip must stay in lockstep; bump the version byte if the framing
  ever changes.
- **MTU is 500, not 250.** ESP-NOW's *native* action-frame body is ≤250 B, but
  the v2 API fragments under the hood, so registering MTU 500 and sending a
  500-byte RNS packet in one `esp_now_send` is correct. Don't "fix" the MTU down
  to 250.
- **The radio belongs to net.** Never init Wi-Fi here — gate on `netIsUp()` and
  react to `NET_EV_UP`/`DOWN`. The channel is shared; moving it can drop the
  upstream link, which is exactly why the conflict policy exists.
- **Long-range mode is one-way deaf without an overlapping bitmap.** A node set to
  `LR` alone is deaf to standard frames, and a standard-only NIC can't demodulate
  LR (it reads as noise). Keeping `11B|11G|11N|LR` lets the chip hear both;
  every peer must enable LR to hear our frames.
- **Re-register with rnsd if the handle drops.** `onRnsdDisconnect` nulls the
  handle; the task loop re-registers while running and enabled. Don't assume the
  registration is permanent.
- **PSRAM-stack task: no `printf`.** Use `info()`/`warn()`/`err()`. The TX scratch
  buffer is `PSRAM_BSS`.

## 7. A note on the air protocol

Today the interface is deliberately minimal: a single broadcast peer, long-range
PHY only, one RNS packet per ESP-NOW frame, with **all** routing left to rnsd. The
ESP-NOW substrate could support more — directed L2 unicast keyed on a passively
learned MAC ⇄ RNS-identity table, per-neighbor RSSI/rate metrics, an own-layer
rate-adaptation beacon — because every received frame exposes the sender's MAC,
RSSI, channel and decoded PHY rate, and unicast frames get a MAC-layer ACK. None
of that is built; the current code does pure broadcast and does not maintain a
neighbor table.
