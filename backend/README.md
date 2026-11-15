# Boda 360 Telematics | Cloud Backend & API Engine

**Organization / Role:** Central Cloud Backend, Telemetry Ingestion & WebSocket / MQTT Server  
**Lead Engineer:** Akim Ronald David (ISBAT University, Kampala, Uganda)

---

## 1. Overview & Purpose
The **Backend** is the central computing core of the Boda 360 IoT security ecosystem. It runs a Node.js and Express.js server providing:
* **REST API Endpoints:** Authentication, device registration, motorcycle telemetry, bi-directional engine cut-off commands, geofencing, and security events.
* **Real-Time Data Streaming:** WebSocket (`ws://localhost:3000/ws/telemetry`) and Server-Sent Events (`/api/stream`) for low-latency live GPS movement and instant tamper alerts.
* **Relational Storage:** Dual-mode persistence supporting PostgreSQL 14+ with an automated, zero-configuration in-memory relational fallback loaded with Kampala demonstration data.
* **Multi-Portal Static Serving:** Natively serves the Customer Portal (`/customer`), Vendor Portal (`/vendor`), and Manufacturer Portal (`/manufacturer`).

---

## 2. Directory Structure
```text
backend/
├── server.js               # Main HTTP & WebSocket server entrypoint
├── package.json            # Node.js dependencies (express, ws, pg, bcryptjs, jsonwebtoken, dotenv)
├── .env                    # Environment variables (port, db connection, jwt secret)
├── config/
│   └── db.js               # Database adaptor (PostgreSQL + In-memory relational store)
├── middleware/
│   └── auth.js             # JWT verification & role-based access control (RBAC)
├── routes/
│   ├── auth.js             # User registration, login & session endpoints
│   ├── commands.js         # Remote immobilization & ignition restoration
│   ├── events.js           # Security breaches, tamper alarms & acknowledgments
│   ├── geofences.js        # Boundary configuration & polygon violation checks
│   ├── manufacturer.js     # Hardware batch provisioning & system health
│   ├── motorcycles.js      # Motorcycle profile management & device binding
│   ├── telemetry.js        # Time-series GPS coordinate ingestion & trail retrieval
│   └── vendor.js           # Dealership registration terminal & diagnostic checks
├── services/
│   ├── geofenceService.js  # Haversine great-circle perimeter calculations
│   ├── mqttService.js      # Bi-directional MQTT message publisher & command pipeline
│   └── simulatorService.js # Autonomous Kampala road GPS movement generator
└── db/
    ├── schema.sql          # PostgreSQL DDL table definitions
    └── seed.sql            # Seed data (Kampala routes, users, bikes, geofences)
```

---

## 3. Key REST API Endpoints

### A. Authentication (`/api/auth`)
* `POST /api/auth/register` - Create owner, vendor, or manufacturer account
* `POST /api/auth/login` - Authenticate credentials and receive signed JWT
* `GET /api/auth/me` - Retrieve current session profile

### B. Remote Commands & Immobilization (`/api/command`)
* `POST /api/command/immobilize` - Dispatch encrypted engine cut command via MQTT (`PIN: 1234`)
* `POST /api/command/restore` - De-energize immobilizer relay and restore ignition
* `GET /api/command/logs` - Command audit trail with latency benchmarks

### C. Live Telemetry & GPS Ingestion (`/api/telemetry`)
* `GET /api/motorcycles/:imei/telemetry` - Latest telemetry point and historical breadcrumb route
* `POST /api/telemetry/ingest` - Node.js ingestion gateway for incoming ESP32 packets

### D. Vendor Workshop Operations (`/api/vendor`)
* `POST /api/vendor/register-device` - Onboard motorcycle and bind hardware IMEI to customer
* `GET /api/vendor/activation-check/:imei` - 4-point pre-handover hardware diagnostic audit
* `GET /api/vendor/inventory` - Dealership sales ledger and stock count

### E. Manufacturer & Factory Operations (`/api/manufacturer`)
* `POST /api/manufacturer/batches` - Generate batch of hardware nodes with sequential IMEIs
* `GET /api/manufacturer/devices` - Global fleet inventory across all Ugandan dealers
* `GET /api/manufacturer/system-health` - Server cluster uptime, MQTT broker status & DB pool

---

## 4. How to Run the Backend
```bash
# Navigate to the backend directory
cd backend

# Install dependencies
npm install

# Start production server
npm start

# Or start in watch/development mode
npm run dev
```
Server starts on **`http://localhost:3000`**.
