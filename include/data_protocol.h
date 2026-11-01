#pragma once

#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_data.h"

inline size_t utf8SafeLen(const char *s, size_t maxLen);

inline bool parseWeatherDocument(const JsonDocument &doc, WeatherData &weather,
                                 int32_t fallbackOffset) {
  if (!doc["main"]["temp"].is<float>()) return false;
  weather.temperature = doc["main"]["temp"];
  weather.feelsLike = doc["main"]["feels_like"].is<float>() ? doc["main"]["feels_like"].as<float>() : NAN;
  weather.humidity = doc["main"]["humidity"].is<int>() ? doc["main"]["humidity"].as<int>() : -1;
  weather.pressure = doc["main"]["pressure"].is<int>() ? doc["main"]["pressure"].as<int>() : -1;
  weather.windSpeed = doc["wind"]["speed"].is<float>() ? doc["wind"]["speed"].as<float>() : NAN;
  weather.utcOffset = doc["timezone"] | fallbackOffset;
  snprintf(weather.description, sizeof(weather.description), "%s",
           doc["weather"][0]["description"] | "");
  snprintf(weather.icon, sizeof(weather.icon), "%s", doc["weather"][0]["icon"] | "01d");
  weather.valid = true;
  weather.error[0] = '\0';
  return true;
}

// yoRadio playlist.csv / playlistsd.csv are tab-separated "Name<TAB>URL<TAB>Ovol".
// Extracts a short display name. Returns false for empty or comment lines.
inline bool parsePlaylistName(const char *line, char *out, size_t outSize) {
  if (!line || !out || outSize == 0) return false;
  char tmp[256];
  strncpy(tmp, line, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  size_t len = strlen(tmp);
  while (len > 0 && (tmp[len - 1] == '\r' || tmp[len - 1] == '\n' || tmp[len - 1] == ' ' ||
                     tmp[len - 1] == '\t')) {
    tmp[--len] = '\0';
  }
  char *start = tmp;
  while (*start == ' ' || *start == '\t') ++start;
  // Strip a leading UTF-8 BOM (playlist.csv often comes from Windows editors).
  // Without this the first station name turns into garbage and the whole
  // list shifts by one row versus the play=N numbers sent to yoRadio.
  if ((uint8_t)start[0] == 0xEF && (uint8_t)start[1] == 0xBB && (uint8_t)start[2] == 0xBF) start += 3;
  while (*start == ' ' || *start == '\t') ++start;
  if (*start == '\0') return false;

  char *fields[4] = {nullptr, nullptr, nullptr, nullptr};
  int fieldCount = 0;
  fields[fieldCount++] = start;
  for (char *p = start; *p && fieldCount < 4; ++p) {
    if (*p == '\t') {
      *p = '\0';
      fields[fieldCount++] = p + 1;
    }
  }

  const char *name = fields[0];
  if (fieldCount >= 2) {
    const bool firstIsUrl = strncmp(fields[0], "http://", 7) == 0 || strncmp(fields[0], "https://", 8) == 0;
    name = firstIsUrl ? fields[1] : fields[0];
  } else if (strncmp(name, "http://", 7) == 0 || strncmp(name, "https://", 8) == 0) {
    // Bare URL line: show the host instead of the full address.
    const char *scheme = strstr(name, "://");
    const char *host = scheme ? scheme + 3 : name;
    const char *end = host;
    while (*end && *end != '/' && *end != ':' && *end != '?') ++end;
    if (end > host) {
      const size_t hostLen = static_cast<size_t>(end - host);
      snprintf(out, outSize, "%.*s", static_cast<int>(hostLen < outSize - 1 ? hostLen : outSize - 1), host);
      return true;
    }
  }
  while (*name == ' ' || *name == '\t' || *name == '"') ++name;
  size_t nameLen = strlen(name);
  while (nameLen > 0 &&
         (name[nameLen - 1] == ' ' || name[nameLen - 1] == '\t' || name[nameLen - 1] == '"')) {
    --nameLen;
  }
  if (nameLen == 0) return false;
  nameLen = utf8SafeLen(name, outSize - 1);
  if (nameLen == 0) return false;
  memcpy(out, name, nameLen);
  out[nameLen] = '\0';
  return true;
}

inline bool looksLikeUrl(const char *s) {
  return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

inline char *trimField(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '"') ++s;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '"')) s[--n] = '\0';
  return s;
}

// True for a spreadsheet-style header row such as "Name<TAB>URL<TAB>Ovol".
// Both fields must match header words EXACTLY (case-insensitive): a real
// station that merely contains such a word ("Stream Mix") is kept.
// Applied to the first tab-separated line only: the row is hidden, but its
// number is kept, so everything below stays aligned with the receiver.
inline bool headerFieldEquals(const char *field, size_t len, const char *const *words) {
  for (int i = 0; words[i]; ++i) {
    size_t wlen = strlen(words[i]);
    if (len != wlen) continue;
    bool same = true;
    for (size_t k = 0; k < len; ++k) {
      char c = field[k];
      if (c >= 'A' && c <= 'Z') c += 32;
      if (c != words[i][k]) {
        same = false;
        break;
      }
    }
    if (same) return true;
  }
  return false;
}

inline bool isPlaylistHeader(const char *line) {
  if (!line || strstr(line, "://")) return false;
  const char *t1 = strchr(line, '\t');
  if (!t1) return false;
  const char *a0 = line;
  while (a0 < t1 && (*a0 == ' ' || *a0 == '\t' || *a0 == '"')) ++a0;
  const char *a1 = t1;
  while (a1 > a0 && (a1[-1] == ' ' || a1[-1] == '\t' || a1[-1] == '"')) --a1;
  const char *b0 = t1 + 1;
  const char *t2 = strchr(b0, '\t');
  const char *b1 = t2 ? t2 : b0 + strlen(b0);
  while (b0 < b1 && (*b0 == ' ' || *b0 == '\t' || *b0 == '"')) ++b0;
  while (b1 > b0 && (b1[-1] == ' ' || b1[-1] == '\t' || b1[-1] == '"' || b1[-1] == '\r' ||
                     b1[-1] == '\n'))
    --b1;
  static const char *firstWords[] = {"name",  "title",   "station", "track", "название",
                                     "станция", "имя",   "трек",    "НАЗВАНИЕ", "СТАНЦИЯ",
                                     "ИМЯ",   "ТРЕК",    nullptr};
  static const char *secondWords[] = {"url",    "link",   "ovol",   "volume", "адрес",
                                      "ссылка", "URL",    "АДРЕС",  "ССЫЛКА", nullptr};
  return headerFieldEquals(a0, static_cast<size_t>(a1 - a0), firstWords) &&
         headerFieldEquals(b0, static_cast<size_t>(b1 - b0), secondWords);
}

// Mirrors yoRadio's parseCSV gating: a line is a playlist entry iff it has
// at least two TABs with non-empty name, URL and ovol fields (raw lengths,
// exactly as the receiver checks). Anything else is skipped by both sides,
// so numbering can never diverge.
inline bool isPlaylistEntry(const char *line) {
  if (!line) return false;
  const char *t1 = strchr(line, '\t');
  if (!t1 || t1 == line) return false;
  const char *t2 = strchr(t1 + 1, '\t');
  if (!t2 || t2 == t1 + 1) return false;
  const char *r = t2 + 1;
  const char *end = r + strlen(r);
  while (end > r && (end[-1] == '\r' || end[-1] == '\n')) --end;
  return end > r;
}

// Copies at most maxLen bytes without splitting a UTF-8 sequence, so long
// Cyrillic names never end with a garbage glyph.
inline size_t utf8SafeLen(const char *s, size_t maxLen) {
  size_t n = 0;
  while (s[n] && n < maxLen) {
    uint8_t c = static_cast<uint8_t>(s[n]);
    size_t clen = 1;
    if (c >= 0xF0) clen = 4;
    else if (c >= 0xE0) clen = 3;
    else if (c >= 0xC0) clen = 2;
    else if (c >= 0x80) {
      ++n;
      continue;
    }  // stray continuation: copy through
    if (n + clen > maxLen) break;
    n += clen;
  }
  return n;
}

// Splits fetched CSV text into display names plus their receiver numbers
// (1-based positions among valid entries, exactly as play=N expects).
// Returns the number of displayed rows (<= maxCount).
inline uint8_t parseCsvList(const char *text, char (*dest)[48], uint16_t *nums, uint8_t maxCount) {
  uint8_t count = 0;
  uint16_t entryNo = 0;
  bool firstEntrySeen = false;
  const char *p = text ? text : "";
  char line[256];
  while (*p && count < maxCount) {
    size_t i = 0;
    while (*p && *p != '\n' && i < sizeof(line) - 1) line[i++] = *p++;
    if (*p == '\n') ++p;
    line[i] = '\0';
    if (!isPlaylistEntry(line)) continue;
    ++entryNo;
    if (!firstEntrySeen) {
      firstEntrySeen = true;
      if (isPlaylistHeader(line)) continue;  // hidden row, number is kept
    }
    if (!parsePlaylistName(line, dest[count], 48)) {
      snprintf(dest[count], 48, "(без названия)");
    }
    nums[count] = entryNo;
    ++count;
  }
  return count;
}

inline bool parseRatesDocument(const JsonDocument &doc, float (&rates)[4]) {
  if (!doc["securities"]["data"].is<JsonArrayConst>()) return false;
  const char *pairs[4] = {"USD/RUB", "EUR/RUB", "CNY/RUB", "GBP/RUB"};
  bool found = false;
  for (JsonArrayConst row : doc["securities"]["data"].as<JsonArrayConst>()) {
    const char *pair = row[2] | "";
    for (int i = 0; i < 4; ++i) {
      if (strcmp(pair, pairs[i]) == 0 && row[3].is<float>()) {
        rates[i] = row[3];
        found = true;
      }
    }
  }
  return found;
}

enum WeatherIcon {
  WI_SUN,
  WI_MOON,
  WI_PARTLY_DAY,
  WI_PARTLY_NIGHT,
  WI_CLOUD,
  WI_RAIN,
  WI_STORM,
  WI_SNOW,
  WI_MIST,
};

// Maps an OpenWeather icon code ("01d".."50n") to a drawable icon kind.
inline WeatherIcon weatherIconKind(const char *code) {
  if (!code || !code[0] || !code[1]) return WI_CLOUD;
  const bool night = code[2] == 'n';
  if (code[0] == '0' && code[1] == '1') return night ? WI_MOON : WI_SUN;
  if (code[0] == '0' && code[1] == '2') return night ? WI_PARTLY_NIGHT : WI_PARTLY_DAY;
  if (code[0] == '0' && (code[1] == '3' || code[1] == '4')) return WI_CLOUD;
  if (code[0] == '0' && code[1] == '9') return WI_RAIN;
  if (code[0] == '1' && code[1] == '0') return WI_RAIN;
  if (code[0] == '1' && code[1] == '1') return WI_STORM;
  if (code[0] == '1' && code[1] == '3') return WI_SNOW;
  if (code[0] == '5' && code[1] == '0') return WI_MIST;
  return WI_CLOUD;
}