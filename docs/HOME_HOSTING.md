# Host field access on your home laptop

The laptop runs Mosquitto, the command/status relay and the same HTTPS web app.
The ESP32 connects **outbound over LTE or its configured Wi-Fi Internet connection**
to `mqtt.yourdomain.com:8883`. Open `https://control.yourdomain.com` on a phone or
laptop and log in with the separate browser credentials printed by setup.
No public SIM address, database, inbound port on the modem, or browser MQTT
password is needed. Both S3 variants work; dispensing controls stay first.

## Windows: guided setup

1. On the laptop that will stay home, double-click [Setup Home Access.cmd](../Setup%20Home%20Access.cmd).
   Choose the desired variant with `controller home-setup -Variant s3-transport`
   instead if using the generic controller (default: S3 dispenser).
2. Enter your domain and SIM APN. Setup proposes `control.yourdomain.com` and
   `mqtt.yourdomain.com`, detects your public IPv4 and asks you to check it against
   your router's WAN address. They must describe your **home connection**, not the
   ESP32 address or laptop's private `192.168...` address.
3. Missing WSL features and Docker Desktop are installed. Windows can request
   administrator approval or a restart; rerun after restarting. Complete Docker
   Desktop's first-run/license screens and use Linux containers / WSL 2. Python
   and OpenSSL run in a temporary container; no separate Windows installs needed.
4. Setup prints/imports DNS records, optionally creates them through Cloudflare,
   generates and saves private credentials, starts the three services and writes
   `LtePrivate.h` including the public CA for Wi-Fi fallback. It offers confirmed
   modem CA/clock provisioning. It **does not compile or flash**.
5. Add the router rules below, then test from phone cellular data with Wi-Fi off.
   Log in using the printed **browser** username/password. Check connection in the
   console distinguishes a working host/broker from an online controller.

This installs/configures a host only when you run the wizard. A preview without
changes is `controller home-setup -DryRun`.

## Router and Windows settings

Reserve the laptop's private LAN address in the router's DHCP/reservations page
(example `192.168.1.50`). Setup prints the detected laptop and router addresses;
if several network adapters/VPNs are active, use the address on your home router.

Create these rules in **Port forwarding / NAT / Virtual server**:

| Protocol | External port | Destination | Internal port | Purpose |
| --- | --- | --- | --- | --- |
| TCP | 80 | Laptop LAN address | 80 | Certificate issuance/renewal and HTTPS redirect |
| TCP | 443 | Laptop LAN address | 443 | Browser login and live commands/status |
| TCP | 8883 | Laptop LAN address | 8883 | MQTT over TLS from ESP32 |

Use individual TCP rules, not a DMZ rule. Keep the same external/internal ports.
Do not expose MQTT 1883, relay 8080, Docker's API or your router's admin page.
If a router WAN/admin service occupies 80/443, move/disable its WAN admin service.
Existing web servers on the laptop must release these ports before Docker starts.

Setup adds inbound Windows Firewall rules for these ports on **Private** networks.
In Windows Settings > Network & Internet, mark your trusted home connection
Private. Docker Desktop publishes these ports to the Windows host/local network;
you forward to the **Windows laptop**, not a changing WSL virtual address.
See [Docker Desktop networking](https://docs.docker.com/desktop/features/networking/).

## DNS, NAT and ISP checks

At your DNS provider create two **DNS-only A records** to the router's **public**
IPv4: `control.yourdomain.com` and `mqtt.yourdomain.com`. Setup prints an importable
zone file or offers Cloudflare Zone Read + DNS Edit automation. Do not orange-cloud
the MQTT name. Remove conflicting AAAA records unless IPv6 is configured too.

Compare the router's WAN IPv4 with the setup-detected public IPv4:

- If they match and the ISP allows incoming TCP, forwarding should work.
- If the router has a private WAN address because of another home router/modem,
  bridge the upstream router or forward the same ports on both devices.
- If the ISP uses CGNAT (often WAN `100.64.0.0/10`, or another private address),
  ordinary port forwarding cannot open it. Ask the ISP for public IPv4/inbound
  service, or use [Setup Field Access.cmd](../Setup%20Field%20Access.cmd) on a VPS.
- If your public address changes, configure router DDNS or rerun home setup with
  the new public IPv4 and update **both** names. Credentials/CA are reused when
  hostnames and device identity match; no firmware change is needed for an A
  record change.

A normal Cloudflare Tunnel for arbitrary TCP needs client-side `cloudflared`,
which SARA-R4 cannot run; it is not a direct substitute for reachable MQTT/TLS.
See [Cloudflare arbitrary TCP](https://developers.cloudflare.com/cloudflare-one/access-controls/applications/non-http/cloudflared-authentication/arbitrary-tcp/).

Test on cellular first. A router without NAT loopback may fail from home Wi-Fi
even while remote access works. Enable NAT loopback/hairpin or create local DNS
overrides for **both original hostnames** to the laptop LAN IP. Keep the hostnames
for TLS validation; do not replace `LTE_BROKER_HOST` with a numeric LAN address.
This also lets the ESP32's Wi-Fi fallback reach the same home broker locally.

Caddy obtains/renews the public browser certificate after DNS and inbound routing
work. Port 80 supports HTTP certificate validation; see
[Let's Encrypt guidance](https://letsencrypt.org/docs/allow-port-80/). Allow outbound
DNS/HTTPS from the host and DNS/NTP/MQTT from the controller's Wi-Fi network.

## Files, credentials and keeping it running

Setup prints the exact locations. Defaults under `%LOCALAPPDATA%\ESP32ControllerTools`:

- `field\<variant>-credentials.txt`: private report with APN, MQTT hostname,
  port 8883, device ID, username/password, CA file/fingerprint and browser login.
- `field\<variant>.protected`: settings encrypted for your Windows account.
- `home\<variant>\ROUTER-SETUP.txt`: your specific port-forward/DNS checklist.
- `home\<variant>\server\ESP32S3_Modular_Command_Transport_Controller\relay`:
  `.env`, `secrets`, CA signing key under separate `backups`, and host configuration.
  Private host folders allow your Windows account and SYSTEM; back them up securely.

Use one home deployment at a time on one laptop: both variants bind the same
three ports. Reruns reuse the original host files and credentials, not rotate keys
or replace a running host with a different device. A separate `-ToolsDirectory`
creates an independent installation, but requires different host/ports to run
simultaneously. Reruns currently do not update an existing source snapshot.

From the printed relay directory:

```powershell
docker compose --project-name esp32-home-s3-dispenser ps
docker compose --project-name esp32-home-s3-dispenser logs --tail 100
docker compose --project-name esp32-home-s3-dispenser down
docker compose --project-name esp32-home-s3-dispenser up -d
```

Use `esp32-home-s3-transport` for the generic variant. `down` leaves credentials and
named volumes intact; avoid `down -v` if you want to preserve Caddy certificates.
The private broker certificate lasts one year; renew it with the saved CA before
expiry (Caddy renews the browser certificate automatically). No insecure TLS bypass
is enabled. Keep the laptop plugged in, disable sleep while plugged in, and enable
Docker Desktop > Settings > General > Start Docker Desktop when you sign in.
Windows/Docker first-run and router configuration remain interactive steps.

## Linux laptop or home server

On an Ubuntu home machine use the manual relay installer below. The existing
[VPS wizard](FIELD_SETUP.md) assumes its SSH server address is also the public DNS
target, so it should not point DNS at your home machine's private LAN address:

```sh
cd ESP32S3_Modular_Command_Transport_Controller/relay
sudo bash install.sh --web-domain control.yourdomain.com --mqtt-domain mqtt.yourdomain.com --device-id s3-controller --variant s3-dispenser
```

Allow TCP 80/443/8883 in the Linux host firewall too, if enabled. Forward the router
ports to the Linux host's LAN IP, and point DNS to the home public IP as above.
Retrieve `secrets/device-settings.json` privately and apply matching firmware
settings/CA as described in the [LTE guide](../ESP32S3_Modular_Command_Transport_Controller/docs/LTE_FIELD_ACCESS.md).
