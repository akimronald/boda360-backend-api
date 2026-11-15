-- ============================================================================
-- IoT-Based Real-Time Tracking and Remote Immobilization Security System
-- Target Deployment: Kampala, Uganda (Boda-Boda Security Platform)
-- Architecture: Three-Portal System (Customer, Vendor, Manufacturer)
-- Lead Engineer / Author: Akim Ronald David (ISBAT University)
-- Database: PostgreSQL 14+ Schema Specification
-- ============================================================================

DROP TABLE IF EXISTS command_logs CASCADE;
DROP TABLE IF EXISTS security_events CASCADE;
DROP TABLE IF EXISTS telemetry_logs CASCADE;
DROP TABLE IF EXISTS geofences CASCADE;
DROP TABLE IF EXISTS motorcycles CASCADE;
DROP TABLE IF EXISTS devices CASCADE;
DROP TABLE IF EXISTS vendors CASCADE;
DROP TABLE IF EXISTS users CASCADE;

-- ----------------------------------------------------------------------------
-- 1. USERS TABLE
-- Multi-tenant accounts for Customers, Vendors/Dealers, and Manufacturers
-- ----------------------------------------------------------------------------
CREATE TABLE users (
    user_id SERIAL PRIMARY KEY,
    full_name VARCHAR(150) NOT NULL,
    phone_number VARCHAR(30) UNIQUE NOT NULL,
    email VARCHAR(150) UNIQUE NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    role VARCHAR(50) DEFAULT 'customer' CHECK (role IN ('customer', 'vendor', 'manufacturer', 'owner', 'fleet_manager', 'admin')),
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_users_email ON users(email);
CREATE INDEX idx_users_phone ON users(phone_number);
CREATE INDEX idx_users_role ON users(role);

-- ----------------------------------------------------------------------------
-- 2. VENDORS TABLE
-- Authorized dealers, sales agents, and auto-electrician workshops in Kampala
-- ----------------------------------------------------------------------------
CREATE TABLE vendors (
    vendor_id SERIAL PRIMARY KEY,
    user_id INTEGER REFERENCES users(user_id) ON DELETE CASCADE,
    business_name VARCHAR(150) NOT NULL,
    tin_number VARCHAR(50),
    shop_location VARCHAR(200) NOT NULL, -- e.g. 'Katwe Auto Spare Market, Kampala'
    contact_phone VARCHAR(30) NOT NULL,
    status VARCHAR(30) DEFAULT 'approved' CHECK (status IN ('approved', 'pending', 'suspended')),
    allocated_units INTEGER DEFAULT 0,
    sold_units INTEGER DEFAULT 0,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_vendors_user ON vendors(user_id);

-- ----------------------------------------------------------------------------
-- 3. DEVICES TABLE (Hardware Inventory provisioned by Manufacturer)
-- Pre-registered production batches before shipping to vendors
-- ----------------------------------------------------------------------------
CREATE TABLE devices (
    device_id SERIAL PRIMARY KEY,
    device_imei VARCHAR(20) UNIQUE NOT NULL,
    serial_number VARCHAR(50) UNIQUE NOT NULL,
    batch_id VARCHAR(50) NOT NULL, -- e.g. 'BATCH-2026-UG01'
    hardware_version VARCHAR(50) DEFAULT 'ESP32-SIM800L-v2.1',
    firmware_version VARCHAR(50) DEFAULT 'v1.4.2-EAT',
    vendor_id INTEGER REFERENCES vendors(vendor_id) ON DELETE SET NULL,
    status VARCHAR(30) DEFAULT 'manufactured' CHECK (status IN ('manufactured', 'allocated_to_vendor', 'activated_to_customer', 'faulty')),
    qa_passed BOOLEAN DEFAULT TRUE,
    manufactured_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    allocated_at TIMESTAMP WITH TIME ZONE,
    activated_at TIMESTAMP WITH TIME ZONE
);

CREATE INDEX idx_devices_imei ON devices(device_imei);
CREATE INDEX idx_devices_batch ON devices(batch_id);
CREATE INDEX idx_devices_vendor ON devices(vendor_id);
CREATE INDEX idx_devices_status ON devices(status);

-- ----------------------------------------------------------------------------
-- 4. MOTORCYCLES TABLE
-- Registered bikes linked to customer profile, vendor seller, and hardware IMEI
-- ----------------------------------------------------------------------------
CREATE TABLE motorcycles (
    motorcycle_id SERIAL PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    vendor_id INTEGER REFERENCES vendors(vendor_id) ON DELETE SET NULL,
    plate_number VARCHAR(20) UNIQUE NOT NULL,
    device_imei VARCHAR(20) UNIQUE NOT NULL REFERENCES devices(device_imei) ON DELETE RESTRICT,
    status VARCHAR(30) DEFAULT 'active' CHECK (status IN ('active', 'immobilized', 'offline', 'tamper_alert')),
    model VARCHAR(100) DEFAULT 'Bajaj Boxer BM150',
    chassis_number VARCHAR(50),
    pin_code VARCHAR(10) DEFAULT '1234',
    subscription_status VARCHAR(30) DEFAULT 'active' CHECK (subscription_status IN ('active', 'expired', 'trial')),
    subscription_expiry TIMESTAMP WITH TIME ZONE,
    installed_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_motorcycles_imei ON motorcycles(device_imei);
CREATE INDEX idx_motorcycles_user ON motorcycles(user_id);
CREATE INDEX idx_motorcycles_vendor ON motorcycles(vendor_id);
CREATE INDEX idx_motorcycles_plate ON motorcycles(plate_number);

-- ----------------------------------------------------------------------------
-- 5. TELEMETRY_LOGS TABLE (Time-Series Data)
-- ----------------------------------------------------------------------------
CREATE TABLE telemetry_logs (
    log_id BIGSERIAL PRIMARY KEY,
    device_imei VARCHAR(20) NOT NULL REFERENCES devices(device_imei) ON DELETE CASCADE,
    latitude DECIMAL(10, 7) NOT NULL,
    longitude DECIMAL(10, 7) NOT NULL,
    speed DECIMAL(5, 2) DEFAULT 0.00,
    heading DECIMAL(5, 2) DEFAULT 0.00,
    battery_voltage DECIMAL(4, 2) NOT NULL,
    ignition_status BOOLEAN NOT NULL DEFAULT FALSE,
    gsm_signal INTEGER DEFAULT 28,
    timestamp TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_telemetry_imei_time ON telemetry_logs(device_imei, timestamp DESC);
CREATE INDEX idx_telemetry_time ON telemetry_logs(timestamp DESC);

-- ----------------------------------------------------------------------------
-- 6. SECURITY_EVENTS TABLE
-- ----------------------------------------------------------------------------
CREATE TABLE security_events (
    event_id SERIAL PRIMARY KEY,
    device_imei VARCHAR(20) NOT NULL REFERENCES devices(device_imei) ON DELETE CASCADE,
    event_type VARCHAR(50) NOT NULL CHECK (event_type IN (
        'TAMPER_DETECTED',
        'GEOFENCE_BREACH',
        'UNAUTHORIZED_MOVEMENT',
        'LOW_BATTERY',
        'IGNITION_TAMPER',
        'CRASH_DETECTED'
    )),
    severity VARCHAR(20) DEFAULT 'HIGH' CHECK (severity IN ('LOW', 'MEDIUM', 'HIGH', 'CRITICAL')),
    latitude DECIMAL(10, 7) NOT NULL,
    longitude DECIMAL(10, 7) NOT NULL,
    notes TEXT,
    is_acknowledged BOOLEAN DEFAULT FALSE,
    triggered_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_security_imei ON security_events(device_imei);
CREATE INDEX idx_security_time ON security_events(triggered_at DESC);

-- ----------------------------------------------------------------------------
-- 7. COMMAND_LOGS TABLE
-- ----------------------------------------------------------------------------
CREATE TABLE command_logs (
    command_id SERIAL PRIMARY KEY,
    device_imei VARCHAR(20) NOT NULL REFERENCES devices(device_imei) ON DELETE CASCADE,
    command_action VARCHAR(50) NOT NULL CHECK (command_action IN ('ENGINE_CUT', 'ENGINE_RESTORE', 'LOCATE_PING', 'SIREN_ON')),
    command_status VARCHAR(30) DEFAULT 'PENDING' CHECK (command_status IN ('PENDING', 'SENT', 'EXECUTED', 'FAILED', 'TIMED_OUT')),
    payload_hash VARCHAR(64),
    issued_by INTEGER REFERENCES users(user_id),
    issued_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    acknowledged_at TIMESTAMP WITH TIME ZONE,
    latency_ms INTEGER
);

CREATE INDEX idx_command_imei ON command_logs(device_imei);

-- ----------------------------------------------------------------------------
-- 8. GEOFENCES TABLE
-- ----------------------------------------------------------------------------
CREATE TABLE geofences (
    geofence_id SERIAL PRIMARY KEY,
    motorcycle_id INTEGER REFERENCES motorcycles(motorcycle_id) ON DELETE CASCADE,
    name VARCHAR(100) NOT NULL,
    center_lat DECIMAL(10, 7) NOT NULL,
    center_lng DECIMAL(10, 7) NOT NULL,
    radius_meters INTEGER NOT NULL DEFAULT 500,
    is_active BOOLEAN DEFAULT TRUE,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);
