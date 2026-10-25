# 4G -> WiFi tethering (proof of concept)

Bridges the A7682E cellular modem to the ESP32-S3 softAP via PPP + NAPT, so
that any device joining the T-Deck-Pro's WiFi gets routed through the SIM's
data connection.

> **Status: untested.** This sketch needs Arduino-ESP32 3.x, which the rest of
> the repo does not run on yet. The factory firmware will keep building on the
> existing platform; only this example requires the bump.

## Prerequisites

- **4G hardware variant** with battery plugged in (the modem is powered from
  VBAT — USB alone is not enough).
- **Active SIM** with a data plan and the correct APN. Edit `TETHER_APN` in
  `tethering.ino`. Set `TETHER_SIM_PIN` if the SIM is locked. PAP/CHAP
  credentials are not supported (see the comment in `tethering.ino`).

## Build

This sketch is built with `[env:T-Deck-Pro-modem]`, which uses the pioarduino
fork to get Arduino-ESP32 3.x (the `PPP` class and `esp_netif_napt_enable()`
do not exist on the 2.0.x core the default `[env:T-Deck-Pro]` builds with).

```sh
make build-modem     # compile the tethering sketch
make upload-modem    # build + flash it
make monitor
```

Both targets pass `ENV=T-Deck-Pro-modem SRC_DIR=examples/tethering` to
PlatformIO, so `platformio.ini` does not need to be edited and the default
factory build keeps pointing at `examples/factory`.

> The Arduino-ESP32 3.x core is a breaking change for the rest of the repo
> (LVGL flush callback, GxEPD2, SensorLib, etc. may need fixes). Treat this
> example as throw-away validation of the data path — do **not** flip the
> default factory env to 3.x without revalidating everything else.

## What to expect on serial

```
[modem] powering on...
[PPP] dialing carrier...
[PPP] start
[PPP] connected
[PPP] IP=10.x.x.x GW=10.64.64.64
[AP] up
[AP] SSID='T-Deck-Pro' IP=192.168.4.1
[NAPT] enabled
[ready] connect a client to the SSID and try the internet
```

If you see `[NAPT] enable failed -- lwIP not built with NAPT?`, the prebuilt
lwIP shipped with Arduino-ESP32 doesn't have NAPT compiled in. In that case
you need to either:

- use a build of Arduino-as-IDF-component with
  `CONFIG_LWIP_IP_FORWARD=y` and `CONFIG_LWIP_IPV4_NAPT=y`, or
- swap to the `esp_modem` IDF component and do routing at IDF level.

## Limitations

- **Throughput**: CAT-1 caps at ~10 Mbps down / 5 Mbps up; PPP over UART on
  ESP32 typically tops out around 1-2 Mbps.
- **Concurrent clients**: ESP32 softAP supports up to 4 by default. NAPT
  table size is bounded — heavy parallel browsing will exhaust it.
- **Power**: modem TX bursts to ~2 A; sustained tethering will drain the
  battery quickly. Keep USB plugged in.
- **No IPv6** in this sketch.
- **DNS**: the carrier-assigned DNS server is advertised to AP clients via
  DHCP option 6. If the carrier blocks recursive DNS to its resolvers from
  non-direct clients, swap to a public resolver (1.1.1.1 / 8.8.8.8) in
  `propagate_dns_to_ap()`.
