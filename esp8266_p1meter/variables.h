#pragma once
// **********************************
// * Gebruikersinstellingen         *
// **********************************

// * Standaard update interval (ms) bij eerste flash — daarna via /interval/set of /settings
#define UPDATE_INTERVAL 60000UL

// * Reconnect intervallen
#define WIFI_RECONNECT_INTERVAL (15UL * 60UL * 1000UL)  // 15 minuten
#define MQTT_RECONNECT_INTERVAL 5000UL                 // 5 seconden

// * Hostname (mDNS + OTA)
#define HOSTNAME "p1meter"

// * Wachtwoord voor Arduino OTA — VERANDER DIT
#define OTA_PASSWORD "admin"

// * Optionele login voor de webpagina's (leeg = geen login, zoals voorheen)
//   Beschermt o.a. firmware upload, bestandsbeheer en instellingen.
#define WEB_USER ""
#define WEB_PASS ""

// * GitHub repo voor versiecheck en firmware download
#define GITHUB_USER "wpbezemer"
#define GITHUB_REPO "esp8266_p1meter"

// * Standaard OTA versiecheck interval (uren), 0 = uit
#define OTA_CHECK_HOURS_DEFAULT 6

// **********************************
// * Runtime variabelen             *
// **********************************

// * MQTT root topic
char MQTT_ROOT_TOPIC[32] = "homeassistant/sensor/p1meter";

// * Timers
unsigned long LAST_RECONNECT_ATTEMPT      = 0;
unsigned long LAST_WIFI_RECONNECT_ATTEMPT = 0;
unsigned long LAST_UPDATE_SENT            = 0;
unsigned long LAST_OTA_CHECK              = 0;
unsigned long otaCheckInterval            = (unsigned long)OTA_CHECK_HOURS_DEFAULT * 3600UL * 1000UL;

// * Uit EEPROM
char MQTT_HOST[64] = "";
char MQTT_PORT[6]  = "1883";
char MQTT_USER[32] = "";
char MQTT_PASS[32] = "";
bool HA_DISCOVERY  = false;
bool AUTO_UPDATE   = false;

// * Versiecheck
char LATEST_VERSION[16] = "";
bool UPDATE_AVAILABLE   = false;

// * Telegram buffers
char telegram[P1_MAXLINELENGTH + 2];
char fullTelegram[FULL_TELEGRAM_SIZE];
int  fullLen = 0;

// * Uitgelezen waarden (x1000 voor waarden met eenheid)
long CONSUMPTION_LOW_TARIF;
long CONSUMPTION_HIGH_TARIF;
long RETURNDELIVERY_LOW_TARIF;
long RETURNDELIVERY_HIGH_TARIF;
long ACTUAL_CONSUMPTION;
long ACTUAL_RETURNDELIVERY;
long GAS_METER_M3;
long L1_INSTANT_POWER_USAGE;
long L2_INSTANT_POWER_USAGE;
long L3_INSTANT_POWER_USAGE;
long L1_INSTANT_POWER_RETURN;
long L2_INSTANT_POWER_RETURN;
long L3_INSTANT_POWER_RETURN;
long L1_INSTANT_POWER_CURRENT;
long L2_INSTANT_POWER_CURRENT;
long L3_INSTANT_POWER_CURRENT;
long L1_VOLTAGE;
long L2_VOLTAGE;
long L3_VOLTAGE;
long ACTUAL_TARIF_GROUP;
long SHORT_POWER_OUTAGES;
long LONG_POWER_OUTAGES;
long SHORT_POWER_DROPS;
long SHORT_POWER_PEAKS;

char DSRMVERSION[8]     = "";
char DSRMTIMESTAMP[16]  = "";
char EQUIPMENTID[48]    = "";
char GASEQUIPMENTID[48] = "";
char GASMETERTYPE[8]    = "";
char IPADDRESS[16]      = "0.0.0.0";

// * CRC (niet wijzigen)
unsigned int currentCRC = 0;

// * Volledige MQTT topic
String topic;

// * Home Assistant discovery
bool haDiscoverySetupRun = false;
bool setHaDOn  = false;
bool setHaDOff = false;

// * Restart
const unsigned long restartInterval = 5000;
unsigned long restartInitiated = 0;
bool setRestart = false;

// **********************************
// * Telegram veld definities       *
// **********************************
struct TelegramField {
    const char* key;
    long*       target;
    char        stopChar;
};

const TelegramField fields[] = {
    {"1-0:1.8.1",   &CONSUMPTION_LOW_TARIF,     '*'},
    {"1-0:1.8.2",   &CONSUMPTION_HIGH_TARIF,    '*'},
    {"1-0:2.8.1",   &RETURNDELIVERY_LOW_TARIF,  '*'},
    {"1-0:2.8.2",   &RETURNDELIVERY_HIGH_TARIF, '*'},
    {"1-0:1.7.0",   &ACTUAL_CONSUMPTION,        '*'},
    {"1-0:2.7.0",   &ACTUAL_RETURNDELIVERY,     '*'},
    {"1-0:21.7.0",  &L1_INSTANT_POWER_USAGE,    '*'},
    {"1-0:41.7.0",  &L2_INSTANT_POWER_USAGE,    '*'},
    {"1-0:61.7.0",  &L3_INSTANT_POWER_USAGE,    '*'},
    {"1-0:22.7.0",  &L1_INSTANT_POWER_RETURN,   '*'},
    {"1-0:42.7.0",  &L2_INSTANT_POWER_RETURN,   '*'},
    {"1-0:62.7.0",  &L3_INSTANT_POWER_RETURN,   '*'},
    {"1-0:31.7.0",  &L1_INSTANT_POWER_CURRENT,  '*'},
    {"1-0:51.7.0",  &L2_INSTANT_POWER_CURRENT,  '*'},
    {"1-0:71.7.0",  &L3_INSTANT_POWER_CURRENT,  '*'},
    {"1-0:32.7.0",  &L1_VOLTAGE,                '*'},
    {"1-0:52.7.0",  &L2_VOLTAGE,                '*'},
    {"1-0:72.7.0",  &L3_VOLTAGE,                '*'},
    {"0-1:24.2.1",  &GAS_METER_M3,              '*'},
    {"0-0:96.14.0", &ACTUAL_TARIF_GROUP,        ')'},
    {"0-0:96.7.21", &SHORT_POWER_OUTAGES,       ')'},
    {"0-0:96.7.9",  &LONG_POWER_OUTAGES,        ')'},
    {"1-0:32.32.0", &SHORT_POWER_DROPS,         ')'},
    {"1-0:32.36.0", &SHORT_POWER_PEAKS,         ')'},
};

struct TelegramStringField {
    const char* key;
    char*       target;
    char        stopChar;
    int         maxLen;
};

const TelegramStringField stringFields[] = {
    {"1-3:0.2.8",  DSRMVERSION,    ')', sizeof(DSRMVERSION)},
    {"0-0:1.0.0",  DSRMTIMESTAMP,  ')', sizeof(DSRMTIMESTAMP)},
    {"0-0:96.1.1", EQUIPMENTID,    ')', sizeof(EQUIPMENTID)},
    {"0-1:96.1.0", GASEQUIPMENTID, ')', sizeof(GASEQUIPMENTID)},
    {"0-1:24.1.0", GASMETERTYPE,   ')', sizeof(GASMETERTYPE)},
};

// **********************************
// * MQTT metrics                   *
// **********************************
enum MetricType { TYPE_LONG, TYPE_STRING };

struct MetricField {
    const char* name;
    MetricType  type;
    long*       longValue;
    const char* stringValue;
};

const MetricField metricsFields[] = {
    {"consumption_low_tarif",     TYPE_LONG,   &CONSUMPTION_LOW_TARIF,     nullptr},
    {"consumption_high_tarif",    TYPE_LONG,   &CONSUMPTION_HIGH_TARIF,    nullptr},
    {"returndelivery_low_tarif",  TYPE_LONG,   &RETURNDELIVERY_LOW_TARIF,  nullptr},
    {"returndelivery_high_tarif", TYPE_LONG,   &RETURNDELIVERY_HIGH_TARIF, nullptr},
    {"actual_consumption",        TYPE_LONG,   &ACTUAL_CONSUMPTION,        nullptr},
    {"actual_returndelivery",     TYPE_LONG,   &ACTUAL_RETURNDELIVERY,     nullptr},
    {"l1_instant_power_usage",    TYPE_LONG,   &L1_INSTANT_POWER_USAGE,    nullptr},
    {"l2_instant_power_usage",    TYPE_LONG,   &L2_INSTANT_POWER_USAGE,    nullptr},
    {"l3_instant_power_usage",    TYPE_LONG,   &L3_INSTANT_POWER_USAGE,    nullptr},
    {"l1_instant_power_return",   TYPE_LONG,   &L1_INSTANT_POWER_RETURN,   nullptr},
    {"l2_instant_power_return",   TYPE_LONG,   &L2_INSTANT_POWER_RETURN,   nullptr},
    {"l3_instant_power_return",   TYPE_LONG,   &L3_INSTANT_POWER_RETURN,   nullptr},
    {"l1_instant_power_current",  TYPE_LONG,   &L1_INSTANT_POWER_CURRENT,  nullptr},
    {"l2_instant_power_current",  TYPE_LONG,   &L2_INSTANT_POWER_CURRENT,  nullptr},
    {"l3_instant_power_current",  TYPE_LONG,   &L3_INSTANT_POWER_CURRENT,  nullptr},
    {"l1_voltage",                TYPE_LONG,   &L1_VOLTAGE,                nullptr},
    {"l2_voltage",                TYPE_LONG,   &L2_VOLTAGE,                nullptr},
    {"l3_voltage",                TYPE_LONG,   &L3_VOLTAGE,                nullptr},
    {"gas_meter_m3",              TYPE_LONG,   &GAS_METER_M3,              nullptr},
    {"actual_tarif_group",        TYPE_LONG,   &ACTUAL_TARIF_GROUP,        nullptr},
    {"short_power_outages",       TYPE_LONG,   &SHORT_POWER_OUTAGES,       nullptr},
    {"long_power_outages",        TYPE_LONG,   &LONG_POWER_OUTAGES,        nullptr},
    {"short_power_drops",         TYPE_LONG,   &SHORT_POWER_DROPS,         nullptr},
    {"short_power_peaks",         TYPE_LONG,   &SHORT_POWER_PEAKS,         nullptr},
    {"dsrm_version",              TYPE_STRING, nullptr,                    DSRMVERSION},
    {"dsrm_datetime",             TYPE_STRING, nullptr,                    DSRMTIMESTAMP},
    {"dsrm_equipment_id",         TYPE_STRING, nullptr,                    EQUIPMENTID},
    {"gas_meter_type",            TYPE_STRING, nullptr,                    GASMETERTYPE},
    {"gas_equipment_id",          TYPE_STRING, nullptr,                    GASEQUIPMENTID},
    {"ip_address",                TYPE_STRING, nullptr,                    IPADDRESS},
};

// **********************************
// * Home Assistant sensor types    *
// **********************************
struct SensorConfig {
    const char* input_name;
    const char* ha_device_class;
    const char* state_class;
    const char* unit;
    const char* icon;
    bool        divide_by_1000;
};

const SensorConfig sensorConfigs[] = {
    {"power",        "power",   "measurement",      "kW",   "mdi:flash",            true },
    {"current",      "current", "measurement",      "A",    "mdi:current-ac",       true },
    {"voltage",      "voltage", "measurement",      "V",    "mdi:sine-wave",        true },
    {"energy",       "energy",  "total_increasing", "kWh",  "mdi:counter",          true },
    {"gas",          "gas",     "total_increasing", "m³",   "mdi:fire",             true },
    {"outages",      "none",    "total_increasing", "none", "mdi:flash-off",        false},
    {"tarif",        "none",    "none",             "none", "mdi:cash",             false},
    {"date",         "none",    "none",             "none", "mdi:calendar-clock",   false},
    {"equipementid", "none",    "none",             "none", "mdi:identifier",       false},
    {"gasdevice",    "none",    "none",             "none", "mdi:meter-gas",        false},
    {"ipaddress",    "none",    "none",             "none", "mdi:ip-network",       false},
    {"other",        "none",    "none",             "none", "mdi:information",      false},
};
