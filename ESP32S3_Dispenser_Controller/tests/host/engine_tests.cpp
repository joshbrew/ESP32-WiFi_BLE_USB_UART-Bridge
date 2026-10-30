#include "TestPlatform.h"
#include "../../src/core/RoutineEngine.h"
#include "../../src/core/GeoMission.h"
#include "../../src/core/MavlinkPosition.h"
#include <cassert>
#include <iostream>
#include "WiFi.h"

class Payload : public DeviceAddon {
 public:
  bool armed=true, output=false;
  uint32_t end=0, started=0, maxPulse=UINT32_MAX, window=0;
  std::vector<uint32_t> starts;
  bool canStartRoutine(String &reason) const override { reason="test readiness";return armed; }
  bool canRunRoutineFor(uint64_t duration,String &reason) const override { return canStartRoutine(reason)&&(!window||duration<=window); }
  bool validateRoutineCommand(const String &command,String &) const override { return std::stoul(command.substring(9).value)<=maxPulse; }
  bool hasActiveOutput() const override { return output; }
  bool isBusy() const override { return output; }
  bool stopAll(CommandSource,const String &) override { armed=false;output=false;return true; }
  bool handleCommand(const String &command,CommandSource,const String &) override {
    if(command=="Arm") { armed=true;return true; }
    if(command.startsWith("Dispense:")) { if(!armed||output)return false; starts.push_back(millis());output=true;started=millis();end=millis()+std::stoul(command.substring(9).value);return true; }
    return false;
  }
  void service() override { if(output && static_cast<uint32_t>(millis()-started)>=static_cast<uint32_t>(end-started))output=false; }
};
bool submit(void *context,CommandSource source,const String &line,const String &request) {
  return static_cast<Payload*>(context)->handleCommand(line,source,request);
}
void command(RoutineEngine &engine,const char *line) { assert(engine.handleCommand(line,CommandSource::USB,"test")); }
void command(GeoMission &geo,const char *line) { assert(geo.handleCommand(line,CommandSource::USB,"test")); }
void preset(RoutineEngine &engine,const char *name,uint32_t delay,uint32_t pulse,uint32_t repeats=1,uint32_t gap=100) {
  command(engine,("RoutineCreate:"+String(name)).c_str());
  command(engine,("RoutineAdd:"+String(name)+":START_WAIT:"+String(delay)).c_str());
  command(engine,("RoutineAdd:"+String(name)+":DISPENSE:"+String(pulse)).c_str());
  command(engine,("RoutineAdd:"+String(name)+":WAIT_IDLE").c_str());
  command(engine,("RoutineAdd:"+String(name)+":WAIT:"+String(gap)).c_str());
  command(engine,("RoutineRepeat:"+String(name)+":"+String(repeats)).c_str());
  command(engine,("RoutineSave:"+String(name)).c_str());
}
void timingTests() {
  Preferences::records.clear();testNow=0;EventBus events;Payload payload;RoutineEngine engine(events,payload);
  engine.begin();engine.configureSubmitter(submit,&payload);preset(engine,"dots",1000,200,3);
  assert(engine.runNamed("dots",CommandSource::USB,"test"));
  for(testNow=0;testNow<2200;++testNow) { payload.service();engine.service(); }
  assert(!engine.isActive() && engine.lastRunSucceeded() && !payload.output);
  assert(payload.starts.size()==3 && payload.starts[0]>=1000 && payload.starts[0]<1020);
  for(size_t i=1;i<3;++i) assert(payload.starts[i]-payload.starts[i-1]>=300 && payload.starts[i]-payload.starts[i-1]<325);
  RoutineEngine reloaded(events,payload);reloaded.begin();assert(reloaded.stateJson(true).value.find("dots")!=std::string::npos);
  payload.armed=true;payload.maxPulse=100;assert(!reloaded.runNamed("dots",CommandSource::USB,"test"));
  payload.maxPulse=3600000;payload.window=1000;assert(!engine.runNamed("dots",CommandSource::USB,"test"));
  payload.window=3780000;payload.armed=true;assert(engine.runNamed("dots",CommandSource::USB,"test"));
  engine.service();engine.stop(CommandSource::USB,"test","stop during initial delay");
  testNow+=10000;engine.service();assert(!engine.isActive()&&!payload.output&&payload.starts.size()==3);
  payload.armed=true;preset(engine,"hour",120000,3600000,1,0);
  assert(engine.runNamed("hour",CommandSource::USB,"test"));
  engine.service();testNow+=120000;engine.service();++testNow;engine.service();
  assert(payload.output && payload.end-testNow==3600000);
  testNow=payload.end;payload.service();engine.service();++testNow;engine.service();testNow+=100;engine.service();++testNow;engine.service();
  assert(!engine.isActive() && engine.lastRunSucceeded());
  payload.armed=true;preset(engine,"oversize",0,3600000,2);
  assert(!engine.runNamed("oversize",CommandSource::USB,"test")); // Optional arm window still applies.
  payload.window=0;assert(engine.runNamed("oversize",CommandSource::USB,"test"));
  engine.stop(CommandSource::USB,"test","stop");
  std::cout<<"PASS saved reload, initial delay once, optional profile/arm limits, stop, full hour, no total-duration ceiling\n";
}
void arbitraryRoutineTests() {
  Preferences::records.clear();testNow=0;EventBus events;Payload payload;RoutineEngine engine(events,payload);
  engine.begin();engine.configureSubmitter(submit,&payload);preset(engine,"many",0,1,300,0);
  RoutineEngine reloaded(events,payload);reloaded.begin();reloaded.configureSubmitter(submit,&payload);
  assert(reloaded.stateJson(true).value.find(",300,true,")!=std::string::npos);
  assert(reloaded.runNamed("many",CommandSource::USB,"test"));
  for(testNow=0;testNow<5000;++testNow){payload.service();reloaded.service();}
  assert(reloaded.lastRunSucceeded()&&!reloaded.isActive()&&payload.starts.size()==300);
  payload.armed=true;preset(engine,"max",UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX);
  assert(engine.stateJson(true).value.find("4294967295")!=std::string::npos);
  assert(engine.runNamed("max",CommandSource::USB,"test"));
  testNow=UINT32_MAX-100;engine.service(); // Start delay near rollover.
  testNow=0x80000000U;engine.service();assert(payload.starts.size()==300);
  testNow=UINT32_MAX-102;engine.service();assert(payload.starts.size()==300);
  testNow=UINT32_MAX-101;engine.service(); // Full 32-bit delay elapsed across wrap.
  ++testNow;engine.service();assert(payload.output&&payload.end-payload.started==UINT32_MAX);
  ++testNow;engine.service(); // WAIT_IDLE must not expire after an hour.
  testNow+=10000000;payload.service();engine.service();assert(engine.isActive()&&payload.output);
  engine.stop(CommandSource::USB,"test","stop long routine");assert(!payload.output&&!engine.isActive());
  command(engine,"RoutineRepeat:many:0");command(engine,"RoutineRepeat:many:4294967296");
  command(engine,"RoutineRepeat:many:-1");command(engine,"RoutineRepeat:many:1.5");
  assert(engine.stateJson(true).value.find(",300,true,")!=std::string::npos);
  // Convert a current saved record into the exact version-2 layout/checksum.
  preset(engine,"legacy",42,7,6,9);
  auto &record=Preferences::records["drone-routine"]["slot2"];
  assert(!record.empty());auto v3=record;
  record[4]=2;record[5]=0;memmove(record.data()+9,record.data()+12,16);
  record[8]=6;record[25]=record[26]=record[27]=0;
  uint32_t hash=2166136261U;for(size_t i=0;i<record.size()-4;++i){hash^=record[i];hash*=16777619U;}
  memcpy(record.data()+record.size()-4,&hash,4);
  RoutineEngine migrated(events,payload);migrated.begin();
  assert(migrated.hasSaved("legacy")&&migrated.stateJson(true).value.find("[\"legacy\",42,7,9,6,true,")!=std::string::npos);
  record[9]^=1;RoutineEngine corrupted(events,payload);corrupted.begin();assert(!corrupted.hasSaved("legacy"));
  std::cout<<"PASS 300 actual pulses, 32-bit repeats/timers, rollover, long wait/stop, invalid counts, version-2 migration and checksum rejection\n";
}
void continuousRoutineTests() {
  Preferences::records.clear();testNow=0;EventBus events;Payload payload;RoutineEngine engine(events,payload);
  engine.begin();engine.configureSubmitter(submit,&payload);preset(engine,"loop",50,5,1,5);
  command(engine,"RoutineRepeat:loop:FOREVER");command(engine,"RoutineSave:loop");
  RoutineEngine reload(events,payload);reload.begin();reload.configureSubmitter(submit,&payload);
  assert(reload.hasSaved("loop")&&reload.stateJson(true).value.find(",0,true,")!=std::string::npos);
  assert(reload.runNamed("loop",CommandSource::USB,"test"));
  for(testNow=0;testNow<600;++testNow){payload.service();reload.service();}
  assert(reload.isActive()&&payload.starts.size()>30&&payload.starts[0]>=50&&payload.starts[0]<60);
  for(size_t i=1;i<payload.starts.size();++i)assert(payload.starts[i]-payload.starts[i-1]>=10&&payload.starts[i]-payload.starts[i-1]<25);
  // Stop while on, then verify no later pulse appears.
  while(!payload.output){++testNow;payload.service();reload.service();}
  reload.stop(CommandSource::USB,"test","operator stop");const auto count=payload.starts.size();
  testNow+=1000;payload.service();reload.service();assert(!reload.isActive()&&!payload.output&&payload.starts.size()==count);
  payload.armed=true;payload.window=1000;assert(!reload.runNamed("loop",CommandSource::USB,"test"));
  payload.window=0;assert(reload.runNamed("loop",CommandSource::USB,"test"));reload.service();
  reload.stop(CommandSource::USB,"test","stop initial delay");testNow+=100;reload.service();assert(payload.starts.size()==count);
  payload.armed=true;assert(reload.runNamed("loop",CommandSource::USB,"test"));reload.service();
  testNow+=50;reload.service();++testNow;reload.service();assert(payload.output);
  testNow+=5;payload.service();reload.service();assert(!payload.output&&reload.isActive());
  reload.stop(CommandSource::USB,"test","stop gap");testNow+=100;reload.service();assert(!reload.isActive()&&!payload.output);
  std::cout<<"PASS continuous saved reload, one-time delay, recurring on/off, stop during pulse/delay/gap, optional arming limit\n";
}
void geoTests() {
  Preferences::records.clear();testNow=10;EventBus events;Payload payload;RoutineEngine engine(events,payload);
  engine.begin();engine.configureSubmitter(submit,&payload);preset(engine,"dots",100,20);
  GeoMission geo(events,payload,engine);geo.begin();
  command(geo,"GeoSource:API");command(geo,"GeoAdd:37,-122,5,dots");command(geo,"GeoAdd:37.001,-122,5,dots");command(geo,"GeoSave");
  assert(geo.submitPosition("37.001,-122,1,0"));geo.service();command(geo,"GeoStart");geo.service();
  assert(geo.isActive()&&!engine.isActive()); // Next point only, despite being near point 2.
  assert(geo.submitPosition("37,-122,10,0"));geo.service();assert(!engine.isActive()); // Accuracy worse than radius.
  assert(geo.submitPosition("37,-122,1,0"));geo.service();assert(engine.isActive());
  for(unsigned i=0;i<300;++i) { ++testNow;payload.service();engine.service();geo.submitPosition("37,-122,1,0");geo.service(); }
  assert(payload.starts.size()==1 && geo.isActive()&&!engine.isActive()); // Inside old radius cannot repeat.
  assert(geo.submitPosition("37.001,-122,1,0"));geo.service();assert(engine.isActive());
  for(unsigned i=0;i<300;++i) { ++testNow;payload.service();engine.service();geo.submitPosition("37.001,-122,1,0");geo.service(); }
  assert(payload.starts.size()==2 && !geo.isActive());
  command(geo,"GeoStart");assert(geo.isActive());assert(geo.submitPosition("37,-122,1,0"));geo.service();assert(engine.isActive());
  testNow+=3001;geo.service();assert(!geo.isActive()&&geo.consumeSafetyStop());
  engine.stop(CommandSource::INTERNAL,"test","stale fix");assert(!payload.output&&!engine.isActive());
  assert(!geo.submitPosition("nan,-122,1,0"));geo.service();command(geo,"GeoStart");assert(!geo.isActive());
  assert(!geo.submitPosition("37,-122,1,3001"));assert(!geo.submitPosition("37,-122,1,0,extra"));
  assert(geo.submitPosition("+3.7e1,-1.22E2,1.5e0,0"));
  for (const char *invalid : {"37e,-122,1,0", "37.1.2,-122,1,0", "0x25,-122,1,0", "1e99,-122,1,0", "37,-122,-0.5,0", ".,-122,1,0", "37,-122,1,0suffix"}) assert(!geo.submitPosition(invalid));
  assert(geo.submitPosition("37,-122,1,100"));testNow+=2950;geo.service();command(geo,"GeoStart");assert(!geo.isActive()); // Queue age included.
  assert(geo.submitPosition("37,-122,1,0"));assert(geo.submitPosition("0,0,1,0"));geo.service();command(geo,"GeoStart");geo.service();assert(!engine.isActive()); // Latest sample wins.
  geo.stop("test stop");geo.consumeSafetyStop();
  GeoMission reload(events,payload,engine);reload.begin();assert(reload.stateJson(true).value.find("\"source\":\"API\"")!=std::string::npos);
  assert(reload.stateJson(true).value.find("\"fresh\":false")!=std::string::npos);
  // Float trig must retain small distances after subtracting large coordinates,
  // including across the date line and at a geographic pole.
  auto measuredDistance=[&](const char *point,const char *fix) {
    command(reload,"GeoClear");command(reload,"GeoSource:API");command(reload,point);
    assert(reload.submitPosition(fix));reload.service();
    const std::string json=reload.stateJson(true).value;
    const size_t at=json.find("\"distance\":");assert(at!=std::string::npos);
    return std::stod(json.substr(at+11));
  };
  assert(measuredDistance("GeoAdd:37.00001,-122,5,dots","37,-122,1,0")>1.0);
  const double crossing=measuredDistance("GeoAdd:0,-179.999999,0.3,dots","0,179.999999,0.01,0");
  assert(crossing>=0.1&&crossing<=0.3);
  assert(measuredDistance("GeoAdd:90,180,0.1,dots","90,0,0.01,0")==0);
  std::cout<<"PASS ordered points, one trigger per point, accuracy, stale/invalid/queued fixes, latest sample, saved source\n";
}
void expandedGeoTests() {
  Preferences::records.clear();testNow=0;EventBus events;Payload payload;RoutineEngine engine(events,payload);
  engine.begin();engine.configureSubmitter(submit,&payload);preset(engine,"dots",0,1,1,0);
  GeoMission geo(events,payload,engine);geo.begin();command(geo,"GeoSource:API");
  for(unsigned i=0;i<256;++i)command(geo,"GeoAdd:37,-122,5,dots");
  command(geo,"GeoAdd:37,-122,5,dots"); // Extra point must be rejected without changing the plan.
  assert(geo.stateJson(true).value.find("\"count\":256")!=std::string::npos);
  command(geo,"GeoSave");assert(geo.stateJson(true).value.find("\"saved\":true")!=std::string::npos);
  GeoMission reload(events,payload,engine);reload.begin();
  assert(reload.stateJson(true).value.find("\"count\":256")!=std::string::npos);
  assert(reload.submitPosition("37,-122,1,0"));reload.service();command(reload,"GeoStart");
  for(unsigned i=0;i<5000&&reload.isActive();++i){++testNow;payload.service();engine.service();reload.submitPosition("37,-122,1,0");reload.service();}
  assert(!reload.isActive()&&payload.starts.size()==256);
  assert(reload.stateJson(true).value.find("\"next\":256")!=std::string::npos);
  auto &record=Preferences::records["drone-geo"]["plan"];const size_t expandedSize=record.size();
  struct LegacyPoint {double latitude,longitude;float tolerance;char routine[16];};
  struct LegacyPlan {uint32_t magic;uint8_t count,source;LegacyPoint points[12];uint32_t checksum;};
  LegacyPlan old{};old.magic=0x47454F31;old.count=12;old.source=1;
  memcpy(old.points,record.data()+8,sizeof(old.points));old.checksum=2166136261U;
  auto *bytes=reinterpret_cast<uint8_t *>(&old);
  for(size_t i=0;i<offsetof(LegacyPlan,checksum);++i){old.checksum^=bytes[i];old.checksum*=16777619U;}
  record.assign(bytes,bytes+sizeof(old));
  GeoMission migrated(events,payload,engine);migrated.begin();
  const auto json=migrated.stateJson(true).value;
  assert(json.find("\"saved\":true")!=std::string::npos&&json.find("\"count\":12")!=std::string::npos&&json.find("\"source\":\"API\"")!=std::string::npos);
  command(migrated,"GeoSave");assert(record.size()==expandedSize);
  record[8]^=1;GeoMission corrupted(events,payload,engine);corrupted.begin();
  assert(corrupted.stateJson(true).value.find("\"saved\":false")!=std::string::npos);
  assert(corrupted.stateJson(true).value.find("\"count\":0")!=std::string::npos);
  std::cout<<"PASS 256 GPS points saved/reloaded/executed, capacity rejection, legacy 12-point migration, corrupt-plan rejection\n";
}
std::vector<uint8_t> frame(uint32_t id,std::vector<uint8_t> payload,bool v2=true,uint8_t flags=0) {
  std::vector<uint8_t> bytes=v2?std::vector<uint8_t>{0xFD,static_cast<uint8_t>(payload.size()),flags,0,7,1,1,static_cast<uint8_t>(id),0,0}:
    std::vector<uint8_t>{0xFE,static_cast<uint8_t>(payload.size()),7,1,1,static_cast<uint8_t>(id)};
  bytes.insert(bytes.end(),payload.begin(),payload.end());uint16_t crc=0xFFFF;
  auto bitwise=[&](uint8_t byte){crc^=byte;for(int i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0x8408:0);};
  for(size_t i=1;i<bytes.size();++i)bitwise(bytes[i]);bitwise(id==24?24:104);
  bytes.push_back(crc&255);bytes.push_back(crc>>8);if(flags&1)bytes.resize(bytes.size()+13);return bytes;
}
void put32(std::vector<uint8_t>&bytes,size_t at,uint32_t n){for(int i=0;i<4;++i)bytes[at+i]=(n>>(i*8))&255;}
bool decode(const std::vector<uint8_t>&bytes,MavlinkPosition::Message &m){size_t at=0;return MavlinkPosition::next(bytes.data(),bytes.size(),at,m);}
void mavlinkTests() {
  MavlinkPosition::Message m{};std::vector<uint8_t> global(28);put32(global,0,1234);put32(global,4,374219999);put32(global,8,static_cast<uint32_t>(-1220840575));
  for(bool v2:{false,true}) { auto bytes=frame(33,global,v2);assert(decode(bytes,m)&&m.id==33&&m.bootMs==1234&&m.latitudeE7==374219999&&m.longitudeE7==-1220840575&&m.system==1&&m.component==1);
    bytes[bytes.size()-1]^=1;assert(!decode(bytes,m));bytes=frame(33,global,v2);bytes.pop_back();assert(!decode(bytes,m)); }
  assert(!decode(frame(33,global,true,1),m));assert(!decode(frame(33,global,true,2),m));
  assert(decode(frame(33,{1}),m)&&m.bootMs==1&&m.longitudeE7==0); // Zero-trimmed MAVLink 2.
  assert(!decode(frame(33,{1},false),m));
  std::vector<uint8_t> gps(52);gps[28]=3;put32(gps,34,1500);auto one=frame(24,gps);auto two=frame(33,global);one.insert(one.end(),two.begin(),two.end());
  size_t at=0;assert(MavlinkPosition::next(one.data(),one.size(),at,m)&&m.fixType==3&&m.accuracyMm==1500);assert(MavlinkPosition::next(one.data(),one.size(),at,m)&&m.id==33);assert(!MavlinkPosition::next(one.data(),one.size(),at,m));
  gps.resize(30);assert(decode(frame(24,gps,false),m)&&m.accuracyMm==0);
  auto noise=frame(99,{1,2,3});noise.insert(noise.end(),two.begin(),two.end());assert(decode(noise,m)&&m.id==33);
  std::cout<<"PASS MAVLink 1/2 position, CRC, truncation, flags/signatures, negative coordinates, zero trimming, combined datagrams\n";
}
void mavlinkMissionTests() {
  Preferences::records.clear();testNow=10;WiFi.statusValue=WL_CONNECTED;testDatagrams.clear();
  EventBus events;Payload payload;RoutineEngine engine(events,payload);engine.begin();engine.configureSubmitter(submit,&payload);
  preset(engine,"dots",100000,20);GeoMission geo(events,payload,engine);geo.begin();
  command(geo,"GeoAdd:37,-122,5,dots");command(geo,"GeoSave");
  std::vector<uint8_t> gps(30);gps[28]=3;
  std::vector<uint8_t> global(28);put32(global,0,100);put32(global,4,370000000);put32(global,8,static_cast<uint32_t>(-1220000000));
  auto feed=[&](uint32_t boot) { put32(global,0,boot);testDatagrams.push_back(frame(24,gps));testDatagrams.push_back(frame(33,global));geo.service(); };
  feed(100);command(geo,"GeoStart");geo.service();engine.service();assert(geo.isActive()&&engine.isActive());
  testNow=2500;feed(99);assert(geo.isActive()); // Reordered frames cannot refresh position.
  testNow=3000;feed(101);assert(geo.isActive()); // A new timestamp does refresh it.
  testNow=6001;feed(101);assert(!geo.isActive()&&geo.consumeSafetyStop()); // Duplicate after silence cannot recover freshness.
  engine.stop(CommandSource::INTERNAL,"test","stale");assert(!payload.output);
  feed(50);assert(geo.stateJson(true).value.find("\"fresh\":false")!=std::string::npos); // Reboot needs explicit reset.
  command(geo,"GeoResetPosition");feed(50);assert(geo.stateJson(true).value.find("\"fresh\":true")!=std::string::npos);
  command(geo,"GeoStart");geo.service();assert(geo.isActive());
  gps[28]=2;testDatagrams.push_back(frame(24,gps));geo.service();assert(!geo.isActive()&&geo.consumeSafetyStop());
  engine.stop(CommandSource::INTERNAL,"test","invalid fix");WiFi.statusValue=0;
  std::cout<<"PASS MAVLink mission timestamps, reorder/duplicate rejection after timeout, source reboot/reset, lost 3D fix\n";
}
void udpSelfTestTests() {
  Preferences::records.clear();testDatagrams.clear();testNow=10;EventBus events;Payload payload;RoutineEngine engine(events,payload);
  engine.begin();engine.configureSubmitter(submit,&payload);preset(engine,"dots",4000,20,1,0);
  GeoMission geo(events,payload,engine);geo.begin();command(geo,"GeoAdd:37,-122,5,dots");command(geo,"GeoSave");
  WiFi.statusValue=0;WiFi.modeValue=WIFI_AP;
  command(geo,"GeoTestPosition:37.001,-122,1.25");
  assert(geo.stateJson(true).value.find("\"fresh\":false")!=std::string::npos&&testDatagrams.size()==1);
  const auto packet=testDatagrams.front();assert(packet.size()==90);
  // Check both packet CRCs independently of the production accumulator.
  for(size_t start:{size_t(0),size_t(50)}) {
    const size_t end=start+(start?38:48);uint16_t crc=0xFFFF;
    for(size_t i=start+1;i<=end;++i){crc^=i==end?(start?104:24):packet[i];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0x8408:0);}
    assert(crc==(packet[end]|(static_cast<uint16_t>(packet[end+1])<<8)));
  }
  MavlinkPosition::Message message{};size_t offset=0;
  assert(MavlinkPosition::next(packet.data(),packet.size(),offset,message)&&message.id==24&&message.fixType==3&&message.accuracyMm==1250);
  assert(MavlinkPosition::next(packet.data(),packet.size(),offset,message)&&message.id==33&&message.latitudeE7==370010000&&message.longitudeE7==-1220000000);
  geo.service();assert(geo.stateJson(true).value.find("\"fresh\":true")!=std::string::npos);
  command(geo,"GeoStart");geo.service();assert(geo.isActive()&&!engine.isActive());
  for(const char *invalid:{"GeoTestPosition:91,-122,1","GeoTestPosition:nan,-122,1","GeoTestPosition:37,-122,-2","GeoTestPosition:37,-122,1,extra"})command(geo,invalid);
  assert(testDatagrams.empty());
  ++testNow;command(geo,"GeoTestPosition:37,-122,1");assert(!engine.isActive());geo.service();assert(engine.isActive());
  for(unsigned i=0;i<4500&&geo.isActive();++i){++testNow;payload.service();engine.service();if(i%1000==0)command(geo,"GeoTestPosition:37,-122,1");geo.service();}
  assert(!geo.isActive()&&payload.starts.size()==1); // Refresh survives the four-second initial delay.
  WiFi.statusValue=WL_CONNECTED;WiFi.modeValue=0;
  ++testNow;command(geo,"GeoTestPosition:37,-122,1");geo.service();command(geo,"GeoStart");geo.service();assert(engine.isActive());
  testNow+=3001;geo.service();assert(!geo.isActive()&&geo.consumeSafetyStop());engine.stop(CommandSource::INTERNAL,"test","stale test feed");assert(!payload.output);
  command(geo,"GeoSource:API");command(geo,"GeoTestPosition:37,-122,1");assert(testDatagrams.empty());
  WiFi.statusValue=0;WiFi.modeValue=0;
  std::cout<<"PASS manual MAVLink UDP loopback on AP/client, independent CRC/fields, outside/inside radius, refreshed start delay, stale stop, invalid test positions\n";
}
void bluetoothGpsTests() {
  Preferences::records.clear();testDatagrams.clear();testNow=10;WiFi.statusValue=0;WiFi.modeValue=0;
  EventBus events;Payload payload;RoutineEngine engine(events,payload);engine.begin();engine.configureSubmitter(submit,&payload);
  preset(engine,"dots",4000,20,1,0);GeoMission geo(events,payload,engine);geo.begin();
  command(geo,"GeoSource:BLE");command(geo,"GeoAdd:37,-122,5,dots");command(geo,"GeoSave");
  GeoMission reload(events,payload,engine);reload.begin();assert(reload.stateJson(true).value.find("\"source\":\"BLE\"")!=std::string::npos);
  std::vector<uint8_t> gps(38);gps[28]=3;put32(gps,34,1000);
  std::vector<uint8_t> global(28);put32(global,4,370000000);put32(global,8,static_cast<uint32_t>(-1220000000));
  auto feed=[&](uint32_t boot,size_t chunk=20,bool v2=true) {
    put32(global,0,boot);auto bytes=frame(24,gps,v2);auto second=frame(33,global,v2);bytes.insert(bytes.end(),second.begin(),second.end());
    for(size_t i=0;i<bytes.size();i+=chunk)geo.receiveBleMavlink(bytes.data()+i,std::min(chunk,bytes.size()-i),testNow);
    geo.service();
  };
  // BLE alone, one byte writes, multiple frames, and independent UDP source.
  feed(100,1);assert(geo.stateJson(true).value.find("\"fresh\":true")!=std::string::npos);
  command(geo,"GeoStart");geo.service();engine.service();assert(engine.isActive());
  for(unsigned i=0;i<4500&&geo.isActive();++i){++testNow;payload.service();engine.service();if(i%1000==0)feed(101+i,90);geo.service();}
  assert(!geo.isActive()&&payload.starts.size()==1);
  ++testNow;feed(5000,20,false);command(geo,"GeoStart");geo.service();assert(engine.isActive());
  geo.receiveBleMavlink(nullptr,0,testNow);geo.service();assert(!geo.isActive()&&geo.consumeSafetyStop());engine.stop(CommandSource::INTERNAL,"test","BLE disconnect");assert(!payload.output);
  ++testNow;feed(1);command(geo,"GeoStart");geo.service();assert(engine.isActive());
  testNow+=3001;feed(1);assert(!geo.isActive()&&geo.consumeSafetyStop());engine.stop(CommandSource::INTERNAL,"test","stale BLE");
  command(geo,"GeoResetPosition");put32(global,0,200);auto bytes=frame(24,gps);auto second=frame(33,global);bytes.insert(bytes.end(),second.begin(),second.end());
  geo.receiveBleMavlink(bytes.data(),bytes.size(),testNow-3001);geo.service();assert(geo.stateJson(true).value.find("\"fresh\":false")!=std::string::npos);
  ++testNow;feed(200);gps[28]=2;++testNow;feed(201);assert(geo.stateJson(true).value.find("\"fresh\":false")!=std::string::npos);
  command(geo,"GeoSource:API");gps[28]=3;++testNow;feed(202);assert(geo.stateJson(true).value.find("\"fresh\":false")!=std::string::npos);
  command(geo,"GeoSource:BLE");geo.receiveBleMavlink(bytes.data(),bytes.size(),testNow-1);assert(geo.stateJson(true).value.find("\"fresh\":false")!=std::string::npos);
  // Framing cannot interpret unknown, signed, corrupt or incomplete frames.
  MavlinkStream stream;MavlinkPosition::Message message{};uint32_t at=0;unsigned valid=0;
  auto push=[&](const std::vector<uint8_t>& input,uint32_t received,uint32_t now) {for(uint8_t b:input)if(stream.push(b,received,now,message,at))++valid;};
  push({0,1,2,3},testNow,testNow);push(frame(33,global,true,1),testNow,testNow);assert(valid==0);
  auto bad=frame(33,global);bad.back()^=1;push(bad,testNow,testNow);assert(valid==0);
  push(frame(99,std::vector<uint8_t>(255,0xFD),true,1),testNow,testNow);assert(valid==0);
  auto truncated=frame(33,global);truncated.resize(12);push(truncated,testNow,testNow);
  testNow+=1001;push(frame(33,global),testNow,testNow);assert(valid==1&&at==testNow);
  push(frame(24,gps,false),testNow,testNow);assert(valid==2&&message.fixType==3);
  std::cout<<"PASS BLE MAVLink 1/2 fragments/noise/CRC/signatures/timeout, Wi-Fi-free mission, saved source, initial delay, disconnect/stale/invalid/source switch\n";
}
int main(){timingTests();arbitraryRoutineTests();continuousRoutineTests();geoTests();expandedGeoTests();mavlinkTests();mavlinkMissionTests();udpSelfTestTests();bluetoothGpsTests();}
