// Official CBR historical daily exchange rates; the overview uses MOEX.
#include "services.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
namespace {
SemaphoreHandle_t historyMutex = nullptr;
QueueHandle_t historyQueue = nullptr;
RateHistory sharedHistory;
uint32_t generation=0;
struct HistoryJob { uint8_t currency; uint8_t months; uint32_t generation; };
const char *ids[]={"R01235","R01239","R01375","R01035"};
void finish(const HistoryJob &job, RateHistory &result) {
  if (historyMutex && xSemaphoreTake(historyMutex,pdMS_TO_TICKS(200))) {
    if(job.generation==generation) { result.loading=false; result.version=++sharedHistory.version; sharedHistory=result; }
    xSemaphoreGive(historyMutex);
  }
}
void fetch(const HistoryJob &job) {
  RateHistory out; out.currency=job.currency; out.months=job.months;
  time_t now=time(nullptr); if(now<1700000000) { strlcpy(out.message,"Нет времени NTP",sizeof(out.message));finish(job,out);return; }
  // Midnight UTC, exact calendar-month start, clamped at the end of a month.
  struct tm end{}; gmtime_r(&now,&end); end.tm_hour=0;end.tm_min=0;end.tm_sec=0;
  time_t last=mktime(&end); struct tm begin=end; begin.tm_mon-=job.months; int original=begin.tm_mday; begin.tm_mday=1; mktime(&begin); int month=begin.tm_mon; begin.tm_mday=original; time_t first=mktime(&begin); if(begin.tm_mon!=month) { begin.tm_mday=0;first=mktime(&begin); }
  char fromArg[16],toArg[16]; strftime(fromArg,sizeof(fromArg),"%d/%m/%Y",&begin); strftime(toArg,sizeof(toArg),"%d/%m/%Y",&end);
  strftime(out.from,sizeof(out.from),"%d.%m.%y",&begin); strftime(out.to,sizeof(out.to),"%d.%m.%y",&end);
  if(WiFi.status()!=WL_CONNECTED) {strlcpy(out.message,"Нет Wi-Fi",sizeof(out.message));finish(job,out);return;}
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(8); HTTPClient http; http.setConnectTimeout(5000);http.setTimeout(8000);http.useHTTP10(true);
  String url=String("https://www.cbr.ru/scripts/XML_dynamic.asp?date_req1=")+fromArg+"&date_req2="+toArg+"&VAL_NM_RQ="+ids[job.currency];
  if(!http.begin(client,url)) {strlcpy(out.message,"Ошибка HTTPS",sizeof(out.message));finish(job,out);return;}
  int code=http.GET(); if(code!=200) {snprintf(out.message,sizeof(out.message),"ЦБ: HTTP %d",code);http.end();finish(job,out);return;}
  WiFiClient &stream=http.getStream(); char token[384]; bool inRecord=false, wantNominal=false, wantValue=false; float nominal=1, value=0; time_t recordTime=0; int points=0;
  while(http.connected() || stream.available()) {
    size_t n=stream.readBytesUntil('>',token,sizeof(token)-1); if(!n) break; token[n]=0;
    char *record=strstr(token,"<Record Date=\"");
    if(record) { int d=0,m=0,y=0; if(sscanf(record,"<Record Date=\"%d.%d.%d",&d,&m,&y)==3 && d>0 && d<=31 && m>0 && m<=12) { struct tm t{}; t.tm_mday=d;t.tm_mon=m-1;t.tm_year=y-1900;recordTime=mktime(&t);inRecord=true;nominal=1;value=0; } }
    else if(inRecord && strstr(token,"<Nominal")) wantNominal=true;
    else if(inRecord && wantNominal) { nominal=atof(token); wantNominal=false; }
    else if(inRecord && strstr(token,"<Value")) wantValue=true;
    else if(inRecord && wantValue) { for(char *p=token;*p;p++) if(*p==',') *p='.'; value=atof(token);wantValue=false; }
    else if(inRecord && strstr(token,"</Record")) { if(value>0 && nominal>0 && recordTime>=first && recordTime<=last) { int bucket=(int)(((int64_t)(recordTime-first)*(RateHistory::kPoints-1))/(last-first)); if(bucket>=0 && bucket<RateHistory::kPoints) { if(out.values[bucket]==0) points++; out.values[bucket]=value/nominal; } } inRecord=false; }
    yield();
  }
  http.end(); out.valid=points>=2; strlcpy(out.message,out.valid?"ЦБ РФ, руб. за 1 ед.":"ЦБ: нет данных",sizeof(out.message)); finish(job,out);
}
void historyTask(void*) { HistoryJob job; for(;;) if(xQueueReceive(historyQueue,&job,portMAX_DELAY)==pdTRUE) fetch(job); }
void ensureHistory() { if(historyQueue) return; historyMutex=xSemaphoreCreateMutex(); historyQueue=xQueueCreate(1,sizeof(HistoryJob)); if(historyQueue && historyMutex) xTaskCreatePinnedToCore(historyTask,"ratehist",8192,nullptr,1,nullptr,0); }
}
void requestRateHistory(uint8_t currency,uint8_t months) {
  if(currency>=4 || (months!=6 && months!=12)) return; ensureHistory(); if(!historyQueue || !historyMutex) return;
  if(xSemaphoreTake(historyMutex,pdMS_TO_TICKS(100))) { HistoryJob job{currency,months,++generation}; sharedHistory=RateHistory(); sharedHistory.currency=currency;sharedHistory.months=months;sharedHistory.loading=true;sharedHistory.version=job.generation;strlcpy(sharedHistory.message,"Загрузка ЦБ...",sizeof(sharedHistory.message)); xQueueOverwrite(historyQueue,&job);xSemaphoreGive(historyMutex); }
}
void copyRateHistory(RateHistory &destination) { if(historyMutex && xSemaphoreTake(historyMutex,pdMS_TO_TICKS(20))) { destination=sharedHistory; xSemaphoreGive(historyMutex); } }
