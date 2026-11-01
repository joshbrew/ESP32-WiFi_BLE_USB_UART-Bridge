import test from 'node:test';
import assert from 'node:assert/strict';
import {RelayCore} from './core.mjs';
const session='a'.repeat(32);
function fixture(ttl=100,withState=true){const sent=[];let now=100000;const relay=new RelayCore({device:'test',ttl,now:()=>now,publish:(topic,body,options,callback)=>{sent.push({topic,data:JSON.parse(body),options});callback?.()}});relay.setBroker(true);relay.ingest('devices/test/presence',JSON.stringify({online:true,protocol:2,barrier:0,session,uptime:10000}));if(withState)relay.ingest('devices/test/state',JSON.stringify({session,uptime:10000,captured:10000,state:{ok:true}}));return {relay,sent,advance:ms=>now+=ms}}
test('offline commands are rejected rather than queued',async()=>{const {relay,sent}=fixture();relay.setBroker(false);await assert.rejects(relay.command('one','Arm'),/offline/);assert.equal(sent.length,0)});
test('command IDs deduplicate retries and detect changed bodies',async()=>{const {relay,sent}=fixture();const a=relay.command('one','Ping'),b=relay.command('one','Ping');assert.equal(a,b);assert.equal(sent.length,1);assert.equal(sent[0].options.retain,false);assert.equal(sent[0].options.qos,1);await assert.rejects(relay.command('one','Arm'),/different/);relay.ingest('devices/test/ack',JSON.stringify({id:'one',session,accepted:true,acceptedLines:1}));assert.equal((await a).accepted,true)});
test('expiry and changing sessions fail pending delivery',async()=>{const {relay}=fixture(30);await assert.rejects(relay.command('expiry','Status'),/timed out/);const pending=relay.command('changed','Ping');relay.ingest('devices/test/presence',JSON.stringify({online:true,protocol:2,barrier:0,session:'b'.repeat(32),uptime:0}));await assert.rejects(pending,/session changed/);await assert.rejects(relay.command('changed','Ping'),/session changed/)});
test('retained online status does not establish liveness; stale data is explicit',()=>{const {relay,advance}=fixture();advance(21000);assert.equal(relay.online(),false);relay.ingest('devices/test/presence',JSON.stringify({online:true,protocol:2,barrier:0,session,uptime:1}),{retain:true});assert.equal(relay.online(),false);assert.equal(relay.state().uplink.stale,true)});
test('old-session and retained acknowledgements cannot confirm commands',async()=>{const {relay}=fixture(30);const pending=relay.command('one','Ping');relay.ingest('devices/test/ack',JSON.stringify({id:'one',session:'b'.repeat(32),accepted:true}));relay.ingest('devices/test/ack',JSON.stringify({id:'one',session,accepted:true}),{retain:true});await assert.rejects(pending,/timed out/)});

test('reboot clears old state and rejects events from the previous connection',()=>{
  const {relay}=fixture();
  relay.ingest('devices/test/state',JSON.stringify({session,uptime:10001,captured:10001,state:{dispenser:{armed:true}}}));
  assert.equal(relay.state().dispenser.armed,true);
  relay.ingest('devices/test/presence',JSON.stringify({online:true,protocol:2,barrier:0,session:'b'.repeat(32),uptime:0}));
  assert.equal(relay.state().dispenser,undefined);
  relay.ingest('devices/test/event',JSON.stringify({session,id:1,t:'Old event'}));assert.equal(relay.events.length,0);
});

test('Stop bypasses a full normal queue and invalidates older retry barriers',async()=>{
  const {relay,sent}=fixture(5000);
  const pending=Array.from({length:4},(_,i)=>relay.command('old'+i,'Arm').catch(error=>error));
  await assert.rejects(relay.command('overflow','Arm'),/busy/);
  const stopping=relay.command('stop','StopAll');
  for(const result of await Promise.all(pending))assert.match(result.message,/Stop requested/);
  assert.equal(sent.at(-1).data.barrier,1);assert.equal(sent.at(-1).data.body,'StopAll');
  assert(sent.slice(0,4).every(frame=>frame.data.barrier===0));
  relay.ingest('devices/test/ack',JSON.stringify({id:'stop',session,accepted:true,acceptedLines:1}));
  assert.equal((await stopping).accepted,true);
  const before=sent.length;await assert.rejects(relay.command('old0','Arm'),/Stop requested/);
  assert.equal(sent.length,before);
});

test('Stop retains admission when normal request history is full',async()=>{
  const {relay,sent}=fixture(5000);
  for(let i=0;i<120;i++){
    const pending=relay.command('normal'+i,'Ping');
    relay.ingest('devices/test/ack',JSON.stringify({id:'normal'+i,session,accepted:true}));await pending;
  }
  await assert.rejects(relay.command('full','Ping'),/busy/);
  for(let i=0;i<12;i++){
    const stopping=relay.command('stop'+i,'StopAll');
    relay.ingest('devices/test/ack',JSON.stringify({id:'stop'+i,session,accepted:true}));await stopping;
  }
  assert.equal(relay.requests.size,128);assert.equal(sent.at(-1).data.barrier,12);
  assert(relay.requests.has('normal0'),'normal dedup history must survive stop traffic');
});

test('relay resumes the device barrier after restarting, including wraparound',async()=>{
  const {relay,sent}=fixture(5000);
  relay.ingest('devices/test/presence',JSON.stringify({online:true,protocol:2,barrier:0xffffffff,session:'b'.repeat(32),uptime:10}));
  const stopping=relay.command('wrap','Disarm');assert.equal(sent.at(-1).data.barrier,0);
  relay.ingest('devices/test/presence',JSON.stringify({online:true,protocol:2,barrier:0xffffffff,session:'b'.repeat(32),uptime:11}));
  assert.equal(relay.barrier,0,'an older in-flight presence must not lower the stop barrier');
  relay.ingest('devices/test/ack',JSON.stringify({id:'wrap',session:'b'.repeat(32),accepted:true}));await stopping;
});

test('old LTE firmware is shown offline with an explicit protocol reason',()=>{
  const {relay}=fixture();
  relay.ingest('devices/test/presence',JSON.stringify({online:true,session,uptime:10000}));
  assert.equal(relay.online(),false);assert.match(relay.state().uplink.reason,/protocol 2/);
  relay.ingest('devices/test/state',JSON.stringify({session,uptime:10001,captured:10001,state:{}}));
  assert.equal(relay.online(),false);
});

test('an unconfirmed Stop blocks new normal commands until a Stop is acknowledged',async()=>{
  const {relay}=fixture(30);
  await assert.rejects(relay.command('lost-stop','StopAll'),/timed out/);
  assert.equal(relay.state().uplink.stopPending,true);
  await assert.rejects(relay.command('new-arm','Arm'),/Waiting for Stop confirmation/);
  const retry=relay.command('retry-stop','StopAll');
  relay.ingest('devices/test/ack',JSON.stringify({id:'lost-stop',session,accepted:true}));
  assert.equal(relay.state().uplink.stopPending,true,'an older stop acknowledgement cannot clear a newer barrier');
  relay.ingest('devices/test/ack',JSON.stringify({id:'retry-stop',session,accepted:true}));await retry;
  assert.equal(relay.state().uplink.stopPending,false);
  const ping=relay.command('new-ping','Ping');
  relay.ingest('devices/test/ack',JSON.stringify({id:'new-ping',session,accepted:true}));await ping;
});

test('modem presence cannot make an old control snapshot look fresh',()=>{
  const {relay,advance}=fixture();
  relay.ingest('devices/test/state',JSON.stringify({session,uptime:10000,captured:3000,state:{dispenser:{armed:false}}}));
  assert.equal(relay.state().uplink.ageMs,7000);assert.equal(relay.state().uplink.stale,true);
  advance(2000);relay.ingest('devices/test/presence',JSON.stringify({online:true,protocol:2,barrier:0,session,uptime:12000}));
  assert.equal(relay.state().uplink.ageMs,9000);assert.equal(relay.online(),true);
});

test('output resumes after the last displayed event without duplicate or private fields',()=>{
  const {relay,advance}=fixture();
  const event=(id,t='Output')=>relay.ingest('devices/test/event',JSON.stringify({session,id,t,ageMs:200,unused:'x'.repeat(6000)}));
  event(7);event(10);event(10,'Duplicate');event(12);
  advance(500);
  const history=relay.history({since:7,deviceSession:session,relaySession:relay.relaySession});
  assert.deepEqual(history.events.map(event=>event.id),[10,12]);
  assert.equal(history.cursor,12);assert.equal(history.gap,false);assert.equal(history.reset,false);
  assert.equal(history.events[0].ageMs,700);assert.equal(history.events[0].unused,undefined);assert.equal(history.events[0].receivedAt,undefined);
});

test('history reports actual eviction and resets cursors from an older connection',()=>{
  const {relay}=fixture();
  for(let id=1;id<=130;id++)relay.ingest('devices/test/event',JSON.stringify({session,id,t:'Output '+id}));
  const first=relay.history({since:0,limit:16});
  assert.equal(first.gap,true);assert.equal(first.events[0].id,3);assert.equal(first.cursor,18);assert.equal(first.more,true);
  const second=relay.history({since:first.cursor,limit:16});
  assert.equal(second.gap,false);assert.equal(second.events[0].id,19);
  const reset=relay.history({since:900,deviceSession:'old-session',relaySession:relay.relaySession});
  assert.equal(reset.reset,true);assert.equal(reset.events[0].id,3);
});

test('stream replay is byte bounded for Unicode output and marks omitted history',()=>{
  const {relay}=fixture();
  for(let id=1;id<=20;id++)relay.ingest('devices/test/event',JSON.stringify({session,id,t:'💧'.repeat(256)}));
  const history=relay.history({since:0,tail:true,maxBytes:3500});
  assert(Buffer.byteLength(JSON.stringify(history.events))<=3500);
  assert.equal(history.cursor,20);assert.equal(history.gap,true);assert.equal(history.more,false);assert(history.events.length>0&&history.events.length<20);
  const empty=relay.history({since:0,tail:true,maxBytes:10});assert.equal(empty.cursor,20);assert.equal(empty.events.length,0);assert.equal(empty.gap,true);
});

test('new device sessions cannot reuse an old output cursor',()=>{
  const {relay}=fixture();
  relay.ingest('devices/test/event',JSON.stringify({session,id:99,t:'Old output'}));
  const next='b'.repeat(32);
  relay.ingest('devices/test/presence',JSON.stringify({online:true,protocol:2,barrier:0,session:next,uptime:0}));
  const history=relay.history({since:99,deviceSession:session,relaySession:relay.relaySession});
  assert.equal(history.reset,true);assert.equal(history.cursor,0);assert.deepEqual(history.events,[]);
});

test('a late acknowledgement of the newest Stop clears its pending indicator',async()=>{
  const {relay}=fixture(25);
  await assert.rejects(relay.command('late-stop','StopAll'),/timed out/);
  assert.equal(relay.health().device.phase,'stop-unconfirmed');
  relay.ingest('devices/test/ack',JSON.stringify({id:'late-stop',session,accepted:true}));
  assert.equal(relay.state().uplink.stopPending,false);
  await assert.rejects(relay.command('late-stop','StopAll'),/timed out/,'the original timed-out request keeps its result');
  const next=relay.command('after-late','Ping');
  relay.ingest('devices/test/ack',JSON.stringify({id:'after-late',session,accepted:true}));await next;
});

test('health separates relay connectivity, missing state and stale state',()=>{
  const {relay,advance}=fixture(100,false);
  assert.equal(relay.health().device.phase,'waiting-state');
  relay.ingest('devices/test/state',JSON.stringify({session,uptime:10000,captured:10000,state:{}}));
  assert.equal(relay.health().device.phase,'online');advance(6000);
  assert.equal(relay.health().device.phase,'stale-state');
  relay.setBroker(false);
  assert.equal(relay.health().device.phase,'waiting-relay');assert.equal(relay.state().uplink.brokerOnline,false);
  assert.match(relay.state().uplink.reason,/Relay link/);
});

test('presence alone cannot admit commands, while Stop remains available',async()=>{
  const {relay,sent}=fixture(100,false);
  await assert.rejects(relay.command('waiting','Arm'),/Waiting for current device state/);assert.equal(sent.length,0);
  const stop=relay.command('waiting-stop','StopAll');relay.ingest('devices/test/ack',JSON.stringify({id:'waiting-stop',session,accepted:true}));await stop;
  relay.ingest('devices/test/state',JSON.stringify({session,uptime:10001,captured:10001,state:{ok:true}}));
  const ready=relay.command('ready','Ping');relay.ingest('devices/test/ack',JSON.stringify({id:'ready',session,accepted:true}));await ready;
  const next='c'.repeat(32);relay.ingest('devices/test/presence',JSON.stringify({online:true,protocol:2,barrier:0,session:next,uptime:0}));
  await assert.rejects(relay.command('reconnected','Arm'),/Waiting for current device state/);
});
