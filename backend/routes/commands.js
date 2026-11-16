/**
 * Bi-Directional Remote Immobilization Commands Specification:
 * - POST /api/command/immobilize : Publish engine cut command via MQTT + USB Serial
 * - POST /api/command/restore    : Publish ignition restore command via MQTT + USB Serial
 * - GET  /api/command/logs       : Retrieve command audit trail
 */

const express = require('express');
const router = express.Router();
const db = require('../config/db');
const mqttService = require('../services/mqttService');

function optionalAuth(req, res, next) {
  const email = req.headers['x-user-email'];
  const matched = email
    ? db.store.users.find((u) => u.email.toLowerCase() === String(email).toLowerCase())
    : null;
  req.user = matched || { user_id: 1, role: 'customer', full_name: 'Fleet Operator' };
  return next();
}

// POST /api/command/immobilize
router.post('/immobilize', optionalAuth, async (req, res) => {
  try {
    const { device_imei, pin_code } = req.body;

    if (!device_imei) {
      return res.status(400).json({
        success: false,
        message: 'device_imei is required to execute engine cut-off.'
      });
    }

    const result = await mqttService.dispatchCommand(
      device_imei,
      'ENGINE_CUT',
      req.user ? req.user.user_id : 1,
      pin_code
    );

    return res.json({
      success: true,
      message: 'Immobilization command executed. Engine cut relay activated on hardware node.',
      data: result
    });
  } catch (err) {
    console.error('Immobilize command error:', err);
    return res.status(400).json({
      success: false,
      message: err.message || 'Failed to dispatch engine cut-off command.'
    });
  }
});

// POST /api/command/restore
router.post('/restore', optionalAuth, async (req, res) => {
  try {
    const { device_imei, pin_code } = req.body;

    if (!device_imei) {
      return res.status(400).json({
        success: false,
        message: 'device_imei is required to restore motorcycle ignition.'
      });
    }

    const result = await mqttService.dispatchCommand(
      device_imei,
      'ENGINE_RESTORE',
      req.user ? req.user.user_id : 1,
      pin_code
    );

    return res.json({
      success: true,
      message: 'Ignition restoration command executed. Normal operation restored.',
      data: result
    });
  } catch (err) {
    console.error('Restore command error:', err);
    return res.status(400).json({
      success: false,
      message: err.message || 'Failed to dispatch ignition restore command.'
    });
  }
});

// GET /api/command/logs - Retrieve full audit logs
router.get('/logs', optionalAuth, async (req, res) => {
  try {
    const { imei } = req.query;
    let logs = [...db.store.command_logs];

    if (imei) {
      logs = logs.filter((l) => l.device_imei === imei);
    }

    const enriched = logs.map((l) => {
      const moto = db.store.motorcycles.find((m) => m.device_imei === l.device_imei);
      return {
        ...l,
        plate_number: moto ? moto.plate_number : 'UEV 412K'
      };
    });

    return res.json({
      success: true,
      count: enriched.length,
      command_logs: enriched
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to retrieve command logs.' });
  }
});

module.exports = router;
