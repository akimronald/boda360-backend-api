-- ============================================================================
-- Seed Data for Kampala Boda-Boda Telemetry & Security System
-- Sample Data Centered in Kampala, Uganda (ISBAT University & Urban Hubs)
-- ============================================================================

INSERT INTO users (user_id, full_name, phone_number, email, password_hash, role) VALUES
(1, 'Akim Ronald David', '+256770123456', 'akim.david@isbat.ac.ug', '$2a$10$wT8mQ3iF1XwVdF4G7Z0eEOsZ4k5Q3vH5jJ2g7.yKk0PZ5YJqE9v1q', 'fleet_manager'),
(2, 'John Baptist Mukasa', '+256701987654', 'john.mukasa@boda.ug', '$2a$10$wT8mQ3iF1XwVdF4G7Z0eEOsZ4k5Q3vH5jJ2g7.yKk0PZ5YJqE9v1q', 'owner'),
(3, 'Sarah Nalwanga', '+256752445566', 'sarah.fleet@kampala.ug', '$2a$10$wT8mQ3iF1XwVdF4G7Z0eEOsZ4k5Q3vH5jJ2g7.yKk0PZ5YJqE9v1q', 'fleet_manager')
ON CONFLICT (email) DO NOTHING;

INSERT INTO motorcycles (motorcycle_id, user_id, plate_number, device_imei, status, model, chassis_number, pin_code) VALUES
(1, 1, 'UEV 412K', '865421045981201', 'active', 'Bajaj Boxer BM150 (Red)', 'MD2A12AY3PW004128', '1234'),
(2, 1, 'UEX 891B', '865421045981202', 'active', 'TVS Star HLX 125 (Blue)', 'MD625CK45P2008912', '1234'),
(3, 1, 'UFB 204C', '865421045981203', 'immobilized', 'Bajaj Boxer BM150 (Black)', 'MD2A12AY7RW002041', '1234'),
(4, 2, 'UFD 770E', '865421045981204', 'tamper_alert', 'Haojue Lucky 125 (Silver)', 'LC6PCJC18N0077023', '1234'),
(5, 3, 'UFA 319M', '865421045981205', 'active', 'Yamaha Crux 110 (White)', 'JY43AY009PA003195', '1234')
ON CONFLICT (device_imei) DO NOTHING;

INSERT INTO telemetry_logs (device_imei, latitude, longitude, speed, heading, battery_voltage, ignition_status, gsm_signal, timestamp) VALUES
('865421045981201', 0.325500, 32.598200, 38.4, 65.0, 12.8, TRUE, 29, NOW() - INTERVAL '10 minutes'),
('865421045981201', 0.326200, 32.599800, 42.1, 70.0, 12.7, TRUE, 28, NOW() - INTERVAL '8 minutes'),
('865421045981201', 0.326900, 32.601200, 35.0, 55.0, 12.7, TRUE, 30, NOW() - INTERVAL '5 minutes'),
('865421045981201', 0.327500, 32.602500, 0.0,  0.0,  12.6, FALSE, 31, NOW() - INTERVAL '1 minute'),
('865421045981202', 0.318000, 32.589000, 45.2, 240.0, 12.5, TRUE, 27, NOW() - INTERVAL '6 minutes'),
('865421045981202', 0.316200, 32.584100, 41.0, 245.0, 12.5, TRUE, 28, NOW() - INTERVAL '3 minutes'),
('865421045981202', 0.314500, 32.580200, 28.5, 250.0, 12.4, TRUE, 29, NOW() - INTERVAL '30 seconds'),
('865421045981203', 0.334200, 32.569100, 0.0, 0.0, 11.9, FALSE, 26, NOW() - INTERVAL '2 minutes'),
('865421045981204', 0.335500, 32.592500, 0.0, 0.0, 12.1, FALSE, 30, NOW() - INTERVAL '1 minute'),
('865421045981205', 0.301500, 32.571000, 52.0, 180.0, 12.8, TRUE, 25, NOW() - INTERVAL '40 seconds');

INSERT INTO security_events (device_imei, event_type, severity, latitude, longitude, notes, is_acknowledged, triggered_at) VALUES
('865421045981204', 'TAMPER_DETECTED', 'CRITICAL', 0.335500, 32.592500, 'Onboard MPU6050 detected violent vibration exceeding 3.2G while ignition was OFF. Potential theft or tampering attempt.', FALSE, NOW() - INTERVAL '3 minutes'),
('865421045981203', 'GEOFENCE_BREACH', 'HIGH', 0.334200, 32.569100, 'Motorcycle crossed the Wandegeya Boda Perimeter boundary after 11:00 PM curfew. Engine cut initiated.', TRUE, NOW() - INTERVAL '45 minutes'),
('865421045981201', 'LOW_BATTERY', 'MEDIUM', 0.327500, 32.602500, 'Vehicle main battery disconnected; ESP32 operating on backup 3.7V Li-ion battery (3.55V).', TRUE, NOW() - INTERVAL '3 hours'),
('865421045981202', 'UNAUTHORIZED_MOVEMENT', 'HIGH', 0.318000, 32.589000, 'Speed of 15 km/h detected while ignition is switched off (Bike being towed or pushed).', TRUE, NOW() - INTERVAL '6 hours');

INSERT INTO command_logs (device_imei, command_action, command_status, payload_hash, issued_by, issued_at, acknowledged_at, latency_ms) VALUES
('865421045981203', 'ENGINE_CUT', 'EXECUTED', 'a7f92b4c19d83e74', 1, NOW() - INTERVAL '44 minutes', NOW() - INTERVAL '44 minutes' + INTERVAL '320 milliseconds', 320),
('865421045981201', 'LOCATE_PING', 'EXECUTED', 'c3b129840fe431ab', 1, NOW() - INTERVAL '2 hours', NOW() - INTERVAL '2 hours' + INTERVAL '210 milliseconds', 210);

INSERT INTO geofences (geofence_id, motorcycle_id, name, center_lat, center_lng, radius_meters, is_active) VALUES
(1, 1, 'ISBAT University Campus Safe Zone', 0.327500, 32.602500, 600, TRUE),
(2, 2, 'Kampala CBD Commercial Zone', 0.313800, 32.578500, 1200, TRUE),
(3, 3, 'Wandegeya & Makerere Safe Perimeter', 0.334200, 32.569100, 800, TRUE);
