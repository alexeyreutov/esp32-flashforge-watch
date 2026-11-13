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
int printPercent = -1, secondsLeft = -1;
bool caution = false;
// Half-resolution RGB for the LCD; the same buffer is reused for analysis.
constexpr size_t RGB_CAP = 800 * 600 * 3;
#else
constexpr size_t RGB_CAP = 200 * 150 * 3;
#endif

constexpr int BUZZER = 4;  // Active-high, logic-level input on the buzzer module.
constexpr unsigned long PERIOD = 3000;
constexpr size_t JPEG_CAP = 192 * 1024;
WebServer web(80);
Preferences prefs;
Vision vision;
String addr, serialNumber, checkCode, phase = "Waiting", job;
String apPassword;
uint8_t *jpeg, *rgb;
size_t jpegLen = 0;
uint32_t lastPoll, lastBeep;
int roi[] = {10, 25, 90, 92}; // left, top, right, bottom, in percent
int failures = 0, lastLayer = -1;
bool active = false, visualAlarm = false;

// Web task owns the form and NVS. The printer loop consumes whole snapshots.
struct Settings {
  char ip[16], sn[81], code[81];
  int box[4];
};
Settings form{};
QueueHandle_t pendingSettings;
SemaphoreHandle_t previewLock;
String webPhase = "Waiting";
const char *saveStage = "После запуска запросов ещё не было";
uint8_t *preview;
size_t previewLen = 0;

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
String htmlEscape(String s) {
  s.replace("&","&amp;"); s.replace("'","&#39;"); s.replace("\"","&quot;");
  s.replace("<","&lt;"); s.replace(">","&gt;"); return s;
}
void page() {
  xSemaphoreTake(previewLock,portMAX_DELAY);
  String currentPhase=webPhase;
  xSemaphoreGive(previewLock);
  String html = F(R"HTML(<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Flashforge Watch</title>
<style>*{box-sizing:border-box}body{margin:0;background:#edf2f2;color:#142e33;font:16px/1.5 system-ui,sans-serif}main{max-width:680px;margin:auto;padding:28px 16px}section,form.panel{background:white;border:1px solid #cbd8d8;border-radius:16px;padding:20px;margin:14px 0}h1,h2,h3{line-height:1.2;margin:0 0 14px}h1{font-size:clamp(28px,6vw,40px)}h2{font-size:21px}h3{font-size:16px;margin-top:20px}p{overflow-wrap:anywhere}a{color:#00695e;margin-right:14px}label{display:block;font-weight:600;margin:10px 0}input{display:block;width:100%;padding:10px;margin-top:4px;border:1px solid #8babab;border-radius:7px;font:inherit}input:focus-visible,button:focus-visible{outline:3px solid #49bbb1}.region{display:grid;grid-template-columns:repeat(2,1fr);gap:0 12px}button{padding:10px 18px;border:0;border-radius:8px;background:#075f56;color:white;font:inherit;font-weight:600;cursor:pointer}</style>
<main><h1>Flashforge Watch</h1><section><h2>Состояние принтера</h2><p>)HTML");
  html += htmlEscape(currentPhase) + "</p><a href='/'>Обновить</a><a href='/last.jpg'>Последний кадр</a></section>"
#ifndef WAVESHARE_LCD_2
    "<form method=post action='/test'><button>Проверить зуммер</button></form>"
#endif
    "<form class=panel method=post action='/save'><h2>Подключение</h2><label>IP принтера<input required name=ip value='" + htmlEscape(form.ip) +
    "'></label><label>Серийный номер<input required name=sn maxlength=80 value='" + htmlEscape(form.sn) +
    "'></label><label>LAN check code<input required type=password name=code maxlength=80 value='" + htmlEscape(form.code) +
    "'></label><h3>Область наблюдения</h3><div class=region>";
  const char *labels[] = {"Слева", "Сверху", "Справа", "Снизу"};
  for (int i=0;i<4;i++) html += "<label>"+String(labels[i])+" (%)<input type=number min=0 max=100 required name=r"+i+" value="+form.box[i]+"></label>";
  html += "</div><button>Сохранить настройки</button> <output aria-live='polite'></output><p>Последнее сохранение: ";
  html += saveStage;
  html += F(R"HTML(</p></form></main>
<script>
const settings = document.querySelector('form.panel');
settings.addEventListener('submit', async event => {
  event.preventDefault();
  const button = settings.querySelector('button');
  const feedback = settings.querySelector('output');
  if (button.disabled) return;
  button.disabled = true;
  feedback.textContent = 'Сохранение…';
  const request = new AbortController();
  const timeout = setTimeout(() => request.abort(), 10000);
  try {
    const response = await fetch(settings.action, {
      method: 'POST', body: new URLSearchParams(new FormData(settings)),
      headers: {Accept: 'text/plain'}, signal: request.signal, cache: 'no-store'
    });
    const message = await response.text();
    feedback.textContent = response.ok && message === 'OK'
      ? 'Настройки сохранены.' : (response.ok ? 'Неожиданный ответ ESP.' : 'Ошибка ' + response.status + ': ' + message);
  } catch (error) {
    feedback.textContent = 'Ответ ESP не получен. Проверьте подключение к Flashforge-Watch и откройте форму в новой вкладке для проверки записи.';
  } finally {
    clearTimeout(timeout);
    button.disabled = false;
  }
});
</script></html>)HTML");
  web.sendHeader("Cache-Control","no-store");
  web.send(200,"text/html; charset=utf-8",html);
}
void saveConfig() {
  Serial.println("[save] received");
  saveStage="Запрос получен, параметры не прошли проверку";
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
  Settings next{};
  ip.toCharArray(next.ip,sizeof next.ip);
  sn.toCharArray(next.sn,sizeof next.sn);
  code.toCharArray(next.code,sizeof next.code);
  memcpy(next.box,box,sizeof box);
  Settings stored{};
  Serial.println("[save] writing");
  if (prefs.putBytes("config-v1",&next,sizeof next)!=sizeof next ||
      prefs.getBytes("config-v1",&stored,sizeof stored)!=sizeof stored ||
      memcmp(&next,&stored,sizeof stored)!=0) {
    Serial.println("[save] storage error");
    saveStage="Ошибка записи или проверки flash";
    web.send(500,"text/plain; charset=utf-8","Не удалось записать настройки во flash-память ESP.");
    return;
  }
  form=next;
  saveStage="Запись во flash подтверждена";
  xQueueOverwrite(pendingSettings,&next);
  Serial.println("[save] verified");
  web.sendHeader("Cache-Control","no-store");
  if (web.header("Accept")=="text/plain") web.send(200,"text/plain","OK");
  else web.send(200,"text/html; charset=utf-8","<!doctype html><html lang=ru><meta name=viewport content='width=device-width,initial-scale=1'><title>Flashforge Watch</title><main style='font:18px system-ui;padding:24px;color:#142e33'><h1>Настройки сохранены</h1><a href='/'>Вернуться к мониторингу</a></main></html>");
  Serial.println("[save] response sent");
}
void applySettings(const Settings &s) {
  addr=s.ip; serialNumber=s.sn; checkCode=s.code;
  memcpy(roi,s.box,sizeof roi);
}
String field(cJSON *obj,const char *key) {
  const char *s=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj,key));
  return s ? s : "";
}
#ifdef WAVESHARE_LCD_2
double telemetryNumber(cJSON *detail, const char *key) {
  cJSON *item=cJSON_GetObjectItemCaseSensitive(detail,key);
  if (cJSON_IsNumber(item)) return item->valuedouble;
  if (!cJSON_IsString(item)) return NAN;
  char *end;
  double value=strtod(item->valuestring,&end);
  return end!=item->valuestring && !*end ? value : NAN;
}
#endif
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
#ifdef WAVESHARE_LCD_2
  double progress=telemetryNumber(detail,"printProgress");
  printPercent=progress>=0 && progress<=100 ? lround(progress<=1 ? progress*100 : progress) : -1;
  double eta=telemetryNumber(detail,"estimatedTime");
  secondsLeft=eta>0 && eta<=INT_MAX ? int(eta) : -1;
#endif
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
#ifdef WAVESHARE_LCD_2
  cfg.out_scale=JPEG_IMAGE_SCALE_1_2;
  if (esp_jpeg_decode(&cfg,&out)==ESP_OK && out.width && out.height)
    paintCamera(rgb,out.width,out.height);
  else frameVisible=false;
  cfg.priv.read=0;
  cfg.out_scale=JPEG_IMAGE_SCALE_1_8;
#endif
  if(esp_jpeg_decode(&cfg,&out)!=ESP_OK) return false;
#ifdef WAVESHARE_LCD_2
  if (!frameVisible && out.width && out.height) paintCamera(rgb,out.width,out.height);
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
#ifdef WAVESHARE_LCD_2
  caution=!score.ready || score.shifted>0 || score.textured>0;
#endif
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
  if (prefs.getBytesLength("config-v1")!=sizeof form ||
      prefs.getBytes("config-v1",&form,sizeof form)!=sizeof form) {
    prefs.getString("ip","192.168.4.2").toCharArray(form.ip,sizeof form.ip);
    prefs.getString("sn","").toCharArray(form.sn,sizeof form.sn);
    prefs.getString("code","").toCharArray(form.code,sizeof form.code);
    memcpy(form.box,roi,sizeof roi);
    prefs.getBytes("roi",form.box,sizeof form.box);
  }
  applySettings(form);
  pendingSettings=xQueueCreate(1,sizeof(Settings));
  previewLock=xSemaphoreCreateMutex();
  preview=(uint8_t*)ps_malloc(JPEG_CAP);
  if (!pendingSettings || !previewLock) { Serial.println("Web task allocation failed"); abort(); }
  jpeg=(uint8_t*)ps_malloc(JPEG_CAP); rgb=(uint8_t*)ps_malloc(RGB_CAP);
  WiFi.mode(WIFI_AP);
  String password=prefs.getString("apkey","");
  if(password.length()!=12) {
    char key[16]; snprintf(key,sizeof key,"%08lX%04lX",(unsigned long)esp_random(),(unsigned long)(esp_random()&65535));
    password=key; prefs.putString("apkey",password);
  }
  WiFi.softAP("Flashforge-Watch",password.c_str());
  apPassword=password;
#ifdef WAVESHARE_LCD_2
  screenStatus(false,0,-1,-1,apPassword,phase);
#endif
  const char *requestHeaders[]={"Accept"};
  web.collectHeaders(requestHeaders,1);
  web.on("/",HTTP_GET,page); web.on("/save",HTTP_POST,saveConfig);
  web.on("/test",HTTP_POST,[](){ beep(250); web.sendHeader("Location","/"); web.send(303); });
  web.on("/last.jpg",HTTP_GET,[](){
    xSemaphoreTake(previewLock,portMAX_DELAY);
    if (previewLen) {
      web.sendHeader("Cache-Control","no-store");
      web.send_P(200,"image/jpeg",(const char*)preview,previewLen);
    } else web.send(404,"text/plain","No current frame");
    xSemaphoreGive(previewLock);
  });
  web.begin();
  if (xTaskCreate([](void*) {
    for (;;) { web.handleClient(); vTaskDelay(1); }
  },"ff-web",8192,nullptr,1,nullptr)!=pdPASS) abort();
}
void loop() {
  static uint32_t lastInfo=0; static bool shown=false;
  if(!shown || millis()-lastInfo>=15000) {
    Serial.printf("AP Flashforge-Watch | password %s | http://192.168.4.1 | %s\n",
                  apPassword.c_str(),phase.c_str());
    shown=true; lastInfo=millis();
  }
  Settings next;
  if (xQueueReceive(pendingSettings,&next,0)==pdTRUE) {
    applySettings(next);
    resetWatch(); failures=0; lastPoll=millis()-PERIOD;
  }
  if(millis()-lastPoll>=PERIOD) {
    pollPrinter();
    if (xSemaphoreTake(previewLock,0)==pdTRUE) {
      webPhase=phase; previewLen=preview ? jpegLen : 0;
      if (previewLen) memcpy(preview,jpeg,previewLen);
      xSemaphoreGive(previewLock);
    }
#ifdef WAVESHARE_LCD_2
    bool showFrame = active && jpegLen>0;
    uint16_t border = (visualAlarm || failures>0) ? ST77XX_RED :
                      (showFrame && caution) ? ST77XX_YELLOW : 0;
    screenStatus(showFrame,border,printPercent,secondsLeft,apPassword,phase);
#endif
    lastPoll=millis();
  }
  delay(1);
}
