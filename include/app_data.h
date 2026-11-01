#pragma once

#include <stdint.h>
#include <math.h>

struct WeatherData {
  bool valid = false;
  float temperature = 0;
  float feelsLike = NAN;
  float windSpeed = NAN;
  int humidity = -1;
  int pressure = -1;
  int32_t utcOffset = 0;
  char description[96] = "Нет данных";
  char error[48] = "Ожидание сети";
  // OpenWeather icon code, e.g. "01d", "10n". Used for pretty icons.
  char icon[4] = "01d";
};

struct RadioData {
  bool online = false;
  bool playing = false;
  int volume = 0;
  int station = 0;
  // 0 = WEB (playlist.csv), 1 = SD (playlistsd.csv), 255 = unknown.
  uint8_t mode = 255;
  char stationName[96] = "Нет соединения";
  char title[128] = "";
  char message[64] = "";
  static constexpr int kListMax = 12;
  char stations[kListMax][48]{};
  uint16_t stationNums[kListMax]{};
  uint8_t stationCount = 0;
  char sdFiles[kListMax][48]{};
  uint16_t sdNums[kListMax]{};
  uint8_t sdCount = 0;
  // Incremented on every successful list fetch so the UI rebuilds once.
  uint32_t listVersion = 0;
  char listMessage[48] = "Список не загружен";
};

struct TvData {
  bool enabled = false;  // TV_HOST is set
  bool sessionOk = false;
  int volume = -1;  // -1 = unknown
  bool muted = false;
  char channel[32] = "";
  char message[48] = "Нет настроек ТВ";
};

struct AppData {
  static constexpr int kCityCount = 4;
  WeatherData weather[kCityCount];
  RadioData radio;
  float rates[4] = {0, 0, 0, 0};
  bool ratesValid = false;
  char ratesMessage[48] = "Ожидание сети";
  char wifiIp[16] = "";
  int wifiRssi = 0;
  TvData tv;
};

struct RateHistory {
  static constexpr int kPoints = 48;
  float values[kPoints]{};  // zero = no publication in this time bucket
  uint8_t currency = 0;
  uint8_t months = 6;
  bool loading = false;
  bool valid = false;
  uint32_t version = 0;
  char from[11] = "";
  char to[11] = "";
  char message[64] = "";
};
