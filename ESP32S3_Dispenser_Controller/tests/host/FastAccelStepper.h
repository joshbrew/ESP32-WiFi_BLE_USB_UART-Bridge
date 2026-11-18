#pragma once
#include <Arduino.h>
enum class MoveResultCode:int8_t {OK=0,FAILED=-1};
class FastAccelStepper {
  uint8_t enable=255;int32_t position=0;bool running=false;
 public:
  void setDirectionPin(uint8_t,bool,uint16_t){}
  void setEnablePin(uint8_t pin,bool){enable=pin;}
  void setAutoEnable(bool){}
  int8_t setSpeedInHz(uint32_t hz){return hz?0:-1;}
  int8_t setAcceleration(int32_t a){return a>0?0:-1;}
  bool enableOutputs(){digitalWrite(enable,LOW);return true;}
  bool disableOutputs(){digitalWrite(enable,HIGH);return true;}
  MoveResultCode move(int32_t){running=true;return MoveResultCode::OK;}
  bool isRunning()const{return running;}
  int32_t getCurrentPosition()const{return position;}
  void setCurrentPosition(int32_t p){position=p;}
  void forceStopAndNewPosition(int32_t p){running=false;position=p;}
};
class FastAccelStepperEngine {
  FastAccelStepper motors[4];size_t count=0;
 public:
  void init(){}
  FastAccelStepper* stepperConnectToPin(uint8_t){return count<4?&motors[count++]:nullptr;}
};
