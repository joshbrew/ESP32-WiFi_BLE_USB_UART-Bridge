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
  "stopDispense", "routineName", "routinePulse", "routineGap", "outputRows", "outputPin", "addOutput", "singleOutput", "saveOutputs",
  "routineRepeats", "routineContinuous", "routineDelay", "savedRoutines", "saveRoutine", "runRoutine", "stopRoutine", "useHourLimit",
  "wifiRole", "wifiSsid", "wifiPassword", "wifiProtocol", "wifiOpenNetwork", "saveWifi", "wifiSetupStatus",
  "geoPoints", "geoSource", "geoStatus", "saveGeo", "startGeo", "stopGeo", "geoTestPosition", "geoTestSend", "geoTestStream", "geoPhoneGps",
  "routineSummary", "connectBle", "disconnectBle", "transportSummary",
  "compiledSummary", "commandPreset", "commandInput", "sendCommands",
  "stopAll", "clearLog", "log", "statusSummary", "statusGrid",
  "selfTestSummary", "otaFile", "otaUpload", "otaProgress", "otaMessage", "presetGroups", "followLog", "logCount"
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
  pollAbort:null,offlineLogged:false,lastHttpError:"",logLines:0,
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
function setLive(ok,text,kind="") {
  elements.liveDot.classList.toggle("on",ok);
  elements.liveDot.classList.toggle("retrying",kind==="retrying");
  elements.liveDot.classList.toggle("offline",!ok&&kind!=="retrying");
  elements.liveText.textContent=text;
  elements.liveText.title=state.lastHttpError||text;
}
function redact(line) {
  return /^(WiFi(?:Ap|Sta)Password:)/i.test(line)
    ? line.split(":", 1)[0] + ":<redacted>"
    : line;
}
function appendLog(line) {
  const nearBottom=elements.log.scrollHeight-elements.log.scrollTop-elements.log.clientHeight<40;
  elements.log.textContent+=line+"\n";
  if(elements.log.textContent.length>32000)elements.log.textContent=elements.log.textContent.slice(-24000);
  state.logLines=elements.log.textContent.split("\n").length-1;
  elements.logCount.textContent=`${state.logLines} lines`;
  if(elements.followLog.checked&&nearBottom)elements.log.scrollTop=elements.log.scrollHeight;
}
function log(message, level = "info") {
  appendLog(`[${now()}] ${level.toUpperCase()} ${message}`);
}
function rawLog(message) { appendLog(String(message)); }
function rid() {
  return `web-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 7)}`;
}

async function fetchTimed(url,options={},ms=4200,background=false) {
  const controller=new AbortController();
  let timedOut=false;
  if(background)state.pollAbort=controller;
  const timer=setTimeout(()=>{timedOut=true;controller.abort()},ms);
  try {
    const response=await fetch(url,{...options,signal:controller.signal});
    // A fetch resolves at headers. Keep the deadline through body consumption.
    const body=await response.text();
    return {ok:response.ok,status:response.status,headers:response.headers,text:async()=>body};
  } catch(error) {
    if(timedOut){const timeout=new Error("Controller response timed out");timeout.name="TimeoutError";throw timeout}
    throw error;
  } finally {
    clearTimeout(timer);
    if(state.pollAbort===controller)state.pollAbort=null;
  }
}

async function readJson(response,label) {
  const text=await response.text();
  let payload;
  try {payload=JSON.parse(text)} catch(error) {
    if(response.ok)throw new Error(`${label}: incomplete or invalid response`);
  }
  if(!response.ok){
    const error=new Error(`${label}: ${payload?.error||`HTTP ${response.status}`}`);
    error.httpStatus=response.status;
    error.retryable=payload?.retryable===true||[408,429,502,503,504].includes(response.status);
    throw error;
  }
  return payload;
}

function down(error) {
  state.lastHttpError=error?.message||String(error);
  setLive(false,"Controller offline");
  if(!state.offlineLogged){log("Connection interrupted; reconnecting automatically.","warning");state.offlineLogged=true}
}

async function pingHttp() {
  return queueHttp(async()=>{
    const response=await fetchTimed(api("/api/ping"),{cache:"no-store"},3200);
    await readJson(response,"Ping");httpRecovered();
  });
}

function queueHttp(work) {
  const pending=state.httpTail.then(work,work);
  state.httpTail=pending.catch(()=>{});
  return pending;
}
function httpRecovered() {
  state.lastContact=Date.now();state.httpFailures=0;state.retryAt=0;state.lastHttpError="";
  if(state.offlineLogged)log("Connection restored.","status");
  state.offlineLogged=false;
  setLive(true,"Connected");
}
function escapeHtml(value){return String(value).replace(/[&<>"']/g,char=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[char]))}
function commandCancelled(){const error=new Error("Queued command cancelled after Stop all or a connection change");error.name="CommandCancelled";return error}
function prefill(command) {
  elements.commandInput.value=command;
  elements.commandPreset.value=command;
  rememberInput();elements.commandInput.focus({preventScroll:true});
}
function rememberInput() {
  const value=elements.commandInput.value.slice(0,2048);
  // Do not persist Wi-Fi passwords in the browser's command draft.
  try {if(/WiFi(?:Ap|Sta)Password:/i.test(value))localStorage.removeItem("dispenser-command-draft");else localStorage.setItem("dispenser-command-draft",value)} catch(error) {}
  for(const button of elements.presetGroups.querySelectorAll("[data-fill]"))button.classList.toggle("selected",button.dataset.fill===value.trim());
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
  for(const [group,items] of ACTION_GROUPS){
    const optgroup=document.createElement("optgroup");optgroup.label=group;
    const section=document.createElement("section");section.className="presetGroup";
    const title=document.createElement("h3");title.textContent=group;
    const buttons=document.createElement("div");buttons.className="presetButtons";
    for(const [label,command,requires,confirm] of items){
      const option=document.createElement("option");option.value=command;option.textContent=label+" · "+command;
      const button=document.createElement("button");button.dataset.fill=command;button.title=command;
      const name=document.createElement("strong");name.textContent=label;
      const code=document.createElement("code");code.textContent=command;
      button.append(name,code);
      for(const item of [option,button]){if(requires)item.dataset.requires=requires;if(confirm)item.dataset.confirm="1"}
      optgroup.append(option);buttons.append(button);
    }
    elements.commandPreset.append(optgroup);section.append(title,buttons);elements.presetGroups.append(section);
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
  const editingOutputs = state.busy || routine.active || geo.active;
  elements.addOutput.disabled = editingOutputs || elements.outputRows.children.length >= (dispenser.outputsCapacity || 8);
  elements.singleOutput.disabled = editingOutputs;
  elements.saveOutputs.disabled = editingOutputs || dispenser.armed || dispenser.dispensing || addon.dispenser !== true;
  for (const input of elements.outputRows.querySelectorAll("input,select,button")) input.disabled = editingOutputs;
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
  syncStationPresets();
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

function card(label,value){return `<div class="status"><span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}</strong></div>`}

function renderState(data) {
  state.latest = data;
  state.lastContact = Date.now();
  if (state.cursor === null) state.cursor = 0;

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
    ? `${(dispenser.pins || [dispenser.pin]).map(pin => `GPIO${pin}`).join(", ")} · ${dispenser.dispensing ? `GPIO${dispenser.activePin ?? dispenser.pin} ACTIVE` : "outputs inactive"} · max pulse ${dispenser.maxPulseMs ? `${dispenser.maxPulseMs} ms` : "unlimited"} · pulse ${dispenser.remainingMs || 0} ms remaining · arm ${dispenser.armTimeoutMs ? `${dispenser.armRemainingMs || 0} ms remaining` : "no expiry"}${dispenser.interlockConfigured ? ` · interlock ${dispenser.interlockOpen ? "open" : "CLOSED"}` : ""}`
    : "This build uses the optional advanced stepper/DAC hardware profile.";
  initializeOutputs(dispenser);
  for (const input of [elements.pulseDuration, ...elements.outputRows.querySelectorAll(".outputPulse")]) {
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

async function postHttpNow(body,id,fast,epoch=state.commandGeneration) {
  const clean=String(body||"").replace(/\r/g,"").trim();if(!clean)throw new Error("Empty command");
  let last;
  const timeout=fast?3200:6500,delays=fast?[0]:[0,400,1000];
  const sentAt=performance.now(),relaySession=window.ESP32_REMOTE?.session,deviceSession=state.latest?.uplink?.session;
  state.httpQuietUntil=Date.now()+timeout+300;
  for(const delay of delays){
    if(delay)await sleep(delay);
    if(epoch!==state.commandGeneration)throw commandCancelled();
    try {
      const response=await fetchTimed(api("/api/command"),{method:"POST",headers:{"Content-Type":"application/octet-stream","Accept":"application/json","X-Request-ID":id,...(relaySession?{"X-Relay-Session":relaySession,"X-Device-Session":deviceSession||""}:{})},body:clean,keepalive:fast},timeout);
      const payload=await readJson(response,"Command");
      if(payload.accepted){
        state.lastCommandMs=Math.round(performance.now()-sentAt);
        // A queue acknowledgement is not an output cursor. Read execution events normally.
        state.httpQuietUntil=Date.now()+200;httpRecovered();return payload;
      }
      last=new Error(payload.error||"Command was not accepted");last.httpStatus=422;
      if(!payload.retryable)break;
    } catch(error){
      last=error;
      if(error.httpStatus&&!error.retryable)break;
    }
  }
  state.httpQuietUntil=Date.now()+200;
  if(epoch!==state.commandGeneration)throw commandCancelled();
  if(last?.httpStatus)throw last;
  throw new Error("Command acknowledgement unavailable. Check the output before sending it again.");
}

function postHttp(body,id=rid(),fast=false) {
  state.pollAbort?.abort();
  if(fast){
    // Stop bypasses a slow request; older queued commands cannot run afterward.
    // runCommand already advances the emergency-stop generation.
    const pending=postHttpNow(body,id,true,state.commandGeneration);
    state.httpTail=Promise.allSettled([state.httpTail,pending]);
    return pending;
  }
  const epoch=state.commandGeneration;
  return queueHttp(()=>{if(epoch!==state.commandGeneration)throw commandCancelled();return postHttpNow(body,id,false,epoch)});
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
  const text=event.t||"",age=Number.isFinite(event.ageMs)&&event.ageMs>=1500?` [${Math.floor(event.ageMs/1000)}s earlier]`:"";
  if(event.q){if(age)log(`Recovered raw output${age}`,"status");rawLog(text);return}
  const source=event.s||"Internal",request=event.r?` #${event.r}`:"";
  log(`${source}${request}${age}: ${text}`,String(event.l||"info").toLowerCase());
}
function consumeEvents(payload,recovered=false) {
  const current=state.latest?.uplink;
  if(window.ESP32_REMOTE&&((payload.relaySession&&payload.relaySession!==current?.relaySession)||(payload.deviceSession&&payload.deviceSession!==current?.session))){state.nextStateAt=0;return}
  if(payload.reset)state.cursor=0;
  const cursor=state.cursor??0;
  // The small local HTTP response omits per-event IDs and supplies a batch
  // cursor instead. The relay uses individual IDs for stream deduplication.
  const batchAdvanced=!Number.isInteger(payload.cursor)||payload.cursor>cursor||payload.reset;
  const fresh=(payload.events||[]).filter(event=>Number.isInteger(event.id)?event.id>cursor:batchAdvanced);
  if(payload.gap||fresh.some(event=>event.gap))log("Some earlier output is no longer available; showing retained messages.","warning");
  if(recovered&&fresh.length)log(`Recovered ${fresh.length} earlier message${fresh.length===1?"":"s"}.`,"status");
  for(const event of fresh){showEvent(event);if(Number.isInteger(event.id))state.cursor=Math.max(state.cursor??0,event.id)}
  if(Number.isInteger(payload.cursor))state.cursor=Math.max(state.cursor??0,payload.cursor);
}
function saveConsoleLog() {
  const file=new Blob([document.getElementById("log").textContent],{type:"text/plain;charset=utf-8"});
  const url=URL.createObjectURL(file),link=document.createElement("a");
  link.href=url;link.download=`controller-console-${new Date().toISOString().replace(/[:.]/g,"-")}.txt`;
  document.body.append(link);link.click();link.remove();setTimeout(()=>URL.revokeObjectURL(url),1000);
}


async function pollHttpOnce() {
  const time=Date.now();
  if(state.busy||state.otaActive||usingBle()||document.hidden||time<state.httpQuietUntil)return 300;
  if(time<state.retryAt)return Math.min(1000,state.retryAt-time);
  const getState=!state.latest||time>=state.nextStateAt;
  try {
    const payload=await queueHttp(async()=>{
      if(state.busy||state.otaActive||usingBle())return null;
      const path=getState?"/api/state":`/api/events?since=${state.cursor??0}&limit=4`;
      const response=await fetchTimed(api(path),{cache:"no-store"},coexHttpMode()?5200:4200,true);
      return readJson(response,getState?"State":"Events");
    });
    if(!payload)return 300;
    httpRecovered();
    if(getState){renderState(payload);state.nextStateAt=Date.now()+1800}
    else {
      consumeEvents(payload);
    }
    return payload.more?180:650;
  } catch(error){
    // A command/BLE/OTA transition cancels background reads intentionally.
    if(error.name==="AbortError")return 300;
    state.lastHttpError=error.message||String(error);
    state.httpFailures++;
    if(error.httpStatus&&!error.retryable){
      setLive(false,error.httpStatus===401||error.httpStatus===403?"Access denied":"API unavailable");
      if(!state.offlineLogged)log(state.lastHttpError,"error");
      state.offlineLogged=true;state.retryAt=Date.now()+5000;return 1000;
    }
    if(state.httpFailures>=3&&Date.now()-state.lastContact>=8000)down(error);
    else setLive(false,"Reconnecting…","retrying");
    state.retryAt=Date.now()+Math.min(4000,500*2**Math.min(state.httpFailures,3));
    return 500;
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

async function withLock(work){
  if(state.busy)return;state.busy=true;state.pollAbort?.abort();renderTransport();
  try {await work()} catch(error){log(error.message||String(error),error.name==="CommandCancelled"?"warning":"error")}
  finally {state.busy=false;renderTransport()}
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
  rememberInput();
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
  const outputs = readOutputs(maxPulse);
  const configured = state.latest?.dispenser?.pins || [state.latest?.dispenser?.pin ?? 26];
  if (outputs.some(row => !configured.includes(row.pin)) || outputs[0].pin !== configured[0]) throw new Error("Disarm and Save output pins before building this routine");
  const repeats = elements.routineContinuous.checked ? "FOREVER" : readInteger(elements.routineRepeats, 1, 4294967295, "Repeats");
  const commands = [
    `RoutineCreate:${name}`,
    `RoutineAdd:${name}:START_WAIT:${delay}`,
    ...(outputs.length === 1 ? [
      `RoutineAdd:${name}:DISPENSE:${outputs[0].pulse}`, `RoutineAdd:${name}:WAIT_IDLE`, `RoutineAdd:${name}:WAIT:${outputs[0].gap}`
    ] : outputs.map(row => `RoutineAdd:${name}:OUTPUT:${row.pin},${row.pulse},${row.gap}`)),
    `RoutineRepeat:${name}:${repeats}`,
    `RoutineSave:${name}`
  ];
  await commandBatches(commands);
}
const DEFAULT_OUTPUT_PINS = [13,14,16,17,18,19,21,22,23,25,26,27,32,33];
function pinChoices(select, pin) {
  select.replaceChildren();
  for (const value of state.latest?.dispenser?.availablePins || DEFAULT_OUTPUT_PINS) {
    const option = document.createElement("option"); option.value = value; option.textContent = `GPIO${value}`; select.append(option);
  }
  select.value = String(pin);
}
function addOutputRow(pin) {
  if (elements.outputRows.children.length >= (state.latest?.dispenser?.outputsCapacity || 8)) throw new Error("Output capacity reached");
  const row = document.createElement("div"); row.className = "outputRow";
  row.innerHTML = '<label>Output pin<select class="outputPin"></select></label><label>Dispense ms<input class="outputPulse" type="number" min="1" value="200"></label><label>Delay after ms<input class="outputGap" type="number" min="0" value="800"></label><button class="danger">Remove</button>';
  pinChoices(row.querySelector("select"), pin);
  row.querySelector("button").addEventListener("click", () => { row.remove(); elements.outputRows.dataset.edited = "true"; updateActions(); });
  elements.outputRows.append(row);
}
function initializeOutputs(dispenser) {
  if (!dispenser || !state.latest?.addon?.dispenser || elements.outputRows.dataset.initialized || elements.outputRows.dataset.edited) return;
  const pins = dispenser.pins || [dispenser.pin ?? 26];
  pinChoices(elements.outputPin, pins[0]);
  for (const pin of pins.slice(1)) addOutputRow(pin);
  elements.outputRows.dataset.initialized = "true";
}
function readOutputs(maxPulse = Number(state.latest?.dispenser?.maxPulseMs) || 4294967295) {
  const rows = [...elements.outputRows.children].map(row => ({
    pin: readInteger(row.querySelector("select"), 0, 48, "Output pin"),
    pulse: readInteger(row.querySelector(".outputPulse"), 1, maxPulse, "Dispense time"),
    gap: readInteger(row.querySelector(".outputGap"), 0, 4294967295, "Delay after")
  }));
  const available = state.latest?.dispenser?.availablePins || DEFAULT_OUTPUT_PINS;
  if (rows.some(row => !available.includes(row.pin)) || new Set(rows.map(row => row.pin)).size !== rows.length) throw new Error("Choose a different available pin for each output");
  return rows;
}
async function commandBatches(commands) {
  const generation = state.commandGeneration;
  for (let i = 0; i < commands.length; i += 7) {
    if (generation !== state.commandGeneration) throw new Error("Save cancelled by stop");
    await runCommand(commands.slice(i, i + 7).join("\n"));
    if (i + 7 < commands.length) await sleep(250);
  }
}
async function saveOutputPins() {
  const rows = readOutputs();
  await runCommand(`DispenserOutputs:${rows.map(row => row.pin).join(",")}\nDispenserSave`);
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
  for (const [name, delay, pulse, gap, repeats, saved, steps, outputs] of library) {
    const row = document.createElement("div"); row.className = "savedRoutine";
    const label = document.createElement("div"); label.textContent = `${name}${saved ? "" : " (unsaved)"}`;
    const details = document.createElement("small");
    const repetition = repeats ? `${repeats} repeats` : "until stopped";
    details.textContent = outputs ? `${outputs} outputs in order · initial delay ${delay} ms · ${repetition}` : pulse ? `Delay ${delay} ms · on ${pulse} ms / off ${gap} ms · ${repetition}` : `${steps} steps · ${repetition}`;
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
  const fill = event.target.closest("button[data-fill]");
  if(fill){prefill(fill.dataset.fill);return}
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
  if (option?.value) { prefill(option.value); }
});
elements.commandInput.addEventListener("input", () => {
  rememberInput();
  if (elements.commandPreset.value !== elements.commandInput.value.trim()) elements.commandPreset.selectedIndex = 0;
});
elements.commandInput.addEventListener("keydown", event => {
  if (event.ctrlKey && event.key === "Enter") { event.preventDefault(); sendInput(); }
});
elements.sendCommands.addEventListener("click", sendInput);
elements.stopAll.addEventListener("click", () => runCommand("StopAll", true).catch(error => log(error.message, "error")));
elements.dispensePulse.addEventListener("click", () => withLock(customDispense));
elements.saveRoutine.addEventListener("click", () => withLock(saveRoutinePreset));
elements.outputRows.addEventListener("input", () => { elements.outputRows.dataset.edited = "true"; });
elements.addOutput.addEventListener("click", () => {
  const used = [...elements.outputRows.querySelectorAll("select")].map(select => Number(select.value));
  const pin = (state.latest?.dispenser?.availablePins || DEFAULT_OUTPUT_PINS).find(value => !used.includes(value));
  if (pin === undefined) { log("No unused output pins available", "error"); return; }
  addOutputRow(pin); elements.outputRows.dataset.edited = "true"; updateActions();
});
elements.singleOutput.addEventListener("click", () => {
  for (const row of [...elements.outputRows.children].slice(1)) row.remove();
  pinChoices(elements.outputPin, state.latest?.dispenser?.defaultPin ?? 26);
  elements.outputRows.dataset.edited = "true"; updateActions();
});
elements.saveOutputs.addEventListener("click", () => withLock(saveOutputPins));
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
elements.clearLog.addEventListener("click", () => { elements.log.textContent = ""; state.logLines=0; elements.logCount.textContent="0 lines"; });
elements.otaUpload.addEventListener("click", () => withLock(uploadFirmware));

// Enabled only by the authenticated remote host's configuration script.
function installRemoteConsole() {
  if(!window.ESP32_REMOTE)return;
  let socket=null,reconnect=null,connectTimer=null,lastFrame=0,openedAt=0,stateAt=Date.now(),session=null;
  let relaySession=window.ESP32_REMOTE.session||null;
  const baseRender=renderState,baseTransport=renderTransport,baseRecovered=httpRecovered;
  const basePoll=pollHttpOnce,baseCommand=runCommand;
  const baseApi=api;
  api=function(path){
    if(!path.startsWith("/api/events?"))return baseApi(path);
    const url=new URL(baseApi(path),location.href);
    url.searchParams.set("deviceSession",state.latest?.uplink?.session||"");
    url.searchParams.set("relaySession",window.ESP32_REMOTE.session||"");url.searchParams.set("limit","16");
    return url.href;
  };
  const metrics=document.createElement("div");metrics.className="connectionMetrics";metrics.setAttribute("aria-label","Remote connection details");
  const streamInfo=document.createElement("span"),ageInfo=document.createElement("span"),ackInfo=document.createElement("span");
  ackInfo.title="Time to controller queue acknowledgement; execution results appear in Output.";
  metrics.append(streamInfo,ageInfo,ackInfo);document.querySelector(".transportLine").after(metrics);
  const connection=document.createElement("details");connection.className="connectionDetails";
  const summary=document.createElement("summary"),summaryTitle=document.createElement("span"),summaryState=document.createElement("small");
  summaryTitle.textContent="Connection";summary.append(summaryTitle,summaryState);
  const connectionBody=document.createElement("div");connectionBody.className="connectionBody";
  const connectionRows={};
  for(const [name,label] of [["browser","This browser"],["broker","Relay to broker"],["device","Controller"]]){
    const row=document.createElement("div"),title=document.createElement("span"),value=document.createElement("strong");
    row.className="connectionRow";title.textContent=label;value.textContent="Waiting for connection";row.append(title,value);connectionRows[name]=value;connectionBody.append(row);
  }
  const connectionNote=document.createElement("p");connectionNote.className="hint";connectionNote.textContent="Checks read connection status only. Commands are never resent by this button.";
  const checkButton=document.createElement("button");checkButton.type="button";checkButton.textContent="Check connection";
  const checkResult=document.createElement("span");checkResult.className="hint";checkResult.setAttribute("role","status");
  const checkActions=document.createElement("div");checkActions.className="connectionActions";checkActions.append(checkButton,checkResult);
  connectionBody.append(connectionNote,checkActions);connection.append(summary,connectionBody);metrics.after(connection);
  let checking=false,lastDiagnosticScope=null;
  function connectionScope(){const link=state.latest?.uplink;return `${link?.relaySession||""}/${link?.session||""}/${link?.brokerOnline??""}/${link?.reason||""}`}
  async function checkConnection(){
    if(checking)return;
    if(state.busy||state.otaActive){checkResult.textContent="Finish the current command, then check again.";return}
    checking=true;checkButton.disabled=true;checkResult.textContent="Checking…";
    const scope=connectionScope();
    try{
      const health=await queueHttp(async()=>{
        const response=await fetchTimed(api("/api/health"),{cache:"no-store"},4200,true);
        return readJson(response,"Connection check");
      });
      if(scope!==connectionScope()){checkResult.textContent="Connection changed; check again.";return}
      if(health?.relay?.http===undefined||health?.broker?.phase===undefined)throw new Error("Incomplete connection report");
      connectionRows.broker.textContent=String(health.broker.message||health.broker.phase).slice(0,220);
      checkResult.textContent=health.ok===true?`Checked ${now()}.` : "Console files need attention on the host.";
    }catch(error){
      checkResult.textContent=error.httpStatus===401||error.httpStatus===403?"Sign-in expired or access denied. Reload to sign in again.":error.name==="AbortError"?"Check paused for a command. Try again when ready.":"Connection check unavailable. Automatic reconnect is still running.";
      connectionRows.broker.textContent="No current broker report";
    }finally{checking=false;checkButton.disabled=false}
  }
  checkButton.addEventListener("click",checkConnection);
  connection.addEventListener("toggle",()=>{if(connection.open)checkConnection()});
  function renderConnectionDetails({reachable,online,age,stale}){
    const uplink=state.latest?.uplink;
    summaryState.textContent=!reachable?"Relay unreachable":!online?"Device offline":age===null?"Waiting for state":stale?"State is old":"Connected";
    connectionRows.browser.textContent=streamLive()?"Live stream connected":reachable?"Connected using HTTP fallback":"Relay unreachable; reconnecting automatically";
    connectionRows.device.textContent=!online?(uplink?.reason||"Offline; waiting for the controller"):age===null?"Connected; waiting for current device state":`${uplink.stopPending?"Stop awaiting confirmation · ":""}State ${age}s old${stale?" · cached reading":""}`;
    if(lastDiagnosticScope!==connectionScope()){
      lastDiagnosticScope=connectionScope();connectionRows.broker.textContent=uplink?.brokerOnline===true?"Connected; open this panel to check details":uplink?.brokerOnline===false?"Relay is reconnecting to the broker":"Open this panel to check broker status";
      if(!checking)checkResult.textContent="";
    }
  }

  function relayAlive(){return Date.now()-Math.max(lastFrame,state.lastContact||0)<12000}
  function priorityStop(body){return /^(StopAll|Disarm|DispenserDisarm|DispenserOff|RoutineStop|GeoStop|DispenseStop|GPIO26:OFF)$/i.test(String(body).trim())}
  function streamLive(){return socket?.readyState===1&&lastFrame>0&&Date.now()-lastFrame<12000}
  function connectionLabel(){
    const uplink=state.latest?.uplink;
    const reachable=relayAlive(),online=reachable&&uplink?.online===true;
    const age=Number.isFinite(uplink?.ageMs)?Math.max(0,Math.floor((uplink.ageMs+Date.now()-stateAt)/1000)):null;
    const stale=uplink?.stale||age===null||age>5;
    const link=uplink?.transport==="WiFi"?"Wi-Fi":"LTE";
    setLive(online,online?(age===null?"Waiting for device state":stale?`${link} connected · state ${age}s old`:`${link} connected`):reachable?(uplink?.reason||"Device offline"):"Relay unavailable",online&&stale?"retrying":undefined);
    elements.transportSummary.textContent=`Remote ${link}`;
    streamInfo.textContent=uplink?.stopPending?"Stop awaiting confirmation":streamLive()?"● Live stream":reachable?"↻ HTTP fallback":"Relay unreachable";
    ageInfo.textContent=age===null?"Waiting for device state":`State ${age}s old`;
    ageInfo.classList.toggle("stale",stale);
    ackInfo.textContent=Number.isFinite(state.lastCommandMs)?`Last ack ${state.lastCommandMs} ms`:"Last ack —";
    renderConnectionDetails({reachable,online,age,stale});
  }
  renderTransport=function(){
    baseTransport();
    elements.connectBle.disabled=elements.disconnectBle.disabled=true;
    elements.connectBle.title="Connect to the controller's local console to use BLE";
    elements.otaUpload.disabled=true;
    elements.otaMessage.textContent="For firmware updates, connect locally over Wi-Fi or USB.";
    const offline=!relayAlive()||state.latest?.uplink?.online!==true;
    const stopping=state.latest?.uplink?.stopPending===true;
    const waiting=!Number.isFinite(state.latest?.uplink?.ageMs);
    elements.sendCommands.disabled=state.busy||offline||stopping||waiting;
    for(const button of document.querySelectorAll("[data-command]"))if(offline||((stopping||waiting)&&!priorityStop(button.dataset.command)))button.disabled=true;
    if(offline||stopping||waiting)for(const id of ["dispensePulse","runRoutine","startGeo","saveRoutine","saveOutputs","saveGeo","saveWifi","useHourLimit"]){const button=document.getElementById(id);if(button)button.disabled=true}
    if(waiting&&document.getElementById("payloadState")){
      elements.payloadState.textContent="UNKNOWN";elements.payloadState.className="stateBadge";
      elements.payloadMeta.textContent="Waiting for current controller state. Stop all remains available when connected.";
      elements.statusSummary.textContent="Waiting for current controller state";
      elements.statusGrid.replaceChildren();
      elements.routineSummary.textContent="Waiting for current routine state";
      elements.geoStatus.textContent="Waiting for current position state";
    }
    connectionLabel();
  };
  renderState=function(data){
    if(!data?.uplink)return;
    const previous=state.latest?.uplink;
    if(previous?.stopPending&&data.uplink.stopPending===false&&previous.session===data.uplink.session&&previous.relaySession===data.uplink.relaySession)log("Stop acknowledged by the controller queue. Check Output for the result.","status");
    stateAt=Date.now();
    const nextRelay=data.uplink.relaySession;
    if(nextRelay&&nextRelay!==relaySession){
      if(relaySession){
        if("commandGeneration" in state)state.commandGeneration++;else state.commandEpoch++;
        log("Relay restarted. Queued commands cancelled; review device state before sending again.","warning");
      }
      relaySession=nextRelay;window.ESP32_REMOTE.session=nextRelay;state.cursor=null;
    }
    if(data.uplink.session!==session){
      if(session){
        if("commandGeneration" in state)state.commandGeneration++;else state.commandEpoch++;
        log("Device reconnected. Older queued commands cancelled; resuming available output.","status");
      }
      session=data.uplink.session;state.cursor=0;
    }
    // Until the first full snapshot, the relay may only know device presence.
    if(data.dispenser===undefined&&document.getElementById("payloadState")){
      state.latest={uplink:data.uplink};renderTransport();return;
    }
    const cursor=state.cursor;
    baseRender(data);state.cursor=cursor??0;renderTransport();
  };
  httpRecovered=function(){baseRecovered();connectionLabel()};
  runCommand=async function(body,fast=false){
    if(!relayAlive()||state.latest?.uplink?.online!==true)throw new Error("Device is offline. Commands are not queued.");
    if(!Number.isFinite(state.latest.uplink.ageMs)&&!priorityStop(body))throw new Error("Waiting for current device state. Commands are not queued.");
    if(state.latest.uplink.stopPending&&!priorityStop(body))throw new Error("Waiting for Stop confirmation. Use Stop all again to retry.");
    return baseCommand(body,fast);
  };
  pollHttpOnce=async function(){
    connectionLabel();
    if(streamLive())return 1000;
    if(socket?.readyState===1&&Date.now()-Math.max(lastFrame,openedAt)>12000)socket.close();
    return basePoll();
  };
  function connect(){
    clearTimeout(reconnect);
    if(document.hidden||socket?.readyState===0||socket?.readyState===1)return;
    const url=new URL(window.ESP32_REMOTE.wsPath||"/ws",location.href);url.protocol=location.protocol==="https:"?"wss:":"ws:";
    if(Number.isInteger(state.cursor))url.searchParams.set("since",String(state.cursor));
    url.searchParams.set("deviceSession",state.latest?.uplink?.session||"");
    url.searchParams.set("relaySession",window.ESP32_REMOTE.session||"");
    const current=new WebSocket(url);socket=current;openedAt=Date.now();lastFrame=0;
    connectTimer=setTimeout(()=>{if(socket===current&&current.readyState===0)current.close()},6000);
    current.onmessage=event=>{
      if(socket!==current)return;
      try{
        const frame=JSON.parse(event.data);
        if(!["state","event","sync"].includes(frame.type))return;
        lastFrame=Date.now();
        if(frame.type==="state")renderState(frame.data);
        else if(frame.type==="sync"){renderState(frame.data.state);consumeEvents(frame.data.history,true)}
        else consumeEvents({events:[frame.data],deviceSession:frame.data.deviceSession,relaySession:frame.data.relaySession});
      } catch(error){log("Remote stream data was incomplete; refreshing state.","warning");state.nextStateAt=0}
    };
    current.onclose=()=>{if(socket!==current)return;clearTimeout(connectTimer);socket=null;state.nextStateAt=0;connectionLabel();if(!document.hidden)reconnect=setTimeout(connect,2000)};
    current.onerror=()=>current.close();
  }
  document.addEventListener("visibilitychange",()=>{if(document.hidden){clearTimeout(reconnect);clearTimeout(connectTimer);socket?.close()}else connect()});
  window.addEventListener("pagehide",()=>{clearTimeout(reconnect);clearTimeout(connectTimer);socket?.close()});
  connect();
}


function installMobileDock() {
  const dock=document.createElement("nav");dock.className="mobileDock";dock.setAttribute("aria-label","Mobile controller shortcuts");
  const consoleLink=document.createElement("a"),controlsLink=document.createElement("a"),stop=document.createElement("button");
  consoleLink.href="#commandConsole";consoleLink.textContent="Console";
  controlsLink.href="#presets";controlsLink.textContent="Controls";
  stop.id="mobileStop";stop.type="button";stop.className="danger";stop.textContent="Stop all";stop.setAttribute("aria-label","Stop all outputs");
  stop.addEventListener("click",()=>document.getElementById("stopAll").click());
  dock.append(consoleLink,controlsLink,stop);document.body.append(dock);
}


installMobileDock();
document.getElementById("saveLog").addEventListener("click",saveConsoleLog);
installRemoteConsole();
function installPresetSearch() {
  const input=document.getElementById("presetSearch"),groups=document.getElementById("presetGroups");
  const count=document.getElementById("presetCount"),empty=document.getElementById("presetEmpty");
  if(!input||!groups)return;
  function filter(){
    const terms=input.value.toLowerCase().trim().split(/\s+/).filter(Boolean);
    let visible=0,supported=0;
    for(const group of groups.children){
      const title=group.querySelector("h3")?.textContent||"";
      for(const button of group.querySelectorAll("[data-fill]")){
        const unavailable=typeof compiled==="function"&&["spp","stepper","dac"].includes(button.dataset.requires)&&!compiled(button.dataset.requires);
        const words=`${title} ${button.textContent} ${button.dataset.fill}`.toLowerCase();
        button.hidden=unavailable||!terms.every(term=>words.includes(term));
        if(!unavailable)supported++;
        if(!button.hidden)visible++;
      }
      group.hidden=[...group.querySelectorAll("[data-fill]")].every(button=>button.hidden);
    }
    count.textContent=terms.length?`${visible} matching preset${visible===1?"":"s"}`:`${supported} presets`;
    empty.hidden=visible>0;
  }
  input.addEventListener("input",filter);
  input.addEventListener("keydown",event=>{if(event.key==="Escape"){event.preventDefault();input.value="";filter()}});
  // Capability updates must not undo the active filter. Preserve each variant's
  // existing disabled-state and hardware checks before filtering visibility.
  if(typeof syncStationPresets==="function"){
    const original=syncStationPresets;syncStationPresets=function(){original();filter()};
  }else if(typeof updateActions==="function"){
    const original=updateActions;updateActions=function(){original();filter()};
  }else if(typeof updateButtons==="function"){
    const original=updateButtons;updateButtons=function(){original();filter()};
  }
  filter();
}


buildCommandMenu();
installPresetSearch();
try {prefill(localStorage.getItem("dispenser-command-draft")||"DispenserStatus")} catch(error){prefill("DispenserStatus")}
renderTransport();
log("Controller console ready", "status");
pollLoop();

function syncStationPresets(){
    for(const button of elements.presetGroups.querySelectorAll('[data-fill]')){
      const option=[...elements.commandPreset.options].find(item=>item.value===button.dataset.fill);
      button.disabled=option?.disabled??false;
      button.hidden=['stepper','dac','spp'].includes(button.dataset.requires)&&!compiled(button.dataset.requires);
    }
    for(const group of elements.presetGroups.children)group.hidden=[...group.querySelectorAll('[data-fill]')].every(button=>button.hidden);
  }
