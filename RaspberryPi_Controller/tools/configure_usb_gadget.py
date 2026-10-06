"""Optional Pi installer step. Preserve boot arguments and back up before edits."""
import json
import os
import subprocess
from pathlib import Path


def boot_text(config_text, cmdline_text):
    if any(line.strip().startswith("dtoverlay=") and "dwc2" in line and "dr_mode=host" in line for line in config_text.splitlines()):
        raise ValueError("existing dwc2 host overlay conflicts with the serial gadget")
    arguments = cmdline_text.split()
    module_args = [argument for argument in arguments if argument.startswith("modules-load=")]
    modules = [module for argument in module_args for module in argument.partition("=")[2].split(",")]
    if any(module in {"g_ether", "g_multi", "g_mass_storage", "g_hid"} for module in modules) or "rpi-usb-gadget" in config_text:
        raise ValueError("another gadget is configured; configure a composite gadget manually")
    section, configured = "all", False
    for line in config_text.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"): section = line[1:-1]
        if line.startswith("dtoverlay=dwc2"):
            if section != "all":
                raise ValueError("conditional dwc2 overlay requires manual gadget configuration")
            configured = True
    if not configured:
        config_text = config_text.rstrip() + "\n\n[all]\n# Pi controller serial gadget\ndtoverlay=dwc2,dr_mode=peripheral\n"
    for module in ("dwc2", "g_serial"):
        if module not in modules: modules.append(module)
    arguments = [argument for argument in arguments if not argument.startswith("modules-load=")]
    arguments.append("modules-load=" + ",".join(modules))
    return config_text, " ".join(arguments) + "\n"


def main():
    if not hasattr(os, "geteuid") or os.geteuid() != 0:
        raise ValueError("run this optional step as root on the Pi")
    gadget = subprocess.run(["systemctl", "is-enabled", "rpi-usb-gadget.service"], capture_output=True, text=True)
    if gadget.stdout.strip() in {"enabled", "enabled-runtime", "static", "indirect"}:
        raise ValueError("rpi-usb-gadget is configured; configure composite serial manually")
    directory = Path("/boot/firmware") if Path("/boot/firmware/config.txt").exists() else Path("/boot")
    config_path, cmdline_path = directory / "config.txt", directory / "cmdline.txt"
    current_config, current_cmdline = config_path.read_text(), cmdline_path.read_text()
    new_config, new_cmdline = boot_text(current_config, current_cmdline)
    for path, original, updated in ((config_path, current_config, new_config), (cmdline_path, current_cmdline, new_cmdline)):
        backup = path.with_suffix(path.suffix + ".pi-controller.bak")
        if not backup.exists(): backup.write_text(original)
        path.write_text(updated)
    controller_path = Path("/etc/pi-controller/config.json")
    controller = json.loads(controller_path.read_text())
    controller["usb_device"] = "/dev/ttyGS0"
    controller_path.write_text(json.dumps(controller, indent=2) + "\n")
    subprocess.run(["systemctl", "disable", "--now", "serial-getty@ttyGS0.service"], check=False)
    print("Serial gadget configured. Reboot the Pi to enumerate USB serial.")


if __name__ == "__main__":
    main()
