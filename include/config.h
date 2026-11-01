#pragma once

// Configure these values before building. The web configurator generates this file.
// IMPORTANT: in #define lines do NOT add ';' at the end and do NOT use spaces
// inside numbers. Correct: 36000. Wrong: 36000; or 36 000.
#define WIFI_SSID "17843C"
#define WIFI_PASSWORD "01Suhomvit10"
#define DEVICE_HOSTNAME "multiinformer-cyd"
#define OWM_API_KEY "c0a36cc87aff8f6062834fcdfc346ef2"
#define CITY_A_NAME "Хабаровск"
#define CITY_A_QUERY "Khabarovsk,ru"
#define CITY_A_UTC_OFFSET 36000
#define CITY_B_NAME "Phuket"
#define CITY_B_QUERY "Phuket,th"
#define CITY_B_UTC_OFFSET 25200
#define CITY_C_NAME " Duisburg"
#define CITY_C_QUERY " Duisburg,de"
#define CITY_C_UTC_OFFSET 3600
#define CITY_D_NAME "Миасс"
#define CITY_D_QUERY "Miass,ru"
#define CITY_D_UTC_OFFSET 18000
#define WEATHER_INTERVAL_MINUTES 15
#define RADIO_HOST "192.168.100.3"
#define RADIO_PORT 80
#define RADIO_USER ""
#define RADIO_PASSWORD ""
// LG webOS 1.x (LB65): IP телевизора. Подтвердите запрос на экране ТВ.
// TV_PAIR_KEY оставлен для совместимости и не используется.
#define TV_HOST "192.168.100.2"
#define TV_PAIR_KEY ""
#define DEFAULT_ALARM_ENABLED false
#define DEFAULT_ALARM_HOUR 7
#define DEFAULT_ALARM_MINUTE 30
#define DEFAULT_ALARM_DAYS 31
#define DEFAULT_TIMER_MINUTES 10
#define DISPLAY_BRIGHTNESS 50
// GPIO 26 drives the CYD's audio amplifier. A connected speaker is required.
#define ALARM_SPEAKER_ENABLED 1
// SD web admin. Set a non-empty password and reflash to enable upload/delete.
// Basic authentication is sent over plain HTTP: use only a trusted LAN.
#define SD_WEB_USER "admin"
#define SD_WEB_PASSWORD "14051964"
