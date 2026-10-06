# Aircraft position and ordered routines on Pi

Create/save dispenser routines, then build/save up to 500 ordered points with
latitude, longitude, radius in meters and a saved routine name. **GeoStart / Start
sequence — auto-arm** authorizes automatic arming at each next point. A manual
routine still needs a separate Arm. GeoStart begins at point 1 every time.
The plan/source survive reboot; enabled state, position and progress do not.

The Pi requires the dispenser profile, a saved nonempty plan, fresh aircraft
position, healthy interlock and idle output. It preflights every saved point
routine against optional pulse/arm limits. Each point executes the persisted
routine snapshot, so unsaved library edits cannot change a coordinate run.
Only the next point is considered. Overlapping points may run consecutively;
there is no return-to-start or re-entry requirement. Outputs disarm after every
routine. Known horizontal accuracy greater than the next radius blocks triggering;
-1 means unknown and permits radius checks without an accuracy bound.

Initial delay is measured after reaching the point and runs once before repeats.
WAIT/gap runs every repeat, including the final gap. The sequence advances only
after the full routine ends. Position must remain fresh during delays, gaps and
pulses. Age above three seconds, an invalid custom fix, interlock/fault or any
stop/disarm cancels the sequence and active routine. All later repeats are
cancelled. A GPIO watchdog enforces position inhibition independently of async
commands; Linux remains subject to OS scheduling and hardware-driver limitations.

## MAVLink

Configure the flight controller/companion to send **GPS_RAW_INT (24)** and
**GLOBAL_POSITION_INT (33)** to the Pi IP, UDP port **14550**, at 2–5 Hz or faster
within normal link capacity. `geo_udp_host`, `geo_udp_port`, `geo_system_id` and
`geo_component_id` in the installation JSON set the bind address/port and accepted
sender IDs (default 1/1). Both messages must be received; GPS fix type 3–6 and its
GPS report must remain fresh. A GPS report alone does not refresh aircraft position.

```text
GeoClear
GeoSource:MAVLINK
GeoAdd:37.4219999,-122.0840575,5,dots
GeoAdd:37.4221,-122.0841,5,dots
GeoSave
GeoStatus
GeoStart
```

Submit commands separately or in batches of at most eight. Supply fresh position
before GeoStart. The console sequences longer plan builds automatically.
Large HTTP plans save in batches of eight, with progress and bounded retries when
admission is busy. The saved plan has its own 256 KiB allowance; the Pi's capacity
is larger than the current ESP32's twelve-point storage. HTTP state returns the
full plan. Stream status remains compact: `@GEO:offset` reads up to sixteen points
with offset/count/points/next fields, and `GeoList:offset` prints the same page.
The BLE editor uses pages on **Load saved sequence**, avoiding a full-plan status
notification on every poll. A 500-point BLE import/read is slower than HTTP.

The receiver accepts whole MAVLink 1/2 frames, including multiple frames per
datagram, with a 1024-byte datagram limit. It verifies CRC extras, restores MAVLink
2 trailing zeros, uses GPS h_acc millimeters when present (zero means unknown),
and requires strictly advancing GLOBAL_POSITION_INT time_boot_ms, with wrap
handling. Repeated/reordered frames cannot refresh a fix. It rejects signing and
unknown incompatibility flags; there is no signature-verification implementation.
Only positions are received: no heartbeats, stream requests, mission uploads or
flight controls are transmitted. It does not authenticate an unsigned UDP sender.

After a flight-controller reboot, stop the sequence and send `GeoResetPosition`
to clear the previous boot clock before supplying fresh GPS/global reports.
Radio profiles that disable Wi-Fi also disable MAVLink reception.

Field layouts: [MAVLink common messages](https://mavlink.io/en/messages/common.html)
and [packet serialization](https://mavlink.io/en/guide/serialization.html).

## Custom provider API

Choose/save `GeoSource:API`. A provider must supply **aircraft** latitude,
longitude, horizontal accuracy and measurement time. Do not use handset location.
The console includes a one-sample bench input; a running sequence needs a continuous
feed from the actual position source.

POST to `http://<pi-ip>:8080/api/position` with content type `text/plain`:

```text
37.4219999,-122.0840575,1.5,100
```

Fields are latitude (-90…90), longitude (-180…180), accuracy (-1 unknown or
0…100000 meters), measurement age (0…3000 milliseconds). Maximum body 128 bytes.
Successful admission returns HTTP 200 with `accepted: true`; it does not report
point execution. Invalid fields, oversized/malformed data or stale measurement
age return 400 and invalidate the prior API fix. Source mismatch returns 400
without overriding MAVLink. Cross-origin browser posts are blocked. Send 2–5
samples/second; all network delay counts toward freshness. Each update replaces
the previous pending sample in a single slot, without filling the command queue.
`GeoPosition:lat,lon,accuracy,ageMs` is available for newline bench commands.

The ESP32's [position_bridge.py](../../ESP32_Dispenser_Controller/examples/position_bridge.py)
already supports this Pi endpoint. Feed newline JSON from your verified provider:

```json
{"latitude":37.4219999,"longitude":-122.0840575,"accuracyMeters":1.5,"measuredAtUnixMs":1780000000000}
```

The measurement timestamp must reflect the actual sample time, not this example.
Run from the repository root:

```sh
your-provider | python ESP32_Dispenser_Controller/examples/position_bridge.py http://<pi-ip>:8080
```

Its `--mavlink <pi-ip>` mode can instead emit the common GPS/global subset to
UDP 14550. A stale/invalid provider sample stops forwarding and the controller's
freshness timeout stops dispensing. XAG/proprietary apps need a verified vendor
aircraft-position adapter; no guessed proprietary API is implemented.

## Bench checks

Begin in simulation. Save a short pulse routine and a two-point API plan. Supply
fresh fixes, start explicitly, and verify ordered execution and disarming. Test
position loss during START_WAIT and while output is active; also test DispenseStop,
GeoStop, StopAll and the interlock. Reboot and verify the saved plan is stopped.
Repeat on each physical Pi with the actuator disconnected before live operation;
confirm wiring polarity, external inhibit and observed shutdown timing.
