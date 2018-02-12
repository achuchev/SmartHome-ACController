#ifndef SETTING_H
#define SETTING_H

#define DEVICE_NAME "LivingRoomAC"

#define MQTT_TOPIC_GET "get/apartment/livingRoom/ac"
#define MQTT_TOPIC_SET "set/apartment/livingRoom/ac"
#define MQTT_TOPIC_LOCK_GET "get/home/lock"

#define AUTOMATED_STATE_AREA_NAME "Apartment"
#define AUTOMATED_STATE_TEMP_ARMED (byte)17
#define AUTOMATED_STATE_TEMP_DISARMED (byte)22

#define PIN_POWER_ACTUAL A0 // Photoresistor PIN
#define PIN_IR_SEND D2      // IR LED PIN

#endif // ifndef SETTING_H
