/**
 * Real-Time ESP32 Hardware Serial Gateway Service
 * Bridges COM6 serial telemetry directly into Node.js WebSockets & REST API
 */

const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');
const db = require('../config/db');
const simulatorService = require('./simulatorService');
const voiceCallService = require('./voiceCallService');
const deviceStatusService = require('./deviceStatusService');

let childProcess = null;
let broadcastCallback = null;
let isConnected = false;
let portName = process.env.ESP32_PORT || 'COM7';
let baudRate = 115200;
let lastSerialImei = '869951038535641';

function getScriptsDir() {
  const localDir = path.join(__dirname, '..', 'scripts');
  if (fs.existsSync(path.join(localDir, 'serial_bridge.ps1'))) {
    return localDir;
  }
  return path.join(__dirname, '..', '..', 'scripts');
}

function setBroadcastCallback(cb) {
  broadcastCallback = cb;
}

function startGateway(broadcast) {
  if (broadcast) broadcastCallback = broadcast;

  console.log(`🔌 [Serial Gateway] Initializing hardware bridge on ${portName} @ ${baudRate} baud...`);

  const scriptPath = path.join(getScriptsDir(), 'serial_bridge.ps1');

  try {
    const psExe = process.env.SystemRoot 
      ? path.join(process.env.SystemRoot, 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe') 
      : 'powershell.exe';

    childProcess = spawn(psExe, [
      '-NoProfile',
      '-ExecutionPolicy', 'Bypass',
      '-File', scriptPath,
      '-Port', portName,
      '-Baud', String(baudRate)
    ]);

    let lineBuffer = '';

    childProcess.stdout.on('data', (data) => {
      lineBuffer += data.toString();
      const lines = lineBuffer.split(/\r?\n/);
      lineBuffer = lines.pop(); // keep last incomplete line

      for (const line of lines) {
        processSerialLine(line.trim());
      }
    });

    childProcess.stderr.on('data', (errData) => {
      console.error('[Serial Gateway stderr]:', errData.toString());
    });

    childProcess.on('close', (code) => {
      const wasConnected = isConnected;
      isConnected = false;
      deviceStatusService.handleUsbDisconnected('865421045981201');
      if (wasConnected) {
        console.log(`🔌 [Serial Gateway] ESP32 USB disconnected (code ${code}). Retrying ${portName} in 3s...`);
      }
      if (process.env.ENABLE_SIMULATOR === 'true') {
        simulatorService.startSimulator(broadcastCallback);
      }
      setTimeout(() => startGateway(broadcastCallback), 3000);
    });
  } catch (err) {
    console.error('[-] Failed to spawn serial gateway:', err);
    deviceStatusService.handleUsbDisconnected('865421045981201');
    setTimeout(() => startGateway(broadcastCallback), 4000);
  }
}

function processSerialLine(line) {
  if (!line) return;

  if (line.includes('__SERIAL_CONNECTED__')) {
    isConnected = true;
    console.log(`✅ [Serial Gateway] Opened ${portName} — waiting for ESP32 telemetry heartbeat...`);
    return;
  }

  if (line.includes('__SERIAL_ERROR__')) {
    isConnected = false;
    deviceStatusService.handleUsbDisconnected('865421045981201');
    return;
  }

  // Look for JSON payload in serial line
  const jsonMatch = line.match(/\{.*"device_imei".*\}/);
  if (jsonMatch) {
    try {
      const payload = JSON.parse(jsonMatch[0]);
      if (payload.device_imei) {
        lastSerialImei = String(payload.device_imei).trim();
      }
      const matchedMoto = db.store.motorcycles.find(m => m.device_imei === lastSerialImei) || db.store.motorcycles[0];
      deviceStatusService.recordHeartbeat(
        lastSerialImei,
        'USB_SERIAL',
        matchedMoto ? matchedMoto.plate_number : (payload.plate || 'UEV 412K')
      );

      // Auto-register hardware unit in db.store.devices if not present
      if (db.store && Array.isArray(db.store.devices) && lastSerialImei) {
        let dev = db.store.devices.find(d => d.device_imei === lastSerialImei);
        if (!dev) {
          dev = {
            device_id: db.store.devices.length + 1,
            device_imei: lastSerialImei,
            serial_number: `SN-USB-${lastSerialImei.slice(-6)}`,
            batch_id: 'BATCH-AUTO-DISCOVERED',
            board_type: payload.board_type || 'lilygo_tsim7000g',
            hardware_version: 'ESP32 LILYGO T-SIM7000G',
            firmware_version: payload.fw_version ? 'v' + String(payload.fw_version).replace(/^v/i, '') : 'v2.3.0',
            vendor_id: 1,
            status: matchedMoto ? 'activated_to_customer' : 'available_online',
            qa_passed: true,
            manufactured_at: new Date(),
            allocated_at: new Date(),
            activated_at: matchedMoto ? new Date() : null
          };
          db.store.devices.push(dev);
        }
      }

      if (payload.command_action) {
        console.log(`✅ [Hardware ACK Received]: Action ${payload.command_action} -> Status ${payload.status}`);
        if (matchedMoto) {
          if (payload.command_action === 'ENGINE_CUT') matchedMoto.status = 'immobilized';
          else if (payload.command_action === 'ENGINE_RESTORE') matchedMoto.status = 'active';
        }
        if (broadcastCallback) {
          broadcastCallback({
            type: 'COMMAND_ACKNOWLEDGED',
            command: {
              device_imei: matchedMoto ? matchedMoto.device_imei : payload.device_imei,
              hardware_imei: payload.device_imei,
              command_action: payload.command_action,
              command_status: payload.status
            }
          });
        }
      } else if (payload.event_type) {
        const imei = matchedMoto ? matchedMoto.device_imei : payload.device_imei;
        const moto = matchedMoto;
        const latestTel = db.store.telemetry_logs
          .filter(t => t.device_imei === imei || t.device_imei === payload.device_imei)
          .sort((a, b) => new Date(b.timestamp) - new Date(a.timestamp))[0];

        const gForce = parseFloat(payload.g_force || 3.8);
        const piezoAdc = payload.piezo_adc !== undefined ? ` (Piezo ADC: ${payload.piezo_adc}/4095)` : '';
        const isGeofence = payload.event_type === 'GEOFENCE_BREACH';
        const isBatteryCut =
          payload.event_type === 'BATTERY_DISCONNECTED' ||
          String(payload.sensor || '').includes('12V_BATTERY');
        const resolvedEventType = isBatteryCut ? 'BATTERY_DISCONNECTED' : payload.event_type;

        // Deduplicate if wireless Cellular MQTT Bridge already logged this exact hardware event < 3.5s ago
        const latestExisting = db.store.security_events[0] || null;
        if (
          latestExisting &&
          (latestExisting.device_imei === imei || latestExisting.device_imei === payload.device_imei) &&
          latestExisting.event_type === resolvedEventType &&
          (Date.now() - new Date(latestExisting.triggered_at).getTime() < 3500)
        ) {
          return;
        }

        if (moto && !isGeofence) {
          moto.status = 'tamper_alert';
        }

        const eventNotes = isGeofence
          ? `Perimeter Breach! Motorcycle is ${Math.round(payload.distance_m || 0)}m outside marked boundary (Limit: ${Math.round(payload.radius_m || 500)}m). Offline GSM GPS SMS dispatched to ${payload.alert_phone || '0748802162'}.`
          : isBatteryCut
          ? `CRITICAL POWER CUT: 12V Motorcycle Battery Removed / Wire Cut! Node running on ${payload.backup_voltage || 3.9}V internal 18650 backup battery. Emergency SMS + Voice Call dispatched to ${payload.alert_phone || '0748802162'}!`
          : `Piezo Ceramic Vibration Sensor (GPIO 32) detected physical shock of ${gForce.toFixed(1)}G${piezoAdc}. Offline GSM Tamper SMS dispatched directly to ${payload.alert_phone || '0748802162'} + Local Alarm Strobe active!`;

        const newEvent = {
          event_id: db.store.security_events.length + 1,
          device_imei: imei,
          hardware_imei: payload.device_imei,
          plate_number: moto ? moto.plate_number : (payload.plate || 'UEV 412K'),
          event_type: resolvedEventType,
          severity: isGeofence ? 'HIGH' : 'CRITICAL',
          latitude: payload.latitude !== undefined ? parseFloat(payload.latitude) : (latestTel ? latestTel.latitude : 0.184309),
          longitude: payload.longitude !== undefined ? parseFloat(payload.longitude) : (latestTel ? latestTel.longitude : 32.524229),
          notes: eventNotes,
          is_acknowledged: false,
          triggered_at: new Date()
        };

        db.store.security_events.unshift(newEvent);
        console.log(`🚨 [Hardware Security Event]: ${resolvedEventType} | ${eventNotes}`);

        if (!isGeofence) {
          const cfg = voiceCallService.getLiveConfig ? voiceCallService.getLiveConfig() : {};
          if (cfg.TWILIO_ACCOUNT_SID || cfg.CALLMEBOT_USER || cfg.ALERT_PHONE_NUMBER) {
            voiceCallService.placeEmergencyTamperCall({
              phone: payload.alert_phone || '0748802162',
              plate: moto ? moto.plate_number : 'UEV 412K',
              gForce,
              eventType: resolvedEventType,
              backupVoltage: parseFloat(payload.backup_voltage || 3.9),
              force: isBatteryCut
            });
          }
        }

        if (broadcastCallback) {
          broadcastCallback({
            type: 'SECURITY_ALERT',
            imei,
            hardware_imei: payload.device_imei,
            plate: moto ? moto.plate_number : 'UEV 412K',
            event: newEvent,
            is_real_hardware: true
          });
        }
      } else if (payload.latitude !== undefined && !isNaN(parseFloat(payload.latitude))) {
        // Real hardware active -> stop artificial simulator
        simulatorService.stopSimulator();
        handleIncomingTelemetry(payload);
      }
    } catch (e) {
      // Malformed JSON snippet
    }
  } else if (line.includes('[GPS STATUS]')) {
    deviceStatusService.recordHeartbeat(lastSerialImei, 'USB_SERIAL');
    console.log(`🛰️  ${line}`);
  } else if (
    line.includes('[SMS]') ||
    line.includes('[SMS RX') ||
    line.includes('[ALARM]') ||
    line.includes('[CALL]') ||
    line.includes('[!]') ||
    line.includes('[GEOFENCE]') ||
    line.includes('[12V GUARD]') ||
    line.includes('[MPU6050') ||
    line.includes('Piezo Sensor calibrated') ||
    line.includes('SMS delivered') ||
    line.includes('SMS send failed')
  ) {
    console.log(`📟 [SIM7000G]: ${line}`);
  }
}

function handleIncomingTelemetry(payload) {
  const imei = payload.device_imei;
  const moto = db.store.motorcycles.find(m => m.device_imei === imei) || db.store.motorcycles[0];

  const parsedBat = payload.battery_voltage !== undefined ? parseFloat(payload.battery_voltage) : 12.6;
  const newLog = {
    log_id: db.store.telemetry_logs.length + 1,
    device_imei: imei,
    latitude: parseFloat(payload.latitude),
    longitude: parseFloat(payload.longitude),
    speed: parseFloat(payload.speed || 0),
    heading: parseFloat(payload.heading || 0),
    battery_voltage: isNaN(parsedBat) ? 12.6 : parsedBat,
    ignition_status: Boolean(payload.ignition_status),
    is_immobilized: Boolean(payload.is_immobilized),
    gsm_signal: parseInt(payload.gsm_signal || 29, 10),
    satellites: parseInt(payload.satellites || 0, 10),
    gps_lock: Boolean(payload.gps_lock),
    timestamp: new Date()
  };

  db.store.telemetry_logs.push(newLog);
  console.log(`📡 [Real GPS Telemetry Ingested] Lat: ${newLog.latitude.toFixed(6)}, Lng: ${newLog.longitude.toFixed(6)} | Fix: ${newLog.gps_lock ? '3D LOCK' : 'SEARCHING'} | Sats: ${newLog.satellites}`);

  // Keep logs bounded
  if (db.store.telemetry_logs.length > 3000) {
    db.store.telemetry_logs.splice(0, 300);
  }

  // Update motorcycle state
  if (moto) {
    moto.status = newLog.is_immobilized ? 'immobilized' : 'active';
  }

  // Broadcast in real-time to all web clients (Leaflet Map)
  if (broadcastCallback) {
    broadcastCallback({
      type: 'TELEMETRY_UPDATE',
      imei: imei,
      plate: moto ? moto.plate_number : 'UEV 412K',
      telemetry: newLog,
      status: moto ? moto.status : 'active',
      is_real_hardware: true
    });
  }
}

function sendCommand(action) {
  if (childProcess && isConnected) {
    try {
      const cmdFile = path.join(getScriptsDir(), 'pending_command.txt');
      fs.writeFileSync(cmdFile, action + '\n', 'utf8');
      console.log(`⚡ [Serial Gateway] Dispatched hardware command over ${portName}: ${action}`);
      return true;
    } catch (err) {
      console.error('[-] Failed to write to serial bridge:', err);
      return false;
    }
  }
  return false;
}

function stopGateway() {
  if (childProcess) {
    try {
      childProcess.kill();
    } catch (e) {}
    childProcess = null;
    isConnected = false;
  }
}

module.exports = {
  startGateway,
  stopGateway,
  sendCommand,
  setBroadcastCallback,
  isConnected: () => isConnected
};
