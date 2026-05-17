# esp8266_p1meter

[![Release](https://img.shields.io/badge/release-v2.7.0-blue)](https://github.com/wpbezemer/esp8266_p1meter/releases/tag/v2.7.0)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-green)](LICENSE)
[![Platform: ESP8266](https://img.shields.io/badge/platform-ESP8266-orange)](https://www.espressif.com/)

Software voor de ESP8266 die DSMR5 P1 slimme meter data uitleest en verstuurt naar een MQTT broker — met OTA firmware updates via web en Arduino, WiFi- en MQTT-configuratie via WiFiManager, Home Assistant Auto Discovery, automatische versiecheck via GitHub, en een volledige REST API.

---

## Inhoudsopgave

- [Over dit project](#over-dit-project)
- [Functies](#functies)
- [Benodigdheden](#benodigdheden)
- [Hardware aansluiting](#hardware-aansluiting)
- [Installatie](#installatie)
- [Eerste configuratie](#eerste-configuratie)
- [MQTT topics](#mqtt-topics)
- [REST API](#rest-api)
- [Home Assistant Auto Discovery](#home-assistant-auto-discovery)
- [JSON Telegram endpoint](#json-telegram-endpoint)
- [OTA updates](#ota-updates)
- [Firmware update via web](#firmware-update-via-web)
- [Debug modus](#debug-modus)
- [Instellingen aanpassen](#instellingen-aanpassen)
- [Gerelateerde projecten](#gerelateerde-projecten)
- [Release notes](#release-notes)
- [Credits](#credits)

---

## Over dit project

De originele firmware had moeite met DSMR5.0 meters die elke seconde een telegram sturen op 115200 baud. SoftwareSerial kon dit niet bijhouden en ontving regelmatig corrupte berichten. Deze versie gebruikt de hardware UART (RX) poort, wat stabiel werkt op hoge baudrates.

Verder uitgebreid met een volledige REST API, Home Assistant Auto Discovery, delta detection voor efficiënte MQTT updates, een configureerbaar update interval dat tot 1 seconde kan, en een web-gebaseerde OTA updater met automatische versiecheck via GitHub.

---

## Functies

| Functie | Details |
|---------|---------|
| **DSMR5 uitlezen** | Hardware UART, stabiel op 115200 baud |
| **MQTT publicatie** | 30 topics, elk afzonderlijk |
| **Delta detection** | Stuurt alleen gewijzigde waarden — maakt 1 sec interval realistisch |
| **Configureerbaar interval** | 1–3600 seconden, via API instelbaar zonder herstart, opgeslagen in EEPROM |
| **HA Auto Discovery** | Alle sensors verschijnen automatisch in Home Assistant |
| **Per-fase teruglevering** | L1, L2, L3 teruglevering als aparte MQTT topics |
| **REST API** | 12 endpoints voor beheer, monitoring en configuratie |
| **JSON telegram endpoint** | Laatste telegram beschikbaar als JSON via HTTP |
| **Web OTA updater** | Firmware updaten via browser — GitHub download of eigen .bin upload |
| **Automatische versiecheck** | ESP checkt GitHub periodiek op nieuwe versie, publiceert naar MQTT en HA |
| **Auto-update** | Optioneel automatisch installeren bij nieuwe versie |
| **Arduino OTA** | Draadloos firmware updaten via Arduino IDE |
| **WiFiManager** | WiFi + MQTT configuratie via captive portal — geen hercompilatie nodig |
| **Stabiele reconnect** | WiFi reconnect elke 15 min, MQTT elke 5 sec, HA discovery herverstuurd na reconnect |
| **Versienummer** | Zichtbaar via Serial, REST API, web updater en Home Assistant device info |
| **DEBUG flag** | Alle Serial output achter compile-time flag — standaard uit in productie |

---

## Benodigdheden

**Hardware:**
- ESP8266 (getest: NodeMCU v1/v3, Wemos D1 Mini)
- 10kΩ weerstand
- RJ11 (4-pins) of RJ12 (6-pins) kabel — RJ12 kan de ESP ook voeden vanuit de meter

**Software / libraries (Arduino IDE):**

| Library | Installatie |
|---------|-------------|
| `PubSubClient` | Arduino Library Manager |
| `WiFiManager` | Arduino Library Manager |
| `ArduinoJson` | Arduino Library Manager |
| `ESPAsyncTCP` | Arduino Library Manager |
| `ESPAsyncWebServer` | Arduino Library Manager |
| `ESP8266HTTPClient` | Ingebouwd in ESP8266 core |
| `ESP8266httpUpdate` | Ingebouwd in ESP8266 core |
| `ArduinoOTA` | Ingebouwd in ESP8266 core |
| `LittleFS` | Ingebouwd in ESP8266 core |

---

## Hardware aansluiting

### Standaard (via USB voeding)

| P1 poort pin | ESP8266 pin | Omschrijving |
|-------------|-------------|-------------|
| 2 - RTS | 3.3V | Request To Send — hoog houden om data te ontvangen |
| 3 - GND | GND | Ground |
| 5 - RXD | RX (GPIO3) | Data lijn |

Gebruik een **10kΩ pull-up weerstand** tussen 3.3V en de DATA (RXD) pin van de P1 poort. Zonder deze weerstand is het signaal geïnverteerd en ontvangt de ESP geen data.

### Optioneel: gevoed door de meter (RJ12)

| P1 poort pin | ESP8266 pin | Omschrijving |
|-------------|-------------|-------------|
| 1 - 5V out | 5V / VIN | Voeding vanuit de meter |
| 2 - RTS | 3.3V | Request To Send |
| 3 - GND | GND | Ground |
| 5 - RXD | RX (GPIO3) | Data lijn |
| 6 - GND | GND | Ground |

> Werkt op de meeste DSMR5+ meters. Op een Landis+Gyr E360 werkt 3.3V op RTS — 5V voeding is niet noodzakelijk.

---

## Installatie

### Arduino IDE

1. Installeer de **ESP8266 board package** via **Tools → Board → Board Manager** — zoek op `esp8266`
2. Installeer alle benodigde libraries via **Tools → Manage Libraries**
3. Open `esp8266_p1meter_v2_7.ino`
4. Pas `OTA_PASSWORD` aan in `variables.h` — gebruik iets unieks, standaard is `admin`
5. Stel `GITHUB_USER` en `GITHUB_REPO` in `variables.h` in op jouw GitHub repo voor de versiecheck
6. Selecteer het juiste board: **Tools → Board → LOLIN(WEMOS) D1 R2 & mini** (of NodeMCU 1.0)
7. Selecteer de juiste COM poort
8. Klik op **Upload**

### PlatformIO

1. Open het project in PlatformIO
2. Pas `OTA_PASSWORD` aan in `variables.h`
3. Klik op **Upload**

---

## Eerste configuratie

Na het flashen start de ESP als WiFi Access Point:

1. Zoek op je telefoon of laptop naar het netwerk **`ESP******`** en verbind ermee
2. Er verschijnt automatisch een configuratiepagina — anders navigeer naar `192.168.4.1`
3. Vul in:
   - **WiFi SSID + wachtwoord**
   - **MQTT hostname** (IP-adres of hostname van je broker, bijv. `192.168.1.10`)
   - **MQTT port** (standaard `1883`)
   - **MQTT gebruikersnaam** (leeg laten indien geen authenticatie)
   - **MQTT wachtwoord** (leeg laten indien geen authenticatie)
   - **MQTT root topic** (standaard `homeassistant/sensor/p1meter`)
   - **Home Assistant discovery** (`true` of `false`)
4. Klik op **Save** — de ESP herstart en verbindt met je netwerk

Het IP-adres is daarna te vinden via je router, de seriële monitor, of het MQTT topic `homeassistant/sensor/p1meter/ip_address/state`.

---

## MQTT topics

Alle waarden worden verstuurd naar hun eigen topic:

```
{MQTT_ROOT_TOPIC}/{sensor_id}/state
```

Met de standaard root topic `homeassistant/sensor/p1meter`:

### Energie

| Topic | Omschrijving | Eenheid |
|-------|-------------|---------|
| `.../consumption_low_tarif/state` | Verbruik laag tarief | Wh (÷1000 = kWh) |
| `.../consumption_high_tarif/state` | Verbruik hoog tarief | Wh (÷1000 = kWh) |
| `.../returndelivery_low_tarif/state` | Teruglevering laag tarief | Wh (÷1000 = kWh) |
| `.../returndelivery_high_tarif/state` | Teruglevering hoog tarief | Wh (÷1000 = kWh) |

### Actueel vermogen

| Topic | Omschrijving | Eenheid |
|-------|-------------|---------|
| `.../actual_consumption/state` | Totaal actueel verbruik | W (÷1000 = kW) |
| `.../actual_returndelivery/state` | Totale actuele teruglevering | W (÷1000 = kW) |
| `.../l1_instant_power_usage/state` | L1 actueel verbruik | W (÷1000 = kW) |
| `.../l2_instant_power_usage/state` | L2 actueel verbruik | W (÷1000 = kW) |
| `.../l3_instant_power_usage/state` | L3 actueel verbruik | W (÷1000 = kW) |
| `.../l1_instant_power_return/state` | L1 actuele teruglevering | W (÷1000 = kW) |
| `.../l2_instant_power_return/state` | L2 actuele teruglevering | W (÷1000 = kW) |
| `.../l3_instant_power_return/state` | L3 actuele teruglevering | W (÷1000 = kW) |

### Stroom & spanning

| Topic | Omschrijving | Eenheid |
|-------|-------------|---------|
| `.../l1_instant_power_current/state` | L1 stroom | mA (÷1000 = A) |
| `.../l2_instant_power_current/state` | L2 stroom | mA (÷1000 = A) |
| `.../l3_instant_power_current/state` | L3 stroom | mA (÷1000 = A) |
| `.../l1_voltage/state` | L1 spanning | mV (÷1000 = V) |
| `.../l2_voltage/state` | L2 spanning | mV (÷1000 = V) |
| `.../l3_voltage/state` | L3 spanning | mV (÷1000 = V) |

### Gas

| Topic | Omschrijving | Eenheid |
|-------|-------------|---------|
| `.../gas_meter_m3/state` | Gasverbruik | dm³ (÷1000 = m³) |
| `.../gas_meter_type/state` | Type gasmeter | — |
| `.../gas_equipment_id/state` | Serienummer gasmeter | — |

### Status & info

| Topic | Omschrijving |
|-------|-------------|
| `.../actual_tarif_group/state` | Huidig tarief (1 = laag, 2 = hoog) |
| `.../short_power_outages/state` | Aantal korte stroomonderbrekingen |
| `.../long_power_outages/state` | Aantal lange stroomonderbrekingen |
| `.../short_power_drops/state` | Aantal korte spanningsdalingen |
| `.../short_power_peaks/state` | Aantal korte spanningspieken |
| `.../dsrm_version/state` | DSMR versie van de meter |
| `.../dsrm_datetime/state` | Datum/tijd van het laatste telegram |
| `.../dsrm_equipment_id/state` | Serienummer elektriciteitsmeter |
| `.../ip_address/state` | Huidig IP-adres van de ESP |
| `.../update_available/state` | `true` als nieuwe firmware beschikbaar is |
| `.../latest_version/state` | Laatste versie gevonden op GitHub |

> Waarden worden als integers verstuurd. Home Assistant past via de `value_template` de deling toe bij sensors met HA Auto Discovery.

---

## REST API

De ESP biedt een HTTP API op poort 80.

**Basis-URL:** `http://{ip-adres}/{endpoint}`

### Overzicht endpoints

| Methode | Endpoint | Omschrijving |
|---------|----------|-------------|
| `GET` | `/` | Status + versienummer |
| `GET` | `/version` | Alleen versienummer |
| `GET` | `/restart` | Herstart de ESP |
| `GET` | `/haon` | HA Auto Discovery inschakelen |
| `GET` | `/haoff` | HA Auto Discovery uitschakelen |
| `GET` | `/interval` | Huidig update interval opvragen |
| `GET` | `/interval/set?seconds={n}` | Update interval instellen |
| `GET` | `/mqtt?server={ip}` | MQTT server bijwerken |
| `GET` | `/telegram` | Laatste telegram als JSON |
| `GET` | `/update` | Web firmware updater pagina |
| `GET` | `/update/check` | Gecachede versiecheck als JSON |
| `POST` | `/update/upload` | Firmware uploaden als .bin bestand |

---

### `GET /`

**Response:**
```
Ok - P1Meter v2.7.0
```

---

### `GET /version`

Handig voor scripts of automatische update checks.

**Response:**
```
2.7.0
```

---

### `GET /restart`

Herstart de ESP na 5 seconden.

**Response:**
```
Server is restarting in 5 seconds, restarting can take up to 1 minute...
```

```bash
curl http://192.168.1.x/restart
```

---

### `GET /haon`

Schakelt Home Assistant Auto Discovery **in**. Stuurt direct de config-berichten naar MQTT. Opgeslagen in EEPROM.

**Response:**
```
Ok - HA Discovery is on
```

```bash
curl http://192.168.1.x/haon
```

---

### `GET /haoff`

Schakelt Home Assistant Auto Discovery **uit**. Stuurt lege retained berichten zodat HA de sensors verwijdert. Opgeslagen in EEPROM.

**Response:**
```
Ok - HA Discovery is off
```

```bash
curl http://192.168.1.x/haoff
```

---

### `GET /interval`

Geeft het huidige update interval terug.

**Response:**
```
Update interval: 5000 ms (5.0 sec)
```

```bash
curl http://192.168.1.x/interval
```

---

### `GET /interval/set?seconds={n}`

Stelt het update interval in. Werkt **direct zonder herstart** en wordt opgeslagen in EEPROM.

**Parameters:**

| Parameter | Verplicht | Waarde |
|-----------|-----------|--------|
| `seconds` | Ja | 1 t/m 3600 |

**Response (success):**
```
Update interval set to 5 seconds. No restart needed.
```

**Foutmeldingen:**

| Input | Response |
|-------|----------|
| `?seconds=0` | `Invalid value: minimum is 1 second. The P1 meter sends at most 1 telegram per second.` |
| `?seconds=abc` | `Invalid value: 'abc' is not a number.` |
| `?seconds=9999` | `Invalid value: maximum is 3600 seconds (1 hour).` |

```bash
curl "http://192.168.1.x/interval/set?seconds=5"   # 5 seconden
curl "http://192.168.1.x/interval/set?seconds=1"   # maximale snelheid
curl "http://192.168.1.x/interval/set?seconds=60"  # standaard
```

> De instelling blijft actief na een herstart. Na een verse flash geldt de standaard uit `variables.h` (60 seconden).

---

### `GET /mqtt?server={ip}`

Werkt het MQTT server-adres bij, slaat het op in EEPROM en herstart de ESP automatisch.

**Parameters:**

| Parameter | Verplicht | Omschrijving |
|-----------|-----------|-------------|
| `server` | Ja | IP-adres of hostname van de MQTT broker |

**Response (success):**
```
MQTT server updated successfully!
```

```bash
curl "http://192.168.1.x/mqtt?server=192.168.1.20"
```

---

### `GET /telegram`

Geeft het meest recent ontvangen en geverifieerde telegram terug als JSON, opgeslagen in LittleFS. Wordt bijgewerkt bij elke succesvolle CRC-check.

**Response (voorbeeld):**
```json
{
  "device": {
    "ip_address": "192.168.1.50",
    "dsrm_version": "50",
    "dsrm_timestamp": "250101120000W",
    "dsrm_equipment_id": "E0000000000000000"
  },
  "readings": {
    "consumption_low_tarif":  { "value": 1234.567, "unit": "kWh" },
    "consumption_high_tarif": { "value": 2345.678, "unit": "kWh" },
    "return_low_tarif":       { "value": 100.000,  "unit": "kWh" },
    "return_high_tarif":      { "value": 200.000,  "unit": "kWh" }
  },
  "actuals": {
    "actual_consumption": { "value": 1.234, "unit": "kW" },
    "actual_return":      { "value": 0.000, "unit": "kW" },
    "tarif": 2
  },
  "l1": {
    "power":        { "value": 0.800, "unit": "kW" },
    "power_return": { "value": 0.000, "unit": "kW" },
    "current":      { "value": 3,     "unit": "A"  },
    "voltage":      { "value": 233,   "unit": "V"  }
  },
  "l2": { "...": "..." },
  "l3": { "...": "..." },
  "gas": {
    "gas_delivered":    { "value": 987.654, "unit": "m3" },
    "gas_equipment_id": "G0000000000000000",
    "gas_meter_type":   "003"
  },
  "status": {
    "short_power_outage": 3,
    "long_power_outage":  1,
    "short_power_drops":  0,
    "short_power_peaks":  0
  },
  "rawtelegram": [
    "/XMX5LGBBLA4415650678",
    "...",
    "!ABCD"
  ]
}
```

```bash
curl http://192.168.1.x/telegram
```

**Gebruik in Home Assistant (RESTful sensor):**
```yaml
sensor:
  - platform: rest
    name: P1 Telegram
    resource: http://192.168.1.x/telegram
    json_attributes:
      - readings
      - actuals
      - l1
      - l2
      - l3
      - gas
    value_template: "{{ value_json.device.dsrm_timestamp }}"
```

---

### `GET /update/check`

Geeft de gecachede versiecheck terug als JSON — zonder live GitHub call.

**Response:**
```json
{
  "success": true,
  "current": "2.7.0",
  "latest": "2.7.0",
  "update_available": false,
  "free_heap": 17984
}
```

```bash
curl http://192.168.1.x/update/check
```

---

## Home Assistant Auto Discovery

Wanneer HA Discovery ingeschakeld is, publiceert de ESP automatisch MQTT config-berichten zodat alle sensors direct in Home Assistant verschijnen — zonder handmatige YAML configuratie.

**Inschakelen:**
- Via de configuratiepagina van WiFiManager bij eerste setup
- Of achteraf via de API: `GET /haon`

**Uitschakelen:**
- Via de API: `GET /haoff` — stuurt lege retained berichten zodat HA de sensors verwijdert

De sensors verschijnen in Home Assistant onder het device **"P1 Meter"**. Alle sensors zijn aan hetzelfde device gekoppeld, inclusief de update status sensors.

**Automatisch geconfigureerde sensor-types:**

| Type | HA device_class | HA state_class | Eenheid |
|------|----------------|----------------|---------|
| Vermogen | `power` | `measurement` | kW |
| Stroom | `current` | `measurement` | A |
| Spanning | `voltage` | `measurement` | V |
| Energie | `energy` | `total_increasing` | kWh |
| Gas | `gas` | `total_increasing` | m³ |
| Tarief, ID's, datum | — | — | — |
| Update beschikbaar | — | — | `true` / `false` |
| Laatste versie | — | — | versienummer |

---

## JSON Telegram endpoint

Zie `GET /telegram` in de REST API sectie hierboven.

---

## OTA updates

### Via Arduino IDE

1. Ga naar **Tools → Port**
2. Onder **Network ports** verschijnt `p1meter at 192.168.x.x`
3. Selecteer die poort
4. Upload zoals normaal — de IDE vraagt om het OTA wachtwoord

**Werkt het device niet in de lijst?**
- Controleer of je PC op hetzelfde netwerk/VLAN zit als de ESP
- Op Windows kan de firewall mDNS blokkeren — voeg een inbound rule toe voor UDP poort 5353 en 8266
- Als alternatief: gebruik `espota.py` via de command line

**Via command line (`espota.py`):**
```bash
python "C:\...\tools\espota.py" -i 192.168.1.x -p 8266 -a admin -f firmware.bin
```

`espota.py` is te vinden in `%APPDATA%\Arduino15\packages\esp8266\hardware\esp8266\{versie}\tools\`

**OTA poort:** `8266`  
**Hostname:** `p1meter` (aanpasbaar via `HOSTNAME` in `variables.h`)  
**Wachtwoord:** instelbaar via `OTA_PASSWORD` in `variables.h` — standaard `admin`, **verander dit**

---

## Firmware update via web

Navigeer naar `http://{ip-adres}/update` voor de web updater. Deze pagina biedt:

### Versiecheck
Bij elke aanroep van de pagina wordt de gecachede versiecheck getoond. De ESP checkt periodiek (elke 6 uur standaard) `version.json` op GitHub en publiceert het resultaat naar MQTT.

### Instellingen
- **Auto-update** — schakel in om automatisch te updaten zodra een nieuwe versie gevonden wordt. De instelling wordt opgeslagen in EEPROM.

### Update via GitHub
Downloadt de laatste release direct van GitHub en flasht automatisch. De ESP herstart na het flashen. De pagina toont een voortgangsbalk en redirectt automatisch terug na 25 seconden.

### Update via .bin bestand
Upload een lokaal gecompileerd `.bin` bestand. Exporteer dit vanuit Arduino IDE via **Sketch → Export Compiled Binary**.

> De versiecheck gebruikt `rawcdn.githack.com` als HTTP proxy voor `raw.githubusercontent.com` — dit vermijdt een HTTPS heap probleem op de ESP8266 (BearSSL vereist ~20KB vrij heap voor SSL, de ESP8266 heeft typisch ~17KB beschikbaar). De firmware download zelf gebruikt wél HTTPS voor integriteit.

### `version.json` instellen voor eigen repo

Zet een `version.json` bestand in de root van je `master` branch:

```json
{
  "version": "2.7.0",
  "url": "https://github.com/{gebruiker}/{repo}/releases/latest/download/firmware.bin",
  "changelog": "https://github.com/{gebruiker}/{repo}/blob/master/RELEASE_NOTES.md"
}
```

Update dit bestand bij elke nieuwe release zodat de versiecheck correct werkt.

---

## Debug modus

Standaard is alle Serial output uitgeschakeld in productie. Schakel in via `variables.h` of bovenaan het `.ino` bestand:

```cpp
#define DEBUG 1  // 0 = uit (productie), 1 = aan (ontwikkeling)
```

Met `DEBUG 1` verschijnt in de Serial monitor (115200 baud):
- Versiecheck resultaat bij opstarten
- MQTT publish per topic
- Delta detection — hoeveel topics verstuurd per cyclus
- WiFi en MQTT reconnect pogingen
- CRC validatie per telegram

---

## Instellingen aanpassen

De meeste instellingen staan in `variables.h` en worden eenmalig bij het flashen ingesteld. Het update interval en auto-update zijn daarna ook via de web updater of API aan te passen.

| Instelling | Standaard | Omschrijving |
|-----------|-----------|-------------|
| `DEBUG` | `0` | Zet op `1` voor Serial output tijdens ontwikkeling |
| `UPDATE_INTERVAL` | `60000` (60 sec) | Standaard interval bij eerste flash — daarna via `/interval/set` |
| `WIFI_RECONNECT_INTERVAL` | 15 minuten | Wachttijd tussen WiFi reconnect-pogingen |
| `MQTT_RECONNECT_INTERVAL` | `5000` (5 sec) | Wachttijd tussen MQTT reconnect-pogingen |
| `HOSTNAME` | `p1meter` | mDNS hostname en OTA naam |
| `MQTT_ROOT_TOPIC` | `homeassistant/sensor/p1meter` | MQTT root topic |
| `OTA_PASSWORD` | `admin` | **Verander dit vóór het flashen** |
| `GITHUB_USER` | `wpbezemer` | GitHub gebruikersnaam voor versiecheck |
| `GITHUB_REPO` | `esp8266_p1meter` | GitHub repository naam voor versiecheck |

Instellingen in `settings.h` (buffergroottes, timeouts, versienummer) hoeven normaal niet aangepast te worden.

### EEPROM adresmap

| Adres | Grootte | Inhoud |
|-------|---------|--------|
| 0–63 | 64 bytes | MQTT hostname |
| 64–69 | 6 bytes | MQTT poort |
| 70–101 | 32 bytes | MQTT gebruikersnaam |
| 102–133 | 32 bytes | MQTT wachtwoord |
| 134–165 | 32 bytes | MQTT root topic |
| 166 | 1 byte | Instellingen beschikbaar vlag |
| 167 | 1 byte | HA Discovery aan/uit |
| 168–171 | 4 bytes | Update interval (ms) |
| 172 | 1 byte | OTA check interval (uren, 0 = uit) |
| 173 | 1 byte | Auto-update aan/uit |

---

## Gerelateerde projecten

Zodra de P1 meter data naar Home Assistant stuurt via MQTT, kun je de data direct visualiseren met deze custom Lovelace cards:

| Project | Omschrijving |
|---------|-------------|
| [ha-energie-overzicht-card](https://github.com/wpbezemer/ha-energie-overzicht-card) | Totaaloverzicht van gridverbruik, solar productie en teruglevering — met SVG arc gauge, zelfvoorzieningsgraad en bronverdeling |
| [ha-energie-card-3fase](https://github.com/wpbezemer/ha-energie-card-3fase) | Detail per fase voor 3-fase meters — geanimeerde vermogenspijltjes, stroom, spanning en voortgangsbalk per fase (L1/L2/L3), inclusief historiek grafiek |

Beide cards ondersteunen HACS-installatie en werken direct met de MQTT topics die deze firmware publiceert.

---

## Release notes

Zie [RELEASE_NOTES.md](RELEASE_NOTES.md) voor een volledig overzicht per versie.

---

## Credits

Dit project is opgebouwd op het werk van velen. Zonder deze forks en bronnen had dit project niet bestaan:

| Project | Auteur | Bijdrage |
|---------|--------|---------|
| [esp8266_p1meter](https://github.com/fliphess/esp8266_p1meter) | fliphess | Origineel project — basis P1 uitlezing met SoftwareSerial |
| [esp8266_p1meter](https://github.com/daniel-jong/esp8266_p1meter) | daniel-jong | DSMR5.0 ondersteuning, overstap naar hardware UART voor stabiele 115200 baud |
| [esp8266_p1meter](https://github.com/wpbezemer/esp8266_p1meter) | wpbezemer | REST API, HA Auto Discovery, WiFiManager, web OTA updater, LittleFS telegram opslag |
| [P1-Meter-ESP8266](https://github.com/jantenhove/P1-Meter-ESP8266) | jantenhove | Vroege ESP8266 P1 implementatie |
| [P1-Meter-ESP8266-MQTT](https://github.com/neographikal/P1-Meter-ESP8266-MQTT) | neographikal | MQTT integratie |
| [Slimme meter uitlezen](http://gejanssen.com/howto/Slimme-meter-uitlezen/) | gejanssen.com | Uitgebreide documentatie over het DSMR P1 protocol |

---

*Licentie: GPL-3.0*
