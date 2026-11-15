/**
 * Geofencing Calculation & Perimeter Detection Service
 * Calculates Haversine spherical distance between GPS coordinates
 * Author: Akim Ronald David (ISBAT University)
 */

const db = require('../config/db');

// Calculate great-circle distance in meters between two lat/lng points
function haversineDistance(lat1, lon1, lat2, lon2) {
  const R = 6371e3; // Earth radius in meters
  const phi1 = (lat1 * Math.PI) / 180;
  const phi2 = (lat2 * Math.PI) / 180;
  const deltaPhi = ((lat2 - lat1) * Math.PI) / 180;
  const deltaLambda = ((lon2 - lon1) * Math.PI) / 180;

  const a =
    Math.sin(deltaPhi / 2) * Math.sin(deltaPhi / 2) +
    Math.cos(phi1) * Math.cos(phi2) * Math.sin(deltaLambda / 2) * Math.sin(deltaLambda / 2);
  const c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));

  return R * c; // in meters
}

function checkGeofenceBreach(motorcycle, currentLat, currentLng) {
  const fences = db.store.geofences.filter(
    g => g.motorcycle_id === motorcycle.motorcycle_id && g.is_active
  );

  for (const fence of fences) {
    const dist = haversineDistance(
      fence.center_lat,
      fence.center_lng,
      currentLat,
      currentLng
    );

    if (dist > fence.radius_meters) {
      return {
        breached: true,
        geofence_id: fence.geofence_id,
        name: fence.name,
        distance: dist,
        radius_meters: fence.radius_meters
      };
    }
  }

  return null;
}

module.exports = {
  haversineDistance,
  checkGeofenceBreach
};
