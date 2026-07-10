#ifndef ESP32_STEPPER_DUAL_DAC_TRANSPORTHUB_H
#define ESP32_STEPPER_DUAL_DAC_TRANSPORTHUB_H

#include <Arduino.h>
#include <freertos/semphr.h>

#include "../config/AppConfig.h"

#if APP_CLASSIC_BT_SPP_ENABLED
#include <BluetoothSerial.h>
#endif
#if APP_BLE_ENABLED
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#endif
#if APP_AUX_UART_ENABLED
#include <HardwareSerial.h>
#endif

#include "../core/AppTypes.h"
#include "../util/ByteRingBuffer.h"
#include "../core/EventBus.h"
#include "../util/LineAssembler.h"

// EDIT HERE to add a physical command transport or change stream throttling.
// New transports should assemble newline-delimited input, call submitLine(), and
// consume EventBus records with their own cursor and bounded output buffer.
class TransportHub {
 public:
  explicit TransportHub(EventBus &events);

  void begin();
  void configureCommandSubmitter(CommandSubmitter submitter, void *context);
  void configureStateProvider(StateProvider provider, void *context);
  void service();

  void setSppEnabled(bool enabled);
  bool prepareBleController();
  void setBleEnabled(bool enabled);
  void setBleDormant(bool dormant);
  void restartBleAdvertising();

  bool isSppRunning() const;
  bool isSppConnected() const;
  bool isBleRunning() const;
  bool isBleConnected() const;
  bool isBleOutputReady() const;
  bool isBleDormant() const;
  bool isBleInitialized() const;
  bool isUartRunning() const;
  bool isTransportAvailable(CommandSource source) const;
  String usbStatusText() const;
  String stateJson() const;

 private:
#if APP_BLE_ENABLED
  class BleServerCallbacks;
  class BleRxCallbacks;
  class BleTxDescriptorCallbacks;
#endif

  void serviceUsbInput();
  void serviceSppInput();
  void serviceBleInput();
  void serviceBleIdleSubmit();
  void serviceUartInput();
  void serviceOutput();

  void handleBleLine(const String &line);
  void queueBleDirectResponse(const String &line);
  void submitLine(CommandSource source, const String &line);
  String makeRequestId(CommandSource source);
  void inputError(CommandSource source, const String &message);

  bool ensureBluetoothDualMode();
  bool initializeBleController();
  bool initializeBleStack();
  void startSpp();
  void stopSpp();
  void startBle();
  void stopBle();

  void onBleConnected();
  void onBleDisconnected();
  void onBleWrite(const char *data, size_t length);
  void onBleNotificationsChanged(bool enabled);

  void fillOutputQueue(
    uint32_t &cursor,
    ByteRingBuffer<AppConfig::TRANSPORT_OUTPUT_BUFFER_BYTES> &queue,
    uint8_t eventBudget,
    const char *transportName,
    TransportMask transportMask
  );
  void flushUsb();
  void flushSpp();
  void flushBle();
  void flushUart();
  String formatEvent(const EventRecord &record) const;

  EventBus &events_;
  CommandSubmitter submitter_;
  void *submitContext_;
  StateProvider stateProvider_;
  void *stateContext_;

#if APP_CLASSIC_BT_SPP_ENABLED
  BluetoothSerial serialBt_;
#endif
#if APP_AUX_UART_ENABLED
  HardwareSerial auxUart_;
#endif
  bool uartRunning_;
#if APP_CLASSIC_BT_SPP_ENABLED
  bool sppDesired_;
  bool sppInitialized_;
  bool sppRunning_;
  bool sppConnected_;
  bool previousSppConnected_;
#endif

  bool bleDesired_;
  bool bleControllerInitialized_;
  bool bleInitialized_;
  bool bleStandalone_;
  bool bleRunning_;
  bool bleConnected_;
  bool bleNotificationsEnabled_;
  bool bleDormant_;
  bool bleRestartPending_;
  bool bleInputOverflowPending_;
  bool bleIdleSubmitPending_;
  bool bleDormancyHeapCheckPending_;
  uint32_t bleDormancyHeapCheckAtMs_;
  uint32_t bleDormancyHeapBefore_;
  uint32_t bleDormancyLargestBefore_;
  int8_t bleConnectionEvent_;
  int8_t bleSubscriptionEvent_;
  uint32_t bleConnectedAtMs_;
  uint32_t bleNotificationsEnabledAtMs_;
  uint32_t bleLastPressureWarningAtMs_;
#if APP_BLE_ENABLED
  BLEServer *bleServer_;
  BLEService *bleService_;
  BLECharacteristic *bleTx_;
  BLECharacteristic *bleRx_;
  BLE2902 *bleTxCccd_;
#endif

  LineAssembler usbAssembler_;
#if APP_CLASSIC_BT_SPP_ENABLED
  LineAssembler sppAssembler_;
#endif
#if APP_BLE_ENABLED
  LineAssembler bleAssembler_;
#endif
#if APP_AUX_UART_ENABLED
  LineAssembler uartAssembler_;
#endif

#if APP_BLE_ENABLED
  ByteRingBuffer<512> bleInput_;
  portMUX_TYPE bleInputMux_;
  mutable portMUX_TYPE bleStateMux_;
  SemaphoreHandle_t bleNotifyMutex_;
#endif

  ByteRingBuffer<AppConfig::TRANSPORT_OUTPUT_BUFFER_BYTES> usbOutput_;
#if APP_CLASSIC_BT_SPP_ENABLED
  ByteRingBuffer<AppConfig::TRANSPORT_OUTPUT_BUFFER_BYTES> sppOutput_;
#endif
#if APP_BLE_ENABLED
  ByteRingBuffer<AppConfig::TRANSPORT_OUTPUT_BUFFER_BYTES> bleOutput_;
#endif
#if APP_AUX_UART_ENABLED
  ByteRingBuffer<AppConfig::TRANSPORT_OUTPUT_BUFFER_BYTES> uartOutput_;
#endif
  uint32_t usbCursor_;
#if APP_CLASSIC_BT_SPP_ENABLED
  uint32_t sppCursor_;
#endif
#if APP_BLE_ENABLED
  uint32_t bleCursor_;
#endif
#if APP_AUX_UART_ENABLED
  uint32_t uartCursor_;
#endif
#if APP_BLE_ENABLED
  uint32_t lastBleNotifyAtMs_;
  uint32_t bleLastInputAtMs_;
  String bleDirectOutput_;
  size_t bleDirectOffset_;
#endif
  uint32_t requestCounter_;
};

#endif  // ESP32_STEPPER_DUAL_DAC_TRANSPORTHUB_H
