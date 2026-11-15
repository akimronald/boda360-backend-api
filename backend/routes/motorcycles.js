/**
 * Motorcycles & Multi-Bike Profile Association Endpoints:
 * - GET    /api/motorcycles                   : Retrieve all registered motorcycles for current user profile
 * - GET    /api/motorcycles/available-devices : Retrieve unbound tracker IMEIs ready to pair
 * - POST   /api/motorcycles                   : Add / associate another motorcycle to user profile
 * - GET    /api/motorcycles/:imei             : Single motorcycle detail
 * - PUT    /api/motorcycles/:imei             : Update motorcycle details (model, SIM, PIN, plate)
 * - DELETE /api/motorcycles/:imei             : Remove an added motorcycle from user profile
 */

const express = require('express');
const jwt = require('jsonwebtoken');
const router = express.Router();
const db = require('../config/db');
const { JWT_SECRET } = require('../middleware/auth');
const deviceStatusService = require('../services/deviceStatusService');
const cloudMqttBridge = require('../services/cloudMqttBridge');

/**
 * Resolves the active customer profile from JWT Bearer token, x-user-email header,
 * query/body email, or defaults to the primary customer session.
 */
function resolveCustomerUser(req) {
  let user = null;
  const authHeader = req.headers['authorization'];

  if (authHeader && authHeader.startsWith('Bearer ')) {
    const token = authHeader.split(' ')[1];
    if (token && !token.startsWith('bg_')) {
      try {
        const decoded = jwt.verify(token, JWT_SECRET);
        user = db.store.users.find(
          (u) =>
            u.user_id === decoded.user_id ||
            (decoded.email && u.email.toLowerCase() === String(decoded.email).toLowerCase())
        );
      } catch (_) {
        // Fallback to x-user-email lookup below
      }
    }
  }

  if (!user) {
    const emailHeader = req.headers['x-user-email'] || req.query?.email || req.body?.user_email;
    if (emailHeader) {
      const clean = String(emailHeader).trim().toLowerCase();
      user = db.store.users.find(
        (u) => u.email.toLowerCase() === clean || u.phone_number === String(emailHeader).trim()
      );
    }
  }

  if (!user) {
    user = db.store.users.find((u) => u.role === 'customer') || null;
  }

  return user;
}

function enrichMotorcycle(m) {
  const owner = db.store.users.find((u) => u.user_id === m.user_id);
  const logs = db.store.telemetry_logs
    .filter((t) => t.device_imei === m.device_imei)
    .sort((a, b) => new Date(b.timestamp) - new Date(a.timestamp));

  const latest = logs[0] || {
    latitude: typeof m.initial_lat === 'number' ? m.initial_lat : 0.327500,
    longitude: typeof m.initial_lng === 'number' ? m.initial_lng : 32.602500,
    speed: 0,
    heading: 0,
    battery_voltage: 12.6,
    ignition_status: false,
    gsm_signal: 28,
    timestamp: m.created_at || new Date()
  };

  const unackAlerts = db.store.security_events.filter(
    (e) => e.device_imei === m.device_imei && !e.is_acknowledged
  );

  const activeGeofences = db.store.geofences.filter(
    (g) => g.motorcycle_id === m.motorcycle_id && g.is_active
  );

  const liveStatus = deviceStatusService.getDeviceStatus(m.device_imei);

  return {
    ...m,
    owner_name: owner ? owner.full_name : 'Registered Owner',
    owner_phone: owner ? owner.phone_number : '+256 701 987654',
    owner_email: owner ? owner.email : '',
    latest_telemetry: latest,
    is_online: Boolean(liveStatus && liveStatus.is_online),
    connection_transport: liveStatus ? liveStatus.transport : 'OFFLINE',
    last_seen_at: liveStatus ? liveStatus.last_seen_at : null,
    active_alerts: unackAlerts.length,
    has_critical_alert: unackAlerts.some((a) => a.severity === 'CRITICAL'),
    geofences_count: activeGeofences.length
  };
}

// GET /api/motorcycles/available-devices - Unbound tracker IMEIs ready to pair
router.get('/available-devices', (req, res) => {
  try {
    const boundImeis = new Set(db.store.motorcycles.map((m) => m.device_imei));
    const available = db.store.devices.filter((d) => !boundImeis.has(d.device_imei));

    let nextIdx = db.store.devices.length + 1;
    let suggestedImei = `8654210459812${String(nextIdx).padStart(2, '0')}`;
    while (boundImeis.has(suggestedImei)) {
      nextIdx++;
      suggestedImei = `8654210459812${String(nextIdx).padStart(2, '0')}`;
    }

    return res.json({
      success: true,
      count: available.length,
      suggested_imei: available[0] ? available[0].device_imei : suggestedImei,
      devices: available.map((d) => ({
        device_imei: d.device_imei,
        serial_number: d.serial_number,
        hardware_version: d.hardware_version
      }))
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to retrieve available tracker IMEIs.' });
  }
});

// GET /api/motorcycles - All motorcycles registered to current user profile
router.get('/', async (req, res) => {
  try {
    const user = resolveCustomerUser(req);
    if (!user) {
      return res.json({
        success: true,
        user: null,
        count: 0,
        motorcycles: []
      });
    }

    const isFleet =
      user.role === 'fleet_manager' || user.role === 'admin' || user.role === 'manufacturer';

    let list = db.store.motorcycles;
    if (!isFleet) {
      list = list.filter((m) => m.user_id === user.user_id);
    }

    const enriched = list.map(enrichMotorcycle);

    return res.json({
      success: true,
      user: user
        ? {
            user_id: user.user_id,
            full_name: user.full_name,
            email: user.email,
            phone_number: user.phone_number
          }
        : null,
      count: enriched.length,
      motorcycles: enriched
    });
  } catch (err) {
    console.error('Fetch motorcycles error:', err);
    return res.status(500).json({ success: false, message: 'Failed to retrieve motorcycles list.' });
  }
});

// POST /api/motorcycles - Add another motorcycle to the user's profile
router.post('/', async (req, res) => {
  try {
    const user = resolveCustomerUser(req);
    if (!user) {
      return res.status(401).json({ success: false, message: 'User profile could not be resolved.' });
    }

    const {
      plate_number,
      device_imei,
      model,
      bike_color,
      sim_number,
      chassis_number,
      pin_code,
      latitude,
      longitude,
      location_name
    } = req.body;

    if (!plate_number || !device_imei) {
      return res.status(400).json({
        success: false,
        message: 'Both License Plate Number and Tracker Device IMEI are required to add a motorcycle.'
      });
    }

    const cleanImei = String(device_imei).replace(/\s+/g, '').trim();
    const cleanPlate = String(plate_number).trim().toUpperCase();

    if (!/^[0-9A-Za-z_-]{8,22}$/.test(cleanImei)) {
      return res.status(400).json({
        success: false,
        message: 'Tracker IMEI must be 8 to 20 digits/characters (standard 15-digit GSM IMEI recommended).'
      });
    }

    if (cleanPlate.length < 3) {
      return res.status(400).json({
        success: false,
        message: 'Please enter a valid motorcycle license plate number (e.g. UFD 882M).'
      });
    }

    // Check duplicate IMEI
    const existingByImei = db.store.motorcycles.find((m) => m.device_imei === cleanImei);
    if (existingByImei) {
      return res.status(409).json({
        success: false,
        message: `Tracker IMEI ${cleanImei} is already linked to motorcycle ${existingByImei.plate_number}.`
      });
    }

    // Check duplicate Plate Number
    const existingByPlate = db.store.motorcycles.find(
      (m) => m.plate_number.toUpperCase() === cleanPlate
    );
    if (existingByPlate) {
      return res.status(409).json({
        success: false,
        message: `Motorcycle Plate ${cleanPlate} is already registered in the system.`
      });
    }

    const nextId = db.store.motorcycles.reduce((max, m) => Math.max(max, Number(m.motorcycle_id) || 0), 0) + 1;
    const parsedLat = latitude !== undefined && latitude !== '' && !isNaN(parseFloat(latitude))
      ? parseFloat(latitude)
      : 0.327500 + (Math.random() - 0.5) * 0.016;
    const parsedLng = longitude !== undefined && longitude !== '' && !isNaN(parseFloat(longitude))
      ? parseFloat(longitude)
      : 32.602500 + (Math.random() - 0.5) * 0.016;

    const cleanPin = pin_code && /^\d{4,6}$/.test(String(pin_code).trim())
      ? String(pin_code).trim()
      : '1234';

    const cleanModel = model && String(model).trim() ? String(model).trim() : 'Bajaj Boxer BM150';
    const cleanColor = bike_color ? String(bike_color).trim() : '';
    const displayModel = cleanColor && !cleanModel.toLowerCase().includes(cleanColor.toLowerCase())
      ? `${cleanModel} (${cleanColor})`
      : cleanModel;

    const newMotorcycle = {
      motorcycle_id: nextId,
      user_id: user.user_id,
      vendor_id: 1,
      plate_number: cleanPlate,
      device_imei: cleanImei,
      status: 'active',
      model: displayModel,
      bike_color: cleanColor,
      sim_number: sim_number && String(sim_number).trim()
        ? String(sim_number).trim()
        : (user.phone_number || '+256 770 000000 (MTN Uganda)'),
      chassis_number: chassis_number && String(chassis_number).trim()
        ? String(chassis_number).trim().toUpperCase()
        : `MD2A12AY${nextId}PW0099`,
      location_name: location_name && String(location_name).trim()
        ? String(location_name).trim()
        : 'Kampala Registered Base',
      initial_lat: parsedLat,
      initial_lng: parsedLng,
      pin_code: cleanPin,
      subscription_status: 'active',
      subscription_expiry: new Date(Date.now() + 365 * 24 * 3600 * 1000),
      installed_at: new Date(),
      created_at: new Date(),
      updated_at: new Date()
    };

    db.store.motorcycles.push(newMotorcycle);

    // Update or auto-provision hardware unit in devices table
    let device = db.store.devices.find((d) => d.device_imei === cleanImei);
    if (!device) {
      device = {
        device_id: db.store.devices.length + 1,
        device_imei: cleanImei,
        serial_number: `SN-UG26-${String(db.store.devices.length + 1).padStart(3, '0')}`,
        batch_id: 'BATCH-2026-USER',
        hardware_version: 'LILYGO-TSIM7000G-ESP32',
        firmware_version: 'v2.0-EAT',
        vendor_id: 1,
        status: 'activated_to_customer',
        qa_passed: true,
        manufactured_at: new Date(),
        allocated_at: new Date(),
        activated_at: new Date()
      };
      db.store.devices.push(device);
    } else {
      device.status = 'activated_to_customer';
      device.activated_at = new Date();
    }

    // Seed initial telemetry point for map marker placement
    const initialTelemetry = {
      log_id: db.store.telemetry_logs.length + 1,
      device_imei: cleanImei,
      latitude: parsedLat,
      longitude: parsedLng,
      speed: 0,
      heading: 0,
      battery_voltage: 12.6,
      ignition_status: false,
      gsm_signal: 28,
      timestamp: new Date()
    };
    db.store.telemetry_logs.push(initialTelemetry);

    // Register with hardware online/offline watchdog
    deviceStatusService.registerDevice(cleanImei, cleanPlate);

    // Push Owner Phone, Email & Plate down to the new bike's ESP32 NVS storage over MQTT + USB Serial
    if (cloudMqttBridge && typeof cloudMqttBridge.syncOwnerProfileToHardware === 'function') {
      cloudMqttBridge.syncOwnerProfileToHardware({
        imei: cleanImei,
        plate: cleanPlate,
        ownerPhone: user.phone_number,
        ownerEmail: user.email
      });
    }

    // Persist to disk so the user's added bike survives server restarts
    if (typeof db.saveProfileStore === 'function') {
      db.saveProfileStore(null, cleanImei);
    }

    const enrichedMoto = enrichMotorcycle(newMotorcycle);

    // Broadcast live to open WebSocket sessions (phone + computer sync)
    if (req.app && req.app.locals && typeof req.app.locals.broadcastTelemetry === 'function') {
      req.app.locals.broadcastTelemetry({
        type: 'MOTORCYCLE_ADDED',
        user_id: user.user_id,
        user_email: user.email,
        motorcycle: enrichedMoto
      });
    }

    return res.status(201).json({
      success: true,
      message: `Motorcycle ${cleanPlate} (${newMotorcycle.model}) successfully added to ${user.full_name}'s profile!`,
      motorcycle: enrichedMoto
    });
  } catch (err) {
    console.error('Create motorcycle error:', err);
    return res.status(500).json({ success: false, message: 'Failed to add motorcycle to profile.' });
  }
});

// PUT /api/motorcycles/:imei - Update motorcycle details in profile
router.put('/:imei', async (req, res) => {
  try {
    const user = resolveCustomerUser(req);
    const { imei } = req.params;
    const moto = db.store.motorcycles.find((m) => m.device_imei === imei);

    if (!moto) {
      return res.status(404).json({ success: false, message: 'Motorcycle not found.' });
    }

    if (user && user.role !== 'fleet_manager' && user.role !== 'admin' && moto.user_id !== user.user_id) {
      return res.status(403).json({ success: false, message: 'You can only edit motorcycles on your own profile.' });
    }

    const { plate_number, model, sim_number, chassis_number, pin_code } = req.body;
    if (plate_number && String(plate_number).trim()) {
      const nextPlate = String(plate_number).trim().toUpperCase();
      const conflict = db.store.motorcycles.find(
        (m) => m.device_imei !== imei && m.plate_number.toUpperCase() === nextPlate
      );
      if (conflict) {
        return res.status(409).json({ success: false, message: `Plate ${nextPlate} is already used by another bike.` });
      }
      moto.plate_number = nextPlate;
    }
    if (model && String(model).trim()) moto.model = String(model).trim();
    if (sim_number !== undefined) moto.sim_number = String(sim_number).trim();
    if (chassis_number !== undefined) moto.chassis_number = String(chassis_number).trim().toUpperCase();
    if (pin_code && /^\d{4,6}$/.test(String(pin_code).trim())) {
      moto.pin_code = String(pin_code).trim();
    }
    moto.updated_at = new Date();

    if (typeof db.saveProfileStore === 'function') {
      db.saveProfileStore();
    }

    // Sync updated plate / owner phone down to ESP32 NVS
    const motoOwner = db.store.users.find((u) => u.user_id === moto.user_id) || user;
    if (motoOwner && cloudMqttBridge && typeof cloudMqttBridge.syncOwnerProfileToHardware === 'function') {
      cloudMqttBridge.syncOwnerProfileToHardware({
        imei: moto.device_imei,
        plate: moto.plate_number,
        ownerPhone: motoOwner.phone_number,
        ownerEmail: motoOwner.email
      });
    }

    return res.json({
      success: true,
      message: `Updated ${moto.plate_number} settings.`,
      motorcycle: enrichMotorcycle(moto)
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to update motorcycle.' });
  }
});

// DELETE /api/motorcycles/:imei - Remove an added motorcycle from user's profile
router.delete('/:imei', async (req, res) => {
  try {
    const user = resolveCustomerUser(req);
    const { imei } = req.params;
    const idx = db.store.motorcycles.findIndex((m) => m.device_imei === imei);

    if (idx === -1) {
      return res.status(404).json({ success: false, message: 'Motorcycle not found.' });
    }

    const moto = db.store.motorcycles[idx];
    if (user && user.role !== 'fleet_manager' && user.role !== 'admin' && moto.user_id !== user.user_id) {
      return res.status(403).json({ success: false, message: 'You can only remove motorcycles from your own profile.' });
    }

    const removed = db.store.motorcycles.splice(idx, 1)[0];
    const dev = db.store.devices.find((d) => d.device_imei === imei);
    if (dev) {
      dev.status = 'allocated_to_vendor';
      dev.activated_at = null;
    }

    deviceStatusService.unregisterDevice(imei);

    if (typeof db.saveProfileStore === 'function') {
      db.saveProfileStore(imei, null);
    }

    if (req.app && req.app.locals && typeof req.app.locals.broadcastTelemetry === 'function') {
      req.app.locals.broadcastTelemetry({
        type: 'MOTORCYCLE_REMOVED',
        user_id: user ? user.user_id : null,
        user_email: user ? user.email : null,
        imei
      });
    }

    return res.json({
      success: true,
      message: `Motorcycle ${removed.plate_number} removed from your profile.`,
      removed_imei: imei
    });
  } catch (err) {
    console.error('Delete motorcycle error:', err);
    return res.status(500).json({ success: false, message: 'Failed to remove motorcycle.' });
  }
});

// GET /api/motorcycles/:imei - Single motorcycle detail
router.get('/:imei', async (req, res) => {
  try {
    const user = resolveCustomerUser(req);
    const { imei } = req.params;
    const moto = db.store.motorcycles.find((m) => m.device_imei === imei);

    if (!moto) {
      return res.status(404).json({ success: false, message: 'Motorcycle not found.' });
    }

    if (user && (user.role === 'owner' || user.role === 'customer') && moto.user_id !== user.user_id) {
      return res.status(403).json({ success: false, message: 'Access denied to this vehicle.' });
    }

    const events = db.store.security_events
      .filter((e) => e.device_imei === imei)
      .sort((a, b) => new Date(b.triggered_at) - new Date(a.triggered_at));

    const commands = db.store.command_logs
      .filter((c) => c.device_imei === imei)
      .sort((a, b) => new Date(b.issued_at) - new Date(a.issued_at));

    const geofences = db.store.geofences.filter((g) => g.motorcycle_id === moto.motorcycle_id);

    return res.json({
      success: true,
      motorcycle: {
        ...enrichMotorcycle(moto),
        recent_events: events.slice(0, 5),
        recent_commands: commands.slice(0, 5),
        geofences
      }
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to retrieve vehicle details.' });
  }
});

module.exports = router;
