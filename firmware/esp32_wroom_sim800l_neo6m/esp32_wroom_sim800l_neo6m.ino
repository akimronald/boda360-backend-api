/**
 * ============================================================================
 * BODA 360 — ESP32-WROOM-32 + SIM800L (VOICE CALL + SMS + GPRS) + NEO-6M GPS
 * Dual-Board Internet OTA Firmware Architecture (Board Profile: "esp32_wroom")
 * Author / Lead Engineer: Akim Ronald David (ISBAT University, Kampala)
 * ============================================================================
 *
 * HARDWARE WIRING SUMMARY (ESP32-WROOM-32 30-Pin / 38-Pin DevKit):
 *   1. SIM800L Quad-Band GSM/GPRS Voice Modem (UART1 @ 9600 baud):
 *      - SIM800L VCC  -> Dedicated 3.7V-4.1V (>= 2A peak! Li-Ion 18650 or LM2596 set to 4.0V + 1000uF cap)
 *                        [WARNING: NEVER connect SIM800L VCC to 5V or ESP32 3.3V!]
 *      - SIM800L GND  -> Common GND with ESP32 & Buck/Battery GND
 *      - SIM800L TXD  -> ESP32 GPIO 26 (PIN_SIM800_RX)
 *      - SIM800L RXD  <- ESP32 GPIO 27 (PIN_SIM800_TX) via 10k/20k divider (or 10k series resistor)
 *      - SIM800L RST  <- ESP32 GPIO 4  (PIN_SIM800_RST)
 *
 *   2. u-blox NEO-6M GPS Module (UART2 @ 9600 baud):
 *      - NEO-6M VCC   -> 3.3V or 5V (check module regulator)
 *      - NEO-6M GND   -> Common GND
 *      - NEO-6M TX    -> ESP32 GPIO 16 (RX2 / PIN_GPS_RX)
 *      - NEO-6M RX    <- ESP32 GPIO 17 (TX2 / PIN_GPS_TX)
 *
 *   3. Security Sensors & Immobilizer Actuators:
 *      - Immobilizer 5V Relay IN : GPIO 23 (HIGH = Cut CDI Ignition, LOW = Normal Run)
 *      - Local Siren / Buzzer    : GPIO 14 (HIGH = Siren ON)
 *      - Onboard Status LED      : GPIO 2  (Active HIGH on ESP32-WROOM DevKit)
 *      - Piezo Vibration Sensor  : GPIO 32 (ADC1_CH4 with 1M-ohm parallel bleed resistor)
 *      - Optional MPU-6050 I2C   : SDA = GPIO 21, SCL = GPIO 22 (I2C addr 0x68)
 *      - 12V Bike Battery Sense  : GPIO 34 (R1 = 100k to +12V, R2 = 22k to GND)
 *      - 3.7V Backup Cell Sense  : GPIO 35 (R1 = 100k, R2 = 100k divider)
 *
 * ARDUINO IDE FLASH & INTERNET OTA SETTINGS:
 *   - Board            : "ESP32 Dev Module"
 *   - Partition Scheme : "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)"
 *   - Export Binary    : Sketch -> Export Compiled Binary (.bin) -> Upload in Manufacturer Portal!
 * ============================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <math.h>

// ============================================================================
// BOARD IDENTITY & INTERNET OTA METADATA
// ============================================================================
const char* BOARD_TYPE            = "esp32_wroom"; // Matches Manufacturer Portal "esp32_wroom"
const char* FIRMWARE_VERSION      = "2.2.0";

// Optional Workshop / Hotspot Wi-Fi for ultra-fast 5-second OTA (falls back to SIM800L Cellular GPRS automatically!)
const char* WIFI_OTA_SSID         = "";            // Optional: e.g. "Boda360_Hotspot"
const char* WIFI_OTA_PASS         = "";            // Optional Wi-Fi password

// Factory Fallback Defaults (Overridden automatically by NVS Flash + SIM800L AT+GSN + Customer Sign-Up Sync!)
const char* DEFAULT_FALLBACK_PLATE = "UNBOUND";
const char* DEFAULT_PHONE_LOCAL    = "0748802162";    // Direct SIM800L GSM Voice Call target (ATD0748802162;)
const char* DEFAULT_PHONE_INTL     = "+256748802162"; // Direct SIM800L GSM SMS target (AT+CMGS)
const char* CELLULAR_APN           = "internet";      // Universal APN for MTN Uganda & Airtel Uganda
const char* MQTT_BROKER            = "broker.hivemq.com";
const uint16_t MQTT_PORT           = 1883;

// ============================================================================
// PIN DEFINITIONS (ESP32-WROOM-32)
// ============================================================================
#define PIN_STATUS_LED        2   // ESP32-WROOM Onboard Blue LED (Active HIGH)
#define PIN_SIM800_RST        4   // SIM800L Hardware Reset Pin (Active LOW pulse)
#define PIN_ALARM_BUZZER      14  // Local Siren / Active Buzzer
#define PIN_GPS_RX            16  // ESP32 RX2 <- NEO-6M GPS TX
#define PIN_GPS_TX            17  // ESP32 TX2 -> NEO-6M GPS RX
#define PIN_I2C_SDA           21  // Optional MPU-6050 SDA
#define PIN_I2C_SCL           22  // Optional MPU-6050 SCL
#define PIN_RELAY_IMMOBILIZER 23  // HIGH = Cut Engine Ignition
#define PIN_SIM800_RX         26  // ESP32 RX1 <- SIM800L TXD
#define PIN_SIM800_TX         27  // ESP32 TX1 -> SIM800L RXD (via resistor divider)
#define PIN_PIEZO_VIBRATION   32  // Analog Piezo Shock Sensor (ADC1_CH4)
#define PIN_MAIN_BATT_ADC     34  // 12V Motorcycle Battery Voltage Divider
#define PIN_BACKUP_BATT_ADC   35  // 3.7V Backup Li-Ion Voltage Divider

#define MPU6050_I2C_ADDR      0x68

// ============================================================================
// HARDWARE SERIAL INSTANCES & STATE
// ============================================================================
HardwareSerial SerialSIM800(1); // UART1: SIM800L Voice + SMS + GPRS
HardwareSerial SerialGPS(2);    // UART2: u-blox NEO-6M GPS (9600 baud)
WiFiClient     wifiMqttSock;
Preferences    nvs;             // Permanent Non-Volatile Storage (survives reboots & Internet OTA!)

// Dynamic Per-Board Identity & Synced Customer Profile (Stored in NVS + Auto AT+GSN / ESP32 eFuse MAC)
String chipFallbackImei = "";
String hardwareImei     = "";
String deviceImei       = "";
bool   imeiCustomLocked = false; // true only if SET_IMEI:<id> was explicitly locked into NVS
String bikePlate        = DEFAULT_FALLBACK_PLATE;
String alertPhoneLocal  = DEFAULT_PHONE_LOCAL;
String alertPhoneIntl   = DEFAULT_PHONE_INTL;
String ownerEmail       = "";

String TOPIC_TELEMETRY;
String TOPIC_COMMAND;
String TOPIC_COMMAND_HW;
String TOPIC_ACK;
String TOPIC_TAMPER;
const char* TOPIC_OTA_BROADCAST = "boda/ota/broadcast";

bool sim800Ready          = false;
bool gprsTcpConnected     = false;
bool mqttConnected        = false;
bool usingWifiMqtt        = false;
bool mpuAvailable         = false;
bool isImmobilized        = false;
bool mainPowerCutAlerted  = false;

// NEO-6M Live GPS State
struct Neo6mState {
  bool   hasFix      = false;
  int    satellites  = 0;
  double latitude    = 0.317600; // Default Kampala fallback until 3D sky lock
  double longitude   = 32.582500;
  float  speedKmh    = 0.0f;
  float  headingDeg  = 0.0f;
  int    gsmCsq      = 22;
  String carrier     = "MTN-UGANDA";
} gps;

String nmeaLineBuf   = "";
String simUrcLineBuf = "";
bool   awaitingSmsBody = false;
String incomingSmsSender = "";

// Timers
unsigned long lastTelemetryMs     = 0;
unsigned long lastMqttRetryMs     = 0;
unsigned long lastMqttPingMs      = 0;
const unsigned long TELEMETRY_INTERVAL_MS = 3000UL;
const unsigned long MQTT_RETRY_INTERVAL_MS = 25000UL;

// Voice Call & Local Alarm State
bool   voiceCallActive          = false;
unsigned long voiceCallStartMs  = 0;
unsigned long lastVoiceCallMs   = 0;
unsigned long lastTamperSmsMs   = 0;
bool   localAlarmActive         = false;
unsigned long localAlarmStartMs = 0;
unsigned long lastStrobeMs      = 0;
bool   strobeState              = false;

const unsigned long CALL_RING_DURATION_MS   = 25000UL; // Ring owner for 25 seconds
const unsigned long CALL_COOLDOWN_MS        = 40000UL; // Prevent redial loop
const unsigned long TAMPER_SMS_COOLDOWN_MS  = 25000UL;
const unsigned long LOCAL_ALARM_DURATION_MS = 10000UL;

// Geofence (10-Minute Perimeter Breach SMS)
bool   geofenceAnchored        = false;
double geofenceCenterLat       = 0.317600;
double geofenceCenterLng       = 32.582500;
double geofenceRadiusMeters    = 500.0;
bool   isOutsidePerimeter      = false;
unsigned long lastGeofenceSmsMs = 0;
const unsigned long GEOFENCE_SMS_INTERVAL_MS = 600000UL; // 10 minutes

// Piezo + MPU6050 Tamper Detection
float piezoBaseline        = 475.0f;
int   piezoTamperThreshold = 90;
int   windowPeakPiezoDelta = 0;
float previousGForce       = 1.0f;
unsigned long lastTamperAlertMs     = 0;
unsigned long lastModemRfActivityMs = 0;
const unsigned long TAMPER_COOLDOWN_MS = 5000UL;

// Deferred OTA execution state (executed cleanly outside packet parser)
bool   pendingOtaExecution = false;
String pendingOtaUrl       = "";
String pendingOtaVersion   = "";
String pendingOtaMd5       = "";
bool   pendingOtaForce     = false;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
String sendAT(const String& cmd, uint32_t timeoutMs = 1200);
bool initSim800L();
void pollNeo6mGps();
void parseNmeaSentence(const String& line);
double nmeaToDecimalDegrees(const String& rawCoord, const String& hemi);
String getCsvField(const String& csv, int index);
String extractJsonString(const String& json, const char* key);
bool initOptionalMpu6050();
void checkMpu6050Tamper();
int  readFilteredPiezoADC();
void calibratePiezoSensor();
void checkPiezoVibrationSensor();
void checkBatteryCutTamper(float mainBattV, float backupBattV);
void triggerTamperAlert(float gForce, int piezoDelta, const char* sourceName);
void placeSim800VoiceCall(const char* reason);
void manageActiveVoiceCall();
void triggerLocalAlarm();
void manageLocalAlarm();
bool sendSim800Sms(const String& smsBody, const char* label);
void handleIncomingSms(const String& sender, const String& body);
void pollSim800Serial();
void checkGeofencePerimeter();
void applyGeofenceCommand(const String& spec);
double calculateHaversineMeters(double lat1, double lon1, double lat2, double lon2);
bool connectInternetMqtt();
bool publishMqttPacket(const String& topic, const String& payload);
void pollWifiMqttIncoming();
void handleIncomingCommand(const String& topic, const String& message);
void publishTelemetry();
void sendAck(const String& action, const String& status);
void executeInternetOtaUpdate(const String& url, const String& targetVer, const String& expectedMd5);
bool runWifiHttpOta(const String& url, const String& expectedMd5);
bool runSim800GprsHttpOta(const String& url, const String& expectedMd5);
String deriveChipImeiFromMac();
void loadIdentityFromNvs();
void refreshMqttTopics();
void normalizeAndSavePhoneNvs(const String& rawPhone);
void printIdentityCard();
bool isAuthorizedOwnerSender(const String& sender);

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(800);

  // 0. Load NVS Permanent Identity + Derive Unique 15-Digit ESP32 Silicon ID Fallback
  loadIdentityFromNvs();

  Serial.println("==================================================================");
  Serial.println("BODA 360 TELEMATICS — ESP32-WROOM + SIM800L VOICE + NEO-6M GPS");
  Serial.println("Board Profile    : " + String(BOARD_TYPE));
  Serial.println("Firmware Version : v" + String(FIRMWARE_VERSION) + " (Internet OTA + NVS Identity)");
  Serial.println("==================================================================");

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, HIGH); // Ignition Allowed = LED ON

  pinMode(PIN_ALARM_BUZZER, OUTPUT);
  digitalWrite(PIN_ALARM_BUZZER, LOW);

  pinMode(PIN_RELAY_IMMOBILIZER, OUTPUT);
  digitalWrite(PIN_RELAY_IMMOBILIZER, LOW);

  pinMode(PIN_MAIN_BATT_ADC, INPUT);
  pinMode(PIN_BACKUP_BATT_ADC, INPUT);
  pinMode(PIN_PIEZO_VIBRATION, INPUT);
  analogSetPinAttenuation(PIN_PIEZO_VIBRATION, ADC_11db);

  // 1. Start u-blox NEO-6M GPS UART2 (RX=16, TX=17 @ 9600 baud)
  SerialGPS.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  Serial.println("[+] u-blox NEO-6M GPS listening on UART2 (RX=16, TX=17 @ 9600 baud).");

  // 2. Optional Wi-Fi connection (for bench/hotspot fast OTA & MQTT)
  if (strlen(WIFI_OTA_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_OTA_SSID, WIFI_OTA_PASS);
    Serial.print("[WIFI] Connecting to " + String(WIFI_OTA_SSID));
    unsigned long wfStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wfStart < 5000) {
      delay(250);
      Serial.print(".");
    }
    Serial.println(WiFi.status() == WL_CONNECTED ? " CONNECTED!" : " (Not in range, using SIM800L GSM/GPRS)");
  }

  // 3. Initialize SIM800L Voice + SMS + GPRS Modem (auto-reads physical AT+GSN IMEI!)
  sim800Ready = initSim800L();
  refreshMqttTopics();
  printIdentityCard();

  // 4. Calibrate Piezo Tamper Sensor & Check Optional MPU-6050
  delay(200);
  calibratePiezoSensor();
  mpuAvailable = initOptionalMpu6050();

  // 5. Connect Live MQTT (Wi-Fi or SIM800L GPRS) & Subscribe to Command + OTA Broadcast
  lastMqttRetryMs = millis();
  connectInternetMqtt();

  // Report active firmware version & IMEI on boot (triggers automatic OWNER_SYNC from server!)
  sendAck("BOOT_READY", "ONLINE_V" + String(FIRMWARE_VERSION));
  Serial.println("[INFO] Commands: WHOAMI | SET_IMEI:<15d> | CLEAR_IMEI | SET_PHONE:<num> | SET_PLATE:<plt> | TEST_CALL | TEST_TAMPER");
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  // 1. Continuous u-blox NEO-6M NMEA GPS Stream Decoding
  pollNeo6mGps();

  // 2. High-Speed Piezo Vibration + Optional MPU-6050 Shock Sampling
  checkPiezoVibrationSensor();
  if (mpuAvailable) {
    checkMpu6050Tamper();
  }

  // 3. Non-Blocking Local Siren Strobe & SIM800L 25s Voice Call Auto-Hangup
  manageLocalAlarm();
  manageActiveVoiceCall();

  // 4. Poll Incoming SIM800L Serial URCs (SMS Commands, Ring, TCP MQTT Packets)
  if (sim800Ready) {
    pollSim800Serial();
  }
  if (usingWifiMqtt) {
    pollWifiMqttIncoming();
  }

  // 5. Execute Queued Internet OTA Firmware Upgrade Safely
  if (pendingOtaExecution) {
    pendingOtaExecution = false;
    executeInternetOtaUpdate(pendingOtaUrl, pendingOtaVersion, pendingOtaMd5);
  }

  // 6. Periodic 3-Second Telemetry, Battery Cut Check & Geofence Perimeter Check
  if (millis() - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryMs = millis();

    if (sim800Ready && !voiceCallActive) {
      String csqResp = sendAT("AT+CSQ", 500);
      int idx = csqResp.indexOf("+CSQ:");
      if (idx >= 0) {
        int comma = csqResp.indexOf(',', idx);
        if (comma > idx) {
          int rssi = csqResp.substring(idx + 5, comma).toInt();
          if (rssi >= 0 && rssi <= 31) gps.gsmCsq = rssi;
        }
      }
    }

    checkGeofencePerimeter();

    if (!mqttConnected && !voiceCallActive && (millis() - lastMqttRetryMs >= MQTT_RETRY_INTERVAL_MS)) {
      lastMqttRetryMs = millis();
      connectInternetMqtt();
    }

    publishTelemetry();
  }

  // 7. MQTT Keep-Alive Ping (every 40s)
  if (mqttConnected && (millis() - lastMqttPingMs >= 40000UL)) {
    lastMqttPingMs = millis();
    uint8_t pingPkt[2] = { 0xC0, 0x00 };
    if (usingWifiMqtt && wifiMqttSock.connected()) {
      wifiMqttSock.write(pingPkt, 2);
    }
  }

  // 8. Interactive USB Serial Console Commands
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("TEST_CALL")) {
      lastVoiceCallMs = 0;
      placeSim800VoiceCall("MANUAL_TEST_CALL");
    } else if (cmd.equalsIgnoreCase("TEST_TAMPER")) {
      lastVoiceCallMs = 0;
      lastTamperSmsMs = 0;
      triggerTamperAlert(4.9f, 340, "SERIAL_TEST_TAMPER");
    } else if (cmd.equalsIgnoreCase("TEST_SMS")) {
      String msg = "[BODA " + bikePlate + "]\nSIM800L + NEO-6M READY (v" + String(FIRMWARE_VERSION) + ")\n" +
                   "IMEI: " + deviceImei + "\n" +
                   "https://maps.google.com/?q=" + String(gps.latitude, 6) + "," + String(gps.longitude, 6);
      sendSim800Sms(msg, "Bench Test");
    } else if (cmd.equalsIgnoreCase("WHOAMI")) {
      printIdentityCard();
    } else if (cmd.startsWith("AT") || cmd.startsWith("at")) {
      Serial.println("[SIM800L] " + sendAT(cmd, 2000));
    } else if (cmd.length() > 0) {
      handleIncomingCommand("serial/gateway", cmd);
    }
  }
}

// ============================================================================
// SIM800L INITIALIZATION & DIRECT GSM VOICE CALL ENGINE
// ============================================================================
bool initSim800L() {
  pinMode(PIN_SIM800_RST, OUTPUT);
  digitalWrite(PIN_SIM800_RST, HIGH);

  SerialSIM800.begin(9600, SERIAL_8N1, PIN_SIM800_RX, PIN_SIM800_TX);
  delay(300);

  bool awake = false;
  for (int attempt = 0; attempt < 5; attempt++) {
    if (sendAT("AT", 500).indexOf("OK") >= 0) {
      awake = true;
      break;
    }
    delay(300);
  }

  if (!awake) {
    Serial.println("[SIM800L] Pulsing RST pin (GPIO 4) to wake module...");
    digitalWrite(PIN_SIM800_RST, LOW);
    delay(200);
    digitalWrite(PIN_SIM800_RST, HIGH);
    delay(3000);
    for (int attempt = 0; attempt < 6; attempt++) {
      if (sendAT("AT", 600).indexOf("OK") >= 0) {
        awake = true;
        break;
      }
      delay(400);
    }
  }

  if (!awake) {
    Serial.println("[-] SIM800L not responding! Using NVS / ESP32 Silicon Chip ID: " + deviceImei);
    return false;
  }

  sendAT("ATE0", 500);
  sendAT("AT+CMEE=2", 500);

  // Read physical SIM800L factory IMEI (AT+GSN) & cache in NVS
  String gsn = sendAT("AT+GSN", 800);
  gsn.replace("OK", "");
  gsn.trim();
  if (gsn.length() >= 14 && gsn.length() <= 16) {
    hardwareImei = gsn;
    nvs.putString("auto_imei", hardwareImei);
    if (!imeiCustomLocked) {
      deviceImei = hardwareImei;
    }
  }

  // Configure GSM Voice Call + Offline Direct SMS Text Mode
  sendAT("AT+CMGF=1", 600);              // Text SMS mode
  sendAT("AT+CSCS=\"GSM\"", 600);        // Standard 7-bit GSM alphabet
  sendAT("AT+CNMI=2,2,0,0,0", 600);      // Stream incoming SMS (+CMT) directly to ESP32
  sendAT("AT+CLIP=1", 500);              // Enable Caller ID on incoming calls
  sendAT("AT+CLVL=90", 500);             // Speaker/Audio level 90%
  sendAT("AT+CMIC=0,12", 500);           // High sensitivity microphone gain if owner answers call

  String cpin = sendAT("AT+CPIN?", 1000);
  Serial.println(cpin.indexOf("READY") >= 0
    ? "[+] SIM800L SIM Card Ready (+CPIN: READY) | Modem IMEI: " + hardwareImei
    : "[-] SIM800L SIM Card NOT detected: " + cpin);

  return true;
}

/**
 * Places a Direct 2G GSM Circuit-Switched Voice Call from SIM800L to Owner Phone
 * Works 100% WITHOUT internet or mobile data!
 */
void placeSim800VoiceCall(const char* reason) {
  if (!sim800Ready) {
    Serial.println("[-] Cannot place voice call: SIM800L not ready.");
    return;
  }
  if (voiceCallActive) {
    Serial.println("[VOICE CALL] Call to " + alertPhoneLocal + " already ringing/in-progress.");
    return;
  }
  if (lastVoiceCallMs != 0 && (millis() - lastVoiceCallMs < CALL_COOLDOWN_MS)) {
    Serial.println("[VOICE CALL] Cooldown active (preventing back-to-back redial).");
    return;
  }

  lastModemRfActivityMs = millis();
  lastVoiceCallMs = millis();

  Serial.println("[SIM800L CALL] 📞 Dialing Owner " + alertPhoneLocal + " via 2G GSM Voice (" + String(reason) + ")...");

  // Note the trailing semicolon ';' -> mandatory GSM AT standard for VOICE call (not data call)
  String dialResp = sendAT("ATD" + alertPhoneLocal + ";", 2500);
  lastModemRfActivityMs = millis();

  if (dialResp.indexOf("OK") >= 0) {
    voiceCallActive = true;
    voiceCallStartMs = millis();
    Serial.println("[+] 📞 RINGING OWNER PHONE " + alertPhoneLocal + " (25s ring window)!");
  } else {
    Serial.println("[-] SIM800L ATD failed (check SIM airtime & 4.0V 2A capacitor): " + dialResp);
  }
}

void manageActiveVoiceCall() {
  if (!voiceCallActive) return;

  if (millis() - voiceCallStartMs >= CALL_RING_DURATION_MS) {
    sendAT("ATH", 1000); // Hang up voice call cleanly after 25s
    voiceCallActive = false;
    lastModemRfActivityMs = millis();
    Serial.println("[SIM800L CALL] 📴 Hung up tamper voice alert call after 25s.");
  }
}

/**
 * Sends Direct Offline GSM SMS via SIM800L (AT+CMGS) with Live NEO-6M Google Maps Link
 */
bool sendSim800Sms(const String& smsBody, const char* label) {
  if (!sim800Ready || voiceCallActive) return false;

  Serial.println("[SIM800L SMS] 📩 Sending " + String(label) + " SMS to " + alertPhoneIntl + "...");
  lastModemRfActivityMs = millis();

  sendAT("AT+CMGF=1", 600);
  while (SerialSIM800.available()) SerialSIM800.read();

  SerialSIM800.print("AT+CMGS=\"" + alertPhoneIntl + "\"\r");

  unsigned long start = millis();
  bool gotPrompt = false;
  String resp = "";
  while (millis() - start < 4000) {
    while (SerialSIM800.available()) {
      char c = (char)SerialSIM800.read();
      resp += c;
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
    if (gotPrompt || resp.indexOf("ERROR") >= 0) break;
    delay(5);
  }

  if (!gotPrompt) {
    Serial.println("[-] SIM800L SMS '>' prompt timeout: " + resp);
    return false;
  }

  SerialSIM800.print(smsBody);
  SerialSIM800.write(0x1A); // Ctrl+Z

  resp = "";
  start = millis();
  while (millis() - start < 12000) {
    while (SerialSIM800.available()) {
      resp += (char)SerialSIM800.read();
    }
    if (resp.indexOf("+CMGS:") >= 0 || resp.indexOf("\r\nOK\r\n") >= 0) {
      lastModemRfActivityMs = millis();
      Serial.println("[+] ✅ SIM800L SMS delivered (" + String(label) + ")!");
      return true;
    }
    if (resp.indexOf("ERROR") >= 0) break;
    delay(10);
  }

  Serial.println("[-] SIM800L SMS failed: " + resp);
  return false;
}

// ============================================================================
// u-blox NEO-6M NMEA GPS PARSER ($GPRMC / $GPGGA) — ZERO EXTERNAL LIBRARIES
// ============================================================================
void pollNeo6mGps() {
  while (SerialGPS.available()) {
    char c = (char)SerialGPS.read();
    if (c == '\n') {
      nmeaLineBuf.trim();
      if (nmeaLineBuf.startsWith("$GP") || nmeaLineBuf.startsWith("$GN")) {
        parseNmeaSentence(nmeaLineBuf);
      }
      nmeaLineBuf = "";
    } else if (c != '\r' && nmeaLineBuf.length() < 120) {
      nmeaLineBuf += c;
    }
  }
}

double nmeaToDecimalDegrees(const String& rawCoord, const String& hemi) {
  if (rawCoord.length() < 4) return 0.0;
  int dotIdx = rawCoord.indexOf('.');
  if (dotIdx < 2) return 0.0;

  int degDigits = dotIdx - 2;
  double degrees = rawCoord.substring(0, degDigits).toDouble();
  double minutes = rawCoord.substring(degDigits).toDouble();
  double decimal = degrees + (minutes / 60.0);

  if (hemi == "S" || hemi == "W") {
    decimal = -decimal;
  }
  return decimal;
}

void parseNmeaSentence(const String& line) {
  // Strip NMEA "*XX" checksum suffix
  int starIdx = line.indexOf('*');
  String clean = (starIdx > 0) ? line.substring(0, starIdx) : line;

  if (clean.startsWith("$GPRMC") || clean.startsWith("$GNRMC")) {
    // $GPRMC,hhmmss.ss,A,llll.ll,a,yyyyy.yy,a,x.x,x.x,ddmmyy,,,A
    String status  = getCsvField(clean, 2); // 'A' = Valid 3D/2D Fix, 'V' = Void
    String rawLat  = getCsvField(clean, 3);
    String ns      = getCsvField(clean, 4);
    String rawLng  = getCsvField(clean, 5);
    String ew      = getCsvField(clean, 6);
    String knots   = getCsvField(clean, 7);
    String course  = getCsvField(clean, 8);

    if (status == "A" && rawLat.length() > 0 && rawLng.length() > 0) {
      gps.hasFix     = true;
      gps.latitude   = nmeaToDecimalDegrees(rawLat, ns);
      gps.longitude  = nmeaToDecimalDegrees(rawLng, ew);
      gps.speedKmh   = knots.length() ? (knots.toFloat() * 1.852f) : 0.0f;
      gps.headingDeg = course.length() ? course.toFloat() : 0.0f;
    } else {
      gps.hasFix = false;
    }
  } else if (clean.startsWith("$GPGGA") || clean.startsWith("$GNGGA")) {
    // $GPGGA,hhmmss.ss,llll.ll,a,yyyyy.yy,a,q,numSV,...
    String fixQual = getCsvField(clean, 6);
    String numSats = getCsvField(clean, 7);
    gps.satellites = numSats.length() ? numSats.toInt() : 0;
    if (fixQual.toInt() > 0) {
      String rawLat = getCsvField(clean, 2);
      String ns     = getCsvField(clean, 3);
      String rawLng = getCsvField(clean, 4);
      String ew     = getCsvField(clean, 5);
      if (rawLat.length() > 0 && rawLng.length() > 0) {
        gps.hasFix    = true;
        gps.latitude  = nmeaToDecimalDegrees(rawLat, ns);
        gps.longitude = nmeaToDecimalDegrees(rawLng, ew);
      }
    }
  }
}

// ============================================================================
// INTERNET OVER-THE-AIR (OTA) FIRMWARE ENGINE (WI-FI HTTP + SIM800L GPRS CHUNKED)
// ============================================================================
void executeInternetOtaUpdate(const String& url, const String& targetVer, const String& expectedMd5) {
  Serial.println("==================================================================");
  Serial.println("[OTA] 🚀 INTERNET FIRMWARE UPDATE TRIGGERED!");
  Serial.println("[OTA] Target Board : " + String(BOARD_TYPE));
  Serial.println("[OTA] New Version  : v" + targetVer + " (Current: v" + String(FIRMWARE_VERSION) + ")");
  Serial.println("[OTA] Binary URL   : " + url);
  Serial.println("==================================================================");

  // Critical Motorcycle Safety Check: NEVER flash or reboot MCU while bike is moving!
  if (gps.hasFix && gps.speedKmh >= 2.0f) {
    Serial.println("[OTA SAFETY] ⛔ Motorcycle is moving (" + String(gps.speedKmh, 1) + " km/h)! Deferring OTA.");
    sendAck("OTA_DEFERRED_MOVING", "SPEED_" + String(gps.speedKmh, 1));
    return;
  }

  if (!pendingOtaForce && targetVer.length() > 0 && targetVer == String(FIRMWARE_VERSION)) {
    Serial.println("[OTA] Already running v" + String(FIRMWARE_VERSION) + ". Skipping (use force=true to re-flash).");
    sendAck("OTA_SKIPPED", "ALREADY_V" + String(FIRMWARE_VERSION));
    return;
  }

  sendAck("OTA_STARTED", "DOWNLOADING_V" + targetVer);

  bool ok = false;

  // Path 1: Fast Wi-Fi / Mobile Hotspot HTTP Stream if connected
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[OTA] Using Fast Wi-Fi HTTP Streaming...");
    ok = runWifiHttpOta(url, expectedMd5);
  }

  // Path 2: Nationwide Cellular SIM800L GPRS Chunked HTTP Range Download
  if (!ok && sim800Ready) {
    Serial.println("[OTA] Using SIM800L Cellular GPRS Chunked HTTP Range Download...");
    ok = runSim800GprsHttpOta(url, expectedMd5);
  }

  if (ok) {
    Serial.println("[OTA] ✅ FLASH VERIFIED! Sending OTA_SUCCESS & Rebooting into v" + targetVer + "...");
    sendAck("OTA_SUCCESS", "INSTALLED_V" + targetVer);
    delay(800);
    ESP.restart();
  } else {
    Serial.println("[OTA] ❌ Firmware update failed — keeping existing v" + String(FIRMWARE_VERSION) + " safely.");
    sendAck("OTA_FAILED", "ROLLED_BACK_V" + String(FIRMWARE_VERSION));
  }
}

bool runWifiHttpOta(const String& url, const String& expectedMd5) {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(25000);

  if (!http.begin(url)) {
    Serial.println("[-] Wi-Fi HTTPClient begin() failed.");
    return false;
  }

  const char* headerKeys[] = { "x-MD5" };
  http.collectHeaders(headerKeys, 1);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.println("[-] Wi-Fi HTTP GET returned code: " + String(code));
    http.end();
    return false;
  }

  int totalLen = http.getSize();
  String md5 = expectedMd5.length() == 32 ? expectedMd5 : http.header("x-MD5");
  md5.trim();

  if (totalLen <= 0 || !Update.begin(totalLen)) {
    Serial.println("[-] Not enough OTA partition space for " + String(totalLen) + " bytes! Select 'Minimal SPIFFS (1.9MB APP with OTA)'.");
    http.end();
    return false;
  }

  if (md5.length() == 32) {
    Update.setMD5(md5.c_str());
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  http.end();

  if (written != (size_t)totalLen) {
    Serial.println("[-] Incomplete stream: wrote " + String(written) + " / " + String(totalLen));
    Update.abort();
    return false;
  }

  if (!Update.end(true)) {
    Serial.println("[-] Update.end() MD5/Flash error: " + String(Update.errorString()));
    return false;
  }

  return Update.isFinished();
}

/**
 * Streams ESP32 .bin over 2G SIM800L GPRS using HTTP Bearer + 4 KB AT+HTTPREAD chunks!
 * Note: SIM800L built-in HTTP stack supports plain http:// URLs natively.
 */
bool runSim800GprsHttpOta(const String& url, const String& expectedMd5) {
  if (url.startsWith("https://")) {
    Serial.println("[OTA NOTE] SIM800L native HTTP engine requires plain http:// URL (or enable Wi-Fi hotspot for https://).");
  }

  // Close TCP MQTT socket temporarily so SIM800L SAPBR HTTP bearer gets full RAM & bandwidth
  sendAT("AT+CIPSHUT", 2000);
  gprsTcpConnected = false;
  mqttConnected = false;

  sendAT("AT+SAPBR=0,1", 1500);
  sendAT("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", 800);
  sendAT("AT+SAPBR=3,1,\"APN\",\"" + String(CELLULAR_APN) + "\"", 800);
  sendAT("AT+SAPBR=1,1", 8000);

  sendAT("AT+HTTPTERM", 800);
  if (sendAT("AT+HTTPINIT", 1200).indexOf("OK") < 0) {
    Serial.println("[-] SIM800L AT+HTTPINIT failed.");
    return false;
  }

  sendAT("AT+HTTPPARA=\"CID\",1", 800);
  sendAT("AT+HTTPPARA=\"URL\",\"" + url + "\"", 1000);

  // Trigger HTTP GET (AT+HTTPACTION=0) and wait for "+HTTPACTION: 0,200,<total_bytes>"
  while (SerialSIM800.available()) SerialSIM800.read();
  SerialSIM800.println("AT+HTTPACTION=0");

  String actResp = "";
  unsigned long waitStart = millis();
  while (millis() - waitStart < 45000UL) {
    while (SerialSIM800.available()) {
      actResp += (char)SerialSIM800.read();
    }
    if (actResp.indexOf("+HTTPACTION:") >= 0 && actResp.indexOf('\n', actResp.indexOf("+HTTPACTION:")) >= 0) {
      break;
    }
    delay(25);
  }

  int haIdx = actResp.indexOf("+HTTPACTION:");
  if (haIdx < 0) {
    Serial.println("[-] SIM800L HTTPACTION timeout: " + actResp);
    sendAT("AT+HTTPTERM", 800);
    return false;
  }

  String haLine = actResp.substring(haIdx + 12);
  haLine.trim();
  int httpStatus = getCsvField(haLine, 1).toInt();
  int totalBytes = getCsvField(haLine, 2).toInt();

  Serial.println("[SIM800L OTA] HTTP Status: " + String(httpStatus) + " | Firmware Size: " + String(totalBytes) + " bytes");
  if ((httpStatus != 200 && httpStatus != 206) || totalBytes < 1024) {
    sendAT("AT+HTTPTERM", 800);
    return false;
  }

  if (!Update.begin(totalBytes)) {
    Serial.println("[-] Update.begin() failed — check Partition Scheme!");
    sendAT("AT+HTTPTERM", 800);
    return false;
  }
  if (expectedMd5.length() == 32) {
    Update.setMD5(expectedMd5.c_str());
  }

  // Read downloaded image from SIM800L internal HTTP buffer in safe 2048-byte slices
  const int CHUNK_SIZE = 2048;
  uint8_t chunkBuf[CHUNK_SIZE];
  int offset = 0;

  while (offset < totalBytes) {
    int want = min(CHUNK_SIZE, totalBytes - offset);
    while (SerialSIM800.available()) SerialSIM800.read();
    SerialSIM800.println("AT+HTTPREAD=" + String(offset) + "," + String(want));

    // Wait for "+HTTPREAD: <len>\r\n" header line
    String hdr = "";
    unsigned long t0 = millis();
    bool gotHeader = false;
    while (millis() - t0 < 8000UL) {
      if (SerialSIM800.available()) {
        char c = (char)SerialSIM800.read();
        hdr += c;
        int hrPos = hdr.indexOf("+HTTPREAD:");
        if (hrPos >= 0 && c == '\n' && hdr.indexOf('\n', hrPos) >= 0) {
          gotHeader = true;
          break;
        }
      }
    }

    if (!gotHeader) {
      Serial.println("[-] SIM800L +HTTPREAD header timeout at offset " + String(offset));
      Update.abort();
      sendAT("AT+HTTPTERM", 800);
      return false;
    }

    // Read exact `want` raw binary bytes
    int got = 0;
    t0 = millis();
    while (got < want && (millis() - t0 < 8000UL)) {
      if (SerialSIM800.available()) {
        chunkBuf[got++] = (uint8_t)SerialSIM800.read();
      }
    }

    if (got != want || Update.write(chunkBuf, got) != (size_t)got) {
      Serial.println("[-] Flash chunk write mismatch at offset " + String(offset));
      Update.abort();
      sendAT("AT+HTTPTERM", 800);
      return false;
    }

    offset += got;
    if ((offset % 32768) == 0 || offset == totalBytes) {
      int pct = (int)((offset * 100L) / totalBytes);
      Serial.println("[SIM800L OTA] Progress: " + String(pct) + "% (" + String(offset) + "/" + String(totalBytes) + ")");
    }
  }

  sendAT("AT+HTTPTERM", 800);
  sendAT("AT+SAPBR=0,1", 1000);

  if (!Update.end(true)) {
    Serial.println("[-] MD5/Flash verification failed: " + String(Update.errorString()));
    return false;
  }

  return Update.isFinished();
}

// ============================================================================
// TAMPER, 12V BATTERY CUT, SIREN & GEOFENCE ENGINE
// ============================================================================
void triggerLocalAlarm() {
  localAlarmActive = true;
  localAlarmStartMs = millis();
  lastStrobeMs = millis();
  strobeState = false;
  digitalWrite(PIN_ALARM_BUZZER, HIGH);
}

void manageLocalAlarm() {
  if (!localAlarmActive) return;

  unsigned long now = millis();
  if (now - localAlarmStartMs >= LOCAL_ALARM_DURATION_MS) {
    localAlarmActive = false;
    digitalWrite(PIN_ALARM_BUZZER, LOW);
    digitalWrite(PIN_STATUS_LED, isImmobilized ? LOW : HIGH);
    return;
  }

  if (now - lastStrobeMs >= 120) {
    lastStrobeMs = now;
    strobeState = !strobeState;
    digitalWrite(PIN_STATUS_LED, strobeState ? HIGH : LOW);
    digitalWrite(PIN_ALARM_BUZZER, strobeState ? HIGH : LOW);
  }
}

void triggerTamperAlert(float gForce, int piezoDelta, const char* sourceName) {
  Serial.println("[!] 🚨 TAMPER DETECTED (" + String(sourceName) + "): Shock " + String(gForce, 2) + "G!");

  // 1. Immediate Local Siren + Strobe
  triggerLocalAlarm();

  // 2. Build & Emit Cloud/Serial Tamper Event
  String payload = "{";
  payload += "\"device_imei\":\"" + deviceImei + "\",";
  payload += "\"hardware_imei\":\"" + hardwareImei + "\",";
  payload += "\"board_type\":\"" + String(BOARD_TYPE) + "\",";
  payload += "\"fw_version\":\"" + String(FIRMWARE_VERSION) + "\",";
  payload += "\"event_type\":\"TAMPER_DETECTED\",";
  payload += "\"sensor\":\"" + String(sourceName) + "\",";
  payload += "\"g_force\":" + String(gForce, 2) + ",";
  payload += "\"piezo_adc\":" + String(piezoDelta) + ",";
  payload += "\"latitude\":" + String(gps.latitude, 6) + ",";
  payload += "\"longitude\":" + String(gps.longitude, 6) + ",";
  payload += "\"plate\":\"" + bikePlate + "\",";
  payload += "\"alert_phone\":\"" + alertPhoneLocal + "\"";
  payload += "}";

  Serial.println("[MQTT TAMPER -> " + TOPIC_TAMPER + "]: " + payload);
  publishMqttPacket(TOPIC_TAMPER, payload);

  // 3. Send Direct Offline GSM SMS with Live NEO-6M Google Maps Location
  if (lastTamperSmsMs == 0 || (millis() - lastTamperSmsMs >= TAMPER_SMS_COOLDOWN_MS)) {
    lastTamperSmsMs = millis();
    String sms = "[BODA TAMPER " + bikePlate + "]\n";
    sms += "ALERT: " + String(sourceName) + " (" + String(gForce, 1) + "G)!\n";
    sms += String(isImmobilized ? "Engine: LOCKED\n" : "Reply STOP to cut engine\n");
    sms += "https://maps.google.com/?q=" + String(gps.latitude, 6) + "," + String(gps.longitude, 6);
    sendSim800Sms(sms, "Tamper Alert");
  }

  // 4. Place Direct 2G GSM Voice Call via SIM800L to Ring Owner Immediately!
  placeSim800VoiceCall(sourceName);
}

void checkBatteryCutTamper(float mainBattV, float backupBattV) {
  // If main 12V bike battery wire is cut (< 4.5V) while backup 3.7V Li-Ion is powering ESP32+SIM800L (> 3.2V)
  if (mainBattV < 4.5f && backupBattV > 3.2f && !mainPowerCutAlerted) {
    mainPowerCutAlerted = true;
    triggerTamperAlert(5.0f, 500, "12V_BATTERY_CUT");
  } else if (mainBattV > 9.5f) {
    mainPowerCutAlerted = false;
  }
}

int readFilteredPiezoADC() {
  int a = analogRead(PIN_PIEZO_VIBRATION);
  int b = analogRead(PIN_PIEZO_VIBRATION);
  int c = analogRead(PIN_PIEZO_VIBRATION);
  return (a + b + c) - min(a, min(b, c)) - max(a, max(b, c));
}

void calibratePiezoSensor() {
  long sum = 0;
  for (int i = 0; i < 200; i++) {
    sum += readFilteredPiezoADC();
    delay(1);
  }
  piezoBaseline = (float)sum / 200.0f;
  int maxNoise = 0;
  for (int i = 0; i < 200; i++) {
    int d = abs(readFilteredPiezoADC() - (int)roundf(piezoBaseline));
    if (d > maxNoise) maxNoise = d;
    delay(1);
  }
  piezoTamperThreshold = constrain(max(85, maxNoise * 3 + 35), 85, 260);
  Serial.println("[+] Piezo Sensor calibrated on GPIO 32 (Threshold: " + String(piezoTamperThreshold) + ")");
}

void checkPiezoVibrationSensor() {
  if (millis() - lastModemRfActivityMs < 220) return;

  int raw = readFilteredPiezoADC();
  int delta = abs(raw - (int)roundf(piezoBaseline));
  if (delta > windowPeakPiezoDelta) windowPeakPiezoDelta = delta;

  piezoBaseline = (delta < 60)
    ? (0.992f * piezoBaseline + 0.008f * raw)
    : (0.9995f * piezoBaseline + 0.0005f * raw);

  if (delta >= piezoTamperThreshold && (millis() - lastTamperAlertMs >= TAMPER_COOLDOWN_MS)) {
    delayMicroseconds(50);
    int confirm = abs(readFilteredPiezoADC() - (int)roundf(piezoBaseline));
    if (confirm >= (piezoTamperThreshold * 3 / 4)) {
      lastTamperAlertMs = millis();
      int peak = max(delta, confirm);
      float gEquiv = 3.4f + constrain((float)(peak - piezoTamperThreshold) / 600.0f, 0.0f, 1.0f) * 8.5f;
      triggerTamperAlert(gEquiv, peak, "PIEZO_GPIO32");
    }
  }
}

bool initOptionalMpu6050() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.beginTransmission(MPU6050_I2C_ADDR);
  if (Wire.endTransmission() != 0) {
    return false;
  }
  // Wake MPU-6050 (PWR_MGMT_1 = 0x6B -> 0x00) & set +-8g range (ACCEL_CONFIG = 0x1C -> 0x10)
  Wire.beginTransmission(MPU6050_I2C_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission();

  Wire.beginTransmission(MPU6050_I2C_ADDR);
  Wire.write(0x1C);
  Wire.write(0x10);
  Wire.endTransmission();
  Serial.println("[+] MPU-6050 6-Axis Accelerometer initialized on I2C (SDA=21, SCL=22).");
  return true;
}

void checkMpu6050Tamper() {
  Wire.beginTransmission(MPU6050_I2C_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return;
  if (Wire.requestFrom(MPU6050_I2C_ADDR, 6, true) != 6) return;

  int16_t ax = (Wire.read() << 8) | Wire.read();
  int16_t ay = (Wire.read() << 8) | Wire.read();
  int16_t az = (Wire.read() << 8) | Wire.read();

  // +-8g sensitivity = 4096 LSB/g
  float gx = ax / 4096.0f;
  float gy = ay / 4096.0f;
  float gz = az / 4096.0f;
  float totalG = sqrtf(gx * gx + gy * gy + gz * gz);
  float deltaG = fabsf(totalG - previousGForce);
  previousGForce = totalG;

  if (deltaG > 3.2f && (millis() - lastTamperAlertMs >= TAMPER_COOLDOWN_MS)) {
    lastTamperAlertMs = millis();
    triggerTamperAlert(deltaG, 0, "MPU6050");
  }
}

// ============================================================================
// GEOFENCE PERIMETER CHECK (10-MINUTE OFFLINE SMS)
// ============================================================================
double calculateHaversineMeters(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0;
  double dLat = (lat2 - lat1) * DEG_TO_RAD;
  double dLon = (lon2 - lon1) * DEG_TO_RAD;
  double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
             cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
             sin(dLon / 2.0) * sin(dLon / 2.0);
  return R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

void applyGeofenceCommand(const String& spec) {
  String sLat = getCsvField(spec, 0);
  String sLng = getCsvField(spec, 1);
  String sRad = getCsvField(spec, 2);
  if (sLat.length() > 0 && sLng.length() > 0) {
    geofenceCenterLat = sLat.toDouble();
    geofenceCenterLng = sLng.toDouble();
    if (sRad.toDouble() >= 20.0) geofenceRadiusMeters = sRad.toDouble();
    geofenceAnchored = true;
    isOutsidePerimeter = false;
    lastGeofenceSmsMs = 0;
  }
}

void checkGeofencePerimeter() {
  if (!gps.hasFix) return;
  if (!geofenceAnchored) {
    geofenceCenterLat = gps.latitude;
    geofenceCenterLng = gps.longitude;
    geofenceAnchored = true;
    return;
  }

  double dist = calculateHaversineMeters(geofenceCenterLat, geofenceCenterLng, gps.latitude, gps.longitude);
  if (dist > geofenceRadiusMeters) {
    bool due = (!isOutsidePerimeter) || (millis() - lastGeofenceSmsMs >= GEOFENCE_SMS_INTERVAL_MS);
    isOutsidePerimeter = true;
    if (due) {
      lastGeofenceSmsMs = millis();
      String sms = "[BODA " + bikePlate + "]\nPERIMETER EXIT: " + String((int)round(dist)) +
                   "m outside safe zone!\nSpd: " + String(gps.speedKmh, 1) + "km/h\n" +
                   "https://maps.google.com/?q=" + String(gps.latitude, 6) + "," + String(gps.longitude, 6);
      sendSim800Sms(sms, "Geofence Breach");
    }
  } else if (isOutsidePerimeter && dist <= geofenceRadiusMeters * 0.9) {
    isOutsidePerimeter = false;
    lastGeofenceSmsMs = 0;
  }
}

// ============================================================================
// LIGHTWEIGHT MQTT 3.1.1 CLIENT (WI-FI + SIM800L GPRS TCP)
// ============================================================================
int encodeMqttRemLen(uint8_t* dst, int len) {
  int idx = 0;
  do {
    uint8_t d = len % 128;
    len /= 128;
    if (len > 0) d |= 0x80;
    dst[idx++] = d;
  } while (len > 0);
  return idx;
}

bool sendRawMqttBytes(const uint8_t* data, size_t len) {
  if (usingWifiMqtt && wifiMqttSock.connected()) {
    return wifiMqttSock.write(data, len) == len;
  }
  if (!sim800Ready || !gprsTcpConnected || voiceCallActive) return false;

  while (SerialSIM800.available()) SerialSIM800.read();
  SerialSIM800.println("AT+CIPSEND=" + String(len));
  unsigned long t0 = millis();
  bool gotPrompt = false;
  while (millis() - t0 < 2500) {
    if (SerialSIM800.available() && (char)SerialSIM800.read() == '>') {
      gotPrompt = true;
      break;
    }
  }
  if (!gotPrompt) {
    gprsTcpConnected = false;
    mqttConnected = false;
    return false;
  }

  SerialSIM800.write(data, len);
  lastModemRfActivityMs = millis();

  String resp = "";
  t0 = millis();
  while (millis() - t0 < 4000) {
    while (SerialSIM800.available()) resp += (char)SerialSIM800.read();
    if (resp.indexOf("SEND OK") >= 0) return true;
    if (resp.indexOf("ERROR") >= 0 || resp.indexOf("CLOSED") >= 0) {
      gprsTcpConnected = false;
      mqttConnected = false;
      return false;
    }
    delay(5);
  }
  return false;
}

bool sendMqttConnectAndSubs() {
  String clientId = "boda_wroom_" + deviceImei;
  int vhLen = 10; // MQTT 3.1.1 header
  int plLen = 2 + clientId.length();
  int remLen = vhLen + plLen;

  uint8_t pkt[128];
  int p = 0;
  pkt[p++] = 0x10; // CONNECT
  p += encodeMqttRemLen(pkt + p, remLen);
  pkt[p++] = 0x00; pkt[p++] = 0x04;
  pkt[p++] = 'M'; pkt[p++] = 'Q'; pkt[p++] = 'T'; pkt[p++] = 'T';
  pkt[p++] = 0x04; // Protocol level 4
  pkt[p++] = 0x02; // Clean session
  pkt[p++] = 0x00; pkt[p++] = 90; // KeepAlive 90s
  pkt[p++] = 0x00; pkt[p++] = (uint8_t)clientId.length();
  memcpy(pkt + p, clientId.c_str(), clientId.length());
  p += clientId.length();

  if (!sendRawMqttBytes(pkt, p)) return false;
  delay(400);

  // Subscribe to `boda/<imei>/command`, `boda/<hw_imei>/command` (if distinct), AND `boda/ota/broadcast`
  const String subs[3] = { TOPIC_COMMAND, TOPIC_COMMAND_HW, String(TOPIC_OTA_BROADCAST) };
  for (int i = 0; i < 3; i++) {
    if (i == 1 && (TOPIC_COMMAND_HW.length() == 0 || TOPIC_COMMAND_HW == TOPIC_COMMAND)) continue;
    uint8_t sp[128];
    int s = 0;
    int tLen = subs[i].length();
    int rLen = 2 + 2 + tLen + 1;
    sp[s++] = 0x82; // SUBSCRIBE
    s += encodeMqttRemLen(sp + s, rLen);
    sp[s++] = 0x00; sp[s++] = (uint8_t)(i + 1);
    sp[s++] = 0x00; sp[s++] = (uint8_t)tLen;
    memcpy(sp + s, subs[i].c_str(), tLen);
    s += tLen;
    sp[s++] = 0x00; // QoS 0
    sendRawMqttBytes(sp, s);
    delay(200);
  }
  return true;
}

bool connectInternetMqtt() {
  // Prefer Wi-Fi if connected
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiMqttSock.connect(MQTT_BROKER, MQTT_PORT)) {
      usingWifiMqtt = true;
      if (sendMqttConnectAndSubs()) {
        mqttConnected = true;
        lastMqttPingMs = millis();
        Serial.println("[MQTT] ✅ Connected via Wi-Fi to " + String(MQTT_BROKER) + " (OTA Broadcast Ready)");
        return true;
      }
    }
  }
  usingWifiMqtt = false;

  if (!sim800Ready || voiceCallActive) return false;

  String creg = sendAT("AT+CREG?", 800);
  if (creg.indexOf(",1") < 0 && creg.indexOf(",5") < 0) return false;

  sendAT("AT+CIPSHUT", 1500);
  sendAT("AT+CIPMUX=0", 600);
  sendAT("AT+CIPRXGET=0", 600); // Push incoming TCP data directly to serial
  sendAT("AT+CSTT=\"" + String(CELLULAR_APN) + "\",\"\",\"\"", 1000);
  sendAT("AT+CIICR", 6000);
  sendAT("AT+CIFSR", 1000);

  String cip = sendAT("AT+CIPSTART=\"TCP\",\"" + String(MQTT_BROKER) + "\",\"" + String(MQTT_PORT) + "\"", 7000);
  if (cip.indexOf("CONNECT OK") < 0 && cip.indexOf("ALREADY CONNECT") < 0) {
    // Wait up to 4s more for async CONNECT OK
    unsigned long t0 = millis();
    while (millis() - t0 < 4000) {
      while (SerialSIM800.available()) cip += (char)SerialSIM800.read();
      if (cip.indexOf("CONNECT OK") >= 0) break;
      delay(25);
    }
  }

  if (cip.indexOf("CONNECT OK") >= 0 || cip.indexOf("ALREADY CONNECT") >= 0) {
    gprsTcpConnected = true;
    if (sendMqttConnectAndSubs()) {
      mqttConnected = true;
      lastMqttPingMs = millis();
      Serial.println("[SIM800L MQTT] ✅ Connected over 2G GPRS to " + String(MQTT_BROKER) + "!");
      return true;
    }
  }

  gprsTcpConnected = false;
  mqttConnected = false;
  return false;
}

bool publishMqttPacket(const String& topic, const String& payload) {
  if (!mqttConnected) return false;
  int tLen = topic.length();
  int pLen = payload.length();
  int remLen = 2 + tLen + pLen;
  if (remLen + 5 > 512) return false;

  uint8_t pkt[520];
  int i = 0;
  pkt[i++] = 0x30; // PUBLISH QoS 0
  i += encodeMqttRemLen(pkt + i, remLen);
  pkt[i++] = (uint8_t)((tLen >> 8) & 0xFF);
  pkt[i++] = (uint8_t)(tLen & 0xFF);
  memcpy(pkt + i, topic.c_str(), tLen);
  i += tLen;
  memcpy(pkt + i, payload.c_str(), pLen);
  i += pLen;

  return sendRawMqttBytes(pkt, i);
}

void parseIncomingMqttBuffer(const uint8_t* buf, int len) {
  int pos = 0;
  while (pos + 2 <= len) {
    uint8_t hdr = buf[pos++];
    int mult = 1, remLen = 0;
    uint8_t enc;
    do {
      if (pos >= len) return;
      enc = buf[pos++];
      remLen += (enc & 127) * mult;
      mult *= 128;
    } while ((enc & 128) != 0);

    if (pos + remLen > len) return;
    if ((hdr & 0xF0) == 0x30 && remLen >= 2) {
      int tLen = (buf[pos] << 8) | buf[pos + 1];
      if (2 + tLen <= remLen) {
        String topic = "";
        for (int k = 0; k < tLen; k++) topic += (char)buf[pos + 2 + k];
        String msg = "";
        for (int k = 2 + tLen; k < remLen; k++) msg += (char)buf[pos + k];
        handleIncomingCommand(topic, msg);
      }
    }
    pos += remLen;
  }
}

void pollWifiMqttIncoming() {
  if (!wifiMqttSock.connected()) {
    mqttConnected = false;
    return;
  }
  if (wifiMqttSock.available() > 0) {
    uint8_t buf[384];
    int n = wifiMqttSock.read(buf, sizeof(buf));
    if (n > 0) parseIncomingMqttBuffer(buf, n);
  }
}

void pollSim800Serial() {
  while (SerialSIM800.available()) {
    char c = (char)SerialSIM800.read();

    // Detect binary MQTT PUBLISH frame (0x30) arriving from SIM800L transparent TCP socket
    if (c == 0x30 && simUrcLineBuf.length() == 0 && !awaitingSmsBody) {
      uint8_t pkt[384];
      pkt[0] = 0x30;
      int idx = 1;
      unsigned long t0 = millis();
      while (millis() - t0 < 250 && idx < (int)sizeof(pkt)) {
        while (SerialSIM800.available() && idx < (int)sizeof(pkt)) {
          pkt[idx++] = (uint8_t)SerialSIM800.read();
          t0 = millis();
        }
        delay(5);
      }
      parseIncomingMqttBuffer(pkt, idx);
      continue;
    }

    if (c == '\n') {
      simUrcLineBuf.trim();
      if (simUrcLineBuf.length() > 0) {
        if (awaitingSmsBody) {
          awaitingSmsBody = false;
          handleIncomingSms(incomingSmsSender, simUrcLineBuf);
          incomingSmsSender = "";
        } else if (simUrcLineBuf.startsWith("+CMT:")) {
          int q1 = simUrcLineBuf.indexOf('"');
          int q2 = simUrcLineBuf.indexOf('"', q1 + 1);
          incomingSmsSender = (q1 >= 0 && q2 > q1) ? simUrcLineBuf.substring(q1 + 1, q2) : "";
          awaitingSmsBody = true;
        } else if (simUrcLineBuf.indexOf("CLOSED") >= 0 || simUrcLineBuf.indexOf("+PDP: DEACT") >= 0) {
          gprsTcpConnected = false;
          mqttConnected = false;
        } else if (simUrcLineBuf.indexOf("NO CARRIER") >= 0 || simUrcLineBuf.indexOf("BUSY") >= 0) {
          voiceCallActive = false;
        }
      }
      simUrcLineBuf = "";
    } else if (c != '\r' && simUrcLineBuf.length() < 240) {
      simUrcLineBuf += c;
    }
  }
}

// ============================================================================
// REMOTE COMMAND & SMS DISPATCHER (IMMOBILIZER + OTA_UPDATE + VOICE TEST)
// ============================================================================
void handleIncomingSms(const String& sender, const String& body) {
  String upper = body;
  upper.trim();
  upper.toUpperCase();
  Serial.println("[SIM800L SMS RX <- " + sender + "]: " + body);

  if (!isAuthorizedOwnerSender(sender)) {
    Serial.println("[SMS AUTH] Ignored SMS from unauthorized sender: " + sender + " (Owner: " + alertPhoneIntl + ")");
    return;
  }

  if (upper.startsWith("OTA:")) {
    pendingOtaUrl = body.substring(4);
    pendingOtaUrl.trim();
    pendingOtaVersion = "sms-ota";
    pendingOtaMd5 = "";
    pendingOtaForce = true;
    pendingOtaExecution = true;
  } else if (upper.indexOf("STOP") >= 0 || upper.indexOf("CUT") >= 0 || upper.indexOf("LOCK") >= 0) {
    handleIncomingCommand("sms/owner", "ENGINE_CUT");
    sendSim800Sms("[BODA " + bikePlate + "]\nENGINE IMMOBILIZED!\nhttps://maps.google.com/?q=" +
                  String(gps.latitude, 6) + "," + String(gps.longitude, 6), "Cut Confirm");
  } else if (upper.indexOf("START") >= 0 || upper.indexOf("RESTORE") >= 0 || upper.indexOf("UNLOCK") >= 0) {
    handleIncomingCommand("sms/owner", "ENGINE_RESTORE");
    sendSim800Sms("[BODA " + bikePlate + "]\nENGINE RESTORED!\nhttps://maps.google.com/?q=" +
                  String(gps.latitude, 6) + "," + String(gps.longitude, 6), "Restore Confirm");
  } else if (upper.indexOf("CALLME") >= 0) {
    lastVoiceCallMs = 0;
    placeSim800VoiceCall("OWNER_SMS_CALLBACK");
  } else if (upper.indexOf("GPS") >= 0 || upper.indexOf("LOC") >= 0 || upper.indexOf("STATUS") >= 0) {
    String reply = "[BODA " + bikePlate + " v" + String(FIRMWARE_VERSION) + "]\n" +
                   "IMEI: " + deviceImei + "\n" +
                   "Fix: " + String(gps.hasFix ? "3D LOCK" : "SEARCHING") +
                   " (" + String(gps.satellites) + " sats) | Spd: " + String(gps.speedKmh, 1) + "km/h\n" +
                   "https://maps.google.com/?q=" + String(gps.latitude, 6) + "," + String(gps.longitude, 6);
    sendSim800Sms(reply, "GPS Status");
  }
}

void handleIncomingCommand(const String& topic, const String& message) {
  Serial.println("[CMD RX <- " + topic + "]: " + message);

  // 1. Internet Over-The-Air Firmware Rollout Command ("cmd":"OTA_UPDATE")
  if (message.indexOf("OTA_UPDATE") >= 0) {
    String targetBoard = extractJsonString(message, "board");
    if (targetBoard.length() > 0 && targetBoard != String(BOARD_TYPE)) {
      Serial.println("[OTA] Ignoring firmware broadcast for board='" + targetBoard + "' (this MCU is '" + String(BOARD_TYPE) + "').");
      return;
    }
    String url = extractJsonString(message, "url");
    if (url.length() > 0) {
      pendingOtaUrl       = url;
      pendingOtaVersion   = extractJsonString(message, "version");
      pendingOtaMd5       = extractJsonString(message, "md5");
      pendingOtaForce     = (message.indexOf("\"force\":true") >= 0);
      pendingOtaExecution = true;
    }
    return;
  }

  // 1B. Automatic Customer Sign-Up / Profile Sync ("cmd":"OWNER_SYNC") -> Saved Permanently in NVS!
  if (message.indexOf("OWNER_SYNC") >= 0) {
    String syncPlate = extractJsonString(message, "plate");
    String syncPhone = extractJsonString(message, "owner_phone");
    String syncEmail = extractJsonString(message, "owner_email");

    if (syncPlate.length() >= 2) {
      bikePlate = syncPlate;
      bikePlate.toUpperCase();
      nvs.putString("plate", bikePlate);
    }
    if (syncPhone.length() >= 7) {
      normalizeAndSavePhoneNvs(syncPhone);
    }
    if (syncEmail.length() >= 3) {
      ownerEmail = syncEmail;
      nvs.putString("email", ownerEmail);
    }
    Serial.println("[NVS OWNER_SYNC] ✅ Saved Customer Profile -> Plate: " + bikePlate +
                   " | Phone: " + alertPhoneIntl + " | Email: " + ownerEmail);
    printIdentityCard();
    sendAck("OWNER_SYNC", "SAVED_NVS_" + bikePlate);
    return;
  }

  // 1C. Factory / USB / Remote NVS Provisioning Commands (SET_IMEI, CLEAR_IMEI, SET_PHONE, SET_PLATE, SET_EMAIL)
  if (message.startsWith("SET_IMEI:")) {
    String newImei = message.substring(9);
    newImei.trim();
    if (newImei.length() >= 8 && newImei.length() <= 20) {
      deviceImei = newImei;
      imeiCustomLocked = true;
      nvs.putString("imei", deviceImei);
      nvs.putBool("imei_lock", true);
      refreshMqttTopics();
      mqttConnected = false; // Re-subscribe under new IMEI on next cycle
      printIdentityCard();
      sendAck("SET_IMEI", "LOCKED_" + deviceImei);
    }
    return;
  }
  if (message.equalsIgnoreCase("CLEAR_IMEI")) {
    imeiCustomLocked = false;
    nvs.putBool("imei_lock", false);
    nvs.remove("imei");
    deviceImei = (hardwareImei.length() >= 14) ? hardwareImei : chipFallbackImei;
    refreshMqttTopics();
    mqttConnected = false;
    printIdentityCard();
    sendAck("CLEAR_IMEI", "AUTO_" + deviceImei);
    return;
  }
  if (message.startsWith("SET_PHONE:")) {
    normalizeAndSavePhoneNvs(message.substring(10));
    printIdentityCard();
    sendAck("SET_PHONE", alertPhoneIntl);
    return;
  }
  if (message.startsWith("SET_PLATE:")) {
    bikePlate = message.substring(10);
    bikePlate.trim();
    bikePlate.toUpperCase();
    nvs.putString("plate", bikePlate);
    printIdentityCard();
    sendAck("SET_PLATE", bikePlate);
    return;
  }
  if (message.startsWith("SET_EMAIL:")) {
    ownerEmail = message.substring(10);
    ownerEmail.trim();
    ownerEmail.toLowerCase();
    nvs.putString("email", ownerEmail);
    printIdentityCard();
    sendAck("SET_EMAIL", ownerEmail);
    return;
  }

  // 2. Dynamic Geofence Update
  if (message.startsWith("GEOFENCE_SET:")) {
    applyGeofenceCommand(message.substring(13));
    return;
  }

  // 3. Remote Immobilization & Engine Restore
  if (message.indexOf("ENGINE_CUT") >= 0) {
    isImmobilized = true;
    digitalWrite(PIN_RELAY_IMMOBILIZER, HIGH);
    digitalWrite(PIN_STATUS_LED, LOW);
    Serial.println("[ACTION] ⛔ ENGINE IMMOBILIZED (Relay GPIO 23 HIGH).");
    sendAck("ENGINE_CUT", "EXECUTED");
  } else if (message.indexOf("ENGINE_RESTORE") >= 0) {
    isImmobilized = false;
    digitalWrite(PIN_RELAY_IMMOBILIZER, LOW);
    digitalWrite(PIN_STATUS_LED, HIGH);
    Serial.println("[ACTION] ✅ ENGINE RESTORED (Relay GPIO 23 LOW).");
    sendAck("ENGINE_RESTORE", "EXECUTED");
  }
}

// ============================================================================
// TELEMETRY & ACK PUBLISHING
// ============================================================================
void publishTelemetry() {
  int rawMain = analogRead(PIN_MAIN_BATT_ADC);
  float mainBattV = (rawMain / 4095.0f) * 3.3f * ((100.0f + 22.0f) / 22.0f);
  int rawBackup = analogRead(PIN_BACKUP_BATT_ADC);
  float backupBattV = (rawBackup / 4095.0f) * 3.3f * 2.0f * 1.1f;
  (void)analogRead(PIN_PIEZO_VIBRATION);

  checkBatteryCutTamper(mainBattV, backupBattV);
  float reportedV = (mainBattV > 5.0f) ? mainBattV : ((backupBattV > 2.5f) ? backupBattV : 12.6f);

  String payload = "{";
  payload += "\"device_imei\":\"" + deviceImei + "\",";
  payload += "\"hardware_imei\":\"" + hardwareImei + "\",";
  payload += "\"board_type\":\"" + String(BOARD_TYPE) + "\",";
  payload += "\"fw_version\":\"" + String(FIRMWARE_VERSION) + "\",";
  payload += "\"plate\":\"" + bikePlate + "\",";
  payload += "\"owner_phone\":\"" + alertPhoneLocal + "\",";
  payload += "\"latitude\":" + String(gps.latitude, 6) + ",";
  payload += "\"longitude\":" + String(gps.longitude, 6) + ",";
  payload += "\"speed\":" + String(gps.hasFix ? gps.speedKmh : 0.0f, 1) + ",";
  payload += "\"heading\":" + String(gps.hasFix ? gps.headingDeg : 0.0f, 1) + ",";
  payload += "\"battery_voltage\":" + String(reportedV, 2) + ",";
  payload += "\"ignition_status\":" + String(!isImmobilized ? "true" : "false") + ",";
  payload += "\"is_immobilized\":" + String(isImmobilized ? "true" : "false") + ",";
  payload += "\"gsm_signal\":" + String(gps.gsmCsq) + ",";
  payload += "\"satellites\":" + String(gps.satellites) + ",";
  payload += "\"gps_lock\":" + String(gps.hasFix ? "true" : "false");
  payload += "}";

  publishMqttPacket(TOPIC_TELEMETRY, payload);
  Serial.println("[MQTT TX -> " + TOPIC_TELEMETRY + "]: " + payload);
  windowPeakPiezoDelta = 0;
}

void sendAck(const String& action, const String& status) {
  String ack = "{";
  ack += "\"device_imei\":\"" + deviceImei + "\",";
  ack += "\"hardware_imei\":\"" + hardwareImei + "\",";
  ack += "\"board_type\":\"" + String(BOARD_TYPE) + "\",";
  ack += "\"fw_version\":\"" + String(FIRMWARE_VERSION) + "\",";
  ack += "\"plate\":\"" + bikePlate + "\",";
  ack += "\"command_action\":\"" + action + "\",";
  ack += "\"status\":\"" + status + "\",";
  ack += "\"ack_timestamp\":" + String(millis());
  ack += "}";

  publishMqttPacket(TOPIC_ACK, ack);
  Serial.println("[MQTT ACK -> " + TOPIC_ACK + "]: " + ack);
}

// ============================================================================
// NVS PERMANENT IDENTITY & AUTOMATIC HARDWARE IMEI / CHIP ID ENGINE
// ============================================================================
String deriveChipImeiFromMac() {
  uint64_t mac = ESP.getEfuseMac();
  uint64_t num = (mac & 0xFFFFFFFFFFFFULL) % 10000000000ULL;
  char buf[20];
  snprintf(buf, sizeof(buf), "86360%010llu", (unsigned long long)num);
  return String(buf);
}

void loadIdentityFromNvs() {
  chipFallbackImei = deriveChipImeiFromMac();
  nvs.begin("boda360", false);

  imeiCustomLocked = nvs.getBool("imei_lock", false);
  String savedCustom = nvs.getString("imei", "");
  String cachedAuto  = nvs.getString("auto_imei", "");

  bikePlate       = nvs.getString("plate", DEFAULT_FALLBACK_PLATE);
  alertPhoneLocal = nvs.getString("phone_loc", DEFAULT_PHONE_LOCAL);
  alertPhoneIntl  = nvs.getString("phone_intl", DEFAULT_PHONE_INTL);
  ownerEmail      = nvs.getString("email", "");

  hardwareImei = (cachedAuto.length() >= 14) ? cachedAuto : chipFallbackImei;

  if (imeiCustomLocked && savedCustom.length() >= 8) {
    deviceImei = savedCustom;
  } else if (cachedAuto.length() >= 14) {
    deviceImei = cachedAuto;
  } else {
    deviceImei = chipFallbackImei;
  }
  refreshMqttTopics();
}

void refreshMqttTopics() {
  TOPIC_TELEMETRY  = "boda/" + deviceImei + "/telemetry";
  TOPIC_COMMAND    = "boda/" + deviceImei + "/command";
  TOPIC_COMMAND_HW = "boda/" + hardwareImei + "/command";
  TOPIC_ACK        = "boda/" + deviceImei + "/ack";
  TOPIC_TAMPER     = "boda/" + deviceImei + "/tamper";
}

void normalizeAndSavePhoneNvs(const String& rawPhone) {
  String digits = "";
  for (unsigned int i = 0; i < rawPhone.length(); i++) {
    char c = rawPhone.charAt(i);
    if ((c >= '0' && c <= '9') || (c == '+' && digits.length() == 0)) {
      digits += c;
    }
  }
  if (digits.startsWith("+256") && digits.length() >= 12) {
    alertPhoneIntl  = digits;
    alertPhoneLocal = "0" + digits.substring(4);
  } else if (digits.startsWith("256") && digits.length() >= 11) {
    alertPhoneIntl  = "+" + digits;
    alertPhoneLocal = "0" + digits.substring(3);
  } else if (digits.startsWith("0") && digits.length() >= 10) {
    alertPhoneLocal = digits;
    alertPhoneIntl  = "+256" + digits.substring(1);
  } else if (digits.length() >= 8) {
    alertPhoneLocal = digits;
    alertPhoneIntl  = digits.startsWith("+") ? digits : ("+" + digits);
  }
  nvs.putString("phone_loc", alertPhoneLocal);
  nvs.putString("phone_intl", alertPhoneIntl);
}

bool isAuthorizedOwnerSender(const String& sender) {
  if (sender.length() == 0) return true;
  // Match last 8 digits of owner's registered phone so both 07... and +2567... match cleanly
  String tail = alertPhoneLocal.length() >= 8
    ? alertPhoneLocal.substring(alertPhoneLocal.length() - 8)
    : alertPhoneLocal;
  return tail.length() > 0 && sender.indexOf(tail) >= 0;
}

void printIdentityCard() {
  String modeStr = imeiCustomLocked
    ? "CUSTOM NVS LOCKED"
    : ((hardwareImei != chipFallbackImei) ? "AUTO SIM800L AT+GSN" : "AUTO ESP32 SILICON MAC");
  Serial.println("┌──────── BODA 360 PERMANENT BOARD IDENTITY (NVS) ────────┐");
  Serial.println("│ Active Device IMEI : " + deviceImei + " (" + modeStr + ")");
  Serial.println("│ Modem AT+GSN IMEI  : " + hardwareImei);
  Serial.println("│ ESP32 Chip Fallback: " + chipFallbackImei);
  Serial.println("│ Bound Bike Plate   : " + bikePlate);
  Serial.println("│ Owner Voice/SMS    : " + alertPhoneLocal + " (" + alertPhoneIntl + ")");
  Serial.println("│ Owner Account Email: " + (ownerEmail.length() ? ownerEmail : String("(awaiting customer sign-up)")));
  Serial.println("└─────────────────────────────────────────────────────────┘");
}

// ============================================================================
// UTILITY HELPERS
// ============================================================================
String sendAT(const String& cmd, uint32_t timeoutMs) {
  pollSim800Serial();
  lastModemRfActivityMs = millis();
  SerialSIM800.println(cmd);

  String resp = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (SerialSIM800.available()) {
      resp += (char)SerialSIM800.read();
    }
    if (resp.indexOf("\r\nOK\r\n") >= 0 || resp.indexOf("\r\nERROR\r\n") >= 0 ||
        resp.indexOf("+CME ERROR:") >= 0 || resp.indexOf("+CMS ERROR:") >= 0) {
      break;
    }
    delay(5);
  }
  return resp;
}

String getCsvField(const String& csv, int index) {
  int cur = 0, start = 0, len = csv.length();
  for (int i = 0; i <= len; i++) {
    if (i == len || csv.charAt(i) == ',') {
      if (cur == index) {
        String f = csv.substring(start, i);
        f.trim();
        return f;
      }
      cur++;
      start = i + 1;
    }
  }
  return "";
}

String extractJsonString(const String& json, const char* key) {
  String pat = "\"" + String(key) + "\"";
  int kIdx = json.indexOf(pat);
  if (kIdx < 0) return "";
  int colon = json.indexOf(':', kIdx + pat.length());
  if (colon < 0) return "";
  int q1 = json.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  int q2 = json.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return json.substring(q1 + 1, q2);
}

