import json
import copy
import re
from pathlib import Path


DEFAULTS = dict(
    host="127.0.0.1", port=8080, simulate=True, data_dir="data", ble=False,
    ble_name="PiBridge", ble_adapter="hci0", usb_device="", uart_device="",
    baud=115200, dispenser_pin=26,
    allowed_output_pins=[4, 17, 18, 22, 23, 24, 25, 26, 27], active_high=True,
    default_pulse_ms=250, max_pulse_ms=0, arm_timeout_ms=0,
    interlock_pin=None, interlock_open_high=True,
    hardware_profile="dispenser", stepper_pins=[18, 19, 20, 21],
    stepper_steps_per_rev=2048, stepper_max_rpm=12.0, stepper_min_interval_us=2000,
    dac_i2c_bus=1, dac_i2c_address=96, dac_reference_mv=3300,
    indicators=False, connection_led_pin=23, activity_led_pin=24,
    spp=False, spp_channel=1, enable_os_control=False,
    wifi_interface="wlan0", wifi_ap_interface="wlan0", update_token="",
    geo_udp_host="0.0.0.0", geo_udp_port=14550, geo_system_id=1, geo_component_id=1,
)

UINT32_MAX = 4294967295

OUTPUT_PINS = [4, 5, 6, 12, 13, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27]


def reserved_pins(config):
    pins = set()
    if config["indicators"]:
        pins.update((config["connection_led_pin"], config["activity_led_pin"]))
    if config["hardware_profile"] == "stepper_dac":
        pins.update(config["stepper_pins"])
    return pins


def validate(config):
    if set(config) != set(DEFAULTS):
        raise ValueError("unknown or missing configuration keys")
    for key in ("simulate", "ble", "active_high", "interlock_open_high", "indicators", "spp", "enable_os_control"):
        if type(config[key]) is not bool:
            raise ValueError(f"{key} must be true or false")
    for key in ("host", "data_dir", "ble_name", "ble_adapter", "usb_device", "uart_device", "update_token", "hardware_profile", "wifi_interface", "wifi_ap_interface", "geo_udp_host"):
        if not isinstance(config[key], str):
            raise ValueError(f"{key} must be a string")
    for key, upper in (("port", 65535), ("baud", 4000000), ("default_pulse_ms", UINT32_MAX),
                       ("geo_udp_port", 65535), ("geo_system_id", 255), ("geo_component_id", 255)):
        if type(config[key]) is not int or not 1 <= config[key] <= upper:
            raise ValueError(f"invalid {key}")
    for key in ("max_pulse_ms", "arm_timeout_ms"):
        if type(config[key]) is not int or not 0 <= config[key] <= UINT32_MAX:
            raise ValueError(f"invalid {key}")
    if config["max_pulse_ms"] and config["default_pulse_ms"] > config["max_pulse_ms"]:
        raise ValueError("default pulse exceeds maximum pulse")
    if config["arm_timeout_ms"] and config["arm_timeout_ms"] < (config["max_pulse_ms"] or config["default_pulse_ms"]):
        raise ValueError("arm timeout must accommodate a maximum/default pulse")
    pins = config["allowed_output_pins"]
    if not isinstance(pins, list) or not pins or any(type(p) is not int or p not in OUTPUT_PINS for p in pins):
        raise ValueError("allowed_output_pins contains reserved or invalid BCM pins")
    interlock = config["interlock_pin"]
    if interlock is not None and (type(interlock) is not int or interlock not in OUTPUT_PINS):
        raise ValueError("invalid interlock BCM pin")
    if type(config["dispenser_pin"]) is not int or config["dispenser_pin"] not in pins or config["dispenser_pin"] == interlock:
        raise ValueError("dispenser pin is unavailable or conflicts with interlock")
    if config["usb_device"] and config["usb_device"] == config["uart_device"]:
        raise ValueError("USB and UART must use distinct serial devices")
    if not 1 <= len(config["ble_name"].encode()) <= 8:
        raise ValueError("BLE name must fit 1-8 bytes beside the UART service UUID")
    if not config["ble_adapter"].startswith("hci") or not config["ble_adapter"][3:].isdigit():
        raise ValueError("ble_adapter must be hci followed by a number")
    if config["hardware_profile"] not in {"dispenser", "stepper_dac", "none"}:
        raise ValueError("hardware_profile must be dispenser, stepper_dac, or none")
    owned = []
    if config["indicators"]:
        owned.extend((config["connection_led_pin"], config["activity_led_pin"]))
    if config["hardware_profile"] == "stepper_dac":
        if not isinstance(config["stepper_pins"], list) or len(config["stepper_pins"]) != 4:
            raise ValueError("stepper requires four BCM pins")
        owned.extend(config["stepper_pins"])
    if config["hardware_profile"] != "none":
        owned.append(config["dispenser_pin"])
    if interlock is not None:
        owned.append(interlock)
    if any(type(pin) is not int or pin not in OUTPUT_PINS for pin in owned) or len(owned) != len(set(owned)):
        raise ValueError("hardware/indicator/interlock BCM pin ownership conflicts")
    for key, low, high in (("stepper_steps_per_rev", 1, 1000000), ("stepper_min_interval_us", 1000, 1000000),
                           ("dac_i2c_bus", 0, 10), ("dac_i2c_address", 96, 103),
                           ("dac_reference_mv", 2500, 3600), ("spp_channel", 1, 30)):
        if type(config[key]) is not int or not low <= config[key] <= high:
            raise ValueError(f"invalid {key}")
    if type(config["stepper_max_rpm"]) not in (int, float) or not 0.1 <= config["stepper_max_rpm"] <= 60:
        raise ValueError("invalid stepper_max_rpm")
    for key in ("wifi_interface", "wifi_ap_interface"):
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_.-]{0,14}", config[key]):
            raise ValueError("invalid network interface name")
    if config["update_token"] and len(config["update_token"]) < 24:
        raise ValueError("update_token must contain at least 24 characters")
    return config


def load_config(path=None):
    config = copy.deepcopy(DEFAULTS)
    if path:
        location = Path(path).resolve()
        override = json.loads(location.read_text(encoding="utf-8"))
        if not isinstance(override, dict) or set(override) - set(DEFAULTS):
            raise ValueError("unknown configuration keys")
        config.update(override)
        config["data_dir"] = str((location.parent / config["data_dir"]).resolve())
    return validate(config)
