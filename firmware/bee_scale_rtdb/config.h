// config.h — NEM titkos konfiguráció, egy helyen.
// A titkokat (WiFi jelszó, API key, DB URL, eszköz-user) lásd: secrets.h
#pragma once

// --- Kaptár-azonosító -------------------------------------------------------
// Az RTDB írási út: /hives/<HIVE_ID>/readings
#define HIVE_ID "hive1"

// --- Küldési ütemezés -------------------------------------------------------
// 5 perces, nem-blokkoló (millis()) periódus.
#define SEND_INTERVAL_MS (5UL * 60UL * 1000UL)

// --- RAM-only offline FIFO buffer -------------------------------------------
// Fix kapacitás. Megtelve a LEGRÉGEBBI minta esik ki (telemetria: az újabb a
// fontosabb). Reboot/áramszünet esetén a buffer tartalma elvész — elfogadott.
#define BUFFER_CAPACITY 64

// --- Írási retry / exponenciális backoff ------------------------------------
#define RETRY_BACKOFF_BASE_MS 2000UL    // első retry késleltetés
#define RETRY_BACKOFF_MAX_MS  60000UL   // backoff felső korlát
// (A push szinkron/await — a siker/hiba determinisztikus; nincs külön push-timeout.)

// --- WiFi -------------------------------------------------------------------
#define WIFI_RECONNECT_INTERVAL_MS 5000UL   // manuális reconnect próbálkozás köze

// --- NTP / idő --------------------------------------------------------------
// UTC epoch másodpercet tárolunk (offset 0). A megjelenítés a dashboardon kezeli a TZ-t.
#define NTP_SERVER          "pool.ntp.org"
#define GMT_OFFSET_SEC      0
#define DAYLIGHT_OFFSET_SEC 0
// Ez alatti epoch = az óra még nincs szinkronban (nem tűzünk ts-t a mintára).
#define VALID_EPOCH_MIN     1700000000UL

// --- Firebase Auth ----------------------------------------------------------
// ID-token lejárati periódus (sec, < 3600). A lib auto-refresh-eli.
#define USER_AUTH_EXPIRE_SEC 3000

// --- Egyéb ------------------------------------------------------------------
#define SERIAL_BAUD 115200
