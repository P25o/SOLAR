/* ============================================================
   PHASE 1 — ทดสอบ Relay + Firebase (ยังไม่มีเรื่องเงิน)
   บอร์ด: ESP32-2432S028 (Cheap Yellow Display)

   สิ่งที่สเก็ตช์นี้ทำ:
   1. ต่อ WiFi (มี timeout — ต่อไม่ได้ก็ไม่ค้าง)
   2. ทุก 2 วินาที ไปอ่านคำสั่งจาก /stations/station_01
   3. ถ้าเจอ "รอบใหม่" -> เปิด Relay + นับถอยหลังในตัวเอง
   4. เวลาหมด -> ปิด Relay + เขียนสถานะกลับขึ้น Firebase

   ------------------------------------------------------------
   แก้บั๊กจากฉบับก่อนหน้า:
   [1] *สำคัญที่สุด* Relay ไม่มีวันดับ
       ของเดิม: ทุก 2 วินาทีไปอ่าน remainingSeconds จาก Firebase
                มาเขียนทับตัวนับถอยหลัง แต่ Firebase ไม่เคยถูกอัปเดต
                ระหว่างทาง -> ค่าถูกรีเซ็ตกลับที่เดิมตลอด ชาร์จฟรีไม่จำกัด
       ของใหม่: ใช้ sessionId เป็นตัวตัดสิน จะรับเวลาใหม่ "เฉพาะตอน
                sessionId เปลี่ยน" เท่านั้น ระหว่างนับถอยหลังจะไม่สนใจ
                ค่าจาก Firebase เลย
   [2] ต่อ WiFi ไม่ได้แล้วค้างตายใน setup() -> ใส่ timeout + ต่อใหม่
       แบบไม่บล็อกใน loop()
   [3] HTTP ไม่มี timeout -> เน็ตอืดทีเดียวหยุดนับถอยหลัง
   [4] ไม่เคยเช็กว่าเขียนกลับ Firebase สำเร็จไหม -> ถ้าเขียน "off"
       ไม่สำเร็จ จะลองใหม่เรื่อย ๆ จนกว่าจะติด
   [5] ถ้ายังไม่มี node ใน Firebase -> สร้างค่าเริ่มต้นให้อัตโนมัติ
   [6] ใส่เพดานเวลาสูงสุด กันค่าขยะทำให้ Relay ค้างเปิด
   ------------------------------------------------------------

   *** สเก็ตช์นี้แยกจากโค้ดหลักของคุณ ***
   ให้ทดสอบตัวนี้ให้ผ่านก่อน แล้วค่อยเอาไปรวมกับโค้ดหลักทีหลัง
   ============================================================ */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ===== ตั้งค่า WiFi =====
const char* WIFI_SSID = "ใส่ชื่อ WiFi ของคุณ";
const char* WIFI_PASS = "ใส่รหัส WiFi ของคุณ";

// ===== ตั้งค่า Firebase =====
const char* FB_HOST    = "https://solar-station-5b0a8-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* STATION_ID = "station_01";

// ===== ขา Relay =====
// GPIO 27 เป็นขาว่างบนคอนเนกเตอร์ของ CYD
// ห้ามใช้ GPIO 34/35/36/39 เพราะเป็นขา "อ่านอย่างเดียว" สั่งงานไม่ได้
//
// ⚠️ ตอนรวมร่างกับโค้ดหลัก (ด่าน 4) ต้องระวัง:
//    โค้ดหลักใช้ GPIO 22 และ 27 เป็น Serial2 (PIN_RX2 / PIN_TX2)
//    ตอนนี้ไม่ชนเพราะโค้ดหลักตั้ง USE_SERIAL2 = false
//    ถ้าจะเปลี่ยนไปใช้โหมด 2 บอร์ด ต้องย้าย Relay ไป GPIO 26 แทน
const int RELAY_PIN = 27;

// รีเลย์โมดูลส่วนใหญ่เป็นแบบ Active LOW
// (ส่ง LOW = รีเลย์ทำงาน / ส่ง HIGH = รีเลย์ตัด)
// ถ้าของคุณทำงานกลับด้าน ให้เปลี่ยนเป็น false
const bool RELAY_ACTIVE_LOW = true;

// ===== เพดานความปลอดภัย =====
// กันกรณีค่าใน Firebase เพี้ยนหรือถูกแก้มั่ว แล้ว Relay ค้างเปิดข้ามวัน
const long MAX_SESSION_SECONDS = 4L * 60 * 60;   // 4 ชั่วโมง

// ===== จังหวะเวลา =====
const unsigned long POLL_INTERVAL       = 2000;    // ถาม Firebase ทุก 2 วินาที
const unsigned long STATUS_INTERVAL     = 5000;    // รายงานเวลาที่เหลือทุก 5 วินาที
const unsigned long WIFI_RETRY_INTERVAL = 10000;   // ถ้า WiFi หลุด ลองใหม่ทุก 10 วินาที
const unsigned long WIFI_BOOT_TIMEOUT   = 15000;   // ตอนบูตรอ WiFi ไม่เกิน 15 วินาที

// ===== ตัวแปรสถานะ =====
bool relayOn = false;
long remainingSeconds = 0;

// sessionId ของรอบที่กำลังทำงานอยู่
// ค่าเริ่มต้นเป็นอักขระที่เป็นไปไม่ได้ เพื่อให้รอบแรกที่เจอถูกรับเสมอ
String acceptedSessionId = "\x01";

// true = ตัดไฟแล้ว แต่เขียนสถานะกลับ Firebase ไม่สำเร็จ ต้องลองใหม่
bool pendingOffPush = false;

unsigned long lastPollMs   = 0;
unsigned long lastCountMs  = 0;
unsigned long lastStatusMs = 0;
unsigned long lastWifiTryMs = 0;

// ------------------------------------------------------------
// สั่งเปิด/ปิด Relay จริง
// ------------------------------------------------------------
void setRelay(bool on) {
  bool changed = (relayOn != on);
  relayOn = on;

  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  }

  if (changed) {
    Serial.print(">>> RELAY: ");
    Serial.println(on ? "ON  (USB มีไฟ)" : "OFF (USB ไม่มีไฟ)");
  }
}

// ------------------------------------------------------------
// ดึงค่าออกจากข้อความ JSON แบบง่าย ๆ
// (ไม่ใช้ไลบรารีเสริม เพื่อให้ติดตั้งง่ายที่สุด)
// ------------------------------------------------------------
String extractValue(const String& json, const String& key) {
  String needle = "\"" + key + "\":";
  int i = json.indexOf(needle);
  if (i < 0) return "";

  i += needle.length();
  while (i < (int)json.length() && json[i] == ' ') i++;
  if (i >= (int)json.length()) return "";        // กันอ่านเลยท้ายข้อความ

  bool isString = (json[i] == '"');
  if (isString) i++;

  int j = i;
  while (j < (int)json.length()) {
    char c = json[j];
    if (isString) {
      if (c == '"') break;
    } else if (c == ',' || c == '}') {
      break;
    }
    j++;
  }

  String v = json.substring(i, j);
  v.trim();
  if (!isString && v == "null") return "";
  return v;
}

// ------------------------------------------------------------
// คุย HTTP กับ Firebase (ใช้ร่วมกันทั้ง GET / PUT / PATCH)
// ------------------------------------------------------------
bool fbRequest(const char* method, const String& path, const String& body, String* out) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();          // ไม่ตรวจใบรับรอง (พอสำหรับงานนี้)

  HTTPClient http;
  http.setConnectTimeout(5000);  // เน็ตอืดก็ไม่หยุดนับถอยหลังนานเกินไป
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
  if (!ok) {
    Serial.printf("Firebase %s %s ล้มเหลว code=%d\n", method, path.c_str(), code);
  }

  http.end();
  return ok;
}

String stationPath() {
  return "/stations/" + String(STATION_ID);
}

// ------------------------------------------------------------
// รายงานสถานะปัจจุบันขึ้น Firebase (ให้หน้าเว็บเห็นเวลาที่เหลือ)
// ใช้ PATCH เพื่อไม่ให้ไปลบ sessionId ที่เซิร์ฟเวอร์เขียนไว้
// ------------------------------------------------------------
bool pushStatus() {
  String body = "{\"relayState\":\"";
  body += (relayOn ? "on" : "off");
  body += "\",\"remainingSeconds\":";
  body += String(remainingSeconds);
  body += "}";

  return fbRequest("PATCH", stationPath(), body, nullptr);
}

// ------------------------------------------------------------
// เริ่มรอบใหม่
// ------------------------------------------------------------
void startSession(const String& sessionId, long secs) {
  if (secs > MAX_SESSION_SECONDS) {
    Serial.println("เวลาเกินเพดาน — ตัดเหลือ " + String(MAX_SESSION_SECONDS) + " วินาที");
    secs = MAX_SESSION_SECONDS;
  }

  acceptedSessionId = sessionId;
  remainingSeconds  = secs;
  lastCountMs       = millis();   // เริ่มจับเวลาใหม่ ไม่ให้วินาทีแรกหายไป
  pendingOffPush    = false;

  setRelay(true);
  Serial.println("=== เริ่มรอบใหม่ | session=" + sessionId +
                 " | เวลา " + String(secs) + " วินาที ===");
}

// ------------------------------------------------------------
// จบรอบ + รายงานกลับ Firebase
// ------------------------------------------------------------
void stopSession(const char* reason) {
  remainingSeconds = 0;
  setRelay(false);

  Serial.printf("=== ตัดไฟเรียบร้อย (%s) ===\n", reason);

  // ถ้าเขียนกลับไม่สำเร็จ ตั้งธงไว้ให้ลองใหม่ทุกรอบ poll
  // ไม่งั้น Firebase จะค้างเป็น "on" แล้วรอบหน้าจะเข้าใจผิดว่ายังจ่ายอยู่
  pendingOffPush = !pushStatus();
}

// ------------------------------------------------------------
// อ่านคำสั่งจาก Firebase
// ------------------------------------------------------------
void pollFirebase() {
  String payload;
  if (!fbRequest("GET", stationPath(), "", &payload)) return;

  payload.trim();

  // ยังไม่มี node นี้ -> สร้างค่าเริ่มต้นให้เลย จะได้ไม่ต้องพิมพ์เองใน Console
  if (payload.length() == 0 || payload == "null") {
    Serial.println("ไม่พบ /stations/" + String(STATION_ID) + " — กำลังสร้างให้");
    fbRequest("PUT", stationPath(),
              "{\"relayState\":\"off\",\"remainingSeconds\":0,\"sessionId\":\"\"}",
              nullptr);
    return;
  }

  Serial.print("Firebase -> ");
  Serial.println(payload);

  String state     = extractValue(payload, "relayState");
  String sessionId = extractValue(payload, "sessionId");
  long   secs      = extractValue(payload, "remainingSeconds").toInt();

  // --- สั่งหยุดกลางคัน (คืนเงิน / แอดมินสั่งปิด) ---
  if (state == "off") {
    if (relayOn) stopSession("Firebase สั่งหยุด");
    pendingOffPush = false;
    return;
  }

  if (state != "on") return;   // ค่าเพี้ยน ไม่ต้องทำอะไร

  // --- รอบใหม่ (หรือเติมเวลา): sessionId ต้องไม่ซ้ำของเดิม ---
  if (sessionId != acceptedSessionId && secs > 0) {
    startSession(sessionId, secs);
    return;
  }

  // --- sessionId เดิม ---
  // ตรงนี้คือหัวใจของการแก้บั๊ก: ห้ามแตะ remainingSeconds เด็ดขาด
  // ปล่อยให้บอร์ดนับถอยหลังของตัวเองไป ไม่งั้นจะโดนรีเซ็ตทุก 2 วินาที
  if (!relayOn && pendingOffPush) {
    pendingOffPush = !pushStatus();   // รอบที่แล้วเขียน "off" ไม่ติด ลองใหม่
  }
}

// ------------------------------------------------------------
// WiFi
// ------------------------------------------------------------
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
  } else {
    Serial.println("\nต่อ WiFi ไม่ได้ — ระบบยังทำงานต่อ และจะลองใหม่ให้เรื่อย ๆ");
  }
  lastWifiTryMs = millis();
}

// ต่อ WiFi ใหม่แบบไม่บล็อก — สั่งแล้วปล่อย ไม่ยืนรอ
void wifiEnsure() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWifiTryMs < WIFI_RETRY_INTERVAL) return;
  lastWifiTryMs = now;

  Serial.println("WiFi หลุด — สั่งต่อใหม่ (ไม่ค้าง การนับถอยหลังยังเดินอยู่)");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ------------------------------------------------------------
void setup() {
  // ปิด Relay ทันทีตั้งแต่บูต กันไฟติดค้างตอนไฟดับแล้วกลับมา
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  relayOn = false;

  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- Phase 1: Relay Test ---");

  wifiConnectAtBoot();

  Serial.println("รอคำสั่งจาก Firebase...");
  pollFirebase();   // อ่านรอบแรกทันที (ถ้าบอร์ดรีบูตกลางรอบ จะรับ session เดิมกลับมาต่อ)
  lastPollMs = millis();
}

// ------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  // --- งานที่ 1: ดูแล WiFi ---
  wifiEnsure();

  // --- งานที่ 2: ถาม Firebase เป็นระยะ ---
  if (now - lastPollMs >= POLL_INTERVAL) {
    lastPollMs = now;
    pollFirebase();
  }

  // --- งานที่ 3: นับถอยหลังทุก 1 วินาที ---
  // นับในบอร์ดล้วน ๆ ไม่พึ่งเน็ต -> เน็ตหลุดก็ยังตัดไฟตรงเวลา
  if (relayOn && (now - lastCountMs >= 1000)) {
    lastCountMs += 1000;   // บวกทีละ 1000 (ไม่ใช่ = now) เวลาจะได้ไม่เพี้ยนสะสม
    remainingSeconds--;

    Serial.print("เหลือเวลา: ");
    Serial.print(remainingSeconds);
    Serial.println(" วินาที");

    if (remainingSeconds <= 0) {
      stopSession("หมดเวลา");
    }
  }

  // --- งานที่ 4: รายงานเวลาที่เหลือให้หน้าเว็บเห็น ---
  if (relayOn && (now - lastStatusMs >= STATUS_INTERVAL)) {
    lastStatusMs = now;
    pushStatus();
  }
}
