/**
 * Over-The-Air (OTA) Internet Firmware Manager & Streaming Server
 * Supports Dual Microcontroller Fleet:
 *   1. "lilygo_tsim7000g" -> ESP32 LILYGO TTGO T-SIM7000G (SIM7000G Modem + GNSS)
 *   2. "esp32_wroom"      -> ESP32-WROOM-32 + SIM800L (Voice Call/SMS/GPRS) + u-blox NEO-6M GPS
 *
 * Endpoints:
 *   - GET  /api/ota/manifest         : Active firmware builds for both board types
 *   - POST /api/ota/upload           : Upload .bin firmware (raw octet-stream or JSON base64/URL)
 *   - GET  /api/ota/check            : ESP32 boot/periodic version check (?board=...&version=...&imei=...)
 *   - GET  /api/ota/download/:board  : Streams .bin with Content-Length, x-MD5 & HTTP Range chunk support
 *   - POST /api/ota/trigger          : Dispatches OTA_UPDATE over Internet MQTT (HiveMQ) + USB Serial
 */

const express = require('express');
const router = express.Router();
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const os = require('os');
const db = require('../config/db');
const cloudMqttBridge = require('../services/cloudMqttBridge');
const serialGateway = require('../services/serialGatewayService');

const OTA_DIR = path.join(__dirname, '..', 'ota_builds');
const MANIFEST_FILE = path.join(OTA_DIR, 'manifest.json');

const SUPPORTED_BOARDS = {
  lilygo_tsim7000g: {
    board_type: 'lilygo_tsim7000g',
    label: 'ESP32 LILYGO T-SIM7000G (SIM7000G + GNSS)',
    filename: 'boda-lilygo_tsim7000g.bin'
  },
  esp32_wroom: {
    board_type: 'esp32_wroom',
    label: 'ESP32-WROOM-32 (SIM800L Voice + NEO-6M GPS)',
    filename: 'boda-esp32_wroom.bin'
  }
};

function ensureOtaDir() {
  if (!fs.existsSync(OTA_DIR)) {
    fs.mkdirSync(OTA_DIR, { recursive: true });
  }
}

function readManifest() {
  ensureOtaDir();
  try {
    if (fs.existsSync(MANIFEST_FILE)) {
      return JSON.parse(fs.readFileSync(MANIFEST_FILE, 'utf8'));
    }
  } catch (err) {
    console.warn('⚠️ [OTA] Could not read manifest.json:', err.message);
  }
  return {
    updated_at: null,
    boards: {
      lilygo_tsim7000g: null,
      esp32_wroom: null
    },
    history: []
  };
}

function writeManifest(manifest) {
  ensureOtaDir();
  manifest.updated_at = new Date().toISOString();
  fs.writeFileSync(MANIFEST_FILE, JSON.stringify(manifest, null, 2), 'utf8');
}

function detectLanOrigin(req) {
  const port = process.env.PORT || 3000;
  const hostHeader = req && req.get && req.get('host');
  if (hostHeader && !hostHeader.startsWith('localhost') && !hostHeader.startsWith('127.0.0.1')) {
    const proto = (req.headers['x-forwarded-proto'] || req.protocol || 'http').split(',')[0].trim();
    return `${proto}://${hostHeader}`;
  }
  const nets = os.networkInterfaces();
  for (const iface of Object.values(nets)) {
    for (const addr of (iface || [])) {
      if (addr.family === 'IPv4' && !addr.internal) {
        return `http://${addr.address}:${port}`;
      }
    }
  }
  return `http://localhost:${port}`;
}

function normalizeBoardType(raw) {
  const s = String(raw || '').trim().toLowerCase();
  if (s.includes('lilygo') || s.includes('7000')) return 'lilygo_tsim7000g';
  if (s.includes('wroom') || s.includes('vroom') || s.includes('800')) return 'esp32_wroom';
  return SUPPORTED_BOARDS[s] ? s : null;
}

function compareSemver(a, b) {
  if (!a || !b) return a !== b ? 1 : 0;
  const pa = String(a).replace(/^v/i, '').split('.').map(n => parseInt(n, 10) || 0);
  const pb = String(b).replace(/^v/i, '').split('.').map(n => parseInt(n, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const da = pa[i] || 0;
    const dbv = pb[i] || 0;
    if (da > dbv) return 1;
    if (da < dbv) return -1;
  }
  return 0;
}

// GET /api/ota/manifest
router.get('/manifest', (req, res) => {
  const manifest = readManifest();
  const baseOrigin = detectLanOrigin(req);
  return res.json({
    success: true,
    server_origin: baseOrigin,
    supported_boards: SUPPORTED_BOARDS,
    manifest
  });
});

// POST /api/ota/upload
// Supports BOTH raw binary body (application/octet-stream) AND JSON { board_type, version, base64_bin, external_url, notes }
router.post(
  '/upload',
  express.raw({ type: 'application/octet-stream', limit: '4mb' }),
  (req, res) => {
    try {
      ensureOtaDir();
      const isBinaryBody = Buffer.isBuffer(req.body) && req.body.length > 0;

      const rawBoard = isBinaryBody
        ? req.headers['x-board-type']
        : (req.body && req.body.board_type);
      const boardKey = normalizeBoardType(rawBoard);

      if (!boardKey) {
        return res.status(400).json({
          success: false,
          message: 'Invalid board_type. Choose "lilygo_tsim7000g" or "esp32_wroom".'
        });
      }

      const rawVersion = isBinaryBody
        ? req.headers['x-fw-version']
        : (req.body && req.body.version);
      const version = String(rawVersion || '').trim().replace(/^v/i, '');
      if (!version) {
        return res.status(400).json({
          success: false,
          message: 'Firmware version is required (e.g. "2.1.0").'
        });
      }

      const notes = String(
        (isBinaryBody ? decodeURIComponent(req.headers['x-release-notes'] || '') : (req.body && req.body.notes)) || ''
      ).trim();

      const customPublicUrl = String(
        (isBinaryBody ? req.headers['x-public-url'] : (req.body && req.body.external_url)) || ''
      ).trim();

      let binBuffer = null;
      if (isBinaryBody) {
        binBuffer = req.body;
      } else if (req.body && req.body.base64_bin) {
        const cleanB64 = String(req.body.base64_bin).replace(/^data:[^;]+;base64,/, '');
        binBuffer = Buffer.from(cleanB64, 'base64');
      }

      if ((!binBuffer || binBuffer.length < 256) && !customPublicUrl) {
        return res.status(400).json({
          success: false,
          message: 'Please attach a valid ESP32 compiled .bin file (Sketch -> Export Compiled Binary) or provide a public .bin URL.'
        });
      }

      const boardMeta = SUPPORTED_BOARDS[boardKey];
      let sizeBytes = 0;
      let md5Hash = String((req.body && req.body.md5) || '').trim().toLowerCase();
      let sha256Hash = '';
      let storedLocally = false;

      if (binBuffer && binBuffer.length >= 256) {
        // ESP32 magic byte is 0xE9 at offset 0 of application .bin
        const targetPath = path.join(OTA_DIR, boardMeta.filename);
        fs.writeFileSync(targetPath, binBuffer);
        sizeBytes = binBuffer.length;
        md5Hash = crypto.createHash('md5').update(binBuffer).digest('hex');
        sha256Hash = crypto.createHash('sha256').update(binBuffer).digest('hex');
        storedLocally = true;
      }

      const baseOrigin = detectLanOrigin(req);
      const downloadUrl = customPublicUrl || `${baseOrigin}/api/ota/download/${boardKey}`;

      const releaseRecord = {
        board_type: boardKey,
        board_label: boardMeta.label,
        version,
        size_bytes: sizeBytes,
        md5: md5Hash,
        sha256: sha256Hash,
        stored_locally: storedLocally,
        download_url: downloadUrl,
        external_url: customPublicUrl || null,
        notes: notes || `OTA build v${version} for ${boardMeta.label}`,
        uploaded_at: new Date().toISOString()
      };

      const manifest = readManifest();
      manifest.boards[boardKey] = releaseRecord;
      manifest.history = [releaseRecord, ...(manifest.history || [])].slice(0, 20);
      writeManifest(manifest);

      console.log(`🚀 [OTA Server] Stored ${boardKey} firmware v${version} (${Math.round(sizeBytes / 1024)} KB, MD5: ${md5Hash || 'external'})`);

      return res.status(201).json({
        success: true,
        message: `Firmware v${version} ready for ${boardMeta.label}!`,
        release: releaseRecord,
        manifest
      });
    } catch (err) {
      console.error('OTA upload error:', err);
      return res.status(500).json({ success: false, message: 'Failed to store OTA binary: ' + err.message });
    }
  }
);

// GET /api/ota/check?board=esp32_wroom&version=2.0.0&imei=8654...
router.get('/check', (req, res) => {
  const boardKey = normalizeBoardType(req.query.board);
  const currentVer = String(req.query.version || '0.0.0').trim().replace(/^v/i, '');
  if (!boardKey) {
    return res.status(400).json({ update_available: false, error: 'Unknown board parameter' });
  }

  const manifest = readManifest();
  const latest = manifest.boards && manifest.boards[boardKey];
  if (!latest) {
    return res.json({ update_available: false, board: boardKey, current_version: currentVer });
  }

  const isNewer = compareSemver(latest.version, currentVer) > 0;
  const baseOrigin = detectLanOrigin(req);
  const resolvedUrl = latest.external_url || `${baseOrigin}/api/ota/download/${boardKey}`;

  return res.json({
    update_available: isNewer,
    board: boardKey,
    current_version: currentVer,
    version: latest.version,
    size: latest.size_bytes,
    md5: latest.md5,
    url: resolvedUrl,
    notes: latest.notes
  });
});

// GET /api/ota/download/:board
// Supports full stream (Wi-Fi HTTPUpdate) AND HTTP Range chunks (SIM800L / SIM7000G cellular streaming!)
router.get('/download/:board', (req, res) => {
  const boardKey = normalizeBoardType(req.params.board);
  if (!boardKey) {
    return res.status(404).send('Unknown board type');
  }

  const boardMeta = SUPPORTED_BOARDS[boardKey];
  const filePath = path.join(OTA_DIR, boardMeta.filename);
  if (!fs.existsSync(filePath)) {
    return res.status(404).send(`No local .bin uploaded yet for ${boardKey}`);
  }

  const stat = fs.statSync(filePath);
  const totalSize = stat.size;
  const manifest = readManifest();
  const rel = manifest.boards && manifest.boards[boardKey];
  const md5 = rel && rel.md5 ? rel.md5 : '';

  res.setHeader('Accept-Ranges', 'bytes');
  res.setHeader('Content-Type', 'application/octet-stream');
  res.setHeader('Content-Disposition', `attachment; filename="${boardMeta.filename}"`);
  if (md5) {
    res.setHeader('x-MD5', md5);
  }
  if (rel && rel.version) {
    res.setHeader('x-Firmware-Version', rel.version);
  }

  const rangeHeader = req.headers.range;
  if (rangeHeader) {
    const match = /bytes=(\d+)-(\d*)/.exec(rangeHeader);
    if (match) {
      const start = parseInt(match[1], 10);
      const end = match[2] ? Math.min(parseInt(match[2], 10), totalSize - 1) : totalSize - 1;
      if (start >= totalSize || start > end) {
        res.status(416).setHeader('Content-Range', `bytes */${totalSize}`);
        return res.end();
      }
      const chunkLen = end - start + 1;
      res.status(206);
      res.setHeader('Content-Range', `bytes ${start}-${end}/${totalSize}`);
      res.setHeader('Content-Length', chunkLen);
      return fs.createReadStream(filePath, { start, end }).pipe(res);
    }
  }

  res.status(200);
  res.setHeader('Content-Length', totalSize);
  return fs.createReadStream(filePath).pipe(res);
});

// POST /api/ota/trigger
// Dispatches OTA_UPDATE command over Internet MQTT (broker.hivemq.com) + USB Serial Gateway
router.post('/trigger', (req, res) => {
  try {
    const { board_type, target_imei, override_url, force } = req.body || {};
    const boardKey = normalizeBoardType(board_type);
    if (!boardKey) {
      return res.status(400).json({
        success: false,
        message: 'Specify board_type: "lilygo_tsim7000g" or "esp32_wroom".'
      });
    }

    const manifest = readManifest();
    const rel = manifest.boards && manifest.boards[boardKey];
    if (!rel && !override_url) {
      return res.status(404).json({
        success: false,
        message: `No firmware uploaded yet for ${SUPPORTED_BOARDS[boardKey].label}. Upload a .bin file first.`
      });
    }

    const baseOrigin = detectLanOrigin(req);
    const downloadUrl = (override_url && String(override_url).trim()) ||
      (rel && rel.external_url) ||
      `${baseOrigin}/api/ota/download/${boardKey}`;

    const otaCommandPayload = {
      cmd: 'OTA_UPDATE',
      board: boardKey,
      version: rel ? rel.version : 'latest',
      size: rel ? rel.size_bytes : 0,
      md5: rel ? rel.md5 : '',
      force: Boolean(force),
      url: downloadUrl,
      issued_at: Date.now()
    };

    const cleanTarget = target_imei ? String(target_imei).trim() : 'ALL';
    const mqttTopic = (!cleanTarget || cleanTarget.toUpperCase() === 'ALL')
      ? 'boda/ota/broadcast'
      : `boda/${cleanTarget}/command`;

    const sentCloudMqtt = cloudMqttBridge.publishCloudMqtt(mqttTopic, otaCommandPayload);

    // Also forward compact serial trigger if a board is plugged into USB COM7 during bench testing
    if (serialGateway && typeof serialGateway.sendCommand === 'function') {
      serialGateway.sendCommand(JSON.stringify(otaCommandPayload));
    }

    if (req.app && req.app.locals && typeof req.app.locals.broadcastTelemetry === 'function') {
      req.app.locals.broadcastTelemetry({
        type: 'OTA_STATUS_UPDATE',
        event: 'OTA_DISPATCHED',
        board: boardKey,
        target_imei: cleanTarget,
        version: otaCommandPayload.version,
        url: downloadUrl,
        mqtt_published: sentCloudMqtt,
        timestamp: new Date().toISOString()
      });
    }

    // Update target device status metadata in memory if matching
    if (db.store && Array.isArray(db.store.devices) && cleanTarget !== 'ALL') {
      const dev = db.store.devices.find(d => d.device_imei === cleanTarget);
      if (dev) {
        dev.pending_ota_version = otaCommandPayload.version;
        dev.board_type = boardKey;
      }
    }

    return res.json({
      success: true,
      message: `OTA v${otaCommandPayload.version} dispatched to ${cleanTarget === 'ALL' ? `all ${SUPPORTED_BOARDS[boardKey].label} nodes` : `IMEI ${cleanTarget}`}!`,
      mqtt_topic: mqttTopic,
      mqtt_published: sentCloudMqtt,
      command: otaCommandPayload
    });
  } catch (err) {
    console.error('OTA trigger error:', err);
    return res.status(500).json({ success: false, message: 'Failed to dispatch OTA command.' });
  }
});

module.exports = router;

