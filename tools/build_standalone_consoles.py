#!/usr/bin/env python3
"""Build portable, offline controller consoles. Uses only the Python standard library."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VARIANTS = (
    ("ESP32S3_Dispenser_Controller", "s3-dispenser.html", "S3 dispenser + motors"),
    ("ESP32S3_Modular_Command_Transport_Controller", "s3-transport.html", "S3 transport (no motor outputs)"),
    ("ESP32_Dispenser_Controller", "lolin32-dispenser.html", "LOLIN32 dispenser + motors"),
    ("ESP32_Modular_Command_Transport_Controller", "lolin32-transport.html", "LOLIN32 transport (no motor outputs)"),
)


def read(path):
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def render(web):
    html = read(web / "index.html")
    css = read(web / "app.css").rstrip() + "\n" + read(web / "standalone.css").rstrip()
    js = read(web / "app.js").rstrip().replace("</script", "<\\/script")
    bootstrap = read(web / "standalone_bootstrap.js").rstrip().replace("</script", "<\\/script")
    toolbar = read(web / "standalone_toolbar.html").rstrip()
    style_tag = '<link rel="stylesheet" href="/app.css">'
    script_tag = '<script src="/app.js"></script>'
    if style_tag not in html or script_tag not in html or "<body>" not in html:
        raise ValueError(f"Web asset markers changed: {web}")
    html = html.replace(style_tag, "<style>\n" + css.replace("</style", "<\\/style") + "\n</style>", 1)
    html = html.replace("<body>", "<body>\n" + toolbar, 1)
    html = html.replace(script_tag, "<script>\n" + bootstrap + "\n</script>\n<script>\n" + js + "\n</script>", 1)
    return "<!-- GENERATED STANDALONE FILE. Not embedded or served by firmware. -->\n" + html


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Report stale files without changing them")
    args = parser.parse_args()
    output = ROOT / "Standalone_Consoles"
    generated = {}
    links = []
    for folder, filename, title in VARIANTS:
        web = ROOT / folder / "web"
        text = render(web)
        generated[web / "standalone_console.html"] = text
        generated[output / filename] = text
        links.append(f'<a href="{filename}">{title}<span>Open offline console →</span></a>')
    generated[output / "index.html"] = '''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Controller consoles</title><style>
:root{color-scheme:dark;font:16px system-ui;background:#080f18;color:#e1edf5}
body{max-width:760px;margin:8vh auto;padding:20px}h1{font-size:1.8rem;margin-bottom:8px}
p{color:#9aafc2;line-height:1.55}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;margin:24px 0}
a{display:block;border:1px solid #304858;border-radius:12px;padding:20px;text-decoration:none;color:#62e9cd;background:#101d2a}
a:hover,a:focus{border-color:#62e9cd}a span{display:block;font-size:.8rem;color:#9aafc2;margin-top:8px}
@media(max-width:540px){.grid{grid-template-columns:1fr}}
</style></head><body><h1>Controller consoles</h1>
<p>Choose the firmware on your controller. Join its Wi-Fi access point and the console connects automatically.
Away from Wi-Fi, choose <strong>Connect BLE</strong> inside the console. For DRV8871 or TMC2209 controls, choose a dispenser console; motor support must also be enabled in firmware.</p>
<div class="grid">''' + "\n".join(links) + '''</div>
<p>These files work offline. Allow local network access if asked. Each console HTML file can be copied by itself.
Use Chrome or Edge with Web Bluetooth support for BLE; the controller must have BLE enabled.</p></body></html>
'''
    generated[output / "README.md"] = '''# Offline controller consoles

Open `index.html` in Chrome or Edge and choose your firmware, or open its HTML file directly.

1. Join the controller's Wi-Fi access point. No internet is needed.
2. The console automatically connects to `http://192.168.4.1`, including if you join Wi-Fi after opening it.
3. Allow local network access if the browser asks. If local file policy blocks requests, use **Open controller page**.

Without Wi-Fi, click **Connect BLE** and select the controller. BLE needs browser support and a BLE-enabled firmware build.
Pairing requires a click; the page does not open a device chooser automatically.
Disconnect BLE to resume Wi-Fi polling. Heartbeat and commands use the selected transport.

For another IP address, expand **Connection address**, enter it, and choose **Use address**.
Choose **Auto Wi-Fi** to return to the controller access point. Old saved addresses do not override auto mode.

Each of the four console HTML files includes all CSS and JavaScript. Copy just the matching file to your laptop.
The index needs the other files beside it. No Python, Arduino, web server, or npm installation is needed to open these files.

To regenerate all four from current source, run `python tools/build_standalone_consoles.py` from the repository root.
Use `--check` to check freshness. Generation uses only Python's standard library and does not build firmware.
'''
    stale = []
    for path, content in generated.items():
        if path.exists() and read(path) == content:
            continue
        stale.append(str(path.relative_to(ROOT)))
        if not args.check:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8", newline="\n")
    if args.check and stale:
        raise SystemExit("Stale standalone files: " + ", ".join(stale))
    print("Standalone consoles are current." if args.check else f"Updated {len(stale)} standalone files; open Standalone_Consoles/index.html")


if __name__ == "__main__":
    main()
