#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>
#include "secrets.h"

#define SerialAT Serial2
#define SerialMon Serial

// ── DS18B20 ───────────────────────────────────────────────
OneWire oneWire(4);
DallasTemperature tempSensor(&oneWire);

const char *topicGPS    = "tracker/gps";
const char *topicStatus = "tracker/status";

TinyGsm modem(SerialAT);
WiFiClient espClient;
PubSubClient mqtt(espClient);

uint32_t lastPublish  = 0;
uint32_t lastReconnect = 0;

// ── AT helpers ────────────────────────────────────────────
String sendAT(const char *cmd, uint32_t timeout = 2000)
{
    SerialAT.println(cmd);
    String resp;
    uint32_t t = millis();
    while (millis() - t < timeout)
        while (SerialAT.available())
            resp += (char)SerialAT.read();
    return resp;
}

bool gpsSetup()
{
    SerialMon.println(sendAT("AT+CGNSSPWR=1"));
    sendAT("AT+CGNSSCMD=0,\"PAIR_COMMON_SET_GNSS_MODE,1,1,0,0,0,0\"");
    sendAT("AT+CGNSSTST=1");

    String status = sendAT("AT+CGNSSPWR?");
    SerialMon.println(status);
    if (status.indexOf("+CGNSSPWR: 1") < 0)
    {
        SerialMon.println("{\"event\":\"gps_power\",\"status\":\"error\",\"msg\":\"GPS failed to power on\"}");
        return false;
    }
    SerialMon.println("{\"event\":\"gps_power\",\"status\":\"ok\"}");
    return true;
}

// ── GPS + Temperatura → JSON ──────────────────────────────
String getGPSData()
{
    String r = sendAT("AT+CGNSSINFO");
    SerialMon.println(r);

    int start = r.indexOf("+CGNSSINFO:");
    if (start < 0) return "";

    String data = r.substring(start + 12);
    data.trim();

    String f[16];
    int idx = 0;
    for (int i = 0; i < 16 && idx >= 0; i++)
    {
        int comma = data.indexOf(',');
        f[i] = (comma < 0) ? data : data.substring(0, comma);
        data = (comma < 0) ? "" : data.substring(comma + 1);
        if (f[i].length() == 0 && i < 5) return ""; // sin fix
    }

    // Leer temperatura
    tempSensor.requestTemperatures();
    float celsius = tempSensor.getTempCByIndex(0);

    char tempStr[8];
    if (celsius == DEVICE_DISCONNECTED_C)
        snprintf(tempStr, sizeof(tempStr), "null");
    else
        snprintf(tempStr, sizeof(tempStr), "%.2f", celsius);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"lat\":\"%s%s\",\"lon\":\"%s%s\",\"alt\":\"%s\","
             "\"speed\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
             "\"temp_c\":%s}",
             f[4].c_str(), f[5].c_str(),
             f[6].c_str(), f[7].c_str(),
             f[10].c_str(), f[11].c_str(),
             f[8].c_str(),  f[9].c_str(),
             tempStr);

    return String(buf);
}

// ── MQTT ──────────────────────────────────────────────────
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    String msg = "";
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    SerialMon.printf("{\"event\":\"mqtt_rx\",\"topic\":\"%s\",\"payload\":%s}\n", topic, msg.c_str());
}

bool mqttReconnect()
{
    String id = "esp32-" + String(WiFi.macAddress());
    if (mqtt.connect(id.c_str(), mqtt_username, mqtt_password))
        mqtt.publish(topicStatus, "{\"event\":\"tracker_status\",\"status\":\"online\"}");
    return mqtt.connected();
}

// ── Setup ─────────────────────────────────────────────────
void setup()
{
    SerialMon.begin(115200);
    delay(3000);

    // Inicializar sensor de temperatura
    tempSensor.begin();
    SerialMon.printf("{\"event\":\"ds18b20_init\",\"sensors_found\":%d}\n", tempSensor.getDeviceCount());

    // Detectar baudrate del modem
    uint32_t bauds[] = {115200, 9600, 57600, 38400, 19200};
    bool found = false;
    for (uint32_t baud : bauds)
    {
        SerialAT.begin(baud, SERIAL_8N1, 16, 17);
        delay(500);
        for (int attempt = 0; attempt < 5; attempt++)
        {
            SerialAT.println("AT");
            delay(500);
            String r = "";
            while (SerialAT.available()) r += (char)SerialAT.read();
            r.trim();
            SerialMon.printf("{\"event\":\"modem_probe\",\"baud\":%d,\"attempt\":%d,\"response\":\"%s\"}\n",
                             baud, attempt + 1, r.c_str());
            if (r.indexOf("OK") >= 0)
            {
                SerialMon.printf("{\"event\":\"modem_ready\",\"baud\":%d}\n", baud);
                found = true;
                break;
            }
        }
        if (found) break;
    }

    if (!found)
    {
        SerialMon.println("{\"event\":\"modem_error\",\"msg\":\"modem not found\"}");
        while (true) delay(1000);
    }

    delay(3000);
    SerialMon.println("{\"event\":\"modem_init\",\"status\":\"starting\"}");
    modem.restart();
    SerialMon.printf("{\"event\":\"modem_info\",\"info\":\"%s\"}\n", modem.getModemInfo().c_str());

    if (!gpsSetup()) while (true) delay(1000);

    SerialMon.println(sendAT("AT+CGNSSPWR?"));
    SerialMon.println(sendAT("AT+CGNSSINFO"));
    SerialMon.println(sendAT("AT+CGNSSPORTSWITCH=0,1"));
    delay(2000);

    uint32_t t = millis();
    while (millis() - t < 3000)
        while (SerialAT.available())
            SerialMon.write(SerialAT.read());

    WiFiManager wm;
    if (!wm.autoConnect("Tracker-AP", "12345678")) ESP.restart();
    SerialMon.printf("{\"event\":\"wifi_connected\",\"ip\":\"%s\"}\n", WiFi.localIP().toString().c_str());

    mqtt.setServer(mqtt_broker, mqtt_port);
    mqtt.setCallback(mqttCallback);
}

// ── Loop ──────────────────────────────────────────────────
void loop()
{
    if (!mqtt.connected())
    {
        if (millis() - lastReconnect > 10000)
        {
            lastReconnect = millis();
            mqttReconnect();
        }
        return;
    }
    mqtt.loop();

    if (millis() - lastPublish > 2000)
    {
        lastPublish = millis();

        String gps = getGPSData();
        if (gps.length() > 0)
        {
            SerialMon.printf("{\"event\":\"mqtt_tx\",\"topic\":\"%s\",\"payload\":%s}\n", topicGPS, gps.c_str());
            mqtt.publish(topicGPS, gps.c_str());
        }
        else
        {
            String info = sendAT("AT+CGNSSINFO");
            String sats = sendAT("AT+CGNSSNMEA?");
            SerialMon.printf("{\"event\":\"gps_no_fix\",\"info\":\"%s\",\"nmea\":\"%s\"}\n",
                             info.c_str(), sats.c_str());
            mqtt.publish(topicStatus, "{\"event\":\"tracker_status\",\"status\":\"waiting_gps_fix\"}");
        }
    }
}