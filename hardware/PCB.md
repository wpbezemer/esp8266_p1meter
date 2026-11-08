# P1 meter PCB (D1 mini)

Eenvoudig draagbord voor een Wemos D1 mini. Gevoed uit de P1 poort van de meter, of via de USB poort van de D1 mini als de meter geen voeding levert.

## Schema

```
 RJ12 (P1 poort)                                       Wemos D1 mini

 pin 1  +5V  ────►|──────┬──────┬──────┬──────────────►  5V
                 D1      │      │      │
                SS14    C1     C2      │
                       470µF  100nF    │
                         │      │      │
 pin 2  RTS  ────────────┼──────┼──────┘   (RTS aan 5V)
                         │      │
 pin 3  GND  ────────────┴──────┴─────────────────────►  G
 pin 6  GND  ──────────────────────────────────────────►  G

 pin 5  DATA ──────────────────────────┬──────────────►  RX
                                      R1 10kΩ
                                       └──────────────►  3V3
 pin 4  n.c.
```

| Net | Verbinding |
|---|---|
| `P1_5V` | RJ12 pin 1 → anode D1 |
| `5V` | kathode D1, C1+, C2, RJ12 pin 2 (RTS), D1 mini **5V** |
| `GND` | RJ12 pin 3 en 6, C1−, C2, D1 mini **G** |
| `DATA` | RJ12 pin 5, R1, D1 mini **RX** |
| `3V3` | R1, D1 mini **3V3** |

## Onderdelen (BOM)

| Ref | Onderdeel | Waarde / type | Opmerking |
|---|---|---|---|
| U1 | Wemos D1 mini | **v3 of v4, 4 MB flash** | Niet de D1 mini *Lite* (1 MB): daar past OTA niet op |
| J1 | RJ12 jack | 6P6C, PCB-montage | Of RJ11 (6P4C) als je de meter niet als voeding gebruikt |
| D1 | Schottky diode | SS14 (SMA) of 1N5819 (THT) | Voorkomt terugvoeding van USB naar de meter |
| C1 | Elco | 470 µF / 10 V, low-ESR | Vangt zendpieken van de ESP op (zie hieronder) |
| C2 | Condensator | 100 nF | Dicht bij de 5V pin van de D1 mini |
| R1 | Weerstand | 10 kΩ | Pull-up datalijn naar 3,3 V |
| — | 2× vrouwelijke header | 1×8, 2,54 mm | Optioneel: D1 mini insteekbaar |

## Waarom zo

- **Voeding uit de meter.** DSMR 5 levert 5 V, max 250 mA. De ESP8266 gebruikt gemiddeld ~70–80 mA, maar heeft bij zenden pieken van 300–400 mA (enkele ms). C1 levert die pieken, zodat de meter ze niet hoeft te leveren. Zonder buffer zakt de spanning in, en krijg je resets die op crashes lijken.
- **Externe USB.** Steek een USB-voeding in de D1 mini zelf. D1 voorkomt dat die 5 V terug de meter in gaat. Beide tegelijk aansluiten mag.
- **RTS.** De meter stuurt alleen data als RTS hoog is (DSMR: 4–5,5 V). Het hangt aan het 5V net, dus werkt met P1 voeding én met USB voeding.
- **Datalijn.** Open collector; de firmware inverteert het signaal zelf (UART RX inversie), dus geen transistor nodig. Alleen de pull-up R1.
- **Spanningsregelaar.** De 3,3 V regelaar van de D1 mini zelf (LDO). Simpel en ruim voldoende; geen extra regelaar op het bord.

## Let op

- **Kabel.** Gebruik een **rechte** RJ12 kabel (pin 1 ↔ pin 1). Gekruiste telefoonkabels werken niet.
- **Na flashen via USB eerst stroomloos.** Haal na een USB-flash één keer alle voeding los (USB én P1). Anders blijft de eerste herstart op afstand hangen (ESP8266 eigenschap).
- **Flashen via USB.** De datalijn deelt de RX pin met de USB-chip. Haal tijdens flashen via USB de P1 kabel los. Daarna kan alles via `/update`.
- **Plaatsing.** Leg de USB poort van de D1 mini aan de rand van het bord en houd het antennegedeelte (bovenkant D1 mini) vrij van koper en componenten.
- **C1 en de meter.** Een enkele meter schakelt de P1 voeding kort uit bij een grote inschakelstroom. Gebeurt dat, gebruik dan 220 µF.
