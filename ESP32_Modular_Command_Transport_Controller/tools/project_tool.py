#!/usr/bin/env python3
"""Build web artifacts and validate the simplified ESP32 project layout."""

from __future__ import annotations

import argparse
import gzip
import re
import shutil
import tempfile
import subprocess
import sys
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
WEB = ROOT / "web"
OUTPUT = SRC / "web" / "WebAssets.h"
STANDALONE = WEB / "standalone_console.html"
PORTAL_GZIP_BENCHMARK_BYTES = 8192

# The firmware embeds only a generated self-contained gzip portal object so
# loading the control page requires one HTTP response. The individual web source
# files remain editable but are not separately compiled or served.
# standalone_console.html is deliberately absent from this list.
ASSETS: list[tuple[str, str, str, str]] = []



def minify_css(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"\s+", " ", text)
    text = re.sub(r"\s*([{}:;,>])\s*", r"\1", text)
    return text.strip()


def minify_html(text: str) -> str:
    text = re.sub(r"<!--(?!\[if).*?-->", "", text, flags=re.DOTALL)
    text = re.sub(r">\s+<", "><", text)
    return text.strip()


def minify_javascript(text: str) -> str:
    npx = shutil.which("npx") or shutil.which("npx.cmd")
    if npx is None:
        fail("Node npx/terser is required only when regenerating embedded web assets")
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "app.js"
        output = Path(directory) / "app.min.js"
        source.write_text(text, encoding="utf-8")
        subprocess.run(
            [npx, "terser", str(source), "--compress", "passes=3", "--mangle", "--comments", "false", "-o", str(output)],
            cwd=ROOT,
            check=True,
        )
        return output.read_text(encoding="utf-8").strip()


def build_portal_text() -> str:
    html = minify_html((WEB / "index.html").read_text(encoding="utf-8").replace("\r\n", "\n"))
    css = minify_css((WEB / "app.css").read_text(encoding="utf-8").replace("\r\n", "\n"))
    javascript = minify_javascript((WEB / "app.js").read_text(encoding="utf-8").replace("\r\n", "\n"))
    stylesheet_marker = '<link rel="stylesheet" href="/app.css">'
    script_marker = '<script src="/app.js"></script>'
    if stylesheet_marker not in html:
        fail("index.html stylesheet marker changed; update build_portal_text()")
    if script_marker not in html:
        fail("index.html script marker changed; update build_portal_text()")
    return html.replace(stylesheet_marker, f"<style>{css}</style>").replace(script_marker, f"<script>{javascript}</script>")


class IdCollector(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.ids: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        del tag
        for name, value in attrs:
            if name == "id" and value:
                self.ids.append(value)


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    raise SystemExit(1)


def run(command: list[str]) -> None:
    print("+", " ".join(command))
    subprocess.run(command, cwd=ROOT, check=True)


def build_web_assets() -> None:
    portal = build_portal_text()

    lines = [
        "#ifndef ESP32_MODULAR_CONTROLLER_WEBASSETS_H",
        "#define ESP32_MODULAR_CONTROLLER_WEBASSETS_H",
        "",
        "// GENERATED FILE. EDIT web/index.html, web/app.css, or web/app.js,",
        "// THEN RUN: python tools/project_tool.py embed",
        "",
        "#include <Arduino.h>",
        "",
        "namespace WebAssets {",
        "",
    ]

    portal_bytes = portal.encode("utf-8")
    portal_gzip = gzip.compress(portal_bytes, compresslevel=9, mtime=0)
    gzip_rows = []
    for offset in range(0, len(portal_gzip), 16):
        chunk = portal_gzip[offset:offset + 16]
        gzip_rows.append("  " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")

    lines.extend([
        'constexpr const char PORTAL_HTML_CONTENT_TYPE[] = "text/html; charset=utf-8";',
        'constexpr const char PORTAL_HTML_CONTENT_ENCODING[] = "gzip";',
        'static const uint8_t PORTAL_HTML_GZIP[] PROGMEM = {',
        *gzip_rows,
        '};',
        'constexpr size_t PORTAL_HTML_GZIP_LENGTH = sizeof(PORTAL_HTML_GZIP);',
        f'constexpr size_t PORTAL_HTML_UNCOMPRESSED_LENGTH = {len(portal_bytes)};',
        "",
    ])

    for symbol, filename, content_type, delimiter in ASSETS:
        path = WEB / filename
        if not path.exists():
            fail(f"missing web source: {path.relative_to(ROOT)}")
        asset_text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
        terminator = f'){delimiter}"'
        if terminator in asset_text:
            fail(f"raw string terminator collision in {filename}")
        lines.extend([
            f'constexpr const char {symbol}_CONTENT_TYPE[] = "{content_type}";',
            f'static const char {symbol}[] PROGMEM = R"{delimiter}({asset_text}){delimiter}";',
            f'constexpr size_t {symbol}_LENGTH = sizeof({symbol}) - 1;',
            "",
        ])

    lines.extend(["}  // namespace WebAssets", "", "#endif  // ESP32_MODULAR_CONTROLLER_WEBASSETS_H", ""])
    OUTPUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote {OUTPUT.relative_to(ROOT)}")


def build_standalone_html() -> None:
    """Create one portable HTML file without adding it to firmware assets."""
    html = (WEB / "index.html").read_text(encoding="utf-8").replace("\r\n", "\n")
    css = (WEB / "app.css").read_text(encoding="utf-8").replace("\r\n", "\n")
    javascript = (WEB / "app.js").read_text(encoding="utf-8").replace("\r\n", "\n")

    stylesheet_marker = '<link rel="stylesheet" href="/app.css">'
    script_marker = '<script src="/app.js"></script>'
    if stylesheet_marker not in html:
        fail("index.html stylesheet marker changed; update build_standalone_html()")
    if script_marker not in html:
        fail("index.html script marker changed; update build_standalone_html()")
    if "<body>" not in html:
        fail("index.html body marker changed; update build_standalone_html()")

    standalone_css = """
.standaloneToolbar {
  max-width: 1260px;
  margin: 16px auto 0;
  padding: 12px 20px;
  display: grid;
  grid-template-columns: minmax(220px, 1fr) auto minmax(240px, 1fr);
  gap: 10px;
  align-items: end;
}
.standaloneToolbar label { display: grid; gap: 6px; color: var(--muted); font-size: 0.82rem; }
.standaloneToolbar .standaloneHint { color: var(--muted); line-height: 1.35; font-size: 0.82rem; }
@media (max-width: 760px) {
  .standaloneToolbar { grid-template-columns: 1fr; }
}
"""

    toolbar = """
  <section class="standaloneToolbar panel">
    <label>Device URL
      <input id="standaloneDeviceUrl" type="url" value="http://192.168.4.1" spellcheck="false">
    </label>
    <button id="standaloneUseDevice" class="success">Use device URL</button>
    <p id="standaloneDeviceStatus" class="standaloneHint">Standalone console. This file is not embedded in or served by the ESP32.</p>
  </section>
"""

    bootstrap = r'''
<script>
(() => {
  "use strict";
  const storageKey = "esp32-controller-device-url";
  const normalize = (value) => {
    const trimmed = String(value || "").trim().replace(/\/+$/, "");
    return trimmed || "http://192.168.4.1";
  };

  window.ESP32_API_BASE = normalize(localStorage.getItem(storageKey));
  const input = document.getElementById("standaloneDeviceUrl");
  const button = document.getElementById("standaloneUseDevice");
  const status = document.getElementById("standaloneDeviceStatus");
  input.value = window.ESP32_API_BASE;
  status.textContent = `Standalone console targeting ${window.ESP32_API_BASE}`;

  button.addEventListener("click", () => {
    const next = normalize(input.value);
    localStorage.setItem(storageKey, next);
    window.location.reload();
  });
})();
</script>
'''

    css = (css + "\n" + standalone_css).replace("</style", "<\\/style")
    javascript = javascript.replace("</script", "<\\/script")
    output = html.replace(stylesheet_marker, f"<style>\n{css}\n</style>")
    output = output.replace("<body>", "<body>" + toolbar, 1)
    output = output.replace(script_marker, bootstrap + f"\n<script>\n{javascript}\n</script>")
    output = output.replace(
        "<title>ESP32 Controller Console</title>",
        "<title>ESP32 Modular Controller Standalone Console</title>",
    )
    output = "<!-- GENERATED STANDALONE FILE. Not embedded or served by firmware. -->\n" + output
    STANDALONE.write_text(output, encoding="utf-8")
    print(f"Wrote {STANDALONE.relative_to(ROOT)}")


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def require_snippets(relative: str, snippets: list[str], missing: list[str]) -> None:
    path = ROOT / relative
    if not path.exists():
        missing.append(f"missing file: {relative}")
        return
    text = path.read_text(encoding="utf-8")
    for snippet in snippets:
        if snippet not in text:
            missing.append(f"{relative}: {snippet}")


def check_layout() -> None:
    sketch = ROOT / "ESP32_Modular_Command_Transport_Controller.ino"
    required_root_files = {sketch.name, "build_opt.h", "README.md", "COMMANDS.md"}
    allowed_root_files = set(required_root_files)
    root_files = {path.name for path in ROOT.iterdir() if path.is_file()}
    missing_root_files = sorted(required_root_files - root_files)
    unexpected_root_files = sorted(root_files - allowed_root_files)
    if missing_root_files or unexpected_root_files:
        fail(
            "project root file mismatch; missing=" + ",".join(missing_root_files) +
            " unexpected=" + ",".join(unexpected_root_files)
        )

    for folder in [
        "src/addons", "src/config", "src/core", "src/hardware",
        "src/radio", "src/transports", "src/util", "src/web", "web", "tools", "docs",
    ]:
        if not (ROOT / folder).is_dir():
            fail(f"missing project folder: {folder}")

    inc_files = sorted(ROOT.rglob("*.inc"))
    if inc_files:
        fail("legacy .inc implementation files remain: " + ", ".join(str(x.relative_to(ROOT)) for x in inc_files))
    if (ROOT / "ControllerConfig.h").exists():
        fail("ControllerConfig.h remains; use src/config/AppConfig.h")
    if (ROOT / "partitions.csv").exists():
        fail("custom partitions.csv is not allowed; use the Arduino partition selection")
    build_artifacts = [
        path for path in ROOT.rglob("*")
        if path.name in {".pio", "__pycache__"} or path.suffix == ".pyc"
    ]
    if build_artifacts:
        fail("generated build/cache artifacts remain: " + ", ".join(
            str(path.relative_to(ROOT)) for path in build_artifacts
        ))

    ino = sketch.read_text(encoding="utf-8")
    if '#include "src/config/AppConfig.h"' not in ino:
        fail("the sketch must include src/config/AppConfig.h directly")
    if re.search(r'#include\s+"[^"]+\.(?:cpp|inc)"', ino):
        fail("the sketch directly includes an implementation file")
    if "ESP32_MODULAR_BUILD_PROFILE" in ino:
        fail("the obsolete sketch profile guard remains")

    app_config = read("src/config/AppConfig.h")
    for marker in [
        "#define APP_ENABLE_WIFI",
        "#define APP_ENABLE_BLE",
        "#define APP_ENABLE_HTTP_OTA",
    ]:
        if marker not in app_config:
            fail(f"AppConfig.h is missing build switch: {marker}")
    if re.search(r'#include\s+"[^"]+\.ino"', app_config):
        fail("AppConfig.h must never include the Arduino sketch")

    cpp_files = sorted(SRC.rglob("*.cpp"))
    if not cpp_files:
        fail("no normal C++ implementation modules were found")
    for source in cpp_files:
        lines = [line.strip() for line in source.read_text(encoding="utf-8").splitlines() if line.strip()]
        expected = source.with_suffix(".h")
        if expected.exists() and (not lines or lines[0] != f'#include "{expected.name}"'):
            fail(f"{source.relative_to(ROOT)} must include its own header first")

    print(f"OK: AppConfig-owned build switches plus {len(cpp_files)} normal .cpp modules")


def check_comment_terminators() -> None:
    suspicious: list[str] = []
    for path in [ROOT / "ESP32_Modular_Command_Transport_Controller.ino", *sorted(SRC.rglob("*.h")), *sorted(SRC.rglob("*.cpp"))]:
        text = path.read_text(encoding="utf-8")
        for match in re.finditer(r"\*/\S", text):
            line = text.count("\n", 0, match.start()) + 1
            suspicious.append(f"{path.relative_to(ROOT)}:{line}")
    if suspicious:
        fail("suspicious block-comment terminator followed by code/text: " + ", ".join(suspicious))
    print("OK: no accidental block-comment terminators")

def check_web_bindings() -> None:
    html = read("web/index.html")
    javascript = read("web/app.js")
    parser = IdCollector()
    parser.feed(html)
    duplicates = sorted({item for item in parser.ids if parser.ids.count(item) > 1})
    if duplicates:
        fail("duplicate HTML IDs: " + ", ".join(duplicates))
    binding_match = re.search(r"for\s*\(const id of\s*\[(.*?)\]\s*\)", javascript, flags=re.DOTALL)
    if not binding_match:
        fail("could not find the app.js element binding list")
    bound_ids = set(re.findall(r'"([A-Za-z_][A-Za-z0-9_-]*)"', binding_match.group(1)))
    html_ids = set(parser.ids)
    missing_html = sorted(bound_ids - html_ids)
    if missing_html:
        fail("JavaScript binds missing HTML IDs: " + ", ".join(missing_html))
    referenced = set(re.findall(r"elements\.([A-Za-z_][A-Za-z0-9_]*)", javascript))
    missing_bindings = sorted(referenced - bound_ids)
    if missing_bindings:
        fail("app.js uses unbound elements: " + ", ".join(missing_bindings))
    print(f"OK: {len(html_ids)} unique HTML IDs and {len(bound_ids)} JavaScript bindings")


def check_embedded_portal() -> None:
    expected = build_portal_text().encode("utf-8")
    generated = OUTPUT.read_text(encoding="utf-8")
    array_match = re.search(r"PORTAL_HTML_GZIP\[\]\s+PROGMEM\s*=\s*\{(.*?)\};", generated, flags=re.DOTALL)
    if not array_match:
        fail("generated portal gzip array is missing")
    values = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", array_match.group(1)))
    try:
        decoded = gzip.decompress(values)
    except OSError as error:
        fail(f"generated portal gzip is invalid: {error}")
    if decoded != expected:
        fail("generated portal gzip does not match the minified web sources")
    if len(values) >= len(expected):
        fail("embedded portal compression did not reduce flash use")
    saved = len(expected) - len(values)
    if len(values) > PORTAL_GZIP_BENCHMARK_BYTES:
        print(
            f"WARNING: portal gzip {len(values)} bytes exceeds the "
            f"{PORTAL_GZIP_BENCHMARK_BYTES}-byte advisory benchmark by "
            f"{len(values) - PORTAL_GZIP_BENCHMARK_BYTES} bytes",
            file=sys.stderr,
        )
    else:
        print(f"OK: portal gzip {len(values)} bytes, saving {saved} bytes")


def check_standalone_html() -> None:
    if not STANDALONE.exists():
        fail("standalone console is missing")
    text = STANDALONE.read_text(encoding="utf-8")
    for marker in ["GENERATED STANDALONE FILE", "standaloneDeviceUrl", "window.ESP32_API_BASE"]:
        if marker not in text:
            fail(f"standalone console is missing marker: {marker}")
    if 'href="/app.css"' in text or 'src="/app.js"' in text:
        fail("standalone console still has external web assets")
    generated = OUTPUT.read_text(encoding="utf-8")
    if "standaloneDeviceUrl" in generated:
        fail("standalone-only controls leaked into embedded WebAssets.h")
    print("OK: standalone console is single-file and excluded from firmware assets")


def check_markers_and_architecture() -> None:
    missing: list[str] = []
    required: dict[str, list[str]] = {
        "ESP32_Modular_Command_Transport_Controller.ino": [
            '#include "src/config/AppConfig.h"', "TransportBridge transportBridge",
            "DeviceAddon deviceAddon", "deviceAddon.begin();",
            "deviceAddon.queueStartupTests(dispatcher);", "CommandRouter::webStateThunk",
        ],
        "src/config/AppConfig.h": [
            'FIRMWARE_VERSION = "2026-07-10-v5.16.0-base-controller"',
            "#define APP_ENABLE_WIFI", "#define APP_ENABLE_BLE",
            "#define APP_ENABLE_STATUS_INDICATORS",
            "#define APP_ENABLE_STATUS_LED_BOOT_TEST", "HTTP_MAX_ACTIVE_REQUESTS = 2",
            "HTTP_REMAINING_FREE_HEAP_BYTES = 9000",
            "HTTP_MIN_LARGEST_BLOCK_BYTES = 6000",
            "HTTP_CLIENT_RX_TIMEOUT_SECONDS", "HTTP_CLIENT_ACK_TIMEOUT_MS",
            "BLE_TX_MIN_FREE_HEAP_BYTES", "BLE_NOTIFY_SUBSCRIPTION_SETTLE_MS",
            'DEFAULT_WIFI_AP_SSID = "ESP32-Controller"',
            'BLE_NAME = "ESP32-Controller-BLE"',
        ],
        "src/addons/DeviceAddon.h": [
            "class DeviceAddon", "virtual bool handleCommand", "virtual void service",
            'virtual const char *name() const { return "none"; }',
        ],
        "src/core/AppTypes.h": [
            "TRANSPORT_MASK_DEFAULT", "defaultTransportMaskForEvent", "SendWiFi/SendBLE",
        ],
        "src/core/EventBus.cpp": [
            "targetMask == TRANSPORT_MASK_DEFAULT", "defaultTransportMaskForEvent(source)",
        ],
        "src/transports/TransportBridge.cpp": [
            '"SendBLE"', '"SendWiFi"', '"SendUSB"',
            'parsePayload(command, "Send", payload)', "isBleOutputReady()",
            "hasRecentClient()",
        ],
        "src/transports/TransportHub.h": [
            "BLEService *bleService_;", "SemaphoreHandle_t bleNotifyMutex_;",
            "bleNotificationsEnabledAtMs_",
        ],
        "src/transports/TransportHub.cpp": [
            "BleTxDescriptorCallbacks", "BLE_NOTIFY_SUBSCRIPTION_SETTLE_MS",
            "xSemaphoreCreateMutex()", "xSemaphoreTake(bleNotifyMutex_",
            "BLE output paused for heap recovery", "bleServer_->getConnectedCount()",
            "record.targetMask & transportMask",
        ],
        "src/web/WebPortal.cpp": [
            "admitRequest", "request->onDisconnect", "releaseRequest(request)",
            "beginCommandBody", "appendCommandBody", "takeCommandBody",
            "AsyncBasicResponse owns its String content", "setRxTimeout",
            "setAckTimeout", "request->abort()", "HTTP_MAX_ACTIVE_REQUESTS",
            "HTTP_MAX_ACTIVE_RESERVED_BYTES", "inspectOrReserveRequest",
            "WebAssets::PORTAL_HTML_GZIP", "Update.begin(otaExpectedBytes_, U_FLASH)",
            "Update.end(false)", "X-Firmware-Size", "OTA_IMAGE_MAGIC",
            "otaRequest_ == request",
        ],
        "src/radio/RadioManager.cpp": [
            "BootRadioMode::WIFI_BLE_P", "ModeWiFiBLEP",
            "isPersistentWifiBleProfile", "webPortal_.discardClientMetadata()",
        ],
        "web/index.html": [
            "Modular Controller", "Firmware OTA", "SendBLE:hello",
        ],
        "web/app.js": [
            "httpTail:Promise.resolve()", "function postHttp", "state.httpTail.then",
            '"Content-Type":"application/octet-stream"', "body:clean",
            'if(event.q)', '"ModeWiFiBLEP"', "new FormData()",
            'form.append("firmware",file,file.name)', '"X-Firmware-Size"',
            "prepareHttpForOta", "magic!==0xe9",
        ],
        "README.md": [
            "# ESP32 Modular Command + Transport Controller",
            "## Source layout", "## Troubleshooting",
        ],
        "COMMANDS.md": [
            "# ESP32 controller command reference", "## Commands",
        ],
    }
    for relative, snippets in required.items():
        require_snippets(relative, snippets, missing)

    source_text = "\n".join(path.read_text(encoding="utf-8") for path in SRC.rglob("*.*") if path.suffix in {".h", ".cpp"})
    for forbidden in [
        "bleTx_->getService(", "BridgePair:", "BridgeRoute:", "BridgeAuto:",
        "BridgeSave", "BridgeLoad", "BridgeClear",
    ]:
        if forbidden in source_text:
            missing.append(f"forbidden legacy/private token remains: {forbidden}")

    if "TRANSPORT_MASK_ALL" in read("src/core/EventBus.h").split("targetMask =", 1)[-1].split(",", 1)[0]:
        missing.append("normal EventBus publications still default to every transport")

    if missing:
        fail("base-controller architecture validation failed: " + "; ".join(missing))
    print("OK: transport-only base, neutral addon interface, BLE lifecycle guard, and bounded HTTP admission are present")


def check_header_guards() -> None:
    guards: set[str] = set()
    for header in sorted(SRC.rglob("*.h")):
        text = header.read_text(encoding="utf-8")
        match = re.match(r"#ifndef ([A-Z0-9_]+)\n#define \1\n", text)
        if not match:
            fail(f"{header.relative_to(ROOT)} is missing an explicit include guard")
        guard = match.group(1)
        if guard in guards:
            fail(f"duplicate include guard: {guard}")
        guards.add(guard)
        if f"#endif  // {guard}" not in text and not text.rstrip().endswith("#endif"):
            fail(f"{header.relative_to(ROOT)} is missing its guard terminator")
    print(f"OK: {len(guards)} unique include guards")


def check_pin_map() -> None:
    config = read("src/config/AppConfig.h")
    expected = {
        "PIN_STATUS_CONNECTION": 23,
        "PIN_STATUS_ACTIVITY": 5,
    }
    found: dict[str, int] = {}
    for name, expected_pin in expected.items():
        match = re.search(rf"constexpr\s+int\s+{name}\s*=\s*(\d+)\s*;", config)
        if not match:
            fail(f"missing pin assignment: {name}")
        actual = int(match.group(1))
        if actual != expected_pin:
            fail(f"unexpected {name}: GPIO{actual}, expected GPIO{expected_pin}")
        found[name] = actual
    if len(set(found.values())) != len(found):
        fail("indicator pin map contains duplicate GPIO assignments")
    compiled_sources = "\n".join(
        path.read_text(encoding="utf-8")
        for path in SRC.rglob("*")
        if path.suffix in {".h", ".cpp"}
    )
    for forbidden in ["StepperController", "DacController", "StepperDacAddon", "APP_STEPPER_DAC_ADDON_ENABLED"]:
        if forbidden in compiled_sources:
            fail(f"custom stepper/DAC implementation remains in compiled source: {forbidden}")
    print("OK: only the GPIO23/GPIO5 status indicators are owned by the base hardware layer")


def check_radio_boot_transaction() -> None:
    source = read("src/radio/RadioManager.cpp")
    config = read("src/config/AppConfig.h")
    required = [
        'KEY_TXN_PENDING = "txnpending"', 'KEY_TXN_STARTED = "txnstarted"',
        'KEY_TXN_PROFILE = "txnprofile"', 'KEY_LAST_GOOD = "lastgood"',
        "preferences.putBool(KEY_TXN_STARTED, true)", "[BOOT][RADIO][OK]",
        "[BOOT][RADIO][ROLLBACK]", "RADIO_TRIAL_MIN_FREE_HEAP_BYTES",
        "RADIO_TRIAL_MIN_LARGEST_BLOCK_BYTES",
    ]
    missing = [item for item in required if item not in source]
    if missing:
        fail("radio transaction invariants missing: " + "; ".join(missing))
    if source.find("prepareBootTransaction();") > source.find("if (!initializeBootBluetooth())"):
        fail("trial crash marker is not prepared before Bluetooth initialization")
    keys = re.findall(r'KEY_[A-Z_]+\s*=\s*"([^"]+)"', source)
    oversized = sorted(key for key in keys if len(key) > 15)
    if oversized:
        fail("Preferences keys exceed 15 characters: " + ", ".join(oversized))
    for name in [
        "RADIO_TRIAL_TIMEOUT_MS", "RADIO_TRIAL_STABILIZE_MS",
        "RADIO_TRIAL_MIN_FREE_HEAP_BYTES", "RADIO_TRIAL_MIN_LARGEST_BLOCK_BYTES",
    ]:
        if name not in config:
            fail(f"missing radio transaction threshold: {name}")
    print(f"OK: transactional radio rollback with {len(keys)} bounded NVS keys")


def check_placeholders() -> None:
    ignored = {OUTPUT, STANDALONE}
    forbidden = re.compile(r"\b(TODO|FIXME|XXX)\b", flags=re.IGNORECASE)
    unsafe_c_strings = re.compile(r"\b(strcpy|strcat|sprintf)\s*\(")
    unfinished: list[str] = []
    unsafe: list[str] = []
    for path in ROOT.rglob("*"):
        if not path.is_file() or path in ignored or ".pio" in path.parts:
            continue
        if path.suffix.lower() not in {".h", ".cpp", ".ino", ".js", ".css", ".md"}:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if forbidden.search(text):
            unfinished.append(str(path.relative_to(ROOT)))
        if path.suffix.lower() in {".h", ".cpp", ".ino"} and unsafe_c_strings.search(text):
            unsafe.append(str(path.relative_to(ROOT)))
    if unfinished:
        fail("unfinished markers found in: " + ", ".join(sorted(unfinished)))
    if unsafe:
        fail("unsafe C string calls found in: " + ", ".join(sorted(unsafe)))
    print("OK: no unfinished markers or unsafe unbounded C string calls")


def validate() -> None:
    check_layout()
    check_comment_terminators()
    node = shutil.which("node")
    if node:
        run([node, "--check", str(WEB / "app.js")])
    else:
        print("WARNING: node was not found, JavaScript syntax check skipped")
    check_web_bindings()
    check_embedded_portal()
    check_standalone_html()
    check_markers_and_architecture()
    check_header_guards()
    check_pin_map()
    check_radio_boot_transaction()
    check_placeholders()
    print("SOURCE VALIDATION PASSED")

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "action",
        nargs="?",
        choices=("embed", "standalone", "validate", "all"),
        default="all",
        help="embed hosted assets, build standalone HTML, validate, or run all",
    )
    args = parser.parse_args()

    if args.action in {"standalone", "all"}:
        build_standalone_html()
    if args.action in {"embed", "all"}:
        build_web_assets()
    if args.action in {"validate", "all"}:
        validate()


if __name__ == "__main__":
    main()
