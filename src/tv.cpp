#include "services.h"

#include "config.h"
#include "lg_tv.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClient.h>

namespace {
Preferences tvPrefs;
SemaphoreHandle_t tvMutex = nullptr;
QueueHandle_t tvQueue = nullptr;
bool tvStarted = false;
TvData sharedTv;
char tvSession[32] = "";

struct TvCommand {
  uint8_t kind;
  int code;
};

enum : uint8_t { TV_KEY = 0, TV_PAIR_SHOW = 1, TV_POLL_NOW = 2 };

void tvSetMessage(const char *message) {
  if (tvMutex && xSemaphoreTake(tvMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    strlcpy(sharedTv.message, message ? message : "", sizeof(sharedTv.message));
    xSemaphoreGive(tvMutex);
  }
}

void tvSetSessionOk(bool ok) {
  if (tvMutex && xSemaphoreTake(tvMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    sharedTv.sessionOk = ok;
    xSemaphoreGive(tvMutex);
  }
}

// Plain HTTP POST to the TV. Returns the HTTP status or -1 on transport error.
int tvPost(const char *path, const char *body, char *resp, size_t respSize) {
  if (!strlen(TV_HOST)) return -1;
  WiFiClient client;
  client.setTimeout(2500);
  HTTPClient http;
  http.setConnectTimeout(2500);
  http.setTimeout(3500);
  const String url = String("http://") + TV_HOST + ":" + LG_TV_PORT + path;
  if (!http.begin(client, url)) return -1;
  http.addHeader("Content-Type", "application/atom+xml");
  const int code = http.POST((uint8_t *)(body ? body : ""), body ? strlen(body) : 0);
  if (code > 0 && resp && respSize > 1) {
    const String payload = http.getString();
    strlcpy(resp, payload.c_str(), respSize);
  }
  http.end();
  return code;
}

int tvGet(const char *path, char *resp, size_t respSize) {
  if (!strlen(TV_HOST)) return -1;
  WiFiClient client;
  client.setTimeout(2500);
  HTTPClient http;
  http.setConnectTimeout(2500);
  http.setTimeout(3500);
  const String url = String("http://") + TV_HOST + ":" + LG_TV_PORT + path;
  if (!http.begin(client, url)) return -1;
  const int code = http.GET();
  if (code > 0 && resp && respSize > 1) {
    const String payload = http.getString();
    strlcpy(resp, payload.c_str(), respSize);
  }
  http.end();
  return code;
}

int tvVariant = 0;  // 0 = unknown (try ROAP first), 1 = ROAP, 2 = HDCP

void tvRememberVariant(int v) {
  if (tvVariant != v) {
    tvVariant = v;
    tvPrefs.putUChar("var", static_cast<uint8_t>(v));
  }
}

// POST with automatic ROAP -> HDCP fallback. Retried only when the first
// attempt clearly did not execute (transport error or HTTP 404), so a key
// press can never be delivered twice.
int tvPostAny(const char *roapPath, const char *hdcpPath, const char *body, char *resp,
              size_t respSize) {
  const int first = (tvVariant == 2) ? 1 : 0;
  int code = -1;
  for (int k = 0; k < 2; ++k) {
    const bool hdcp = ((first + k) % 2) == 1;
    const char *path = hdcp ? hdcpPath : roapPath;
    if (!path) continue;
    code = tvPost(path, body, resp, respSize);
    if (code == 200) {
      tvRememberVariant(hdcp ? 2 : 1);
      return code;
    }
    if (code >= 0 && code != 404) return code;
  }
  return code;
}

void tvDropSession() {
  tvSession[0] = '\0';
  tvPrefs.remove("sess");
  tvSetSessionOk(false);
}

// Returns true with a usable session in tvSession.
bool tvAuth() {
  if (tvSession[0]) return true;
  if (!strlen(TV_PAIR_KEY)) {
    tvSetMessage("Нужен ключ сопряжения");
    return false;
  }
  char body[200];
  char resp[512];
  lgAuthRequest(TV_PAIR_KEY, body, sizeof(body));
  const int code = tvPostAny("/roap/api/auth", "/hdcp/api/auth", body, resp, sizeof(resp));
  if (code != 200 || !lgAuthOk(resp) ||
      !lgFindTag(resp, "session", tvSession, sizeof(tvSession)) || !tvSession[0]) {
    tvSession[0] = '\0';
    tvSetSessionOk(false);
    if (code < 0) tvSetMessage("ТВ недоступен");
    else {
      char msg[48];
      snprintf(msg, sizeof(msg), "Auth: HTTP %d", code);
      tvSetMessage(msg);
    }
    return false;
  }
  tvPrefs.putString("sess", tvSession);
  tvSetSessionOk(true);
  return true;
}

void tvExecuteKey(int code) {
  if (WiFi.status() != WL_CONNECTED) {
    tvSetMessage("Нет Wi-Fi");
    return;
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!tvAuth()) return;
    char body[220];
    char resp[256];
    lgKeyCommand(tvSession, code, body, sizeof(body));
    const int httpCode =
        tvPostAny("/roap/api/command", "/hdcp/api/dtv_wifirc", body, resp, sizeof(resp));
    if (httpCode == 200 && lgAuthOk(resp)) {
      tvSetSessionOk(true);
      tvSetMessage("OK");
      return;
    }
    if (httpCode >= 0 && httpCode != 404 && httpCode != 401) {
      char msg[48];
      snprintf(msg, sizeof(msg), "ТВ: HTTP %d", httpCode);
      tvSetMessage(msg);
      return;
    }
    // Stale session (e.g. after the TV was power-cycled): drop it and let
    // the stored pairing key silently mint a fresh one.
    tvDropSession();
  }
  tvSetMessage("Сессия отклонена");
}

void tvPairShow() {
  if (WiFi.status() != WL_CONNECTED) {
    tvSetMessage("Нет Wi-Fi");
    return;
  }
  char body[160];
  char resp[256];
  lgPairKeyRequest(body, sizeof(body));
  const int code = tvPostAny("/roap/api/auth", "/hdcp/api/auth", body, resp, sizeof(resp));
  if (code == 200) {
    tvSetMessage("Ключ на экране ТВ");
  } else if (code < 0) {
    tvSetMessage("ТВ недоступен");
  } else {
    char msg[48];
    snprintf(msg, sizeof(msg), "ТВ: HTTP %d", code);
    tvSetMessage(msg);
  }
}

void tvPoll() {
  if (WiFi.status() != WL_CONNECTED || !strlen(TV_HOST)) return;
  if (tvVariant == 2) return;  // HDCP: status query format unknown, status via commands
  if (!tvAuth()) return;
  char resp[1024];
  char path[96];
  snprintf(path, sizeof(path), "/roap/api/data?target=volume_info&session=%s", tvSession);
  bool ok = false;
  if (tvGet(path, resp, sizeof(resp)) == 200 && lgAuthOk(resp)) {
    ok = true;
    char value[16];
    if (tvMutex && xSemaphoreTake(tvMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (lgFindTag(resp, "level", value, sizeof(value))) sharedTv.volume = atoi(value);
      if (lgFindTag(resp, "mute", value, sizeof(value))) {
        sharedTv.muted = strcmp(value, "1") == 0 || strcmp(value, "true") == 0;
      }
      xSemaphoreGive(tvMutex);
    }
  }
  snprintf(path, sizeof(path), "/roap/api/data?target=cur_channel&session=%s", tvSession);
  if (tvGet(path, resp, sizeof(resp)) == 200 && lgAuthOk(resp)) {
    ok = true;
    char name[32];
    if ((lgFindTag(resp, "inputSourceName", name, sizeof(name)) ||
         lgFindTag(resp, "chname", name, sizeof(name))) &&
        tvMutex && xSemaphoreTake(tvMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      strlcpy(sharedTv.channel, name, sizeof(sharedTv.channel));
      xSemaphoreGive(tvMutex);
    }
  }
  if (ok) tvSetSessionOk(true);
}

void tvTask(void *) {
  tvPrefs.begin("lgtv", false);
  const String saved = tvPrefs.getString("sess", "");
  strlcpy(tvSession, saved.c_str(), sizeof(tvSession));
  tvVariant = tvPrefs.getUChar("var", 0);
  if (tvMutex && xSemaphoreTake(tvMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    sharedTv.enabled = strlen(TV_HOST) > 0;
    sharedTv.sessionOk = tvSession[0] != '\0';
    if (!sharedTv.enabled) strlcpy(sharedTv.message, "Нет настроек ТВ", sizeof(sharedTv.message));
    else strlcpy(sharedTv.message, "Нет сессии", sizeof(sharedTv.message));
    xSemaphoreGive(tvMutex);
  }
  unsigned long lastPoll = 0;
  for (;;) {
    TvCommand cmd{};
    const bool have =
        tvQueue && xQueueReceive(tvQueue, &cmd, pdMS_TO_TICKS(200)) == pdTRUE;
    if (have && sharedTv.enabled) {
      if (cmd.kind == TV_KEY) tvExecuteKey(cmd.code);
      else if (cmd.kind == TV_PAIR_SHOW) tvPairShow();
      else if (cmd.kind == TV_POLL_NOW) {
        tvPoll();
        lastPoll = millis();
      }
    }
    if (sharedTv.enabled && WiFi.status() == WL_CONNECTED && millis() - lastPoll >= 60000UL) {
      tvPoll();
      lastPoll = millis();
    }
  }
}

void ensureTv() {
  if (tvStarted) return;
  tvStarted = true;
  tvMutex = xSemaphoreCreateMutex();
  tvQueue = xQueueCreate(16, sizeof(TvCommand));
  xTaskCreatePinnedToCore(tvTask, "lgtv", 8192, nullptr, 1, nullptr, 1);
}
}  // namespace

bool sendTvKey(int code) {
  ensureTv();
  if (!tvQueue) return false;
  TvCommand cmd{TV_KEY, code};
  return xQueueSend(tvQueue, &cmd, 0) == pdTRUE;
}

void tvPairShowKey() {
  ensureTv();
  if (!tvQueue) return;
  TvCommand cmd{TV_PAIR_SHOW, 0};
  xQueueSend(tvQueue, &cmd, 0);
}

void requestTvStatus() {
  ensureTv();
  if (!tvQueue) return;
  TvCommand cmd{TV_POLL_NOW, 0};
  xQueueSend(tvQueue, &cmd, 0);
}

void copyTvData(TvData &destination) {
  if (tvMutex && xSemaphoreTake(tvMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    destination = sharedTv;
    xSemaphoreGive(tvMutex);
  }
}
