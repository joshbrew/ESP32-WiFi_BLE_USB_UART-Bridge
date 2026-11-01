# Host the field console

This is a self-hostable, single-device relay bundle. It is not deployed yet. It needs a Linux server with a public IP, Docker Compose, OpenSSL and two DNS names pointing to it, for example `control.your-domain.com` and `mqtt.your-domain.com`. Allow inbound TCP 80/443 for HTTPS and 8883 for the modem. Node's internal port 8080 is not exposed. A static-only web host cannot run this MQTT broker.

Copy the repository to that host, enter `ESP32S3_Modular_Command_Transport_Controller/relay`, and run once as root (the script assigns the broker key to its container user):

```sh
sudo sh setup.sh control.your-domain.com mqtt.your-domain.com s3-controller
```

The script refuses to overwrite an existing `.env` or `secrets` directory. It generates independent browser, device, relay and internal proxy credentials. Save `secrets/device-settings.txt` privately. Move the CA private key to encrypted offline storage after signing; only the public CA certificate goes to the modem. Never commit `.env`, `secrets` or `LtePrivate.h`.

For the generic S3 firmware, keep `WEB_SOURCE=../web`. For the dispenser, set this in `.env` before starting:

```text
WEB_SOURCE=../../ESP32S3_Dispenser_Controller/web
```

Then start the bundle:

```sh
sudo docker compose config --quiet
sudo docker compose up -d --build
sudo docker compose logs --tail=80
```

Open `https://control.your-domain.com` from a laptop or phone, and use the browser's username/password prompt (`field` and the generated password). Caddy obtains the public HTTPS certificate when DNS/ports are ready. It protects the HTML, scripts, API and WebSocket path. Basic authentication stores a password hash and needs HTTPS; see [Caddy's documentation](https://caddyserver.com/docs/caddyfile/directives/basic_auth). The browser never receives device/broker passwords. Browser login credentials may be cached by the browser; use a private session on shared devices. This initial small relay has one operator account and one device; per-person accounts/SSO and device selection can be added later.

Complete modem CA, clock and firmware configuration in [LTE field access](../docs/LTE_FIELD_ACCESS.md). Until the physical device connects, the page explicitly says Device offline, with Send disabled. HTTPS login alone does not establish device connectivity. Local Wi-Fi/BLE operation remains available using the device's own console.

## Connection diagnostics

The console's Connection disclosure shows the browser link and device state age
live. Open it or press Check connection for a current broker report. This read-only
request shares the browser's background request path, yields to Stop and never
resends a command or refreshes the controller's state timestamp. An expired login
has a visible sign-in message. Broker reports are cleared when their connection
scope changes, and each successful check displays its time.

Open `/api/health` on your logged-in console host to see relay readiness, broker
connection/subscription status and the device state age. Broker sign-in, TLS,
DNS and topic-permission failures have separate fixed messages. Credentials and
raw connection errors are excluded from the response.

Compose checks the internal authenticated `/healthz` route every 30 seconds.
This checks the HTTP service and its console assets. An offline modem or broker
does not make the HTTP container unhealthy: the login and diagnostics must stay
available while the device reconnects. Device readiness is a separate field.
Inspect container status with `sudo docker compose ps`, then use `/api/health`
to distinguish a host problem from a broker or device problem. See
[Compose health checks](https://docs.docker.com/reference/compose-file/services/#healthcheck).

Missing console files stop startup with a direct setup error. `WEB_SOURCE` selects
the mounted web folder in Compose. If running Node directly, `WEB_DIRECTORY` is
the optional absolute web folder; otherwise it uses the sibling `../web` folder.

## Operations

The broker requires device credentials and restricts topics by role. Commands/watch are relay-only writes; device state/events/acks are device-only writes. It uses TLS 1.2 minimum, clean sessions, no retained messages and no persistent offline command queue. The relay also checks the browser Origin and bounds messages, event history, pending requests, WebSocket clients and command rate. Its private proxy key cannot be supplied by an external client to bypass Caddy login: Caddy overwrites that header. See [Mosquitto configuration](https://mosquitto.org/man/mosquitto-conf-5.html).

Device presence alone does not establish the current control state. Normal
commands receive HTTP 409 until the relay has a snapshot from that device session.
Stop/disarm requests remain available while waiting. The UI shows UNKNOWN for
the dispenser and disables normal actions until the snapshot arrives, so an old
armed state is not shown as current after reconnect. Routine HTTP state reads
request a fresh snapshot from the connected modem worker automatically.

Stop/disarm requests have reserved capacity and bypass the normal operator request limit. They cancel earlier pending retries and advance the device's stop counter so delayed older commands cannot be admitted afterward. Protocol 2 firmware is required. Stopping does not prove an earlier physical action never ran; unconfirmed requests keep that distinction in their error message. A late acknowledgement for the newest Stop can clear the pending indicator, even after its HTTP request timed out. The earlier timeout remains unconfirmed in the log; the UI reports acknowledgement by the controller queue and asks you to check Output for execution results. Older Stop acknowledgements cannot clear a newer pending Stop.

On relay restart, a new process token invalidates older browser requests before publishing anything to MQTT. The live UI updates its token and cancels its older queued work. API clients must read the current `uplink.relaySession` from authenticated `/api/state` and supply it in `X-Relay-Session`, capturing it once for each logical request and keeping it unchanged on retries. The token is a restart guard; authentication still comes from HTTPS login and the private proxy header. Normal command requests also require `X-Device-Session` from that same state snapshot. Capture both headers once for a logical request and preserve them on retries. The relay rejects a previous device session with HTTP 409 before publishing. The browser cancels its older queued requests when the device reconnects. Priority Stop/disarm can target the current device even if the browser has not received its new session yet.

Output recovery uses `/ws?since=<cursor>&deviceSession=<device-session>&relaySession=<relay-session>`. The first frame has type `sync`, with `data.state` and `data.history`; subsequent frames have type `state` or `event`. History contains only retained output after the supplied cursor, or resets the cursor when the connection scope changes. Its replay byte limit is 24,000 bytes and its storage limit is 128 records. Known gaps are explicit. Numbered messages are deduplicated, and event metadata is bounded before storage. Command acknowledgements are separate from output cursors.

HTTP readers can use the same `since`, `deviceSession` and `relaySession` query parameters on `/api/events`, with `limit` from 1 to 16. Follow `cursor` and `more`, and honor `reset`/`gap`. Save log in the browser keeps a local text copy of displayed output. The relay still has no persistent message archive.

The broker certificate generated by setup lasts 365 days. Renew it before expiry with the same CA and hostname/SANs, update `secrets/broker.crt`/`broker.key`, restore UID 1883 ownership and mode 600 on the key, and restart the broker. The modem clock must remain correct. Rotating the CA also requires reprovisioning the modem. Caddy renews its separate browser certificate automatically.

Back up `.env` and operator/device credentials privately. Deployments should pin reviewed image digests and install host/image security updates. Node dependencies are pinned in `package-lock.json`. Firmware upload over LTE is intentionally not provided in this iteration. Data/history is in memory; restarting the relay clears console history and fails outstanding requests. Review the actual device state before manually resending an uncertain physical action.

## Checks without firmware compilation

```sh
npm ci --ignore-scripts
npm test
npm run test:server
node --check server.mjs
```

The Node tests cover offline rejection, duplicates, expiry, connection sessions, retained-message handling, late Stop acknowledgements and health diagnostics. The server integration check needs OpenSSL (set `OPENSSL` to its executable if needed), and uses a local TLS/MQTT simulator to check command/ack bridging, proxy authentication headers, Origin checks, WebSocket events, rejected subscriptions, health authentication and missing console assets. Browser tests under the sketch's `tests` exercise local and remote presentation against mocks. Container startup, public DNS/HTTPS and modem TLS must be verified on the selected host/hardware before field use. No host has been selected or paid for by this task.
