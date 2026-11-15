/**
 * Database Adaptor Layer for Three-Portal Architecture:
 * 1. Customer Portal (Bike Owners)
 * 2. Vendor Portal (Dealers / Sales Agents)
 * 3. Manufacturer Portal (System Administration & Hardware Provisioning)
 *
 * Lead Engineer: Akim Ronald David (ISBAT University)
 */

const { Pool } = require('pg');
require('dotenv').config();

let pgPool = null;
let usePostgres = false;

// Relational In-Memory Store
const inMemoryStore = {
  users: [
    {
      user_id: 1,
      full_name: 'System Superadmin',
      phone_number: '+256770123456',
      email: 'admin@boda360.io',
      password_hash: '$2a$10$wT8mQ3iF1XwVdF4G7Z0eEOsZ4k5Q3vH5jJ2g7.yKk0PZ5YJqE9v1q', // 'password123'
      role: 'manufacturer',
      created_at: new Date('2026-01-01T08:00:00Z')
    },
    {
      user_id: 2,
      full_name: 'Sarah Nalwanga (Katwe Dealer)',
      phone_number: '+256752445566',
      email: 'sarah.vendor@katwe-boda.ug',
      password_hash: '$2a$10$wT8mQ3iF1XwVdF4G7Z0eEOsZ4k5Q3vH5jJ2g7.yKk0PZ5YJqE9v1q',
      role: 'vendor',
      created_at: new Date('2026-01-10T09:15:00Z')
    },
    {
      user_id: 3,
      full_name: 'John Baptist Mukasa',
      phone_number: '+256701987654',
      email: 'john.mukasa@boda.ug',
      password_hash: '$2a$10$wT8mQ3iF1XwVdF4G7Z0eEOsZ4k5Q3vH5jJ2g7.yKk0PZ5YJqE9v1q',
      role: 'customer',
      created_at: new Date('2026-01-15T10:30:00Z')
    },
    {
      user_id: 4,
      full_name: 'Denis Kato (Kisekka Dealer)',
      phone_number: '+256782112233',
      email: 'denis@kisekka-electronics.ug',
      password_hash: '$2a$10$wT8mQ3iF1XwVdF4G7Z0eEOsZ4k5Q3vH5jJ2g7.yKk0PZ5YJqE9v1q',
      role: 'vendor',
      created_at: new Date('2026-01-20T11:00:00Z')
    },
    {
      user_id: 5,
      full_name: 'Emmanuel Otim',
      phone_number: '+256775998877',
      email: 'emmanuel.otim@gmail.com',
      password_hash: '$2a$10$wT8mQ3iF1XwVdF4G7Z0eEOsZ4k5Q3vH5jJ2g7.yKk0PZ5YJqE9v1q',
      role: 'customer',
      created_at: new Date('2026-02-01T14:00:00Z')
    }
  ],

  vendors: [
    {
      vendor_id: 1,
      user_id: 2,
      business_name: 'Katwe Auto Electronics Ltd',
      tin_number: '1004928172',
      shop_location: 'Katwe Auto Spare Market, Ring Road, Kampala',
      contact_phone: '+256752445566',
      status: 'approved',
      allocated_units: 15,
      sold_units: 4,
      created_at: new Date('2026-01-10T09:30:00Z')
    },
    {
      vendor_id: 2,
      user_id: 4,
      business_name: 'Kisekka Market Boda Solutions',
      tin_number: '1005829104',
      shop_location: 'Kisekka Market Block B-12, Kampala',
      contact_phone: '+256782112233',
      status: 'approved',
      allocated_units: 10,
      sold_units: 1,
      created_at: new Date('2026-01-20T11:15:00Z')
    },
    {
      vendor_id: 3,
      user_id: null,
      business_name: 'Lugogo Express Auto Care',
      tin_number: '1006192834',
      shop_location: 'Lugogo Bypass, Shell Petrol Station, Kampala',
      contact_phone: '+256700554433',
      status: 'pending',
      allocated_units: 0,
      sold_units: 0,
      created_at: new Date('2026-02-15T08:00:00Z')
    }
  ],

  batches: [
    {
      batch_id: 'BATCH-2026-UG01',
      production_date: new Date('2026-01-05T00:00:00Z'),
      hardware_version: 'ESP32-SIM800L-v2.1',
      firmware_version: 'v1.4.2-EAT',
      total_units: 25,
      qa_status: 'Passed (100%)',
      notes: 'First commercial batch for Kampala urban test rollout.'
    },
    {
      batch_id: 'BATCH-2026-UG02',
      production_date: new Date('2026-02-10T00:00:00Z'),
      hardware_version: 'ESP32-SIM800L-v2.1',
      firmware_version: 'v1.4.2-EAT',
      total_units: 50,
      qa_status: 'Passed (100%)',
      notes: 'Second production run. High-temperature ceramic capacitors installed.'
    }
  ],

  devices: [
    {
      device_id: 1,
      device_imei: '865421045981201',
      serial_number: 'SN-UG26-001',
      batch_id: 'BATCH-2026-UG01',
      hardware_version: 'ESP32-SIM800L-v2.1',
      firmware_version: 'v1.4.2-EAT',
      vendor_id: 1,
      status: 'activated_to_customer',
      qa_passed: true,
      manufactured_at: new Date('2026-01-05T00:00:00Z'),
      allocated_at: new Date('2026-01-10T00:00:00Z'),
      activated_at: new Date('2026-01-15T10:30:00Z')
    },
    {
      device_id: 2,
      device_imei: '865421045981202',
      serial_number: 'SN-UG26-002',
      batch_id: 'BATCH-2026-UG01',
      hardware_version: 'ESP32-SIM800L-v2.1',
      firmware_version: 'v1.4.2-EAT',
      vendor_id: 1,
      status: 'activated_to_customer',
      qa_passed: true,
      manufactured_at: new Date('2026-01-05T00:00:00Z'),
      allocated_at: new Date('2026-01-10T00:00:00Z'),
      activated_at: new Date('2026-01-18T14:20:00Z')
    },
    {
      device_id: 3,
      device_imei: '865421045981203',
      serial_number: 'SN-UG26-003',
      batch_id: 'BATCH-2026-UG01',
      hardware_version: 'ESP32-SIM800L-v2.1',
      firmware_version: 'v1.4.2-EAT',
      vendor_id: 1,
      status: 'activated_to_customer',
      qa_passed: true,
      manufactured_at: new Date('2026-01-05T00:00:00Z'),
      allocated_at: new Date('2026-01-10T00:00:00Z'),
      activated_at: new Date('2026-01-22T09:10:00Z')
    },
    {
      device_id: 4,
      device_imei: '865421045981204',
      serial_number: 'SN-UG26-004',
      batch_id: 'BATCH-2026-UG01',
      hardware_version: 'ESP32-SIM800L-v2.1',
      firmware_version: 'v1.4.2-EAT',
      vendor_id: 1,
      status: 'activated_to_customer',
      qa_passed: true,
      manufactured_at: new Date('2026-01-05T00:00:00Z'),
      allocated_at: new Date('2026-01-10T00:00:00Z'),
      activated_at: new Date('2026-01-25T11:45:00Z')
    },
    {
      device_id: 5,
      device_imei: '865421045981205',
      serial_number: 'SN-UG26-005',
      batch_id: 'BATCH-2026-UG01',
      hardware_version: 'ESP32-SIM800L-v2.1',
      firmware_version: 'v1.4.2-EAT',
      vendor_id: 2,
      status: 'activated_to_customer',
      qa_passed: true,
      manufactured_at: new Date('2026-01-05T00:00:00Z'),
      allocated_at: new Date('2026-01-20T00:00:00Z'),
      activated_at: new Date('2026-02-01T15:00:00Z')
    },
    // In-Stock at Vendor 1 (Katwe) ready to be sold to a customer!
    {
      device_id: 6,
      device_imei: '865421045981206',
      serial_number: 'SN-UG26-006',
      batch_id: 'BATCH-2026-UG02',
      hardware_version: 'ESP32-SIM800L-v2.1',
      firmware_version: 'v1.4.2-EAT',
      vendor_id: 1,
      status: 'allocated_to_vendor',
      qa_passed: true,
      manufactured_at: new Date('2026-02-10T00:00:00Z'),
      allocated_at: new Date('2026-02-15T00:00:00Z'),
      activated_at: null
    },
    {
      device_id: 7,
      device_imei: '865421045981207',
      serial_number: 'SN-UG26-007',
      batch_id: 'BATCH-2026-UG02',
      hardware_version: 'ESP32-SIM800L-v2.1',
      firmware_version: 'v1.4.2-EAT',
      vendor_id: 1,
      status: 'allocated_to_vendor',
      qa_passed: true,
      manufactured_at: new Date('2026-02-10T00:00:00Z'),
      allocated_at: new Date('2026-02-15T00:00:00Z'),
      activated_at: null
    },
    // In-Stock at Vendor 2 (Kisekka)
    {
      device_id: 8,
      device_imei: '865421045981208',
      serial_number: 'SN-UG26-008',
      batch_id: 'BATCH-2026-UG02',
      hardware_version: 'ESP32-SIM800L-v2.1',
      firmware_version: 'v1.4.2-EAT',
      vendor_id: 2,
      status: 'allocated_to_vendor',
      qa_passed: true,
      manufactured_at: new Date('2026-02-10T00:00:00Z'),
      allocated_at: new Date('2026-02-16T00:00:00Z'),
      activated_at: null
    },
    // At Factory (Manufacturer warehouse) unallocated
    {
      device_id: 9,
      device_imei: '865421045981209',
      serial_number: 'SN-UG26-009',
      batch_id: 'BATCH-2026-UG02',
      hardware_version: 'ESP32-SIM800L-v2.1',
      firmware_version: 'v1.4.2-EAT',
      vendor_id: null,
      status: 'manufactured',
      qa_passed: true,
      manufactured_at: new Date('2026-02-10T00:00:00Z'),
      allocated_at: null,
      activated_at: null
    },
    {
      device_id: 10,
      device_imei: '865421045981210',
      serial_number: 'SN-UG26-010',
      batch_id: 'BATCH-2026-UG02',
      hardware_version: 'ESP32-SIM800L-v2.1',
      firmware_version: 'v1.4.2-EAT',
      vendor_id: null,
      status: 'manufactured',
      qa_passed: true,
      manufactured_at: new Date('2026-02-10T00:00:00Z'),
      allocated_at: null,
      activated_at: null
    }
  ],

  motorcycles: [
    {
      motorcycle_id: 1,
      user_id: 3, // John Mukasa
      vendor_id: 1, // Katwe Auto Electronics
      plate_number: 'UEV 412K',
      device_imei: '865421045981201',
      status: 'active',
      model: 'Bajaj Boxer BM150 (ESP32 Hardware Node)',
      chassis_number: 'MD2A12AY3PW004128',
      sim_number: '+256 701 445892 (MTN Uganda)',
      pin_code: '1234',
      subscription_status: 'active',
      subscription_expiry: new Date('2026-12-31T23:59:59Z'),
      installed_at: new Date('2026-01-15T10:30:00Z')
    }
  ],

  telemetry_logs: [
    {
      log_id: 1,
      device_imei: '865421045981201',
      latitude: 0.327500,
      longitude: 32.602500,
      speed: 38.4,
      heading: 65.0,
      battery_voltage: 12.7,
      ignition_status: true,
      gsm_signal: 29,
      timestamp: new Date()
    },
    {
      log_id: 2,
      device_imei: '865421045981202',
      latitude: 0.314500,
      longitude: 32.580200,
      speed: 28.5,
      heading: 250.0,
      battery_voltage: 12.4,
      ignition_status: true,
      gsm_signal: 29,
      timestamp: new Date()
    },
    {
      log_id: 3,
      device_imei: '865421045981203',
      latitude: 0.334200,
      longitude: 32.569100,
      speed: 0.0,
      heading: 0.0,
      battery_voltage: 11.9,
      ignition_status: false,
      gsm_signal: 26,
      timestamp: new Date()
    },
    {
      log_id: 4,
      device_imei: '865421045981204',
      latitude: 0.335500,
      longitude: 32.592500,
      speed: 0.0,
      heading: 0.0,
      battery_voltage: 12.1,
      ignition_status: false,
      gsm_signal: 30,
      timestamp: new Date()
    },
    {
      log_id: 5,
      device_imei: '865421045981205',
      latitude: 0.301500,
      longitude: 32.571000,
      speed: 52.0,
      heading: 180.0,
      battery_voltage: 12.8,
      ignition_status: true,
      gsm_signal: 25,
      timestamp: new Date()
    }
  ],

  security_events: [
    {
      event_id: 1,
      device_imei: '865421045981204',
      event_type: 'TAMPER_DETECTED',
      severity: 'CRITICAL',
      latitude: 0.335500,
      longitude: 32.592500,
      notes: 'Onboard MPU6050 detected violent vibration exceeding 3.5G while parked. Potential theft attempt.',
      is_acknowledged: false,
      triggered_at: new Date(Date.now() - 3 * 60 * 1000)
    },
    {
      event_id: 2,
      device_imei: '865421045981203',
      event_type: 'GEOFENCE_BREACH',
      severity: 'HIGH',
      latitude: 0.334200,
      longitude: 32.569100,
      notes: 'Motorcycle crossed Wandegeya safe boundary after 11:00 PM curfew. Remote engine cut initiated.',
      is_acknowledged: true,
      triggered_at: new Date(Date.now() - 45 * 60 * 1000)
    }
  ],

  command_logs: [
    {
      command_id: 1,
      device_imei: '865421045981203',
      command_action: 'ENGINE_CUT',
      command_status: 'EXECUTED',
      payload_hash: 'a7f92b4c19d83e74',
      issued_by: 1,
      issued_at: new Date(Date.now() - 44 * 60 * 1000),
      acknowledged_at: new Date(Date.now() - 44 * 60 * 1000 + 320),
      latency_ms: 320
    }
  ],

  geofences: [
    {
      geofence_id: 1,
      motorcycle_id: 1,
      name: 'Kampala Central Campus Safe Zone',
      center_lat: 0.327500,
      center_lng: 32.602500,
      radius_meters: 600,
      is_active: true
    },
    {
      geofence_id: 2,
      motorcycle_id: 2,
      name: 'Kampala CBD Commercial Zone',
      center_lat: 0.313800,
      center_lng: 32.578500,
      radius_meters: 1200,
      is_active: true
    }
  ]
};

const profileStore = require('./profileStore');
profileStore.hydrateStore(inMemoryStore);

async function initDatabase() {
  if (process.env.DATABASE_URL || (process.env.DB_HOST && process.env.DB_NAME)) {
    try {
      const config = process.env.DATABASE_URL
        ? { connectionString: process.env.DATABASE_URL }
        : {
            host: process.env.DB_HOST || 'localhost',
            port: process.env.DB_PORT || 5432,
            user: process.env.DB_USER || 'postgres',
            password: process.env.DB_PASSWORD || 'postgres',
            database: process.env.DB_NAME || 'boda_iot_db',
            connectionTimeoutMillis: 2000
          };

      pgPool = new Pool(config);
      const client = await pgPool.connect();
      const res = await client.query('SELECT NOW()');
      client.release();
      usePostgres = true;
      console.log('✅ [PostgreSQL] Connected successfully at:', res.rows[0].now);
    } catch (err) {
      console.warn('⚠️  [PostgreSQL] Connection unavailable (' + err.message + '). Active In-Memory Relational Engine ready.');
      usePostgres = false;
    }
  } else {
    usePostgres = false;
  }
}

async function query(text, params = []) {
  if (usePostgres && pgPool) {
    return await pgPool.query(text, params);
  }
  return { rows: [] };
}

module.exports = {
  query,
  initDatabase,
  store: inMemoryStore,
  saveProfileStore: (removedImei, restoredImei) =>
    profileStore.saveStoreSnapshot(inMemoryStore, removedImei, restoredImei),
  isPostgres: () => usePostgres
};
