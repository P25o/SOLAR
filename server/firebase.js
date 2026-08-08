/* ============================================================
   คุยกับ Firebase Realtime Database ผ่าน REST API

   ทำไมไม่ใช้ firebase-admin?
   - firebase-admin ต้องใช้ service account (ไฟล์ JSON ยาว ๆ) ตั้งค่ายุ่ง
   - เราเขียนแค่ 2-3 ฟิลด์ ใช้ REST ตรง ๆ ง่ายกว่าและพังยากกว่า
   - ESP32 ก็คุยด้วยวิธีเดียวกันเป๊ะ ดีบักที่เดียวจบ

   Node 18 ขึ้นไปมี fetch มาให้ในตัว ไม่ต้องลง node-fetch
   ============================================================ */

const DB_URL = (process.env.FIREBASE_DB_URL || '').replace(/\/+$/, '');

// ไม่บังคับ — ใส่เมื่อไหร่ก็ต่อเมื่อคุณล็อก Firebase Rules แล้ว
// (Firebase Console > Project settings > Service accounts > Database secrets)
const DB_SECRET = process.env.FIREBASE_DB_SECRET || '';

function buildUrl(path) {
  if (!DB_URL) throw new Error('ยังไม่ได้ตั้ง FIREBASE_DB_URL');
  const query = DB_SECRET ? `?auth=${encodeURIComponent(DB_SECRET)}` : '';
  return `${DB_URL}${path}.json${query}`;
}

async function fbGet(path) {
  const res = await fetch(buildUrl(path));
  if (!res.ok) {
    throw new Error(`Firebase GET ${path} ล้มเหลว: ${res.status} ${await res.text()}`);
  }
  return res.json();   // ถ้าไม่มีข้อมูลจะได้ null
}

async function fbPatch(path, data) {
  const res = await fetch(buildUrl(path), {
    method: 'PATCH',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(data)
  });
  if (!res.ok) {
    throw new Error(`Firebase PATCH ${path} ล้มเหลว: ${res.status} ${await res.text()}`);
  }
  return res.json();
}

async function fbPut(path, data) {
  const res = await fetch(buildUrl(path), {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(data)
  });
  if (!res.ok) {
    throw new Error(`Firebase PUT ${path} ล้มเหลว: ${res.status} ${await res.text()}`);
  }
  return res.json();
}

module.exports = { fbGet, fbPatch, fbPut };
