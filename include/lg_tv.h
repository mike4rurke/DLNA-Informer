#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// LG NetCast TVs (e.g. LB series like 42LB65): ROAP protocol over plain HTTP,
// port 8080. Pairing key (6 digits, shown on the TV screen) is persistent per
// TV; the session id returned by AuthReq is cached and reused.
#define LG_TV_PORT 8080

// Virtual key codes (UDAP Annex A subset).
enum LgKey : int {
  LG_POWER = 1,
  LG_N0 = 2,
  LG_N1 = 3,
  LG_N2 = 4,
  LG_N3 = 5,
  LG_N4 = 6,
  LG_N5 = 7,
  LG_N6 = 8,
  LG_N7 = 9,
  LG_N8 = 10,
  LG_N9 = 11,
  LG_UP = 12,
  LG_DOWN = 13,
  LG_LEFT = 14,
  LG_RIGHT = 15,
  LG_OK = 20,
  LG_HOME_MENU = 21,
  LG_BACK = 23,
  LG_VOLUME_UP = 24,
  LG_VOLUME_DOWN = 25,
  LG_MUTE_TOGGLE = 26,
  LG_CHANNEL_UP = 27,
  LG_CHANNEL_DOWN = 28,
  LG_PLAY = 33,
  LG_PAUSE = 34,
  LG_STOP = 35,
  LG_EXTERNAL_INPUT = 47,
  LG_EXIT = 412,
};

// Endpoint paths: ROAP for 2012+ sets (incl. LB), HDCP for pre-2012 sets.
// The firmware tries ROAP first and falls back automatically.
#define LG_ROAP_AUTH "/roap/api/auth"
#define LG_ROAP_COMMAND "/roap/api/command"
#define LG_HDCP_AUTH "/hdcp/api/auth"
#define LG_HDCP_COMMAND "/hdcp/api/dtv_wifirc"

// POST /roap/api/auth — makes the TV display the pairing key.
inline void lgPairKeyRequest(char *out, size_t outSize) {
  snprintf(out, outSize,
           "<?xml version=\"1.0\" encoding=\"utf-8\"?><auth><type>AuthKeyReq</type></auth>");
}

// POST /roap/api/auth — exchanges the pairing key for a session id.
inline void lgAuthRequest(const char *pairKey, char *out, size_t outSize) {
  snprintf(out, outSize,
           "<?xml version=\"1.0\" encoding=\"utf-8\"?><auth><type>AuthReq</type><value>%s</value></auth>",
           pairKey ? pairKey : "");
}

// POST /roap/api/command — presses a remote key.
inline void lgKeyCommand(const char *session, int code, char *out, size_t outSize) {
  snprintf(out, outSize,
           "<command><session>%s</session><type>HandleKeyInput</type><value>%d</value></command>",
           session ? session : "", code);
}

// Extracts <tag>value</tag> from a small XML response. No XML parser needed.
inline bool lgFindTag(const char *xml, const char *tag, char *out, size_t outSize) {
  if (!xml || !tag || !out || outSize == 0) return false;
  char open[32];
  char close[32];
  snprintf(open, sizeof(open), "<%s>", tag);
  snprintf(close, sizeof(close), "</%s>", tag);
  const char *a = strstr(xml, open);
  if (!a) return false;
  a += strlen(open);
  const char *b = strstr(a, close);
  if (!b || b < a) return false;
  size_t n = static_cast<size_t>(b - a);
  if (n > outSize - 1) n = outSize - 1;
  memcpy(out, a, n);
  out[n] = '\0';
  return true;
}

// Command responses carry <ROAPError>200</ROAPError> on success. Some builds
// omit the tag on success, which we also accept.
inline bool lgAuthOk(const char *body) {
  if (!body) return false;
  char err[16];
  if (!lgFindTag(body, "ROAPError", err, sizeof(err))) return true;
  return strcmp(err, "200") == 0;
}
