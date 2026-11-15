/**
 * Security & Tamper Alert Center Endpoints Specification:
 * - GET  /api/motorcycles/:imei/events : Retrieve security and tamper logs
 * - GET  /api/events                  : All alerts across fleet
 * - POST /api/events/:id/ack          : Acknowledge alert
 * - POST /api/events/trigger-tamper   : Simulate/report MEMS vibration trigger
 */

const express = require('express');
const router = express.Router();
const db = require('../config/db');
const { verifyToken } = require('../middleware/auth');
const voiceCallService = require('../services/voiceCallService');

// GET /api/motorcycles/:imei/events
router.get('/motorcycles/:imei/events', verifyToken, async (req, res) => {
  try {
    const { imei } = req.params;
    const events = db.store.security_events
      .filter(e => e.device_imei === imei)
      .sort((a, b) => new Date(b.triggered_at) - new Date(a.triggered_at));

    const moto = db.store.motorcycles.find(m => m.device_imei === imei);

    return res.json({
      success: true,
      device_imei: imei,
      plate_number: moto ? moto.plate_number : '',
      count: events.length,
      events
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to retrieve vehicle events.' });
  }
});

// GET /api/events - All alerts
router.get('/', verifyToken, async (req, res) => {
  try {
    let events = [...db.store.security_events];

    if (req.user.role === 'owner') {
      const userImeis = db.store.motorcycles
        .filter(m => m.user_id === req.user.user_id)
        .map(m => m.device_imei);
      events = events.filter(e => userImeis.includes(e.device_imei));
    }

    events.sort((a, b) => new Date(b.triggered_at) - new Date(a.triggered_at));

    const enriched = events.map(e => {
      const moto = db.store.motorcycles.find(m => m.device_imei === e.device_imei);
      return {
        ...e,
        plate_number: moto ? moto.plate_number : 'UNKNOWN',
        model: moto ? moto.model : ''
      };
    });

    return res.json({
      success: true,
      count: enriched.length,
      events: enriched
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to retrieve security events.' });
  }
});

// POST /api/events/:id/ack - Acknowledge alert
router.post('/:id/ack', verifyToken, async (req, res) => {
  try {
    const eventId = parseInt(req.params.id, 10);
    const event = db.store.security_events.find(e => e.event_id === eventId);

    if (!event) {
      return res.status(404).json({ success: false, message: 'Event not found.' });
    }

    event.is_acknowledged = true;

    // Reset motorcycle status if it was in tamper_alert and has no more unack critical alerts
    const moto = db.store.motorcycles.find(m => m.device_imei === event.device_imei);
    if (moto && moto.status === 'tamper_alert') {
      const remainingCritical = db.store.security_events.some(
        e => e.device_imei === event.device_imei && !e.is_acknowledged && e.severity === 'CRITICAL'
      );
      if (!remainingCritical) {
        moto.status = 'active';
      }
    }

    return res.json({
      success: true,
      message: 'Alert acknowledged successfully.',
      event
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to acknowledge alert.' });
  }
});

// GET /api/events/customer-feed - Customer Dashboard Danger Alert Feed (supports token or direct session)
router.get('/customer-feed', async (req, res) => {
  try {
    let events = [...db.store.security_events];
    events.sort((a, b) => new Date(b.triggered_at) - new Date(a.triggered_at));

    const enriched = events.slice(0, 50).map(e => {
      const moto = db.store.motorcycles.find(m => m.device_imei === e.device_imei);
      return {
        ...e,
        plate_number: moto ? moto.plate_number : 'UEV 412K',
        model: moto ? moto.model : 'Bajaj Boxer BM150 (ESP32 Node)'
      };
    });

    const unreadCount = enriched.filter(e => !e.is_acknowledged).length;

    return res.json({
      success: true,
      count: enriched.length,
      unread_count: unreadCount,
      events: enriched
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to retrieve customer danger alerts.' });
  }
});

// POST /api/events/customer-ack-all - Mark all danger alerts as checked on Customer Dashboard
router.post('/customer-ack-all', async (req, res) => {
  try {
    db.store.security_events.forEach(e => {
      e.is_acknowledged = true;
    });

    db.store.motorcycles.forEach(m => {
      if (m.status === 'tamper_alert') {
        m.status = 'active';
      }
    });

    if (req.app.locals.broadcastTelemetry) {
      req.app.locals.broadcastTelemetry({
        type: 'ALERT_ACKNOWLEDGED_ALL',
        timestamp: new Date()
      });
    }

    return res.json({
      success: true,
      message: 'All danger alerts marked as checked.'
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to acknowledge all alerts.' });
  }
});

// POST /api/events/customer-ack/:id - Mark a single danger alert as checked on Customer Dashboard
router.post('/customer-ack/:id', async (req, res) => {
  try {
    const eventId = parseInt(req.params.id, 10);
    const event = db.store.security_events.find(e => e.event_id === eventId);

    if (!event) {
      return res.status(404).json({ success: false, message: 'Danger alert not found.' });
    }

    event.is_acknowledged = true;

    const moto = db.store.motorcycles.find(m => m.device_imei === event.device_imei);
    if (moto && moto.status === 'tamper_alert') {
      const remainingCritical = db.store.security_events.some(
        e => e.device_imei === event.device_imei && !e.is_acknowledged && e.severity === 'CRITICAL'
      );
      if (!remainingCritical) {
        moto.status = 'active';
      }
    }

    if (req.app.locals.broadcastTelemetry) {
      req.app.locals.broadcastTelemetry({
        type: 'ALERT_ACKNOWLEDGED',
        event_id: eventId
      });
    }

    return res.json({
      success: true,
      message: 'Danger alert marked as checked.',
      event
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to acknowledge alert.' });
  }
});

// POST /api/events/trigger-tamper - Simulate/report physical vibration, battery cut, or perimeter alert
router.post('/trigger-tamper', async (req, res) => {
  try {
    const { device_imei, g_force, custom_note, event_type, severity } = req.body;

    if (!device_imei) {
      return res.status(400).json({ success: false, message: 'device_imei is required.' });
    }

    const moto = db.store.motorcycles.find(m => m.device_imei === device_imei) || db.store.motorcycles[0];
    if (!moto) {
      return res.status(404).json({ success: false, message: 'Motorcycle not found.' });
    }

    const latestTel = db.store.telemetry_logs
      .filter(t => t.device_imei === device_imei)
      .sort((a, b) => new Date(b.timestamp) - new Date(a.timestamp))[0];

    const currentLat = req.body.latitude !== undefined ? parseFloat(req.body.latitude) : (latestTel ? latestTel.latitude : 0.327500);
    const currentLng = req.body.longitude !== undefined ? parseFloat(req.body.longitude) : (latestTel ? latestTel.longitude : 32.602500);

    const resolvedType = event_type || 'TAMPER_DETECTED';
    const resolvedSeverity = severity || (resolvedType === 'GEOFENCE_BREACH' ? 'HIGH' : 'CRITICAL');

    if (resolvedType !== 'GEOFENCE_BREACH') {
      moto.status = 'tamper_alert';
    }

    const defaultNote = resolvedType === 'BATTERY_DISCONNECTED'
      ? 'CRITICAL POWER CUT: 12V Motorcycle Battery Removed / Wire Cut! Node running on internal 18650 backup battery — Emergency SMS & Voice Call sent to owner.'
      : resolvedType === 'GEOFENCE_BREACH'
      ? 'PERIMETER BREACH: Motorcycle crossed outside authorized safe perimeter zone — Live GPS SMS sent to owner.'
      : `TAMPER / SHOCK ALERT: Piezo sensor detected physical shock exceeding ${(g_force || 3.8).toFixed(1)}G while parked — Emergency SMS & Voice Call sent to owner.`;

    const newEvent = {
      event_id: db.store.security_events.length + 1,
      device_imei,
      event_type: resolvedType,
      severity: resolvedSeverity,
      latitude: currentLat,
      longitude: currentLng,
      notes: custom_note || defaultNote,
      is_acknowledged: false,
      triggered_at: new Date()
    };

    db.store.security_events.unshift(newEvent);

    if (req.app.locals.broadcastTelemetry) {
      req.app.locals.broadcastTelemetry({
        type: 'SECURITY_ALERT',
        imei: device_imei,
        plate: moto.plate_number,
        event: newEvent
      });
    }

    // Trigger non-blocking outbound emergency phone voice call (CallMeBot / Twilio) if configured
    if (resolvedType !== 'GEOFENCE_BREACH') {
      try {
        const cfg = voiceCallService.getLiveConfig();
        if (cfg.TWILIO_ACCOUNT_SID || cfg.CALLMEBOT_USER || cfg.ALERT_PHONE_NUMBER) {
          voiceCallService.placeEmergencyTamperCall({
            plate: moto.plate_number || 'UEV 412K',
            gForce: parseFloat(g_force || 4.2),
            eventType: resolvedType,
            backupVoltage: 3.9,
            force: resolvedType === 'BATTERY_DISCONNECTED'
          }).catch(() => {});
        }
      } catch (_) {}
    }

    return res.status(201).json({
      success: true,
      message: 'Danger alert recorded and broadcast.',
      event: newEvent
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to trigger danger alert.' });
  }
});

module.exports = router;
