/* ============================================================
   Solar Charging Station — บอร์ดหลัก CYD (ESP32-2432S028)
   ฉบับแก้ไขปัญหาทั้งหมด

   แก้อะไรไปบ้าง:
   [1] เพิ่มการส่งข้อมูลขึ้น Firebase (ของเดิมไม่มีเลย เว็บเลยว่าง)
   [2] เพิ่มโหมดรับข้อมูลจาก Serial2 (เลือกได้ว่าจะใช้ 1 หรือ 2 บอร์ด)
   [3] คืนขา GPIO32 ให้จอสัมผัส (ตัดเซนเซอร์อุณหภูมิออก)
   [4] เตือนเรื่อง LDR บน GPIO34 (ดูหมายเหตุด้านล่าง)
   [5] แก้สูตร % แบตเตอรี่ ให้คิดจากช่วง 11.5V - 14.4V จริง
   [6] เพิ่มการบันทึกประวัติรายชั่วโมงลง /history

   รอบแก้ที่ 2:
   [7] ledcAttach เป็นคำสั่งของ ESP32 core 3.x เท่านั้น ถ้าเครื่องลง 2.x
       ไว้จะคอมไพล์ไม่ผ่านเลย -> เขียนให้รองรับทั้งสองเวอร์ชัน
   [8] WiFi หลุดแล้วเรียก connectWiFi() ใน loop ซึ่งยืนรอ 20 วินาที
       หน้าจอค้างแข็งทั้งช่วงนั้น -> เปลี่ยนเป็นต่อใหม่แบบไม่บล็อก
   [9] HTTP ไม่มี timeout -> เน็ตอืดทีเดียวจอค้างยาว

   *** ต้องแก้ 2 จุดก่อนอัปโหลด: WIFI_SSID, WIFI_PASS ***
   (FB_HOST ตั้งให้ตรงกับหน้าเว็บแล้ว)
   ============================================================ */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <time.h>

// ============================================================
// ส่วนที่ 1 : ตั้งค่าที่ต้องแก้
// ============================================================

const char* WIFI_SSID = "ใส่ชื่อ WiFi ของคุณ";
const char* WIFI_PASS = "ใส่รหัส WiFi ของคุณ";

const char* FB_HOST = "https://solar-station-5b0a8-default-rtdb.asia-southeast1.firebasedatabase.app";

// ------------------------------------------------------------
// เลือกว่าจะใช้กี่บอร์ด
//   false = ใช้บอร์ดเดียว (CYD อ่านเซนเซอร์เอง)  <-- ง่ายกว่า
//   true  = ใช้ 2 บอร์ด (รับข้อมูลจากบอร์ดเซนเซอร์ผ่าน Serial2)
// ------------------------------------------------------------
const bool USE_SERIAL2 = false;

// ============================================================
// ส่วนที่ 2 : ขาที่ใช้
// ============================================================

// ขา ADC ที่ใช้วัดแรงดัน (เป็นขาอ่านอย่างเดียว ใช้ได้เฉพาะรับค่า)
const int PIN_GENERATOR = 34;   // แรงดันจากมอเตอร์กำเนิดไฟฟ้า
const int PIN_BATTERY   = 35;   // แรงดันแบตเตอรี่

/* ⚠️ หมายเหตุเรื่อง GPIO34
   บอร์ด CYD บางรุ่นมีตัววัดแสง (LDR) ต่อไว้ที่ GPIO34 จากโรงงาน
   ซึ่งจะทำให้ค่าที่วัดได้เพี้ยน

   วิธีเช็ก: อัปโหลดโค้ดนี้ แล้วยังไม่ต้องต่อสายวัดอะไรเลย
            เอามือบังแสงบนบอร์ด ถ้าค่า Generator เปลี่ยน = มี LDR อยู่จริง

   ถ้ามี LDR ให้เลือกทางใดทางหนึ่ง:
     ก) เปลี่ยนไปใช้โหมด 2 บอร์ด (ตั้ง USE_SERIAL2 = true)
     ข) บัดกรี LDR ออกจากบอร์ด
*/

// ขาสำหรับ Serial2 (ใช้เมื่อ USE_SERIAL2 = true)
const int PIN_RX2 = 22;
const int PIN_TX2 = 27;

// แสงพื้นหลังจอ
constexpr int TFT_BL = 21;

// ============================================================
// ส่วนที่ 3 : ค่าคงที่สำหรับคำนวณ
// ============================================================

// อัตราส่วนวงจรแบ่งแรงดัน (voltage divider)
// ถ้าใช้ R1=40k, R2=10k จะได้อัตราส่วน 5.0
// ⚠️ ห้ามต่อไฟ 12V เข้าขา ADC ตรง ๆ ขาจะไหม้ทันที
const float DIVIDER_RATIO = 5.0;

// ช่วงแรงดันของแบตเตอรี่ 12V
const float BATT_EMPTY_V = 11.5;   // ต่ำกว่านี้ถือว่าหมด (และแบตจะเสื่อม)
const float BATT_FULL_V  = 14.4;   // ชาร์จเต็ม

// ============================================================
// ส่วนที่ 4 : ตัวแปรเก็บค่า
// ============================================================

float batteryVoltage   = 0.0f;   // แรงดันแบตเตอรี่ (V)
float generatorVoltage = 0.0f;   // แรงดันเจนเนอเรเตอร์ (V)
int   batteryPercent   = 0;      // เปอร์เซ็นต์แบตเตอรี่ (0-100)

unsigned long lastReadMs    = 0;
unsigned long lastPushMs    = 0;
unsigned long lastScreenMs  = 0;
unsigned long lastWifiTryMs = 0;
int lastLoggedHour = -1;

const unsigned long READ_INTERVAL   = 1000;   // อ่านเซนเซอร์ทุก 1 วินาที
const unsigned long PUSH_INTERVAL   = 5000;   // ส่งขึ้น Firebase ทุก 5 วินาที
const unsigned long SCREEN_INTERVAL = 1000;   // อัปเดตจอทุก 1 วินาที
const unsigned long WIFI_RETRY_INTERVAL = 10000;   // WiFi หลุด ลองใหม่ทุก 10 วินาที

String serial2Buffer = "";

// ============================================================
// ส่วนที่ 5 : หน้าจอ TFT
// ============================================================

constexpr uint16_t COLOR_BG     = TFT_BLACK;
constexpr uint16_t COLOR_PANEL  = 0x10A2;
constexpr uint16_t COLOR_HEADER = 0x03EF;
constexpr uint16_t COLOR_TEXT   = TFT_WHITE;
constexpr uint16_t COLOR_MUTED  = 0xAD55;
constexpr uint16_t COLOR_GEN    = TFT_CYAN;
constexpr uint16_t COLOR_BATT   = 0xFD20;
constexpr uint16_t COLOR_OK     = TFT_GREEN;

TFT_eSPI tft = TFT_eSPI();

struct InfoBox {
  int x, y, w, h;
  uint16_t accent;
  const char *title;
};

InfoBox boxes[3] = {
  {10,  45, 300, 55, COLOR_GEN,  "GENERATOR VOLTAGE (V)"},
  {10, 105, 300, 55, COLOR_BATT, "BATTERY LEVEL (%)"},
  {10, 165, 300, 55, COLOR_OK,   "SYSTEM STATUS"},
};

void drawHeader(const char *title) {
  tft.fillRect(0, 0, 320, 42, COLOR_HEADER);
  tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
  tft.setTextSize(2);
  tft.setCursor(12, 12);
  tft.print(title);
}

void drawBoxFrame(const InfoBox &box) {
  tft.fillRoundRect(box.x, box.y, box.w, box.h, 12, COLOR_PANEL);
  tft.drawRoundRect(box.x, box.y, box.w, box.h, 12, box.accent);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(box.x + 12, box.y + 8);
  tft.print(box.title);
}

void drawStaticLayout() {
  tft.fillScreen(COLOR_BG);
  drawHeader("Solar Station");
  for (int i = 0; i < 3; i++) drawBoxFrame(boxes[i]);
}

void updateDisplay() {
  // กล่อง 1 : แรงดันเจนเนอเรเตอร์
  tft.fillRect(boxes[0].x + 12, boxes[0].y + 20, boxes[0].w - 24, boxes[0].h - 26, COLOR_PANEL);
  tft.setTextColor(COLOR_TEXT, COLOR_PANEL);
  tft.setTextSize(3);
  tft.setCursor(boxes[0].x + 12, boxes[0].y + 25);
  tft.printf("%.2f V", generatorVoltage);

  // กล่อง 2 : เปอร์เซ็นต์แบตเตอรี่ + แรงดัน
  tft.fillRect(boxes[1].x + 12, boxes[1].y + 20, boxes[1].w - 24, boxes[1].h - 26, COLOR_PANEL);
  tft.setTextColor(COLOR_BATT, COLOR_PANEL);
  tft.setTextSize(3);
  tft.setCursor(boxes[1].x + 12, boxes[1].y + 25);
  tft.printf("%d %%", batteryPercent);

  tft.setTextSize(1);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setCursor(boxes[1].x + 180, boxes[1].y + 35);
  tft.printf("%.2f V", batteryVoltage);

  // กล่อง 3 : สถานะเครือข่าย
  tft.fillRect(boxes[2].x + 12, boxes[2].y + 20, boxes[2].w - 24, boxes[2].h - 26, COLOR_PANEL);
  tft.setTextSize(2);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(TFT_GREEN, COLOR_PANEL);
    tft.setCursor(boxes[2].x + 12, boxes[2].y + 25);
    tft.print("IP: ");
    tft.print(WiFi.localIP());
  } else {
    tft.setTextColor(TFT_RED, COLOR_PANEL);
    tft.setCursor(boxes[2].x + 12, boxes[2].y + 25);
    tft.print("WIFI LOST");
  }
}

// ============================================================
// ส่วนที่ 6 : อ่านเซนเซอร์
// ============================================================

// อ่านแรงดันจากขา ADC โดยเฉลี่ยหลายครั้งเพื่อลดค่าแกว่ง
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

// แปลงแรงดันแบตเตอรี่เป็นเปอร์เซ็นต์
int voltageToPercent(float v) {
  if (v < 1.0) return 0;   // ยังไม่ได้ต่อสาย

  float pct = (v - BATT_EMPTY_V) / (BATT_FULL_V - BATT_EMPTY_V) * 100.0;

  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;

  return (int)(pct + 0.5);
}

// อ่านเซนเซอร์เอง (โหมดบอร์ดเดียว)
void readSensorsLocal() {
  generatorVoltage = readVoltage(PIN_GENERATOR);
  batteryVoltage   = readVoltage(PIN_BATTERY);
  batteryPercent   = voltageToPercent(batteryVoltage);
}

// รับข้อมูลจากบอร์ดเซนเซอร์ (โหมด 2 บอร์ด)
// รูปแบบที่รอรับ: SENS|85|13.72|8.51
void readSensorsSerial2() {
  while (Serial2.available()) {
    char c = Serial2.read();

    if (c == '\n') {
      serial2Buffer.trim();

      if (serial2Buffer.startsWith("SENS|")) {
        int p1 = serial2Buffer.indexOf('|');
        int p2 = serial2Buffer.indexOf('|', p1 + 1);
        int p3 = serial2Buffer.indexOf('|', p2 + 1);

        if (p2 > 0 && p3 > 0) {
          batteryPercent   = serial2Buffer.substring(p1 + 1, p2).toInt();
          batteryVoltage   = serial2Buffer.substring(p2 + 1, p3).toFloat();
          generatorVoltage = serial2Buffer.substring(p3 + 1).toFloat();
        }
      }
      serial2Buffer = "";

    } else if (c != '\r') {
      serial2Buffer += c;
      if (serial2Buffer.length() > 80) serial2Buffer = "";  // กันข้อมูลขยะล้น
    }
  }
}

// ============================================================
// ส่วนที่ 7 : ส่งข้อมูลขึ้น Firebase   <-- นี่คือส่วนที่ของเดิมไม่มี
// ============================================================

bool firebasePut(const String& path, const String& jsonBody) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();          // ไม่ตรวจใบรับรอง (พอสำหรับงานนี้)

  HTTPClient http;
  http.setConnectTimeout(5000);   // เน็ตอืดแล้วหน้าจอค้างยาว ๆ ไม่ได้
  http.setTimeout(5000);

  String url = String(FB_HOST) + path + ".json";

  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");

  int code = http.PUT(jsonBody);
  http.end();

  if (code != 200) {
    Serial.print("Firebase ล้มเหลว code=");
    Serial.println(code);
    return false;
  }
  return true;
}

// ส่งค่าปัจจุบันไปที่ /live
// ชื่อ field ต้องตรงกับที่หน้าเว็บอ่าน: battery, voltage, generator, uptime, heap, ip
void pushLive() {
  String body = "{";
  body += "\"battery\":"   + String(batteryPercent) + ",";
  body += "\"voltage\":"   + String(batteryVoltage, 2) + ",";
  body += "\"generator\":" + String(generatorVoltage, 2) + ",";
  body += "\"uptime\":"    + String(millis() / 1000) + ",";
  body += "\"heap\":"      + String(ESP.getFreeHeap()) + ",";
  body += "\"ip\":\""      + WiFi.localIP().toString() + "\"";
  body += "}";

  if (firebasePut("/live", body)) {
    Serial.println("ส่งขึ้น Firebase สำเร็จ: " + body);
  }
}

// บันทึกประวัติรายชั่วโมงไปที่ /history/วันที่/ชั่วโมง
void pushHistoryIfNewHour() {
  struct tm t;
  if (!getLocalTime(&t)) return;      // ยังไม่ได้เวลาจาก NTP

  if (t.tm_hour == lastLoggedHour) return;   // ชั่วโมงนี้บันทึกไปแล้ว

  char dateStr[12];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &t);

  String path = "/history/" + String(dateStr) + "/" + String(t.tm_hour);
  String body = "{\"batt\":" + String(batteryPercent) +
                ",\"gen\":"  + String(generatorVoltage, 2) + "}";

  if (firebasePut(path, body)) {
    lastLoggedHour = t.tm_hour;
    Serial.println("บันทึกประวัติชั่วโมงที่ " + String(t.tm_hour));
  }
}

// ============================================================
// ส่วนที่ 8 : WiFi
// ============================================================

void connectWiFi() {
  Serial.print("กำลังต่อ WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nต่อสำเร็จ! IP: " + WiFi.localIP().toString());
    // ตั้งเวลาไทย (UTC+7) เพื่อใช้บันทึกประวัติ
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println("\nต่อ WiFi ไม่ได้ — ระบบจะทำงานต่อแบบออฟไลน์");
  }
  lastWifiTryMs = millis();
}

// ต่อ WiFi ใหม่แบบไม่บล็อก — สั่งแล้วปล่อย ไม่ยืนรอ
// (ของเดิมเรียก connectWiFi() ใน loop ซึ่งยืนรอได้ถึง 20 วินาที
//  หน้าจอจะค้างแข็งทั้งช่วงนั้น เห็นชัดมากตอนสาธิต)
void wifiEnsure() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWifiTryMs < WIFI_RETRY_INTERVAL) return;
  lastWifiTryMs = now;

  Serial.println("WiFi หลุด — สั่งต่อใหม่ (หน้าจอยังลื่นอยู่)");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ============================================================
// setup
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\n=== Solar Charging Station ===");

  // เปิดจอ
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(COLOR_BG);

  // แสงหน้าจอ — คำสั่ง ledc เปลี่ยนหน้าตาระหว่าง ESP32 core 2.x กับ 3.x
  // เขียนแบบนี้เพื่อให้คอมไพล์ผ่านทั้งสองเวอร์ชัน (กันพลาดวันแข่ง)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, 255);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, 255);
#endif

  drawHeader("Booting...");
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  tft.drawString("Connecting WiFi...", 10, 80, 2);

  // ตั้งความละเอียด ADC (0 - 3.3V)
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_GENERATOR, ADC_11db);
  analogSetPinAttenuation(PIN_BATTERY,   ADC_11db);

  if (USE_SERIAL2) {
    Serial2.begin(9600, SERIAL_8N1, PIN_RX2, PIN_TX2);
    Serial.println("โหมด: 2 บอร์ด (รับข้อมูลผ่าน Serial2)");
  } else {
    Serial.println("โหมด: บอร์ดเดียว (อ่าน ADC เอง)");
  }

  connectWiFi();

  drawStaticLayout();
  updateDisplay();
}

// ============================================================
// loop
// ============================================================

void loop() {
  unsigned long now = millis();

  // --- งานที่ 1 : อ่านค่าเซนเซอร์ ---
  if (USE_SERIAL2) {
    readSensorsSerial2();          // อ่านต่อเนื่อง ไม่ต้องหน่วง
  } else if (now - lastReadMs >= READ_INTERVAL) {
    lastReadMs = now;
    readSensorsLocal();
  }

  // --- งานที่ 2 : อัปเดตหน้าจอ ---
  if (now - lastScreenMs >= SCREEN_INTERVAL) {
    lastScreenMs = now;
    updateDisplay();
  }

  // --- งานที่ 3 : ดูแล WiFi (ไม่บล็อก) ---
  wifiEnsure();

  // --- งานที่ 4 : ส่งขึ้น Firebase ---
  if (now - lastPushMs >= PUSH_INTERVAL) {
    lastPushMs = now;

    if (WiFi.status() == WL_CONNECTED) {
      pushLive();
      pushHistoryIfNewHour();
    }
  }
}
