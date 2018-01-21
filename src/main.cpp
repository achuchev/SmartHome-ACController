#include <Arduino.h>
#include <IRsend.h>
#include <ArduinoJson.h>
#include <IRremoteESP8266.h>
#include <ir_Daikin.h>
#include <CommonConfig.h>
#include <MqttClient.h>
#include <FotaClient.h>
#include <ESPWifiClient.h>
#include <RemotePrint.h>
#include "settings.h"

IRDaikinESP daikinAC(PIN_IR_SEND);
MqttClient *mqttClient    = NULL;
FotaClient *fotaClient    = new FotaClient(DEVICE_NAME);
ESPWifiClient *wifiClient = new ESPWifiClient(WIFI_SSID, WIFI_PASS);

long lastStatusMsgSentAt = 0;

bool isACPowerOn() {
  int acPowerStatusValue = analogRead(PIN_POWER_ACTUAL);

  if (acPowerStatusValue >= 700) {
    return true;
  }
  return false;
}

bool setACPowerStatus(bool powerOn) {
  PRINT("AC: Power Status set to: ");

  if (powerOn) {
    daikinAC.on();
    PRINTLN("ON");
    return true;
  } else {
    daikinAC.off();
    PRINTLN("OFF");
    return false;
  }
}

void acPublishStatus(const char *messageId     = NULL,
                     bool        forcePublish  = false,
                     bool        forcedPowerOn = false) {
  long now = millis();

  if ((forcePublish) or (now - lastStatusMsgSentAt >
                         MQTT_PUBLISH_STATUS_INTERVAL)) {
    lastStatusMsgSentAt = now;

    const size_t bufferSize = JSON_ARRAY_SIZE(4) + 5 * JSON_OBJECT_SIZE(1);
    DynamicJsonBuffer jsonBuffer(bufferSize);
    JsonObject& root   = jsonBuffer.createObject();
    JsonObject& status = root.createNestedObject("status");

    if (messageId != NULL) {
      root["messageId"] = messageId;
      status["powerOn"] = forcedPowerOn;
    } else {
      status["powerOn"] = isACPowerOn();
    }

    status["temp"] = daikinAC.getTemp();

    switch (daikinAC.getMode()) {
      case DAIKIN_AUTO: {
        status["mode"] = "auto";
        break;
      }

      case DAIKIN_COOL: {
        status["mode"] = "cool";
        break;
      }

      case DAIKIN_HEAT: {
        status["mode"] = "heat";
        break;
      }

      case DAIKIN_FAN: {
        status["mode"] = "fan";
        break;
      }

      case DAIKIN_DRY: {
        status["mode"] = "dry";
        break;
      }

      default: {
        PRINT("Invalid Mode: ");
        PRINTLN(daikinAC.getMode());
      }
    }

    switch (daikinAC.getFan()) {
      case DAIKIN_FAN_AUTO: {
        status["fan"] = "auto";
        break;
      }

      case DAIKIN_FAN_MIN: {
        status["fan"] = "min";
        break;
      }

      case DAIKIN_FAN_MAX: {
        status["fan"] = "max";
        break;
      }

      default: {
        status["fan"] = char(daikinAC.getFan());
      }
    }

    status["swingVertical"]   = bool(daikinAC.getSwingVertical());
    status["swingHorizontal"] = bool(daikinAC.getSwingHorizontal());

    status["quiet"]    = daikinAC.getQuiet();
    status["powerful"] = daikinAC.getPowerful();

    // convert to String
    String outString;
    root.printTo(outString);

    // publish the message
    mqttClient->publish(MQTT_TOPIC_GET, outString);
  }
}

void acIRSend(String payload) {
  PRINTLN("AC: Sending IR signal.");

  // parse the JSON
  const size_t bufferSize = JSON_OBJECT_SIZE(1) + JSON_OBJECT_SIZE(8) + 130;
  DynamicJsonBuffer jsonBuffer(bufferSize);
  JsonObject& root   = jsonBuffer.parseObject(payload);
  JsonObject& status = root.get<JsonObject&>("status");

  if (!status.success()) {
    PRINTLN_E("AC: JSON with \"status\" key not received.");
    PRINTLN_E(payload);
    return;
  }
  bool needToSendIR   = false;
  const char *powerOn = status.get<const char *>("powerOn");
  bool powerOnBool    = isACPowerOn();

  if (powerOn) {
    needToSendIR = true;
    powerOnBool  = setACPowerStatus(strcasecmp(powerOn, "true") == 0);
  }

  const char *modeValue = status.get<const char *>("mode");

  if (modeValue) {
    needToSendIR = true;
    PRINT("AC: Mode set to: ");

    if (strcasecmp(modeValue, "auto") == 0) {
      daikinAC.setMode(DAIKIN_AUTO);
      PRINTLN("AUTO");
    } else if (strcasecmp(modeValue, "cool") == 0) {
      daikinAC.setMode(DAIKIN_COOL);
      PRINTLN("COOL");
    } else if (strcasecmp(modeValue, "heat") == 0) {
      daikinAC.setMode(DAIKIN_HEAT);
      PRINTLN("HEAT");
    } else if (strcasecmp(modeValue, "fan") == 0) {
      daikinAC.setMode(DAIKIN_FAN);
      PRINTLN("FAN");
    } else if (strcasecmp(modeValue, "dry") == 0) {
      daikinAC.setMode(DAIKIN_DRY);
      PRINTLN("DRY");
    } else {
      PRINT("Invalid value (");
      PRINT(modeValue);
      PRINTLN(")");
    }
  }

  int tempDeltaValue = status.get<int>("tempDelta");

  if (tempDeltaValue) {
    needToSendIR = true;
    PRINT("AC: Temp Delta is: ");
    PRINTLN(tempDeltaValue);
    daikinAC.setTemp(daikinAC.getTemp() + tempDeltaValue);
  }

  uint8_t tempValue = status.get<int>("temp");

  if (tempValue) {
    needToSendIR = true;
    PRINT("AC: Temp set to: ");

    if (tempValue < DAIKIN_MIN_TEMP) {
      tempValue = DAIKIN_MIN_TEMP;
      PRINT("(MIN TEMP) ");
    }
    else if (tempValue > DAIKIN_MAX_TEMP) {
      tempValue = DAIKIN_MAX_TEMP;
      PRINT("(MAX TEMP) ");
    }
    daikinAC.setTemp(tempValue);
    PRINTLN(tempValue);
  }

  const char *fanValue = status.get<const char *>("fan");

  if (fanValue) {
    needToSendIR = true;
    PRINT("AC: Fan set to: ");
    int fanValueInt;

    if (strcasecmp(fanValue, "auto") == 0) {
      fanValueInt = DAIKIN_FAN_AUTO;
      PRINTLN("AUTO");
    } else if (strcasecmp(fanValue, "max") == 0) {
      fanValueInt = DAIKIN_FAN_MAX;
      PRINTLN("MAX");
    } else if (strcasecmp(fanValue, "min") == 0) {
      fanValueInt = DAIKIN_FAN_MIN;
      PRINTLN("MIN");
    } else {
      fanValueInt = atoi(fanValue);

      if ((fanValueInt < DAIKIN_FAN_MIN) or (fanValueInt > DAIKIN_FAN_MAX)) {
        fanValue = 0;
        PRINT("Invalid value (");
        PRINT(fanValue);
        PRINTLN(")");
      } else {
        PRINTLN(fanValue);
      }
    }

    if (fanValue != 0) {
      daikinAC.setFan(fanValueInt);
    }
  }

  bool swingVerticalValue = status.get<bool>("swingVertical");

  if (swingVerticalValue) {
    needToSendIR = true;
    PRINT("AC: Swing Vertical set to: ");
    daikinAC.setSwingVertical(swingVerticalValue);
    PRINTLN(swingVerticalValue);
  }

  bool swingHorizontalValue = status.get<bool>("swingHorizontal");

  if (swingHorizontalValue) {
    needToSendIR = true;
    PRINT("AC: Swing Horizontal set to: ");
    daikinAC.setSwingHorizontal(swingHorizontalValue);
    PRINTLN(swingHorizontalValue);
  }

  const char *quietValue = status.get<const char *>("quiet");

  if (quietValue) {
    needToSendIR = true;
    PRINT("AC: Quiet set to: ");
    PRINTLN(quietValue);
    daikinAC.setQuiet(strcasecmp(quietValue, "true") == 0);
  }

  const char *powerfulValue = status.get<const char *>("powerful");

  if (powerfulValue) {
    needToSendIR = true;
    PRINT("AC: Powerful set to: ");
    PRINTLN(powerfulValue);
    daikinAC.setPowerful(strcasecmp(powerfulValue, "true") == 0);
  }

  if (needToSendIR == true) {
    // Now send the IR signal.
    daikinAC.send();
  }
  const char *messageId = root.get<const char *>("messageId");
  acPublishStatus(messageId, true, powerOnBool);
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  PRINT("MQTT Message arrived [");
  PRINT(topic);
  PRINTLN("] ");

  String payloadString = String((char *)payload);

  // Do something according the topic
  if (strcmp(topic, MQTT_TOPIC_SET) == 0) {
    acIRSend(payloadString);
  }
  else {
    PRINT("MQTT: Warning: Unknown topic: ");
    PRINTLN(topic);
  }
}

void setup() {
  wifiClient->init();
  mqttClient = new MqttClient(MQTT_SERVER,
                              MQTT_SERVER_PORT,
                              DEVICE_NAME,
                              MQTT_USERNAME,
                              MQTT_PASS,
                              MQTT_TOPIC_SET,
                              MQTT_SERVER_FINGERPRINT,
                              mqttCallback);
  fotaClient->init();

  // AC initial configuration
  daikinAC.begin();
  daikinAC.setTemp(15);
  daikinAC.setFan(DAIKIN_FAN_AUTO);

  // Set the initial Power Status based on the actual status
  setACPowerStatus(isACPowerOn());
}

void loop() {
  RemotePrint::instance()->handle();
  fotaClient->loop();
  mqttClient->loop();
  acPublishStatus();
}
