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
// One file for every merchant (v0.8.1): when the logged-in person's shop is a
// restaurant or cafe, the box asks who served the table and offers a tip before
// the QR; for a shop it goes straight from the amount to the QR.
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

#define BOX_VERSION "0.12.2"
static const bool RESTAURANT = true;  // restaurant features follow the server's business type
#define BOX_KIND "Shop+Cafe"
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
  String token;  // the login of the person using the box (mobile + PIN + SMS code)
  bool invert = false;
  int volume = 2;  // 1..3
  bool calOk = false;
  bool swap = false;
  float ax = 0.0857f, bx = -17, ay = 0.0674f, by = -16;
} cfg;

// What the server told us last time (kept so the box also works offline).
struct Cache {
  String shopName, qrBase, termId, termName;
  String termQr;  // the short terminal label in the QR (W01, C02...)
  long next = 0, to = 0;
} cache;

void loadSettings() {
  prefs.begin("hpbox", true);
  cfg.url = prefs.getString("url", "");
  cfg.key = prefs.getString("key", "");
  cfg.shop = prefs.getString("shop", "A");
  cfg.term = prefs.getString("term", "Main counter");
  cfg.token = prefs.getString("token", "");
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
  cache.termQr = prefs.getString("tqr", "");
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
  prefs.putString("token", cfg.token);
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
  prefs.putString("tqr", cache.termQr);
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
  String biller, account;  // a bill the customer pays by QR, card or InstaPay (set after the braces)
};

struct SaleView {
  String id;  // the server's id once the sale has reached it (for the card page)
  long number = 0, amount = 0, tip = 0;
  bool registered = false;
  String status = "";  // from the server: waiting, paid, screenshot, approval, expired, cancelled
  long paid = 0, diff = 0;
  String from3, method;
  String billStatus, billRef, billName;  // a bill sale: the bill's own status once paid
  long billShort = 0;                     // the customer paid this much less than the bill
};

// Bills and recharges the box can sell (from the server's hello).
struct BillerItem {
  String id, name, kind, field;
  int min = 8, max = 12;
};
// The bill being sold: the amount due (looked up) and, once paid in cash, its reference.
struct BillView {
  String biller, name, account, customer, ref, result;
  long amount = 0;
  double commission = 0;
  long cardFee = 0;  // a card payment adds this fee for the customer; QR and InstaPay are free
};

// Trip mode (intercity microbus): seats paid by transfer, cash seats, seats missing.
struct TripView {
  bool on = false;
  long seats = 0, fare = 0, paid = 0, cash = 0, missing = 0, over = 0, partial = 0, unassigned = 0, card = 0;
  String carName;
};
// A car of a transport business (each soundbox is a car).
struct CarItem {
  String id, code, name;
  long seats = 0, fare = 0;  // saved by the owner
  bool locked = false;       // fixed fare: the driver can't change it
};

struct Event {
  String id, label, from3;
  long amount, sale;
  bool check;
};

struct StaffItem {
  String id, label;
};

// A payment the cashier could attach to the sale on screen.
struct InItem {
  String id, label, from3;
  long amount;
  bool match;
};

struct TodayItem {
  long number, amount;
  String status;
};

// One cashier request at a time (digits, attach, buyer, tip, today); the network task sends it.
struct Req {
  bool pending = false, done = false, ok = false;
  String query, result, msg;
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
  std::vector<InItem> incoming;  // for the sale on screen
  std::vector<TodayItem> today;
  String mode;                   // standard, mixed or strict
  bool signin = false;           // the shop's staff log in (mobile + PIN + SMS code)
  bool signedOut = false;        // the server ended this box's login
  String ticket, sentTo;         // login in progress
  String wallet;                 // the shop's wallet number (transfers and USSD)
  String user;                   // who is signed in on this counter
  std::vector<BillerItem> billers;
  BillView bill;
  TripView trip, tripEnd;  // the running trip, and the last one ended
  String kind;             // business type from the server: shop, restaurant or transport
  std::vector<CarItem> cars;  // microbuses only (taxis need no trip)
  bool tripsOn = true;         // false when every car is a taxi
  bool tripChanged = false;
  Req req;
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
  String token;
  { Lock l; token = cfg.token; }
  String url = cfg.url + "?type=box&key=" + cfg.key + "&shop=" + cfg.shop + "&device=" + cfg.device +
               "&terminal=" + urlEncode(query.startsWith("action=hello") ? cfg.term : term) +
               (token.length() && !query.startsWith("action=login") ? "&token=" + token : String("")) + "&" + query;
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
    // The login ended (owner logged them out, shift closed, or too long unused).
    if (S.error.startsWith("Signed out")) { S.signedOut = true; }
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

// Staff sign-in state (called with the lock held).

// Reads a trip object from the server's answer.
TripView readTrip(JsonObject t) {
  TripView v;
  v.on = String((const char *)(t["status"] | "open")) == "open";
  v.seats = t["seats"] | 0;
  v.fare = lround((double)(t["fare"] | 0.0));
  v.paid = t["paid"] | 0;
  v.cash = t["cash"] | 0;
  v.missing = t["missing"] | 0;
  v.over = t["over"] | 0;
  v.partial = t["partial"] | 0;
  v.unassigned = t["unassigned"] | 0;
  v.carName = (const char *)(t["carName"] | "");
  v.card = t["card"] | 0;
  return v;
}

void readUser(JsonDocument &doc) {
  if (doc["signin"].is<bool>()) S.signin = doc["signin"] | false;
  if (doc["user"].is<const char *>()) S.user = (const char *)(doc["user"] | "");
}

// Restaurant: whether tips are on, and who is on shift (called with the lock held).
void readStaff(JsonDocument &doc) {
  if (!doc["tips"].is<bool>()) return;
  S.tips = doc["tips"] | false;
  if (!doc["kind"].isNull()) S.kind = (const char *)(doc["kind"] | "shop");
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
  S.wallet = (const char *)(doc["wallet"] | "");
  cache.next = doc["block"]["from"] | 0L;
  cache.to = doc["block"]["to"] | 0L;
  cache.termId = (const char *)(doc["terminal"]["id"] | "");
  cache.termQr = (const char *)(doc["terminal"]["qr"] | "");
  cache.termName = (const char *)(doc["terminal"]["name"] | "");
  S.serverNow = doc["now"] | 0LL;
  readStaff(doc);
  readUser(doc);
  S.tripsOn = doc["trips"] | true;
  S.cars.clear();
  for (JsonObject c : doc["cars"].as<JsonArray>()) {
    CarItem it;
    it.id = (const char *)(c["id"] | "");
    it.code = (const char *)(c["code"] | "");
    it.name = (const char *)(c["name"] | "");
    it.seats = c["seats"] | 0;
    it.fare = lround((double)(c["fare"] | 0.0));
    it.locked = c["locked"] | false;
    if (it.code.length() && S.cars.size() < 4) S.cars.push_back(it);
  }
  S.billers.clear();
  for (JsonObject b : doc["billers"].as<JsonArray>()) {
    BillerItem it;
    it.id = (const char *)(b["id"] | "");
    it.name = (const char *)(b["name"] | "");
    it.kind = (const char *)(b["kind"] | "bill");
    it.field = (const char *)(b["field"] | "Account number");
    it.min = b["min"] | 8;
    it.max = b["max"] | 12;
    if (it.id.length() && S.billers.size() < 6) S.billers.push_back(it);
  }
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
  if (op.kind == OP_SALE && op.tip >= 0) q += "&tip=" + String(op.tip);  // -1: no tip asked (shop)
  if (op.kind == OP_SALE && op.waiter.length()) q += "&waiter=" + urlEncode(op.waiter);
  if (op.kind == OP_SALE && op.biller.length()) q += "&biller=" + urlEncode(op.biller) + "&account=" + urlEncode(op.account);
  bool ok = callServer(q, doc);
  Lock l;
  if (!ok && S.online && op.kind == OP_SALE) {
    // The server refused it (not a connection problem): drop it and show why.
    if (S.sale.number == op.number) S.sale.status = "refused";
  }
  if (ok || S.online) {
    if (!S.ops.empty() && S.ops.front().number == op.number && S.ops.front().kind == op.kind) S.ops.erase(S.ops.begin());
    if (ok && op.kind == OP_SALE && S.sale.number == op.number) {
      S.sale.registered = true;
      // The server answers with the sale's id: the Card tab can open at once.
      const char *sid = doc["id"] | "";
      if (sid[0]) S.sale.id = sid;
    }
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
  readUser(doc);
  for (JsonObject s : doc["sales"].as<JsonArray>()) {
    if ((long)(s["number"] | 0L) != S.sale.number) continue;
    S.sale.status = (const char *)(s["status"] | "");
    S.sale.paid = lround((double)(s["paidAmount"] | 0.0));
    S.sale.diff = lround((double)(s["diff"] | 0.0));
    S.sale.from3 = (const char *)(s["from3"] | "");
    S.sale.method = (const char *)(s["method"] | "");
    S.sale.id = (const char *)(s["id"] | "");
    if (s["bill"].is<JsonObject>()) {
      S.sale.billStatus = (const char *)(s["bill"]["status"] | "");
      S.sale.billRef = (const char *)(s["bill"]["ref"] | "");
      S.sale.billName = (const char *)(s["bill"]["name"] | "");
      S.sale.billShort = lround((double)(s["bill"]["shortBy"] | 0.0));
    }
  }
  S.mode = (const char *)(doc["mode"] | "standard");
  S.incoming.clear();
  for (JsonObject x : doc["incoming"].as<JsonArray>()) {
    InItem it;
    it.id = (const char *)(x["id"] | "");
    it.label = (const char *)(x["label"] | "");
    it.from3 = (const char *)(x["from3"] | "");
    it.amount = lround((double)(x["amount"] | 0.0));
    it.match = x["match"] | false;
    if (it.id.length() && S.incoming.size() < 4) S.incoming.push_back(it);
  }
  {
    TripView tv;
    if (doc["trip"].is<JsonObject>()) tv = readTrip(doc["trip"].as<JsonObject>());
    if (tv.on != S.trip.on || tv.paid != S.trip.paid || tv.cash != S.trip.cash || tv.missing != S.trip.missing || tv.seats != S.trip.seats) S.tripChanged = true;
    S.trip = tv;
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

void netReq() {
  String q;
  {
    Lock l;
    if (!S.req.pending) return;
    q = S.req.query;
  }
  JsonDocument doc;
  bool ok = callServer(q, doc);
  Lock l;
  S.req.ok = ok;
  S.req.result = ok ? String((const char *)(doc["result"] | "")) : String("");
  S.req.msg = ok ? String("") : S.error;
  if (ok) readUser(doc);
  if (q.startsWith("action=logout")) S.signedOut = true;  // forget the login here, whatever the server said
  if (ok && q.startsWith("action=login1")) { S.ticket = (const char *)(doc["ticket"] | ""); S.sentTo = (const char *)(doc["sentTo"] | ""); }
  if (ok && q.startsWith("action=login2")) {
    cfg.token = (const char *)(doc["token"] | "");
    String sh = (const char *)(doc["shop"]["id"] | "");
    if (sh.length()) cfg.shop = sh;
    S.user = (const char *)(doc["user"] | "");
    cache.termId = (const char *)(doc["term"] | "");
    cache.termQr = "";  // the next hello gives the short label
    cache.termName = S.user;
    saveSettings();
    saveCache();
    S.needHello = true;  // this person's shop, QR and payment point
  }
  if (ok && q.startsWith("action=trip") && doc["trip"].is<JsonObject>()) {
    TripView tv = readTrip(doc["trip"].as<JsonObject>());
    if (tv.on) S.trip = tv;
    else { S.tripEnd = tv; S.trip = TripView(); }
    S.tripChanged = true;
  }
  if (ok && q.startsWith("action=billquote")) {
    S.bill.name = (const char *)(doc["name"] | "");
    S.bill.account = (const char *)(doc["account"] | "");
    S.bill.customer = (const char *)(doc["customer"] | "");
    S.bill.amount = lround((double)(doc["amount"] | 0.0));
    S.bill.commission = doc["commission"] | 0.0;
    S.bill.cardFee = lround((double)(doc["cardFee"] | 0.0));
    S.bill.ref = ""; S.bill.result = "";
  }
  if (ok && q.startsWith("action=billpay")) {
    S.bill.ref = (const char *)(doc["ref"] | "");
    S.bill.result = (const char *)(doc["result"] | "");
    S.bill.commission = doc["commission"] | S.bill.commission;
  }
  if (ok && q.startsWith("action=billrest")) {
    S.sale.billStatus = (const char *)(doc["result"] | "");
    S.sale.billRef = (const char *)(doc["ref"] | "");
    S.sale.billShort = 0;
  }
  if (ok && q.startsWith("action=today")) {
    S.today.clear();
    for (JsonObject x : doc["sales"].as<JsonArray>()) {
      TodayItem t;
      t.number = x["number"] | 0L;
      t.amount = lround((double)(x["amount"] | 0.0));
      t.status = (const char *)(x["status"] | "");
      S.today.push_back(t);
    }
  }
  S.req.pending = false;
  S.req.done = true;
  S.lastPollMs = 0;  // poll at once so the sale's new status shows
}

void netTask(void *) {
  for (;;) {
    if (WiFi.status() != WL_CONNECTED || cfg.url.isEmpty()) {
      { Lock l; S.online = false; }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    {
      Lock l;
      if (S.signedOut) {
        S.signedOut = false;
        cfg.token = "";
        S.user = "";
        cache.termId = "";
        cache.termQr = "";
        saveSettings();
        saveCache();
        S.needHello = true;
      }
    }
    bool hello;
    { Lock l; hello = S.needHello; }
    if (hello) { netHello(); vTaskDelay(pdMS_TO_TICKS(hello && !S.helloDone ? 5000 : 200)); continue; }
    netOps();
    netReq();
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

// ---------------------------------------------------------------- audio (GPIO26 DAC, 16 kHz or the rate in /rate.txt, 8-bit clips in flash)
static uint8_t *abuf = nullptr;
static const uint32_t ABUF_MAX = 48000;  // 3 seconds at 16 kHz
static volatile uint32_t alen = 0, apos = 0;
static uint32_t abufCap = 0;
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
  abufCap = abuf ? ABUF_MAX : 0;
#ifdef BOX_AUDIO_EN
  pinMode(BOX_AUDIO_EN, OUTPUT);
#endif
  ampOn(false);
  dacWrite(26, 128);
  // The clips' sample rate comes with them (rate.txt, written by tools/make_voice.py).
  uint32_t rate = 12000;
  File rf = LittleFS.open("/rate.txt", "r");
  if (rf) { uint32_t r = (uint32_t)rf.readString().toInt(); rf.close(); if (r >= 8000 && r <= 24000) rate = r; }
  if (!abuf) { abuf = (uint8_t *)malloc(24000); abufCap = abuf ? 24000 : 0; }  // low memory: shorter clips still play
  atimer = timerBegin(0, 4, true);  // 80 MHz / 4 = 20 MHz
  timerAttachInterrupt(atimer, &onAudioTick, true);
  timerAlarmWrite(atimer, 20000000 / rate, true);
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
  uint32_t n = f.read(abuf, abufCap);
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
enum Screen { SCR_HOME, SCR_QR, SCR_RESULT, SCR_SETTINGS, SCR_WAITER, SCR_TIP,
              SCR_MORE, SCR_DIGITS, SCR_INCOMING, SCR_CONFIRM, SCR_TODAY, SCR_SIGNIN,
              SCR_MENU, SCR_BILLERS, SCR_BILLACCT, SCR_BILLQUOTE, SCR_BILLDONE,
              SCR_TRIPSET, SCR_TRIP, SCR_TRIPEND, SCR_TRIPCAR };
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
// Cashier tools for the sale on screen.
bool showCard = false;          // the QR shows the card page instead of the wallet code
bool wantCard = false;          // Card was tapped before the server knew the sale: open it when it does
bool showTransfer = false;      // the number to transfer to, InstaPay and USSD codes
std::string walletQr;           // the sale's wallet (Meeza) code
enum Entry { EN_CLAIM, EN_BUYER, EN_CASHTIP };
Entry entryKind = EN_CLAIM;
String entry = "";
unsigned entryMax = 3;
String note = "";               // a short message under the title (digits, incoming, QR)
uint32_t noteUntil = 0;
int pick = -1;                  // the incoming payment chosen to attach
Screen afterReq = SCR_QR;       // where to go when a request is answered

void setNote(const String &n, uint32_t ms = 6000) { note = n; noteUntil = millis() + ms; }
bool noteOn() { return note.length() && millis() < noteUntil; }

// Sends one cashier request; the answer is handled in loop().
bool sendReq(const String &q) {
  Lock l;
  if (S.req.pending) return false;
  S.req = Req();
  S.req.query = q;
  S.req.pending = true;
  return true;
}
bool reqBusy() { Lock l; return S.req.pending; }

bool tipsOn() {
  if (!RESTAURANT) return false;
  Lock l;
  return S.tips;
}

const char *KEYS[4][4] = {{"1", "2", "3", "Del"}, {"4", "5", "6", "Clear"}, {"7", "8", "9", ""}, {"00", "0", "Menu", ""}};
const char *DKEYS[4][4] = {{"1", "2", "3", "Del"}, {"4", "5", "6", "Clear"}, {"7", "8", "9", ""}, {"", "0", "", ""}};
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
  String user;
  { Lock l; user = S.user; }
  tft.fillCircle(240, 15, 5, online ? C_GREEN : C_RED);
  tft.drawString(!online ? String("Offline") : user.length() ? user.substring(0, 9) : String("Online"), 312, 15, 2);
}

bool needSignIn() {
  Lock l;
  return S.signin && cfg.token.isEmpty() && S.helloDone;
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
  button(BTN_QR, "Pay", ok ? C_ORANGE : C_LINE, TFT_WHITE, 4);
}

void drawAmountOnly() {
  tft.fillRect(0, 31, 290, 50, C_BG);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(C_INK, C_BG);
  tft.drawString(typed.length() ? money(typed.toInt()) : String("0"), 270, 58, 6);
  bool ok = typed.toInt() > 0;
  button(BTN_QR, "Pay", ok ? C_ORANGE : C_LINE, TFT_WHITE, 4);
}

std::string qrText;
// Payment screen: three tabs on top (Wallet, Card, InstaPay), the code or the number on the left, the amount on the right.
const Btn TAB_W{0, 0, 106, 30}, TAB_C{107, 0, 106, 30}, TAB_I{214, 0, 106, 30};
const Btn BTN_MORE{238, 124, 78, 30}, BTN_CANCEL{238, 202, 78, 34}, BTN_DIGITS{8, 112, 218, 40};

String cardUrl() {
  String id;
  { Lock l; id = S.sale.id; }
  return id.length() ? cfg.url + "?page=pay&sale=" + id : String("");
}

// The "Transfer" view: the wallet number, InstaPay and the USSD codes with the amount.
void drawTransfer(long amount) {
  String num, claim;
  { Lock l; num = S.wallet; }
  tft.setTextColor(C_INK, TFT_WHITE);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Send " + money(amount) + " EGP by InstaPay to:", 8, 38, 2);
  String spaced = num.length() == 11 ? num.substring(0, 4) + " " + num.substring(4, 7) + " " + num.substring(7) : num;
  tft.drawString(num.length() ? spaced : String("(number not set)"), 8, 58, 4);
  tft.setTextColor(C_MUTED, TFT_WHITE);
  tft.drawString("Orange Cash wallet", 8, 88, 2);
  button(BTN_DIGITS, "His last 3 digits", C_ORANGE, TFT_WHITE, 2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_MUTED, TFT_WHITE);
  tft.drawString("No smartphone? Dial:", 8, 160, 2);
  tft.setTextColor(C_INK, TFT_WHITE);
  tft.drawString("Vodafone *9*7*" + num + "*" + String(amount) + "#", 8, 178, 2);
  tft.drawString("Orange #115# > Transfer", 8, 196, 2);
  tft.drawString("e& *777*1# > number > amount", 8, 214, 2);
}

void drawTabs() {
  bool w = !showCard && !showTransfer;
  button(TAB_W, "Wallet", w ? C_ORANGE : C_KEY, w ? TFT_WHITE : C_INK, 2);
  button(TAB_C, "Card", showCard ? C_ORANGE : C_KEY, showCard ? TFT_WHITE : C_INK, 2);
  button(TAB_I, "InstaPay", showTransfer ? C_ORANGE : C_KEY, showTransfer ? TFT_WHITE : C_INK, 2);
}

void drawIncomingBtn();
void drawQr() {
  static uint8_t q[hpqr_BUFFER_LEN_FOR_VERSION(14)], tmp[hpqr_BUFFER_LEN_FOR_VERSION(14)];
  tft.fillScreen(C_BG);
  tft.fillRect(0, 32, 234, 208, TFT_WHITE);
  long amount0;
  { Lock l; amount0 = S.sale.amount; }
  if (showTransfer) { drawTransfer(amount0); }
  String card = showCard ? cardUrl() : String("");
  if (showCard && card.length()) qrText = card.c_str();
  else { showCard = false; qrText = walletQr; }
  bool ok = !showTransfer && hpqr_encodeText(qrText.c_str(), tmp, q, hpqr_Ecc_MEDIUM, hpqr_VERSION_MIN, 14, hpqr_Mask_AUTO, true);
  if (showTransfer) {
  } else if (ok) {
    int n = hpqr_getSize(q);
    int scale = 200 / (n + 4);
    if (scale < 1) scale = 1;
    int size = n * scale, ox = (234 - size) / 2, oy = 32 + (208 - size) / 2;
    for (int y = 0; y < n; y++)
      for (int x = 0; x < n; x++)
        if (hpqr_getModule(q, x, y)) tft.fillRect(ox + x * scale, oy + y * scale, scale, scale, TFT_BLACK);
  } else {
    tft.setTextColor(C_RED, TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("QR too long", 117, 136, 2);
  }
  drawTabs();
  long amount, number, tip;
  { Lock l; amount = S.sale.amount; number = S.sale.number; tip = S.sale.tip; }
  tft.setTextColor(C_MUTED, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Total", 240, 36, 2);
  tft.setTextColor(C_INK, C_BG);
  tft.drawString(money(amount), 240, 52, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("EGP", 240, 78, 2);
  tft.drawString("Bill " + String(number), 240, 94, 2);
  if (tip > 0) { tft.setTextColor(C_GREEN_DARK, C_BG); tft.drawString("Tip " + money(tip), 240, 108, 1); tft.setTextColor(C_MUTED, C_BG); }
  drawIncomingBtn();
  // A cashier message, in three short lines under More.
  if (noteOn()) {
    tft.setTextColor(C_RED, C_BG);
    tft.setTextDatum(TL_DATUM);
    for (int i = 0; i < 3; i++) {
      String part = note.substring(i * 13, i * 13 + 13);
      if (part.length()) tft.drawString(part, 238, 158 + i * 9, 1);
    }
  }
  button(BTN_CANCEL, "Cancel", C_KEY, C_INK, 2);
}

// "Check (n)": shown only when transfers that could belong to this sale are waiting
// for the cashier (two same-amount payments, or a manual wallet or USSD transfer).
// Payments that match by themselves are announced and need nothing.
void drawIncomingBtn() {
  size_t n;
  { Lock l; n = S.incoming.size(); }
  tft.fillRect(BTN_MORE.x, BTN_MORE.y, BTN_MORE.w, BTN_MORE.h, C_BG);
  if (n) button(BTN_MORE, ("Check " + String(n)).c_str(), C_ORANGE, TFT_WHITE, 2);
}

void drawQrStatus() {
  bool reg, online;
  String status;
  { Lock l; reg = S.sale.registered; online = S.online; status = S.sale.status; }
  tft.fillRect(236, 184, 84, 16, C_BG);
  tft.setTextDatum(TL_DATUM);
  uint16_t c = reg ? C_GREEN_DARK : online ? C_MUTED : C_RED;
  tft.setTextColor(c, C_BG);
  String line = status == "refused" ? "Refused" : (status == "screenshot" || status == "approval") ? "Check" : reg ? "Waiting" : online ? "Saving..." : "Offline";
  tft.drawString(line, 240, 185, 2);
}

const Btn RB_EXTRA{4, 196, 152, 40}, RB_CASH{164, 196, 152, 40};
const Btn RB_BILLREST{30, 188, 260, 40};  // a short bill: the rest was collected in cash

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
  String bst, bref, bname;
  { Lock l; bst = S.sale.billStatus; bref = S.sale.billRef; bname = S.sale.billName; }
  if (bst == "short") {
    long sh;
    { Lock l; sh = S.sale.billShort; }
    tft.drawString(bname + " bill not paid: " + money(sh) + " EGP short", 160, 172, 2);
    button(RB_BILLREST, reqBusy() ? "Paying..." : ("Collected " + money(sh) + " cash: pay bill").c_str(), TFT_WHITE, C_INK, 2);
    if (noteOn()) tft.drawString(note.substring(0, 40), 160, 128, 2);
    return;
  }
  if (bst.length()) {
    tft.drawString(bst == "paid" ? bname + " bill paid  ref " + bref : bst == "waiting" ? String("Paying the bill...") : bname + ": not paid, see the owner", 160, 185, 2);
    tft.drawString("Tap for a new sale", 160, 222, 2);
    return;
  }
  if (tipsOn()) {
    // Restaurant: the extra can be his tip, and a cash tip can be recorded.
    if (diff < 0) tft.drawString("Still owed " + money(-diff) + " EGP", 160, 174, 2);
    else if (diff > 0) tft.drawString(money(diff) + " EGP more than the bill", 160, 174, 2);
    else if (tip > 0) tft.drawString("Includes a tip of " + money(tip) + " EGP. Thank you!", 160, 174, 2);
    if (diff > 0) button(RB_EXTRA, ("Extra " + money(diff) + " is a tip").c_str(), TFT_WHITE, C_INK, 2);
    button(RB_CASH, "Cash tip", TFT_WHITE, C_INK, 2);
    if (noteOn()) tft.drawString(note.substring(0, 40), 160, 128, 2);
    return;
  }
  if (diff < 0) tft.drawString("Still owed " + money(-diff) + " EGP", 160, 185, 4);
  else if (diff > 0) tft.drawString(money(diff) + " EGP more than the bill", 160, 185, 2);
  else if (tip > 0) tft.drawString("Includes a tip of " + money(tip) + " EGP. Thank you!", 160, 185, 2);
  tft.drawString("Tap for a new sale", 160, 222, 2);
}

// Settings
const Btn SB_WIFI{8, 36, 148, 36}, SB_CAL{164, 36, 148, 36}, SB_INV{8, 78, 148, 36}, SB_VOL{164, 78, 148, 36},
    SB_TEST{8, 120, 148, 36}, SB_USER{164, 120, 148, 36}, SB_BACK{8, 160, 304, 32};

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
  String u;
  { Lock l; u = S.user; }
  button(SB_USER, u.length() ? ("Log out " + u.substring(0, 9)).c_str() : "Log out", C_KEY, C_INK, 2);
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
Btn tipBtn(int i) { return i == 0 ? Btn{8, 168, 304, 66} : Btn{8 + (i - 1) * 104, 94, 96, 66}; }
const Btn TB_BACK{232, 0, 88, 30};

void tipButton(int i, const String &big, const String &small) {
  Btn b = tipBtn(i);
  bool none = i == 0;
  uint16_t bg = none ? C_KEY : C_ORANGE, fg = none ? C_INK : TFT_WHITE;
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 10, bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(big, b.x + b.w / 2, small.length() ? b.y + 22 : b.y + b.h / 2, 4);
  if (small.length()) tft.drawString(small, b.x + b.w / 2, b.y + 50, 2);
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
  tipButton(0, "No thanks", "");
  for (int i = 0; i < 3; i++) tipButton(i + 1, "+" + money(tipChoice[i]), "Total " + money(billAmount + tipChoice[i]));
}

// Cashier tools: a dark title bar with Back on the right.
void titleBar(const String &t) {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 30, C_INK);
  tft.setTextColor(TFT_WHITE, C_INK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(t, 8, 15, 2);
  tft.setTextDatum(MR_DATUM);
  tft.drawString("Back", 312, 15, 2);
}
const Btn BTN_BACKBAR{232, 0, 88, 30};
Btn rowBtn(int i) { return Btn{4, 36 + i * 50, 312, 44}; }

void drawMore() {
  long number, amount;
  size_t inc;
  { Lock l; number = S.sale.number; amount = S.sale.amount; inc = S.incoming.size(); }
  titleBar("Bill " + String(number) + "  " + money(amount) + " EGP");
  button(rowBtn(0), "InstaPay: his last 3 digits", C_KEY, C_INK, 2);
  button(rowBtn(1), ("Incoming payments (" + String(inc) + ")").c_str(), C_KEY, C_INK, 2);
  button(rowBtn(2), "Back to payment", C_ORANGE, TFT_WHITE, 2);
}

String entryTitle() {
  if (entryKind == EN_CASHTIP) return "Cash tip (EGP)";
  if (entryKind == EN_BUYER) return "Buyer's mobile (11 digits)";
  return entryMax == 11 ? "His full mobile number" : "Last 3 digits of his number";
}

void drawEntryOnly() {
  tft.fillRect(0, 31, 320, 52, C_BG);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_INK, C_BG);
  tft.drawString(entry.length() ? entry : String("-"), 8, 50, 4);
  if (noteOn()) { tft.setTextColor(C_RED, C_BG); tft.drawString(note.substring(0, 44), 8, 74, 2); }
  else if (reqBusy()) { tft.setTextColor(C_MUTED, C_BG); tft.drawString("Checking...", 8, 74, 2); }
}

void drawDigits() {
  titleBar(entryTitle());
  drawEntryOnly();
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      if (!DKEYS[r][c][0]) continue;
      button(keyBtn(r, c), DKEYS[r][c], C_KEY, C_INK, strlen(DKEYS[r][c]) > 2 ? 2 : 4);
    }
  bool ok = entryKind == EN_CASHTIP ? entry.toInt() > 0 : entry.length() == entryMax;
  button(BTN_QR, "OK", ok ? C_ORANGE : C_LINE, TFT_WHITE, 4);
}

void drawIncoming() {
  std::vector<InItem> inc;
  String mode;
  { Lock l; inc = S.incoming; mode = S.mode; }
  titleBar("Incoming: tap the right one");
  tft.setTextDatum(ML_DATUM);
  if (inc.empty()) {
    tft.setTextColor(C_MUTED, C_BG);
    tft.drawString("Nothing to check now.", 8, 60, 2);
    tft.drawString("Tap the bar above to go back.", 8, 78, 2);
  }
  for (size_t i = 0; i < inc.size() && i < 4; i++) {
    Btn b = rowBtn(i);
    uint16_t bg = inc[i].match ? 0xCFF9 : C_KEY;
    tft.fillRoundRect(b.x, b.y, b.w, b.h, 8, bg);
    tft.setTextColor(C_INK, bg);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(money(inc[i].amount) + " EGP", b.x + 10, b.y + b.h / 2, 4);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(inc[i].label + (inc[i].from3.length() ? "  ..." + inc[i].from3 : String("")), b.x + b.w - 10, b.y + b.h / 2, 2);
  }
  if (mode == "mixed") {
    tft.setTextColor(C_MUTED, C_BG);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("InstaPay: use the InstaPay tab > his last 3 digits", 8, 232, 1);
  }
}

const Btn CB_OK{8, 180, 148, 50}, CB_BACK{164, 180, 148, 50};
void drawConfirm() {
  InItem it;
  long number, amount;
  {
    Lock l;
    number = S.sale.number; amount = S.sale.amount;
    if (pick < 0 || pick >= (int)S.incoming.size()) { pick = -1; return; }
    it = S.incoming[pick];
  }
  titleBar("Attach this payment?");
  tft.setTextColor(C_INK, C_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(money(it.amount) + " EGP", 8, 58, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(it.label + (it.from3.length() ? "  from ..." + it.from3 : String("")), 8, 86, 2);
  tft.drawString("to bill " + String(number) + " (" + money(amount) + " EGP)", 8, 106, 2);
  long d = it.amount - amount;
  tft.setTextColor(d == 0 ? C_GREEN_DARK : C_INK, C_BG);
  tft.drawString(d == 0 ? String("Same amount") : d > 0 ? money(d) + " EGP more than the bill" : money(-d) + " EGP short", 8, 132, 2);
  if (noteOn()) { tft.setTextColor(C_RED, C_BG); tft.drawString(note.substring(0, 44), 8, 156, 2); }
  button(CB_OK, reqBusy() ? "Wait..." : "Confirm", C_ORANGE, TFT_WHITE, 4);
  button(CB_BACK, "Back", C_KEY, C_INK, 4);
}

void drawToday() {
  std::vector<TodayItem> t;
  { Lock l; t = S.today; }
  titleBar("Today on this counter");
  tft.setTextDatum(ML_DATUM);
  if (reqBusy()) { tft.setTextColor(C_MUTED, C_BG); tft.drawString("Loading...", 8, 60, 2); return; }
  if (t.empty()) { tft.setTextColor(C_MUTED, C_BG); tft.drawString("No sales yet today.", 8, 60, 2); return; }
  for (size_t i = 0; i < t.size() && i < 6; i++) {
    int y = 48 + i * 32;
    String st = t[i].status;
    uint16_t c = st == "paid" ? C_GREEN_DARK : (st == "waiting" || st == "screenshot" || st == "approval") ? C_AMBER : C_MUTED;
    tft.setTextColor(C_INK, C_BG);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("Bill " + String(t[i].number), 8, y, 2);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(money(t[i].amount) + " EGP", 220, y, 4);
    tft.setTextColor(c, C_BG);
    tft.drawString(st == "paid" ? "Paid" : st == "waiting" ? "Waiting" : st == "expired" ? "Expired" : st == "cancelled" ? "Cancelled" : "Check", 312, y, 2);
  }
}

// Login: mobile number, then PIN, then the code sent by SMS (demo: 123456).
// No device setup: the person's payments follow them to this box.
enum LoginStep { LS_MOBILE, LS_PIN, LS_CODE };
LoginStep loginStep = LS_MOBILE;
String pinTyped = "", loginMobile = "";
unsigned loginMax() { return loginStep == LS_MOBILE ? 11 : loginStep == LS_PIN ? 4 : 6; }
void drawPinOnly() {
  tft.fillRect(0, 31, 320, 52, C_BG);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_INK, C_BG);
  String shown = pinTyped;
  if (loginStep == LS_PIN) { shown = ""; for (unsigned i = 0; i < 4; i++) shown += i < pinTyped.length() ? "* " : "_ "; }
  tft.drawString(shown.length() ? shown : String("-"), 8, 50, 4);
  String sentTo;
  { Lock l; sentTo = S.sentTo; }
  if (noteOn()) { tft.setTextColor(C_RED, C_BG); tft.drawString(note.substring(0, 44), 8, 74, 2); }
  else if (reqBusy()) { tft.setTextColor(C_MUTED, C_BG); tft.drawString("Checking...", 8, 74, 2); }
  else {
    tft.setTextColor(C_MUTED, C_BG);
    tft.drawString(loginStep == LS_CODE ? "Code sent to " + sentTo + " (demo 123456)" : String("Payments are still announced"), 8, 74, 2);
  }
  bool ok = pinTyped.length() == loginMax();
  button(BTN_QR, "OK", ok ? C_ORANGE : C_LINE, TFT_WHITE, 4);
}
void drawSignIn() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 30, C_ORANGE);
  tft.setTextColor(TFT_WHITE, C_ORANGE);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(loginStep == LS_MOBILE ? "Log in: your mobile number" : loginStep == LS_PIN ? "Log in: your PIN" : "Log in: the SMS code", 8, 15, 2);
  if (loginStep != LS_MOBILE) { tft.setTextDatum(MR_DATUM); tft.drawString("Back", 312, 15, 2); }
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      if (!DKEYS[r][c][0]) continue;
      button(keyBtn(r, c), DKEYS[r][c], C_KEY, C_INK, strlen(DKEYS[r][c]) > 2 ? 2 : 4);
    }
  drawPinOnly();
}

// ---------------------------------------------------------------- bills and recharges (v0.9.0)
// The customer pays the shop; the bill is paid from the shop's wallet and the
// shop earns a commission. Demo billers only: nothing real is paid.
int billerPick = -1;
int billStep = 0;            // 0: account number, 1: recharge amount
String billAcct = "", billAmt = "";
const Btn QB_CASH{8, 150, 148, 50}, QB_QR{164, 150, 148, 50};
Btn billerBtn(int i) { return Btn{4 + (i % 2) * 158, 36 + (i / 2) * 50, 154, 44}; }

void drawMenu() {
  titleBar("Menu");
  bool tripOn;
  { Lock l; tripOn = S.trip.on; }
  button(rowBtn(0), "Bills and recharges", C_ORANGE, TFT_WHITE, 4);
  button(rowBtn(1), tripOn ? "Trip mode (running)" : "Trip mode (microbus)", C_KEY, C_INK, 2);
  button(rowBtn(2), "Today's sales on this counter", C_KEY, C_INK, 2);
  button(rowBtn(3), "Back", C_KEY, C_INK, 2);
}
void drawBillers() {
  std::vector<BillerItem> list;
  { Lock l; list = S.billers; }
  titleBar("Bills and recharges");
  if (list.empty()) {
    tft.setTextColor(C_MUTED, C_BG);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("Loading the services... tap Back", 8, 60, 2);
    return;
  }
  for (size_t i = 0; i < list.size() && i < 6; i++) button(billerBtn(i), list[i].name.c_str(), C_KEY, C_INK, 2);
  tft.setTextColor(C_MUTED, C_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Demo: simulated services. No real bill is paid.", 8, 196, 1);
}
BillerItem billerNow() {
  Lock l;
  if (billerPick < 0 || billerPick >= (int)S.billers.size()) return BillerItem();
  return S.billers[billerPick];
}
String billEntry() { return billStep == 0 ? billAcct : billAmt; }
bool billEntryOk() {
  BillerItem b = billerNow();
  if (billStep == 1) { long a = billAmt.toInt(); return a >= 5 && a <= 1000; }
  return (int)billAcct.length() >= b.min && (int)billAcct.length() <= b.max;
}
void drawBillEntryOnly() {
  tft.fillRect(0, 31, 320, 52, C_BG);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_INK, C_BG);
  String e = billEntry();
  tft.drawString(e.length() ? e : String("-"), 8, 50, 4);
  if (noteOn()) { tft.setTextColor(C_RED, C_BG); tft.drawString(note.substring(0, 44), 8, 74, 2); }
  else if (reqBusy()) { tft.setTextColor(C_MUTED, C_BG); tft.drawString("Checking the bill...", 8, 74, 2); }
  else {
    BillerItem b = billerNow();
    tft.setTextColor(C_MUTED, C_BG);
    tft.drawString(billStep == 1 ? String("5 to 1,000 EGP") : b.min == b.max ? String(b.min) + " digits" : String(b.min) + " to " + String(b.max) + " digits", 8, 74, 2);
  }
  button(BTN_QR, "OK", billEntryOk() ? C_ORANGE : C_LINE, TFT_WHITE, 4);
}
void drawBillAcct() {
  BillerItem b = billerNow();
  titleBar(b.name + ": " + (billStep == 1 ? String("amount (EGP)") : b.field));
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      if (!DKEYS[r][c][0]) continue;
      button(keyBtn(r, c), DKEYS[r][c], C_KEY, C_INK, strlen(DKEYS[r][c]) > 2 ? 2 : 4);
    }
  drawBillEntryOnly();
}
void drawBillQuote() {
  BillView v;
  { Lock l; v = S.bill; }
  titleBar(v.name);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(v.account + "  " + v.customer, 8, 46, 2);
  tft.setTextColor(C_INK, C_BG);
  tft.drawString(money(v.amount), 8, 86, 6);
  tft.drawString("EGP", 16 + tft.textWidth(money(v.amount), 6), 98, 4);
  tft.setTextColor(C_GREEN_DARK, C_BG);
  tft.drawString("The shop earns " + String(v.commission, v.commission == (long)v.commission ? 0 : 2) + " EGP", 8, 124, 2);
  if (noteOn()) { tft.setTextColor(C_RED, C_BG); tft.drawString(note.substring(0, 44), 8, 140, 2); }
  button(QB_CASH, reqBusy() ? "Paying..." : "Cash", C_ORANGE, TFT_WHITE, 4);
  button(QB_QR, "QR / card", C_KEY, C_INK, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Cash: paid now from the shop's wallet.", 8, 210, 1);
  tft.drawString("QR / InstaPay: no fee. Card: +" + String(v.cardFee) + " EGP card fee.", 8, 224, 1);
}
void drawBillDone() {
  BillView v;
  { Lock l; v = S.bill; }
  bool ok = v.result == "paid";
  uint16_t bg = ok ? C_GREEN : C_AMBER;
  tft.fillScreen(bg);
  tft.setTextColor(TFT_WHITE, bg);
  if (ok) drawTick(56, 70, 70, TFT_WHITE);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(ok ? "Bill paid" : "Not paid", 110, 40, 4);
  tft.drawString(money(v.amount), 110, 90, 6);
  tft.drawString("EGP", 110 + tft.textWidth(money(v.amount), 6) + 8, 102, 4);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(v.name + "  " + v.account, 160, 140, 2);
  if (ok) {
    tft.drawString("Ref " + v.ref, 160, 162, 4);
    tft.drawString("The shop earned " + String(v.commission, v.commission == (long)v.commission ? 0 : 2) + " EGP", 160, 190, 2);
  } else {
    tft.drawString("The shop's wallet is too low", 160, 170, 2);
  }
  tft.drawString("Tap to continue", 160, 222, 2);
}

// ---------------------------------------------------------------- trip mode (v0.10.0)
// Intercity microbus: the driver sets the seats and the fare, starts a trip,
// and the box counts the seats paid by transfer plus the cash seats he adds.
CarItem carByCode(const String &code);  // below: the chosen car's saved settings
int tripStep = 0;  // 0: seats, 1: fare
String tripSeats = "", tripFare = "", tripCar = "";
const Btn TB_CASH{4, 180, 104, 54}, TB_MINUS{112, 180, 56, 54}, TB_CHECK{172, 180, 70, 54}, TB_END{246, 180, 70, 54};
const Btn TE_NEW{8, 184, 148, 50}, TE_HOME{164, 184, 148, 50};

bool tripEntryOk() {
  long v = (tripStep == 0 ? tripSeats : tripFare).toInt();
  return tripStep == 0 ? (v >= 1 && v <= 60) : (v >= 1 && v <= 2000);
}
void drawTripEntryOnly() {
  tft.fillRect(0, 31, 320, 52, C_BG);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_INK, C_BG);
  String e = tripStep == 0 ? tripSeats : tripFare;
  tft.drawString(e.length() ? e : String("-"), 8, 50, 4);
  if (noteOn()) { tft.setTextColor(C_RED, C_BG); tft.drawString(note.substring(0, 44), 8, 74, 2); }
  else {
    bool fixed = carByCode(tripCar).locked;
    tft.setTextColor(C_MUTED, C_BG);
    tft.drawString(tripStep == 0 ? (fixed ? "Seats. Fare fixed: " + tripFare + " EGP" : String("Passenger seats, 1 to 60")) : String("Fare per seat, EGP"), 8, 74, 2);
  }
  button(BTN_QR, tripStep == 0 && !carByCode(tripCar).locked ? "Next" : "Start", tripEntryOk() ? C_ORANGE : C_LINE, TFT_WHITE, 4);
}
void drawTripSet() {
  titleBar(tripStep == 0 ? "New trip: seats" : "New trip: fare (EGP)");
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      if (!DKEYS[r][c][0]) continue;
      button(keyBtn(r, c), DKEYS[r][c], C_KEY, C_INK, strlen(DKEYS[r][c]) > 2 ? 2 : 4);
    }
  drawTripEntryOnly();
}
void drawTrip() {
  TripView t;
  { Lock l; t = S.trip; }
  titleBar((t.carName.length() ? t.carName + "  " : String("Trip  ")) + String(t.seats) + " x " + money(t.fare) + " EGP");
  long total = t.paid + t.cash;
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_INK, C_BG);
  tft.drawString(String(total), 8, 66, 6);
  tft.drawString("/ " + String(t.seats) + " paid", 16 + tft.textWidth(String(total), 6), 74, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("Transfer " + String(t.paid - t.card) + (t.card ? "   Card " + String(t.card) : String("")) + "   Cash " + String(t.cash) + (t.partial ? "   Part " + String(t.partial) : String("")), 8, 106, 2);
  if (t.missing > 0) { tft.setTextColor(C_RED, C_BG); tft.drawString(String(t.missing) + " not paid", 8, 140, 4); }
  else if (t.over > 0) { tft.setTextColor(C_AMBER, C_BG); tft.drawString("All paid, " + String(t.over) + " extra", 8, 140, 4); }
  else { tft.setTextColor(C_GREEN_DARK, C_BG); tft.drawString("All paid", 8, 140, 4); }
  if (noteOn()) { tft.setTextColor(C_RED, C_BG); tft.drawString(note.substring(0, 44), 8, 164, 2); }
  else if (t.unassigned > 0) {
    tft.setTextColor(C_AMBER, C_BG);
    tft.drawString(String(t.unassigned) + " transfer(s): which car? Ask", 8, 164, 2);
  }
  button(TB_CASH, "+1 cash", C_ORANGE, TFT_WHITE, 4);
  button(TB_MINUS, "-1", C_KEY, C_INK, 4);
  button(TB_CHECK, "Check", C_KEY, C_INK, 2);
  button(TB_END, "End", C_KEY, C_INK, 2);
}
void drawTripEnd() {
  TripView t;
  { Lock l; t = S.tripEnd; }
  uint16_t bg = t.missing > 0 ? C_AMBER : C_GREEN;
  tft.fillScreen(bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Trip ended", 160, 30, 4);
  tft.drawString(String(t.paid + t.cash) + " of " + String(t.seats) + " paid", 160, 74, 4);
  tft.drawString("Transfer " + String(t.paid - t.card) + (t.card ? "   Card " + String(t.card) : String("")) + "   Cash " + String(t.cash), 160, 106, 2);
  tft.drawString(t.missing > 0 ? String(t.missing) + " not paid" : String("All paid"), 160, 142, 4);

  button(TE_NEW, "New trip", C_ORANGE, TFT_WHITE, 4);
  button(TE_HOME, "Home", TFT_WHITE, C_INK, 4);
}
// What to say when the driver checks or ends: how many haven't paid.
std::vector<std::string> tripVoice(const TripView &t) { return words::tripLeft(t.missing, true); }
// The chosen car's saved settings fill the trip; a fixed fare can't be changed.
CarItem carByCode(const String &code) {
  Lock l;
  for (auto &c : S.cars) if (c.code == code) return c;
  return CarItem();
}
void applyCar() {
  CarItem c = carByCode(tripCar);
  if (c.seats > 0) tripSeats = String(c.seats);
  if (c.fare > 0) tripFare = String(c.fare);
}
void drawTripCar() {
  std::vector<CarItem> cars;
  { Lock l; cars = S.cars; }
  titleBar("New trip: which car?");
  for (size_t i = 0; i < cars.size() && i < 4; i++)
    button(rowBtn(i), cars[i].name.c_str(), cars[i].code == tripCar ? C_ORANGE : C_KEY, cars[i].code == tripCar ? TFT_WHITE : C_INK, 2);
}
void openTrip() {
  bool on;
  { Lock l; on = S.trip.on; }
  if (on) { screen = SCR_TRIP; }
  else {
    tripStep = 0;
    tripSeats = prefs.getString("tseats", "14");
    tripFare = prefs.getString("tfare", "");
    tripCar = prefs.getString("tcar", "");
    note = "";
    size_t nc;
    { Lock l; nc = S.cars.size(); if (nc == 1) tripCar = S.cars[0].code; }
    if (nc == 1) applyCar();
    screen = nc > 1 ? SCR_TRIPCAR : SCR_TRIPSET;
  }
  dirty = true;
}

void openEntry(Entry k) {
  entryKind = k;
  entry = "";
  entryMax = k == EN_BUYER ? 11 : k == EN_CASHTIP ? 4 : 3;
  note = "";
  screen = SCR_DIGITS;
  dirty = true;
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
  cache.termQr = "";
  saveCache();
  saveSettings();
  ESP.restart();
}

// ---------------------------------------------------------------- sale flow
void startSale(long amount, long tip = -1, const String &waiter = "", const String &biller = "", const String &account = "") {
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
    S.sale.tip = tip > 0 ? tip : 0;
    S.sale.status = "waiting";
    Op op{OP_SALE, number, amount, tip, waiter};
    op.biller = biller;
    op.account = account;
    S.ops.push_back(op);
    S.lastPollMs = millis();
  }
  walletQr = meeza::saleQr(cache.qrBase.c_str(), amount, number,
                           cache.termQr.length() ? cache.termQr.c_str() : cache.termId.length() ? cache.termId.c_str() : "main");
  qrText = walletQr;
  showCard = false;
  wantCard = false;
  showTransfer = false;
  note = "";
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
  bool tripOnNow;
  long tripLeftNow;
  {
    Lock l;
    tripOnNow = S.trip.on;
    tripLeftNow = S.trip.missing;
  }
  for (auto &e : evs) {
    announce(e);
    if (tripOnNow) say(words::tripLeft(tripLeftNow, false));
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

  // Card tapped while the sale was still being saved: switch to it as soon as the server knows the sale.
  if (wantCard && screen == SCR_QR) {
    bool known;
    { Lock l; known = S.sale.id.length() > 0; }
    if (known) { wantCard = false; showCard = true; showTransfer = false; setNote("", 0); dirty = true; }
  }
  if (wantCard && screen != SCR_QR) wantCard = false;

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
    else if (screen == SCR_MORE) drawMore();
    else if (screen == SCR_DIGITS) drawDigits();
    else if (screen == SCR_INCOMING) drawIncoming();
    else if (screen == SCR_CONFIRM) drawConfirm();
    else if (screen == SCR_TODAY) drawToday();
    else if (screen == SCR_SIGNIN) drawSignIn();
    else if (screen == SCR_MENU) drawMenu();
    else if (screen == SCR_BILLERS) drawBillers();
    else if (screen == SCR_BILLACCT) drawBillAcct();
    else if (screen == SCR_BILLQUOTE) drawBillQuote();
    else if (screen == SCR_BILLDONE) drawBillDone();
    else if (screen == SCR_TRIPSET) drawTripSet();
    else if (screen == SCR_TRIP) drawTrip();
    else if (screen == SCR_TRIPEND) drawTripEnd();
    else if (screen == SCR_TRIPCAR) drawTripCar();
  }

  // A cashier request was answered.
  bool rdone, rok;
  String rres, rmsg, rq;
  {
    Lock l;
    rdone = S.req.done; rok = S.req.ok; rres = S.req.result; rmsg = S.req.msg; rq = S.req.query;
    if (rdone) S.req.done = false;
  }
  if (rdone) {
    if (!rok) {
      setNote(rmsg.length() ? rmsg : String("Not sent: check the connection"));
      if (rq.startsWith("action=billpay") && rmsg.indexOf("Not enough") >= 0) setNote("Wallet too low: use QR / card, or the owner", 10000);
      if (rq.startsWith("action=login")) pinTyped = "";
      if (rq.startsWith("action=login2") && rmsg.indexOf("start again") >= 0) loginStep = LS_MOBILE;
    } else if (rq.startsWith("action=claim")) {
      if (rres == "paid") { setNote("Found. Confirming...", 3000); screen = SCR_QR; }
      else if (rres == "need_full") { entryMax = 11; entry = ""; setNote("Two numbers end the same: enter his full number"); }
      else if (rres == "locked") { entryMax = 11; entry = ""; setNote("3 tries: owner alerted. Full number needed"); }
      else { setNote("Linked ..." + entry.substring(entry.length() > 3 ? entry.length() - 3 : 0) + ". Paid when it arrives", 8000); screen = SCR_QR; }
    } else if (rq.startsWith("action=attach")) {
      setNote("Attached. Confirming...", 3000);
      screen = SCR_QR;
    } else if (rq.startsWith("action=buyer")) {
      setNote(rres == "paid" ? String("Found his payment. Confirming...") : String("Saved: his transfer will match this bill"), 6000);
      screen = SCR_QR;
    } else if (rq.startsWith("action=login1")) {
      pinTyped = "";
      loginStep = LS_CODE;
    } else if (rq.startsWith("action=login2")) {
      String u;
      { Lock l; u = S.user; }
      pinTyped = ""; loginStep = LS_MOBILE; loginMobile = "";
      toast = "Logged in: " + u; toastUntil = millis() + 4000;
      screen = SCR_HOME;
    } else if (rq.startsWith("action=logout")) {
      pinTyped = ""; loginStep = LS_MOBILE;
      screen = SCR_SIGNIN;
    } else if (rq.startsWith("action=trip&op=start")) {
      note = "";
      screen = SCR_TRIP;
    } else if (rq.startsWith("action=trip&op=end")) {
      TripView t;
      { Lock l; t = S.tripEnd; }
      say(tripVoice(t));
      screen = SCR_TRIPEND;
    } else if (rq.startsWith("action=trip")) {
      // +1 / -1 cash: the trip screen redraws below
    } else if (rq.startsWith("action=billrest")) {
      setNote("Bill paid. Thank you!", 4000);
      resultSince = millis();
    } else if (rq.startsWith("action=billquote")) {
      note = "";
      screen = SCR_BILLQUOTE;
    } else if (rq.startsWith("action=billpay")) {
      screen = SCR_BILLDONE;
      resultSince = millis();
    } else if (rq.startsWith("action=tip")) {
      Lock l;
      if (rq.indexOf("kind=extra") > 0) { S.sale.tip += S.sale.diff; S.sale.diff = 0; }
      note = "Tip recorded. Thank you!"; noteUntil = millis() + 6000;
      screen = SCR_RESULT;
      resultSince = millis();
    }
    dirty = true;
  }
  // Sign-in needed: the home screen becomes the PIN pad (and back once signed in).
  if (screen == SCR_HOME && needSignIn()) { screen = SCR_SIGNIN; pinTyped = ""; loginStep = LS_MOBILE; dirty = true; }
  if (screen == SCR_SIGNIN && !needSignIn() && !reqBusy()) { screen = SCR_HOME; dirty = true; }
  // Notes expire: redraw once when they do.
  static bool noteShown = false;
  if (noteShown && !noteOn() && screen != SCR_HOME) dirty = true;
  noteShown = noteOn();

  // The sale on screen: watch its status (also while a cashier tool is open).
  bool saleTools = screen == SCR_MORE || screen == SCR_INCOMING || screen == SCR_CONFIRM ||
                   (screen == SCR_DIGITS && entryKind != EN_CASHTIP);
  if (saleTools) {
    String st;
    { Lock l; st = S.sale.status; }
    if (st == "paid" || st == "expired" || st == "cancelled" || st == "refused") { screen = SCR_RESULT; resultSince = millis(); dirty = true; }
    // The incoming list changes as payments arrive.
    static size_t lastInc = 99;
    size_t n;
    { Lock l; n = S.incoming.size(); }
    if ((screen == SCR_INCOMING || screen == SCR_MORE) && n != lastInc) dirty = true;
    lastInc = n;
  }
  if (screen == SCR_QR) {
    static size_t lastQrInc = 99;
    size_t n;
    { Lock l; n = S.incoming.size(); }
    if (n != lastQrInc) { drawIncomingBtn(); lastQrInc = n; }
  }
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
  // A transport business (taxi, microbus): open trip mode once, as soon as the home screen shows.
  {
    static bool tripAuto = false;
    if (!tripAuto && screen == SCR_HOME) {
      String k;
      { Lock l; k = S.kind; }
      bool on;
      { Lock l; on = S.tripsOn; }
      if (k == "transport" && on) { tripAuto = true; openTrip(); }
      else if (k == "transport") tripAuto = true;  // taxis only: the box stays on the amount keypad
      else if (k.length()) tripAuto = true;
    }
  }
  {
    bool ch;
    { Lock l; ch = S.tripChanged; S.tripChanged = false; }
    if (ch && (screen == SCR_TRIP || screen == SCR_MENU)) dirty = true;
  }
  if (screen == SCR_RESULT) {
    static String lastBill;
    String b;
    { Lock l; b = S.sale.billStatus + S.sale.billRef; }
    if (b != lastBill) { lastBill = b; dirty = true; }
  }
  if (screen == SCR_BILLDONE && millis() - resultSince > 20000UL) { screen = SCR_HOME; typed = ""; dirty = true; }
  bool billWaits;
  { Lock l; billWaits = S.sale.billStatus == "short"; }
  if (screen == SCR_RESULT && !billWaits && millis() - resultSince > (tipsOn() ? 25000UL : 12000UL)) {
    { Lock l; S.sale = SaleView(); }
    typed = "";
    screen = SCR_HOME;
    dirty = true;
  }

  // Long press on the top bar opens the settings.
  static uint32_t topPressSince = 0;
  int px, py;
  if (readTouch(px, py) && BTN_TOP.hit(px, py) && (screen == SCR_HOME || screen == SCR_SIGNIN)) {
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
        if (!strcmp(k, "Menu")) { screen = SCR_MENU; dirty = true; return; }
        if (!strcmp(k, "Del")) { if (typed.length()) typed.remove(typed.length() - 1); }
        else if (!strcmp(k, "Clear")) typed = "";
        else if (typed.length() + strlen(k) <= 6 && !(typed.isEmpty() && k[0] == '0')) typed += k;
        drawAmountOnly();
        return;
      }
  } else if (screen == SCR_QR) {
    bool known;
    { Lock l; known = S.sale.id.length() > 0; }
    if (BTN_CANCEL.hit(x, y)) {
      cancelSale();
      screen = SCR_HOME;
      dirty = true;
    } else if (TAB_W.hit(x, y)) {
      wantCard = false; showCard = false; showTransfer = false; dirty = true;
    } else if (TAB_C.hit(x, y)) {
      if (!known) { wantCard = true; showTransfer = false; setNote("Preparing the card page...", 8000); dirty = true; return; }
      wantCard = false; showCard = true; showTransfer = false; dirty = true;
    } else if (TAB_I.hit(x, y)) {
      wantCard = false; showCard = false; showTransfer = true; dirty = true;
    } else if (showTransfer && BTN_DIGITS.hit(x, y)) {
      if (!known) { setNote("Saving the sale... try again in a moment", 3000); dirty = true; return; }
      openEntry(EN_CLAIM);
    } else if (BTN_MORE.hit(x, y)) {
      size_t n;
      { Lock l; n = S.incoming.size(); }
      if (!n || !known) return;
      screen = SCR_INCOMING;
      dirty = true;
    }
  } else if (screen == SCR_MORE) {
    if (BTN_BACKBAR.hit(x, y) || rowBtn(2).hit(x, y)) { screen = SCR_QR; dirty = true; }
    else if (rowBtn(0).hit(x, y)) openEntry(EN_CLAIM);
    else if (rowBtn(1).hit(x, y)) { screen = SCR_INCOMING; dirty = true; }
  } else if (screen == SCR_DIGITS) {
    if (BTN_BACKBAR.hit(x, y)) { screen = entryKind == EN_CASHTIP ? SCR_RESULT : SCR_QR; resultSince = millis(); dirty = true; return; }
    if (BTN_QR.hit(x, y)) {
      bool ok = entryKind == EN_CASHTIP ? entry.toInt() > 0 : entry.length() == entryMax;
      if (!ok || reqBusy()) return;
      long number;
      { Lock l; number = S.sale.number; }
      String q = entryKind == EN_CLAIM ? "action=claim&number=" + String(number) + "&digits=" + entry
               : entryKind == EN_BUYER ? "action=buyer&number=" + String(number) + "&phone=" + entry
               : "action=tip&number=" + String(number) + "&amount=" + entry + "&kind=cash";
      note = "";
      sendReq(q);
      drawEntryOnly();
      return;
    }
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++) {
        const char *k = DKEYS[r][c];
        if (!k[0] || !keyBtn(r, c).hit(x, y)) continue;
        if (!strcmp(k, "Del")) { if (entry.length()) entry.remove(entry.length() - 1); }
        else if (!strcmp(k, "Clear")) entry = "";
        else if (entry.length() < entryMax) entry += k;
        note = "";
        drawEntryOnly();
        bool ok = entryKind == EN_CASHTIP ? entry.toInt() > 0 : entry.length() == entryMax;
        button(BTN_QR, "OK", ok ? C_ORANGE : C_LINE, TFT_WHITE, 4);
        return;
      }
  } else if (screen == SCR_INCOMING) {
    if (BTN_BACKBAR.hit(x, y)) { screen = SCR_QR; dirty = true; return; }
    size_t n;
    { Lock l; n = S.incoming.size(); }
    for (size_t i = 0; i < n && i < 4; i++)
      if (rowBtn(i).hit(x, y)) { pick = i; note = ""; screen = SCR_CONFIRM; dirty = true; return; }
  } else if (screen == SCR_CONFIRM) {
    if (CB_BACK.hit(x, y) || BTN_BACKBAR.hit(x, y)) { screen = SCR_INCOMING; dirty = true; return; }
    if (CB_OK.hit(x, y) && !reqBusy()) {
      String pid;
      long number;
      {
        Lock l;
        number = S.sale.number;
        pid = pick >= 0 && pick < (int)S.incoming.size() ? S.incoming[pick].id : String("");
      }
      if (!pid.length()) { screen = SCR_INCOMING; dirty = true; return; }
      sendReq("action=attach&number=" + String(number) + "&pay=" + urlEncode(pid));
      dirty = true;
    }
  } else if (screen == SCR_TODAY) {
    screen = SCR_HOME;
    dirty = true;
  } else if (screen == SCR_SIGNIN) {
    if (reqBusy()) return;
    if (loginStep != LS_MOBILE && BTN_BACKBAR.hit(x, y)) { loginStep = LS_MOBILE; pinTyped = ""; note = ""; dirty = true; return; }
    if (BTN_QR.hit(x, y)) {
      if (pinTyped.length() != loginMax()) return;
      note = "";
      if (loginStep == LS_MOBILE) { loginMobile = pinTyped; pinTyped = ""; loginStep = LS_PIN; dirty = true; return; }
      String ticket;
      { Lock l; ticket = S.ticket; }
      if (loginStep == LS_PIN) sendReq("action=login1&mobile=" + loginMobile + "&pin=" + pinTyped);
      else sendReq("action=login2&ticket=" + urlEncode(ticket) + "&code=" + pinTyped);
      drawPinOnly();
      return;
    }
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++) {
        const char *k = DKEYS[r][c];
        if (!k[0] || !keyBtn(r, c).hit(x, y)) continue;
        if (!strcmp(k, "Del")) { if (pinTyped.length()) pinTyped.remove(pinTyped.length() - 1); }
        else if (!strcmp(k, "Clear")) pinTyped = "";
        else if (pinTyped.length() < loginMax()) pinTyped += k;
        note = "";
        drawPinOnly();
        return;
      }
  } else if (screen == SCR_RESULT) {
    bool billSale;
    String bstat;
    long bnum;
    { Lock l; billSale = S.sale.billStatus.length() > 0; bstat = S.sale.billStatus; bnum = S.sale.number; }
    if (bstat == "short") {
      if (RB_BILLREST.hit(x, y) && !reqBusy()) { sendReq("action=billrest&number=" + String(bnum)); dirty = true; }
      return;  // the bill stays on screen until it's paid (the owner or the app can also cancel it)
    }
    if (tipsOn() && !billSale) {
      long diff, number;
      String st;
      { Lock l; diff = S.sale.diff; number = S.sale.number; st = S.sale.status; }
      bool paid = st == "paid" || st == "screenshot";
      if (paid && diff > 0 && RB_EXTRA.hit(x, y)) {
        if (sendReq("action=tip&number=" + String(number) + "&amount=" + String(diff) + "&kind=extra")) { setNote("Recording the tip...", 4000); resultSince = millis(); dirty = true; }
        return;
      }
      if (paid && RB_CASH.hit(x, y)) { openEntry(EN_CASHTIP); return; }
    }
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
  } else if (screen == SCR_MENU) {
    if (BTN_BACKBAR.hit(x, y) || rowBtn(3).hit(x, y)) { screen = SCR_HOME; dirty = true; }
    else if (rowBtn(0).hit(x, y)) { billerPick = -1; screen = SCR_BILLERS; dirty = true; }
    else if (rowBtn(1).hit(x, y)) openTrip();
    else if (rowBtn(2).hit(x, y)) { sendReq("action=today"); screen = SCR_TODAY; dirty = true; }
  } else if (screen == SCR_BILLERS) {
    if (BTN_BACKBAR.hit(x, y)) { screen = SCR_MENU; dirty = true; return; }
    size_t n;
    { Lock l; n = S.billers.size(); }
    for (size_t i = 0; i < n && i < 6; i++)
      if (billerBtn(i).hit(x, y)) { billerPick = i; billStep = 0; billAcct = ""; billAmt = ""; note = ""; screen = SCR_BILLACCT; dirty = true; return; }
  } else if (screen == SCR_BILLACCT) {
    if (reqBusy()) return;
    if (BTN_BACKBAR.hit(x, y)) {
      if (billStep == 1) { billStep = 0; billAmt = ""; } else screen = SCR_BILLERS;
      note = ""; dirty = true; return;
    }
    BillerItem b = billerNow();
    if (BTN_QR.hit(x, y)) {
      if (!billEntryOk()) return;
      note = "";
      if (billStep == 0 && b.kind == "topup") { billStep = 1; dirty = true; return; }
      sendReq("action=billquote&biller=" + urlEncode(b.id) + "&account=" + billAcct + (b.kind == "topup" ? "&amount=" + billAmt : String("")));
      drawBillEntryOnly();
      return;
    }
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++) {
        const char *k = DKEYS[r][c];
        if (!k[0] || !keyBtn(r, c).hit(x, y)) continue;
        String &e = billStep == 0 ? billAcct : billAmt;
        unsigned mx = billStep == 0 ? (unsigned)b.max : 4;
        if (!strcmp(k, "Del")) { if (e.length()) e.remove(e.length() - 1); }
        else if (!strcmp(k, "Clear")) e = "";
        else if (e.length() < mx && !(billStep == 1 && e.isEmpty() && k[0] == '0')) e += k;
        note = "";
        drawBillEntryOnly();
        return;
      }
  } else if (screen == SCR_BILLQUOTE) {
    if (reqBusy()) return;
    if (BTN_BACKBAR.hit(x, y)) { screen = SCR_BILLACCT; dirty = true; return; }
    BillerItem b = billerNow();
    if (QB_CASH.hit(x, y)) {
      sendReq("action=billpay&biller=" + urlEncode(b.id) + "&account=" + billAcct + (b.kind == "topup" ? "&amount=" + billAmt : String("")));
      dirty = true;
      return;
    }
    if (QB_QR.hit(x, y)) {
      long amount;
      { Lock l; amount = S.bill.amount; }
      // A normal sale with the bill attached: the same Wallet / Card / InstaPay screen.
      startSale(amount, -1, "", b.id, billAcct);
      if (screen != SCR_QR) { screen = SCR_BILLQUOTE; dirty = true; }
      return;
    }
  } else if (screen == SCR_BILLDONE) {
    screen = SCR_HOME;
    typed = "";
    dirty = true;
  } else if (screen == SCR_TRIPSET) {
    if (reqBusy()) return;
    if (BTN_BACKBAR.hit(x, y)) {
      size_t nc;
      { Lock l; nc = S.cars.size(); }
      if (tripStep == 1) tripStep = 0; else screen = nc > 1 ? SCR_TRIPCAR : SCR_MENU;
      note = ""; dirty = true; return;
    }
    if (BTN_QR.hit(x, y)) {
      if (!tripEntryOk()) return;
      if (tripStep == 0 && !carByCode(tripCar).locked) { tripStep = 1; dirty = true; return; }
      prefs.putString("tseats", tripSeats);
      prefs.putString("tfare", tripFare);
      if (tripCar.length()) prefs.putString("tcar", tripCar);
      sendReq("action=trip&op=start&seats=" + tripSeats + "&fare=" + tripFare + (tripCar.length() ? "&car=" + urlEncode(tripCar) : String("")));
      drawTripEntryOnly();
      return;
    }
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++) {
        const char *k = DKEYS[r][c];
        if (!k[0] || !keyBtn(r, c).hit(x, y)) continue;
        String &e = tripStep == 0 ? tripSeats : tripFare;
        if (!strcmp(k, "Del")) { if (e.length()) e.remove(e.length() - 1); }
        else if (!strcmp(k, "Clear")) e = "";
        else if (e.length() < (tripStep == 0 ? 2u : 4u) && !(e.isEmpty() && k[0] == '0')) e += k;
        note = "";
        drawTripEntryOnly();
        return;
      }
  } else if (screen == SCR_TRIPCAR) {
    if (BTN_BACKBAR.hit(x, y)) { screen = SCR_MENU; dirty = true; return; }
    std::vector<CarItem> cars;
    { Lock l; cars = S.cars; }
    for (size_t i = 0; i < cars.size() && i < 4; i++)
      if (rowBtn(i).hit(x, y)) { tripCar = cars[i].code; applyCar(); tripStep = 0; screen = SCR_TRIPSET; dirty = true; return; }
  } else if (screen == SCR_TRIP) {
    if (BTN_BACKBAR.hit(x, y)) { screen = SCR_HOME; typed = ""; dirty = true; return; }
    if (reqBusy()) return;
    if (TB_CASH.hit(x, y)) { sendReq("action=trip&op=cash&delta=1"); return; }
    if (TB_MINUS.hit(x, y)) { sendReq("action=trip&op=cash&delta=-1"); return; }
    if (TB_CHECK.hit(x, y)) {
      TripView t;
      { Lock l; t = S.trip; }
      say(tripVoice(t));
      return;
    }
    if (TB_END.hit(x, y)) { sendReq("action=trip&op=end"); return; }
  } else if (screen == SCR_TRIPEND) {
    if (TE_NEW.hit(x, y)) { openTrip(); return; }
    if (TE_HOME.hit(x, y)) { screen = SCR_HOME; typed = ""; dirty = true; }
  } else if (screen == SCR_SETTINGS) {
    if (SB_WIFI.hit(x, y)) runPortal();
    else if (SB_CAL.hit(x, y)) { calibrate(); dirty = true; }
    else if (SB_INV.hit(x, y)) { cfg.invert = !cfg.invert; tft.invertDisplay(cfg.invert); saveSettings(); dirty = true; }
    else if (SB_VOL.hit(x, y)) { cfg.volume = cfg.volume % 3 + 1; saveSettings(); say(words::received(50, "620")); dirty = true; }
    else if (SB_TEST.hit(x, y)) say(words::received(1250, "620"));
    else if (SB_USER.hit(x, y)) {
      // Log out here and on the server; this box then asks the next person to log in.
      sendReq("action=logout");
      screen = SCR_HOME; dirty = true;
    }
    else if (SB_BACK.hit(x, y)) { screen = SCR_HOME; dirty = true; }
  }
}
