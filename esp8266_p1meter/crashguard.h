#pragma once
// **********************************
// * Crash bewaking & veilige modus *
// **********************************
//
// - Bij een crash (exception, watchdog, panic) worden reden, adressen en het
//   tijdstip van de meter vastgelegd in RTC geheugen. Dat overleeft een herstart
//   (niet een stroomonderbreking).
// - Crasht de ESP SAFE_MODE_CRASHES keer kort na elkaar, dan start hij in
//   veilige modus: WiFi, webpagina's, /update en MQTT status werken, maar de P1
//   uitlezing en MQTT data staan uit. Zo blijft een foute firmware op afstand
//   te vervangen.
// - Een normale herstart (/restart) start weer in normale modus.

#include <user_interface.h>

#define SAFE_MODE_CRASHES   3          // zoveel crashes kort na elkaar → veilige modus
#define STABLE_AFTER_MS     120000UL   // na 2 min stabiel draaien telt de crash-teller opnieuw

#define RTC_STATE_MAGIC     0x50314D32UL   // "P1M2"
// RTC user memory loopt van 0x60001200 t/m 0x600013FF. Het begin wordt door de
// OTA bootloader (eboot) gebruikt, daarom de tweede helft.
#define RTC_STATE_ADDR      0x60001300UL

struct RtcState {
    uint32_t magic;
    uint32_t crashCount;      // crashes na elkaar zonder 2 min stabiel te draaien
    uint32_t forceSafe;       // 1 = volgende start in veilige modus (/restart?safe=1)
    uint32_t pending;         // 1 = crash net vastgelegd, nog niet verwerkt bij opstarten
    uint32_t hasCrash;        // 1 = er is crash-info hieronder
    uint32_t reason;          // rst_info reason (1=HW WDT, 2=exception, 3=SW WDT, 254=panic, ...)
    uint32_t exccause, epc1, epc2, epc3, excvaddr, depc;
    uint32_t uptimeSec;       // uptime op moment van crash
    uint32_t freeHeap;        // vrij geheugen kort voor de crash
    uint32_t addrs[8];        // code-adressen van de stack (te decoderen met de .elf)
    char     meterTime[16];   // tijd van de meter (DSMR timestamp) bij de crash
    uint32_t checksum;
};
static_assert(sizeof(RtcState) % 4 == 0, "RtcState moet een veelvoud van 4 bytes zijn");
static_assert(sizeof(RtcState) <= 256, "RtcState past niet in RTC geheugen");

RtcState bootState;          // kopie in RAM, bijgewerkt bij opstarten
bool     safeMode = false;
bool     crashedThisBoot = false;

// Wordt in loop() bijgewerkt, zodat de crash handler zelf niets hoeft op te vragen
volatile uint32_t cachedUptimeSec = 0;
volatile uint32_t cachedFreeHeap  = 0;

static uint32_t rtcChecksum(const RtcState &s) {
    const uint32_t *w = (const uint32_t*)&s;
    uint32_t c = 0x12345678UL;
    for (size_t i = 0; i < sizeof(RtcState) / 4 - 1; i++) c = (c ^ w[i]) * 16777619UL;
    return c;
}

// RTC geheugen alleen per 32-bit woord benaderen
static void rtcWrite(RtcState &s) {
    s.magic = RTC_STATE_MAGIC;
    s.checksum = rtcChecksum(s);
    volatile uint32_t *dst = (volatile uint32_t*)RTC_STATE_ADDR;
    const uint32_t *src = (const uint32_t*)&s;
    for (size_t i = 0; i < sizeof(RtcState) / 4; i++) dst[i] = src[i];
}

static bool rtcRead(RtcState &s) {
    volatile uint32_t *src = (volatile uint32_t*)RTC_STATE_ADDR;
    uint32_t *dst = (uint32_t*)&s;
    for (size_t i = 0; i < sizeof(RtcState) / 4; i++) dst[i] = src[i];
    return s.magic == RTC_STATE_MAGIC && s.checksum == rtcChecksum(s);
}

// Wordt door de ESP8266 core aangeroepen bij een exception, software watchdog of panic.
// Niet bij een hardware watchdog (die wordt bij het opstarten herkend).
extern "C" void custom_crash_callback(struct rst_info *ri, uint32_t stack, uint32_t stack_end) {
    RtcState s;
    if (!rtcRead(s)) memset(&s, 0, sizeof(s));
    s.pending   = 1;
    s.hasCrash  = 1;
    s.reason    = ri->reason;
    s.exccause  = ri->exccause;
    s.epc1      = ri->epc1;
    s.epc2      = ri->epc2;
    s.epc3      = ri->epc3;
    s.excvaddr  = ri->excvaddr;
    s.depc      = ri->depc;
    s.uptimeSec = cachedUptimeSec;
    s.freeHeap  = cachedFreeHeap;
    memset(s.addrs, 0, sizeof(s.addrs));
    int n = 0;
    for (uint32_t p = stack; p < stack_end && n < 8; p += 4) {
        uint32_t v = *(uint32_t*)p;
        // Code in flash (0x402xxxxx) of IRAM (0x401xxxxx)
        if ((v >= 0x40200000UL && v < 0x40300000UL) || (v >= 0x40100000UL && v < 0x40110000UL)) s.addrs[n++] = v;
    }
    memset(s.meterTime, 0, sizeof(s.meterTime));
    strncpy(s.meterTime, DSRMTIMESTAMP, sizeof(s.meterTime) - 1);
    rtcWrite(s);
}

// Aanroepen als allereerste in setup()
void crashguardBoot() {
    RtcState s;
    if (!rtcRead(s)) memset(&s, 0, sizeof(s));   // na stroomonderbreking: leeg

    const uint32_t r = ESP.getResetInfoPtr()->reason;
    crashedThisBoot = s.pending || r == REASON_WDT_RST || r == REASON_EXCEPTION_RST || r == REASON_SOFT_WDT_RST;

    if (crashedThisBoot) {
        s.crashCount++;
        if (!s.pending) {
            // Hardware watchdog: de crash handler is niet aangeroepen, dus geen details
            s.hasCrash = 1;
            s.reason = r;
            s.exccause = s.epc1 = s.epc2 = s.epc3 = s.excvaddr = s.depc = 0;
            s.uptimeSec = 0;
            s.freeHeap = 0;
            memset(s.addrs, 0, sizeof(s.addrs));
            memset(s.meterTime, 0, sizeof(s.meterTime));
        }
    } else {
        s.crashCount = 0;   // stroom aan, reset knop of normale herstart
    }
    s.pending = 0;
    safeMode = s.forceSafe || s.crashCount >= SAFE_MODE_CRASHES;
    s.forceSafe = 0;
    rtcWrite(s);
    bootState = s;
}

// In loop(): na 2 min stabiel de teller op 0
void crashguardLoop(uint32_t uptimeSec) {
    cachedUptimeSec = uptimeSec;
    cachedFreeHeap  = ESP.getFreeHeap();
    static bool stableMarked = false;
    if (!stableMarked && !safeMode && millis() > STABLE_AFTER_MS) {
        stableMarked = true;
        if (bootState.crashCount) {
            RtcState s;
            if (rtcRead(s)) { s.crashCount = 0; rtcWrite(s); }
        }
    }
}

// Volgende start in veilige modus
void crashguardForceSafeNextBoot() {
    RtcState s;
    if (!rtcRead(s)) memset(&s, 0, sizeof(s));
    s.forceSafe = 1;
    rtcWrite(s);
}

// Leesbare reden van de laatste crash
String crashReasonText(uint32_t reason, uint32_t exccause) {
    switch (reason) {
        case REASON_WDT_RST:       return F("Hardware watchdog");
        case REASON_EXCEPTION_RST: return String(F("Exception ")) + exccause;
        case REASON_SOFT_WDT_RST:  return F("Software watchdog");
        case 253:                  return F("Stack overflow");
        case 254:                  return F("Panic / abort");
        default:                   return String(F("Reset ")) + reason;
    }
}

// Reden van deze start, voor MQTT / HA
String bootReasonText() {
    if (crashedThisBoot) return crashReasonText(bootState.reason, bootState.exccause);
    return ESP.getResetReason();
}

// Crash details als JSON (zonder buitenste accolades)
void printCrashJson(Print &out) {
    const RtcState &s = bootState;
    out.printf_P(PSTR("\"safe_mode\":%s,\"crash_count\":%u,\"crashed_this_boot\":%s,\"last_crash\":"),
        safeMode ? "true" : "false", s.crashCount, crashedThisBoot ? "true" : "false");
    if (!s.hasCrash) { out.print(F("null")); return; }
    out.print(F("{\"reason\":\""));
    out.print(crashReasonText(s.reason, s.exccause));
    out.printf_P(PSTR("\",\"exccause\":%u,\"epc1\":\"0x%08x\",\"epc2\":\"0x%08x\",\"epc3\":\"0x%08x\",\"excvaddr\":\"0x%08x\",\"depc\":\"0x%08x\""),
        s.exccause, s.epc1, s.epc2, s.epc3, s.excvaddr, s.depc);
    out.printf_P(PSTR(",\"uptime\":%u,\"free_heap\":%u,\"meter_time\":\""), s.uptimeSec, s.freeHeap);
    for (const char *c = s.meterTime; *c && c < s.meterTime + sizeof(s.meterTime); c++)
        if (isalnum((unsigned char)*c)) out.print(*c);
    out.print(F("\",\"stack\":\""));
    bool first = true;
    for (int i = 0; i < 8; i++) {
        if (!s.addrs[i]) continue;
        out.printf_P(first ? PSTR("0x%08x") : PSTR(" 0x%08x"), s.addrs[i]);
        first = false;
    }
    out.print(F("\"}"));
}
