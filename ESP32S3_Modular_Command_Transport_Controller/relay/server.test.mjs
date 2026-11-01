// Integration uses an ephemeral local TLS/MQTT simulator; no modem or public service.
import test from 'node:test';
import assert from 'node:assert/strict';
import tls from 'node:tls';
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawn,execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import mqttPacket from 'mqtt-packet';
import {WebSocket} from 'ws';
import {checkHealth} from './healthcheck.mjs';

test('relay bridges HTTPS-proxy requests and browser streams through verified MQTT TLS',{timeout:15000},async()=>{
  const directory=fs.mkdtempSync(path.join(os.tmpdir(),'field-relay-'));
  const openssl=process.env.OPENSSL||'openssl';
  const key=path.join(directory,'server.key'),cert=path.join(directory,'server.crt');
  execFileSync(openssl,['req','-x509','-newkey','rsa:2048','-nodes','-keyout',key,'-out',cert,'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],{stdio:'ignore'});
  const session='a'.repeat(32),seen=[];let child,webSocket,missingAssetsChild,denySubscription=false,holdState=true,emitState;
  const sockets=new Set();
  const broker=tls.createServer({key:fs.readFileSync(key),cert:fs.readFileSync(cert)},socket=>{
    sockets.add(socket);socket.on('close',()=>sockets.delete(socket));socket.on('error',()=>{});
    const parser=mqttPacket.parser();socket.on('data',data=>parser.parse(data));
    const send=packet=>socket.write(mqttPacket.generate(packet));
    const publish=(kind,payload)=>send({cmd:'publish',topic:'devices/test/'+kind,payload:JSON.stringify(payload),qos:0,retain:false});
    parser.on('packet',packet=>{
      if(packet.cmd==='connect')send({cmd:'connack',returnCode:0,sessionPresent:false});
      if(packet.cmd==='subscribe'){
        send({cmd:'suback',messageId:packet.messageId,granted:packet.subscriptions.map(()=>denySubscription?128:1)});
        if(denySubscription)return;
        publish('presence',{online:true,protocol:2,barrier:0,session,uptime:10000});
        emitState=()=>publish('state',{session,uptime:10001,captured:10001,state:{ok:true,firmware:'TLS simulator',latestEventId:0,radio:{},addon:{},queue:{}}});
        if(!holdState)emitState();
      }
      if(packet.cmd==='publish'){
        if(packet.qos===1)send({cmd:'puback',messageId:packet.messageId});
        if(packet.topic.endsWith('/command')){
          const envelope=JSON.parse(packet.payload);seen.push(envelope);
          publish('ack',{id:envelope.id,session,accepted:true,acceptedLines:1});
          publish('event',{session,id:1,l:'status',s:'LTE',t:'Executed simulated Ping'});
        }
      }
      if(packet.cmd==='pingreq')send({cmd:'pingresp'});
    });
  });
  try{
    await new Promise(resolve=>broker.listen(0,'127.0.0.1',resolve));
    const reservation=net.createServer();await new Promise(resolve=>reservation.listen(0,'127.0.0.1',resolve));
    const port=reservation.address().port;await new Promise(resolve=>reservation.close(resolve));
    child=spawn(process.execPath,['server.mjs'],{cwd:path.dirname(fileURLToPath(import.meta.url)),env:{...process.env,PORT:String(port),PUBLIC_ORIGIN:'https://field.example.com',INTERNAL_RELAY_KEY:'k'.repeat(40),MQTT_URL:`mqtts://localhost:${broker.address().port}`,MQTT_USER:'relay',MQTT_PASSWORD:'test-only',MQTT_CA_FILE:cert,DEVICE_ID:'test'},stdio:['ignore','pipe','pipe']});
    await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('Relay startup timed out')),5000);child.stdout.on('data',data=>{if(String(data).includes('Field relay ready')){clearTimeout(timer);resolve()}});child.on('exit',code=>{clearTimeout(timer);reject(Error('Relay exited '+code))})});
    const base=`http://127.0.0.1:${port}`,headers={'x-relay-key':'k'.repeat(40)};
    assert.equal((await fetch(base+'/api/state')).status,403);
    let state;
    for(let i=0;i<50;i++){state=await(await fetch(base+'/api/state',{headers})).json();if(state.uplink.online)break;await new Promise(resolve=>setTimeout(resolve,30))}
    assert.equal(state.uplink.online,true);assert.equal(state.uplink.ageMs,null);
    const waitingHeaders={...headers,Origin:'https://field.example.com','Content-Type':'application/octet-stream','X-Request-ID':'waiting-state','X-Relay-Session':state.uplink.relaySession,'X-Device-Session':session};
    const waitingResponse=await fetch(base+'/api/command',{method:'POST',headers:waitingHeaders,body:'Arm'});
    assert.equal(waitingResponse.status,409);assert.match((await waitingResponse.json()).error,/Waiting for current device state/);assert.equal(seen.length,0);
    holdState=false;emitState();
    for(let i=0;i<50;i++){state=await(await fetch(base+'/api/state',{headers})).json();if(state.firmware==='TLS simulator')break;await new Promise(resolve=>setTimeout(resolve,30))}
    assert.equal(state.firmware,'TLS simulator');
    assert.equal((await fetch(base+'/api/health')).status,403);
    const diagnostic=await(await fetch(base+'/api/health',{headers})).json();
    assert.equal(diagnostic.relay.http,'ready');assert.equal(diagnostic.broker.connected,true);assert.equal(diagnostic.broker.phase,'connected');assert.equal(diagnostic.device.phase,'online');
    assert.doesNotMatch(JSON.stringify(diagnostic),/test-only|kkkkkkkk/);
    assert.equal((await checkHealth({port,key:'k'.repeat(40)})).ok,true);
    const html=await(await fetch(base+'/',{headers})).text();assert.match(html,/relay-config.js/);
    const commandHeaders={...headers,Origin:'https://field.example.com','Content-Type':'application/octet-stream','X-Request-ID':'integration','X-Relay-Session':state.uplink.relaySession,'X-Device-Session':session};
    assert.equal((await fetch(base+'/api/command',{method:'POST',headers:{...commandHeaders,Origin:'https://other.example.com'},body:'Ping'})).status,403);
    assert.equal((await fetch(base+'/api/command',{method:'POST',headers:{...commandHeaders,'X-Relay-Session':'old-relay'},body:'Arm'})).status,409);
    assert.equal((await fetch(base+'/api/command',{method:'POST',headers:{...commandHeaders,'X-Device-Session':'previous-device'},body:'Arm'})).status,409);
    assert.equal((await fetch(base+'/api/command',{method:'POST',headers:{...commandHeaders,'X-Device-Session':''},body:'Arm'})).status,409);
    assert.equal(seen.length,0);
    webSocket=new WebSocket(base.replace('http:','ws:')+'/ws',{headers:{...headers,Origin:'https://field.example.com'}});
    const first=await new Promise((resolve,reject)=>{webSocket.once('message',message=>resolve(JSON.parse(message)));webSocket.once('error',reject)});
    assert.equal(first.type,'sync');assert.equal(first.data.state.uplink.online,true);assert.deepEqual(first.data.history.events,[]);
    const event=new Promise(resolve=>webSocket.on('message',message=>{const frame=JSON.parse(message);if(frame.type==='event')resolve(frame.data)}));
    const answer=await(await fetch(base+'/api/command',{method:'POST',headers:commandHeaders,body:'Ping'})).json();
    assert.equal(answer.accepted,true);assert.equal(seen.length,1);assert.equal(seen[0].session,session);
    assert.match((await event).t,/simulated Ping/);
    const scope=`deviceSession=${session}&relaySession=${state.uplink.relaySession}`;
    const history=await(await fetch(base+'/api/events?since=0&'+scope,{headers})).json();
    assert.deepEqual(history.events.map(event=>event.id),[1]);assert.equal(history.reset,false);
    webSocket.terminate();
    webSocket=new WebSocket(base.replace('http:','ws:')+'/ws?since=0&'+scope,{headers:{...headers,Origin:'https://field.example.com'}});
    const resumed=await new Promise((resolve,reject)=>{webSocket.once('message',message=>resolve(JSON.parse(message)));webSocket.once('error',reject)});
    assert.equal(resumed.type,'sync');assert.equal(resumed.data.history.cursor,1);assert.equal(resumed.data.history.events[0].t,'Executed simulated Ping');
    const duplicate=await(await fetch(base+'/api/command',{method:'POST',headers:commandHeaders,body:'Ping'})).json();
    assert.equal(duplicate.accepted,true);assert.equal(seen.length,1);
    // Ordinary traffic reaches its limit; Stop must still cross the relay.
    for(let i=0;i<57;i++)assert.equal((await fetch(base+'/api/command',{method:'POST',headers:{...commandHeaders,'X-Request-ID':'rate'+i},body:'Ping'})).status,200);
    assert.equal((await fetch(base+'/api/command',{method:'POST',headers:{...commandHeaders,'X-Request-ID':'limited'},body:'Ping'})).status,429);
    const stopped=await(await fetch(base+'/api/command',{method:'POST',headers:{...commandHeaders,'X-Request-ID':'priority-stop','X-Device-Session':'previous-device'},body:'StopAll'})).json();
    assert.equal(stopped.accepted,true);assert.equal(seen.at(-1).body,'StopAll');assert.equal(seen.at(-1).barrier,1);
    // A rejected subscription is an operator diagnostic, not HTTP failure.
    denySubscription=true;for(const socket of sockets)socket.destroy();
    let offline;
    for(let i=0;i<120;i++){
      offline=await(await fetch(base+'/api/health',{headers})).json();
      if(offline.broker.phase==='subscription-error')break;
      await new Promise(resolve=>setTimeout(resolve,30));
    }
    assert.equal(offline.broker.phase,'subscription-error');assert.equal(offline.broker.connected,false);assert.equal(offline.device.connected,false);
    assert.match(offline.broker.message,/topic permissions/);
    assert.equal((await checkHealth({port,key:'k'.repeat(40)})).ok,true);
    // Missing mounted console files must fail before the MQTT connection starts.
    const startup=await new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>reject(Error('Missing-assets startup check timed out')),3000);let output='';
      missingAssetsChild=spawn(process.execPath,['server.mjs'],{cwd:path.dirname(fileURLToPath(import.meta.url)),env:{...process.env,PORT:String(port),PUBLIC_ORIGIN:'https://field.example.com',INTERNAL_RELAY_KEY:'k'.repeat(40),MQTT_URL:'mqtts://localhost:1',WEB_DIRECTORY:path.join(directory,'missing-web')},stdio:['ignore','pipe','pipe']});
      missingAssetsChild.stderr.on('data',data=>output+=data);
      missingAssetsChild.on('error',error=>{clearTimeout(timer);reject(error)});
      missingAssetsChild.on('exit',code=>{clearTimeout(timer);resolve({code,output})});
    });
    assert.notEqual(startup.code,0);assert.match(startup.output,/Missing console assets/);assert.doesNotMatch(startup.output,/test-only|kkkkkkkk/);

  }finally{
    webSocket?.terminate();child?.kill();missingAssetsChild?.kill();for(const socket of sockets)socket.destroy();
    await new Promise(resolve=>broker.close(resolve));
    // The target is the unique temporary directory created above.
    const resolved=path.resolve(directory),tempRoot=path.resolve(os.tmpdir())+path.sep;
    if(!resolved.startsWith(tempRoot)||!path.basename(resolved).startsWith('field-relay-'))throw Error('Unexpected temporary test path');
    fs.rmSync(resolved,{recursive:true,force:true});
  }
});
