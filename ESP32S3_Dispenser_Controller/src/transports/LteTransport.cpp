#include "LteTransport.h"

#if APP_LTE_ENABLED
#define TINY_GSM_MODEM_SARAR4
#define TINY_GSM_RX_BUFFER 1024
#include <TinyGsmClient.h>
#include <ArduinoMqttClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <esp_system.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>
#include <new>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "../util/TextUtil.h"

namespace {
bool isPriorityStop(const char *body) {
  String line(body);line.trim();
  return line.equalsIgnoreCase("StopAll") || line.equalsIgnoreCase("Disarm") ||
    line.equalsIgnoreCase("DispenserDisarm") || line.equalsIgnoreCase("DispenserOff") ||
    line.equalsIgnoreCase("RoutineStop") || line.equalsIgnoreCase("GeoStop") ||
    line.equalsIgnoreCase("DispenseStop") || line.equalsIgnoreCase("GPIO26:OFF");
}
}

struct LteTransport::Impl {
  struct Command { char id[24]; char body[2049]; uint32_t deadline, generation, barrier; };
  struct Reply { char id[24]; char error[64]; bool accepted; uint16_t lines; uint32_t generation; };
  struct Seen { char id[24]{}; uint8_t hash[32]{}; bool ready=false; uint32_t at=0; Reply reply{}; };
  LteTransport &owner;
  HardwareSerial uart{LteConfig::UART_PORT};
  TinyGsm modem{uart};
  TinyGsmClientSecure socket{modem};
  MqttClient mqtt{socket};
  QueueHandle_t incoming=nullptr, outgoing=nullptr, stops=nullptr, stopReplies=nullptr;
  SemaphoreHandle_t snapshotMutex=nullptr;
  BatchCommandSubmitter submitter=nullptr;
  StateProvider provider=nullptr;
  void *submitContext=nullptr, *stateContext=nullptr;
  char snapshot[LteConfig::SNAPSHOT_BYTES]{};
  char frame[LteConfig::MESSAGE_BYTES]{};
  char session[33]{};
  String topic;
  Seen seen[32];
  Reply deferred[2]{};uint8_t deferredHead=0,deferredCount=0;
  std::atomic<uint32_t> generation{0}, barrier{0};
  uint32_t snapshotAt=0, snapshotCapturedAt=0, watchUntil=0;
  // ArduinoMqttClient's default callback ABI is a C function pointer. Each
  // firmware owns one LTE transport; do not change the library callback ABI.
  static Impl *receiver;
  static void messageThunk(int size) { if(receiver)receiver->receive(size); }
  explicit Impl(LteTransport &value) : owner(value), topic("devices/" + String(LteConfig::DEVICE_ID) + "/") {
    receiver=this;mqtt.onMessage(messageThunk);mqtt.setTimeout(1500);
  }
  void deferReply(const Reply &value) {
    // Worker-only ring: callbacks must not fill the control task's reply queue
    // between its space check and enqueue, or an original reply could be lost.
    if(deferredCount>=2)return;
    Reply copy=value;copy.generation=generation.load();
    deferred[(deferredHead+deferredCount)%2]=copy;++deferredCount;
  }
  void sendDeferredReplies() {
    const uint8_t available=deferredCount;
    for(uint8_t i=0;i<available&&uxQueueMessagesWaiting(stopReplies)==0;++i) {
      Reply response=deferred[deferredHead];deferredHead=(deferredHead+1)%2;--deferredCount;
      if(response.generation==generation.load())reply(response);
    }
  }
  void status(const char *text) { owner.events_.publish(EventLevel::STATUS,String("[LTE] ")+text); }
  bool send(const char *suffix, const String &payload, uint8_t qos=0) {
    if(!mqtt.connected() || payload.length() >= LteConfig::MESSAGE_BYTES) return false;
    if(!mqtt.beginMessage(topic+suffix,static_cast<unsigned long>(payload.length()),false,qos)) return false;
    // Bound each modem socket write; SARA-R4 AT sockets have smaller transfer
    // limits than the complete JSON state frame.
    for(size_t at=0;at<payload.length();at+=512) {
      const size_t remaining=payload.length()-at, count=remaining<512?remaining:512;
      if(mqtt.write(reinterpret_cast<const uint8_t*>(payload.c_str()+at),count)!=count){mqtt.stop();return false;}
    }
    return mqtt.endMessage() == 1;
  }
  void reply(const Reply &value) {
    JsonDocument json; json["id"]=value.id;json["session"]=session;
    json["accepted"]=value.accepted;json["acceptedLines"]=value.lines;
    json["latestEventId"]=owner.events_.latestId();
    if(value.error[0])json["error"]=value.error;
    String text;serializeJson(json,text);send("ack",text,1);
  }
  // TinyGSM enables socket encryption, but does not configure peer validation.
  // A CA must first be provisioned in the modem as LTE_CA_NAME. No insecure
  // fallback: unsupported commands, absent CA or bad modem time fail the link.
  bool tlsProfile() {
    modem.sendAT("+USECPRF=0");if(modem.waitResponse()!=1)return false;
    modem.sendAT("+USECPRF=0,0,3");if(modem.waitResponse()!=1)return false;
    modem.sendAT("+USECPRF=0,1,3");if(modem.waitResponse()!=1)return false;
    modem.sendAT("+USECPRF=0,3,\"",LteConfig::CA_NAME,"\"");if(modem.waitResponse()!=1)return false;
    modem.sendAT("+USECPRF=0,4,\"",LteConfig::HOST,"\"");return modem.waitResponse()==1;
  }
  bool connect() {
    status("connecting; modem calls are isolated from the control task");
    if(!modem.init() || !modem.waitForNetwork(15000L))return false;
    if(!modem.isGprsConnected() && !modem.gprsConnect(LteConfig::APN,LteConfig::APN_USER,LteConfig::APN_PASSWORD))return false;
    if(!tlsProfile()){status("TLS profile rejected; check CA, clock and modem firmware");return false;}
    for(unsigned i=0;i<4;++i)snprintf(session+i*8,9,"%08lx",static_cast<unsigned long>(esp_random()));
    generation.fetch_add(1);barrier.store(0);
    memset(seen,0,sizeof(seen));watchUntil=0;
    mqtt.setId(LteConfig::DEVICE_ID);mqtt.setUsernamePassword(LteConfig::MQTT_USER,LteConfig::MQTT_PASSWORD);
    mqtt.setCleanSession(true);mqtt.setKeepAliveInterval(15000);mqtt.setConnectionTimeout(5000);
    const String will="{\"online\":false}";
    if(!mqtt.beginWill(topic+"presence",static_cast<unsigned short>(will.length()),false,1))return false;
    mqtt.print(will);if(!mqtt.endWill())return false;
    if(!mqtt.connect(LteConfig::HOST,LteConfig::BROKER_PORT))return false;
    if(!mqtt.subscribe(topic+"command",1) || !mqtt.subscribe(topic+"watch",0)) {mqtt.stop();return false;}
    owner.connected_.store(true);status("MQTT/TLS connected");return true;
  }
  void receive(int size) {
    if(size<=0)return;
    if(size>=static_cast<int>(sizeof(frame)) || mqtt.messageRetain()){mqtt.stop();return;}
    const String receivedTopic=mqtt.messageTopic();
    const size_t got=mqtt.readBytes(frame,size);frame[got]='\0';
    if(got!=static_cast<size_t>(size)){mqtt.stop();return;}
    JsonDocument json;
    if(deserializeJson(json,frame,got,DeserializationOption::NestingLimit(5)))return;
    if(strcmp(json["session"]|"",session))return;
    if(receivedTopic==topic+"watch") {watchUntil=millis()+10000;return;}
    if(receivedTopic!=topic+"command")return;
    const char *id=json["id"]|"", *body=json["body"]|"";
    const size_t idLength=strlen(id), bodyLength=strlen(body);
    if(!idLength || idLength>23 || !bodyLength || bodyLength>2048 || (!json["deadline"].is<uint32_t>() || !json["barrier"].is<uint32_t>()))return;
    for(size_t i=0;i<idLength;++i)if(!isalnum(static_cast<unsigned char>(id[i]))&&id[i]!='-'&&id[i]!='_'&&id[i]!='.')return;
    uint8_t hash[32];mbedtls_sha256(reinterpret_cast<const uint8_t*>(body),bodyLength,hash,0);
    for(auto &entry:seen)if(!strcmp(entry.id,id)) {
      if(!memcmp(hash,entry.hash,sizeof(hash)) && entry.ready)deferReply(entry.reply);
      return; // Never re-execute a duplicate, even when its payload changed.
    }
    const uint32_t deadline=json["deadline"], now=millis();
    const int32_t remaining=static_cast<int32_t>(deadline-now);
    if(remaining<=0 || remaining>static_cast<int32_t>(LteConfig::COMMAND_TTL_MS))return;
    const bool priority=isPriorityStop(body);
    const uint32_t requestedBarrier=json["barrier"], currentBarrier=barrier.load();
    if(priority) {
      if(static_cast<int32_t>(requestedBarrier-currentBarrier)<0)return;
      if(requestedBarrier!=currentBarrier) {
        barrier.store(requestedBarrier);
        xQueueReset(incoming);
        // Cancel cached pending requests as well, so discarded queue entries
        // cannot hold dedup slots forever. Executed requests keep their result.
        for(auto &entry:seen)if(entry.id[0]&&!entry.ready) {
          entry.ready=true;strncpy(entry.reply.id,entry.id,sizeof(entry.reply.id)-1);
          strncpy(entry.reply.error,"Cancelled by newer remote stop",sizeof(entry.reply.error)-1);
          deferReply(entry.reply);
        }
      }
    } else if(requestedBarrier!=currentBarrier)return;
    Seen *slot=nullptr;
    for(auto &entry:seen)if(!entry.id[0] || (entry.ready && static_cast<uint32_t>(now-entry.at)>10000)){slot=&entry;break;}
    if(!slot&&!priority){Reply busy{};strncpy(busy.id,id,sizeof(busy.id)-1);strncpy(busy.error,"Remote request history busy",sizeof(busy.error)-1);deferReply(busy);return;}
    // A full normal-command history must not prevent a stop. An uncached stop
    // can repeat, but cannot repeat an actuation.
    if(slot){*slot=Seen{};strncpy(slot->id,id,sizeof(slot->id)-1);memcpy(slot->hash,hash,sizeof(hash));slot->at=now;}
    Command command{};strncpy(command.id,id,sizeof(command.id)-1);memcpy(command.body,body,bodyLength+1);
    command.deadline=deadline;command.generation=generation.load();command.barrier=requestedBarrier;
    if(priority) xQueueOverwrite(stops,&command);
    else if(xQueueSend(incoming,&command,0)!=pdTRUE && slot){
      slot->ready=true;strncpy(slot->reply.id,id,sizeof(slot->reply.id)-1);
      strncpy(slot->reply.error,"Remote command queue full",sizeof(slot->reply.error)-1);deferReply(slot->reply);
    }
  }
  void sendReplies(QueueHandle_t queue) {
    Reply response{};
    for(unsigned i=0;i<2;++i) {
      if(queue!=stopReplies&&uxQueueMessagesWaiting(stopReplies)>0)break;
      if(xQueueReceive(queue,&response,0)!=pdTRUE)break;
      if(response.generation!=generation.load())continue;
      for(auto &entry:seen)if(!strcmp(entry.id,response.id)){entry.reply=response;entry.ready=true;break;}
      reply(response);
    }
  }
  static void run(void *context) {static_cast<Impl*>(context)->loop();}
  void loop() {
    uart.setRxBufferSize(2048);uart.begin(LteConfig::UART_BAUD,SERIAL_8N1,LteConfig::RX_PIN,LteConfig::TX_PIN);
    uint32_t nextConnect=0,lastPresence=0,lastState=0,eventCursor=owner.events_.latestId();
    bool haveConnected=false,pendingEventGap=false;
    for(;;) {
      if(!mqtt.connected()) {
        if(owner.connected_.exchange(false)){generation.fetch_add(1);status("link lost; old command sessions invalidated");}
        if(haveConnected && static_cast<int32_t>(millis()-nextConnect)<0){vTaskDelay(pdMS_TO_TICKS(100));continue;}
        haveConnected=true;
        if(!connect()){nextConnect=millis()+15000;vTaskDelay(pdMS_TO_TICKS(100));continue;}
        lastPresence=millis()-10000;lastState=millis()-LteConfig::IDLE_STATE_MS;
      }
      // The callback also drains incoming publishes encountered while waiting
      // for a QoS acknowledgement. Never call parseMessage after poll here.
      mqtt.poll();
      sendReplies(stopReplies);
      sendReplies(outgoing);
      sendDeferredReplies();
      const uint32_t now=millis();
      const bool watched=static_cast<int32_t>(watchUntil-now)>0;
      if(now-lastPresence>=10000U || (watched && now-lastPresence>=2000U)) {
        send("presence","{\"online\":true,\"protocol\":2,\"barrier\":"+String(barrier.load())+",\"session\":\""+String(session)+"\",\"uptime\":"+String(now)+"}");lastPresence=now;
      }
      if(now-lastState>=(watched?LteConfig::ACTIVE_STATE_MS:LteConfig::IDLE_STATE_MS)) {
        String current;uint32_t captured=0;
        if(xSemaphoreTake(snapshotMutex,0)==pdTRUE){current=snapshot;captured=snapshotCapturedAt;xSemaphoreGive(snapshotMutex);}
        // Read the send timestamp after the protected snapshot. A control-core
        // update between the loop timestamp and the lock must not look like a
        // snapshot captured in the future (including millis wraparound).
        const uint32_t stateNow=millis();
        if(current.length())send("state","{\"session\":\""+String(session)+"\",\"uptime\":"+String(stateNow)+",\"captured\":"+String(captured)+",\"state\":"+current+"}");
        lastState=now;
      }
      EventRecord event{};bool gap=false;
      for(unsigned i=0;i<2;++i){
        uint32_t candidate=eventCursor;
        if(!owner.events_.nextAfter(candidate,event,gap))break;
        pendingEventGap=pendingEventGap||gap;
        if(!(event.targetMask&TRANSPORT_MASK_LTE)){eventCursor=candidate;continue;}
        JsonDocument json;json["session"]=session;json["id"]=event.id;json["t"]=event.text;json["l"]=eventLevelName(event.level);
        json["s"]=commandSourceName(event.source);json["r"]=event.requestId;json["q"]=event.rawPayload;
        json["gap"]=pendingEventGap;json["ageMs"]=static_cast<uint32_t>(millis()-event.timestampMs);
        String text;serializeJson(json,text);
        // Preserve available output across modem reconnects. A failed socket
        // write must not advance past that event; the fixed ring may still wrap.
        if(!send("event",text))break;
        eventCursor=candidate;pendingEventGap=false;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
};

LteTransport::Impl *LteTransport::Impl::receiver=nullptr;

bool LteTransport::begin(BatchCommandSubmitter submitter,void *submitContext,StateProvider provider,void *stateContext) {
  if(impl_)return started_;
  impl_=new(std::nothrow)Impl(*this);if(!impl_)return false;
  impl_->submitter=submitter;impl_->submitContext=submitContext;impl_->provider=provider;impl_->stateContext=stateContext;
  impl_->incoming=xQueueCreate(2,sizeof(Impl::Command));impl_->outgoing=xQueueCreate(2,sizeof(Impl::Reply));
  impl_->stops=xQueueCreate(1,sizeof(Impl::Command));impl_->stopReplies=xQueueCreate(1,sizeof(Impl::Reply));
  impl_->snapshotMutex=xSemaphoreCreateMutex();
  if(!impl_->incoming||!impl_->outgoing||!impl_->stops||!impl_->stopReplies||!impl_->snapshotMutex){events_.publish(EventLevel::ERROR,"[LTE] queue allocation failed");return false;}
  if(xTaskCreatePinnedToCore(Impl::run,"lte-modem",LteConfig::TASK_STACK_BYTES,impl_,1,nullptr,0)!=pdPASS){events_.publish(EventLevel::ERROR,"[LTE] worker allocation failed");return false;}
  started_ = true;
  return true;
}
void LteTransport::serviceControl() {
  if(!started_)return;
  Impl::Command command{};
  // Stop admission is independent of normal replies, including a modem worker
  // waiting for a network acknowledgement. The control task remains responsive.
  const bool priority=xQueueReceive(impl_->stops,&command,0)==pdTRUE;
  if(priority || (uxQueueSpacesAvailable(impl_->outgoing)>0 && xQueueReceive(impl_->incoming,&command,0)==pdTRUE)) {
    Impl::Reply reply{};strncpy(reply.id,command.id,sizeof(reply.id)-1);reply.generation=command.generation;
    if(command.generation!=impl_->generation.load() || command.barrier!=impl_->barrier.load() || static_cast<int32_t>(command.deadline-millis())<=0)
      strncpy(reply.error,"Command expired, stopped or link session changed",sizeof(reply.error)-1);
    else if(impl_->submitter) {
      reply.accepted=impl_->submitter(impl_->submitContext,CommandSource::LTE,String(command.body),String(command.id),reply.lines);
      if(!reply.accepted)strncpy(reply.error,"Command queue rejected the batch",sizeof(reply.error)-1);
    }
    if(priority)xQueueOverwrite(impl_->stopReplies,&reply);
    else xQueueSend(impl_->outgoing,&reply,0);
  }
  if(impl_->provider && static_cast<uint32_t>(millis()-impl_->snapshotAt)>=1000) {
    String snapshot=impl_->provider(impl_->stateContext);
    if(xSemaphoreTake(impl_->snapshotMutex,0)==pdTRUE){
      if(snapshot.length()<LteConfig::SNAPSHOT_BYTES){snapshot.toCharArray(impl_->snapshot,sizeof(impl_->snapshot));impl_->snapshotCapturedAt=millis();}
      else impl_->snapshot[0]='\0'; // Never refresh the timestamp of an older, smaller snapshot.
      xSemaphoreGive(impl_->snapshotMutex);
    }
    impl_->snapshotAt=millis();
  }
}
#else
bool LteTransport::begin(BatchCommandSubmitter,void*,StateProvider,void*) { return true; }
void LteTransport::serviceControl() {}
#endif
