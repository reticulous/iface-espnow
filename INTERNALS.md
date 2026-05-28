# reticulous-espnow — internals

## Wire format

One RNS packet = one ESP-NOW v2 frame. No HDLC, no length-prefix, no
on-air framing of any kind — the ESP-NOW frame *is* the unit. Maximum
frame size matches the chip's ESP-NOW v2 limit (~250 bytes typical;
the long-range PHY does not extend this).

## Single broadcast peer

The MAC address `FF:FF:FF:FF:FF:FF` is registered as the only ESP-NOW
peer. All sends go out as broadcast; all incoming frames are accepted
regardless of source MAC. Filtering / pairing happens above ESP-NOW
inside RNS (IFAC if you want auth, otherwise the destination hash is
the de-facto filter).

## Long-range PHY

`s.espnow.lr_rate` selects the long-range modulation (Espressif LR @
250 kbps or 500 kbps). LR PHY trades throughput for receive
sensitivity — useful in poor RF environments (concrete, distance).

## Radio ownership

WiFi ownership today **stays with `net`** — espnow gates on
`netIsUp()` and (de)inits its ESP-NOW endpoint on
`NET_EV_UP`/`NET_EV_DOWN`. The plan calls for espnow to own the radio
independently (so an espnow-only deployment can drop `spangap-net`
entirely), but until that rework, `spangap-net` is a transitive dep.

## Iface registration

Registers as `espnow.<channel>.<lr_rate>` with rnsd at startup. There
is exactly one iface; ESP-NOW does not have multiple-iface semantics
the way TCP or AutoInterface do.

## CLI

`espnow status` — frame counters, last error, channel/rate.
`espnow flap` — bounce the radio (mostly for testing).
