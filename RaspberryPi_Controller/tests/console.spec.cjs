// Browser layout/recovery checks against a simulated controller, never GPIO.
const {chromium}=require('playwright'),assert=require('node:assert/strict'),http=require('node:http'),fs=require('node:fs'),path=require('node:path'),{execFileSync}=require('node:child_process');
const root=path.resolve(__dirname,'..'),web=path.join(root,'web');
const snapshot=process.env.PI_SNAPSHOT?JSON.parse(fs.readFileSync(process.env.PI_SNAPSHOT,'utf8')):JSON.parse(execFileSync(process.env.PYTHON||'python',['-B','-c',`import copy,json,tempfile
from bridge.config import DEFAULTS
from bridge.core import Controller
from bridge.hardware import SimulatedGPIO
with tempfile.TemporaryDirectory() as directory:
    config=copy.deepcopy(DEFAULTS);config['data_dir']=directory;config['simulate']=True
    controller=Controller(config,SimulatedGPIO(config),watchdog=False)
    print(json.dumps(controller.state()))
    controller.dispenser.close();controller.geo.close()
`],{cwd:root,encoding:'utf8'}));
const commands=[];let dropping=false;
const server=http.createServer((req,res)=>{
  if(req.url.startsWith('/api/')){
    if(dropping){req.socket.destroy();return}
    res.setHeader('Content-Type','application/json');
    if(req.method==='POST'){let body='';req.on('data',part=>body+=part);req.on('end',()=>{commands.push(body);res.end(JSON.stringify({accepted:true,latestEventId:0,requestId:req.headers['x-request-id']}))});return}
    res.end(JSON.stringify(req.url==='/api/state'?snapshot:{ok:true,events:[],cursor:0}));return;
  }
  const file=req.url==='/'?'index.html':req.url.slice(1);
  if(!['index.html','app.js','app.css'].includes(file)){res.writeHead(404).end();return}
  let source=fs.readFileSync(path.join(web,file),'utf8');
  if(file==='app.js')source=source.replace(/updateButtons\(\); poll\(\);\s*$/,'updateButtons();');
  res.setHeader('Content-Type',file.endsWith('js')?'application/javascript':file.endsWith('css')?'text/css':'text/html');res.end(source);
});
(async()=>{
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const browser=await chromium.launch({channel:'chrome',headless:true});
  try{
    const page=await browser.newPage({viewport:{width:1280,height:900}}),errors=[];
    page.on('pageerror',error=>errors.push(error.message));
    await page.goto(`http://127.0.0.1:${server.address().port}`);
    await page.evaluate(async()=>{const timer=window.setTimeout;window.setTimeout=(f,ms)=>f===poll?0:timer(f,ms);await poll()});
    assert.match(await page.locator('#liveText').innerText(),/Connected/);
    assert.equal(await page.locator('.controlSide > section').first().getAttribute('id'),'dispenserPanel');

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
    await page.locator('[data-fill="Ping"]').click();assert.equal(commands.length,0);
    assert.equal(await page.locator('#commandInput').inputValue(),'Ping');
    await page.locator('#sendCommands').click();await page.waitForFunction(()=>!state.busy);assert.equal(commands.at(-1),'Ping');
    await page.locator('#commandInput').fill('Status');await page.reload();assert.equal(await page.locator('#commandInput').inputValue(),'Status');
    dropping=true;await page.evaluate(()=>poll());assert.match(await page.locator('#liveText').innerText(),/Reconnecting/);
    assert.doesNotMatch(await page.locator('#log').innerText(),/Failed to fetch/);
    dropping=false;await page.evaluate(()=>poll());assert.match(await page.locator('#liveText').innerText(),/Connected/);
    await page.evaluate(()=>{document.getElementById('followLog').checked=false;for(let i=0;i<100;i++)log('Test line '+i);document.getElementById('log').scrollTop=0;log('Scrollback stays here')});
    assert.equal(await page.locator('#log').evaluate(el=>el.scrollTop),0);
    const displayed=await page.locator('#log').innerText();
    const downloadPromise=page.waitForEvent('download');await page.locator('#saveLog').click();const download=await downloadPromise;
    assert.equal(fs.readFileSync(await download.path(),'utf8'),displayed);

    for(const width of [320,390,720,1280]){await page.setViewportSize({width,height:900});assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1),`overflow at ${width}`)}
    await page.setViewportSize({width:390,height:844});
    await page.locator('.mobileDock a[href="#presets"]').click();
    await page.evaluate(()=>{state.busy=true;updateButtons()});
    assert(await page.locator('#mobileStop').isEnabled());
    const stopAck=page.waitForResponse(response=>response.url().includes('/api/command')&&response.request().postData()==='StopAll');
    await page.locator('#mobileStop').click();await stopAck;assert.equal(commands.at(-1),'StopAll');
    await page.evaluate(()=>{state.busy=false;updateButtons()});
    await page.setViewportSize({width:1280,height:900});
    if(process.env.CONSOLE_SCREENSHOT)await page.screenshot({path:process.env.CONSOLE_SCREENSHOT,fullPage:false});
    assert.deepEqual(errors,[]);console.log('PASS Pi preset search, controls order, prefill/draft/send, transient read recovery, scrollback, log download and mobile layout');
  }finally{await browser.close();server.closeAllConnections();await new Promise(resolve=>server.close(resolve))}
})().catch(error=>{console.error(error);server.closeAllConnections();server.close();process.exitCode=1});
