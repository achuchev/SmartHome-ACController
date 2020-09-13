#include <Arduino.h>
#include <IRsend.h>
#include <ArduinoJson.h>
#include <IRremoteESP8266.h>
#include <ir_Daikin.h>
#include <MqttClient.h>
#include <FotaClient.h>
#include <ESPWifiClient.h>
#include <RemotePrint.h>
#include "settings.h"

IRDaikinESP daikinAC(PIN_IR_SEND);
MqttClient *mqttClient    = NULL;
String mqttTopics[2]      = { String(MQTT_TOPIC_SET), String(MQTT_TOPIC_LOCK_GET) };
FotaClient *fotaClient    = new FotaClient(DEVICE_NAME);
ESPWifiClient *wifiClient = new ESPWifiClient(WIFI_SSID, WIFI_PASS);

long lastStatusMsgSentAt  = 0;
bool lastApartmentIsArmed = false;

bool isACPowerOn() {
  int acPowerStatusValue = analogRead(PIN_POWER_ACTUAL);

  PRINT_D("AC: Power On value is ");
  PRINT_D(acPowerStatusValue);
  PRINT_D(". The limit is ");
  PRINT_D(POWER_ON_VALUE);
  PRINT_D(". AC is ");

  if (acPowerStatusValue >= POWER_ON_VALUE) {
    PRINTLN_D("ON.");
    return true;
  }
  PRINTLN_D("OFF.");
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
    DynamicJsonDocument root(bufferSize);
    JsonObject status = root.createNestedObject("status");

    if (messageId != NULL) {
      root["messageId"] = messageId;
      status["powerOn"] = forcedPowerOn;
    } else {
      status["powerOn"] = isACPowerOn();
    }

    status["temp"] = daikinAC.getTemp();

    switch (daikinAC.getMode()) {
      case kDaikinAuto: {
        status["mode"] = "auto";
        break;
      }

      case kDaikinCool: {
        status["mode"] = "cool";
        break;
      }

      case kDaikinHeat: {
        status["mode"] = "heat";
        break;
      }

      case kDaikinFan: {
        status["mode"] = "fan";
        break;
      }

      case kDaikinDry: {
        status["mode"] = "dry";
        break;
      }

      default: {
        PRINT("Invalid Mode: ");
        PRINTLN(daikinAC.getMode());
      }
    }

    switch (daikinAC.getFan()) {
      case kDaikinFanAuto: {
        status["fan"] = "auto";
        break;
      }

      case kDaikinFanMin: {
        status["fan"] = "min";
        break;
      }

      case kDaikinFanMax: {
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
    serializeJson(root, outString);

    // publish the message
    mqttClient->publish(MQTT_TOPIC_GET, outString);
  }
}

void acSetStatus(String payload) {
  PRINTLN("AC: Setting New Status.");

  // parse the JSON
  const size_t bufferSize = JSON_OBJECT_SIZE(1) + JSON_OBJECT_SIZE(8) + 130;
  DynamicJsonDocument  jsonDoc(bufferSize);
  DeserializationError error = deserializeJson(jsonDoc, payload);

  if (error) {
    PRINT_E("Failed to deserialize the received payload. Error: ");
    PRINTLN_E(error.c_str());
    PRINTLN_E("The payload is: ");
    PRINTLN_E(payload)
    return;
  }
  PRINTLN_D(payload);

  JsonObject root   = jsonDoc.as<JsonObject>();
  JsonObject status = root["status"];

  bool needToSendIR = false;
  bool powerOnBool  = isACPowerOn();

  JsonVariant powerOnJV = status["powerOn"];

  if (!powerOnJV.isNull()) {
    bool powerOn = powerOnJV.as<bool>();

    needToSendIR = true;
    powerOnBool  = setACPowerStatus(powerOn);
  }

  JsonVariant modeValueJV = status["mode"];

  if (modeValueJV) {
    uint8_t modeNew = kDaikinAuto;
    String  modeNewStr;
    const char *modeValue = modeValueJV.as<const char *>();

    if (strcasecmp(modeValue, "auto") == 0) {
      modeNew    = kDaikinAuto;
      modeNewStr = "AUTO";
    } else if (strcasecmp(modeValue, "cool") == 0) {
      modeNew    = kDaikinCool;
      modeNewStr = "COOL";
    } else if (strcasecmp(modeValue, "heat") == 0) {
      modeNew    = kDaikinHeat;
      modeNewStr = "HEAT";
    } else if (strcasecmp(modeValue, "fan") == 0) {
      modeNew    = kDaikinFan;
      modeNewStr = "FAN";
    } else if (strcasecmp(modeValue, "dry") == 0) {
      modeNew    = kDaikinDry;
      modeNewStr = "DRY";
    } else {
      modeNewStr  = "Invalid value (";
      modeNewStr += modeValue;
      modeNewStr += ")";
    }

    if (daikinAC.getMode() != modeNew) {
      needToSendIR = true;
      PRINT("AC: Mode set to: ");
      PRINTLN(modeNewStr);
      daikinAC.setMode(modeNew);
    } else {
      PRINT("AC: Mode is already set to ");
      PRINTLN(modeValue);
    }
  }

  JsonVariant tempDeltaValueJV = status["tempDelta"];

  if (tempDeltaValueJV) {
    int tempDeltaValue = tempDeltaValueJV.as<int>();
    needToSendIR = true;
    PRINT("AC: Temp Delta is: ");
    PRINTLN(tempDeltaValue);
    daikinAC.setTemp(daikinAC.getTemp() + tempDeltaValue);
  }

  JsonVariant tempValueJV = status["temp"];

  if (tempValueJV) {
    int tempValue = tempValueJV.as<int>();

    if (daikinAC.getTemp() == tempValue) {
      PRINT("AC: Temp is already set to ");
      PRINTLN(tempValue);
    } else {
      needToSendIR = true;
      PRINT("AC: Temp set to: ");

      if (tempValue < kDaikinMinTemp) {
        tempValue = kDaikinMinTemp;
        PRINT("(MIN TEMP) ");
      }
      else if (tempValue > kDaikinMaxTemp) {
        tempValue = kDaikinMaxTemp;
        PRINT("(MAX TEMP) ");
      }
      daikinAC.setTemp(tempValue);
      PRINTLN(tempValue);
    }
  }
  JsonVariant fanValueJV = status["fan"];

  if (fanValueJV) {
    const char *fanValue = fanValueJV.as<const char *>();
    needToSendIR = true;

    PRINT("AC: Fan set to: ");
    int fanValueInt;

    if (strcasecmp(fanValue, "auto") == 0) {
      fanValueInt = kDaikinFanAuto;
      PRINTLN("AUTO");
    } else if (strcasecmp(fanValue, "max") == 0) {
      fanValueInt = kDaikinFanMax;
      PRINTLN("MAX");
    } else if (strcasecmp(fanValue, "min") == 0) {
      fanValueInt = kDaikinFanMin;
      PRINTLN("MIN");
    } else {
      fanValueInt = atoi(fanValue);

      if ((fanValueInt < kDaikinFanMin) or (fanValueInt > kDaikinFanMax)) {
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

  JsonVariant swingVerticalValueJV = status["swingVertical"];

  if (swingVerticalValueJV) {
    bool swingVerticalValue = swingVerticalValueJV.as<bool>();
    needToSendIR = true;

    PRINT("AC: Swing Vertical set to: ");
    daikinAC.setSwingVertical(swingVerticalValue);
    PRINTLN(swingVerticalValue);
  }

  JsonVariant swingHorizontalValueJV = status["swingHorizontal"];

  if (swingHorizontalValueJV) {
    bool swingHorizontalValue = swingHorizontalValueJV.as<bool>();
    needToSendIR = true;

    PRINT("AC: Swing Horizontal set to: ");
    daikinAC.setSwingHorizontal(swingHorizontalValue);
    PRINTLN(swingHorizontalValue);
  }

  JsonVariant quietValueJV = status["quiet"];

  if (quietValueJV) {
    bool quietValue = quietValueJV.as<bool>();
    needToSendIR = true;

    PRINT("AC: Quiet set to: ");
    PRINTLN(quietValue);
    daikinAC.setQuiet(quietValue);
  }

  JsonVariant powerfulValueJV = status["powerful"];

  if (powerfulValueJV) {
    bool powerfulValue = powerfulValueJV.as<bool>();
    needToSendIR = true;

    PRINT("AC: Powerful set to: ");
    PRINTLN(powerfulValue);
    daikinAC.setPowerful(powerfulValue);
  }

  if (needToSendIR == true) {
    // Now send the IR signal.
    daikinAC.send();
  }
  JsonVariant messageIdJV = root["messageId"];
  const char *messageId   = messageIdJV.as<const char *>();

  acPublishStatus(messageId, true, powerOnBool);
}

void acSetAutomaticProfile(String payload) {
  if (!AUTOMATED_PROFILE_ENABLED) {
    PRINTLN("AC: Setting automatic profile is disabled.");
    return;
  }
  PRINTLN("AC: Setting automatic profile.");

  if (!isACPowerOn()) {
    PRINTLN("AC: The Air Conditioner is OFF, skipping the automated status change.");
    return;
  }

  // parse the JSON
  const size_t bufferSize = JSON_OBJECT_SIZE(1) + JSON_OBJECT_SIZE(8) + 130;
  DynamicJsonDocument  jsonDoc(bufferSize);
  DeserializationError error = deserializeJson(jsonDoc, payload);

  if (error) {
    PRINT_E("Failed to deserialize the received payload. Error: ");
    PRINTLN_E(error.c_str());
    PRINTLN_E("The payload is: ");
    PRINTLN_E(payload)
    return;
  }
  JsonObject  root          = jsonDoc.as<JsonObject>();
  JsonObject  status        = root["status"];
  JsonVariant areasStatusJV = status["areasStatus"];

  if (!areasStatusJV) {
    PRINTLN_E("AC: JSON with \"areasStatus\" key not received.");
    PRINTLN_E(payload);
    return;
  }
  JsonArray areasStatus = areasStatusJV.as<JsonArray>();

  for (uint8_t idx = 0; idx < areasStatus.size(); ++idx) {
    JsonVariant areaJV = areasStatus[idx];

    if (!areaJV) {
      PRINTLN_E("AC: JSON with \"area\" key not received.");
      PRINTLN_E(payload);
      return;
    }
    JsonObject area = areaJV.as<JsonObject>();

    if (strcmp(area["name"], AUTOMATED_STATE_AREA_NAME) != 0) {
      // Not matched
      continue;
    }

    JsonVariant apartmentIsArmedJV = area["isArmed"];

    if (!apartmentIsArmedJV) {
      PRINTLN_E("AC: JSON with \"isArmed\" key not received.");
      PRINTLN_E(payload);
      return;
    }
    bool apartmentIsArmed = apartmentIsArmedJV.as<bool>();

    if (lastApartmentIsArmed == apartmentIsArmed) {
      // Apartment state not changed
      PRINTLN("AC: Apartment status is not changed. No need to set automated state.");
      return;
    }
    lastApartmentIsArmed = apartmentIsArmed;
    byte deservedTemp = 0;
    byte deservedMode = kDaikinAuto;

    if (apartmentIsArmed == true) {
      // ARMED profile
      PRINT("AC AUTO: Set 'ARMED' profile.");

      if (daikinAC.getMode() == kDaikinHeat) {
        if (AUTOMATED_STATE_HEAT_ARMED_POWERON == false)
        {
          PRINT(" Turning off.");
          daikinAC.off();
        }
        else {
          daikinAC.on();
          deservedTemp = AUTOMATED_STATE_HEAT_ARMED_TEMP;
          deservedMode = AUTOMATED_STATE_HEAT_ARMED_MODE;
        }
      } else if (daikinAC.getMode() == kDaikinCool) {
        if (AUTOMATED_STATE_COOL_ARMED_POWERON == false) {
          PRINT(" Turning off.");
          daikinAC.off();
        }
        else {
          daikinAC.on();
          deservedTemp = AUTOMATED_STATE_COOL_ARMED_TEMP;
          deservedMode = AUTOMATED_STATE_COOL_ARMED_MODE;
        }
      }
    } else {
      // DISARMED profile
      if (((daikinAC.getMode() == kDaikinHeat) && ((daikinAC.getMode() != AUTOMATED_STATE_HEAT_ARMED_MODE) ||
                                                   (daikinAC.getTemp() != AUTOMATED_STATE_HEAT_ARMED_TEMP)))
          ||
          ((daikinAC.getMode() == kDaikinCool) && ((daikinAC.getMode() != AUTOMATED_STATE_COOL_ARMED_MODE) ||
                                                   (daikinAC.getTemp() != AUTOMATED_STATE_COOL_ARMED_TEMP))))
      {
        PRINTLN("AC: No need to set automatic status, as the mode or temperature was already changed.");
        return;
      }
      PRINT("AC AUTO: Set 'DISARMED' profile.");

      if (daikinAC.getMode() == kDaikinHeat) {
        if (AUTOMATED_STATE_HEAT_DISARMED_POWERON == false)
        {
          PRINT(" Turning off.");
          daikinAC.off();
        }
        else {
          daikinAC.on();
          deservedTemp = AUTOMATED_STATE_HEAT_DISARMED_TEMP;
          deservedMode = AUTOMATED_STATE_HEAT_DISARMED_MODE;
        }
      } else if (daikinAC.getMode() == kDaikinCool) {
        if (AUTOMATED_STATE_COOL_DISARMED_POWERON == false)
        {
          PRINT(" Turning off.");
          daikinAC.off();
        }
        else {
          daikinAC.on();
          deservedTemp = AUTOMATED_STATE_COOL_DISARMED_TEMP;
          deservedMode = AUTOMATED_STATE_COOL_DISARMED_MODE;
        }
      }
    }
    PRINT(" Temperature: ");
    PRINT(deservedTemp);
    PRINT(" Mode: ");
    PRINTLN(deservedMode);
    daikinAC.setTemp(deservedTemp);
    daikinAC.setMode(deservedMode);
    daikinAC.send();
    lastStatusMsgSentAt = 0;
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  PRINT("MQTT Message arrived in '");
  PRINT(topic);
  PRINTLN("' topic.");

  // Convert the payload to string
  char spayload[length + 1];
  memcpy(spayload, payload, length);
  spayload[length] = '\0';
  String payloadString = String(spayload);
  PRINT_D("MQTT: Message: ");
  PRINTLN_D(payloadString);

  // Do something according the topic
  if (strcmp(topic, MQTT_TOPIC_SET) == 0) {
    acSetStatus(payloadString);
    return;
  }

  if (strcmp(topic, MQTT_TOPIC_LOCK_GET) == 0) {
    acSetAutomaticProfile(payloadString);
    return;
  }
  PRINT("MQTT: Warning: Unknown topic: ");
  PRINTLN(topic);
}

void setup() {
  wifiClient->init();
  mqttClient = new MqttClient(MQTT_SERVER,
                              MQTT_SERVER_PORT,
                              DEVICE_NAME,
                              MQTT_USERNAME,
                              MQTT_PASS,
                              mqttTopics,
                              2,
                              MQTT_SERVER_FINGERPRINT,
                              mqttCallback);
  fotaClient->init();

  // AC initial configuration
  daikinAC.begin();
  daikinAC.setTemp(15);
  daikinAC.setFan(kDaikinFanAuto);

  // Set the initial Power Status based on the actual status
  setACPowerStatus(isACPowerOn());
}

void loop() {
  wifiClient->reconnectIfNeeded();
  RemotePrint::instance()->handle();
  fotaClient->loop();
  mqttClient->loop();
  acPublishStatus();
}
