# Guided field access setup

Double-click **Setup Field Access.cmd** in the repository root. It is the guided
path for the ESP32-S3 DevKitC-1 N16R8, SparkFun SARA-R4 shield and hosted console.
It defaults to the dispenser; use `-Variant s3-transport` for the generic S3 UI.

## Before the first run

Have an Ubuntu VPS with a public IPv4 address and SSH access. The SSH account
needs passwordless sudo, or use the server's root account. Supported releases
are Ubuntu 22.04, 24.04, 26.04 and 26.10. A fresh Ubuntu VPS is the simplest path;
Docker does not need to be preinstalled.

Have a domain you control. The wizard creates the names `control.yourdomain.com`
and `mqtt.yourdomain.com` from that one domain, and walks you through DNS below.
Allow inbound TCP 80, 443 and 8883 in the VPS cloud firewall,
plus your SSH port for administration. Have an activated compatible SIM, antenna,
shield power supply and the SIM provider's APN ready.

The wizard cannot purchase a VPS/domain, activate a SIM, change a cloud-account
firewall or create DNS records without access to those accounts. It checks DNS
and tells you when a record does not point at the selected server. SSH host-key,
login and Windows administrator prompts may appear on first use.

## Run the wizard

1. Double-click **Setup Field Access.cmd**.
2. Enter the server IP/hostname, SSH username, your domain and SIM APN.
   The two hostnames are derived automatically. On later runs, saved settings are reused.
3. Let it install server and Arduino dependencies and create matching settings.
4. When ready, connect the modem adapter and accept modem provisioning.
5. Reconnect the shield UART to the ESP32. Accept the optional compile/flash
   step when you are ready to build; flashing still asks you to confirm the ESP32
   COM port. Declining leaves setup/settings ready for a later build.
6. Open the printed HTTPS console URL and use the generated browser login.

The wizard uploads the current working sources, so it includes local changes
without requiring a Git commit or push. It sends only a whitelist of relay and
web files. Private settings, signing keys, precompiled images and node_modules
are excluded from the deployment archive.

## DNS without guessing

For `yourdomain.com`, setup prints the exact A-record names, VPS IPv4 address,
300-second TTL and DNS-only proxy setting. Other providers can use the generated
`field/<variant>-dns.zone` import file, or you can copy the two table rows into
their DNS editor. The wizard waits for your changes and rechecks them; Q exits
with the instructions saved. No server changes are made while DNS is pending.

For Cloudflare, accept the automatic DNS option. Create a token scoped to this
one zone with **Zone Read** and **DNS Edit/Write**, then paste it into the hidden
prompt. Setup creates or updates only the requested A records, sets proxy off,
and waits up to a minute for DNS visibility before offering a manual recheck.
Matching records are reused and no existing records are deleted. Duplicate A
records or conflicting CNAME/AAAA settings may need manual correction.

The token is not saved in the profile, report, firmware or server. Cloudflare
imports also honor the file's `cf-proxied:false` tag. See
[Cloudflare API](https://developers.cloudflare.com/api/resources/dns/subresources/records/methods/create/)
and [zone import](https://developers.cloudflare.com/dns/manage-dns-records/how-to/import-and-export/).

The server installer adds missing CA/curl/OpenSSL/Python/tar dependencies and
installs Docker Engine plus the Compose plugin from Docker's official Ubuntu
repository. A compatible existing Docker/Compose installation is reused. It
enables Docker, configures the selected UI, starts the services and checks relay
HTTP readiness and broker connectivity. Existing unrelated services and firewall
rules are not disabled. It refuses package changes that require removing an
existing package; a conflicting old Docker installation may need manual repair.

Existing credentials and the CA are reused when rerunning against the same
server names and device ID. A different existing deployment is rejected rather
than assigned new credentials. Failed initial credential generation is staged
away from the live configuration. An old signing key is kept in root-only
`relay/backups/ca.key`, outside the broker's mounted secrets folder.

## Automated modem provisioning

Use a **3.3 V USB-to-UART adapter** for this step. Disconnect the ESP32 UART from
the shield, put its routing switch at SOFT and set IOREF to 3.3 V. Power the
shield separately and turn the modem on.

| Adapter | Shield |
|---|---|
| TX | D9 (modem RX) |
| RX | D8 (modem TX) |
| GND | GND |

Keep VUSB_DET configured for UART operation. The adapter carries UART data; do
not use its small logic supply to power the modem. See the
[SparkFun hookup guide](https://learn.sparkfun.com/tutorials/lte-cat-m1nb-iot-shield-hookup-guide/all).

The tool asks you to confirm the adapter port, probes 115200 and 9600 baud and
requires a SARA-R4 model response before writing settings. It compares the stored
CA identifier, transfers the exact certificate bytes when needed, verifies the
stored result, sets/checks UTC time and configures CA/hostname/date verification
with TLS 1.2. It does not disable verification to make an unsupported modem work.
The detected baud is written to `LtePrivate.h` as `LTE_UART_BAUD`.

An identical stored CA is left in place on later runs. Only the application's
`relay-ca` certificate name is imported/replaced; no modem factory reset or
certificate-store wipe is performed. Modem clock correctness still matters after
future power loss; rerun provisioning if its clock is wrong and network time is
unavailable. The tool does not activate SIMs or bypass SIM PIN locks.

Afterward disconnect the adapter and reconnect **ESP32 GPIO17 <- shield D8**,
**GPIO18 -> shield D9**, and common ground. IOREF stays at 3.3 V. GPIO17/18 are
reserved for LTE in both S3 firmware variants.

## Commands and reruns

```powershell
# Review the complete flow without prompts or side effects:
.\controller.cmd field-setup -DryRun

# Run the guided setup:
.\controller.cmd field-setup -Variant s3-dispenser

# Use a root SSH account, or provide values directly:
.\tools\field_setup.ps1 -SshUser root -Server 203.0.113.10 `
  -WebDomain control.yourdomain.com -MqttDomain mqtt.yourdomain.com -Apn YOUR_APN

# Configure/recheck the modem later:
.\controller.cmd modem-provision -Variant s3-dispenser

# Build/upload later after completing setup:
.\controller.cmd compile -Variant s3-dispenser -Lte
.\controller.cmd flash -Variant s3-dispenser
```

`field_setup.ps1` also accepts `-SshPort`, `-DeviceId`, `-ApnUser`,
`-ApnPassword`, `-Domain`, custom `-WebDomain`/`-MqttDomain`, `-SkipModem`,
`-BuildAndFlash` and `-ToolsDirectory`.
Avoid putting private passwords into terminal history. `-BuildAndFlash` is an
explicit request to compile after setup; it does not bypass flash confirmation.
The default wizard asks before compiling. The older `setup`, `compile`, `flash`,
`ports` and `doctor` commands remain available.

For a server already containing the sources, the equivalent server-only command is:

```bash
cd ESP32S3_Modular_Command_Transport_Controller/relay
sudo bash install.sh --web-domain control.yourdomain.com \
  --mqtt-domain mqtt.yourdomain.com --device-id s3-controller --variant s3-dispenser
```

Add `--dry-run` to see its plan without changing anything.

## Private data and status

The server's root-readable `secrets/device-settings.json` contains the device MQTT
credentials, public CA and browser login. It excludes the CA signing key, internal
proxy key and relay broker password. The wizard retrieves it over SSH and stores
the local profile encrypted with Windows DPAPI for the current Windows account.
It generates the ignored `LtePrivate.h` with restricted file access. Changed
previous header settings are backed up encrypted; unrelated macros are preserved.
Keep the tools directory outside source control.

Setup displays and saves a **private credentials report** under
`%LOCALAPPDATA%\ESP32ControllerTools\field\<variant>-credentials.txt`, restricted
to the Windows account and SYSTEM. Its LTE section contains APN/user/password,
MQTT host, port 8883, TLS mode, device ID, MQTT username/password, CA store name,
certificate path/fingerprint, UART baud and generated firmware settings path.
The separate browser section contains the HTTPS URL and browser login. This
report is intentionally readable for manual entry into another LTE client;
the resumable profile remains DPAPI-encrypted. Keep the report private.

No secret
bundle is uploaded with the public-source package. On another Windows account or
computer, rerun the wizard over SSH to obtain its own protected local profile.

The server can be ready while the device is offline. Caddy's public HTTPS may
still be waiting for DNS/firewall/certificate issuance; the installer reports
that condition. Use the console's Connection panel once the page is reachable.
An LTE connection needs the modem physically wired, powered and provisioned, plus
the LTE-enabled firmware flashed. Hosting/software setup alone does not establish it.

Automated checks use fake package/container commands, temporary files and a
simulated modem. No real VPS deployment, modem provisioning or LTE firmware
compile/flash has been performed by adding this automation. First hardware and
Ubuntu deployment validation remains required.

References: [Docker Ubuntu install](https://docs.docker.com/engine/install/ubuntu/),
[u-blox SARA-R4 AT manual](https://files.seeedstudio.com/wiki/Wio_LTE_Cat_M1_NB-IoT_Tracker/res/SARA-R4-SARA-N4_ATCommands_%28UBX-17003787%29.pdf).

## Hosting at home instead

Use [Setup Home Access.cmd](../Setup%20Home%20Access.cmd) for a Windows laptop, or `controller home-setup`. It uses the same broker/web app and generates matching LTE/Wi-Fi TLS settings. See [home hosting](HOME_HOSTING.md) for router forwarding, public IP/CGNAT, DNS, laptop sleep and Docker startup. The VPS wizard assumes a public server address and should not be used to point DNS at a private home LAN address.
