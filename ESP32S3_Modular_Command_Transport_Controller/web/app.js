/* WEB UI EXTENSION MAP: add tutorial commands in ACTION_GROUPS and status cards in renderState(). */
"use strict";

const BLE_SERVICE_UUID="6e400001-b5a3-f393-e0a9-e50e24dcca9e",BLE_RX_UUID="6e400002-b5a3-f393-e0a9-e50e24dcca9e",BLE_TX_UUID="6e400003-b5a3-f393-e0a9-e50e24dcca9e",BLE_CHUNK=20;
const API_BASE=String(window.ESP32_API_BASE||"").replace(/\/+$/,""),enc=new TextEncoder(),dec=new TextDecoder(),elements={};
for(const id of ["buildVersion","liveDot","liveText","connectBle","disconnectBle","transportSummary","compiledSummary","commandPreset","commandInput","sendCommands","stopAll","clearLog","log","statusSummary","statusGrid","selfTestSummary","otaFile","otaUpload","otaProgress","otaMessage","presetGroups","followLog","logCount"])elements[id]=document.getElementById(id);

const state={transport:"http",latest:null,cursor:null,lastContact:0,busy:false,otaActive:false,bleDevice:null,bleRx:null,bleTx:null,bleBuffer:"",bleStatePending:false,bleStateAt:0,httpQuietUntil:0,nextStateAt:0,retryAt:0,httpFailures:0,httpTail:Promise.resolve(),pollAbort:null,commandEpoch:0,offlineLogged:false,lastHttpError:"",logLines:0};
const ACTION_GROUPS=[
  ["Core",[["Ping","Ping"],["Status","Status"],["Read config","ConfigRead"],["Help","Help"],["Indicators","IndicatorTest"]]],
  ["Send / bridge",[["Send","Send:hello from controller"],["Send to BLE","SendBLE:hello from Wi-Fi","ble-output"],["Send Wi-Fi","SendWiFi:hello from BLE","wifi-output"],["Send USB","SendSerial:hello from controller"],["Send status","SendStatus"]]],
  ["Radio profiles",[["Wi-Fi","ModeWiFi","wifi",1],["Wi-Fi + BLE","ModeWiFiBLE","wifi+ble",1],["Wi-Fi + BLE P","ModeWiFiBLEP","wifi+ble",1],["BLE only","ModeBLE","ble",1],["BT serial","ModeBTSerial","spp",1],["USB only","ModeUSB",null,1],["Radio status","RadioStatus"]]],
  ["Wi-Fi LR",[["Enable LR (ESP32 peers only)","WiFiLR:ON","wifi",1],["Use ordinary Wi-Fi","WiFiLR:OFF","wifi"],["Apply Wi-Fi settings","ConfigApply","wifi",1],["Save radio settings","ConfigSave"]]],
  ["Self-test",[["Start sweep","SelfTestStart",null,1],["Print status","SelfTestStatus"],["Abort","SelfTestAbort",null,1],["Clear report","SelfTestClear"]]],
  ["Debug",[["Debug boot tests","DebugMode"],["Production boot","ProductionMode"],["Load saved config","ConfigLoad"],["Clear STA","WiFiStaClear","wifi",1]]]
];

function api(path){return API_BASE+path}function sleep(ms){return new Promise(resolve=>setTimeout(resolve,ms))}function now(){return new Date().toLocaleTimeString()}function usingBle(){return state.transport==="ble"&&state.bleRx&&state.bleTx}
function setLive(ok,text,kind="") {
  elements.liveDot.classList.toggle("on",ok);
  elements.liveDot.classList.toggle("retrying",kind==="retrying");
  elements.liveDot.classList.toggle("offline",!ok&&kind!=="retrying");
  elements.liveText.textContent=text;
  elements.liveText.title=state.lastHttpError||text;
}
function redact(line){return /^(WiFi(?:Ap|Sta)Password:)/i.test(line)?line.split(":",1)[0]+":<redacted>":line}
function appendLog(line) {
  const nearBottom=elements.log.scrollHeight-elements.log.scrollTop-elements.log.clientHeight<40;
  elements.log.textContent+=line+"\n";
  if(elements.log.textContent.length>32000)elements.log.textContent=elements.log.textContent.slice(-24000);
  state.logLines=elements.log.textContent.split("\n").length-1;
  elements.logCount.textContent=`${state.logLines} lines`;
  if(elements.followLog.checked&&nearBottom)elements.log.scrollTop=elements.log.scrollHeight;
}
function log(message,level="info"){appendLog(`[${now()}] ${level.toUpperCase()} ${message}`)}
function rawLog(message){appendLog(String(message))}
function rid(){return `web-${Date.now().toString(36)}-${Math.random().toString(36).slice(2,7)}`}
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
async function pingHttp() {
  return queueHttp(async()=>{
    const response=await fetchTimed(api("/api/ping"),{cache:"no-store"},3200);
    await readJson(response,"Ping");httpRecovered();
  });
}
function compiled(requirement){const radio=state.latest?.radio||{},send=state.latest?.send||{};if(!requirement)return true;if(requirement==="wifi")return radio.wifiCompiled!==false;if(requirement==="wifi-output")return send.wifi===true;if(requirement==="ble")return radio.bleCompiled!==false;if(requirement==="ble-output")return send.ble===true;if(requirement==="spp")return radio.sppCompiled!==false;return radio.wifiCompiled!==false&&radio.bleCompiled!==false}
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
function rememberInput() {
  const value=elements.commandInput.value.slice(0,2048);
  // Do not persist Wi-Fi passwords in the browser's command draft.
  try {if(/WiFi(?:Ap|Sta)Password:/i.test(value))localStorage.removeItem("s3-command-draft");else localStorage.setItem("s3-command-draft",value)} catch(error) {}
  for(const button of elements.presetGroups.querySelectorAll("[data-fill]"))button.classList.toggle("selected",button.dataset.fill===value.trim());
}
function prefill(command) {
  elements.commandInput.value=command;
  elements.commandPreset.value=command;
  rememberInput();elements.commandInput.focus({preventScroll:true});
}
function updateActions() {
  for(const item of document.querySelectorAll("[data-command],[data-fill],option[data-requires]")){
    const supported=compiled(item.dataset.requires);
    item.disabled=state.busy||!supported;
    item.hidden=item.dataset.requires==="spp"&&!supported;
  }
  if(elements.commandPreset.selectedOptions[0]?.disabled)elements.commandPreset.selectedIndex=0;
}
function renderTransport(){const ble=usingBle();elements.transportSummary.textContent=ble?"BLE transport":"Wi-Fi transport";elements.connectBle.disabled=state.busy||ble||!navigator.bluetooth||state.latest?.radio?.bleCompiled===false;elements.disconnectBle.disabled=state.busy||!ble;elements.otaUpload.disabled=state.busy||state.otaActive;elements.sendCommands.disabled=state.busy;updateActions()}
function escapeHtml(value){return String(value).replace(/[&<>"']/g,char=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[char]))}
function card(label,value){return `<div class="status"><span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}</strong></div>`}
function renderState(data){state.latest=data;state.lastContact=Date.now();if(state.cursor===null)state.cursor=0;const radio=data.radio||{},send=data.send||{},queue=data.queue||{},test=data.selfTest||{};const wifi=radio.wifiCompiled===false?"not compiled":`${radio.wifiLRActive?"LR · ":""}${radio.wifiState||"off"} ${radio.ip||""}`.trim(),ble=radio.bleCompiled===false?"not compiled":send.ble?"connected + TX":send.bleConnected?"connected, notifications off":radio.bleRunning?"advertising":"off",spp=radio.sppCompiled===false?"not compiled":radio.sppConnected?"connected":radio.sppRunning?"running":"off",addon=data.addon?.name||"none",profile=radio.bootModeActive||data.bootMode||"?",indicators=data.indicators?.enabled===false?"Off":data.indicators?.rgb?"Onboard RGB":"Status LEDs";elements.statusGrid.innerHTML=[card("Boot",profile),card("Addon",addon),card("Wi-Fi",wifi),card("BLE",ble),card("SPP",spp),card("Indicators",indicators),card("Queue",`${queue.waiting??0}/${queue.capacity??0}`),card("Heap",`${data.freeHeap??0} B free · ${data.largestHeapBlock??0} B largest`),card("Self-test",test.active?`${test.current||0}/${test.total||0}`:test.phase||"idle")].join("");elements.statusSummary.textContent=`${profile} · ${addon} · Wi-Fi ${wifi} · BLE ${ble}`;elements.buildVersion.textContent=`${data.firmware||"ESP32 controller"} · ${data.version||"unknown build"}`;elements.compiledSummary.textContent=`Compiled: Wi-Fi ${radio.wifiCompiled===false?"off":"on"}, BLE ${radio.bleCompiled===false?"off":"on"}, SPP ${radio.sppCompiled===false?"off":"on"}`;elements.selfTestSummary.textContent=`${test.phase||"Idle"}: ${test.current||0}/${test.total||0}, pass ${test.pass||0}, fail ${test.fail||0}, skip ${test.skip||0}, boots ${test.boots||0}. ${test.lastResult||"No run recorded."}`;setLive(true,usingBle()?"BLE connected":"Connected");renderTransport()}
function commandCancelled(){const error=new Error("Queued command cancelled after Stop all or a connection change");error.name="CommandCancelled";return error}
async function postHttpNow(body,id,fast,epoch=state.commandEpoch) {
  const clean=String(body||"").replace(/\r/g,"").trim();if(!clean)throw new Error("Empty command");
  let last;
  const timeout=fast?3200:6500,delays=fast?[0]:[0,400,1000];
  const sentAt=performance.now(),relaySession=window.ESP32_REMOTE?.session,deviceSession=state.latest?.uplink?.session;
  state.httpQuietUntil=Date.now()+timeout+300;
  for(const delay of delays){
    if(delay)await sleep(delay);
    if(epoch!==state.commandEpoch)throw commandCancelled();
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
  if(epoch!==state.commandEpoch)throw commandCancelled();
  if(last?.httpStatus)throw last;
  throw new Error("Command acknowledgement unavailable. Check the output before sending it again.");
}
function postHttp(body,id=rid(),fast=false) {
  state.pollAbort?.abort();
  if(fast){
    // Stop bypasses a slow request; older queued commands cannot run afterward.
    state.commandEpoch++;
    const pending=postHttpNow(body,id,true,state.commandEpoch);
    state.httpTail=Promise.allSettled([state.httpTail,pending]);
    return pending;
  }
  const epoch=state.commandEpoch;
  return queueHttp(()=>{if(epoch!==state.commandEpoch)throw commandCancelled();return postHttpNow(body,id,false,epoch)});
}
async function writeBle(text){if(!usingBle())throw new Error("BLE disconnected");const bytes=enc.encode(text);for(let index=0;index<bytes.length;index+=BLE_CHUNK){const chunk=bytes.slice(index,index+BLE_CHUNK);if(state.bleRx.writeValueWithoutResponse)await state.bleRx.writeValueWithoutResponse(chunk);else await state.bleRx.writeValue(chunk);await sleep(14)}}
async function runCommand(body,fast=false){const clean=String(body||"").trim();if(!clean)throw new Error("Empty command");log(`TX ${usingBle()?"BLE":"HTTP"} ${clean.split("\n").map(redact).join(" | ")}`,"send");if(usingBle()){await writeBle(clean+"\n");return}const result=await postHttp(clean,rid(),fast);log(`HTTP accepted ${result.acceptedLines||1} line${result.acceptedLines===1?"":"s"}`,"status");return result}
function handleBleLine(line){if(!line)return;state.lastContact=Date.now();if(line.startsWith("@STATE ")){state.bleStatePending=false;state.bleStateAt=0;try{renderState(JSON.parse(line.slice(7)))}catch(error){log(`BLE state parse: ${error.message}`,"error")}return}const match=line.match(/^\[(\d+)\]\[([^\]]+)\]\[([^\]]+)\](?:\[([^\]]+)\])?\s*(.*)$/);if(match){state.cursor=Math.max(state.cursor??0,Number(match[1])||0);log(`${match[3]}${match[4]?` #${match[4]}`:""}: ${match[5]}`,match[2].toLowerCase());return}line.startsWith("@ERROR")?log(line,"error"):rawLog(line)}
function handleBleData(event){state.bleBuffer+=dec.decode(event.target.value,{stream:true});let at;while((at=state.bleBuffer.indexOf("\n"))>=0){handleBleLine(state.bleBuffer.slice(0,at).replace(/\r$/,"").trim());state.bleBuffer=state.bleBuffer.slice(at+1)}}
function bleDisconnected(){const wasBle=state.transport==="ble"||state.bleRx||state.bleTx;state.transport="http";state.bleRx=null;state.bleTx=null;state.bleBuffer="";state.nextStateAt=0;setLive(false,"BLE disconnected");renderTransport();if(wasBle)log("BLE disconnected; Wi-Fi resumed","warning")}
async function connectBle(){if(!navigator.bluetooth)throw new Error("Web Bluetooth is unavailable");const arm=Date.now()-state.lastContact<7000?postHttp("BLEWebHandoff","ble-arm-"+Date.now()).catch(()=>{}):Promise.resolve(),devicePromise=navigator.bluetooth.requestDevice({filters:[{services:[BLE_SERVICE_UUID]}],optionalServices:[BLE_SERVICE_UUID]}),[,device]=await Promise.all([arm,devicePromise]);device.addEventListener("gattserverdisconnected",bleDisconnected);const server=await device.gatt.connect(),service=await server.getPrimaryService(BLE_SERVICE_UUID);state.bleRx=await service.getCharacteristic(BLE_RX_UUID);state.bleTx=await service.getCharacteristic(BLE_TX_UUID);await state.bleTx.startNotifications();state.bleTx.addEventListener("characteristicvaluechanged",handleBleData);state.bleDevice=device;state.transport="ble";state.bleBuffer="";renderTransport();setLive(true,"BLE connected");log(`BLE connected to ${device.name||"ESP32"}`,"status");await writeBle("@STATE\n")}
function disconnectBle(){if(state.bleDevice?.gatt?.connected)state.bleDevice.gatt.disconnect();else bleDisconnected()}
function coexHttpMode(){const m=state.latest?.radio?.bootModeActive;return m==="WIFI_BLE"||m==="WIFI_BLE_P"}
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
async function pollLoop(){if(state.otaActive){setTimeout(pollLoop,250);return}if(usingBle()){if(state.bleStatePending&&Date.now()-state.bleStateAt>4000)state.bleStatePending=false;if(!state.bleStatePending){state.bleStatePending=true;state.bleStateAt=Date.now();writeBle("@STATE\n").catch(error=>{state.bleStatePending=false;state.bleStateAt=0;log(error.message,"error")})}setTimeout(pollLoop,900);return}setTimeout(pollLoop,await pollHttpOnce())}
async function withLock(work){
  if(state.busy)return;state.busy=true;renderTransport();
  try {await work()} catch(error){log(error.message||String(error),error.name==="CommandCancelled"?"warning":"error")}
  finally {state.busy=false;renderTransport()}
}
async function prepareHttpForOta(){if(!usingBle())return;elements.otaMessage.textContent="Preparing the controller and handing OTA from BLE to Wi-Fi...";await writeBle("StopAll\n");await sleep(180);disconnectBle();for(let attempt=0;attempt<8&&usingBle();attempt++)await sleep(100);if(usingBle())bleDisconnected();let last;for(let attempt=0;attempt<6;attempt++){try{await pingHttp();log("OTA control handed from BLE to Wi-Fi","status");return}catch(error){last=error;await sleep(500)}}throw new Error(`Wi-Fi OTA endpoint is unavailable after BLE handoff${last?.message?`: ${last.message}`:""}`)}
async function uploadFirmware(){const file=elements.otaFile.files?.[0];if(!file)throw new Error("Choose an application .bin first");if(!file.name.toLowerCase().endsWith(".bin"))throw new Error("Choose an ESP32 application .bin file");if(file.size<1024)throw new Error("Firmware image is too small to be a valid ESP32 application");const magic=new Uint8Array(await file.slice(0,1).arrayBuffer())[0];if(magic!==0xe9)throw new Error("Selected file does not have an ESP32 application image header");state.otaActive=true;state.pollAbort?.abort();await state.httpTail;renderTransport();elements.otaProgress.value=0;elements.otaMessage.textContent="Preparing OTA...";try{await prepareHttpForOta();elements.otaMessage.textContent="Uploading over Wi-Fi...";const form=new FormData();form.append("firmware",file,file.name);await new Promise((resolve,reject)=>{const xhr=new XMLHttpRequest();xhr.open("POST",api("/api/ota"));xhr.timeout=180000;xhr.setRequestHeader("X-Request-ID",rid());xhr.setRequestHeader("X-Firmware-Size",String(file.size));xhr.upload.onprogress=event=>{if(event.lengthComputable){const progress=Math.min(99,Math.round(event.loaded/event.total*100));elements.otaProgress.value=progress;elements.otaMessage.textContent=`Uploading ${progress}% over Wi-Fi`}};xhr.onerror=()=>reject(new Error("OTA upload connection failed"));xhr.ontimeout=()=>reject(new Error("OTA upload timed out"));xhr.onabort=()=>reject(new Error("OTA upload was aborted"));xhr.onload=()=>{let payload={};try{payload=JSON.parse(xhr.responseText||"{}")}catch(error){}if(xhr.status>=200&&xhr.status<300&&payload.ok)resolve(payload);else reject(new Error(payload.error||`OTA HTTP ${xhr.status}`))};xhr.send(form)});elements.otaProgress.value=100;elements.otaMessage.textContent="Image validated. Controller is rebooting.";state.httpQuietUntil=Date.now()+10000;log("OTA image validated; reboot scheduled","status")}finally{state.otaActive=false;renderTransport()}}
function selectedOption(){const option=elements.commandPreset.selectedOptions[0];return option&&option.value===elements.commandInput.value.trim()?option:null}
function sendInput(){rememberInput();const option=selectedOption(),command=elements.commandInput.value;if(option?.dataset.confirm&&!window.confirm(`Send ${command}?\n\nUSB serial remains the recovery path.`))return;withLock(()=>runCommand(command))}

document.addEventListener("click",event=>{const fill=event.target.closest("button[data-fill]");if(fill){prefill(fill.dataset.fill);return}const button=event.target.closest("button[data-command]");if(!button)return;const command=button.dataset.command;if(button.dataset.confirm&&!window.confirm(`Send ${command}?\n\nUSB serial remains the recovery path.`))return;withLock(()=>runCommand(command))});
elements.commandPreset.addEventListener("change",()=>{const option=elements.commandPreset.selectedOptions[0];if(option?.value){prefill(option.value)}});
elements.commandInput.addEventListener("input",()=>{rememberInput();if(elements.commandPreset.value!==elements.commandInput.value.trim())elements.commandPreset.selectedIndex=0});
elements.commandInput.addEventListener("keydown",event=>{if(event.ctrlKey&&event.key==="Enter"){event.preventDefault();sendInput()}});
elements.sendCommands.addEventListener("click",sendInput);elements.stopAll.addEventListener("click",()=>runCommand("StopAll",true).catch(error=>log(error.message,"error")));elements.connectBle.addEventListener("click",()=>withLock(connectBle));elements.disconnectBle.addEventListener("click",disconnectBle);elements.clearLog.addEventListener("click",()=>{elements.log.textContent="";state.logLines=0;elements.logCount.textContent="0 lines"});elements.otaUpload.addEventListener("click",()=>withLock(uploadFirmware));

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
      state.latest={...(state.latest||{}),uplink:data.uplink};renderTransport();return;
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
try {prefill(localStorage.getItem("s3-command-draft")||"Status")} catch(error){prefill("Status")}
renderTransport();log("Console ready. Choose a preset or enter a command.","status");
document.addEventListener("visibilitychange",()=>{if(!document.hidden){state.nextStateAt=0;state.retryAt=0}});
pollLoop();
