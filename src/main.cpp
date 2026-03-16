#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include "USB.h"
#include "USBMIDI.h"

Preferences prefs;
bool shouldSaveConfig = false;
WiFiManager wm;
WebServer webServer(80);

USBMIDI MIDI;

#define LED_PIN 48
#define NUM_LEDS 1
Adafruit_NeoPixel led(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

enum LedMode
{
    LED_NO_WIFI,
    LED_NO_MQTT,
    LED_CONNECTED_IDLE,
    LED_TRACK_ARMED,
    LED_RECORDING
};
LedMode currentLedMode = LED_NO_WIFI;
bool blinkState = false;
unsigned long lastBlinkToggle = 0;

const unsigned long BLINK_INTERVAL_NO_WIFI = 150;
const unsigned long BLINK_INTERVAL_NO_MQTT = 150;
const unsigned long BLINK_INTERVAL_ARMED = 100;

char mqtt_server[40] = "10.1.10.3";
char mqtt_port[6] = "1883";
char mqtt_user[32] = "dmtech";
char mqtt_pass[32] = "stamedia";
char topic_recording[32] = "midi/recording";
char topic_armed[32] = "midi/armed";
char rec_note_str[4] = "25";
char armed_note_str[4] = "24";
char wifi_restart_sec_str[6] = "60";
char periodic_restart_min_str[6] = "180";

WiFiManagerParameter p_mqtt_server("server", "MQTT Server", mqtt_server, 40);
WiFiManagerParameter p_mqtt_port("port", "MQTT Port", mqtt_port, 6);
WiFiManagerParameter p_mqtt_user("user", "MQTT User", mqtt_user, 32);
WiFiManagerParameter p_mqtt_pass("pass", "MQTT Password", mqtt_pass, 32);
WiFiManagerParameter p_topic_rec("topic_rec", "Recording Topic", topic_recording, 32);
WiFiManagerParameter p_topic_arm("topic_arm", "Armed Topic", topic_armed, 32);
WiFiManagerParameter p_note_rec("note_rec", "Recording MIDI Note", rec_note_str, 4);
WiFiManagerParameter p_note_arm("note_arm", "Armed MIDI Note", armed_note_str, 4);
WiFiManagerParameter p_wifi_restart_sec("wifi_restart_sec", "WiFi Disconnect Restart (sec)", wifi_restart_sec_str, 6);
WiFiManagerParameter p_periodic_restart_min("periodic_restart_min", "Periodic Restart (min)", periodic_restart_min_str, 6);

WiFiClient espClient;
PubSubClient client(espClient);

int recNote = 25;
int armedNote = 24;

#define AVAILABILITY_TOPIC "midi/availability"
#define RESET_COMMAND_TOPIC "midi/reset/command"
bool ha_discovery_sent = false;

unsigned long lastArmedPulse = 0;
const unsigned long ARMED_TIMEOUT = 2500;
bool armedState = false;

bool recordingState = false;

unsigned long lastWifiCheck = 0;
unsigned long lastMqttAttempt = 0;
const unsigned long WIFI_RETRY_INTERVAL = 2500;
const unsigned long MQTT_RETRY_INTERVAL = 1000;

unsigned long wifiDisconnectRestartIntervalMs = 60000;
unsigned long periodicRestartIntervalMs = 10800000;

unsigned long wifiDisconnectedSince = 0;

const char WEBPAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>MIDI Record Light</title>
    <style>
        :root { color-scheme: dark; }
        body {
            margin: 0;
            font-family: Inter, Segoe UI, sans-serif;
            background: #0f172a;
            color: #e2e8f0;
        }
        .container {
            max-width: 860px;
            margin: 0 auto;
            padding: 20px;
        }
        .header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            gap: 12px;
            margin-bottom: 16px;
        }
        .title {
            font-size: 1.4rem;
            font-weight: 700;
        }
        .pill {
            background: #1e293b;
            border: 1px solid #334155;
            border-radius: 999px;
            padding: 6px 10px;
            font-size: 0.85rem;
            color: #93c5fd;
        }
        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 12px;
            margin-bottom: 18px;
        }
        .card {
            background: #111827;
            border: 1px solid #1f2937;
            border-radius: 12px;
            padding: 12px;
        }
        .label {
            font-size: 0.78rem;
            color: #94a3b8;
            margin-bottom: 6px;
            text-transform: uppercase;
            letter-spacing: 0.06em;
        }
        .value {
            font-size: 1.02rem;
            font-weight: 600;
            color: #f8fafc;
            word-break: break-word;
        }
        .section-title {
            font-size: 1rem;
            font-weight: 600;
            margin: 10px 0;
            color: #cbd5e1;
        }
        form {
            background: #111827;
            border: 1px solid #1f2937;
            border-radius: 12px;
            padding: 14px;
        }
        .form-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
            gap: 10px;
        }
        .field {
            display: flex;
            flex-direction: column;
            gap: 6px;
        }
        .field input {
            background: #0b1220;
            border: 1px solid #334155;
            border-radius: 8px;
            color: #f8fafc;
            padding: 8px;
            font-size: 0.94rem;
        }
        button {
            margin-top: 12px;
            background: #2563eb;
            border: none;
            border-radius: 8px;
            color: white;
            font-weight: 600;
            padding: 10px 14px;
            cursor: pointer;
        }
        .hint {
            color: #94a3b8;
            font-size: 0.82rem;
            margin-top: 10px;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <div class="title">MIDI Record Light</div>
            <div class="pill" id="build">Live Status</div>
        </div>

        <div class="grid">
            <div class="card"><div class="label">WiFi</div><div class="value" id="wifi">--</div></div>
            <div class="card"><div class="label">MQTT</div><div class="value" id="mqtt">--</div></div>
            <div class="card"><div class="label">LED Mode</div><div class="value" id="led">--</div></div>
            <div class="card"><div class="label">Recording</div><div class="value" id="rec">--</div></div>
            <div class="card"><div class="label">Armed</div><div class="value" id="armed">--</div></div>
            <div class="card"><div class="label">Uptime (s)</div><div class="value" id="uptime">--</div></div>
            <div class="card"><div class="label">IP Address</div><div class="value" id="ip">--</div></div>
            <div class="card"><div class="label">RSSI</div><div class="value" id="rssi">--</div></div>
            <div class="card"><div class="label">Next WiFi Reset (s)</div><div class="value" id="wifi_reset_in">--</div></div>
            <div class="card"><div class="label">Next Periodic Reset (s)</div><div class="value" id="periodic_reset_in">--</div></div>
        </div>

        <div class="section-title">WiFiManager Parameters</div>
        <form method="POST" action="/config">
            <div class="form-grid">
                <label class="field">MQTT Server<input name="mqtt_server" maxlength="39" required /></label>
                <label class="field">MQTT Port<input name="mqtt_port" maxlength="5" required /></label>
                <label class="field">MQTT User<input name="mqtt_user" maxlength="31" /></label>
                <label class="field">MQTT Password<input name="mqtt_pass" maxlength="31" type="password" /></label>
                <label class="field">Recording Topic<input name="topic_rec" maxlength="31" required /></label>
                <label class="field">Armed Topic<input name="topic_arm" maxlength="31" required /></label>
                <label class="field">Recording MIDI Note<input name="note_rec" maxlength="3" required /></label>
                <label class="field">Armed MIDI Note<input name="note_arm" maxlength="3" required /></label>
                <label class="field">WiFi Disconnect Restart (sec)<input name="wifi_restart_sec" maxlength="5" required /></label>
                <label class="field">Periodic Restart (min)<input name="periodic_restart_min" maxlength="5" required /></label>
            </div>
            <button type="submit">Save Parameters</button>
            <div class="hint">After save, values apply immediately and persist to NVS.</div>
        </form>
    </div>

    <script>
        async function refreshStatus() {
            try {
                const response = await fetch('/status.json', { cache: 'no-store' });
                const data = await response.json();
                document.getElementById('wifi').textContent = data.wifi;
                document.getElementById('mqtt').textContent = data.mqtt;
                document.getElementById('led').textContent = data.led_mode;
                document.getElementById('rec').textContent = data.recording;
                document.getElementById('armed').textContent = data.armed;
                document.getElementById('uptime').textContent = data.uptime_seconds;
                document.getElementById('ip').textContent = data.ip;
                document.getElementById('rssi').textContent = data.rssi;
                document.getElementById('wifi_reset_in').textContent = data.wifi_reset_countdown_seconds;
                document.getElementById('periodic_reset_in').textContent = data.periodic_reset_countdown_seconds;

                const form = document.forms[0];
                form.mqtt_server.value = data.cfg_mqtt_server;
                form.mqtt_port.value = data.cfg_mqtt_port;
                form.mqtt_user.value = data.cfg_mqtt_user;
                form.mqtt_pass.value = data.cfg_mqtt_pass;
                form.topic_rec.value = data.cfg_topic_rec;
                form.topic_arm.value = data.cfg_topic_arm;
                form.note_rec.value = data.cfg_note_rec;
                form.note_arm.value = data.cfg_note_arm;
                form.wifi_restart_sec.value = data.cfg_wifi_restart_sec;
                form.periodic_restart_min.value = data.cfg_periodic_restart_min;
            } catch (_) {}
        }

        refreshStatus();
        setInterval(refreshStatus, 2000);
    </script>
</body>
</html>
)rawliteral";

const char SAVE_OK_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Saved</title></head>
<body style="font-family:Segoe UI,sans-serif;background:#0f172a;color:#e2e8f0;padding:24px;">
<h2>Parameters saved</h2>
<p>Settings were persisted and applied. Returning to status page…</p>
<script>setTimeout(()=>location.href='/',1200);</script>
</body></html>
)rawliteral";

void copyToBuffer(char *destination, size_t destinationSize, const String &value)
{
        value.toCharArray(destination, destinationSize);
}

const char *ledModeToString(LedMode mode)
{
        switch (mode)
        {
        case LED_NO_WIFI:
                return "NO_WIFI";
        case LED_NO_MQTT:
                return "NO_MQTT";
        case LED_CONNECTED_IDLE:
                return "CONNECTED_IDLE";
        case LED_TRACK_ARMED:
                return "TRACK_ARMED";
        case LED_RECORDING:
                return "RECORDING";
        default:
                return "UNKNOWN";
        }
}

String wifiStateString()
{
        wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED)
                return "Connected";
        if (status == WL_DISCONNECTED)
                return "Disconnected";
        if (status == WL_CONNECT_FAILED)
                return "Connect Failed";
        if (status == WL_CONNECTION_LOST)
                return "Connection Lost";
        return "Not Ready";
}

void applyCurrentConfigToRuntime()
{
        recNote = atoi(rec_note_str);
        armedNote = atoi(armed_note_str);
        if (recNote < 0)
                recNote = 0;
        if (recNote > 127)
                recNote = 127;
        if (armedNote < 0)
                armedNote = 0;
        if (armedNote > 127)
                armedNote = 127;

        snprintf(rec_note_str, sizeof(rec_note_str), "%d", recNote);
        snprintf(armed_note_str, sizeof(armed_note_str), "%d", armedNote);

        unsigned long wifiRestartSec = strtoul(wifi_restart_sec_str, nullptr, 10);
        unsigned long periodicRestartMin = strtoul(periodic_restart_min_str, nullptr, 10);

        if (wifiRestartSec < 10)
            wifiRestartSec = 10;
        if (wifiRestartSec > 86400)
            wifiRestartSec = 86400;

        if (periodicRestartMin < 1)
            periodicRestartMin = 1;
        if (periodicRestartMin > 10080)
            periodicRestartMin = 10080;

        wifiDisconnectRestartIntervalMs = wifiRestartSec * 1000UL;
        periodicRestartIntervalMs = periodicRestartMin * 60000UL;

        snprintf(wifi_restart_sec_str, sizeof(wifi_restart_sec_str), "%lu", wifiRestartSec);
        snprintf(periodic_restart_min_str, sizeof(periodic_restart_min_str), "%lu", periodicRestartMin);

        client.setServer(mqtt_server, atoi(mqtt_port));

        p_mqtt_server.setValue(mqtt_server, sizeof(mqtt_server));
        p_mqtt_port.setValue(mqtt_port, sizeof(mqtt_port));
        p_mqtt_user.setValue(mqtt_user, sizeof(mqtt_user));
        p_mqtt_pass.setValue(mqtt_pass, sizeof(mqtt_pass));
        p_topic_rec.setValue(topic_recording, sizeof(topic_recording));
        p_topic_arm.setValue(topic_armed, sizeof(topic_armed));
        p_note_rec.setValue(rec_note_str, sizeof(rec_note_str));
        p_note_arm.setValue(armed_note_str, sizeof(armed_note_str));
        p_wifi_restart_sec.setValue(wifi_restart_sec_str, sizeof(wifi_restart_sec_str));
        p_periodic_restart_min.setValue(periodic_restart_min_str, sizeof(periodic_restart_min_str));
}

void saveCurrentConfigToPrefs()
{
        prefs.begin("midicfg", false);
        prefs.putString("mqtt_server", mqtt_server);
        prefs.putString("mqtt_port", mqtt_port);
        prefs.putString("mqtt_user", mqtt_user);
        prefs.putString("mqtt_pass", mqtt_pass);
        prefs.putString("topic_rec", topic_recording);
        prefs.putString("topic_arm", topic_armed);
        prefs.putString("note_rec", rec_note_str);
        prefs.putString("note_arm", armed_note_str);
        prefs.putString("wifi_rst_sec", wifi_restart_sec_str);
        prefs.putString("periodic_rst_min", periodic_restart_min_str);
        prefs.end();
}

void handleStatusJson()
{
    unsigned long now = millis();
    unsigned long wifiCountdownSec = 0;
    if (WiFi.status() != WL_CONNECTED)
    {
        if (wifiDisconnectedSince == 0)
            wifiCountdownSec = wifiDisconnectRestartIntervalMs / 1000;
        else if (now - wifiDisconnectedSince >= wifiDisconnectRestartIntervalMs)
            wifiCountdownSec = 0;
        else
            wifiCountdownSec = (wifiDisconnectRestartIntervalMs - (now - wifiDisconnectedSince)) / 1000;
    }

    unsigned long periodicCountdownSec;
    if (now >= periodicRestartIntervalMs)
        periodicCountdownSec = 0;
    else
        periodicCountdownSec = (periodicRestartIntervalMs - now) / 1000;

        StaticJsonDocument<768> doc;
        doc["wifi"] = wifiStateString();
        doc["mqtt"] = client.connected() ? "Connected" : "Disconnected";
        doc["led_mode"] = ledModeToString(currentLedMode);
        doc["recording"] = recordingState ? "ON" : "OFF";
        doc["armed"] = armedState ? "ON" : "OFF";
        doc["uptime_seconds"] = millis() / 1000;
        doc["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : "0.0.0.0";
        doc["rssi"] = WiFi.isConnected() ? String(WiFi.RSSI()) + " dBm" : "n/a";
        doc["wifi_reset_countdown_seconds"] = wifiCountdownSec;
        doc["periodic_reset_countdown_seconds"] = periodicCountdownSec;
        doc["cfg_mqtt_server"] = mqtt_server;
        doc["cfg_mqtt_port"] = mqtt_port;
        doc["cfg_mqtt_user"] = mqtt_user;
        doc["cfg_mqtt_pass"] = mqtt_pass;
        doc["cfg_topic_rec"] = topic_recording;
        doc["cfg_topic_arm"] = topic_armed;
        doc["cfg_note_rec"] = rec_note_str;
        doc["cfg_note_arm"] = armed_note_str;
        doc["cfg_wifi_restart_sec"] = wifi_restart_sec_str;
        doc["cfg_periodic_restart_min"] = periodic_restart_min_str;

        String json;
        serializeJson(doc, json);
        webServer.send(200, "application/json", json);
}

void handleSaveConfig()
{
        if (webServer.hasArg("mqtt_server"))
                copyToBuffer(mqtt_server, sizeof(mqtt_server), webServer.arg("mqtt_server"));
        if (webServer.hasArg("mqtt_port"))
                copyToBuffer(mqtt_port, sizeof(mqtt_port), webServer.arg("mqtt_port"));
        if (webServer.hasArg("mqtt_user"))
                copyToBuffer(mqtt_user, sizeof(mqtt_user), webServer.arg("mqtt_user"));
        if (webServer.hasArg("mqtt_pass"))
                copyToBuffer(mqtt_pass, sizeof(mqtt_pass), webServer.arg("mqtt_pass"));
        if (webServer.hasArg("topic_rec"))
                copyToBuffer(topic_recording, sizeof(topic_recording), webServer.arg("topic_rec"));
        if (webServer.hasArg("topic_arm"))
                copyToBuffer(topic_armed, sizeof(topic_armed), webServer.arg("topic_arm"));
        if (webServer.hasArg("note_rec"))
                copyToBuffer(rec_note_str, sizeof(rec_note_str), webServer.arg("note_rec"));
        if (webServer.hasArg("note_arm"))
                copyToBuffer(armed_note_str, sizeof(armed_note_str), webServer.arg("note_arm"));
        if (webServer.hasArg("wifi_restart_sec"))
            copyToBuffer(wifi_restart_sec_str, sizeof(wifi_restart_sec_str), webServer.arg("wifi_restart_sec"));
        if (webServer.hasArg("periodic_restart_min"))
            copyToBuffer(periodic_restart_min_str, sizeof(periodic_restart_min_str), webServer.arg("periodic_restart_min"));

        applyCurrentConfigToRuntime();
        saveCurrentConfigToPrefs();

        Serial.println("[WEB] Parameters updated from web UI");
        webServer.send(200, "text/html", SAVE_OK_HTML);
}

void setupWebServer()
{
        webServer.on("/", HTTP_GET, []()
                                 { webServer.send(200, "text/html", WEBPAGE_HTML); });
        webServer.on("/status.json", HTTP_GET, handleStatusJson);
        webServer.on("/config", HTTP_POST, handleSaveConfig);
        webServer.begin();
        Serial.println("[WEB] HTTP server started on port 80");
}

void restartDevice(const char *reason)
{
    Serial.printf("[SYS] Restarting: %s\n", reason);
    delay(100);
    led.setPixelColor(0, led.Color(0, 0, 0));
    led.show();
    delay(100);
    ESP.restart();
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    if (strcmp(topic, RESET_COMMAND_TOPIC) != 0)
        return;

    String message;
    for (unsigned int i = 0; i < length; i++)
        message += (char)payload[i];
    message.trim();

    if (message.equalsIgnoreCase("PRESS") || message.equalsIgnoreCase("RESET") || message.equalsIgnoreCase("RESTART") || message.length() == 0)
        restartDevice("Home Assistant reset button");
}

void checkResetConditions()
{
    unsigned long now = millis();

    if (WiFi.status() != WL_CONNECTED)
    {
        if (wifiDisconnectedSince == 0)
            wifiDisconnectedSince = now;
        else if (now - wifiDisconnectedSince >= wifiDisconnectRestartIntervalMs)
            restartDevice("WiFi disconnected timeout");
    }
    else
    {
        wifiDisconnectedSince = 0;
    }

    if (now >= periodicRestartIntervalMs)
        restartDevice("Periodic restart interval reached");
}

void renderLED()
{
    unsigned long now = millis();
    unsigned long interval = 500;

    switch (currentLedMode)
    {
    case LED_NO_WIFI:
        interval = BLINK_INTERVAL_NO_WIFI;
        break;
    case LED_NO_MQTT:
        interval = BLINK_INTERVAL_NO_MQTT;
        break;
    case LED_TRACK_ARMED:
        interval = BLINK_INTERVAL_ARMED;
        break;
    default:
        interval = 500;
        break;
    }

    if (now - lastBlinkToggle >= interval)
    {
        lastBlinkToggle = now;
        blinkState = !blinkState;
    }

    uint32_t color = led.Color(0, 0, 0);

    switch (currentLedMode)
    {
    case LED_NO_WIFI:
        color = blinkState ? led.Color(0, 200, 255) : led.Color(0, 0, 255);
        break;
    case LED_NO_MQTT:
        color = blinkState ? led.Color(200, 0, 255) : led.Color(0, 0, 255);
        break;
    case LED_CONNECTED_IDLE:
        color = led.Color(100, 0, 255);
        break;
    case LED_TRACK_ARMED:
        color = blinkState ? led.Color(255, 30, 0) : led.Color(0, 0, 0);
        break;
    case LED_RECORDING:
        color = led.Color(255, 0, 0);
        break;
    }

    led.setPixelColor(0, color);
    led.show();
}

void saveConfigCallback()
{
    Serial.println(">>> WiFiManager says: SAVE NEEDED <<<");
    prefs.begin("midicfg", false);
    prefs.putString("mqtt_server", p_mqtt_server.getValue());
    prefs.putString("mqtt_port", p_mqtt_port.getValue());
    prefs.putString("mqtt_user", p_mqtt_user.getValue());
    prefs.putString("mqtt_pass", p_mqtt_pass.getValue());
    prefs.putString("topic_rec", p_topic_rec.getValue());
    prefs.putString("topic_arm", p_topic_arm.getValue());
    prefs.putString("note_rec", p_note_rec.getValue());
    prefs.putString("note_arm", p_note_arm.getValue());
    prefs.putString("wifi_rst_sec", p_wifi_restart_sec.getValue());
    prefs.putString("periodic_rst_min", p_periodic_restart_min.getValue());
    prefs.end();
    shouldSaveConfig = true;
    Serial.println("[CFG] Settings saved to NVS in callback");
}

void checkWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
        return;
    if (millis() - lastWifiCheck < WIFI_RETRY_INTERVAL)
        return;
    lastWifiCheck = millis();
    Serial.println("[WiFi] Lost connection, attempting reconnect...");
    WiFi.reconnect();
}

void reconnectMQTT()
{
    if (WiFi.status() != WL_CONNECTED || client.connected())
        return;
    if (millis() - lastMqttAttempt < MQTT_RETRY_INTERVAL)
        return;
    lastMqttAttempt = millis();

    Serial.println("[MQTT] Attempting reconnect...");
    String clientId = "ESP32-MIDI-" + String(random(0xffff), HEX);
    client.setBufferSize(1024);

    bool connected = strlen(mqtt_user) > 0 ? client.connect(clientId.c_str(), mqtt_user, mqtt_pass, AVAILABILITY_TOPIC, 0, true, "offline") : client.connect(clientId.c_str(), nullptr, nullptr, AVAILABILITY_TOPIC, 0, true, "offline");

    if (connected)
    {
        Serial.println("[MQTT] Connected!");
        client.publish(AVAILABILITY_TOPIC, "online", true);
        client.subscribe(RESET_COMMAND_TOPIC);
        ha_discovery_sent = false;
    }
}

void sendHADiscovery()
{
    if (!client.connected())
        return;

    //yes all this is depracated but if it aint broke don't fix it    

    StaticJsonDocument<768> doc;
    doc["name"] = "Recording";
    doc["state_topic"] = String(topic_recording);
    doc["payload_on"] = "ON";
    doc["payload_off"] = "OFF";
    doc["unique_id"] = "esp32_midi_record_light";
    doc["availability_topic"] = AVAILABILITY_TOPIC;
    JsonObject device = doc.createNestedObject("device");
    device["identifiers"][0] = "esp32_midi_light";
    device["name"] = "MIDI Record Light";
    device["manufacturer"] = "Christopher Seibel";
    device["model"] = "ESP32S3 MIDI USB";

    char buf1[768];
    serializeJson(doc, buf1);
    client.publish("homeassistant/binary_sensor/esp32_midi_record_light/config", (const uint8_t *)buf1, strlen(buf1), true);

    StaticJsonDocument<768> doc2;
    doc2["name"] = "Armed";
    doc2["state_topic"] = String(topic_armed);
    doc2["payload_on"] = "ON";
    doc2["payload_off"] = "OFF";
    doc2["unique_id"] = "esp32_midi_armed_light";
    doc2["availability_topic"] = AVAILABILITY_TOPIC;
    JsonObject device2 = doc2.createNestedObject("device");
    device2["identifiers"][0] = "esp32_midi_light";
    device2["name"] = "MIDI Record Light";
    device2["manufacturer"] = "Christopher Seibel";
    device2["model"] = "ESP32S3 MIDI USB";

    char buf2[768];
    serializeJson(doc2, buf2);
    client.publish("homeassistant/binary_sensor/esp32_midi_armed_light/config", (const uint8_t *)buf2, strlen(buf2), true);

    StaticJsonDocument<768> doc3;
    doc3["name"] = "Restart Device";
    doc3["command_topic"] = RESET_COMMAND_TOPIC;
    doc3["payload_press"] = "PRESS";
    doc3["unique_id"] = "esp32_midi_restart_button";
    doc3["availability_topic"] = AVAILABILITY_TOPIC;
    JsonObject device3 = doc3.createNestedObject("device");
    device3["identifiers"][0] = "esp32_midi_light";
    device3["name"] = "MIDI Record Light";
    device3["manufacturer"] = "Christopher Seibel";
    device3["model"] = "ESP32S3 MIDI USB";

    char buf3[768];
    serializeJson(doc3, buf3);
    client.publish("homeassistant/button/esp32_midi_restart/config", (const uint8_t *)buf3, strlen(buf3), true);

    ha_discovery_sent = true;
}

void setupWiFiManager()
{
    Serial.println("[WIFIMGR] Entering setupWiFiManager");

    WiFi.mode(WIFI_STA);
    delay(100);

    wm.setHostname("ESP32-RECLIGHT");
    WiFi.setHostname("ESP32-RECLIGHT");

    wm.setConnectTimeout(20);
    wm.setConfigPortalTimeout(120);
    wm.setCleanConnect(false);

    renderLED();

    prefs.begin("midicfg", true);
    String s;
    s = prefs.getString("mqtt_server", mqtt_server);
    s.toCharArray(mqtt_server, sizeof(mqtt_server));
    s = prefs.getString("mqtt_port", mqtt_port);
    s.toCharArray(mqtt_port, sizeof(mqtt_port));
    s = prefs.getString("mqtt_user", mqtt_user);
    s.toCharArray(mqtt_user, sizeof(mqtt_user));
    s = prefs.getString("mqtt_pass", mqtt_pass);
    s.toCharArray(mqtt_pass, sizeof(mqtt_pass));
    s = prefs.getString("topic_rec", topic_recording);
    s.toCharArray(topic_recording, sizeof(topic_recording));
    s = prefs.getString("topic_arm", topic_armed);
    s.toCharArray(topic_armed, sizeof(topic_armed));
    s = prefs.getString("note_rec", rec_note_str);
    s.toCharArray(rec_note_str, sizeof(rec_note_str));
    s = prefs.getString("note_arm", armed_note_str);
    s.toCharArray(armed_note_str, sizeof(armed_note_str));
    s = prefs.getString("wifi_rst_sec", wifi_restart_sec_str);
    s.toCharArray(wifi_restart_sec_str, sizeof(wifi_restart_sec_str));
    s = prefs.getString("periodic_rst_min", periodic_restart_min_str);
    s.toCharArray(periodic_restart_min_str, sizeof(periodic_restart_min_str));
    prefs.end();

    applyCurrentConfigToRuntime();

    wm.addParameter(&p_mqtt_server);
    wm.addParameter(&p_mqtt_port);
    wm.addParameter(&p_mqtt_user);
    wm.addParameter(&p_mqtt_pass);
    wm.addParameter(&p_topic_rec);
    wm.addParameter(&p_topic_arm);
    wm.addParameter(&p_note_rec);
    wm.addParameter(&p_note_arm);
    wm.addParameter(&p_wifi_restart_sec);
    wm.addParameter(&p_periodic_restart_min);

    wm.setSaveConfigCallback(saveConfigCallback);

    Serial.println("[WIFIMGR] Starting autoConnect...");
    bool wifiConnected = false;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (wm.autoConnect("REC-MIDI-AP", "configureme"))
        {
            wifiConnected = true;
            Serial.println("[WIFIMGR] autoConnect succeeded");
            break;
        }
        Serial.printf("[WIFIMGR] autoConnect attempt %d failed, retrying...\n", attempt + 1);
        delay(500);
    }

    if (!wifiConnected)
    {
        Serial.println("[WIFIMGR] autoConnect FAILED after retries - entering AP mode");
        wm.startConfigPortal("REC-MIDI-AP", "configureme");
    }

    strcpy(mqtt_server, p_mqtt_server.getValue());
    strcpy(mqtt_port, p_mqtt_port.getValue());
    strcpy(mqtt_user, p_mqtt_user.getValue());
    strcpy(mqtt_pass, p_mqtt_pass.getValue());
    strcpy(topic_recording, p_topic_rec.getValue());
    strcpy(topic_armed, p_topic_arm.getValue());
    strcpy(rec_note_str, p_note_rec.getValue());
    strcpy(armed_note_str, p_note_arm.getValue());
    strcpy(wifi_restart_sec_str, p_wifi_restart_sec.getValue());
    strcpy(periodic_restart_min_str, p_periodic_restart_min.getValue());

    applyCurrentConfigToRuntime();

    delay(200);
}

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n[SETUP] Starting up...");

    led.begin();
    led.setBrightness(150);
    led.show();
    currentLedMode = LED_NO_WIFI;

    setupWiFiManager();

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[SETUP] WiFi not connected after setupWiFiManager, will retry in loop");
        currentLedMode = LED_NO_WIFI;
    }
    else
    {
        Serial.printf("[SETUP] WiFi connected to %s, IP: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }

    client.setServer(mqtt_server, atoi(mqtt_port));
    client.setCallback(mqttCallback);

    setupWebServer();

    led.clear();
    led.show();

    delay(500);
    Serial.println("[SETUP] Setup complete, entering loop");
    MIDI.begin();
    USB.begin();
}

void loop()
{
    checkResetConditions();
    checkWiFi();
    reconnectMQTT();
    client.loop();
    webServer.handleClient();

    if (!ha_discovery_sent && client.connected())
        sendHADiscovery();

    midiEventPacket_t in = {0, 0, 0, 0};
    if (MIDI.readPacket(&in))
    {
        bool noteOn = (in.header == MIDI_CIN_NOTE_ON && in.byte3 > 0);
        bool noteOff = (in.header == MIDI_CIN_NOTE_OFF) || (in.header == MIDI_CIN_NOTE_ON && in.byte3 == 0);

        if (in.byte2 == recNote)
        {
            recordingState = noteOn;
            if (client.connected())
                client.publish(topic_recording, noteOn ? "ON" : "OFF", true);
        }

        if (in.byte2 == armedNote && noteOn)
        {
            lastArmedPulse = millis();
            if (!armedState)
            {
                armedState = true;
                if (client.connected())
                    client.publish(topic_armed, "ON", true);
            }
        }
    }

    if (armedState && (millis() - lastArmedPulse > ARMED_TIMEOUT))
    {
        armedState = false;
        if (client.connected())
            client.publish(topic_armed, "OFF", true);
    }

    if (WiFi.status() != WL_CONNECTED)
        currentLedMode = LED_NO_WIFI;
    else if (!client.connected())
        currentLedMode = LED_NO_MQTT;
    else if (recordingState)
        currentLedMode = LED_RECORDING;
    else if (armedState)
        currentLedMode = LED_TRACK_ARMED;
    else
        currentLedMode = LED_CONNECTED_IDLE;

    renderLED();
}
