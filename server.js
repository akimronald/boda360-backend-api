/**
 * Backend Runtime & Framework: Node.js with Express.js
 * Project: IoT-Based Real-Time Tracking and Remote Immobilization Security System for Motorcycles
 * System: Central Command & Telematics Gateway
 */

const express = require('express');
const http = require('http');
const path = require('path');
const cors = require('cors');
const WebSocket = require('ws');
require('dotenv').config();

const db = require('./config/db');
const authRoutes = require('./routes/auth');
const motorcycleRoutes = require('./routes/motorcycles');
const telemetryRoutes = require('./routes/telemetry');
const commandRoutes = require('./routes/commands');
const eventRoutes = require('./routes/events');
const geofenceRoutes = require('./routes/geofences');
const mqttService = require('./services/mqttService');
const simulatorService = require('./services/simulatorService');
const serialGateway = require('./services/serialGatewayService');
const voiceCallService = require('./services/voiceCallService');
const cloudMqttBridge = require('./services/cloudMqttBridge');
const deviceStatusService = require('./services/deviceStatusService');

const app = express();
const server = http.createServer(app);

const PORT = process.env.PORT || 3000;

// Middleware
app.use(cors());
app.use(express.json());
app.use(express.urlencoded({ extended: true }));

// Serve frontend static files
app.use(express.static(path.join(__dirname, 'public')));
app.use('/image', express.static(path.join(__dirname, 'image')));
app.use('/customer', express.static(path.join(__dirname, 'customer-portal')));
app.use('/manufacturer', express.static(path.join(__dirname, 'manufacturer-portal')));

// WebSocket Server for low-latency live telemetry & command ACK stream
const wss = new WebSocket.Server({ server, path: '/ws/telemetry' });
const sseClients = new Set();

function broadcastToClients(data) {
  const message = JSON.stringify(data);

  // Broadcast to WebSockets
  wss.clients.forEach(client => {
    if (client.readyState === WebSocket.OPEN) {
      try {
        client.send(message);
      } catch (err) {
        // Ignored
      }
    }
  });

  // Broadcast to SSE clients
  sseClients.forEach(res => {
    try {
      res.write(`data: ${message}\n\n`);
    } catch (err) {
      sseClients.delete(res);
    }
  });
}

// Attach broadcast method to app locals and MQTT service
app.locals.broadcastTelemetry = broadcastToClients;
mqttService.setBroadcastCallback(broadcastToClients);

wss.on('connection', (ws) => {
  console.log('🔌 [WebSocket] Client connected for real-time boda telemetry.');

  // Send initial welcome & system status
  ws.send(JSON.stringify({
    type: 'CONNECTION_ESTABLISHED',
    system: 'Boda-Boda Telematics Command Center (Kampala)',
    timestamp: new Date().toISOString()
  }));

  ws.send(JSON.stringify(deviceStatusService.getDeviceStatus('865421045981201')));

  ws.on('message', (message) => {
    try {
      const data = JSON.parse(message);
      if (data.type === 'PING') {
        ws.send(JSON.stringify({ type: 'PONG', timestamp: Date.now() }));
      }
    } catch (e) {
      // Ignored
    }
  });

  ws.on('close', () => {
    // console.log('🔌 [WebSocket] Client disconnected.');
  });
});

// SSE Fallback Stream: /api/stream
app.get('/api/stream', (req, res) => {
  res.writeHead(200, {
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache',
    'Connection': 'keep-alive'
  });
  res.write('\n');
  sseClients.add(res);

  req.on('close', () => {
    sseClients.delete(res);
  });
});

const manufacturerRoutes = require('./routes/manufacturer');
const otaRoutes = require('./routes/ota');

// API Routes
app.use('/api/auth', authRoutes);
app.use('/api/motorcycles', motorcycleRoutes);
app.use('/api/motorcycles', telemetryRoutes); // For GET /api/motorcycles/:imei/telemetry
app.use('/api/telemetry', telemetryRoutes);
app.use('/api/command', commandRoutes);
app.use('/api/events', eventRoutes);
app.use('/api/geofences', geofenceRoutes);
app.use('/api/manufacturer', manufacturerRoutes);
app.use('/api/ota', otaRoutes);

// Voice Call & Hardware Tamper Diagnostic Endpoints
app.get('/voice-setup', (req, res) => {
  res.sendFile(path.join(publicPortalPath, 'twilio-setup.html'));
});

app.get('/api/voice/status', (req, res) => {
  res.json(voiceCallService.getVoiceStatus());
});

app.post('/api/voice/config', async (req, res) => {
  try {
    const saved = voiceCallService.saveTwilioConfig(req.body || {});
    let testCall = null;
    if (req.body && req.body.testNow) {
      testCall = await voiceCallService.placeEmergencyTamperCall({
        target: saved.callmebotUser,
        phone: saved.alertPhone,
        plate: 'UEV 412K',
        gForce: 4.5,
        force: true
      });
    }
    res.json({ success: true, ...saved, testCall });
  } catch (err) {
    res.status(400).json({ success: false, error: err.message });
  }
});

app.post('/api/voice/test', async (req, res) => {
  const result = await voiceCallService.placeEmergencyTamperCall({
    target: req.body && req.body.target,
    phone: req.body && req.body.phone,
    plate: 'UEV 412K',
    gForce: 4.5,
    force: true
  });
  res.json(result);
});

app.post('/api/hardware/test-tamper', (req, res) => {
  serialGateway.sendCommand('TEST_TAMPER');
  res.json({ success: true, message: 'Sent TEST_TAMPER to LILYGO T-SIM7000G on COM7' });
});

app.get('/api/hardware/status', (req, res) => {
  const primary = deviceStatusService.getDeviceStatus('865421045981201');
  res.json({
    success: true,
    device: primary,
    devices: deviceStatusService.getAllDeviceStatuses()
  });
});

// System Health & Info
app.get('/api/system/status', (req, res) => {
  const hwStatus = deviceStatusService.getDeviceStatus('865421045981201');
  res.json({
    status: 'ONLINE',
    hardware_online: hwStatus.is_online,
    hardware_status: hwStatus,
    system: 'IoT Motorcycle Security Command & Control Gateway',
    location: 'Kampala, Uganda',
    system_authority: 'Central Telematics Security Command',
    database_mode: db.isPostgres() ? 'PostgreSQL (Active Connection)' : 'In-Memory Relational Engine (Kampala Seed)',
    active_motorcycles: db.store.motorcycles.length,
    active_telemetry_points: db.store.telemetry_logs.length,
    total_security_events: db.store.security_events.length,
    timestamp: new Date()
  });
});

// Dedicated Portal Endpoints
app.get('/customer*', (req, res) => {
  res.sendFile(path.join(__dirname, 'customer-portal', 'index.html'));
});

app.get('/manufacturer*', (req, res) => {
  res.sendFile(path.join(__dirname, 'manufacturer-portal', 'index.html'));
});

// Serve motorbike SVG favicon on /favicon.ico requests (prevents '*' catch-all returning HTML)
app.get('/favicon.ico', (req, res) => {
  res.type('image/svg+xml');
  res.sendFile(path.join(__dirname, 'public', 'favicon.svg'));
});

// Catch-all route to serve master portal gateway
app.get('*', (req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

// Start server
async function start() {
  await db.initDatabase();

  server.listen(PORT, () => {
    console.log('================================================================');
    console.log('🏍️  KAMPALA BODA-BODA IOT REAL-TIME TRACKING & SECURITY SYSTEM');
    console.log(`🌐 Server running at: http://localhost:${PORT}`);
    console.log(`📡 WebSocket endpoint: ws://localhost:${PORT}/ws/telemetry`);
    console.log(`🚀 Open http://localhost:${PORT} in your web browser`);
    console.log('================================================================');

    deviceStatusService.startWatchdog(broadcastToClients);

    // Start hardware serial gateway to LILYGO T-SIM7000G on COM7
    serialGateway.startGateway(broadcastToClients);

    // Start wireless MTN Cellular MQTT subscriber for standalone SIM7000G tamper rings
    cloudMqttBridge.startCloudMqttBridge(broadcastToClients);

    if (process.env.ENABLE_SIMULATOR === 'true') {
      simulatorService.startSimulator(broadcastToClients);
    }
  });
}

start().catch(err => {
  console.error('Failed to start server:', err);
});
