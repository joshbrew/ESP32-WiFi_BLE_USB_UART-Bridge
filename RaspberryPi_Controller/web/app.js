"use strict";
const SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
const TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
const enc = new TextEncoder(), dec = new TextDecoder();
const $ = id => document.getElementById(id);
const U32 = 4294967295;
const state = {latest: null, cursor: 0, busy: false, connected: false, device: null, rx: null, buffer: "", blePending: false, bleRequested: 0};
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let httpTail = Promise.resolve(), bleTail = Promise.resolve();
let commandEpoch = 0;
let formProfile = null;
let routineCards = "", geoFormLoaded = false;
let geoPageWaiter = null;
let bleCommandWaiter = null;
let testGpsTimer = null, testGpsGeneration = 0, testGpsSending = false;

function log(message) {
  $("log").textContent += `[${new Date().toLocaleTimeString()}] ${message}\n`;
  if ($("log").textContent.length > 32000) $("log").textContent = $("log").textContent.slice(-24000);
  $("log").scrollTop = $("log").scrollHeight;
}
function live(ok, message) {
  state.connected = ok;
  if (!ok) stopTestGps();
  $("liveDot").classList.toggle("on", ok);
  $("liveText").textContent = message;
  updateButtons();
}
function render(data) {
  state.latest = data;
  const p = data.dispenser, routine = data.routine;
  const isDispenser = data.addon.dispenser, advanced = data.addon.stepper;
  $("dispenserPanel").hidden = !isDispenser;
  $("payloadSettings").hidden = !isDispenser;
  $("motorPanel").hidden = !advanced;
  $("dotBuilder").hidden = !isDispenser;
  $("continuousBuilder").hidden = !isDispenser;
  $("geoPanel").hidden = !isDispenser;
  $("saveRoutine").hidden = $("runRoutine").hidden = !isDispenser;
  $("buildVersion").textContent = `${data.firmware} · ${data.version}`;
  $("simulation").textContent = data.simulate ? "SIMULATION — commands do not drive physical hardware or change the Pi OS." : `HARDWARE LIVE — ${data.addon.name} profile.`;
  $("payloadState").textContent = p.faulted ? "FAULT" : p.dispensing ? "DISPENSING" : p.armed ? "ARMED" : "DISARMED";
  $("payloadState").className = "stateBadge " + (p.faulted ? "fault" : p.dispensing ? "active" : p.armed ? "armed" : "safe");
  $("payloadMeta").textContent = `BCM GPIO ${p.pin} · profile ${p.profile} · arm ${p.armUnlimited ? "no expiry" : p.armRemainingMs + " ms"} · output ${p.remainingMs} ms · maximum ${p.maxPulseMs ? p.maxPulseMs + " ms" : "unlimited"}`;
  $("pulseDuration").max = p.maxPulseMs || U32;
  $("routineSummary").textContent = routine.active ? `Running ${routine.name}, step ${routine.step + 1}, ${routine.continuous ? "repeating until stopped" : "repeat " + (routine.repeat + 1) + "/" + routine.repeats}${routine.delayRemainingMs ? " · initial delay " + routine.delayRemainingMs + " ms" : ""}` : `${routine.stored} routines in memory · ${routine.lastResult}`;
  $("selfTestSummary").textContent = `${data.selfTest.phase} · ${data.selfTest.current}/${data.selfTest.total} · pass ${data.selfTest.pass}, fail ${data.selfTest.fail}, skip ${data.selfTest.skip}. ${data.selfTest.lastResult}`;
  $("selfTestProgress").value = data.selfTest.current;
  $("bootSummary").textContent = `Boot policy: ${data.bootMode}. Indicators: ${data.indicators.enabled ? "enabled" : "disabled in installation config"}.`;
  $("profileSummary").textContent = `Saved profiles: ${Object.keys(data.payloadProfiles).join(", ") || "none"}`;
  $("librarySummary").textContent = `Saved routines: ${data.savedRoutineNames.join(", ") || "none"}. Four routines, ten steps each. Timing and repeats support 32-bit values; no total runtime cap.`;
  renderRoutineCards(data);
  const geo = data.geo;
  $("geoSummary").textContent = `${geo.active ? "ENABLED" : "Stopped"} · ${geo.result} · ${geo.count ? Math.min(geo.next + 1, geo.count) + "/" + geo.count + " points" : "No points"} · ${geo.saved ? "plan saved" : "unsaved plan"} · ${geo.fresh ? "fresh aircraft position" : "position unavailable / stale"}${geo.distance !== undefined ? " · next point " + geo.distance + " m" : ""}${geo.source === "MAVLINK" ? " · UDP " + geo.udpPort : " · custom API"}${geo.udpError ? " · " + geo.udpError : ""}`;
  if (!geoFormLoaded) { loadGeoEditor(geo); geoFormLoaded = true; }
  $("radioSummary").textContent = `${data.radio.bootModeActive} active · ${data.radio.desiredProfile} desired · ${data.radio.bootModeSaved} saved · Wi-Fi ${data.radio.wifiState} · BLE ${data.radio.bleRunning ? "advertising" : "off"} · Bluetooth serial ${data.radio.sppConnected ? "connected" : data.radio.sppRunning ? "listening" : "off"} · OS control ${data.radio.osControl ? "enabled" : "disabled"}. ${data.radio.applying ? "Applying…" : data.radio.lastError}`;
  $("updateSummary").textContent = `${data.update.enabled ? "Updates enabled" : "Updates disabled until an installation token is configured"} · ${data.update.phase}${data.update.version ? " · " + data.update.version : ""}`;
  if (advanced) $("motorSummary").textContent = `${data.stepper.moving ? "Moving" : "Idle"} · ${data.stepper.remainingSteps} steps left · coils ${data.stepper.coilsOn ? "energized" : "off"} · DAC ${data.dac.enabled ? data.dac.millivolts + " mV" : "off"} · digital ${data.dac.digitalEnabled ? "on" : "off"}${data.dac.fault ? " · FAULT: " + data.dac.fault : ""}`;
  if (formProfile !== data.addon.name) {
    formProfile = data.addon.name;
    $("settingPin").value = p.pin; $("settingPolarity").value = p.activeHigh ? "ON" : "OFF";
    $("settingDefault").value = p.defaultPulseMs; $("settingMax").value = p.maxPulseMs; $("settingArm").value = p.armTimeoutMs;
    $("wifiRole").value = data.radio.wifiMode; $("wifiFallback").value = data.radio.fallbackAP ? "ON" : "OFF";
    $("staSsid").value = data.radio.staSSID; $("apSsid").value = data.radio.apSSID;
    $("bootRadio").value = data.radio.bootModeSaved;
    if (advanced) {
      $("motorRpm").max = data.stepper.maxRpm; $("motorRev").value = data.stepper.baseStepsPerRev;
      $("motorMode").value = data.stepper.stepMode; $("motorOrder").value = data.stepper.stepOrder;
      $("dacMv").value = data.dac.millivolts; $("dacMv").max = data.dac.referenceMv;
    }
  }
  const cards = [["Hardware", data.addon.name], ["Queue", `${data.queue.waiting}/${data.queue.capacity}`], ["BLE", data.radio.bleError || (data.send.ble ? "Subscribed" : data.radio.bleRunning ? "Advertising" : "Off")], ["Bluetooth serial", data.send.spp ? "Connected" : data.radio.sppRunning ? "Listening" : "Off"], ["USB serial", data.send.usb ? "Open" : "Off"], ["UART", data.send.uart ? "Open" : "Off"], ["Stop lateness", `${p.maxStopLatenessMs} ms`], ["Memory available", data.memory.availableBytes === null ? "Unavailable on this host" : Math.round(data.memory.availableBytes / 1048576) + " MiB"]];
  $("statusGrid").replaceChildren(...cards.map(([title, value]) => {
    const card = document.createElement("div"), label = document.createElement("span"), content = document.createElement("strong");
    card.className = "status"; label.textContent = title; content.textContent = value;
    card.append(label, content); return card;
  }));
  updateButtons();
}
function updateButtons() {
  const p = state.latest?.dispenser, running = state.latest?.routine?.active;
  const geo = state.latest?.geo, mission = geo?.active;
  const adminBusy = state.latest?.administrationBusy;
  for (const button of document.querySelectorAll("button")) {
    if (["stopAll", "clearLog", "connectBle", "disconnectBle", "loadCustom"].includes(button.id)) continue;
    const safe = /^(StopAll|Disarm|DispenseStop|RoutineStop|GeoStop|Stop|CoilsOff|SelfTestAbort|.*:OFF)$/i.test(button.dataset.command || "");
    button.disabled = !safe && (state.busy || !state.connected || adminBusy || mission);
  }
  const idle = !state.busy && !adminBusy && state.connected && !running && !mission;
  for (const button of document.querySelectorAll("[data-pulse]")) button.disabled = !idle || !p?.armed || p.dispensing || (p.maxPulseMs && +button.dataset.pulse > p.maxPulseMs);
  document.querySelector('[data-command="Arm"]').disabled = !idle || !p || p.faulted || p.interlockOpen || p.dispensing;
  $("dispensePulse").disabled = $("runRoutine").disabled = !idle || !p?.armed || p.dispensing;
  $("runCustom").disabled = !idle || (state.latest?.addon?.dispenser && (!p?.armed || p.dispensing));
  $("saveRoutine").disabled = !idle || !state.latest;
  for (const id of ["saveCustom", "eraseCustom", "saveGeo", "loadGeo"]) $(id).disabled = !idle;
  for (const button of document.querySelectorAll("[data-saved-run]")) button.disabled = !idle || button.dataset.dirty === "true" || (state.latest?.addon?.dispenser && (!p?.armed || p.dispensing));
  $("startGeo").disabled = !idle || !geo?.saved || !geo.count || !geo.fresh || p?.faulted || p?.interlockOpen || p?.dispensing;
  $("sendFix").disabled = state.busy || !state.connected || geo?.source !== "API";
  const testGpsReady = state.connected && !adminBusy && geo?.source === "MAVLINK" && geo.udpReady && state.latest?.radio.wifiEnabled;
  $("geoTestSend").disabled = state.busy || !testGpsReady;
  $("geoTestStream").disabled = state.busy || !testGpsReady;
  if (!testGpsReady) stopTestGps();
  $("routineRepeats").disabled = $("routineContinuous").checked;
  $("editRepeats").disabled = $("customContinuous").checked;
  $("removeLimits").disabled = !idle || !!p?.installationMaxPulseMs || !!p?.installationArmTimeoutMs;
  $("sendCommands").disabled = state.busy;
  $("uploadUpdate").disabled = !idle || !!state.rx || !state.latest?.update?.enabled;
  $("handoffUpdate").disabled = state.busy || mission || !state.rx;
  $("moveMotor").disabled = !idle || state.latest?.stepper?.moving;
  $("connectBle").disabled = !!state.rx || state.busy || !navigator.bluetooth || !window.isSecureContext || state.latest?.radio.bleCompiled === false;
  $("disconnectBle").disabled = !state.rx;
  $("transportSummary").textContent = state.rx ? "BLE transport" : "HTTP transport";
  $("compiledSummary").textContent = "Wi-Fi · BLE UART · Bluetooth serial · USB · UART";
}
async function fetchJson(path, options = {}) {
  const controller = new AbortController(), timer = setTimeout(() => controller.abort(), 5000);
  try {
    const response = await fetch(path, {...options, signal: controller.signal, cache: "no-store"});
    const payload = await response.json();
    if (!response.ok) { const error = new Error(payload.error || `HTTP ${response.status}`); error.retryable = payload.retryable; throw error; }
    return payload;
  } finally { clearTimeout(timer); }
}
function writeBLE(text) {
  const work = async () => {
    const rx = state.rx;
    if (!rx) throw new Error("BLE disconnected");
    const bytes = enc.encode(text);
    for (let at = 0; at < bytes.length; at += 20) {
      const chunk = bytes.slice(at, at + 20);
      if (rx.writeValueWithResponse) await rx.writeValueWithResponse(chunk);
      else await rx.writeValue(chunk);
      await pause(14);
    }
  };
  const result = bleTail.then(work, work);
  bleTail = result.catch(() => {});
  return result;
}
function submit(body, emergency = false) {
  const commands = body.split(/\r?\n/).map(line => line.trim());
  if (commands.some(line => /^(StopAll|Disarm|DispenserDisarm|DispenseStop|DispenserOff|RoutineStop|GeoStop|Stop|CoilsOff|GPIO26:OFF)$/i.test(line))) { stopTestGps(); emergency = true; }
  if (commands.some(line => /^(GeoClear|GeoLoad|GeoResetPosition|GeoSource:|Mode|ConfigApply|Reboot|WebRestart|SelfTestStart)/i.test(line))) stopTestGps();
  const epoch = emergency ? ++commandEpoch : commandEpoch;
  const work = async () => {
    if (epoch !== commandEpoch) return;
    if (state.rx) return writeBLE(body.trim() + "\n");
    const id = `web-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
    // Reuse the ID after timeout so a pulse cannot execute twice.
    const deadline = Date.now() + 35000;
    for (let attempt = 0; ; attempt++) {
      if (epoch !== commandEpoch) return;
      try {
        return await fetchJson("/api/command", {method: "POST", headers: {"Content-Type": "text/plain", "X-Request-ID": id}, body});
      } catch (error) {
        if ((!error.retryable && attempt) || Date.now() >= deadline) throw error;
        await pause(error.retryable ? 500 : 150);
      }
    }
  };
  // Emergency HTTP stops bypass any pending request chain.
  if (emergency) return work();
  const result = httpTail.then(work, work);
  httpTail = result.catch(() => {});
  return result;
}
async function action(work) {
  if (state.busy) return;
  state.busy = true; updateButtons();
  try { await work(); } catch (error) { log(error.message); }
  finally { state.busy = false; updateButtons(); }
}
function bleData(event) {
  state.buffer += dec.decode(event.target.value, {stream: true});
  if (state.buffer.length > 32768) { state.buffer = ""; log("BLE frame overflow"); }
  let at;
  while ((at = state.buffer.indexOf("\n")) >= 0) {
    const line = state.buffer.slice(0, at).trim(); state.buffer = state.buffer.slice(at + 1);
    if (line.startsWith("@STATE ")) {
      state.blePending = false;
      try { render(JSON.parse(line.slice(7))); live(true, "BLE connected"); }
      catch (error) { log(`State parse: ${error.message}`); }
    } else if (line.startsWith("@GEO ")) {
      try { const page = JSON.parse(line.slice(5)); geoPageWaiter?.resolve(page); }
      catch (error) { geoPageWaiter?.reject(error); }
    } else if (line) {
      log(line);
      if (bleCommandWaiter) {
        if (line.includes("[ERROR]") || line.startsWith("@ERROR")) bleCommandWaiter.reject(new Error(line));
        else if (line.toUpperCase().includes("[ACK] " + bleCommandWaiter.command) || (bleCommandWaiter.stop && line.includes("[DONE] stopped and disarmed"))) bleCommandWaiter.resolve();
      }
    }
  }
}
function disconnected() {
  stopTestGps();
  geoPageWaiter?.reject(new Error("BLE disconnected during plan read"));
  bleCommandWaiter?.reject(new Error("BLE disconnected during command"));
  state.rx = null; state.buffer = ""; state.blePending = false;
  dec.decode(); updateButtons(); live(false, "BLE disconnected");
}
async function connectBLE() {
  const device = await navigator.bluetooth.requestDevice({filters: [{services: [SERVICE]}]});
  state.device = device;
  device.addEventListener("gattserverdisconnected", disconnected);
  try {
    const server = await device.gatt.connect(), service = await server.getPrimaryService(SERVICE);
    const tx = await service.getCharacteristic(TX);
    tx.addEventListener("characteristicvaluechanged", bleData);
    await tx.startNotifications();
    state.rx = await service.getCharacteristic(RX);
    updateButtons(); await writeBLE("@STATE\n");
  } catch (error) { device.gatt.disconnect(); disconnected(); throw error; }
}
async function poll() {
  try {
    if (state.rx) {
      if (!state.blePending || Date.now() - state.bleRequested > 5000) {
        state.blePending = true; state.bleRequested = Date.now();
        await writeBLE("@STATE\n");
      }
    } else {
      const data = await fetchJson("/api/state");
      // A restart resets the event sequence; fetch history from the beginning.
      if (data.latestEventId < state.cursor) state.cursor = 0;
      render(data);
      const events = await fetchJson(`/api/events?since=${state.cursor}&limit=16`);
      if (events.gap) log("Event history wrapped or controller restarted");
      for (const event of events.events) log(event.q ? event.t : `${event.l.toUpperCase()} ${event.s}: ${event.t}`);
      state.cursor = events.cursor; live(true, "HTTP connected");
    }
  } catch (error) { live(false, "Disconnected"); updateButtons(); }
  setTimeout(poll, state.rx ? 1000 : 700);
}
function routineName() {
  const name = $("routineName").value.trim();
  if (!/^[A-Za-z0-9_-]{1,15}$/.test(name)) throw new Error("Use 1-15 letters, digits, hyphens, or underscores for the routine name");
  return name;
}
for (const button of document.querySelectorAll("[data-command]")) button.addEventListener("click", () => {
  const command = button.dataset.command;
  if (/^(Disarm|DispenseStop|RoutineStop|GeoStop|Stop|CoilsOff|.*:OFF)$/i.test(command)) submit(command, true).catch(error => log(error.message));
  else action(() => submit(command));
});
$("stopAll").addEventListener("click", () => submit("StopAll", true).catch(error => log(error.message)));
$("sendCommands").addEventListener("click", () => action(() => submit($("commandInput").value)));
$("dispensePulse").addEventListener("click", () => action(() => submit(`Dispense:${$("pulseDuration").value}`)));
$("saveRoutine").addEventListener("click", () => action(() => {
  const name = routineName();
  const delay = integerField("routineDelay", 0), pulse = integerField("routinePulse"), gap = integerField("routineGap", 0), repeats = $("routineContinuous").checked ? "FOREVER" : integerField("routineRepeats");
  return sequence([`RoutineCreate:${name}`, `RoutineAdd:${name}:START_WAIT:${delay}`, `RoutineAdd:${name}:DISPENSE:${pulse}`, `RoutineAdd:${name}:WAIT_IDLE`, `RoutineAdd:${name}:WAIT:${gap}`, `RoutineRepeat:${name}:${repeats}`, `RoutineSave:${name}`]);
}));
$("runRoutine").addEventListener("click", () => action(() => submit(`RoutineRun:${routineName()}`)));
$("connectBle").addEventListener("click", () => action(connectBLE));
$("disconnectBle").addEventListener("click", () => state.device?.gatt.disconnect());
$("clearLog").addEventListener("click", () => { $("log").textContent = ""; });
for (const command of ["Ping", "Help", "Status", "PayloadProfileList", "PayloadProfileSave:fine", "PayloadProfileUse:fine", "RoutineList", "RoutineShow:dots", "DispenserDefaultPulse:150", "DispenserMaxPulse:750", "DispenserSave", "SendBLE:hello from Wi-Fi", "SendWiFi:hello from BLE", "SendUSB:hello", "SendUART:hello"]) {
  const option = document.createElement("option"); option.value = option.textContent = command; $("commandPreset").append(option);
}
$("commandPreset").addEventListener("change", () => { if ($("commandPreset").value) $("commandInput").value = $("commandPreset").value; });

function nameOf(id) {
  const name = $(id).value.trim();
  if (!/^[A-Za-z0-9_-]{1,15}$/.test(name)) throw new Error("Names need 1-15 letters, digits, hyphens or underscores");
  return name;
}
async function sequence(lines, progress = () => {}) {
  let epoch = commandEpoch;
  for (const line of lines) {
    if (epoch !== commandEpoch) throw new Error("Operation cancelled by stop");
    // A standalone disarm inside a settings sequence is its authorized barrier.
    if (/^(Disarm|StopAll|GeoStop)$/i.test(line)) epoch++;
    if (state.rx) {
      const confirmation = new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error("Controller did not confirm BLE command")), 15000);
        bleCommandWaiter = {command: line.split(":", 1)[0].toUpperCase(), stop: /^(Disarm|StopAll|GeoStop)$/i.test(line), resolve: () => { clearTimeout(timer); resolve(); }, reject: error => { clearTimeout(timer); reject(error); }};
      });
      try { await Promise.all([confirmation, submit(line)]); }
      catch (error) { bleCommandWaiter?.reject(error); throw error; }
      finally { bleCommandWaiter = null; }
      progress(line); continue;
    }
    const result = await submit(line);
    if (epoch !== commandEpoch) throw new Error("Operation cancelled by stop");
    let done = false;
    for (let attempt = 0; attempt < 50; attempt++) {
      await pause(60);
      const events = await fetchJson(`/api/events?since=${result.latestEventId}&limit=32`);
      const responses = events.events.filter(event => event.r === result.requestId);
      const error = responses.find(event => event.l === "error");
      if (error) throw new Error(error.t);
      if (responses.length >= line.split("\n").length) { done = true; break; }
    }
    if (!done) throw new Error("Controller did not confirm the command");
    progress(line);
  }
}
function bind(id, handler) { $(id).addEventListener("click", () => action(handler)); }
function integerField(id, minimum = 1, maximum = U32) {
  const text = $(id).value.trim(), value = Number(text);
  if (!/^\d{1,10}$/.test(text) || !Number.isInteger(value) || value < minimum || value > maximum) throw new Error(`Enter a whole number from ${minimum} to ${maximum}`);
  return value;
}
function renderRoutineCards(data) {
  const signature = JSON.stringify([data.routineLibrary, data.savedRoutineLibrary]);
  if (signature === routineCards) return;
  routineCards = signature;
  $("savedRoutines").replaceChildren(...data.savedRoutineNames.map(name => {
    const record = data.savedRoutineLibrary[name], dirty = JSON.stringify(record) !== JSON.stringify(data.routineLibrary[name]);
    const card = document.createElement("div"), button = document.createElement("button"), summary = document.createElement("span");
    const timing = type => record.steps.map(step => step.replace(/^COMMAND:/i, "")).filter(step => step.toUpperCase().startsWith(type + ":")).map(step => step.split(":").at(-1)).join(", ") || "0";
    card.className = "savedRoutine"; button.className = "primary";
    button.textContent = `Run ${name}`; button.dataset.savedRun = name; button.dataset.dirty = String(dirty);
    summary.textContent = `Delay ${timing("START_WAIT")} ms · pulse ${timing("DISPENSE")} ms · gap ${timing("WAIT")} ms · ${record.repeats ? record.repeats + " repeats" : "until stopped"}${dirty ? " · editor changes need saving" : ""}`;
    button.addEventListener("click", () => action(() => submit(`RoutineRun:${name}`)));
    card.append(button, summary); return card;
  }));
}
bind("applyPayload", () => {
  const previous = state.latest.dispenser;
  const pulse = integerField("settingDefault"), maximum = integerField("settingMax", 0), arm = integerField("settingArm", 0);
  if ((maximum && pulse > maximum) || (arm && (maximum || pulse) > arm)) throw new Error("Default pulse must fit the enabled maximum; maximum/default must fit an enabled arm timeout");
  const lines = [];
  const armExpand = !arm || (previous.armTimeoutMs && arm > previous.armTimeoutMs);
  const maxExpand = !maximum || (previous.maxPulseMs && maximum > previous.maxPulseMs);
  if (armExpand) lines.push(`DispenserArmTimeout:${arm}`);
  if (maxExpand) lines.push(`DispenserMaxPulse:${maximum}`);
  lines.push(`DispenserDefaultPulse:${pulse}`);
  if (!maxExpand) lines.push(`DispenserMaxPulse:${maximum}`);
  if (!armExpand) lines.push(`DispenserArmTimeout:${arm}`);
  return sequence([...lines, `DispenserPin:${$("settingPin").value}`, `DispenserActiveHigh:${$("settingPolarity").value}`]);
});
bind("removeLimits", async () => {
  await sequence(["Disarm", "DispenserArmTimeout:0", "DispenserMaxPulse:0", "DispenserSave"]);
  $("settingMax").value = $("settingArm").value = "0";
});
bind("saveProfile", () => submit(`PayloadProfileSave:${nameOf("profileName")}`));
bind("useProfile", () => submit(`PayloadProfileUse:${nameOf("profileName")}`));
bind("deleteProfile", () => submit(`PayloadProfileDelete:${nameOf("profileName")}`));
bind("moveMotor", () => submit(`${$("motorUnits").value}:${$("motorRpm").value},${$("motorTravel").value},${$("motorDirection").value}`));
bind("motorSettings", () => sequence([`StepMode:${$("motorMode").value}`, `SetRevSteps:${$("motorRev").value}`, `StepOrder:${$("motorOrder").value}`]));
bind("setDac", () => submit(`DAC1:MV:${$("dacMv").value}`));
bind("saveCustom", async () => {
  const name = nameOf("editRoutineName"), steps = $("routineSteps").value.split(/\r?\n/).map(line => line.trim()).filter(Boolean);
  if (!steps.length || steps.length > 10) throw new Error("Use 1-10 routine steps");
  const repeats = $("customContinuous").checked ? "FOREVER" : integerField("editRepeats");
  if (steps.some((step, index) => index > 0 && /^START_WAIT:/i.test(step))) throw new Error("Initial delay must be the first step");
  await sequence([`RoutineCreate:${name}`, ...steps.map(step => `RoutineAdd:${name}:${step}`), `RoutineRepeat:${name}:${repeats}`, `RoutineSave:${name}`]);
});
bind("loadCustom", () => {
  const record = state.latest?.routineLibrary[nameOf("editRoutineName").toLowerCase()];
  if (!record) throw new Error("Choose a routine from the saved library");
  $("customContinuous").checked = record.repeats === 0;
  $("editRepeats").value = record.repeats || 1;
  updateButtons();
  $("routineSteps").value = record.steps.join("\n");
});
bind("runCustom", () => submit(`RoutineRun:${nameOf("editRoutineName")}`));
bind("eraseCustom", () => submit(`RoutineErase:${nameOf("editRoutineName")}`));
async function saveNetwork(apply = false) {
  const lines = [`WiFiMode:${$("wifiRole").value}`, `WiFiFallbackAP:${$("wifiFallback").value}`, `WiFiTxPower:${$("wifiPower").value}`, `WiFiStaSSID:${$("staSsid").value}`, `WiFiApSSID:${$("apSsid").value}`];
  if ($("staOpen").checked || $("staPassword").value) lines.push(`WiFiStaPassword:${$("staOpen").checked ? "" : $("staPassword").value}`);
  if ($("apPassword").value) lines.push(`WiFiApPassword:${$("apPassword").value}`);
  await sequence([...lines, "ConfigSave"]);
  $("staPassword").value = $("apPassword").value = "";
  if (apply) await sequence(["ConfigApply"]);
}
bind("saveRadio", () => saveNetwork());
bind("saveApplyRadio", () => saveNetwork(true));
function loadGeoEditor(geo) {
  $("geoSource").value = geo.source;
  if (geo.points) $("geoPoints").value = geo.points.map(item => `${item.latitude},${item.longitude},${item.radiusMeters},${item.routine}`).join("\n");
}
bind("saveGeo", async () => {
  const points = $("geoPoints").value.split(/\r?\n/).map(line => line.trim()).filter(Boolean);
  if (!points.length || points.length > (state.latest.geo.capacity || 500)) throw new Error("Use 1–500 coordinate points");
  for (const line of points) {
    const fields = line.split(",").map(field => field.trim()), [lat, lon, radius] = fields.slice(0, 3).map(Number);
    if (fields.length !== 4 || fields.slice(0, 3).some(field => !field) || ![lat, lon, radius].every(Number.isFinite) || Math.abs(lat) > 90 || Math.abs(lon) > 180 || radius < .1 || radius > 1000 || !/^[A-Za-z0-9_-]{1,15}$/.test(fields[3])) throw new Error("Each point needs valid latitude, longitude, radius 0.1–1000 m and saved routine name");
    if (!state.latest.savedRoutineNames.includes(fields[3].toLowerCase())) throw new Error(`Save routine ${fields[3]} first`);
  }
  const additions = points.map(line => `GeoAdd:${line}`), batches = [];
  // HTTP admits eight lines atomically. BLE sends individual newline commands.
  const size = state.rx ? 1 : 8;
  for (let at = 0; at < additions.length; at += size) batches.push(additions.slice(at, at + size).join("\n"));
  $("geoBuildProgress").max = points.length; $("geoBuildProgress").value = 0;
  $("geoBuildStatus").textContent = `Building ${points.length} points…`;
  try {
    await sequence(["GeoClear", `GeoSource:${$("geoSource").value}`, ...batches, "GeoSave"], line => {
      $("geoBuildProgress").value += line.split("\n").filter(command => command.startsWith("GeoAdd:")).length;
      $("geoBuildStatus").textContent = `${$("geoBuildProgress").value}/${points.length} points added; saving after all points are accepted.`;
    });
    $("geoBuildStatus").textContent = `${points.length} points saved.`;
  } catch (error) { $("geoBuildStatus").textContent = "Build interrupted; the previous saved plan can be reloaded."; throw error; }
});
async function readGeoPages() {
  const points = [], epoch = commandEpoch;
  let offset = 0;
  while (true) {
    if (epoch !== commandEpoch) throw new Error("Plan read cancelled by stop");
    const page = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("Timed out reading coordinate page")), 10000);
      geoPageWaiter = {resolve: value => { clearTimeout(timer); resolve(value); }, reject: error => { clearTimeout(timer); reject(error); }};
      writeBLE(`@GEO:${offset}\n`).catch(geoPageWaiter.reject);
    }).finally(() => { geoPageWaiter = null; });
    if (page.offset !== offset || page.next !== offset + page.points.length || page.next > 500) throw new Error("Invalid coordinate page");
    points.push(...page.points);
    $("geoBuildStatus").textContent = `Reading ${points.length}/${page.count} saved points…`;
    if (page.next >= page.count) return points;
    if (!page.points.length) throw new Error("Empty coordinate page");
    offset = page.next;
  }
}
bind("loadGeo", async () => {
  await sequence(["GeoLoad"]);
  if (!state.rx) { render(await fetchJson("/api/state")); loadGeoEditor(state.latest.geo); }
  else { const points = await readGeoPages(); loadGeoEditor({...state.latest.geo, points}); }
  $("geoBuildStatus").textContent = "Saved plan loaded into editor.";
});
bind("startGeo", () => submit("GeoStart"));
bind("sendFix", () => {
  const body = [$("fixLatitude").value, $("fixLongitude").value, $("fixAccuracy").value, integerField("fixAge", 0, 3000)].join(",");
  return state.rx ? submit("GeoPosition:" + body) : fetchJson("/api/position", {method: "POST", headers: {"Content-Type": "text/plain"}, body});
});
bind("saveBootRadio", () => submit(`RadioBoot:${$("bootRadio").value}`));
bind("handoffUpdate", async () => {
  await submit("ModeWiFiBLE");
  await pause(1500);
  state.device?.gatt.disconnect();
  log("Reconnect over the Pi Wi-Fi network, then upload the bundle using HTTP.");
});
bind("uploadUpdate", async () => {
  stopTestGps();
  const file = $("updateFile").files[0], token = $("updateToken").value;
  if (!file || !file.name.toLowerCase().endsWith(".zip") || file.size > 8 * 1024 * 1024) throw new Error("Choose a Pi .zip bundle no larger than 8 MiB");
  if (!token) throw new Error("Enter the private installation update token");
  const hash = crypto.subtle ? [...new Uint8Array(await crypto.subtle.digest("SHA-256", await file.arrayBuffer()))].map(byte => byte.toString(16).padStart(2, "0")).join("") : "";
  await new Promise((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open("POST", "/api/ota"); request.timeout = 65000;
    request.setRequestHeader("Content-Type", "application/zip");
    request.setRequestHeader("X-Update-Token", token);
    if (hash) request.setRequestHeader("X-Update-SHA256", hash);
    request.upload.onprogress = event => { if (event.lengthComputable) $("uploadProgress").value = event.loaded / event.total * 100; };
    request.onload = () => {
      try {
        const result = JSON.parse(request.responseText);
        if (request.status !== 202) throw new Error(result.error || "Upload rejected");
        log(`Update ${result.version}: ${result.phase}. The page reconnects automatically after restart.`);
        $("updateToken").value = ""; resolve();
      } catch (error) { reject(error); }
    };
    request.onerror = request.ontimeout = () => reject(new Error("Upload connection interrupted; check update status before retrying"));
    request.send(file);
  });
});
function stopTestGps() {
  clearInterval(testGpsTimer); testGpsTimer = null; testGpsGeneration++;
  $("geoTestStream").checked = false;
  $("geoTestStatus").textContent = "Test feed off. Stop sequence or STOP ALL also ends this feed.";
}
async function sendTestGps() {
  const fields = $("geoTestPosition").value.split(",").map(value => value.trim()), [lat, lon, accuracy] = fields.map(Number);
  if (fields.length !== 3 || fields.some(value => !value) || ![lat, lon, accuracy].every(Number.isFinite) || Math.abs(lat) > 90 || Math.abs(lon) > 180 || (accuracy < 0 && accuracy !== -1) || accuracy > 100000) throw new Error("Enter latitude,longitude,accuracy meters (-1 unknown or 0–100000)");
  await sequence([`GeoTestPosition:${lat},${lon},${accuracy}`]);
}
bind("geoTestSend", sendTestGps);
$("geoTestStream").addEventListener("change", () => {
  if (!$("geoTestStream").checked) { stopTestGps(); return; }
  const generation = ++testGpsGeneration;
  action(async () => {
    try {
      await sendTestGps();
      if (generation !== testGpsGeneration || !$("geoTestStream").checked) return;
      $("geoTestStatus").textContent = "Sending test GPS every second from the current input.";
      testGpsTimer = setInterval(async () => {
        if (state.busy || testGpsSending) return;
        testGpsSending = true;
        try { await sendTestGps(); }
        catch (error) { stopTestGps(); log(error.message); }
        finally { testGpsSending = false; }
      }, 1000);
    } catch (error) { stopTestGps(); throw error; }
  });
});
for (const id of ["routineContinuous", "customContinuous"]) $(id).addEventListener("change", updateButtons);
window.addEventListener("pagehide", stopTestGps);
window.addEventListener("beforeunload", stopTestGps);
updateButtons(); poll();
