#include <FS.h>
#include <EEPROM.h>
#include <DNSServer.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

//for reboot api
#define WEBSERVER_H
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>

// * Include settings
#include "settings.h"

// * Initiate led blinker library
Ticker ticker;

// * Initiate WIFI client
WiFiClient espClient;

// * Initiate MQTT client
PubSubClient mqtt_client(espClient);

// * Initiate API settings
AsyncWebServer httpRestServer(HTTP_REST_PORT);
JsonDocument jsonDoc;

// **********************************
// * API                           *
// **********************************
// Serving Reboot API
void handleRoot(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", "Ok - P1Meter");
}
void handleRestart(AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Server is restarting in 5 seconds, restarting can take up to 1 minute...");
    
    Serial.println("Server is restarting in 5 seconds, restarting can take up to 1 minute...");
    
    restartInitiated = millis();
    setRestart = true;
}
// Serving Home Assistant Discovery on
void handleHaon(AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Ok - HA Discovery is on");
    setHaDOn = true;
}
// Serving Home Assistant Discovery off
void handleHaoff(AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Ok - HA Discovery is off");
    setHaDOff = true;
}
void handleMQTTSettings(AsyncWebServerRequest *request){
    if (request->hasParam("server")) {
      const char* server = request->getParam("server")->value().c_str();

      // Update MQTT server
      read_eeprom(0, 64).toCharArray(MQTT_HOST, 64);
      Serial.printf("MQTT Server0: %s\n", MQTT_HOST);

      strcpy(MQTT_HOST, server);

      Serial.printf("MQTT Server1: %s\n", MQTT_HOST);

      write_eeprom(0, 64, MQTT_HOST);
      EEPROM.commit();

      read_eeprom(0, 64).toCharArray(MQTT_HOST, 64);
      Serial.printf("MQTT Server2: %s\n", MQTT_HOST);

      //mqttClient.publish(mqtt_topic, mqtt_message); // Publish message
      request->send(200, "text/plain", "MQTT server updated successfully!");
      restartInitiated = millis();
      setRestart = true;
    } else {
      request->send(400, "text/plain", "Missing server details");
    }
}

void handleHaDiscoveryOn() {
  HA_DISCOVERY = EEPROM.read(167);
  if(HA_DISCOVERY == false){
    write_eeprom(167, 5, "true");
    EEPROM.commit();
    HA_DISCOVERY = true;
  }
  haDiscovery("on");
  haDiscoverySetupRun = true;

  Serial.println("Home Assistant discovery is on...");
}
void handleHaDiscoveryOff() {
  HA_DISCOVERY = EEPROM.read(167);
  if(HA_DISCOVERY == true){
    write_eeprom(167, 5, "false");
    EEPROM.commit();
    HA_DISCOVERY = false;
  }
  haDiscovery("off");
  haDiscoverySetupRun = false;

  Serial.println("Home Assistant discovery is off...");
}

// Creating the Home Assistant config messages
void sendHomeAssistantConfig(const char *sensorname, const char *sensorid, const String& deviceclass, const String& setHomeAssistant) {
    topic = String(MQTT_ROOT_TOPIC) + "/" + String(sensorid) + "/config";

    if (setHomeAssistant == "on") {
        jsonDoc.clear();

        jsonDoc["platform"] = "mqtt";
        jsonDoc["unique_id"] = "p1_" + String(sensorid);
        jsonDoc["name"] = String(sensorname);
        jsonDoc["state_topic"] = String(MQTT_ROOT_TOPIC) + "/" + String(sensorid) + "/state";

        jsonDoc["device"]["identifiers"][0] = "p1meter1337";
        jsonDoc["device"]["name"] = "P1 Meter";
        jsonDoc["device"]["model"] = "P1 Meter with HA and API integration";
        jsonDoc["device"]["manufacturer"] = "tedzor";
        jsonDoc["device"]["sw_version"] = "1.4";

        jsonDoc["device_class"] = String(deviceclass);

        if (deviceclass == "power" || deviceclass == "current" || deviceclass == "voltage") {
            jsonDoc["state_class"] = "measurement";
            jsonDoc["value_template"] = "{{ value|float / 1000 }}";

            if (deviceclass == "power") {
                jsonDoc["unit_of_measurement"] = "kW";
                jsonDoc["icon"] = "mdi:lightning-bolt";
            } else if (deviceclass == "current") {
                jsonDoc["unit_of_measurement"] = "A";
                jsonDoc["icon"] = "mdi:current-ac";
            } else if (deviceclass == "voltage") {
                jsonDoc["unit_of_measurement"] = "V";
                jsonDoc["icon"] = "mdi:sine-wave";
            }
        } else if (deviceclass == "energy") {
            jsonDoc["state_class"] = "total_increasing";
            jsonDoc["value_template"] = "{{ value|float / 1000 }}";
            jsonDoc["unit_of_measurement"] = "kWh";
            jsonDoc["icon"] = "mdi:meter-electric-outline";
        } else if (deviceclass == "gas") {
            jsonDoc["state_class"] = "total_increasing";
            jsonDoc["value_template"] = "{{ value|float / 1000 }}";
            jsonDoc["unit_of_measurement"] = "m³";
            jsonDoc["icon"] = "mdi:meter-gas";
        } else {
            // fallback of default waarden (optioneel)
        }

        String jsonString;
        serializeJson(jsonDoc, jsonString);

        bool result = mqtt_client.publish(topic.c_str(), jsonString.c_str(), true);

        if (!result) {
            Serial.printf("MQTT publish to topic %s failed\n", topic.c_str());
        }
    } else if (setHomeAssistant == "off") {
        bool result = mqtt_client.publish(topic.c_str(), "", true);

        if (!result) {
            Serial.printf("MQTT publish to set HA discovery off; topic %s failed\n", topic.c_str());
        }
    }
}

// Initiating the Home Assistant config messages
void haDiscovery(const String& setHomeAssistant) {
    sendHomeAssistantConfig("L1 instant power usage", "l1_instant_power_usage", "power", setHomeAssistant);
    sendHomeAssistantConfig("L1 Instant Power Current", "l1_instant_power_current", "current", setHomeAssistant);
    sendHomeAssistantConfig("L1 Voltage", "l1_voltage", "voltage", setHomeAssistant);
    sendHomeAssistantConfig("L2 instant power usage", "l2_instant_power_usage", "power", setHomeAssistant);
    sendHomeAssistantConfig("L2 Instant Power Current", "l2_instant_power_current", "current", setHomeAssistant);
    sendHomeAssistantConfig("L2 Voltage", "l2_voltage", "voltage", setHomeAssistant);
    sendHomeAssistantConfig("L3 instant power usage", "l3_instant_power_usage", "power", setHomeAssistant);
    sendHomeAssistantConfig("L3 Instant Power Current", "l3_instant_power_current", "current", setHomeAssistant);
    sendHomeAssistantConfig("L3 Voltage", "l3_voltage", "voltage", setHomeAssistant);
    sendHomeAssistantConfig("Actual Power Consumption", "actual_consumption", "power", setHomeAssistant);
    sendHomeAssistantConfig("Actual Return Delivery", "actual_returndelivery", "power", setHomeAssistant);
    sendHomeAssistantConfig("Gas Usage", "gas_meter_m3", "gas", setHomeAssistant);
    sendHomeAssistantConfig("Consumption High Tariff", "consumption_high_tarif", "energy", setHomeAssistant);
    sendHomeAssistantConfig("Consumption Low Tariff", "consumption_low_tarif", "energy", setHomeAssistant);
    sendHomeAssistantConfig("Return Delivery High Tariff", "returndelivery_high_tarif", "energy", setHomeAssistant);
    sendHomeAssistantConfig("Return Delivery Low Tariff", "returndelivery_low_tarif", "energy", setHomeAssistant);
    sendHomeAssistantConfig("Long Power Outages", "long_power_outages", "other", setHomeAssistant);
    sendHomeAssistantConfig("Short Power Outages", "short_power_outages", "other", setHomeAssistant);
    sendHomeAssistantConfig("Short Power Drops", "short_power_drops", "other", setHomeAssistant);
    sendHomeAssistantConfig("Short Power Peaks", "short_power_peaks", "other", setHomeAssistant);
    sendHomeAssistantConfig("Actual Tariff Group", "actual_tarif_group", "other", setHomeAssistant);
}

// **********************************
// * WIFI                           *
// **********************************
// * Gets called when WiFiManager enters configuration mode
void configModeCallback(WiFiManager *myWiFiManager) {
    Serial.println(F("Entered config mode"));
    Serial.println(WiFi.softAPIP());

    // * If you used auto generated SSID, print it
    Serial.println(myWiFiManager->getConfigPortalSSID());

    // * Entered config mode, make led toggle faster
    ticker.attach(0.2, tick);
}

// **********************************
// * Ticker (System LED Blinker)    *
// **********************************
// * Blink on-board Led
void tick() {
    // * Toggle state
    int state = digitalRead(LED_BUILTIN);    // * Get the current state of GPIO1 pin
    digitalWrite(LED_BUILTIN, !state);       // * Set pin to the opposite state
}

// **********************************
// * MQTT                           *
// **********************************
// * Send a message to a broker topic
void send_mqtt_message(const char *topic, char *payload) {
    Serial.printf("MQTT Outgoing on %s: ", topic);
    Serial.println(payload);

    bool result = mqtt_client.publish(topic, payload, false);

    if (!result) {
        Serial.printf("MQTT publish to topic %s failed\n", topic);
    }
}

// * Reconnect to MQTT server and subscribe to in and out topics
bool mqtt_reconnect() {
    // * Loop until we're reconnected
    int MQTT_RECONNECT_RETRIES = 0;

    while (!mqtt_client.connected() && MQTT_RECONNECT_RETRIES < MQTT_MAX_RECONNECT_TRIES) {
        MQTT_RECONNECT_RETRIES++;
        Serial.printf("MQTT connection attempt %d / %d ...\n", MQTT_RECONNECT_RETRIES, MQTT_MAX_RECONNECT_TRIES);

        // * Attempt to connect
        //if (mqtt_client.connect(HOSTNAME, MQTT_USER, MQTT_PASS)) {
        if (mqtt_client.connect(HOSTNAME, MQTT_USER, MQTT_PASS)) {
            Serial.println(F("MQTT connected!"));
        } else {
            Serial.print(F("MQTT Connection failed: rc="));
            Serial.println(mqtt_client.state());
            Serial.println(F(" Retrying in 5 seconds"));
            Serial.println("");

            // * Wait 5 seconds before retrying
            delay(5000);
        }
    }

    if (MQTT_RECONNECT_RETRIES >= MQTT_MAX_RECONNECT_TRIES) {
        Serial.printf("*** MQTT connection failed, giving up after %d tries ...\n", MQTT_RECONNECT_RETRIES);
        return false;
    }

    return true;
}

void send_metric(const String& name, long metric) {
    Serial.print(F("Sending metric to broker: "));
    Serial.print(name);
    Serial.print(F("="));
    Serial.println(metric);

    char output[12]; // genoeg voor long met sign + null
    ltoa(metric, output, 10);  // correct basis 10

    topic = String(MQTT_ROOT_TOPIC) + "/" + name + "/state";
    send_mqtt_message(topic.c_str(), output);
}

void send_data_to_broker() {
    send_metric("consumption_low_tarif", CONSUMPTION_LOW_TARIF);
    send_metric("consumption_high_tarif", CONSUMPTION_HIGH_TARIF);
    send_metric("returndelivery_low_tarif", RETURNDELIVERY_LOW_TARIF);
    send_metric("returndelivery_high_tarif", RETURNDELIVERY_HIGH_TARIF);
    send_metric("actual_consumption", ACTUAL_CONSUMPTION);
    send_metric("actual_returndelivery", ACTUAL_RETURNDELIVERY);

    send_metric("l1_instant_power_usage", L1_INSTANT_POWER_USAGE);
    send_metric("l2_instant_power_usage", L2_INSTANT_POWER_USAGE);
    send_metric("l3_instant_power_usage", L3_INSTANT_POWER_USAGE);
    send_metric("l1_instant_power_current", L1_INSTANT_POWER_CURRENT);
    send_metric("l2_instant_power_current", L2_INSTANT_POWER_CURRENT);
    send_metric("l3_instant_power_current", L3_INSTANT_POWER_CURRENT);
    send_metric("l1_voltage", L1_VOLTAGE);
    send_metric("l2_voltage", L2_VOLTAGE);
    send_metric("l3_voltage", L3_VOLTAGE);
    
    send_metric("gas_meter_m3", GAS_METER_M3);

    send_metric("actual_tarif_group", ACTUAL_TARIF);
    send_metric("short_power_outages", SHORT_POWER_OUTAGES);
    send_metric("long_power_outages", LONG_POWER_OUTAGES);
    send_metric("short_power_drops", SHORT_POWER_DROPS);
    send_metric("short_power_peaks", SHORT_POWER_PEAKS);
}

// **********************************
// * P1                             *
// **********************************
unsigned int CRC16(unsigned int crc, unsigned char *buf, int len) {
	for (int pos = 0; pos < len; pos++) {
		crc ^= (unsigned int)buf[pos];    // * XOR byte into least sig. byte of crc
                                          // * Loop over each bit
        for (int i = 8; i != 0; i--) {
            // * If the LSB is set
            if ((crc & 0x0001) != 0) {
                // * Shift right and XOR 0xA001
                crc >>= 1;
				        crc ^= 0xA001;
			      }
            // * Else LSB is not set
            else {
                // * Just shift right
                crc >>= 1;
            }
        }
	}
	return crc;
}

bool isNumber(const char *res, int len) {
    bool decimalPointFound = false;

    for (int i = 0; i < len && res[i] != '\0'; i++) {
        if (res[i] == '.') {
            if (decimalPointFound) return false; // meer dan één punt → ongeldig
            decimalPointFound = true;
        } else if (res[i] < '0' || res[i] > '9') {
            return false;
        }
    }

    return len > 0;
}

int FindCharInArrayRev(char array[], char c, int len) {
    for (int i = len - 1; i >= 0; i--) {
        if (array[i] == c)
            return i;
    }
    return -1;
}

long getValue(char *buffer, int maxlen, char startchar, char endchar) {
    int s = FindCharInArrayRev(buffer, startchar, maxlen - 2);
    int e = FindCharInArrayRev(buffer, endchar, maxlen - 2);

    if (s < 0 || e < 0 || e <= s) {
        return 0;
    }

    int l = e - s - 1;
    if (l <= 0 || l >= 16) {
        return 0;
    }

    char res[16];
    strncpy(res, buffer + s + 1, l);
    res[l] = '\0';  // altijd null-termineren!

    if (!isNumber(res, l)) {
        return 0;
    }

    double value = atof(res);

    if (endchar == '*') {
        // lazy convert float to long (x1000)
        return (long)(value * 1000);
    } else if (endchar == ')') {
        return (long)value;
    }

    return 0;
}

bool decode_telegram(int len) {
    int startChar = FindCharInArrayRev(telegram, '/', len);
    int endChar = FindCharInArrayRev(telegram, '!', len);
    bool validCRCFound = false;

    for (int cnt = 0; cnt < len; cnt++) {
        Serial.print(telegram[cnt]);
    }
    Serial.print("\n");

    if (startChar >= 0) {
        currentCRC = CRC16(0x0000, (unsigned char*)telegram + startChar, len - startChar);
    } else if (endChar >= 0) {
        currentCRC = CRC16(currentCRC, (unsigned char*)telegram + endChar, 1);

        char messageCRC[5] = {0};

        if (endChar + 5 <= len) {
            strncpy(messageCRC, telegram + endChar + 1, 4);
            messageCRC[4] = 0;
            validCRCFound = (strtol(messageCRC, NULL, 16) == currentCRC);
        }

        Serial.println(validCRCFound ? F("CRC Valid!") : F("CRC Invalid!"));
        currentCRC = 0;
    } else {
        currentCRC = CRC16(currentCRC, (unsigned char*)telegram, len);
    }

    struct TelegramField {
        const char* key;
        long* target;
        char stopChar;
    };

    const TelegramField fields[] = {
        {"1-0:1.8.1", &CONSUMPTION_LOW_TARIF, '*'},
        {"1-0:1.8.2", &CONSUMPTION_HIGH_TARIF, '*'},
        {"1-0:2.8.1", &RETURNDELIVERY_LOW_TARIF, '*'},
        {"1-0:2.8.2", &RETURNDELIVERY_HIGH_TARIF, '*'},
        {"1-0:1.7.0", &ACTUAL_CONSUMPTION, '*'},
        {"1-0:2.7.0", &ACTUAL_RETURNDELIVERY, '*'},
        {"1-0:21.7.0", &L1_INSTANT_POWER_USAGE, '*'},
        {"1-0:41.7.0", &L2_INSTANT_POWER_USAGE, '*'},
        {"1-0:61.7.0", &L3_INSTANT_POWER_USAGE, '*'},
        {"1-0:31.7.0", &L1_INSTANT_POWER_CURRENT, '*'},
        {"1-0:51.7.0", &L2_INSTANT_POWER_CURRENT, '*'},
        {"1-0:71.7.0", &L3_INSTANT_POWER_CURRENT, '*'},
        {"1-0:32.7.0", &L1_VOLTAGE, '*'},
        {"1-0:52.7.0", &L2_VOLTAGE, '*'},
        {"1-0:72.7.0", &L3_VOLTAGE, '*'},
        {"0-1:24.2.1", &GAS_METER_M3, '*'},
        {"0-0:96.14.0", &ACTUAL_TARIF, ')'},
        {"0-0:96.7.21", &SHORT_POWER_OUTAGES, ')'},
        {"0-0:96.7.9", &LONG_POWER_OUTAGES, ')'},
        {"1-0:32.32.0", &SHORT_POWER_DROPS, ')'},
        {"1-0:32.36.0", &SHORT_POWER_PEAKS, ')'},
    };

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (strncmp(telegram, fields[i].key, strlen(fields[i].key)) == 0) {
            *fields[i].target = getValue(telegram, len, '(', fields[i].stopChar);
        }
    }

    return validCRCFound;
}

void read_p1_hardwareserial() {
    if (Serial.available()) {
        memset(telegram, 0, sizeof(telegram));

        while (Serial.available()) {
            ESP.wdtDisable();
            int len = Serial.readBytesUntil('\n', telegram, P1_MAXLINELENGTH);
            ESP.wdtEnable(1);

            processLine(len);
        }
    }
}

void processLine(int len) {
    telegram[len] = '\n';
    telegram[len + 1] = 0;
    yield();

    bool result = decode_telegram(len + 1);
    if (result) {
        send_data_to_broker();
        LAST_UPDATE_SENT = millis();
    }
}

// **********************************
// * EEPROM helpers                 *
// **********************************
String read_eeprom(int offset, int len) {
    Serial.print(F("read_eeprom()"));

    String res = "";
    for (int i = 0; i < len; ++i) {
        res += char(EEPROM.read(i + offset));
    }
    return res;
}

void write_eeprom(int offset, int len, String value) {
    Serial.println(F("write_eeprom()"));
    for (int i = 0; i < len; ++i) {
        if ((unsigned)i < value.length()) {
            EEPROM.write(i + offset, value[i]);
        } else {
            EEPROM.write(i + offset, 0);
        }
    }
}

// ******************************************
// * Callback for saving WIFI config        *
// ******************************************

bool shouldSaveConfig = false;

// * Callback notifying us of the need to save config
void save_wifi_config_callback () {
    Serial.println(F("Should save config"));
    shouldSaveConfig = true;
}

// **********************************
// * Setup OTA                      *
// **********************************
void setup_ota() {
    // Configure OTA settings
    ArduinoOTA.setPort(8266);
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    // Setup OTA callbacks
    ArduinoOTA.onStart([]() {
        Serial.println(F("Arduino OTA: Start"));
    });

    ArduinoOTA.onEnd([]() {
        Serial.println(F("Arduino OTA: End (Running reboot)"));
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Arduino OTA Progress: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Arduino OTA Error[%u]: ", error);
        switch (error) {
            case OTA_AUTH_ERROR:    Serial.println(F("Auth Failed"));    break;
            case OTA_BEGIN_ERROR:   Serial.println(F("Begin Failed"));   break;
            case OTA_CONNECT_ERROR: Serial.println(F("Connect Failed")); break;
            case OTA_RECEIVE_ERROR: Serial.println(F("Receive Failed")); break;
            case OTA_END_ERROR:     Serial.println(F("End Failed"));     break;
        }
    });

    ArduinoOTA.begin();
    Serial.println(F("Arduino OTA activated."));
}

// **********************************
// * Setup MDNS discovery service   *
// **********************************
void setup_mdns() {
    Serial.println(F("Starting MDNS responder service"));

    bool mdns_result = MDNS.begin(HOSTNAME);
    if (mdns_result) {
        MDNS.addService("http", "tcp", 80);
    }
}

// **********************************
// * Setup Main                     *
// **********************************
void setup() {
    // * Configure EEPROM
    EEPROM.begin(512);

    // Setup a hw serial connection for communication with the P1 meter and logging (not using inversion)
    Serial.begin(BAUD_RATE, SERIAL_8N1, SERIAL_FULL);
    Serial.setRxBufferSize(1024);
    Serial.println("");
    Serial.println("Swapping UART0 RX to inverted");
    Serial.flush();

    // Invert the RX serialport by setting a register value, this way the TX might continue normally allowing the serial monitor to read println's
    USC0(UART0) = USC0(UART0) | BIT(UCRXI);
    Serial.println("Serial port is ready to recieve.");

    // * Set led pin as output
    pinMode(LED_BUILTIN, OUTPUT);

    // * Start ticker with 0.5 because we start in AP mode and try to connect
    ticker.attach(0.6, tick);

    // * Get MQTT Server settings
    String settings_available = read_eeprom(166, 1);

    if (settings_available == "1") {
        read_eeprom(0, 64).toCharArray(MQTT_HOST, 64);   // * 0-63
        read_eeprom(64, 6).toCharArray(MQTT_PORT, 6);    // * 64-69
        read_eeprom(70, 32).toCharArray(MQTT_USER, 32);  // * 70-101
        read_eeprom(102, 32).toCharArray(MQTT_PASS, 32); // * 102-133
        read_eeprom(134, 32).toCharArray(MQTT_ROOT_TOPIC, 32); // * 134-165
        //read_eeprom(167, 5).toCharArray(HA_DISCOVERY, 5); // * 167-171
        HA_DISCOVERY = EEPROM.read(167);
        
    }

    WiFiManagerParameter CUSTOM_MQTT_HOST("host", "MQTT hostname", MQTT_HOST, 64);
    WiFiManagerParameter CUSTOM_MQTT_PORT("port", "MQTT port",     MQTT_PORT, 6);
    WiFiManagerParameter CUSTOM_MQTT_USER("user", "MQTT user",     MQTT_USER, 32);
    WiFiManagerParameter CUSTOM_MQTT_PASS("pass", "MQTT pass",     MQTT_PASS, 32);
    WiFiManagerParameter CUSTOM_MQTT_ROOT_TOPIC("topic", "MQTT root topic",     MQTT_ROOT_TOPIC, 32);
    WiFiManagerParameter CUSTOM_HA_DISCOVERY("had", "Home Assistant discovery?", HA_DISCOVERY ? "true" : "false", 5);

    // * WiFiManager local initialization. Once its business is done, there is no need to keep it around
    WiFiManager wifiManager;

    // * Reset settings - uncomment for testing
    //wifiManager.resetSettings();

    // * Set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
    wifiManager.setAPCallback(configModeCallback);

    // * Set timeout
    wifiManager.setConfigPortalTimeout(WIFI_TIMEOUT);

    // * Set save config callback
    wifiManager.setSaveConfigCallback(save_wifi_config_callback);

    // * Add all your parameters here
    wifiManager.addParameter(&CUSTOM_MQTT_HOST);
    wifiManager.addParameter(&CUSTOM_MQTT_PORT);
    wifiManager.addParameter(&CUSTOM_MQTT_USER);
    wifiManager.addParameter(&CUSTOM_MQTT_PASS);
    wifiManager.addParameter(&CUSTOM_MQTT_ROOT_TOPIC);
    wifiManager.addParameter(&CUSTOM_HA_DISCOVERY);

    // * Fetches SSID and pass and tries to connect
    // * Reset when no connection after 10 seconds
    if (!wifiManager.autoConnect()) {
        Serial.println(F("Failed to connect to WIFI and hit timeout"));

        // * Reset and try again, or maybe put it to deep sleep
        //ESP.reset();
        ESP.restart();
        delay(WIFI_TIMEOUT);
    }

    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);

    // * Read updated parameters
    strcpy(MQTT_HOST, CUSTOM_MQTT_HOST.getValue());
    strcpy(MQTT_PORT, CUSTOM_MQTT_PORT.getValue());
    strcpy(MQTT_USER, CUSTOM_MQTT_USER.getValue());
    strcpy(MQTT_PASS, CUSTOM_MQTT_PASS.getValue());
    strcpy(MQTT_ROOT_TOPIC, CUSTOM_MQTT_ROOT_TOPIC.getValue());
    //strcpy(HA_DISCOVERY, CUSTOM_HA_DISCOVERY.getValue());
    HA_DISCOVERY = strcmp(CUSTOM_HA_DISCOVERY.getValue(), "true") == 0;

    // * Save the custom parameters to FS
    if (shouldSaveConfig) {
        Serial.println(F("Saving WiFiManager config"));

        write_eeprom(0, 64, MQTT_HOST);   // * 0-63
        write_eeprom(64, 6, MQTT_PORT);   // * 64-69
        write_eeprom(70, 32, MQTT_USER);  // * 70-101
        write_eeprom(102, 32, MQTT_PASS); // * 102-133
        write_eeprom(134, 32, MQTT_ROOT_TOPIC); // * 134-165
        write_eeprom(166, 1, "1");        // * 166 --> always "1"
        //write_eeprom(167, 5, HA_DISCOVERY);        // * 167-171
        EEPROM.write(167, HA_DISCOVERY);
        EEPROM.commit();
    }

    // * If you get here you have connected to the WiFi
    Serial.println(F("Connected to WIFI..."));

    // * Keep LED on
    ticker.detach();
    digitalWrite(LED_BUILTIN, LOW);

    // * Configure OTA
    setup_ota();

    // * Startup MDNS Service
    setup_mdns();

    // * Setup MQTT
    Serial.printf("MQTT connecting to: %s:%s\n", MQTT_HOST, MQTT_PORT);
    Serial.printf("The settings are : %d\n", read_eeprom(166, 1));

    mqtt_client.setServer(MQTT_HOST, atoi(MQTT_PORT));
    mqtt_client.setBufferSize(700);
	
    // * Setup API
    // routing
    httpRestServer.on("/", HTTP_GET, handleRoot);
    httpRestServer.on("/restart", HTTP_GET, handleRestart);
    httpRestServer.on("/haon", HTTP_GET, handleHaon);
    httpRestServer.on("/haoff", HTTP_GET, handleHaoff);
    httpRestServer.on("/mqtt", HTTP_GET, handleMQTTSettings);
    
    // Startup API
    httpRestServer.begin();
    Serial.println("HTTP server started");

}

// **********************************
// * Loop                           *
// **********************************
void loop() {
    ArduinoOTA.handle();
    
    const unsigned long now = millis();
    static unsigned long lastWifiReconnectAttempt = 0;
    static constexpr unsigned long WIFI_RECONNECT_INTERVAL = 15UL * 60UL * 1000UL; // 15 minuten
    static constexpr unsigned long MQTT_RECONNECT_INTERVAL = 5000;

    const bool wifiConnected = (WiFi.status() == WL_CONNECTED);

    // --- WiFi Herverbinden ---
    if (!wifiConnected) {
        Serial.println("WiFi not connected.");

        if (now - lastWifiReconnectAttempt > WIFI_RECONNECT_INTERVAL) {
            Serial.println("Attempting to reconnect to WiFi...");
            lastWifiReconnectAttempt = now;

            WiFi.disconnect();  // Force clean reconnect
            WiFi.begin();       // Uses stored credentials
        }

        return; // Stop verder uitvoeren tot WiFi weer terug is
    }

    // Reset WiFi reconnect timer zodra verbinding weer werkt
    lastWifiReconnectAttempt = now;

    // --- MQTT Herverbinden ---
    if (!mqtt_client.connected()) {
        if (now - LAST_RECONNECT_ATTEMPT > MQTT_RECONNECT_INTERVAL) {
            LAST_RECONNECT_ATTEMPT = now;

            Serial.println("Attempting MQTT reconnect...");

            if (mqtt_reconnect()) {
                LAST_RECONNECT_ATTEMPT = 0;

                if (HA_DISCOVERY && !haDiscoverySetupRun) {
                    haDiscovery("on");
                    haDiscoverySetupRun = true;
                }
            }
        }
    } else {
        mqtt_client.loop();  // Houd verbinding actief
    }

    if (now - LAST_UPDATE_SENT > UPDATE_INTERVAL) {
        read_p1_hardwareserial();
    }

    if(setRestart && now - restartInitiated >= restartInterval) {
        ESP.restart();
    }

    if(setHaDOn) {
        handleHaDiscoveryOn();
        setHaDOn = false;
    }

    if(setHaDOff) {
        handleHaDiscoveryOff();
        setHaDOff = false;
    }
}
