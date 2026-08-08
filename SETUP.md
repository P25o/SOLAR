# คู่มือติดตั้ง — ฝั่ง Firebase + ระบบชำระเงิน

ทำตามลำดับนี้ ห้ามข้าม แต่ละขั้นมีวิธีเช็กว่าผ่านแล้วจริง

---

## ขั้นที่ 1 — ปลดล็อก Firebase Rules ⚠️ ทำก่อนอย่างอื่น

**ฐานข้อมูลที่เพิ่งสร้างจะถูกล็อกไว้ทั้งอ่านและเขียน** (ทดสอบแล้วได้ `Permission denied` / HTTP 401)
แปลว่าถ้าอัปโหลดโค้ด ESP32 ตอนนี้ ข้อมูลจะไม่ขึ้น Firebase เลย และระบบชำระเงินก็สั่งเปิด Relay ไม่ได้

1. เปิด [Firebase Console](https://console.firebase.google.com) → เลือกโปรเจกต์ `solar-station-5b0a8`
2. เมนูซ้าย → **Realtime Database** → แท็บ **Rules**
3. ลบของเดิมทิ้ง วางเนื้อหาจากไฟล์ `firebase-rules.json` แทน
4. กด **Publish**

**เช็กว่าผ่าน:** เปิด PowerShell แล้วรัน

```bash
curl -X PUT -H "Content-Type: application/json" -d "{\"ok\":true}" "https://solar-station-5b0a8-default-rtdb.asia-southeast1.firebasedatabase.app/_selftest.json"
```

ต้องได้ `{"ok":true}` กลับมา ถ้ายังได้ `Permission denied` แปลว่ายังไม่ได้กด Publish

> กฎชุดนี้เปิดให้ใครก็เขียนได้ เหมาะกับช่วงพัฒนาและวันแข่ง
> หลังงานจบควรปิด หรือใส่ `FIREBASE_DB_SECRET` ในเซิร์ฟเวอร์แล้วล็อกกฎให้แน่นขึ้น

---

## ขั้นที่ 2 — เปิด PromptPay ใน Stripe

1. [Stripe Dashboard](https://dashboard.stripe.com) → มุมขวาบนต้องเป็น **Test mode** (สวิตช์สีส้ม)
2. **Settings → Payment methods** → หา **PromptPay** → กด **Turn on**
3. ไป **Developers → API keys** → คัดลอก **Secret key** (`sk_test_...`) เก็บไว้

---

## ขั้นที่ 3 — เอาโค้ดขึ้น GitHub

โฟลเดอร์ `SOLAR` ทั้งอันเลย (มี `.gitignore` กัน `.env` หลุดไว้ให้แล้ว)

```bash
git init
git add .
git commit -m "solar charging station"
git branch -M main
git remote add origin https://github.com/ชื่อผู้ใช้/solar.git
git push -u origin main
```

---

## ขั้นที่ 4 — สร้างเซิร์ฟเวอร์บน Render

1. [render.com](https://render.com) → สมัครด้วยบัญชี GitHub
2. **New → Web Service** → เลือก repo ที่เพิ่ง push
3. ตั้งค่า:

   | ช่อง | ใส่ |
   |---|---|
   | Root Directory | `server` |
   | Build Command | `npm install` |
   | Start Command | `npm start` |
   | Instance Type | Free |

4. กด **Advanced → Add Environment Variable** ใส่ทีละตัว:

   | Key | Value |
   |---|---|
   | `STRIPE_SECRET_KEY` | `sk_test_...` จากขั้นที่ 2 |
   | `STRIPE_WEBHOOK_SECRET` | เว้นว่างไว้ก่อน เดี๋ยวได้จากขั้นที่ 5 |
   | `FIREBASE_DB_URL` | `https://solar-station-5b0a8-default-rtdb.asia-southeast1.firebasedatabase.app` |
   | `PUBLIC_WEB_URL` | `https://ชื่อผู้ใช้.github.io/solar/pay.html` |
   | `DEFAULT_STATION_ID` | `station_01` |
   | `DEMO_MODE` | `true` |

5. กด **Create Web Service** รอ deploy ~2 นาที
6. จดที่อยู่ที่ได้ไว้ เช่น `https://solar-xxxx.onrender.com`

**เช็กว่าผ่าน:** เปิดที่อยู่นั้นในเบราว์เซอร์ ต้องเห็น

```json
{"ok":true,"service":"solar-station-server","demoMode":true,"time":"..."}
```

---

## ขั้นที่ 5 — ต่อ Webhook ของ Stripe

1. Stripe Dashboard → **Developers → Webhooks → Add endpoint**
2. **Endpoint URL:** `https://solar-xxxx.onrender.com/api/webhook`
3. **Select events** เลือก 4 ตัวนี้:
   - `checkout.session.completed`
   - `checkout.session.async_payment_succeeded`
   - `checkout.session.async_payment_failed`
   - `checkout.session.expired`
4. กด Add endpoint → คัดลอก **Signing secret** (`whsec_...`)
5. กลับไป Render → **Environment** → แก้ `STRIPE_WEBHOOK_SECRET` เป็นค่านี้ → **Save** (Render จะ deploy ใหม่เอง)

---

## ขั้นที่ 6 — เอาหน้าเว็บขึ้น GitHub Pages

1. (ทำให้แล้ว) ไฟล์หน้า Dashboard ชื่อ `index.html` ตามที่ GitHub Pages บังคับ
2. แก้ `pay.html` บรรทัด `API_BASE` ให้เป็นที่อยู่ Render จริง

   ```js
   API_BASE: 'https://solar-xxxx.onrender.com',
   ```

3. push ขึ้น GitHub
4. GitHub → repo → **Settings → Pages** → Source เลือก `main` / `root` → Save
5. รอ ~1 นาที จะได้ 2 หน้า:
   - `https://ชื่อผู้ใช้.github.io/solar/` — หน้า Dashboard พลังงาน
   - `https://ชื่อผู้ใช้.github.io/solar/pay.html` — หน้าจ่ายเงิน

---

## ขั้นที่ 7 — ทดสอบจ่ายเงิน (ยังไม่ต้องมีบอร์ด)

1. เปิด `pay.html` บนมือถือ → ต้องเห็นแพ็กเกจ 3 อัน
   (ครั้งแรกอาจรอ 30-60 วินาที เพราะ Render ต้องตื่นก่อน)
2. กดแพ็ก 10 บาท → เด้งไปหน้า Stripe แสดง QR PromptPay
3. **หน้านี้เป็นโหมดทดสอบ** จะมีปุ่มจำลองให้กด ไม่ต้องสแกนจริง — กดปุ่มที่แปลว่า "จ่ายสำเร็จ"
4. เด้งกลับมาที่ `pay.html` ขึ้นแถบเขียว "ชำระเงินสำเร็จ"
5. **จุดสำคัญ** ไปเปิด Firebase Console → Realtime Database → ต้องเห็น

   ```
   stations/station_01/
       relayState       : "on"
       remainingSeconds : 15
       sessionId        : "cs_test_..."
       packageName      : "แพ็กเริ่มต้น"
   ```

ถ้าเห็นแบบนี้ = **ระบบชำระเงินเสร็จสมบูรณ์** เหลือแค่รอบอร์ดมาอ่านค่าไปเปิด Relay

---

## เวลาพัง ดูตรงไหน

| อาการ | สาเหตุที่เจอบ่อยสุด |
|---|---|
| `pay.html` ขึ้น "ติดต่อเซิร์ฟเวอร์ไม่ได้" | `API_BASE` ผิด หรือ Render กำลังตื่น รอ 1 นาทีแล้วรีเฟรช |
| กดแพ็กแล้วขึ้น "ยังไม่ได้เปิด PromptPay" | ย้อนไปทำขั้นที่ 2 |
| จ่ายแล้วแต่ Firebase ไม่ขยับ | Render → Logs ดูบรรทัด `ตรวจลายเซ็น webhook ไม่ผ่าน` = `STRIPE_WEBHOOK_SECRET` ผิด |
| จ่ายแล้ว Log ขึ้น `Firebase PATCH ล้มเหลว: 401` | ย้อนไปทำขั้นที่ 1 |
| Stripe Dashboard → Webhooks ขึ้นสีแดง | คลิกเข้าไปดูได้ว่าเซิร์ฟเวอร์ตอบอะไรกลับ |

Render → แท็บ **Logs** คือที่ที่ต้องดูเป็นอันดับแรกเสมอ

---

## รันในเครื่องตัวเอง (ถ้าอยากลองก่อน deploy)

```bash
cd server
npm install
copy .env.example .env
npm start
```

แก้ค่าใน `.env` ให้ครบก่อน แล้วเปิด `http://localhost:3000`
ส่วน webhook ต้องใช้ [Stripe CLI](https://stripe.com/docs/stripe-cli) ช่วยส่งต่อ:

```bash
stripe listen --forward-to localhost:3000/api/webhook
```
