// HealthPay counter box (demo) for the ESP32 "Cheap Yellow Display" boards.
//
// The cashier types an amount; the box shows a Meeza QR for that sale at once
// (it builds the QR itself), registers the sale with the HealthPay demo server
// in the background, asks the server every few seconds whether it was paid,
// then shows a green tick and says the amount in Arabic.
//
// Boards (chosen at build time, see platformio.ini):
//   cyd        ESP32-2432S028R, ILI9341 screen, touch on its own pins
//   cyd_st7789 the same board with an ST7789 screen (some USB-C versions)
//   e32        2.8" ESP32-32E (E32R28T), ILI9341, touch shares the screen's bus
//   e32_st7789 the same with an ST7789 screen
// Each also comes as a restaurant version (*_rest, BOX_RESTAURANT): before the
// QR the cashier picks who served the table and the buyer picks a tip; the QR
// is for the bill plus the tip. It acts like the shop version when the
// server's business type for the shop is not a restaurant.
//
// Demo only: the server's TLS certificate is not checked (setInsecure).

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <vector>
#include "soc/rtc_io_reg.h"
#include "soc/sens_reg.h"
#include "meeza.h"
#include "words.h"
#include "hpqr.h"
#include "tips.h"
#ifdef BOX_TOUCH_OWN_BUS
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#endif

#define BOX_VERSION "0.4.0"
#ifdef BOX_RESTAURANT
static const bool RESTAURANT = true;
#define BOX_KIND "Restaurant"
#else
static const bool RESTAURANT = false;
#define BOX_KIND "Shop"
#endif
#ifndef BOX_BOARD
#define BOX_BOARD "unknown"
#endif

// ---------------------------------------------------------------- colours
#define C_BG 0xFFFF
#define C_INK 0x18E3
#define C_MUTED 0x7BEF
#define C_LINE 0xD69A
#define C_KEY 0xEF7D
#define C_ORANGE 0xFC60
#define C_GREEN 0x2E89
#define C_GREEN_DARK 0x1C46
#define C_RED 0xD8A7
#define C_AMBER 0xFE60

TFT_eSPI tft;
Preferences prefs;

#ifdef BOX_TOUCH_OWN_BUS
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen ts(33, 36);
#endif

// ---------------------------------------------------------------- settings
struct Settings {
  String url, key, shop, device, term;
  bool invert = false;
  int volume = 2;  // 1..3
  bool calOk = false;
  bool swap = false;
  float ax = 0.0857f, bx = -17, ay = 0.0674f, by = -16;
} cfg;

// What the server told us last time (kept so the box also works offline).
struct Cache {
  String shopName, qrBase, termId, termName;
  long next = 0, to = 0;
} cache;

void loadSettings() {
  prefs.begin("hpbox", true);
  cfg.url = prefs.getString("url", "");
  cfg.key = prefs.getString("key", "");
  cfg.shop = prefs.getString("shop", "A");
  cfg.term = prefs.getString("term", "Main counter");
  cfg.invert = prefs.getBool("inv", false);
  cfg.volume = prefs.getInt("vol", 2);
  cfg.calOk = prefs.getBool("calok", false);
  cfg.swap = prefs.getBool("swap", false);
  cfg.ax = prefs.getFloat("ax", cfg.ax);
  cfg.bx = prefs.getFloat("bx", cfg.bx);
  cfg.ay = prefs.getFloat("ay", cfg.ay);
  cfg.by = prefs.getFloat("by", cfg.by);
  cache.shopName = prefs.getString("sname", "");
  cache.qrBase = prefs.getString("qrbase", "");
  cache.termId = prefs.getString("tid", "");
  cache.termName = prefs.getString("tname", "");
  cache.next = prefs.getLong("next", 0);
  cache.to = prefs.getLong("to", 0);
  prefs.end();
  uint64_t mac = ESP.getEfuseMac();
  char id[16];
  snprintf(id, sizeof id, "BOX%04X", (unsigned)((mac >> 32) & 0xFFFF));
  cfg.device = id;
}

void saveSettings() {
  prefs.begin("hpbox", false);
  prefs.putString("url", cfg.url);
  prefs.putString("key", cfg.key);
  prefs.putString("shop", cfg.shop);
  prefs.putString("term", cfg.term);
  prefs.putBool("inv", cfg.invert);
  prefs.putInt("vol", cfg.volume);
  prefs.putBool("calok", cfg.calOk);
  prefs.putBool("swap", cfg.swap);
  prefs.putFloat("ax", cfg.ax);
  prefs.putFloat("bx", cfg.bx);
  prefs.putFloat("ay", cfg.ay);
  prefs.putFloat("by", cfg.by);
  prefs.end();
}

void saveCache() {
  prefs.begin("hpbox", false);
  prefs.putString("sname", cache.shopName);
  prefs.putString("qrbase", cache.qrBase);
  prefs.putString("tid", cache.termId);
  prefs.putString("tname", cache.termName);
  prefs.putLong("next", cache.next);
  prefs.putLong("to", cache.to);
  prefs.end();
}

// ---------------------------------------------------------------- shared state (UI <-> network task)
SemaphoreHandle_t mtx;
struct Lock {
  Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mtx); }
};

enum OpKind { OP_SALE, OP_CANCEL };
struct Op {
  OpKind kind;
  long number, amount;
  long tip;
  String waiter;
};

struct SaleView {
  long number = 0, amount = 0, tip = 0;
  bool registered = false;
  String status = "";  // from the server: waiting, paid, screenshot, approval, expired, cancelled
  long paid = 0, diff = 0;
  String from3, method;
};

struct Event {
  String id, label, from3;
  long amount, sale;
  bool check;
};

struct StaffItem {
  String id, label;
};

struct Shared {
  bool online = false, helloDone = false, needHello = true;
  bool tips = false;               // the server says this shop is a restaurant
  std::vector<StaffItem> staff;    // on shift, for "Served by"
  String error;  // last server error, shown in settings
  long long serverNow = 0, since = 0;
  std::vector<Op> ops;
  SaleView sale;  // the sale on screen (number 0 = none)
  std::vector<Event> events;  // new payments to announce
  uint32_t lastPollMs = 0;
} S;

// ---------------------------------------------------------------- network
String urlEncode(const String &v) {
  String o;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') o += c;
    else { o += '%'; o += hex[(c >> 4) & 15]; o += hex[c & 15]; }
  }
  return o;
}

String httpGet(const String &query, int &code) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  // The counter: its id once the server has told us (after hello), its name before.
  String term;
  { Lock l; term = cache.termId.length() ? cache.termId : cfg.term; }
  String url = cfg.url + "?type=box&key=" + cfg.key + "&shop=" + cfg.shop + "&device=" + cfg.device +
               "&terminal=" + urlEncode(query.startsWith("action=hello") ? cfg.term : term) + "&" + query;
  if (!http.begin(client, url)) { code = -1; return ""; }
  code = http.GET();
  String body = code > 0 ? http.getString() : "";
  http.end();
  return body;
}

// Returns true and fills doc when the server answered with JSON.
bool callServer(const String &query, JsonDocument &doc) {
  int code = 0;
  String body = httpGet(query, code);
  if (code != 200) {
    Lock l;
    S.online = false;
    S.error = code < 0 ? "No connection to the server" : "Server answered " + String(code);
    return false;
  }
  if (body.startsWith("error:")) {
    Lock l;
    S.online = true;
    S.error = body.substring(7);
    return false;
  }
  if (deserializeJson(doc, body)) {
    Lock l;
    S.online = true;
    S.error = "Unexpected answer (is the link the /exec one?)";
    return false;
  }
  Lock l;
  S.online = true;
  S.error = "";
  return true;
}

// Restaurant: whether tips are on, and who is on shift (called with the lock held).
void readStaff(JsonDocument &doc) {
  if (!doc["tips"].is<bool>()) return;
  S.tips = doc["tips"] | false;
  S.staff.clear();
  for (JsonObject st : doc["staff"].as<JsonArray>()) {
    StaffItem it;
    it.id = (const char *)(st["id"] | "");
    it.label = (const char *)(st["label"] | "");
    if (it.id.length() && S.staff.size() < 6) S.staff.push_back(it);
  }
}

void netHello() {
  JsonDocument doc;
  if (!callServer("action=hello", doc)) return;
  Lock l;
  cache.shopName = doc["shop"]["name"] | cache.shopName.c_str();
  cache.qrBase = doc["qrBase"] | "";
  cache.next = doc["block"]["from"] | 0L;
  cache.to = doc["block"]["to"] | 0L;
  cache.termId = (const char *)(doc["terminal"]["id"] | "");
  cache.termName = (const char *)(doc["terminal"]["name"] | "");
  S.serverNow = doc["now"] | 0LL;
  readStaff(doc);
  if (!S.since) S.since = S.serverNow;
  S.helloDone = true;
  S.needHello = false;
  saveCache();
}

void netReserveIfLow() {
  long left;
  { Lock l; left = cache.to - cache.next + 1; }
  if (left >= 4) return;
  JsonDocument doc;
  if (!callServer("action=reserve", doc)) return;
  Lock l;
  cache.next = doc["block"]["from"] | cache.next;
  cache.to = doc["block"]["to"] | cache.to;
  saveCache();
}

void netOps() {
  Op op;
  {
    Lock l;
    if (S.ops.empty()) return;
    op = S.ops.front();
  }
  JsonDocument doc;
  String q = op.kind == OP_SALE ? "action=sale&number=" + String(op.number) + "&amount=" + String(op.amount)
                                : "action=cancel&number=" + String(op.number);
  if (op.kind == OP_SALE && op.tip > 0) q += "&tip=" + String(op.tip);
  if (op.kind == OP_SALE && op.waiter.length()) q += "&waiter=" + urlEncode(op.waiter);
  bool ok = callServer(q, doc);
  Lock l;
  if (!ok && S.online && op.kind == OP_SALE) {
    // The server refused it (not a connection problem): drop it and show why.
    if (S.sale.number == op.number) S.sale.status = "refused";
  }
  if (ok || S.online) {
    if (!S.ops.empty() && S.ops.front().number == op.number && S.ops.front().kind == op.kind) S.ops.erase(S.ops.begin());
    if (ok && op.kind == OP_SALE && S.sale.number == op.number) S.sale.registered = true;
  }
}

void netPoll() {
  long number;
  long long since;
  bool registered;
  {
    Lock l;
    number = S.sale.number;
    registered = S.sale.registered;
    since = S.since;
  }
  char sinceS[24];
  snprintf(sinceS, sizeof sinceS, "%lld", since);
  String q = String("action=poll&since=") + sinceS;
  if (number && registered) q += "&numbers=" + String(number);
  JsonDocument doc;
  if (!callServer(q, doc)) return;
  Lock l;
  S.serverNow = doc["now"] | S.serverNow;
  readStaff(doc);
  for (JsonObject s : doc["sales"].as<JsonArray>()) {
    if ((long)(s["number"] | 0L) != S.sale.number) continue;
    S.sale.status = (const char *)(s["status"] | "");
    S.sale.paid = lround((double)(s["paidAmount"] | 0.0));
    S.sale.diff = lround((double)(s["diff"] | 0.0));
    S.sale.from3 = (const char *)(s["from3"] | "");
    S.sale.method = (const char *)(s["method"] | "");
  }
  for (JsonObject e : doc["events"].as<JsonArray>()) {
    Event ev;
    ev.id = (const char *)(e["id"] | "");
    ev.amount = lround((double)(e["amount"] | 0.0));
    ev.label = (const char *)(e["label"] | "");
    ev.from3 = (const char *)(e["from3"] | "");
    ev.sale = e["sale"] | 0L;
    ev.check = e["check"] | false;
    long long ms = e["ms"] | 0LL;
    if (ms > S.since) S.since = ms;
    if (ev.amount > 0) S.events.push_back(ev);
  }
  if (S.events.size() > 8) S.events.erase(S.events.begin(), S.events.end() - 8);
}

void netTask(void *) {
  for (;;) {
    if (WiFi.status() != WL_CONNECTED || cfg.url.isEmpty()) {
      { Lock l; S.online = false; }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    bool hello;
    { Lock l; hello = S.needHello; }
    if (hello) { netHello(); vTaskDelay(pdMS_TO_TICKS(hello && !S.helloDone ? 5000 : 200)); continue; }
    netOps();
    netReserveIfLow();
    uint32_t every;
    bool hasSale, opsWaiting;
    {
      Lock l;
      hasSale = S.sale.number != 0;
      opsWaiting = !S.ops.empty();
      every = hasSale ? 2500 : 5000;
    }
    if (!opsWaiting && millis() - S.lastPollMs > every) {
      netPoll();
      S.lastPollMs = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

// ---------------------------------------------------------------- audio (GPIO26 DAC, 12 kHz, 8-bit clips in flash)
static uint8_t *abuf = nullptr;
static const uint32_t ABUF_MAX = 40000;
static volatile uint32_t alen = 0, apos = 0;
static volatile bool aplaying = false;
static hw_timer_t *atimer = nullptr;
std::vector<String> clipQueue;
uint32_t audioIdleSince = 0;

void IRAM_ATTR onAudioTick() {
  if (apos < alen) {
    uint8_t v = abuf[apos++];
    SET_PERI_REG_BITS(RTC_IO_PAD_DAC2_REG, RTC_IO_PDAC2_DAC, v, RTC_IO_PDAC2_DAC_S);
  } else {
    aplaying = false;
  }
}

void ampOn(bool on) {
#ifdef BOX_AUDIO_EN
  digitalWrite(BOX_AUDIO_EN, on ? LOW : HIGH);
#endif
}

void audioBegin() {
  abuf = (uint8_t *)malloc(ABUF_MAX);
#ifdef BOX_AUDIO_EN
  pinMode(BOX_AUDIO_EN, OUTPUT);
#endif
  ampOn(false);
  dacWrite(26, 128);
  atimer = timerBegin(0, 4, true);  // 80 MHz / 4 = 20 MHz
  timerAttachInterrupt(atimer, &onAudioTick, true);
  timerAlarmWrite(atimer, 1667, true);  // 12 kHz
  timerAlarmEnable(atimer);
}

void say(const std::vector<std::string> &clips) {
  for (auto &c : clips) clipQueue.push_back(String("/") + c.c_str() + ".u8");
}

void audioLoop() {
  if (aplaying) return;
  if (clipQueue.empty()) {
    if (audioIdleSince && millis() - audioIdleSince > 300) { ampOn(false); dacWrite(26, 128); audioIdleSince = 0; }
    return;
  }
  String name = clipQueue.front();
  clipQueue.erase(clipQueue.begin());
  if (!abuf) return;
  File f = LittleFS.open(name, "r");
  if (!f) return;
  uint32_t n = f.read(abuf, ABUF_MAX);
  f.close();
  int g = cfg.volume == 1 ? 6 : cfg.volume == 3 ? 16 : 10;  // gain x10
  for (uint32_t i = 0; i < n; i++) {
    int s = 128 + ((int)abuf[i] - 128) * g / 10;
    abuf[i] = s < 0 ? 0 : s > 255 ? 255 : s;
  }
  ampOn(true);
  apos = 0;
  alen = n;
  aplaying = true;
  audioIdleSince = millis();
}

// ---------------------------------------------------------------- touch
bool rawTouch(int &rx, int &ry) {
#ifdef BOX_TOUCH_OWN_BUS
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z < 250) return false;
  rx = p.x; ry = p.y;
  return true;
#else
  if (tft.getTouchRawZ() < 300) return false;
  uint16_t x, y;
  tft.getTouchRaw(&x, &y);
  rx = x; ry = y;
  return true;
#endif
}

bool readTouch(int &x, int &y) {
  int rx, ry;
  if (!rawTouch(rx, ry)) return false;
  float u = cfg.swap ? ry : rx, v = cfg.swap ? rx : ry;
  x = (int)(cfg.ax * u + cfg.bx);
  y = (int)(cfg.ay * v + cfg.by);
  x = constrain(x, 0, 319);
  y = constrain(y, 0, 239);
  return true;
}

// One tap: returns true once per press, with the press position.
bool tapped(int &x, int &y) {
  static bool down = false;
  static uint32_t lastUp = 0;
  int tx, ty;
  bool now = readTouch(tx, ty);
  if (now && !down && millis() - lastUp > 120) {
    down = true;
    x = tx; y = ty;
    return true;
  }
  if (!now && down) { down = false; lastUp = millis(); }
  return false;
}

bool pressing() { int x, y; return readTouch(x, y); }

void crosshair(int x, int y, uint16_t c) {
  tft.drawFastHLine(x - 12, y, 25, c);
  tft.drawFastVLine(x, y - 12, 25, c);
  tft.drawCircle(x, y, 6, c);
}

bool samplePoint(int x, int y, int &rx, int &ry) {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_INK, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Touch the centre of the cross", 160, 120, 2);
  tft.drawString("(use the pen if you have it)", 160, 140, 2);
  crosshair(x, y, C_ORANGE);
  while (true) {
    int a, b;
    if (rawTouch(a, b)) break;
    delay(10);
  }
  long sx = 0, sy = 0;
  int n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 400) {
    int a, b;
    if (rawTouch(a, b)) { sx += a; sy += b; n++; }
    delay(10);
  }
  while (true) {
    int a, b;
    if (!rawTouch(a, b)) break;
    delay(10);
  }
  delay(200);
  if (n < 5) return false;
  rx = sx / n; ry = sy / n;
  return true;
}

void calibrate() {
  for (;;) {
    int r0x, r0y, r1x, r1y, r2x, r2y;
    if (!samplePoint(30, 30, r0x, r0y) || !samplePoint(290, 30, r1x, r1y) || !samplePoint(30, 210, r2x, r2y)) continue;
    bool swap = abs(r1y - r0y) > abs(r1x - r0x);
    float u0 = swap ? r0y : r0x, u1 = swap ? r1y : r1x;
    float v0 = swap ? r0x : r0y, v2 = swap ? r2x : r2y;
    if (fabs(u1 - u0) < 400 || fabs(v2 - v0) < 400) continue;
    cfg.swap = swap;
    cfg.ax = 260.0f / (u1 - u0);
    cfg.bx = 30 - cfg.ax * u0;
    cfg.ay = 180.0f / (v2 - v0);
    cfg.by = 30 - cfg.ay * v0;
    cfg.calOk = true;
    saveSettings();
    return;
  }
}

// ---------------------------------------------------------------- drawing helpers
struct Btn {
  int x, y, w, h;
  bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

void button(const Btn &b, const char *label, uint16_t bg, uint16_t fg, int font = 4) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 8, bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, b.x + b.w / 2, b.y + b.h / 2 + 1, font);
}

String money(long n) {
  String s = String(n), out;
  int c = 0;
  for (int i = s.length() - 1; i >= 0; i--) {
    out = s[i] + out;
    if (++c % 3 == 0 && i > 0) out = "," + out;
  }
  return out;
}

void drawTick(int cx, int cy, int r, uint16_t c) {
  for (int t = -4; t <= 4; t++) {
    tft.drawLine(cx - r / 2, cy + t, cx - r / 8, cy + r / 3 + t, c);
    tft.drawLine(cx - r / 8, cy + r / 3 + t, cx + r / 2, cy - r / 3 + t, c);
  }
}

// ---------------------------------------------------------------- screens
enum Screen { SCR_HOME, SCR_QR, SCR_RESULT, SCR_SETTINGS, SCR_WAITER, SCR_TIP };
Screen screen = SCR_HOME;
bool dirty = true;
String typed = "";
String toast = "";
uint32_t toastUntil = 0, resultSince = 0;
bool lastOnline = false;
// Restaurant: the bill being paid, who served it, and the tip choices.
long billAmount = 0;
String waiterId = "", waiterLabel = "";
long tipChoice[3] = {1, 2, 5};

bool tipsOn() {
  if (!RESTAURANT) return false;
  Lock l;
  return S.tips;
}

const char *KEYS[4][4] = {{"1", "2", "3", "Del"}, {"4", "5", "6", "Clear"}, {"7", "8", "9", ""}, {"00", "0", "", ""}};
Btn keyBtn(int r, int c) { return Btn{4 + c * 79, 84 + r * 39, 75, 35}; }
const Btn BTN_QR{4 + 3 * 79, 84 + 2 * 39, 75, 74};
const Btn BTN_TOP{0, 0, 320, 30};

void drawTop() {
  bool online;
  String name;
  { Lock l; online = S.online; name = cache.shopName; }
  tft.fillRect(0, 0, 320, 30, C_ORANGE);
  tft.setTextColor(TFT_WHITE, C_ORANGE);
  tft.setTextDatum(ML_DATUM);
  String counter;
  { Lock l; counter = cache.termName.length() ? cache.termName : cfg.term; }
  tft.drawString((name.length() ? name : String("HealthPay box")) + " - " + counter, 8, 15, 2);
  if (toast.length() && millis() < toastUntil) {
    tft.fillRect(0, 0, 320, 30, C_GREEN_DARK);
    tft.setTextColor(TFT_WHITE, C_GREEN_DARK);
    tft.drawString(toast, 8, 15, 2);
    return;
  }
  tft.setTextDatum(MR_DATUM);
  tft.fillCircle(240, 15, 5, online ? C_GREEN : C_RED);
  tft.drawString(online ? "Online" : "Offline", 312, 15, 2);
}

void drawHome() {
  tft.fillScreen(C_BG);
  drawTop();
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("EGP", 312, 60, 2);
  tft.setTextColor(C_INK, C_BG);
  tft.drawString(typed.length() ? money(typed.toInt()) : String("0"), 270, 58, 6);
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      if (!KEYS[r][c][0]) continue;
      bool word = strlen(KEYS[r][c]) > 2;
      button(keyBtn(r, c), KEYS[r][c], C_KEY, C_INK, word ? 2 : 4);
    }
  bool ok = typed.toInt() > 0;
  button(BTN_QR, tipsOn() ? "Next" : "QR", ok ? C_ORANGE : C_LINE, TFT_WHITE, 4);
}

void drawAmountOnly() {
  tft.fillRect(0, 31, 290, 50, C_BG);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(C_INK, C_BG);
  tft.drawString(typed.length() ? money(typed.toInt()) : String("0"), 270, 58, 6);
  bool ok = typed.toInt() > 0;
  button(BTN_QR, tipsOn() ? "Next" : "QR", ok ? C_ORANGE : C_LINE, TFT_WHITE, 4);
}

std::string qrText;
const Btn BTN_CANCEL{238, 196, 78, 40};

void drawQr() {
  static uint8_t q[hpqr_BUFFER_LEN_FOR_VERSION(14)], tmp[hpqr_BUFFER_LEN_FOR_VERSION(14)];
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 234, 240, TFT_WHITE);
  bool ok = hpqr_encodeText(qrText.c_str(), tmp, q, hpqr_Ecc_MEDIUM, hpqr_VERSION_MIN, 14, hpqr_Mask_AUTO, true);
  if (ok) {
    int n = hpqr_getSize(q);
    int scale = 226 / (n + 4);
    if (scale < 1) scale = 1;
    int size = n * scale, ox = (234 - size) / 2, oy = (240 - size) / 2;
    for (int y = 0; y < n; y++)
      for (int x = 0; x < n; x++)
        if (hpqr_getModule(q, x, y)) tft.fillRect(ox + x * scale, oy + y * scale, scale, scale, TFT_BLACK);
  } else {
    tft.setTextColor(C_RED, TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("QR too long", 117, 120, 2);
  }
  long amount, number, tip;
  { Lock l; amount = S.sale.amount; number = S.sale.number; tip = S.sale.tip; }
  tft.setTextColor(C_MUTED, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Pay", 240, 6, 2);
  tft.setTextColor(C_INK, C_BG);
  tft.drawString(money(amount), 240, 24, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("EGP", 240, 52, 2);
  tft.drawString("Bill " + String(number), 240, 78, 2);
  if (tip > 0) { tft.setTextColor(C_GREEN_DARK, C_BG); tft.drawString("Tip " + money(tip), 240, 92, 2); tft.setTextColor(C_MUTED, C_BG); }
  tft.drawString("Scan with", 240, 108, 2);
  tft.drawString("any wallet", 240, 124, 2);
  button(BTN_CANCEL, "Cancel", C_KEY, C_INK, 2);
}

void drawQrStatus() {
  bool reg, online;
  String status;
  { Lock l; reg = S.sale.registered; online = S.online; status = S.sale.status; }
  tft.fillRect(236, 150, 84, 40, C_BG);
  tft.setTextDatum(TL_DATUM);
  uint16_t c = reg ? C_GREEN_DARK : online ? C_MUTED : C_RED;
  tft.setTextColor(c, C_BG);
  String line = status == "refused" ? "Refused" : reg ? "Waiting" : online ? "Saving..." : "Offline";
  tft.drawString(line, 240, 156, 2);
  if (status == "screenshot" || status == "approval") tft.drawString("Check", 240, 172, 2);
}

void drawResult() {
  long amount, paid, diff, tip;
  String from3, method, status;
  { Lock l; amount = S.sale.amount; paid = S.sale.paid; diff = S.sale.diff; from3 = S.sale.from3; method = S.sale.method; status = S.sale.status; tip = S.sale.tip; }
  bool expired = status == "expired" || status == "cancelled" || status == "refused";
  uint16_t bg = expired ? C_MUTED : diff < 0 ? C_AMBER : C_GREEN;
  tft.fillScreen(bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextDatum(MC_DATUM);
  if (expired) {
    tft.drawString(status == "refused" ? "Sale refused" : status == "cancelled" ? "Sale cancelled" : "Sale expired", 160, 100, 4);
    tft.drawString("Tap to continue", 160, 150, 2);
    return;
  }
  drawTick(56, 70, 70, TFT_WHITE);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Received", 110, 40, 4);
  tft.drawString(money(paid ? paid : amount), 110, 90, 6);
  tft.drawString("EGP", 110 + tft.textWidth(money(paid ? paid : amount), 6) + 8, 102, 4);
  String d = method.length() ? method : String("Payment");
  if (from3.length()) d += "  from ..." + from3;
  tft.setTextDatum(MC_DATUM);
  tft.drawString(d, 160, 150, 2);
  if (diff < 0) tft.drawString("Still owed " + money(-diff) + " EGP", 160, 185, 4);
  else if (diff > 0) tft.drawString(money(diff) + " EGP more than the bill", 160, 185, 2);
  else if (tip > 0) tft.drawString("Includes a tip of " + money(tip) + " EGP. Thank you!", 160, 185, 2);
  tft.drawString("Tap for a new sale", 160, 222, 2);
}

// Settings
const Btn SB_WIFI{8, 40, 148, 44}, SB_CAL{164, 40, 148, 44}, SB_INV{8, 92, 148, 44}, SB_VOL{164, 92, 148, 44},
    SB_TEST{8, 144, 148, 44}, SB_BACK{164, 144, 148, 44};

void drawSettings() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 30, C_INK);
  tft.setTextColor(TFT_WHITE, C_INK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Settings  v" BOX_VERSION "  " BOX_KIND "  " BOX_BOARD, 8, 15, 2);
  button(SB_WIFI, "Wi-Fi & server", C_KEY, C_INK, 2);
  button(SB_CAL, "Touch setup", C_KEY, C_INK, 2);
  button(SB_INV, cfg.invert ? "Colours: inverted" : "Colours: normal", C_KEY, C_INK, 2);
  String v = "Volume: " + String(cfg.volume);
  button(SB_VOL, v.c_str(), C_KEY, C_INK, 2);
  button(SB_TEST, "Test voice", C_KEY, C_INK, 2);
  button(SB_BACK, "Back", C_ORANGE, TFT_WHITE, 2);
  String err, ip = WiFi.localIP().toString();
  bool online;
  long next, to;
  { Lock l; err = S.error; online = S.online; next = cache.next; to = cache.to; }
  tft.setTextColor(C_MUTED, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(cfg.device + "  Wi-Fi " + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() + " " + ip : String("not connected")), 8, 196, 2);
  tft.drawString(String("Server ") + (online ? "OK" : "offline") + "  bills " + String(next) + "-" + String(to) + "  shop " + cfg.shop, 8, 212, 2);
  if (err.length()) { tft.setTextColor(C_RED, C_BG); tft.drawString(err.substring(0, 50), 8, 226, 1); }
}

// Restaurant: who served the table (cashier side)
Btn staffBtn(int i) { return Btn{8 + (i % 2) * 156, 40 + (i / 2) * 48, 148, 42}; }
const Btn WB_BACK{8, 190, 148, 44}, WB_NONE{164, 190, 148, 44};

void drawWaiter() {
  std::vector<StaffItem> staff;
  { Lock l; staff = S.staff; }
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 30, C_INK);
  tft.setTextColor(TFT_WHITE, C_INK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Served by?   Bill " + money(billAmount) + " EGP", 8, 15, 2);
  for (size_t i = 0; i < staff.size() && i < 6; i++) button(staffBtn(i), staff[i].label.c_str(), C_KEY, C_INK, 2);
  button(WB_BACK, "Back", C_KEY, C_INK, 2);
  button(WB_NONE, "No one", C_KEY, C_INK, 2);
}

// Restaurant: the buyer picks a tip (turn the box to the buyer)
Btn tipBtn(int i) { return Btn{8 + (i % 2) * 156, 92 + (i / 2) * 72, 148, 64}; }
const Btn TB_BACK{232, 0, 88, 30};

void tipButton(int i, const String &big, const String &small) {
  Btn b = tipBtn(i);
  bool none = i == 0;
  uint16_t bg = none ? C_KEY : C_ORANGE, fg = none ? C_INK : TFT_WHITE;
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 10, bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(big, b.x + b.w / 2, b.y + 22, 4);
  tft.drawString(small, b.x + b.w / 2, b.y + 48, 2);
}

void drawTip() {
  tips::options(billAmount, tipChoice);
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 30, C_ORANGE);
  tft.setTextColor(TFT_WHITE, C_ORANGE);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Add a tip?", 8, 15, 2);
  tft.setTextDatum(MR_DATUM);
  tft.drawString("Back", 312, 15, 2);
  tft.setTextColor(C_INK, C_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Bill " + money(billAmount) + " EGP", 8, 52, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(waiterLabel.length() ? "Served by " + waiterLabel : String("Thank you for visiting"), 8, 78, 2);
  tipButton(0, "No tip", "Pay " + money(billAmount));
  for (int i = 0; i < 3; i++) tipButton(i + 1, "+" + money(tipChoice[i]), "Pay " + money(billAmount + tipChoice[i]));
}

// ---------------------------------------------------------------- Wi-Fi setup portal
String cleanUrl(String u) {
  u.trim();
  int q = u.indexOf('?');
  if (q >= 0) u = u.substring(0, q);
  return u;
}

void showSetupInfo(const String &ap) {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 30, C_ORANGE);
  tft.setTextColor(TFT_WHITE, C_ORANGE);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Set up the box", 8, 15, 2);
  tft.setTextColor(C_INK, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("1. On your phone, open Wi-Fi and join:", 8, 44, 2);
  tft.setTextColor(C_ORANGE, C_BG);
  tft.drawString(ap, 24, 64, 4);
  tft.setTextColor(C_INK, C_BG);
  tft.drawString("2. A page opens (or open 192.168.4.1).", 8, 100, 2);
  tft.drawString("   Tap Configure WiFi, pick your Wi-Fi,", 8, 118, 2);
  tft.drawString("   type its password.", 8, 136, 2);
  tft.drawString("3. Paste the server link and the key,", 8, 158, 2);
  tft.drawString("   shop A, the counter name, then Save.", 8, 176, 2);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("Phone hotspots work too (2.4 GHz only).", 8, 206, 2);
}

void runPortal() {
  WiFiManager wm;
  WiFiManagerParameter pUrl("url", "Server link (ends with /exec)", cfg.url.c_str(), 220);
  WiFiManagerParameter pKey("key", "Key", cfg.key.c_str(), 40);
  WiFiManagerParameter pShop("shop", "Shop", cfg.shop.c_str(), 4);
  WiFiManagerParameter pTerm("term", "Counter name (e.g. Main counter, Front desk)", cfg.term.c_str(), 30);
  wm.addParameter(&pUrl);
  wm.addParameter(&pKey);
  wm.addParameter(&pShop);
  wm.addParameter(&pTerm);
  wm.setTitle("HealthPay box");
  wm.setConfigPortalTimeout(0);
  String ap = "HealthPay-" + cfg.device;
  showSetupInfo(ap);
  wm.startConfigPortal(ap.c_str());
  cfg.url = cleanUrl(pUrl.getValue());
  cfg.key = String(pKey.getValue());
  cfg.key.trim();
  cfg.shop = String(pShop.getValue());
  cfg.shop.trim();
  cfg.shop.toUpperCase();
  if (cfg.shop.isEmpty()) cfg.shop = "A";
  cfg.term = String(pTerm.getValue());
  cfg.term.trim();
  if (cfg.term.isEmpty()) cfg.term = "Main counter";
  cache.termId = "";  // the server gives the id again at the next hello
  saveCache();
  saveSettings();
  ESP.restart();
}

// ---------------------------------------------------------------- sale flow
void startSale(long amount, long tip = 0, const String &waiter = "") {
  long number;
  {
    Lock l;
    if (cache.next == 0 || cache.next > cache.to || !meeza::valid(cache.qrBase.c_str())) {
      toast = S.online ? "Waiting for the server..." : "Connect to Wi-Fi first";
      toastUntil = millis() + 3000;
      S.needHello = true;
      return;
    }
    number = cache.next++;
    saveCache();
    S.sale = SaleView();
    S.sale.number = number;
    S.sale.amount = amount;
    S.sale.tip = tip;
    S.sale.status = "waiting";
    S.ops.push_back(Op{OP_SALE, number, amount, tip, waiter});
    S.lastPollMs = millis();
  }
  qrText = meeza::saleQr(cache.qrBase.c_str(), amount, number, cache.termId.length() ? cache.termId.c_str() : "main");
  screen = SCR_QR;
  dirty = true;
}

void cancelSale() {
  Lock l;
  long n = S.sale.number;
  bool wasQueued = false;
  for (size_t i = 0; i < S.ops.size(); i++)
    if (S.ops[i].kind == OP_SALE && S.ops[i].number == n) { S.ops.erase(S.ops.begin() + i); wasQueued = true; break; }
  if (!wasQueued && n) S.ops.push_back(Op{OP_CANCEL, n, 0, 0, ""});
  S.sale = SaleView();
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  mtx = xSemaphoreCreateMutex();
  loadSettings();
  tft.init();
  tft.setRotation(1);
  tft.invertDisplay(cfg.invert);
#ifdef BOX_TOUCH_OWN_BUS
  touchSpi.begin(25, 39, 32, 33);
  ts.begin(touchSpi);
#endif
  LittleFS.begin(false);
  audioBegin();

  tft.fillScreen(C_BG);
  tft.setTextColor(C_ORANGE, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("HealthPay", 160, 100, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("Counter box demo  v" BOX_VERSION, 160, 132, 2);
  delay(800);

  if (!cfg.calOk) calibrate();

  if (cfg.url.isEmpty() || cfg.key.isEmpty()) runPortal();  // restarts when done

  tft.fillScreen(C_BG);
  tft.setTextColor(C_INK, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting to Wi-Fi...", 160, 110, 2);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("Touch the screen for setup", 160, 135, 2);
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    if (pressing()) runPortal();
    delay(100);
  }
  WiFi.setAutoReconnect(true);
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
  screen = SCR_HOME;
  dirty = true;
}

void announce(const Event &e) {
  say(words::received(e.amount, e.from3.c_str()));
  toast = e.check ? "Check buyer's number" + (e.from3.length() ? " ..." + e.from3 : String("")) + "  " + money(e.amount) + " EGP"
                  : "Received " + money(e.amount) + " EGP  " + e.label + (e.from3.length() ? "  ..." + e.from3 : "");
  toastUntil = millis() + 6000;
}

void loop() {
  audioLoop();

  // New payments: speak each once.
  std::vector<Event> evs;
  {
    Lock l;
    evs.swap(S.events);
  }
  for (auto &e : evs) {
    announce(e);
    if (screen == SCR_HOME) drawTop();
    // A payment of the sale's amount while its QR is showing: show the tick at
    // once. The server attaches it to the sale (straight away, or after the
    // owner's waiting time when two sales could own it).
    if (screen == SCR_QR) {
      Lock l;
      if (S.sale.number && e.amount == S.sale.amount && (e.sale == S.sale.number || (e.sale == 0 && !e.check))) {
        S.sale.status = "paid";
        S.sale.paid = e.amount;
        S.sale.diff = 0;
        S.sale.from3 = e.from3;
        S.sale.method = e.label;
      }
    }
  }

  // Online dot / toast expiry on the home screen.
  bool online;
  { Lock l; online = S.online; }
  static bool toastShown = false;
  bool toastNow = toast.length() && millis() < toastUntil;
  if (screen == SCR_HOME && (online != lastOnline || toastNow != toastShown)) drawTop();
  lastOnline = online;
  toastShown = toastNow;

  if (dirty) {
    dirty = false;
    if (screen == SCR_HOME) drawHome();
    else if (screen == SCR_QR) { drawQr(); drawQrStatus(); }
    else if (screen == SCR_RESULT) drawResult();
    else if (screen == SCR_SETTINGS) drawSettings();
    else if (screen == SCR_WAITER) drawWaiter();
    else if (screen == SCR_TIP) drawTip();
  }

  // The sale on screen: watch its status.
  if (screen == SCR_QR) {
    static String lastLine;
    String status;
    bool reg;
    { Lock l; status = S.sale.status; reg = S.sale.registered; }
    String line = status + (reg ? "1" : "0") + (online ? "1" : "0");
    if (line != lastLine) { drawQrStatus(); lastLine = line; }
    if (status == "paid" || status == "expired" || status == "cancelled" || status == "refused") {
      screen = SCR_RESULT;
      resultSince = millis();
      dirty = true;
    }
  }
  if (screen == SCR_RESULT && millis() - resultSince > 12000) {
    { Lock l; S.sale = SaleView(); }
    typed = "";
    screen = SCR_HOME;
    dirty = true;
  }

  // Long press on the top bar opens the settings.
  static uint32_t topPressSince = 0;
  int px, py;
  if (readTouch(px, py) && BTN_TOP.hit(px, py) && (screen == SCR_HOME)) {
    if (!topPressSince) topPressSince = millis();
    if (millis() - topPressSince > 2000) {
      topPressSince = 0;
      screen = SCR_SETTINGS;
      dirty = true;
      while (pressing()) delay(10);
      return;
    }
  } else {
    topPressSince = 0;
  }

  int x, y;
  if (!tapped(x, y)) { delay(5); return; }

  if (screen == SCR_HOME) {
    if (BTN_QR.hit(x, y)) {
      long amount = typed.toInt();
      if (amount <= 0) return;
      if (tipsOn()) {
        // Restaurant: who served it, then the buyer's tip, then the QR.
        billAmount = amount;
        waiterId = "";
        waiterLabel = "";
        bool anyStaff;
        { Lock l; anyStaff = !S.staff.empty(); }
        screen = anyStaff ? SCR_WAITER : SCR_TIP;
        dirty = true;
      } else {
        startSale(amount);
      }
      return;
    }
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++) {
        const char *k = KEYS[r][c];
        if (!k[0] || !keyBtn(r, c).hit(x, y)) continue;
        if (!strcmp(k, "Del")) { if (typed.length()) typed.remove(typed.length() - 1); }
        else if (!strcmp(k, "Clear")) typed = "";
        else if (typed.length() + strlen(k) <= 6 && !(typed.isEmpty() && k[0] == '0')) typed += k;
        drawAmountOnly();
        return;
      }
  } else if (screen == SCR_QR) {
    if (BTN_CANCEL.hit(x, y)) {
      cancelSale();
      screen = SCR_HOME;
      dirty = true;
    }
  } else if (screen == SCR_RESULT) {
    { Lock l; S.sale = SaleView(); }
    typed = "";
    screen = SCR_HOME;
    dirty = true;
  } else if (screen == SCR_WAITER) {
    if (WB_BACK.hit(x, y)) { screen = SCR_HOME; dirty = true; return; }
    if (WB_NONE.hit(x, y)) { waiterId = ""; waiterLabel = ""; screen = SCR_TIP; dirty = true; return; }
    std::vector<StaffItem> staff;
    { Lock l; staff = S.staff; }
    for (size_t i = 0; i < staff.size() && i < 6; i++)
      if (staffBtn(i).hit(x, y)) { waiterId = staff[i].id; waiterLabel = staff[i].label; screen = SCR_TIP; dirty = true; return; }
  } else if (screen == SCR_TIP) {
    if (TB_BACK.hit(x, y)) {
      bool anyStaff;
      { Lock l; anyStaff = !S.staff.empty(); }
      screen = anyStaff ? SCR_WAITER : SCR_HOME;
      dirty = true;
      return;
    }
    for (int i = 0; i < 4; i++) {
      if (!tipBtn(i).hit(x, y)) continue;
      long tip = i == 0 ? 0 : tipChoice[i - 1];
      startSale(billAmount + tip, tip, waiterId);
      if (screen != SCR_QR) { screen = SCR_HOME; dirty = true; }  // no numbers yet: the toast says why
      return;
    }
  } else if (screen == SCR_SETTINGS) {
    if (SB_WIFI.hit(x, y)) runPortal();
    else if (SB_CAL.hit(x, y)) { calibrate(); dirty = true; }
    else if (SB_INV.hit(x, y)) { cfg.invert = !cfg.invert; tft.invertDisplay(cfg.invert); saveSettings(); dirty = true; }
    else if (SB_VOL.hit(x, y)) { cfg.volume = cfg.volume % 3 + 1; saveSettings(); say(words::received(50, "620")); dirty = true; }
    else if (SB_TEST.hit(x, y)) say(words::received(1250, "620"));
    else if (SB_BACK.hit(x, y)) { screen = SCR_HOME; dirty = true; }
  }
}
