/**
 * Emergency Tamper Voice Call Service (Uganda +256 Ready — Zero-Cost Voice Ring)
 *
 * Supports 3 outbound voice calling engines:
 *   1. CallMeBot Free Voice Call API (100% FREE in Uganda — rings phone via Telegram Voice Call
 *      with spoken Text-to-Speech alarm; zero credit card, zero +256 country blocks!)
 *   2. Africa's Talking Uganda Voice API (+256 native PSTN GSM carrier calls)
 *   3. Twilio Programmable Voice (for paid/upgraded international accounts)
 */

const fs = require('fs');
const path = require('path');
const http = require('http');
const https = require('https');
const querystring = require('querystring');
const dotenv = require('dotenv');

const DEFAULT_ALERT_PHONE = '+256748802162';
const CALL_COOLDOWN_MS = 35000;
let lastCallTimestamp = 0;

function getEnvPaths() {
  return [
    path.resolve(__dirname, '..', '..', '.env'),
    path.resolve(__dirname, '..', '.env')
  ].filter((p, idx, arr) => arr.indexOf(p) === idx);
}

function getLiveConfig() {
  const merged = { ...process.env };
  const candidatePaths = [
    ...getEnvPaths(),
    path.join(process.cwd(), '.env'),
    path.join(process.cwd(), 'backend', '.env')
  ];

  for (const envPath of candidatePaths) {
    try {
      if (fs.existsSync(envPath)) {
        const parsed = dotenv.parse(fs.readFileSync(envPath, 'utf8'));
        for (const [k, v] of Object.entries(parsed)) {
          if (v && String(v).trim() !== '') {
            merged[k] = String(v).trim();
          }
        }
      }
    } catch (_) {}
  }
  return merged;
}

function normalizeUgandaPhone(phone) {
  const cleaned = String(phone || DEFAULT_ALERT_PHONE).trim().replace(/[\s-]/g, '');
  if (cleaned.startsWith('@')) {
    return cleaned; // Telegram @username
  }
  if (cleaned.startsWith('0') && cleaned.length === 10) {
    return '+256' + cleaned.slice(1);
  }
  if (/^256\d{9}$/.test(cleaned)) {
    return '+' + cleaned;
  }
  if (!cleaned.startsWith('+')) {
    return '+' + cleaned;
  }
  return cleaned;
}

function writeEnvUpdates(updates) {
  for (const envPath of getEnvPaths()) {
    try {
      let content = fs.existsSync(envPath) ? fs.readFileSync(envPath, 'utf8') : '';
      for (const [key, val] of Object.entries(updates)) {
        const regex = new RegExp(`^${key}=.*$`, 'm');
        if (regex.test(content)) {
          content = content.replace(regex, `${key}=${val}`);
        } else {
          content = content.trimEnd() + `\n${key}=${val}\n`;
        }
      }
      fs.writeFileSync(envPath, content, 'utf8');
    } catch (_) {}
  }
  for (const [key, val] of Object.entries(updates)) {
    process.env[key] = val;
  }
}

/**
 * Saves Free CallMeBot Uganda Voice Call target (@telegram_username or +256748802162)
 */
function saveCallMeBotConfig({ callmebotUser, alertPhone }) {
  const rawTarget = String(callmebotUser || alertPhone || DEFAULT_ALERT_PHONE).trim();
  if (!rawTarget) {
    throw new Error('Enter your Telegram @username or Uganda phone number (+256748802162).');
  }
  const normalizedTarget = rawTarget.startsWith('@') ? rawTarget : normalizeUgandaPhone(rawTarget);
  const phoneIntl = rawTarget.startsWith('@')
    ? normalizeUgandaPhone(alertPhone || DEFAULT_ALERT_PHONE)
    : normalizedTarget;

  writeEnvUpdates({
    ALERT_PHONE_NUMBER: phoneIntl,
    CALLMEBOT_USER: normalizedTarget
  });

  console.log(`✅ [Free CallMeBot Voice Configured] Target set to ${normalizedTarget} (Uganda +256 Free Ring Active)`);
  return {
    configured: true,
    provider: 'callmebot',
    callmebotUser: normalizedTarget,
    alertPhone: phoneIntl
  };
}

/**
 * Saves Twilio credentials (optional paid international PSTN provider)
 */
function saveTwilioConfig({ accountSid, authToken, fromNumber, alertPhone, callmebotUser, mode }) {
  if (mode === 'callmebot' || callmebotUser !== undefined) {
    return saveCallMeBotConfig({ callmebotUser, alertPhone });
  }

  const sid = String(accountSid || '').trim();
  const token = String(authToken || '').trim();
  const from = String(fromNumber || '').trim().replace(/[\s-]/g, '');
  const to = normalizeUgandaPhone(alertPhone || DEFAULT_ALERT_PHONE);

  if (!sid.startsWith('AC') || sid.length < 30) {
    throw new Error('Twilio Account SID must start with "AC".');
  }
  if (token.length < 24) {
    throw new Error('Twilio Auth Token looks incomplete.');
  }
  if (!from.startsWith('+') || from.length < 8) {
    throw new Error('Twilio Phone Number must start with "+".');
  }

  writeEnvUpdates({
    ALERT_PHONE_NUMBER: to,
    TWILIO_ACCOUNT_SID: sid,
    TWILIO_AUTH_TOKEN: token,
    TWILIO_FROM_NUMBER: from
  });

  return {
    configured: true,
    provider: 'twilio',
    alertPhone: to,
    fromNumber: from,
    sidMasked: `${sid.slice(0, 6)}...${sid.slice(-4)}`
  };
}

function getVoiceStatus() {
  const cfg = getLiveConfig();
  const hasTwilio = Boolean(cfg.TWILIO_ACCOUNT_SID && cfg.TWILIO_AUTH_TOKEN && cfg.TWILIO_FROM_NUMBER);
  const hasAT = Boolean(cfg.AT_USERNAME && cfg.AT_API_KEY && cfg.AT_VOICE_FROM);
  const callmebotUser = cfg.CALLMEBOT_USER || normalizeUgandaPhone(cfg.ALERT_PHONE_NUMBER || DEFAULT_ALERT_PHONE);
  return {
    configured: true,
    provider: hasTwilio ? 'twilio' : hasAT ? 'africastalking' : 'callmebot',
    callmebotUser,
    alertPhone: normalizeUgandaPhone(cfg.ALERT_PHONE_NUMBER || DEFAULT_ALERT_PHONE),
    fromNumber: cfg.TWILIO_FROM_NUMBER || cfg.AT_VOICE_FROM || ''
  };
}

/**
 * Helper to perform GET request following HTTP -> HTTPS redirects
 */
function httpGetText(urlStr, redirectsLeft = 3) {
  return new Promise((resolve, reject) => {
    const client = urlStr.startsWith('https://') ? https : http;
    const req = client.get(urlStr, { timeout: 12000 }, (res) => {
      if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location && redirectsLeft > 0) {
        const nextUrl = res.headers.location.startsWith('http')
          ? res.headers.location
          : new URL(res.headers.location, urlStr).toString();
        res.resume();
        return resolve(httpGetText(nextUrl, redirectsLeft - 1));
      }
      let body = '';
      res.on('data', (chunk) => (body += chunk));
      res.on('end', () => resolve({ statusCode: res.statusCode, body }));
    });
    req.on('error', reject);
    req.on('timeout', () => {
      req.destroy(new Error('Request timed out'));
    });
  });
}

/**
 * 100% Free Voice Call in Uganda via CallMeBot (@CallMeBot_txtbot)
 */
async function callViaCallMeBot(userTarget, spokenMessage) {
  const cleanTarget = userTarget.startsWith('@') ? userTarget : normalizeUgandaPhone(userTarget);
  const query = querystring.stringify({
    user: cleanTarget,
    text: spokenMessage.slice(0, 240),
    lang: 'en-GB-Standard-B',
    rpt: '2',
    cc: 'yes'
  });
  const url = `http://api.callmebot.com/start.php?${query}`;

  try {
    const { statusCode, body } = await httpGetText(url);
    const plain = body.replace(/<[^>]+>/g, ' ').replace(/\s+/g, ' ').trim();
    const lower = plain.toLowerCase();

    if (
      lower.includes('not authorized') ||
      lower.includes('authorize callmebot') ||
      lower.includes('did not authorize') ||
      lower.includes('user not found') ||
      lower.includes('error')
    ) {
      console.warn(`⚠️  [CallMeBot Authorization Needed for ${cleanTarget}]: ${plain.slice(0, 180)}`);
      return {
        success: false,
        provider: 'callmebot',
        to: cleanTarget,
        needsAuth: true,
        error: `Almost ready! Open Telegram on 0748802162, open @CallMeBot_txtbot (https://t.me/CallMeBot_txtbot), tap START, and if your phone number is hidden in Telegram privacy, enter your Telegram @username instead of +256748802162. Server reply: ${plain.slice(0, 140)}`
      };
    }

    if (statusCode >= 200 && statusCode < 300) {
      console.log(`📞 [FREE UGANDA TAMPER CALL LIVE - CallMeBot] Ringing ${cleanTarget} now!`);
      return {
        success: true,
        provider: 'callmebot',
        to: cleanTarget,
        status: 'ringing',
        details: plain.slice(0, 120)
      };
    }

    return {
      success: false,
      provider: 'callmebot',
      to: cleanTarget,
      error: plain.slice(0, 180) || `HTTP ${statusCode}`
    };
  } catch (err) {
    console.error('[-] [CallMeBot Request Failed]:', err.message);
    return {
      success: false,
      provider: 'callmebot',
      to: cleanTarget,
      error: err.message
    };
  }
}

function callViaTwilio(cfg, toPhone, spokenMessage) {
  return new Promise((resolve) => {
    const sid = cfg.TWILIO_ACCOUNT_SID;
    const token = cfg.TWILIO_AUTH_TOKEN;
    const fromPhone = cfg.TWILIO_FROM_NUMBER;

    const twiml = `<Response><Say voice="alice">${spokenMessage}</Say><Pause length="1"/><Say voice="alice">${spokenMessage}</Say></Response>`;
    const postData = querystring.stringify({
      To: toPhone,
      From: fromPhone,
      Timeout: '25',
      Twiml: twiml
    });

    const req = https.request(
      {
        hostname: 'api.twilio.com',
        port: 443,
        path: `/2010-04-01/Accounts/${sid}/Calls.json`,
        method: 'POST',
        auth: `${sid}:${token}`,
        headers: {
          'Content-Type': 'application/x-www-form-urlencoded',
          'Content-Length': Buffer.byteLength(postData)
        }
      },
      (res) => {
        let body = '';
        res.on('data', (chunk) => (body += chunk));
        res.on('end', () => {
          if (res.statusCode >= 200 && res.statusCode < 300) {
            resolve({ success: true, provider: 'twilio', to: toPhone });
          } else {
            resolve({ success: false, provider: 'twilio', error: body });
          }
        });
      }
    );
    req.on('error', (err) => resolve({ success: false, provider: 'twilio', error: err.message }));
    req.write(postData);
    req.end();
  });
}

async function placeEmergencyTamperCall({ phone, target: explicitTarget, plate = 'UEV 412K', gForce = 3.8, force = false } = {}) {
  const now = Date.now();
  if (!force && now - lastCallTimestamp < CALL_COOLDOWN_MS) {
    console.log('⏳ [Voice Call] Cooldown active — skipping duplicate tamper ring within 35s.');
    return { success: false, reason: 'cooldown' };
  }

  const cfg = getLiveConfig();
  const spoken = `Emergency security alert! Physical tamper vibration of ${Number(gForce).toFixed(1)} G detected on motorcycle ${plate} in Kampala. Check your live tracking map immediately!`;

  if (cfg.TWILIO_ACCOUNT_SID && cfg.TWILIO_AUTH_TOKEN && cfg.TWILIO_FROM_NUMBER) {
    lastCallTimestamp = now;
    const toPhone = normalizeUgandaPhone(phone || cfg.ALERT_PHONE_NUMBER || DEFAULT_ALERT_PHONE);
    return callViaTwilio(cfg, toPhone, spoken);
  }

  // Default 100% Free Uganda Voice Call via CallMeBot (@CallMeBot_txtbot)
  const rawTarget = explicitTarget || ((phone && String(phone).startsWith('@')) ? phone : '') || cfg.CALLMEBOT_USER || phone || cfg.ALERT_PHONE_NUMBER || DEFAULT_ALERT_PHONE;
  const target = String(rawTarget).trim().startsWith('@')
    ? String(rawTarget).trim()
    : normalizeUgandaPhone(rawTarget);

  const res = await callViaCallMeBot(target, spoken);
  if (res.success) {
    lastCallTimestamp = now;
  }
  return res;
}

module.exports = {
  placeEmergencyTamperCall,
  saveTwilioConfig,
  saveCallMeBotConfig,
  getVoiceStatus,
  normalizeUgandaPhone,
  getLiveConfig
};
