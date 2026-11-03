// Remote presentation checks use a simulated socket and API, never a controller.
const {chromium}=require('playwright');
const assert=require('node:assert/strict'),http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const web=process.env.WEB_ROOT||path.join(__dirname,'../web');
const snapshot={ok:true,firmware:'S3 field console',version:'Simulated LTE preview',latestEventId:0,freeHeap:140000,
  addon:{name:'drone-dispenser',dispenser:true,active:true},
  dispenser:{armed:false,dispensing:false,faulted:false,pin:4,defaultPin:4,pins:[4],availablePins:[4,5,6,7],outputsCapacity:8,maxPulseMs:1000,armTimeoutMs:60000},
  routine:{active:false,library:[]},geo:{active:false,saved:false,fresh:false,count:0,source:'API',capacity:256},
  radio:{bootModeActive:'WIFI_BLE_P',wifiCompiled:true,bleCompiled:true,sppCompiled:false,wifiState:'off'},send:{},queue:{waiting:0,capacity:8},selfTest:{phase:'idle'},
  uplink:{relaySession:'relay-one',online:false,session:'a'.repeat(32),ageMs:0,stale:false}};
const commands=[];let healthMode="ready",healthReads=0;
const server=http.createServer((req,res)=>{
  if(req.url.startsWith('/api/')){
    res.setHeader('Content-Type','application/json');
    if(req.url==='/api/health'){
      healthReads++;
      if(healthMode==='slow'){
        const timer=setTimeout(()=>res.end(JSON.stringify({ok:true,relay:{http:'ready'},broker:{phase:'connected'}})),5000);
        res.on('close',()=>clearTimeout(timer));return;
      }
      if(healthMode==='denied'){res.writeHead(403).end(JSON.stringify({error:'Access denied'}));return}
      if(healthMode==='broken'){res.end('{');return}
      res.end(JSON.stringify({ok:true,relay:{http:'ready'},broker:{phase:'connected',message:'Broker connected and subscribed'},device:{connected:snapshot.uplink.online,phase:'online'}}));return;
    }
    if(req.method==='POST'){let body='';req.on('data',part=>body+=part);req.on('end',()=>{commands.push({body,nonce:req.headers['x-relay-session'],deviceSession:req.headers['x-device-session']});res.end(JSON.stringify({accepted:true,acceptedLines:1,latestEventId:900}))});return}
    res.end(JSON.stringify(req.url==='/api/state'?snapshot:{ok:true,events:[],cursor:0}));return;
  }
  const name=req.url==='/'?'index.html':req.url.slice(1);
  if(!['index.html','app.js','app.css'].includes(name)){res.writeHead(404).end();return}
  res.setHeader('Content-Type',name.endsWith('js')?'application/javascript':name.endsWith('css')?'text/css':'text/html');res.end(fs.readFileSync(path.join(web,name)));
});
(async()=>{
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const browser=await chromium.launch({channel:'chrome',headless:true});
  try{
    const page=await browser.newPage({viewport:{width:1280,height:900}}),errors=[];
    page.on('pageerror',error=>errors.push(error.message));
    await page.addInitScript(()=>{
      window.ESP32_REMOTE={wsPath:'/ws',session:'relay-one'};
      window.WebSocket=class{constructor(url){this.url=String(url);this.readyState=1;window.testSocket=this}close(){this.readyState=3;this.onclose?.()}};
    });
    await page.goto(`http://127.0.0.1:${server.address().port}`);
    await page.waitForFunction(()=>state.latest?.uplink);
    assert.match(await page.locator('#liveText').innerText(),/Device offline/);
    assert(await page.locator('#sendCommands').isDisabled());
    assert(await page.locator('#connectBle').isDisabled());
    assert(await page.locator('#otaUpload').isDisabled());
    assert.equal(commands.length,0);

    const search=page.locator('#presetSearch');
    await search.fill('status');
    assert(await page.locator('#presetGroups [data-fill]:visible').count()>0);
    assert.match(await page.locator('#presetCount').innerText(),/matching preset/);
    const firstPreset=page.locator('#presetGroups [data-fill]:visible').first();
    const presetBody=await firstPreset.getAttribute('data-fill');
    await firstPreset.click();assert.equal(await page.locator('#commandInput').inputValue(),presetBody);assert.equal(commands.length,0);
    await search.fill('no_such_preset');
    assert(await page.locator('#presetEmpty').isVisible());assert.equal(await page.locator('#presetGroups [data-fill]:visible').count(),0);
    await search.press('Escape');assert.equal(await search.inputValue(),'');assert(await page.locator('#presetGroups [data-fill]:visible').count()>0);
    const push=data=>page.evaluate(data=>window.testSocket.onmessage({data:JSON.stringify({type:'state',data})}),data);
    snapshot.uplink.online=true;
    await push(snapshot);
    assert.match(await page.locator('#liveText').innerText(),/LTE connected/);
    assert(await page.locator('#sendCommands').isEnabled());
    await search.fill('no_such_preset');await push(snapshot);
    assert.equal(await page.locator('#presetGroups [data-fill]:visible').count(),0,'state refresh preserves the search');
    await search.press('Escape');
    assert.equal(await page.locator('#presetGroups [data-requires="spp"]:visible').count(),0,'search must not expose unavailable SPP presets');
    await page.locator('#commandInput').fill('Ping');await page.locator('#sendCommands').click();
    await page.waitForFunction(()=>!state.busy);assert.equal(commands.at(-1).body,'Ping');assert.equal(commands.at(-1).nonce,'relay-one');assert.equal(commands.at(-1).deviceSession,snapshot.uplink.session);
    assert.match(await page.locator('.connectionMetrics').innerText(),/Last ack \d+ ms/);
    await page.evaluate(()=>window.testSocket.onmessage({data:JSON.stringify({type:'event',data:{id:12,t:'Field link active',s:'LTE',l:'status'}})}));
    assert.match(await page.locator('#log').innerText(),/Field link active/);
    assert.equal(await page.evaluate(()=>state.cursor),12,'the acknowledgement must not skip unseen execution output');
    await page.evaluate(data=>{
      window.testSocket.onmessage({data:JSON.stringify({type:'sync',data:{state:{...data,latestEventId:900},history:{events:[{id:13,t:'Missed result',s:'LTE',l:'status',ageMs:4000},{id:14,t:'Next result',s:'LTE',l:'status'}],cursor:14,deviceSession:data.uplink.session,relaySession:data.uplink.relaySession,reset:false,gap:false}}})});
    },snapshot);
    assert.match(await page.locator('#log').innerText(),/Recovered 2 earlier messages/);
    assert.match(await page.locator('#log').innerText(),/4s earlier.*Missed result/);
    assert.equal(await page.evaluate(()=>state.cursor),14);
    await page.evaluate(()=>{
      window.testHidden=true;Object.defineProperty(document,'hidden',{configurable:true,get:()=>window.testHidden});
      document.dispatchEvent(new Event('visibilitychange'));window.testHidden=false;document.dispatchEvent(new Event('visibilitychange'));
    });
    const resumedUrl=new URL(await page.evaluate(()=>window.testSocket.url));
    assert.equal(resumedUrl.searchParams.get('since'),'14');assert.equal(resumedUrl.searchParams.get('deviceSession'),snapshot.uplink.session);
    await page.evaluate(data=>window.testSocket.onmessage({data:JSON.stringify({type:'sync',data:{state:data,history:{events:[{id:15,t:'Result while phone slept',s:'LTE',l:'status'}],cursor:15,deviceSession:data.uplink.session,relaySession:data.uplink.relaySession,reset:false,gap:false}}})}),snapshot);
    assert.match(await page.locator('#log').innerText(),/Result while phone slept/);assert.equal(await page.evaluate(()=>state.cursor),15);
    await page.evaluate(()=>consumeEvents({events:[{id:14,t:'Duplicate result'}],cursor:14}));
    assert.doesNotMatch(await page.locator('#log').innerText(),/Duplicate result/);
    await page.evaluate(()=>consumeEvents({events:[{id:100,t:'Wrong session'}],cursor:100,deviceSession:'other-device-session',relaySession:window.ESP32_REMOTE.session}));
    assert.doesNotMatch(await page.locator('#log').innerText(),/Wrong session/);assert.equal(await page.evaluate(()=>state.cursor),15);
    const beforeExport=await page.locator('#log').innerText();
    const downloadPromise=page.waitForEvent('download');await page.locator('#saveLog').click();const download=await downloadPromise;
    assert.match(download.suggestedFilename(),/^controller-console-.*\.txt$/);
    assert.equal(fs.readFileSync(await download.path(),'utf8'),beforeExport);

    snapshot.uplink.transport='WiFi';await push(snapshot);
    assert.equal(await page.locator('#transportSummary').textContent(),'Remote Wi-Fi');
    assert.match(await page.locator('#liveText').textContent(),/Wi-Fi connected/);
    snapshot.uplink.transport='LTE';await push(snapshot);
    assert.equal(await page.locator('#transportSummary').textContent(),'Remote LTE');
    snapshot.uplink.stopPending=true;await push(snapshot);
    assert(await page.locator('#sendCommands').isDisabled());assert(await page.locator('#stopAll').isEnabled());
    assert.match(await page.evaluate(()=>runCommand('Arm').catch(error=>error.message)),/Waiting for Stop confirmation/);
    assert.match(await page.locator('.connectionMetrics').innerText(),/Stop awaiting confirmation/);
    snapshot.uplink.stopPending=false;await push(snapshot);assert(await page.locator('#sendCommands').isEnabled());
    assert.match(await page.locator('#log').innerText(),/Stop acknowledged by the controller queue/);
    // Read-only diagnostics must never resend a command or alter its draft.
    const beforeCheck=commands.length;
    await page.locator('#commandInput').fill('Status');
    await page.locator('.connectionDetails summary').click();
    await page.waitForFunction(()=>document.querySelector('.connectionActions [role="status"]').textContent.startsWith('Checked'));
    assert(healthReads>0);assert.match(await page.locator('.connectionDetails').innerText(),/Broker connected and subscribed/);
    assert.equal(commands.length,beforeCheck);assert.equal(await page.locator('#commandInput').inputValue(),'Status');
    healthMode='denied';await page.locator('.connectionActions button').click();
    await page.waitForFunction(()=>document.querySelector('.connectionActions [role="status"]').textContent.includes('access denied'));
    healthMode='broken';await page.locator('.connectionActions button').click();
    await page.waitForFunction(()=>document.querySelector('.connectionActions [role="status"]').textContent.includes('unavailable'));
    healthMode='ready';
    // A fresh presence after reconnect must not expose the previous armed state.
    const waiting={ok:true,uplink:{...snapshot.uplink,session:'b'.repeat(32),ageMs:null,stale:true}};
    await page.evaluate(()=>{
      const gate=new Promise(resolve=>window.releaseOldCommand=resolve);
      state.httpTail=state.httpTail.then(()=>gate);
      window.oldCommand=postHttp('Arm','queued-before-reconnect').then(()=> 'sent',error=>error.name);
    });
    await push(waiting);
    assert.equal(await page.evaluate(()=>{window.releaseOldCommand();return window.oldCommand}),'CommandCancelled');
    assert.equal(commands.length,beforeCheck,'an old browser queue must not send Arm to the new connection');
    assert(await page.locator('#sendCommands').isDisabled());assert(await page.locator('#stopAll').isEnabled());
    assert.match(await page.locator('#liveText').innerText(),/Waiting for device state/);
    assert.match(await page.evaluate(()=>runCommand('Arm').catch(error=>error.message)),/Waiting for current device state/);
    if(await page.locator('#payloadState').count()){
      assert.equal(await page.locator('#payloadState').innerText(),'UNKNOWN');
      assert(await page.locator('[data-command="Arm"]').isDisabled());
      assert(await page.locator('#dispensePulse').isDisabled());
    }
    await push(snapshot);assert(await page.locator('#sendCommands').isEnabled());
    await page.locator('.connectionActions button').click();
    await page.waitForFunction(()=>document.querySelector('.connectionActions [role="status"]').textContent.startsWith('Checked'));
    assert.equal(commands.length,beforeCheck);
    await page.locator('.connectionDetails summary').click();
    const beforeEpoch=await page.evaluate(()=>state.commandGeneration??state.commandEpoch);
    snapshot.uplink.relaySession='relay-two';await push(snapshot);
    assert.equal(await page.evaluate(()=>window.ESP32_REMOTE.session),'relay-two');
    assert.equal(await page.evaluate(()=>state.commandGeneration??state.commandEpoch),beforeEpoch+1);
    assert.match(await page.locator('#log').innerText(),/Relay restarted/);
    await page.locator('#commandInput').fill('Status');await page.locator('#sendCommands').click();
    await page.waitForFunction(()=>!state.busy);assert.equal(commands.at(-1).nonce,'relay-two');
    snapshot.uplink.ageMs=9500;snapshot.uplink.stale=true;await push(snapshot);
    assert.match(await page.locator('#liveText').innerText(),/state 9s old/);
    snapshot.uplink.online=false;await push(snapshot);
    const error=await page.evaluate(()=>runCommand('Arm').catch(error=>error.message));
    assert.match(error,/offline/);assert.equal(commands.length,2);
    snapshot.uplink.online=true;snapshot.uplink.ageMs=50;snapshot.uplink.stale=false;await push(snapshot);
    await page.evaluate(()=>window.testSocket.close());
    assert.match(await page.locator('.connectionMetrics').innerText(),/HTTP fallback/);
    await page.evaluate(()=>{state.retryAt=0;state.nextStateAt=0;state.httpQuietUntil=0;return pollHttpOnce()});
    assert.match(await page.locator('#liveText').innerText(),/LTE connected/);
    if(await page.locator('.payloadPanel').count()){
      assert.equal(await page.locator('.controlSide > section').first().getAttribute('class'),'panel payloadPanel');
    }
    for(const width of [320,390,720,1280]){
      await page.setViewportSize({width,height:900});
      assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1),`overflow at ${width}`);
    }
    await page.setViewportSize({width:390,height:844});
    await page.locator('.mobileDock a[href="#presets"]').click();
    assert(await page.locator('#mobileStop').isVisible());
    await page.evaluate(()=>{state.busy=true;renderTransport()});
    assert(await page.locator('#mobileStop').isEnabled());
    const stopAck=page.waitForResponse(response=>response.url().includes('/api/command')&&response.request().postData()==='StopAll');
    await page.locator('#mobileStop').click();await stopAck;
    assert.equal(commands.at(-1).body,'StopAll');
    await page.evaluate(()=>{state.busy=false;renderTransport()});
    // A blocked read-only check yields to the priority Stop path.
    healthMode='slow';const checksBeforeStop=healthReads;
    await page.locator('.connectionDetails summary').click();
    await page.waitForFunction(()=>document.querySelector('.connectionActions button').disabled);
    while(healthReads===checksBeforeStop)await new Promise(resolve=>setTimeout(resolve,10));
    await page.evaluate(()=>runCommand('StopAll',true));
    await page.waitForFunction(()=>document.querySelector('.connectionActions [role="status"]').textContent.includes('paused for a command'));
    assert.equal(commands.at(-1).body,'StopAll');healthMode='ready';
    await page.locator('.connectionDetails summary').click();
    if(process.env.DIAGNOSTICS_SCREENSHOT){
      await page.locator('.connectionDetails summary').click();
      await page.waitForFunction(()=>document.querySelector('.connectionActions [role="status"]').textContent.startsWith('Checked'));
      await page.evaluate(()=>document.getElementById('commandConsole').scrollIntoView());
      await page.screenshot({path:process.env.DIAGNOSTICS_SCREENSHOT,fullPage:false});
      await page.locator('.connectionDetails summary').click();
    }
    if(process.env.MOBILE_SCREENSHOT)await page.screenshot({path:process.env.MOBILE_SCREENSHOT,fullPage:false});
    await page.setViewportSize({width:1280,height:900});
    if(process.env.CONSOLE_SCREENSHOT){await page.evaluate(()=>window.scrollTo(0,0));snapshot.uplink.online=true;snapshot.uplink.ageMs=300;snapshot.uplink.stale=false;await page.evaluate(data=>renderState(data),snapshot);await page.screenshot({path:process.env.CONSOLE_SCREENSHOT,fullPage:false})}
    assert.deepEqual(errors,[]);
    console.log('PASS read-only connection checks yielding to Stop, sign-in failures, waiting-state guards, preset search/prefill/capability refresh, remote state, restart/stop guards, fallback, output recovery after phone sleep, dedup/scopes, log download, fixed mobile Stop and layout');
  }finally{await browser.close();server.closeAllConnections();await new Promise(resolve=>server.close(resolve))}
})().catch(error=>{console.error(error);server.closeAllConnections();server.close();process.exitCode=1});

