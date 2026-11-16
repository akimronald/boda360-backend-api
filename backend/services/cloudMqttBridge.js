/**
 * Zero-Dependency MQTT v3.1.1 TCP Subscriber for Wireless SIM7000G Tamper Alerts
 *
 * Subscribes directly to `boda/865421045981201/tamper` on broker.hivemq.com:1883
 * so that even when the LILYGO T-SIM7000G is unplugged from USB and running wirelessly
 * over MTN Uganda Cellular Data, physical vibration triggers the Free Twilio Tamper Ring.
 */

const net = require('net');
const db = require('../config/db');
const voiceCallService = require('./voiceCallService');
const deviceStatusService = require('./deviceStatusService');
const serialGateway = require('./serialGatewayService');

const BROKER_HOST = 'broker.hivemq.com';
const BROKER_PORT = 1883;
const TAMPER_TOPIC = 'boda/+/#';

let socket = null;
let isBrokerConnected = false;
let pingTimer = null;
let activeBroadcastCallback = null;
const lastOwnerSyncSentMap = new Map(); // imei -> timestamp ms (prevents duplicate burst syncs)

function normalizeUgandaPhones(rawPhone) {
  const digits = String(rawPhone || '').replace(/[^\d+]/g, '');
  let local = '0748802162';
  let intl = '+256748802162';
  if (digits.startsWith('+256') && digits.length >= 12) {
    intl = digits;
    local = '0' + digits.slice(4);
  } else if (digits.startsWith('256') && digits.length >= 11) {
    intl = '+' + digits;
    local = '0' + digits.slice(3);
  } else if (digits.startsWith('0') && digits.length >= 10) {
    local = digits;
    intl = '+256' + digits.slice(1);
  } else if (digits.length >= 9) {
    local = digits;
    intl = digits.startsWith('+') ? digits : '+' + digits;
  }
  return { local, intl };
}

/**
 * Pushes owner phone, email, and motorcycle plate down to an ESP32 board (LILYGO or ESP32-WROOM)
 * so the board saves them permanently in NVS (<Preferences.h>) across reboots & Internet OTA!
 */
function syncOwnerProfileToHardware({ imei, plate, ownerPhone, ownerEmail }) {
  const cleanImei = String(imei || '').trim();
  if (!cleanImei) return false;

  const phones = normalizeUgandaPhones(ownerPhone);
  const syncPayload = {
    cmd: 'OWNER_SYNC',
    imei: cleanImei,
    plate: String(plate || 'UNBOUND').trim().toUpperCase(),
    owner_phone: phones.local,
    owner_phone_intl: phones.intl,
    owner_email: String(ownerEmail || '').trim().toLowerCase(),
    synced_at: Date.now()
  };

  lastOwnerSyncSentMap.set(cleanImei, Date.now());

  const cmdTopic = `boda/${cleanImei}/command`;
  const sentMqtt = publishCloudMqtt(cmdTopic, syncPayload);

  // Also forward over USB Serial if this board is currently plugged into bench USB
  if (serialGateway && typeof serialGateway.sendCommand === 'function') {
    serialGateway.sendCommand(JSON.stringify(syncPayload));
  }

  console.log(
    `🔐 [Owner -> ESP32 NVS Sync] IMEI ${cleanImei} | Plate: ${syncPayload.plate} | Phone: ${phones.intl} | Email: ${syncPayload.owner_email || 'n/a'} (CloudMQTT: ${sentMqtt ? 'SENT' : 'QUEUED_FOR_BOOT'})`
  );
  return sentMqtt;
}

function buildPublishPacket(topic, messageStr) {
  const topicBuf = encodeString(topic);
  const msgBuf = Buffer.from(
    typeof messageStr === 'string' ? messageStr : JSON.stringify(messageStr),
    'utf8'
  );
  const body = Buffer.concat([topicBuf, msgBuf]);
  return Buffer.concat([Buffer.from([0x30]), encodeRemainingLength(body.length), body]);
}

function publishCloudMqtt(topic, payload) {
  if (!socket || socket.destroyed || !isBrokerConnected) {
    return false;
  }
  try {
    const pkt = buildPublishPacket(topic, payload);
    socket.write(pkt);
    return true;
  } catch (err) {
    console.warn('⚠️ [Cellular MQTT Bridge] Publish failed:', err.message);
    return false;
  }
}

function encodeRemainingLength(len) {
  const bytes = [];
  do {
    let encoded = len % 128;
    len = Math.floor(len / 128);
    if (len > 0) encoded |= 0x80;
    bytes.push(encoded);
  } while (len > 0);
  return Buffer.from(bytes);
}

function encodeString(str) {
  const strBuf = Buffer.from(str, 'utf8');
  const lenBuf = Buffer.alloc(2);
  lenBuf.writeUInt16BE(strBuf.length, 0);
  return Buffer.concat([lenBuf, strBuf]);
}

function buildConnectPacket(clientId) {
  const varHeader = Buffer.from([
    0x00, 0x04, 0x4d, 0x51, 0x54, 0x54, // "MQTT"
    0x04,                               // Protocol Level 4 (v3.1.1)
    0x02,                               // Clean Session
    0x00, 0x3c                          // KeepAlive = 60s
  ]);
  const payload = encodeString(clientId);
  const body = Buffer.concat([varHeader, payload]);
  return Buffer.concat([Buffer.from([0x10]), encodeRemainingLength(body.length), body]);
}

function buildSubscribePacket(packetId, topic) {
  const pktIdBuf = Buffer.alloc(2);
  pktIdBuf.writeUInt16BE(packetId, 0);
  const body = Buffer.concat([pktIdBuf, encodeString(topic), Buffer.from([0x00])]);
  return Buffer.concat([Buffer.from([0x82]), encodeRemainingLength(body.length), body]);
}

function startCloudMqttBridge(broadcastCallback) {
  if (broadcastCallback) activeBroadcastCallback = broadcastCallback;
  if (socket) {
    try { socket.destroy(); } catch (_) {}
  }
  isBrokerConnected = false;
  if (pingTimer) clearInterval(pingTimer);

  const clientId = 'boda_twilio_gw_' + Math.random().toString(16).slice(2, 10);
  socket = net.createConnection({ host: BROKER_HOST, port: BROKER_PORT }, () => {
    socket.write(buildConnectPacket(clientId));
  });

  let rxBuf = Buffer.alloc(0);

  socket.on('data', (chunk) => {
    rxBuf = Buffer.concat([rxBuf, chunk]);

    while (rxBuf.length >= 2) {
      const packetType = rxBuf[0] & 0xf0;
      let multiplier = 1;
      let remLen = 0;
      let idx = 1;
      let completeHeader = false;

      while (idx < rxBuf.length && idx <= 4) {
        const byte = rxBuf[idx++];
        remLen += (byte & 0x7f) * multiplier;
        multiplier *= 128;
        if ((byte & 0x80) === 0) {
          completeHeader = true;
          break;
        }
      }

      if (!completeHeader || rxBuf.length < idx + remLen) break;

      const body = rxBuf.slice(idx, idx + remLen);
      rxBuf = rxBuf.slice(idx + remLen);

      if (packetType === 0x20) {
        // CONNACK -> Subscribe to wildcard boda/+/#
        isBrokerConnected = true;
        socket.write(buildSubscribePacket(1, TAMPER_TOPIC));
        console.log(`☁️  [Cellular MQTT Bridge] Listening on ${BROKER_HOST} (${TAMPER_TOPIC}) for wireless SIM7000G/SIM800L telemetry, OTA & tamper alerts.`);
        pingTimer = setInterval(() => {
          if (socket && !socket.destroyed) {
            socket.write(Buffer.from([0xc0, 0x00])); // PINGREQ
          }
        }, 45000);
      } else if (packetType === 0x30 && body.length >= 2) {
        // PUBLISH
        const topicLen = body.readUInt16BE(0);
        if (body.length >= 2 + topicLen) {
          const topicStr = body.slice(2, 2 + topicLen).toString('utf8');
          const msgStr = body.slice(2 + topicLen).toString('utf8');
          try {
            const payload = JSON.parse(msgStr);
            const topicParts = topicStr.split('/');
            const imeiFromTopic = (topicParts.length >= 2 && topicParts[0] === 'boda') ? topicParts[1] : null;
            const imeiFromPayload = payload.device_imei || imeiFromTopic || '865421045981201';
            const matchedMoto = (db.store && db.store.motorcycles)
              ? db.store.motorcycles.find(m => m.device_imei === imeiFromPayload)
              : null;
            if (topicStr.endsWith('/telemetry') || topicStr.endsWith('/tamper') || topicStr.endsWith('/ack')) {
              deviceStatusService.recordHeartbeat(
                imeiFromPayload,
                'CELLULAR_MQTT',
                matchedMoto ? matchedMoto.plate_number : (payload.plate || 'UNBOUND')
              );
              if (db.store && Array.isArray(db.store.devices)) {
                let dev = db.store.devices.find(d => d.device_imei === imeiFromPayload);
                if (!dev) {
                  // Auto-discover brand-new ESP32 board (AT+GSN modem IMEI or eFuse chip ID)
                  dev = {
                    device_id: db.store.devices.length + 1,
                    device_imei: imeiFromPayload,
                    serial_number: `SN-AUTO-${imeiFromPayload.slice(-6)}`,
                    batch_id: 'BATCH-AUTO-DISCOVERED',
                    board_type: payload.board_type || 'esp32_wroom',
                    hardware_version: payload.board_type === 'lilygo_tsim7000g'
                      ? 'ESP32 LILYGO T-SIM7000G'
                      : 'ESP32-WROOM-32 (SIM800L + NEO-6M)',
                    firmware_version: payload.fw_version ? 'v' + String(payload.fw_version).replace(/^v/i, '') : 'v2.1.0',
                    vendor_id: 1,
                    status: matchedMoto ? 'activated_to_customer' : 'available_online',
                    qa_passed: true,
                    manufactured_at: new Date(),
                    allocated_at: new Date(),
                    activated_at: matchedMoto ? new Date() : null
                  };
                  db.store.devices.push(dev);
                  console.log(`🆕 [Auto-Discovered ESP32 Board] Registered hardware IMEI ${imeiFromPayload} (${dev.hardware_version})`);
                } else {
                  if (payload.fw_version) dev.firmware_version = 'v' + String(payload.fw_version).replace(/^v/i, '');
                  if (payload.board_type) dev.board_type = payload.board_type;
                }
              }

              // Automatic NVS Catch-Up Sync: if this IMEI belongs to a registered customer and the board
              // just booted (BOOT_READY) or still has an un-synced plate/phone in NVS, push OWNER_SYNC automatically!
              if (matchedMoto && db.store && Array.isArray(db.store.users)) {
                const owner = db.store.users.find(u => u.user_id === matchedMoto.user_id);
                if (owner) {
                  const isBootReady = topicStr.endsWith('/ack') && payload.command_action === 'BOOT_READY';
                  const plateMismatch = payload.plate && String(payload.plate).toUpperCase() !== String(matchedMoto.plate_number).toUpperCase();
                  const lastSent = lastOwnerSyncSentMap.get(imeiFromPayload) || 0;
                  if (isBootReady || (plateMismatch && (Date.now() - lastSent > 60000))) {
                    syncOwnerProfileToHardware({
                      imei: imeiFromPayload,
                      plate: matchedMoto.plate_number,
                      ownerPhone: owner.phone_number,
                      ownerEmail: owner.email
                    });
                  }
                }
              }
            }

            const cb = broadcastCallback || activeBroadcastCallback;

            // Broadcast OTA Firmware Flash Progress / Completion OR Engine Cut/Restore ACKs from ESP32 nodes
            if (topicStr.endsWith('/ack') && payload.command_action) {
              const cmdAct = String(payload.command_action);
              if (cmdAct.startsWith('OTA_')) {
                console.log(`🚀 [Wireless OTA ACK] IMEI ${imeiFromPayload} (${payload.board_type || 'ESP32'}): ${cmdAct} -> ${payload.status}`);
                if (cb) {
                  cb({
                    type: 'OTA_STATUS_UPDATE',
                    event: cmdAct,
                    status: payload.status,
                    imei: imeiFromPayload,
                    board: payload.board_type || null,
                    fw_version: payload.fw_version || null,
                    timestamp: new Date().toISOString()
                  });
                }
              } else if (cmdAct === 'ENGINE_CUT' || cmdAct === 'ENGINE_RESTORE') {
                const moto = matchedMoto || (db.store && db.store.motorcycles && db.store.motorcycles[0]) || null;
                if (moto) {
                  moto.status = cmdAct === 'ENGINE_CUT' ? 'immobilized' : 'active';
                }
                console.log(`✅ [Wireless MQTT ACK] IMEI ${imeiFromPayload}: ${cmdAct} -> ${payload.status}`);
                if (cb) {
                  cb({
                    type: 'COMMAND_ACKNOWLEDGED',
                    command: {
                      device_imei: moto ? moto.device_imei : imeiFromPayload,
                      hardware_imei: imeiFromPayload,
                      command_action: cmdAct,
                      command_status: payload.status || 'EXECUTED'
                    }
                  });
                }
              }
            }

            // Ingest wireless cellular GPS telemetry when USB Serial hasn't logged a packet in last 2.2s
            if (topicStr.endsWith('/telemetry') && payload.latitude !== undefined && !isNaN(parseFloat(payload.latitude))) {
              const latestTel = (db.store && db.store.telemetry_logs && db.store.telemetry_logs.length > 0)
                ? db.store.telemetry_logs[db.store.telemetry_logs.length - 1]
                : null;
              const serialFresh = latestTel && (Date.now() - new Date(latestTel.timestamp).getTime() < 2200);
              if (!serialFresh) {
                const moto = (db.store && db.store.motorcycles)
                  ? (db.store.motorcycles.find(m => m.device_imei === imeiFromPayload) || db.store.motorcycles[0])
                  : null;
                const parsedBat = payload.battery_voltage !== undefined ? parseFloat(payload.battery_voltage) : 12.6;
                const newLog = {
                  log_id: (db.store && db.store.telemetry_logs ? db.store.telemetry_logs.length : 0) + 1,
                  device_imei: moto ? moto.device_imei : imeiFromPayload,
                  hardware_imei: imeiFromPayload,
                  latitude: parseFloat(payload.latitude),
                  longitude: parseFloat(payload.longitude),
                  speed: parseFloat(payload.speed || 0),
                  heading: parseFloat(payload.heading || 0),
                  battery_voltage: isNaN(parsedBat) ? 12.6 : parsedBat,
                  battery_disconnected: Boolean(payload.battery_disconnected),
                  ignition_status: Boolean(payload.ignition_status),
                  is_immobilized: Boolean(payload.is_immobilized),
                  gsm_signal: parseInt(payload.gsm_signal || 24, 10),
                  satellites: parseInt(payload.satellites || 0, 10),
                  gps_lock: Boolean(payload.gps_lock),
                  timestamp: new Date()
                };
                if (moto) {
                  moto.status = newLog.is_immobilized ? 'immobilized' : 'active';
                }
                if (db.store && Array.isArray(db.store.telemetry_logs)) {
                  db.store.telemetry_logs.push(newLog);
                }
                if (cb) {
                  cb({
                    type: 'TELEMETRY_UPDATE',
                    imei: moto ? moto.device_imei : imeiFromPayload,
                    hardware_imei: imeiFromPayload,
                    plate: moto ? moto.plate_number : (payload.plate || 'UEV 412K'),
                    telemetry: newLog,
                    status: moto ? moto.status : 'active',
                    is_real_hardware: true,
                    transport: 'CELLULAR_MQTT'
                  });
                }
              }
            }

            if (
              payload.event_type === 'TAMPER_DETECTED' ||
              payload.event_type === 'BATTERY_DISCONNECTED' ||
              payload.event_type === 'GEOFENCE_BREACH'
            ) {
              const rawImei = payload.device_imei || imeiFromPayload || '869951038535641';
              const isBatteryCut =
                payload.event_type === 'BATTERY_DISCONNECTED' ||
                String(payload.sensor || '').includes('12V_BATTERY');
              const isGeofence = payload.event_type === 'GEOFENCE_BREACH';
              const resolvedEventType = isBatteryCut ? 'BATTERY_DISCONNECTED' : payload.event_type;
              const moto = (db.store && db.store.motorcycles)
                ? (db.store.motorcycles.find(m => m.device_imei === rawImei) || db.store.motorcycles[0])
                : null;
              const imei = moto ? moto.device_imei : rawImei;
              const plate = moto ? moto.plate_number : (payload.plate || 'UEV 412K');

              // Deduplicate if USB Serial Gateway already logged this exact hardware event < 3.5s ago
              const latestExisting = (db.store && db.store.security_events && db.store.security_events[0]) || null;
              const isDuplicate = latestExisting &&
                (latestExisting.device_imei === imei || latestExisting.device_imei === rawImei) &&
                latestExisting.event_type === resolvedEventType &&
                (Date.now() - new Date(latestExisting.triggered_at).getTime() < 3500);

              if (isDuplicate) {
                return;
              }

              if (isBatteryCut) {
                console.log(`☁️  [Wireless SIM7000G 12V Battery Cut Alert]: 12V battery removed! Running on ${payload.backup_voltage || 3.9}V backup — Ringing owner & SMS dispatched to +256748802162!`);
              } else if (isGeofence) {
                console.log(`☁️  [Wireless SIM7000G Geofence Breach]: Bike ${Math.round(payload.distance_m || 0)}m outside safe zone — SMS dispatched to +256748802162!`);
              } else {
                console.log(`☁️  [Wireless SIM7000G Tamper Sync]: Shock ${payload.g_force}G (Offline GSM SMS sent directly by SIM7000G to +256748802162)`);
              }

              if (!isGeofence) {
                const cfg = voiceCallService.getLiveConfig ? voiceCallService.getLiveConfig() : {};
                if (cfg.TWILIO_ACCOUNT_SID || cfg.CALLMEBOT_USER || cfg.ALERT_PHONE_NUMBER) {
                  voiceCallService.placeEmergencyTamperCall({
                    phone: payload.alert_phone || '0748802162',
                    plate,
                    gForce: parseFloat(payload.g_force || 4.2),
                    eventType: resolvedEventType,
                    backupVoltage: parseFloat(payload.backup_voltage || 3.9),
                    force: isBatteryCut
                  });
                }
              }

              const latestTel = (db.store && db.store.telemetry_logs)
                ? db.store.telemetry_logs
                    .filter(t => t.device_imei === imei || t.device_imei === rawImei)
                    .sort((a, b) => new Date(b.timestamp) - new Date(a.timestamp))[0]
                : null;

              const eventNotes = isBatteryCut
                ? `CRITICAL POWER CUT: 12V Motorcycle Battery Disconnected / Wire Cut! Tracker running on ${payload.backup_voltage || 3.9}V backup cell — Emergency SMS & Voice Call sent to ${payload.alert_phone || '+256748802162'}.`
                : isGeofence
                ? `PERIMETER BREACH: Motorcycle is ${Math.round(payload.distance_m || 0)}m outside marked safe zone (Limit: ${Math.round(payload.radius_m || 500)}m) — Live GPS SMS sent to ${payload.alert_phone || '+256748802162'}.`
                : `TAMPER / SHOCK ALERT: Physical vibration (${payload.g_force || 4.0}G) detected while parked — Emergency SMS & Voice Call sent to ${payload.alert_phone || '+256748802162'}.`;

              const newEvent = {
                event_id: (db.store && db.store.security_events ? db.store.security_events.length : 0) + 1,
                device_imei: imei,
                hardware_imei: rawImei,
                plate_number: plate,
                event_type: resolvedEventType,
                severity: isGeofence ? 'HIGH' : 'CRITICAL',
                latitude: payload.latitude !== undefined ? parseFloat(payload.latitude) : (latestTel ? latestTel.latitude : 0.184309),
                longitude: payload.longitude !== undefined ? parseFloat(payload.longitude) : (latestTel ? latestTel.longitude : 32.524292),
                notes: eventNotes,
                is_acknowledged: false,
                triggered_at: new Date()
              };

              if (moto && !isGeofence) {
                moto.status = 'tamper_alert';
              }
              if (db.store && Array.isArray(db.store.security_events)) {
                db.store.security_events.unshift(newEvent);
              }

              if (cb) {
                cb({
                  type: 'SECURITY_ALERT',
                  imei,
                  hardware_imei: rawImei,
                  plate,
                  is_real_hardware: true,
                  event: newEvent
                });
              }
            }
          } catch (_) {}
        }
      }
    }
  });

  socket.on('error', () => {
    isBrokerConnected = false;
  });
  socket.on('close', () => {
    isBrokerConnected = false;
    if (pingTimer) clearInterval(pingTimer);
    setTimeout(() => startCloudMqttBridge(broadcastCallback || activeBroadcastCallback), 10000);
  });
}

module.exports = { startCloudMqttBridge, publishCloudMqtt, syncOwnerProfileToHardware };

