#include "TransportBridge.h"

#include <cstring>

#include "../core/EventBus.h"
#include "../util/TextUtil.h"
#include "TransportHub.h"
#include "../web/WebPortal.h"

TransportBridge::TransportBridge(
  EventBus &events,
  TransportHub &transports,
  WebPortal &webPortal
) : events_(events),
    transports_(transports),
    webPortal_(webPortal),
    sentCount_(0),
    rejectedCount_(0) {}

bool TransportBridge::parsePayload(
  const String &command,
  const char *prefix,
  String &payload
) const {
  const size_t prefixLength = strlen(prefix);
  if (command.length() < prefixLength || !command.substring(0, static_cast<unsigned int>(prefixLength)).equalsIgnoreCase(String(prefix))) {
    return false;
  }
  if (command.length() == prefixLength) {
    payload = String();
    return true;
  }
  const char separator = command.charAt(prefixLength);
  if (separator != ':' && separator != ' ' && separator != '\t') {
    return false;
  }
  payload = command.substring(prefixLength + 1);
  payload.trim();
  return true;
}

bool TransportBridge::handleCommand(
  const String &command,
  CommandSource source,
  const String &requestId
) {
  if (command.equalsIgnoreCase("SendStatus")) {
    events_.publish(EventLevel::STATUS, "[SEND] " + statusText(), source, requestId);
    return true;
  }

  struct RouteCommand {
    const char *name;
    TransportMask mask;
  };
  static const RouteCommand ROUTES[] = {
    {"SendBLE", TRANSPORT_MASK_BLE},
    {"SendWiFi", TRANSPORT_MASK_WIFI},
    {"SendUSB", TRANSPORT_MASK_USB},
    {"SendSerial", TRANSPORT_MASK_USB},
    {"SendUART", TRANSPORT_MASK_UART},
    {"SendSPP", TRANSPORT_MASK_SPP},
  };

  String payload;
  for (const RouteCommand &route : ROUTES) {
    if (!parsePayload(command, route.name, payload)) {
      continue;
    }
    if (payload.length() == 0) {
      error(source, requestId, String(route.name) + " payload is empty");
      return true;
    }
    sendMask(source, route.mask, payload, requestId);
    return true;
  }

  if (!parsePayload(command, "Send", payload)) {
    return false;
  }
  if (payload.length() == 0) {
    error(source, requestId, "Send payload is empty");
    return true;
  }
  const TransportMask destinations = availableDestinations(source);
  if (destinations == TRANSPORT_MASK_NONE) {
    error(source, requestId, "no other active output transport is available");
    return true;
  }
  sendMask(source, destinations, payload, requestId);
  return true;
}

bool TransportBridge::destinationAvailable(
  CommandSource destination,
  String &reason
) const {
  if (destination == CommandSource::WIFI) {
    if (!webPortal_.isRunning()) {
      reason = "Wi-Fi portal is not running";
      return false;
    }
    if (!webPortal_.hasRecentClient()) {
      reason = "Wi-Fi portal has no active browser client";
      return false;
    }
    reason = String();
    return true;
  }
  if (destination == CommandSource::BLE) {
    if (!transports_.isBleConnected()) {
      reason = "BLE client is not connected";
      return false;
    }
    if (!transports_.isBleOutputReady()) {
      reason = "BLE TX notifications are not enabled or still settling";
      return false;
    }
    reason = String();
    return true;
  }
  if (transports_.isTransportAvailable(destination)) {
    reason = String();
    return true;
  }
  switch (destination) {
    case CommandSource::BLE: reason = "BLE client is not connected"; break;
    case CommandSource::SPP: reason = "Classic Bluetooth SPP client is not connected"; break;
    case CommandSource::UART: reason = "auxiliary UART is not running"; break;
    case CommandSource::USB: reason = "USB serial is unavailable"; break;
    default: reason = "destination is unavailable"; break;
  }
  return false;
}

TransportMask TransportBridge::availableDestinations(CommandSource origin) const {
  TransportMask mask = TRANSPORT_MASK_NONE;
  static const CommandSource DESTINATIONS[] = {
    CommandSource::USB,
    CommandSource::SPP,
    CommandSource::BLE,
    CommandSource::WIFI,
    CommandSource::UART,
  };
  for (CommandSource destination : DESTINATIONS) {
    if (destination == origin) {
      continue;
    }
    String reason;
    if (destinationAvailable(destination, reason)) {
      mask |= transportMaskForSource(destination);
    }
  }
  return mask;
}

String TransportBridge::transportList(TransportMask mask) const {
  String text;
  static const CommandSource DESTINATIONS[] = {
    CommandSource::USB,
    CommandSource::SPP,
    CommandSource::BLE,
    CommandSource::WIFI,
    CommandSource::UART,
  };
  for (CommandSource destination : DESTINATIONS) {
    if ((mask & transportMaskForSource(destination)) == 0) {
      continue;
    }
    if (text.length() > 0) {
      text += ',';
    }
    text += commandSourceName(destination);
  }
  return text.length() > 0 ? text : String("none");
}

bool TransportBridge::sendMask(
  CommandSource origin,
  TransportMask destinations,
  const String &payload,
  const String &requestId
) {
  destinations &= TRANSPORT_MASK_ALL;

  TransportMask active = TRANSPORT_MASK_NONE;
  String unavailable;
  static const CommandSource DESTINATIONS[] = {
    CommandSource::USB,
    CommandSource::SPP,
    CommandSource::BLE,
    CommandSource::WIFI,
    CommandSource::UART,
  };
  for (CommandSource destination : DESTINATIONS) {
    const TransportMask bit = transportMaskForSource(destination);
    if ((destinations & bit) == 0) {
      continue;
    }
    String reason;
    if (destinationAvailable(destination, reason)) {
      active |= bit;
    } else {
      if (unavailable.length() > 0) {
        unavailable += "; ";
      }
      unavailable += commandSourceName(destination);
      unavailable += '=';
      unavailable += reason;
    }
  }

  if (active == TRANSPORT_MASK_NONE) {
    rejectedCount_++;
    error(origin, requestId, "send destination unavailable" + (unavailable.length() > 0 ? ": " + unavailable : String()));
    return false;
  }

  if (payload.length() >= AppConfig::EVENT_TEXT_BYTES) {
    rejectedCount_++;
    error(origin, requestId, "payload exceeds " + String(AppConfig::EVENT_TEXT_BYTES - 1) + " bytes");
    return false;
  }

  sentCount_++;
  events_.publish(
    EventLevel::STATUS,
    payload,
    origin,
    requestId,
    active,
    true
  );

  TransportMask acknowledgementTargets = isExternalCommandSource(origin)
    ? static_cast<TransportMask>(transportMaskForSource(origin) | TRANSPORT_MASK_USB | TRANSPORT_MASK_UART)
    : static_cast<TransportMask>(TRANSPORT_MASK_USB | TRANSPORT_MASK_UART);
  acknowledgementTargets = static_cast<TransportMask>(acknowledgementTargets & ~active);
  if (acknowledgementTargets != TRANSPORT_MASK_NONE) {
    events_.publish(
      unavailable.length() == 0 ? EventLevel::STATUS : EventLevel::WARNING,
      "Queued for " + transportList(active) +
        (unavailable.length() > 0 ? "; unavailable=" + unavailable : String()),
      origin,
      requestId,
      acknowledgementTargets
    );
  }
  return true;
}

String TransportBridge::statusText() const {
  String wifiReason;
  const bool wifi = destinationAvailable(CommandSource::WIFI, wifiReason);
  return "sent=" + String(sentCount_) +
    " rejected=" + String(rejectedCount_) +
    " usb=ready" +
    " wifi=" + String(wifi ? "ready" : "off") +
    " ble=" + String(transports_.isBleOutputReady() ? "ready" : (transports_.isBleConnected() ? "connected-no-notify" : "off")) +
    " spp=" + String(transports_.isTransportAvailable(CommandSource::SPP) ? "connected" : "off") +
    " uart=" + String(transports_.isTransportAvailable(CommandSource::UART) ? "ready" : "off");
}

String TransportBridge::stateJson() const {
  String wifiReason;
  const bool wifi = destinationAvailable(CommandSource::WIFI, wifiReason);
  String json = "{\"sent\":" + String(sentCount_);
  json += ",\"rejected\":" + String(rejectedCount_);
  json += ",\"usb\":true";
  json += ",\"wifi\":" + TextUtil::jsonBool(wifi);
  json += ",\"ble\":" + TextUtil::jsonBool(transports_.isBleOutputReady());
  json += ",\"bleConnected\":" + TextUtil::jsonBool(transports_.isBleConnected());
  json += ",\"spp\":" + TextUtil::jsonBool(transports_.isTransportAvailable(CommandSource::SPP));
  json += ",\"uart\":" + TextUtil::jsonBool(transports_.isTransportAvailable(CommandSource::UART));
  json += "}";
  return json;
}

void TransportBridge::error(
  CommandSource source,
  const String &requestId,
  const String &message
) {
  events_.publish(EventLevel::ERROR, "[SEND] " + message, source, requestId);
}
