#include "../../src/addons/dispenser/DispenserAddon.h"
#include "../../src/core/RoutineEngine.h"
#include "../../src/util/TextUtil.h"
#include <cassert>
#include <iostream>

static String state(DispenserAddon &addon){String out="{";addon.appendStateJson(out,false);return out+"}";}
static void command(DispenserAddon &addon,const char *line){assert(addon.handleCommand(line,CommandSource::USB,"host"));}

int main(){
  for (const char *line : {"MotorStop", "MotorCoast:1", "TMCStop:4", "TMCDisable:2"}) assert(TextUtil::isMotorStop(line));
  for (const char *line : {"MotorStop:5", "TMCStop:0", "TMCStop:1\nArm", "TMCPulse:100,30,1"}) assert(!TextUtil::isMotorStop(line));
  DriverWiring unused;unused.defaults();String unusedReason;
  assert(unused.update("MotorPins:1,OFF",unusedReason)&&unused.dcCount==0);
  assert(unused.update("TMCPins:1,OFF",unusedReason)&&unused.stepCount==0);
  assert(!unused.owns(AppConfig::TMC2209_TX_PIN));
  Tmc2209Motor noStepper;noStepper.begin(unused);assert(!noStepper.busy());
  Preferences::records.clear();EventBus events;DispenserAddon initial(events);initial.begin();
  assert(!initial.isBusy());
  command(initial,"MotorStart:128");assert(!initial.isBusy());
  command(initial,"TMCRun:30,1");assert(!initial.isBusy());
  DriverWiring defaults;defaults.defaults();
  const auto before=Preferences::records["motor-wiring"];
  command(initial,"MotorPins:1,19,20"); // Reserved USB/strapping or default stepper ownership.
  assert(Preferences::records["motor-wiring"]==before);
  command(initial,"MotorPins:2,13,14");
#if DRONE_CFG_BOARD_ESP32_S3
  command(initial,"TMCPins:2,15,16,21,1");
#else
  command(initial,"TMCPins:2,16,17,25,1");
#endif
  assert(state(initial).value.find("\"pending\":true")!=std::string::npos);
  command(initial,"Arm");command(initial,"MotorStart:128");assert(!initial.isBusy());
  initial.stopAll(CommandSource::USB,"host");
  DriverWiring saved;saved.load();assert(saved.dcCount==2&&saved.stepCount==2);
  DispenserAddon addon(events);addon.begin();
  command(addon,"Arm");
  command(addon,"MotorPulse:100,128,1");command(addon,"MotorPulse:300,180,2");
  command(addon,"TMCPulse:200,30,1,1");command(addon,"TMCPulse:400,30,-1,2");
  assert(testPwm.count(saved.dc[0].in2)&&testPwm.count(saved.dc[1].in2));
  assert(testLevels[saved.step[0].enable]==LOW&&testLevels[saved.step[1].enable]==LOW);
  testNow+=100;addon.service();assert(!testPwm.count(saved.dc[0].in2)&&testPwm.count(saved.dc[1].in2));
  assert(testLevels[saved.step[0].enable]==LOW&&testLevels[saved.step[1].enable]==LOW);
  testNow+=100;addon.service();assert(testLevels[saved.step[0].enable]==HIGH&&testLevels[saved.step[1].enable]==LOW);
  command(addon,"TMCStop:2");assert(testLevels[saved.step[1].enable]==HIGH&&testPwm.count(saved.dc[1].in2));
  command(addon,"MotorCoast:2");assert(!addon.isBusy());
  command(addon,"MotorStart:180,1");command(addon,"MotorStart:200,2");
  command(addon,"MotorStop:1");assert(!testPwm.count(saved.dc[0].in2)&&testPwm.count(saved.dc[1].in2));
  assert(testLevels[saved.dc[0].in1]==HIGH&&testLevels[saved.dc[0].in2]==HIGH);
  command(addon,"Disarm");assert(!addon.isBusy()&&testPwm.empty());
  assert(testLevels[saved.dc[0].in1]==LOW&&testLevels[saved.dc[0].in2]==LOW);
  command(addon,"TMCCurrent:351");assert(events.messages.back().value.find("50..350")!=std::string::npos);
  command(addon,"TMCMicrosteps:3");assert(events.messages.back().value.find("power of two")!=std::string::npos);
  testUartOnline=false;command(addon,"TMCInit");command(addon,"Arm");command(addon,"TMCRun:30,1");
  assert(!addon.isBusy()&&testLevels[saved.step[0].enable]==HIGH);
  command(addon,"Disarm");testUartOnline=true;testUartAcceptWrites=false;command(addon,"TMCInit");
  command(addon,"Arm");command(addon,"TMCRun:30,1");assert(!addon.isBusy());
  command(addon,"Disarm");testUartAcceptWrites=true;testUartDropCurrentWrite=true;command(addon,"TMCInit");
  command(addon,"Arm");command(addon,"TMCRun:30,1");assert(!addon.isBusy());
  testUartDropCurrentWrite=false;
  command(addon,"Disarm");testUartAcceptWrites=true;command(addon,"TMCInit");
  command(addon,"Arm");testUartOnline=false;command(addon,"TMCPulse:100,30,1,1");
  assert(!addon.isBusy()&&testLevels[saved.step[0].enable]==HIGH);
  command(addon,"Disarm");testUartOnline=true;command(addon,"TMCInit");
  command(addon,"Arm");testNow=UINT32_MAX-40;
  command(addon,"MotorPulse:100,120,1");command(addon,"TMCPulse:100,30,1,1");
  testNow+=100;addon.service();assert(!addon.isBusy());
  const auto protectedMap=Preferences::records["motor-wiring"];
  command(addon,"MotorPins:1,13,14"); // Armed: no configuration change.
  assert(Preferences::records["motor-wiring"]==protectedMap);
  command(addon,"Disarm");command(addon,"TMCPins:2,13,14,15,0"); // DC ownership + duplicate UART address.
  assert(Preferences::records["motor-wiring"]==protectedMap);
  RoutineEngine routine(events,addon);routine.begin();
  assert(routine.handleCommand("RoutineCreate:step",CommandSource::USB,"host"));
  assert(routine.handleCommand("RoutineAdd:step:TMC:200,300,30,1,2",CommandSource::USB,"host"));
  assert(routine.handleCommand("RoutineRepeat:step:3",CommandSource::USB,"host"));
  assert(routine.handleCommand("RoutineSave:step",CommandSource::USB,"host"));
  assert(routine.stateJson(false).value.find(",1,30,1,2]")!=std::string::npos);
  command(addon,"Arm");String reason;assert(addon.validateRoutineCommand("TMCPulse:200,30,1,2",reason));
  command(addon,"TMCPulse:200,30,1,2");assert(addon.isBusy());
  assert(!routine.runNamed("step",CommandSource::USB,"host"));
  addon.stopAll(CommandSource::USB,"host");
  // Corrupt persisted wiring never drives unchecked GPIOs at boot.
  auto &record=Preferences::records["motor-wiring"]["pins"];record[0]^=0x40;
  DriverWiring fallback;fallback.load();assert(fallback.dcCount==AppConfig::DRV8871_MOTOR_COUNT);
  std::cout<<"PASS pin validation/persistence/restart, simultaneous DC+TMC pulses, independent stops, global disarm, UART failure, current limits, timer rollover and saved TMC routines\n";
}
