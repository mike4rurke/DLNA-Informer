#include "services.h"
#include "config.h"
#include "data_protocol.h"
#include "radio_protocol.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

namespace {
AppData shared;
SemaphoreHandle_t dataMutex = nullptr;
QueueHandle_t commandQueue = nullptr;
WebSocketsClient radioSocket;
bool socketStarted = false;
bool forceRefresh = false;
bool listsRefresh = false;

struct RadioCommand {
  char query[32];
};

static const char *cityQueries[AppData::kCityCount] = {CITY_A_QUERY, CITY_B_QUERY, CITY_C_QUERY,
                                                       CITY_D_QUERY};
static const int32_t cityOffsets[AppData::kCityCount] = {CITY_A_UTC_OFFSET, CITY_B_UTC_OFFSET,
                                                         CITY_C_UTC_OFFSET, CITY_D_UTC_OFFSET};

void updateRadioConnection(bool online) {
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    shared.radio.online = online;
    if (!online) strlcpy(shared.radio.message, "Нет связи с yoRadio", sizeof(shared.radio.message));
    xSemaphoreGive(dataMutex);
  }
}

void radioEvent(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    updateRadioConnection(true);
    // The fork's parseWsCommand expects command=value, not JSON.
    radioSocket.sendTXT("getindex=1");
    return;
  }
  if (type == WStype_DISCONNECTED) {
    updateRadioConnection(false);
    return;
  }
  if (type != WStype_TEXT || length == 0 || length > 4096) return;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  parseRadioMessage(payload, length, shared.radio);
  xSemaphoreGive(dataMutex);
}

void publishWeather(int index, const WeatherData &weather) {
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    shared.weather[index] = weather;
    xSemaphoreGive(dataMutex);
  }
}

void publishWifiInfo() {
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (WiFi.status() == WL_CONNECTED) {
      strlcpy(shared.wifiIp, WiFi.localIP().toString().c_str(), sizeof(shared.wifiIp));
      shared.wifiRssi = WiFi.RSSI();
    } else {
      shared.wifiIp[0] = '\0';
      shared.wifiRssi = 0;
    }
    xSemaphoreGive(dataMutex);
  }
}

String urlEncode(const char *input) {
  static const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(strlen(input) * 3);
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(input); *p; ++p) {
    const unsigned char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 15];
    }
  }
  return encoded;
}

bool fetchWeather(int index, const char *cityQuery, int32_t fallbackOffset) {
  WeatherData weather;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    weather = shared.weather[index];
    xSemaphoreGive(dataMutex);
  }
  if (!weather.valid) weather.utcOffset = fallbackOffset;
  if (strlen(OWM_API_KEY) == 0) {
    strlcpy(weather.error, "Укажите ключ OpenWeather", sizeof(weather.error));
    publishWeather(index, weather);
    return false;
  }

  // ESP32 does not have a system CA store. This retains encryption but does
  // not authenticate the server; use a trusted network and see README.
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(7000);
  http.useHTTP10(true);
  const String url = String("https://api.openweathermap.org/data/2.5/weather?q=") +
                     urlEncode(cityQuery) + "&units=metric&lang=ru&appid=" + OWM_API_KEY;
  if (!http.begin(client, url)) {
    strlcpy(weather.error, "Ошибка HTTPS", sizeof(weather.error));
    publishWeather(index, weather);
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(weather.error, sizeof(weather.error), "OpenWeather: HTTP %d", code);
    http.end();
    publishWeather(index, weather);
    return false;
  }

  DynamicJsonDocument doc(4096);
  const DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error || !parseWeatherDocument(doc, weather, fallbackOffset)) {
    strlcpy(weather.error, "Ошибка данных погоды", sizeof(weather.error));
    publishWeather(index, weather);
    return false;
  }
  publishWeather(index, weather);
  return true;
}

bool fetchRates() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(7000);
  http.useHTTP10(true);
  if (!http.begin(client, "https://iss.moex.com/iss/statistics/engines/futures/markets/indicativerates/securities.json?iss.meta=off&iss.only=securities")) return false;
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      snprintf(shared.ratesMessage, sizeof(shared.ratesMessage), "MOEX: HTTP %d", code);
      xSemaphoreGive(dataMutex);
    }
    return false;
  }
  DynamicJsonDocument doc(16384);
  const DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  float rates[4] = {0, 0, 0, 0};
  if (error) return false;
  const bool found = parseRatesDocument(doc, rates);
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < 4; ++i) {
      shared.rates[i] = rates[i];
    }
    shared.ratesValid = found;
    strlcpy(shared.ratesMessage, found ? "MOEX: индикативные курсы" : "MOEX: нет данных", sizeof(shared.ratesMessage));
    xSemaphoreGive(dataMutex);
  }
  return found;
}

bool fetchTextFromRadio(const char *path, char *out, size_t outSize) {
  if (!strlen(RADIO_HOST) || !out || outSize < 2) return false;
  WiFiClient client;
  client.setTimeout(2000);
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(6000);
  const String url = String("http://") + RADIO_HOST + ":" + RADIO_PORT + path;
  if (!http.begin(client, url)) return false;
  if (strlen(RADIO_USER)) http.setAuthorization(RADIO_USER, RADIO_PASSWORD);
  const int code = http.GET();
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    WiFiClient &stream = http.getStream();
    size_t pos = 0;
    while (http.connected() && pos + 1 < outSize) {
      const int c = stream.read();
      if (c < 0) break;
      out[pos++] = static_cast<char>(c);
    }
    out[pos] = '\0';
    ok = pos > 0;
  }
  http.end();
  return ok;
}

// yoRadio keeps tab-separated playlists: /data/playlist.csv (WEB radio)
// and /data/playlistsd.csv (SD card files). Parsing lives in data_protocol.h
// so numbering always matches the receiver (see parseCsvList).
// Copies a fresh list over the stored one. Returns true when visible content
// changed, so the UI rebuilds only when needed.
bool applyList(char (*dstNames)[48], uint16_t *dstNums, uint8_t &dstCount,
               char (*srcNames)[48], const uint16_t *srcNums, uint8_t srcCount) {
  if (dstCount != srcCount ||
      memcmp(dstNames, srcNames, sizeof(char[RadioData::kListMax][48])) != 0 ||
      memcmp(dstNums, srcNums, sizeof(uint16_t[RadioData::kListMax])) != 0) {
    memcpy(dstNames, srcNames, sizeof(char[RadioData::kListMax][48]));
    memcpy(dstNums, srcNums, sizeof(uint16_t[RadioData::kListMax]));
    dstCount = srcCount;
    return true;
  }
  return false;
}

void fetchRadioLists() {
  char text[4096];
  uint8_t mode = 255;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    mode = shared.radio.mode;
    xSemaphoreGive(dataMutex);
  }
  // NOTE: in SD mode the receiver serves the SD playlist at /data/playlist.csv;
  // /data/playlistsd.csv exists only if an SD playlist was saved in the web UI.
  char namesA[RadioData::kListMax][48]{};
  uint16_t numsA[RadioData::kListMax]{};
  uint8_t countA = 0;
  bool okA = false;
  char namesB[RadioData::kListMax][48]{};
  uint16_t numsB[RadioData::kListMax]{};
  uint8_t countB = 0;
  bool okB = false;
  if (fetchTextFromRadio("/data/playlist.csv", text, sizeof(text))) {
    okA = true;
    countA = parseCsvList(text, namesA, numsA, RadioData::kListMax);
  }
  if (fetchTextFromRadio("/data/playlistsd.csv", text, sizeof(text))) {
    okB = true;
    countB = parseCsvList(text, namesB, numsB, RadioData::kListMax);
  }
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    bool changed = false;
    if (mode == 1) {
      if (okB && countB) {
        changed |= applyList(shared.radio.sdFiles, shared.radio.sdNums, shared.radio.sdCount,
                             namesB, numsB, countB);
      } else if (okA) {
        changed |= applyList(shared.radio.sdFiles, shared.radio.sdNums, shared.radio.sdCount,
                             namesA, numsA, countA);
      }
    } else {
      if (okA) {
        changed |= applyList(shared.radio.stations, shared.radio.stationNums,
                             shared.radio.stationCount, namesA, numsA, countA);
      }
      if (okB) {
        changed |= applyList(shared.radio.sdFiles, shared.radio.sdNums, shared.radio.sdCount,
                             namesB, numsB, countB);
      }
    }
    if (changed) ++shared.radio.listVersion;
    if (okA || okB) {
      const uint8_t sc = shared.radio.stationCount;
      const uint8_t fc = shared.radio.sdCount;
      if (sc && fc) strlcpy(shared.radio.listMessage, "Списки обновлены", sizeof(shared.radio.listMessage));
      else if (sc) strlcpy(shared.radio.listMessage, "SD: нет списка", sizeof(shared.radio.listMessage));
      else if (fc) strlcpy(shared.radio.listMessage, "WEB: нет списка", sizeof(shared.radio.listMessage));
      else strlcpy(shared.radio.listMessage, "Списки недоступны", sizeof(shared.radio.listMessage));
    }
    xSemaphoreGive(dataMutex);
  }
}

void executeRadioCommand(const RadioCommand &command) {
  if (!strlen(RADIO_HOST)) return;
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2500);
  const String url = String("http://") + RADIO_HOST + ":" + RADIO_PORT + "/?" + command.query;
  int code = -1;
  if (http.begin(client, url)) {
    if (strlen(RADIO_USER)) http.setAuthorization(RADIO_USER, RADIO_PASSWORD);
    code = http.GET();
    http.end();
  }
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    snprintf(shared.radio.message, sizeof(shared.radio.message), "Команда: HTTP %d", code);
    xSemaphoreGive(dataMutex);
  }
}

void networkTask(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(DEVICE_HOSTNAME);
  if (strlen(WIFI_SSID)) WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  bool connectedPreviously = false;
  unsigned long lastReconnect = millis();
  unsigned long lastWeather = 0;
  unsigned long lastRates = 0;
  unsigned long lastRadioPoll = 0;
  unsigned long lastRadioLists = 0;
  bool firstWeather = true;
  bool firstRates = true;
  bool firstRadioLists = true;

  for (;;) {
    const bool connected = WiFi.status() == WL_CONNECTED;
    const unsigned long now = millis();
    if (!connected) {
      if (connectedPreviously) {
        radioSocket.disconnect();
        socketStarted = false;
        updateRadioConnection(false);
        connectedPreviously = false;
      }
      if (strlen(WIFI_SSID) && now - lastReconnect >= 15000UL) {
        WiFi.reconnect();
        lastReconnect = now;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (!connectedPreviously) {
      connectedPreviously = true;
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      publishWifiInfo();
      firstWeather = true;
      firstRates = true;
      firstRadioLists = true;
      if (strlen(RADIO_HOST)) {
        radioSocket.begin(RADIO_HOST, RADIO_PORT, "/ws");
        if (strlen(RADIO_USER)) radioSocket.setAuthorization(RADIO_USER, RADIO_PASSWORD);
        radioSocket.onEvent(radioEvent);
        radioSocket.setReconnectInterval(5000);
        socketStarted = true;
      }
    }

    if (socketStarted) radioSocket.loop();
    RadioCommand command{};
    if (xQueueReceive(commandQueue, &command, 0) == pdTRUE) executeRadioCommand(command);
    if (socketStarted && now - lastRadioPoll >= 30000UL) {
      radioSocket.sendTXT("getindex=1");
      publishWifiInfo();
      lastRadioPoll = now;
    }

    bool refresh = false;
    bool refreshLists = false;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      refresh = forceRefresh;
      forceRefresh = false;
      refreshLists = listsRefresh;
      listsRefresh = false;
      xSemaphoreGive(dataMutex);
    }
    if (refresh) {
      firstWeather = true;
      firstRates = true;
      firstRadioLists = true;
    }
    if (firstWeather || now - lastWeather >= static_cast<unsigned long>(WEATHER_INTERVAL_MINUTES) * 60000UL) {
      firstWeather = false;
      lastWeather = now;
      for (int i = 0; i < AppData::kCityCount; ++i) {
        fetchWeather(i, cityQueries[i], cityOffsets[i]);
        if (socketStarted) radioSocket.loop();
      }
    }
    if (firstRates || now - lastRates >= 1800000UL) {
      firstRates = false;
      lastRates = now;
      fetchRates();
    }
    if (strlen(RADIO_HOST) &&
        (firstRadioLists || refreshLists || now - lastRadioLists >= 300000UL)) {
      firstRadioLists = false;
      lastRadioLists = now;
      fetchRadioLists();
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}
}  // namespace

void startNetworkServices() {
  dataMutex = xSemaphoreCreateMutex();
  commandQueue = xQueueCreate(12, sizeof(RadioCommand));
  for (int i = 0; i < AppData::kCityCount; ++i) shared.weather[i].utcOffset = cityOffsets[i];
  xTaskCreatePinnedToCore(networkTask, "network", 16384, nullptr, 1, nullptr, 0);
}

void copyAppData(AppData &destination) {
  if (dataMutex && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    destination = shared;
    xSemaphoreGive(dataMutex);
  }
}

bool sendRadioCommand(const char *query) {
  if (!commandQueue || !query) return false;
  RadioCommand command{};
  strlcpy(command.query, query, sizeof(command.query));
  return xQueueSend(commandQueue, &command, 0) == pdTRUE;
}

void setRadioMode(uint8_t mode) {
  if (dataMutex && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    shared.radio.mode = mode;
    xSemaphoreGive(dataMutex);
  }
  char query[16];
  snprintf(query, sizeof(query), "mode=%u", static_cast<unsigned>(mode));
  sendRadioCommand(query);
}

void requestDataRefresh() {
  if (dataMutex && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    forceRefresh = true;
    xSemaphoreGive(dataMutex);
  }
}

void requestRadioLists() {
  if (dataMutex && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    listsRefresh = true;
    xSemaphoreGive(dataMutex);
  }
}
