/* ============================================================
   SOLAR CHARGING STATION — บอร์ดที่ 2 "จอแสดงผล"
   บอร์ด: CYD / ESP32-2432S028 (จอ 2.8" 320x240 ILI9341)

   หน้าที่ของบอร์ดนี้: อ่านอย่างเดียว ไม่สั่งงานอะไรเลย
   1. อ่าน /live จาก Firebase (ครั้งเดียวได้ครบทุกค่า)
   2. แสดงแบตเตอรี่ / เจนเนอเรเตอร์ / สถานะการชาร์จ
   3. นับถอยหลังบนจอให้ลื่นตา ระหว่างรอข้อมูลรอบถัดไป

   *** ไม่ต้องต่อสายอะไรเพิ่มเลย เสียบไฟอย่างเดียว ***
   ตัวที่วัดไฟและคุม Relay คือบอร์ดที่ 1 (station_controller.ino)

   ------------------------------------------------------------
   ⚠️ ก่อนอัปโหลด ต้องตั้งค่าไลบรารี TFT_eSPI ก่อน ไม่งั้นจอจะขาวโพลน
      อ่านวิธีทำในไฟล์ README_display.md
   ------------------------------------------------------------

   *** ต้องแก้ 2 บรรทัดก่อนอัปโหลด: WIFI_SSID, WIFI_PASS ***

   ตั้งค่าใน Arduino IDE:
     Board: "ESP32 Dev Module"
   ============================================================ */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>

// ============================================================
// ส่วนที่ 1 : ตั้งค่าที่ต้องแก้
// ============================================================

const char* WIFI_SSID = "ใส่ชื่อ WiFi ของคุณ";
const char* WIFI_PASS = "ใส่รหัส WiFi ของคุณ";

const char* FB_HOST = "https://solar-station-5b0a8-default-rtdb.asia-southeast1.firebasedatabase.app";

// ============================================================
// ส่วนที่ 2 : จังหวะเวลา
// ============================================================

const unsigned long POLL_INTERVAL       = 3000;   // ดึงข้อมูลจาก Firebase
const unsigned long SCREEN_INTERVAL     = 1000;   // วาดจอใหม่
const unsigned long WIFI_RETRY_INTERVAL = 10000;
const unsigned long WIFI_BOOT_TIMEOUT   = 15000;

// ข้อมูลเก่าเกินกี่มิลลิวินาทีถือว่าบอร์ดคุมสถานีขาดการติดต่อ
const unsigned long STALE_MS = 20000;

// ============================================================
// ส่วนที่ 3 : จอ
// ============================================================

/* ขาแสงหน้าจอของ CYD
   ⚠️ ห้ามตั้งชื่อตัวแปรว่า TFT_BL เด็ดขาด
      เพราะไฟล์ User_Setup.h ของ TFT_eSPI ประกาศ TFT_BL ไว้เป็นมาโครแล้ว
      ตัวแปลภาษาจะแทนชื่อด้วยเลข 21 ก่อน กลายเป็น "constexpr int 21 = 21;"
      ซึ่งคอมไพล์ไม่ผ่าน                                                   */
#ifdef TFT_BL
  constexpr int BACKLIGHT_PIN = TFT_BL;   // ใช้ค่าที่ User_Setup.h ตั้งไว้
#else
  constexpr int BACKLIGHT_PIN = 21;       // เผื่อกรณีไม่ได้ประกาศไว้
#endif

constexpr uint16_t COLOR_BG     = TFT_BLACK;
constexpr uint16_t COLOR_PANEL  = 0x10A2;
constexpr uint16_t COLOR_HEADER = 0x03EF;
constexpr uint16_t COLOR_TEXT   = TFT_WHITE;
constexpr uint16_t COLOR_MUTED  = 0xAD55;
constexpr uint16_t COLOR_GEN    = TFT_CYAN;
constexpr uint16_t COLOR_BATT   = 0xFD20;
constexpr uint16_t COLOR_OK     = TFT_GREEN;

TFT_eSPI tft = TFT_eSPI();

/* หมายเหตุ: ฟอนต์ในตัวของ TFT_eSPI ไม่มีภาษาไทย
   ข้อความบนจอจึงเป็นภาษาอังกฤษทั้งหมด (ถ้าใส่ไทยจะขึ้นเป็นสี่เหลี่ยม) */

// ============================================================
// ส่วนที่ 4 : ตัวแปรสถานะ
// ============================================================

int   batteryPercent   = 0;
float batteryVoltage   = 0.0f;
float generatorVoltage = 0.0f;
bool  charging         = false;
long  remainingSeconds = 0;

bool  haveData      = false;
unsigned long lastDataMs   = 0;   // ได้ข้อมูลจาก Firebase ล่าสุดเมื่อไหร่
unsigned long lastPollMs   = 0;
unsigned long lastScreenMs = 0;
unsigned long lastTickMs   = 0;
unsigned long lastWifiTryMs = 0;

// ============================================================
// ส่วนที่ 5 : อ่าน JSON แบบง่าย
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

// ============================================================
// ส่วนที่ 6 : ดึงข้อมูลจาก Firebase
// ============================================================

void pollFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);

  String url = String(FB_HOST) + "/live.json";
  if (!http.begin(client, url)) return;

  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    payload.trim();

    if (payload.length() > 2 && payload != "null") {
      batteryPercent   = extractValue(payload, "battery").toInt();
      batteryVoltage   = extractValue(payload, "voltage").toFloat();
      generatorVoltage = extractValue(payload, "generator").toFloat();

      String st = extractValue(payload, "relayState");
      charging  = (st == "on");

      String rs = extractValue(payload, "remainingSeconds");
      if (rs.length()) remainingSeconds = rs.toInt();
      if (!charging) remainingSeconds = 0;

      haveData   = true;
      lastDataMs = millis();
      lastTickMs = millis();

      Serial.println("Firebase -> " + payload);
    }
  } else {
    Serial.printf("อ่าน Firebase ไม่สำเร็จ code=%d\n", code);
  }

  http.end();
}

// ============================================================
// ส่วนที่ 7 : วาดหน้าจอ
// ============================================================

void drawStaticLayout() {
  tft.fillScreen(COLOR_BG);

  // แถบหัว
  tft.fillRect(0, 0, 320, 34, COLOR_HEADER);
  tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
  tft.setTextSize(2);
  tft.setCursor(10, 9);
  tft.print("Solar Charging Station");

  // กล่องซ้าย: แบตเตอรี่
  tft.fillRoundRect(8, 42, 148, 62, 8, COLOR_PANEL);
  tft.drawRoundRect(8, 42, 148, 62, 8, COLOR_BATT);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(18, 50);
  tft.print("BATTERY");

  // กล่องขวา: เจนเนอเรเตอร์
  tft.fillRoundRect(164, 42, 148, 62, 8, COLOR_PANEL);
  tft.drawRoundRect(164, 42, 148, 62, 8, COLOR_GEN);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(174, 50);
  tft.print("GENERATOR");

  // กล่องใหญ่: สถานะการชาร์จ
  tft.fillRoundRect(8, 112, 304, 84, 10, COLOR_PANEL);
  tft.drawRoundRect(8, 112, 304, 84, 10, COLOR_OK);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(18, 120);
  tft.print("CHARGING STATUS");
}

void drawValues() {
  // --- แบตเตอรี่ ---
  tft.fillRect(16, 62, 132, 36, COLOR_PANEL);
  tft.setTextColor(COLOR_BATT, COLOR_PANEL);
  tft.setTextSize(3);
  tft.setCursor(18, 64);
  tft.printf("%d%%", batteryPercent);

  tft.setTextSize(1);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setCursor(18, 90);
  tft.printf("%.2f V", batteryVoltage);

  // --- เจนเนอเรเตอร์ ---
  tft.fillRect(172, 62, 132, 36, COLOR_PANEL);
  tft.setTextColor(COLOR_GEN, COLOR_PANEL);
  tft.setTextSize(3);
  tft.setCursor(174, 64);
  tft.printf("%.1f", generatorVoltage);

  tft.setTextSize(1);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setCursor(174, 90);
  tft.print("VOLT");

  // --- สถานะการชาร์จ ---
  tft.fillRect(16, 132, 288, 58, COLOR_PANEL);

  bool stale = haveData && (millis() - lastDataMs > STALE_MS);

  if (!haveData) {
    tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
    tft.setTextSize(2);
    tft.setCursor(18, 150);
    tft.print("Waiting for data...");

  } else if (stale) {
    tft.setTextColor(TFT_RED, COLOR_PANEL);
    tft.setTextSize(2);
    tft.setCursor(18, 142);
    tft.print("CONTROLLER OFFLINE");
    tft.setTextSize(1);
    tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
    tft.setCursor(18, 168);
    tft.print("No update from station board");

  } else if (charging && remainingSeconds > 0) {
    long m = remainingSeconds / 60;
    long s = remainingSeconds % 60;

    tft.setTextColor(COLOR_OK, COLOR_PANEL);
    tft.setTextSize(2);
    tft.setCursor(18, 136);
    tft.print("CHARGING");

    tft.setTextSize(4);
    tft.setCursor(18, 158);
    tft.printf("%02ld:%02ld", m, s);

    tft.setTextSize(1);
    tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
    tft.setCursor(150, 176);
    tft.print("time remaining");

  } else {
    tft.setTextColor(COLOR_TEXT, COLOR_PANEL);
    tft.setTextSize(3);
    tft.setCursor(18, 142);
    tft.print("READY");

    tft.setTextSize(1);
    tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
    tft.setCursor(18, 174);
    tft.print("Scan QR to start charging");
  }

  // --- แถบล่าง: สถานะเครือข่าย ---
  tft.fillRect(0, 204, 320, 36, COLOR_BG);
  tft.setTextSize(1);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(COLOR_OK, COLOR_BG);
    tft.setCursor(10, 216);
    tft.print("WiFi OK  ");
    tft.print(WiFi.localIP());
  } else {
    tft.setTextColor(TFT_RED, COLOR_BG);
    tft.setCursor(10, 216);
    tft.print("WiFi DISCONNECTED");
  }
}

// ============================================================
// ส่วนที่ 8 : WiFi
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

  if (WiFi.status() == WL_CONNECTED) Serial.println("\nต่อสำเร็จ! IP: " + WiFi.localIP().toString());
  else                               Serial.println("\nต่อ WiFi ไม่ได้ — จะลองใหม่เรื่อย ๆ");

  lastWifiTryMs = millis();
}

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
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Solar Charging Station — Display ===");

  tft.init();
  tft.setRotation(1);           // แนวนอน 320x240
  tft.fillScreen(COLOR_BG);

  // แสงหน้าจอ — คำสั่ง ledc ต่างกันระหว่าง ESP32 core 2.x กับ 3.x
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BACKLIGHT_PIN, 5000, 8);
  ledcWrite(BACKLIGHT_PIN, 255);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(BACKLIGHT_PIN, 0);
  ledcWrite(0, 255);
#endif

  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  tft.setTextSize(2);
  tft.setCursor(10, 100);
  tft.print("Connecting WiFi...");

  wifiConnectAtBoot();

  drawStaticLayout();
  pollFirebase();
  drawValues();

  lastPollMs   = millis();
  lastScreenMs = millis();
}

// ============================================================
void loop() {
  unsigned long now = millis();

  wifiEnsure();

  // --- ดึงข้อมูลจาก Firebase เป็นระยะ ---
  if (now - lastPollMs >= POLL_INTERVAL) {
    lastPollMs = now;
    pollFirebase();
  }

  /* --- นับถอยหลังบนจอเองระหว่างรอข้อมูลรอบถัดไป ---
     ตัวเลขจะได้เดินทุกวินาทีให้ลูกค้าเห็น ไม่กระตุกเป็นช่วง ๆ
     ค่าจริงยังยึดตามที่บอร์ดคุมสถานีส่งมา พอถึงรอบ poll จะถูกแก้ให้ตรง */
  if (charging && remainingSeconds > 0 && (now - lastTickMs >= 1000)) {
    lastTickMs += 1000;
    remainingSeconds--;
  }

  // --- วาดจอใหม่ ---
  if (now - lastScreenMs >= SCREEN_INTERVAL) {
    lastScreenMs = now;
    drawValues();
  }
}
