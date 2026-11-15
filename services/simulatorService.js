/**
 * Kampala Boda-Boda Hardware Node Simulator Engine
 * Author: Akim Ronald David (ISBAT University)
 *
 * Simulates real-time telemetry streaming from ESP32 nodes across Kampala road networks:
 * - Route 1: Lugogo Bypass Hub -> Jinja Road -> Kampala Road
 * - Route 2: Nakasero Market -> Entebbe Road -> Clock Tower
 * - Route 3: Wandegeya Junction -> Makerere Hill -> Bombo Road
 * - Simulates battery drainage, GSM signal fluctuations, and ignition states.
 * - Responds to ENGINE_CUT commands by stopping the vehicle.
 */

const db = require('../config/db');
const geofenceService = require('./geofenceService');

// Waypoint paths in Kampala
const KAMPALA_ROUTES = {
  '865421045981201': [
    // Lugogo Bypass circuit (ESP32 Hardware Node)
    { lat: 0.327500, lng: 32.602500, heading: 45 },
    { lat: 0.328200, lng: 32.603800, heading: 60 },
    { lat: 0.328900, lng: 32.605200, heading: 85 },
    { lat: 0.327100, lng: 32.607400, heading: 140 },
    { lat: 0.325000, lng: 32.608500, heading: 170 },
    { lat: 0.323200, lng: 32.607000, heading: 220 },
    { lat: 0.324100, lng: 32.603500, heading: 290 },
    { lat: 0.326000, lng: 32.602000, heading: 340 }
  ]
};

const routeIndices = {
  '865421045981201': 0
};

let simulationInterval = null;
let broadcastFn = null;

function startSimulator(broadcast) {
  broadcastFn = broadcast;

  if (simulationInterval) {
    clearInterval(simulationInterval);
  }

  // Update telemetry every 3 seconds
  simulationInterval = setInterval(() => {
    simulateTick();
  }, 3000);

  console.log('🚀 [Simulator] Kampala Boda-Boda Telemetry Engine started (3s cadence).');
}

function simulateTick() {
  const activeBikes = db.store.motorcycles.filter(m => m.status === 'active' || m.status === 'tamper_alert');

  for (const bike of activeBikes) {
    const imei = bike.device_imei;
    const route = KAMPALA_ROUTES[imei];

    if (!route || route.length === 0) continue;

    // Advance waypoint index
    routeIndices[imei] = (routeIndices[imei] + 1) % route.length;
    const targetPoint = route[routeIndices[imei]];

    // Add tiny realistic GPS jitter (+/- 0.00005 deg ~ 5 meters)
    const jitterLat = (Math.random() - 0.5) * 0.0001;
    const jitterLng = (Math.random() - 0.5) * 0.0001;

    const lat = targetPoint.lat + jitterLat;
    const lng = targetPoint.lng + jitterLng;

    const speed = Math.floor(Math.random() * 25) + 25; // 25 - 50 km/h
    const heading = targetPoint.heading;
    const battery = Number((12.4 + (Math.random() * 0.4)).toFixed(2));
    const gsmSignal = Math.floor(Math.random() * 6) + 26; // 26-31 CSQ

    const newLog = {
      log_id: db.store.telemetry_logs.length + 1,
      device_imei: imei,
      latitude: lat,
      longitude: lng,
      speed,
      heading,
      battery_voltage: battery,
      ignition_status: true,
      gsm_signal: gsmSignal,
      timestamp: new Date()
    };

    db.store.telemetry_logs.push(newLog);

    // Keep log buffer bounded to avoid memory exhaustion during long runs
    if (db.store.telemetry_logs.length > 5000) {
      db.store.telemetry_logs.splice(0, 500);
    }

    // Check Geofence
    const breach = geofenceService.checkGeofenceBreach(bike, lat, lng);
    if (breach) {
      // Check if breach event logged recently to avoid duplicates
      const recentBreach = db.store.security_events.find(
        e => e.device_imei === imei && e.event_type === 'GEOFENCE_BREACH' && (Date.now() - new Date(e.triggered_at).getTime()) < 60000
      );

      if (!recentBreach) {
        const newEvent = {
          event_id: db.store.security_events.length + 1,
          device_imei: imei,
          event_type: 'GEOFENCE_BREACH',
          severity: 'HIGH',
          latitude: lat,
          longitude: lng,
          notes: `Breach detected: Exited boundary '${breach.name}'. Distance: ${Math.round(breach.distance)}m (Limit: ${breach.radius_meters}m)`,
          is_acknowledged: false,
          triggered_at: new Date()
        };
        db.store.security_events.unshift(newEvent);

        if (broadcastFn) {
          broadcastFn({
            type: 'SECURITY_ALERT',
            imei,
            plate: bike.plate_number,
            event: newEvent
          });
        }
      }
    }

    // Broadcast live telemetry update
    if (broadcastFn) {
      broadcastFn({
        type: 'TELEMETRY_UPDATE',
        imei,
        plate: bike.plate_number,
        telemetry: newLog,
        status: bike.status
      });
    }
  }
}

function stopSimulator() {
  if (simulationInterval) {
    clearInterval(simulationInterval);
    simulationInterval = null;
    console.log('⏹️  [Simulator] Kampala Telemetry Simulator stopped.');
  }
}

module.exports = {
  startSimulator,
  stopSimulator,
  KAMPALA_ROUTES
};
