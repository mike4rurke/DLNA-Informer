// Passwordless firmware update, intentionally available to every LAN client.
// The random form token prevents blind cross-origin form submissions; it is
// not authentication. All handlers run in the UI task via sdWeb.handleClient.

bool otaStarted=false, otaComplete=false, otaHasError=false;
String otaError;
size_t otaReceived=0;

void webOtaError(const char *text) {
  if(!otaHasError) otaError=text;
  otaHasError=true;
  if(Update.isRunning()) Update.abort();
}
void webOtaUpload() {
  HTTPUpload &u=sdWeb.upload();
  if(u.status==UPLOAD_FILE_START) {
    if(otaStarted) { webOtaError("Отправлен более чем один файл"); return; }
    otaStarted=true; otaComplete=false; otaHasError=false; otaError=""; otaReceived=0;
    if(!sdTokenValid()) { webOtaError("Неверный токен; обновите страницу"); return; }
    if(u.name!="firmware" || !u.filename.endsWith(".bin")) { webOtaError("Выберите файл прошивки .bin"); return; }
    if(!esp_ota_get_next_update_partition(nullptr)) { webOtaError("Нет второго OTA-раздела: сначала прошейте проект по USB"); return; }
    if(!Update.begin(UPDATE_SIZE_UNKNOWN,U_FLASH)) {
      Serial.printf("OTA begin error: %s\n",Update.errorString());
      webOtaError("Невозможно начать запись прошивки"); return;
    }
    Serial.println("Web OTA: upload started");
  } else if(u.status==UPLOAD_FILE_WRITE) {
    if(!otaHasError && Update.isRunning()) {
      size_t n=Update.write(u.buf,u.currentSize);
      if(n!=u.currentSize) { Serial.printf("OTA write error: %s\n",Update.errorString()); webOtaError("Ошибка записи; файл может быть слишком большим"); }
      else otaReceived+=n;
    }
  } else if(u.status==UPLOAD_FILE_END) {
    if(!otaHasError && Update.isRunning() && otaReceived==u.totalSize && otaReceived>0) otaComplete=true;
    else if(!otaHasError) webOtaError("Файл получен не полностью");
  } else if(u.status==UPLOAD_FILE_ABORTED) {
    webOtaError("Загрузка прервана"); otaStarted=false; otaComplete=false;
  }
}
void finishWebOta() {
  bool success=false;
  if(!sdTokenValid()) webOtaError("Неверный токен; обновите страницу");
  else if(!otaStarted || !otaComplete || otaHasError) {
    if(!otaHasError) webOtaError("Файл не был загружен");
  } else if(!Update.end(true)) {
    Serial.printf("OTA validation error: %s\n",Update.errorString());
    webOtaError("Ошибка проверки образа прошивки");
  } else success=true;
  otaStarted=false; otaComplete=false;
  sdWeb.sendHeader("Cache-Control","no-store");
  if(!success) {
    String error=htmlEscape(otaError);
    otaHasError=false; otaError="";
    sdWeb.send(400,"text/html; charset=utf-8","<!doctype html><meta charset=utf-8><h2>Обновление не выполнено</h2><p>"+error+"</p><a href='/update'>Назад</a>");
    return;
  }
  Serial.println("Web OTA complete; rebooting");
  sdWeb.send(200,"text/html; charset=utf-8","<!doctype html><meta charset=utf-8><h2>Прошивка загружена</h2><p>Устройство перезагружается. Подождите и обновите страницу.</p><a href='/'>На главную</a>");
  delay(1200); ESP.restart();
}
void webOtaPage() {
  sdWeb.sendHeader("Cache-Control","no-store");
  sdWeb.sendHeader("Referrer-Policy","no-referrer");
  sdWeb.sendHeader("X-Content-Type-Options","nosniff");
  String page="<!doctype html><html lang=ru><meta charset=utf-8><meta name=viewport content='width=device-width'><title>Обновление CYD</title><style>body{font:16px sans-serif;max-width:700px;margin:2em auto;padding:0 1em;background:#101827;color:#eee}a{color:#67d8c1}button{padding:.7em;margin-top:1em}input{display:block;margin-top:1em}</style><h1>Обновление CYD</h1><p>Выберите <b>firmware.bin</b>, собранный для этого проекта. Не отключайте питание.</p><p>Пароль не требуется: любой клиент сети может заменить прошивку. Используйте только доверенную сеть.</p><form method='post' enctype='multipart/form-data' action='/update?token="+sdCsrf+"' onsubmit=\"return confirm('Установить прошивку и перезагрузить CYD?')\"><input type='file' name='firmware' accept='.bin,application/octet-stream' required><button type='submit'>Обновить прошивку</button></form><p><a href='/'>Файлы SD</a></p></html>";
  sdWeb.send(200,"text/html; charset=utf-8",page);
}
void setupWebOta() {
  sdWeb.on("/update",HTTP_GET,webOtaPage);
  sdWeb.on("/update",HTTP_POST,finishWebOta,webOtaUpload);
}
