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

#if DRONE_CFG_ENABLE_DRV8871
void checkMotor() {
  constexpr int in1 = AppConfig::PIN_DRV8871_IN1;
  constexpr int in2 = AppConfig::PIN_DRV8871_IN2;
  constexpr int gpio = AppConfig::PIN_DISPENSER;
  EventBus events;
  DispenserAddon addon(events);
  addon.begin();
  allInactive({in1, in2, gpio});
  command(addon, "MotorStart:255");
  assert(!addon.hasActiveOutput() && testPwm.empty());
  command(addon, ("DispenserPin:" + String(in1)).c_str());
  assert(state(addon).find("\"pins\":[4]") != std::string::npos);

  command(addon, "Arm");
  command(addon, "MotorPulse:10,256");
  assert(!addon.hasActiveOutput() && testPwm.empty());
  command(addon, "MotorPulse:10,128");
  assert(addon.hasActiveOutput() && testPwm.at(in2) == 127);
  command(addon, "Dispense:10");
  assert(testLevels[in1] == HIGH && testLevels[gpio] == LOW);
  testNow += 5;
  command(addon, "MotorLevel:255");
  assert(testPwm.at(in2) == 0);
  testNow += 5;
  addon.service();
  allInactive({in1, in2, gpio});
  assert(!addon.hasActiveOutput() && testPwm.empty());

  command(addon, "MotorStart");
  assert(testPwm.at(in2) == 0);
  command(addon, "MotorStop");
  assert(!addon.hasActiveOutput() && testPwm.empty());
  allInactive({in1, in2}, HIGH);
  testWrites.clear();
  command(addon, "MotorStop");
  assert(testWrites.empty());
  command(addon, "MotorCoast");
  allInactive({in1, in2});

  command(addon, "MotorStart:100");
  command(addon, "MotorLevel:0");
  assert(!addon.hasActiveOutput() && testPwm.empty());
  allInactive({in1, in2});

  command(addon, "MotorStart:1");
  testNow += AppConfig::DRV8871_MAX_RUN_MS;
  addon.service();
  assert(!addon.hasActiveOutput() && testPwm.empty());
  command(addon, "MotorStart:100");
  assert(addon.stopAll(CommandSource::USB, "test"));
  allInactive({in1, in2, gpio});
  assert(testPwm.empty());
  std::cout << "PASS DRV8871 arming, reserved pins, duty, timed release, brake, coast and StopAll\n";
}

void checkMotorRoutines() {
  Preferences::records.clear();
  testNow = 0;
  EventBus events;
  DispenserAddon addon(events);
  addon.begin();
  RoutineEngine builder(events, addon);
  builder.begin();
  command(builder, "RoutineCreate:motor");
  command(builder, "RoutineAdd:motor:START_WAIT:8");
  command(builder, "RoutineAdd:motor:MOTOR:20,15,147");
  command(builder, "RoutineRepeat:motor:3");
  command(builder, "RoutineSave:motor");

  RoutineEngine reloaded(events, addon);
  reloaded.begin();
  reloaded.configureSubmitter(send, &addon);
  assert(reloaded.hasSaved("motor"));
  assert(reloaded.stateJson(true).value.find("[\"motor\",8,20,15,3,true,2,1,1,147]") != std::string::npos);
  assert(!reloaded.runNamed("motor", CommandSource::USB, "test"));
  command(addon, "Arm");
  assert(reloaded.runNamed("motor", CommandSource::USB, "test"));
  constexpr int in2 = AppConfig::PIN_DRV8871_IN2;
  std::vector<uint32_t> starts, stops;
  bool running = false;
  while (reloaded.isActive() && testNow < 300) {
    tick(addon, reloaded, 1);
    const auto pwm = testPwm.find(in2);
    const bool active = pwm != testPwm.end();
    if (active) assert(pwm->second == 255 - 147);
    if (active && !running) starts.push_back(testNow);
    if (!active && running) stops.push_back(testNow);
    running = active;
  }
  assert(reloaded.lastRunSucceeded() && starts.size() == 3 && stops.size() == 3);
  assert(starts[0] >= 8);
  for (size_t i = 0; i < starts.size(); ++i) {
    assert(stops[i] - starts[i] == 20);
    if (i > 0) assert(starts[i] - stops[i - 1] >= 15 && starts[i] - stops[i - 1] < 23);
  }

  command(reloaded, "RoutineCreate:steady");
  command(reloaded, "RoutineAdd:steady:MOTOR:40,0,200");
  command(reloaded, "RoutineRepeat:steady:FOREVER");
  command(reloaded, "RoutineSave:steady");
  command(addon, "Arm");
  assert(!reloaded.runNamed("steady", CommandSource::USB, "test"));
  command(reloaded, "RoutineRepeat:steady:1");
  command(reloaded, "RoutineSave:steady");
  assert(reloaded.runNamed("steady", CommandSource::USB, "test"));
  tick(addon, reloaded, 1);
  assert(testPwm.at(in2) == 55);
  tick(addon, reloaded, 45);
  assert(testPwm.empty() && reloaded.lastRunSucceeded());

  command(reloaded, "RoutineRepeat:motor:FOREVER");
  command(reloaded, "RoutineSave:motor");
  command(addon, "Arm");
  assert(reloaded.runNamed("motor", CommandSource::USB, "test"));
  tick(addon, reloaded, 50);
  assert(reloaded.isActive() && !testPwm.empty());
  assert(reloaded.stop(CommandSource::USB, "test", "operator stop", false));
  tick(addon, reloaded, 100);
  assert(!reloaded.isActive() && testPwm.empty());
  std::cout << "PASS saved motor timing, stored levels, steady run, repeat-until-stopped and cancellation\n";
}

void checkMultipleMotors() {
  if (AppConfig::DRV8871_MOTOR_COUNT < 2) return;
  Preferences::records.clear();
  testNow = 0;
  EventBus events;
  DispenserAddon addon(events);
  addon.begin();
  const auto &one = AppConfig::DRV8871_MOTORS[0];
  const auto &two = AppConfig::DRV8871_MOTORS[1];
  allInactive({one.in1, one.in2, two.in1, two.in2});
  command(addon, ("DispenserPin:" + String(two.in1)).c_str());
  assert(state(addon).find("\"pins\":[" + std::to_string(AppConfig::PIN_DISPENSER) + "]") != std::string::npos);
  command(addon, "Arm");
  command(addon, "MotorPulse:20,180,2");
  assert(testPwm.size() == 1 && testPwm.at(two.in2) == 75);
  command(addon, "MotorStart:220,1");
  command(addon, "MotorLevel:0,1");
  assert(testPwm.size() == 1 && testPwm.at(two.in2) == 75);
  command(addon, "MotorLevel:220,2");
  assert(testPwm.at(two.in2) == 35);
  command(addon, "MotorStop");
  allInactive({two.in1, two.in2}, HIGH);
  command(addon, "MotorStart:155,1");
  allInactive({two.in1, two.in2});
  assert(testPwm.size() == 1 && testPwm.at(one.in2) == 100);
  command(addon, "MotorCoast");

  RoutineEngine routine(events, addon);
  routine.begin();
  command(routine, "RoutineCreate:both");
  command(routine, "RoutineAdd:both:MOTOR:15,5,100");
  command(routine, "RoutineAdd:both:MOTOR:15,5,200,2");
  command(routine, "RoutineSave:both");
  RoutineEngine reloaded(events, addon);
  reloaded.begin();
  reloaded.configureSubmitter(send, &addon);
  assert(reloaded.runNamed("both", CommandSource::USB, "test"));
  std::vector<int> starts;
  int lastPin = -1;
  while (reloaded.isActive() && testNow < 100) {
    tick(addon, reloaded, 1);
    assert(testPwm.size() <= 1);
    const int pin = testPwm.empty() ? -1 : testPwm.begin()->first;
    if (pin >= 0 && lastPin != pin) starts.push_back(pin);
    lastPin = pin;
  }
  assert(reloaded.lastRunSucceeded());
  assert((starts == std::vector<int>{one.in2, two.in2}));
  allInactive({one.in1, one.in2, two.in1, two.in2});
  std::cout << "PASS independent pin pairs and saved sequence targeting two DRV8871 motors\n";
}
#endif

int main(){
#if DRONE_CFG_ENABLE_DRV8871
  checkMotor();
  checkMotorRoutines();
  checkMultipleMotors();
#endif
  Preferences::records.clear();testNow=0;EventBus events;DispenserAddon addon(events);addon.begin();
  assert(state(addon).find("\"pins\":[4]")!=std::string::npos);assert(!addon.hasActiveOutput());
  command(addon,"DispenserOutputs:4,47,15");command(addon,"DispenserSave");
  assert(state(addon).find("\"pins\":[4,47,15]")!=std::string::npos);allInactive({4,47,15});
  for(const char *bad:{"DispenserOutputs:4,4","DispenserOutputs:4,35","DispenserOutputs:4,3","DispenserOutputs:4,","DispenserOutputs:9999999999","DispenserOutputs:4,47,15,40,41,18,17,21,42"}){
    command(addon,bad);assert(state(addon).find("\"pins\":[4,47,15]")!=std::string::npos);
  }
  DispenserAddon reboot(events);reboot.begin();assert(state(reboot).find("\"pins\":[4,47,15]")!=std::string::npos);allInactive({4,47,15});
  command(reboot,"Arm");command(reboot,"DispenserOutputs:4");assert(state(reboot).find("\"pins\":[4,47,15]")!=std::string::npos);
  command(reboot,"DispensePin:47,10");assert(testLevels[47]==HIGH);allInactive({4,15});
  command(reboot,"DispensePin:15,10");assert(testLevels[47]==HIGH&&testLevels[15]==LOW); // No overlap.
  testNow+=10;reboot.service();allInactive({4,47,15});
  command(reboot,"DispensePin:40,10");allInactive({4,47,15});assert(!reboot.hasActiveOutput());
  command(reboot,"Dispense:10");assert(testLevels[4]==HIGH);command(reboot,"Disarm");allInactive({4,47,15});
  command(reboot,"DispenserActiveHigh:OFF");allInactive({4,47,15},HIGH);command(reboot,"Arm");command(reboot,"DispensePin:15,1");assert(testLevels[15]==LOW&&testLevels[4]==HIGH);
  reboot.stopAll(CommandSource::USB,"test");allInactive({4,47,15},HIGH);
  command(reboot,"DispenserActiveHigh:ON");command(reboot,"DispenserOutputs:4,47");assert(testModes[15]==INPUT&&testLevels[15]==LOW);
  command(reboot,"DispenserOutputs:4,47,15");command(reboot,"PayloadProfileSave:multi");
  command(reboot,"DispenserOutputs:4");command(reboot,"PayloadProfileUse:multi");assert(state(reboot).find("\"pins\":[4,47,15]")!=std::string::npos);allInactive({4,47,15});
  DispenserAddon profileBoot(events);profileBoot.begin();assert(state(profileBoot).find("\"pins\":[4,47,15]")!=std::string::npos);
  // Construct the original version-1, single-pin profile and its original CRC.
  auto &record=Preferences::records["drone-disp"]["prof0"];std::vector<uint8_t> legacy(record.begin(),record.begin()+44);
  legacy[4]=1;legacy[5]=0;uint32_t hash=2166136261U;for(size_t i=0;i<40;++i){hash^=legacy[i];hash*=16777619U;}memcpy(legacy.data()+40,&hash,4);record=legacy;
  DispenserAddon migrated(events);migrated.begin();assert(state(migrated).find("\"pins\":[4]")!=std::string::npos);
  command(migrated,"DispenserOutputs:4,47,15");command(migrated,"DispenserSave");
  RoutineEngine engine(events,migrated);engine.begin();engine.configureSubmitter(send,&migrated);
  command(engine,"RoutineCreate:multi");command(engine,"RoutineAdd:multi:START_WAIT:20");
  command(engine,"RoutineAdd:multi:OUTPUT:4,10,7");command(engine,"RoutineAdd:multi:OUTPUT:47,15,9");command(engine,"RoutineAdd:multi:OUTPUT:15,5,11");command(engine,"RoutineRepeat:multi:2");command(engine,"RoutineSave:multi");
  RoutineEngine reload(events,migrated);reload.begin();reload.configureSubmitter(send,&migrated);
  command(migrated,"Arm");testWrites.clear();const uint32_t start=testNow;assert(reload.runNamed("multi",CommandSource::USB,"test"));tick(migrated,reload,300);
  std::vector<GpioWrite> starts;for(auto write:testWrites)if(write.level==HIGH)starts.push_back(write);
  assert(starts.size()==6&&!reload.isActive()&&reload.lastRunSucceeded());assert(starts[0].at-start>=20);
  for(size_t i=0;i<starts.size();++i)assert(starts[i].pin==(i%3==0?4:i%3==1?47:15));
  assert(starts[1].at-starts[0].at>=17&&starts[2].at-starts[1].at>=24&&starts[3].at-starts[2].at>=16);
  assert(starts[3].at-starts[2].at<30);allInactive({4,47,15}); // Initial delay only once.
  command(migrated,"Arm");command(engine,"RoutineRepeat:multi:FOREVER");command(engine,"RoutineSave:multi");assert(engine.runNamed("multi",CommandSource::USB,"test"));tick(migrated,engine,300);assert(engine.isActive());engine.stop(CommandSource::USB,"test","stop");allInactive({4,47,15});
  for(unsigned elapsed:{5U,25U,35U}) {
    command(migrated,"Arm");assert(engine.runNamed("multi",CommandSource::USB,"test"));tick(migrated,engine,elapsed);
    engine.stop(CommandSource::USB,"test","stop phase");allInactive({4,47,15});testWrites.clear();tick(migrated,engine,200);
    for(auto write:testWrites)assert(write.level!=HIGH);
  }
  command(engine,"RoutineCreate:async");command(engine,"RoutineAdd:async:OUTPUT:4,1,5");command(engine,"RoutineAdd:async:OUTPUT:47,1,5");command(engine,"RoutineSave:async");
  struct Queue {String line;uint32_t at=0;} queue;
  auto enqueue=[](void *context,CommandSource,const String &line,const String &){auto &q=*static_cast<Queue*>(context);q.line=line;q.at=millis();return true;};
  engine.configureSubmitter(enqueue,&queue);command(migrated,"Arm");assert(engine.runNamed("async",CommandSource::USB,"test"));testWrites.clear();
  for(unsigned i=0;i<200&&engine.isActive();++i) {
    ++testNow;if(queue.line.length()&&testNow-queue.at>=30){migrated.handleCommand(queue.line,CommandSource::INTERNAL,"test");queue.line="";}
    engine.service();migrated.service();
  }
  starts.clear();for(auto write:testWrites)if(write.level==HIGH)starts.push_back(write);
  assert(!engine.isActive()&&engine.lastRunSucceeded()&&starts.size()==2&&starts[0].pin==4&&starts[1].pin==47);allInactive({4,47,15});
  command(migrated,"Arm");assert(engine.runNamed("async",CommandSource::USB,"test"));tick(migrated,engine,1200);
  assert(!engine.isActive()&&!engine.lastRunSucceeded());queue.line="";allInactive({4,47,15}); // Accepted but never executed must fail safe.
  engine.configureSubmitter(send,&migrated);
  command(migrated,"DispenserOutputs:4");command(migrated,"Arm");assert(!reload.runNamed("multi",CommandSource::USB,"test"));allInactive({4}); // Removed pin preflight.
  command(migrated,"Disarm");command(migrated,"DispenserOutputs:4,47,15");command(migrated,"DispenserMaxPulse:250");command(migrated,"DispenserArmTimeout:1000");command(migrated,"Arm");assert(!reload.runNamed("multi",CommandSource::USB,"test"));
  command(migrated,"Disarm");command(migrated,"DispenserArmTimeout:0");command(migrated,"DispenserMaxPulse:0");command(migrated,"Arm");
  testNow=UINT32_MAX-5;command(migrated,"DispensePin:47,10");testNow=4;migrated.service();allInactive({4,47,15});
  command(migrated,"Disarm");command(migrated,"DispenserOutputs:4,47,15,40,41,18,17,21");command(migrated,"DispenserSave");
  DispenserAddon eight(events);eight.begin();RoutineEngine eightEngine(events,eight);eightEngine.begin();eightEngine.configureSubmitter(send,&eight);
  command(eightEngine,"RoutineCreate:eight");command(eightEngine,"RoutineAdd:eight:START_WAIT:1");
  for(int pin:{4,47,15,40,41,18,17,21})command(eightEngine,("RoutineAdd:eight:OUTPUT:"+String(pin)+",1,0").c_str());
  command(eightEngine,"RoutineSave:eight");command(eight,"Arm");testWrites.clear();assert(eightEngine.runNamed("eight",CommandSource::USB,"test"));tick(eight,eightEngine,100);
  starts.clear();for(auto write:testWrites)if(write.level==HIGH)starts.push_back(write);assert(starts.size()==8&&eightEngine.lastRunSucceeded());allInactive({4,47,15,40,41,18,17,21});
  command(migrated,"Disarm");command(migrated,"DispenserDefaults");assert(state(migrated).find("\"pins\":[4]")!=std::string::npos);allInactive({4,47,15});
  std::cout<<"PASS actual dispenser pins/timing/no overlap, boot/settings/profiles and legacy migration, polarity, stop, repeat/delay, limits, removed pins, rollover\n";
}
