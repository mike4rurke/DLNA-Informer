// All handlers run in loop() on the owner of the shared touch/SD SPI bus.
String sdCsrf;
File uploadFile;
String uploadTemp, uploadDestination, uploadDir;
bool uploadMounted=false, uploadSeen=false, uploadDone=false;
int uploadError=0;

bool sdAdminConfigured() { return strlen(SD_WEB_USER)>0 && strlen(SD_WEB_PASSWORD)>0; }
bool sdAuthenticated() { return sdAdminConfigured() && sdWeb.authenticate(SD_WEB_USER,SD_WEB_PASSWORD); }
bool sdAuthorize() {
  if (!sdAdminConfigured()) { sdWeb.send(403,"text/plain; charset=utf-8","Укажите SD_WEB_PASSWORD в config.h и перепрошейте устройство"); return false; }
  if (!sdAuthenticated()) { sdWeb.requestAuthentication(); return false; }
  return true;
}
bool sdTokenValid() { return sdCsrf.length() && sdWeb.arg("token").equalsConstantTime(sdCsrf); }
bool validUploadName(const String &s) {
  if(!s.length() || s.length()>120 || s[0]=='.' || s[s.length()-1]==' ' || s[s.length()-1]=='.') return false;
  for(unsigned i=0;i<s.length();++i) { unsigned char c=s[i]; if(c<32 || c==127 || strchr("/\\:*?\"<>|",c)) return false; }
  return true;
}
void uploadCleanup(bool removePartial) {
  if(uploadFile) uploadFile.close();
  if(uploadMounted) { if(removePartial && uploadTemp.length()) SD.remove(uploadTemp.c_str()); sdUnmount(); uploadMounted=false; }
}
void onSdUpload() {
  HTTPUpload &u=sdWeb.upload();
  if(u.status==UPLOAD_FILE_START) {
    if(uploadSeen) { uploadError=400; uploadCleanup(true); return; }
    uploadSeen=true; uploadDone=false; uploadError=0; uploadTemp=""; uploadDestination="";
    if(!sdAuthenticated()) { uploadError=401; return; }
    if(!sdTokenValid()) { uploadError=403; return; }
    uploadDir=sdWeb.arg("path");
    if(!safeSdPath(uploadDir) || !validUploadName(u.filename)) { uploadError=400; return; }
    uploadDestination=uploadDir=="/" ? "/"+u.filename : uploadDir+"/"+u.filename;
    if(!safeSdPath(uploadDestination)) { uploadError=400; return; }
    if(!sdMount()) { uploadError=503; return; } uploadMounted=true;
    File dir=SD.open(uploadDir.c_str()); bool directory=dir && dir.isDirectory(); dir.close();
    if(!directory) { uploadError=404; uploadCleanup(true); return; }
    if(SD.exists(uploadDestination.c_str())) { uploadError=409; uploadCleanup(true); return; }
    for(int i=0;i<8;++i) {
      char tempName[32]; snprintf(tempName,sizeof(tempName),".cyd-%08lx.tmp",(unsigned long)esp_random());
      uploadTemp=uploadDir=="/" ? "/"+String(tempName) : uploadDir+"/"+tempName;
      if(uploadTemp.length()<=240 && !SD.exists(uploadTemp.c_str())) break;
      uploadTemp="";
    }
    if(!uploadTemp.length()) { uploadError=500; uploadCleanup(true); return; }
    uploadFile=SD.open(uploadTemp.c_str(),FILE_WRITE);
    if(!uploadFile) { uploadError=500; uploadCleanup(true); return; }
  } else if(u.status==UPLOAD_FILE_WRITE) {
    if(!uploadError && uploadMounted && uploadFile && uploadFile.write(u.buf,u.currentSize)!=u.currentSize) { uploadError=507; uploadCleanup(true); }
  } else if(u.status==UPLOAD_FILE_END) {
    if(!uploadError && uploadMounted) {
      uploadFile.flush();
      if (uploadFile.size() != u.totalSize) uploadError=507;
      uploadFile.close();
      // Never replace an existing file, even if it appeared during upload.
      if(!uploadError && SD.exists(uploadDestination.c_str())) uploadError=409;
      else if(!uploadError && !SD.rename(uploadTemp.c_str(),uploadDestination.c_str())) uploadError=500;
      uploadDone=!uploadError;
      uploadCleanup(!uploadDone);
    }
  } else if(u.status==UPLOAD_FILE_ABORTED) {
    uploadError=400; uploadCleanup(true); uploadSeen=false; uploadDone=false;
  }
}
void finishSdUpload() {
  uploadCleanup(!uploadDone);
  int error=uploadError; bool done=uploadDone, seen=uploadSeen; String dir=uploadDir;
  uploadSeen=false; uploadDone=false; uploadError=0;
  if(!sdAuthorize()) return;
  if(!sdTokenValid()) { sdWeb.send(403,"text/plain; charset=utf-8","Неверный токен; обновите страницу"); return; }
  if(!seen || !done || error) {
    if(!error) error=400;
    String msg=error==409?"Файл уже существует; сначала удалите его":error==507?"Ошибка записи: возможно карта заполнена":error==503?"SD карта недоступна":"Ошибка загрузки файла";
    sdWeb.send(error,"text/plain; charset=utf-8",msg); return;
  }
  sdWeb.sendHeader("Location","/?path="+urlEncode(dir)); sdWeb.send(303,"text/plain","");
}
void deleteSdFile() {
  if(!sdAuthorize()) return;
  if(!sdTokenValid()) {sdWeb.send(403,"text/plain; charset=utf-8","Неверный токен; обновите страницу");return;}
  String path=sdWeb.arg("path");
  if(!safeSdPath(path) || path=="/") {sdWeb.send(400,"text/plain","Invalid path");return;}
  if(!sdMount()) {sdWeb.send(503,"text/plain","SD unavailable");return;}
  File f=SD.open(path.c_str(),FILE_READ); bool exists=bool(f), directory=exists && f.isDirectory(); f.close();
  if(!exists || directory) {sdUnmount();sdWeb.send(directory?400:404,"text/plain; charset=utf-8",directory?"Удаление папок не поддерживается":"Файл не найден");return;}
  bool ok=SD.remove(path.c_str()); sdUnmount();
  if(!ok) {sdWeb.send(500,"text/plain; charset=utf-8","Не удалось удалить файл");return;}
  int i=path.lastIndexOf('/'); String parent=i<=0?"/":path.substring(0,i);
  sdWeb.sendHeader("Location","/?path="+urlEncode(parent));sdWeb.send(303,"text/plain","");
}
void setupSdAdmin() {
  char token[33]; snprintf(token,sizeof(token),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random()); sdCsrf=token;
  sdWeb.on("/upload",HTTP_POST,finishSdUpload,onSdUpload);
  sdWeb.on("/delete",HTTP_POST,deleteSdFile);
}
