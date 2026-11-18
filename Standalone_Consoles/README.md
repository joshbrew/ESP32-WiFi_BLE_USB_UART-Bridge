# Offline controller consoles

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
