#!/bin/sh
# Run on Raspberry Pi OS: sudo sh deploy/install.sh
set -eu
if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo sh deploy/install.sh" >&2
    exit 1
fi
SOURCE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TARGET_DIR=/opt/pi-controller
ENABLE_OS_CONTROL=0
ENABLE_USB_GADGET=0
for flag in "$@"; do
    case "$flag" in
        --enable-os-control) ENABLE_OS_CONTROL=1 ;;
        --usb-gadget) ENABLE_USB_GADGET=1 ;;
        *) echo "Unknown option: $flag" >&2; exit 1 ;;
    esac
done
apt-get update
apt-get install -y python3-venv python3-pip python3-aiohttp python3-serial python3-gpiozero python3-lgpio python3-smbus bluez network-manager iw sudo avahi-daemon
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else "Python 3.11 or newer required")'
if ! id pi-controller >/dev/null 2>&1; then
    useradd --system --user-group --home-dir /var/lib/pi-controller --shell /usr/sbin/nologin pi-controller
fi
usermod -a -G gpio,dialout,i2c pi-controller
install -d -m 0755 "$TARGET_DIR" /etc/pi-controller
# Stop before replacing service code; the shutdown handler disarms the output.
systemctl stop pi-controller.service 2>/dev/null || true
python3 -m venv --system-site-packages "$TARGET_DIR/.venv"
# The OS packages avoid C-extension wheel builds on the ARMv6 Zero W.
"$TARGET_DIR/.venv/bin/python" -m pip install dbus-next==0.2.3
python3 - "$SOURCE_DIR" "$ENABLE_OS_CONTROL" <<'PY'
import json, secrets, sys
from pathlib import Path
source = Path(sys.argv[1])
sys.path.insert(0, str(source))
from bridge.config import DEFAULTS, validate
from bridge.release import build_bundle, validate_bundle
from bridge import VERSION
config_path = Path('/etc/pi-controller/config.json')
config = dict(DEFAULTS)
config.update(json.loads(config_path.read_text()) if config_path.exists() else json.loads((source / 'config.example.json').read_text()))
config['data_dir'] = '/var/lib/pi-controller'
if sys.argv[2] == '1': config['enable_os_control'] = True
if not config['update_token']: config['update_token'] = secrets.token_urlsafe(32)
validate(config)
config_path.write_text(json.dumps(config, indent=2) + '\n')
sys.path.insert(0, str(source / 'deploy'))
from admin import HostManager
manager = HostManager(config)
manager.recover_update()
bundle = validate_bundle(build_bundle(source, VERSION))
manager.switch(manager.install_release(bundle))
PY
chown root:pi-controller /etc/pi-controller/config.json
chmod 0640 /etc/pi-controller/config.json
install -m 0644 "$SOURCE_DIR/deploy/pi-controller-dbus.conf" /etc/dbus-1/system.d/pi-controller.conf
install -d -m 0755 /usr/local/lib/pi-controller
install -m 0644 "$SOURCE_DIR/deploy/admin.py" /usr/local/lib/pi-controller/admin.py
install -m 0644 "$SOURCE_DIR/bridge/release.py" /usr/local/lib/pi-controller/release.py
SUDO_RULE=$(mktemp)
trap 'rm -f "$SUDO_RULE"' EXIT HUP INT TERM
echo 'pi-controller ALL=(root) NOPASSWD: /usr/bin/python3 -I /usr/local/lib/pi-controller/admin.py' > "$SUDO_RULE"
visudo -cf "$SUDO_RULE"
install -m 0440 "$SUDO_RULE" /etc/sudoers.d/pi-controller
install -m 0644 "$SOURCE_DIR/deploy/pi-controller.service" /etc/systemd/system/pi-controller.service
install -m 0644 "$SOURCE_DIR/deploy/pi-controller-recovery.service" /etc/systemd/system/pi-controller-recovery.service
if [ "$ENABLE_USB_GADGET" = 1 ]; then
    python3 "$SOURCE_DIR/tools/configure_usb_gadget.py"
fi
systemctl daemon-reload
systemctl enable --now pi-controller.service
echo "Installed. Open http://<pi-address>:8080/ (simulation by default)."
echo "Edit /etc/pi-controller/config.json, then restart pi-controller to enable hardware or BLE/SPP."
echo "The private update token is in /etc/pi-controller/config.json; it is never printed in logs."
