#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_random.h>
#include <jpeg_decoder.h>
#include <cJSON.h>
#include "vision.h"
#ifdef WAVESHARE_LCD_2
#include "lcd.h"
String screenKey;
#endif

constexpr int BUZZER = 4;  // Active-high, logic-level input on the buzzer module.
constexpr unsigned long PERIOD = 3000;
constexpr size_t JPEG_CAP = 192 * 1024, RGB_CAP = 200 * 150 * 3;
WebServer web(80);
Preferences prefs;
Vision vision;
String addr, serialNumber, checkCode, phase = "Waiting", job;
uint8_t *jpeg, *rgb;
size_t jpegLen = 0;
uint32_t lastPoll, lastBeep;
int roi[] = {10, 25, 90, 92}; // left, top, right, bottom, in percent
int failures = 0, lastLayer = -1;
bool active = false, visualAlarm = false;

void beep(int ms) {
#ifndef WAVESHARE_LCD_2
  digitalWrite(BUZZER, HIGH); delay(ms); digitalWrite(BUZZER, LOW);
#endif
}
void resetWatch() {
  vision.reset(); jpegLen = 0; visualAlarm = active = false; job = ""; lastLayer = -1;
}
void fault(const String &message) {
  phase = message; resetWatch();
  if (failures < 3) ++failures;
  if (failures == 3 && millis() - lastBeep > 15000) {
    beep(60); delay(120); beep(60); lastBeep = millis();
  }
}
bool printerIP(const String &s) {
  IPAddress ip;
  return ip.fromString(s) && ip[0]==192 && ip[1]==168 && ip[2]==4 && ip[3]>=2 && ip[3]<255;
}
void page() {
  String html = F("<meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Flashforge Watch</title><h1>Flashforge Watch</h1><p>");
  html += phase + "</p><a href='/'>Обновить</a> · <a href='/last.jpg'>Последний кадр</a>"
#ifndef WAVESHARE_LCD_2
    "<form method=post action='/test'><button>Проверить зуммер</button></form>"
#endif
    "<form method=post action='/save'><p>IP принтера <input required name=ip value='" + addr +
    "'></p><p>Серийный номер <input required name=sn maxlength=80></p>"
    "<p>LAN check code <input required type=password name=code maxlength=80></p>";
  const char *labels[] = {"Слева", "Сверху", "Справа", "Снизу"};
  for (int i=0;i<4;i++) html += String(labels[i])+" (%) <input type=number min=0 max=100 required name=r"+i+" value="+roi[i]+"><br>";
  html += "<button>Сохранить настройки</button></form>";
  web.send(200,"text/html; charset=utf-8",html);
}
void saveConfig() {
  String ip = web.arg("ip"), sn = web.arg("sn"), code = web.arg("code");
  sn.trim(); code.trim();
  int box[4];
  for(int i=0;i<4;i++) {
    String v = web.arg("r"+String(i));
    if (v.isEmpty() || v.length()>3 || strspn(v.c_str(),"0123456789") != v.length()) {
      web.send(400,"text/plain","Invalid region"); return;
    }
    box[i]=v.toInt();
  }
  if (!printerIP(ip) || !sn.length() || sn.length()>80 || !code.length() || code.length()>80 ||
      box[2]>100 || box[3]>100 || box[2]-box[0]<20 || box[3]-box[1]<20) {
    web.send(400,"text/plain","Invalid settings"); return;
  }
  addr=ip; serialNumber=sn; checkCode=code;
  prefs.putString("ip",addr); prefs.putString("sn",sn); prefs.putString("code",code);
  memcpy(roi,box,sizeof roi); prefs.putBytes("roi",roi,sizeof roi);
  resetWatch(); failures=0; phase="Settings saved";
  web.sendHeader("Location","/"); web.send(303);
}
String field(cJSON *obj,const char *key) {
  const char *s=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj,key));
  return s ? s : "";
}
String status(String &state, String &file, int &layer) {
  WiFiClient socket; HTTPClient http;
  http.setConnectTimeout(3000); http.setTimeout(5000); http.useHTTP10(true);
  if (!http.begin(socket,addr,8898,"/detail")) return "Cannot start HTTP client";
  http.addHeader("Content-Type","application/json");
  cJSON *req=cJSON_CreateObject();
  if (!req) return "Status: out of memory";
  cJSON_AddStringToObject(req,"serialNumber",serialNumber.c_str());
  cJSON_AddStringToObject(req,"checkCode",checkCode.c_str());
  char *body=cJSON_PrintUnformatted(req); cJSON_Delete(req);
  if (!body) return "Status: out of memory";
  int rc=http.POST((uint8_t*)body,strlen(body)); cJSON_free(body);
  if(rc<0) return addr+":8898 "+HTTPClient::errorToString(rc)+" ("+String(rc)+")";
  if(rc!=200) return "Status HTTP "+String(rc);
  if(http.getSize()>32768) return "Status body exceeds 32 KiB";
  String response=http.getString();
  cJSON *root=cJSON_Parse(response.c_str());
  if(!root) return "Status: invalid JSON or incomplete reply";
  cJSON *detail=cJSON_GetObjectItemCaseSensitive(root,"detail");
  cJSON *err=cJSON_GetObjectItemCaseSensitive(root,"code");
  state=field(detail,"status"); file=field(detail,"printFileName");
  cJSON *l=cJSON_GetObjectItemCaseSensitive(detail,"printLayer");
  layer=cJSON_IsNumber(l)?l->valueint:-1;
  String problem;
  if (err && !cJSON_IsNumber(err)) problem="Status: unexpected API code format";
  else if (err && err->valueint!=0) problem="Printer API code "+String(err->valueint);
  else if (!state.length()) problem="Status: missing detail.status";
  cJSON_Delete(root); state.toLowerCase(); return problem;
}
// Read a single JPEG from mjpg-streamer's first multipart part.
bool capture() {
  jpegLen=0;
  WiFiClient socket; HTTPClient http;
  http.useHTTP10(true); http.setConnectTimeout(1200); http.setTimeout(1800);
  if(!http.begin(socket,addr,8080,"/?action=stream") || http.GET()!=200) return false;
  auto *stream=http.getStreamPtr();
  uint32_t start=millis();
  size_t used=0; int prev=-1; uint8_t block[768];
  while(millis()-start<3500) {
    int avail=stream->available();
    if(avail<=0) { if(!stream->connected()) break; delay(1); continue; }
    int n=stream->read(block,min(avail,(int)sizeof block));
    for(int i=0;i<n;i++) {
      int b=block[i];
      if(!used) {
        if(prev==255 && b==216) { jpeg[0]=255; jpeg[1]=216; used=2; }
      } else {
        if(used==JPEG_CAP) return false;
        jpeg[used++]=b;
        if(prev==255 && b==217) { jpegLen=used; return true; }
      }
      prev=b;
    }
  }
  return false;
}
int light(const uint8_t *p) { return (p[0]*77+p[1]*150+p[2]*29)>>8; }
bool measure(Cell (&cells)[Vision::count]) {
  esp_jpeg_image_cfg_t cfg{}; esp_jpeg_image_output_t out{};
  cfg.indata=jpeg; cfg.indata_size=jpegLen; cfg.outbuf=rgb; cfg.outbuf_size=RGB_CAP;
  cfg.out_format=JPEG_IMAGE_FORMAT_RGB888; cfg.out_scale=JPEG_IMAGE_SCALE_1_8;
  if(esp_jpeg_decode(&cfg,&out)!=ESP_OK) return false;
#ifdef WAVESHARE_LCD_2
  if (!out.width || !out.height) return false;
  paintCamera(rgb,out.width,out.height);
#endif
  int x0=out.width*roi[0]/100, y0=out.height*roi[1]/100;
  int w=out.width*roi[2]/100-x0, h=out.height*roi[3]/100-y0;
  if(w<Vision::cols*2 || h<Vision::rows*2) return false;
  for(int r=0;r<Vision::rows;r++) for(int c=0;c<Vision::cols;c++) {
    int sum=0, edges=0, pixels=0;
    for(int y=y0+h*r/Vision::rows;y<y0+h*(r+1)/Vision::rows;y++)
      for(int x=x0+w*c/Vision::cols;x<x0+w*(c+1)/Vision::cols;x++) {
        const uint8_t *p=rgb+3*(y*out.width+x);
        int value=light(p);
        bool edge=(x>x0 && abs(value-light(p-3))>24) ||
                  (y>y0 && abs(value-light(p-3*out.width))>24);
        sum+=value; edges+=edge; ++pixels;
      }
    cells[r*Vision::cols+c]={sum/pixels,100*edges/pixels};
  }
  return true;
}
void pollPrinter() {
  if(!jpeg || !rgb) { phase="Memory unavailable: check PSRAM setting"; return; }
  if(!serialNumber.length() || !checkCode.length()) { phase="Enter printer credentials"; return; }
  String state,file; int layer;
  String problem=status(state,file,layer);
  if(problem.length()) { fault(problem); return; }
  if(state=="error") { fault("Printer reports an error"); return; }
  if(state!="printing") { resetWatch(); failures=0; phase="Printer: "; 
    for (char c : state) if (isalnum((unsigned char)c) || c=='_') phase+=c;
    return;
  }
  if(!active || file!=job || (layer>=0 && lastLayer>layer)) resetWatch();
  active=true; job=file; lastLayer=layer;
  Cell cells[Vision::count];
  if(!capture() || !measure(cells)) { fault("Camera or JPEG unavailable"); return; }
  failures=0;
  auto score=vision.update(cells);
  bool suspect=score.shifted>=5 || score.textured>=4;
  phase=!score.ready?"Learning scene":suspect?"ALARM: persistent visual change":"Watching";
  phase+=" | brightness group="+String(score.shifted)+" | edge group="+String(score.textured);
  if(suspect && (!visualAlarm || millis()-lastBeep>15000)) { beep(450); lastBeep=millis(); }
  visualAlarm=suspect;
}
void setup() {
#ifdef WAVESHARE_LCD_2
  beginScreen();
#else
  pinMode(BUZZER,OUTPUT); digitalWrite(BUZZER,LOW);
#endif
  Serial.begin(115200);
  prefs.begin("ff-watch",false);
  addr=prefs.getString("ip","192.168.4.2"); serialNumber=prefs.getString("sn","");
  checkCode=prefs.getString("code",""); prefs.getBytes("roi",roi,sizeof roi);
  jpeg=(uint8_t*)ps_malloc(JPEG_CAP); rgb=(uint8_t*)ps_malloc(RGB_CAP);
  WiFi.mode(WIFI_AP);
  String password=prefs.getString("apkey","");
  if(password.length()!=12) {
    char key[16]; snprintf(key,sizeof key,"%08lX%04lX",(unsigned long)esp_random(),(unsigned long)(esp_random()&65535));
    password=key; prefs.putString("apkey",password);
  }
  WiFi.softAP("Flashforge-Watch",password.c_str());
#ifdef WAVESHARE_LCD_2
  screenKey=password;
  screenStatus(phase,false,-1,false,false,screenKey);
#endif
  web.on("/",HTTP_GET,page); web.on("/save",HTTP_POST,saveConfig);
  web.on("/test",HTTP_POST,[](){ beep(250); web.sendHeader("Location","/"); web.send(303); });
  web.on("/last.jpg",HTTP_GET,[](){
    if(!jpegLen) { web.send(404,"text/plain","No current frame"); return; }
    web.sendHeader("Cache-Control","no-store");
    web.send_P(200,"image/jpeg",(const char*)jpeg,jpegLen);
  });
  web.begin();
}
void loop() {
  static uint32_t lastInfo=0; static bool shown=false;
  if(!shown || millis()-lastInfo>=15000) {
    Serial.printf("AP Flashforge-Watch | password %s | http://192.168.4.1 | %s\n",
                  prefs.getString("apkey","").c_str(),phase.c_str());
    shown=true; lastInfo=millis();
  }
  web.handleClient();
  if(millis()-lastPoll>=PERIOD) {
    pollPrinter();
#ifdef WAVESHARE_LCD_2
    screenStatus(phase,active,lastLayer,jpegLen>0,visualAlarm || failures>0,screenKey);
#endif
    lastPoll=millis();
  }
  delay(1);
}
