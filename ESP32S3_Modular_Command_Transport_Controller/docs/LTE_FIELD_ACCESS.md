# LTE field console (source iteration)

Use [Setup Field Access.cmd](../../Setup%20Field%20Access.cmd) for the guided
workflow. It automates server dependency installation, matching credentials,
firmware settings and modem CA/clock provisioning through a confirmed UART
adapter. See [field setup](../../docs/FIELD_SETUP.md). Manual steps remain below
for reference. Generated `LTE_UART_BAUD` overrides the default 115200 baud.

This optional transport is implemented for both ESP32-S3 sketches and is **off by default**. It has not been compiled or tested on the modem in this iteration, as requested. The browser and relay are being checked separately. Leave the existing local transports enabled while bringing up LTE.

## Live connection

```text
ESP32-S3 + SparkFun SARA-R4 -- MQTT over TLS:8883 --> Mosquitto
                                                        |
                                              small Node relay
                                                        |
Phone / laptop <-- HTTPS login + WebSocket stream <-- Caddy
```

The device opens the cellular connection outbound. There is no public IP requirement on the SIM, inbound modem port, browser MQTT password, or MongoDB dependency. The relay holds bounded recent state/events in memory. History storage can be added outside the command path later.

The same web UI supports local HTTP/BLE and remote use. A host-supplied configuration script enables its remote stream. A reachable server is distinguished from a connected device; old state is labeled with its age. Remote firmware upload is disabled; use local Wi-Fi or USB. For the dispenser UI, dispensing controls are first in the side panel.

On phones, the fixed bottom bar keeps Console, Controls and Stop all reachable while scrolling through setup or routine panels. It forwards Stop to the same priority path used by the main Stop button. Save log downloads exactly the currently displayed output as a timestamped text file.

The preset search filters by group, name or command; selecting a result still
only prefills the editor. Dispensing controls stay first in the side panel.
On the hosted console, authenticated `/api/health` separates host readiness,
broker sign-in/TLS/topic failures and device state age. The relay remains
available for login when the modem is offline. See the
[relay diagnostics](../../ESP32S3_Modular_Command_Transport_Controller/relay/README.md#connection-diagnostics).

The Connection disclosure in the remote console shows browser and controller
link status, with a read-only Check connection button for broker diagnostics.
New device sessions wait for their first current snapshot before accepting normal
commands; Stop can still be sent while waiting. Prior armed/routine state is not
presented as current for the new session. Checks do not replay any commands.

## Output recovery

An acknowledgement never advances the browser's output cursor: queued command results must still be read and shown. The local ESP HTTP API uses a compact batch cursor with ID-less messages; the remote stream uses individual message IDs. The UI handles both, including numeric raw-payload flags from local firmware. A local ESP restart resets a cursor from the previous boot so new output does not wait for old IDs to catch up.

When the remote stream reconnects after a phone sleeps or loses coverage, the browser supplies its last displayed cursor and the device/relay sessions. The relay sends one ordered sync frame containing current state and available cached output, followed by live events. Replay is bounded to 24,000 bytes of events from the 128-record host cache; a visible gap message identifies known omissions. HTTP fallback uses the same connection scopes, so old responses cannot mix output from different sessions.

The S3 modem worker also keeps its output cursor across modem reconnects, retries known failed socket writes and forwards event age and ring-overflow information. The existing device event ring remains bounded (16 records by default). Recovered output is labeled as earlier output; it does not replay commands. This is recovery of available buffers, not an archive: overwritten records, QoS 0 packets lost after a successful socket write and output never received by the relay cannot be reconstructed. Save a log during the field session when a lasting record is needed. These device-side changes still need the later compile and bench checks.

## SparkFun shield wiring

Verified against the [SparkFun LTE CAT M1/NB-IoT Shield guide](https://learn.sparkfun.com/tutorials/lte-cat-m1nb-iot-shield-hookup-guide/all), for its SARA-R410M-02B shield:

| Shield connection | ESP32-S3 / purpose |
|---|---|
| GND | Common ground |
| IOREF | 3.3 V; the shield's UART level shifter follows this voltage |
| D8 with UART switch at SOFT | GPIO17, ESP UART2 RX, receiving modem TX |
| D9 with UART switch at SOFT | GPIO18, ESP UART2 TX, transmitting to modem RX |
| Shield power | Use a supported shield supply: its own USB, suitable LiPo, or specified 5 V input |

SOFT describes the shield's pin routing; this firmware still uses a hardware UART. Do not stack the shield onto ESP headers by position. Do not connect bare 1.8 V modem signals to ESP GPIO. Do not power the modem from an ESP GPIO or assume the ESP's 3.3 V regulator can supply it. Fit the correct LTE antenna and an activated compatible SIM. Start with the shield's manual POWER/RESET controls; no automatic power-key GPIO is assigned.

For UART operation, follow SparkFun's VUSB_DET jumper instructions: modem USB data and its UART are mutually exclusive. Its USB socket may supply power with USB detection disconnected. This socket is separate from the ESP board's USB/COM sockets.

GPIO17/18 are reserved when LTE is enabled. S3 dispenser pin validation rejects these pins, including saved output configurations. Config checks also reject collisions with the dispenser default, interlock, optional hardware, RGB indicator and auxiliary UART. N16R8 octal-PSRAM GPIO35/36/37, USB GPIO19/20 and UART0 GPIO43/44 are not used for LTE. The board's RGB remains on GPIO48 (GPIO38 option for boards that use it).

## Enable later in Arduino

Keep the README's **ESP32S3 Dev Module**, 16 MB flash and **OPI PSRAM** settings. Once we are ready to compile, install TinyGSM, ArduinoMqttClient (relay API developed against 0.1.8) and ArduinoJson 7 using Library Manager. The modem includes are excluded entirely when LTE is off.

For the S3 dispenser, set `DRONE_CFG_ENABLE_LTE=1` in the `QuickConfig.h` Arduino tab. For the generic S3 sketch, change `APP_ENABLE_LTE` from `0` to `1` in `src/config/AppConfig.h` (or provide the equivalent compiler definition). Create the ignored file `src/config/LtePrivate.h`:

```cpp
#define LTE_APN "your-sim-apn"
#define LTE_BROKER_HOST "mqtt.your-domain.com"
#define LTE_DEVICE_ID "s3-controller"
#define LTE_MQTT_USER "s3-controller"
#define LTE_MQTT_PASSWORD "the-generated-device-password"
#define LTE_CA_NAME "relay-ca"
// Optional if the SIM requires them:
#define LTE_APN_USER ""
#define LTE_APN_PASSWORD ""
```

The relay setup generates the MQTT password. Its `DEVICE_ID` and the firmware's ID must match. `LtePrivate.h` is local only. Change UART pins/baud in `LteConfig.h` only after rechecking ownership and wiring. The default UART is 115200 baud. SparkFun's examples reconfigure the modem to 9600 baud; if those have been run, set `UART_BAUD` to 9600 or restore the modem to 115200 before using this transport. No modem dependencies or extra worker stack are used by the LOLIN32 variants; their local UI receives the same layout and connection handling.

## TLS provisioning is required

[TinyGSM's SARA-R4 implementation](https://github.com/vshymanskyy/TinyGSM/blob/master/src/TinyGsmClientSaraR4.h) enables encryption on security profile 0; it does not itself provision the trusted CA or enforce hostname/date verification. This transport explicitly configures that profile and refuses to connect if any setting fails. There is no insecure fallback.

Before first connection, use a direct modem AT session / UART passthrough to import the relay's public `secrets/ca.crt` into the modem's certificate store as `relay-ca`. The u-blox command is `AT+USECMNG=0,0,"relay-ca",N`, with N equal to the exact PEM byte count; wait for the modem's data prompt, send exactly those bytes and require a successful result. This is a binary-length transfer, not pasting the PEM as separate AT commands. Keep the matching CA private key off the device. Confirm the import with `AT+USECMNG=3` on firmware supporting that command. Check `AT+CCLK?` and establish a correct current clock before enabling certificate validity checks.

The worker configures:

```text
AT+USECPRF=0
AT+USECPRF=0,0,3
AT+USECPRF=0,1,3
AT+USECPRF=0,3,"relay-ca"
AT+USECPRF=0,4,"mqtt.your-domain.com"
```

These request CA, hostname and date verification plus TLS 1.2. The exact SARA-R4 firmware must support them; check its [u-blox AT manual](https://cdn.sparkfun.com/assets/8/6/3/1/e/SARA-R4_ATCommands_UBX-17003787.pdf). The supplied broker uses a dedicated listener and RSA certificate; it does not require SNI. If the module rejects a profile command or cipher negotiation, diagnose/upgrade its firmware before continuing. Do not reduce verification to get a connection.

## Command delivery and responsiveness

The modem runs in a separate FreeRTOS worker so cellular AT/TLS waits do not block the control service. Queue crossings and buffers are bounded. Replies mean **accepted into the controller command queue**; read the output for execution results.

The remote console shows live-stream / HTTP-fallback mode, estimated device-state age and the last command acknowledgement round-trip time. State age starts when the control task captured its snapshot; a still-connected modem cannot make a stalled control snapshot look fresh. It is an estimate, rather than a measurement of one-way cellular transit time.

Protocol 2 adds a stop counter to each command. StopAll and the dispenser stop/disarm commands advance that counter, cancel earlier relay retries and bypass the normal request limit. The S3 worker rejects older counters and uses dedicated one-entry stop and stop-reply queues, independent of normal command replies. This is software priority once messages reach the device; it does not make a cellular link a guaranteed emergency-stop path. The previous command may already have executed, so cancellation reports earlier delivery as unconfirmed.

If Stop is unconfirmed, the relay blocks new normal commands and the console shows “Stop awaiting confirmation.” Stop remains available to retry. A matching acknowledgement for the newest stop request clears that state; an older delayed acknowledgement cannot clear a newer stop request. Local controls remain independent of this relay admission rule.

Each relay process also has a new browser-session token. A retry from before a relay restart gets HTTP 409 and cannot become a new command after in-memory history was lost. The browser captures that token once per request; on a detected restart it cancels older queued browser commands, refreshes the token and leaves the draft available for review. Deploy the matching relay/browser and S3 sources together. Older LTE firmware is reported offline with a protocol-version explanation.

Commands use MQTT QoS 1, a request ID, a device connection session and a short device-uptime deadline. The relay retries the same envelope once within its 3.5 s window, and the device deduplicates IDs. Retained commands are rejected, broker retention is disabled, sessions are clean and offline requests are rejected. A lost acknowledgement is reported as unconfirmed; it does not imply that the command never ran. Inspect output before issuing a new request ID for a physical action.

Active browsers request 1 s state updates, with immediate event forwarding; idle state updates are 15 s and presence is at least every 10 s. This reduces idle data use. Cellular registration, radio coverage, packet delivery and modem operations still determine latency. MQTT/TLS avoids repeated TLS handshakes; it cannot guarantee a stop deadline across a failed radio link. Keep local hardware time limits/interlocks in force. Prefer a carrier-supported LTE-M plan for interactive use, and verify power-saving settings do not sleep the modem during an active field session. [TinyGSM documents TCP/TLS support for SARA-R4 and currently does not offer UDP](https://github.com/vshymanskyy/TinyGSM#features).

## Bring-up evidence still needed

After the source iteration, compile each S3 configuration with LTE off and on, then test the actual shield. Verify CA/hostname/date failures refuse connection; SIM attach and TLS recovery; unplug/replug during commands; duplicate IDs and delayed/retained messages; ESP/modem/server reboot; stale state labels; GPIO ownership with saved outputs; and Stop during a slow modem request. Measure control-task responsiveness, heap/stack headroom and real LTE latency with BLE enabled. No working modem connection, firmware size or deployed URL is claimed by the source/browser tests.

For protocol 2, also verify delayed pre-Stop commands are rejected after Stop, Stop is serviced while the normal reply queue is full, counters handle wraparound, and a paused control task produces increasing snapshot age even while modem presence continues. Host-side tests cover relay ordering, queue/history pressure, normal rate-limit saturation, old browser-session rejection and HTTP fallback. Device-side queue/counter behavior still requires the later firmware and bench checks.

Hosting instructions: [relay/README.md](../relay/README.md).



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
