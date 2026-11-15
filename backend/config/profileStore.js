/**
 * Persistent JSON Store for Registered Customer Profiles & Added Motorcycles
 * Ensures accounts and additional motorcycles added by registered users
 * survive Node.js server restarts when running in standalone In-Memory mode.
 */

const fs = require('fs');
const path = require('path');

const PERSIST_FILE = path.join(__dirname, 'persisted_profiles.json');

function readPersistedData() {
  try {
    if (!fs.existsSync(PERSIST_FILE)) {
      return { users: [], motorcycles: [], batches: [], devices: [], removed_imeis: [] };
    }
    const raw = fs.readFileSync(PERSIST_FILE, 'utf8');
    const parsed = JSON.parse(raw);
    return {
      users: Array.isArray(parsed.users) ? parsed.users : [],
      motorcycles: Array.isArray(parsed.motorcycles) ? parsed.motorcycles : [],
      batches: Array.isArray(parsed.batches) ? parsed.batches : [],
      devices: Array.isArray(parsed.devices) ? parsed.devices : [],
      removed_imeis: Array.isArray(parsed.removed_imeis) ? parsed.removed_imeis : []
    };
  } catch (err) {
    console.warn('⚠️ [ProfileStore] Could not read persisted_profiles.json:', err.message);
    return { users: [], motorcycles: [], batches: [], devices: [], removed_imeis: [] };
  }
}

function hydrateStore(inMemoryStore) {
  if (!inMemoryStore) return;
  const data = readPersistedData();

  // 1. Hydrate persisted users
  data.users.forEach((u) => {
    const existing = inMemoryStore.users.find(
      (eu) => eu.user_id === u.user_id || (eu.email && u.email && eu.email.toLowerCase() === u.email.toLowerCase())
    );
    if (existing) {
      Object.assign(existing, u);
    } else {
      inMemoryStore.users.push(u);
    }
  });

  // 1b. Hydrate persisted batches & devices
  data.batches.forEach((b) => {
    const existingBatch = inMemoryStore.batches.find((eb) => eb.batch_id === b.batch_id);
    if (existingBatch) {
      Object.assign(existingBatch, b);
    } else {
      inMemoryStore.batches.push(b);
    }
  });

  data.devices.forEach((d) => {
    const existingDev = inMemoryStore.devices.find((ed) => ed.device_imei === d.device_imei);
    if (existingDev) {
      Object.assign(existingDev, d);
    } else {
      inMemoryStore.devices.push(d);
    }
  });

  // 2. Remove any deleted motorcycles
  if (data.removed_imeis.length > 0) {
    inMemoryStore.motorcycles = inMemoryStore.motorcycles.filter(
      (m) => !data.removed_imeis.includes(m.device_imei)
    );
  }

  // 3. Hydrate persisted motorcycles & initial telemetry
  data.motorcycles.forEach((m) => {
    if (data.removed_imeis.includes(m.device_imei)) return;

    const existing = inMemoryStore.motorcycles.find((em) => em.device_imei === m.device_imei);
    if (existing) {
      Object.assign(existing, m);
    } else {
      inMemoryStore.motorcycles.push(m);
    }

    // Mark or add device in devices inventory
    let dev = inMemoryStore.devices.find((d) => d.device_imei === m.device_imei);
    if (!dev) {
      dev = {
        device_id: inMemoryStore.devices.length + 1,
        device_imei: m.device_imei,
        serial_number: `SN-UG26-${String(inMemoryStore.devices.length + 1).padStart(3, '0')}`,
        batch_id: 'BATCH-2026-USER',
        hardware_version: 'LILYGO-TSIM7000G-ESP32',
        firmware_version: 'v2.0-EAT',
        vendor_id: m.vendor_id || 1,
        status: 'activated_to_customer',
        qa_passed: true,
        manufactured_at: m.created_at || new Date(),
        allocated_at: m.created_at || new Date(),
        activated_at: m.created_at || new Date()
      };
      inMemoryStore.devices.push(dev);
    } else {
      dev.status = 'activated_to_customer';
      dev.activated_at = dev.activated_at || m.created_at || new Date();
    }

    // Ensure at least one telemetry point exists for map pin placement
    const hasTel = inMemoryStore.telemetry_logs.some((t) => t.device_imei === m.device_imei);
    if (!hasTel) {
      inMemoryStore.telemetry_logs.push({
        log_id: inMemoryStore.telemetry_logs.length + 1,
        device_imei: m.device_imei,
        latitude: typeof m.initial_lat === 'number' ? m.initial_lat : 0.327500,
        longitude: typeof m.initial_lng === 'number' ? m.initial_lng : 32.602500,
        speed: 0,
        heading: 0,
        battery_voltage: 12.6,
        ignition_status: false,
        gsm_signal: 28,
        timestamp: m.created_at || new Date()
      });
    }
  });
}

function saveStoreSnapshot(inMemoryStore, removedImeiToRecord = null, restoredImei = null) {
  try {
    const prev = readPersistedData();
    let removedSet = new Set(prev.removed_imeis || []);
    if (removedImeiToRecord) removedSet.add(removedImeiToRecord);
    if (restoredImei) removedSet.delete(restoredImei);

    // Persist customer/owner users beyond static seeds or modified users
    const usersToSave = (inMemoryStore.users || []).filter((u) => u.user_id >= 2 || u.role === 'customer');
    const motorcyclesToSave = (inMemoryStore.motorcycles || []).slice();
    const batchesToSave = (inMemoryStore.batches || []).slice();
    const devicesToSave = (inMemoryStore.devices || []).slice();

    const payload = {
      updated_at: new Date().toISOString(),
      users: usersToSave,
      motorcycles: motorcyclesToSave,
      batches: batchesToSave,
      devices: devicesToSave,
      removed_imeis: Array.from(removedSet)
    };

    fs.writeFileSync(PERSIST_FILE, JSON.stringify(payload, null, 2), 'utf8');
  } catch (err) {
    console.warn('⚠️ [ProfileStore] Failed to persist profile store:', err.message);
  }
}

module.exports = {
  hydrateStore,
  saveStoreSnapshot
};

