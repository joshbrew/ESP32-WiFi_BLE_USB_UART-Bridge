#pragma once
#include "HardwareSerial.h"
inline bool testUartOnline = true, testUartAcceptWrites = true, testUartDropCurrentWrite = false;
class TMC2209Stepper {
  uint8_t counter=0;uint32_t config=0;uint16_t ma=300,steps=16;
  void write(){if(testUartAcceptWrites)++counter;}
  void bit(int n,bool on){if(testUartAcceptWrites){if(on)config|=1U<<n;else config&=~(1U<<n);write();}}
 public:
  bool CRCerror=false;
  uint32_t IHOLD_IRUN(){return ma;}
  void IHOLD_IRUN(uint32_t){if(!testUartDropCurrentWrite)write();}
  TMC2209Stepper(HardwareSerial*,float,uint8_t) {}
  uint32_t IOIN(){return testUartOnline?0x21000000U:0;}
  uint8_t IFCNT(){CRCerror=!testUartOnline;return testUartOnline?counter:0;}
  uint32_t GCONF(){return testUartOnline?config:0;}
  void begin(){write();}
  void pdn_disable(bool b){bit(6,b);}void I_scale_analog(bool b){bit(0,b);}
  void internal_Rsense(bool b){bit(1,b);}void mstep_reg_select(bool b){bit(7,b);}
  void toff(int){write();}void blank_time(int){write();}
  void rms_current(uint16_t value,float){if(testUartAcceptWrites)ma=value;write();}
  uint16_t rms_current(){return testUartOnline?ma:0;}
  void microsteps(uint16_t value){if(testUartAcceptWrites)steps=value;write();}
  uint16_t microsteps(){return testUartOnline?steps:0;}
  void intpol(bool){write();}void en_spreadCycle(bool){write();}
  void pwm_autoscale(bool){write();}void VACTUAL(int){write();}
};
