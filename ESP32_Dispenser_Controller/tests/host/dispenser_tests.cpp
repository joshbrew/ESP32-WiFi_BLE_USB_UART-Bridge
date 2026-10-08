#include "TestPlatform.h"
#include "../../src/addons/dispenser/DispenserAddon.h"
#include "../../src/core/RoutineEngine.h"
#include <cassert>
#include <iostream>

void command(DispenserAddon &addon,const char *line){assert(addon.handleCommand(line,CommandSource::USB,"test"));}
void command(RoutineEngine &engine,const char *line){assert(engine.handleCommand(line,CommandSource::USB,"test"));}
bool send(void *context,CommandSource source,const String &line,const String &request){return static_cast<DispenserAddon*>(context)->handleCommand(line,source,request);}
std::string state(DispenserAddon &addon){String json="{";addon.appendStateJson(json,true);return json.value;}
void allInactive(std::initializer_list<int> pins,int level=LOW){for(int pin:pins)assert(testLevels[pin]==level);}
void tick(DispenserAddon &addon,RoutineEngine &engine,unsigned count){for(unsigned i=0;i<count;++i){++testNow;engine.service();addon.service();}}

int main(){
  Preferences::records.clear();testNow=0;EventBus events;DispenserAddon addon(events);addon.begin();
  assert(state(addon).find("\"pins\":[26]")!=std::string::npos);assert(!addon.hasActiveOutput());
  command(addon,"DispenserOutputs:26,27,25");command(addon,"DispenserSave");
  assert(state(addon).find("\"pins\":[26,27,25]")!=std::string::npos);allInactive({26,27,25});
  for(const char *bad:{"DispenserOutputs:26,26","DispenserOutputs:26,6","DispenserOutputs:26,2","DispenserOutputs:26,","DispenserOutputs:9999999999","DispenserOutputs:26,27,25,32,33,18,19,21,22"}){
    command(addon,bad);assert(state(addon).find("\"pins\":[26,27,25]")!=std::string::npos);
  }
  DispenserAddon reboot(events);reboot.begin();assert(state(reboot).find("\"pins\":[26,27,25]")!=std::string::npos);allInactive({26,27,25});
  command(reboot,"Arm");command(reboot,"DispenserOutputs:26");assert(state(reboot).find("\"pins\":[26,27,25]")!=std::string::npos);
  command(reboot,"DispensePin:27,10");assert(testLevels[27]==HIGH);allInactive({26,25});
  command(reboot,"DispensePin:25,10");assert(testLevels[27]==HIGH&&testLevels[25]==LOW); // No overlap.
  testNow+=10;reboot.service();allInactive({26,27,25});
  command(reboot,"DispensePin:32,10");allInactive({26,27,25});assert(!reboot.hasActiveOutput());
  command(reboot,"Dispense:10");assert(testLevels[26]==HIGH);command(reboot,"Disarm");allInactive({26,27,25});
  command(reboot,"DispenserActiveHigh:OFF");allInactive({26,27,25},HIGH);command(reboot,"Arm");command(reboot,"DispensePin:25,1");assert(testLevels[25]==LOW&&testLevels[26]==HIGH);
  reboot.stopAll(CommandSource::USB,"test");allInactive({26,27,25},HIGH);
  command(reboot,"DispenserActiveHigh:ON");command(reboot,"DispenserOutputs:26,27");assert(testModes[25]==INPUT&&testLevels[25]==LOW);
  command(reboot,"DispenserOutputs:26,27,25");command(reboot,"PayloadProfileSave:multi");
  command(reboot,"DispenserOutputs:26");command(reboot,"PayloadProfileUse:multi");assert(state(reboot).find("\"pins\":[26,27,25]")!=std::string::npos);allInactive({26,27,25});
  DispenserAddon profileBoot(events);profileBoot.begin();assert(state(profileBoot).find("\"pins\":[26,27,25]")!=std::string::npos);
  // Construct the original version-1, single-pin profile and its original CRC.
  auto &record=Preferences::records["drone-disp"]["prof0"];std::vector<uint8_t> legacy(record.begin(),record.begin()+44);
  legacy[4]=1;legacy[5]=0;uint32_t hash=2166136261U;for(size_t i=0;i<40;++i){hash^=legacy[i];hash*=16777619U;}memcpy(legacy.data()+40,&hash,4);record=legacy;
  DispenserAddon migrated(events);migrated.begin();assert(state(migrated).find("\"pins\":[26]")!=std::string::npos);
  command(migrated,"DispenserOutputs:26,27,25");command(migrated,"DispenserSave");
  RoutineEngine engine(events,migrated);engine.begin();engine.configureSubmitter(send,&migrated);
  command(engine,"RoutineCreate:multi");command(engine,"RoutineAdd:multi:START_WAIT:20");
  command(engine,"RoutineAdd:multi:OUTPUT:26,10,7");command(engine,"RoutineAdd:multi:OUTPUT:27,15,9");command(engine,"RoutineAdd:multi:OUTPUT:25,5,11");command(engine,"RoutineRepeat:multi:2");command(engine,"RoutineSave:multi");
  RoutineEngine reload(events,migrated);reload.begin();reload.configureSubmitter(send,&migrated);
  command(migrated,"Arm");testWrites.clear();const uint32_t start=testNow;assert(reload.runNamed("multi",CommandSource::USB,"test"));tick(migrated,reload,300);
  std::vector<GpioWrite> starts;for(auto write:testWrites)if(write.level==HIGH)starts.push_back(write);
  assert(starts.size()==6&&!reload.isActive()&&reload.lastRunSucceeded());assert(starts[0].at-start>=20);
  for(size_t i=0;i<starts.size();++i)assert(starts[i].pin==(i%3==0?26:i%3==1?27:25));
  assert(starts[1].at-starts[0].at>=17&&starts[2].at-starts[1].at>=24&&starts[3].at-starts[2].at>=16);
  assert(starts[3].at-starts[2].at<30);allInactive({26,27,25}); // Initial delay only once.
  command(migrated,"Arm");command(engine,"RoutineRepeat:multi:FOREVER");command(engine,"RoutineSave:multi");assert(engine.runNamed("multi",CommandSource::USB,"test"));tick(migrated,engine,300);assert(engine.isActive());engine.stop(CommandSource::USB,"test","stop");allInactive({26,27,25});
  for(unsigned elapsed:{5U,25U,35U}) {
    command(migrated,"Arm");assert(engine.runNamed("multi",CommandSource::USB,"test"));tick(migrated,engine,elapsed);
    engine.stop(CommandSource::USB,"test","stop phase");allInactive({26,27,25});testWrites.clear();tick(migrated,engine,200);
    for(auto write:testWrites)assert(write.level!=HIGH);
  }
  command(engine,"RoutineCreate:async");command(engine,"RoutineAdd:async:OUTPUT:26,1,5");command(engine,"RoutineAdd:async:OUTPUT:27,1,5");command(engine,"RoutineSave:async");
  struct Queue {String line;uint32_t at=0;} queue;
  auto enqueue=[](void *context,CommandSource,const String &line,const String &){auto &q=*static_cast<Queue*>(context);q.line=line;q.at=millis();return true;};
  engine.configureSubmitter(enqueue,&queue);command(migrated,"Arm");assert(engine.runNamed("async",CommandSource::USB,"test"));testWrites.clear();
  for(unsigned i=0;i<200&&engine.isActive();++i) {
    ++testNow;if(queue.line.length()&&testNow-queue.at>=30){migrated.handleCommand(queue.line,CommandSource::INTERNAL,"test");queue.line="";}
    engine.service();migrated.service();
  }
  starts.clear();for(auto write:testWrites)if(write.level==HIGH)starts.push_back(write);
  assert(!engine.isActive()&&engine.lastRunSucceeded()&&starts.size()==2&&starts[0].pin==26&&starts[1].pin==27);allInactive({26,27,25});
  command(migrated,"Arm");assert(engine.runNamed("async",CommandSource::USB,"test"));tick(migrated,engine,1200);
  assert(!engine.isActive()&&!engine.lastRunSucceeded());queue.line="";allInactive({26,27,25}); // Accepted but never executed must fail safe.
  engine.configureSubmitter(send,&migrated);
  command(migrated,"DispenserOutputs:26");command(migrated,"Arm");assert(!reload.runNamed("multi",CommandSource::USB,"test"));allInactive({26}); // Removed pin preflight.
  command(migrated,"Disarm");command(migrated,"DispenserOutputs:26,27,25");command(migrated,"DispenserMaxPulse:250");command(migrated,"DispenserArmTimeout:1000");command(migrated,"Arm");assert(!reload.runNamed("multi",CommandSource::USB,"test"));
  command(migrated,"Disarm");command(migrated,"DispenserArmTimeout:0");command(migrated,"DispenserMaxPulse:0");command(migrated,"Arm");
  testNow=UINT32_MAX-5;command(migrated,"DispensePin:27,10");testNow=4;migrated.service();allInactive({26,27,25});
  command(migrated,"Disarm");command(migrated,"DispenserOutputs:26,27,25,32,33,18,19,21");command(migrated,"DispenserSave");
  DispenserAddon eight(events);eight.begin();RoutineEngine eightEngine(events,eight);eightEngine.begin();eightEngine.configureSubmitter(send,&eight);
  command(eightEngine,"RoutineCreate:eight");command(eightEngine,"RoutineAdd:eight:START_WAIT:1");
  for(int pin:{26,27,25,32,33,18,19,21})command(eightEngine,("RoutineAdd:eight:OUTPUT:"+String(pin)+",1,0").c_str());
  command(eightEngine,"RoutineSave:eight");command(eight,"Arm");testWrites.clear();assert(eightEngine.runNamed("eight",CommandSource::USB,"test"));tick(eight,eightEngine,100);
  starts.clear();for(auto write:testWrites)if(write.level==HIGH)starts.push_back(write);assert(starts.size()==8&&eightEngine.lastRunSucceeded());allInactive({26,27,25,32,33,18,19,21});
  command(migrated,"Disarm");command(migrated,"DispenserDefaults");assert(state(migrated).find("\"pins\":[26]")!=std::string::npos);allInactive({26,27,25});
  std::cout<<"PASS actual dispenser pins/timing/no overlap, boot/settings/profiles and legacy migration, polarity, stop, repeat/delay, limits, removed pins, rollover\n";
}
