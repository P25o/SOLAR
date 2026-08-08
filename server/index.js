/* ============================================================
   Solar Charging Station — เซิร์ฟเวอร์ชำระเงิน
   รันบน Render (Node 18+)

   หน้าที่ทั้งหมดของไฟล์นี้:
   1. ให้หน้าเว็บดึงรายการแพ็กเกจไปแสดง
   2. ลูกค้ากดเลือกแพ็ก -> สร้าง Stripe Checkout (PromptPay) ส่ง URL กลับไป
   3. ลูกค้าสแกนจ่ายเสร็จ -> Stripe ยิง webhook มาบอก
   4. เราเขียนคำสั่งลง Firebase -> ESP32 อ่านเจอ -> เปิด Relay

   *** จุดสำคัญที่พลาดกันบ่อย ***
   - route /api/webhook ต้องรับ body แบบ "ดิบ" (raw) เท่านั้น
     ถ้าโดน express.json() แปลงก่อน ลายเซ็นของ Stripe จะตรวจไม่ผ่าน
     เพราะฉะนั้นบรรทัด app.use(express.json()) ต้องอยู่ "หลัง" route นี้
   ============================================================ */

// โหลดไฟล์ .env ตอนรันในเครื่องตัวเอง
// (บน Render ไม่ต้องมีไฟล์นี้ ใช้หน้า Environment ของ Render แทน)
// ต้องอยู่บรรทัดบนสุด เพราะไฟล์อื่นอ่าน process.env ตั้งแต่ตอนถูก require
try { require('dotenv').config(); } catch (_) {}

const express = require('express');
const Stripe = require('stripe');

const { PACKAGES, findPackage } = require('./packages');
const { fbGet, fbPatch } = require('./firebase');

const app = express();
const PORT = process.env.PORT || 3000;

const stripe = new Stripe(process.env.STRIPE_SECRET_KEY || '');

const WEBHOOK_SECRET  = process.env.STRIPE_WEBHOOK_SECRET || '';
const PUBLIC_WEB_URL  = process.env.PUBLIC_WEB_URL || 'http://localhost:5500/pay.html';
const DEFAULT_STATION = process.env.DEFAULT_STATION_ID || 'station_01';

// โหมดสาธิต: แปลง "นาที" เป็น "วินาที"
// จ่าย 10 บาท -> Relay ตัดใน 15 วินาที กรรมการเห็นครบวงจรโดยไม่ต้องยืนรอ
const DEMO_MODE = process.env.DEMO_MODE === 'true';

// เพดานเดียวกับที่ ESP32 ตั้งไว้ (4 ชั่วโมง) กันเติมเวลาจนบานปลาย
const MAX_SESSION_SECONDS = 4 * 60 * 60;

function packageSeconds(pkg) {
  return DEMO_MODE ? pkg.minutes : pkg.minutes * 60;
}

// ------------------------------------------------------------
// CORS — หน้าเว็บอยู่บน GitHub Pages คนละโดเมนกับเซิร์ฟเวอร์
// ------------------------------------------------------------
app.use((req, res, next) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  res.setHeader('Access-Control-Allow-Methods', 'GET,POST,OPTIONS');
  if (req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});

// ============================================================
// Webhook — ต้องมาก่อน express.json() เสมอ (อ่านหมายเหตุด้านบน)
// ============================================================

app.post('/api/webhook', express.raw({ type: 'application/json' }), async (req, res) => {
  let event;

  try {
    event = stripe.webhooks.constructEvent(
      req.body,
      req.headers['stripe-signature'],
      WEBHOOK_SECRET
    );
  } catch (err) {
    console.error('ตรวจลายเซ็น webhook ไม่ผ่าน:', err.message);
    return res.status(400).send(`Webhook Error: ${err.message}`);
  }

  // ตอบ Stripe ให้ไวที่สุด ไม่งั้นมันจะคิดว่าเราล่มแล้วยิงซ้ำ
  res.json({ received: true });

  try {
    switch (event.type) {
      // PromptPay จ่ายเสร็จภายในหน้า Checkout เลย -> เข้าเคสนี้
      case 'checkout.session.completed': {
        const session = event.data.object;
        if (session.payment_status === 'paid') {
          await handlePaid(session);
        } else {
          console.log('session เสร็จแล้วแต่ยังไม่จ่าย รอ async:', session.id);
        }
        break;
      }

      // เผื่อ Stripe จัดให้เป็นวิธีจ่ายแบบหน่วงเวลา
      case 'checkout.session.async_payment_succeeded':
        await handlePaid(event.data.object);
        break;

      case 'checkout.session.async_payment_failed':
        console.log('จ่ายไม่สำเร็จ:', event.data.object.id);
        break;

      case 'checkout.session.expired':
        console.log('QR หมดอายุก่อนจ่าย:', event.data.object.id);
        break;

      default:
        break;
    }
  } catch (err) {
    console.error('ประมวลผล webhook พลาด:', err);
  }
});

// ============================================================
// ตั้งแต่บรรทัดนี้ลงไปถึงจะแปลง JSON ได้
// ============================================================

app.use(express.json());

// ------------------------------------------------------------
// สั่งเปิด Relay หลังจ่ายเงินสำเร็จ
// ------------------------------------------------------------
async function handlePaid(session) {
  const sessionId = session.id;
  const stationId = session.metadata?.stationId || DEFAULT_STATION;
  const seconds   = parseInt(session.metadata?.seconds || '0', 10);
  const pkgName   = session.metadata?.packageName || '-';

  if (!seconds) {
    console.error('ไม่มีข้อมูลเวลาใน metadata ของ', sessionId);
    return;
  }

  // --- กันทำงานซ้ำ ---
  // Stripe ยิง webhook ซ้ำได้ถ้าคิดว่าเราตอบช้า ถ้าไม่กันตรงนี้
  // ลูกค้าจ่ายรอบเดียวแต่ได้เวลา 2 เท่า
  const already = await fbGet(`/payments/${sessionId}`);
  if (already) {
    console.log('webhook ซ้ำ ข้ามไป:', sessionId);
    return;
  }

  // --- ถ้าสถานีกำลังใช้งานอยู่ ให้เติมเวลาต่อจากของเดิม ---
  const current = await fbGet(`/stations/${stationId}`);
  let base = 0;
  if (current && current.relayState === 'on' && Number(current.remainingSeconds) > 0) {
    base = Number(current.remainingSeconds);
    console.log(`สถานีใช้งานอยู่ เหลือ ${base} วินาที -> เติมต่อ`);
  }

  let total = base + seconds;
  if (total > MAX_SESSION_SECONDS) total = MAX_SESSION_SECONDS;

  // sessionId ของ Stripe คือตัวเดียวกับที่ ESP32 ใช้ตัดสินว่า "รอบใหม่"
  // ค่านี้ไม่มีทางซ้ำ จึงกันบั๊ก Relay ค้างเปิดได้ในตัว
  await fbPatch(`/stations/${stationId}`, {
    relayState: 'on',
    remainingSeconds: total,
    sessionId: sessionId,
    packageName: pkgName,
    updatedAt: Date.now()
  });

  await fbPatch(`/payments/${sessionId}`, {
    stationId,
    packageName: pkgName,
    amountSatang: session.amount_total,
    seconds,
    paidAt: Date.now()
  });

  console.log(`✅ เปิด ${stationId} รวม ${total} วินาที (${pkgName}) session=${sessionId}`);
}

// ============================================================
// API สำหรับหน้าเว็บ
// ============================================================

// เช็กว่าเซิร์ฟเวอร์ตื่นอยู่ไหม — หน้าเว็บจะยิงมาปลุกตอนเปิดหน้า
app.get('/', (req, res) => {
  res.json({
    ok: true,
    service: 'solar-station-server',
    demoMode: DEMO_MODE,
    time: new Date().toISOString()
  });
});

// รายการแพ็กเกจ (หน้าเว็บดึงไปแสดง จะได้ไม่ต้องแก้ราคา 2 ที่)
app.get('/api/packages', (req, res) => {
  res.json({
    demoMode: DEMO_MODE,
    packages: PACKAGES.map(p => ({
      id: p.id,
      name: p.name,
      note: p.note,
      baht: p.amountSatang / 100,
      minutes: p.minutes,
      seconds: packageSeconds(p)
    }))
  });
});

// สถานะสถานีตอนนี้
app.get('/api/status', async (req, res) => {
  try {
    const stationId = req.query.stationId || DEFAULT_STATION;
    const data = await fbGet(`/stations/${stationId}`);
    res.json({
      stationId,
      relayState: data?.relayState || 'off',
      remainingSeconds: Number(data?.remainingSeconds || 0),
      packageName: data?.packageName || null
    });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: 'อ่านสถานะไม่ได้' });
  }
});

// สร้างหน้าจ่ายเงิน Stripe
app.post('/api/checkout', async (req, res) => {
  try {
    const { packageId, stationId } = req.body || {};

    const pkg = findPackage(packageId);
    if (!pkg) return res.status(400).json({ error: 'ไม่รู้จักแพ็กเกจนี้' });

    const station = stationId || DEFAULT_STATION;
    const seconds = packageSeconds(pkg);

    const timeLabel = DEMO_MODE
      ? `${pkg.minutes} วินาที (โหมดสาธิต)`
      : `${pkg.minutes} นาที`;

    const session = await stripe.checkout.sessions.create({
      mode: 'payment',
      payment_method_types: ['promptpay'],
      line_items: [{
        price_data: {
          currency: 'thb',
          unit_amount: pkg.amountSatang,
          product_data: {
            name: `${pkg.name} — ชาร์จ ${timeLabel}`,
            description: `สถานี ${station}`
          }
        },
        quantity: 1
      }],
      metadata: {
        stationId: station,
        packageId: pkg.id,
        packageName: pkg.name,
        seconds: String(seconds)
      },
      success_url: `${PUBLIC_WEB_URL}?paid=1&session_id={CHECKOUT_SESSION_ID}`,
      cancel_url: `${PUBLIC_WEB_URL}?canceled=1`
    });

    res.json({ url: session.url, sessionId: session.id });

  } catch (err) {
    console.error('สร้าง checkout ไม่สำเร็จ:', err.message);

    // ข้อความช่วยดีบักตอนตั้งค่ายังไม่ครบ
    if (/promptpay/i.test(err.message)) {
      return res.status(500).json({
        error: 'ยังไม่ได้เปิดใช้ PromptPay ใน Stripe Dashboard ' +
               '(Settings > Payment methods > PromptPay > Turn on)'
      });
    }
    res.status(500).json({ error: err.message });
  }
});

// ------------------------------------------------------------
// ตรวจว่าตั้งค่าครบไหม แล้วค่อยเปิดเซิร์ฟเวอร์
// ------------------------------------------------------------
const required = ['STRIPE_SECRET_KEY', 'STRIPE_WEBHOOK_SECRET', 'FIREBASE_DB_URL'];
const missing = required.filter(k => !process.env[k]);

app.listen(PORT, () => {
  console.log(`เซิร์ฟเวอร์ทำงานที่พอร์ต ${PORT}`);
  console.log(`โหมดสาธิต: ${DEMO_MODE ? 'เปิด (นาที -> วินาที)' : 'ปิด'}`);
  if (missing.length) {
    console.warn(`⚠️  ยังไม่ได้ตั้งค่า: ${missing.join(', ')} — API จะยังใช้ไม่ได้`);
  }
});
