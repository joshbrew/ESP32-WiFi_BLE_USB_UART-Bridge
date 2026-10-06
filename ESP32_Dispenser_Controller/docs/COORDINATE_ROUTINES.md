# Saved routines, networks, and drone position

The dispenser console has a one-time initial delay, pulse duration, off-time,
and repeat count. Save a named routine and it appears as a Run button, including
after reboot. Arm before a manual run. Stop routine, Stop output, Disarm, and
Stop all cancel the sequence and release the output. An initial delay is stored
as the first `START_WAIT` step and runs only before the first pulse. `WAIT`
steps run every repetition, including the final trailing gap.

Check **Repeat until stopped** for continuous on/off cycling. The initial delay
still runs only once. Save it as a named routine and use its Run button later;
the continuous option survives reboot. **Stop routine** releases the output and
cancels further pulses. Continuous routines need arming expiry disabled; use
**Remove saved time limits** if your older payload settings have an expiry.
An unlimited routine assigned to a GPS point holds that point until stopped;
use a finite repeat count when the sequence should advance automatically.

Repeat counts and timed steps use unsigned 32-bit integers. Repeats accept
1–4294967295; pulses accept 1–4294967295 ms (about 49.7 days), and initial delay
and gaps accept 0–4294967295 ms. These are storage bounds: there is no 20-repeat,
two-minute delay, 60-minute pulse, total-runtime, or WAIT_IDLE timeout cap.
New configurations have no maximum-pulse limit and no arming expiry. Optional
saved payload limits remain effective: disarm and use **Remove saved time
limits** to set `DispenserArmTimeout:0` and `DispenserMaxPulse:0` and save the
standalone settings. Also save the active named payload profile if you want
these settings in that profile. Nonzero limits are still available if desired.
Existing saved routines retain their names, steps, and repeat counts after
updating firmware. Stop, interlocks, and stale-position shutdown remain active.

## Join a controller or drone hotspot

Under Network settings, select **AP + client** for an initial setup with a local
recovery AP, enter the hotspot SSID and password, select **Normal Wi-Fi**, and
save/apply. Wi-Fi must be enabled in the selected boot radio mode. Empty password
keeps the stored password unless the open-network checkbox is selected. Read
the station address from WiFiStatus or USB; use that address for the console and
position feed. STA-only falls back to the configured AP if initial connection
fails. AP + client uses the upstream router's channel.

The [XAG P100 product page](https://www.xa.com/en/p100) describes RC Networking
and local controller hotspots. A hotspot is a possible network path; it does
not establish access to GPS telemetry. No public P100 position API was identified
for this implementation. XAG, an authorized integration provider, or your own
onboard GPS bridge can supply the adapter below. This is not a verified P100
integration. Client isolation, unavailable credentials, band compatibility, or
proprietary telemetry may prevent using its hotspot.

## Ordered coordinate sequence

1. Save each dispenser routine, such as `dots`.
2. Select one position source: MAVLink or custom API bridge.
3. Enter one point per line: `latitude,longitude,radiusMeters,routineName`.
   Up to 256 points are supported; radius is 0.1–1000 meters, names 1–15 ASCII
   letters/digits/underscore/hyphen. Coordinates use WGS84 degrees.
4. Save the sequence, establish a fresh position feed, then press Start sequence.

Example:

```text
37.4219999,-122.0840575,5,dots
37.4221000,-122.0842000,8,longpulse
```

Existing saved 12-point plans load after the update and use the expanded format
the next time you save. Each plan can now contain up to 256 ordered points.

Only the next point is considered. Entering its horizontal radius arms through
the normal interlock checks and starts its saved routine, including its initial
delay. The point advances once that routine completes. Repeated position updates
inside the radius do not retrigger it. Overlapping radii can trigger the next
point immediately after the previous routine finishes. This is the requested
warm-up behavior; there is no speed prediction, altitude test, or distance lead.

The sequence never starts automatically after reboot. Start always begins at
point 1. Output waits inactive between points. All stop buttons end the mission
and active output; a new Start is required. Lost position for over 3 seconds,
routine failure, or interlock failure ends the sequence. Position updates must
continue during delays and pulses. Known horizontal accuracy worse than the
point's radius prevents a new trigger. Unknown accuracy (`-1`) is allowed; choose
a radius appropriate for the actual GPS quality. It does not compensate for
GPS offset, dispensing lag, wind, or movement during an initial delay.

## Standard MAVLink input

Forward unsigned [MAVLink common](https://mavlink.io/en/messages/common.html)
`GPS_RAW_INT` (ID 24) and `GLOBAL_POSITION_INT` (ID 33) to the ESP32 station or AP IP,
UDP port **14550**, as complete MAVLink 1 or 2 frames per datagram. Multiple
frames in one datagram are accepted. Set `GeoSource:MAVLINK` then `GeoSave`.
System/component default to **1/1**; change `GEO_MAVLINK_SYSTEM_ID`,
`GEO_MAVLINK_COMPONENT_ID`, and port in `src/config/AppConfig.h` if needed.

Both message streams should arrive at 2–5 Hz. A fresh 3D or better GPS fix
(fix_type 3–6) is required. GLOBAL_POSITION_INT time_boot_ms must advance;
duplicates and reordered frames do not refresh position. MAVLink 2 h_acc supplies
horizontal accuracy when present. Signed frames, unknown incompatible flags,
invalid CRC, and truncated datagrams are rejected. This small receiver does not
send heartbeat, change flight modes, request message rates, or verify signatures;
configure a flight controller or companion to forward those streams. Stop the
sequence and send `GeoResetPosition` after a flight-controller reboot or clock
reset, then wait for new fixes before starting again.

## Test GPS manually without a drone

1. Save a short test routine and a coordinate sequence using **MAVLink over Wi-Fi**.
2. Connect to the ESP32 AP or use its client connection. Stop real telemetry while testing.
3. In **Test current GPS**, enter `latitude,longitude,accuracyMeters`. Start with a
   coordinate outside the first point's radius and click **Send test GPS position**.
4. Wait for fresh position status, then click **Start sequence**.
5. Change the test coordinate to inside the radius and send again. The routine
   starts with its saved initial delay, then runs its pulses.
6. Check **Keep sending every second** for delays or routines over three seconds.
   It resends the currently entered coordinate. **Stop sequence** stops the feed
   and output. Closing the page or losing updates causes stale-position shutdown.

The ESP32 sends actual MAVLink 2 GPS_RAW_INT and GLOBAL_POSITION_INT packets to
its own Wi-Fi IP on the configured UDP port. They pass through the normal socket,
CRC, fix-quality, coordinate, and mission handling. The test does not bypass the
receiver or directly set a fix. Hardware UDP delivery still needs a bench check.
USB/BLE/HTTP command equivalent: `GeoTestPosition:37.4219999,-122.0840575,1`.
One command sends one datagram; repeat it within three seconds for a long test.

## Interface for XAG or another proprietary provider

Set `GeoSource:API`, save the plan, and POST **text/plain** to:

```text
http://ESP32-STATION-IP/api/position
```

Request body:

```text
37.4219999,-122.0840575,1.5,100
```

Fields are latitude degrees, longitude degrees, horizontal accuracy in meters
(`-1` if unavailable), and **measurement age in milliseconds**. Age must include
provider buffering and bridge delay, not just HTTP send time. Send at 2–5 Hz,
latest sample only, maximum body 128 bytes. HTTP 200 `accepted:true` means the
sample entered a single-slot latest-position mailbox; it does not start a
mission or acknowledge hardware actuation. The controller processes it on its
main loop. Select API source before use; samples cannot override MAVLink mode.
Invalid/stale samples invalidate the fix. Network admission can return 503 under
pressure; do not retry an old sample indefinitely. Replace it with a new one.

An adapter must use the **aircraft** position, never the ground phone/controller
position. It must preserve fix quality and actual measurement age. On loss of
fix, send an invalid sample or stop sending. Do not relabel cached telemetry as
fresh. A bridge can use `GeoPosition:lat,lon,accuracy,age` over USB/BLE for low-rate
bench work; use `/api/position` for regular HTTP telemetry so it does not flood
the command queue or event log.

`examples/position_bridge.py` reads newline-delimited JSON from a provider on
stdin and posts the newest sample. It requires `latitude`, `longitude`,
`accuracyMeters`, and `measuredAtUnixMs`; it calculates age at send time. Connect
XAG's eventual authorized SDK or another GPS producer to that contract.
The provider and bridge must agree on Unix time so measurement age stays valid.

```sh
your-provider | python ESP32_Dispenser_Controller/examples/position_bridge.py http://ESP32-STATION-IP
your-provider | python ESP32_Dispenser_Controller/examples/position_bridge.py --mavlink ESP32-STATION-IP
```

Replace `your-provider` with the program that produces the aircraft measurements.

```text
{"latitude":37.4219999,"longitude":-122.0840575,"accuracyMeters":1.5,"measuredAtUnixMs":YOUR_MEASUREMENT_TIME}
```

Use the bridge's `--mavlink` mode for a bench-only common-standard UDP fixture.
Keep the applicator disconnected while testing position, delays, repeats, stop,
and dropped telemetry. These network interfaces have no authentication and are
intended for a trusted local network. Retain an independent physical stop and
interlock for the aircraft.

## Commands

```text
GeoClear
GeoSource:MAVLINK
GeoSource:API
GeoAdd:latitude,longitude,radiusMeters,savedRoutineName
GeoSave
GeoLoad
GeoList
GeoStatus
GeoStart
GeoStop
GeoResetPosition
GeoPosition:latitude,longitude,accuracyMeters,ageMs
```

Stop before editing or changing source. `GeoClear` resets the source to MAVLink.
Plan and source persist in `drone-geo`; active mission and position do not.
See [ESP32 LR setup](../../docs/WIFI_LR.md) for a BLE-to-LR ground relay. A
standard drone hotspot requires normal Wi-Fi; an LR-only ESP32 cannot associate
with it. The standalone LR relay supports commands/events, saved routine buttons,
and sequence status; it does not forward continuous position telemetry. Connect
the position producer directly to the aircraft ESP32 or adapt the relay for that
separate stream.
