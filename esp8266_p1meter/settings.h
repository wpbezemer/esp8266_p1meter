// **********************************
// * Settings                       *
// **********************************

// * Baud rate for both hardware and software
#define BAUD_RATE 115200            // do not change

// The used serial pins, note that this can only be UART0, as other serial port doesn't support inversion
// By default the UART0 serial will be used. These settings displayed here just as a reference.
// #define SERIAL_RX RX
// #define SERIAL_TX TX

// * Max telegram length
#define P1_MAXLINELENGTH 1050       // do not change
#define LINE_MAX_SIZE 128           // do not change
#define FULL_TELEGRAM_SIZE 2048     // do not change

// * Set to store received telegram
// +2: read_p1_hardwareserial() zet na de regel nog '\n' en '\0' (telegram[len] en telegram[len + 1])
char telegram[P1_MAXLINELENGTH + 2];    // do not change
char fullTelegram[FULL_TELEGRAM_SIZE];  // do not change
int fullLen = 0;                    // do not change

// * The version of tedzor
#define APPVERSION "2.8.0"            // do not change

// * Wifi timeout in milliseconds
#define WIFI_TIMEOUT 30000          // do not change

// * MQTT network settings
#define MQTT_MAX_RECONNECT_TRIES 10 // do not change

// * API Port
#define HTTP_REST_PORT 80           // do not change

#define CONFIG_ASYNC_TCP_RUN_IN_IRAM 0

// * MQTT Last reconnection counter
unsigned long LAST_RECONNECT_ATTEMPT = 0;      // do not change
unsigned long LAST_UPDATE_SENT = 0;            // do not change
unsigned long LAST_WIFI_RECONNECT_ATTEMPT = 0; // do not change

// * To be filled with EEPROM data
char MQTT_HOST[64] = "";            // do not change
char MQTT_PORT[6]  = "";            // do not change
char MQTT_USER[32] = "";            // do not change
char MQTT_PASS[32] = "";            // do not change
bool HA_DISCOVERY = false;          // do not change

// * Set during CRC checking
unsigned int currentCRC = 0;        // do not change

// * Set to store full mqtt topic
String topic;                       // do not change

// * Set home assistant discovery for device startup
bool haDiscoverySetupRun = false;   // do not change
bool setHaDOn = false;              // do not change
bool setHaDOff = false;             // do not change

// * Restart delay Settings
const long restartInterval = 5000;  // 5 seconds delay before restart
unsigned long restartInitiated = 0; // do not change
bool setRestart = false;            // do not change

struct SensorConfig {
    const char* input_name;        // Jouw deviceclass in code
    const char* ha_device_class;   // Officiële HA device_class of "none"
    const char* state_class;       // "measurement", "total_increasing", "none"
    const char* unit;              // Unit of ""
    const char* icon;              // Design icon
    bool divide_by_1000;           // true = deel waarde door 1000
};

struct TelegramField {
    const char* key;               // Start of line
    long* target;                  // Variable
    char stopChar;                 // End character of line
};

struct TelegramStringField {
    const char* key;               // Start of line
    char* target;                  // Variable
    size_t maxLen;                 // maxLenght of line ny sizeof(GASEQUIPMENTID)
    char stopChar;                 // End character of line
};

enum MetricType {
    TYPE_LONG,
    TYPE_STRING
};

struct MetricField {
    const char* name;               // Variable_name to be send with MQTT
    MetricType type;                // datatype
    union {
        const long* longValue;      // Value
        const char* stringValue;    // Value
    };
};
