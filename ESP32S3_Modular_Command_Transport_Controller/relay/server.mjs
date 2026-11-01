import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {timingSafeEqual} from 'node:crypto';
import mqtt from 'mqtt';
import {WebSocketServer} from 'ws';
import {RelayCore,isPriorityStop} from './core.mjs';
import {brokerFailure} from './broker-health.mjs';

const required=name=>{const value=process.env[name];if(!value)throw Error(`Set ${name}`);return value};
const origin=required('PUBLIC_ORIGIN'),key=required('INTERNAL_RELAY_KEY'),url=required('MQTT_URL');
if(!origin.startsWith('https://')||new URL(origin).origin!==origin||key.length<32||!url.startsWith('mqtts://'))throw Error('HTTPS origin, a strong internal key, and mqtts:// are required');
const web=path.resolve(process.env.WEB_DIRECTORY||path.join(path.dirname(fileURLToPath(import.meta.url)),'../web'));
const files={'/':'index.html','/app.css':'app.css','/app.js':'app.js'};
function webReady(){try{for(const name of Object.values(files)){const file=path.join(web,name);fs.accessSync(file,fs.constants.R_OK);if(!fs.statSync(file).isFile())return false}return true}catch{return false}}
if(!webReady())throw Error('Missing console assets. Set WEB_SOURCE to a folder containing index.html, app.css and app.js.');

const client=mqtt.connect(url,{username:required('MQTT_USER'),password:required('MQTT_PASSWORD'),ca:fs.readFileSync(required('MQTT_CA_FILE')),rejectUnauthorized:true,clean:true,reconnectPeriod:2000,connectTimeout:10000,queueQoSZero:false,protocolVersion:4});
const relay=new RelayCore({device:required('DEVICE_ID'),publish:(...args)=>client.publish(...args)});
const authenticated=req=>{const actual=Buffer.from(String(req.headers['x-relay-key']||'')),expected=Buffer.from(key);return actual.length===expected.length&&timingSafeEqual(actual,expected)};
let brokerStatus={phase:'connecting',message:'Connecting to broker',retries:0};
let subscriptionRetry=null;
function subscribe(){
  if(!client.connected)return;
  brokerStatus={phase:'subscribing',message:'Waiting for broker subscription',retries:brokerStatus.retries};
  client.subscribe(relay.topic+'+',{qos:1},(error,granted)=>{
    if(error||!granted?.length||granted.some(item=>item.qos>=128)){
      brokerStatus={phase:'subscription-error',message:'Broker subscription was rejected. Check the relay topic permissions.',retries:brokerStatus.retries};
      relay.setBroker(false,'Relay link is unavailable');clearTimeout(subscriptionRetry);subscriptionRetry=setTimeout(subscribe,5000);return;
    }
    brokerStatus={phase:'connected',message:'Broker connected and subscribed',retries:brokerStatus.retries};relay.setBroker(true);
  });
}
client.on('connect',()=>{clearTimeout(subscriptionRetry);subscribe()});
client.on('reconnect',()=>{brokerStatus.retries++});
client.on('close',()=>{clearTimeout(subscriptionRetry);if(['connected','subscribing'].includes(brokerStatus.phase))brokerStatus={phase:'reconnecting',message:'Reconnecting to broker',retries:brokerStatus.retries};relay.setBroker(false)});
client.on('error',error=>{brokerStatus={...brokerFailure(error),retries:brokerStatus.retries};relay.setBroker(false,'Relay link is unavailable')});
client.on('message',(topic,payload,packet)=>relay.ingest(topic,payload,{retain:packet.retain}));
function json(res,status,body){res.writeHead(status,{'Content-Type':'application/json','Cache-Control':'no-store','X-Content-Type-Options':'nosniff'});res.end(JSON.stringify(body))}
const rates=new Map();
const server=http.createServer(async(req,res)=>{
  if(!authenticated(req)){json(res,403,{error:'Access denied'});return}
  let route;try{route=new URL(req.url,'http://relay').pathname}catch{json(res,400,{error:'Invalid request URL'});return}
  if(req.method==='GET'&&['/healthz','/api/health'].includes(route)){const health=relay.health();health.broker={...health.broker,...brokerStatus};health.ok=webReady();health.relay.http=health.ok?'ready':'assets-unavailable';json(res,route==='/healthz'&&!health.ok?503:200,health);return}
  if(req.method==='GET'&&route==='/api/ping'){json(res,200,{ok:true});return}
  if(req.method==='GET'&&route==='/api/state'){relay.watch();json(res,200,relay.state());return}
  if(req.method==='GET'&&route==='/api/events'){
    const params=new URL(req.url,'http://relay').searchParams;
    json(res,200,relay.history({since:params.has('since')?Number(params.get('since')):null,limit:Math.max(1,Math.min(16,Number(params.get('limit'))||4)),deviceSession:params.get('deviceSession'),relaySession:params.get('relaySession')}));return;
  }
  if(req.method==='POST'&&route==='/api/command'){
    if(req.headers.origin!==origin||!String(req.headers['content-type']).startsWith('application/octet-stream')){json(res,403,{error:'Invalid command origin or content type'});return}
    if(req.headers['x-relay-session']!==relay.relaySession){json(res,409,{error:'Relay restarted. Review the current device state before sending again.',code:'RELAY_RESTARTED'});return}
    const chunks=[];let size=0;req.setTimeout(10000,()=>req.destroy());
    try {
      for await(const chunk of req){size+=chunk.length;if(size>2048){json(res,413,{error:'Command batch too large'});return}chunks.push(chunk)}
      const body=Buffer.concat(chunks).toString('utf8');
      if(!isPriorityStop(body)&&req.headers['x-device-session']!==relay.session){json(res,409,{error:'Device reconnected. Review the current state before sending again.',code:'DEVICE_RECONNECTED'});return}
      // Stop bypasses the normal operator rate limit, while message sizes,
      // authentication and bounded request history still apply.
      if(!isPriorityStop(body)){
        const address=String(req.headers['x-forwarded-for']||req.socket.remoteAddress).split(',')[0];
        const now=Date.now();for(const [ip,rate] of rates)if(now-rate.at>=60000)rates.delete(ip);
        const rate=rates.get(address)||{at:now,count:0};
        if((rates.size>=64&&!rates.has(address))||++rate.count>60){json(res,429,{error:'Command rate limit exceeded'});return}rates.set(address,rate);
      }
      const answer=await relay.command(String(req.headers['x-request-id']||''),body.trim());json(res,200,answer);
    } catch(error){if(!res.destroyed)json(res,error.status||400,{error:error.status?error.message:'Invalid command request'})}
    return;
  }
  if(route==='/api/ota'){json(res,501,{error:'Remote firmware updates are not enabled; use local Wi-Fi or USB'});return}
  if(req.method==='GET'&&route==='/relay-config.js'){res.writeHead(200,{'Content-Type':'application/javascript','Cache-Control':'no-store'});res.end('window.ESP32_REMOTE='+JSON.stringify({wsPath:'/ws',session:relay.relaySession})+';');return}
  if(req.method!=='GET'||!files[route]){json(res,404,{error:'Not found'});return}
  let content;try{content=fs.readFileSync(path.join(web,files[route]),'utf8')}catch{json(res,503,{error:'Console assets unavailable. Check WEB_SOURCE on the host.'});return}
  if(route==='/')content=content.replace('<script src="/app.js"></script>','<script src="/relay-config.js"></script><script src="/app.js"></script>');
  res.writeHead(200,{'Content-Type':route==='/'?'text/html; charset=utf-8':route.endsWith('.css')?'text/css':'application/javascript','Cache-Control':'no-store','X-Content-Type-Options':'nosniff','Content-Security-Policy':"default-src 'self'; connect-src 'self' wss:; script-src 'self'; style-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"});res.end(content);
});
const ws=new WebSocketServer({noServer:true,maxPayload:256});
server.on('upgrade',(req,socket,head)=>{
  let url;try{url=new URL(req.url,'http://relay')}catch{socket.destroy();return}
  if(!authenticated(req)||url.pathname!=='/ws'||req.headers.origin!==origin||ws.clients.size>=16){socket.end('HTTP/1.1 403 Forbidden\r\n\r\n');return}
  ws.handleUpgrade(req,socket,head,connection=>ws.emit('connection',connection,url.searchParams));
});
function push(type,data){const frame=JSON.stringify({type,data});for(const connection of ws.clients){if(connection.bufferedAmount>65536)connection.terminate();else if(connection.readyState===1)connection.send(frame)}}
relay.on('state',data=>push('state',data));relay.on('event',data=>push('event',{...data,deviceSession:relay.session,relaySession:relay.relaySession}));
ws.on('connection',(connection,params)=>{
  connection.alive=true;connection.on('pong',()=>connection.alive=true);
  const history=relay.history({since:params.has('since')?Number(params.get('since')):null,deviceSession:params.get('deviceSession'),relaySession:params.get('relaySession'),tail:true});
  // A single ordered frame establishes the scope and replays bounded history
  // before live events can arrive. It stays below the slow-client buffer cap.
  connection.send(JSON.stringify({type:'sync',data:{state:relay.state(),history}}));relay.watch();
});
const timer=setInterval(()=>{if(ws.clients.size)relay.watch();for(const connection of ws.clients){if(!connection.alive)connection.terminate();else{connection.alive=false;connection.ping()}}push('state',relay.state())},5000);
server.listen(Number(process.env.PORT)||8080,'0.0.0.0',()=>console.log('Field relay ready behind the authenticated HTTPS proxy'));
process.on('SIGTERM',()=>{clearInterval(timer);clearTimeout(subscriptionRetry);relay.failPending('Relay is shutting down');client.end(true);ws.close();server.close(()=>process.exit(0))});
