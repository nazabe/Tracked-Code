#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>

#include "secrets.h"
// secrets.h should define:
// const char* mqtt_broker
// const int   mqtt_port
// const char* mqtt_username
// const char* mqtt_password
// const char* topic

// ── Serial ports ──────────────────────────────────────────
#define SerialAT  Serial2          // GPIO16=RX2, GPIO17=TX2
#define SerialMon Serial

// ── MQTT topics ───────────────────────────────────────────
const char* topicGPS    = "tracker/gps";
const char* topicStatus = "tracker/status";

// ── TinyGSM ───────────────────────────────────────────────
TinyGsm modem(SerialAT);

// ── MQTT over WiFi ────────────────────────────────────────
WiFiClient   espClient;
PubSubClient mqtt(espClient);

uint32_t lastPublish       = 0;
uint32_t lastReconnect     = 0;
const uint32_t PUBLISH_INTERVAL   = 5000;   // publish GPS every 5s
const uint32_t RECONNECT_INTERVAL = 10000;

// ──────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    SerialMon.print("MQTT message [");
    SerialMon.print(topic);
    SerialMon.print("]: ");
    for (unsigned int i = 0; i < length; i++) SerialMon.print((char)payload[i]);
    SerialMon.println();
}

bool mqttReconnect() {
    String clientId = "esp32-tracker-" + String(WiFi.macAddress());
    SerialMon.printf("Connecting to MQTT broker as %s... ", clientId.c_str());

    if (mqtt.connect(clientId.c_str(), mqtt_username, mqtt_password)) {
        SerialMon.println("connected!");
        mqtt.publish(topicStatus, "Tracker online");
        mqtt.subscribe(topicStatus);  // add more subscriptions if needed
    } else {
        SerialMon.printf("failed, rc=%d\n", mqtt.state());
    }
    return mqtt.connected();
}

// Returns a JSON string with GPS data, or empty string on failure
String getGPSData() {
    float lat      = 0, lon  = 0, speed = 0, alt = 0;
    int   vsat     = 0, usat = 0;
    float accuracy = 0;
    int   year     = 0, month = 0, day  = 0;
    int   hour     = 0, min   = 0, sec  = 0;

    if (!modem.getGPS(&lat, &lon, &speed, &alt, &vsat, &usat, &accuracy,
                      &year, &month, &day, &hour, &min, &sec)) {
        return "";   // no fix yet
    }

    char buf[200];
    snprintf(buf, sizeof(buf),
        "{\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f,"
        "\"speed\":%.1f,\"accuracy\":%.1f,"
        "\"datetime\":\"%04d-%02d-%02dT%02d:%02d:%02dZ\"}",
        lat, lon, alt, speed, accuracy,
        year, month, day, hour, min, sec);
    return String(buf);
}

// ──────────────────────────────────────────────────────────
void setup() {
    SerialMon.begin(115200);
    SerialAT.begin(115200, SERIAL_8N1, 16, 17);  // RX=16, TX=17
    delay(3000);

    // ── Init modem ────────────────────────────────────────
    SerialMon.println("Initializing SIM7600 modem...");
    modem.restart();
    SerialMon.print("Modem: ");
    SerialMon.println(modem.getModemInfo());

    // ── Enable GPS ────────────────────────────────────────
    SerialMon.println("Enabling GPS...");
    modem.enableGPS();

    // ── Connect WiFi via WiFiManager ──────────────────────
    WiFiManager wm;
    SerialMon.println("Starting WiFiManager...");
    if (!wm.autoConnect("Tracker-AP", "12345678")) {
        SerialMon.println("WiFi connection failed! Restarting...");
        ESP.restart();
    }
    SerialMon.println("WiFi connected: " + WiFi.localIP().toString());

    // ── Setup MQTT ────────────────────────────────────────
    mqtt.setServer(mqtt_broker, mqtt_port);
    mqtt.setCallback(mqttCallback);
}

// ──────────────────────────────────────────────────────────
void loop() {
    // Keep MQTT alive
    if (!mqtt.connected()) {
        uint32_t now = millis();
        if (now - lastReconnect > RECONNECT_INTERVAL) {
            lastReconnect = now;
            mqttReconnect();
        }
    } else {
        mqtt.loop();

        // Publish GPS every PUBLISH_INTERVAL ms
        if (millis() - lastPublish > PUBLISH_INTERVAL) {
            lastPublish = millis();

            String gpsJson = getGPSData();
            if (gpsJson.length() > 0) {
                SerialMon.println("Publishing GPS: " + gpsJson);
                mqtt.publish(topicGPS, gpsJson.c_str());
            } else {
                SerialMon.println("Waiting for GPS fix...");
                mqtt.publish(topicStatus, "Waiting for GPS fix");
            }
        }
    }
}