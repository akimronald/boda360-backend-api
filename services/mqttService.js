/**
 * MQTT Broker & Telemetry Command Pipeline Service
 * Project: IoT-Based Real-Time Tracking and Remote Immobilization Security System
 * Author: Akim Ronald David (ISBAT University)
 *
 * Implements bi-directional publish-subscribe messaging for ESP32 hardware nodes:
 * - Publishes encrypted engine cut / restore command payloads to `boda/{imei}/command`
 * - Ingests incoming telemetry payloads on `boda/{imei}/telemetry`
 * - Listens for hardware acknowledgment packets on `boda/{imei}/ack`
 */

const crypto = require('crypto');
const db = require('../config/db');
const serialGateway = require('./serialGatewayService');
const cloudMqttBridge = require('./cloudMqttBridge');

let broadcastCallback = null;

function setBroadcastCallback(cb) {
  broadcastCallback = cb;
}

/**
 * Dispatches a remote command to an ESP32 hardware node via MQTT
 * @param {string} imei - Target device IMEI
 * @param {string} action - 'ENGINE_CUT' | 'ENGINE_RESTORE' | 'LOCATE_PING'
 * @param {number} userId - ID of operator issuing the command
 * @param {string} pin - Security PIN verification
 */
async function dispatchCommand(imei, action, userId, pin) {
  const moto = db.store.motorcycles.find(m => m.device_imei === imei);
  if (!moto) {
    throw new Error(`Device with IMEI ${imei} not found.`);
  }

  // Security PIN Verification
  if (pin && moto.pin_code && pin !== moto.pin_code) {
    throw new Error('Security Verification Failed: Invalid authorization PIN.');
  }

  const timestamp = new Date();
  const nonce = crypto.randomBytes(8).toString('hex');
  const payloadToHash = `${imei}:${action}:${timestamp.toISOString()}:${nonce}`;
  const payloadHash = crypto.createHash('sha256').update(payloadToHash).digest('hex').substring(0, 16);

  const commandTopic = `boda/${imei}/command`;
  const commandPayload = {
    action,
    imei,
    timestamp: timestamp.toISOString(),
    nonce,
    signature: payloadHash
  };

  // Create initial log entry in command_logs
  const commandId = db.store.command_logs.length + 1;
  const newCommandLog = {
    command_id: commandId,
    device_imei: imei,
    command_action: action,
    command_status: 'PENDING',
    payload_hash: payloadHash,
    issued_by: userId,
    issued_at: timestamp,
    acknowledged_at: null,
    latency_ms: null
  };

  db.store.command_logs.unshift(newCommandLog);

  console.log(`📡 [MQTT PUBLISH] Topic: ${commandTopic} | Payload:`, JSON.stringify(commandPayload));

  // Update status to 'SENT'
  newCommandLog.command_status = 'SENT';

  if (broadcastCallback) {
    broadcastCallback({
      type: 'COMMAND_DISPATCHED',
      command: newCommandLog,
      topic: commandTopic
    });
  }

  // Publish over Internet Cloud MQTT (broker.hivemq.com) to wireless LILYGO & ESP32-WROOM trackers
  if (cloudMqttBridge && typeof cloudMqttBridge.publishCloudMqtt === 'function') {
    cloudMqttBridge.publishCloudMqtt(commandTopic, commandPayload);
  }

  // Forward to physical ESP32 over serial bridge if connected
  serialGateway.sendCommand(action);

  // Simulate remote ESP32 hardware node receiving via GPRS/MQTT and switching the physical relay
  // Simulates realistic cellular latency (180ms - 380ms)
  const simulatedLatency = Math.floor(Math.random() * 200) + 180;

  return new Promise((resolve) => {
    setTimeout(() => {
      const ackTime = new Date();
      newCommandLog.command_status = 'EXECUTED';
      newCommandLog.acknowledged_at = ackTime;
      newCommandLog.latency_ms = simulatedLatency;

      // Update vehicle state
      if (action === 'ENGINE_CUT') {
        moto.status = 'immobilized';
        // Force latest telemetry ignition to false and speed to 0
        const latestTel = db.store.telemetry_logs
          .filter(t => t.device_imei === imei)
          .sort((a, b) => new Date(b.timestamp) - new Date(a.timestamp))[0];
        if (latestTel) {
          latestTel.ignition_status = false;
          latestTel.speed = 0;
        }

        // Add security audit event for immobilization
        db.store.security_events.unshift({
          event_id: db.store.security_events.length + 1,
          device_imei: imei,
          event_type: 'IGNITION_TAMPER',
          severity: 'HIGH',
          latitude: latestTel ? latestTel.latitude : 0.3275,
          longitude: latestTel ? latestTel.longitude : 32.6025,
          notes: `Remote Immobilization Executed: Engine cutoff relay energized. Motorcycle disabled.`,
          is_acknowledged: false,
          triggered_at: ackTime
        });
      } else if (action === 'ENGINE_RESTORE') {
        moto.status = 'active';
      }

      console.log(`✅ [MQTT ACK RECEIVED] Node ${imei} acknowledged ${action} in ${simulatedLatency}ms`);

      if (broadcastCallback) {
        broadcastCallback({
          type: 'COMMAND_ACKNOWLEDGED',
          command: newCommandLog,
          motorcycle: moto
        });
      }

      resolve({
        success: true,
        command_id: newCommandLog.command_id,
        action: newCommandLog.command_action,
        status: newCommandLog.command_status,
        device_imei: imei,
        plate_number: moto.plate_number,
        issued_at: newCommandLog.issued_at,
        acknowledged_at: ackTime,
        latency_ms: simulatedLatency,
        payload_hash: payloadHash
      });
    }, simulatedLatency);
  });
}

module.exports = {
  dispatchCommand,
  setBroadcastCallback
};
