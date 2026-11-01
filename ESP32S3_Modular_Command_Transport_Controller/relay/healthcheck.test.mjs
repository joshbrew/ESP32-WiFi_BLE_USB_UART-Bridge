import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import {checkHealth} from './healthcheck.mjs';
import {brokerFailure} from './broker-health.mjs';

test('broker diagnostics classify errors without copying credentials or raw errors',()=>{
  const secret='private-user:private-password@private-broker';
  for(const [code,phase] of [['5','auth-error'],['CERT_HAS_EXPIRED','tls-error'],['ENOTFOUND','dns-error'],['ECONNREFUSED','connection-error']]){
    const status=brokerFailure({code,message:secret});assert.equal(status.phase,phase);assert(!JSON.stringify(status).includes(secret));
  }
});

test('HTTP health remains valid without a device; bad responses and timeouts fail',{timeout:5000},async()=>{
  let mode='healthy';
  const server=http.createServer((request,response)=>{
    if(request.headers['x-relay-key']!=='health-test-key'){response.writeHead(403).end();return}
    if(mode==='hold')return;
    if(mode==='broken'){response.end('{');return}
    if(mode==='large'){response.end('x'.repeat(70000));return}
    const ok=mode==='healthy';response.writeHead(ok?200:503,{'Content-Type':'application/json'});
    response.end(JSON.stringify({ok,relay:{http:ok?'ready':'assets-unavailable'},broker:{connected:false},device:{connected:false,phase:'waiting-relay'}}));
  });
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const options={port:server.address().port,key:'health-test-key',timeout:100};
  try{
    const healthy=await checkHealth(options);assert.equal(healthy.device.connected,false);assert.equal(healthy.relay.http,'ready');
    await assert.rejects(checkHealth({...options,key:'wrong'}));
    for(const next of ['broken','large','not-ready','hold']){mode=next;await assert.rejects(checkHealth(options),/health|ready/i)}
    await assert.rejects(checkHealth({...options,key:''}),/key is missing/);
  }finally{server.closeAllConnections();await new Promise(resolve=>server.close(resolve))}
});
