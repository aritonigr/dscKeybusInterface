/*
 *  DSC Status with MQTT (Arduino, esp8266)
 *
 *  Processes the security system status and allows for control using Apple HomeKit, including the iOS Home app and
 *  Siri.  This uses MQTT to interface with Homebridge and the homebridge-mqttthing plugin for HomeKit integration
 *  and demonstrates using the armed and alarm states for the HomeKit securitySystem object, zone states
 *  for the contactSensor objects, and fire alarm states for the smokeSensor object.
 *
 *  Homebridge: https://github.com/nfarina/homebridge
 *  homebridge-mqttthing: https://github.com/arachnetech/homebridge-mqttthing
 *  Mosquitto MQTT broker: https://mosquitto.org
 *
 *  In this example, the commands to set the alarm state are setup in Homebridge as:
 *    Stay arm: "S"
 *    Away arm: "A"
 *    Night arm (arm without an entry delay): "N"
 *    Disarm: "D"
 *
 *  The interface listens for commands in the configured mqttSubscribeTopic, and publishes partition status in a
 *  separate topic per partition with the configured mqttPartitionTopic appended with the partition number:
 *    Stay arm: "SA"
 *    Away arm: "AA"
 *    Night arm: "NA"
 *    Disarm: "D"
 *    Alarm tripped: "T"
 *
 *  Zone states are published in a separate topic per zone with the configured mqttZoneTopic appended with the zone
 *  number.  The zone state is published as an integer:
 *    "0": closed
 *    "1": open
 *
 *  Fire states are published in a separate topic per partition with the configured mqttFireTopic appended with the
 *  partition number.  The fire state is published as an integer:
 *    "0": fire alarm restored
 *    "1": fire alarm tripped
 *
 *  Example Homebridge config.json "accessories" configuration:

        {
            "accessory": "mqttthing",
            "type": "securitySystem",
            "name": "Security System",
            "url": "http://127.0.0.1:1883",
            "topics":
            {
                "getCurrentState":    "dsc/Get/Partition1",
                "setTargetState":     "dsc/Set"
            },
            "targetStateValues": ["S", "A", "N", "D"]
        },
        {
            "accessory": "mqttthing",
            "type": "contactSensor",
            "name": "Zone 1",
            "url": "http://127.0.0.1:1883",
            "topics":
            {
                "getContactSensorState": "dsc/Get/Zone1"
            },
            "integerValue": "true"
        },
        {
            "accessory": "mqttthing",
            "type": "contactSensor",
            "name": "Zone 8",
            "url": "http://127.0.0.1:1883",
            "topics":
            {
                "getContactSensorState": "dsc/Get/Zone8"
            },
            "integerValue": "true"
        },
        {
            "accessory": "mqttthing",
            "type": "smokeSensor",
            "name": "Smoke Alarm",
            "url": "http://127.0.0.1:1883",
            "topics":
            {
                "getSmokeDetected": "dsc/Get/Fire1"
            },
            "integerValue": "true"
        }

 *  Wiring:
 *      DSC Aux(-) --- Arduino/esp8266 ground
 *
 *                                         +--- dscClockPin (Arduino Uno: 2,3 / esp8266: D1,D2,D8)
 *      DSC Yellow --- 15k ohm resistor ---|
 *                                         +--- 10k ohm resistor --- Ground
 *
 *                                         +--- dscReadPin (Arduino Uno: 2-12 / esp8266: D1,D2,D8)
 *      DSC Green ---- 15k ohm resistor ---|
 *                                         +--- 10k ohm resistor --- Ground
 *
 *  Virtual keypad (optional):
 *      DSC Green ---- NPN collector --\
 *                                      |-- NPN base --- 1k ohm resistor --- dscWritePin (Arduino Uno: 2-12 / esp8266: D1,D2,D8)
 *            Ground --- NPN emitter --/
 *
 *  Power (when disconnected from USB):
 *      DSC Aux(+) ---+--- Arduino Vin pin
 *                    |
 *                    +--- 5v voltage regulator --- esp8266 development board 5v pin (NodeMCU, Wemos)
 *                    |
 *                    +--- 3.3v voltage regulator --- esp8266 bare module VCC pin (ESP-12, etc)
 *
 *  Virtual keypad uses an NPN transistor to pull the data line low - most small signal NPN transistors should
 *  be suitable, for example:
 *   -- 2N3904
 *   -- BC547, BC548, BC549
 *
 *  Issues and (especially) pull requests are welcome:
 *  https://github.com/taligentx/dscKeybusInterface
 *
 *  This example code is in the public domain.
 */

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <dscKeybusInterface.h>

const char* wifiSSID = "";
const char* wifiPassword = "";
const char* accessCode = "";  // An access code is required to disarm/night arm and may be required to arm based on panel configuration.
const char* mqttServer = "";

const char* mqttClientName = "dscKeybusInterface";
const char* mqttPartitionTopic = "dsc/Get/Partition";  // Sends armed and alarm status per partition: dsc/Get/Partition1 ... dsc/Get/Partition8
const char* mqttZoneTopic = "dsc/Get/Zone";            // Sends zone status per zone: dsc/Get/Zone1 ... dsc/Get/Zone64
const char* mqttFireTopic = "dsc/Get/Fire";            // Sends fire status per partition: dsc/Get/Fire1 ... dsc/Get/Fire8
const char* mqttSubscribeTopic = "dsc/Set";            // Receives messages to write to the panel
unsigned long mqttPreviousTime;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Configures the Keybus interface with the specified pins - dscWritePin is
// optional, leaving it out disables the virtual keypad
#define dscClockPin D1   // GPIO5
#define dscReadPin D2    // GPIO4
#define dscWritePin D8   // GPIO15
dscKeybusInterface dsc(dscClockPin, dscReadPin, dscWritePin);


void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println();

  WiFi.begin(wifiSSID, wifiPassword);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.print("WiFi connected: ");
  Serial.println(WiFi.localIP());

  mqtt.setServer(mqttServer, 1883);
  mqtt.setCallback(mqttCallback);
  if (mqttConnect()) mqttPreviousTime = millis();
  else mqttPreviousTime = 0;

  // Starts the Keybus interface and optionally specifies how to print data.
  // begin() sets Serial by default and can accept a different stream: begin(Serial1), etc.
  dsc.begin();

  Serial.println(F("DSC Keybus Interface is online."));
}


void loop() {
  mqttHandle();

  if (dsc.handlePanel() && dsc.statusChanged) {  // Processes data only when a valid Keybus command has been read
    dsc.statusChanged = false;                   // Reset the status tracking flag

    // If the Keybus data buffer is exceeded, the sketch is too busy to process all Keybus commands.  Call
    // handlePanel() more often, or increase dscBufferSize in the library: src/dscKeybusInterface.h
    if (dsc.bufferOverflow) Serial.println(F("Keybus buffer overflow"));
    dsc.bufferOverflow = false;

    // Sends the access code when needed by the panel for arming
    if (dsc.accessCodePrompt && dsc.writeReady) {
      dsc.accessCodePrompt = false;
      dsc.write(accessCode);
    }

    // Publishes status per partition
    for (byte partitionIndex = 0; partitionIndex < dscPartitions; partitionIndex++) {

      if (dsc.partitionsArmedChanged[partitionIndex]) {
        dsc.partitionsArmedChanged[partitionIndex] = false;  // Resets the partition armed status flag

        // Appends the mqttPartitionTopic with the partition number
        char partitionPublishTopic[strlen(mqttPartitionTopic) + 1];
        char partition[2];
        strcpy(partitionPublishTopic, mqttPartitionTopic);
        itoa(partitionIndex + 1, partition, 10);
        strcat(partitionPublishTopic, partition);

        if (dsc.partitionsArmed[partitionIndex]) {
          if (dsc.partitionsArmedAway[partitionIndex] && dsc.partitionsNoEntryDelay[partitionIndex]) mqtt.publish(partitionPublishTopic, "NA", true);       // Night armed
          else if (dsc.partitionsArmedAway[partitionIndex]) mqtt.publish(partitionPublishTopic, "AA", true);                                                // Away armed
          else if (dsc.partitionsArmedStay[partitionIndex] && dsc.partitionsNoEntryDelay[partitionIndex]) mqtt.publish(partitionPublishTopic, "NA", true);  // Night armed
          else if (dsc.partitionsArmedStay[partitionIndex]) mqtt.publish(partitionPublishTopic, "SA", true);                                                // Stay armed
        }
        else mqtt.publish(partitionPublishTopic, "D", true);  // Disarmed
      }

      if (dsc.partitionsAlarmChanged[partitionIndex]) {
        dsc.partitionsAlarmChanged[partitionIndex] = false;  // Resets the partition alarm status flag
        if (dsc.partitionsAlarm[partitionIndex]) {

          // Appends the mqttPartitionTopic with the partition number
          char partitionPublishTopic[strlen(mqttPartitionTopic) + 1];
          char partition[2];
          strcpy(partitionPublishTopic, mqttPartitionTopic);
          itoa(partitionIndex + 1, partition, 10);
          strcat(partitionPublishTopic, partition);

          mqtt.publish(partitionPublishTopic, "T", true);  // Alarm tripped
        }
      }

      if (dsc.partitionsFireChanged[partitionIndex]) {
        dsc.partitionsFireChanged[partitionIndex] = false;  // Resets the fire status flag

        // Appends the mqttFireTopic with the partition number
        char firePublishTopic[strlen(mqttFireTopic) + 1];
        char partition[2];
        strcpy(firePublishTopic, mqttFireTopic);
        itoa(partitionIndex + 1, partition, 10);
        strcat(firePublishTopic, partition);

        if (dsc.partitionsFire[partitionIndex]) mqtt.publish(firePublishTopic, "1");  // Fire alarm tripped
        else mqtt.publish(firePublishTopic, "0");                                     // Fire alarm restored
      }
    }

    // Publishes zones 1-64 status in a separate topic per zone
    // Zone status is stored in the openZones[] and openZonesChanged[] arrays using 1 bit per zone, up to 64 zones:
    //   openZones[0] and openZonesChanged[0]: Bit 0 = Zone 1 ... Bit 7 = Zone 8
    //   openZones[1] and openZonesChanged[1]: Bit 0 = Zone 9 ... Bit 7 = Zone 16
    //   ...
    //   openZones[7] and openZonesChanged[7]: Bit 0 = Zone 57 ... Bit 7 = Zone 64
    if (dsc.openZonesStatusChanged) {
      dsc.openZonesStatusChanged = false;                           // Resets the open zones status flag
      for (byte zoneGroup = 0; zoneGroup < 8; zoneGroup++) {
        for (byte zoneBit = 0; zoneBit < 8; zoneBit++) {
          if (bitRead(dsc.openZonesChanged[zoneGroup], zoneBit)) {  // Checks an individual open zone status flag
            bitWrite(dsc.openZonesChanged[zoneGroup], zoneBit, 0);  // Resets the individual open zone status flag

            // Appends the mqttZoneTopic with the zone number
            char zonePublishTopic[strlen(mqttZoneTopic) + 2];
            char zone[3];
            strcpy(zonePublishTopic, mqttZoneTopic);
            itoa(zoneBit + 1 + (zoneGroup * 8), zone, 10);
            strcat(zonePublishTopic, zone);

            if (bitRead(dsc.openZones[zoneGroup], zoneBit)) {
              mqtt.publish(zonePublishTopic, "1", true);            // Zone open
            }
            else mqtt.publish(zonePublishTopic, "0", true);         // Zone closed
          }
        }
      }
    }

    mqtt.subscribe(mqttSubscribeTopic);
  }
}


// Handles messages received in the mqttSubscribeTopic
void mqttCallback(char* topic, byte* payload, unsigned int length) {

  // Handles unused parameters
  (void)topic;
  (void)length;

  // homebridge-mqttthing STAY_ARM
  if (payload[0] == 'S' && !dsc.partitionArmed && !dsc.exitDelay) {
    while (!dsc.writeReady) dsc.handlePanel();  // Continues processing Keybus data until ready to write
    dsc.write('s');  // Keypad stay arm
  }

  // homebridge-mqttthing AWAY_ARM
  else if (payload[0] == 'A' && !dsc.partitionArmed && !dsc.exitDelay) {
    while (!dsc.writeReady) dsc.handlePanel();
    dsc.write('w');  // Keypad away arm
  }

  // homebridge-mqttthing NIGHT_ARM
  else if (payload[0] == 'N' && !dsc.partitionArmed && !dsc.exitDelay) {
    while (!dsc.writeReady) dsc.handlePanel();
    dsc.write('n');  // Keypad arm with no entry delay
  }

  // homebridge-mqttthing DISARM
  else if (payload[0] == 'D' && (dsc.partitionArmed || dsc.exitDelay)) {
    while (!dsc.writeReady) dsc.handlePanel();
    dsc.write(accessCode);
  }
}


void mqttHandle() {
  if (!mqtt.connected()) {
    unsigned long mqttCurrentTime = millis();
    if (mqttCurrentTime - mqttPreviousTime > 5000) {
      mqttPreviousTime = mqttCurrentTime;
      if (mqttConnect()) {
        Serial.println(F("MQTT disconnected, successfully reconnected."));
        mqttPreviousTime = 0;
      }
      else Serial.println(F("MQTT disconnected, failed to reconnect."));
    }
  }
  else mqtt.loop();
}


bool mqttConnect() {
  if (mqtt.connect(mqttClientName)) {
    Serial.print(F("MQTT connected: "));
    Serial.println(mqttServer);
    mqtt.subscribe(mqttSubscribeTopic);
  }
  else {
    Serial.print(F("MQTT connection failed: "));
    Serial.println(mqttServer);
  }
  return mqtt.connected();
}

