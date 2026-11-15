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
    }
  ],

  vendors: [],

  batches: [],

  devices: [],

  motorcycles: [],

  telemetry_logs: [],

  security_events: [],

  command_logs: [],

  geofences: []
};

const profileStore = require('./profileStore');
profileStore.hydrateStore(inMemoryStore);

async function initDatabase() {
  if (process.env.DATABASE_URL || (process.env.DB_HOST && process.env.DB_NAME)) {
    try {
      const isLocalDb =
        process.env.DATABASE_URL &&
        (process.env.DATABASE_URL.includes('localhost') ||
          process.env.DATABASE_URL.includes('127.0.0.1'));
      const config = process.env.DATABASE_URL
        ? {
            connectionString: process.env.DATABASE_URL,
            ssl: isLocalDb ? false : { rejectUnauthorized: false }
          }
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
