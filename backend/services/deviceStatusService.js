/**
 * Real-Time Hardware Device Online / Offline Heartbeat Watchdog Service
 *
 * Tracks live connectivity of physical ESP32 + SIM7000G / SIM800L tracker nodes across:
 *   1. USB Serial Bridge (`COM7` — 3s telemetry cadence)
 *   2. Wireless MTN/Airtel Cellular MQTT (`broker.hivemq.com` — 3s telemetry / tamper packets)
 */

const db = require('../config/db');

const DEFAULT_IMEI = '869951038535641';
const DEFAULT_PLATE = 'UEV 412K';
const HEARTBEAT_TIMEOUT_MS = 25000; // 25s window prevents false offline flicker during cellular/sleep intervals

let broadcastCallback = null;
let watchdogTimer = null;
let lastActiveHardwareImei = DEFAULT_IMEI;

const devices = new Map();

function ensureDevice(imei = DEFAULT_IMEI, plate = DEFAULT_PLATE) {
  const cleanImei = String(imei || DEFAULT_IMEI).trim();
  if (!devices.has(cleanImei)) {
    devices.set(cleanImei, {
      imei: cleanImei,
      plate: plate || DEFAULT_PLATE,
      is_online: false,
      usb_connected: false,
      transport: 'OFFLINE',
      last_seen_ms: 0,
      last_serial_ms: 0,
      last_mqtt_ms: 0,
      last_seen_at: null,
      offline_since: new Date().toISOString(),
      ever_connected: false
    });
  }
  const entry = devices.get(cleanImei);
  if (plate && plate !== DEFAULT_PLATE && plate !== 'UNBOUND') {
    entry.plate = plate;
  }
  return entry;
}

function syncMotorcycleStore(entry) {
  try {
    if (db.store && Array.isArray(db.store.motorcycles)) {
      db.store.motorcycles.forEach((moto) => {
        if (moto.device_imei === entry.imei || db.store.motorcycles.length === 1) {
          moto.is_online = entry.is_online;
          moto.connection_transport = entry.transport;
          moto.last_seen_at = entry.last_seen_at;
          moto.offline_since = entry.offline_since;
        }
      });
    }
  } catch (_) {}
}

function buildStatusPayload(entry) {
  return {
    type: 'DEVICE_STATUS',
    imei: entry.imei,
    plate: entry.plate,
    is_online: entry.is_online,
    transport: entry.transport,
    last_seen_at: entry.last_seen_at,
    offline_since: entry.offline_since,
    ever_connected: entry.ever_connected,
    timestamp: new Date().toISOString()
  };
}

function broadcastDeviceStatus(entry) {
  syncMotorcycleStore(entry);
  if (typeof broadcastCallback === 'function') {
    broadcastCallback(buildStatusPayload(entry));
  }
}

function markOffline(entry, reason = 'HEARTBEAT_TIMEOUT') {
  if (!entry.is_online && entry.transport === 'OFFLINE') {
    return;
  }
  const wasOnline = entry.is_online;
  entry.is_online = false;
  entry.transport = 'OFFLINE';
  entry.offline_since = new Date().toISOString();

  if (wasOnline) {
    console.log(`📴 [Device Monitor] ${entry.plate} (${entry.imei}) went OFFLINE (${reason}). Last seen: ${entry.last_seen_at || 'never'}`);
  }
  broadcastDeviceStatus(entry);
}

function applyHeartbeatToEntry(entry, transport, now) {
  const wasOnline = entry.is_online;
  const prevTransport = entry.transport;

  entry.last_seen_ms = now;
  entry.last_seen_at = new Date(now).toISOString();
  entry.ever_connected = true;
  entry.offline_since = null;

  if (transport === 'USB_SERIAL') {
    entry.usb_connected = true;
    entry.last_serial_ms = now;
  } else if (transport === 'CELLULAR_MQTT') {
    entry.last_mqtt_ms = now;
  }

  entry.transport = (now - entry.last_serial_ms < 8000) ? 'USB_SERIAL' : transport;
  entry.is_online = true;

  syncMotorcycleStore(entry);

  if (!wasOnline || prevTransport !== entry.transport) {
    console.log(`🟢 [Device Monitor] ${entry.plate} (${entry.imei}) is ONLINE via ${entry.transport}`);
    broadcastDeviceStatus(entry);
  }
}

function recordHeartbeat(imei = DEFAULT_IMEI, transport = 'USB_SERIAL', plate = DEFAULT_PLATE) {
  const cleanImei = String(imei || lastActiveHardwareImei || DEFAULT_IMEI).trim();
  lastActiveHardwareImei = cleanImei;
  const now = Date.now();

  const entry = ensureDevice(cleanImei, plate || DEFAULT_PLATE);
  applyHeartbeatToEntry(entry, transport, now);

  // Also mirror heartbeat to any customer-registered motorcycle if it is linked to this hardware
  try {
    if (db.store && Array.isArray(db.store.motorcycles)) {
      db.store.motorcycles.forEach((m) => {
        if (m && m.device_imei && m.device_imei !== cleanImei && db.store.motorcycles.length === 1) {
          const linkedEntry = ensureDevice(m.device_imei, m.plate_number || DEFAULT_PLATE);
          applyHeartbeatToEntry(linkedEntry, transport, now);
        } else if (m && m.device_imei === cleanImei && m.plate_number) {
          entry.plate = m.plate_number;
        }
      });
    }
  } catch (_) {}
}

function handleUsbDisconnected(imei) {
  const targetImei = imei || lastActiveHardwareImei || DEFAULT_IMEI;
  const entry = ensureDevice(targetImei);
  entry.usb_connected = false;
  const now = Date.now();

  // If wireless Cellular MQTT is actively streaming (< 15s ago), stay online via CELLULAR_MQTT
  if (entry.last_mqtt_ms > 0 && (now - entry.last_mqtt_ms) < 15000) {
    entry.transport = 'CELLULAR_MQTT';
    broadcastDeviceStatus(entry);
    return;
  }

  markOffline(entry, 'USB_DISCONNECTED');
}

function startWatchdog(broadcast) {
  if (broadcast) {
    broadcastCallback = broadcast;
  }

  try {
    if (db.store && Array.isArray(db.store.motorcycles)) {
      db.store.motorcycles.forEach((m) => {
        if (m && m.device_imei) {
          ensureDevice(m.device_imei, m.plate_number || DEFAULT_PLATE);
        }
      });
    }
  } catch (_) {}

  if (watchdogTimer) {
    clearInterval(watchdogTimer);
  }

  watchdogTimer = setInterval(() => {
    const now = Date.now();
    for (const entry of devices.values()) {
      if (entry.is_online && (now - entry.last_seen_ms > HEARTBEAT_TIMEOUT_MS)) {
        markOffline(entry, 'NO_TELEMETRY_HEARTBEAT');
      }
    }
  }, 2500);
}

function registerDevice(imei, plate) {
  if (!imei) return null;
  const entry = ensureDevice(imei, plate || DEFAULT_PLATE);
  // If physical hardware is currently online, immediately link its online state to this newly registered bike
  const activeHw = devices.get(lastActiveHardwareImei);
  if (activeHw && activeHw.is_online && (Date.now() - activeHw.last_seen_ms < HEARTBEAT_TIMEOUT_MS)) {
    applyHeartbeatToEntry(entry, activeHw.transport, Date.now());
  }
  syncMotorcycleStore(entry);
  return buildStatusPayload(entry);
}

function unregisterDevice(imei) {
  if (!imei) return;
  devices.delete(imei);
}

function getDeviceStatus(imei) {
  const targetImei = imei || lastActiveHardwareImei || DEFAULT_IMEI;
  if (devices.has(targetImei)) {
    return buildStatusPayload(devices.get(targetImei));
  }
  const activeHw = devices.get(lastActiveHardwareImei);
  if (activeHw && activeHw.is_online) {
    return buildStatusPayload(activeHw);
  }
  return buildStatusPayload(ensureDevice(targetImei));
}

function getAllDeviceStatuses() {
  try {
    if (db.store && Array.isArray(db.store.motorcycles)) {
      db.store.motorcycles.forEach((m) => {
        if (m && m.device_imei) {
          ensureDevice(m.device_imei, m.plate_number || DEFAULT_PLATE);
        }
      });
    }
  } catch (_) {}
  return Array.from(devices.values()).map((entry) => buildStatusPayload(entry));
}

function getLastActiveHardwareImei() {
  return lastActiveHardwareImei;
}

module.exports = {
  startWatchdog,
  recordHeartbeat,
  handleUsbDisconnected,
  registerDevice,
  unregisterDevice,
  getDeviceStatus,
  getAllDeviceStatuses,
  getLastActiveHardwareImei,
  HEARTBEAT_TIMEOUT_MS
};
