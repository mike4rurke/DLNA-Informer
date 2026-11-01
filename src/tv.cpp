// webOS 1.x SSAP (LG LB65): approval on the TV, persistent client key.
#include "services.h"
#include "config.h"
#include "lg_tv.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
namespace {
Preferences prefs; SemaphoreHandle_t mutex=nullptr; QueueHandle_t queue=nullptr;
TvData state; WebSocketsClient socket, input; bool started=false, ready=false, inputReady=false;
String clientKey; struct Command { int kind; int code; }; enum { KEY, PAIR, POLL, CHANNEL };
void message(const char *s) { if(mutex && xSemaphoreTake(mutex,pdMS_TO_TICKS(100))) { strlcpy(state.message,s,sizeof(state.message)); xSemaphoreGive(mutex); } }
void online(bool ok) { if(mutex && xSemaphoreTake(mutex,pdMS_TO_TICKS(100))) { state.sessionOk=ok; xSemaphoreGive(mutex); } }
void request(const char *id,const char *uri) { String data=String("{\"id\":\"")+id+"\",\"type\":\"request\",\"uri\":\"ssap://"+uri+"\",\"payload\":{}}"; socket.sendTXT(data); }
void launchInputPicker(bool alternate) {
  const char *app=alternate ? "com.webos.app.inputmgr" : "com.webos.app.inputpicker";
  String data=String("{\"id\":\"")+(alternate?"source-alt":"source")+"\",\"type\":\"request\",\"uri\":\"ssap://system.launcher/launch\",\"payload\":{\"id\":\""+app+"\"}}";
  socket.sendTXT(data);
  message("Открываем источники");
}
void subscribeChannel() { socket.sendTXT("{\"id\":\"channel-sub\",\"type\":\"subscribe\",\"uri\":\"ssap://tv/getCurrentChannel\",\"payload\":{}}"); }
void registerTv() {
 StaticJsonDocument<2048> doc; doc["id"]="register_0"; doc["type"]="register";
 JsonObject p=doc.createNestedObject("payload"); p["pairingType"]="PROMPT"; p["forcePairing"]=false; if(clientKey.length()) p["client-key"]=clientKey;
 JsonObject m=p.createNestedObject("manifest"); m["manifestVersion"]=1; m["appVersion"]="1.1";
 JsonArray a=m.createNestedArray("permissions"); const char *permissions[]={"CONTROL_AUDIO","CONTROL_POWER","CONTROL_INPUT_TV","CONTROL_MOUSE_AND_KEYBOARD","CONTROL_INPUT_TEXT","READ_CURRENT_CHANNEL","READ_INPUT_DEVICE_LIST","READ_TV_CHANNEL_LIST","LAUNCH","READ_POWER_STATE"}; for(auto x:permissions) a.add(x);
 String data; serializeJson(doc,data); socket.sendTXT(data); message(clientKey.length()?"Подключение к ТВ":"Подтвердите на ТВ");
}
void inputEvent(WStype_t t,uint8_t*,size_t) { if(t==WStype_CONNECTED) inputReady=true; else if(t==WStype_DISCONNECTED) inputReady=false; }
void event(WStype_t type,uint8_t *payload,size_t length) {
 if(type==WStype_CONNECTED) { registerTv(); return; }
 if(type==WStype_DISCONNECTED) { ready=false; inputReady=false; input.disconnect(); online(false); if(mutex && xSemaphoreTake(mutex,pdMS_TO_TICKS(100))) { state.channel[0]=0; xSemaphoreGive(mutex); } message("ТВ: нет соединения"); return; }
 if(type!=WStype_TEXT) return;
 DynamicJsonDocument doc(4096); if(deserializeJson(doc,payload,length)) { message("ТВ: ошибка JSON"); return; }
 const char *kind=doc["type"] | "", *id=doc["id"] | "";
 if(!strcmp(kind,"registered")) { const char *key=doc["payload"]["client-key"] | ""; if(key[0]) { clientKey=key; prefs.putString("key",clientKey); } ready=true; online(true); message("ТВ подключён"); request("pointer","com.webos.service.networkinput/getPointerInputSocket"); request("volume","audio/getVolume"); subscribeChannel(); request("channel","tv/getCurrentChannel"); return; }
 if(!strcmp(id,"preset")) {
   bool failed=!strcmp(kind,"error") || (doc["payload"]["returnValue"].is<bool>() && !doc["payload"]["returnValue"].as<bool>());
   if(failed) message("ТВ: канал недоступен");
   else if(!strcmp(kind,"response")) { message("Канал переключен"); request("channel","tv/getCurrentChannel"); }
   return;
 }
 if(!strcmp(id,"source") || !strcmp(id,"source-alt")) {
   const bool error=!strcmp(kind,"error") || (doc["payload"]["returnValue"].is<bool>() && !doc["payload"]["returnValue"].as<bool>());
   if(error) {
     Serial.printf("webOS input picker: %s\n", doc["error"] | (doc["payload"]["errorText"] | "request rejected"));
     if(!strcmp(id,"source")) launchInputPicker(true);
     else message("ТВ: источник недоступен");
   } else if(!strcmp(kind,"response")) message("Выберите источник на ТВ");
   return;
 }
 if(!strcmp(kind,"error")) { if(!strcmp(id,"channel") || !strcmp(id,"channel-sub")) { if(mutex && xSemaphoreTake(mutex,pdMS_TO_TICKS(100))) { state.channel[0]=0; xSemaphoreGive(mutex); } } else message("ТВ отклонил запрос"); return; }
 if(!strcmp(id,"pointer")) { const char *raw=doc["payload"]["socketPath"] | ""; String url(raw), prefix=String("ws://")+TV_HOST+":"; if(!url.startsWith(prefix)) { message("ТВ: ошибка адреса пульта"); return; } int slash=url.indexOf('/',prefix.length()); if(slash<0) return; int port=url.substring(prefix.length(),slash).toInt(); if(port<1 || port>65535) return; input.begin(TV_HOST,port,url.substring(slash)); input.onEvent(inputEvent); input.setReconnectInterval(3000); }
 if((!strcmp(id,"channel") || !strcmp(id,"channel-sub")) && mutex && xSemaphoreTake(mutex,pdMS_TO_TICKS(100))) {
   JsonVariant number=doc["payload"]["channelNumber"];
   bool failed=doc["payload"]["returnValue"].is<bool>() && !doc["payload"]["returnValue"].as<bool>();
   if (failed) state.channel[0]=0;
   else if (number.is<const char*>()) strlcpy(state.channel,number.as<const char*>(),sizeof(state.channel));
   else if (number.is<int>()) snprintf(state.channel,sizeof(state.channel),"%d",number.as<int>());
   xSemaphoreGive(mutex);
 }
 if(!strcmp(id,"volume") && mutex && xSemaphoreTake(mutex,pdMS_TO_TICKS(100))) { if(doc["payload"]["volume"].is<int>()) state.volume=doc["payload"]["volume"].as<int>(); if(doc["payload"]["mute"].is<bool>()) state.muted=doc["payload"]["mute"].as<bool>(); xSemaphoreGive(mutex); }
}
void press(int code) {
 if(!ready) { message("Сначала сопряжение"); return; } const char *uri=nullptr,*button=nullptr;
 switch(code) { case LG_POWER:uri="system/turnOff";break; case LG_VOLUME_UP:uri="audio/volumeUp";break; case LG_VOLUME_DOWN:uri="audio/volumeDown";break; case LG_CHANNEL_UP:uri="tv/channelUp";break; case LG_CHANNEL_DOWN:uri="tv/channelDown";break; case LG_UP:button="UP";break; case LG_DOWN:button="DOWN";break; case LG_LEFT:button="LEFT";break; case LG_RIGHT:button="RIGHT";break; case LG_OK:button="ENTER";break; case LG_HOME_MENU:button="HOME";break; case LG_BACK:button="BACK";break; case LG_EXIT:button="EXIT";break; case LG_EXTERNAL_INPUT:launchInputPicker(false);return; case LG_MUTE_TOGGLE:button="MUTE";break; }
 if(uri) request("key",uri); else if(button && inputReady) input.sendTXT(String("type:button\nname:")+button+"\n\n"); else message("Пульт ТВ не подключён");
}
void task(void*) {
 prefs.begin("webostv",false); if(prefs.getString("host","")!=TV_HOST) { prefs.clear(); prefs.putString("host",TV_HOST); } clientKey=prefs.getString("key","");
 if(mutex && xSemaphoreTake(mutex,pdMS_TO_TICKS(100))) { state.enabled=strlen(TV_HOST)>0; xSemaphoreGive(mutex); }
 if(strlen(TV_HOST)) { socket.begin(TV_HOST,3000,"/"); socket.onEvent(event); socket.setReconnectInterval(5000); message("Подключение к ТВ"); } else message("Укажите IP ТВ");
 uint32_t lastChannelPoll=0;
 for(;;) { socket.loop(); input.loop(); if(ready && millis()-lastChannelPoll>=5000) { request("channel","tv/getCurrentChannel"); lastChannelPoll=millis(); } Command c; while(xQueueReceive(queue,&c,0)==pdTRUE) { if(c.kind==KEY) press(c.code); else if(c.kind==CHANNEL) {
      if(!ready) message("Сначала сопряжение");
      else { char frame[160]; snprintf(frame,sizeof(frame),"{\"id\":\"preset\",\"type\":\"request\",\"uri\":\"ssap://tv/openChannel\",\"payload\":{\"channelNumber\":\"%d\"}}",c.code); socket.sendTXT(frame); message("Переключаем канал"); }
    } else if(c.kind==POLL && ready) { request("volume","audio/getVolume"); request("channel","tv/getCurrentChannel"); } else if(c.kind==PAIR) { clientKey=""; prefs.remove("key"); ready=false; online(false); input.disconnect(); socket.disconnect(); if(strlen(TV_HOST)) socket.begin(TV_HOST,3000,"/"); message("Подтвердите на ТВ"); } } vTaskDelay(pdMS_TO_TICKS(15)); }
}
void ensure() { if(started) return; started=true; mutex=xSemaphoreCreateMutex(); queue=xQueueCreate(16,sizeof(Command)); if(mutex && queue) xTaskCreatePinnedToCore(task,"webostv",8192,nullptr,1,nullptr,1); }
}
bool sendTvKey(int code) { ensure(); Command c{KEY,code}; return queue && xQueueSend(queue,&c,0)==pdTRUE; }
bool sendTvChannel(unsigned number) { ensure(); if(number==0 || number>9999 || !queue) return false; Command c{CHANNEL,(int)number}; return xQueueSend(queue,&c,0)==pdTRUE; }
void tvPairShowKey() { ensure(); Command c{PAIR,0}; if(queue) xQueueSend(queue,&c,0); }
void requestTvStatus() { ensure(); Command c{POLL,0}; if(queue) xQueueSend(queue,&c,0); }
void copyTvData(TvData &out) { if(mutex && xSemaphoreTake(mutex,pdMS_TO_TICKS(20))) { out=state; xSemaphoreGive(mutex); } }
