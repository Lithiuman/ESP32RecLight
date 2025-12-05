#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include "USB.h"
#include "USBMIDI.h"

Preferences prefs;
bool shouldSaveConfig = false;
WiFiManager wm;

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

WiFiManagerParameter p_mqtt_server("server", "MQTT Server", mqtt_server, 40);
WiFiManagerParameter p_mqtt_port("port", "MQTT Port", mqtt_port, 6);
WiFiManagerParameter p_mqtt_user("user", "MQTT User", mqtt_user, 32);
WiFiManagerParameter p_mqtt_pass("pass", "MQTT Password", mqtt_pass, 32);
WiFiManagerParameter p_topic_rec("topic_rec", "Recording Topic", topic_recording, 32);
WiFiManagerParameter p_topic_arm("topic_arm", "Armed Topic", topic_armed, 32);
WiFiManagerParameter p_note_rec("note_rec", "Recording MIDI Note", rec_note_str, 4);
WiFiManagerParameter p_note_arm("note_arm", "Armed MIDI Note", armed_note_str, 4);

WiFiClient espClient;
PubSubClient client(espClient);

int recNote = 25;
int armedNote = 24;

#define AVAILABILITY_TOPIC "midi/availability"
bool ha_discovery_sent = false;

unsigned long lastArmedPulse = 0;
const unsigned long ARMED_TIMEOUT = 2500;
bool armedState = false;

bool recordingState = false;

unsigned long lastWifiCheck = 0;
unsigned long lastMqttAttempt = 0;
const unsigned long WIFI_RETRY_INTERVAL = 2500;
const unsigned long MQTT_RETRY_INTERVAL = 1000;

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
        ha_discovery_sent = false;
        client.publish(AVAILABILITY_TOPIC, "online", true);
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

    ha_discovery_sent = true;
}

void setupWiFiManager()
{
    Serial.println("[WIFIMGR] Entering setupWiFiManager");

    renderLED();

    prefs.begin("midicfg", true);
    String s;
    s = prefs.getString("mqtt_server", mqtt_server);
    s.toCharArray(mqtt_server, sizeof(s));
    s = prefs.getString("mqtt_port", mqtt_port);
    s.toCharArray(mqtt_port, sizeof(s));
    s = prefs.getString("mqtt_user", mqtt_user);
    s.toCharArray(mqtt_user, sizeof(s));
    s = prefs.getString("mqtt_pass", mqtt_pass);
    s.toCharArray(mqtt_pass, sizeof(s));
    s = prefs.getString("topic_rec", topic_recording);
    s.toCharArray(topic_recording, sizeof(s));
    s = prefs.getString("topic_arm", topic_armed);
    s.toCharArray(topic_armed, sizeof(s));
    s = prefs.getString("note_rec", rec_note_str);
    s.toCharArray(rec_note_str, sizeof(s));
    s = prefs.getString("note_arm", armed_note_str);
    s.toCharArray(armed_note_str, sizeof(s));
    prefs.end();

    recNote = atoi(rec_note_str);
    armedNote = atoi(armed_note_str);

    p_mqtt_server.setValue(mqtt_server, sizeof(mqtt_server));
    p_mqtt_port.setValue(mqtt_port, sizeof(mqtt_port));
    p_mqtt_user.setValue(mqtt_user, sizeof(mqtt_user));
    p_mqtt_pass.setValue(mqtt_pass, sizeof(mqtt_pass));
    p_topic_rec.setValue(topic_recording, sizeof(topic_recording));
    p_topic_arm.setValue(topic_armed, sizeof(topic_armed));
    p_note_rec.setValue(rec_note_str, sizeof(rec_note_str));
    p_note_arm.setValue(armed_note_str, sizeof(armed_note_str));

    wm.addParameter(&p_mqtt_server);
    wm.addParameter(&p_mqtt_port);
    wm.addParameter(&p_mqtt_user);
    wm.addParameter(&p_mqtt_pass);
    wm.addParameter(&p_topic_rec);
    wm.addParameter(&p_topic_arm);
    wm.addParameter(&p_note_rec);
    wm.addParameter(&p_note_arm);

    wm.setSaveConfigCallback(saveConfigCallback);

    Serial.println("[WIFIMGR] Starting autoConnect...");
    if (!wm.autoConnect("REC-MIDI-AP", "configureme"))
    {
        Serial.println("[WIFIMGR] autoConnect FAILED - restarting");
        prefs.begin("midicfg", false);
        prefs.putString("mqtt_server", p_mqtt_server.getValue());
        prefs.putString("mqtt_port", p_mqtt_port.getValue());
        prefs.putString("mqtt_user", p_mqtt_user.getValue());
        prefs.putString("mqtt_pass", p_mqtt_pass.getValue());
        prefs.putString("topic_rec", p_topic_rec.getValue());
        prefs.putString("topic_arm", p_topic_arm.getValue());
        prefs.putString("note_rec", p_note_rec.getValue());
        prefs.putString("note_arm", p_note_arm.getValue());
        prefs.end();
        ESP.restart();
    }
    Serial.println("[WIFIMGR] autoConnect returned");

    strcpy(mqtt_server, p_mqtt_server.getValue());
    strcpy(mqtt_port, p_mqtt_port.getValue());
    strcpy(mqtt_user, p_mqtt_user.getValue());
    strcpy(mqtt_pass, p_mqtt_pass.getValue());
    strcpy(topic_recording, p_topic_rec.getValue());
    strcpy(topic_armed, p_topic_arm.getValue());
    strcpy(rec_note_str, p_note_rec.getValue());
    strcpy(armed_note_str, p_note_arm.getValue());

    recNote = atoi(rec_note_str);
    armedNote = atoi(armed_note_str);

    delay(200);
}

void setup()
{
    Serial.begin(115200);

    led.begin();
    led.setBrightness(150);
    led.show();
    currentLedMode = LED_NO_WIFI;

    setupWiFiManager();

    client.setServer(mqtt_server, atoi(mqtt_port));

    led.clear();
    led.show();

    delay(2000);
    MIDI.begin();
    USB.begin();
}

void loop()
{
    checkWiFi();
    reconnectMQTT();
    client.loop();

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
