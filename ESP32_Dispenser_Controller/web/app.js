/* User-facing payload controls with an advanced shared command console. */
"use strict";

const BLE_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const BLE_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
const BLE_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
const BLE_GPS_UUID = "6e400004-b5a3-f393-e0a9-e50e24dcca9e";
const BLE_CHUNK = 20;
const API_BASE = String(window.ESP32_API_BASE || "").replace(/\/+$/, "");
const enc = new TextEncoder();
const dec = new TextDecoder();
const elements = {};

for (const id of [
  "buildVersion", "liveDot", "liveText", "payloadState", "payloadMeta",
  "armDispenser", "disarmDispenser", "pulseDuration", "dispensePulse",
  "stopDispense", "routineName", "routinePulse", "routineGap",
  "routineRepeats", "routineContinuous", "routineDelay", "savedRoutines", "saveRoutine", "runRoutine", "stopRoutine", "useHourLimit",
  "wifiRole", "wifiSsid", "wifiPassword", "wifiProtocol", "wifiOpenNetwork", "saveWifi", "wifiSetupStatus",
  "geoPoints", "geoSource", "geoStatus", "saveGeo", "startGeo", "stopGeo", "geoTestPosition", "geoTestSend", "geoTestStream", "geoPhoneGps",
  "routineSummary", "connectBle", "disconnectBle", "transportSummary",
  "compiledSummary", "commandPreset", "commandInput", "sendCommands",
  "stopAll", "clearLog", "log", "statusSummary", "statusGrid",
  "selfTestSummary", "otaFile", "otaUpload", "otaProgress", "otaMessage"
]) {
  elements[id] = document.getElementById(id);
}

const state = {
  transport: "http",
  latest: null,
  cursor: null,
  lastContact: 0,
  busy: false,
  otaActive: false,
  bleDevice: null,
  bleRx: null,
  bleTx: null,
  bleGpsRx: null,
  gpsSequence: 0,
  gpsBootMs: 0,
  phoneGpsWatch: null,
  bleBuffer: "",
  bleStatePending: false,
  bleStateAt: 0,
  httpQuietUntil: 0,
  nextStateAt: 0,
  retryAt: 0,
  httpFailures: 0,
  httpTail: Promise.resolve(),
  bleWriteTail: Promise.resolve(),
  commandGeneration: 0
};

const ACTION_GROUPS = [
  ["Dispenser", [
    ["Arm", "Arm", "dispenser", true],
    ["Disarm", "Disarm", "dispenser"],
    ["Pulse 100 ms", "Dispense:100", "armed"],
    ["Pulse 250 ms", "Dispense:250", "armed"],
    ["Stop output", "DispenseStop", "dispenser"],
    ["Dispenser status", "DispenserStatus", "dispenser"]
  ]],
  ["Payload profiles", [
    ["List profiles", "PayloadProfileList", "dispenser"],
    ["Show example profile", "PayloadProfileShow:profile1", "dispenser"],
    ["Use example profile", "PayloadProfileUse:profile1", "dispenser", true],
    ["Save current as profile1", "PayloadProfileSave:profile1", "dispenser", true]
  ]],
  ["Saved routines", [
    ["List routines", "RoutineList"],
    ["Routine status", "RoutineStatus"],
    ["Run dots", "RoutineRun:dots", "armed", true],
    ["Stop routine", "RoutineStop"]
  ]],
  ["Core", [
    ["Ping", "Ping"], ["Status", "Status"], ["Read config", "ConfigRead"],
    ["Help", "Help"], ["Indicators", "IndicatorTest"]
  ]],
  ["Send / bridge", [
    ["Send", "Send:hello from controller"],
    ["Send to BLE", "SendBLE:hello from Wi-Fi", "ble-output"],
    ["Send Wi-Fi", "SendWiFi:hello from BLE", "wifi-output"],
    ["Send USB", "SendSerial:hello from controller"], ["Send status", "SendStatus"]
  ]],
  ["Optional stepper / DAC", [
    ["360 deg CW", "DEG:10,360,1", "stepper"],
    ["360 deg CCW", "DEG:10,360,2", "stepper"],
    ["Coils off", "CoilsOff", "stepper"], ["DAC status", "DACStatus", "dac"]
  ]],
  ["Radio profiles", [
    ["Wi-Fi", "ModeWiFi", "wifi", true],
    ["Wi-Fi + BLE", "ModeWiFiBLE", "wifi+ble", true],
    ["Wi-Fi + BLE persistent", "ModeWiFiBLEP", "wifi+ble", true],
    ["BLE only", "ModeBLE", "ble", true], ["USB only", "ModeUSB", null, true],
    ["Radio status", "RadioStatus"]
  ]],
  ["Wi-Fi LR", [
    ["Enable LR (ESP32 peers only)", "WiFiLR:ON", "wifi", true],
    ["Use ordinary Wi-Fi", "WiFiLR:OFF", "wifi"],
    ["Apply Wi-Fi settings", "ConfigApply", "wifi", true],
    ["Save radio settings", "ConfigSave"]
  ]],
  ["Self-test", [
    ["Start sweep", "SelfTestStart", null, true], ["Print status", "SelfTestStatus"],
    ["Abort", "SelfTestAbort", null, true], ["Clear report", "SelfTestClear"]
  ]],
  ["Advanced", [
    ["Debug boot policy", "DebugMode"], ["Production boot policy", "ProductionMode"],
    ["Load saved radio config", "ConfigLoad"], ["Clear station network", "WiFiStaClear", "wifi", true]
  ]]
];

function api(path) { return API_BASE + path; }
function sleep(ms) { return new Promise(resolve => setTimeout(resolve, ms)); }
function now() { return new Date().toLocaleTimeString(); }
function usingBle() { return state.transport === "ble" && state.bleRx && state.bleTx; }
function setLive(ok, text) {
  elements.liveDot.classList.toggle("on", ok);
  elements.liveText.textContent = text;
}
function redact(line) {
  return /^(WiFi(?:Ap|Sta)Password:)/i.test(line)
    ? line.split(":", 1)[0] + ":<redacted>"
    : line;
}
function appendLog(line) {
  elements.log.textContent += line + "\n";
  if (elements.log.textContent.length > 32000) {
    elements.log.textContent = elements.log.textContent.slice(-24000);
  }
  elements.log.scrollTop = elements.log.scrollHeight;
}
function log(message, level = "info") {
  appendLog(`[${now()}] ${level.toUpperCase()} ${message}`);
}
function rawLog(message) { appendLog(String(message)); }
function rid() {
  return `web-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 7)}`;
}

async function fetchTimed(url, options = {}, ms = 4200) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), ms);
  try {
    return await fetch(url, { ...options, signal: controller.signal });
  } finally {
    clearTimeout(timer);
  }
}

async function readJson(response, label) {
  const body = await response.text();
  if (!response.ok) {
    throw new Error(`${label} HTTP ${response.status}${body ? `: ${body.slice(0, 100)}` : ""}`);
  }
  try {
    return JSON.parse(body);
  } catch (error) {
    throw new Error(`${label} invalid JSON (${body.length} bytes)`);
  }
}

function down(error) {
  const message = error?.name === "AbortError" ? "HTTP timeout" : error?.message || String(error);
  const wasLive = elements.liveDot.classList.contains("on");
  setLive(false, message);
  if (wasLive) log(message, "error");
}

async function pingHttp() {
  const response = await fetchTimed(api("/api/ping"), { cache: "no-store" }, 2200);
  await readJson(response, "Ping");
  state.lastContact = Date.now();
}

function compiled(requirement) {
  const radio = state.latest?.radio || {};
  const send = state.latest?.send || {};
  const addon = state.latest?.addon || {};
  const dispenser = state.latest?.dispenser || {};
  if (!requirement) return true;
  if (requirement === "addon") return addon.active !== false;
  if (requirement === "dispenser") return addon.dispenser === true;
  if (requirement === "armed") return addon.dispenser === true && dispenser.armed && !dispenser.faulted;
  if (requirement === "stepper") return addon.stepper === true || addon.name === "stepper-dac";
  if (requirement === "dac") return addon.dac === true || addon.name === "stepper-dac";
  if (requirement === "wifi") return radio.wifiCompiled !== false;
  if (requirement === "wifi-output") return send.wifi === true;
  if (requirement === "ble") return radio.bleCompiled !== false;
  if (requirement === "ble-output") return send.ble === true;
  if (requirement === "spp") return radio.sppCompiled !== false;
  return radio.wifiCompiled !== false && radio.bleCompiled !== false;
}

function buildCommandMenu() {
  for (const [group, items] of ACTION_GROUPS) {
    const optgroup = document.createElement("optgroup");
    optgroup.label = group;
    for (const [label, command, requires, confirm] of items) {
      const option = document.createElement("option");
      option.value = command;
      option.textContent = `${label} — ${command}`;
      if (requires) option.dataset.requires = requires;
      if (confirm) option.dataset.confirm = "1";
      optgroup.append(option);
    }
    elements.commandPreset.append(optgroup);
  }
}

function updateActions() {
  const ble = usingBle();
  const addon = state.latest?.addon || {};
  const dispenser = state.latest?.dispenser || {};
  const routine = state.latest?.routine || {};
  const geo = state.latest?.geo || {};
  for (const item of document.querySelectorAll("[data-command], option[data-requires]")) {
    const command = item.dataset.command || item.value || "";
    let disabled = state.busy || !compiled(item.dataset.requires);
    if (/^(StopAll|RoutineStop|GeoStop|DispenseStop|Disarm)$/i.test(command)) disabled = false;
    if (/^Arm$/i.test(command) && dispenser.armed && !dispenser.faulted) disabled = true;
    if (/^Dispense:/i.test(command) && (!dispenser.armed || dispenser.faulted || dispenser.dispensing)) disabled = true;
    if (routine.active && (/^(?:Arm|DispenserArm)$/i.test(command) || /^Dispense:/i.test(command))) disabled = true;
    if (/^PayloadProfile(?:Save|Use|Delete):/i.test(command) && (dispenser.armed || dispenser.dispensing || routine.active)) disabled = true;
    if (/^RoutineStop$/i.test(command) && !routine.active && !geo.active) disabled = false;
    item.disabled = disabled;
  }
  elements.dispensePulse.disabled = state.busy || !dispenser.armed || dispenser.faulted || dispenser.dispensing || routine.active || geo.active;
  elements.saveRoutine.disabled = state.busy || routine.active || geo.active || addon.dispenser !== true;
  elements.runRoutine.disabled = state.busy || addon.dispenser !== true || !dispenser.armed || dispenser.faulted || routine.active || geo.active;
  for (const button of elements.savedRoutines.querySelectorAll("button")) button.disabled = state.busy || !dispenser.armed || dispenser.faulted || routine.active || geo.active;
  elements.useHourLimit.disabled = state.busy || dispenser.armed || dispenser.dispensing || routine.active || geo.active || addon.dispenser !== true;
  elements.saveGeo.disabled = state.busy || geo.active || addon.dispenser !== true;
  elements.startGeo.disabled = state.busy || geo.active || !geo.saved || !geo.count || !geo.fresh || routine.active || addon.dispenser !== true;
  const gpsReady = geo.source === "MAVLINK" || (geo.source === "BLE" && ble && state.bleGpsRx);
  elements.geoTestSend.disabled = state.busy || !gpsReady || state.phoneGpsWatch !== null;
  elements.geoTestStream.disabled = !gpsReady || state.phoneGpsWatch !== null;
  elements.geoPhoneGps.disabled = geo.source !== "BLE" || !ble || !state.bleGpsRx || !navigator.geolocation;
  if (!gpsReady || (state.phoneGpsWatch !== null && geo.source !== "BLE")) stopTestGps();
  elements.saveWifi.disabled = state.busy || routine.active || geo.active;
  if (elements.commandPreset.selectedOptions[0]?.disabled) elements.commandPreset.selectedIndex = 0;
}

function renderTransport() {
  const ble = usingBle();
  elements.transportSummary.textContent = ble ? "BLE transport" : "Wi-Fi transport";
  elements.connectBle.disabled = state.busy || ble || !navigator.bluetooth || state.latest?.radio?.bleCompiled === false;
  elements.disconnectBle.disabled = state.busy || !ble;
  elements.otaUpload.disabled = state.busy || state.otaActive;
  elements.sendCommands.disabled = state.busy;
  updateActions();
}

function card(label, value) {
  return `<div class="status"><span>${label}</span><strong>${value}</strong></div>`;
}

function renderState(data) {
  state.latest = data;
  state.lastContact = Date.now();
  if (state.cursor === null) state.cursor = Number(data.latestEventId) || 0;

  const radio = data.radio || {};
  const send = data.send || {};
  const addon = data.addon || {};
  const dispenser = data.dispenser || {};
  const routine = data.routine || {};
  const motor = data.motor || {};
  const channels = data.dac?.channels || [];
  const queue = data.queue || {};
  const test = data.selfTest || {};
  const wifi = radio.wifiCompiled === false ? "not compiled" : `${radio.wifiLRActive ? "LR · " : ""}${radio.wifiState || "off"} ${radio.ip || ""}`.trim();
  const ble = radio.bleCompiled === false ? "not compiled" : send.ble ? "connected + TX" : send.bleConnected ? "connected, notifications off" : radio.bleRunning ? "advertising" : "off";
  const profile = radio.bootModeActive || data.bootMode || "?";

  let payload = "NOT COMPILED";
  if (addon.dispenser) {
    payload = dispenser.faulted ? "FAULT" : dispenser.dispensing ? "DISPENSING" : dispenser.armed ? "ARMED" : "DISARMED";
  }
  elements.payloadState.textContent = payload;
  elements.payloadState.className = `stateBadge ${dispenser.faulted ? "fault" : dispenser.dispensing ? "active" : dispenser.armed ? "armed" : "safe"}`;
  elements.payloadMeta.textContent = addon.dispenser
    ? `GPIO${dispenser.pin} · output ${dispenser.dispensing ? "ACTIVE" : "inactive"} · max pulse ${dispenser.maxPulseMs ? `${dispenser.maxPulseMs} ms` : "unlimited"} · pulse ${dispenser.remainingMs || 0} ms remaining · arm ${dispenser.armTimeoutMs ? `${dispenser.armRemainingMs || 0} ms remaining` : "no expiry"}${dispenser.interlockConfigured ? ` · interlock ${dispenser.interlockOpen ? "open" : "CLOSED"}` : ""}`
    : "This build uses the optional advanced stepper/DAC hardware profile.";
  for (const input of [elements.pulseDuration, elements.routinePulse]) {
    if (dispenser.maxPulseMs) input.max = String(dispenser.maxPulseMs);
    else input.removeAttribute("max");
  }
  elements.routineSummary.textContent = routine.active
    ? `Running ${routine.name}: step ${routine.step}/${routine.steps}, ${routine.repeats ? `repeat ${routine.repeat}/${routine.repeats}` : "repeating until stopped"}${routine.delayRemainingMs ? ` · initial delay ${Math.ceil(routine.delayRemainingMs / 1000)} s left` : ""}`
    : `${routine.stored || 0}/${routine.capacity || 0} routine slots used. Arm the dispenser before running.`;
  renderSavedRoutines(routine.library || []);
  const geo = data.geo || {};
  if (!elements.geoSource.dataset.initialized && !elements.geoSource.dataset.edited && ["API", "MAVLINK", "BLE"].includes(geo.source)) {
    elements.geoSource.value = geo.source; elements.geoSource.dataset.initialized = "true";
  }
  elements.geoStatus.textContent = `${geo.active ? (geo.running ? "Running routine" : "Waiting for next point") : "Stopped"} · ${geo.count || 0} points · next ${(geo.next || 0) + 1} · ${geo.source || "MAVLINK"} position ${geo.fresh ? "fresh" : "unavailable / stale"}${geo.distance !== undefined ? ` · ${geo.distance} m from next point` : ""}`;
  elements.wifiSetupStatus.textContent = `Current Wi-Fi: ${radio.wifiLRActive ? "LR" : "ordinary"} · ${radio.wifiState || "off"} · ${radio.ip || "no IP"}`;

  const cards = [
    card("Payload", payload),
    card("Payload profile", dispenser.profile || "compiled"),
    card("Routine", routine.active ? `${routine.name} ${routine.step}/${routine.steps}` : "idle"),
    card("Boot", profile), card("Wi-Fi", wifi), card("BLE", ble),
    card("Queue", `${queue.waiting ?? 0}/${queue.capacity ?? 0}`),
    card("Heap", `${data.freeHeap ?? 0} B free — ${data.largestHeapBlock ?? 0} B largest`),
    card("Self-test", test.active ? `${test.current || 0}/${test.total || 0}` : test.phase || "idle")
  ];
  if (addon.stepper || addon.name === "stepper-dac") {
    cards.push(card("Motor", motor.moving ? `${motor.currentRpm} RPM, ${motor.remainingSteps} steps` : motor.testActive ? "test active" : "idle"));
  }
  if (addon.dac || addon.name === "stepper-dac") {
    const channel = channels[0] || {};
    cards.push(card("DAC 1", channel.enabled ? `${channel.outputMv || 0} mV` : "off"));
  }
  elements.statusGrid.innerHTML = cards.join("");
  elements.statusSummary.textContent = `${payload} — ${profile} — Wi-Fi ${wifi} — BLE ${ble}`;
  elements.buildVersion.textContent = `${data.firmware || "ESP32 controller"} — ${data.version || "build"}`;
  elements.compiledSummary.textContent = `Profile: ${addon.name || "none"}; Wi-Fi ${radio.wifiCompiled === false ? "off" : "on"}, BLE ${radio.bleCompiled === false ? "off" : "on"}`;
  elements.selfTestSummary.textContent = `${test.phase || "Idle"}: ${test.current || 0}/${test.total || 0}, pass ${test.pass || 0}, fail ${test.fail || 0}, skip ${test.skip || 0}, boots ${test.boots || 0}. ${test.lastResult || "No run recorded."}`;
  setLive(true, usingBle() ? "BLE connected" : "HTTP connected");
  renderTransport();
}

async function postHttpNow(body, id, fast, generation) {
  const clean = String(body || "").replace(/\r/g, "").trim();
  if (!clean) throw new Error("Empty command");
  let last;
  const timeout = fast ? 2200 : 6500;
  const delays = fast ? [0] : [0, 180, 500];
  state.httpQuietUntil = Date.now() + timeout + 300;
  for (const delay of delays) {
    if (delay) await sleep(delay);
    if (generation !== state.commandGeneration && !/^(StopAll|RoutineStop|GeoStop|DispenseStop|Disarm)$/i.test(clean)) throw new Error("Pending command cancelled by stop");
    try {
      const response = await fetchTimed(api("/api/command"), {
        method: "POST",
        headers: { "Content-Type": "application/octet-stream", "Accept": "application/json", "X-Request-ID": id },
        body: clean,
        keepalive: fast
      }, timeout);
      const payload = await readJson(response, "Command");
      state.lastContact = Date.now();
      if (payload.accepted) {
        state.cursor = Math.max(state.cursor ?? 0, Number(payload.latestEventId) || 0);
        state.httpQuietUntil = Date.now() + 100;
        setLive(true, "HTTP connected");
        return payload;
      }
      last = new Error(payload.error || `Command HTTP ${response.status}`);
      if (!(response.status === 429 || response.status === 503 || payload.retryable)) break;
    } catch (error) {
      last = error;
    }
  }
  state.httpQuietUntil = Date.now() + 100;
  throw last || new Error("Command failed");
}

function postHttp(body, id = rid(), fast = false) {
  const generation = state.commandGeneration;
  const run = () => postHttpNow(body, id, fast, generation);
  if (fast && /^(StopAll|RoutineStop|GeoStop|DispenseStop|Disarm)$/i.test(body.trim())) return run();
  const pending = state.httpTail.then(run, run);
  state.httpTail = pending.catch(() => {});
  return pending;
}

async function writeBleNow(text, characteristic = state.bleRx) {
  if (!usingBle()) throw new Error("BLE disconnected");
  if (!characteristic) throw new Error("Bluetooth GPS channel unavailable; update the controller firmware");
  const bytes = typeof text === "string" ? enc.encode(text) : text;
  for (let index = 0; index < bytes.length; index += BLE_CHUNK) {
    const chunk = bytes.slice(index, index + BLE_CHUNK);
    if (characteristic.writeValueWithoutResponse) await characteristic.writeValueWithoutResponse(chunk);
    else await characteristic.writeValue(chunk);
    await sleep(14);
  }
}

function writeBle(text, characteristic = state.bleRx) {
  const generation = state.commandGeneration;
  const run = () => {
    if (generation !== state.commandGeneration) throw new Error("Pending BLE command cancelled by stop");
    return writeBleNow(text, characteristic);
  };
  // Keep complete lines intact when a stop arrives during a fragmented write.
  const pending = state.bleWriteTail.then(run, run);
  state.bleWriteTail = pending.catch(() => {});
  return pending;
}

async function runCommand(body, fast = false) {
  const clean = String(body || "").trim();
  if (!clean) throw new Error("Empty command");
  if (/^(StopAll|RoutineStop|GeoStop|DispenseStop|Disarm)$/i.test(clean)) { stopTestGps(); fast = true; ++state.commandGeneration; }
  log(`TX ${usingBle() ? "BLE" : "HTTP"} ${clean.split("\n").map(redact).join(" | ")}`, "send");
  if (usingBle()) {
    await writeBle(clean + "\n");
    return;
  }
  const result = await postHttp(clean, rid(), fast);
  log(`HTTP accepted ${result.acceptedLines || 1} line${result.acceptedLines === 1 ? "" : "s"}`, "status");
  return result;
}

function handleBleLine(line) {
  if (!line) return;
  state.lastContact = Date.now();
  if (line.startsWith("@STATE ")) {
    state.bleStatePending = false;
    state.bleStateAt = 0;
    try { renderState(JSON.parse(line.slice(7))); }
    catch (error) { log(`BLE state parse: ${error.message}`, "error"); }
    return;
  }
  const match = line.match(/^\[(\d+)\]\[([^\]]+)\]\[([^\]]+)\](?:\[([^\]]+)\])?\s*(.*)$/);
  if (match) {
    state.cursor = Math.max(state.cursor ?? 0, Number(match[1]) || 0);
    log(`${match[3]}${match[4] ? ` #${match[4]}` : ""}: ${match[5]}`, match[2].toLowerCase());
    return;
  }
  line.startsWith("@ERROR") ? log(line, "error") : rawLog(line);
}

function handleBleData(event) {
  state.bleBuffer += dec.decode(event.target.value, { stream: true });
  let at;
  while ((at = state.bleBuffer.indexOf("\n")) >= 0) {
    handleBleLine(state.bleBuffer.slice(0, at).replace(/\r$/, "").trim());
    state.bleBuffer = state.bleBuffer.slice(at + 1);
  }
}

function bleDisconnected() {
  stopTestGps();
  const wasBle = state.transport === "ble" || state.bleRx || state.bleTx;
  state.transport = "http";
  state.bleRx = null;
  state.bleTx = null;
  state.bleGpsRx = null;
  state.bleBuffer = "";
  state.nextStateAt = 0;
  setLive(false, "BLE disconnected");
  renderTransport();
  if (wasBle) log("BLE disconnected; Wi-Fi resumed", "warning");
}

async function connectBle() {
  if (!navigator.bluetooth) throw new Error("Web Bluetooth is unavailable");
  const armHandoff = Date.now() - state.lastContact < 7000
    ? postHttp("BLEWebHandoff", "ble-arm-" + Date.now()).catch(() => {})
    : Promise.resolve();
  const devicePromise = navigator.bluetooth.requestDevice({
    filters: [{ services: [BLE_SERVICE_UUID] }], optionalServices: [BLE_SERVICE_UUID]
  });
  const [, device] = await Promise.all([armHandoff, devicePromise]);
  device.addEventListener("gattserverdisconnected", bleDisconnected);
  const server = await device.gatt.connect();
  const service = await server.getPrimaryService(BLE_SERVICE_UUID);
  state.bleRx = await service.getCharacteristic(BLE_RX_UUID);
  state.bleTx = await service.getCharacteristic(BLE_TX_UUID);
  state.bleGpsRx = await service.getCharacteristic(BLE_GPS_UUID).catch(() => null);
  await state.bleTx.startNotifications();
  state.bleTx.addEventListener("characteristicvaluechanged", handleBleData);
  state.bleDevice = device;
  state.transport = "ble";
  state.bleBuffer = "";
  renderTransport();
  setLive(true, "BLE connected");
  log(`BLE connected to ${device.name || "ESP32"}`, "status");
  await writeBle("@STATE\n");
}

function disconnectBle() {
  if (state.bleDevice?.gatt?.connected) state.bleDevice.gatt.disconnect();
  else bleDisconnected();
}

function coexHttpMode() {
  const mode = state.latest?.radio?.bootModeActive;
  return mode === "WIFI_BLE" || mode === "WIFI_BLE_P";
}

function showEvent(event) {
  const text = event.t || "";
  if (event.q) { rawLog(text); return; }
  const source = event.s || "Internal";
  log(`${source}${event.r ? ` #${event.r}` : ""}: ${text}`, String(event.l || "info").toLowerCase());
}

async function pollHttpOnce() {
  const time = Date.now();
  if (state.busy || state.otaActive || time < state.httpQuietUntil) return 100;
  if (time < state.retryAt) return Math.min(350, state.retryAt - time);
  const getState = !state.latest || time >= state.nextStateAt;
  try {
    if (getState) {
      const response = await fetchTimed(api("/api/state"), { cache: "no-store" }, coexHttpMode() ? 4800 : 3200);
      renderState(await readJson(response, "State"));
      state.nextStateAt = Date.now() + 1000;
    } else {
      const response = await fetchTimed(api(`/api/events?since=${state.cursor ?? 0}&limit=4`), { cache: "no-store" }, coexHttpMode() ? 4200 : 2800);
      const payload = await readJson(response, "Events");
      state.lastContact = Date.now();
      if (payload.gap) log("Event history wrapped", "warning");
      for (const event of payload.events || []) showEvent(event);
      state.cursor = Math.max(state.cursor ?? 0, Number(payload.cursor) || 0);
      setLive(true, "HTTP connected");
      state.httpFailures = 0;
      state.retryAt = 0;
      return payload.more ? 120 : 400;
    }
    state.httpFailures = 0;
    state.retryAt = 0;
    return 300;
  } catch (error) {
    state.httpFailures++;
    if (state.httpFailures < 2) return 250;
    try {
      await pingHttp();
      setLive(true, "HTTP connected — API retrying");
      state.retryAt = Date.now() + (state.httpFailures < 4 ? 700 : 2200);
      return 300;
    } catch (pingError) {
      down(pingError);
      state.retryAt = Date.now() + 3000;
      return 1000;
    }
  }
}

async function pollLoop() {
  if (state.otaActive) { setTimeout(pollLoop, 250); return; }
  if (usingBle()) {
    if (state.bleStatePending && Date.now() - state.bleStateAt > 4000) state.bleStatePending = false;
    if (!state.bleStatePending) {
      state.bleStatePending = true;
      state.bleStateAt = Date.now();
      writeBle("@STATE\n").catch(error => {
        state.bleStatePending = false;
        state.bleStateAt = 0;
        log(error.message, "error");
      });
    }
    setTimeout(pollLoop, 700);
    return;
  }
  setTimeout(pollLoop, await pollHttpOnce());
}

async function withLock(work) {
  if (state.busy) return;
  state.busy = true;
  renderTransport();
  try { await work(); }
  catch (error) { log(error.message || String(error), "error"); }
  finally { state.busy = false; renderTransport(); }
}

async function prepareHttpForOta() {
  if (!usingBle()) return;
  elements.otaMessage.textContent = "Stopping outputs and handing OTA from BLE to Wi-Fi...";
  await writeBle("StopAll\n");
  await sleep(180);
  disconnectBle();
  let last;
  for (let attempt = 0; attempt < 6; attempt++) {
    try { await pingHttp(); log("OTA handed to Wi-Fi", "status"); return; }
    catch (error) { last = error; await sleep(500); }
  }
  throw new Error(`Wi-Fi OTA endpoint unavailable${last?.message ? `: ${last.message}` : ""}`);
}

async function uploadFirmware() {
  const file = elements.otaFile.files?.[0];
  if (!file) throw new Error("Choose an application .bin first");
  if (!file.name.toLowerCase().endsWith(".bin")) throw new Error("Choose an ESP32 application .bin file");
  if (file.size < 1024) throw new Error("Firmware image is too small");
  const magic = new Uint8Array(await file.slice(0, 1).arrayBuffer())[0];
  if (magic !== 0xe9) throw new Error("Selected file is not an ESP32 application image");
  stopTestGps(); state.otaActive = true;
  renderTransport();
  elements.otaProgress.value = 0;
  try {
    await prepareHttpForOta();
    elements.otaMessage.textContent = "Uploading over Wi-Fi...";
    const form = new FormData();
    form.append("firmware", file, file.name);
    await new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open("POST", api("/api/ota"));
      xhr.timeout = 180000;
      xhr.setRequestHeader("X-Request-ID", rid());
      xhr.setRequestHeader("X-Firmware-Size", String(file.size));
      xhr.upload.onprogress = event => {
        if (!event.lengthComputable) return;
        const progress = Math.min(99, Math.round(event.loaded / event.total * 100));
        elements.otaProgress.value = progress;
        elements.otaMessage.textContent = `Uploading ${progress}%`;
      };
      xhr.onerror = () => reject(new Error("OTA upload connection failed"));
      xhr.ontimeout = () => reject(new Error("OTA upload timed out"));
      xhr.onload = () => {
        let payload = {};
        try { payload = JSON.parse(xhr.responseText || "{}"); } catch (error) {}
        if (xhr.status >= 200 && xhr.status < 300 && payload.ok) resolve(payload);
        else reject(new Error(payload.error || `OTA HTTP ${xhr.status}`));
      };
      xhr.send(form);
    });
    elements.otaProgress.value = 100;
    elements.otaMessage.textContent = "Image validated. Controller is rebooting.";
    state.httpQuietUntil = Date.now() + 10000;
  } finally {
    state.otaActive = false;
    renderTransport();
  }
}

function selectedOption() {
  const option = elements.commandPreset.selectedOptions[0];
  return option && option.value === elements.commandInput.value.trim() ? option : null;
}
function sendInput() {
  const option = selectedOption();
  const command = elements.commandInput.value;
  if (option?.dataset.confirm && !window.confirm(`Send ${command}?\n\nUSB serial remains the recovery path.`)) return;
  withLock(() => runCommand(command));
}
function readInteger(element, low, high, label) {
  const value = Number(element.value);
  if (!Number.isInteger(value) || value < low || value > high) {
    throw new Error(`${label} must be ${low} to ${high}`);
  }
  return value;
}
function routineName() {
  const name = elements.routineName.value.trim();
  if (!/^[A-Za-z0-9_-]{1,15}$/.test(name)) throw new Error("Routine name must use 1-15 letters, digits, '-' or '_'");
  return name;
}
async function saveRoutinePreset() {
  const name = routineName();
  const maxPulse = Number(state.latest?.dispenser?.maxPulseMs) || 4294967295;
  const delay = readInteger(elements.routineDelay, 0, 4294967295, "Initial delay");
  const pulse = readInteger(elements.routinePulse, 1, maxPulse, "Pulse");
  const gap = readInteger(elements.routineGap, 0, 4294967295, "Gap");
  const repeats = elements.routineContinuous.checked ? "FOREVER" : readInteger(elements.routineRepeats, 1, 4294967295, "Repeats");
  await runCommand([
    `RoutineCreate:${name}`,
    `RoutineAdd:${name}:START_WAIT:${delay}`,
    `RoutineAdd:${name}:DISPENSE:${pulse}`,
    `RoutineAdd:${name}:WAIT_IDLE`,
    `RoutineAdd:${name}:WAIT:${gap}`,
    `RoutineRepeat:${name}:${repeats}`,
    `RoutineSave:${name}`
  ].join("\n"));
}
async function runNamedRoutine() {
  await runCommand(`RoutineRun:${routineName()}`);
}
async function customDispense() {
  const max = Number(state.latest?.dispenser?.maxPulseMs) || 4294967295;
  const duration = readInteger(elements.pulseDuration, 1, max, "Pulse");
  await runCommand(`Dispense:${duration}`, true);
}

function renderSavedRoutines(library) {
  const signature = JSON.stringify(library);
  if (elements.savedRoutines.dataset.signature === signature) return;
  elements.savedRoutines.dataset.signature = signature;
  elements.savedRoutines.replaceChildren();
  for (const [name, delay, pulse, gap, repeats, saved, steps] of library) {
    const row = document.createElement("div"); row.className = "savedRoutine";
    const label = document.createElement("div"); label.textContent = `${name}${saved ? "" : " (unsaved)"}`;
    const details = document.createElement("small");
    const repetition = repeats ? `${repeats} repeats` : "until stopped";
    details.textContent = pulse ? `Delay ${delay} ms · on ${pulse} ms / off ${gap} ms · ${repetition}` : `${steps} steps · ${repetition}`;
    label.append(details); row.append(label);
    if (saved) {
      const button = document.createElement("button"); button.textContent = `Run ${name}`; button.className = "primary";
      button.addEventListener("click", () => withLock(() => runCommand(`RoutineRun:${name}`)));
      row.append(button);
    }
    elements.savedRoutines.append(row);
  }
  if (!library.length) elements.savedRoutines.textContent = "No saved routines yet.";
}

async function saveWifiSettings() {
  const ssid = elements.wifiSsid.value.trim(), password = elements.wifiPassword.value;
  if (!/^(WIFI|WIFI_BLE|WIFI_BLE_P)$/.test(state.latest?.radio?.bootModeActive || "")) throw new Error("Select a Wi-Fi boot profile before applying client settings");
  if (elements.wifiRole.value !== "AP" && !ssid) throw new Error("Enter the network name to join");
  if (/[\r\n]/.test(ssid) || /[\r\n]/.test(password)) throw new Error("Network credentials must be one line");
  if (enc.encode(ssid).length > 32) throw new Error("Network name must fit 32 UTF-8 bytes");
  if (password && (enc.encode(password).length < 8 || enc.encode(password).length > 63)) throw new Error("Password must be 8-63 UTF-8 bytes");
  const commands = [`WiFiMode:${elements.wifiRole.value}`, `WiFiLR:${elements.wifiProtocol.value}`];
  if (ssid) commands.push(`WiFiStaSSID:${ssid}`);
  if (elements.wifiOpenNetwork.checked) commands.push("WiFiStaPassword:");
  else if (password) commands.push(`WiFiStaPassword:${password}`);
  commands.push("ConfigSave", "ConfigApply");
  await runCommand(commands.join("\n"));
  elements.wifiPassword.value = "";
}

function coordinateCommands(text) {
  const lines = text.trim().split(/\r?\n/).filter(line => line.trim());
  const capacity = Number(state.latest?.geo?.capacity) || 256;
  if (!lines.length || lines.length > capacity) throw new Error(`Enter 1-${capacity} coordinate lines`);
  return lines.map(line => {
    const fields = line.split(",").map(value => value.trim());
    const [lat, lon, radius] = fields.slice(0, 3).map(Number), name = fields[3];
    if (fields.length !== 4 || fields.slice(0, 3).some(value => !value) || !Number.isFinite(lat) || Math.abs(lat) > 90 || !Number.isFinite(lon) || Math.abs(lon) > 180 || !Number.isFinite(radius) || radius < 0.1 || radius > 1000 || !/^[A-Za-z0-9_-]{1,15}$/.test(name)) throw new Error("Each line needs latitude,longitude,radius 0.1-1000 m,saved routine name");
    return `GeoAdd:${lat},${lon},${radius},${name}`;
  });
}
async function saveCoordinateSequence() {
  const commands = ["GeoClear", `GeoSource:${elements.geoSource.value}`, ...coordinateCommands(elements.geoPoints.value), "GeoSave"];
  // The controller admits eight lines per batch; leave room for housekeeping.
  for (let i = 0; i < commands.length; i += 4) { await runCommand(commands.slice(i, i + 4).join("\n")); await sleep(250); }
}
function stopTestGps() {
  clearInterval(state.testGpsTimer); state.testGpsTimer = null;
  elements.geoTestStream.checked = false;
  if (state.phoneGpsWatch !== null) navigator.geolocation.clearWatch(state.phoneGpsWatch);
  state.phoneGpsWatch = null; elements.geoPhoneGps.checked = false;
}
function bluetoothPosition(lat, lon, accuracy, measuredAt = Date.now()) {
  // Keep GLOBAL_POSITION_INT advancing for new samples, including two writes
  // in one millisecond. Cached phone measurements never get a new timestamp.
  const boot = Math.max(state.gpsBootMs + 1, Math.floor(performance.now())) >>> 0;
  state.gpsBootMs = boot;
  const gps = new Uint8Array(38), global = new Uint8Array(28);
  const g = new DataView(gps.buffer), p = new DataView(global.buffer);
  const micros = Math.floor(measuredAt * 1000);
  g.setUint32(0, micros >>> 0, true); g.setUint32(4, Math.floor(micros / 4294967296), true);
  for (const [view, offset] of [[g, 8], [p, 4]]) {
    view.setInt32(offset, Math.round(lat * 1e7), true); view.setInt32(offset + 4, Math.round(lon * 1e7), true);
  }
  for (const offset of [20, 22, 24, 26]) g.setUint16(offset, 65535, true);
  gps[28] = 3; gps[29] = 255; g.setUint32(34, Math.max(1, Math.round(accuracy * 1000)), true);
  p.setUint32(0, boot, true); p.setUint16(26, 65535, true);
  const frame = (id, payload, extra) => {
    const bytes = new Uint8Array(payload.length + 12);
    bytes.set([253, payload.length, 0, 0, state.gpsSequence++ & 255, 1, 1, id, 0, 0]); bytes.set(payload, 10);
    let crc = 65535;
    for (const byte of [...bytes.slice(1, -2), extra]) {
      crc ^= byte; for (let i = 0; i < 8; ++i) crc = (crc >>> 1) ^ (crc & 1 ? 0x8408 : 0);
    }
    bytes[bytes.length - 2] = crc & 255; bytes[bytes.length - 1] = crc >>> 8;
    return bytes;
  };
  const result = new Uint8Array(90); result.set(frame(24, gps, 24)); result.set(frame(33, global, 104), 50);
  return result;
}
async function sendTestGps() {
  const fields = elements.geoTestPosition.value.split(",").map(value => value.trim());
  const [lat, lon, accuracy] = fields.map(Number);
  if (fields.length !== 3 || fields.some(value => !value) || !Number.isFinite(lat) || Math.abs(lat) > 90 || !Number.isFinite(lon) || Math.abs(lon) > 180 || !Number.isFinite(accuracy) || accuracy < 0 || accuracy > 100000)
    throw new Error("Enter latitude,longitude,accuracy in meters");
  if (state.latest?.geo?.source === "BLE") {
    await writeBle(bluetoothPosition(lat, lon, accuracy), state.bleGpsRx);
    log("Test GPS position sent over Bluetooth", "status");
  } else await runCommand(`GeoTestPosition:${lat},${lon},${accuracy}`);
}
function startPhoneGps() {
  stopTestGps();
  if (!usingBle() || !state.bleGpsRx || state.latest?.geo?.source !== "BLE") throw new Error("Connect BLE and save Bluetooth as the position source first");
  elements.geoPhoneGps.checked = true;
  let lastMeasurement = 0;
  state.phoneGpsWatch = navigator.geolocation.watchPosition(position => {
    if (state.phoneGpsWatch === null || position.timestamp <= lastMeasurement || Date.now() - position.timestamp > 3000 || position.timestamp > Date.now()) return;
    lastMeasurement = position.timestamp;
    const generation = state.commandGeneration;
    // Skip a measurement if another write is in progress. Never queue stale GPS.
    if (state.busy || state.otaActive) return;
    withLock(async () => {
      if (generation !== state.commandGeneration || state.phoneGpsWatch === null || Date.now() - position.timestamp > 3000) return;
      await writeBle(bluetoothPosition(position.coords.latitude, position.coords.longitude, position.coords.accuracy, position.timestamp), state.bleGpsRx);
    });
  }, error => { stopTestGps(); updateActions(); log(`Phone GPS: ${error.message}`, "error"); },
  { enableHighAccuracy: true, maximumAge: 0, timeout: 3000 });
  updateActions();
}

document.addEventListener("click", event => {
  const button = event.target.closest("button[data-command]");
  if (!button) return;
  const command = button.dataset.command;
  if (/^(StopAll|RoutineStop|GeoStop|DispenseStop|Disarm)$/i.test(command)) {
    runCommand(command, true).catch(error => log(error.message, "error"));
    return;
  }
  if (button.dataset.confirm && !window.confirm(`Send ${command}?`)) return;
  withLock(() => runCommand(command));
});
elements.commandPreset.addEventListener("change", () => {
  const option = elements.commandPreset.selectedOptions[0];
  if (option?.value) { elements.commandInput.value = option.value; elements.commandInput.focus(); }
});
elements.commandInput.addEventListener("input", () => {
  if (elements.commandPreset.value !== elements.commandInput.value.trim()) elements.commandPreset.selectedIndex = 0;
});
elements.commandInput.addEventListener("keydown", event => {
  if (event.ctrlKey && event.key === "Enter") { event.preventDefault(); sendInput(); }
});
elements.sendCommands.addEventListener("click", sendInput);
elements.stopAll.addEventListener("click", () => runCommand("StopAll", true).catch(error => log(error.message, "error")));
elements.dispensePulse.addEventListener("click", () => withLock(customDispense));
elements.saveRoutine.addEventListener("click", () => withLock(saveRoutinePreset));
elements.routineContinuous.addEventListener("change", () => { elements.routineRepeats.disabled = elements.routineContinuous.checked; });
elements.runRoutine.addEventListener("click", () => withLock(runNamedRoutine));
elements.useHourLimit.addEventListener("click", () => withLock(async () => {
  await runCommand("Disarm", true); await sleep(250);
  await runCommand("DispenserArmTimeout:0\nDispenserMaxPulse:0\nDispenserSave");
}));
elements.saveWifi.addEventListener("click", () => withLock(saveWifiSettings));
elements.geoSource.addEventListener("change", () => { elements.geoSource.dataset.edited = "true"; });
elements.saveGeo.addEventListener("click", () => withLock(saveCoordinateSequence));
elements.geoTestSend.addEventListener("click", () => withLock(sendTestGps));
elements.geoPhoneGps.addEventListener("change", () => {
  if (!elements.geoPhoneGps.checked) { stopTestGps(); updateActions(); return; }
  try { startPhoneGps(); } catch (error) { stopTestGps(); log(error.message, "error"); }
});
elements.geoTestStream.addEventListener("change", () => {
  if (!elements.geoTestStream.checked) { stopTestGps(); return; }
  withLock(async () => {
    await sendTestGps();
    if (elements.geoTestStream.checked) state.testGpsTimer = setInterval(() => {
      if (!state.busy && !state.otaActive) withLock(sendTestGps);
    }, 1000);
  }).finally(() => { if (!state.testGpsTimer) elements.geoTestStream.checked = false; });
});
elements.startGeo.addEventListener("click", () => withLock(() => runCommand("GeoStart")));
elements.stopGeo.addEventListener("click", () => runCommand("GeoStop", true).catch(error => log(error.message, "error")));
elements.connectBle.addEventListener("click", () => withLock(connectBle));
elements.disconnectBle.addEventListener("click", disconnectBle);
elements.clearLog.addEventListener("click", () => { elements.log.textContent = ""; });
elements.otaUpload.addEventListener("click", () => withLock(uploadFirmware));

buildCommandMenu();
renderTransport();
log("Controller console ready", "status");
pollLoop();
