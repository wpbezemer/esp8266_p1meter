#include <FS.h>
#include <EEPROM.h>
#include <DNSServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
#include <Ticker.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

//for reboot api
#define WEBSERVER_H
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>

// * Debug mode — zet op 1 om Serial output te activeren, 0 voor productie
#define DEBUG 0
#if DEBUG
  #define DEBUG_PRINT(x)    Serial.print(x)
  #define DEBUG_PRINTLN(x)  Serial.println(x)
  #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(...)
#endif

// * Include settings
#include "settings.h"
#include "variables.h"
#include "webui.h"

// * Forward declarations
void tick();
String read_eeprom(int offset, int len);
void write_eeprom(int offset, int len, String value);
bool validateTelegram(int len);
bool decode_telegram(int len);
void writeTelegramJson(Print &out);
void haDiscovery(const String& setHomeAssistant);
void handleHaDiscoveryOn();
void handleHaDiscoveryOff();
void send_mqtt_message(const char *topic, const char *payload);
void send_metric(const String& name, const String& metric);
void send_data_to_broker();
void read_p1_hardwareserial();
void save_wifi_config_callback();
void setup_ota();
void setup_mdns();
void setup_webserver();
bool isNewerVersion(const char* a, const char* b);
bool checkGitHubVersion();
void publishUpdateStatus();
void performGitHubUpdate();
void scheduleRestart(unsigned long delayMs);

// * EEPROM address map
#define EEPROM_ADDR_MQTT_HOST         0    // 0-63
#define EEPROM_ADDR_MQTT_PORT        64    // 64-69
#define EEPROM_ADDR_MQTT_USER        70    // 70-101
#define EEPROM_ADDR_MQTT_PASS       102    // 102-133
#define EEPROM_ADDR_MQTT_TOPIC      134    // 134-165
#define EEPROM_ADDR_SETTINGS_FLAG   166    // 166
#define EEPROM_ADDR_HA_DISCOVERY    167    // 167 (1 byte, 0/1)
#define EEPROM_ADDR_UPDATE_INTERVAL 168    // 168-171 (4 bytes, unsigned long)
#define EEPROM_ADDR_OTA_CHECK_HOURS 172    // 172 (1 byte, 0=uit, 1-24 uur)
#define EEPROM_ADDR_AUTO_UPDATE     173    // 173 (1 byte, bool)

// * Initiate led blinker library
Ticker ticker;

// * Runtime update interval (milliseconds), default = UPDATE_INTERVAL from variables.h
unsigned long updateInterval = UPDATE_INTERVAL;

// * Een telegram mag tot zoveel ms "te vroeg" komen en toch verstuurd worden.
//   De meter stuurt ~elke 1000 ms, met jitter. Zonder marge wordt bij interval=1s
//   regelmatig een telegram overgeslagen (effectief 2s).
#define INTERVAL_TOLERANCE 500UL

// * Delta detection — previous values to compare against
#define METRICS_COUNT (sizeof(metricsFields) / sizeof(metricsFields[0]))
static long     prevLongValues[METRICS_COUNT];
static char     prevStringValues[METRICS_COUNT][64];
static bool     deltaInitialized = false;

// * Forced full update every 5 minutes regardless of delta
#define FULL_UPDATE_INTERVAL 300000UL
unsigned long LAST_FULL_UPDATE = 0;

// * Vlag: P1 cyclus volledig afgerond (telegram + MQTT)
bool p1CycleComplete = false;

// * Laatste geldige telegram (snapshot, zodat webrequests nooit een half telegram zien)
char lastTelegram[FULL_TELEGRAM_SIZE];
int  lastTelegramLen = 0;

// * Statistieken voor de info pagina
struct Stats {
    uint32_t telegramsOk     = 0;
    uint32_t crcErrors       = 0;
    uint32_t mqttPublishes   = 0;
    uint32_t mqttFailures    = 0;
    uint32_t mqttReconnects  = 0;
    uint32_t wifiReconnects  = 0;
    unsigned long lastTelegramAt = 0;   // millis() van laatste geldige telegram
    unsigned long lastPublishAt  = 0;   // millis() van laatste MQTT cyclus
    uint16_t lastPublishCount    = 0;   // aantal topics in laatste cyclus
    unsigned long maxLoopUs      = 0;   // langste loop() in de laatste 10 s
} stats;

// * Uptime zonder millis() overflow na 49 dagen
uint32_t uptimeRollovers = 0;
uint32_t lastMillis      = 0;
uint64_t uptimeMs() {
    return ((uint64_t)uptimeRollovers << 32) | millis();
}

// * Acties die vanuit async webhandlers in loop() uitgevoerd moeten worden
//   (blokkerende code in een async callback laat de ESP crashen)
volatile bool doGitHubUpdate = false;
unsigned long gitHubUpdateRequestedAt = 0;
unsigned long restartDelayMs = restartInterval;
char lastUpdateError[64] = "";

// * Initiate WIFI client
WiFiClient espClient;

// * Initiate MQTT client
PubSubClient mqtt_client(espClient);

// * Initiate API settings
AsyncWebServer httpRestServer(HTTP_REST_PORT);
JsonDocument jsonDoc;

// **********************************
// * Helpers                        *
// **********************************

void scheduleRestart(unsigned long delayMs) {
    restartDelayMs   = delayMs;
    restartInitiated = millis();
    setRestart       = true;
}

// Optionele basic-auth (WEB_USER leeg = uit)
bool authOK(AsyncWebServerRequest *request) {
    if (WEB_USER[0] == '\0') return true;
    if (request->authenticate(WEB_USER, WEB_PASS)) return true;
    request->requestAuthentication();
    return false;
}

bool authSilent(AsyncWebServerRequest *request) {
    return WEB_USER[0] == '\0' || request->authenticate(WEB_USER, WEB_PASS);
}

String paramValue(AsyncWebServerRequest *request, const char *name) {
    if (request->hasParam(name, true)) return request->getParam(name, true)->value();
    if (request->hasParam(name))       return request->getParam(name)->value();
    return String();
}

bool hasAnyParam(AsyncWebServerRequest *request, const char *name) {
    return request->hasParam(name, true) || request->hasParam(name);
}

// Schrijf een string als JSON (met escaping) naar een Print
void printJsonString(Print &out, const char *s) {
    out.print('"');
    for (; *s; s++) {
        char c = *s;
        if (c == '"' || c == '\\') { out.print('\\'); out.print(c); }
        else if (c == '\r') continue;
        else if ((uint8_t)c < 0x20) out.printf("\\u%04x", c);
        else out.print(c);
    }
    out.print('"');
}

void updateIpAddress() {
    IPAddress ip = WiFi.localIP();
    snprintf(IPADDRESS, sizeof(IPADDRESS), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

// Bestandsnaam veilig maken: altijd beginnen met '/', geen '..'
bool sanitizePath(String &path) {
    if (path.length() == 0) return false;
    if (path[0] != '/') path = "/" + path;
    if (path.indexOf("..") >= 0) return false;
    if (path.length() > 64) return false;
    return true;
}

// **********************************
// * API handlers                   *
// **********************************

void handleRoot(AsyncWebServerRequest *request) {
    // Browser krijgt het dashboard, scripts (curl/HA) krijgen de oude tekst-response
    if (request->hasHeader("Accept") &&
        request->getHeader("Accept")->value().indexOf("text/html") >= 0) {
        if (!authOK(request)) return;
        request->send_P(200, "text/html", WEBUI_HTML);
        return;
    }
    request->send(200, "text/plain", "Ok - P1Meter v" APPVERSION);
}

void handlePage(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    request->send_P(200, "text/html", WEBUI_HTML);
}

void handleVersion(AsyncWebServerRequest *request) {
    request->send(200, "text/plain", APPVERSION);
}

void handleRestart(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    request->send(200, "text/plain", "Server is restarting in 5 seconds, restarting can take up to 1 minute...");
    DEBUG_PRINTLN("Server is restarting in 5 seconds...");
    scheduleRestart(restartInterval);
}

void handleHaon(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    request->send(200, "text/plain", "Ok - HA Discovery is on");
    setHaDOn = true;
}

void handleHaoff(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    request->send(200, "text/plain", "Ok - HA Discovery is off");
    setHaDOff = true;
}

void handleMQTTSettings(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    if (request->hasParam("server")) {
        strlcpy(MQTT_HOST, request->getParam("server")->value().c_str(), sizeof(MQTT_HOST));
        DEBUG_PRINTF("MQTT Server updated to: %s\n", MQTT_HOST);
        write_eeprom(EEPROM_ADDR_MQTT_HOST, 64, MQTT_HOST);
        EEPROM.commit();
        request->send(200, "text/plain", "MQTT server updated successfully!");
        scheduleRestart(restartInterval);
    } else {
        request->send(400, "text/plain", "Missing server details");
    }
}

void handleGetInterval(AsyncWebServerRequest *request) {
    String msg = "Update interval: " + String(updateInterval) + " ms (" + String(updateInterval / 1000.0, 1) + " sec)";
    request->send(200, "text/plain", msg);
}

void handleSetInterval(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    if (!request->hasParam("seconds")) {
        request->send(400, "text/plain", "Missing parameter: seconds (min: 1, max: 3600)");
        return;
    }
    String secondsParam = request->getParam("seconds")->value();
    for (size_t i = 0; i < secondsParam.length(); i++) {
        if (!isDigit(secondsParam[i])) {
            request->send(400, "text/plain", "Invalid value: '" + secondsParam + "' is not a number.");
            return;
        }
    }
    int seconds = secondsParam.toInt();
    if (seconds < 1) {
        request->send(400, "text/plain", "Invalid value: minimum is 1 second. The P1 meter sends at most 1 telegram per second.");
        return;
    }
    if (seconds > 3600) {
        request->send(400, "text/plain", "Invalid value: maximum is 3600 seconds (1 hour).");
        return;
    }
    updateInterval = (unsigned long)seconds * 1000UL;
    EEPROM.put(EEPROM_ADDR_UPDATE_INTERVAL, updateInterval);
    EEPROM.commit();
    DEBUG_PRINTF("Update interval gewijzigd naar %d seconden (%lu ms)\n", seconds, updateInterval);
    request->send(200, "text/plain", "Update interval set to " + String(seconds) + " seconds. No restart needed.");
}

// GET /telegram — laatste geldige telegram als JSON, direct uit RAM
// (voorheen elke seconde naar flash geschreven → flash slijtage)
void handleTelegramJson(AsyncWebServerRequest *request) {
    if (stats.telegramsOk == 0) {
        request->send(404, "text/plain", "nog geen geldig telegram ontvangen");
        return;
    }
    AsyncResponseStream *resp = request->beginResponseStream("application/json");
    writeTelegramJson(*resp);
    request->send(resp);
}

// GET /api/live — compacte actuele waarden voor het dashboard
void handleApiLive(AsyncWebServerRequest *request) {
    AsyncResponseStream *resp = request->beginResponseStream("application/json");
    const unsigned long now = millis();
    resp->print('{');
    resp->printf("\"age\":%ld,", stats.telegramsOk ? (long)(now - stats.lastTelegramAt) : -1L);
    resp->printf("\"ok\":%u,\"crc\":%u,", stats.telegramsOk, stats.crcErrors);
    resp->printf("\"mqtt\":%s,", mqtt_client.connected() ? "true" : "false");
    resp->printf("\"interval\":%lu,", updateInterval);
    resp->print(F("\"ts\":"));  printJsonString(*resp, DSRMTIMESTAMP);
    resp->printf(",\"tarif\":%ld", ACTUAL_TARIF_GROUP);
    resp->printf(",\"p\":%ld,\"r\":%ld", ACTUAL_CONSUMPTION, ACTUAL_RETURNDELIVERY);
    resp->printf(",\"t1\":%ld,\"t2\":%ld,\"r1\":%ld,\"r2\":%ld",
        CONSUMPTION_LOW_TARIF, CONSUMPTION_HIGH_TARIF, RETURNDELIVERY_LOW_TARIF, RETURNDELIVERY_HIGH_TARIF);
    resp->printf(",\"gas\":%ld", GAS_METER_M3);
    resp->printf(",\"ph\":[[%ld,%ld,%ld,%ld],[%ld,%ld,%ld,%ld],[%ld,%ld,%ld,%ld]]",
        L1_INSTANT_POWER_USAGE, L1_INSTANT_POWER_RETURN, L1_INSTANT_POWER_CURRENT, L1_VOLTAGE,
        L2_INSTANT_POWER_USAGE, L2_INSTANT_POWER_RETURN, L2_INSTANT_POWER_CURRENT, L2_VOLTAGE,
        L3_INSTANT_POWER_USAGE, L3_INSTANT_POWER_RETURN, L3_INSTANT_POWER_CURRENT, L3_VOLTAGE);
    resp->printf(",\"ev\":[%ld,%ld,%ld,%ld]",
        SHORT_POWER_OUTAGES, LONG_POWER_OUTAGES, SHORT_POWER_DROPS, SHORT_POWER_PEAKS);
    resp->print('}');
    request->send(resp);
}

// GET /api/info — device info en status
void handleApiInfo(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    FSInfo fsi;
    bool fsOk = LittleFS.info(fsi);
    const unsigned long now = millis();
    AsyncResponseStream *resp = request->beginResponseStream("application/json");
    resp->print('{');
    resp->print(F("\"version\":\"" APPVERSION "\""));
    resp->print(F(",\"hostname\":\"" HOSTNAME "\""));
    resp->printf(",\"uptime\":%lu", (unsigned long)(uptimeMs() / 1000ULL));
    resp->print(F(",\"reset_reason\":")); printJsonString(*resp, ESP.getResetReason().c_str());
    resp->printf(",\"chip_id\":\"%06X\"", ESP.getChipId());
    resp->printf(",\"cpu_mhz\":%u", ESP.getCpuFreqMHz());
    resp->printf(",\"flash_size\":%u", ESP.getFlashChipRealSize());
    resp->printf(",\"sketch_size\":%u,\"sketch_free\":%u", ESP.getSketchSize(), ESP.getFreeSketchSpace());
    resp->printf(",\"heap_free\":%u,\"heap_max_block\":%u,\"heap_frag\":%u",
        ESP.getFreeHeap(), ESP.getMaxFreeBlockSize(), ESP.getHeapFragmentation());
    resp->print(F(",\"core\":")); printJsonString(*resp, ESP.getCoreVersion().c_str());
    resp->print(F(",\"sdk\":"));  printJsonString(*resp, ESP.getSdkVersion());
    resp->printf(",\"fs_total\":%u,\"fs_used\":%u", fsOk ? (unsigned)fsi.totalBytes : 0, fsOk ? (unsigned)fsi.usedBytes : 0);
    resp->print(F(",\"ssid\":"));  printJsonString(*resp, WiFi.SSID().c_str());
    resp->print(F(",\"bssid\":\"")); resp->print(WiFi.BSSIDstr()); resp->print('"');
    resp->printf(",\"rssi\":%d,\"channel\":%d", WiFi.RSSI(), WiFi.channel());
    resp->printf(",\"ip\":\"%s\"", IPADDRESS);
    resp->print(F(",\"gateway\":\"")); resp->print(WiFi.gatewayIP().toString()); resp->print('"');
    resp->print(F(",\"dns\":\""));     resp->print(WiFi.dnsIP().toString());     resp->print('"');
    resp->print(F(",\"mac\":\""));     resp->print(WiFi.macAddress());           resp->print('"');
    resp->print(F(",\"mqtt_host\":")); printJsonString(*resp, MQTT_HOST);
    resp->printf(",\"mqtt_port\":\"%s\"", MQTT_PORT);
    resp->print(F(",\"mqtt_topic\":")); printJsonString(*resp, MQTT_ROOT_TOPIC);
    resp->printf(",\"mqtt_connected\":%s,\"mqtt_state\":%d", mqtt_client.connected() ? "true" : "false", mqtt_client.state());
    resp->printf(",\"mqtt_publishes\":%u,\"mqtt_failures\":%u,\"mqtt_reconnects\":%u",
        stats.mqttPublishes, stats.mqttFailures, stats.mqttReconnects);
    resp->printf(",\"last_publish_age\":%ld,\"last_publish_count\":%u",
        stats.lastPublishAt ? (long)(now - stats.lastPublishAt) : -1L, stats.lastPublishCount);
    resp->printf(",\"wifi_reconnects\":%u", stats.wifiReconnects);
    resp->printf(",\"telegrams_ok\":%u,\"crc_errors\":%u", stats.telegramsOk, stats.crcErrors);
    resp->printf(",\"last_telegram_age\":%ld", stats.telegramsOk ? (long)(now - stats.lastTelegramAt) : -1L);
    resp->printf(",\"max_loop_us\":%lu", stats.maxLoopUs);
    resp->printf(",\"interval\":%lu", updateInterval);
    resp->printf(",\"ha_discovery\":%s", HA_DISCOVERY ? "true" : "false");
    resp->print(F(",\"dsmr_version\":")); printJsonString(*resp, DSRMVERSION);
    resp->print(F(",\"meter_id\":"));     printJsonString(*resp, EQUIPMENTID);
    resp->print(F(",\"gas_id\":"));       printJsonString(*resp, GASEQUIPMENTID);
    resp->print('}');
    request->send(resp);
}

// GET /api/settings — huidige instellingen (zonder wachtwoord)
void handleApiSettingsGet(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    AsyncResponseStream *resp = request->beginResponseStream("application/json");
    resp->print('{');
    resp->printf("\"interval\":%lu", updateInterval / 1000UL);
    resp->printf(",\"ha_discovery\":%s", HA_DISCOVERY ? "true" : "false");
    resp->printf(",\"auto_update\":%s", AUTO_UPDATE ? "true" : "false");
    resp->printf(",\"ota_hours\":%lu", otaCheckInterval / 3600000UL);
    resp->print(F(",\"mqtt_host\":"));  printJsonString(*resp, MQTT_HOST);
    resp->print(F(",\"mqtt_port\":"));  printJsonString(*resp, MQTT_PORT);
    resp->print(F(",\"mqtt_user\":"));  printJsonString(*resp, MQTT_USER);
    resp->printf(",\"mqtt_pass_set\":%s", MQTT_PASS[0] ? "true" : "false");
    resp->print(F(",\"mqtt_topic\":")); printJsonString(*resp, MQTT_ROOT_TOPIC);
    resp->print('}');
    request->send(resp);
}

// POST /api/settings — alleen meegestuurde velden worden aangepast
void handleApiSettingsPost(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    bool needRestart = false;

    if (hasAnyParam(request, "interval")) {
        long s = paramValue(request, "interval").toInt();
        if (s < 1 || s > 3600) { request->send(400, "application/json", "{\"ok\":false,\"error\":\"interval moet 1-3600 zijn\"}"); return; }
        updateInterval = (unsigned long)s * 1000UL;
        EEPROM.put(EEPROM_ADDR_UPDATE_INTERVAL, updateInterval);
    }
    if (hasAnyParam(request, "ota_hours")) {
        long h = paramValue(request, "ota_hours").toInt();
        if (h < 0 || h > 24) { request->send(400, "application/json", "{\"ok\":false,\"error\":\"versiecheck moet 0-24 uur zijn\"}"); return; }
        otaCheckInterval = (unsigned long)h * 3600000UL;
        EEPROM.write(EEPROM_ADDR_OTA_CHECK_HOURS, (uint8_t)h);
    }
    if (hasAnyParam(request, "auto_update")) {
        AUTO_UPDATE = paramValue(request, "auto_update") == "1";
        EEPROM.write(EEPROM_ADDR_AUTO_UPDATE, AUTO_UPDATE ? 1 : 0);
    }
    if (hasAnyParam(request, "ha_discovery")) {
        bool on = paramValue(request, "ha_discovery") == "1";
        if (on != HA_DISCOVERY) { if (on) setHaDOn = true; else setHaDOff = true; }
    }

    struct { const char *param; char *target; size_t size; int addr; } mqttFields[] = {
        {"mqtt_host",  MQTT_HOST,       sizeof(MQTT_HOST),       EEPROM_ADDR_MQTT_HOST},
        {"mqtt_port",  MQTT_PORT,       sizeof(MQTT_PORT),       EEPROM_ADDR_MQTT_PORT},
        {"mqtt_user",  MQTT_USER,       sizeof(MQTT_USER),       EEPROM_ADDR_MQTT_USER},
        {"mqtt_pass",  MQTT_PASS,       sizeof(MQTT_PASS),       EEPROM_ADDR_MQTT_PASS},
        {"mqtt_topic", MQTT_ROOT_TOPIC, sizeof(MQTT_ROOT_TOPIC), EEPROM_ADDR_MQTT_TOPIC},
    };
    for (auto &f : mqttFields) {
        if (!hasAnyParam(request, f.param)) continue;
        String v = paramValue(request, f.param);
        // Leeg wachtwoord-veld = ongewijzigd laten (tenzij expliciet gewist)
        if (strcmp(f.param, "mqtt_pass") == 0 && v.length() == 0 && !hasAnyParam(request, "mqtt_pass_clear")) continue;
        if (v.length() >= f.size) {
            request->send(400, "application/json", String("{\"ok\":false,\"error\":\"") + f.param + " te lang\"}");
            return;
        }
        if (strcmp(v.c_str(), f.target) == 0) continue;
        strlcpy(f.target, v.c_str(), f.size);
        write_eeprom(f.addr, f.size, f.target);
        needRestart = true;
    }
    if (needRestart) write_eeprom(EEPROM_ADDR_SETTINGS_FLAG, 1, "1");

    EEPROM.commit();
    if (needRestart) scheduleRestart(2000);
    request->send(200, "application/json",
        needRestart ? "{\"ok\":true,\"restart\":true}" : "{\"ok\":true,\"restart\":false}");
}

// **********************************
// * File server (LittleFS)         *
// **********************************

void listDir(Print &out, const String &dirPath, bool &first) {
    Dir dir = LittleFS.openDir(dirPath);
    while (dir.next()) {
        String full = dirPath + (dirPath.endsWith("/") ? "" : "/") + dir.fileName();
        if (dir.isDirectory()) {
            listDir(out, full, first);
            continue;
        }
        if (!first) out.print(',');
        first = false;
        out.print(F("{\"name\":")); printJsonString(out, full.c_str());
        out.printf(",\"size\":%u}", (unsigned)dir.fileSize());
    }
}

void handleFsList(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    FSInfo fsi;
    bool fsOk = LittleFS.info(fsi);
    AsyncResponseStream *resp = request->beginResponseStream("application/json");
    resp->printf("{\"total\":%u,\"used\":%u,\"files\":[",
        fsOk ? (unsigned)fsi.totalBytes : 0, fsOk ? (unsigned)fsi.usedBytes : 0);
    bool first = true;
    listDir(*resp, "/", first);
    resp->print("]}");
    request->send(resp);
}

void handleFsDownload(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    String path = paramValue(request, "path");
    if (!sanitizePath(path) || !LittleFS.exists(path)) {
        request->send(404, "text/plain", "bestand niet gevonden");
        return;
    }
    bool download = hasAnyParam(request, "download");
    request->send(LittleFS, path, String(), download);
}

void handleFsDelete(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    String path = paramValue(request, "path");
    if (!sanitizePath(path) || !LittleFS.exists(path)) {
        request->send(404, "application/json", "{\"ok\":false,\"error\":\"bestand niet gevonden\"}");
        return;
    }
    bool ok = LittleFS.remove(path);
    request->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

File fsUploadFile;
bool fsUploadFailed = false;

void handleFsUploadChunk(AsyncWebServerRequest *request, String filename,
                         size_t index, uint8_t *data, size_t len, bool final) {
    if (index == 0) {
        fsUploadFailed = false;
        if (!authSilent(request)) { fsUploadFailed = true; return; }
        String path = filename;
        if (!sanitizePath(path)) { fsUploadFailed = true; return; }
        if (fsUploadFile) fsUploadFile.close();
        fsUploadFile = LittleFS.open(path, "w");
        if (!fsUploadFile) { fsUploadFailed = true; return; }
        DEBUG_PRINTF("FS upload start: %s\n", path.c_str());
    }
    if (fsUploadFailed || !fsUploadFile) return;
    if (len && fsUploadFile.write(data, len) != len) {
        fsUploadFailed = true;   // waarschijnlijk vol
    }
    if (final) {
        fsUploadFile.close();
        DEBUG_PRINTF("FS upload klaar: %u bytes\n", (unsigned)(index + len));
    }
}

void handleFsUploadDone(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    if (fsUploadFailed)
        request->send(500, "application/json", "{\"ok\":false,\"error\":\"upload mislukt (naam te lang of geen ruimte?)\"}");
    else
        request->send(200, "application/json", "{\"ok\":true}");
}

// **********************************
// * OTA Update                     *
// **********************************

// Vergelijk twee versiestrings semantisch (major.minor.patch)
// Geeft true terug als b NIEUWER is dan a
bool isNewerVersion(const char* a, const char* b) {
    int a1 = 0, a2 = 0, a3 = 0;
    int b1 = 0, b2 = 0, b3 = 0;
    sscanf(a, "%d.%d.%d", &a1, &a2, &a3);
    sscanf(b, "%d.%d.%d", &b1, &b2, &b3);
    if (b1 != a1) return b1 > a1;
    if (b2 != a2) return b2 > a2;
    return b3 > a3;
}

bool checkGitHubVersion() {
    // Gebruik plain WiFiClient (geen SSL) — spaart ~20KB heap
    // versiecheck haalt alleen een klein JSON bestand op, geen gevoelige data
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(5000);

    if (!http.begin(client, GITHUB_VERSION_HOST, 80, GITHUB_VERSION_PATH)) {
        DEBUG_PRINTLN(F("Versiecheck: HTTP begin mislukt"));
        http.end();
        return false;
    }

    int code = http.GET();
    DEBUG_PRINTF("Versiecheck HTTP code: %d\n", code);

    if (code != 200) {
        DEBUG_PRINTF("Versiecheck mislukt: HTTP %d\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok) {
        DEBUG_PRINTLN(F("Versiecheck: JSON parse fout"));
        return false;
    }

    const char* latest = doc["version"];
    if (!latest || latest[0] == '\0') return false;

    strlcpy(LATEST_VERSION, latest, sizeof(LATEST_VERSION));
    UPDATE_AVAILABLE = isNewerVersion(APPVERSION, LATEST_VERSION);

    DEBUG_PRINTF("Versiecheck: huidig=%s latest=%s update=%s\n",
        APPVERSION, LATEST_VERSION, UPDATE_AVAILABLE ? "JA" : "nee");
    return true;
}

// Publiceer update status naar MQTT
void publishUpdateStatus() {
    String topicAvail = String(MQTT_ROOT_TOPIC) + "/update_available/state";
    String topicVer   = String(MQTT_ROOT_TOPIC) + "/latest_version/state";
    mqtt_client.publish(topicAvail.c_str(), UPDATE_AVAILABLE ? "true" : "false", true);
    mqtt_client.publish(topicVer.c_str(),   LATEST_VERSION[0] ? LATEST_VERSION : APPVERSION, true);
}

// Voer de GitHub OTA update uit (alleen vanuit loop() aanroepen!)
void performGitHubUpdate() {
    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
    client->setInsecure();
    ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);
    ESPhttpUpdate.rebootOnUpdate(true);
    ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GitHub releases redirecten
    DEBUG_PRINTLN(F("GitHub OTA update starten..."));
    t_httpUpdate_return ret = ESPhttpUpdate.update(*client, GITHUB_FIRMWARE_URL);
    switch (ret) {
        case HTTP_UPDATE_FAILED:
            strlcpy(lastUpdateError, ESPhttpUpdate.getLastErrorString().c_str(), sizeof(lastUpdateError));
            DEBUG_PRINTF("Update mislukt: %s\n", lastUpdateError);
            break;
        case HTTP_UPDATE_NO_UPDATES:
            strlcpy(lastUpdateError, "geen update gevonden", sizeof(lastUpdateError));
            DEBUG_PRINTLN(F("Geen update gevonden op GitHub"));
            break;
        case HTTP_UPDATE_OK:
            DEBUG_PRINTLN(F("Update geslaagd!"));
            break;
    }
}

// GET /update/settings - sla auto-update instelling op (compatibel met v2.7 formulier)
void handleUpdateSettings(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    AUTO_UPDATE = request->hasParam("autoupdate");
    EEPROM.write(EEPROM_ADDR_AUTO_UPDATE, AUTO_UPDATE ? 1 : 0);
    EEPROM.commit();
    DEBUG_PRINTF("Auto-update: %s\n", AUTO_UPDATE ? "aan" : "uit");
    AsyncWebServerResponse *resp = request->beginResponse(302);
    resp->addHeader("Location", "/update");
    request->send(resp);
}

// GET /update/github — vraag loop() om te downloaden en flashen
void handleUpdateGitHub(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    lastUpdateError[0] = '\0';
    gitHubUpdateRequestedAt = millis();
    doGitHubUpdate = true;
    request->send(200, "text/plain", "GitHub update gestart. Dit duurt 30-60 seconden, daarna herstart de ESP.");
}

// POST /update/upload — bin bestand upload (chunks)
bool otaUploadFailed = false;

void handleUpdateUpload(AsyncWebServerRequest *request, String filename,
                        size_t index, uint8_t *data, size_t len, bool final) {
    if (index == 0) {
        otaUploadFailed = false;
        if (!authSilent(request)) { otaUploadFailed = true; return; }
        DEBUG_PRINTF("OTA upload start: %s\n", filename.c_str());
        Update.runAsync(true);
        if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
            otaUploadFailed = true;
            return;
        }
    }
    if (otaUploadFailed) return;
    if (Update.isRunning() && !Update.hasError()) {
        if (Update.write(data, len) != len) otaUploadFailed = true;
    }
    if (final) {
        if (!Update.end(true)) otaUploadFailed = true;
        DEBUG_PRINTF("OTA upload %s: %u bytes\n", otaUploadFailed ? "mislukt" : "geslaagd", (unsigned)(index + len));
    }
}

void handleUpdateUploadDone(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    if (otaUploadFailed || Update.hasError()) {
        request->send(500, "text/plain", "Update mislukt: fout " + String(Update.getError()));
        return;
    }
    request->send(200, "text/plain", "Update geslaagd! De ESP herstart nu met de nieuwe firmware.");
    scheduleRestart(1000);
}

// GET /update/check - geeft gecachede versiecheck waarden terug (geen live check)
void handleUpdateCheck(AsyncWebServerRequest *request) {
    String json = "{";
    json += F("\"success\":true");
    json += F(",\"current\":\"");
    json += APPVERSION;
    json += F("\",\"latest\":\"");
    json += LATEST_VERSION[0] ? LATEST_VERSION : "not_checked_yet";
    json += F("\",\"update_available\":");
    json += UPDATE_AVAILABLE ? F("true") : F("false");
    json += F(",\"auto_update\":");
    json += AUTO_UPDATE ? F("true") : F("false");
    json += F(",\"updating\":");
    json += doGitHubUpdate ? F("true") : F("false");
    json += F(",\"last_error\":\"");
    json += lastUpdateError;
    json += F("\",\"free_heap\":");
    json += ESP.getFreeHeap();
    json += F("}");
    request->send(200, "application/json", json);
}

void handleHaDiscoveryOn() {
    if (!HA_DISCOVERY) {
        EEPROM.write(EEPROM_ADDR_HA_DISCOVERY, 1);
        EEPROM.commit();
        HA_DISCOVERY = true;
    }
    if (mqtt_client.connected()) {
        haDiscovery("on");
        haDiscoverySetupRun = true;
    }
    DEBUG_PRINTLN("Home Assistant discovery is on...");
}

void handleHaDiscoveryOff() {
    if (HA_DISCOVERY) {
        EEPROM.write(EEPROM_ADDR_HA_DISCOVERY, 0);
        EEPROM.commit();
        HA_DISCOVERY = false;
    }
    if (mqtt_client.connected()) haDiscovery("off");
    haDiscoverySetupRun = false;
    DEBUG_PRINTLN("Home Assistant discovery is off...");
}

SensorConfig getSensorConfig(const String& deviceclass) {
    for (auto& cfg : sensorConfigs) {
        if (deviceclass.equalsIgnoreCase(cfg.input_name)) return cfg;
    }
    return { "unknown", "none", "none", "none", "mdi:help-circle", false };
}

void sendHomeAssistantConfig(const char *sensorname, const char *sensorid,
                             const String& deviceclass, const String& setHomeAssistant) {
    topic = String(MQTT_ROOT_TOPIC) + "/" + String(sensorid) + "/config";
    if (setHomeAssistant == "on") {
        jsonDoc.clear();
        jsonDoc["platform"]    = "mqtt";
        jsonDoc["unique_id"]   = "p1_" + String(sensorid);
        jsonDoc["name"]        = String(sensorname);
        jsonDoc["state_topic"] = String(MQTT_ROOT_TOPIC) + "/" + String(sensorid) + "/state";
        jsonDoc["device"]["identifiers"][0]      = "p1meter1337";
        jsonDoc["device"]["name"]                = "P1 Meter";
        jsonDoc["device"]["model"]               = "P1 Meter with HA and API integration";
        jsonDoc["device"]["manufacturer"]        = "tedzor";
        jsonDoc["device"]["sw_version"]          = APPVERSION;
        jsonDoc["device"]["configuration_url"]   = String("http://") + IPADDRESS + "/";
        SensorConfig cfg = getSensorConfig(deviceclass);
        if (strcmp(cfg.ha_device_class, "none") != 0) jsonDoc["device_class"]       = cfg.ha_device_class;
        if (strcmp(cfg.state_class,     "none") != 0) jsonDoc["state_class"]         = cfg.state_class;
        if (strcmp(cfg.unit,            "none") != 0) jsonDoc["unit_of_measurement"] = cfg.unit;
        jsonDoc["icon"] = cfg.icon;
        jsonDoc["value_template"] = cfg.divide_by_1000 ? "{{ value|float / 1000 }}" : "{{ value }}";
        String jsonString;
        serializeJson(jsonDoc, jsonString);
        if (!mqtt_client.publish(topic.c_str(), jsonString.c_str(), true))
            DEBUG_PRINTF("MQTT publish to topic %s failed\n", topic.c_str());
    } else if (setHomeAssistant == "off") {
        if (!mqtt_client.publish(topic.c_str(), "", true))
            DEBUG_PRINTF("MQTT publish off topic %s failed\n", topic.c_str());
    }
    yield();
}

void haDiscovery(const String& setHomeAssistant) {
    sendHomeAssistantConfig("L1 instant power usage",       "l1_instant_power_usage",    "power",       setHomeAssistant);
    sendHomeAssistantConfig("L1 Instant Power Current",     "l1_instant_power_current",  "current",     setHomeAssistant);
    sendHomeAssistantConfig("L1 Voltage",                   "l1_voltage",                "voltage",     setHomeAssistant);
    sendHomeAssistantConfig("L2 instant power usage",       "l2_instant_power_usage",    "power",       setHomeAssistant);
    sendHomeAssistantConfig("L2 Instant Power Current",     "l2_instant_power_current",  "current",     setHomeAssistant);
    sendHomeAssistantConfig("L2 Voltage",                   "l2_voltage",                "voltage",     setHomeAssistant);
    sendHomeAssistantConfig("L3 instant power usage",       "l3_instant_power_usage",    "power",       setHomeAssistant);
    sendHomeAssistantConfig("L3 Instant Power Current",     "l3_instant_power_current",  "current",     setHomeAssistant);
    sendHomeAssistantConfig("L3 Voltage",                   "l3_voltage",                "voltage",     setHomeAssistant);
    sendHomeAssistantConfig("L1 Instant Power Return",      "l1_instant_power_return",   "power",       setHomeAssistant);
    sendHomeAssistantConfig("L2 Instant Power Return",      "l2_instant_power_return",   "power",       setHomeAssistant);
    sendHomeAssistantConfig("L3 Instant Power Return",      "l3_instant_power_return",   "power",       setHomeAssistant);
    sendHomeAssistantConfig("Actual Power Consumption",     "actual_consumption",        "power",       setHomeAssistant);
    sendHomeAssistantConfig("Actual Return Delivery",       "actual_returndelivery",     "power",       setHomeAssistant);
    sendHomeAssistantConfig("Gas Usage",                    "gas_meter_m3",              "gas",         setHomeAssistant);
    sendHomeAssistantConfig("Consumption High Tariff",      "consumption_high_tarif",    "energy",      setHomeAssistant);
    sendHomeAssistantConfig("Consumption Low Tariff",       "consumption_low_tarif",     "energy",      setHomeAssistant);
    sendHomeAssistantConfig("Return Delivery High Tariff",  "returndelivery_high_tarif", "energy",      setHomeAssistant);
    sendHomeAssistantConfig("Return Delivery Low Tariff",   "returndelivery_low_tarif",  "energy",      setHomeAssistant);
    sendHomeAssistantConfig("Long Power Outages",           "long_power_outages",        "outages",     setHomeAssistant);
    sendHomeAssistantConfig("Short Power Outages",          "short_power_outages",       "outages",     setHomeAssistant);
    sendHomeAssistantConfig("Short Power Drops",            "short_power_drops",         "outages",     setHomeAssistant);
    sendHomeAssistantConfig("Short Power Peaks",            "short_power_peaks",         "outages",     setHomeAssistant);
    sendHomeAssistantConfig("Actual Tariff Group",          "actual_tarif_group",        "tarif",       setHomeAssistant);
    sendHomeAssistantConfig("DSRM Version",                 "dsrm_version",              "other",       setHomeAssistant);
    sendHomeAssistantConfig("DSRM DateTime",                "dsrm_datetime",             "date",        setHomeAssistant);
    sendHomeAssistantConfig("DSRM Equipment ID",            "dsrm_equipment_id",         "equipementid",setHomeAssistant);
    sendHomeAssistantConfig("GAS Meter Type",               "gas_meter_type",            "gasdevice",   setHomeAssistant);
    sendHomeAssistantConfig("GAS Equipment ID",             "gas_equipment_id",          "gasdevice",   setHomeAssistant);
    sendHomeAssistantConfig("IP Address",                   "ip_address",                "ipaddress",   setHomeAssistant);
    sendHomeAssistantConfig("Update Available",             "update_available",          "other",       setHomeAssistant);
    sendHomeAssistantConfig("Latest Version",               "latest_version",            "other",       setHomeAssistant);

    // Extra: stuur ook direct de huidige waarden als retained berichten
    if (setHomeAssistant == "on" && LATEST_VERSION[0] != '\0') {
        publishUpdateStatus();
    }
}

// **********************************
// * WIFI                           *
// **********************************
void configModeCallback(WiFiManager *myWiFiManager) {
    DEBUG_PRINTLN(F("Entered config mode"));
    DEBUG_PRINTLN(WiFi.softAPIP());
    DEBUG_PRINTLN(myWiFiManager->getConfigPortalSSID());
    ticker.attach(0.2, tick);
}

// **********************************
// * Ticker (System LED Blinker)    *
// **********************************
void tick() {
    int state = digitalRead(LED_BUILTIN);
    digitalWrite(LED_BUILTIN, !state);
}

// **********************************
// * MQTT                           *
// **********************************
bool mqtt_reconnect() {
    DEBUG_PRINTLN("Attempting MQTT reconnect...");
    if (mqtt_client.connect(HOSTNAME, MQTT_USER, MQTT_PASS)) {
        DEBUG_PRINTLN(F("MQTT connected!"));
        stats.mqttReconnects++;
        return true;
    } else {
        DEBUG_PRINT(F("MQTT Connection failed: rc="));
        DEBUG_PRINTLN(mqtt_client.state());
        return false;
    }
}

void send_mqtt_message(const char *topic, const char *payload) {
    DEBUG_PRINTF("MQTT Outgoing on %s: ", topic);
    DEBUG_PRINTLN(payload);
    bool result = mqtt_client.publish(topic, payload, false);
    if (result) stats.mqttPublishes++;
    else { stats.mqttFailures++; DEBUG_PRINTF("MQTT publish to topic %s failed\n", topic); }
}

void send_metric(const char *name, const char *metric) {
    DEBUG_PRINTF("Sending metric: %s=%s\n", name, metric);
    char topicBuf[96];
    snprintf(topicBuf, sizeof(topicBuf), "%s/%s/state", MQTT_ROOT_TOPIC, name);
    send_mqtt_message(topicBuf, metric);
}

void send_metric(const String& name, const String& metric) {
    send_metric(name.c_str(), metric.c_str());
}

void send_data_to_broker() {
    const size_t count = METRICS_COUNT;
    const unsigned long now = millis();
    bool forceAll = !deltaInitialized || (now - LAST_FULL_UPDATE >= FULL_UPDATE_INTERVAL);
    if (forceAll) {
        DEBUG_PRINTLN(F("Delta: forced full update"));
        LAST_FULL_UPDATE = now;
        deltaInitialized = true;
    }
    int sent = 0;
    char valueBuf[16];
    for (size_t i = 0; i < count; i++) {
        if (metricsFields[i].type == TYPE_LONG) {
            long current = *(metricsFields[i].longValue);
            if (forceAll || current != prevLongValues[i]) {
                prevLongValues[i] = current;
                snprintf(valueBuf, sizeof(valueBuf), "%ld", current);
                send_metric(metricsFields[i].name, valueBuf);
                sent++;
            }
        } else {
            const char* current = metricsFields[i].stringValue;
            if (forceAll || strncmp(current, prevStringValues[i], 63) != 0) {
                strlcpy(prevStringValues[i], current, sizeof(prevStringValues[i]));
                send_metric(metricsFields[i].name, prevStringValues[i]);
                sent++;
            }
        }
    }
    stats.lastPublishAt    = now;
    stats.lastPublishCount = sent;
    DEBUG_PRINTF("Delta: %d/%d topics verstuurd\n", sent, (int)count);
}

// **********************************
// * P1                             *
// **********************************
unsigned int CRC16(unsigned int crc, unsigned char *buf, int len) {
    for (int pos = 0; pos < len; pos++) {
        crc ^= (unsigned int)buf[pos];
        for (int i = 8; i != 0; i--) {
            if ((crc & 0x0001) != 0) { crc >>= 1; crc ^= 0xA001; }
            else crc >>= 1;
        }
    }
    return crc;
}

int FindCharInArrayRev(char array[], char c, int len) {
    for (int i = len - 1; i >= 0; i--)
        if (array[i] == c) return i;
    return -1;
}

long getValue(char *buffer, int maxlen, char startchar, char endchar) {
    char *end = strrchr(buffer, endchar);
    if (!end) return 0;
    char *start = NULL;
    for (char *p = end; p >= buffer; p--) {
        if (*p == startchar) { start = p; break; }
    }
    if (!start) return 0;
    int l = end - (start + 1);
    if (l <= 0 || l >= 15) return 0;
    char res[16];
    strncpy(res, start + 1, l);
    res[l] = '\0';
    for (int i = 0; i < l; i++) {
        if (!(isdigit(res[i]) || res[i] == '.' || res[i] == '-')) return 0;
    }
    double value = atof(res);
    return (endchar == '*') ? (long)(value * 1000 + (value >= 0 ? 0.5 : -0.5)) : (long)value;
}

bool getStringValue(char *buffer, int maxlen, char startchar, char endchar, char *out, int outSize) {
    char *end = strrchr(buffer, endchar);
    if (!end) return false;
    char *start = NULL;
    for (char *p = end; p >= buffer; p--) {
        if (*p == startchar) { start = p; break; }
    }
    if (!start) return false;
    int l = end - (start + 1);
    if (l <= 0 || l >= outSize) return false;
    strncpy(out, start + 1, l);
    out[l] = '\0';
    return true;
}

// Leest beschikbare regels van de P1 poort. Wordt ELKE loop aangeroepen, zodat de
// 1KB RX buffer nooit overloopt (ook niet bij een interval van 60 s). Of er naar
// MQTT gestuurd wordt bepaalt validateTelegram() op basis van het interval.
void read_p1_hardwareserial() {
    while (Serial.available()) {
        ESP.wdtDisable();
        int len = Serial.readBytesUntil('\n', telegram, P1_MAXLINELENGTH);
        ESP.wdtEnable(1);
        telegram[len] = '\n';
        telegram[len + 1] = 0;

        // Nieuw telegram begint met '/' → raw buffer opnieuw beginnen
        if (len > 0 && telegram[0] == '/') fullLen = 0;
        if (fullLen + len + 2 < FULL_TELEGRAM_SIZE) {
            memcpy(fullTelegram + fullLen, telegram, len + 1);
            fullLen += len + 1;
            fullTelegram[fullLen] = 0;
        }
        yield();
        if (validateTelegram(len)) return;
    }
}

bool validateTelegram(int len) {
    bool result = decode_telegram(len + 1);
    if (result) {
        const unsigned long now = millis();
        stats.telegramsOk++;
        stats.lastTelegramAt = now;

        // Snapshot voor /telegram (geen yield tussendoor → altijd consistent)
        memcpy(lastTelegram, fullTelegram, fullLen + 1);
        lastTelegramLen = fullLen;

        if (mqtt_client.connected() &&
            (LAST_UPDATE_SENT == 0 || now - LAST_UPDATE_SENT + INTERVAL_TOLERANCE >= updateInterval)) {
            LAST_UPDATE_SENT = now;
            send_data_to_broker();
            p1CycleComplete = true; // volledig klaar: telegram + MQTT
        }
    }
    return result;
}

bool decode_telegram(int len) {
    int startChar = FindCharInArrayRev(telegram, '/', len);
    int endChar   = FindCharInArrayRev(telegram, '!', len);
    bool validCRCFound = false;
#if DEBUG
    for (int cnt = 0; cnt < len; cnt++) DEBUG_PRINT(telegram[cnt]);
    DEBUG_PRINT("\n");
#endif
    if (startChar >= 0) {
        currentCRC = CRC16(0x0000, (unsigned char*)telegram + startChar, len - startChar);
    } else if (endChar >= 0) {
        currentCRC = CRC16(currentCRC, (unsigned char*)telegram + endChar, 1);
        char messageCRC[5] = {0};
        if (endChar + 5 <= len) {
            strncpy(messageCRC, telegram + endChar + 1, 4);
            messageCRC[4] = 0;
            validCRCFound = (strtol(messageCRC, NULL, 16) == (long)currentCRC);
        }
        if (!validCRCFound) stats.crcErrors++;
        DEBUG_PRINTLN(validCRCFound ? F("CRC Valid!") : F("CRC Invalid!"));
        currentCRC = 0;
    } else {
        currentCRC = CRC16(currentCRC, (unsigned char*)telegram, len);
    }
    char *line = strtok(telegram, "\r\n");
    while (line != NULL) {
        bool matched = false;
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
            if (strncmp(line, fields[i].key, strlen(fields[i].key)) == 0) {
                *fields[i].target = getValue(line, strlen(line), '(', fields[i].stopChar);
                matched = true;
                break;
            }
        }
        if (!matched) {
            for (size_t i = 0; i < sizeof(stringFields) / sizeof(stringFields[0]); ++i) {
                if (strncmp(line, stringFields[i].key, strlen(stringFields[i].key)) == 0) {
                    getStringValue(line, strlen(line), '(', stringFields[i].stopChar,
                                   stringFields[i].target, stringFields[i].maxLen);
                    break;
                }
            }
        }
        line = strtok(NULL, "\r\n");
    }
    return validCRCFound;
}

// Zelfde JSON formaat als de oude latest_telegram.json, maar direct naar de HTTP response
void writeTelegramJson(Print &f) {
    f.println(F("{"));
    f.println(F("  \"device\": {"));
    f.printf("    \"ip_address\": \"%s\",\n", IPADDRESS);
    f.print(F("    \"dsrm_version\": "));      printJsonString(f, DSRMVERSION);   f.println(',');
    f.print(F("    \"dsrm_timestamp\": "));    printJsonString(f, DSRMTIMESTAMP); f.println(',');
    f.print(F("    \"dsrm_equipment_id\": ")); printJsonString(f, EQUIPMENTID);   f.println();
    f.println(F("  },"));
    f.println(F("  \"readings\": {"));
    f.printf("    \"consumption_low_tarif\": { \"value\": %.3f, \"unit\": \"kWh\" },\n", CONSUMPTION_LOW_TARIF / 1000.00);
    f.printf("    \"consumption_high_tarif\": { \"value\": %.3f, \"unit\": \"kWh\" },\n", CONSUMPTION_HIGH_TARIF / 1000.00);
    f.printf("    \"return_low_tarif\": { \"value\": %.3f, \"unit\": \"kWh\" },\n", RETURNDELIVERY_LOW_TARIF / 1000.00);
    f.printf("    \"return_high_tarif\": { \"value\": %.3f, \"unit\": \"kWh\" }\n", RETURNDELIVERY_HIGH_TARIF / 1000.00);
    f.println(F("  },"));
    f.println(F("  \"actuals\": {"));
    f.printf("    \"actual_consumption\": { \"value\": %.3f, \"unit\": \"kW\" },\n", ACTUAL_CONSUMPTION / 1000.00);
    f.printf("    \"actual_return\": { \"value\": %.3f, \"unit\": \"kW\" },\n", ACTUAL_RETURNDELIVERY / 1000.00);
    f.printf("    \"tarif\": %ld\n", ACTUAL_TARIF_GROUP);
    f.println(F("  },"));
    long *phases[3][4] = {
        {&L1_INSTANT_POWER_USAGE, &L1_INSTANT_POWER_RETURN, &L1_INSTANT_POWER_CURRENT, &L1_VOLTAGE},
        {&L2_INSTANT_POWER_USAGE, &L2_INSTANT_POWER_RETURN, &L2_INSTANT_POWER_CURRENT, &L2_VOLTAGE},
        {&L3_INSTANT_POWER_USAGE, &L3_INSTANT_POWER_RETURN, &L3_INSTANT_POWER_CURRENT, &L3_VOLTAGE},
    };
    for (int p = 0; p < 3; p++) {
        f.printf("  \"l%d\": {\n", p + 1);
        f.printf("    \"power\": { \"value\": %.3f, \"unit\": \"kW\" },\n", *phases[p][0] / 1000.00);
        f.printf("    \"power_return\": { \"value\": %.3f, \"unit\": \"kW\" },\n", *phases[p][1] / 1000.00);
        f.printf("    \"current\": { \"value\": %ld, \"unit\": \"A\" },\n", *phases[p][2] / 1000);
        f.printf("    \"voltage\": { \"value\": %ld, \"unit\": \"V\" }\n", *phases[p][3] / 1000);
        f.println(F("  },"));
    }
    f.println(F("  \"gas\": {"));
    f.printf("    \"gas_delivered\": { \"value\": %.3f, \"unit\": \"m3\" },\n", GAS_METER_M3 / 1000.00);
    f.print(F("    \"gas_equipment_id\": ")); printJsonString(f, GASEQUIPMENTID); f.println(',');
    f.print(F("    \"gas_meter_type\": "));   printJsonString(f, GASMETERTYPE);   f.println();
    f.println(F("  },"));
    f.println(F("  \"status\": {"));
    f.printf("    \"short_power_outage\": %ld,\n", SHORT_POWER_OUTAGES);
    f.printf("    \"long_power_outage\": %ld,\n", LONG_POWER_OUTAGES);
    f.printf("    \"short_power_drops\": %ld,\n", SHORT_POWER_DROPS);
    f.printf("    \"short_power_peaks\": %ld\n", SHORT_POWER_PEAKS);
    f.println(F("  },"));
    f.println(F("  \"rawtelegram\": ["));
    // Regels uit de snapshot, zonder de buffer te wijzigen (geen strtok)
    bool firstLine = true;
    int i = 0;
    while (i < lastTelegramLen) {
        int start = i;
        while (i < lastTelegramLen && lastTelegram[i] != '\n') i++;
        int end = i;
        while (end > start && lastTelegram[end - 1] == '\r') end--;
        i++;
        if (end == start) continue;
        if (!firstLine) f.println(',');
        firstLine = false;
        f.print(F("    \""));
        for (int k = start; k < end; k++) {
            char c = lastTelegram[k];
            if (c == '"' || c == '\\') f.print('\\');
            f.print(c);
        }
        f.print('"');
    }
    f.println();
    f.println(F("  ]"));
    f.println(F("}"));
}

// **********************************
// * EEPROM helpers                 *
// **********************************
String read_eeprom(int offset, int len) {
    String res = "";
    for (int i = 0; i < len; ++i) {
        char c = char(EEPROM.read(i + offset));
        if (c == 0) break;
        res += c;
    }
    return res;
}

void write_eeprom(int offset, int len, String value) {
    for (int i = 0; i < len; ++i) {
        EEPROM.write(i + offset, (unsigned)i < value.length() ? value[i] : 0);
    }
}

// **********************************
// * WiFi config callback           *
// **********************************
bool shouldSaveConfig = false;
void save_wifi_config_callback() {
    DEBUG_PRINTLN(F("Should save config"));
    shouldSaveConfig = true;
}

// **********************************
// * Setup OTA                      *
// **********************************
void setup_ota() {
    ArduinoOTA.setPort(8266);
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]()    { DEBUG_PRINTLN(F("Arduino OTA: Start")); });
    ArduinoOTA.onEnd([]()      { DEBUG_PRINTLN(F("Arduino OTA: End (Running reboot)")); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        DEBUG_PRINTF("Arduino OTA Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        DEBUG_PRINTF("Arduino OTA Error[%u]\n", error);
    });
    ArduinoOTA.begin();
    DEBUG_PRINTLN(F("Arduino OTA activated."));
}

// **********************************
// * Setup MDNS                     *
// **********************************
void setup_mdns() {
    DEBUG_PRINTLN(F("Starting MDNS responder service"));
    bool mdns_result = MDNS.begin(HOSTNAME);
    if (mdns_result) MDNS.addService("http", "tcp", 80);
}

// **********************************
// * Setup webserver                *
// **********************************
void setup_webserver() {
    // Webpagina's (één single-page app uit PROGMEM)
    httpRestServer.on("/", HTTP_GET, handleRoot);
    httpRestServer.on("/info", HTTP_GET, handlePage);
    httpRestServer.on("/settings", HTTP_GET, handlePage);
    httpRestServer.on("/files", HTTP_GET, handlePage);
    httpRestServer.on("/update", HTTP_GET, handlePage);
    httpRestServer.on("/update/", HTTP_GET, handlePage);

    // JSON API voor de pagina's
    httpRestServer.on("/api/live", HTTP_GET, handleApiLive);
    httpRestServer.on("/api/info", HTTP_GET, handleApiInfo);
    httpRestServer.on("/api/settings", HTTP_GET, handleApiSettingsGet);
    httpRestServer.on("/api/settings", HTTP_POST, handleApiSettingsPost);
    httpRestServer.on("/api/fs/list", HTTP_GET, handleFsList);
    httpRestServer.on("/api/fs/file", HTTP_GET, handleFsDownload);
    httpRestServer.on("/api/fs/delete", HTTP_POST, handleFsDelete);
    httpRestServer.on("/api/fs/upload", HTTP_POST, handleFsUploadDone, handleFsUploadChunk);

    // Bestaande REST API (v2.7 compatibel)
    httpRestServer.on("/version", HTTP_GET, handleVersion);
    httpRestServer.on("/restart", HTTP_GET, handleRestart);
    httpRestServer.on("/interval/set", HTTP_GET, handleSetInterval);
    httpRestServer.on("/interval", HTTP_GET, handleGetInterval);
    httpRestServer.on("/haon", HTTP_GET, handleHaon);
    httpRestServer.on("/haoff", HTTP_GET, handleHaoff);
    httpRestServer.on("/mqtt", HTTP_GET, handleMQTTSettings);
    httpRestServer.on("/telegram", HTTP_GET, handleTelegramJson);
    httpRestServer.on("/update/check", HTTP_GET, handleUpdateCheck);
    httpRestServer.on("/update/settings", HTTP_GET, handleUpdateSettings);
    httpRestServer.on("/update/github", HTTP_GET, handleUpdateGitHub);
    httpRestServer.on("/update/upload", HTTP_POST, handleUpdateUploadDone, handleUpdateUpload);

    httpRestServer.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not found");
    });

    httpRestServer.begin();
    DEBUG_PRINTLN("HTTP server started");
}

// **********************************
// * Setup Main                     *
// **********************************
void setup() {
    EEPROM.begin(512);
    Serial.setRxBufferSize(1024);
    Serial.begin(BAUD_RATE, SERIAL_8N1, SERIAL_FULL);
    Serial.setTimeout(200);   // een regel komt binnen ~10 ms; voorkomt 1 s blokkeren
    DEBUG_PRINTLN("");
    DEBUG_PRINTLN("P1 Meter firmware v" APPVERSION " - starting up...");
    DEBUG_PRINTLN("Swapping UART0 RX to inverted");
    Serial.flush();
    USC0(UART0) = USC0(UART0) | BIT(UCRXI);
    DEBUG_PRINTLN("Serial port is ready to recieve.");
    pinMode(LED_BUILTIN, OUTPUT);
    ticker.attach(0.6, tick);

    String settings_available = read_eeprom(EEPROM_ADDR_SETTINGS_FLAG, 1);
    if (settings_available == "1") {
        read_eeprom(EEPROM_ADDR_MQTT_HOST,  64).toCharArray(MQTT_HOST,       64);
        read_eeprom(EEPROM_ADDR_MQTT_PORT,   6).toCharArray(MQTT_PORT,        6);
        read_eeprom(EEPROM_ADDR_MQTT_USER,  32).toCharArray(MQTT_USER,       32);
        read_eeprom(EEPROM_ADDR_MQTT_PASS,  32).toCharArray(MQTT_PASS,       32);
        read_eeprom(EEPROM_ADDR_MQTT_TOPIC, 32).toCharArray(MQTT_ROOT_TOPIC, 32);

        // v2.7 schreef hier de string "true"/"false" (5 bytes, overschreef ook het interval).
        // 1 of 't' = aan, al het andere = uit.
        uint8_t had = EEPROM.read(EEPROM_ADDR_HA_DISCOVERY);
        HA_DISCOVERY = (had == 1 || had == 't');

        // Laad update interval
        unsigned long savedInterval = 0;
        EEPROM.get(EEPROM_ADDR_UPDATE_INTERVAL, savedInterval);
        if (savedInterval >= 1000 && savedInterval <= 3600000) {
            updateInterval = savedInterval;
            DEBUG_PRINTF("Update interval geladen: %lu ms\n", updateInterval);
        }

        // Laad OTA check interval
        uint8_t savedCheckHours = EEPROM.read(EEPROM_ADDR_OTA_CHECK_HOURS);
        if (savedCheckHours <= 24) {
            otaCheckInterval = (unsigned long)savedCheckHours * 3600000UL; // 0 = uit
            DEBUG_PRINTF("OTA check interval geladen: %d uur\n", savedCheckHours);
        }

        // Laad auto-update
        AUTO_UPDATE = (EEPROM.read(EEPROM_ADDR_AUTO_UPDATE) == 1);
        DEBUG_PRINTF("Auto-update: %s\n", AUTO_UPDATE ? "aan" : "uit");
    }

    WiFiManagerParameter CUSTOM_MQTT_HOST("host",  "MQTT hostname",      MQTT_HOST,       64);
    WiFiManagerParameter CUSTOM_MQTT_PORT("port",  "MQTT port",          MQTT_PORT,        6);
    WiFiManagerParameter CUSTOM_MQTT_USER("user",  "MQTT user",          MQTT_USER,       32);
    WiFiManagerParameter CUSTOM_MQTT_PASS("pass",  "MQTT pass",          MQTT_PASS,       32);
    WiFiManagerParameter CUSTOM_MQTT_ROOT_TOPIC("topic", "MQTT root topic", MQTT_ROOT_TOPIC, 32);
    WiFiManagerParameter CUSTOM_HA_DISCOVERY("had", "Home Assistant discovery?", HA_DISCOVERY ? "true" : "false", 5);

    WiFiManager wifiManager;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);

    wifiManager.setAPCallback(configModeCallback);
    wifiManager.setConfigPortalTimeout(300);
    wifiManager.setConnectTimeout(60);
    wifiManager.setSaveConfigCallback(save_wifi_config_callback);
    wifiManager.addParameter(&CUSTOM_MQTT_HOST);
    wifiManager.addParameter(&CUSTOM_MQTT_PORT);
    wifiManager.addParameter(&CUSTOM_MQTT_USER);
    wifiManager.addParameter(&CUSTOM_MQTT_PASS);
    wifiManager.addParameter(&CUSTOM_MQTT_ROOT_TOPIC);
    wifiManager.addParameter(&CUSTOM_HA_DISCOVERY);

    if (!wifiManager.autoConnect()) {
        DEBUG_PRINTLN(F("WiFi autoConnect mislukt — opnieuw proberen..."));
        delay(5000);
        ESP.restart();
    }

    if (shouldSaveConfig) {
        DEBUG_PRINTLN(F("Saving WiFiManager config"));
        strlcpy(MQTT_HOST,       CUSTOM_MQTT_HOST.getValue(),       sizeof(MQTT_HOST));
        strlcpy(MQTT_PORT,       CUSTOM_MQTT_PORT.getValue(),       sizeof(MQTT_PORT));
        strlcpy(MQTT_USER,       CUSTOM_MQTT_USER.getValue(),       sizeof(MQTT_USER));
        strlcpy(MQTT_PASS,       CUSTOM_MQTT_PASS.getValue(),       sizeof(MQTT_PASS));
        strlcpy(MQTT_ROOT_TOPIC, CUSTOM_MQTT_ROOT_TOPIC.getValue(), sizeof(MQTT_ROOT_TOPIC));
        HA_DISCOVERY = strcmp(CUSTOM_HA_DISCOVERY.getValue(), "true") == 0;
        write_eeprom(EEPROM_ADDR_MQTT_HOST,      64, MQTT_HOST);
        write_eeprom(EEPROM_ADDR_MQTT_PORT,       6, MQTT_PORT);
        write_eeprom(EEPROM_ADDR_MQTT_USER,      32, MQTT_USER);
        write_eeprom(EEPROM_ADDR_MQTT_PASS,      32, MQTT_PASS);
        write_eeprom(EEPROM_ADDR_MQTT_TOPIC,     32, MQTT_ROOT_TOPIC);
        write_eeprom(EEPROM_ADDR_SETTINGS_FLAG,   1, "1");
        EEPROM.write(EEPROM_ADDR_HA_DISCOVERY, HA_DISCOVERY ? 1 : 0);
        EEPROM.commit();
        DEBUG_PRINTLN(F("Config opgeslagen in EEPROM"));
    }

    DEBUG_PRINTLN(F("Connected to WIFI..."));
    ticker.detach();
    digitalWrite(LED_BUILTIN, LOW);

    setup_ota();
    setup_mdns();

    DEBUG_PRINTF("MQTT connecting to: %s:%s\n", MQTT_HOST, MQTT_PORT);
    mqtt_client.setServer(MQTT_HOST, atoi(MQTT_PORT));
    mqtt_client.setBufferSize(700);
    // Kortere timeouts: een onbereikbare broker mag de P1 uitlezing niet lang blokkeren
    mqtt_client.setSocketTimeout(5);
    espClient.setTimeout(2000);

    LittleFS.begin();
    // Oude telegram-dump (v2.7 schreef deze elke cyclus naar flash) opruimen
    if (LittleFS.exists("/latest_telegram.json")) LittleFS.remove("/latest_telegram.json");

    setup_webserver();

    updateIpAddress();

    // Versiecheck wordt gedaan zodra de eerste P1 cyclus klaar is in loop()
    LAST_OTA_CHECK = 0;
}

// **********************************
// * Loop                           *
// **********************************
void loop() {
    const unsigned long loopStart = micros();
    const unsigned long now = millis();

    // Uptime rollover bijhouden
    if (now < lastMillis) uptimeRollovers++;
    lastMillis = now;

    // P1 poort altijd leegtrekken — ook als WiFi/MQTT weg is
    read_p1_hardwareserial();

    ArduinoOTA.handle();
    MDNS.update();

    if (setRestart && millis() - restartInitiated >= restartDelayMs) {
        ESP.restart();
    }

    // GitHub update aangevraagd via web: 1 s wachten zodat de HTTP response eruit is
    if (doGitHubUpdate && millis() - gitHubUpdateRequestedAt >= 1000) {
        performGitHubUpdate();   // herstart bij succes
        doGitHubUpdate = false;
    }

    if (setHaDOn)  { setHaDOn  = false; handleHaDiscoveryOn();  }
    if (setHaDOff) { setHaDOff = false; handleHaDiscoveryOff(); }

    // --- WiFi check & reconnect ---
    static bool wifiWasConnected = true;
    if (WiFi.status() != WL_CONNECTED) {
        wifiWasConnected    = false;
        haDiscoverySetupRun = false;
        deltaInitialized    = false;
        p1CycleComplete     = false;
        if (now - LAST_WIFI_RECONNECT_ATTEMPT > WIFI_RECONNECT_INTERVAL) {
            LAST_WIFI_RECONNECT_ATTEMPT = now;
            stats.wifiReconnects++;
            DEBUG_PRINTLN(F("WiFi verbroken, probeer opnieuw te verbinden..."));
            WiFi.disconnect();
            WiFi.reconnect();
        }
        return;
    }
    if (!wifiWasConnected) {
        wifiWasConnected = true;
        updateIpAddress();   // IP kan veranderd zijn na reconnect
    }

    // --- MQTT check & reconnect ---
    if (!mqtt_client.connected()) {
        haDiscoverySetupRun = false;
        deltaInitialized    = false;
        p1CycleComplete     = false;
        if (now - LAST_RECONNECT_ATTEMPT > MQTT_RECONNECT_INTERVAL) {
            LAST_RECONNECT_ATTEMPT = now;
            DEBUG_PRINTLN(F("MQTT verbroken, probeer opnieuw te verbinden..."));
            if (mqtt_reconnect()) {
                DEBUG_PRINTLN(F("MQTT herverbonden!"));
                if (HA_DISCOVERY && !haDiscoverySetupRun) {
                    haDiscovery("on");
                    haDiscoverySetupRun = true;
                }
                if (LATEST_VERSION[0] != '\0') {
                    publishUpdateStatus();
                }
            }
        }
        return;
    }

    mqtt_client.loop();

    if (HA_DISCOVERY && !haDiscoverySetupRun) {
        haDiscovery("on");
        haDiscoverySetupRun = true;
    }

    // --- Periodieke OTA versiecheck ---
    // Direct na een afgeronde P1 cyclus, zodat de blocking HTTP call (max 5 s)
    // in de pauze tussen twee telegrammen valt.
    if (otaCheckInterval > 0 && p1CycleComplete) {
        p1CycleComplete = false;
        if (LAST_OTA_CHECK == 0 || now - LAST_OTA_CHECK >= otaCheckInterval) {
            LAST_OTA_CHECK = now ? now : 1;
            DEBUG_PRINTLN(F("Periodieke OTA versiecheck..."));
            if (checkGitHubVersion() && mqtt_client.connected()) {
                publishUpdateStatus();
            }
            if (AUTO_UPDATE && UPDATE_AVAILABLE) {
                DEBUG_PRINTLN(F("Auto-update: nieuwe versie gevonden, update starten..."));
                performGitHubUpdate();
            }
        }
    }

    // Langste loop-duur bijhouden (venster van 10 s) — handig om te zien of iets blokkeert
    static unsigned long maxWindowStart = 0;
    static unsigned long maxInWindow = 0;
    unsigned long dur = micros() - loopStart;
    if (dur > maxInWindow) maxInWindow = dur;
    if (now - maxWindowStart >= 10000) {
        stats.maxLoopUs = maxInWindow;
        maxInWindow = 0;
        maxWindowStart = now;
    }
}
