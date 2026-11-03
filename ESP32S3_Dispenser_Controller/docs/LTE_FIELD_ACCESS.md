# S3 dispenser LTE field access

See the [shared LTE guide](../../ESP32S3_Modular_Command_Transport_Controller/docs/LTE_FIELD_ACCESS.md) for wiring, server setup and modem provisioning.


## Wi-Fi fallback when LTE fails

Both S3 LTE builds keep local Wi-Fi independent of the modem worker. A missing
SIM/modem, registration failure or MQTT/TLS error does not turn off the local
console. If LTE has not connected within a 15-second startup grace period (or a later LTE connection fails) and the saved Wi-Fi station has Internet, the same
MQTT/TLS connection can use Wi-Fi. The hosted UI labels the active path **LTE** or
**Wi-Fi**. On Wi-Fi failure, LTE gets the next connection attempt; otherwise a
working Wi-Fi session stays connected until it fails or the device restarts.
Long cellular attachment runs in a separate worker so Wi-Fi can connect even while TinyGSM waits for the modem. UART ownership passes to the MQTT worker only after attachment finishes. Modem calls/probes stay off the application control task, and no modem recovery
probes interrupt a working Wi-Fi MQTT session.

Configure Wi-Fi once using the local console's Wi-Fi settings (router or phone
hotspot SSID/password). Recommended role is **AP+STA**: local AP stays available
while the station automatically reconnects. Equivalent commands on a Wi-Fi boot
profile, sent locally, are:

```text
WiFiStaSSID:YOUR_ROUTER_OR_HOTSPOT
WiFiStaPassword:YOUR_WIFI_PASSWORD
WiFiMode:APSTA
WiFiFallbackAP:ON
ConfigSave
ConfigApply
```

Wait for successful configuration acknowledgements. `ConfigSave` preserves the
settings across resets. USB/BLE-only boot profiles or deliberately disabled Wi-Fi
remain respected: choose a Wi-Fi-capable boot profile before expecting fallback.
The recovery AP alone is not an Internet uplink. In S3 LTE builds, STA timeout
also enables a recovery AP **without abandoning station reconnection**.

`LTE_WIFI_FALLBACK` defaults to `APP_WIFI_ENABLED`. Field/Home setup now embeds the
public broker CA as `LTE_WIFI_CA_PEM` in the ignored `LtePrivate.h`; rerun setup to
update older settings. Manual setup must add this string with PEM line breaks
escaped as `\n` (the public `secrets/ca.crt`, never `ca.key`). Without this CA,
Wi-Fi MQTT stays disabled while local Wi-Fi and LTE still work. Wi-Fi also waits
for a valid clock from asynchronous SNTP (`pool.ntp.org` / `time.cloudflare.com`)
or an already-set system clock, and validates the broker certificate and hostname.
The modem still requires its own CA/clock provisioning. No `setInsecure()` path.

Path changes create a new command session and expire old in-flight commands;
actuation is not replayed. Local Stop and dispensing continue independently of
the modem/network worker. Test LTE outage, Wi-Fi outage, both unavailable and
recovery on hardware before field use. This iteration does not compile firmware.

For a broker/web app on a home laptop, use [home hosting](../../docs/HOME_HOSTING.md)
and its port-forward, DNS, CGNAT and NAT-loopback checklist.
