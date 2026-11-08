#pragma once
// **********************************
// * Systeem instellingen           *
// * (normaal niet aanpassen)       *
// **********************************

// * Firmware versie
#define APPVERSION "2.8.0"

// * Baud rate van de P1 poort (DSMR5 = 115200)
#define BAUD_RATE 115200

// * Max lengte van één telegram regel
#define P1_MAXLINELENGTH 1050

// * Buffer voor het volledige telegram (alle regels samen)
#define FULL_TELEGRAM_SIZE 2048

// * HTTP poort voor API en webpagina's
#define HTTP_REST_PORT 80

// * GitHub versiecheck (plain HTTP via githack proxy, zie README)
#define GITHUB_VERSION_HOST "rawcdn.githack.com"
#define GITHUB_VERSION_PATH "/" GITHUB_USER "/" GITHUB_REPO "/master/version.json"
#define GITHUB_FIRMWARE_URL "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download/firmware.bin"
