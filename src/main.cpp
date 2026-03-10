#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>
#include "secrets.h"

#define SerialAT Serial2
#define SerialMon Serial

const char *topicGPS = "tracker/gps";
const char *topicStatus = "tracker/status";

TinyGsm modem(SerialAT);
WiFiClient espClient;
PubSubClient mqtt(espClient);

uint32_t lastPublish = 0;
uint32_t lastReconnect = 0;

// ── AT helpers ────────────────────────────────────────────
String sendAT(const char *cmd, uint32_t timeout = 2000)
{
    SerialAT.println(cmd);
    String resp;
    uint32_t t = millis();
    while (millis() - t < timeout)
    {
        while (SerialAT.available())
            resp += (char)SerialAT.read();
    }
    return resp;
}

bool gpsSetup()
{
    SerialMon.println(sendAT("AT+CGNSSPWR=1")); // SIM7670G usa CGNSSPWR ✓ (ya funciona)

    // Activar modo GPS + GLONASS para mejor fix
    sendAT("AT+CGNSSCMD=0,\"PAIR_COMMON_SET_GNSS_MODE,1,1,0,0,0,0\"");

    // Configurar salida NMEA para ver señal cruda
    sendAT("AT+CGNSSTST=1"); // activa mensajes NMEA por SerialAT

    String status = sendAT("AT+CGNSSPWR?");
    SerialMon.println(status);
    if (status.indexOf("+CGNSSPWR: 1") < 0)
    {
        SerialMon.println("GPS failed to power on!");
        return false;
    }
    SerialMon.println("GPS powered on OK");
    return true;
}

// Parses AT+CGNSSINFO response into JSON
String getGPSData()
{
    String r = sendAT("AT+CGNSSINFO");
    SerialMon.println(r); // raw debug

    // Expected: +CGNSSINFO: <mode>,<GPS sats>,<GLONASS sats>,<BEIDOU sats>,<lat>,<N/S>,<lon>,<E/W>,<date>,<UTC time>,<alt>,<speed>,<course>,<PDOP>,<HDOP>,<VDOP>
    int start = r.indexOf("+CGNSSINFO:");
    if (start < 0)
        return "";

    String data = r.substring(start + 12);
    data.trim();

    // Split by comma
    String f[16];
    int idx = 0;
    for (int i = 0; i < 16 && idx >= 0; i++)
    {
        int comma = data.indexOf(',');
        f[i] = (comma < 0) ? data : data.substring(0, comma);
        data = (comma < 0) ? "" : data.substring(comma + 1);
        if (f[i].length() == 0 && i < 5)
            return ""; // no fix yet
    }

    char buf[220];
    snprintf(buf, sizeof(buf),
             "{\"lat\":\"%s%s\",\"lon\":\"%s%s\",\"alt\":\"%s\","
             "\"speed\":\"%s\",\"date\":\"%s\",\"time\":\"%s\"}",
             f[4].c_str(), f[5].c_str(),
             f[6].c_str(), f[7].c_str(),
             f[10].c_str(), f[11].c_str(),
             f[8].c_str(), f[9].c_str());
    return String(buf);
}

// ── MQTT ──────────────────────────────────────────────────
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    SerialMon.print("MQTT [");
    SerialMon.print(topic);
    SerialMon.print("]: ");
    for (unsigned int i = 0; i < length; i++)
        SerialMon.print((char)payload[i]);
    SerialMon.println();
}

bool mqttReconnect()
{
    String id = "esp32-" + String(WiFi.macAddress());
    if (mqtt.connect(id.c_str(), mqtt_username, mqtt_password))
    {
        mqtt.publish(topicStatus, "Tracker online");
    }
    return mqtt.connected();
}

// ──────────────────────────────────────────────────────────
void setup()
{
    SerialMon.begin(115200);
    delay(3000);

    // Probar baudrates comunes hasta que el modem responda
    uint32_t bauds[] = {115200, 9600, 57600, 38400, 19200};
    bool found = false;
    for (uint32_t baud : bauds)
    {
        SerialAT.begin(baud, SERIAL_8N1, 16, 17);
        delay(500);

        // Intentar hasta 5 veces por baudrate
        for (int attempt = 0; attempt < 5; attempt++)
        {
            SerialAT.println("AT");
            delay(500);
            String r = "";
            while (SerialAT.available())
                r += (char)SerialAT.read();
            r.trim();
            SerialMon.printf("[%d baud, intento %d]: '%s'\n", baud, attempt + 1, r.c_str());

            if (r.indexOf("OK") >= 0)
            {
                SerialMon.printf("Modem listo a %d baud!\n", baud);
                found = true;
                break;
            }
        }
        if (found)
            break;
    }

    if (!found)
    {
        SerialMon.println("No se encontro el modem. Verifica cableado.");
        while (true)
            delay(1000);
    }

    // SerialAT.begin(115200, SERIAL_8N1, 16, 17);
    delay(3000);

    SerialMon.println("Initializing modem...");
    modem.restart();
    SerialMon.println(modem.getModemInfo());

    if (!gpsSetup())
        while (true)
            delay(1000); // halt on GPS failure

    // En setup(), después de gpsSetup()
    SerialMon.println(sendAT("AT+CGNSSPWR?"));           // debe ser 1
    SerialMon.println(sendAT("AT+CGNSSINFO"));           // estado raw
    SerialMon.println(sendAT("AT+CGNSSPORTSWITCH=0,1")); // redirige NMEA a SerialAT
    delay(2000);
    // Leer NMEA crudo por 3 segundos
    uint32_t t = millis();
    while (millis() - t < 3000)
    {
        while (SerialAT.available())
            SerialMon.write(SerialAT.read());
    }

    WiFiManager wm;
    if (!wm.autoConnect("Tracker-AP", "12345678"))
        ESP.restart();
    SerialMon.println("WiFi: " + WiFi.localIP().toString());

    mqtt.setServer(mqtt_broker, mqtt_port);
    mqtt.setCallback(mqttCallback);
}

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

    // Debug temporal — ver SNR de satélites
    if (millis() - lastPublish > 2000)
    {
        lastPublish = millis();
        String gps = getGPSData();
        if (gps.length() > 0)
        {
            mqtt.publish(topicGPS, gps.c_str());
        }
        else
        {
            // Ver cuántos satélites está viendo
            String info = sendAT("AT+CGNSSINFO");
            String sats = sendAT("AT+CGNSSNMEA?"); // estado NMEA
            SerialMon.println("SATS: " + sats);
            SerialMon.println("INFO: " + info);
            mqtt.publish(topicStatus, "Waiting for GPS fix");
        }
    }
}