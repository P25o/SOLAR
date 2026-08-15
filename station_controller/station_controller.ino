/* ============================================================
   SOLAR CHARGING STATION — บอร์ดที่ 1 "ตัวคุมสถานี"
   บอร์ด: ESP32 DevKit V1 (30 ขา)

   หน้าที่ของบอร์ดนี้:
   1. วัดแรงดันแบตเตอรี่ + เจนเนอเรเตอร์ (โซลาร์/กังหันลม)
   2. ส่งค่าขึ้น Firebase /live และบันทึกประวัติลง /history
   3. อ่านคำสั่งจาก /stations/station_01 มาเปิด/ปิด Relay
   4. นับถอยหลังในตัวเอง — เน็ตหลุดก็ยังตัดไฟตรงเวลา

   บอร์ดนี้ไม่มีจอ — จอเป็นหน้าที่ของบอร์ดที่ 2 (display_unit.ino)
   ทั้งสองบอร์ดไม่ต้องเดินสายถึงกัน คุยผ่าน Firebase อย่างเดียว

   *** ต้องแก้ 2 บรรทัดก่อนอัปโหลด: WIFI_SSID, WIFI_PASS ***

   ตั้งค่าใน Arduino IDE:
     Board: "ESP32 Dev Module"
     Upload Speed: 921600
   ============================================================ */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// ============================================================
// ส่วนที่ 1 : ตั้งค่าที่ต้องแก้
// ============================================================

const char* WIFI_SSID = "ใส่ชื่อ WiFi ของคุณ";
const char* WIFI_PASS = "ใส่รหัส WiFi ของคุณ";

const char* FB_HOST    = "https://solar-station-5b0a8-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* STATION_ID = "station_01";

// ============================================================
// ส่วนที่ 2 : ขาที่ใช้
// ============================================================

/* ⚠️ ขาวัดไฟต้องเป็น ADC1 เท่านั้น (32, 33, 34, 35, 36/VP, 39/VN)
      เพราะ ADC2 (25, 26, 27, 12, 13, 14...) ใช้ร่วมกับ WiFi ไม่ได้
      ถ้าเผลอไปใช้ ADC2 ค่าจะอ่านได้ 0 ตลอดและหาสาเหตุไม่เจอ        */

const int PIN_GENERATOR = 34;   // แรงดันจากโซลาร์/กังหันลม (ผ่านวงจรแบ่งแรงดัน)
const int PIN_BATTERY   = 35;   // แรงดันแบตเตอรี่ (ผ่านวงจรแบ่งแรงดัน)

// ขาสั่ง Relay — ใช้ ADC2 ได้ เพราะเป็นการ "สั่งงาน" ไม่ใช่ "อ่านค่า"
const int RELAY_PIN = 27;

// รีเลย์โมดูลส่วนใหญ่เป็น Active LOW (ส่ง LOW = ทำงาน)
// ถ้าของคุณทำงานกลับด้าน เปลี่ยนเป็น false
const bool RELAY_ACTIVE_LOW = true;

// ============================================================
// ส่วนที่ 3 : ค่าคงที่สำหรับคำนวณ
// ============================================================

/* อัตราส่วนวงจรแบ่งแรงดัน = (R1 + R2) / R2
     R1 = 40k, R2 = 10k  ->  5.0
     R1 = 39k, R2 = 10k  ->  4.9
   ⚠️ ห้ามต่อไฟ 12V เข้าขา ADC ตรง ๆ ขาจะไหม้ทันที           */
const float DIVIDER_RATIO = 5.0;

// ช่วงแรงดันของแบตเตอรี่ 12V
const float BATT_EMPTY_V = 11.5;   // ต่ำกว่านี้ถือว่าหมด
const float BATT_FULL_V  = 14.4;   // ชาร์จเต็ม

// เพดานเวลาสูงสุด กันค่าขยะทำให้ Relay ค้างเปิดข้ามวัน
const long MAX_SESSION_SECONDS = 4L * 60 * 60;   // 4 ชั่วโมง

// ============================================================
// ส่วนที่ 4 : จังหวะเวลา
// ============================================================

const unsigned long READ_INTERVAL       = 1000;    // อ่านเซนเซอร์
const unsigned long PUSH_INTERVAL       = 5000;    // ส่ง /live
const unsigned long POLL_INTERVAL       = 2000;    // ถามคำสั่ง Relay
const unsigned long WIFI_RETRY_INTERVAL = 10000;
const unsigned long WIFI_BOOT_TIMEOUT   = 15000;

// ============================================================
// ส่วนที่ 5 : ตัวแปรสถานะ
// ============================================================

float batteryVoltage   = 0.0f;
float generatorVoltage = 0.0f;
int   batteryPercent   = 0;

bool relayOn = false;
long remainingSeconds = 0;

// sessionId ของรอบที่กำลังทำงานอยู่
// ค่าเริ่มต้นเป็นอักขระที่เป็นไปไม่ได้ เพื่อให้รอบแรกที่เจอถูกรับเสมอ
String acceptedSessionId = "\x01";
bool pendingOffPush = false;   // ตัดไฟแล้วแต่เขียนกลับ Firebase ไม่สำเร็จ

unsigned long lastReadMs    = 0;
unsigned long lastPushMs    = 0;
unsigned long lastPollMs    = 0;
unsigned long lastCountMs   = 0;
unsigned long lastWifiTryMs = 0;
int lastLoggedHour = -1;

// ============================================================
// ส่วนที่ 6 : อ่านเซนเซอร์
// ============================================================

float readVoltage(int pin) {
  const int SAMPLES = 16;
  uint32_t total = 0;

  for (int i = 0; i < SAMPLES; i++) {
    // analogReadMilliVolts แม่นกว่า analogRead เพราะชดเชยความไม่เป็นเส้นตรงให้แล้ว
    total += analogReadMilliVolts(pin);
    delay(2);
  }

  float mv = (float)total / SAMPLES;
  return (mv / 1000.0) * DIVIDER_RATIO;
}

int voltageToPercent(float v) {
  if (v < 1.0) return 0;   // ยังไม่ได้ต่อสาย

  float pct = (v - BATT_EMPTY_V) / (BATT_FULL_V - BATT_EMPTY_V) * 100.0;
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return (int)(pct + 0.5);
}

void readSensors() {
  generatorVoltage = readVoltage(PIN_GENERATOR);
  batteryVoltage   = readVoltage(PIN_BATTERY);
  batteryPercent   = voltageToPercent(batteryVoltage);
}

// ============================================================
// ส่วนที่ 7 : Relay
// ============================================================

void setRelay(bool on) {
  bool changed = (relayOn != on);
  relayOn = on;

  if (RELAY_ACTIVE_LOW) digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  else                  digitalWrite(RELAY_PIN, on ? HIGH : LOW);

  if (changed) {
    Serial.print(">>> RELAY: ");
    Serial.println(on ? "ON  (USB มีไฟ)" : "OFF (USB ไม่มีไฟ)");
  }
}

// ============================================================
// ส่วนที่ 8 : คุยกับ Firebase
// ============================================================

String extractValue(const String& json, const String& key) {
  String needle = "\"" + key + "\":";
  int i = json.indexOf(needle);
  if (i < 0) return "";

  i += needle.length();
  while (i < (int)json.length() && json[i] == ' ') i++;
  if (i >= (int)json.length()) return "";

  bool isString = (json[i] == '"');
  if (isString) i++;

  int j = i;
  while (j < (int)json.length()) {
    char c = json[j];
    if (isString) { if (c == '"') break; }
    else if (c == ',' || c == '}') break;
    j++;
  }

  String v = json.substring(i, j);
  v.trim();
  if (!isString && v == "null") return "";
  return v;
}

bool fbRequest(const char* method, const String& path, const String& body, String* out) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);

  String url = String(FB_HOST) + path + ".json";
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");

  int code;
  if (strcmp(method, "GET") == 0)      code = http.GET();
  else if (strcmp(method, "PUT") == 0) code = http.PUT(body);
  else                                 code = http.PATCH(body);

  bool ok = (code == 200);
  if (ok && out) *out = http.getString();
  if (!ok) Serial.printf("Firebase %s %s ล้มเหลว code=%d\n", method, path.c_str(), code);

  http.end();
  return ok;
}

String stationPath() { return "/stations/" + String(STATION_ID); }

/* ส่งค่าปัจจุบันไปที่ /live

   ชื่อ field ต้องตรงกับที่หน้าเว็บอ่าน: battery, voltage, generator, uptime, heap, ip
   และแถม relayState / remainingSeconds ไปด้วย เพื่อให้บอร์ดจอ (display_unit)
   ดึงข้อมูลครบทุกอย่างได้ด้วยการอ่าน "ครั้งเดียว" ไม่ต้องยิง 2 รอบ  */
void pushLive() {
  String body = "{";
  body += "\"battery\":"          + String(batteryPercent) + ",";
  body += "\"voltage\":"          + String(batteryVoltage, 2) + ",";
  body += "\"generator\":"        + String(generatorVoltage, 2) + ",";
  body += "\"uptime\":"           + String(millis() / 1000) + ",";
  body += "\"heap\":"             + String(ESP.getFreeHeap()) + ",";
  body += "\"ip\":\""             + WiFi.localIP().toString() + "\",";
  body += "\"relayState\":\""     + String(relayOn ? "on" : "off") + "\",";
  body += "\"remainingSeconds\":" + String(remainingSeconds);
  body += "}";

  if (fbRequest("PUT", "/live", body, nullptr)) {
    Serial.println("ส่งขึ้น Firebase สำเร็จ: " + body);
  }
}

void pushHistoryIfNewHour() {
  struct tm t;
  if (!getLocalTime(&t)) return;            // ยังไม่ได้เวลาจาก NTP
  if (t.tm_hour == lastLoggedHour) return;  // ชั่วโมงนี้บันทึกไปแล้ว

  char dateStr[12];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &t);

  String path = "/history/" + String(dateStr) + "/" + String(t.tm_hour);
  String body = "{\"batt\":" + String(batteryPercent) +
                ",\"gen\":"  + String(generatorVoltage, 2) + "}";

  if (fbRequest("PUT", path, body, nullptr)) {
    lastLoggedHour = t.tm_hour;
    Serial.println("บันทึกประวัติชั่วโมงที่ " + String(t.tm_hour));
  }
}

// รายงานสถานะ Relay กลับขึ้น /stations (ใช้ PATCH เพื่อไม่ลบ sessionId ที่เซิร์ฟเวอร์เขียนไว้)
bool pushStationStatus() {
  String body = "{\"relayState\":\"";
  body += (relayOn ? "on" : "off");
  body += "\",\"remainingSeconds\":";
  body += String(remainingSeconds);
  body += "}";
  return fbRequest("PATCH", stationPath(), body, nullptr);
}

// ============================================================
// ส่วนที่ 9 : จัดการรอบการชาร์จ
// ============================================================

void startSession(const String& sessionId, long secs) {
  if (secs > MAX_SESSION_SECONDS) {
    Serial.println("เวลาเกินเพดาน — ตัดเหลือ " + String(MAX_SESSION_SECONDS) + " วินาที");
    secs = MAX_SESSION_SECONDS;
  }

  acceptedSessionId = sessionId;
  remainingSeconds  = secs;
  lastCountMs       = millis();
  pendingOffPush    = false;

  setRelay(true);
  Serial.println("=== เริ่มรอบใหม่ | session=" + sessionId +
                 " | เวลา " + String(secs) + " วินาที ===");
}

void stopSession(const char* reason) {
  remainingSeconds = 0;
  setRelay(false);
  Serial.printf("=== ตัดไฟเรียบร้อย (%s) ===\n", reason);

  // ถ้าเขียนกลับไม่สำเร็จ ตั้งธงไว้ลองใหม่ทุกรอบ poll
  // ไม่งั้น Firebase จะค้างเป็น "on" แล้วรอบหน้าจะเข้าใจผิดว่ายังจ่ายอยู่
  pendingOffPush = !pushStationStatus();
}

void pollStation() {
  String payload;
  if (!fbRequest("GET", stationPath(), "", &payload)) return;

  payload.trim();

  // ยังไม่มี node นี้ -> สร้างค่าเริ่มต้นให้อัตโนมัติ
  if (payload.length() == 0 || payload == "null") {
    Serial.println("ไม่พบ " + stationPath() + " — กำลังสร้างให้");
    fbRequest("PUT", stationPath(),
              "{\"relayState\":\"off\",\"remainingSeconds\":0,\"sessionId\":\"\"}", nullptr);
    return;
  }

  String state     = extractValue(payload, "relayState");
  String sessionId = extractValue(payload, "sessionId");
  long   secs      = extractValue(payload, "remainingSeconds").toInt();

  // สั่งหยุดกลางคัน (คืนเงิน / แอดมินสั่งปิด)
  if (state == "off") {
    if (relayOn) stopSession("Firebase สั่งหยุด");
    pendingOffPush = false;
    return;
  }

  if (state != "on") return;

  // รอบใหม่ (หรือเติมเวลา): sessionId ต้องไม่ซ้ำของเดิม
  if (sessionId != acceptedSessionId && secs > 0) {
    startSession(sessionId, secs);
    return;
  }

  /* sessionId เดิม -> ห้ามแตะ remainingSeconds เด็ดขาด
     ถ้าเผลอเอาค่าจาก Firebase มาเขียนทับ ตัวนับจะถูกรีเซ็ตทุก 2 วินาที
     แล้ว Relay จะไม่มีวันดับ = ชาร์จฟรีไม่จำกัด                        */
  if (!relayOn && pendingOffPush) {
    pendingOffPush = !pushStationStatus();
  }
}

// ============================================================
// ส่วนที่ 10 : WiFi
// ============================================================

void wifiConnectAtBoot() {
  Serial.print("กำลังต่อ WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_BOOT_TIMEOUT) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nต่อ WiFi สำเร็จ! IP: " + WiFi.localIP().toString());
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");   // เวลาไทย UTC+7
  } else {
    Serial.println("\nต่อ WiFi ไม่ได้ — ระบบยังทำงานต่อ และจะลองใหม่เรื่อย ๆ");
  }
  lastWifiTryMs = millis();
}

// ต่อ WiFi ใหม่แบบไม่บล็อก — สั่งแล้วปล่อย ไม่ยืนรอ
void wifiEnsure() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWifiTryMs < WIFI_RETRY_INTERVAL) return;
  lastWifiTryMs = now;

  Serial.println("WiFi หลุด — สั่งต่อใหม่ (การนับถอยหลังยังเดินอยู่)");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ============================================================
void setup() {
  // ปิด Relay ทันทีตั้งแต่บูต กันไฟติดค้างตอนไฟดับแล้วกลับมา
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  relayOn = false;

  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Solar Charging Station — Controller ===");

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_GENERATOR, ADC_11db);
  analogSetPinAttenuation(PIN_BATTERY,   ADC_11db);

  wifiConnectAtBoot();

  readSensors();
  pollStation();   // อ่านรอบแรกทันที (บอร์ดรีบูตกลางรอบ จะรับ session เดิมมานับต่อ)

  unsigned long now = millis();
  lastPollMs = now;
  lastPushMs = now;
  lastReadMs = now;

  Serial.println("พร้อมทำงาน");
}

// ============================================================
void loop() {
  unsigned long now = millis();

  // --- งานที่ 1 : ดูแล WiFi (ไม่บล็อก) ---
  wifiEnsure();

  // --- งานที่ 2 : อ่านเซนเซอร์ ---
  if (now - lastReadMs >= READ_INTERVAL) {
    lastReadMs = now;
    readSensors();
  }

  // --- งานที่ 3 : ถามคำสั่ง Relay จาก Firebase ---
  if (now - lastPollMs >= POLL_INTERVAL) {
    lastPollMs = now;
    pollStation();
  }

  // --- งานที่ 4 : นับถอยหลังทุก 1 วินาที ---
  // นับในบอร์ดล้วน ๆ ไม่พึ่งเน็ต -> เน็ตหลุดก็ยังตัดไฟตรงเวลา
  if (relayOn && (now - lastCountMs >= 1000)) {
    lastCountMs += 1000;   // บวกทีละ 1000 (ไม่ใช่ = now) เวลาจะได้ไม่เพี้ยนสะสม
    remainingSeconds--;

    if (remainingSeconds % 10 == 0 || remainingSeconds <= 10) {
      Serial.println("เหลือเวลา: " + String(remainingSeconds) + " วินาที");
    }

    if (remainingSeconds <= 0) stopSession("หมดเวลา");
  }

  // --- งานที่ 5 : ส่งข้อมูลขึ้น Firebase ---
  if (now - lastPushMs >= PUSH_INTERVAL) {
    lastPushMs = now;

    if (WiFi.status() == WL_CONNECTED) {
      pushLive();                 // /live มี relayState + remainingSeconds ติดไปด้วย
      if (relayOn) pushStationStatus();
      pushHistoryIfNewHour();
    }
  }
}
