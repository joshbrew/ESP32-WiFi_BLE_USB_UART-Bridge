// Local mocked-device browser checks; never connects to controller hardware.
const { chromium } = require("playwright");
const assert = require("node:assert/strict");
const http = require("node:http");
const fs = require("node:fs");
const path = require("node:path");
const zlib = require("node:zlib");
const web = path.resolve(__dirname, "../web");
const commands = [];
const header = fs.readFileSync(path.resolve(web, "../src/web/WebAssets.h"), "utf8");
const embeddedHtml = zlib.gunzipSync(Buffer.from([...header.matchAll(/0x([0-9a-f]{2})/gi)].map(match => parseInt(match[1], 16))));
let heldResponse;
const snapshot = {
  ok: true, firmware: "Dispenser test", version: "test", freeHeap: 100000,
  addon: { name: "drone-dispenser", dispenser: true, active: true },
  dispenser: { armed: true, dispensing: false, faulted: false, maxPulseMs: 0, armTimeoutMs: 0, profile: "test", pin: 26, defaultPin: 26, pins: [26], availablePins: [13,14,16,17,18,19,21,22,23,25,26,27,32,33], outputsCapacity: 8 },
  routine: { active: false, library: [["dots", 1000, 200, 800, 6, true, 4]] },
  geo: { active: false, saved: true, fresh: true, count: 2, next: 0, source: "API", capacity: 256 },
  radio: { bootModeActive: "WIFI", wifiCompiled: true, bleCompiled: true, wifiState: "connected", ip: "192.168.4.1" },
  send: {}, selfTest: {}
};
const server = http.createServer((request, response) => {
  if (request.url === "/api/command") {
    let body = "";
    request.on("data", data => body += data);
    request.on("end", () => {
      commands.push(body);
      for (const line of body.split("\n")) if (line.startsWith("DispenserOutputs:")) snapshot.dispenser.pins = line.slice(17).split(",").map(Number);
      if (body === "RoutineRun:hold") { heldResponse = response; return; }
      response.setHeader("Content-Type", "application/json");
      response.end(JSON.stringify({ accepted: true, acceptedLines: body.split("\n").length, latestEventId: 0 }));
    });
    return;
  }
  if (request.url.startsWith("/api/")) {
    response.setHeader("Content-Type", "application/json");
    response.end(JSON.stringify(request.url === "/api/state" ? snapshot : { ok: true, cursor: 0, events: [] }));
    return;
  }
  const url = new URL(request.url, "http://localhost");
  if (url.pathname === "/" && url.searchParams.has("embedded")) {
    response.setHeader("Content-Type", "text/html"); response.end(embeddedHtml); return;
  }
  const file = url.pathname === "/" ? "index.html" : url.pathname.slice(1);
  if (!["index.html", "app.css", "app.js"].includes(file)) { response.writeHead(404); response.end(); return; }
  response.setHeader("Content-Type", file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html");
  response.end(fs.readFileSync(path.join(web, file)));
});
(async () => {
  await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
  const browser = await chromium.launch({ channel: "chrome", headless: true });
  try {
    const page = await browser.newPage({ viewport: { width: 1200, height: 900 } });
    const errors = [];
    page.on("pageerror", error => errors.push(error.message));
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.getByRole("button", { name: "Run dots", exact: true }).waitFor();
    assert.equal(await page.locator("#geoSource").inputValue(), "API");
    assert.match(await page.locator("#savedRoutines").innerText(), /Delay 1000 ms.*6 repeats/);
    for (const id of ["routineDelay", "routinePulse", "routineGap", "routineRepeats", "pulseDuration"])
      assert.equal(await page.locator(`#${id}`).getAttribute("max"), null);
    await page.locator("#routineDelay").fill("500000");
    await page.locator("#routinePulse").fill("7200000");
    await page.locator("#routineGap").fill("500000");
    await page.locator("#routineRepeats").fill("1000");
    await page.locator("#saveRoutine").click();
    await page.waitForFunction(() => !state.busy);
    assert.deepEqual(commands[0].split("\n"), ["RoutineCreate:dots", "RoutineAdd:dots:START_WAIT:500000", "RoutineAdd:dots:DISPENSE:7200000", "RoutineAdd:dots:WAIT_IDLE", "RoutineAdd:dots:WAIT:500000", "RoutineRepeat:dots:1000", "RoutineSave:dots"]);
    for (const value of ["0", "-1", "1.5", "4294967296"]) {
      const before=commands.length;
      await page.locator("#routineRepeats").fill(value);
      await page.locator("#saveRoutine").click();await page.waitForFunction(() => !state.busy);
      assert.equal(commands.length,before);
    }
    await page.locator("#routineRepeats").fill("4294967295");
    await page.locator("#saveRoutine").click();await page.waitForFunction(() => !state.busy);
    assert(commands.at(-1).includes("RoutineRepeat:dots:4294967295"));
    await page.locator("#routineContinuous").check();
    assert(await page.locator("#routineRepeats").isDisabled());
    await page.locator("#saveRoutine").click();await page.waitForFunction(() => !state.busy);
    assert(commands.at(-1).includes("RoutineRepeat:dots:FOREVER"));
    assert(commands.at(-1).includes("RoutineAdd:dots:START_WAIT:500000"));
    await page.locator("#routineContinuous").uncheck();
    assert(await page.locator("#routineRepeats").isEnabled());
    snapshot.dispenser.armed=false;
    await page.waitForFunction(() => !document.getElementById("saveOutputs").disabled);
    await page.locator("#addOutput").click();await page.locator("#addOutput").click();
    assert.equal(await page.locator(".outputRow").count(),3);
    await page.locator(".outputPin").nth(1).selectOption("27");await page.locator(".outputPin").nth(2).selectOption("25");
    await page.locator(".outputPulse").nth(1).fill("10");await page.locator(".outputGap").nth(1).fill("11");
    await page.locator(".outputPulse").nth(2).fill("20");await page.locator(".outputGap").nth(2).fill("12");
    await page.locator("#saveOutputs").click();await page.waitForFunction(() => !state.busy && state.latest.dispenser.pins.length===3);
    assert.equal(commands.at(-1),"DispenserOutputs:26,27,25\nDispenserSave");
    await page.locator("#routineName").fill("multi");await page.locator("#routineRepeats").fill("2");
    await page.locator("#saveRoutine").click();await page.waitForFunction(() => !state.busy);
    assert.match(commands.at(-1),/RoutineAdd:multi:OUTPUT:26,7200000,500000/);
    assert.match(commands.at(-1),/RoutineAdd:multi:OUTPUT:27,10,11/);assert.match(commands.at(-1),/RoutineAdd:multi:OUTPUT:25,20,12/);
    const beforeDuplicate=commands.length;await page.locator(".outputPin").nth(1).selectOption("26");
    await page.locator("#saveOutputs").click();await page.waitForFunction(() => !state.busy);assert.equal(commands.length,beforeDuplicate);
    await page.locator(".outputPin").nth(1).selectOption("27");
    while(await page.locator(".outputRow").count()<8)await page.locator("#addOutput").click();
    assert(await page.locator("#addOutput").isDisabled());
    await page.locator("#saveOutputs").click();await page.waitForFunction(() => !state.busy&&state.latest.dispenser.pins.length===8);
    const beforeLarge=commands.length;await page.locator("#saveRoutine").click();await page.waitForFunction(() => !state.busy);
    const outputBatches=commands.slice(beforeLarge);assert.equal(outputBatches.length,2);assert(outputBatches.every(body=>body.split("\n").length<=7));
    assert.equal(outputBatches.join("\n").split("\n").filter(line=>line.includes(":OUTPUT:")).length,8);assert.match(outputBatches.at(-1),/RoutineSave:multi$/);
    await page.locator(".outputRow button").first().click();assert.equal(await page.locator(".outputRow").count(),7);
    await page.locator("#singleOutput").click();assert.equal(await page.locator(".outputRow").count(),1);
    await page.locator("#saveOutputs").click();await page.waitForFunction(() => !state.busy&&state.latest.dispenser.pins.length===1);
    assert.equal(commands.at(-1),"DispenserOutputs:26\nDispenserSave");await page.locator("#routineName").fill("dots");
    await page.waitForFunction(() => !document.getElementById("useHourLimit").disabled);
    const beforeLimits=commands.length;
    await page.getByRole("button", { name: "Remove saved time limits", exact: true }).click();
    await page.waitForFunction(() => !state.busy);
    assert.deepEqual(commands.slice(beforeLimits), ["Disarm", "DispenserArmTimeout:0\nDispenserMaxPulse:0\nDispenserSave"]);
    snapshot.dispenser.armed=true;
    await page.getByText("Coordinate-triggered routines", { exact: true }).click();
    await page.locator("#geoSource").selectOption("API");
    await page.locator("#geoPoints").fill(Array.from({ length: 256 }, (_, i) => `37.${i},-122,5,dots`).join("\n"));
    const beforePlan = commands.length;
    await page.locator("#saveGeo").click();
    await page.waitForFunction(() => !state.busy);
    const batches = commands.slice(beforePlan);
    assert.equal(batches.length, 65);
    assert(batches.every(body => body.split("\n").length <= 4));
    assert.equal(batches.join("\n").split("\n").filter(line => line.startsWith("GeoAdd:")).length, 256);
    assert(await page.evaluate(() => { try { coordinateCommands(Array(257).fill("37,-122,5,dots").join("\n")); return false; } catch { return true; } }));
    assert.match(batches[0], /GeoSource:API/);
    assert.match(batches.at(-1), /GeoSave$/);
    assert(await page.evaluate(() => { try { coordinateCommands("91,-122,5,dots"); return false; } catch { return true; } }));
    assert(await page.locator("#geoTestSend").isDisabled());
    snapshot.geo.source="MAVLINK";
    await page.waitForFunction(() => !document.getElementById("geoTestSend").disabled);
    await page.locator("#geoTestPosition").fill("37,-122,1.25");
    await page.locator("#geoTestSend").click();await page.waitForFunction(() => !state.busy);
    assert.equal(commands.at(-1),"GeoTestPosition:37,-122,1.25");
    await page.locator("#geoTestStream").check();await page.waitForTimeout(1250);
    assert(commands.slice(-2).every(body=>body==="GeoTestPosition:37,-122,1.25"));
    await page.locator("#stopGeo").click();await page.waitForTimeout(100);
    assert(!(await page.locator("#geoTestStream").isChecked()));
    const afterTestStop=commands.length;await page.waitForTimeout(1200);assert.equal(commands.length,afterTestStop);
    await page.locator("#geoTestPosition").fill("91,-122,1");
    await page.locator("#geoTestSend").click();await page.waitForFunction(() => !state.busy);assert.equal(commands.length,afterTestStop);
    snapshot.geo.source="API";
    await page.getByText("Wi-Fi network / drone hotspot", { exact: true }).click();
    const invalidSsid = await page.evaluate(async () => {
      elements.wifiSsid.value = "é".repeat(17);
      try { await saveWifiSettings(); return "unexpectedly accepted"; }
      catch (error) { return error.message; }
    });
    assert.match(invalidSsid, /32 UTF-8 bytes/);
    await page.locator("#wifiSsid").fill("drone-hotspot");
    await page.locator("#wifiPassword").fill("test-secret-123");
    await page.locator("#saveWifi").click();
    await page.waitForFunction(() => !state.busy);
    assert.match(commands.at(-1), /WiFiMode:APSTA\nWiFiLR:OFF\nWiFiStaSSID:drone-hotspot\nWiFiStaPassword:test-secret-123\nConfigSave\nConfigApply/);
    assert(!(await page.locator("#log").innerText()).includes("test-secret-123"));
    assert.equal(await page.locator("#wifiPassword").inputValue(), "");
    // Hold one ordinary HTTP command; stop must bypass it and cancel queued work.
    await page.evaluate(() => {
      runCommand("RoutineRun:hold").catch(() => {});
      runCommand("Dispense:123").catch(() => {});
      state.busy = true; updateActions();
    });
    await page.waitForTimeout(100);
    assert(heldResponse);
    await page.locator("#stopRoutine").click();
    await page.waitForTimeout(150);
    assert.equal(commands.at(-1), "RoutineStop");
    heldResponse.end(JSON.stringify({ accepted: true, acceptedLines: 1 })); heldResponse = undefined;
    await page.waitForTimeout(200);
    assert(!commands.includes("Dispense:123"));
    await page.evaluate(() => { state.busy = false; updateActions(); });
    // Concurrent BLE polls and stop must not interleave fragmented lines.
    const bytes = await page.evaluate(async () => {
      const writes = [];
      state.transport = "ble"; state.bleTx = {};
      state.bleStatePending = true; state.bleStateAt = Date.now();
      state.bleRx = { writeValueWithoutResponse: async value => { writes.push(...value); await sleep(1); } };
      await Promise.all([writeBle("GeoPosition:37,-122,1,0\n"), writeBle("@STATE\n")]);
      state.transport = "http";state.bleTx = null;state.bleRx = null;
      state.bleStatePending = false;
      return new TextDecoder().decode(new Uint8Array(writes));
    });
    assert.equal(bytes, "GeoPosition:37,-122,1,0\n@STATE\n");
    // Exercise discovery of the new characteristic, raw writes, and phone GPS
    // through the real Connect BLE workflow (same mock reused by gzip tests).
    const installBluetooth = async target => target.evaluate(({ source }) => {
      window.gpsWrites = []; window.commandWrites = []; window.locationCallbacks = {}; window.clearedWatches = [];
      const rx = { writeValueWithoutResponse: async value => window.commandWrites.push(...value) };
      const gps = { writeValueWithoutResponse: async value => window.gpsWrites.push(Array.from(value)) };
      const tx = { startNotifications: async () => {}, addEventListener: () => {} };
      const service = { getCharacteristic: async uuid => uuid.includes("0004-") ? gps : uuid.includes("0002-") ? rx : tx };
      const device = { name: "Mock GPS controller", addEventListener: () => {}, gatt: {
        connected: true, connect: async () => ({ getPrimaryService: async () => service }), disconnect: () => {}
      } };
      Object.defineProperty(navigator, "bluetooth", { configurable: true, value: { requestDevice: async () => device } });
      Object.defineProperty(navigator, "geolocation", { configurable: true, value: {
        watchPosition: (success, error) => { window.locationCallbacks = { success, error }; return 42; },
        clearWatch: id => window.clearedWatches.push(id)
      } });
      // The BLE state notification arrives through the same production handler.
      window.mockGpsState = () => tx;
      window.mockSnapshot = source;
    }, { source: { ...snapshot, geo: { ...snapshot.geo, source: "BLE" } } });
    await installBluetooth(page);
    await page.evaluate(() => { updateActions(); });
    await page.locator("#connectBle").click();await page.waitForFunction(() => !state.busy);
    await page.evaluate(() => handleBleLine("@STATE " + JSON.stringify(window.mockSnapshot)));
    await page.locator("#geoTestPosition").fill("37,-122,1.25");await page.locator("#geoTestSend").click();
    await page.waitForFunction(() => !state.busy);
    const chunks = await page.evaluate(() => window.gpsWrites);
    assert.deepEqual(chunks.map(value => value.length), [20, 20, 20, 20, 10]);
    const packet = Buffer.from(chunks.flat());assert.equal(packet.length,90);
    assert.equal(packet[0],253);assert.equal(packet[7],24);assert.equal(packet[38],3);
    assert.equal(packet.readUInt32LE(44),1250);assert.equal(packet[50],253);assert.equal(packet[57],33);
    assert.equal(packet.readInt32LE(64),370000000);assert.equal(packet.readInt32LE(68),-1220000000);
    for (const [start, end, extra] of [[0, 48, 24], [50, 88, 104]]) {
      let crc=65535;for(const byte of [...packet.subarray(start+1,end),extra]) {crc^=byte;for(let i=0;i<8;++i)crc=(crc>>>1)^(crc&1?0x8408:0);}
      assert.equal(packet.readUInt16LE(end),crc);
    }
    await page.locator("#geoPhoneGps").check();
    await page.evaluate(() => {
      window.phoneSample={timestamp:Date.now(),coords:{latitude:37.1,longitude:-122.2,accuracy:2}};
      window.locationCallbacks.success(window.phoneSample);
    });await page.waitForFunction(() => !state.busy);
    assert.equal(await page.evaluate(() => window.gpsWrites.flat().length),180);
    await page.evaluate(() => { window.locationCallbacks.success(window.phoneSample); window.locationCallbacks.success({...window.phoneSample,timestamp:Date.now()-5000}); });
    await page.waitForTimeout(100);assert.equal(await page.evaluate(() => window.gpsWrites.flat().length),180);
    await page.locator("#stopGeo").click();await page.waitForTimeout(150);
    assert(!(await page.locator("#geoPhoneGps").isChecked()));assert.deepEqual(await page.evaluate(() => window.clearedWatches),[42]);
    await page.evaluate(() => { window.locationCallbacks.success({...window.phoneSample,timestamp:Date.now()}); bleDisconnected(); });
    await page.waitForTimeout(100);assert.equal(await page.evaluate(() => window.gpsWrites.flat().length),180);
    const screenshot = process.env.CONSOLE_SCREENSHOT;
    if (screenshot) await page.screenshot({ path: screenshot, fullPage: true });
    await page.setViewportSize({ width: 390, height: 844 });
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1));
    assert.deepEqual(errors, []);
    await page.close();
    // Exercise the actual minified gzip page embedded in firmware too.
    snapshot.routine.library.push(["hold", 0, 20, 0, 1, true, 3]);
    snapshot.routine.library.push(["loop", 5000, 20, 100, 0, true, 4]);
    const embedded = await browser.newPage();
    embedded.on("pageerror", error => errors.push(error.message));
    await embedded.goto(`http://127.0.0.1:${server.address().port}/?embedded=1`);
    await embedded.getByRole("button", { name: "Run dots", exact: true }).waitFor();
    assert.equal(await embedded.locator("#geoSource").inputValue(), "API");
    await embedded.locator("#routineDelay").fill("7000");
    await embedded.locator("#routineRepeats").fill("300");
    await embedded.locator("#saveRoutine").click();
    await embedded.waitForFunction(() => !document.getElementById("saveRoutine").disabled);
    assert(commands.at(-1).includes("RoutineRepeat:dots:300"));
    assert.match(commands.at(-1), /RoutineAdd:dots:START_WAIT:7000/);
    await embedded.locator("#routineContinuous").check();
    await embedded.locator("#saveRoutine").click();
    await embedded.waitForFunction(() => !document.getElementById("saveRoutine").disabled);
    assert(commands.at(-1).includes("RoutineRepeat:dots:FOREVER"));
    assert.match(await embedded.locator("#savedRoutines").innerText(), /Delay 5000 ms.*until stopped/);
    snapshot.geo.source="MAVLINK";
    await embedded.waitForFunction(() => !document.getElementById("geoTestSend").disabled);
    await embedded.getByText("Coordinate-triggered routines", { exact: true }).click();
    await embedded.locator("#geoTestPosition").fill("37,-122,1");
    await embedded.locator("#geoTestSend").click();
    await embedded.waitForFunction(() => !document.getElementById("geoTestSend").disabled);
    assert.equal(commands.at(-1),"GeoTestPosition:37,-122,1");
    await embedded.getByRole("button", { name: "Run hold", exact: true }).click();
    await embedded.waitForTimeout(100); assert(heldResponse);
    await embedded.locator("#stopRoutine").click();
    await embedded.waitForTimeout(150); assert.equal(commands.at(-1), "RoutineStop");
    heldResponse.end(JSON.stringify({ accepted: true, acceptedLines: 1 })); heldResponse = undefined;
    assert.deepEqual(errors, []);
    console.log("PASS saved buttons, one-time delay, uncapped input, 32-bit repeats and invalid counts, coordinate batches/validation, client settings/redaction, immediate stop/cancel, BLE framing, mobile width");
    console.log("PASS production minified gzip page, saved buttons, source readback, initial delay, stop during an in-flight request");
    // Deliver an actual notification; production code is minified and private.
    await embedded.evaluate(({ source }) => {
      window.gpsWrites=[];
      const rx={writeValueWithoutResponse:async value=>{
        if(new TextDecoder().decode(value)==="@STATE\n") window.bleNotify({target:{value:new DataView(new TextEncoder().encode("@STATE "+JSON.stringify(source)+"\n").buffer)}});
      }};
      const tx={startNotifications:async()=>{},addEventListener:(_name,callback)=>window.bleNotify=callback};
      const gps={writeValueWithoutResponse:async value=>window.gpsWrites.push(...value)};
      const device={name:"GPS",addEventListener:()=>{},gatt:{connected:true,connect:async()=>({getPrimaryService:async()=>({getCharacteristic:async uuid=>uuid.includes("0004-")?gps:uuid.includes("0002-")?rx:tx})})}};
      Object.defineProperty(navigator,"bluetooth",{configurable:true,value:{requestDevice:async()=>device}});
    }, { source: { ...snapshot, geo: { ...snapshot.geo, source: "BLE" } } });
    await embedded.waitForTimeout(1200);await embedded.locator("#connectBle").click();
    await embedded.waitForFunction(() => !document.getElementById("geoTestSend").disabled);
    await embedded.locator("#geoTestPosition").fill("37,-122,1");await embedded.locator("#geoTestSend").click();
    await embedded.waitForFunction(() => window.gpsWrites.length >= 90);
    assert.equal(await embedded.evaluate(() => window.gpsWrites[7]),24);
    assert.equal(await embedded.evaluate(() => window.gpsWrites[57]),33);
    assert.deepEqual(errors, []);
    console.log("PASS Bluetooth GPS discovery/raw fragments/CRC/fields, phone GPS freshness/duplicates/stop, production Bluetooth GPS");
    console.log("PASS editable output rows, per-output timings, pin save, duplicates, eight-output batching, remove/default single mode");
  } finally {
    if (heldResponse) heldResponse.destroy();
    await browser.close();
    server.close();
  }
})().catch(error => { console.error(error); server.close(); process.exitCode = 1; });
