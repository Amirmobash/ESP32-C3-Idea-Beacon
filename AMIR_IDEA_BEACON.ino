#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <BLEDevice.h>

// ============================================================================
// Amir Mobasheraghdam — ESP32-C3 Idea Beacon
// Target: Seeed Studio XIAO ESP32-C3 + 1.3" SH1106 128x64 OLED
// ============================================================================

namespace Config {
constexpr char WIFI_NAME[] = "Amir-Message";
constexpr char BLE_NAME[] = "Amir";
constexpr char ADMIN_PATH[] = "/admin";

constexpr uint8_t MAX_QUOTES = 18;
constexpr uint8_t MAX_MESSAGES = 20;
constexpr uint8_t IDEA_QUEUE_SIZE = 6;

constexpr uint16_t GUEST_NAME_MAX = 40;
constexpr uint16_t GUEST_IDEA_MAX = 180;
constexpr uint16_t QUOTE_TEXT_MAX = 180;
constexpr uint16_t QUOTE_AUTHOR_MAX = 70;

constexpr uint32_t POST_COOLDOWN_MS = 3500;
constexpr uint32_t LOGIN_COOLDOWN_MS = 1500;
constexpr uint8_t MAX_LOGIN_FAILURES = 5;
constexpr uint32_t LOGIN_LOCKOUT_MS = 30000;
constexpr uint32_t ALERT_DURATION_MS = 1800;
constexpr uint32_t END_PAUSE_MS = 600;
}

IPAddress AP_IP(10, 77, 0, 1);
IPAddress AP_GW(10, 77, 0, 1);
IPAddress AP_MASK(255, 255, 255, 0);

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
// For SSD1306 displays, replace the line above with:
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

WebServer server(80);
DNSServer dns;
Preferences prefs;

struct Quote {
  String text;
  String author;
};

struct Message {
  String name;
  String text;
};

struct PendingIdea {
  String name;
  String text;
};

Quote quotes[Config::MAX_QUOTES];
Message messages[Config::MAX_MESSAGES];
PendingIdea ideaQueue[Config::IDEA_QUEUE_SIZE];

uint8_t quoteCount = 0;
uint8_t messageCount = 0;
uint8_t currentQuote = 0;

uint8_t queueHead = 0;
uint8_t queueTail = 0;
uint8_t queueCount = 0;

uint16_t unread = 0;

const char* DEFAULT_QUOTES[] = {
  "Was mich nicht umbringt, macht mich stärker.",
  "Habe Mut, dich deines eigenen Verstandes zu bedienen!",
  "Es ist nicht genug zu wissen, man muss auch anwenden.",
  "Der Mensch ist nur da ganz Mensch, wo er spielt.",
  "Jedem Anfang wohnt ein Zauber inne.",
  "Es gibt nichts Gutes, außer man tut es.",
  "Du musst dein Leben ändern."
};

const char* DEFAULT_AUTHORS[] = {
  "Friedrich Nietzsche",
  "Immanuel Kant",
  "Johann Wolfgang von Goethe",
  "Friedrich Schiller",
  "Hermann Hesse",
  "Erich Kästner",
  "Rainer Maria Rilke"
};

constexpr uint8_t DEFAULT_QUOTE_COUNT =
    sizeof(DEFAULT_QUOTES) / sizeof(DEFAULT_QUOTES[0]);

uint8_t quoteSpeed = 42;
uint8_t ideaSpeed = 54;
uint8_t contrast = 220;
uint16_t minQuoteSeconds = 12;

enum DisplayMode {
  MODE_QUOTE,
  MODE_ALERT,
  MODE_IDEA
};

DisplayMode mode = MODE_QUOTE;

int32_t scrollX = 128;
int32_t textWidth = 0;
uint32_t lastTick = 0;
uint32_t modeStart = 0;
uint32_t endPauseAt = 0;
bool endPause = false;

String activeName;
String activeIdea;

String sessionToken;
String adminPin;

uint32_t lastGuestPost = 0;
uint32_t lastLoginAttempt = 0;
uint32_t loginLockoutUntil = 0;
uint8_t loginFailures = 0;

bool dnsStarted = false;

// ---------------- helpers ----------------

String cleanText(String value) {
  value.replace("\r", " ");
  value.replace("\n", " ");
  value.trim();

  while (value.indexOf("  ") >= 0) {
    value.replace("  ", " ");
  }
  return value;
}

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}

String prefKey(char prefix, uint8_t index) {
  char key[5];
  snprintf(key, sizeof(key), "%c%02u", prefix, index);
  return String(key);
}

String makeToken() {
  char buffer[33];
  snprintf(
      buffer,
      sizeof(buffer),
      "%08lX%08lX%08lX%08lX",
      static_cast<unsigned long>(esp_random()),
      static_cast<unsigned long>(esp_random()),
      static_cast<unsigned long>(esp_random()),
      static_cast<unsigned long>(esp_random()));
  return String(buffer);
}

String generateAdminPin() {
  uint32_t value = 100000 + (esp_random() % 900000);
  return String(value);
}

void drawCentered(const String& text, int y) {
  int x = (128 - u8g2.getUTF8Width(text.c_str())) / 2;
  if (x < 0) {
    x = 0;
  }
  u8g2.drawUTF8(x, y, text.c_str());
}

bool elapsed(uint32_t now, uint32_t start, uint32_t duration) {
  return static_cast<uint32_t>(now - start) >= duration;
}

bool validIndexArg(const String& arg, uint8_t upperExclusive, uint8_t& out) {
  if (!arg.length()) {
    return false;
  }

  for (size_t i = 0; i < arg.length(); ++i) {
    if (!isDigit(arg[i])) {
      return false;
    }
  }

  long value = arg.toInt();
  if (value < 0 || value >= upperExclusive) {
    return false;
  }

  out = static_cast<uint8_t>(value);
  return true;
}

// ---------------- storage ----------------

void saveSettings() {
  prefs.putUChar("qs", quoteSpeed);
  prefs.putUChar("is", ideaSpeed);
  prefs.putUShort("qd", minQuoteSeconds);
  prefs.putUChar("ct", contrast);
}

void loadSettings() {
  quoteSpeed = constrain(static_cast<int>(prefs.getUChar("qs", 42)), 10, 100);
  ideaSpeed = constrain(static_cast<int>(prefs.getUChar("is", 54)), 15, 110);
  minQuoteSeconds =
      constrain(static_cast<int>(prefs.getUShort("qd", 12)), 3, 300);
  contrast =
      constrain(static_cast<int>(prefs.getUChar("ct", 220)), 20, 255);
}

void saveQuotes() {
  prefs.putUChar("qc", quoteCount);

  for (uint8_t i = 0; i < Config::MAX_QUOTES; ++i) {
    String qKey = prefKey('q', i);
    String aKey = prefKey('a', i);

    if (i < quoteCount) {
      prefs.putString(qKey.c_str(), quotes[i].text);
      prefs.putString(aKey.c_str(), quotes[i].author);
    } else {
      prefs.remove(qKey.c_str());
      prefs.remove(aKey.c_str());
    }
  }
}

void restoreDefaultQuotes() {
  quoteCount = min(
      static_cast<int>(DEFAULT_QUOTE_COUNT),
      static_cast<int>(Config::MAX_QUOTES));

  for (uint8_t i = 0; i < quoteCount; ++i) {
    quotes[i].text = DEFAULT_QUOTES[i];
    quotes[i].author = DEFAULT_AUTHORS[i];
  }

  currentQuote = 0;
  saveQuotes();
}

void loadQuotes() {
  quoteCount = prefs.getUChar("qc", 0);

  if (quoteCount == 0 || quoteCount > Config::MAX_QUOTES) {
    restoreDefaultQuotes();
    return;
  }

  uint8_t validCount = 0;

  for (uint8_t i = 0; i < quoteCount; ++i) {
    String quote = cleanText(prefs.getString(prefKey('q', i).c_str(), ""));
    String author = cleanText(prefs.getString(prefKey('a', i).c_str(), ""));

    if (quote.length()) {
      quotes[validCount].text = quote;
      quotes[validCount].author = author;
      ++validCount;
    }
  }

  quoteCount = validCount;

  if (quoteCount == 0) {
    restoreDefaultQuotes();
  } else {
    saveQuotes();
  }
}

void saveMessages() {
  prefs.putUChar("mc", messageCount);
  prefs.putUShort("ur", unread);

  for (uint8_t i = 0; i < Config::MAX_MESSAGES; ++i) {
    String nKey = prefKey('n', i);
    String mKey = prefKey('m', i);

    if (i < messageCount) {
      prefs.putString(nKey.c_str(), messages[i].name);
      prefs.putString(mKey.c_str(), messages[i].text);
    } else {
      prefs.remove(nKey.c_str());
      prefs.remove(mKey.c_str());
    }
  }
}

void loadMessages() {
  messageCount = prefs.getUChar("mc", 0);
  unread = prefs.getUShort("ur", 0);

  if (messageCount > Config::MAX_MESSAGES) {
    messageCount = 0;
  }

  uint8_t validCount = 0;

  for (uint8_t i = 0; i < messageCount; ++i) {
    String name = cleanText(prefs.getString(prefKey('n', i).c_str(), ""));
    String text = cleanText(prefs.getString(prefKey('m', i).c_str(), ""));

    if (text.length()) {
      messages[validCount].name = name;
      messages[validCount].text = text;
      ++validCount;
    }
  }

  messageCount = validCount;
}

void loadOrCreateAdminPin() {
  adminPin = prefs.getString("apin", "");

  bool valid = adminPin.length() == 6;
  for (size_t i = 0; valid && i < adminPin.length(); ++i) {
    valid = isDigit(adminPin[i]);
  }

  if (!valid) {
    adminPin = generateAdminPin();
    prefs.putString("apin", adminPin);
  }
}

// ---------------- queue ----------------

void enqueueIdea(const String& name, const String& text) {
  // If the queue is full, drop the oldest pending display item.
  // The message still remains stored in the admin dashboard.
  if (queueCount >= Config::IDEA_QUEUE_SIZE) {
    ideaQueue[queueHead].name = "";
    ideaQueue[queueHead].text = "";
    queueHead = (queueHead + 1) % Config::IDEA_QUEUE_SIZE;
    --queueCount;
  }

  ideaQueue[queueTail].name = name;
  ideaQueue[queueTail].text = text;
  queueTail = (queueTail + 1) % Config::IDEA_QUEUE_SIZE;
  ++queueCount;
}

bool dequeueIdea(PendingIdea& pending) {
  if (queueCount == 0) {
    return false;
  }

  pending = ideaQueue[queueHead];
  ideaQueue[queueHead].name = "";
  ideaQueue[queueHead].text = "";
  queueHead = (queueHead + 1) % Config::IDEA_QUEUE_SIZE;
  --queueCount;
  return true;
}

// ---------------- OLED ----------------

void prepareQuote() {
  if (quoteCount == 0) {
    restoreDefaultQuotes();
  }

  mode = MODE_QUOTE;
  u8g2.setFont(u8g2_font_10x20_tf);
  textWidth = u8g2.getUTF8Width(quotes[currentQuote].text.c_str());
  scrollX = 128;
  lastTick = millis();
  modeStart = millis();
  endPause = false;
}

void randomQuote() {
  if (quoteCount > 1) {
    uint8_t next = random(quoteCount);
    while (next == currentQuote) {
      next = random(quoteCount);
    }
    currentQuote = next;
  }

  prepareQuote();
}

void drawQuote() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.drawUTF8(scrollX, 31, quotes[currentQuote].text.c_str());
  u8g2.drawHLine(3, 40, 122);

  u8g2.setFont(u8g2_font_6x12_tf);
  String author = quotes[currentQuote].author;
  if (!author.length()) {
    author = "Unbekannt";
  }
  drawCentered(author, 59);

  u8g2.sendBuffer();
}

void nextIdea() {
  PendingIdea pending;

  if (!dequeueIdea(pending)) {
    prepareQuote();
    drawQuote();
    return;
  }

  activeName = pending.name;
  activeIdea = pending.text;
  mode = MODE_ALERT;
  modeStart = millis();
  endPause = false;
}

void beginIdeaScroll() {
  mode = MODE_IDEA;
  u8g2.setFont(u8g2_font_10x20_tf);
  textWidth = u8g2.getUTF8Width(activeIdea.c_str());
  scrollX = 128;
  lastTick = millis();
  endPause = false;
}

void drawAlert() {
  u8g2.clearBuffer();

  bool inverted = ((millis() / 220UL) & 1U) != 0;
  if (inverted) {
    u8g2.drawBox(0, 0, 128, 64);
    u8g2.setDrawColor(0);
  } else {
    u8g2.drawFrame(1, 1, 126, 62);
  }

  u8g2.setFont(u8g2_font_10x20_tf);
  drawCentered("HEY!", 24);

  u8g2.setFont(u8g2_font_6x12_tf);
  drawCentered("NEUE IDEE!", 44);
  drawCentered(String(unread) + " ungelesen", 59);

  u8g2.setDrawColor(1);
  u8g2.sendBuffer();
}

void drawIdea() {
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_6x12_tf);
  String sender = activeName.length() ? ("VON " + activeName) : "ANONYM";
  u8g2.drawUTF8(2, 11, sender.c_str());
  u8g2.drawHLine(0, 14, 128);

  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.drawUTF8(scrollX, 42, activeIdea.c_str());

  u8g2.setFont(u8g2_font_6x12_tf);
  drawCentered("NEUE IDEE", 61);

  u8g2.sendBuffer();
}

void updateDisplay() {
  uint32_t now = millis();

  if (mode == MODE_ALERT) {
    drawAlert();

    if (elapsed(now, modeStart, Config::ALERT_DURATION_MS)) {
      beginIdeaScroll();
      drawIdea();
    }
    return;
  }

  if (endPause) {
    if (!elapsed(now, endPauseAt, Config::END_PAUSE_MS)) {
      return;
    }

    if (mode == MODE_IDEA) {
      if (queueCount > 0) {
        nextIdea();
      } else {
        prepareQuote();
        drawQuote();
      }
      return;
    }

    if (elapsed(
            now,
            modeStart,
            static_cast<uint32_t>(minQuoteSeconds) * 1000UL) &&
        quoteCount > 1) {
      randomQuote();
    } else {
      scrollX = 128;
      endPause = false;
      lastTick = now;
    }
    return;
  }

  uint8_t speed = (mode == MODE_IDEA) ? ideaSpeed : quoteSpeed;
  uint16_t frameMs =
      max(static_cast<uint16_t>(9), static_cast<uint16_t>(1000UL / speed));

  if (!elapsed(now, lastTick, frameMs)) {
    return;
  }

  lastTick = now;
  --scrollX;

  if (mode == MODE_IDEA) {
    drawIdea();
  } else {
    drawQuote();
  }

  if (scrollX < -(textWidth + 16)) {
    endPause = true;
    endPauseAt = now;
  }
}

// ---------------- content management ----------------

bool addMessage(String name, String text) {
  name = cleanText(name);
  text = cleanText(text);

  if (!text.length() ||
      name.length() > Config::GUEST_NAME_MAX ||
      text.length() > Config::GUEST_IDEA_MAX) {
    return false;
  }

  if (messageCount < Config::MAX_MESSAGES) {
    messages[messageCount].name = name;
    messages[messageCount].text = text;
    ++messageCount;
  } else {
    for (uint8_t i = 1; i < Config::MAX_MESSAGES; ++i) {
      messages[i - 1] = messages[i];
    }
    messages[Config::MAX_MESSAGES - 1].name = name;
    messages[Config::MAX_MESSAGES - 1].text = text;
  }

  if (unread < 999) {
    ++unread;
  }

  saveMessages();
  enqueueIdea(name, text);

  if (mode == MODE_QUOTE) {
    nextIdea();
  }

  return true;
}

void deleteMessage(uint8_t index) {
  if (index >= messageCount) {
    return;
  }

  for (uint8_t i = index + 1; i < messageCount; ++i) {
    messages[i - 1] = messages[i];
  }

  --messageCount;
  saveMessages();
}

bool addQuote(String quote, String author) {
  quote = cleanText(quote);
  author = cleanText(author);

  if (!quote.length() ||
      quote.length() > Config::QUOTE_TEXT_MAX ||
      author.length() > Config::QUOTE_AUTHOR_MAX ||
      quoteCount >= Config::MAX_QUOTES) {
    return false;
  }

  quotes[quoteCount].text = quote;
  quotes[quoteCount].author = author;
  currentQuote = quoteCount;
  ++quoteCount;

  saveQuotes();

  if (mode == MODE_QUOTE) {
    prepareQuote();
    drawQuote();
  }

  return true;
}

void deleteQuote(uint8_t index) {
  if (quoteCount <= 1 || index >= quoteCount) {
    return;
  }

  for (uint8_t i = index + 1; i < quoteCount; ++i) {
    quotes[i - 1] = quotes[i];
  }

  --quoteCount;

  if (currentQuote >= quoteCount) {
    currentQuote = 0;
  }

  saveQuotes();

  if (mode == MODE_QUOTE) {
    prepareQuote();
  }
}

// ---------------- authentication ----------------

String cookieValue(const String& cookieHeader, const String& key) {
  String marker = key + "=";
  int start = cookieHeader.indexOf(marker);

  while (start >= 0) {
    bool boundaryBefore =
        start == 0 || cookieHeader[start - 1] == ' ' || cookieHeader[start - 1] == ';';

    if (boundaryBefore) {
      int valueStart = start + marker.length();
      int end = cookieHeader.indexOf(';', valueStart);
      if (end < 0) {
        end = cookieHeader.length();
      }
      String value = cookieHeader.substring(valueStart, end);
      value.trim();
      return value;
    }

    start = cookieHeader.indexOf(marker, start + 1);
  }

  return "";
}

bool adminAuthenticated() {
  if (!server.hasHeader("Cookie")) {
    return false;
  }

  String value = cookieValue(server.header("Cookie"), "AS");
  return value.length() && value == sessionToken;
}

bool csrfValid() {
  return server.hasArg("csrf") && server.arg("csrf") == sessionToken;
}

bool requireAdmin(bool requireCsrf = true) {
  if (!adminAuthenticated()) {
    server.send(403, "text/plain; charset=utf-8", "Forbidden");
    return false;
  }

  if (requireCsrf && !csrfValid()) {
    server.send(403, "text/plain; charset=utf-8", "CSRF check failed");
    return false;
  }

  return true;
}

void setSessionCookie() {
  server.sendHeader(
      "Set-Cookie",
      "AS=" + sessionToken + "; Path=/; HttpOnly; SameSite=Strict");
}

void redirectAdmin() {
  server.sendHeader("Location", Config::ADMIN_PATH);
  server.send(303, "text/plain", "");
}

String csrfInput() {
  return "<input type=hidden name=csrf value='" + sessionToken + "'>";
}

// ---------------- web UI ----------------

const char PAGE_TOP[] PROGMEM =
    "<!doctype html><html lang=de><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{margin:0;padding:14px;background:#091321;color:#eef;font:16px Arial}"
    ".c{max-width:640px;margin:12px auto;background:#16243a;padding:15px;border-radius:14px}"
    "input,textarea,button{box-sizing:border-box;width:100%;padding:11px;margin-top:8px;border:0;border-radius:9px;font:inherit}"
    "textarea{min-height:95px}"
    "button{background:#2672ff;color:white;font-weight:bold}"
    ".g{background:#087f5b}.r{background:#b42318}.d{background:#40506a}"
    ".m,.q{background:#08111f;padding:9px;margin-top:7px;border-radius:9px}"
    ".row{display:grid;grid-template-columns:1fr auto;gap:7px}.row button{width:auto;margin:0}"
    ".ok{background:#075c45;padding:10px}.warn{background:#7a3e00;padding:10px}"
    "small{color:#aab8cd}</style><body>";

String guestPage(const String& notice = "") {
  String page = FPSTR(PAGE_TOP);
  page.reserve(1800);

  page += "<div class=c><h1>Hast du eine Idee?</h1>"
          "<p>Schick Amir deine beste Idee.</p>";
  page += notice;
  page +=
      "<form method=post action=/idea>"
      "<input name=name maxlength=40 placeholder='Name (optional)'>"
      "<textarea name=idea maxlength=180 placeholder='Deine Idee...' required></textarea>"
      "<button>Idee an Amir senden</button>"
      "</form>"
      "<p><small>Deine Idee erscheint gleich auf dem OLED.</small></p>"
      "</div></body></html>";

  return page;
}

String loginPage(const String& notice = "") {
  String page = FPSTR(PAGE_TOP);
  page.reserve(1200);

  page += "<div class=c><h1>Admin</h1>";
  page += notice;
  page += "<form method=post action='";
  page += Config::ADMIN_PATH;
  page +=
      "'><input type=password name=pin inputmode=numeric maxlength=6 "
      "placeholder='6-stellige PIN' required>"
      "<button>Öffnen</button></form>"
      "</div></body></html>";

  return page;
}

String adminPage() {
  String page = FPSTR(PAGE_TOP);
  page.reserve(12000);

  page += "<div class=c><h1>AMIR IDEA</h1><div class=q><b>";
  page += htmlEscape(quotes[currentQuote].text);
  page += "</b><br><small>— ";
  page += htmlEscape(quotes[currentQuote].author);
  page += "</small></div>";

  page += "<form method=post action=/admin/next>";
  page += csrfInput();
  page += "<button class=g>Nächster</button></form></div>";

  page += "<div class=c><h2>Status</h2><p>Sprüche: <b>";
  page += String(quoteCount);
  page += "</b> | Ideen: <b>";
  page += String(messageCount);
  page += "</b> | Ungelesen: <b>";
  page += String(unread);
  page += "</b> | Verbunden: <b>";
  page += String(WiFi.softAPgetStationNum());
  page += "</b></p></div>";

  page +=
      "<div class=c><h2>OLED</h2>"
      "<form method=post action=/admin/settings>";
  page += csrfInput();
  page += "<small>Spruch-Speed</small><input type=range name=qs min=10 max=100 value=";
  page += String(quoteSpeed);
  page += "><small>Ideen-Speed</small><input type=range name=is min=15 max=110 value=";
  page += String(ideaSpeed);
  page += "><small>Sekunden pro Spruch</small><input type=number name=dur min=3 max=300 value=";
  page += String(minQuoteSeconds);
  page += "><small>Kontrast</small><input type=range name=ct min=20 max=255 value=";
  page += String(contrast);
  page += "><button>Speichern</button></form></div>";

  page +=
      "<div class=c><h2>Spruch +</h2>"
      "<form method=post action=/admin/addq>";
  page += csrfInput();
  page +=
      "<textarea name=q maxlength=180 placeholder='Spruch...' required></textarea>"
      "<input name=a maxlength=70 placeholder='Autor'>"
      "<button>Hinzufügen</button></form></div>";

  page += "<div class=c><h2>Sprüche</h2>";

  for (uint8_t i = 0; i < quoteCount; ++i) {
    page += "<div class=q><div class=row><div><b>";
    page += htmlEscape(quotes[i].text);
    page += "</b><br><small>— ";
    page += htmlEscape(quotes[i].author);
    page += "</small></div>";

    if (quoteCount > 1) {
      page += "<form method=post action=/admin/delq>";
      page += csrfInput();
      page += "<input type=hidden name=i value=";
      page += String(i);
      page += "><button class=r>×</button></form>";
    }

    page += "</div></div>";
  }

  page += "<form method=post action=/admin/reset>";
  page += csrfInput();
  page += "<button class=d>Reset Sprüche</button></form></div>";

  page += "<div class=c><h2>Ideen</h2>";

  if (messageCount == 0) {
    page += "<small>Noch keine Ideen.</small>";
  } else {
    for (int i = messageCount - 1; i >= 0; --i) {
      String name = messages[i].name.length() ? messages[i].name : "Anonym";

      page += "<div class=m><div class=row><div><b>";
      page += htmlEscape(name);
      page += "</b><br><small>";
      page += htmlEscape(messages[i].text);
      page += "</small></div><form method=post action=/admin/delm>";
      page += csrfInput();
      page += "<input type=hidden name=i value=";
      page += String(i);
      page += "><button class=r>×</button></form></div></div>";
    }

    page += "<form method=post action=/admin/clear>";
    page += csrfInput();
    page += "<button class=r>Ideen löschen</button></form>";
  }

  page += "</div>";

  page +=
      "<div class=c><h2>Sicherheit</h2>"
      "<p><small>Die Admin-PIN wird im ESP32 gespeichert und steht nicht im öffentlichen Quellcode.</small></p>"
      "<form method=post action=/admin/newpin>";
  page += csrfInput();
  page +=
      "<input type=password name=newpin inputmode=numeric minlength=6 maxlength=6 "
      "placeholder='Neue 6-stellige PIN' required>"
      "<button>PIN ändern</button></form></div>";

  page += "<div class=c><small>Wi-Fi: ";
  page += Config::WIFI_NAME;
  page += "<br>Gast: http://10.77.0.1/<br>BLE: ";
  page += Config::BLE_NAME;
  page += "<br>Admin: http://10.77.0.1";
  page += Config::ADMIN_PATH;
  page += "</small><form method=post action=/admin/logout>";
  page += csrfInput();
  page += "<button class=d>Abmelden</button></form></div></body></html>";

  return page;
}

// ---------------- routes ----------------

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", guestPage());
}

void handleIdea() {
  uint32_t now = millis();

  if (lastGuestPost != 0 &&
      !elapsed(now, lastGuestPost, Config::POST_COOLDOWN_MS)) {
    server.send(
        429,
        "text/html; charset=utf-8",
        guestPage("<div class=warn>Bitte kurz warten.</div>"));
    return;
  }

  String name = server.hasArg("name") ? server.arg("name") : "";
  String idea = server.hasArg("idea") ? server.arg("idea") : "";

  if (!addMessage(name, idea)) {
    server.send(
        400,
        "text/html; charset=utf-8",
        guestPage("<div class=warn>Eingabe ungültig.</div>"));
    return;
  }

  lastGuestPost = now;

  server.send(
      200,
      "text/html; charset=utf-8",
      guestPage("<div class=ok>Gespeichert! Schau aufs OLED 👀</div>"));
}

void handleAdmin() {
  if (server.method() == HTTP_GET) {
    if (adminAuthenticated()) {
      if (unread > 0) {
        unread = 0;
        prefs.putUShort("ur", 0);
      }
      server.send(200, "text/html; charset=utf-8", adminPage());
    } else {
      server.send(200, "text/html; charset=utf-8", loginPage());
    }
    return;
  }

  uint32_t now = millis();

  if (loginLockoutUntil != 0) {
    // Signed subtraction is wrap-safe for intervals well below 2^31 ms.
    if (static_cast<int32_t>(now - loginLockoutUntil) < 0) {
      server.send(
          429,
          "text/html; charset=utf-8",
          loginPage("<div class=warn>Zu viele Versuche. Bitte kurz warten.</div>"));
      return;
    }
    loginLockoutUntil = 0;
  }

  if (lastLoginAttempt != 0 &&
      !elapsed(now, lastLoginAttempt, Config::LOGIN_COOLDOWN_MS)) {
    server.send(
        429,
        "text/html; charset=utf-8",
        loginPage("<div class=warn>Bitte kurz warten.</div>"));
    return;
  }

  lastLoginAttempt = now;

  if (server.hasArg("pin") && server.arg("pin") == adminPin) {
    loginFailures = 0;
    loginLockoutUntil = 0;
    sessionToken = makeToken();
    setSessionCookie();
    redirectAdmin();
    return;
  }

  ++loginFailures;

  if (loginFailures >= Config::MAX_LOGIN_FAILURES) {
    loginFailures = 0;
    loginLockoutUntil = now + Config::LOGIN_LOCKOUT_MS;
  }

  server.send(
      403,
      "text/html; charset=utf-8",
      loginPage("<div class=warn>Falsche PIN.</div>"));
}

void adminNext() {
  if (!requireAdmin()) return;
  randomQuote();
  if (mode == MODE_QUOTE) drawQuote();
  redirectAdmin();
}

void adminSettings() {
  if (!requireAdmin()) return;

  if (server.hasArg("qs")) {
    quoteSpeed = constrain(server.arg("qs").toInt(), 10, 100);
  }
  if (server.hasArg("is")) {
    ideaSpeed = constrain(server.arg("is").toInt(), 15, 110);
  }
  if (server.hasArg("dur")) {
    minQuoteSeconds = constrain(server.arg("dur").toInt(), 3, 300);
  }
  if (server.hasArg("ct")) {
    contrast = constrain(server.arg("ct").toInt(), 20, 255);
  }

  u8g2.setContrast(contrast);
  saveSettings();
  redirectAdmin();
}

void adminAddQuote() {
  if (!requireAdmin()) return;
  addQuote(server.arg("q"), server.arg("a"));
  redirectAdmin();
}

void adminDeleteQuote() {
  if (!requireAdmin()) return;

  uint8_t index;
  if (!validIndexArg(server.arg("i"), quoteCount, index)) {
    server.send(400, "text/plain; charset=utf-8", "Invalid index");
    return;
  }

  deleteQuote(index);

  if (mode == MODE_QUOTE) {
    drawQuote();
  }

  redirectAdmin();
}

void adminResetQuotes() {
  if (!requireAdmin()) return;

  restoreDefaultQuotes();
  currentQuote = random(quoteCount);

  if (mode == MODE_QUOTE) {
    prepareQuote();
    drawQuote();
  }

  redirectAdmin();
}

void adminDeleteMessage() {
  if (!requireAdmin()) return;

  uint8_t index;
  if (!validIndexArg(server.arg("i"), messageCount, index)) {
    server.send(400, "text/plain; charset=utf-8", "Invalid index");
    return;
  }

  deleteMessage(index);
  redirectAdmin();
}

void adminClearMessages() {
  if (!requireAdmin()) return;

  messageCount = 0;
  unread = 0;
  saveMessages();
  redirectAdmin();
}

void adminNewPin() {
  if (!requireAdmin()) return;

  String newPin = server.arg("newpin");
  bool valid = newPin.length() == 6;

  for (size_t i = 0; valid && i < newPin.length(); ++i) {
    valid = isDigit(newPin[i]);
  }

  if (!valid) {
    server.send(400, "text/plain; charset=utf-8", "PIN must be exactly 6 digits");
    return;
  }

  adminPin = newPin;
  prefs.putString("apin", adminPin);

  // Invalidate all existing sessions after a PIN change.
  sessionToken = makeToken();
  setSessionCookie();
  redirectAdmin();
}

void adminLogout() {
  if (!requireAdmin()) return;

  sessionToken = makeToken();
  server.sendHeader("Set-Cookie", "AS=x; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  server.sendHeader("Location", Config::ADMIN_PATH);
  server.send(303, "text/plain", "");
}

void captivePortalRedirect() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Location", "http://10.77.0.1/");
  server.send(302, "text/plain", "");
}

// ---------------- Wi-Fi AP ----------------

bool startWiFiAP() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(250);

  WiFi.mode(WIFI_AP);
  delay(250);

  if (!WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK)) {
    return false;
  }

  bool ok = WiFi.softAP(Config::WIFI_NAME, nullptr, 1, 0, 4);

  if (!ok) {
    WiFi.mode(WIFI_OFF);
    delay(400);
    WiFi.mode(WIFI_AP);
    delay(250);

    if (!WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK)) {
      return false;
    }

    ok = WiFi.softAP(Config::WIFI_NAME, nullptr, 6, 0, 4);
  }

  if (ok) {
    WiFi.setSleep(false);
    delay(300);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tf);
    drawCentered("WIFI READY", 18);
    drawCentered(Config::WIFI_NAME, 38);
    drawCentered("10.77.0.1", 57);
    u8g2.sendBuffer();
    delay(1200);
  } else {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tf);
    drawCentered("WIFI ERROR", 26);
    drawCentered("RESET BOARD", 48);
    u8g2.sendBuffer();
    delay(2500);
  }

  return ok;
}

// ---------------- BLE ----------------

void startBLE() {
  BLEDevice::init(Config::BLE_NAME);

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setScanResponse(true);
  advertising->start();
}

// ---------------- first-run PIN ----------------

void showAdminPinOnceIfNeeded() {
  bool shownBefore = prefs.getBool("pinseen", false);

  if (shownBefore) {
    return;
  }

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  drawCentered("ADMIN PIN", 14);

  u8g2.setFont(u8g2_font_10x20_tf);
  drawCentered(adminPin, 39);

  u8g2.setFont(u8g2_font_6x12_tf);
  drawCentered("save it now", 58);
  u8g2.sendBuffer();

  delay(12000);
  prefs.putBool("pinseen", true);
}

// ---------------- setup / loop ----------------

void setup() {
  delay(250);
  randomSeed(esp_random());

  prefs.begin("amir-beacon", false);

  loadSettings();
  loadQuotes();
  loadMessages();
  loadOrCreateAdminPin();

  Wire.begin();

  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setContrast(contrast);

  showAdminPinOnceIfNeeded();

  sessionToken = makeToken();

  bool wifiReady = startWiFiAP();

  // Start BLE only after Wi-Fi initialization for more reliable coexistence.
  startBLE();

  if (wifiReady) {
    dnsStarted = dns.start(53, "*", AP_IP);
  }

  const char* headers[] = {"Cookie"};
  server.collectHeaders(headers, 1);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/idea", HTTP_POST, handleIdea);

  server.on(Config::ADMIN_PATH, HTTP_GET, handleAdmin);
  server.on(Config::ADMIN_PATH, HTTP_POST, handleAdmin);

  server.on("/admin/next", HTTP_POST, adminNext);
  server.on("/admin/settings", HTTP_POST, adminSettings);
  server.on("/admin/addq", HTTP_POST, adminAddQuote);
  server.on("/admin/delq", HTTP_POST, adminDeleteQuote);
  server.on("/admin/reset", HTTP_POST, adminResetQuotes);
  server.on("/admin/delm", HTTP_POST, adminDeleteMessage);
  server.on("/admin/clear", HTTP_POST, adminClearMessages);
  server.on("/admin/newpin", HTTP_POST, adminNewPin);
  server.on("/admin/logout", HTTP_POST, adminLogout);

  // Common captive-portal probes.
  server.on("/generate_204", HTTP_ANY, captivePortalRedirect);       // Android
  server.on("/gen_204", HTTP_ANY, captivePortalRedirect);            // Android variants
  server.on("/hotspot-detect.html", HTTP_ANY, captivePortalRedirect);// Apple
  server.on("/library/test/success.html", HTTP_ANY, captivePortalRedirect);
  server.on("/connecttest.txt", HTTP_ANY, captivePortalRedirect);    // Windows
  server.on("/ncsi.txt", HTTP_ANY, captivePortalRedirect);           // Windows
  server.on("/redirect", HTTP_ANY, captivePortalRedirect);

  server.onNotFound(captivePortalRedirect);

  server.begin();

  currentQuote = random(quoteCount);
  prepareQuote();
  drawQuote();
}

void loop() {
  if (dnsStarted) {
    dns.processNextRequest();
  }

  server.handleClient();
  updateDisplay();

  delay(1);
}
