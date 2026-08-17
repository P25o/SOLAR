/* ============================================================
   SOLAR CHARGING STATION — บอร์ดที่ 2 "จอแสดงผล"
   บอร์ด: CYD / ESP32-2432S028 (จอ 2.8" 320x240 ILI9341)

   หน้าจอมี 2 โหมด สลับกันเองอัตโนมัติ:

   [ว่าง]     แสดง QR ให้ลูกค้าสแกนไปหน้าเลือกแพ็กเกจ
              พร้อม % แบตเตอรี่ และแรงดันเจนเนอเรเตอร์

   [ชาร์จอยู่] แสดงเลขนับถอยหลังตัวใหญ่
              พอหมดเวลาจะกลับไปหน้า QR เองอัตโนมัติ

   บอร์ดนี้อ่านอย่างเดียว ไม่สั่งงานอะไรเลย
   ตัวที่วัดไฟและคุม Relay คือบอร์ดที่ 1 (station_controller.ino)

   ------------------------------------------------------------
   ⚠️ ต้องติดตั้งไลบรารีก่อน 2 ตัว
      1. TFT_eSPI          (ตั้งค่า User_Setup.h ด้วย — ดู README_display.md)
      2. QRCode ของ Richard Moore   <-- ตัวใหม่ที่เพิ่งเพิ่ม

      วิธีลง: Arduino IDE > Sketch > Include Library > Manage Libraries
              ค้นหา "QRCode" เลือกอันที่ผู้พัฒนาชื่อ Richard Moore
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
#include "qrcode.h"

// ============================================================
// ส่วนที่ 1 : ตั้งค่าที่ต้องแก้
// ============================================================

const char* WIFI_SSID = "ใส่ชื่อ WiFi ของคุณ";
const char* WIFI_PASS = "ใส่รหัส WiFi ของคุณ";

const char* FB_HOST = "https://solar-station-5b0a8-default-rtdb.asia-southeast1.firebasedatabase.app";

// ที่อยู่เว็บที่ QR จะพาลูกค้าไป
// ⚠️ ยิ่งสั้นยิ่งดี เพราะ QR จะมีจุดน้อยลง สแกนติดง่ายขึ้น
const char* PAY_URL = "https://p25o.github.io/SOLAR/pay.html";

// ============================================================
// ส่วนที่ 2 : จังหวะเวลา
// ============================================================

const unsigned long POLL_INTERVAL       = 3000;   // ดึงข้อมูลจาก Firebase
const unsigned long SCREEN_INTERVAL     = 1000;   // อัปเดตตัวเลขบนจอ
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
  constexpr int BACKLIGHT_PIN = TFT_BL;
#else
  constexpr int BACKLIGHT_PIN = 21;
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
   ข้อความบนจอจึงเป็นภาษาอังกฤษทั้งหมด (ถ้าใส่ไทยจะขึ้นเป็นสี่เหลี่ยมเปล่า) */

// ============================================================
// ส่วนที่ 4 : QR Code
// ============================================================

/* QR ต้องเป็น "จุดดำบนพื้นขาว" เท่านั้น
   ถ้าวาดกลับสีเป็นจุดขาวบนพื้นดำ กล้องมือถือหลายรุ่นจะสแกนไม่ติด
   จึงต้องถมพื้นขาวก่อน แล้วค่อยวาดจุดดำทับ                          */

const int QR_BOX = 168;   // ขนาดกล่องสีขาวทั้งหมด (รวมขอบขาวรอบนอก)
const int QR_X   = 8;     // ตำแหน่งมุมซ้ายบนของกล่อง
const int QR_Y   = 40;
const int QR_PAD = 10;    // ขอบขาวรอบนอก ห้ามน้อยกว่านี้ ไม่งั้นสแกนยาก

#define QR_MAX_VERSION 5  // เผื่อไว้กรณี URL ยาวขึ้นในอนาคต

QRCode  qrcode;
uint8_t qrcodeData[qrcode_getBufferSize(QR_MAX_VERSION)];
bool    qrReady = false;

void buildQR() {
  // ลองเวอร์ชันเล็กก่อน เพราะจุดจะใหญ่และสแกนง่ายกว่า
  if (qrcode_initText(&qrcode, qrcodeData, 3, ECC_MEDIUM, PAY_URL) == 0) { qrReady = true; return; }
  if (qrcode_initText(&qrcode, qrcodeData, 4, ECC_MEDIUM, PAY_URL) == 0) { qrReady = true; return; }
  if (qrcode_initText(&qrcode, qrcodeData, 5, ECC_LOW,    PAY_URL) == 0) { qrReady = true; return; }

  qrReady = false;   // URL ยาวเกินไป
  Serial.println("สร้าง QR ไม่สำเร็จ — URL ยาวเกินไป ลองใช้ที่อยู่ที่สั้นลง");
}

void drawQR() {
  // พื้นขาวเต็มกล่อง (ทำหน้าที่เป็นขอบขาวรอบ QR ไปในตัว)
  tft.fillRect(QR_X, QR_Y, QR_BOX, QR_BOX, TFT_WHITE);

  if (!qrReady) {
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.setTextSize(1);
    tft.setCursor(QR_X + 12, QR_Y + QR_BOX / 2 - 8);
    tft.print("QR ERROR");
    tft.setCursor(QR_X + 12, QR_Y + QR_BOX / 2 + 4);
    tft.print("URL too long");
    return;
  }

  int usable = QR_BOX - QR_PAD * 2;
  int mod    = usable / qrcode.size;        // ขนาดจุดละกี่พิกเซล (ปัดลง)
  if (mod < 1) mod = 1;

  int drawn = mod * qrcode.size;
  int ox    = QR_X + (QR_BOX - drawn) / 2;  // จัดกึ่งกลางกล่อง
  int oy    = QR_Y + (QR_BOX - drawn) / 2;

  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t x = 0; x < qrcode.size; x++) {
      if (qrcode_getModule(&qrcode, x, y)) {
        tft.fillRect(ox + x * mod, oy + y * mod, mod, mod, TFT_BLACK);
      }
    }
  }
}

// ============================================================
// ส่วนที่ 5 : ตัวแปรสถานะ
// ============================================================

int   batteryPercent   = 0;
float batteryVoltage   = 0.0f;
float generatorVoltage = 0.0f;
bool  charging         = false;
long  remainingSeconds = 0;

bool  haveData     = false;
unsigned long lastDataMs    = 0;
unsigned long lastPollMs    = 0;
unsigned long lastScreenMs  = 0;
unsigned long lastTickMs    = 0;
unsigned long lastWifiTryMs = 0;

// หน้าจอมีกี่แบบ
enum ScreenMode { SCREEN_NONE, SCREEN_WAIT, SCREEN_IDLE, SCREEN_CHARGING, SCREEN_OFFLINE };
ScreenMode currentScreen = SCREEN_NONE;

// ค่าที่วาดไปแล้วรอบก่อน ใช้เช็กว่าต้องวาดใหม่ไหม (กันจอกะพริบ)
int  lastDrawnBatt = -999;
long lastDrawnSec  = -999;
float lastDrawnGen = -999.0f;

// ============================================================
// ส่วนที่ 6 : ตัวช่วยวาดข้อความกึ่งกลาง
// ============================================================

// ฟอนต์พื้นฐานของ TFT_eSPI กว้าง 6 พิกเซล สูง 8 พิกเซล ต่อขนาด 1 เท่า
void printCentered(const char* s, int y, uint8_t size, uint16_t fg, uint16_t bg, int centerX = 160) {
  int w = (int)strlen(s) * 6 * size;
  tft.setTextSize(size);
  tft.setTextColor(fg, bg);
  tft.setCursor(centerX - w / 2, y);
  tft.print(s);
}

// ============================================================
// ส่วนที่ 7 : อ่าน JSON แบบง่าย
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
// ส่วนที่ 8 : ดึงข้อมูลจาก Firebase
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
// ส่วนที่ 9 : วาดหน้าจอ
// ============================================================

void drawHeaderBar() {
  tft.fillRect(0, 0, 320, 32, COLOR_HEADER);
  tft.setTextColor(COLOR_TEXT, COLOR_HEADER);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.print("Solar Charging");
}

void drawWifiBar() {
  tft.fillRect(0, 224, 320, 16, COLOR_BG);
  tft.setTextSize(1);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(COLOR_MUTED, COLOR_BG);
    tft.setCursor(8, 228);
    tft.print("WiFi OK  ");
    tft.print(WiFi.localIP());
  } else {
    tft.setTextColor(TFT_RED, COLOR_BG);
    tft.setCursor(8, 228);
    tft.print("WiFi DISCONNECTED");
  }
}

// ---------- หน้า "ว่าง" : QR + แบตเตอรี่ ----------
void drawIdleStatic() {
  tft.fillRect(0, 32, 320, 192, COLOR_BG);
  drawHeaderBar();

  drawQR();

  // คำเชิญใต้ QR
  printCentered("SCAN TO CHARGE", 213, 1, COLOR_OK, COLOR_BG, QR_X + QR_BOX / 2);

  // กล่องแบตเตอรี่ (ขวาบน)
  tft.fillRoundRect(184, 40, 128, 84, 8, COLOR_PANEL);
  tft.drawRoundRect(184, 40, 128, 84, 8, COLOR_BATT);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(194, 48);
  tft.print("BATTERY");

  // กล่องเจนเนอเรเตอร์ (ขวาล่าง)
  tft.fillRoundRect(184, 132, 128, 76, 8, COLOR_PANEL);
  tft.drawRoundRect(184, 132, 128, 76, 8, COLOR_GEN);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setTextSize(1);
  tft.setCursor(194, 140);
  tft.print("GENERATOR");

  lastDrawnBatt = -999;
  lastDrawnGen  = -999.0f;
}

void drawIdleValues() {
  // ---- แบตเตอรี่ ----
  if (batteryPercent != lastDrawnBatt) {
    lastDrawnBatt = batteryPercent;

    tft.fillRect(192, 62, 112, 52, COLOR_PANEL);

    char buf[12];
    snprintf(buf, sizeof(buf), "%d%%", batteryPercent);
    tft.setTextColor(COLOR_BATT, COLOR_PANEL);
    tft.setTextSize(3);
    tft.setCursor(194, 64);
    tft.print(buf);

    snprintf(buf, sizeof(buf), "%.2f V", batteryVoltage);
    tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
    tft.setTextSize(1);
    tft.setCursor(194, 96);
    tft.print(buf);
  }

  // ---- เจนเนอเรเตอร์ ----
  if (fabs(generatorVoltage - lastDrawnGen) > 0.005f) {
    lastDrawnGen = generatorVoltage;

    tft.fillRect(192, 154, 112, 46, COLOR_PANEL);

    char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", generatorVoltage);
    tft.setTextColor(COLOR_GEN, COLOR_PANEL);
    tft.setTextSize(3);
    tft.setCursor(194, 156);
    tft.print(buf);

    tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
    tft.setTextSize(1);
    tft.setCursor(194, 186);
    tft.print("VOLT");
  }
}

// ---------- หน้า "กำลังชาร์จ" : นับถอยหลังตัวใหญ่ ----------
void drawChargingStatic() {
  tft.fillRect(0, 32, 320, 192, COLOR_BG);
  drawHeaderBar();

  printCentered("CHARGING", 48, 3, COLOR_OK, COLOR_BG);
  printCentered("TIME REMAINING", 178, 1, COLOR_MUTED, COLOR_BG);

  lastDrawnSec  = -999;
  lastDrawnBatt = -999;
}

void drawChargingValues() {
  // ---- ตัวเลขนับถอยหลัง ----
  if (remainingSeconds != lastDrawnSec) {
    lastDrawnSec = remainingSeconds;

    long total = remainingSeconds > 0 ? remainingSeconds : 0;
    long m = total / 60;
    long s = total % 60;

    char buf[12];
    snprintf(buf, sizeof(buf), "%02ld:%02ld", m, s);

    tft.fillRect(0, 88, 320, 76, COLOR_BG);

    // เหลือน้อยกว่า 1 นาที เปลี่ยนเป็นสีส้มเตือน
    uint16_t c = (total <= 60) ? COLOR_BATT : COLOR_OK;
    printCentered(buf, 92, 6, c, COLOR_BG);
  }

  // ---- แถบข้อมูลแบตด้านล่าง ----
  if (batteryPercent != lastDrawnBatt) {
    lastDrawnBatt = batteryPercent;

    char buf[40];
    snprintf(buf, sizeof(buf), "Battery %d%%   Gen %.1fV", batteryPercent, generatorVoltage);

    tft.fillRect(0, 198, 320, 18, COLOR_BG);
    printCentered(buf, 200, 1, COLOR_MUTED, COLOR_BG);
  }
}

// ---------- หน้ารอข้อมูล / บอร์ดคุมสถานีหาย ----------
void drawWaitStatic() {
  tft.fillRect(0, 32, 320, 192, COLOR_BG);
  drawHeaderBar();
  printCentered("Waiting for data...", 110, 2, COLOR_MUTED, COLOR_BG);
}

void drawOfflineStatic() {
  tft.fillRect(0, 32, 320, 192, COLOR_BG);
  drawHeaderBar();
  printCentered("CONTROLLER OFFLINE", 100, 2, TFT_RED, COLOR_BG);
  printCentered("No update from station board", 130, 1, COLOR_MUTED, COLOR_BG);
}

// ---------- ตัวสลับหน้าจอ ----------
void updateScreen() {
  bool stale = haveData && (millis() - lastDataMs > STALE_MS);

  ScreenMode want;
  if (!haveData)                            want = SCREEN_WAIT;
  else if (stale)                           want = SCREEN_OFFLINE;
  else if (charging && remainingSeconds > 0) want = SCREEN_CHARGING;
  else                                       want = SCREEN_IDLE;

  // เปลี่ยนหน้า -> วาดโครงใหม่ทั้งหน้า
  if (want != currentScreen) {
    currentScreen = want;
    switch (want) {
      case SCREEN_IDLE:     drawIdleStatic();     break;
      case SCREEN_CHARGING: drawChargingStatic(); break;
      case SCREEN_OFFLINE:  drawOfflineStatic();  break;
      default:              drawWaitStatic();     break;
    }
    Serial.printf("เปลี่ยนหน้าจอ -> %d\n", (int)want);
  }

  // วาดเฉพาะตัวเลขที่เปลี่ยน (กันจอกะพริบ)
  if (currentScreen == SCREEN_IDLE)     drawIdleValues();
  if (currentScreen == SCREEN_CHARGING) drawChargingValues();

  drawWifiBar();
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

  drawHeaderBar();
  printCentered("Connecting WiFi...", 110, 2, COLOR_TEXT, COLOR_BG);

  buildQR();          // สร้าง QR เก็บไว้ในหน่วยความจำ ทำครั้งเดียวพอ
  wifiConnectAtBoot();

  pollFirebase();
  updateScreen();

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

    // หมดเวลาแล้ว -> เด้งกลับหน้า QR ทันที ไม่ต้องรอรอบ poll ถัดไป
    if (remainingSeconds <= 0) {
      remainingSeconds = 0;
      charging = false;
    }
  }

  // --- อัปเดตหน้าจอ ---
  if (now - lastScreenMs >= SCREEN_INTERVAL) {
    lastScreenMs = now;
    updateScreen();
  }
}
