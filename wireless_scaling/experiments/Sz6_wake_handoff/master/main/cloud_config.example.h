/*
 * Sz6 master cloud konfiguracio MINTA.
 *
 * Hasznalat:
 *   cp main/cloud_config.example.h main/cloud_config.h
 *   majd ird at a WiFi adatokat es a webhook URL-t.
 *
 * A main/cloud_config.h gitignore alatt van, ne keruljon repoba.
 */
#pragma once

#define SZ6_WIFI_SSID "my-wifi-ssid"
#define SZ6_WIFI_PASS "my-wifi-password"

/* Firebase RTDB REST auth/config. Erdemes a meglévő prod secrets.h-ból átvenni. */
#define SZ6_FIREBASE_API_KEY "firebase-web-api-key"
#define SZ6_FIREBASE_DATABASE_URL "https://project-default-rtdb.region.firebasedatabase.app"
#define SZ6_FIREBASE_EMAIL "esp32@beescale.local"
#define SZ6_FIREBASE_PASSWORD "device-user-password"
#define SZ6_FIREBASE_ROOT_PATH "experiments/sz6_overnight"
