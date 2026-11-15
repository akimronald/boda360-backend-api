# 🏍️ IoT-Based Real-Time Tracking and Remote Immobilization Security System for Motorcycles
## Role-Based Portals: Customer and Manufacturer

**Target Users:** Commercial *boda-boda* operators, private owners, and system administrators (Kampala, Uganda)  
**Author / Lead Engineer:** Akim Ronald David (ISBAT University)  
**Architecture:** Multi-Tenant Role-Based Telematics Cloud Platform  

---

## 📋 System Architecture: Two-Portal Ecosystem

The system connects two key operational personas through a unified PostgreSQL / In-Memory database and MQTT telematics cloud backend:

```
+─────────────────────────────────────────────────────────────────────────────────────────────+
|                                    1. MANUFACTURER PORTAL                                   |
|                      (System Administration, z-tech, Lead Engineers)                        |
|   • Hardware Batch Provisioning (Pre-load ESP32 IMEIs into `devices` table)                 |
|   • Global System Health Monitor (Active vs Offline, MQTT Broker 99.98%, PostgreSQL Pool)   |
|   • Global Fleet Inventory & OTA Firmware Management                                        |
+──────────────────────────────────────────────┬──────────────────────────────────────────────+
                                               │ Provisions hardware inventory
                                               ▼
+─────────────────────────────────────────────────────────────────────────────────────────────+
|                                      2. CUSTOMER PORTAL                                     |
|                 (Commercial Boda-Boda Operators, Private Owners, Fleets)                    |
|   • Live Dashboard Map View (Leaflet + OpenStreetMap centered in Kampala, Uganda)           |
|   • Multi-Bike Profile Management (Add & manage multiple motorcycles per account)           |
|   • Remote Security Controls ("CUT ENGINE / IMMOBILIZE" & "RESTORE IGNITION" + PIN check)   |
|   • Alert Notification Center (Instant MEMS tamper shocks, geofence breaches, sound siren)  |
+─────────────────────────────────────────────────────────────────────────────────────────────+
```

---

## 💾 Relational Database Integration for the Three Portals

The PostgreSQL relational database links the workflow seamlessly across all three portals:

1. **`devices` Table (Manufacturer Provisioning):**
   * Pre-registered batch inventory created by manufacturers before physical shipment.
   * `device_imei`, `serial_number`, `batch_id`, `hardware_version`, `status` (`manufactured`, `allocated_to_vendor`, `activated_to_customer`).

2. **`vendors` Table (Dealer Management):**
   * Stores approved dealer franchises across Kampala (e.g., Katwe Auto Electronics, Kisekka Market Solutions).
   * `vendor_id`, `business_name`, `shop_location`, `allocated_units`, `sold_units`.

3. **`motorcycles` Table (Customer Binding):**
   * Created when a Vendor sells and binds an in-stock device to a Customer.
   * `plate_number`, `device_imei` (FK referencing `devices`), `user_id` (FK referencing `users`), `vendor_id`, `pin_code`, `subscription_status`.

4. **`telemetry_logs` Table (Time-Series Data):**
   * High-frequency GPS telemetry (`latitude`, `longitude`, `speed`, `heading`, `battery_voltage`, `ignition_status`, `gsm_signal`).

5. **`security_events` Table (Tamper & Breach Logs):**
   * Accelerometer shock events, boundary violations, low battery alerts.

6. **`command_logs` Table (Audit Trail):**
   * Cryptographic remote cut-off commands dispatched, status, and node roundtrip latency in ms.

---

## 🚀 The Three Portals in Action

### Portal 1: Customer Portal (Bike Owners)
* **Target Users:** Boda-boda operators and private motorcycle owners.
* **Dashboard Map View:** High-resolution OpenStreetMap display with moving motorcycle markers, animated heading angles, digital speedometer, 12V battery gauge, and ignition indicator.
* **Multi-Bike Profile Management:** Registered users can add multiple motorcycles and hardware IMEIs directly to their profile.
* **Remote Security Controls:** Prominent **"CUT ENGINE"** emergency button requiring a 4-digit PIN (`1234`), 4-stage pipeline visualizer (`Dispatched` -> `MQTT Pub` -> `ESP32 Node` -> `Relay ACK`), and mechanical relay audio snap.
* **Alert Center:** Notification cards for tamper vibrations and geofence exits with one-click acknowledgment.
* **My Profile:** Displays active 1-year prepaid subscription status, assigned IMEI, and motorcycle registration plate.

### Portal 2: Manufacturer Portal (System Admin & Oversight)
* **Target Users:** Lead Engineer / Super-Administrator (Akim Ronald David / z-tech).
* **Hardware Batch Provisioning:** Form to create new factory production batches (e.g. `BATCH-2026-UG03`), generate sequential IMEIs and serial numbers, and pre-register them in the cloud database.
* **Global System Health Monitor:** Real-time KPI counters of total manufactured units, active on-road bikes, MQTT broker uptime (99.98%), PostgreSQL connection pool status, and a **Live MQTT Ingest Console** inspecting incoming JSON packets.

---

## 🛠️ Technology Stack

| Layer | Technology |
| :--- | :--- |
| **Backend Runtime** | Node.js with Express.js |
| **Database** | PostgreSQL 14+ (with automatic resilient in-memory relational engine) |
| **IoT Broker Protocol** | MQTT 3.1.1 (Pub/Sub on port 1883) + WebSockets / SSE |
| **Geospatial Mapping** | Leaflet API + OpenStreetMap (OSM) centered on Kampala, Uganda |
| **Audio Synthesis** | Web Audio API (tactile relay clicks, security sirens) |
| **Hardware Node** | ESP32-WROOM-32 + SIM800L GPRS + u-blox NEO-6M GPS + MPU6050 Accelerometer + 5V Relay |

---

## 📂 Four-Module Project Architecture

The project is structured into four main, self-contained modules corresponding to their different organizations and field deployment environments, alongside the embedded firmware:

```text
Project/
├── 📁 customer-portal/       # 1. FRONTEND FOR CUSTOMERS (Bike Owners / Boda Riders)
│   ├── index.html            # Standalone Live GPS Map, Telemetry Gauges & Controls
│   ├── css/styles.css        # Avionics dark telematics styling
│   ├── js/app.js             # Customer state, Leaflet GIS, multi-bike & cut-off controls
│   ├── js/audioService.js    # Procedural relay clicks & tamper alarm siren
│   └── README.md             # Field instructions for bike owners
│
├── 📁 manufacturer-portal/   # 2. MANUFACTURER PORTAL (ISBAT R&D Lab & Cloud Superadmins)
│   ├── index.html            # Batch Provisioning & Health Oversight Command
│   ├── css/styles.css        # Emerald infrastructure layout & console styling
│   ├── js/app.js             # Batch generation, global fleet & MQTT stream inspector
│   └── README.md             # Factory assembly & administration manual
│
├── 📁 backend/               # 3. CLOUD BACKEND & API ENGINE (Node.js & Express)
│   ├── server.js             # Express & WebSocket streaming server
│   ├── package.json          # Backend dependencies
│   ├── .env                  # Configuration & database credentials
│   ├── config/db.js          # PostgreSQL / In-Memory relational engine
│   ├── middleware/auth.js    # JWT verification & RBAC
│   ├── routes/               # REST API endpoints (auth, motorcycles, telemetry, etc.)
│   ├── services/             # MQTT command pipeline, geofence & simulator engines
│   ├── db/                   # PostgreSQL schema.sql and seed.sql
│   └── README.md             # Backend architecture documentation
│
└── 📁 firmware/              # EMBEDDED NODE FIRMWARE
    └── esp32_boda_node.ino   # Arduino ESP32 C++ sketch (SIM800L, NEO-6M, Relay)
```

---

## 💻 How to Run Each Portal & The Backend

### Standalone Field Deployment (Direct in Browser)
Each frontend runs 100% independently with no external server required:
* **Customer Portal:** [`customer-portal/index.html`](file:///f:/project%20and%20assignent/Project/customer-portal/index.html)
* **Manufacturer Portal:** [`manufacturer-portal/index.html`](file:///f:/project%20and%20assignent/Project/manufacturer-portal/index.html)

### Running the Cloud Backend
```bash
# Option 1: Start directly from root
npm start

# Option 2: Run from the backend directory
cd backend
npm install
npm start
```
When the backend is active, all frontends can also be accessed through the server:
* **Customer Portal:** `http://localhost:3000/customer`
* **Manufacturer Portal:** `http://localhost:3000/manufacturer`
* **Interactive Multi-Device Studio:** `http://localhost:3000/device-preview.html` (Test portals on Phone, Tablet & Desktop)
* **REST API Status:** `http://localhost:3000/api/system/status`

---

## 🧪 Demonstration & Evaluation Quick Script

1. **Customer Experience (`customer-portal/`):**
   * Open `customer-portal/index.html`. Observe motorcycle **UEV 412K** moving along Kampala streets.
   * Click **"CUT ENGINE"**, enter PIN `1234`. Observe the 3-step pipeline, hear the relay snap, and see the speed drop to 0 km/h with ignition switched OFF.
2. **Vendor Onboarding Experience (`vendor-portal/`):**
   * Open `vendor-portal/index.html`.
   * Fill out the **Device Registration Terminal** to onboard a new bike (e.g. Plate `UFE 992P`, Customer `Emmanuel Otim`). Click **"Complete Registration & Bind"**.
   * Switch to **Pre-Handover Diagnostic Check** and click **"Run 4-Point Diagnostic Audit"** to verify GPRS, GPS, battery, and relay continuity.
3. **Manufacturer Oversight Experience (`manufacturer-portal/`):**
   * Open `manufacturer-portal/index.html`.
   * In **Hardware Batch Provisioning**, generate a new batch of 10 or 25 units.
   * In **System Health Monitor**, inspect the live MQTT broker metrics, latency, and live incoming telemetry console stream!
