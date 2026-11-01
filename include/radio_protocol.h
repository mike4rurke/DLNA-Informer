#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_data.h"

// yoRadio_spdif publishes status as a sequence of small JSON WebSocket frames.
inline bool parseRadioMessage(const uint8_t *payload, size_t length, RadioData &radio) {
  if (!payload || !length || length > 4096) return false;
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, payload, length)) return false;
  if (doc["current"].is<int>()) radio.station = doc["current"];
  if (doc["playermode"].is<const char *>()) {
    radio.mode = strcmp(doc["playermode"], "modesd") == 0 ? 1 : 0;
  }
  for (JsonObject item : doc["payload"].as<JsonArray>()) {
    const char *id = item["id"] | "";
    if (strcmp(id, "nameset") == 0) {
      snprintf(radio.stationName, sizeof(radio.stationName), "%s", item["value"] | "");
    } else if (strcmp(id, "meta") == 0) {
      snprintf(radio.title, sizeof(radio.title), "%s", item["value"] | "");
    } else if (strcmp(id, "playerwrap") == 0) {
      radio.playing = strcmp(item["value"] | "", "playing") == 0;
    } else if (strcmp(id, "volume") == 0) {
      const int volume = item["value"].as<int>();
      radio.volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    }
  }
  return true;
}