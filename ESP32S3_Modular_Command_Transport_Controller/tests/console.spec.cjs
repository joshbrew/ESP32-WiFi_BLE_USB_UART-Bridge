// Browser checks against a local mock, never an attached controller.
const {chromium}=require("playwright"),http=require("node:http"),fs=require("node:fs"),path=require("node:path"),assert=require("node:assert/strict");
const web=path.join(__dirname,"../web");
const snapshot={ok:true,firmware:"ESP32-S3 Modular Controller",version:"UI preview · simulated device",freeHeap:132600,largestHeapBlock:98640,latestEventId:0,bootMode:"production",addon:{name:"none",active:false},radio:{bootModeActive:"WIFI_BLE_P",wifiCompiled:true,bleCompiled:true,sppCompiled:false,wifiState:"connected",ip:"192.168.4.1",bleRunning:true},send:{wifi:true,ble:false,bleConnected:false},queue:{waiting:0,capacity:8},indicators:{enabled:true,rgb:true,rgbPin:48},selfTest:{phase:"idle",current:0,total:0,pass:0,fail:0,skip:0,boots:0}};
let mode="ok",delayRead=0,delayCommand=0,wireEvents=null;const commands=[];
const server=http.createServer((req,res)=>{
  if(req.url==="/slow-body"){res.writeHead(200,{"Content-Type":"application/json"});res.write('{"ok":');setTimeout(()=>res.end('true}'),300);return}
  if(req.url.startsWith("/api/")){
    if(req.method==="POST"){
      let body="";req.on("data",chunk=>body+=chunk);req.on("end",()=>{
        commands.push({body,id:req.headers["x-request-id"],at:Date.now()});
        setTimeout(()=>{res.setHeader("Content-Type","application/json");res.end(JSON.stringify({accepted:true,acceptedLines:body.split("\n").length,latestEventId:0}))},body==="StopAll"?0:delayCommand);
      });return;
    }
    if(mode==="drop"){req.socket.destroy();return}
    if(mode==="denied"){res.writeHead(401,{"Content-Type":"application/json"});res.end('{"error":"Sign in required"}');return}
    setTimeout(()=>{res.setHeader("Content-Type","application/json");res.end(JSON.stringify(req.url.startsWith("/api/state")?snapshot:(wireEvents||{ok:true,events:[],cursor:0})))},delayRead);return;
  }
  const file=req.url.split("?")[0]==="/"?"index.html":req.url.split("?")[0].slice(1);
  if(!["index.html","app.css","app.js"].includes(file)){res.writeHead(404).end();return}
  let source=fs.readFileSync(path.join(web,file),"utf8");
  // Drive the production polling function explicitly for deterministic failures.
  if(file==="app.js")source=source.replace(/pollLoop\(\);\s*$/,"void 0;");
  res.setHeader("Content-Type",file.endsWith("js")?"application/javascript":file.endsWith("css")?"text/css":"text/html");res.end(source);
});
(async()=>{
  await new Promise(resolve=>server.listen(0,"127.0.0.1",resolve));
  const browser=await chromium.launch({channel:"chrome",headless:true});
  try{
    const page=await browser.newPage({viewport:{width:1320,height:900}}),errors=[];
    page.on("pageerror",error=>errors.push(error.message));
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.evaluate(()=>pollHttpOnce());
    assert.equal(await page.locator("#liveText").textContent(),"Connected");
    assert.equal(await page.locator('button[data-fill="ModeBTSerial"]:visible').count(),0);
    await page.locator('button[data-fill="SendBLE:hello from Wi-Fi"]').isDisabled().then(value=>assert(value));
    await page.locator('button[data-fill="Status"]').click();
    assert.equal(commands.length,0);assert.equal(await page.locator("#commandInput").inputValue(),"Status");
    const consoleBox=await page.locator(".console").boundingBox(),presetBox=await page.locator(".presets").boundingBox();
    assert(presetBox.x>consoleBox.x+consoleBox.width&&Math.abs(presetBox.y-consoleBox.y)<2);
    await page.locator("#sendCommands").click();await page.waitForFunction(()=>!state.busy);assert.equal(commands.at(-1).body,"Status");
    const deadline=await page.evaluate(async()=>{try{await fetchTimed(api("/slow-body"),{},60);return "missed"}catch(error){return error.name}});
    assert.equal(deadline,"TimeoutError");
    mode="drop";
    await page.evaluate(()=>{state.retryAt=0;state.httpQuietUntil=0;state.nextStateAt=0;return pollHttpOnce()});
    assert.equal(await page.locator("#liveText").textContent(),"Reconnecting…");
    assert(!/ERROR.*Failed to fetch/.test(await page.locator("#log").textContent()));
    mode="ok";await page.evaluate(()=>{state.retryAt=0;return pollHttpOnce()});
    assert.equal(await page.locator("#liveText").textContent(),"Connected");
    mode="drop";await page.evaluate(()=>{state.lastContact=Date.now()-10000;state.httpFailures=2;state.retryAt=0;return pollHttpOnce()});
    assert.equal(await page.locator("#liveText").textContent(),"Controller offline");
    mode="ok";await page.evaluate(()=>{state.retryAt=0;return pollHttpOnce()});
    mode="denied";await page.evaluate(()=>{state.offlineLogged=false;state.retryAt=0;return pollHttpOnce()});
    assert.equal(await page.locator("#liveText").textContent(),"Access denied");
    mode="ok";await page.evaluate(()=>{state.retryAt=0;return pollHttpOnce()});
    await page.locator("#commandInput").fill("Ping");await page.reload();await page.evaluate(()=>pollHttpOnce());
    assert.equal(await page.locator("#commandInput").inputValue(),"Ping");
    await page.locator("#commandInput").fill("WiFiStaPassword:private-test-value");
    assert.equal(await page.evaluate(()=>localStorage.getItem("s3-command-draft")),null);
    await page.locator("#commandInput").fill("Status");
    delayRead=300;
    await page.evaluate(()=>{state.httpQuietUntil=0;state.retryAt=0;state.nextStateAt=0;window.readInFlight=pollHttpOnce()});
    await page.waitForFunction(()=>state.pollAbort!==null);await page.locator("#sendCommands").click();
    await page.waitForFunction(()=>!state.busy);delayRead=0;
    delayCommand=250;const start=commands.length;
    await page.evaluate(()=>{window.a=postHttp("Ping");window.b=postHttp("Help").catch(error=>error.name)});
    await page.waitForFunction(()=>state.httpQuietUntil>Date.now());await page.locator("#stopAll").click();
    await page.waitForFunction(()=>state.httpTail.then(()=>true));
    await page.waitForTimeout(500);
    assert(commands.slice(start).some(command=>command.body==="StopAll"));
    assert(!commands.slice(start).some(command=>command.body==="Help"));delayCommand=0;
    await page.evaluate(()=>{renderState({...state.latest,addon:{name:'<img id="injected" src=x>',active:false}})});
    assert.equal(await page.locator("#injected").count(),0);
    await page.evaluate(()=>{renderState({...state.latest,addon:{name:"none",active:false}});elements.log.textContent="";state.logLines=0;log("Connected to ESP32S3-Controller.","status");log("Wi-Fi + BLE ready. Onboard RGB enabled.","status");log("TX HTTP Status","send");log("Internal: ESP32-S3 Modular Command + Transport Controller","status");log("Wi-Fi connected · BLE advertising · addon none","status");log("Command queue 0 / 8 · heap 132600 B","info");log("Ready for your next command.","status");prefill("Status")});
    if(process.env.CONSOLE_SCREENSHOT)await page.screenshot({path:process.env.CONSOLE_SCREENSHOT,fullPage:true});
    for(const width of [320,390,412,720,900]){await page.setViewportSize({width,height:844});assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1),`Horizontal overflow at ${width}px`)}
    wireEvents={ok:true,events:[{l:"status",s:"WiFi",r:"wire-test",t:"Compact local result"},{q:1,t:"Raw compact payload"}],cursor:12,more:false};
    await page.evaluate(()=>{state.cursor=10;state.httpQuietUntil=0;state.retryAt=0;state.nextStateAt=Date.now()+60000;return pollHttpOnce()});
    assert.match(await page.locator("#log").innerText(),/Compact local result/);
    assert.match(await page.locator("#log").innerText(),/Raw compact payload/);
    assert.equal(await page.evaluate(()=>state.cursor),12);
    await page.evaluate(()=>{state.httpQuietUntil=0;return pollHttpOnce()});
    assert.equal((await page.locator("#log").innerText()).split("Compact local result").length-1,1);
    wireEvents={ok:true,events:[{l:"status",s:"Internal",t:"Output after controller reboot"}],cursor:2,reset:true};
    await page.evaluate(()=>{state.httpQuietUntil=0;return pollHttpOnce()});
    assert.match(await page.locator("#log").innerText(),/Output after controller reboot/);
    assert.equal(await page.evaluate(()=>state.cursor),2);
    wireEvents=null;
    assert.deepEqual(errors,[]);
    console.log("PASS presets/draft/privacy, full-body timeout, transient recovery, real outage/auth errors, poll cancellation, Stop priority, escaped state, desktop/mobile layout");
  } finally {await browser.close();server.close()}
})().catch(error=>{console.error(error);server.close();process.exitCode=1});
