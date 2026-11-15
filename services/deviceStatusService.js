/**
 * Real-Time Hardware Device Online / Offline Heartbeat Watchdog Service
 */

const db = require('../config/db');

const DEFAULT_IMEI = '865421045981201';
const DEFAULT_PLATE = 'UEV 412K';
const HEARTBEAT_TIMEOUT_MS = 9500;

let broadcastCallback = null;
let watchdogTimer = null;

const devices = new Map();

function ensureDevice(imei = DEFAULT_IMEI, plate = DEFAULT_PLATE) {
  if (!devices.has(imei)) {
    devices.set(imei, {
      imei,
      plate,
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
  const entry = devices.get(imei);
  if (plate && plate !== DEFAULT_PLATE) {
    entry.plate = plate;
  }
  return entry;
}

function syncMotorcycleStore(entry) {
  try {
    if (db.store && Array.isArray(db.store.motorcycles)) {
      const moto = db.store.motorcycles.find((m) => m.device_imei === entry.imei);
      if (moto) {
        moto.is_online = entry.is_online;
        moto.connection_transport = entry.transport;
        moto.last_seen_at = entry.last_seen_at;
        moto.offline_since = entry.offline_since;
      }
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

function recordHeartbeat(imei = DEFAULT_IMEI, transport = 'USB_SERIAL', plate = DEFAULT_PLATE) {
  const entry = ensureDevice(imei || DEFAULT_IMEI, plate || DEFAULT_PLATE);
  const now = Date.now();
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

  entry.transport = (now - entry.last_serial_ms < 6000) ? 'USB_SERIAL' : transport;
  entry.is_online = true;

  syncMotorcycleStore(entry);

  if (!wasOnline || prevTransport !== entry.transport) {
    console.log(`🟢 [Device Monitor] ${entry.plate} (${entry.imei}) is ONLINE via ${entry.transport}`);
    broadcastDeviceStatus(entry);
  }
}

function handleUsbDisconnected(imei = DEFAULT_IMEI) {
  const entry = ensureDevice(imei || DEFAULT_IMEI);
  entry.usb_connected = false;
  const now = Date.now();

  if (entry.last_mqtt_ms > 0 && (now - entry.last_mqtt_ms) < 7000) {
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

  ensureDevice(DEFAULT_IMEI, DEFAULT_PLATE);
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
  }, 2000);
}

function registerDevice(imei, plate) {
  if (!imei) return null;
  const entry = ensureDevice(imei, plate || DEFAULT_PLATE);
  syncMotorcycleStore(entry);
  return buildStatusPayload(entry);
}

function unregisterDevice(imei) {
  if (!imei || imei === DEFAULT_IMEI) return;
  devices.delete(imei);
}

function getDeviceStatus(imei = DEFAULT_IMEI) {
  const entry = ensureDevice(imei || DEFAULT_IMEI);
  return buildStatusPayload(entry);
}

function getAllDeviceStatuses() {
  ensureDevice(DEFAULT_IMEI, DEFAULT_PLATE);
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

module.exports = {
  startWatchdog,
  recordHeartbeat,
  handleUsbDisconnected,
  registerDevice,
  unregisterDevice,
  getDeviceStatus,
  getAllDeviceStatuses,
  HEARTBEAT_TIMEOUT_MS
};

