/**
 * Geofence Perimeter Management Routes
 * Author: Akim Ronald David (ISBAT University)
 */

const express = require('express');
const router = express.Router();
const db = require('../config/db');
const { verifyToken } = require('../middleware/auth');
const serialGateway = require('../services/serialGatewayService');

// GET /api/geofences
router.get('/', verifyToken, async (req, res) => {
  try {
    const list = db.store.geofences.map(g => {
      const moto = db.store.motorcycles.find(m => m.motorcycle_id === g.motorcycle_id);
      return {
        ...g,
        plate_number: moto ? moto.plate_number : 'All Fleet',
        device_imei: moto ? moto.device_imei : null
      };
    });

    return res.json({ success: true, count: list.length, geofences: list });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to retrieve geofences.' });
  }
});

// POST /api/geofences - Create new geofence
router.post('/', verifyToken, async (req, res) => {
  try {
    const { motorcycle_id, name, center_lat, center_lng, radius_meters } = req.body;

    if (!name || center_lat === undefined || center_lng === undefined) {
      return res.status(400).json({
        success: false,
        message: 'Name, center_lat, and center_lng are required.'
      });
    }

    const newFence = {
      geofence_id: db.store.geofences.length + 1,
      motorcycle_id: motorcycle_id ? parseInt(motorcycle_id, 10) : 1,
      name,
      center_lat: parseFloat(center_lat),
      center_lng: parseFloat(center_lng),
      radius_meters: parseInt(radius_meters || 500, 10),
      is_active: true,
      created_at: new Date()
    };

    db.store.geofences.push(newFence);

    // Sync active geofence perimeter directly to physical LILYGO T-SIM7000G node
    serialGateway.sendCommand(
      `GEOFENCE_SET:${newFence.center_lat},${newFence.center_lng},${newFence.radius_meters}`
    );

    return res.status(201).json({
      success: true,
      message: 'Geofence created successfully.',
      geofence: newFence
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to create geofence.' });
  }
});

// PUT /api/geofences/:id/toggle
router.put('/:id/toggle', verifyToken, async (req, res) => {
  try {
    const id = parseInt(req.params.id, 10);
    const fence = db.store.geofences.find(g => g.geofence_id === id);

    if (!fence) {
      return res.status(404).json({ success: false, message: 'Geofence not found.' });
    }

    fence.is_active = !fence.is_active;

    return res.json({
      success: true,
      message: `Geofence ${fence.is_active ? 'activated' : 'deactivated'}.`,
      geofence: fence
    });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to toggle geofence.' });
  }
});

// DELETE /api/geofences/:id
router.delete('/:id', verifyToken, async (req, res) => {
  try {
    const id = parseInt(req.params.id, 10);
    const idx = db.store.geofences.findIndex(g => g.geofence_id === id);

    if (idx === -1) {
      return res.status(404).json({ success: false, message: 'Geofence not found.' });
    }

    db.store.geofences.splice(idx, 1);

    return res.json({ success: true, message: 'Geofence removed successfully.' });
  } catch (err) {
    return res.status(500).json({ success: false, message: 'Failed to delete geofence.' });
  }
});

module.exports = router;
