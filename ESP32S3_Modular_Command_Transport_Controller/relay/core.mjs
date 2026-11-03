import {EventEmitter} from 'node:events';
import {createHash,randomUUID} from 'node:crypto';

const failure=(status,message)=>Object.assign(new Error(message),{status});
const uint32=value=>Number.isInteger(value)&&value>=0&&value<=0xffffffff;
export const isPriorityStop=body=>/^(StopAll|Disarm|DispenserDisarm|DispenserOff|RoutineStop|GeoStop|DispenseStop|GPIO26:OFF)$/i.test(body.trim());
export class RelayCore extends EventEmitter {
  constructor({device,publish,now=Date.now,ttl=3500}){
    super();if(!/^[a-zA-Z0-9_-]{1,40}$/.test(device))throw Error('Invalid DEVICE_ID');
    this.device=device;this.topic=`devices/${device}/`;this.publish=publish;this.now=now;this.ttl=ttl;
    this.brokerOnline=false;this.deviceOnline=false;this.session=null;this.lastSeen=0;this.lastState=0;
    this.uptime=0;this.snapshot=null;this.events=[];this.requests=new Map();
    this.barrier=0;this.relaySession=randomUUID();this.linkError=null;this.stopPending=false;
    this.droppedThrough=0;this.lastEventId=0;
    this.transport='LTE';this.startedAt=this.now();this.brokerReason='Relay link is connecting';
  }
  online(){return this.brokerOnline&&this.deviceOnline&&this.now()-this.lastSeen<20000}
  state(){
    const snapshot=this.snapshot||{ok:true,firmware:'LTE field relay',version:'Waiting for device',radio:{bootModeActive:'LTE',wifiCompiled:false,bleCompiled:false,sppCompiled:false},addon:{name:'none',active:false},send:{},queue:{waiting:0,capacity:8}};
    return {...snapshot,uplink:{transport:this.transport,online:this.online(),brokerOnline:this.brokerOnline,session:this.session,relaySession:this.relaySession,reason:this.brokerOnline?this.linkError:this.brokerReason,stopPending:this.stopPending,ageMs:this.lastState?this.now()-this.lastState:null,stale:!this.lastState||this.now()-this.lastState>5000}};
  }
  setBroker(online,reason='Relay link is reconnecting'){this.brokerOnline=online;this.brokerReason=online?null:reason;if(!online){this.deviceOnline=false;this.failPending('Broker connection lost; command delivery is unconfirmed')}this.emit('state',this.state())}
  health(){
    const uplink=this.state().uplink;
    const phase=!this.brokerOnline?'waiting-relay':this.linkError?'protocol-error':!uplink.online?(this.session?'offline':'waiting-device'):this.stopPending?'stop-unconfirmed':uplink.ageMs===null?'waiting-state':uplink.stale?'stale-state':'online';
    return {ok:true,relay:{http:'ready',uptimeSeconds:Math.max(0,Math.floor((this.now()-this.startedAt)/1000))},broker:{connected:this.brokerOnline},device:{id:this.device,connected:uplink.online,phase,stateAgeMs:uplink.ageMs,stopPending:this.stopPending}};
  }
  failPending(message){for(const record of this.requests.values())if(!record.done)record.finish(failure(504,message))}
  history({since=null,deviceSession=null,relaySession=null,limit=16,tail=false,maxBytes=24000}={}){
    const scoped=(!deviceSession&&!relaySession)||(deviceSession===this.session&&relaySession===this.relaySession);
    const reset=!scoped||!uint32(since);
    let cursor=reset?0:since,gap=cursor<this.droppedThrough;
    const candidates=this.events.filter(event=>event.id>cursor).map(({receivedAt,...event})=>({...event,ageMs:Math.max(0,event.ageMs+this.now()-receivedAt)}));
    let events=[];
    if(tail){
      events=candidates.slice();
      while(events.length&&Buffer.byteLength(JSON.stringify(events))>maxBytes){events.shift();gap=true}
      cursor=events.at(-1)?.id??candidates.at(-1)?.id??cursor;
    }else{
      let bytes=2;
      for(const event of candidates){
        if(events.length>=limit)break;
        const size=Buffer.byteLength(JSON.stringify(event))+1;
        if(size>maxBytes){if(events.length)break;cursor=event.id;gap=true;continue}
        if(bytes+size>maxBytes)break;
        events.push(event);bytes+=size;cursor=event.id;
      }
    }
    return {ok:true,events,cursor,gap,reset,deviceSession:this.session,relaySession:this.relaySession,more:!tail&&candidates.some(event=>event.id>cursor)};
  }
  ingest(topic,payload,{retain=false}={}){
    if(!topic.startsWith(this.topic)||payload.length>12288)return;
    let data;try{data=JSON.parse(payload)}catch{return}
    if(!data||typeof data!=='object'||Array.isArray(data))return;
    const kind=topic.slice(this.topic.length);
    if(kind==='presence'){
      if(data.online===false){this.deviceOnline=false;this.failPending('Device disconnected; command delivery is unconfirmed');this.emit('state',this.state());return}
      if(retain||data.online!==true||!/^[a-f0-9]{32}$/.test(data.session)||!uint32(data.uptime))return;
      if(data.protocol!==2||!uint32(data.barrier)){
        this.deviceOnline=false;this.linkError='Device firmware needs LTE protocol 2';
        this.failPending('Device protocol changed; command delivery is unconfirmed');this.emit('state',this.state());return;
      }
      this.linkError=null;this.transport=data.transport==='WiFi'?'WiFi':'LTE';
      if(this.session!==data.session){this.failPending('Device session changed; old commands were cancelled');this.events=[];this.droppedThrough=0;this.lastEventId=0;this.snapshot=null;this.lastState=0;this.session=data.session;this.barrier=data.barrier}
      // A presence frame sent just before Stop must not lower its new barrier.
      if(((data.barrier-this.barrier)|0)>0)this.barrier=data.barrier;
      this.uptime=data.uptime;this.lastSeen=this.now();this.deviceOnline=true;this.emit('state',this.state());
    } else if(kind==='state'){
      if(retain||this.linkError||data.session!==this.session||!data.state||typeof data.state!=='object'||Array.isArray(data.state)||!uint32(data.uptime)||!uint32(data.captured))return;
      const age=(data.uptime-data.captured)>>>0;if(age>0x7fffffff)return;
      this.snapshot=data.state;this.uptime=data.uptime>>>0;this.lastSeen=this.now();this.lastState=this.lastSeen-age;this.deviceOnline=true;this.emit('state',this.state());
    } else if(kind==='event'){
      if(retain||data.session!==this.session||!this.online()||!uint32(data.id)||data.id<=this.lastEventId||typeof data.t!=='string'||data.t.length>1024)return;
      const bounded=(value,size,fallback)=>typeof value==='string'?value.slice(0,size):fallback;
      const event={id:data.id,t:data.t,l:bounded(data.l,16,'info'),s:bounded(data.s,32,'LTE'),r:bounded(data.r,23,''),q:data.q===true,gap:data.gap===true,ageMs:uint32(data.ageMs)?data.ageMs:0};
      this.lastEventId=event.id;this.events.push({...event,receivedAt:this.now()});
      if(this.events.length>128)this.droppedThrough=this.events.shift().id;
      this.emit('event',event);
    } else if(kind==='ack'){
      if(retain||data.session!==this.session)return;
      const record=this.requests.get(data.id);if(!record||record.session!==this.session)return;
      // A timed-out HTTP request stays unconfirmed, but a late valid reply to
      // the newest Stop can still clear the operator's pending-Stop state.
      if(record.priority&&record.barrier===this.barrier&&data.accepted===true&&this.stopPending){this.stopPending=false;this.emit('state',this.state())}
      if(record.done)return;
      record.finish(null,{accepted:data.accepted===true,acceptedLines:Math.max(0,Math.min(8,Number(data.acceptedLines)||0)),latestEventId:Number(data.latestEventId)||0,...(data.error?{error:String(data.error).slice(0,200)}:{})});
    }
  }
  watch(){if(this.online())this.publish(this.topic+'watch',JSON.stringify({session:this.session}),{qos:0,retain:false})}
  command(id,body){
    if(!/^[a-zA-Z0-9_.-]{1,23}$/.test(id)||!body.trim()||body.includes('\0')||Buffer.byteLength(body)>2048)return Promise.reject(failure(400,'Invalid request ID or command size'));
    const now=this.now(),hash=createHash('sha256').update(body).digest('hex');
    for(const [key,record] of this.requests)if(record.done&&now-record.at>30000)this.requests.delete(key);
    const old=this.requests.get(id);
    if(old)return old.hash===hash?old.promise:Promise.reject(failure(409,'Request ID was already used for different commands'));
    if(!this.online())return Promise.reject(failure(503,'Device is offline; commands are not queued'));
    const priority=isPriorityStop(body);
    if(!priority&&this.snapshot===null)return Promise.reject(failure(409,'Waiting for current device state; commands are not queued'));
    if(!priority&&this.stopPending)return Promise.reject(failure(409,'Waiting for Stop confirmation. Use Stop all again before sending more commands.'));
    if(priority){
      // Reserve history capacity for stop requests. Repeating an evicted stop
      // can only stop again; never evict an actuation request's dedup record.
      if(this.requests.size>=128){
        const removable=[...this.requests].find(([,record])=>record.done&&record.priority);
        if(removable)this.requests.delete(removable[0]);
      }
      if(this.requests.size>=128)return Promise.reject(failure(429,'Remote stop history is busy'));
      this.barrier=(this.barrier+1)>>>0;
      this.stopPending=true;
      this.failPending('Stop requested; earlier command delivery is unconfirmed');
      this.emit('state',this.state());
    }else if(this.requests.size>=120||[...this.requests.values()].filter(record=>!record.done).length>=4)return Promise.reject(failure(429,'Remote command queue is busy'));
    const deadline=(this.uptime+Math.max(0,now-this.lastSeen)+this.ttl)>>>0;
    const envelope=JSON.stringify({id,body,session:this.session,deadline,barrier:this.barrier});
    let resolve,reject;
    const promise=new Promise((yes,no)=>{resolve=yes;reject=no});
    const record={hash,promise,at:now,session:this.session,priority,barrier:this.barrier,done:false,timers:[],finish:(error,result)=>{
      if(record.done)return;record.done=true;record.timers.forEach(clearTimeout);error?reject(error):resolve(result);
    }};
    this.requests.set(id,record);
    const send=()=>{if(!record.done&&this.online()&&record.session===this.session)this.publish(this.topic+'command',envelope,{qos:1,retain:false},error=>{if(error)record.finish(failure(504,'Publish failed; command delivery is unconfirmed'))})};
    record.timers.push(setTimeout(()=>record.finish(failure(504,'Device acknowledgement timed out; check output before sending again')),this.ttl));
    record.timers.push(setTimeout(send,Math.min(1100,this.ttl/2)));
    send();return promise;
  }
}
