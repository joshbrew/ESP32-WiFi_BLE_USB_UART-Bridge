#!/usr/bin/python3
"""Installed root helper. No arbitrary command, path, service, or shell input.

Service sudo permission invokes this exact file without arguments. Operations
run in separate systemd units so service restarts cannot interrupt rollback.
"""
import configparser
import hashlib
import importlib.util
import json
import os
import re
import stat
import subprocess
import sys
import time
import tempfile
import urllib.request
import uuid
from pathlib import Path


CONFIG = Path("/etc/pi-controller/config.json")
STATE = Path("/var/lib/pi-controller")
INSTALL = Path("/opt/pi-controller")
NETWORK = Path("/etc/NetworkManager/system-connections")
HELPER = "/usr/local/lib/pi-controller/admin.py"
RADIO_RECORD = Path("/run/pi-controller-radio.json")
OPS = {"radio", "radioCommit", "radioRollback", "restart", "reboot", "update"}


def command(arguments, timeout=35):
    result = subprocess.run(arguments, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, timeout=timeout, env={"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL": "C"})
    return result.stdout.strip()


def atomic_json(path, data, mode=0o600):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, "w") as file:
            json.dump(data, file)
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
        os.chmod(path, mode)
        sync_directory(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def sync_directory(path):
    if os.name == "posix":
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try: os.fsync(fd)
        finally: os.close(fd)


def validate_request(request, config):
    if not isinstance(request, dict) or request.get("operation") not in OPS:
        raise ValueError("unsupported OS operation")
    operation = request["operation"]
    if operation == "update":
        if not config.get("update_token") or set(request) != {"operation"}:
            raise ValueError("updates are disabled")
    elif not config.get("enable_os_control"):
        raise ValueError("OS control is disabled")
    if operation == "radio":
        if set(request) != {"operation", "settings", "wifi"} or type(request["wifi"]) is not bool:
            raise ValueError("invalid radio request")
        settings = request["settings"]
        keys = {"wifiMode", "fallbackAP", "staSSID", "staPassword", "apSSID", "apPassword", "txPower"}
        if not isinstance(settings, dict) or set(settings) != keys:
            raise ValueError("invalid radio settings")
        if settings["wifiMode"] not in {"AP", "STA", "APSTA"} or type(settings["fallbackAP"]) is not bool:
            raise ValueError("invalid radio role")
        for key in ("staSSID", "apSSID", "staPassword", "apPassword"):
            value = settings[key]
            if not isinstance(value, str) or any(ord(c) < 32 or ord(c) == 127 for c in value):
                raise ValueError("invalid Wi-Fi text")
            if len(value.encode()) > (32 if key.endswith("SSID") else 63):
                raise ValueError("Wi-Fi field too long")
        if not settings["apSSID"] or not 8 <= len(settings["apPassword"]) <= 63:
            raise ValueError("invalid AP credentials")
        if settings["staPassword"] and not 8 <= len(settings["staPassword"]) <= 63:
            raise ValueError("invalid station password")
        power = settings["txPower"]
        if power not in {"LOW", "MAX"} and (not isinstance(power, str) or not re.fullmatch(r"(?:[0-9]|[12][0-9]|30)(?:\.[0-9]+)?", power) or not 0 <= float(power) <= 30):
            raise ValueError("invalid transmission power")
        for key in ("wifi_interface", "wifi_ap_interface"):
            if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_.-]{0,14}", config[key]):
                raise ValueError("invalid installation interface")
    elif set(request) != {"operation"}:
        raise ValueError("unexpected OS operation fields")
    return operation


class HostManager:
    def __init__(self, config, runner=command, state=STATE, install=INSTALL, network=NETWORK, radio_record=RADIO_RECORD):
        self.config, self.run = config, runner
        self.state, self.install, self.network, self.radio_record = map(Path, (state, install, network, radio_record))

    def managed_connections(self):
        return {"STA": ("PiControllerSTA", self.config["wifi_interface"]),
                "AP": ("PiControllerAP", self.config["wifi_ap_interface"])}

    def snapshot(self):
        interfaces = {self.config["wifi_interface"], self.config["wifi_ap_interface"]}
        active = []
        for line in self.run(["/usr/bin/nmcli", "-t", "-f", "UUID,DEVICE", "connection", "show", "--active"]).splitlines():
            ident, _, interface = line.partition(":")
            if interface in interfaces:
                active.append((ident, interface))
        files = {}
        for name, _ in self.managed_connections().values():
            path = self.network / (name + ".nmconnection")
            if path.is_symlink(): raise ValueError("managed connection file is a symbolic link")
            files[path.name] = path.read_text() if path.exists() else None
        return dict(active=active, files=files, powered=self.run(["/usr/bin/nmcli", "radio", "wifi"]) == "enabled",
                    token=uuid.uuid4().hex)

    def keyfile(self, role, settings):
        name, interface = self.managed_connections()[role]
        ident = str(uuid.uuid5(uuid.NAMESPACE_DNS, name + ":" + interface))
        text = configparser.ConfigParser(interpolation=None)
        text["connection"] = {"id": name, "uuid": ident, "type": "wifi", "interface-name": interface, "autoconnect": "false"}
        ssid = settings["apSSID" if role == "AP" else "staSSID"]
        text["wifi"] = {"mode": "ap" if role == "AP" else "infrastructure",
                        "ssid": ";".join(str(byte) for byte in ssid.encode()) + ";"}
        password = settings["apPassword" if role == "AP" else "staPassword"]
        if password:
            # GKeyFile escaping prevents backslashes in a password changing its
            # parsed meaning. ConfigParser itself does not perform interpolation.
            escaped = password.replace("\\", "\\\\").replace("\t", "\\t")
            if escaped.startswith(" "): escaped = "\\s" + escaped[1:]
            if escaped.endswith(" "): escaped = escaped[:-1] + "\\s"
            text["wifi-security"] = {"key-mgmt": "wpa-psk", "psk": escaped}
        text["ipv4"] = {"method": "shared" if role == "AP" else "auto"}
        text["ipv6"] = {"method": "disabled" if role == "AP" else "auto"}
        path = self.network / (name + ".nmconnection")
        self.network.mkdir(parents=True, exist_ok=True)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "w") as file:
            text.write(file, space_around_delimiters=False)
        os.chmod(path, 0o600)
        return ident

    def restore_radio(self, token=None):
        if not self.radio_record.exists(): return
        backup = json.loads(self.radio_record.read_text())
        if token and token != backup["token"]: return
        for interface in {self.config["wifi_interface"], self.config["wifi_ap_interface"]}:
            try:
                self.run(["/usr/bin/nmcli", "device", "disconnect", interface])
            except (subprocess.SubprocessError, OSError):
                pass
            self.run(["/usr/sbin/iw", "dev", interface, "set", "txpower", "auto"])
        for name, content in backup["files"].items():
            if name not in {"PiControllerSTA.nmconnection", "PiControllerAP.nmconnection"}:
                raise ValueError("invalid rollback file")
            path = self.network / name
            if content is None:
                path.unlink(missing_ok=True)
            else:
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
                with os.fdopen(fd, "w") as file: file.write(content)
        self.run(["/usr/bin/nmcli", "connection", "reload"])
        self.run(["/usr/bin/nmcli", "radio", "wifi", "on" if backup["powered"] else "off"])
        for ident, interface in backup["active"]:
            self.run(["/usr/bin/nmcli", "--wait", "20", "connection", "up", "uuid", ident, "ifname", interface])
        self.radio_record.unlink(missing_ok=True)

    def radio(self, settings, powered):
        if self.radio_record.exists():
            self.restore_radio()
        snapshot = self.snapshot()
        atomic_json(self.radio_record, snapshot)
        self.run(["/usr/bin/systemd-run", "--collect", "--unit=pi-controller-radio-guard-" + snapshot["token"],
                  "--on-active=90", "--timer-property=AccuracySec=1s", "--timer-property=RemainAfterElapse=no",
                  "/usr/bin/python3", "-I", HELPER, "--radio-guard", snapshot["token"]])
        try:
            self.run(["/usr/bin/nmcli", "radio", "wifi", "on" if powered else "off"])
            if not powered: return dict(ok=True, role="off")
            ap = self.keyfile("AP", settings)
            sta = self.keyfile("STA", settings) if settings["staSSID"] else None
            self.run(["/usr/bin/nmcli", "connection", "reload"])
            role = settings["wifiMode"]
            connected = False
            if role != "AP" and sta:
                try:
                    self.run(["/usr/bin/nmcli", "--wait", "20", "connection", "up", "uuid", sta, "ifname", self.config["wifi_interface"]])
                    connected = True
                except (subprocess.SubprocessError, OSError):
                    if not settings["fallbackAP"] and role == "STA": raise
            if role == "AP" or not connected and (settings["fallbackAP"] or role == "APSTA"):
                self.run(["/usr/bin/nmcli", "--wait", "20", "connection", "up", "uuid", ap, "ifname", self.config["wifi_ap_interface"]])
                actual = "AP"
            elif connected and role == "APSTA" and self.config["wifi_interface"] != self.config["wifi_ap_interface"]:
                self.run(["/usr/bin/nmcli", "--wait", "20", "connection", "up", "uuid", ap, "ifname", self.config["wifi_ap_interface"]])
                actual = "AP+STA"
            elif connected:
                actual = "STA"
            else:
                raise ValueError("station credentials absent and fallback AP disabled")
            power = settings["txPower"]
            # MAX restores driver/regulatory automatic control, rather than
            # assuming ESP32's highest calibrated dBm applies to Broadcom radios.
            for interface in ({self.config["wifi_interface"], self.config["wifi_ap_interface"]} if actual == "AP+STA" else {self.config["wifi_ap_interface"] if actual == "AP" else self.config["wifi_interface"]}):
                arguments = ["/usr/sbin/iw", "dev", interface, "set", "txpower"]
                arguments += ["auto"] if power == "MAX" else ["fixed", str(round((11 if power == "LOW" else float(power)) * 100))]
                self.run(arguments)
            return dict(ok=True, role=actual)
        except BaseException:
            self.restore_radio(snapshot["token"])
            raise

    def switch(self, release):
        self.install.mkdir(parents=True, exist_ok=True)
        temporary = self.install / "current.next"
        temporary.unlink(missing_ok=True)
        temporary.symlink_to(release, target_is_directory=True)
        os.replace(temporary, self.install / "current")
        sync_directory(self.install)

    def install_release(self, bundle):
        destination = self.install / "releases" / bundle["releaseId"]
        if destination.exists():
            if destination.is_symlink(): raise ValueError("release directory is a symbolic link")
            # Existing committed immutable release can be reused only verbatim.
            for name, content in bundle["files"].items():
                path = destination / name
                if path.is_symlink() or path.read_bytes() != content: raise ValueError("existing release differs")
            return destination
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".stage-", dir=destination.parent) as directory:
            staging = Path(directory)
            for name, content in bundle["files"].items():
                path = staging / name
                path.parent.mkdir(parents=True, exist_ok=True, mode=0o755)
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
                with os.fdopen(fd, "wb") as file:
                    file.write(content); file.flush(); os.fsync(file.fileno())
            os.chmod(staging, 0o755)
            os.replace(staging, destination)
            sync_directory(destination.parent)
        return destination

    def apply_update(self, validator, health=None):
        path = self.state / "pending-update.zip"
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as file:
            if os.fstat(file.fileno()).st_size > 8 * 1024 * 1024: raise ValueError("update too large")
            bundle = validator(file.read(8 * 1024 * 1024 + 1))
        old = (self.install / "current").resolve(strict=True)
        releases = (self.install / "releases").resolve(strict=True)
        if old.parent != releases: raise ValueError("current release is outside managed releases")
        candidate = self.install_release(bundle)
        status = dict(phase="applying", candidate=bundle["releaseId"], previous=old.name,
                      version=bundle["version"], trial=uuid.uuid4().hex)
        atomic_json(self.state / "update-status.json", status, 0o644)
        self.run(["/usr/bin/systemd-run", "--collect", "--unit=pi-controller-update-guard-" + status["trial"],
                  "--on-active=90", "--timer-property=AccuracySec=1s", "--timer-property=RemainAfterElapse=no",
                  "/usr/bin/python3", "-I", HELPER, "--update-guard", status["trial"]])
        try:
            self.switch(candidate)
            self.run(["/usr/bin/systemctl", "restart", "pi-controller.service"])
            if health:
                healthy = health(bundle["releaseId"])
            else:
                host = self.config.get("host", "127.0.0.1")
                if host in {"0.0.0.0", "::"}: host = "127.0.0.1"
                if ":" in host: host = "[" + host + "]"
                url = f"http://{host}:{self.config['port']}/api/ping"
                deadline = time.monotonic() + 35
                healthy = False
                while time.monotonic() < deadline:
                    try:
                        with urllib.request.urlopen(url, timeout=2) as response:
                            data = json.load(response)
                        if data.get("ok") and data.get("ready") and data.get("releaseId") == bundle["releaseId"]:
                            healthy = True
                            break
                    except (OSError, ValueError):
                        pass
                    time.sleep(.5)
            if not healthy: raise ValueError("candidate failed startup health check")
            status["phase"] = "committed"
        except BaseException:
            self.switch(old)
            self.run(["/usr/bin/systemctl", "restart", "pi-controller.service"])
            status["phase"] = "rolled-back"
            atomic_json(self.state / "update-status.json", status, 0o644)
            raise
        atomic_json(self.state / "update-status.json", status, 0o644)
        path.unlink(missing_ok=True)
        return dict(ok=True, **status)

    def recover_update(self, trial=None, restart=False):
        path = self.state / "update-status.json"
        if not path.exists(): return
        status = json.loads(path.read_text())
        if status.get("phase") != "applying" or trial and status.get("trial") != trial:
            return
        for key in ("candidate", "previous"):
            if not isinstance(status.get(key), str) or not re.fullmatch(r"[0-9a-f]{64}", status[key]):
                raise ValueError("invalid update recovery record")
        releases = (self.install / "releases").resolve(strict=True)
        previous = (releases / status["previous"]).resolve(strict=True)
        if previous.parent != releases: raise ValueError("recovery release is outside managed releases")
        self.switch(previous)
        status["phase"] = "rolled-back"
        atomic_json(path, status, 0o644)
        if restart: self.run(["/usr/bin/systemctl", "restart", "pi-controller.service"])


def validator():
    spec = importlib.util.spec_from_file_location("pi_release_validator", "/usr/local/lib/pi-controller/release.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.validate_bundle


def main():
    if os.geteuid() != 0: raise ValueError("root helper requires sudo")
    config = json.loads(CONFIG.read_text())
    manager = HostManager(config)
    if sys.argv[1:] == ["--recover-update"]:
        manager.recover_update(); return
    if len(sys.argv) == 3 and sys.argv[1] == "--update-guard" and re.fullmatch(r"[0-9a-f]{32}", sys.argv[2]):
        manager.recover_update(sys.argv[2], restart=True); return
    if len(sys.argv) == 3 and sys.argv[1] == "--radio-guard" and re.fullmatch(r"[0-9a-f]{32}", sys.argv[2]):
        manager.restore_radio(sys.argv[2]); return
    if sys.argv[1:] == ["--apply-update"]:
        try:
            result = manager.apply_update(validator())
            print(json.dumps(result))
        except Exception:
            status = STATE / "update-status.json"
            if not status.exists() or json.loads(status.read_text()).get("phase") != "rolled-back":
                pending = STATE / "pending-update.zip"
                candidate = hashlib.sha256(pending.read_bytes()).hexdigest() if pending.exists() else ""
                atomic_json(status, dict(phase="failed", candidate=candidate, detail="Update rejected before activation"), 0o644)
            raise
        return
    request = json.loads(sys.stdin.buffer.read(8193))
    operation = validate_request(request, config)
    if sys.argv[1:] == ["--execute"]:
        if operation == "radio": result = manager.radio(request["settings"], request["wifi"])
        elif operation == "radioRollback": manager.restore_radio(); result = dict(ok=True)
        elif operation == "radioCommit": manager.radio_record.unlink(missing_ok=True); result = dict(ok=True)
        else: raise ValueError("unsupported synchronous OS operation")
    elif sys.argv[1:]:
        raise ValueError("unsupported helper arguments")
    elif operation in {"restart", "reboot", "update"}:
        name = "pi-controller-" + operation + "-" + uuid.uuid4().hex
        arguments = ["/usr/bin/systemd-run", "--collect", "--quiet", "--unit=" + name, "--on-active=2",
                     "--timer-property=AccuracySec=1s", "--timer-property=RemainAfterElapse=no"]
        arguments += ["/usr/bin/python3", "-I", HELPER, "--apply-update"] if operation == "update" else ["/usr/bin/systemctl", "restart", "pi-controller.service"] if operation == "restart" else ["/usr/bin/systemctl", "reboot"]
        manager.run(arguments)
        result = dict(ok=True, scheduled=operation)
    else:
        arguments = ["/usr/bin/systemd-run", "--quiet", "--wait", "--pipe", "--collect", "--unit=pi-controller-admin-" + uuid.uuid4().hex,
                     "/usr/bin/python3", "-I", HELPER, "--execute"]
        output = subprocess.run(arguments, input=json.dumps(request), check=True, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
        result = json.loads(output.stdout)
    print(json.dumps(result))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        print(json.dumps(dict(ok=False, error="OS operation failed; no credentials are logged")))
        sys.exit(1)
