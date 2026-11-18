(() => {
  "use strict";
  const accessPoint = "http://192.168.4.1";
  const storageKey = "esp32-standalone-custom-url";
  const input = document.getElementById("standaloneDeviceUrl");
  const status = document.getElementById("standaloneDeviceStatus");
  const connect = document.getElementById("standaloneUseDevice");
  const automatic = document.getElementById("standaloneAutoDevice");
  const open = document.getElementById("standaloneOpenDevice");

  function normalize(value) {
    let text = String(value || "").trim();
    if (!text) return accessPoint;
    if (!/^https?:\/\//i.test(text)) text = "http://" + text;
    const url = new URL(text);
    if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password ||
        url.pathname !== '/' || url.search || url.hash) {
      throw new Error("Use a controller address such as http://192.168.4.1");
    }
    return url.origin;
  }
  function savedAddress() {
    try { return localStorage.getItem(storageKey) || accessPoint; }
    catch (_) { return accessPoint; }
  }
  const params = new URLSearchParams(location.hash.slice(1));
  let custom = null;
  try { if (params.has('controller')) custom = normalize(params.get('controller')); }
  catch (_) { /* An invalid bookmark falls back to the controller access point. */ }
  // Old saved addresses never redirect an automatic AP connection. A custom
  // address is explicit in this file's URL so separately opened files stay auto.
  window.ESP32_API_BASE = custom || accessPoint;
  input.value = custom || savedAddress();
  automatic.disabled = !custom;
  open.href = window.ESP32_API_BASE;
  status.textContent = (custom ? "Custom Wi-Fi: " : "Auto Wi-Fi: ") + window.ESP32_API_BASE +
    ". Join the controller Wi-Fi; connection retries automatically. Or choose Connect BLE.";

  function reload(address) {
    const url = new URL(location.href);
    url.hash = address ? new URLSearchParams({controller: address}).toString() : '';
    if (url.href === location.href) location.reload();
    else { history.replaceState(null, '', url.href); location.reload(); }
  }
  function useAddress() {
    try {
      const address = normalize(input.value);
      input.setCustomValidity('');
      try { localStorage.setItem(storageKey, address); } catch (_) {}
      reload(address);
    } catch (error) {
      input.setCustomValidity(error.message);
      input.reportValidity();
    }
  }
  connect.addEventListener('click', useAddress);
  input.addEventListener('input', () => input.setCustomValidity(''));
  input.addEventListener('keydown', event => {
    if (event.key === 'Enter') { event.preventDefault(); useAddress(); }
  });
  automatic.addEventListener('click', () => reload(null));
})();
