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
#include <LittleFS.h>
#include <Updater.h>
#include <StreamString.h>

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
#include "crashguard.h"

// * Optioneel: reserve WiFi in secrets.h (staat niet in git, zie secrets.h.example).
//   Wordt alleen gebruikt als de ESP geen opgeslagen WiFi gegevens heeft.
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef WIFI_FALLBACK_SSID
  #define WIFI_FALLBACK_SSID ""
  #define WIFI_FALLBACK_PASS ""
#endif

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
bool send_mqtt_message(const char *topic, const char *payload);
bool send_metric(const String& name, const String& metric);
void send_data_to_broker();
void read_p1_hardwareserial();
void save_wifi_config_callback();
void setup_ota();
void setup_mdns();
void setup_webserver();
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
// 172-173 waren GitHub versiecheck / auto-update (v2.7), nu ongebruikt

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
static uint32_t prevStringHash[METRICS_COUNT];   // hash i.p.v. kopie: scheelt ~1,9 KB RAM
static bool     deltaInitialized = false;

// * Forced full update every 5 minutes regardless of delta
#define FULL_UPDATE_INTERVAL 300000UL
unsigned long LAST_FULL_UPDATE = 0;

// * Ruw telegram, alleen op aanvraag (debug). Geen permanente buffers:
//   pas bij een aanvraag wordt 2 KB gereserveerd, en na gebruik / 5 min weer vrijgegeven.
enum RawState : uint8_t { RAW_IDLE, RAW_WAIT, RAW_CAPTURING, RAW_READY };
volatile RawState rawState = RAW_IDLE;
char *rawBuf = nullptr;
int   rawLen = 0;
unsigned long rawStateAt = 0;
#define RAW_WAIT_TIMEOUT  30000UL     // max wachten op een geldig telegram
#define RAW_KEEP_TIMEOUT  300000UL    // opgevangen telegram 5 min bewaren

// * Lange regel (langer dan de regelbuffer) wordt in stukken gelezen
bool lineContinues = false;

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

// * Herstart wordt vanuit loop() gedaan, nooit vanuit een async callback
unsigned long restartDelayMs = restartInterval;

// * Initiate WIFI client
WiFiClient espClient;

// * Initiate MQTT client
PubSubClient mqtt_client(espClient);

// {root}/status: "online" of "offline". "offline" komt ook als MQTT "last will" van de
// broker wanneer de ESP wegvalt (crash, stroom, WiFi). In veilige modus ook "offline",
// zodat de P1 sensors in HA op "niet beschikbaar" gaan.
char statusTopic[48];

// * Initiate API settings
AsyncWebServer httpRestServer(HTTP_REST_PORT);

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
        else if ((uint8_t)c < 0x20) out.printf_P(PSTR("\\u%04x"), c);
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
    if (request->hasParam("safe")) {
        crashguardForceSafeNextBoot();
        request->send(200, "text/plain", "Restarting in safe mode in 5 seconds...");
    } else {
        // Normale herstart: crash-teller op 0, dus ook weer uit veilige modus
        request->send(200, "text/plain", "Server is restarting in 5 seconds, restarting can take up to 1 minute...");
    }
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

// GET /api/raw — ruw telegram voor debugging, op aanvraag
//   202 = bezig met opvangen (opnieuw proberen), 200 = tekst van het telegram
//   ?free=1 geeft het geheugen direct weer vrij
void handleApiRaw(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    if (hasAnyParam(request, "free")) {
        if (rawState == RAW_READY || rawState == RAW_IDLE) rawFree();
        request->send(200, "text/plain", "ok");
        return;
    }
    if (rawState == RAW_READY && rawBuf) {
        AsyncResponseStream *resp = request->beginResponseStream("text/plain", rawLen + 16);
        resp->write((const uint8_t*)rawBuf, rawLen);
        request->send(resp);
        return;
    }
    if (rawState == RAW_IDLE) { rawStateAt = millis(); rawState = RAW_WAIT; }
    request->send(202, "text/plain", "wachten op volgend telegram...");
}

// GET /api/live — compacte actuele waarden voor het dashboard
void handleApiLive(AsyncWebServerRequest *request) {
    AsyncResponseStream *resp = request->beginResponseStream("application/json");
    const unsigned long now = millis();
    resp->print('{');
    resp->printf_P(PSTR("\"age\":%ld,"), stats.telegramsOk ? (long)(now - stats.lastTelegramAt) : -1L);
    resp->printf_P(PSTR("\"ok\":%u,\"crc\":%u,"), stats.telegramsOk, stats.crcErrors);
    resp->printf_P(PSTR("\"mqtt\":%s,\"safe\":%s,"), mqtt_client.connected() ? "true" : "false", safeMode ? "true" : "false");
    resp->printf_P(PSTR("\"interval\":%lu,"), updateInterval);
    resp->print(F("\"ts\":"));  printJsonString(*resp, DSRMTIMESTAMP);
    resp->printf_P(PSTR(",\"tarif\":%ld"), ACTUAL_TARIF_GROUP);
    resp->printf_P(PSTR(",\"p\":%ld,\"r\":%ld"), ACTUAL_CONSUMPTION, ACTUAL_RETURNDELIVERY);
    resp->printf_P(PSTR(",\"t1\":%ld,\"t2\":%ld,\"r1\":%ld,\"r2\":%ld"),
        CONSUMPTION_LOW_TARIF, CONSUMPTION_HIGH_TARIF, RETURNDELIVERY_LOW_TARIF, RETURNDELIVERY_HIGH_TARIF);
    resp->printf_P(PSTR(",\"gas\":%ld"), GAS_METER_M3);
    resp->printf_P(PSTR(",\"ph\":[[%ld,%ld,%ld,%ld],[%ld,%ld,%ld,%ld],[%ld,%ld,%ld,%ld]]"),
        L1_INSTANT_POWER_USAGE, L1_INSTANT_POWER_RETURN, L1_INSTANT_POWER_CURRENT, L1_VOLTAGE,
        L2_INSTANT_POWER_USAGE, L2_INSTANT_POWER_RETURN, L2_INSTANT_POWER_CURRENT, L2_VOLTAGE,
        L3_INSTANT_POWER_USAGE, L3_INSTANT_POWER_RETURN, L3_INSTANT_POWER_CURRENT, L3_VOLTAGE);
    resp->printf_P(PSTR(",\"ev\":[%ld,%ld,%ld,%ld]"),
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
    resp->printf_P(PSTR(",\"uptime\":%lu"), (unsigned long)(uptimeMs() / 1000ULL));
    resp->print(F(",\"reset_reason\":")); printJsonString(*resp, ESP.getResetReason().c_str());
    resp->printf_P(PSTR(",\"chip_id\":\"%06X\""), ESP.getChipId());
    resp->printf_P(PSTR(",\"cpu_mhz\":%u"), ESP.getCpuFreqMHz());
    // Wijkt de ingestelde flash grootte (build) af van de echte chip, dan staan WiFi en
    // EEPROM op een andere plek dan verwacht: instellingen lijken dan "gewist".
    resp->printf_P(PSTR(",\"flash_size\":%u,\"flash_config\":%u"), ESP.getFlashChipRealSize(), ESP.getFlashChipSize());
    resp->printf_P(PSTR(",\"sketch_size\":%u,\"sketch_free\":%u"), ESP.getSketchSize(), ESP.getFreeSketchSpace());
    resp->printf_P(PSTR(",\"heap_free\":%u,\"heap_max_block\":%u,\"heap_frag\":%u"),
        ESP.getFreeHeap(), ESP.getMaxFreeBlockSize(), ESP.getHeapFragmentation());
    resp->print(F(",\"core\":")); printJsonString(*resp, ESP.getCoreVersion().c_str());
    resp->print(F(",\"sdk\":"));  printJsonString(*resp, ESP.getSdkVersion());
    resp->printf_P(PSTR(",\"fs_total\":%u,\"fs_used\":%u"), fsOk ? (unsigned)fsi.totalBytes : 0, fsOk ? (unsigned)fsi.usedBytes : 0);
    resp->print(F(",\"ssid\":"));  printJsonString(*resp, WiFi.SSID().c_str());
    resp->print(F(",\"bssid\":\"")); resp->print(WiFi.BSSIDstr()); resp->print('"');
    resp->printf_P(PSTR(",\"rssi\":%d,\"channel\":%d"), WiFi.RSSI(), WiFi.channel());
    resp->printf_P(PSTR(",\"ip\":\"%s\""), IPADDRESS);
    resp->print(F(",\"gateway\":\"")); resp->print(WiFi.gatewayIP().toString()); resp->print('"');
    resp->print(F(",\"dns\":\""));     resp->print(WiFi.dnsIP().toString());     resp->print('"');
    resp->print(F(",\"mac\":\""));     resp->print(WiFi.macAddress());           resp->print('"');
    resp->print(F(",\"mqtt_host\":")); printJsonString(*resp, MQTT_HOST);
    resp->printf_P(PSTR(",\"mqtt_port\":\"%s\""), MQTT_PORT);
    resp->print(F(",\"mqtt_topic\":")); printJsonString(*resp, MQTT_ROOT_TOPIC);
    resp->printf_P(PSTR(",\"mqtt_connected\":%s,\"mqtt_state\":%d"), mqtt_client.connected() ? "true" : "false", mqtt_client.state());
    resp->printf_P(PSTR(",\"mqtt_publishes\":%u,\"mqtt_failures\":%u,\"mqtt_reconnects\":%u"),
        stats.mqttPublishes, stats.mqttFailures, stats.mqttReconnects);
    resp->printf_P(PSTR(",\"last_publish_age\":%ld,\"last_publish_count\":%u"),
        stats.lastPublishAt ? (long)(now - stats.lastPublishAt) : -1L, stats.lastPublishCount);
    resp->printf_P(PSTR(",\"wifi_reconnects\":%u"), stats.wifiReconnects);
    resp->printf_P(PSTR(",\"telegrams_ok\":%u,\"crc_errors\":%u"), stats.telegramsOk, stats.crcErrors);
    resp->printf_P(PSTR(",\"last_telegram_age\":%ld"), stats.telegramsOk ? (long)(now - stats.lastTelegramAt) : -1L);
    resp->printf_P(PSTR(",\"max_loop_us\":%lu"), stats.maxLoopUs);
    resp->printf_P(PSTR(",\"interval\":%lu"), updateInterval);
    resp->print(','); printCrashJson(*resp);
    resp->print(F(",\"boot_reason\":")); printJsonString(*resp, bootReasonText().c_str());
    resp->printf_P(PSTR(",\"ha_discovery\":%s"), HA_DISCOVERY ? "true" : "false");
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
    resp->printf_P(PSTR("\"interval\":%lu"), updateInterval / 1000UL);
    resp->printf_P(PSTR(",\"ha_discovery\":%s"), HA_DISCOVERY ? "true" : "false");
    resp->print(F(",\"mqtt_host\":"));  printJsonString(*resp, MQTT_HOST);
    resp->print(F(",\"mqtt_port\":"));  printJsonString(*resp, MQTT_PORT);
    resp->print(F(",\"mqtt_user\":"));  printJsonString(*resp, MQTT_USER);
    resp->printf_P(PSTR(",\"mqtt_pass_set\":%s"), MQTT_PASS[0] ? "true" : "false");
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
        // Ongeldige waarde = ESP onbereikbaar voor MQTT na herstart; vooraf weigeren
        const char *err = nullptr;
        if (strcmp(f.param, "mqtt_host") == 0 && v.length() == 0) err = "MQTT host mag niet leeg zijn";
        if (strcmp(f.param, "mqtt_port") == 0) {
            long port = v.toInt();
            bool digits = v.length() > 0;
            for (size_t k = 0; k < v.length(); k++) if (!isDigit(v[k])) digits = false;
            if (!digits || port < 1 || port > 65535) err = "MQTT poort moet 1-65535 zijn";
        }
        if (strcmp(f.param, "mqtt_topic") == 0 &&
            (v.length() == 0 || v.indexOf('#') >= 0 || v.indexOf('+') >= 0 || v.endsWith("/")))
            err = "Ongeldig root topic (leeg, eindigt op / of bevat # of +)";
        if (err) {
            request->send(400, "application/json", String("{\"ok\":false,\"error\":\"") + err + "\"}");
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
        out.printf_P(PSTR(",\"size\":%u}"), (unsigned)dir.fileSize());
    }
}

void handleFsList(AsyncWebServerRequest *request) {
    if (!authOK(request)) return;
    FSInfo fsi;
    bool fsOk = LittleFS.info(fsi);
    AsyncResponseStream *resp = request->beginResponseStream("application/json");
    resp->printf_P(PSTR("{\"total\":%u,\"used\":%u,\"files\":["),
        fsOk ? (unsigned)fsi.totalBytes : 0, fsOk ? (unsigned)fsi.usedBytes : 0);
    bool first = true;
    listDir(*resp, "/", first);
    resp->print(F("]}"));
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
        request->onDisconnect([]() { if (fsUploadFile) fsUploadFile.close(); });
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
// * Firmware update (upload .bin)  *
// **********************************

// POST /update/upload — bin bestand upload (chunks)
bool otaUploadFailed = false;

void handleUpdateUpload(AsyncWebServerRequest *request, String filename,
                        size_t index, uint8_t *data, size_t len, bool final) {
    if (index == 0) {
        otaUploadFailed = false;
        if (!authSilent(request)) { otaUploadFailed = true; return; }
        DEBUG_PRINTF("OTA upload start: %s\n", filename.c_str());
        // Een eerder afgebroken upload laat de updater "running" achter (met 4 KB buffer);
        // dan zou elke nieuwe upload mislukken tot een herstart. Eerst opruimen.
        if (Update.isRunning()) Update.end(false);
        Update.clearError();
        // Verbinding weg midden in de upload (browser dicht, WiFi weg): direct opruimen
        request->onDisconnect([]() {
            if (Update.isRunning() && !Update.isFinished()) Update.end(false);
        });
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

// GET /update/check - huidige versie (de versiecheck tegen GitHub doet de browser zelf)
void handleUpdateCheck(AsyncWebServerRequest *request) {
    String json = F("{\"success\":true,\"current\":\"" APPVERSION "\",\"repo\":\"" GITHUB_USER "/" GITHUB_REPO "\",\"free_heap\":");
    json += ESP.getFreeHeap();
    json += '}';
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

// Namen/ids staan in flash (F()), niet in RAM
void sendHomeAssistantConfig(const __FlashStringHelper *sensorname, const __FlashStringHelper *sensorid_f,
                             const __FlashStringHelper *deviceclass, const String& setHomeAssistant,
                             bool diagnostic) {
    const String sensorid(sensorid_f);
    topic = String(MQTT_ROOT_TOPIC) + "/" + sensorid + F("/config");
    if (setHomeAssistant == "on") {
        JsonDocument jsonDoc;   // lokaal: geheugen wordt direct weer vrijgegeven
        jsonDoc[F("platform")]    = F("mqtt");
        jsonDoc[F("unique_id")]   = String(F("p1_")) + sensorid;
        jsonDoc[F("name")]        = sensorname;
        jsonDoc[F("state_topic")] = String(MQTT_ROOT_TOPIC) + "/" + sensorid + F("/state");
        JsonObject dev = jsonDoc[F("device")].to<JsonObject>();
        dev[F("identifiers")][0]    = F("p1meter1337");
        dev[F("name")]              = F("P1 Meter");
        dev[F("model")]             = F("P1 Meter with HA and API integration");
        dev[F("manufacturer")]      = F("tedzor");
        dev[F("sw_version")]        = F(APPVERSION);
        dev[F("configuration_url")] = String(F("http://")) + IPADDRESS + "/";
        SensorConfig cfg = getSensorConfig(String(deviceclass));
        if (strcmp(cfg.ha_device_class, "none") != 0) jsonDoc[F("device_class")]       = cfg.ha_device_class;
        if (strcmp(cfg.state_class,     "none") != 0) jsonDoc[F("state_class")]         = cfg.state_class;
        if (strcmp(cfg.unit,            "none") != 0) jsonDoc[F("unit_of_measurement")] = cfg.unit;
        jsonDoc[F("icon")] = cfg.icon;
        if (diagnostic) {
            // Blijft zichtbaar als de meter offline / in veilige modus is
            jsonDoc[F("entity_category")] = F("diagnostic");
            if (strcmp_P(sensorid.c_str(), PSTR("reset_reason")) == 0)
                jsonDoc[F("json_attributes_topic")] = String(MQTT_ROOT_TOPIC) + F("/crash/state");
        } else {
            jsonDoc[F("availability_topic")] = statusTopic;
        }
        if (cfg.divide_by_1000) jsonDoc[F("value_template")] = F("{{ value|float / 1000 }}");
        else                    jsonDoc[F("value_template")] = F("{{ value }}");
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
    sendHomeAssistantConfig(F("L1 instant power usage"),       F("l1_instant_power_usage"),    F("power"),       setHomeAssistant, false);
    sendHomeAssistantConfig(F("L1 Instant Power Current"),     F("l1_instant_power_current"),  F("current"),     setHomeAssistant, false);
    sendHomeAssistantConfig(F("L1 Voltage"),                   F("l1_voltage"),                F("voltage"),     setHomeAssistant, false);
    sendHomeAssistantConfig(F("L2 instant power usage"),       F("l2_instant_power_usage"),    F("power"),       setHomeAssistant, false);
    sendHomeAssistantConfig(F("L2 Instant Power Current"),     F("l2_instant_power_current"),  F("current"),     setHomeAssistant, false);
    sendHomeAssistantConfig(F("L2 Voltage"),                   F("l2_voltage"),                F("voltage"),     setHomeAssistant, false);
    sendHomeAssistantConfig(F("L3 instant power usage"),       F("l3_instant_power_usage"),    F("power"),       setHomeAssistant, false);
    sendHomeAssistantConfig(F("L3 Instant Power Current"),     F("l3_instant_power_current"),  F("current"),     setHomeAssistant, false);
    sendHomeAssistantConfig(F("L3 Voltage"),                   F("l3_voltage"),                F("voltage"),     setHomeAssistant, false);
    sendHomeAssistantConfig(F("L1 Instant Power Return"),      F("l1_instant_power_return"),   F("power"),       setHomeAssistant, false);
    sendHomeAssistantConfig(F("L2 Instant Power Return"),      F("l2_instant_power_return"),   F("power"),       setHomeAssistant, false);
    sendHomeAssistantConfig(F("L3 Instant Power Return"),      F("l3_instant_power_return"),   F("power"),       setHomeAssistant, false);
    sendHomeAssistantConfig(F("Actual Power Consumption"),     F("actual_consumption"),        F("power"),       setHomeAssistant, false);
    sendHomeAssistantConfig(F("Actual Return Delivery"),       F("actual_returndelivery"),     F("power"),       setHomeAssistant, false);
    sendHomeAssistantConfig(F("Gas Usage"),                    F("gas_meter_m3"),              F("gas"),         setHomeAssistant, false);
    sendHomeAssistantConfig(F("Consumption High Tariff"),      F("consumption_high_tarif"),    F("energy"),      setHomeAssistant, false);
    sendHomeAssistantConfig(F("Consumption Low Tariff"),       F("consumption_low_tarif"),     F("energy"),      setHomeAssistant, false);
    sendHomeAssistantConfig(F("Return Delivery High Tariff"),  F("returndelivery_high_tarif"), F("energy"),      setHomeAssistant, false);
    sendHomeAssistantConfig(F("Return Delivery Low Tariff"),   F("returndelivery_low_tarif"),  F("energy"),      setHomeAssistant, false);
    sendHomeAssistantConfig(F("Long Power Outages"),           F("long_power_outages"),        F("outages"),     setHomeAssistant, false);
    sendHomeAssistantConfig(F("Short Power Outages"),          F("short_power_outages"),       F("outages"),     setHomeAssistant, false);
    sendHomeAssistantConfig(F("Short Power Drops"),            F("short_power_drops"),         F("outages"),     setHomeAssistant, false);
    sendHomeAssistantConfig(F("Short Power Peaks"),            F("short_power_peaks"),         F("outages"),     setHomeAssistant, false);
    sendHomeAssistantConfig(F("Actual Tariff Group"),          F("actual_tarif_group"),        F("tarif"),       setHomeAssistant, false);
    sendHomeAssistantConfig(F("DSRM Version"),                 F("dsrm_version"),              F("other"),       setHomeAssistant, false);
    sendHomeAssistantConfig(F("DSRM DateTime"),                F("dsrm_datetime"),             F("date"),        setHomeAssistant, false);
    sendHomeAssistantConfig(F("DSRM Equipment ID"),            F("dsrm_equipment_id"),         F("equipementid"),setHomeAssistant, false);
    sendHomeAssistantConfig(F("GAS Meter Type"),               F("gas_meter_type"),            F("gasdevice"),   setHomeAssistant, false);
    sendHomeAssistantConfig(F("GAS Equipment ID"),             F("gas_equipment_id"),          F("gasdevice"),   setHomeAssistant, false);
    sendHomeAssistantConfig(F("Reset Reason"),                 F("reset_reason"),              F("other"),       setHomeAssistant, true);
    sendHomeAssistantConfig(F("Crash Count"),                  F("crash_count"),               F("other"),       setHomeAssistant, true);
    sendHomeAssistantConfig(F("Safe Mode"),                    F("safe_mode"),                 F("other"),       setHomeAssistant, true);
    sendHomeAssistantConfig(F("IP Address"),                   F("ip_address"),                F("ipaddress"),   setHomeAssistant, false);

    // v2.7 had "Update Available" en "Latest Version" sensors: retained config en state opruimen
    static const char* const oldSensors[] = {"update_available", "latest_version"};
    for (const char* id : oldSensors) {
        String base = String(MQTT_ROOT_TOPIC) + "/" + id;
        mqtt_client.publish((base + "/config").c_str(), "", true);
        mqtt_client.publish((base + "/state").c_str(), "", true);
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
// Diagnose naar MQTT, na elke (her)verbinding. Alles retained, behalve het event.
void publishDiagnostics() {
    char t[64];
    char v[16];
    snprintf(t, sizeof(t), "%s/reset_reason/state", MQTT_ROOT_TOPIC);
    mqtt_client.publish(t, bootReasonText().c_str(), true);
    snprintf(t, sizeof(t), "%s/crash_count/state", MQTT_ROOT_TOPIC);
    snprintf(v, sizeof(v), "%u", bootState.crashCount);
    mqtt_client.publish(t, v, true);
    snprintf(t, sizeof(t), "%s/safe_mode/state", MQTT_ROOT_TOPIC);
    mqtt_client.publish(t, safeMode ? "ON" : "OFF", true);

    // Crash details (ook als attributen van de "Reset Reason" sensor in HA)
    String js = "{";
    { StreamString ss; printCrashJson(ss); js += ss; }
    js += F(",\"version\":\"" APPVERSION "\",\"ip\":\"");
    js += IPADDRESS;
    js += "\"}";
    snprintf(t, sizeof(t), "%s/crash/state", MQTT_ROOT_TOPIC);
    mqtt_client.publish(t, js.c_str(), true);

    // Eenmalig per opstart een event (niet retained) voor HA automations met een MQTT trigger
    static bool eventSent = false;
    if (!eventSent) {
        eventSent = true;
        String ev = F("{\"event\":\"");
        ev += safeMode ? F("safe_mode") : (crashedThisBoot ? F("crash") : F("boot"));
        ev += F("\",\"reason\":\"");
        ev += bootReasonText();
        ev += F("\",\"crash_count\":");
        ev += bootState.crashCount;
        ev += F(",\"version\":\"" APPVERSION "\"}");
        snprintf(t, sizeof(t), "%s/event", MQTT_ROOT_TOPIC);
        mqtt_client.publish(t, ev.c_str(), false);
    }
}

bool mqtt_reconnect() {
    DEBUG_PRINTLN("Attempting MQTT reconnect...");
    if (mqtt_client.connect(HOSTNAME, MQTT_USER, MQTT_PASS, statusTopic, 1, true, "offline")) {
        DEBUG_PRINTLN(F("MQTT connected!"));
        stats.mqttReconnects++;
        mqtt_client.publish(statusTopic, safeMode ? "offline" : "online", true);
        publishDiagnostics();
        return true;
    } else {
        DEBUG_PRINT(F("MQTT Connection failed: rc="));
        DEBUG_PRINTLN(mqtt_client.state());
        return false;
    }
}

bool send_mqtt_message(const char *topic, const char *payload) {
    DEBUG_PRINTF("MQTT Outgoing on %s: ", topic);
    DEBUG_PRINTLN(payload);
    bool result = mqtt_client.publish(topic, payload, false);
    if (result) stats.mqttPublishes++;
    else { stats.mqttFailures++; DEBUG_PRINTF("MQTT publish to topic %s failed\n", topic); }
    return result;
}

bool send_metric(const char *name, const char *metric) {
    DEBUG_PRINTF("Sending metric: %s=%s\n", name, metric);
    char topicBuf[96];
    snprintf(topicBuf, sizeof(topicBuf), "%s/%s/state", MQTT_ROOT_TOPIC, name);
    return send_mqtt_message(topicBuf, metric);
}

bool send_metric(const String& name, const String& metric) {
    return send_metric(name.c_str(), metric.c_str());
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
                snprintf(valueBuf, sizeof(valueBuf), "%ld", current);
                // Alleen onthouden als het echt verstuurd is, anders volgende keer opnieuw
                if (send_metric(metricsFields[i].name, valueBuf)) prevLongValues[i] = current;
                sent++;
            }
        } else {
            const char* current = metricsFields[i].stringValue;
            uint32_t h = 2166136261UL;                       // FNV-1a
            for (const char *c = current; *c; c++) { h ^= (uint8_t)*c; h *= 16777619UL; }
            if (forceAll || h != prevStringHash[i]) {
                if (send_metric(metricsFields[i].name, current)) prevStringHash[i] = h;
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

// Ruw telegram opvangen (alleen als er om gevraagd is)
void rawAppend(const char *data, int len, bool isStart) {
    if (rawState == RAW_WAIT && isStart) {
        if (!rawBuf) rawBuf = (char*)malloc(FULL_TELEGRAM_SIZE);
        if (!rawBuf) { rawState = RAW_IDLE; return; }
        rawLen = 0;
        rawState = RAW_CAPTURING;
    }
    if (rawState != RAW_CAPTURING) return;
    if (rawLen + len < FULL_TELEGRAM_SIZE) {
        memcpy(rawBuf + rawLen, data, len);
        rawLen += len;
        rawBuf[rawLen] = 0;
    }
}

void rawFree() {
    rawState = RAW_IDLE;
    if (rawBuf) { free(rawBuf); rawBuf = nullptr; }
    rawLen = 0;
}

// Leest beschikbare regels van de P1 poort. Wordt ELKE loop aangeroepen, zodat de
// 1KB RX buffer nooit overloopt (ook niet bij een interval van 60 s). Of er naar
// MQTT gestuurd wordt bepaalt validateTelegram() op basis van het interval.
//
// De regelbuffer is klein (P1_MAXLINELENGTH). Een langere regel (bv. een tekstbericht
// 0-0:96.13.0 of de storingslog 1-0:99.97.0) wordt in stukken gelezen: die stukken tellen
// mee voor de CRC maar worden niet uitgelezen. Zo blijft de CRC kloppen.
void read_p1_hardwareserial() {
    while (Serial.available()) {
        ESP.wdtDisable();
        int len = Serial.readBytesUntil('\n', telegram, P1_MAXLINELENGTH);
        ESP.wdtEnable(1);

        const bool complete   = len < P1_MAXLINELENGTH;   // '\n' gevonden
        const bool partOfLong = lineContinues || !complete;
        const bool isStart    = !lineContinues && len > 0 && telegram[0] == '/';
        lineContinues = !complete;
        if (complete) telegram[len++] = '\n';
        telegram[len] = 0;

        if (rawState == RAW_WAIT || rawState == RAW_CAPTURING) rawAppend(telegram, len, isStart);
        yield();

        if (partOfLong) {
            // Stuk van een lange regel: alleen CRC bijwerken (bevat geen '/' of '!')
            currentCRC = CRC16(currentCRC, (unsigned char*)telegram, len);
            continue;
        }
        if (validateTelegram(len)) return;
    }
}

bool validateTelegram(int len) {
    const bool isEnd = memchr(telegram, '!', len) != nullptr;
    bool result = decode_telegram(len);
    // Ongeldig telegram tijdens opvangen: opnieuw proberen met het volgende
    if (!result && isEnd && rawState == RAW_CAPTURING) rawState = RAW_WAIT;
    if (result) {
        const unsigned long now = millis();
        stats.telegramsOk++;
        stats.lastTelegramAt = now;

        if (rawState == RAW_CAPTURING) { rawState = RAW_READY; rawStateAt = now; }

        if (mqtt_client.connected() &&
            (LAST_UPDATE_SENT == 0 || now - LAST_UPDATE_SENT + INTERVAL_TOLERANCE >= updateInterval)) {
            LAST_UPDATE_SENT = now;
            send_data_to_broker();
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
    f.printf_P(PSTR("    \"ip_address\": \"%s\",\n"), IPADDRESS);
    f.print(F("    \"dsrm_version\": "));      printJsonString(f, DSRMVERSION);   f.println(',');
    f.print(F("    \"dsrm_timestamp\": "));    printJsonString(f, DSRMTIMESTAMP); f.println(',');
    f.print(F("    \"dsrm_equipment_id\": ")); printJsonString(f, EQUIPMENTID);   f.println();
    f.println(F("  },"));
    f.println(F("  \"readings\": {"));
    f.printf_P(PSTR("    \"consumption_low_tarif\": { \"value\": %.3f, \"unit\": \"kWh\" },\n"), CONSUMPTION_LOW_TARIF / 1000.00);
    f.printf_P(PSTR("    \"consumption_high_tarif\": { \"value\": %.3f, \"unit\": \"kWh\" },\n"), CONSUMPTION_HIGH_TARIF / 1000.00);
    f.printf_P(PSTR("    \"return_low_tarif\": { \"value\": %.3f, \"unit\": \"kWh\" },\n"), RETURNDELIVERY_LOW_TARIF / 1000.00);
    f.printf_P(PSTR("    \"return_high_tarif\": { \"value\": %.3f, \"unit\": \"kWh\" }\n"), RETURNDELIVERY_HIGH_TARIF / 1000.00);
    f.println(F("  },"));
    f.println(F("  \"actuals\": {"));
    f.printf_P(PSTR("    \"actual_consumption\": { \"value\": %.3f, \"unit\": \"kW\" },\n"), ACTUAL_CONSUMPTION / 1000.00);
    f.printf_P(PSTR("    \"actual_return\": { \"value\": %.3f, \"unit\": \"kW\" },\n"), ACTUAL_RETURNDELIVERY / 1000.00);
    f.printf_P(PSTR("    \"tarif\": %ld\n"), ACTUAL_TARIF_GROUP);
    f.println(F("  },"));
    long *phases[3][4] = {
        {&L1_INSTANT_POWER_USAGE, &L1_INSTANT_POWER_RETURN, &L1_INSTANT_POWER_CURRENT, &L1_VOLTAGE},
        {&L2_INSTANT_POWER_USAGE, &L2_INSTANT_POWER_RETURN, &L2_INSTANT_POWER_CURRENT, &L2_VOLTAGE},
        {&L3_INSTANT_POWER_USAGE, &L3_INSTANT_POWER_RETURN, &L3_INSTANT_POWER_CURRENT, &L3_VOLTAGE},
    };
    for (int p = 0; p < 3; p++) {
        f.printf_P(PSTR("  \"l%d\": {\n"), p + 1);
        f.printf_P(PSTR("    \"power\": { \"value\": %.3f, \"unit\": \"kW\" },\n"), *phases[p][0] / 1000.00);
        f.printf_P(PSTR("    \"power_return\": { \"value\": %.3f, \"unit\": \"kW\" },\n"), *phases[p][1] / 1000.00);
        f.printf_P(PSTR("    \"current\": { \"value\": %ld, \"unit\": \"A\" },\n"), *phases[p][2] / 1000);
        f.printf_P(PSTR("    \"voltage\": { \"value\": %ld, \"unit\": \"V\" }\n"), *phases[p][3] / 1000);
        f.println(F("  },"));
    }
    f.println(F("  \"gas\": {"));
    f.printf_P(PSTR("    \"gas_delivered\": { \"value\": %.3f, \"unit\": \"m3\" },\n"), GAS_METER_M3 / 1000.00);
    f.print(F("    \"gas_equipment_id\": ")); printJsonString(f, GASEQUIPMENTID); f.println(',');
    f.print(F("    \"gas_meter_type\": "));   printJsonString(f, GASMETERTYPE);   f.println();
    f.println(F("  },"));
    f.println(F("  \"status\": {"));
    f.printf_P(PSTR("    \"short_power_outage\": %ld,\n"), SHORT_POWER_OUTAGES);
    f.printf_P(PSTR("    \"long_power_outage\": %ld,\n"), LONG_POWER_OUTAGES);
    f.printf_P(PSTR("    \"short_power_drops\": %ld,\n"), SHORT_POWER_DROPS);
    f.printf_P(PSTR("    \"short_power_peaks\": %ld\n"), SHORT_POWER_PEAKS);
    f.println(F("  },"));
    f.println(F("  \"rawtelegram\": ["));
    // Alleen gevuld als er recent een ruw telegram is opgevangen via /api/raw (debug)
    bool firstLine = true;
    if (rawState == RAW_READY && rawBuf) {
        int i = 0;
        while (i < rawLen) {
            int start = i;
            while (i < rawLen && rawBuf[i] != '\n') i++;
            int end = i;
            while (end > start && rawBuf[end - 1] == '\r') end--;
            i++;
            if (end == start) continue;
            if (!firstLine) f.println(',');
            firstLine = false;
            f.print(F("    \""));
            for (int k = start; k < end; k++) {
                char c = rawBuf[k];
                if (c == '"' || c == '\\') f.print('\\');
                f.print(c);
            }
            f.print('"');
        }
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

    // JSON API voor de pagina's
    httpRestServer.on("/api/live", HTTP_GET, handleApiLive);
    httpRestServer.on("/api/raw", HTTP_GET, handleApiRaw);
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
    httpRestServer.on("/update/upload", HTTP_POST, handleUpdateUploadDone, handleUpdateUpload);
    // /update pas NA de /update/* routes: de server matcht "/update" ook als prefix van "/update/..."
    httpRestServer.on("/update", HTTP_GET, handlePage);
    httpRestServer.on("/update/", HTTP_GET, handlePage);

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
    crashguardBoot();   // als eerste: crash herkennen en eventueel veilige modus kiezen
    EEPROM.begin(256);   // hoogste gebruikte adres is 173; buffer staat permanent in RAM
    Serial.setRxBufferSize(2048);   // ruim 2 s aan P1 data: overleeft een blokkerende MQTT connect
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

    // Geen opgeslagen WiFi (bijv. na een build met andere Flash Size)? Eerst de reserve
    // gegevens uit secrets.h proberen. Lukt dat, dan worden ze opgeslagen (persistent) en
    // is WiFiManager niet nodig: de ESP herstelt zichzelf, ook op afstand.
    if (WIFI_FALLBACK_SSID[0] != '\0' && WiFi.SSID().length() == 0) {
        DEBUG_PRINTLN(F("Geen opgeslagen WiFi, probeer reserve WiFi uit secrets.h..."));
        WiFi.begin(WIFI_FALLBACK_SSID, WIFI_FALLBACK_PASS);
        const unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) delay(250);
    }

    // Setup access point (bij geen WiFi): optioneel met wachtwoord, zodat niet iedereen
    // in de buurt je WiFi/MQTT kan wijzigen als je router even uit staat.
    bool wifiOk;
    if (WIFI_AP_PASSWORD[0] != '\0') {
        char apName[24];
        snprintf(apName, sizeof(apName), "P1meter-%06X", ESP.getChipId());
        wifiOk = wifiManager.autoConnect(apName, WIFI_AP_PASSWORD);
    } else {
        wifiOk = wifiManager.autoConnect();
    }
    if (!wifiOk) {
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
    snprintf(statusTopic, sizeof(statusTopic), "%s/status", MQTT_ROOT_TOPIC);
    mqtt_client.setBufferSize(1024);   // grootste HA discovery bericht is ~675 bytes
    // Kortere timeouts: een onbereikbare broker mag de P1 uitlezing niet lang blokkeren
    mqtt_client.setSocketTimeout(5);
    espClient.setTimeout(2000);

    LittleFS.begin();
    // Oude telegram-dump (v2.7 schreef deze elke cyclus naar flash) opruimen
    if (LittleFS.exists("/latest_telegram.json")) LittleFS.remove("/latest_telegram.json");

    setup_webserver();

    updateIpAddress();
}

// **********************************
// * Loop                           *
// **********************************
void loop() {
    const unsigned long loopStart = micros();
    const unsigned long now = millis();

    // Langste loop-duur bijhouden (venster van 10 s), gemeten van start tot start:
    // telt ook iteraties mee die vroeg stoppen (WiFi/MQTT reconnect) en tijd in de WiFi stack.
    static unsigned long prevLoopStart = 0;
    static unsigned long maxWindowStart = 0;
    static unsigned long maxInWindow = 0;
    if (prevLoopStart) {
        unsigned long dur = loopStart - prevLoopStart;
        if (dur > maxInWindow) maxInWindow = dur;
    }
    prevLoopStart = loopStart;
    if (now - maxWindowStart >= 10000) {
        stats.maxLoopUs = maxInWindow;
        maxInWindow = 0;
        maxWindowStart = now;
    }

    // Uptime rollover bijhouden
    if (now < lastMillis) uptimeRollovers++;
    lastMillis = now;

    crashguardLoop((uint32_t)(uptimeMs() / 1000ULL));

    // P1 poort altijd leegtrekken — ook als WiFi/MQTT weg is. In veilige modus niet.
    if (!safeMode) read_p1_hardwareserial();

    ArduinoOTA.handle();
    MDNS.update();

    // Ruw telegram (debug): niet eindeloos geheugen vasthouden
    if (rawState != RAW_IDLE) {
        unsigned long limit = (rawState == RAW_READY) ? RAW_KEEP_TIMEOUT : RAW_WAIT_TIMEOUT;
        if (millis() - rawStateAt > limit) rawFree();
    }

    if (setRestart && millis() - restartInitiated >= restartDelayMs) {
        ESP.restart();
    }

    if (setHaDOn)  { setHaDOn  = false; handleHaDiscoveryOn();  }
    if (setHaDOff) { setHaDOff = false; handleHaDiscoveryOff(); }

    // --- WiFi check & reconnect ---
    static bool wifiWasConnected = true;
    static unsigned long wifiLostAt = 0;
    if (WiFi.status() != WL_CONNECTED) {
        if (wifiWasConnected) {
            wifiLostAt = now;
            LAST_WIFI_RECONNECT_ATTEMPT = now;   // eerst de automatische reconnect een kans geven
        }
        wifiWasConnected    = false;
        haDiscoverySetupRun = false;
        deltaInitialized    = false;
        // Te lang geen WiFi: herstart (lost vastgelopen WiFi stack op)
        if (now - wifiLostAt > WIFI_RESTART_AFTER) {
            DEBUG_PRINTLN(F("WiFi te lang weg, herstart..."));
            if (safeMode) crashguardForceSafeNextBoot();   // veilige modus niet stilletjes verlaten
            ESP.restart();
        }
        if (now - LAST_WIFI_RECONNECT_ATTEMPT > WIFI_RECONNECT_INTERVAL) {
            LAST_WIFI_RECONNECT_ATTEMPT = now;
            stats.wifiReconnects++;
            DEBUG_PRINTLN(F("WiFi verbroken, probeer opnieuw te verbinden..."));
            // Geen WiFi.disconnect(): die wist bij persistent(true) de opgeslagen WiFi gegevens!
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
        if (now - LAST_RECONNECT_ATTEMPT > MQTT_RECONNECT_INTERVAL) {
            LAST_RECONNECT_ATTEMPT = now;
            DEBUG_PRINTLN(F("MQTT verbroken, probeer opnieuw te verbinden..."));
            if (mqtt_reconnect()) {
                DEBUG_PRINTLN(F("MQTT herverbonden!"));
                if (HA_DISCOVERY && !haDiscoverySetupRun && !safeMode) {
                    haDiscovery("on");
                    haDiscoverySetupRun = true;
                }
            }
        }
        return;
    }

    mqtt_client.loop();

    if (safeMode) return;   // veilige modus: geen discovery / data, alleen status

    if (HA_DISCOVERY && !haDiscoverySetupRun) {
        haDiscovery("on");
        haDiscoverySetupRun = true;
    }
}
