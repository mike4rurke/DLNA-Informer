// Minimal UPnP AV MediaServer for the CYD SD card. Runs only on the UI task.
WiFiUDP dlnaUdp;
bool dlnaJoined=false;
uint32_t dlnaLastNotify=0;
String dlnaUuid() { uint64_t mac=ESP.getEfuseMac(); char s[48]; snprintf(s,sizeof(s),"uuid:4359442d-5344-4d53-%04x-%08lx",(unsigned)((mac>>32)&0xffff),(unsigned long)(mac&0xffffffff)); return s; }
String dlnaBase() { return "http://"+WiFi.localIP().toString(); }
String xmlEsc(const String &s) { return htmlEscape(s); }
String hexPath(const String &s) { const char *h="0123456789abcdef"; String x="p"; for(unsigned i=0;i<s.length();++i){ uint8_t c=s[i]; x+=h[c>>4];x+=h[c&15]; } return x; }
String unhexPath(const String &id) { if(id=="0") return "/"; if(!id.startsWith("p") || !(id.length()&1) || id.length()>481) return ""; String out; for(unsigned i=1;i<id.length();i+=2){ char a=id[i],b=id[i+1]; int x=a>='0'&&a<='9'?a-'0':a>='a'&&a<='f'?a-'a'+10:-1; int y=b>='0'&&b<='9'?b-'0':b>='a'&&b<='f'?b-'a'+10:-1; if(x<0||y<0 || (x*16+y)<32) return ""; out+=(char)(x*16+y); } return safeSdPath(out)?out:""; }
String tagValue(const String &body,const char *tag) { String a=String("<")+tag+">", b=String("</")+tag+">"; int i=body.indexOf(a); if(i<0)return ""; i+=a.length();int j=body.indexOf(b,i); return j<0?"":body.substring(i,j); }
String musicMime(String name) { name.toLowerCase(); if(name.endsWith(".mp3"))return "audio/mpeg"; if(name.endsWith(".flac"))return "audio/flac"; if(name.endsWith(".wav"))return "audio/wav"; if(name.endsWith(".m4a")||name.endsWith(".mp4"))return "audio/mp4"; if(name.endsWith(".aac"))return "audio/aac"; if(name.endsWith(".ogg"))return "audio/ogg"; return ""; }
String dlnaItem(const String &path,const String &parent,File &f) {
 String id=path=="/"?"0":hexPath(path), par=parent=="/"?"0":hexPath(parent); String name=path.substring(path.lastIndexOf('/')+1); if(path=="/") name="SD CYD";
 if(f.isDirectory()) return "<container id=\""+id+"\" parentID=\""+par+"\" restricted=\"1\"><dc:title>"+xmlEsc(name)+"</dc:title><upnp:class>object.container.storageFolder</upnp:class></container>";
 String mime=musicMime(name); if(!mime.length())return "";
 return "<item id=\""+id+"\" parentID=\""+par+"\" restricted=\"1\"><dc:title>"+xmlEsc(name)+"</dc:title><upnp:class>object.item.audioItem.musicTrack</upnp:class><res protocolInfo=\"http-get:*:"+mime+":*\" size=\""+String((unsigned long)f.size())+"\">"+xmlEsc(dlnaBase()+"/media?path="+urlEncode(path))+"</res></item>";
}
void dlnaDescription() {
 String x="<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion><device><deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType><friendlyName>CYD SD Music</friendlyName><manufacturer>CYD</manufacturer><modelName>SD MediaServer</modelName><UDN>"+dlnaUuid()+"</UDN><serviceList><service><serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType><serviceId>urn:upnp-org:serviceId:ContentDirectory</serviceId><SCPDURL>/cd.xml</SCPDURL><controlURL>/cd</controlURL><eventSubURL>/cd-event</eventSubURL></service><service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType><serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId><SCPDURL>/cm.xml</SCPDURL><controlURL>/cm</controlURL><eventSubURL>/cm-event</eventSubURL></service></serviceList></device></root>";
 sdWeb.send(200,"text/xml; charset=utf-8",x);
}
const char cdScpd[] PROGMEM=R"xml(<?xml version="1.0"?><scpd xmlns="urn:schemas-upnp-org:service-1-0"><specVersion><major>1</major><minor>0</minor></specVersion><actionList><action><name>Browse</name><argumentList><argument><name>ObjectID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument><argument><name>BrowseFlag</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_BrowseFlag</relatedStateVariable></argument><argument><name>Filter</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Filter</relatedStateVariable></argument><argument><name>StartingIndex</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Index</relatedStateVariable></argument><argument><name>RequestedCount</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument><argument><name>SortCriteria</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SortCriteria</relatedStateVariable></argument><argument><name>Result</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument><argument><name>NumberReturned</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument><argument><name>TotalMatches</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument><argument><name>UpdateID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_UpdateID</relatedStateVariable></argument></argumentList></action></actionList><serviceStateTable><stateVariable sendEvents="no"><name>A_ARG_TYPE_ObjectID</name><dataType>string</dataType></stateVariable><stateVariable sendEvents="no"><name>A_ARG_TYPE_BrowseFlag</name><dataType>string</dataType></stateVariable><stateVariable sendEvents="no"><name>A_ARG_TYPE_Filter</name><dataType>string</dataType></stateVariable><stateVariable sendEvents="no"><name>A_ARG_TYPE_Index</name><dataType>ui4</dataType></stateVariable><stateVariable sendEvents="no"><name>A_ARG_TYPE_Count</name><dataType>ui4</dataType></stateVariable><stateVariable sendEvents="no"><name>A_ARG_TYPE_SortCriteria</name><dataType>string</dataType></stateVariable><stateVariable sendEvents="no"><name>A_ARG_TYPE_Result</name><dataType>string</dataType></stateVariable><stateVariable sendEvents="no"><name>A_ARG_TYPE_UpdateID</name><dataType>ui4</dataType></stateVariable></serviceStateTable></scpd>)xml";
const char cmScpd[] PROGMEM=R"xml(<?xml version="1.0"?><scpd xmlns="urn:schemas-upnp-org:service-1-0"><specVersion><major>1</major><minor>0</minor></specVersion><actionList><action><name>GetProtocolInfo</name><argumentList><argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument><argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument></argumentList></action></actionList><serviceStateTable><stateVariable sendEvents="no"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable><stateVariable sendEvents="no"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable></serviceStateTable></scpd>)xml";
String soapWrap(const String &inner) {return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"+inner+"</s:Body></s:Envelope>";}
void dlnaControl() {
 String body=sdWeb.arg("plain"), action=sdWeb.header("SOAPACTION");
 if(action.indexOf("Browse")<0) {
   String name, value;
   if(action.indexOf("GetSystemUpdateID")>=0) {name="GetSystemUpdateID";value="<Id>1</Id>";}
   else if(action.indexOf("GetSearchCapabilities")>=0) {name="GetSearchCapabilities";value="<SearchCaps></SearchCaps>";}
   else if(action.indexOf("GetSortCapabilities")>=0) {name="GetSortCapabilities";value="<SortCaps></SortCaps>";}
   else {sdWeb.send(500,"text/plain","Unsupported action"); return;}
   sdWeb.send(200,"text/xml; charset=utf-8",soapWrap("<u:"+name+"Response xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"+value+"</u:"+name+"Response>")); return;
 }
 String path=unhexPath(tagValue(body,"ObjectID")); if(!path.length()) { sdWeb.send(400,"text/plain","Invalid object"); return; }
 if(!sdMount()){ sdWeb.send(503,"text/plain","SD unavailable");return; }
 File dir=SD.open(path.c_str()); if(!dir){sdUnmount();sdWeb.send(404,"text/plain","Object not found");return;}
 String didl="<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">";
 uint32_t count=0,total=0,start=tagValue(body,"StartingIndex").toInt(), requested=tagValue(body,"RequestedCount").toInt(); if(!requested || requested>20) requested=20;
 if(tagValue(body,"BrowseFlag")=="BrowseMetadata") { String parent=path.substring(0,path.lastIndexOf('/')); if(!parent.length())parent="/"; String item=dlnaItem(path,parent,dir); if(item.length()){didl+=item;count=total=1;} }
 else if(dir.isDirectory()) { File f=dir.openNextFile(); while(f) { String name=f.name(); int slash=name.lastIndexOf('/'); if(slash>=0)name=name.substring(slash+1); if(name.length() && name[0]!='.' && (f.isDirectory() || musicMime(name).length())) { if(total>=start && count<requested) { String child=path=="/"?"/"+name:path+"/"+name; if(child.length()<=240) { didl+=dlnaItem(child,path,f);count++; } } total++; } f.close();f=dir.openNextFile();yield(); } }
 dir.close(); sdUnmount(); didl+="</DIDL-Lite>";
 String res="<u:BrowseResponse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\"><Result>"+xmlEsc(didl)+"</Result><NumberReturned>"+String(count)+"</NumberReturned><TotalMatches>"+String(total)+"</TotalMatches><UpdateID>1</UpdateID></u:BrowseResponse>";
 sdWeb.send(200,"text/xml; charset=utf-8",soapWrap(res));
}
void dlnaConnection() { String inner="<u:GetProtocolInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\"><Source>http-get:*:audio/mpeg:*,http-get:*:audio/flac:*,http-get:*:audio/wav:*,http-get:*:audio/mp4:*,http-get:*:audio/aac:*,http-get:*:audio/ogg:*</Source><Sink></Sink></u:GetProtocolInfoResponse>"; sdWeb.send(200,"text/xml; charset=utf-8",soapWrap(inner)); }
void dlnaMedia() {
 String path=sdWeb.arg("path"); if(!safeSdPath(path)||!musicMime(path).length()){sdWeb.send(400,"text/plain","Invalid media");return;}
 if(!sdMount()){sdWeb.send(503,"text/plain","SD unavailable");return;} File f=SD.open(path.c_str()); if(!f||f.isDirectory()){f.close();sdUnmount();sdWeb.send(404,"text/plain","Not found");return;}
 size_t size=f.size(),start=0,end=size?size-1:0; String range=sdWeb.header("Range"); bool partial=false;
 if(range.length()) { if(!range.startsWith("bytes=")||range.indexOf(',')>=0) {f.close();sdUnmount();sdWeb.send(416,"text/plain","Range not supported");return;} int dash=range.indexOf('-',6); if(dash<0||dash==6){f.close();sdUnmount();sdWeb.send(416,"text/plain","Invalid range");return;} start=range.substring(6,dash).toInt(); if(dash+1<(int)range.length()) end=range.substring(dash+1).toInt(); if(start>=size||end<start){f.close();sdUnmount();sdWeb.send(416,"text/plain","Invalid range");return;} if(end>=size)end=size-1;partial=true; }
 size_t n=size?end-start+1:0; f.seek(start); sdWeb.sendHeader("Accept-Ranges","bytes"); sdWeb.sendHeader("transferMode.dlna.org","Streaming"); if(partial) sdWeb.sendHeader("Content-Range",String("bytes ")+start+"-"+end+"/"+size); sdWeb.setContentLength(n); sdWeb.send(partial?206:200,musicMime(path),""); WiFiClient c=sdWeb.client(); uint8_t buf[1024]; while(n && c.connected()) { size_t want=n>sizeof(buf)?sizeof(buf):n; int r=f.read(buf,want); if(r<=0)break; size_t sent=0; while(sent<(size_t)r && c.connected()) { size_t w=c.write(buf+sent,r-sent); if(!w)break;sent+=w; } if(sent!=(size_t)r)break; n-=r; yield(); } f.close();sdUnmount();
}
void ssdpSend(IPAddress ip,uint16_t port,const String &type,bool notify) { String usn=dlnaUuid(); if(type!=usn)usn+="::"+type; String x=notify?"NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNT: "+type+"\r\nNTS: ssdp:alive\r\n":"HTTP/1.1 200 OK\r\nST: "+type+"\r\n"; x+="CACHE-CONTROL: max-age=1800\r\nLOCATION: "+dlnaBase()+"/description.xml\r\nSERVER: ESP32/1.0 UPnP/1.0 CYD/1.0\r\nUSN: "+usn+"\r\n\r\n"; dlnaUdp.beginPacket(ip,port);dlnaUdp.print(x);dlnaUdp.endPacket(); }
void dlnaLoop() {
 if(WiFi.status()!=WL_CONNECTED){if(dlnaJoined){dlnaUdp.stop();dlnaJoined=false;}return;}
 if(!dlnaJoined){dlnaJoined=dlnaUdp.beginMulticast(IPAddress(239,255,255,250),1900);dlnaLastNotify=0;}
 if(!dlnaJoined)return;
 const String types[]={"upnp:rootdevice",dlnaUuid(),"urn:schemas-upnp-org:device:MediaServer:1","urn:schemas-upnp-org:service:ContentDirectory:1","urn:schemas-upnp-org:service:ConnectionManager:1"};
 if(!dlnaLastNotify || millis()-dlnaLastNotify>600000UL){ for(const auto &t:types)ssdpSend(IPAddress(239,255,255,250),1900,t,true);dlnaLastNotify=millis(); }
 int n=dlnaUdp.parsePacket(); if(n){ char buf[1100]; int len=dlnaUdp.read(buf,sizeof(buf)-1);if(len<0)return;buf[len]=0; String req(buf),lower=req;lower.toLowerCase(); if(lower.startsWith("m-search") && lower.indexOf("ssdp:discover")>=0){ int pos=lower.indexOf("\nst:");if(pos>=0){pos+=4;int end=lower.indexOf('\n',pos);String st=req.substring(pos,end<0?req.length():end);st.trim(); for(const auto &t:types)if(st=="ssdp:all" || st.equalsIgnoreCase(t))ssdpSend(dlnaUdp.remoteIP(),dlnaUdp.remotePort(),t,false); } } }
}
void setupDlna() {
 const char *headers[]={"SOAPACTION","Range"};sdWeb.collectHeaders(headers,2);
 sdWeb.on("/description.xml",HTTP_GET,dlnaDescription);
 sdWeb.on("/cd.xml",HTTP_GET,[](){sdWeb.send_P(200,"text/xml",cdScpd);});
 sdWeb.on("/cm.xml",HTTP_GET,[](){sdWeb.send_P(200,"text/xml",cmScpd);});
 sdWeb.on("/cd",HTTP_POST,dlnaControl);sdWeb.on("/cm",HTTP_POST,dlnaConnection);sdWeb.on("/media",HTTP_GET,dlnaMedia);
}
