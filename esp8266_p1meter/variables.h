// **********************************
// * variables                      *
// **********************************

// Treshold in milliseconds for reading telegram message
#define UPDATE_INTERVAL 60000  // 1 minute
//#define UPDATE_INTERVAL 300000 // 5 minutes

// Treshold in milliseconds for WiFi reconnect interval
#define WIFI_RECONNECT_INTERVAL (15UL * 60UL * 1000UL)  // 15 minuten

// Treshold in miliseconds for MQTT reconnect interval
#define MQTT_RECONNECT_INTERVAL 5000

// * The hostname of our little creature
#define HOSTNAME "p1meter"

// * GitHub repo — alleen gebruikt door de browser op /update om de laatste release te tonen
//   (de ESP zelf maakt geen verbinding met GitHub)
#define GITHUB_USER         "wpbezemer"
#define GITHUB_REPO         "esp8266_p1meter"

// * MQTT root topic
char MQTT_ROOT_TOPIC[32] = "homeassistant/sensor/p1meter";

// * The password used for OTA
#define OTA_PASSWORD "admin"

// * Optionele login voor de webpagina's (leeg = geen login)
//   Beschermt pagina's, instellingen, firmware upload, bestanden en restart.
#define WEB_USER ""
#define WEB_PASS ""

// * Set to store the data values read
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
long L1_INSTANT_POWER_CURRENT;
long L2_INSTANT_POWER_CURRENT;
long L3_INSTANT_POWER_CURRENT;
long L1_VOLTAGE;
long L2_VOLTAGE;
long L3_VOLTAGE;
long L1_INSTANT_POWER_RETURN;
long L2_INSTANT_POWER_RETURN;
long L3_INSTANT_POWER_RETURN;
long ACTUAL_TARIF_GROUP;
long SHORT_POWER_OUTAGES;
long LONG_POWER_OUTAGES;
long SHORT_POWER_DROPS;
long SHORT_POWER_PEAKS;
char DSRMVERSION[10];
char DSRMTIMESTAMP[20];
char EQUIPMENTID[64];
char GASMETERTYPE[10];
char GASEQUIPMENTID[64];
char IPADDRESS[16];

// Table of value to be retreived from telegram ****FOR LONG VALUES****. 
// {"start of telegram line", "variable name", "end character"}
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
    {"1-0:22.7.0", &L1_INSTANT_POWER_RETURN, '*'},
    {"1-0:42.7.0", &L2_INSTANT_POWER_RETURN, '*'},
    {"1-0:62.7.0", &L3_INSTANT_POWER_RETURN, '*'},
    {"0-1:24.2.1", &GAS_METER_M3, '*'},
    {"0-0:96.14.0", &ACTUAL_TARIF_GROUP, ')'},
    {"0-0:96.7.21", &SHORT_POWER_OUTAGES, ')'},
    {"0-0:96.7.9", &LONG_POWER_OUTAGES, ')'},
    {"1-0:32.32.0", &SHORT_POWER_DROPS, ')'},
    {"1-0:32.36.0", &SHORT_POWER_PEAKS, ')'},
};

// Table of value to be retreived from telegram ****FOR STRING VALUES****. 
// {"start of telegram line", "variable name", "sizeof(variable name)", "end character"}
const TelegramStringField stringFields[] = {
    {"1-3:0.2.8", DSRMVERSION, sizeof(DSRMVERSION), ')'},
    {"0-0:1.0.0", DSRMTIMESTAMP, sizeof(DSRMTIMESTAMP), ')'},
    {"0-0:96.1.1", EQUIPMENTID, sizeof(EQUIPMENTID), ')'},
    {"0-1:24.1.0", GASMETERTYPE, sizeof(GASMETERTYPE), ')'},
    {"0-1:96.1.0", GASEQUIPMENTID, sizeof(GASEQUIPMENTID), ')'},
};

// table for Home Assistant auto discovery.
// {"input-name", "HA Device-class", "HA state-class", "HA Unit-of-measurement", "HA MDI-ICON", "devide by 1000"}
const SensorConfig sensorConfigs[] = {
    { "power","power","measurement","kW","mdi:lightning-bolt",true },
    { "current","current","measurement","A","mdi:current-ac",true },
    { "voltage","voltage","measurement","V","mdi:sine-wave",true },
    { "energy","energy","total_increasing","kWh","mdi:meter-electric-outline",true },
    { "gas","gas","total_increasing","m³","mdi:meter-gas",true },
    { "tarif","none","none","none","mdi:currency-eur",false },
    { "equipementid","none","none","none","mdi:identifier",false },
    { "date","none","none","none","mdi:update",false },
    { "outages","none","none","none","mdi:transmission-tower-off",false },
    { "other","none","none","none","mdi:home-lightning-bolt",false },
    { "gasdevice","none","none","none","mdi:meter-gas",false },
    { "ipaddress", "none", "none", "none", "mdi:ip-network",false }
};

// table for sending values for MQTT.
// {"sensor-id", "data type (long or string)", ".longValue = &{variable-name} OR .stringValue = {variable-name}"}
const MetricField metricsFields[] = {
    { "consumption_low_tarif", TYPE_LONG, .longValue = &CONSUMPTION_LOW_TARIF },
    { "consumption_high_tarif", TYPE_LONG, .longValue = &CONSUMPTION_HIGH_TARIF },
    { "returndelivery_low_tarif", TYPE_LONG, .longValue = &RETURNDELIVERY_LOW_TARIF },
    { "returndelivery_high_tarif", TYPE_LONG, .longValue = &RETURNDELIVERY_HIGH_TARIF },
    { "actual_consumption", TYPE_LONG, .longValue = &ACTUAL_CONSUMPTION },
    { "actual_returndelivery", TYPE_LONG, .longValue = &ACTUAL_RETURNDELIVERY },
    { "l1_instant_power_usage", TYPE_LONG, .longValue = &L1_INSTANT_POWER_USAGE },
    { "l2_instant_power_usage", TYPE_LONG, .longValue = &L2_INSTANT_POWER_USAGE },
    { "l3_instant_power_usage", TYPE_LONG, .longValue = &L3_INSTANT_POWER_USAGE },
    { "l1_instant_power_current", TYPE_LONG, .longValue = &L1_INSTANT_POWER_CURRENT },
    { "l2_instant_power_current", TYPE_LONG, .longValue = &L2_INSTANT_POWER_CURRENT },
    { "l3_instant_power_current", TYPE_LONG, .longValue = &L3_INSTANT_POWER_CURRENT },
    { "l1_voltage", TYPE_LONG, .longValue = &L1_VOLTAGE },
    { "l2_voltage", TYPE_LONG, .longValue = &L2_VOLTAGE },
    { "l3_voltage", TYPE_LONG, .longValue = &L3_VOLTAGE },
    { "l1_instant_power_return", TYPE_LONG, .longValue = &L1_INSTANT_POWER_RETURN },
    { "l2_instant_power_return", TYPE_LONG, .longValue = &L2_INSTANT_POWER_RETURN },
    { "l3_instant_power_return", TYPE_LONG, .longValue = &L3_INSTANT_POWER_RETURN },
    { "gas_meter_m3", TYPE_LONG, .longValue = &GAS_METER_M3 },
    { "actual_tarif_group", TYPE_LONG, .longValue = &ACTUAL_TARIF_GROUP },
    { "short_power_outages", TYPE_LONG, .longValue = &SHORT_POWER_OUTAGES },
    { "long_power_outages", TYPE_LONG, .longValue = &LONG_POWER_OUTAGES },
    { "short_power_drops", TYPE_LONG, .longValue = &SHORT_POWER_DROPS },
    { "short_power_peaks", TYPE_LONG, .longValue = &SHORT_POWER_PEAKS },
    { "dsrm_version", TYPE_STRING, .stringValue = DSRMVERSION },
    { "dsrm_datetime", TYPE_STRING, .stringValue = DSRMTIMESTAMP },
    { "dsrm_equipment_id", TYPE_STRING, .stringValue = EQUIPMENTID },
    { "gas_meter_type", TYPE_STRING, .stringValue = GASMETERTYPE },
    { "gas_equipment_id", TYPE_STRING, .stringValue = GASEQUIPMENTID },
    { "ip_address", TYPE_STRING, .stringValue = IPADDRESS }
};
