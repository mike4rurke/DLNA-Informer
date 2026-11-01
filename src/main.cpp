#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include <time.h>

#include "clock_logic.h"
#include "config.h"
#include "data_protocol.h"
#include "lg_tv.h"
#include "services.h"

LV_FONT_DECLARE(cyd_ru12);
LV_FONT_DECLARE(cyd_ru14);
LV_FONT_DECLARE(cyd_ru18);

namespace {
// Portrait 240x320.
constexpr int SCREEN_WIDTH = 240;
constexpr int SCREEN_HEIGHT = 320;
constexpr int PAGE_Y = 25;
constexpr int PAGE_H = 247;
constexpr int TOUCH_CS = 33;
constexpr int TOUCH_IRQ = 36;
constexpr int TOUCH_CLK = 25;
constexpr int TOUCH_MISO = 39;
constexpr int TOUCH_MOSI = 32;
constexpr int BACKLIGHT_PIN = 21;
constexpr int SPEAKER_PIN = 26;
// ESP32-2432S028R onboard RGB LED, common anode (LOW = on).
constexpr int RGB_RED_PIN = 4;
constexpr int RGB_GREEN_PIN = 16;
constexpr int RGB_BLUE_PIN = 17;
constexpr int BACKLIGHT_CHANNEL = 0;
constexpr int SPEAKER_CHANNEL = 1;
constexpr int SD_CS = 5;
constexpr int SD_SCK = 18;
constexpr int SD_MISO = 19;
constexpr int SD_MOSI = 23;
constexpr uint32_t BACKGROUND = 0x0b1220;
constexpr uint32_t PANEL = 0x172235;
constexpr uint32_t TEXT = 0xf1f5f9;
constexpr uint32_t MUTED = 0x9baabe;
constexpr uint32_t ACCENT = 0xf4c53f;
constexpr uint32_t MINT = 0x52d3bd;
constexpr uint32_t DARK_TEXT = 0x0b1220;
constexpr uint32_t WEEKEND = 0xE07856;
constexpr uint32_t HOLIDAY = 0xFFB020;

TFT_eSPI display;
SPIClass touchBus(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
Preferences preferences;
lv_disp_draw_buf_t drawBuffer;
// Keep the draw buffer on the heap to leave room in static DRAM.
lv_color_t *pixels = nullptr;
lv_obj_t *page = nullptr;
lv_obj_t *headerTitle = nullptr;
lv_obj_t *headerStatus = nullptr;
lv_obj_t *navButtons[7]{};
lv_obj_t *labels[16]{};
lv_obj_t *daysButtons[7]{};
lv_obj_t *calCells[42]{};
lv_obj_t *calTitle = nullptr;
int calShownKey = -1;
lv_obj_t *alarmToggle = nullptr;
lv_obj_t *timerToggle = nullptr;
lv_obj_t *alertBox = nullptr;
lv_obj_t *modeWebBtn = nullptr;
lv_obj_t *modeSdBtn = nullptr;
lv_obj_t *radioListObj = nullptr;
lv_obj_t *speakerBtn = nullptr;
uint32_t lastRadioListVersion = 0;
bool rateGraphMode = false;
uint8_t rateGraphCurrency = 0, rateGraphMonths = 6;
uint32_t rateGraphVersion = UINT32_MAX;
RateHistory rateHistory;
lv_obj_t *rateLine = nullptr;
static lv_point_t ratePoints[RateHistory::kPoints];
// Fullscreen playlist overlay (covers the whole display).
lv_obj_t *plOverlay = nullptr;
lv_obj_t *plListObj = nullptr;
lv_obj_t *plTitleLbl = nullptr;
lv_obj_t *plWebBtn = nullptr;
lv_obj_t *plSdBtn = nullptr;
uint32_t plSeenVersion = 0;
int plSeenStation = -1;
int plSeenMode = -1;
// Pretty weather icons (RGB565 canvases over PANEL background).
static lv_color_t iconBuf[5][30 * 30];
lv_obj_t *homeIconObj = nullptr;
lv_obj_t *wxIconObjs[4]{};

struct Calibration {
  int x0 = 300;
  int y0 = 300;
  int x1 = 3800;
  int y1 = 3800;
} calibration;

void applyBrightness();
enum Screen { HOME, WEATHER, ALARM, RADIO, RATES, SETTINGS, TV };
Screen activeScreen = HOME;
const char *screenNames[7] = {"Главная", "Погода", "Сигнал", "Радио", "Курсы", "Настройки", "ТВ"};
const char *monthNames[12] = {"Январь", "Февраль", "Март", "Апрель", "Май", "Июнь",
                              "Июль", "Август", "Сентябрь", "Октябрь", "Ноябрь", "Декабрь"};
const char *dayNames[7] = {"Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Вс"};
const char *dayFullNames[7] = {"Понедельник", "Вторник", "Среда",   "Четверг",
                               "Пятница",     "Суббота",  "Воскресенье"};

bool alarmEnabled = DEFAULT_ALARM_ENABLED;
uint8_t alarmHour = DEFAULT_ALARM_HOUR;
uint8_t alarmMinute = DEFAULT_ALARM_MINUTE;
uint8_t alarmDays = DEFAULT_ALARM_DAYS;
int lastAlarmYear = -1;
int lastAlarmDay = -1;
uint32_t timerPreset = DEFAULT_TIMER_MINUTES * 60UL;
uint32_t timerRemaining = DEFAULT_TIMER_MINUTES * 60UL;
uint32_t timerDeadline = 0;
bool timerRunning = false;
bool ringing = false;
uint32_t ringStarted = 0;
int brightPct = DISPLAY_BRIGHTNESS;
bool spkOn = ALARM_SPEAKER_ENABLED != 0;
AppData appData;

// SD-card file browser state (device file manager, read-only).
static constexpr int kSdMax = 12;
char sdNames[kSdMax][48]{};
char sdInfo[kSdMax][24]{};
bool sdDirs[kSdMax]{};
String sdPath = "/";
uint32_t sdOffset = 0;
bool sdHasNext = false;
WebServer sdWeb(80);
uint8_t sdFileCount = 0;
char sdStatus[64] = "SD карта не проверена";

enum Action : intptr_t {
  ALARM_ENABLE = 1, HOUR_MINUS, HOUR_PLUS, MINUTE_MINUS, MINUTE_PLUS,
  DAY_BASE = 20,
  TIMER_1 = 40, TIMER_5, TIMER_10, TIMER_15, TIMER_START, TIMER_RESET,
  RADIO_PREVIOUS = 60, RADIO_TOGGLE, RADIO_NEXT, RADIO_VOLUME_DOWN, RADIO_VOLUME_UP,
  RADIO_MODE_WEB = 65, RADIO_MODE_SD = 66, RADIO_LIST_REFRESH = 67,
  PLAYLIST_OPEN = 68, PLAYLIST_CLOSE = 69,
  TV_POWER = 150, TV_MUTE, TV_VUP, TV_VDOWN, TV_CHUP, TV_CHDOWN,
  TV_UP, TV_DOWN, TV_LEFT, TV_RIGHT, TV_OK, TV_BACK, TV_HOME, TV_EXIT, TV_INPUT, TV_PAIR, TV_STS, TV_TNT, TV_NTV, TV_ORT,
  REFRESH_DATA = 80, ALERT_DISMISS,
  STATION_BASE = 100,
  SET_BRIGHT_DOWN = 140, SET_BRIGHT_UP, SET_SPEAKER, SET_RESET_CAL, SD_REFRESH, SD_PARENT, SD_PREV, SD_NEXT,
  SD_ITEM_BASE = 300, RATE_ITEM_BASE = 350, RATE_GRAPH_BACK = 360, RATE_6M, RATE_1Y,
};

lv_color_t color(uint32_t value) { return lv_color_hex(value); }

void flushDisplay(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *data) {
  display.startWrite();
  display.setAddrWindow(area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
  display.pushColors(reinterpret_cast<uint16_t *>(&data->full),
                     (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1), true);
  display.endWrite();
  lv_disp_flush_ready(driver);
}

void readTouch(lv_indev_drv_t *, lv_indev_data_t *data) {
  if (!touch.touched()) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  const TS_Point p = touch.getPoint();
  if (p.z < 200) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  data->point.x = constrain(map(p.x, calibration.x0, calibration.x1, 10, 230), 0, SCREEN_WIDTH - 1);
  data->point.y = constrain(map(p.y, calibration.y0, calibration.y1, 10, 310), 0, SCREEN_HEIGHT - 1);
  data->state = LV_INDEV_STATE_PRESSED;
}

bool calibrationPoint(int targetX, int targetY, int &rawX, int &rawY) {
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextFont(1);
  display.drawString("TOUCH THE YELLOW CIRCLE", 70, 140);
  display.drawString("Wait 30s to use defaults", 66, 158);
  display.drawCircle(targetX, targetY, 10, TFT_YELLOW);
  display.drawLine(targetX - 14, targetY, targetX + 14, targetY, TFT_YELLOW);
  display.drawLine(targetX, targetY - 14, targetX, targetY + 14, TFT_YELLOW);
  // The calibration prompt is already drawn; make it visible before waiting for touch.
  applyBrightness();
  const uint32_t started = millis();
  while (millis() - started < 30000UL) {
    if (touch.touched()) {
      const TS_Point point = touch.getPoint();
      if (point.z >= 200) {
        rawX = point.x;
        rawY = point.y;
        delay(250);
        while (touch.touched() && millis() - started < 30000UL) delay(20);
        delay(100);
        return true;
      }
    }
    delay(20);
  }
  return false;
}

void setupCalibration() {
  if (preferences.isKey("pcalx0") && digitalRead(0) != LOW) {
    calibration.x0 = preferences.getInt("pcalx0", calibration.x0);
    calibration.y0 = preferences.getInt("pcaly0", calibration.y0);
    calibration.x1 = preferences.getInt("pcalx1", calibration.x1);
    calibration.y1 = preferences.getInt("pcaly1", calibration.y1);
    if (abs(calibration.x1 - calibration.x0) > 100 && abs(calibration.y1 - calibration.y0) > 100) return;
  }
  Calibration next;
  if (calibrationPoint(20, 20, next.x0, next.y0) &&
      calibrationPoint(220, 300, next.x1, next.y1) &&
      abs(next.x1 - next.x0) > 100 && abs(next.y1 - next.y0) > 100) {
    calibration = next;
    preferences.putInt("pcalx0", calibration.x0);
    preferences.putInt("pcaly0", calibration.y0);
    preferences.putInt("pcalx1", calibration.x1);
    preferences.putInt("pcaly1", calibration.y1);
  }
}

lv_obj_t *box(lv_obj_t *parent, int x, int y, int width, int height, uint32_t background = PANEL) {
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, width, height);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(obj, color(background), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_radius(obj, 6, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  return obj;
}

// Preserve original data; replace missing glyphs only in text sent to LVGL.
void setSafeLabelText(lv_obj_t *obj, const char *text) {
  if (!text) { lv_label_set_text(obj, ""); return; }
  const lv_font_t *font=lv_obj_get_style_text_font(obj, LV_PART_MAIN);
  String safe; safe.reserve(strlen(text)+1);
  const size_t len=strlen(text);
  for (size_t i=0; i<len;) {
    const unsigned char c=(unsigned char)text[i]; uint32_t cp=0; size_t bytes=1;
    if(c<0x80) cp=c;
    else if(c>=0xC2 && c<=0xDF && i+1<len && ((unsigned char)text[i+1]&0xC0)==0x80) { cp=((c&31)<<6)|((unsigned char)text[i+1]&63); bytes=2; }
    else if(c>=0xE0 && c<=0xEF && i+2<len && ((unsigned char)text[i+1]&0xC0)==0x80 && ((unsigned char)text[i+2]&0xC0)==0x80) { cp=((c&15)<<12)|(((unsigned char)text[i+1]&63)<<6)|((unsigned char)text[i+2]&63); bytes=3; }
    else if(c>=0xF0 && c<=0xF4 && i+3<len && ((unsigned char)text[i+1]&0xC0)==0x80 && ((unsigned char)text[i+2]&0xC0)==0x80 && ((unsigned char)text[i+3]&0xC0)==0x80) { cp=((c&7)<<18)|(((unsigned char)text[i+1]&63)<<12)|(((unsigned char)text[i+2]&63)<<6)|((unsigned char)text[i+3]&63); bytes=4; }
    lv_font_glyph_dsc_t glyph{};
    bool present = (cp=='\n' || cp=='\r') || (cp>=32 && font && lv_font_get_glyph_dsc(font,&glyph,cp,0) && !glyph.is_placeholder);
    if(present) safe.concat(text+i,bytes); else safe += '?';
    i+=bytes;
  }
  const char *current = lv_label_get_text(obj);
  if (!current || strcmp(current, safe.c_str()) != 0) lv_label_set_text(obj,safe.c_str());
}
#define lv_label_set_text setSafeLabelText

lv_obj_t *label(lv_obj_t *parent, int x, int y, int width, const char *text,
                const lv_font_t *font = &cyd_ru14, uint32_t textColor = TEXT) {
  lv_obj_t *obj = lv_label_create(parent);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_width(obj, width);
  lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_font(obj, font, 0);
  lv_obj_set_style_text_color(obj, color(textColor), 0);
  lv_label_set_text(obj, text);
  return obj;
}

void onAction(lv_event_t *event);
void onNavigate(lv_event_t *event);
void showScreen(Screen screen);
void refreshScreen();
void buildTv();
void refreshTv();

lv_obj_t *button(lv_obj_t *parent, int x, int y, int width, int height,
                 const char *text, intptr_t action, uint32_t background = PANEL) {
  lv_obj_t *obj = lv_btn_create(parent);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, width, height);
  lv_obj_set_style_radius(obj, 5, 0);
  lv_obj_set_style_bg_color(obj, color(background), 0);
  lv_obj_set_style_shadow_width(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_add_event_cb(obj, onAction, LV_EVENT_CLICKED, reinterpret_cast<void *>(action));
  lv_obj_t *textLabel = lv_label_create(obj);
  lv_obj_set_style_text_font(textLabel, &cyd_ru14, 0);
  lv_obj_set_style_text_color(textLabel, color(TEXT), 0);
  lv_label_set_text(textLabel, text);
  lv_obj_center(textLabel);
  return obj;
}

void buttonText(lv_obj_t *obj, const char *text) {
  if (obj) lv_label_set_text(lv_obj_get_child(obj, 0), text);
}

void saveAlarm() {
  preferences.putBool("alarmOn", alarmEnabled);
  preferences.putUChar("alarmHour", alarmHour);
  preferences.putUChar("alarmMin", alarmMinute);
  preferences.putUChar("alarmDays", alarmDays);
}

void applyBrightness() {
  ledcWrite(BACKLIGHT_CHANNEL, map(constrain(brightPct, 20, 100), 0, 100, 0, 255));
}

void saveDeviceSettings() {
  preferences.putUChar("bright", static_cast<uint8_t>(constrain(brightPct, 20, 100)));
  preferences.putBool("spk", spkOn);
}

// Dedicated task so a blocking SD transfer does not freeze the blinking LED.
QueueHandle_t alarmLedQueue = nullptr;
void setAlarmLed(uint8_t mask) {
  digitalWrite(RGB_RED_PIN,   (mask & 1) ? LOW : HIGH);
  digitalWrite(RGB_GREEN_PIN, (mask & 2) ? LOW : HIGH);
  digitalWrite(RGB_BLUE_PIN,  (mask & 4) ? LOW : HIGH);
}
void alarmLedTask(void *) {
  static const uint8_t colors[] = {1, 2, 4, 3, 6, 5, 7};
  bool active=false, lit=false;
  unsigned index=0;
  for (;;) {
    bool requested=false;
    if(xQueueReceive(alarmLedQueue, &requested, active ? pdMS_TO_TICKS(lit ? 300 : 150) : portMAX_DELAY) == pdTRUE) {
      active=requested; lit=active; index=0;
      setAlarmLed(active ? colors[index] : 0);
    } else if(active) {
      lit=!lit;
      if(lit) index=(index+1) % (sizeof(colors)/sizeof(colors[0]));
      setAlarmLed(lit ? colors[index] : 0);
    }
  }
}
void setupAlarmLed() {
  const int pins[] = {RGB_RED_PIN,RGB_GREEN_PIN,RGB_BLUE_PIN};
  for(int pin : pins) { digitalWrite(pin,HIGH); pinMode(pin,OUTPUT); }
  setAlarmLed(0);
  alarmLedQueue=xQueueCreate(1,sizeof(bool));
  if(alarmLedQueue && xTaskCreatePinnedToCore(alarmLedTask,"alarm-led",2048,nullptr,1,nullptr,0)!=pdPASS) {
    vQueueDelete(alarmLedQueue); alarmLedQueue=nullptr;
  }
}
void setAlarmLedActive(bool active) {
  if(alarmLedQueue) xQueueOverwrite(alarmLedQueue,&active);
}

void dismissAlert() {
  ringing = false;
  setAlarmLedActive(false);
  ledcWriteTone(SPEAKER_CHANNEL, 0);
  if (alertBox) {
    lv_obj_del(alertBox);
    alertBox = nullptr;
  }
}

void startAlert(bool alarm) {
  if (ringing) return;
  ringing = true;
  ringStarted = millis();
  setAlarmLedActive(alarm);
  if (alarm) sendRadioCommand("start=1");
  alertBox = box(lv_scr_act(), 15, 100, 210, 110, 0x603b23);
  label(alertBox, 14, 12, 182, alarm ? "БУДИЛЬНИК" : "ТАЙМЕР ЗАВЕРШЁН", &cyd_ru18, 0xffffff);
  label(alertBox, 14, 42, 182, "Тап - остановить сигнал", &cyd_ru14, 0xffffff);
  button(alertBox, 53, 70, 104, 28, "Остановить", ALERT_DISMISS, 0x94602a);
  lv_obj_move_foreground(alertBox);
}

void refreshDays() {
  for (int i = 0; i < 7; ++i) {
    if (!daysButtons[i]) continue;
    lv_obj_set_style_bg_color(daysButtons[i], color((alarmDays & (1U << i)) ? 0x8b6c18 : PANEL), 0);
  }
}

void refreshModeButtons() {
  const bool sd = appData.radio.mode == 1;
  if (modeWebBtn) lv_obj_set_style_bg_color(modeWebBtn, color(sd ? PANEL : 0x8b6c18), 0);
  if (modeSdBtn) lv_obj_set_style_bg_color(modeSdBtn, color(sd ? 0x8b6c18 : PANEL), 0);
  if (plWebBtn) lv_obj_set_style_bg_color(plWebBtn, color(sd ? PANEL : 0x8b6c18), 0);
  if (plSdBtn) lv_obj_set_style_bg_color(plSdBtn, color(sd ? 0x8b6c18 : PANEL), 0);
}

// All icon geometry is authored on a 64x64 grid and scaled to S.
static void iconLine(lv_obj_t *cv, int x1, int y1, int x2, int y2, uint32_t c, int w, int S) {
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = color(c);
  d.width = static_cast<lv_coord_t>(w < 1 ? 1 : w);
  d.round_start = 1;
  d.round_end = 1;
  const lv_point_t p[2] = {{static_cast<lv_coord_t>(x1 * S / 64), static_cast<lv_coord_t>(y1 * S / 64)},
                           {static_cast<lv_coord_t>(x2 * S / 64), static_cast<lv_coord_t>(y2 * S / 64)}};
  lv_canvas_draw_line(cv, p, 2, &d);
}

static void iconDisc(lv_obj_t *cv, int cx, int cy, int r, uint32_t c, int S) {
  if (r < 1) return;
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = color(c);
  d.bg_opa = LV_OPA_COVER;
  d.border_width = 0;
  d.radius = LV_RADIUS_CIRCLE;
  const int rr = r * S / 64;
  lv_canvas_draw_rect(cv, cx * S / 64 - rr, cy * S / 64 - rr, rr * 2, rr * 2, &d);
}

static void iconBar(lv_obj_t *cv, int x, int y, int w, int h, int rad, uint32_t c, int S) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = color(c);
  d.bg_opa = LV_OPA_COVER;
  d.border_width = 0;
  d.radius = static_cast<lv_coord_t>(rad * S / 64);
  lv_canvas_draw_rect(cv, x * S / 64, y * S / 64, w * S / 64, h * S / 64, &d);
}

static void iconBolt(lv_obj_t *cv, const int pts[][2], int n, uint32_t c, int S) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = color(c);
  d.bg_opa = LV_OPA_COVER;
  d.border_width = 0;
  lv_point_t p[8];
  for (int i = 0; i < n && i < 8; ++i) {
    p[i].x = static_cast<lv_coord_t>(pts[i][0] * S / 64);
    p[i].y = static_cast<lv_coord_t>(pts[i][1] * S / 64);
  }
  lv_canvas_draw_polygon(cv, p, static_cast<uint16_t>(n), &d);
}

static void iconSun(lv_obj_t *cv, int cx, int cy, int r, int S) {
  static const int dirs[8][2] = {{10, 0}, {7, 7}, {0, 10}, {-7, 7},
                                 {-10, 0}, {-7, -7}, {0, -10}, {7, -7}};
  for (auto &dir : dirs) {
    iconLine(cv, cx + dir[0] * r / 10, cy + dir[1] * r / 10, cx + dir[0] * (r + 6) / 10,
             cy + dir[1] * (r + 6) / 10, 0xFFC93C, S / 14 + 2, S);
  }
  iconDisc(cv, cx, cy, r, 0xFFD54A, S);
  iconDisc(cv, cx - r / 4, cy - r / 4, r / 2, 0xFFE89A, S);
}

static void iconCloud(lv_obj_t *cv, int cx, int cy, int w, uint32_t top, uint32_t shade, int S) {
  const int h = w * 11 / 22;
  const int r = h / 2;
  iconBar(cv, cx - w / 2, cy - h / 2 + 3, w, h, r, shade, S);
  iconDisc(cv, cx - w / 5, cy - 1, w / 5, shade, S);
  iconDisc(cv, cx + w / 6, cy - 4, w / 4, shade, S);
  iconBar(cv, cx - w / 2, cy - h / 2, w, h, r, top, S);
  iconDisc(cv, cx - w / 5, cy - 1, w / 5, top, S);
  iconDisc(cv, cx + w / 6, cy - 4, w / 4, top, S);
}

static void iconFlake(lv_obj_t *cv, int cx, int cy, int arm, uint32_t c, int S) {
  iconLine(cv, cx - arm, cy, cx + arm, cy, c, 3, S);
  iconLine(cv, cx - arm * 7 / 10, cy - arm * 7 / 10, cx + arm * 7 / 10, cy + arm * 7 / 10, c, 3, S);
  iconLine(cv, cx - arm * 7 / 10, cy + arm * 7 / 10, cx + arm * 7 / 10, cy - arm * 7 / 10, c, 3, S);
}

void paintIcon(lv_obj_t *cv, WeatherIcon kind, int S) {
  iconBar(cv, 0, 0, 64, 64, 0, PANEL, S);
  switch (kind) {
    case WI_SUN: iconSun(cv, 32, 32, 14, S); break;
    case WI_MOON:
      iconDisc(cv, 32, 32, 13, 0xF5F0DC, S);
      iconDisc(cv, 28, 28, 3, 0xD9D2B8, S);
      iconDisc(cv, 36, 35, 2, 0xD9D2B8, S);
      break;
    case WI_PARTLY_DAY:
      iconSun(cv, 23, 22, 10, S);
      iconCloud(cv, 38, 42, 34, 0xFFFFFF, 0xC9D8EC, S);
      break;
    case WI_PARTLY_NIGHT:
      iconDisc(cv, 23, 22, 9, 0xF5F0DC, S);
      iconCloud(cv, 38, 42, 34, 0xFFFFFF, 0xC9D8EC, S);
      break;
    case WI_CLOUD: iconCloud(cv, 32, 34, 46, 0xFFFFFF, 0xC9D8EC, S); break;
    case WI_RAIN:
      iconCloud(cv, 32, 26, 42, 0xFFFFFF, 0xC9D8EC, S);
      iconLine(cv, 20, 44, 15, 56, 0x5AA9FF, 4, S);
      iconLine(cv, 32, 44, 27, 56, 0x5AA9FF, 4, S);
      iconLine(cv, 44, 44, 39, 56, 0x5AA9FF, 4, S);
      break;
    case WI_SNOW:
      iconCloud(cv, 32, 26, 42, 0xFFFFFF, 0xC9D8EC, S);
      iconFlake(cv, 20, 50, 4, 0x8FBDF5, S);
      iconFlake(cv, 32, 53, 4, 0x8FBDF5, S);
      iconFlake(cv, 44, 50, 4, 0x8FBDF5, S);
      break;
    case WI_STORM: {
      iconCloud(cv, 32, 26, 42, 0xDDE6F2, 0xA9BBD4, S);
      static const int bolt[6][2] = {{38, 38}, {28, 56}, {34, 56}, {30, 68}, {44, 48}, {37, 48}};
      iconBolt(cv, bolt, 6, 0xFFD94D, S);
      break;
    }
    case WI_MIST:
      iconCloud(cv, 32, 22, 36, 0xE4EBF5, 0xB9C8DC, S);
      iconLine(cv, 18, 44, 46, 44, 0xA9BCD4, 3, S);
      iconLine(cv, 22, 50, 42, 50, 0xA9BCD4, 3, S);
      iconLine(cv, 18, 56, 46, 56, 0xA9BCD4, 3, S);
      break;
  }
}

lv_obj_t *makeIcon(lv_obj_t *parent, int x, int y, int S, lv_color_t *buf, WeatherIcon kind) {
  lv_obj_t *cv = lv_canvas_create(parent);
  lv_canvas_set_buffer(cv, buf, static_cast<lv_coord_t>(S), static_cast<lv_coord_t>(S), LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(cv, x, y);
  lv_obj_set_size(cv, S, S);
  lv_obj_set_style_border_width(cv, 0, 0);
  lv_obj_set_style_pad_all(cv, 0, 0);
  lv_obj_set_style_radius(cv, 0, 0);
  lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  paintIcon(cv, kind, S);
  return cv;
}

bool dateIsHoliday(int mon1, int mday) {
  const int date = mon1 * 100 + mday;
  return date == 101 || date == 107 || date == 223 || date == 308 || date == 501 || date == 509 ||
         date == 612 || date == 1104;
}

void refillCalendar(const tm &local) {
  const int year = local.tm_year + 1900;
  const int mon = local.tm_mon;
  const int daysPerMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int count = daysPerMonth[mon];
  if (mon == 1 && (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0)) ++count;
  const int firstWeekday = ((local.tm_wday + 6) % 7 - (local.tm_mday - 1) % 7 + 7) % 7;
  if (calTitle) lv_label_set_text_fmt(calTitle, "%s %d", monthNames[mon], year);
  for (int i = 0; i < 42; ++i) {
    lv_obj_t *cell = calCells[i];
    if (!cell) continue;
    lv_obj_t *cap = lv_obj_get_child(cell, 0);
    const int dayNum = i - firstWeekday + 1;
    const bool inMonth = dayNum >= 1 && dayNum <= count;
    if (!inMonth) {
      lv_label_set_text(cap, "");
      lv_obj_set_style_bg_color(cell, color(PANEL), 0);
      continue;
    }
    char num[8];
    const bool holiday = dateIsHoliday(mon + 1, dayNum);
    if (holiday) snprintf(num, sizeof(num), "%d*", dayNum);
    else snprintf(num, sizeof(num), "%d", dayNum);
    lv_label_set_text(cap, num);
    const int col = i % 7;
    if (dayNum == local.tm_mday) {
      lv_obj_set_style_bg_color(cell, color(ACCENT), 0);
      lv_obj_set_style_text_color(cap, color(DARK_TEXT), 0);
    } else {
      lv_obj_set_style_bg_color(cell, color(0x101a2a), 0);
      uint32_t tc = TEXT;
      if (holiday) tc = HOLIDAY;
      else if (col >= 5) tc = WEEKEND;
      lv_obj_set_style_text_color(cap, color(tc), 0);
    }
  }
  calShownKey = year * 1000 + local.tm_yday;
}

void fillList(lv_obj_t *container, bool sdMode, int btnW, int btnH, int step) {
  if (!container) return;
  lv_obj_clean(container);
  const uint8_t count = sdMode ? appData.radio.sdCount : appData.radio.stationCount;
  if (count == 0) {
    lv_obj_t *empty = lv_label_create(container);
    lv_obj_set_pos(empty, 6, 4);
    lv_obj_set_width(empty, btnW - 12);
    lv_obj_set_style_text_font(empty, &cyd_ru14, 0);
    lv_obj_set_style_text_color(empty, color(MUTED), 0);
    lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    lv_label_set_text(empty, sdMode ? "SD пусто: сохраните плейлист в веб-панели" : "Нет станций: проверьте playlist.csv");
    return;
  }
  int y = 2;
  const uint16_t *nums = sdMode ? appData.radio.sdNums : appData.radio.stationNums;
  for (uint8_t i = 0; i < count; ++i) {
    const char *name = sdMode ? appData.radio.sdFiles[i] : appData.radio.stations[i];
    lv_obj_t *item = lv_btn_create(container);
    lv_obj_set_pos(item, 2, y);
    lv_obj_set_size(item, btnW, btnH);
    lv_obj_set_style_radius(item, 4, 0);
    // Rows are numbered from 1 in display order for a clean list; tapping
    // sends the stored receiver number, so display and switching agree.
    const bool current = nums[i] != 0 && nums[i] == appData.radio.station;
    lv_obj_set_style_bg_color(item, color(current ? ACCENT : 0x1d2c46), 0);
    lv_obj_set_style_shadow_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_add_event_cb(item, onAction, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(STATION_BASE + i)));
    lv_obj_t *caption = lv_label_create(item);
    lv_obj_set_style_text_font(caption, &cyd_ru14, 0);
    lv_obj_set_style_text_color(caption, color(current ? DARK_TEXT : TEXT), 0);
    lv_obj_set_width(caption, btnW - 44);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_CLIP);
    char numbered[56];
    snprintf(numbered, sizeof(numbered), "%u. %s", static_cast<unsigned>(i + 1), name);
    lv_label_set_text(caption, numbered);
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, 22, 0);
    y += step;
  }
}

void buildRadioList() {
  if (!radioListObj) return;
  fillList(radioListObj, appData.radio.mode == 1, 216, 24, 27);
}

void fillPlaylist() {
  if (!plOverlay || !plListObj) return;
  const bool sdMode = appData.radio.mode == 1;
  const uint8_t count = sdMode ? appData.radio.sdCount : appData.radio.stationCount;
  if (plTitleLbl) {
    char title[48];
    snprintf(title, sizeof(title), "Плейлист - %s %u", sdMode ? "SD" : "Радио", static_cast<unsigned>(count));
    lv_label_set_text(plTitleLbl, title);
  }
  refreshModeButtons();
  fillList(plListObj, sdMode, 216, 28, 31);
  plSeenVersion = appData.radio.listVersion;
  plSeenStation = appData.radio.station;
  plSeenMode = appData.radio.mode;
}

void openPlaylist() {
  if (plOverlay) {
    fillPlaylist();
    return;
  }
  plOverlay = box(lv_scr_act(), 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, BACKGROUND);
  plTitleLbl = label(plOverlay, 8, 6, 150, "Плейлист", &cyd_ru18, ACCENT);
  button(plOverlay, 196, 4, 36, 28, "X", PLAYLIST_CLOSE, 0x5a2b2b);
  plWebBtn = button(plOverlay, 8, 36, 110, 28, "Радио", RADIO_MODE_WEB);
  plSdBtn = button(plOverlay, 122, 36, 110, 28, "SD", RADIO_MODE_SD);
  plListObj = lv_obj_create(plOverlay);
  lv_obj_set_pos(plListObj, 6, 68);
  lv_obj_set_size(plListObj, 228, 244);
  lv_obj_set_style_bg_color(plListObj, color(0x101a2a), 0);
  lv_obj_set_style_bg_opa(plListObj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(plListObj, 0, 0);
  lv_obj_set_style_radius(plListObj, 6, 0);
  lv_obj_set_style_pad_all(plListObj, 0, 0);
  lv_obj_set_scrollbar_mode(plListObj, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_move_foreground(plOverlay);
  requestRadioLists();
  copyAppData(appData);
  fillPlaylist();
}

void closePlaylist() {
  if (plOverlay) {
    lv_obj_del(plOverlay);
    plOverlay = nullptr;
    plListObj = nullptr;
    plTitleLbl = nullptr;
    plWebBtn = nullptr;
    plSdBtn = nullptr;
  }
}

TvData tvState;

void buildTv() {
  copyTvData(tvState);
  lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  label(page, 8, 2, 200, "Телевизор LG", &cyd_ru14, ACCENT);
  labels[0] = label(page, 8, 22, 224, "Нет настроек ТВ", &cyd_ru12, MUTED);
  button(page, 6, 42, 54, 29, "СТС", TV_STS);
  button(page, 64, 42, 54, 29, "ТНТ", TV_TNT);
  button(page, 122, 42, 54, 29, "НТВ", TV_NTV);
  button(page, 180, 42, 54, 29, "ОРТ", TV_ORT);
  button(page, 6, 76, 150, 30, "Питание", TV_POWER, 0x8b6c18);
  button(page, 160, 76, 74, 30, "Без звука", TV_MUTE);
  button(page, 6, 110, 60, 28, "Громк -", TV_VDOWN);
  labels[1] = label(page, 70, 112, 100, "--", &cyd_ru18);
  lv_obj_set_style_text_align(labels[1], LV_TEXT_ALIGN_CENTER, 0);
  button(page, 174, 110, 60, 28, "Громк +", TV_VUP);
  button(page, 6, 142, 60, 28, "Кан -", TV_CHDOWN);
  labels[2] = label(page, 70, 144, 100, "--", &cyd_ru14);
  lv_obj_set_style_text_align(labels[2], LV_TEXT_ALIGN_CENTER, 0);
  button(page, 174, 142, 60, 28, "Кан +", TV_CHUP);
  button(page, 91, 174, 58, 28, "Вверх", TV_UP);
  button(page, 29, 206, 58, 28, "Влево", TV_LEFT);
  button(page, 91, 206, 58, 28, "OK", TV_OK);
  button(page, 153, 206, 58, 28, "Вправо", TV_RIGHT);
  button(page, 91, 238, 58, 28, "Вниз", TV_DOWN);
  button(page, 6, 270, 72, 28, "Назад", TV_BACK);
  button(page, 82, 270, 72, 28, "Домой", TV_HOME);
  button(page, 158, 270, 72, 28, "Выход", TV_EXIT);
  button(page, 6, 302, 112, 28, "Источник", TV_INPUT);
  button(page, 122, 302, 112, 28, "Сопряжение", TV_PAIR);
  refreshTv();
}

void refreshTv() {
  copyTvData(tvState);
  if (!tvState.enabled) {
    lv_label_set_text(labels[0], "Нет настроек ТВ");
  } else if (!tvState.sessionOk) {
    lv_label_set_text(labels[0], tvState.message[0] ? tvState.message : "Нет сессии");
  } else {
    lv_label_set_text(labels[0], tvState.message[0] ? tvState.message : "Готов");
  }
  if (tvState.volume < 0) lv_label_set_text(labels[1], "--");
  else lv_label_set_text_fmt(labels[1], "%d", tvState.volume);
  lv_label_set_text(labels[2], tvState.channel[0] ? tvState.channel : "--");
}

void onAction(lv_event_t *event) {
  const intptr_t action = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
  if (action >= RATE_ITEM_BASE && action < RATE_ITEM_BASE + 4) {
    rateGraphCurrency = action - RATE_ITEM_BASE; rateGraphMonths = 6; rateGraphMode = true;
    requestRateHistory(rateGraphCurrency,rateGraphMonths); showScreen(RATES); return;
  }
  if (action >= SD_ITEM_BASE && action < SD_ITEM_BASE + kSdMax) {
    unsigned i = action - SD_ITEM_BASE;
    if (i < sdFileCount && sdDirs[i]) {
      String next = sdPath == "/" ? "/" + String(sdNames[i]) : sdPath + "/" + sdNames[i];
      if (next.length() < 240) { sdPath = next; sdOffset = 0; showScreen(SETTINGS); }
    }
    return;
  }
  if (action >= DAY_BASE && action < DAY_BASE + 7) {
    alarmDays ^= static_cast<uint8_t>(1U << (action - DAY_BASE));
    saveAlarm();
    refreshDays();
    return;
  }
  if (action >= STATION_BASE && action < STATION_BASE + RadioData::kListMax) {
    // Send the receiver number shown on the tapped row, not the row position.
    const int idx = static_cast<int>(action - STATION_BASE);
    const bool sdMode = appData.radio.mode == 1;
    const uint8_t count = sdMode ? appData.radio.sdCount : appData.radio.stationCount;
    if (idx >= 0 && idx < count) {
      const uint16_t num =
          sdMode ? appData.radio.sdNums[idx] : appData.radio.stationNums[idx];
      if (num > 0) {
        char query[16];
        snprintf(query, sizeof(query), "play=%u", static_cast<unsigned>(num));
        sendRadioCommand(query);
      }
    }
    refreshScreen();
    return;
  }
  switch (action) {
    case ALARM_ENABLE: alarmEnabled = !alarmEnabled; saveAlarm(); break;
    case HOUR_MINUS: alarmHour = (alarmHour + 23) % 24; saveAlarm(); break;
    case HOUR_PLUS: alarmHour = (alarmHour + 1) % 24; saveAlarm(); break;
    case MINUTE_MINUS: alarmMinute = (alarmMinute + 59) % 60; saveAlarm(); break;
    case MINUTE_PLUS: alarmMinute = (alarmMinute + 1) % 60; saveAlarm(); break;
    case TIMER_1: timerPreset = timerRemaining = 60; timerRunning = false; break;
    case TIMER_5: timerPreset = timerRemaining = 300; timerRunning = false; break;
    case TIMER_10: timerPreset = timerRemaining = 600; timerRunning = false; break;
    case TIMER_15: timerPreset = timerRemaining = 900; timerRunning = false; break;
    case TIMER_START:
      if (timerRunning) {
        timerRunning = false;
      } else {
        if (!timerRemaining) timerRemaining = timerPreset;
        timerDeadline = millis() + timerRemaining * 1000UL;
        timerRunning = true;
      }
      break;
    case TIMER_RESET: timerRunning = false; timerRemaining = timerPreset; break;
    case RADIO_PREVIOUS: sendRadioCommand("prev=1"); break;
    case RADIO_TOGGLE: sendRadioCommand("toggle=1"); break;
    case RADIO_NEXT: sendRadioCommand("next=1"); break;
    case RADIO_VOLUME_DOWN:
    case RADIO_VOLUME_UP: {
      const int direction = action == RADIO_VOLUME_UP ? 5 : -5;
      char command[24];
      snprintf(command, sizeof(command), "vol=%d", constrain(appData.radio.volume + direction, 0, 100));
      sendRadioCommand(command);
      break;
    }
    case RADIO_MODE_WEB:
      setRadioMode(0);
      requestRadioLists();  // lists depend on the receiver mode: refetch now
      buildRadioList();
      if (plOverlay) fillPlaylist();
      break;
    case RADIO_MODE_SD:
      setRadioMode(1);
      requestRadioLists();  // lists depend on the receiver mode: refetch now
      buildRadioList();
      if (plOverlay) fillPlaylist();
      break;
    case RADIO_LIST_REFRESH: requestRadioLists(); break;
    case PLAYLIST_OPEN: openPlaylist(); break;
    case PLAYLIST_CLOSE: closePlaylist(); return;
    case SET_BRIGHT_DOWN:
      brightPct = constrain(brightPct - 5, 20, 100);
      applyBrightness();
      saveDeviceSettings();
      break;
    case SET_BRIGHT_UP:
      brightPct = constrain(brightPct + 5, 20, 100);
      applyBrightness();
      saveDeviceSettings();
      break;
    case SET_SPEAKER: spkOn = !spkOn; saveDeviceSettings(); break;
    case SET_RESET_CAL:
      preferences.remove("pcalx0");
      preferences.remove("pcaly0");
      preferences.remove("pcalx1");
      preferences.remove("pcaly1");
      ESP.restart();
      break;
    case RATE_GRAPH_BACK: rateGraphMode=false; showScreen(RATES); return;
    case RATE_6M: case RATE_1Y:
      rateGraphMonths = action == RATE_6M ? 6 : 12;
      requestRateHistory(rateGraphCurrency,rateGraphMonths); showScreen(RATES); return;
    case SD_REFRESH: showScreen(SETTINGS); return;
    case SD_PARENT: {
      if (sdPath != "/") { int pos = sdPath.lastIndexOf('/'); sdPath = pos <= 0 ? "/" : sdPath.substring(0, pos); }
      sdOffset = 0; showScreen(SETTINGS); return;
    }
    case SD_PREV: sdOffset = sdOffset >= kSdMax ? sdOffset - kSdMax : 0; showScreen(SETTINGS); return;
    case SD_NEXT: if (sdHasNext) sdOffset += kSdMax; showScreen(SETTINGS); return;
    case TV_POWER: sendTvKey(LG_POWER); requestTvStatus(); break;
    case TV_MUTE: sendTvKey(LG_MUTE_TOGGLE); requestTvStatus(); break;
    case TV_VUP: sendTvKey(LG_VOLUME_UP); requestTvStatus(); break;
    case TV_VDOWN: sendTvKey(LG_VOLUME_DOWN); requestTvStatus(); break;
    case TV_CHUP: sendTvKey(LG_CHANNEL_UP); requestTvStatus(); break;
    case TV_CHDOWN: sendTvKey(LG_CHANNEL_DOWN); requestTvStatus(); break;
    case TV_UP: sendTvKey(LG_UP); break;
    case TV_DOWN: sendTvKey(LG_DOWN); break;
    case TV_LEFT: sendTvKey(LG_LEFT); break;
    case TV_RIGHT: sendTvKey(LG_RIGHT); break;
    case TV_OK: sendTvKey(LG_OK); break;
    case TV_BACK: sendTvKey(LG_BACK); break;
    case TV_HOME: sendTvKey(LG_HOME_MENU); break;
    case TV_EXIT: sendTvKey(LG_EXIT); break;
    case TV_INPUT: sendTvKey(LG_EXTERNAL_INPUT); break;
    case TV_PAIR: tvPairShowKey(); break;
    case TV_STS: sendTvChannel(13); break;
    case TV_TNT: sendTvChannel(19); break;
    case TV_NTV: sendTvChannel(4); break;
    case TV_ORT: sendTvChannel(9); break;
    case REFRESH_DATA: requestDataRefresh(); requestRadioLists(); break;
    case ALERT_DISMISS: dismissAlert(); return;
    default: break;
  }
  refreshScreen();
}

void onNavigate(lv_event_t *event) {
  showScreen(static_cast<Screen>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event))));
}

void buildHome() {
  // The weather card and calendar remain readable; scroll down for the last weeks.
  lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  labels[0] = label(page, 8, 0, 224, "--:--", &lv_font_montserrat_48, ACCENT);
  lv_obj_set_style_text_align(labels[0], LV_TEXT_ALIGN_CENTER, 0);
  labels[5] = label(page, 202, 19, 30, "--", &cyd_ru18, MUTED);
  labels[1] = label(page, 8, 55, 224, "Ожидание NTP", &cyd_ru14, MUTED);
  lv_obj_set_style_text_align(labels[1], LV_TEXT_ALIGN_CENTER, 0);
  box(page, 6, 76, 228, 91);
  homeIconObj = makeIcon(page, 14, 82, 30, iconBuf[0], WI_CLOUD);
  labels[2] = label(page, 50, 78, 178, CITY_A_NAME, &cyd_ru14);
  labels[3] = label(page, 50, 96, 178, "--°C", &cyd_ru14, MINT);
  labels[4] = label(page, 50, 113, 178, "Ожидание погоды", &cyd_ru12, MUTED);
  lv_label_set_long_mode(labels[4], LV_LABEL_LONG_SCROLL_CIRCULAR);
  labels[6] = label(page, 14, 131, 215, "Ощ. --  Ветер --", &cyd_ru12);
  labels[7] = label(page, 14, 148, 215, "Влажн. --  Давл. --", &cyd_ru12);
  calTitle = label(page, 8, 171, 224, "", &cyd_ru14, ACCENT);
  for (int i = 0; i < 7; ++i) {
    lv_obj_t *head = label(page, 8 + i * 32, 189, 30, dayNames[i], &cyd_ru12, i >= 5 ? WEEKEND : MUTED);
    lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);
  }
  for (int i = 0; i < 42; ++i) {
    calCells[i] = button(page, 8 + (i % 7) * 32, 203 + (i / 7) * 14, 30, 12, "", 0, 0x101a2a);
  }
}

void buildWeather() {
  static const char *cityNames[4] = {CITY_A_NAME, CITY_B_NAME, CITY_C_NAME, CITY_D_NAME};
  for (int i = 0; i < 4; ++i) {
    const int y = 5 + i * 55;
    box(page, 6, y, 228, 52);
    labels[i * 4] = label(page, 12, y + 2, 110, cityNames[i], &cyd_ru14, ACCENT);
    labels[i * 4 + 1] = label(page, 126, y + 2, 100, "--:--", &cyd_ru14);
    labels[i * 4 + 2] = label(page, 12, y + 22, 78, "--°C", &cyd_ru18, MINT);
    wxIconObjs[i] = makeIcon(page, 192, y + 10, 30, iconBuf[1 + i], WI_CLOUD);
    labels[i * 4 + 3] = label(page, 94, y + 26, 92, "Ожидание", &cyd_ru12, MUTED);
  }
  button(page, 6, 226, 228, 20, "Обновить", REFRESH_DATA);
}

void buildAlarm() {
  label(page, 6, 2, 120, "Будильник", &cyd_ru14, ACCENT);
  alarmToggle = button(page, 160, 0, 74, 26, "Вкл", ALARM_ENABLE);
  button(page, 6, 32, 30, 30, "-", HOUR_MINUS);
  labels[0] = label(page, 40, 34, 36, "07", &cyd_ru18);
  lv_obj_set_style_text_align(labels[0], LV_TEXT_ALIGN_CENTER, 0);
  button(page, 80, 32, 30, 30, "+", HOUR_PLUS);
  label(page, 114, 32, 12, ":", &cyd_ru18, ACCENT);
  button(page, 130, 32, 30, 30, "-", MINUTE_MINUS);
  labels[1] = label(page, 164, 34, 36, "30", &cyd_ru18);
  lv_obj_set_style_text_align(labels[1], LV_TEXT_ALIGN_CENTER, 0);
  button(page, 204, 32, 30, 30, "+", MINUTE_PLUS);
  for (int i = 0; i < 7; ++i) {
    daysButtons[i] = button(page, 8 + i * 32, 68, 30, 26, dayNames[i], DAY_BASE + i);
  }
  refreshDays();
  label(page, 8, 100, 90, "Таймер", &cyd_ru14, MINT);
  labels[2] = label(page, 8, 120, 100, "10:00", &cyd_ru18);
  button(page, 8, 142, 52, 26, "1м", TIMER_1);
  button(page, 64, 142, 52, 26, "5м", TIMER_5);
  button(page, 120, 142, 52, 26, "10м", TIMER_10);
  button(page, 176, 142, 52, 26, "15м", TIMER_15);
  timerToggle = button(page, 8, 172, 108, 30, "Старт", TIMER_START, 0x27695f);
  button(page, 120, 172, 108, 30, "Сброс", TIMER_RESET);
}

void buildRadio() {
  copyAppData(appData);
  modeWebBtn = button(page, 6, 0, 112, 24, "Радио", RADIO_MODE_WEB);
  modeSdBtn = button(page, 122, 0, 112, 24, "SD", RADIO_MODE_SD);
  labels[1] = label(page, 8, 28, 224, "Ожидание станции", &cyd_ru14);
  labels[2] = label(page, 8, 47, 224, "", &cyd_ru12, MUTED);
  labels[3] = label(page, 8, 63, 224, "Нет связи", &cyd_ru14, MINT);
  button(page, 6, 82, 52, 28, "<<", RADIO_PREVIOUS);
  button(page, 62, 82, 110, 28, "Пуск/стоп", RADIO_TOGGLE, 0x8b6c18);
  button(page, 176, 82, 52, 28, ">>", RADIO_NEXT);
  button(page, 6, 114, 70, 28, "-", RADIO_VOLUME_DOWN);
  button(page, 80, 114, 70, 28, "+", RADIO_VOLUME_UP);
  button(page, 154, 114, 80, 28, "Список", PLAYLIST_OPEN);
  labels[4] = label(page, 8, 146, 224, "Станции", &cyd_ru12, MUTED);
  radioListObj = lv_obj_create(page);
  lv_obj_set_pos(radioListObj, 6, 162);
  lv_obj_set_size(radioListObj, 228, 80);
  lv_obj_set_style_bg_color(radioListObj, color(0x101a2a), 0);
  lv_obj_set_style_bg_opa(radioListObj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(radioListObj, 0, 0);
  lv_obj_set_style_radius(radioListObj, 6, 0);
  lv_obj_set_style_pad_all(radioListObj, 0, 0);
  lv_obj_set_scrollbar_mode(radioListObj, LV_SCROLLBAR_MODE_AUTO);
  lastRadioListVersion = appData.radio.listVersion;
  refreshModeButtons();
  buildRadioList();
}

void buildRates() {
  const char *names[4] = {"USD/RUB", "EUR/RUB", "CNY/RUB", "GBP/RUB"};
  if (!rateGraphMode) {
    label(page, 8, 4, 140, "Курсы MOEX", &cyd_ru18, ACCENT);
    button(page, 150, 2, 84, 26, "Обновить", REFRESH_DATA);
    labels[0] = label(page, 8, 34, 224, "Ожидание данных", &cyd_ru14, MUTED);
    for (int i=0;i<4;++i) {
      lv_obj_t *row=button(page, 8, 60+i*38, 224, 34, "", RATE_ITEM_BASE+i, 0x1d2c46);
      labels[i+1]=label(row, 10, 7, 204, names[i], &cyd_ru18);
    }
    return;
  }
  label(page, 8, 2, 120, names[rateGraphCurrency], &cyd_ru18, ACCENT);
  button(page, 158, 2, 74, 25, "Назад", RATE_GRAPH_BACK);
  button(page, 8, 32, 105, 25, "6 месяцев", RATE_6M, rateGraphMonths==6?0x745b16:PANEL);
  button(page, 120, 32, 105, 25, "1 год", RATE_1Y, rateGraphMonths==12?0x745b16:PANEL);
  labels[0]=label(page, 8, 61, 224, "Загрузка ЦБ...", &cyd_ru12, MUTED);
  labels[1]=label(page, 8, 78, 224, "", &cyd_ru12, MINT);
  labels[2]=label(page, 8, 191, 224, "", &cyd_ru12, MINT);
  labels[3]=label(page, 8, 209, 224, "", &cyd_ru12, MUTED);
  rateLine=lv_line_create(page);
  lv_obj_set_pos(rateLine, 18, 96); lv_obj_set_size(rateLine, 204, 88);
  lv_obj_set_style_line_width(rateLine, 2, 0); lv_obj_set_style_line_color(rateLine, color(MINT), 0);
  rateGraphVersion=UINT32_MAX;
}

void refreshRateGraph() {
  copyRateHistory(rateHistory);
  if (rateHistory.currency!=rateGraphCurrency || rateHistory.months!=rateGraphMonths || rateHistory.version==rateGraphVersion) return;
  rateGraphVersion=rateHistory.version;
  lv_label_set_text(labels[0],rateHistory.message);
  if(!rateHistory.valid) { lv_line_set_points(rateLine,ratePoints,0); lv_label_set_text(labels[1],""); lv_label_set_text(labels[2],""); lv_label_set_text(labels[3],""); return; }
  float low=1e9f, high=0;
  for(float x:rateHistory.values) if(x>0) { if(x<low)low=x; if(x>high)high=x; }
  int n=0; float span=high-low; if(span<0.00001f) span=1;
  for(int i=0;i<RateHistory::kPoints;++i) if(rateHistory.values[i]>0) {
    ratePoints[n].x=i*203/(RateHistory::kPoints-1);
    ratePoints[n].y=87-(int)((rateHistory.values[i]-low)*87/span); ++n;
  }
  lv_line_set_points(rateLine,ratePoints,n);
  char text[40];
  snprintf(text,sizeof(text),"Макс: %.4f",high); lv_label_set_text(labels[1],text);
  snprintf(text,sizeof(text),"Мин: %.4f",low); lv_label_set_text(labels[2],text);
  lv_label_set_text_fmt(labels[3],"%s - %s",rateHistory.from,rateHistory.to);
}

// Touch and SD share the VSPI host with different pins, so the bus is
// switched to the SD pins for the listing and restored afterwards.
// All other SPI devices are explicitly deselected first, otherwise a held
// MISO line silently breaks SD mounting on many CYD boards.
void sdParkBus() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
#ifdef TFT_CS
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
#endif
}

// Case-insensitive ASCII order for the file manager (FAT itself is unordered,
// so without sorting the first files look "missing").
bool sdNameLess(const char *a, const char *b) {
  while (*a && *b) {
    char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
    if (ca != cb) return (uint8_t)ca < (uint8_t)cb;
    ++a;
    ++b;
  }
  return *a == '\0' && *b != '\0';
}

// All SD operations, including HTTP, run on the UI task. Stop VSPI before
// reassigning its pins; SPIClass::begin does nothing if it is already started.
bool sdMount() {
  sdParkBus();
  touchBus.end();
  touchBus.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  const uint32_t speeds[] = {4000000, 2000000, 1000000};
  for (uint32_t speed : speeds) {
    SD.end(); sdParkBus();
    if (SD.begin(SD_CS, touchBus, speed)) return true;
    delay(30);
  }
  SD.end(); touchBus.end(); sdParkBus();
  touchBus.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  return false;
}
void sdUnmount() {
  SD.end(); touchBus.end(); sdParkBus();
  touchBus.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
}

void readSdFiles() {
  sdFileCount = 0; sdHasNext = false;
  if (!sdMount()) { strlcpy(sdStatus, "SD: ошибка карты/FAT32", sizeof(sdStatus)); return; }
  File root = SD.open(sdPath.c_str());
  if (!root || !root.isDirectory()) {
    strlcpy(sdStatus, "Ошибка папки SD", sizeof(sdStatus));
    root.close(); sdUnmount(); return;
  }
  uint32_t seen = 0;
  File entry = root.openNextFile();
  while (entry) {
    const char *name = entry.name();
    const char *slash = strrchr(name, '/'); if (slash) name = slash + 1;
    if (name[0] && name[0] != '.') {
      if (seen++ >= sdOffset) {
        if (sdFileCount == kSdMax) { sdHasNext = true; entry.close(); break; }
        // Do not navigate with a truncated filename.
        strlcpy(sdNames[sdFileCount], name, sizeof(sdNames[0]));
        sdDirs[sdFileCount] = entry.isDirectory() && strlen(name) < sizeof(sdNames[0]);
        if (entry.isDirectory()) strlcpy(sdInfo[sdFileCount], "<папка>", sizeof(sdInfo[0]));
        else snprintf(sdInfo[sdFileCount], sizeof(sdInfo[0]), "%lu КБ", (unsigned long)((entry.size()+1023)/1024));
        ++sdFileCount;
      }
    }
    entry.close(); entry = root.openNextFile();
  }
  root.close(); sdUnmount();
  snprintf(sdStatus, sizeof(sdStatus), "SD: %lu-%lu", (unsigned long)(sdOffset+1), (unsigned long)(sdOffset+sdFileCount));
}

// Read-only HTTP browser. The same task owns the bus throughout each request.
bool safeSdPath(const String &path) {
  if (!path.startsWith("/") || path.length() > 240 || path.indexOf('\\') >= 0 || path.indexOf("//") >= 0) return false;
  int start = 1;
  while (start <= (int)path.length()) {
    int end = path.indexOf('/', start); if (end < 0) end = path.length();
    String part = path.substring(start, end);
    if (part == "." || part == "..") return false;
    for (unsigned i=0; i<part.length(); ++i) if ((unsigned char)part[i] < 32) return false;
    start = end + 1;
  }
  return true;
}
String htmlEscape(const String &in) {
  String out;
  for (unsigned i=0; i<in.length(); ++i) {
    char c=in[i]; if(c=='&') out += "&amp;"; else if(c=='<') out += "&lt;";
    else if(c=='>') out += "&gt;"; else if(c=='\"') out += "&quot;"; else if(c=='\'') out += "&#39;"; else out += c;
  }
  return out;
}
String urlEncode(const String &in) {
  const char *hex="0123456789ABCDEF"; String out;
  for (unsigned i=0;i<in.length();++i) { unsigned char c=in[i];
    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out += (char)c;
    else { out += '%'; out += hex[c>>4]; out += hex[c&15]; }
  } return out;
}
#include "sd_admin_impl.h"
#include "web_ota_impl.h"

void handleSdWeb() {
  if (sdAdminConfigured() && !sdAuthorize()) return;
  sdWeb.sendHeader("Cache-Control", "no-store");
  sdWeb.sendHeader("Referrer-Policy", "no-referrer");
  String path = sdWeb.hasArg("path") ? sdWeb.arg("path") : "/";
  if (!safeSdPath(path)) { sdWeb.send(400,"text/plain; charset=utf-8","Invalid path"); return; }
  if (!sdMount()) { sdWeb.send(503,"text/html; charset=utf-8","<meta charset=utf-8>SD карта недоступна. <a href='/update'>Обновить прошивку</a>"); return; }
  File file = SD.open(path.c_str(), FILE_READ);
  if (!file) { sdUnmount(); sdWeb.send(404,"text/plain","Not found"); return; }
  sdWeb.sendHeader("X-Content-Type-Options", "nosniff");
  if (!file.isDirectory()) {
    String name=path.substring(path.lastIndexOf('/')+1);
    sdWeb.sendHeader("Content-Disposition", "attachment; filename=\"download\"; filename*=UTF-8''"+urlEncode(name));
    sdWeb.streamFile(file, "application/octet-stream"); file.close(); sdUnmount(); return;
  }
  sdWeb.setContentLength(CONTENT_LENGTH_UNKNOWN);
  sdWeb.send(200,"text/html; charset=utf-8", "<!doctype html><html lang=ru><meta charset=utf-8><meta name=viewport content='width=device-width'><title>SD карта</title><style>body{font:16px sans-serif;max-width:800px;margin:2em auto;padding:0 1em;background:#101827;color:#eee}a{color:#67d8c1}li{padding:.5em;overflow-wrap:anywhere}form.inline{display:inline}button{padding:.4em;margin-left:1em} .upload{margin:1em 0}</style><h1>SD карта</h1><p><a href='/update'>Обновить прошивку</a></p><p>"+htmlEscape(path)+"</p>"+(sdAdminConfigured()?"":"<p>Запись отключена: настройте SD_WEB_PASSWORD.</p>")+"<ul>");
  if (path != "/") { int pos=path.lastIndexOf('/'); String parent=pos<=0?"/":path.substring(0,pos); sdWeb.sendContent("<li><a href='/?path="+urlEncode(parent)+"'>..</a></li>"); }
  File entry=file.openNextFile();
  while(entry) {
    String name=entry.name(); int slash=name.lastIndexOf('/'); if(slash>=0) name=name.substring(slash+1);
    if(name.length() && name[0]!='.') {
      String child=path=="/"?"/"+name:path+"/"+name;
      String line="<li><a href='/?path="+urlEncode(child)+"'>"+htmlEscape(name)+(entry.isDirectory()?"/":"")+"</a>";
      if(!entry.isDirectory()) {
        line += " ("+String((unsigned long)entry.size())+" байт)";
        if(sdAdminConfigured()) line += " <form class='inline' method='post' action='/delete?path="+urlEncode(child)+"&amp;token="+sdCsrf+"' onsubmit=\"return confirm('Удалить файл?')\"><button type='submit'>Удалить</button></form>";
      }
      sdWeb.sendContent(line+"</li>");
    }
    entry.close(); entry=file.openNextFile(); yield();
  }
  file.close(); sdWeb.sendContent("</ul>");
  if(sdAdminConfigured()) sdWeb.sendContent("<form class='upload' method='post' enctype='multipart/form-data' action='/upload?path="+urlEncode(path)+"&amp;token="+sdCsrf+"'><label>Загрузить файл: <input type='file' name='file' required></label><button type='submit'>Загрузить</button></form><p>Существующие файлы не перезаписываются.</p>");
  sdWeb.sendContent("</html>"); sdWeb.sendContent(""); sdUnmount();
}

#include "dlna_impl.h"

void buildSettings() {
  copyAppData(appData);
  lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  label(page, 8, 2, 200, "Настройки", &cyd_ru14, ACCENT);
  label(page, 8, 22, 70, "Яркость", &cyd_ru14);
  labels[0] = label(page, 82, 22, 40, "85%", &cyd_ru14, MINT);
  button(page, 126, 20, 48, 26, "-", SET_BRIGHT_DOWN);
  button(page, 178, 20, 48, 26, "+", SET_BRIGHT_UP);
  label(page, 8, 52, 150, "Звук CYD", &cyd_ru14);
  speakerBtn = button(page, 162, 50, 66, 26, "Вкл", SET_SPEAKER);
  labels[1] = label(page, 8, 80, 224, "IP: ---", &cyd_ru12, MUTED);
  labels[2] = label(page, 8, 96, 224, "Wi-Fi: нет", &cyd_ru12, MUTED);
  button(page, 8, 114, 108, 28, "Обновить", REFRESH_DATA);
  button(page, 120, 114, 108, 28, "Сброс калибр.", SET_RESET_CAL);
  labels[3] = label(page, 8, 148, 140, "Файлы SD", &cyd_ru12, ACCENT);
  button(page, 162, 144, 66, 22, "Читать", SD_REFRESH);
  label(page, 8, 170, 224, sdPath.c_str(), &cyd_ru12, MUTED);
  button(page, 8, 188, 64, 24, "Вверх", SD_PARENT);
  button(page, 80, 188, 64, 24, "<", SD_PREV);
  button(page, 152, 188, 64, 24, ">", SD_NEXT);
  int y = 216;
  for (uint8_t i = 0; i < sdFileCount; ++i) {
    lv_obj_t *row = sdDirs[i] ? button(page, 8, y, 224, 26, "", static_cast<Action>(SD_ITEM_BASE + i), 0x1d2c46) : box(page, 8, y, 224, 26, 0x1d2c46);
    lv_obj_t *nameLbl = label(row, 8, 4, 140, sdNames[i], &cyd_ru12);
    lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    label(row, 152, 4, 64, sdInfo[i], &cyd_ru12, MINT);
    y += 30;
  }
  if (sdFileCount == 0) {
    label(page, 8, 216, 224, sdStatus, &cyd_ru12, MUTED);
  }
}

void showScreen(Screen screen) {
  if (screen != RATES) rateGraphMode=false;
  activeScreen = screen;
  rateLine=nullptr;
  closePlaylist();
  for (auto &item : labels) item = nullptr;
  for (auto &item : daysButtons) item = nullptr;
  for (auto &item : calCells) item = nullptr;
  calTitle = nullptr;
  calShownKey = -1;
  alarmToggle = nullptr;
  timerToggle = nullptr;
  modeWebBtn = nullptr;
  modeSdBtn = nullptr;
  radioListObj = nullptr;
  speakerBtn = nullptr;
  homeIconObj = nullptr;
  for (auto &item : wxIconObjs) item = nullptr;
  lv_obj_clean(page);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
  lv_label_set_text(headerTitle, screenNames[screen]);
  for (int i = 0; i < 7; ++i) {
    lv_obj_set_style_bg_color(navButtons[i], color(i == screen ? 0x745b16 : 0x101a2a), 0);
  }
  switch (screen) {
    case HOME: buildHome(); break;
    case WEATHER: buildWeather(); break;
    case ALARM: buildAlarm(); break;
    case RADIO: buildRadio(); break;
    case RATES: buildRates(); break;
    case SETTINGS:
      readSdFiles();
      buildSettings();
      break;
    case TV: buildTv(); requestTvStatus(); break;
  }
  refreshScreen();
}

void refreshScreen() {
  copyAppData(appData);
  const time_t utc = time(nullptr);
  const bool timeValid = utc > 1700000000;
  const tm localA = cityTime(utc, appData.weather[0].utcOffset);
  lv_label_set_text(headerStatus, WiFi.status() == WL_CONNECTED ? "Wi-Fi OK" : "Wi-Fi нет");

  if (activeScreen == HOME) {
    const WeatherData &city = appData.weather[0];
    if (timeValid) {
      lv_label_set_text_fmt(labels[0], "%02d:%02d", localA.tm_hour, localA.tm_min);
      lv_label_set_text_fmt(labels[5], "%02d", localA.tm_sec);
      const unsigned mondayFirst = static_cast<unsigned>((localA.tm_wday + 6) % 7);
      lv_label_set_text_fmt(labels[1], "%02d.%02d.%04d %s", localA.tm_mday, localA.tm_mon + 1,
                            localA.tm_year + 1900, dayFullNames[mondayFirst]);
      const int key = (localA.tm_year + 1900) * 1000 + localA.tm_yday;
      if (key != calShownKey) refillCalendar(localA);
    }
    if (city.valid) {
      char temp[48], feels[16], wind[16], humidity[16], pressure[16];
      snprintf(temp, sizeof(temp), "%.1f°C", city.temperature); lv_label_set_text(labels[3], temp);
      lv_label_set_text(labels[4], city.description[0] ? city.description : "Нет описания");
      if(isfinite(city.feelsLike)) snprintf(feels,sizeof(feels),"%.1f°C",city.feelsLike); else strlcpy(feels,"--",sizeof(feels));
      if(isfinite(city.windSpeed)) snprintf(wind,sizeof(wind),"%.1f м/с",city.windSpeed); else strlcpy(wind,"--",sizeof(wind));
      if(city.humidity>=0) snprintf(humidity,sizeof(humidity),"%d%%",city.humidity); else strlcpy(humidity,"--",sizeof(humidity));
      if(city.pressure>=0) snprintf(pressure,sizeof(pressure),"%d гПа",city.pressure); else strlcpy(pressure,"--",sizeof(pressure));
      snprintf(temp,sizeof(temp),"Ощ. %s  Ветер %s",feels,wind); lv_label_set_text(labels[6],temp);
      snprintf(temp,sizeof(temp),"Влажн. %s  Давл. %s",humidity,pressure); lv_label_set_text(labels[7],temp);
    } else {
      lv_label_set_text(labels[3], "--°C"); lv_label_set_text(labels[4], city.error);
      lv_label_set_text(labels[6], "Ощ. --  Ветер --"); lv_label_set_text(labels[7], "Влажн. --  Давл. --");
    }
    if (homeIconObj) paintIcon(homeIconObj, city.valid ? weatherIconKind(city.icon) : WI_CLOUD, 30);
  } else if (activeScreen == WEATHER) {
    for (int i = 0; i < 4; ++i) {
      const tm local = cityTime(utc, appData.weather[i].utcOffset);
      const WeatherData &weather = appData.weather[i];
      if (timeValid) lv_label_set_text_fmt(labels[i * 4 + 1], "%02d:%02d", local.tm_hour, local.tm_min);
      if (weather.valid) {
        char temp[32];
        snprintf(temp, sizeof(temp), "%.1f°C", weather.temperature);
        lv_label_set_text(labels[i * 4 + 2], temp);
        lv_label_set_text(labels[i * 4 + 3], weather.description);
      } else {
        lv_label_set_text(labels[i * 4 + 2], "--°C");
        lv_label_set_text(labels[i * 4 + 3], weather.error);
      }
      if (wxIconObjs[i]) {
        paintIcon(wxIconObjs[i], weather.valid ? weatherIconKind(weather.icon) : WI_CLOUD, 30);
      }
    }
  } else if (activeScreen == ALARM) {
    lv_label_set_text_fmt(labels[0], "%02u", alarmHour);
    lv_label_set_text_fmt(labels[1], "%02u", alarmMinute);
    lv_label_set_text_fmt(labels[2], "%02lu:%02lu", static_cast<unsigned long>(timerRemaining / 60), static_cast<unsigned long>(timerRemaining % 60));
    buttonText(alarmToggle, alarmEnabled ? "Включен" : "Выключен");
    buttonText(timerToggle, timerRunning ? "Пауза" : "Старт");
  } else if (activeScreen == RADIO) {
    const RadioData &radio = appData.radio;
    refreshModeButtons();
    lv_label_set_text(labels[1], radio.online ? radio.stationName : "yoRadio недоступно");
    lv_label_set_text(labels[2], radio.title);
    char vol[64];
    if (radio.online) {
      snprintf(vol, sizeof(vol), "На связи - %d/100 - %s", radio.volume, radio.playing ? "Играет" : "Стоп");
    } else {
      snprintf(vol, sizeof(vol), "Нет связи с yoRadio");
    }
    lv_label_set_text(labels[3], vol);
    char header[48];
    if (radio.mode == 1) snprintf(header, sizeof(header), "Файлы SD (%u)", static_cast<unsigned>(radio.sdCount));
    else snprintf(header, sizeof(header), "Станции (%u)", static_cast<unsigned>(radio.stationCount));
    lv_label_set_text(labels[4], header);
    if (radio.listVersion != lastRadioListVersion) {
      lastRadioListVersion = radio.listVersion;
      buildRadioList();
    }
    if (plOverlay) {
      if (radio.listVersion != plSeenVersion || radio.mode != plSeenMode || radio.station != plSeenStation) {
        fillPlaylist();
      }
    }
  } else if (activeScreen == RATES && rateGraphMode) {
    refreshRateGraph();
  } else if (activeScreen == RATES) {
    lv_label_set_text(labels[0], appData.ratesMessage);
    const char *names[4] = {"USD/RUB", "EUR/RUB", "CNY/RUB", "GBP/RUB"};
    for (int i = 0; i < 4; ++i) {
      char rate[64];
      if (appData.ratesValid && appData.rates[i] > 0) snprintf(rate, sizeof(rate), "%s    %.2f", names[i], appData.rates[i]);
      else snprintf(rate, sizeof(rate), "%s    --", names[i]);
      lv_label_set_text(labels[i + 1], rate);
    }
  } else if (activeScreen == SETTINGS) {
    lv_label_set_text_fmt(labels[0], "%d%%", brightPct);
    buttonText(speakerBtn, spkOn ? "Вкл" : "Выкл");
    if (appData.wifiIp[0]) {
      lv_label_set_text_fmt(labels[1], "IP: %s", appData.wifiIp);
      lv_label_set_text_fmt(labels[2], "Wi-Fi: %d dBm", appData.wifiRssi);
    } else {
      lv_label_set_text(labels[1], "IP: ---");
      lv_label_set_text(labels[2], "Wi-Fi: нет");
    }
    lv_label_set_text(labels[3], sdStatus);
  } else if (activeScreen == TV) {
    refreshTv();
  }
}

void setupUi() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, color(BACKGROUND), 0);
  lv_obj_set_style_text_font(screen, &cyd_ru14, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  box(screen, 0, 0, 240, 24, 0x101a2a);
  headerTitle = label(screen, 8, 3, 150, "Главная", &cyd_ru14, ACCENT);
  headerStatus = label(screen, 160, 3, 74, "Wi-Fi нет", &cyd_ru12, MUTED);
  page = box(screen, 0, PAGE_Y, SCREEN_WIDTH, PAGE_H, BACKGROUND);
  for (int i = 0; i < 7; ++i) {
    navButtons[i] = lv_btn_create(screen);
    if (i < 4) {
      lv_obj_set_pos(navButtons[i], i * 60, 272);
      lv_obj_set_size(navButtons[i], 59, 23);
    } else {
      lv_obj_set_pos(navButtons[i], (i - 4) * 80, 296);
      lv_obj_set_size(navButtons[i], 79, 23);
    }
    lv_obj_set_style_radius(navButtons[i], 0, 0);
    lv_obj_set_style_shadow_width(navButtons[i], 0, 0);
    lv_obj_set_style_pad_all(navButtons[i], 0, 0);
    lv_obj_add_event_cb(navButtons[i], onNavigate, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    lv_obj_t *caption = lv_label_create(navButtons[i]);
    lv_obj_set_style_text_font(caption, &cyd_ru12, 0);
    lv_obj_set_style_text_color(caption, color(TEXT), 0);
    lv_label_set_text(caption, screenNames[i]);
    lv_obj_center(caption);
  }
  showScreen(HOME);
}

void checkAlarmAndTimer() {
  const uint32_t now = millis();
  if (timerRunning) {
    const int32_t left = static_cast<int32_t>(timerDeadline - now);
    if (left <= 0) {
      timerRunning = false;
      timerRemaining = 0;
      startAlert(false);
    } else {
      timerRemaining = static_cast<uint32_t>((left + 999) / 1000);
    }
  }

  const time_t utc = time(nullptr);
  if (utc > 1700000000) {
    const tm local = cityTime(utc, appData.weather[0].utcOffset);
    if (alarmMatches(local, alarmEnabled, alarmHour, alarmMinute, alarmDays) &&
        (lastAlarmYear != local.tm_year || lastAlarmDay != local.tm_yday)) {
      lastAlarmYear = local.tm_year;
      lastAlarmDay = local.tm_yday;
      startAlert(true);
    }
  }
  if (ringing) {
    if (now - ringStarted >= 60000UL) {
      dismissAlert();
    } else if (spkOn) {
      ledcWriteTone(SPEAKER_CHANNEL, ((now - ringStarted) / 350) % 2 ? 0 : 1900);
    } else {
      ledcWriteTone(SPEAKER_CHANNEL, 0);
    }
  }
}
}  // namespace

void setup() {
  // Drive BL off as early as firmware can; reset/bootloader behaviour is hardware-dependent.
  digitalWrite(BACKLIGHT_PIN, LOW);
  pinMode(BACKLIGHT_PIN, OUTPUT);
  Serial.begin(115200);
  setupAlarmLed();
  pinMode(0, INPUT_PULLUP);
  preferences.begin("multi-cyd", false);
  alarmEnabled = preferences.getBool("alarmOn", DEFAULT_ALARM_ENABLED);
  alarmHour = preferences.getUChar("alarmHour", DEFAULT_ALARM_HOUR) % 24;
  alarmMinute = preferences.getUChar("alarmMin", DEFAULT_ALARM_MINUTE) % 60;
  alarmDays = preferences.getUChar("alarmDays", DEFAULT_ALARM_DAYS) & 0x7f;
  brightPct = constrain(preferences.getUChar("bright", DISPLAY_BRIGHTNESS), 20, 100);
  spkOn = preferences.getBool("spk", ALARM_SPEAKER_ENABLED != 0);

  ledcSetup(BACKLIGHT_CHANNEL, 5000, 8);
  ledcWrite(BACKLIGHT_CHANNEL, 0);
  ledcAttachPin(BACKLIGHT_PIN, BACKLIGHT_CHANNEL);
  ledcSetup(SPEAKER_CHANNEL, 1900, 8);
  ledcAttachPin(SPEAKER_PIN, SPEAKER_CHANNEL);
  ledcWriteTone(SPEAKER_CHANNEL, 0);
  display.init();
  display.setRotation(0);
  display.fillScreen(TFT_BLACK);
  touchBus.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchBus);
  touch.setRotation(0);
  setupCalibration();
  ledcWrite(BACKLIGHT_CHANNEL, 0);

  lv_init();
  pixels = static_cast<lv_color_t *>(malloc(sizeof(lv_color_t) * SCREEN_WIDTH * 16));
  if (!pixels) {
    Serial.println("Cannot allocate display buffer");
    for (;;) delay(1000);
  }
  lv_disp_draw_buf_init(&drawBuffer, pixels, nullptr, SCREEN_WIDTH * 16);
  static lv_disp_drv_t displayDriver;
  lv_disp_drv_init(&displayDriver);
  displayDriver.hor_res = SCREEN_WIDTH;
  displayDriver.ver_res = SCREEN_HEIGHT;
  displayDriver.flush_cb = flushDisplay;
  displayDriver.draw_buf = &drawBuffer;
  lv_disp_drv_register(&displayDriver);
  static lv_indev_drv_t touchDriver;
  lv_indev_drv_init(&touchDriver);
  touchDriver.type = LV_INDEV_TYPE_POINTER;
  touchDriver.read_cb = readTouch;
  lv_indev_drv_register(&touchDriver);
  startNetworkServices();
  setupUi();
  // Render the entire initial UI while the backlight is off.
  lv_refr_now(nullptr);
  if (millis() < 750) delay(750 - millis());
  applyBrightness();
  sdWeb.on("/", HTTP_GET, handleSdWeb);
  sdWeb.onNotFound([](){ sdWeb.send(404, "text/plain", "Not found"); });
  setupDlna();
  setupSdAdmin();
  setupWebOta();
  sdWeb.begin();
}

void loop() {
  static uint32_t lastTick = millis();
  static uint32_t lastRefresh = 0;
  const uint32_t now = millis();
  lv_tick_inc(now - lastTick);
  lastTick = now;
  checkAlarmAndTimer();
  if (now - lastRefresh >= 1000UL) {
    refreshScreen();
    lastRefresh = now;
  }
  lv_timer_handler();
  sdWeb.handleClient();
  dlnaLoop();
  delay(5);
}
