/**
 * Telemetry & Time-Series Endpoints Specification:
 * - GET  /api/motorcycles/:imei/telemetry : Latest telemetry & historical route trail
 * - POST /api/telemetry/ingest           : Incoming payload ingestion from ESP32 / MQTT
 */

const express = require('express');
const router = express.Router();
const db = require('../config/db');
const { verifyToken } = require('../middleware/auth');
const geofenceService = require('../services/geofenceService');

// GET /api/motorcycles/:imei/telemetry
router.get('/:imei/telemetry', verifyToken, async (req, res) => {
  try {
    const { imei } = req.params;
    const limit = parseInt(req.query.limit, 10) || 100;
    const startTime = req.query.start ? new Date(req.query.start) : null;
    const endTime = req.query.end ? new Date(req.query.end) : null;

    const moto = db.store.motorcycles.find(m => m.device_imei === imei);
    if (!moto) {
      return res.status(404).json({ success: false, message: 'Motorcycle not found.' });
    }

    let logs = db.store.telemetry_logs.filter(t => t.device_imei === imei);

    if (startTime) {
      logs = logs.filter(t => new Date(t.timestamp) >= startTime);
    }
    if (endTime) {
      logs = logs.filter(t => new Date(t.timestamp) <= endTime);
    }

    // Sort ascending for chronological historical playback trail
    const chronologicalHistory = [...logs].sort((a, b) => new Date(a.timestamp) - new Date(b.timestamp));

    // Latest is the newest point
    const latest = chronologicalHistory[chronologicalHistory.length - 1] || null;

    return res.json({
      success: true,
      device_imei: imei,
      plate_number: moto.plate_number,
      latest_telemetry: latest,
      count: chronologicalHistory.length,
      history: chronologicalHistory.slice(-limit)
    });
  } catch (err) {
    console.error('Fetch telemetry error:', err);
    return res.status(500).json({ success: false, message: 'Failed to retrieve telemetry data.' });
  }
});

// POST /api/telemetry/ingest (Node.js REST endpoint for ESP32 HTTP fallback or MQTT gateway)
router.post('/ingest', async (req, res) => {
  try {
    const { device_imei, latitude, longitude, speed, heading, battery_voltage, ignition_status, gsm_signal } = req.body;

    if (!device_imei || latitude === undefined || longitude === undefined) {
      return res.status(400).json({
        success: false,
        message: 'Invalid payload: device_imei, latitude, and longitude are required.'
      });
    }

    const moto = db.store.motorcycles.find(m => m.device_imei === device_imei);
    if (!moto) {
      return res.status(404).json({
        success: false,
        message: `Device IMEI ${device_imei} is not registered in the system.`
      });
    }

    const newLog = {
      log_id: db.store.telemetry_logs.length + 1,
      device_imei,
      latitude: parseFloat(latitude),
      longitude: parseFloat(longitude),
      speed: parseFloat(speed || 0),
      heading: parseFloat(heading || 0),
      battery_voltage: parseFloat(battery_voltage || 12.6),
      ignition_status: Boolean(ignition_status),
      gsm_signal: parseInt(gsm_signal || 28, 10),
      timestamp: new Date()
    };

    db.store.telemetry_logs.push(newLog);

    // Check geofence violation
    const breach = geofenceService.checkGeofenceBreach(moto, newLog.latitude, newLog.longitude);
    if (breach) {
      const newEvent = {
        event_id: db.store.security_events.length + 1,
        device_imei,
        event_type: 'GEOFENCE_BREACH',
        severity: 'HIGH',
        latitude: newLog.latitude,
        longitude: newLog.longitude,
        notes: `Breach detected: Exited designated boundary '${breach.name}'. Distance: ${Math.round(breach.distance)}m (Limit: ${breach.radius_meters}m)`,
        is_acknowledged: false,
        triggered_at: new Date()
      };
      db.store.security_events.push(newEvent);
    }

    // Check low battery
    if (newLog.battery_voltage < 11.5) {
      const existingLowBat = db.store.security_events.find(
        e => e.device_imei === device_imei && e.event_type === 'LOW_BATTERY' && !e.is_acknowledged
      );
      if (!existingLowBat) {
        db.store.security_events.push({
          event_id: db.store.security_events.length + 1,
          device_imei,
          event_type: 'LOW_BATTERY',
          severity: 'MEDIUM',
          latitude: newLog.latitude,
          longitude: newLog.longitude,
          notes: `Critical low battery detected: ${newLog.battery_voltage}V. Possible battery tamper or cut.`,
          is_acknowledged: false,
          triggered_at: new Date()
        });
      }
    }

    // Broadcast through WebSocket if server is running
    if (req.app.locals.broadcastTelemetry) {
      req.app.locals.broadcastTelemetry({
        type: 'TELEMETRY_UPDATE',
        imei: device_imei,
        plate: moto.plate_number,
        telemetry: newLog,
        breach: breach ? true : false
      });
    }

    return res.status(201).json({
      success: true,
      message: 'Telemetry logged successfully.',
      log_id: newLog.log_id
    });
  } catch (err) {
    console.error('Ingest telemetry error:', err);
    return res.status(500).json({ success: false, message: 'Internal server error ingesting telemetry.' });
  }
});

module.exports = router;
