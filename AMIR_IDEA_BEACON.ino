
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <BLEDevice.h>

const char* WIFI_NAME="Amir-Message";
const char* BLE_NAME="Amir";
const char* ADMIN_PATH="/a7K9m2Q4x8";
const char* ADMIN_PIN="7391";

IPAddress AP_IP(10,77,0,1), AP_GW(10,77,0,1), AP_MASK(255,255,255,0);

// Most 1.3" 128x64 OLEDs:
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0,U8X8_PIN_NONE);
// SSD1306 alternative:
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0,U8X8_PIN_NONE);

WebServer server(80);
DNSServer dns;
Preferences prefs;

const uint8_t MAX_QUOTES=18, MAX_MSG=20, QSIZE=6;

struct Quote { String text, author; };
struct Msg   { String name, text; };
struct Pending { String name, text; };

Quote quotes[MAX_QUOTES];
Msg msgs[MAX_MSG];
Pending qbuf[QSIZE];

uint8_t quoteCount=0,msgCount=0,currentQuote=0;
uint8_t qHead=0,qTail=0,qCount=0;
uint16_t unread=0;

const char* DQ[]={
"Was mich nicht umbringt, macht mich stärker.",
"Habe Mut, dich deines eigenen Verstandes zu bedienen!",
"Es ist nicht genug zu wissen, man muss auch anwenden.",
"Der Mensch ist nur da ganz Mensch, wo er spielt.",
"Jedem Anfang wohnt ein Zauber inne.",
"Es gibt nichts Gutes, außer man tut es.",
"Du musst dein Leben ändern."
};
const char* DA[]={
"Friedrich Nietzsche","Immanuel Kant","Johann Wolfgang von Goethe",
"Friedrich Schiller","Hermann Hesse","Erich Kästner",
"Rainer Maria Rilke"
};
const uint8_t DCOUNT=sizeof(DQ)/sizeof(DQ[0]);

uint8_t quoteSpeed=42,ideaSpeed=54,contrast=220;
uint16_t minQuoteSec=12;

enum Mode { MODE_QUOTE, MODE_ALERT, MODE_IDEA };
Mode mode=MODE_QUOTE;

int32_t x=128,w=0;
unsigned long lastTick=0,modeStart=0,endPauseAt=0;
bool endPause=false;

String activeName,activeIdea,session;
unsigned long lastGuestPost=0;
const unsigned long POST_COOLDOWN=3500;

// ---------------- helpers ----------------

String clean(String s){
  s.replace("\r"," "); s.replace("\n"," "); s.trim();
  while(s.indexOf("  ")>=0) s.replace("  "," ");
  return s;
}
String esc(String s){
  s.replace("&","&amp;"); s.replace("<","&lt;"); s.replace(">","&gt;");
  s.replace("\"","&quot;"); s.replace("'","&#39;"); return s;
}
String key1(char p,uint8_t i){ char k[5]; snprintf(k,sizeof(k),"%c%02u",p,i); return String(k); }
String key2(const char* p,uint8_t i){ char k[6]; snprintf(k,sizeof(k),"%s%02u",p,i); return String(k); }

String makeToken(){
  char b[17];
  snprintf(b,sizeof(b),"%08lX%08lX",(unsigned long)esp_random(),(unsigned long)esp_random());
  return String(b);
}

void center(const String& s,int y){
  int px=(128-u8g2.getUTF8Width(s.c_str()))/2;
  if(px<0) px=0;
  u8g2.drawUTF8(px,y,s.c_str());
}

// ---------------- storage ----------------

void saveSettings(){
  prefs.putUChar("qs",quoteSpeed);
  prefs.putUChar("is",ideaSpeed);
  prefs.putUShort("qd",minQuoteSec);
  prefs.putUChar("ct",contrast);
}
void loadSettings(){
  quoteSpeed=constrain((int)prefs.getUChar("qs",42),10,100);
  ideaSpeed=constrain((int)prefs.getUChar("is",54),15,110);
  minQuoteSec=constrain((int)prefs.getUShort("qd",12),3,300);
  contrast=constrain((int)prefs.getUChar("ct",220),20,255);
}

void saveQuotes(){
  prefs.putUChar("qc",quoteCount);
  for(uint8_t i=0;i<MAX_QUOTES;i++){
    String qk=key1('q',i), ak=key1('a',i);
    if(i<quoteCount){
      prefs.putString(qk.c_str(),quotes[i].text);
      prefs.putString(ak.c_str(),quotes[i].author);
    } else {
      prefs.remove(qk.c_str()); prefs.remove(ak.c_str());
    }
  }
}
void defaults(){
  quoteCount=min((int)DCOUNT,(int)MAX_QUOTES);
  for(uint8_t i=0;i<quoteCount;i++){ quotes[i].text=DQ[i]; quotes[i].author=DA[i]; }
  saveQuotes();
}
void loadQuotes(){
  quoteCount=prefs.getUChar("qc",0);
  if(!quoteCount || quoteCount>MAX_QUOTES){ defaults(); return; }
  uint8_t v=0;
  for(uint8_t i=0;i<quoteCount;i++){
    String q=prefs.getString(key1('q',i).c_str(),"");
    String a=prefs.getString(key1('a',i).c_str(),"");
    q=clean(q); a=clean(a);
    if(q.length()){ quotes[v].text=q; quotes[v].author=a; v++; }
  }
  quoteCount=v;
  if(!quoteCount) defaults(); else saveQuotes();
}

void saveMsgs(){
  prefs.putUChar("mc",msgCount);
  prefs.putUShort("ur",unread);
  for(uint8_t i=0;i<MAX_MSG;i++){
    String nk=key2("n",i),tk=key2("m",i);
    if(i<msgCount){
      prefs.putString(nk.c_str(),msgs[i].name);
      prefs.putString(tk.c_str(),msgs[i].text);
    } else {
      prefs.remove(nk.c_str()); prefs.remove(tk.c_str());
    }
  }
}
void loadMsgs(){
  msgCount=prefs.getUChar("mc",0);
  unread=prefs.getUShort("ur",0);
  if(msgCount>MAX_MSG) msgCount=0;
  uint8_t v=0;
  for(uint8_t i=0;i<msgCount;i++){
    String n=clean(prefs.getString(key2("n",i).c_str(),""));
    String t=clean(prefs.getString(key2("m",i).c_str(),""));
    if(t.length()){ msgs[v].name=n; msgs[v].text=t; v++; }
  }
  msgCount=v;
}

// ---------------- queue ----------------

void enqueueIdea(const String& n,const String& t){
  if(qCount>=QSIZE) return;
  qbuf[qTail].name=n; qbuf[qTail].text=t;
  qTail=(qTail+1)%QSIZE; qCount++;
}
bool dequeueIdea(Pending& p){
  if(!qCount) return false;
  p=qbuf[qHead];
  qbuf[qHead].name=""; qbuf[qHead].text="";
  qHead=(qHead+1)%QSIZE; qCount--;
  return true;
}

// ---------------- OLED ----------------
//
// IMPORTANT SIZE OPTIMIZATION:
// only TWO U8g2 fonts are linked:
//   u8g2_font_10x20_tf -> large German UTF-8 text
//   u8g2_font_6x12_tf  -> small labels / authors
//
// This saves a lot of flash versus multiple Helvetica fonts.

void prepareQuote(){
  mode=MODE_QUOTE;
  u8g2.setFont(u8g2_font_10x20_tf);
  w=u8g2.getUTF8Width(quotes[currentQuote].text.c_str());
  x=128; lastTick=millis(); modeStart=millis(); endPause=false;
}
void randomQuote(){
  if(quoteCount>1){
    uint8_t n=random(quoteCount);
    while(n==currentQuote) n=random(quoteCount);
    currentQuote=n;
  }
  prepareQuote();
}
void drawQuote(){
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.drawUTF8(x,31,quotes[currentQuote].text.c_str());
  u8g2.drawHLine(3,40,122);
  u8g2.setFont(u8g2_font_6x12_tf);
  String a=quotes[currentQuote].author;
  if(!a.length()) a="Unbekannt";
  center(a,59);
  u8g2.sendBuffer();
}

void nextIdea(){
  Pending p;
  if(!dequeueIdea(p)){ prepareQuote(); drawQuote(); return; }
  activeName=p.name; activeIdea=p.text;
  mode=MODE_ALERT; modeStart=millis(); endPause=false;
}
void beginIdeaScroll(){
  mode=MODE_IDEA;
  u8g2.setFont(u8g2_font_10x20_tf);
  w=u8g2.getUTF8Width(activeIdea.c_str());
  x=128; lastTick=millis(); endPause=false;
}
void drawAlert(){
  u8g2.clearBuffer();
  bool inv=((millis()/220UL)&1);
  if(inv){ u8g2.drawBox(0,0,128,64); u8g2.setDrawColor(0); }
  else u8g2.drawFrame(1,1,126,62);

  u8g2.setFont(u8g2_font_10x20_tf);
  center("HEY!",24);
  u8g2.setFont(u8g2_font_6x12_tf);
  center("NEUE IDEE!",44);
  center(String(unread)+" ungelesen",59);

  u8g2.setDrawColor(1);
  u8g2.sendBuffer();
}
void drawIdea(){
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  String who=activeName.length()?("VON "+activeName):"ANONYM";
  u8g2.drawUTF8(2,11,who.c_str());
  u8g2.drawHLine(0,14,128);

  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.drawUTF8(x,42,activeIdea.c_str());

  u8g2.setFont(u8g2_font_6x12_tf);
  center("NEUE IDEE",61);
  u8g2.sendBuffer();
}

void updateDisplay(){
  unsigned long now=millis();

  if(mode==MODE_ALERT){
    drawAlert();
    if(now-modeStart>=1800){ beginIdeaScroll(); drawIdea(); }
    return;
  }

  if(endPause){
    if(now-endPauseAt<600) return;

    if(mode==MODE_IDEA){
      if(qCount) nextIdea();
      else { prepareQuote(); drawQuote(); }
      return;
    }

    if(now-modeStart >= (unsigned long)minQuoteSec*1000UL && quoteCount>1) randomQuote();
    else { x=128; endPause=false; lastTick=now; }
    return;
  }

  uint8_t speed=(mode==MODE_IDEA)?ideaSpeed:quoteSpeed;
  uint16_t frame=max((uint16_t)9,(uint16_t)(1000UL/speed));
  if(now-lastTick<frame) return;
  lastTick=now; x--;

  if(mode==MODE_IDEA) drawIdea(); else drawQuote();

  if(x<-(w+16)){ endPause=true; endPauseAt=now; }
}

// ---------------- message / quote management ----------------

bool addMessage(String n,String t){
  n=clean(n); t=clean(t);
  if(!t.length() || n.length()>60 || t.length()>360) return false;

  if(msgCount<MAX_MSG){
    msgs[msgCount].name=n; msgs[msgCount].text=t; msgCount++;
  } else {
    for(uint8_t i=1;i<MAX_MSG;i++) msgs[i-1]=msgs[i];
    msgs[MAX_MSG-1].name=n; msgs[MAX_MSG-1].text=t;
  }
  if(unread<999) unread++;
  saveMsgs();
  enqueueIdea(n,t);
  if(mode==MODE_QUOTE) nextIdea();
  return true;
}
void delMsg(uint8_t i){
  if(i>=msgCount) return;
  for(uint8_t j=i+1;j<msgCount;j++) msgs[j-1]=msgs[j];
  msgCount--; saveMsgs();
}
bool addQuote(String q,String a){
  q=clean(q); a=clean(a);
  if(!q.length() || quoteCount>=MAX_QUOTES) return false;
  quotes[quoteCount].text=q; quotes[quoteCount].author=a;
  currentQuote=quoteCount++;
  saveQuotes();
  if(mode==MODE_QUOTE){ prepareQuote(); drawQuote(); }
  return true;
}
void delQuote(uint8_t i){
  if(quoteCount<=1 || i>=quoteCount) return;
  for(uint8_t j=i+1;j<quoteCount;j++) quotes[j-1]=quotes[j];
  quoteCount--;
  if(currentQuote>=quoteCount) currentQuote=0;
  saveQuotes();
  if(mode==MODE_QUOTE) prepareQuote();
}

// ---------------- auth ----------------

bool adminOK(){
  if(!server.hasHeader("Cookie")) return false;
  return server.header("Cookie").indexOf("AS="+session)>=0;
}
void setCookie(){ server.sendHeader("Set-Cookie","AS="+session+"; Path=/; HttpOnly; SameSite=Strict"); }
void goAdmin(){ server.sendHeader("Location",ADMIN_PATH); server.send(303,"text/plain",""); }
bool needAdmin(){ if(adminOK()) return true; server.send(403,"text/plain","Forbidden"); return false; }

// ---------------- compact web UI ----------------
//
// HTML/CSS intentionally compact to save flash.

const char PAGE_TOP[] PROGMEM =
"<!doctype html><html lang=de><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<style>body{margin:0;padding:14px;background:#091321;color:#eef;font:16px Arial}.c{max-width:640px;margin:12px auto;background:#16243a;padding:15px;border-radius:14px}"
"input,textarea,button{box-sizing:border-box;width:100%;padding:11px;margin-top:8px;border:0;border-radius:9px;font:inherit}textarea{min-height:95px}button{background:#2672ff;color:white;font-weight:bold}"
".g{background:#087f5b}.r{background:#b42318}.d{background:#40506a}.m,.q{background:#08111f;padding:9px;margin-top:7px;border-radius:9px}.row{display:grid;grid-template-columns:1fr auto;gap:7px}.row button{width:auto;margin:0}"
".ok{background:#075c45;padding:10px}.warn{background:#7a3e00;padding:10px}small{color:#aab8cd}</style><body>";

String guestPage(const String& note=""){
  String p=FPSTR(PAGE_TOP);
  p+="<div class=c><h1>Hast du eine Idee?</h1><p>Schick Amir deine beste Idee.</p>";
  p+=note;
  p+="<form method=post action=/idea><input name=name maxlength=40 placeholder='Name (optional)'><textarea name=idea maxlength=180 placeholder='Deine Idee...' required></textarea>"
     "<button>Idee an Amir senden</button></form><p><small>Deine Idee erscheint gleich auf dem OLED.</small></p></div></body></html>";
  return p;
}

String loginPage(bool bad=false){
  String p=FPSTR(PAGE_TOP);
  p+="<div class=c><h1>Admin</h1>";
  if(bad) p+="<div class=warn>Falsche PIN.</div>";
  p+="<form method=post action='";
  p+=ADMIN_PATH;
  p+="'><input type=password name=pin inputmode=numeric placeholder='PIN' required><button>Öffnen</button></form></div></body></html>";
  return p;
}

String adminPage(){
  String p=FPSTR(PAGE_TOP);
  p.reserve(11000);

  p+="<div class=c><h1>AMIR IDEA</h1><div class=q><b>";
  p+=esc(quotes[currentQuote].text);
  p+="</b><br><small>— "+esc(quotes[currentQuote].author)+"</small></div>"
     "<form method=post action=/admin/next><button class=g>Nächster</button></form>"
     "</div>";

  p+="<div class=c><h2>Status</h2><p>Sprüche: <b>"+String(quoteCount)+"</b> | Ideen: <b>"+String(msgCount)+"</b> | Ungelesen: <b>"+String(unread)+"</b> | Verbunden: <b>"+String(WiFi.softAPgetStationNum())+"</b></p>"
     "</div>";

  p+="<div class=c><h2>OLED</h2><form method=post action=/admin/settings>"
     "<small>Spruch-Speed</small><input type=range name=qs min=10 max=100 value="+String(quoteSpeed)+">"
     "<small>Ideen-Speed</small><input type=range name=is min=15 max=110 value="+String(ideaSpeed)+">"
     "<small>Sekunden pro Spruch</small><input type=number name=dur min=3 max=300 value="+String(minQuoteSec)+">"
     "<small>Kontrast</small><input type=range name=ct min=20 max=255 value="+String(contrast)+">"
     "<button>Speichern</button></form></div>";

  p+="<div class=c><h2>Spruch +</h2><form method=post action=/admin/addq>"
     "<textarea name=q maxlength=180 placeholder='Spruch...' required></textarea><input name=a maxlength=70 placeholder='Autor'>"
     "<button>Hinzufügen</button></form></div>";

  p+="<div class=c><h2>Sprüche</h2>";
  for(uint8_t i=0;i<quoteCount;i++){
    p+="<div class=q><div class=row><div><b>"+esc(quotes[i].text)+"</b><br><small>— "+esc(quotes[i].author)+"</small></div>";
    if(quoteCount>1){
      p+="<form method=post action=/admin/delq><input type=hidden name=i value="+String(i)+"><button class=r>×</button></form>";
    }
    p+="</div></div>";
  }
  p+="<form method=post action=/admin/reset><button class=d>Reset Sprüche</button></form></div>";

  p+="<div class=c><h2>Ideen</h2>";
  if(!msgCount) p+="<small>Noch keine Ideen.</small>";
  else{
    for(int i=msgCount-1;i>=0;i--){
      String n=msgs[i].name.length()?msgs[i].name:"Anonym";
      p+="<div class=m><div class=row><div><b>"+esc(n)+"</b><br><small>"+esc(msgs[i].text)+"</small></div>"
         "<form method=post action=/admin/delm><input type=hidden name=i value="+String(i)+"><button class=r>×</button></form></div></div>";
    }
    p+="<form method=post action=/admin/clear><button class=r>Ideen löschen</button></form>";
  }
  p+="</div><div class=c><small>Wi-Fi: "+String(WIFI_NAME)+"<br>Gast: http://10.77.0.1/<br>BLE: Amir<br>Admin: "+String(ADMIN_PATH)+"</small>"
     "<form method=post action=/admin/logout><button class=d>Abmelden</button></form></div></body></html>";
  return p;
}

// ---------------- routes ----------------

void handleRoot(){ server.send(200,"text/html; charset=utf-8",guestPage()); }

void handleIdea(){
  unsigned long now=millis();
  if(now-lastGuestPost<POST_COOLDOWN){
    server.send(429,"text/html; charset=utf-8",guestPage("<div class=warn>Bitte kurz warten.</div>")); return;
  }
  String n=server.hasArg("name")?server.arg("name"):"";
  String t=server.hasArg("idea")?server.arg("idea"):"";
  if(!addMessage(n,t)){
    server.send(400,"text/html; charset=utf-8",guestPage("<div class=warn>Eingabe ungültig.</div>")); return;
  }
  lastGuestPost=now;
  server.send(200,"text/html; charset=utf-8",guestPage("<div class=ok>Gespeichert! Schau aufs OLED 👀</div>"));
}

void handleAdmin(){
  if(server.method()==HTTP_GET){
    if(adminOK()){
      if(unread){ unread=0; prefs.putUShort("ur",0); }
      server.send(200,"text/html; charset=utf-8",adminPage());
    } else server.send(200,"text/html; charset=utf-8",loginPage(false));
    return;
  }
  if(server.arg("pin")==ADMIN_PIN){ setCookie(); goAdmin(); }
  else server.send(403,"text/html; charset=utf-8",loginPage(true));
}

void aNext(){ if(!needAdmin())return; randomQuote(); if(mode==MODE_QUOTE)drawQuote(); goAdmin(); }
void aSettings(){
  if(!needAdmin())return;
  if(server.hasArg("qs")) quoteSpeed=constrain(server.arg("qs").toInt(),10,100);
  if(server.hasArg("is")) ideaSpeed=constrain(server.arg("is").toInt(),15,110);
  if(server.hasArg("dur")) minQuoteSec=constrain(server.arg("dur").toInt(),3,300);
  if(server.hasArg("ct")) contrast=constrain(server.arg("ct").toInt(),20,255);
  u8g2.setContrast(contrast); saveSettings(); goAdmin();
}
void aAddQ(){ if(!needAdmin())return; addQuote(server.arg("q"),server.arg("a")); goAdmin(); }
void aDelQ(){ if(!needAdmin())return; delQuote((uint8_t)server.arg("i").toInt()); if(mode==MODE_QUOTE)drawQuote(); goAdmin(); }
void aReset(){ if(!needAdmin())return; defaults(); currentQuote=random(quoteCount); if(mode==MODE_QUOTE){prepareQuote();drawQuote();} goAdmin(); }
void aDelM(){ if(!needAdmin())return; delMsg((uint8_t)server.arg("i").toInt()); goAdmin(); }
void aClear(){ if(!needAdmin())return; msgCount=0;unread=0;saveMsgs();goAdmin(); }
void aLogout(){
  if(!needAdmin())return;
  server.sendHeader("Set-Cookie","AS=x; Path=/; Max-Age=0");
  server.sendHeader("Location",ADMIN_PATH); server.send(303,"text/plain","");
}
void portal(){ server.sendHeader("Location","http://10.77.0.1/"); server.send(302,"text/plain",""); }


// ---------------- robust Wi-Fi AP startup ----------------

bool startWiFiAP(){
  // Start Wi-Fi before BLE. On the ESP32-C3 this gives the AP radio
  // priority during initialization and makes startup more reliable.
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(250);

  WiFi.mode(WIFI_AP);
  delay(250);

  WiFi.softAPConfig(AP_IP,AP_GW,AP_MASK);

  // Open 2.4 GHz AP, channel 1, visible SSID, max 4 clients.
  bool ok=WiFi.softAP(WIFI_NAME,NULL,1,0,4);

  if(!ok){
    WiFi.mode(WIFI_OFF);
    delay(400);
    WiFi.mode(WIFI_AP);
    delay(250);
    WiFi.softAPConfig(AP_IP,AP_GW,AP_MASK);
    ok=WiFi.softAP(WIFI_NAME,NULL,6,0,4);
  }

  if(ok){
    WiFi.setSleep(false);
    delay(300);


    // Brief startup confirmation on OLED so you know the AP really started.
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tf);
    center("WIFI READY",18);
    center("Amir-Message",38);
    center("10.77.0.1",57);
    u8g2.sendBuffer();
    delay(1400);
  } else {

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tf);
    center("WIFI ERROR",26);
    center("RESET BOARD",48);
    u8g2.sendBuffer();
    delay(2500);
  }

  return ok;
}


// ---------------- BLE ----------------

void startBLE(){
  BLEDevice::init(BLE_NAME);
  BLEAdvertising* a=BLEDevice::getAdvertising();
  a->setScanResponse(true);
  a->start();
}

// ---------------- setup / loop ----------------

void setup(){
  delay(250);
  randomSeed(esp_random());

  prefs.begin("amir-lite",false);
  loadSettings(); loadQuotes(); loadMsgs();

  Wire.begin();
  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setContrast(contrast);

  session=makeToken();

  // Start Wi-Fi first. This is more reliable on XIAO ESP32-C3.
  bool wifiOK=startWiFiAP();

  // BLE starts only after the Wi-Fi AP is already alive.
  startBLE();

  if(wifiOK){
    dns.start(53,"*",AP_IP);
  }

  const char* hdr[]={"Cookie"};
  server.collectHeaders(hdr,1);

  server.on("/",HTTP_GET,handleRoot);
  server.on("/idea",HTTP_POST,handleIdea);

  server.on(ADMIN_PATH,HTTP_GET,handleAdmin);
  server.on(ADMIN_PATH,HTTP_POST,handleAdmin);

  server.on("/admin/next",HTTP_POST,aNext);
  server.on("/admin/settings",HTTP_POST,aSettings);
  server.on("/admin/addq",HTTP_POST,aAddQ);
  server.on("/admin/delq",HTTP_POST,aDelQ);
  server.on("/admin/reset",HTTP_POST,aReset);
  server.on("/admin/delm",HTTP_POST,aDelM);
  server.on("/admin/clear",HTTP_POST,aClear);
  server.on("/admin/logout",HTTP_POST,aLogout);

  server.on("/generate_204",HTTP_ANY,portal);
  server.on("/hotspot-detect.html",HTTP_ANY,portal);
  server.onNotFound(portal);

  server.begin();

  currentQuote=random(quoteCount);
  prepareQuote();
  drawQuote();

}

void loop(){
  dns.processNextRequest();
  server.handleClient();
  updateDisplay();
  delay(1);
}
