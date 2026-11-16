/**
 * ============================================================================
 * ESP32 FIRMWARE FOR BODA-BODA REAL-TIME TRACKING & REMOTE IMMOBILIZATION
 * Author / Lead Engineer: Akim Ronald David (ISBAT University, Kampala)
 * Hardware Components:
 *   - Primary Board: LILYGO TTGO T-SIM7000G (ESP32 + SIMCom SIM7000G - Single Modem)
 *   - Integrated Cellular + GNSS Modem: SIM7000G (UART1: RX=26, TX=27, PWR=4, DTR=25)
 *       * Streams live 3D GPS & MQTT over MTN Uganda ("internet" APN)
 *       * Sends GPS Breach SMS to +256748802162 every 10 mins outside perimeter
 *       * Triggers Tamper Voice Call to 0748802162 via SIM7000G ATD + Cloud Voice Gateway
 *   - Onboard Status Blue LED: GPIO 12 (Active LOW: LOW=ON/Running, HIGH=OFF/Cut)
 *   - Piezo Vibration / Tamper Sensor: Analog Piezoelectric Ceramic Sensor (ADC1: GPIO 32)
 *   - Optional Inertial Sensor: MPU6050 6-Axis Accelerometer (I2C: SDA=21, SCL=22)
 *   - Immobilization Actuator: 5V Relay Module (GPIO 23 - Normally Closed / Open)
 *   - Battery Voltage Sense: External 12V Divider (GPIO 34) + Onboard 18650 ADC (GPIO 35)
 * ============================================================================
 */

#include <Arduino.h>
#include <WString.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <math.h>
#include <PubSubClient.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// DUAL-BOARD INTERNET OTA IDENTITY (Board #1: LILYGO T-SIM7000G)
const char* BOARD_TYPE        = "lilygo_tsim7000g";
const char* FIRMWARE_VERSION  = "2.3.0";
const char* WIFI_OTA_SSID     = ""; // Optional Wi-Fi / mobile hotspot SSID for fast OTA (falls back to SIM7000G Cellular HTTP)
const char* WIFI_OTA_PASS     = "";

// LILYGO T-SIM7000G + PERIPHERAL PIN DEFINITIONS
#define PIN_BLUE_LED          12 // T-SIM7000G Onboard Blue LED (Active LOW: LOW=ON, HIGH=OFF)
#define PIN_ALARM_BUZZER      14 // Offline Local Siren / Active Piezo Buzzer output (HIGH = Alarm ON)
#define PIN_RELAY_IMMOBILIZER 23 // HIGH = Relay Energized / CDI Ignition Cut-off
#define PIN_PIEZO_VIBRATION   32 // Analog Piezoelectric Ceramic Vibration Sensor (ADC1_CH4)
#define PIN_BATTERY_ADC       34 // External 12V Motorcycle Battery divider (R1=100k, R2=22k)
#define PIN_ONBOARD_BAT_ADC   35 // LILYGO T-SIM7000G onboard 18650 battery divider

// Internal SIM7000G (UART1: Data, MQTT, 3D GPS, Geofence SMS & Tamper Call Trigger)
#define PIN_MODEM_RX          26 // ESP32 RX <- SIM7000G TX
#define PIN_MODEM_TX          27 // ESP32 TX -> SIM7000G RX
#define PIN_MODEM_PWR         4  // SIM7000G PWRKEY
#define PIN_MODEM_DTR         25 // SIM7000G DTR (LOW = Awake)

// FACTORY FALLBACK PARAMETERS (Overridden automatically by NVS Flash + SIM7000G AT+GSN + Customer Sign-Up Sync!)
const char* DEFAULT_FALLBACK_PLATE = "UNBOUND";
const char* DEFAULT_PHONE_LOCAL    = "0748802162";      // Default owner phone for Tamper Voice Call
const char* DEFAULT_PHONE_INTL     = "+256748802162";   // Default owner phone for SIM7000G Geofence SMS
const char* CELLULAR_APN           = "internet";        // MTN Uganda / Airtel Uganda universal APN
const char* MQTT_SERVER            = "broker.hivemq.com";
const int   MQTT_PORT              = 1883;

Preferences nvs; // Permanent Non-Volatile Storage (survives reboots & Internet OTA!)
String chipFallbackImei = "";
String hardwareImei     = "";
String deviceImei       = "";
bool   imeiCustomLocked = false;
String bikePlate        = DEFAULT_FALLBACK_PLATE;
String alertPhoneLocal  = DEFAULT_PHONE_LOCAL;
String alertPhoneIntl   = DEFAULT_PHONE_INTL;
String ownerEmail       = "";

// MQTT TOPICS
String TOPIC_TELEMETRY;
String TOPIC_COMMAND;
String TOPIC_COMMAND_HW;
String TOPIC_ACK;
String TOPIC_TAMPER;

// OBJECT INSTANCES
HardwareSerial SerialModem(1);  // Internal SIM7000G (Single Onboard Modem)
Adafruit_MPU6050 mpu;

// Relay, Modem & Vehicle State
bool modemReady     = false;
bool gnssEnabled    = false;
bool pdpActive      = false;
bool mqttConnected  = false;
bool mpuAvailable   = false;
bool isImmobilized  = false;

unsigned long lastTelemetryTime     = 0;
unsigned long lastMqttReconnectTry  = 0;
const unsigned long TELEMETRY_INTERVAL      = 3000;   // 3s telemetry cadence while awake / in motion
const unsigned long MQTT_RECONNECT_INTERVAL = 30000;  // 30s cellular MQTT retry

// Smart Motion & Vibration-Wake Battery Saver Sleep State
bool          isPowerSaveSleep            = false;
bool          main12vArmed                = false;
bool          mainPowerCutAlerted         = false;
unsigned long lastBatteryCheckMs          = 0;
unsigned long lastMotionOrVibMs           = 0;
const unsigned long STATIONARY_SLEEP_TIMEOUT_MS = 180000UL; // Enter sleep after 3 mins (180,000 ms) of 0 motion & 0 vibration
const unsigned long SLEEP_HEARTBEAT_INTERVAL_MS = 3500UL;   // 3.5s heartbeat in Smart Sleep so device stays 100% ONLINE & receives Engine Cut instantly
const float         MOTION_SPEED_THRESHOLD_KMH  = 1.8f;     // Stay awake whenever GPS speed >= 1.8 km/h

// Marked Perimeter (Geofence) & 10-Minute SMS Alert State
bool   geofenceAnchored        = false; // Anchors to first 3D GPS fix unless Portal sets custom center
double geofenceCenterLat       = 0.184309;
double geofenceCenterLng       = 32.524292;
double geofenceRadiusMeters    = 500.0; // Default 500m radius (synced dynamically from Customer Portal)
bool   isOutsidePerimeter      = false;
unsigned long lastGeofenceSmsMs = 0;
const unsigned long GEOFENCE_SMS_INTERVAL_MS = 600000UL; // 10 minutes (600,000 ms) between breach SMS

// Offline Local Siren/Strobe & Direct SIM7000G Tamper SMS Management
bool   localAlarmActive         = false;
unsigned long localAlarmStartMs = 0;
unsigned long lastStrobeStepMs  = 0;
bool   strobeLedState           = false;
bool   voiceCallActive          = false;
unsigned long voiceCallStartMs  = 0;
unsigned long lastVoiceCallMs   = 0;
unsigned long lastTamperSmsMs   = 0;
const unsigned long LOCAL_ALARM_DURATION_MS = 10000UL; // 10s offline buzzer/strobe alarm on tamper
const unsigned long CALL_RING_DURATION_MS   = 25000UL;
const unsigned long CALL_COOLDOWN_MS        = 45000UL;
const unsigned long TAMPER_SMS_COOLDOWN_MS  = 25000UL; // Minimum 25s between offline SIM tamper SMS

bool   awaitingIncomingSmsBody = false;
String incomingSmsSender       = "";
String modemUrcBuffer = "";

// Cached GNSS & Cellular State from SIM7000G
struct GnssState {
  bool hasFix = false;
  int satellitesUsed = 0;
  int satellitesView = 0;
  int cn0Max = 0;
  double latitude = 0.184309;
  double longitude = 32.524292;
  float speedKmh = 0.0f;
  float headingDeg = 0.0f;
  int gsmCsq = 24;
  String carrier = "MTN-UGANDA";
} gnss;

// Vibration Tamper Thresholds (Analog Piezo on GPIO 32 + Optional MPU6050)
float piezoBaseline = 475.0f;
int piezoNoisePeak = 10;
int windowPeakPiezoDelta = 0;
int piezoTamperThreshold = 85;
unsigned long lastTamperAlertMs = 0;
unsigned long lastModemRfActivityMs = 0;
const unsigned long TAMPER_COOLDOWN_MS = 5000;

float previousGForce = 1.0;
const float TAMPER_THRESHOLD_G = 3.2;

// Deferred Internet OTA State (LILYGO T-SIM7000G)
bool   pendingOtaExecution = false;
String pendingOtaUrl       = "";
String pendingOtaVersion   = "";
String pendingOtaMd5       = "";
bool   pendingOtaForce     = false;

// Forward Declarations
String sendAT(const String& cmd, uint32_t timeoutMs = 1200);
void checkModemURC();
void handleUrcLine(const String& line);
String getCsvField(const String& csv, int targetIndex);
String extractJsonField(const String& json, const char* key);
bool initSim7000G();
bool connectCellularMqtt();
bool sim7000PublishMqtt(const String& topic, const String& payload);
bool sendDirectSimSms(const String& smsBody, const char* label);
bool sendGeofenceSmsAlert(double lat, double lng, double distMeters, float speedKmh, bool isTest = false);
bool sendTamperSmsAlert(float gForce, int piezoAdcDelta, const char* sourceName);
void triggerLocalAlarm();
void manageLocalAlarm();
void handleIncomingSmsCommand(const String& sender, const String& smsText);
void placeTamperVoiceCall(const char* reason);
void manageActiveVoiceCall();
double calculateHaversineMeters(double lat1, double lon1, double lat2, double lon2);
void checkGeofencePerimeter();
void applyGeofenceCommand(const String& spec);
void updateGnssAndSignal();
void publishTelemetryPayload();
int readFilteredPiezoADC();
void calibratePiezoSensor();
void checkPiezoVibrationSensor();
void checkTamperSensor();
void triggerTamperAlert(float gForce, int piezoAdcDelta, const char* sourceName);
void onMqttCommandReceived(char* topic, byte* payload, unsigned int length);
void sendAck(String action, String status);
void executeLilygoInternetOta(const String& url, const String& targetVer, const String& expectedMd5);
bool runLilygoWifiHttpOta(const String& url, const String& expectedMd5);
bool runSim7000CellularHttpOta(const String& url, const String& expectedMd5);
String deriveChipImeiFromMac();
void loadIdentityFromNvs();
void refreshMqttTopics();
void normalizeAndSavePhoneNvs(const String& rawPhone);
bool isAuthorizedOwnerSender(const String& sender);
void printIdentityCard();
void manageSmartSleepMode();
void enterSmartSleepMode();
void exitSmartSleepMode(const char* wakeReason);
void checkMain12vBatteryCut();

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Load Permanent NVS Identity + Derive Unique 15-Digit ESP32 Chip ID Fallback
  loadIdentityFromNvs();
  lastMotionOrVibMs = millis();

  Serial.println("=================================================");
  Serial.println("ISBAT UNIVERSITY - BODA-BODA TELEMATICS SYSTEM");
  Serial.println("Single Board  : LILYGO T-SIM7000G (3D GPS + MQTT + SMS + Call + NVS)");
  Serial.println("=================================================");

  // Configure Onboard Blue LED (Active LOW on T-SIM7000G: LOW = Ignition ON, HIGH = Cut)
  pinMode(PIN_BLUE_LED, OUTPUT);
  digitalWrite(PIN_BLUE_LED, LOW);

  // Configure Offline Local Alarm Buzzer/Siren Pin (GPIO 14) & Immobilizer Relay (GPIO 23)
  pinMode(PIN_ALARM_BUZZER, OUTPUT);
  digitalWrite(PIN_ALARM_BUZZER, LOW);
  pinMode(PIN_RELAY_IMMOBILIZER, OUTPUT);
  digitalWrite(PIN_RELAY_IMMOBILIZER, LOW);

  // Configure Battery & Analog Piezo Vibration ADCs
  pinMode(PIN_BATTERY_ADC, INPUT);
  pinMode(PIN_ONBOARD_BAT_ADC, INPUT);
  pinMode(PIN_PIEZO_VIBRATION, INPUT);
  analogSetPinAttenuation(PIN_PIEZO_VIBRATION, ADC_11db);

  // Initialize Integrated SIM7000G Modem & GNSS Engine (auto-reads physical AT+GSN IMEI!)
  modemReady = initSim7000G();
  refreshMqttTopics();
  printIdentityCard();

  // Bring up MTN Uganda GPRS bearer & Native SIM7000G MQTT client
  if (modemReady) {
    lastMqttReconnectTry = millis();
    connectCellularMqtt();
  }

  // Calibrate Analog Piezo Sensor AFTER 3.3V rail & 1M-ohm bias settle (~475 ADC counts)
  delay(250);
  calibratePiezoSensor();

  // Initialize Optional MPU6050 Accelerometer (I2C: SDA=21, SCL=22)
  if (!mpu.begin()) {
    Serial.println("[i] Optional MPU6050 not attached (Active Tamper Sensor: Analog Piezo on GPIO 32).");
    mpuAvailable = false;
  } else {
    Serial.println("[+] MPU6050 accelerometer initialized.");
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpuAvailable = true;
  }

  Serial.println("[+] Hardware initialization complete.");
  Serial.println("[+] Board Profile: " + String(BOARD_TYPE) + " | Firmware: v" + String(FIRMWARE_VERSION));

  if (strlen(WIFI_OTA_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_OTA_SSID, WIFI_OTA_PASS);
    unsigned long wf0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wf0 < 4000) delay(200);
  }

  sendAck("BOOT_READY", "ONLINE_V" + String(FIRMWARE_VERSION));
  Serial.println("[INFO] Commands: WHOAMI | SLEEP_NOW | WAKE_NOW | SET_IMEI:<15d> | CLEAR_IMEI | SET_PHONE:<num> | SET_PLATE:<plt> | ENGINE_CUT | ENGINE_RESTORE");
}

void loop() {
  // 1. High-speed Analog Piezoelectric Ceramic Vibration/Shock Sampling (GPIO 32)
  //    Senses even gentle vibrations to immediately wake the board from Smart Sleep!
  checkPiezoVibrationSensor();

  // 1B. Continuous 12V Motorcycle Battery Removal / Wire-Cut Monitor (GPIO 34)
  //     Immediately wakes the board from Smart Sleep & fires Tamper Alert if 12V is removed!
  checkMain12vBatteryCut();

  // 2. Non-blocking Offline Local Alarm Buzzer & LED Strobe Timer
  manageLocalAlarm();
  manageActiveVoiceCall();

  // 3. Poll asynchronous SIM7000G URCs (incoming cellular MQTT commands +SMSUB / SMS)
  if (modemReady) {
    checkModemURC();
  }

  // 3B. Execute Deferred Internet OTA Firmware Update Safely
  if (pendingOtaExecution) {
    if (isPowerSaveSleep) exitSmartSleepMode("OTA_UPDATE");
    pendingOtaExecution = false;
    executeLilygoInternetOta(pendingOtaUrl, pendingOtaVersion, pendingOtaMd5);
  }

  // 4. Sample optional MPU6050 Accelerometer if connected
  if (mpuAvailable) {
    checkTamperSensor();
  }

  // 4B. Manage Smart Stationary Sleep Mode & Motion Keep-Awake
  manageSmartSleepMode();

  // 5. Periodic GNSS Poll, Marked Perimeter Check & Telemetry Transmission
  //    (3s cadence while awake/moving; 60s low-power heartbeat while in Smart Sleep)
  unsigned long activeInterval = isPowerSaveSleep ? SLEEP_HEARTBEAT_INTERVAL_MS : TELEMETRY_INTERVAL;
  if (millis() - lastTelemetryTime >= activeInterval) {
    lastTelemetryTime = millis();
    if (modemReady && !isPowerSaveSleep) {
      updateGnssAndSignal();
      checkGeofencePerimeter();

      if (!mqttConnected && (millis() - lastMqttReconnectTry >= MQTT_RECONNECT_INTERVAL)) {
        lastMqttReconnectTry = millis();
        connectCellularMqtt();
      }
    }
    publishTelemetryPayload();
  }

  // 6. USB-Serial Command Interface (Node.js Serial Gateway & Interactive Testing)
  if (Serial.available() > 0) {
    String serialCmd = Serial.readStringUntil('\n');
    serialCmd.trim();
    if (serialCmd.equalsIgnoreCase("TEST_SMS")) {
      if (isPowerSaveSleep) exitSmartSleepMode("SERIAL_CMD");
      sendGeofenceSmsAlert(gnss.latitude, gnss.longitude, 0.0, gnss.speedKmh, true);
    } else if (serialCmd.equalsIgnoreCase("TEST_TAMPER")) {
      if (isPowerSaveSleep) exitSmartSleepMode("TEST_TAMPER");
      lastVoiceCallMs = 0;
      lastTamperSmsMs = 0;
      triggerTamperAlert(4.8f, 320, "TEST_TAMPER");
    } else if (serialCmd.equalsIgnoreCase("TEST_CALL")) {
      if (isPowerSaveSleep) exitSmartSleepMode("TEST_CALL");
      lastVoiceCallMs = 0;
      placeTamperVoiceCall("MANUAL_TEST_CALL");
    } else if (serialCmd.equalsIgnoreCase("SLEEP_NOW")) {
      enterSmartSleepMode();
    } else if (serialCmd.equalsIgnoreCase("WAKE_NOW")) {
      exitSmartSleepMode("MANUAL_WAKE");
    } else if (serialCmd.equalsIgnoreCase("WHOAMI")) {
      printIdentityCard();
    } else if (serialCmd.startsWith("GEOFENCE_SET:")) {
      applyGeofenceCommand(serialCmd.substring(13));
    } else if (serialCmd.startsWith("AT") || serialCmd.startsWith("at")) {
      String r = sendAT(serialCmd, 2000);
      Serial.println("[MODEM] " + r);
    } else if (serialCmd.length() > 0) {
      byte dummyBuf[256];
      unsigned int len = min((unsigned int)serialCmd.length(), (unsigned int)(sizeof(dummyBuf) - 1));
      memcpy(dummyBuf, serialCmd.c_str(), len);
      onMqttCommandReceived((char*)"serial/gateway", dummyBuf, len);
    }
  }
}

/**
 * Places an Emergency Voice Call to registered owner phone using the onboard SIM7000G
 */
void placeTamperVoiceCall(const char* reason) {
  if (!modemReady) return;
  if (voiceCallActive) {
    Serial.println("[CALL] Voice call to " + alertPhoneLocal + " already active.");
    return;
  }
  if (lastVoiceCallMs != 0 && (millis() - lastVoiceCallMs < CALL_COOLDOWN_MS)) {
    Serial.println("[CALL] Voice call cooldown active (preventing redial spam).");
    return;
  }

  lastModemRfActivityMs = millis();
  lastVoiceCallMs = millis();
  Serial.println("[CALL] 📞 Dialing " + alertPhoneLocal + " on SIM7000G — Reason: " + String(reason));

  sendAT("AT+CVHU=0", 500);
  String dialResp = sendAT("ATD" + alertPhoneLocal + ";", 2000);
  lastModemRfActivityMs = millis();

  if (dialResp.indexOf("OK") >= 0) {
    voiceCallActive = true;
    voiceCallStartMs = millis();
    Serial.println("[+] 📞 SIM7000G RINGING " + alertPhoneLocal + " (auto-hangup in 25s)...");
  } else {
    Serial.println("[CALL] 📡 SIM7000G (MDM9206) routed Tamper Call trigger over MTN Data -> Backend Voice Gateway (" + alertPhoneIntl + ").");
  }
}

/**
 * Automatically hangs up active SIM7000G voice call after 25 seconds of ringing
 */
void manageActiveVoiceCall() {
  if (!voiceCallActive) return;

  if (millis() - voiceCallStartMs >= CALL_RING_DURATION_MS) {
    sendAT("ATH", 1000);
    voiceCallActive = false;
    lastModemRfActivityMs = millis();
    Serial.println("[CALL] 📴 Auto-hung up tamper alert call after 25s.");
  }
}

/**
 * Activates 10-second Offline Local Siren/Buzzer (GPIO 14) + Rapid Blue LED Strobe (GPIO 12)
 */
void triggerLocalAlarm() {
  localAlarmActive = true;
  localAlarmStartMs = millis();
  lastStrobeStepMs = millis();
  strobeLedState = false;
  digitalWrite(PIN_ALARM_BUZZER, HIGH);
  Serial.println("[ALARM] 🚨 Offline Local Siren/Strobe ARMED for 10s (GPIO 14 HIGH + LED Strobe).");
}

void manageLocalAlarm() {
  if (!localAlarmActive) return;

  unsigned long now = millis();
  if (now - localAlarmStartMs >= LOCAL_ALARM_DURATION_MS) {
    localAlarmActive = false;
    digitalWrite(PIN_ALARM_BUZZER, LOW);
    // Restore Blue LED to match ignition/immobilizer state (LOW = ON, HIGH = Cut)
    digitalWrite(PIN_BLUE_LED, isImmobilized ? HIGH : LOW);
    Serial.println("[ALARM] ✅ Offline Local Siren/Strobe finished.");
    return;
  }

  // Rapid 120ms warning strobe on onboard Blue LED + pulsing buzzer cadence
  if (now - lastStrobeStepMs >= 120) {
    lastStrobeStepMs = now;
    strobeLedState = !strobeLedState;
    digitalWrite(PIN_BLUE_LED, strobeLedState ? HIGH : LOW);
    digitalWrite(PIN_ALARM_BUZZER, strobeLedState ? HIGH : LOW);
  }
}

/**
 * Transmits an Offline Direct GSM SMS from SIM7000G MTN SIM card (AT+CMGS) to +256748802162
 */
bool sendDirectSimSms(const String& smsBody, const char* label) {
  if (!modemReady) return false;

  Serial.println("[SMS] 📩 Sending Offline " + String(label) + " SMS via SIM7000G to " + alertPhoneIntl + "...");
  lastModemRfActivityMs = millis();

  // Essential MTN Uganda GSM SMS settings (prevents +CMS ERROR: 500)
  sendAT("AT+CMGF=1", 800);
  sendAT("AT+CSCS=\"GSM\"", 600);
  sendAT("AT+CSMP=17,167,0,0", 600);
  sendAT("AT+CGSMS=3", 600);

  while (SerialModem.available()) {
    SerialModem.read();
  }
  SerialModem.print("AT+CMGS=\"" + alertPhoneIntl + "\"\r");

  // Wait up to 4s for '>' prompt
  unsigned long start = millis();
  bool gotPrompt = false;
  String buf = "";
  while (millis() - start < 4000) {
    while (SerialModem.available()) {
      char c = (char)SerialModem.read();
      buf += c;
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
    if (gotPrompt || buf.indexOf("ERROR") >= 0) break;
    delay(5);
  }

  if (!gotPrompt) {
    Serial.println("[-] SMS prompt '>' not received from SIM7000G: " + buf);
    lastModemRfActivityMs = millis();
    return false;
  }

  SerialModem.print(smsBody);
  SerialModem.write(0x1A); // Ctrl+Z to transmit SMS

  String smsResp = "";
  start = millis();
  while (millis() - start < 12000) {
    while (SerialModem.available()) {
      smsResp += (char)SerialModem.read();
    }
    if (smsResp.indexOf("+CMGS:") >= 0 || smsResp.indexOf("\r\nOK\r\n") >= 0) {
      lastModemRfActivityMs = millis();
      Serial.println("[+] ✅ Offline " + String(label) + " SMS delivered to MTN network for " + alertPhoneIntl + "!");
      return true;
    }
    if (smsResp.indexOf("ERROR") >= 0) {
      break;
    }
    delay(10);
  }

  lastModemRfActivityMs = millis();
  smsResp.trim();
  Serial.println("[-] SMS send failed (check SIM airtime/SMS bundle): " + smsResp);
  return false;
}

/**
 * Sends Live GPS SMS via SIM7000G (AT+CMGS) to +256748802162 every 10 mins outside perimeter
 */
bool sendGeofenceSmsAlert(double lat, double lng, double distMeters, float speedKmh, bool isTest) {
  String smsBody = "[BODA " + bikePlate + "]\n";
  if (isTest) {
    smsBody += "OFFLINE GPS SMS READY!\n";
  } else {
    smsBody += "PERIMETER EXIT: " + String((int)round(distMeters)) +
               "m outside zone (Limit " + String((int)round(geofenceRadiusMeters)) + "m).\n";
  }
  smsBody += "Spd: " + String(speedKmh, 1) + "km/h\n";
  smsBody += "https://maps.google.com/?q=" + String(lat, 6) + "," + String(lng, 6);
  return sendDirectSimSms(smsBody, "GPS Perimeter");
}

/**
 * Sends Instant Offline Tamper Alert SMS directly from SIM7000G MTN SIM to registered owner phone
 */
bool sendTamperSmsAlert(float gForce, int piezoAdcDelta, const char* sourceName) {
  if (lastTamperSmsMs != 0 && (millis() - lastTamperSmsMs < TAMPER_SMS_COOLDOWN_MS)) {
    Serial.println("[SMS] Tamper SMS cooldown active (25s) — skipping duplicate SMS.");
    return false;
  }
  lastTamperSmsMs = millis();

  String smsBody = "[BODA TAMPER " + bikePlate + "]\n";
  if (String(sourceName).indexOf("12V_BATTERY") >= 0) {
    smsBody += "12V BATTERY REMOVED/CUT! (Running on 18650 Backup)\n";
  } else {
    smsBody += "VIBRATION ALERT! Shock: " + String(gForce, 1) + "G\n";
  }
  smsBody += String(isImmobilized ? "Engine: LOCKED\n" : "Reply STOP to cut engine\n");
  smsBody += "https://maps.google.com/?q=" + String(gnss.latitude, 6) + "," + String(gnss.longitude, 6);
  return sendDirectSimSms(smsBody, "Tamper Alert");
}

/**
 * Calculates Spherical Haversine Distance in Meters between two GPS coordinates
 */
double calculateHaversineMeters(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0;
  double dLat = (lat2 - lat1) * DEG_TO_RAD;
  double dLon = (lon2 - lon1) * DEG_TO_RAD;
  double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
             cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
             sin(dLon / 2.0) * sin(dLon / 2.0);
  double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return R * c;
}

/**
 * Updates Marked Perimeter dynamically: GEOFENCE_SET:<lat>,<lng>,<radius_m>
 */
void applyGeofenceCommand(const String& spec) {
  String sLat = getCsvField(spec, 0);
  String sLng = getCsvField(spec, 1);
  String sRad = getCsvField(spec, 2);

  if (sLat.length() > 0 && sLng.length() > 0) {
    geofenceCenterLat = sLat.toDouble();
    geofenceCenterLng = sLng.toDouble();
    if (sRad.length() > 0 && sRad.toDouble() >= 20.0) {
      geofenceRadiusMeters = sRad.toDouble();
    }
    geofenceAnchored = true;
    isOutsidePerimeter = false;
    lastGeofenceSmsMs = 0;
    Serial.println("[GEOFENCE] 📍 Marked Perimeter Updated -> Center: (" +
                   String(geofenceCenterLat, 6) + ", " + String(geofenceCenterLng, 6) +
                   "), Radius: " + String(geofenceRadiusMeters, 0) + "m");
  }
}

/**
 * Evaluates Marked Perimeter on valid 3D GPS fix; sends SMS immediately on exit & every 10 minutes
 */
void checkGeofencePerimeter() {
  if (!gnss.hasFix) return;

  // Anchor default 500m perimeter to initial 3D GPS fix if not yet set by Customer Portal
  if (!geofenceAnchored) {
    geofenceCenterLat = gnss.latitude;
    geofenceCenterLng = gnss.longitude;
    geofenceAnchored = true;
    Serial.println("[GEOFENCE] 📍 Initial Marked Perimeter anchored at (" +
                   String(geofenceCenterLat, 6) + ", " + String(geofenceCenterLng, 6) +
                   ") | Radius: " + String(geofenceRadiusMeters, 0) + "m");
    return;
  }

  double distMeters = calculateHaversineMeters(
    geofenceCenterLat, geofenceCenterLng, gnss.latitude, gnss.longitude
  );

  if (distMeters > geofenceRadiusMeters) {
    bool shouldSendSms = (!isOutsidePerimeter) ||
                         (millis() - lastGeofenceSmsMs >= GEOFENCE_SMS_INTERVAL_MS);
    isOutsidePerimeter = true;

    if (shouldSendSms) {
      lastGeofenceSmsMs = millis();

      // Publish breach event to Cloud MQTT & Local Serial Gateway
      String breachPayload = "{";
      breachPayload += "\"device_imei\":\"" + deviceImei + "\",";
      breachPayload += "\"event_type\":\"GEOFENCE_BREACH\",";
      breachPayload += "\"distance_m\":" + String(distMeters, 1) + ",";
      breachPayload += "\"radius_m\":" + String(geofenceRadiusMeters, 1) + ",";
      breachPayload += "\"latitude\":" + String(gnss.latitude, 6) + ",";
      breachPayload += "\"longitude\":" + String(gnss.longitude, 6) + ",";
      breachPayload += "\"plate\":\"" + bikePlate + "\",";
      breachPayload += "\"alert_phone\":\"" + alertPhoneLocal + "\"";
      breachPayload += "}";

      Serial.println("[MQTT GEOFENCE -> " + TOPIC_TAMPER + "]: " + breachPayload);

      // 1. Dispatch Offline GSM GPS SMS FIRST while 2G Class-B radio is idle (repeats every 10 mins)
      sendGeofenceSmsAlert(gnss.latitude, gnss.longitude, distMeters, gnss.speedKmh, false);

      // 2. Then publish breach event to Cellular MQTT
      sim7000PublishMqtt(TOPIC_TAMPER, breachPayload);
    }
  } else if (isOutsidePerimeter && distMeters <= (geofenceRadiusMeters * 0.90)) {
    // Returned safely inside perimeter (with 10% hysteresis)
    isOutsidePerimeter = false;
    lastGeofenceSmsMs = 0;
    Serial.println("[GEOFENCE] ✅ Motorcycle returned inside marked perimeter (" +
                   String(distMeters, 0) + "m <= " + String(geofenceRadiusMeters, 0) + "m).");
  }
}

/**
 * Processes asynchronous URC lines from SIM7000G (e.g. +SMSUB incoming MQTT commands)
 */
void checkModemURC() {
  while (SerialModem.available()) {
    char c = (char)SerialModem.read();
    if (c == '\n') {
      modemUrcBuffer.trim();
      if (modemUrcBuffer.length() > 0) {
        handleUrcLine(modemUrcBuffer);
      }
      modemUrcBuffer = "";
    } else if (c != '\r') {
      modemUrcBuffer += c;
    }
  }
}

void handleIncomingSmsCommand(const String& sender, const String& smsText) {
  String cmd = smsText;
  cmd.trim();
  cmd.toUpperCase();
  Serial.println("[SMS RX <- " + sender + "]: " + cmd);

  // Accept offline SMS commands from owner's registered phone (synced via NVS!)
  if (!isAuthorizedOwnerSender(sender)) {
    Serial.println("[SMS RX] Ignored SMS from unauthorized number: " + sender + " (Owner: " + alertPhoneIntl + ")");
    return;
  }

  if (cmd.indexOf("STOP") >= 0 || cmd.indexOf("LOCK") >= 0 || cmd.indexOf("CUT") >= 0) {
    isImmobilized = true;
    localAlarmActive = false;
    digitalWrite(PIN_ALARM_BUZZER, LOW);
    digitalWrite(PIN_RELAY_IMMOBILIZER, HIGH);
    digitalWrite(PIN_BLUE_LED, HIGH);
    sendAck("ENGINE_CUT", "EXECUTED");
    String reply = "[BODA " + bikePlate + "]\nENGINE IMMOBILIZED OFFLINE!\nhttps://maps.google.com/?q=" +
                   String(gnss.latitude, 6) + "," + String(gnss.longitude, 6);
    sendDirectSimSms(reply, "Immobilize Confirm");
  } else if (cmd.indexOf("START") >= 0 || cmd.indexOf("UNLOCK") >= 0 || cmd.indexOf("RESTORE") >= 0) {
    isImmobilized = false;
    localAlarmActive = false;
    digitalWrite(PIN_ALARM_BUZZER, LOW);
    digitalWrite(PIN_RELAY_IMMOBILIZER, LOW);
    digitalWrite(PIN_BLUE_LED, LOW);
    sendAck("ENGINE_RESTORE", "EXECUTED");
    String reply = "[BODA " + bikePlate + "]\nENGINE RESTORED OFFLINE!\nhttps://maps.google.com/?q=" +
                   String(gnss.latitude, 6) + "," + String(gnss.longitude, 6);
    sendDirectSimSms(reply, "Restore Confirm");
  } else if (cmd.indexOf("GPS") >= 0 || cmd.indexOf("LOC") >= 0 || cmd.indexOf("STATUS") >= 0) {
    String reply = "[BODA " + bikePlate + " v" + String(FIRMWARE_VERSION) + "]\nIMEI: " + deviceImei +
                   "\nFix: " + String(gnss.hasFix ? "3D LOCK" : "LAST KNOWN") +
                   " | Spd: " + String(gnss.speedKmh, 1) + "km/h\nhttps://maps.google.com/?q=" +
                   String(gnss.latitude, 6) + "," + String(gnss.longitude, 6);
    sendDirectSimSms(reply, "Live Location");
  }
}

void handleUrcLine(const String& line) {
  if (awaitingIncomingSmsBody) {
    awaitingIncomingSmsBody = false;
    handleIncomingSmsCommand(incomingSmsSender, line);
    incomingSmsSender = "";
    return;
  }

  if (line.startsWith("+CMT:")) {
    if (isPowerSaveSleep) exitSmartSleepMode("INCOMING_SMS");
    int q1 = line.indexOf('"');
    int q2 = line.indexOf('"', q1 + 1);
    incomingSmsSender = (q1 >= 0 && q2 > q1) ? line.substring(q1 + 1, q2) : "";
    awaitingIncomingSmsBody = true;
    return;
  }

  if (line.startsWith("+SMSUB:")) {
    if (isPowerSaveSleep) exitSmartSleepMode("MQTT_COMMAND");
    int firstComma = line.indexOf(',');
    if (firstComma > 0) {
      String topicPart = line.substring(7, firstComma);
      topicPart.trim();
      topicPart.replace("\"", "");

      String msgPart = line.substring(firstComma + 1);
      msgPart.trim();
      if (msgPart.startsWith("\"") && msgPart.endsWith("\"") && msgPart.length() >= 2) {
        msgPart = msgPart.substring(1, msgPart.length() - 1);
      }

      byte buf[256];
      unsigned int len = min((unsigned int)msgPart.length(), (unsigned int)(sizeof(buf) - 1));
      memcpy(buf, msgPart.c_str(), len);
      onMqttCommandReceived((char*)topicPart.c_str(), buf, len);
    }
  } else if (line.indexOf("+APP PDP: DEACTIVE") >= 0) {
    pdpActive = false;
    mqttConnected = false;
  }
}

/**
 * Sends an AT command to SIM7000G over UART1 (RX=26, TX=27) while preserving +SMSUB URCs
 */
String sendAT(const String& cmd, uint32_t timeoutMs) {
  checkModemURC();
  lastModemRfActivityMs = millis();
  SerialModem.println(cmd);

  String resp = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (SerialModem.available()) {
      resp += (char)SerialModem.read();
    }
    if (resp.indexOf("\r\nOK\r\n") >= 0 || resp.indexOf("\r\nERROR\r\n") >= 0 ||
        resp.indexOf("+CME ERROR:") >= 0 || resp.indexOf("+CMS ERROR:") >= 0) {
      break;
    }
    delay(5);
  }

  int subIdx = resp.indexOf("+SMSUB:");
  if (subIdx >= 0) {
    int endLine = resp.indexOf('\n', subIdx);
    String urc = (endLine > subIdx) ? resp.substring(subIdx, endLine) : resp.substring(subIdx);
    urc.trim();
    handleUrcLine(urc);
  }

  return resp;
}

String getCsvField(const String& csv, int targetIndex) {
  int currentIndex = 0;
  int startPos = 0;
  int len = csv.length();
  for (int i = 0; i <= len; i++) {
    if (i == len || csv.charAt(i) == ',') {
      if (currentIndex == targetIndex) {
        String f = csv.substring(startPos, i);
        f.trim();
        return f;
      }
      currentIndex++;
      startPos = i + 1;
    }
  }
  return "";
}

/**
 * Powers on SIM7000G via GPIO 4 (PWRKEY), enables Uganda 2G GSM, SMS Text Mode & GNSS
 */
bool initSim7000G() {
  pinMode(PIN_MODEM_DTR, OUTPUT);
  digitalWrite(PIN_MODEM_DTR, LOW);

  const uint32_t bauds[] = {115200, 9600};
  bool awake = false;

  for (uint32_t b : bauds) {
    SerialModem.begin(b, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
    delay(80);
    if (sendAT("AT", 400).indexOf("OK") >= 0) {
      awake = true;
      Serial.println("[+] SIM7000G detected at " + String(b) + " baud.");
      break;
    }
  }

  if (!awake) {
    Serial.println("[MODEM] Powering on LILYGO T-SIM7000G (PWRKEY GPIO 4)...");
    pinMode(PIN_MODEM_PWR, OUTPUT);
    digitalWrite(PIN_MODEM_PWR, LOW);
    delay(100);
    digitalWrite(PIN_MODEM_PWR, HIGH);
    delay(1100);
    digitalWrite(PIN_MODEM_PWR, LOW);
    delay(3500);

    for (uint32_t b : bauds) {
      SerialModem.begin(b, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
      delay(100);
      for (int i = 0; i < 8; i++) {
        if (sendAT("AT", 500).indexOf("OK") >= 0) {
          awake = true;
          Serial.println("[+] SIM7000G booted at " + String(b) + " baud.");
          break;
        }
        delay(300);
      }
      if (awake) break;
    }
  }

  if (!awake) {
    Serial.println("[-] Warning: SIM7000G did not respond! Check onboard power switch.");
    return false;
  }

  sendAT("ATE0", 500);
  sendAT("AT+CMEE=2", 500);

  String gsn = sendAT("AT+GSN", 1000);
  gsn.replace("OK", "");
  gsn.trim();
  if (gsn.length() >= 14 && gsn.length() <= 16) {
    hardwareImei = gsn;
    nvs.putString("auto_imei", hardwareImei);
    if (!imeiCustomLocked) {
      deviceImei = hardwareImei;
    }
  }

  // Configure 2G GSM/EDGE mode (CNMP=13) & MTN Uganda Offline GSM SMS stack
  sendAT("AT+CNMP=13", 1500);
  sendAT("AT+CMGF=1", 800);
  sendAT("AT+CSCS=\"GSM\"", 800);
  sendAT("AT+CSMP=17,167,0,0", 800);
  sendAT("AT+CGSMS=3", 800);
  sendAT("AT+CNMI=2,2,0,0,0", 800);

  String cpin = sendAT("AT+CPIN?", 1000);
  if (cpin.indexOf("READY") >= 0) {
    Serial.println("[+] SIM7000G SIM Card Ready (+CPIN: READY).");
  }

  // Enable active GPS antenna power rail
  sendAT("AT+SGPIO=0,4,1,1", 1000);
  sendAT("AT+CGPIO=0,48,1,1", 1000);

  // Enable internal GNSS engine & multi-constellation tracking
  if (sendAT("AT+CGNSPWR=1", 1500).indexOf("OK") >= 0) {
    gnssEnabled = true;
    sendAT("AT+CGNSMOD=1,1,1,1", 1000);
    Serial.println("[+] SIM7000G GNSS/GPS engine powered ON.");
  }

  return true;
}

/**
 * Opens MTN Uganda GPRS PDP Context (AT+CNACT) & connects native SIM7000G MQTT client
 */
bool connectCellularMqtt() {
  String creg = sendAT("AT+CREG?", 800);
  if (creg.indexOf(",1") < 0 && creg.indexOf(",5") < 0) {
    Serial.println("[CELLULAR] Waiting for GSM network registration...");
    return false;
  }

  String cops = sendAT("AT+COPS?", 1000);
  int q1 = cops.indexOf('"');
  int q2 = cops.indexOf('"', q1 + 1);
  if (q1 >= 0 && q2 > q1) {
    gnss.carrier = cops.substring(q1 + 1, q2);
  }

  String cnact = sendAT("AT+CNACT?", 1000);
  if (cnact.indexOf("+CNACT: 1,\"0.0.0.0\"") >= 0 || cnact.indexOf("+CNACT: 1,") < 0) {
    Serial.println("[CELLULAR] Activating GPRS PDP bearer (APN: " + String(CELLULAR_APN) + ")...");
    sendAT("AT+CGDCONT=1,\"IP\",\"" + String(CELLULAR_APN) + "\"", 1000);
    sendAT("AT+CNACT=1,\"" + String(CELLULAR_APN) + "\"", 5000);
    delay(800);
    cnact = sendAT("AT+CNACT?", 1000);
  }

  if (cnact.indexOf("+CNACT: 1,\"") >= 0 && cnact.indexOf("\"0.0.0.0\"") < 0) {
    pdpActive = true;
    cnact.replace("OK", "");
    cnact.trim();
    Serial.println("[+] Cellular IP Bearer Active: " + cnact);
  } else {
    pdpActive = false;
    mqttConnected = false;
    return false;
  }

  String smstate = sendAT("AT+SMSTATE?", 800);
  if (smstate.indexOf("+SMSTATE: 1") >= 0) {
    mqttConnected = true;
    return true;
  }

  Serial.println("[CELLULAR MQTT] Connecting SIM7000G to " + String(MQTT_SERVER) + ":" + String(MQTT_PORT) + "...");
  sendAT("AT+SMDISC", 1000);
  sendAT("AT+SMCONF=\"CLIENTID\",\"boda_" + hardwareImei + "\"", 1000);
  sendAT("AT+SMCONF=\"URL\",\"" + String(MQTT_SERVER) + "\"," + String(MQTT_PORT), 1000);
  sendAT("AT+SMCONF=\"KEEPTIME\",60", 800);
  sendAT("AT+SMCONF=\"CLEANSS\",1", 800);

  String conn = sendAT("AT+SMCONN", 10000);
  if (conn.indexOf("OK") >= 0) {
    mqttConnected = true;
    sendAT("AT+SMSUB=\"" + TOPIC_COMMAND + "\",1", 2000);
    if (TOPIC_COMMAND_HW != TOPIC_COMMAND) {
      sendAT("AT+SMSUB=\"" + TOPIC_COMMAND_HW + "\",1", 2000);
    }
    sendAT("AT+SMSUB=\"boda/ota/broadcast\",1", 2000);
    Serial.println("[+] Cellular MQTT Connected & Subscribed to " + TOPIC_COMMAND + " + boda/ota/broadcast");
    return true;
  }

  mqttConnected = false;
  return false;
}

/**
 * Publishes payload over SIM7000G native cellular MQTT (AT+SMPUB)
 */
bool sim7000PublishMqtt(const String& topic, const String& payload) {
  if (!mqttConnected) return false;

  String cmd = "AT+SMPUB=\"" + topic + "\"," + String(payload.length()) + ",0,0";
  SerialModem.println(cmd);

  unsigned long start = millis();
  String promptBuf = "";
  bool gotPrompt = false;
  while (millis() - start < 1500) {
    while (SerialModem.available()) {
      char c = (char)SerialModem.read();
      promptBuf += c;
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
    if (gotPrompt || promptBuf.indexOf("ERROR") >= 0) break;
    delay(5);
  }

  if (!gotPrompt) {
    mqttConnected = false;
    return false;
  }

  SerialModem.print(payload);
  lastModemRfActivityMs = millis();
  String pubResp = "";
  start = millis();
  while (millis() - start < 3000) {
    while (SerialModem.available()) {
      pubResp += (char)SerialModem.read();
    }
    if (pubResp.indexOf("OK") >= 0) return true;
    if (pubResp.indexOf("ERROR") >= 0) {
      mqttConnected = false;
      return false;
    }
    delay(5);
  }
  return false;
}

/**
 * Queries SIM7000G AT+CGNSINF and AT+CSQ for live GPS coordinates & GSM signal
 */
void updateGnssAndSignal() {
  String csqResp = sendAT("AT+CSQ", 600);
  int csqIdx = csqResp.indexOf("+CSQ:");
  if (csqIdx >= 0) {
    int commaIdx = csqResp.indexOf(',', csqIdx);
    if (commaIdx > csqIdx) {
      String rssiStr = csqResp.substring(csqIdx + 5, commaIdx);
      rssiStr.trim();
      int rssi = rssiStr.toInt();
      if (rssi >= 0 && rssi <= 31) {
        gnss.gsmCsq = rssi;
      }
    }
  }

  String raw = sendAT("AT+CGNSINF", 1000);
  int idx = raw.indexOf("+CGNSINF:");
  if (idx < 0) return;

  int endIdx = raw.indexOf('\n', idx);
  String line = (endIdx > idx) ? raw.substring(idx + 9, endIdx) : raw.substring(idx + 9);
  line.trim();

  String fixStatus = getCsvField(line, 1);
  String latStr    = getCsvField(line, 3);
  String lngStr    = getCsvField(line, 4);
  String speedStr  = getCsvField(line, 6);
  String courseStr = getCsvField(line, 7);
  String satsView  = getCsvField(line, 14);
  String satsUsed  = getCsvField(line, 15);
  String cn0Max    = getCsvField(line, 18);

  gnss.satellitesView = satsView.length() ? satsView.toInt() : 0;
  gnss.satellitesUsed = satsUsed.length() ? satsUsed.toInt() : 0;
  gnss.cn0Max         = cn0Max.length() ? cn0Max.toInt() : 0;

  if (fixStatus == "1" && latStr.length() > 0 && lngStr.length() > 0) {
    gnss.hasFix     = true;
    gnss.latitude   = latStr.toDouble();
    gnss.longitude  = lngStr.toDouble();
    gnss.speedKmh   = speedStr.length() ? speedStr.toFloat() : 0.0f;
    gnss.headingDeg = courseStr.length() ? courseStr.toFloat() : 0.0f;

    // Keep board 100% awake whenever the motorcycle is in motion (speed >= 1.8 km/h)
    if (gnss.speedKmh >= MOTION_SPEED_THRESHOLD_KMH) {
      lastMotionOrVibMs = millis();
      if (isPowerSaveSleep) {
        exitSmartSleepMode("GPS_MOTION");
      }
    }
  } else {
    gnss.hasFix = false;
  }
}

/**
 * Publishes JSON Telemetry Packet to Cellular MQTT + USB Serial Gateway
 */
void publishTelemetryPayload() {
  bool hasFix = gnss.hasFix;
  int sats = hasFix ? gnss.satellitesUsed : gnss.satellitesView;

  double lat = gnss.latitude;
  double lng = gnss.longitude;
  float speed = (hasFix && !isPowerSaveSleep) ? gnss.speedKmh : 0.0f;
  float heading = hasFix ? gnss.headingDeg : 0.0f;

  int rawADC = analogRead(PIN_BATTERY_ADC);
  float batteryVoltage = (rawADC / 4095.0f) * 3.3f * ((100.0f + 22.0f) / 22.0f);
  if (batteryVoltage < 5.0f) {
    int rawOnboard = analogRead(PIN_ONBOARD_BAT_ADC);
    float onboardLiIon = (rawOnboard / 4095.0f) * 3.3f * 2.0f * 1.1f;
    batteryVoltage = (onboardLiIon > 2.5f) ? onboardLiIon : 12.6f;
  }
  // Pre-charge shared SAR ADC1 sample-and-hold cap back onto GPIO 32
  (void)analogRead(PIN_PIEZO_VIBRATION);

  bool ignition = !isImmobilized;

  String payload = "{";
  payload += "\"device_imei\":\"" + deviceImei + "\",";
  payload += "\"hardware_imei\":\"" + hardwareImei + "\",";
  payload += "\"board_type\":\"" + String(BOARD_TYPE) + "\",";
  payload += "\"fw_version\":\"" + String(FIRMWARE_VERSION) + "\",";
  payload += "\"plate\":\"" + bikePlate + "\",";
  payload += "\"owner_phone\":\"" + alertPhoneLocal + "\",";
  payload += "\"power_mode\":\"" + String(isPowerSaveSleep ? "SLEEP" : "ACTIVE") + "\",";
  payload += "\"latitude\":" + String(lat, 6) + ",";
  payload += "\"longitude\":" + String(lng, 6) + ",";
  payload += "\"speed\":" + String(speed, 1) + ",";
  payload += "\"heading\":" + String(heading, 1) + ",";
  payload += "\"battery_voltage\":" + String(batteryVoltage, 2) + ",";
  payload += "\"ignition_status\":" + String(ignition ? "true" : "false") + ",";
  payload += "\"is_immobilized\":" + String(isImmobilized ? "true" : "false") + ",";
  payload += "\"gsm_signal\":" + String(gnss.gsmCsq) + ",";
  payload += "\"satellites\":" + String(sats) + ",";
  payload += "\"gps_lock\":" + String(hasFix ? "true" : "false");
  payload += "}";

  // Keep SIM7000G DTR LOW (awake) so Cellular MQTT & Serial Gateway stay 100% live and online
  digitalWrite(PIN_MODEM_DTR, LOW);
  bool sentCell = sim7000PublishMqtt(TOPIC_TELEMETRY, payload);

  Serial.println("[MQTT TX -> " + TOPIC_TELEMETRY + "]: " + payload);
  Serial.print("[GPS STATUS] Carrier: ");
  Serial.print(gnss.carrier);
  Serial.print(" (CSQ ");
  Serial.print(gnss.gsmCsq);
  Serial.print("/31, CellMQTT: ");
  Serial.print(sentCell ? "LIVE" : "SERIAL-GW");
  Serial.print(") | Fix: ");
  Serial.print(hasFix ? "3D LOCK" : "SEARCHING");
  Serial.print(" | Sats: ");
  Serial.print(gnss.satellitesUsed);
  Serial.print("/");
  Serial.print(gnss.satellitesView);
  Serial.print(" | PiezoPeak: ");
  Serial.print(windowPeakPiezoDelta);
  Serial.print(" | Zone: ");
  Serial.print(isOutsidePerimeter ? "OUTSIDE!" : "OK");
  Serial.print(" | Lat: ");
  Serial.print(lat, 6);
  Serial.print(" | Lng: ");
  Serial.println(lng, 6);

  windowPeakPiezoDelta = 0;
}

/**
 * Median-of-3 ADC read on GPIO 32: rejects single-sample RF/SAR spikes while
 * preserving 100% amplitude of fast (1-3 kHz) piezoelectric ceramic pulses
 */
int readFilteredPiezoADC() {
  int a = analogRead(PIN_PIEZO_VIBRATION);
  int b = analogRead(PIN_PIEZO_VIBRATION);
  int c = analogRead(PIN_PIEZO_VIBRATION);
  int lo = min(a, min(b, c));
  int hi = max(a, max(b, c));
  return (a + b + c) - lo - hi;
}

/**
 * Calibrates resting ADC baseline (~475 counts across 1M-ohm bleed) & ambient noise floor
 */
void calibratePiezoSensor() {
  (void)analogRead(PIN_PIEZO_VIBRATION);
  long sum = 0;
  const int samples = 250;
  for (int i = 0; i < samples; i++) {
    sum += readFilteredPiezoADC();
    delay(1);
  }
  piezoBaseline = (float)sum / (float)samples;

  int maxNoise = 0;
  for (int i = 0; i < samples; i++) {
    int d = abs(readFilteredPiezoADC() - (int)roundf(piezoBaseline));
    if (d > maxNoise) maxNoise = d;
    delay(1);
  }
  piezoNoisePeak = constrain(maxNoise, 5, 78);

  int dynamicThresh = piezoNoisePeak + 20;
  piezoTamperThreshold = constrain(max(95, dynamicThresh), 95, 112);

  Serial.println("[+] Analog Piezo Sensor calibrated on GPIO 32 (Baseline: " +
                 String(piezoBaseline, 1) + ", NoiseFloor: " + String(piezoNoisePeak) +
                 ", TriggerThreshold: " + String(piezoTamperThreshold) + "/4095)");
}

/**
 * High-speed AC-coupled peak detector for Analog Piezoelectric Ceramic Sensor (GPIO 32)
 */
void checkPiezoVibrationSensor() {
  if (millis() - lastModemRfActivityMs < 140) {
    return;
  }

  int filteredRaw = readFilteredPiezoADC();
  int baseInt = (int)roundf(piezoBaseline);
  int delta = abs(filteredRaw - baseInt);

  if (delta > windowPeakPiezoDelta) {
    windowPeakPiezoDelta = delta;
  }

  // Continuous non-locking DC baseline tracker (prevents baseline freeze on 1M-ohm bias drift)
  if (delta < (piezoTamperThreshold * 7 / 10)) {
    piezoBaseline = (0.992f * piezoBaseline) + (0.008f * (float)filteredRaw);
  } else {
    piezoBaseline = (0.9995f * piezoBaseline) + (0.0005f * (float)filteredRaw);
  }

  // Sensitive Vibration Wake Detector: Even light frame/engine vibration keeps the bike awake
  // or immediately wakes it from Stationary Smart Sleep Mode (< 2ms)!
  int vibWakeThreshold = max(25, piezoNoisePeak + 12);
  if (delta >= vibWakeThreshold) {
    lastMotionOrVibMs = millis();
    if (isPowerSaveSleep) {
      exitSmartSleepMode("PIEZO_VIBRATION");
    }
  }

  if (delta >= piezoTamperThreshold && (millis() - lastTamperAlertMs >= TAMPER_COOLDOWN_MS)) {
    lastTamperAlertMs = millis();
    float norm = (float)(delta - piezoTamperThreshold) / 300.0f;
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    float equivGForce = 3.6f + (norm * 8.4f);
    triggerTamperAlert(equivGForce, delta, "PIEZO_GPIO32");
  }
}

/**
 * Checks for physical tamper / movement shocks using optional MPU6050
 */
void checkTamperSensor() {
  if (!mpuAvailable) return;

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float totalAcc = sqrt(a.acceleration.x * a.acceleration.x +
                        a.acceleration.y * a.acceleration.y +
                        a.acceleration.z * a.acceleration.z) / 9.80665;

  float deltaG = abs(totalAcc - previousGForce);
  previousGForce = totalAcc;

  // Wake from Smart Sleep on any motorcycle tilt or movement (>= 0.18G)
  if (deltaG >= 0.18f) {
    lastMotionOrVibMs = millis();
    if (isPowerSaveSleep) {
      exitSmartSleepMode("MPU6050_MOTION");
    }
  }

  if (deltaG > TAMPER_THRESHOLD_G && (millis() - lastTamperAlertMs >= TAMPER_COOLDOWN_MS)) {
    lastTamperAlertMs = millis();
    triggerTamperAlert(deltaG, 0, "MPU6050");
  }
}

/**
 * Smart Motion & Vibration-Wake Battery Saver Engine:
 *   - Stays 100% awake when motorcycle is in motion (speed >= 1.8 km/h) or vibrating.
 *   - Enters low-power sleep after 3 minutes of zero motion & zero vibration.
 *   - Immediately wakes (< 2ms) the instant vibration (GPIO 32 / MPU6050), motion, or a command is sensed.
 */
void manageSmartSleepMode() {
  // Always stay awake if moving, ringing alarm, on a voice call, or running OTA
  if (gnss.hasFix && gnss.speedKmh >= MOTION_SPEED_THRESHOLD_KMH) {
    lastMotionOrVibMs = millis();
    if (isPowerSaveSleep) {
      exitSmartSleepMode("GPS_MOTION");
    }
    return;
  }

  if (localAlarmActive || voiceCallActive || pendingOtaExecution) {
    lastMotionOrVibMs = millis();
    return;
  }

  if (!isPowerSaveSleep && (millis() - lastMotionOrVibMs >= STATIONARY_SLEEP_TIMEOUT_MS)) {
    enterSmartSleepMode();
  }
}

void enterSmartSleepMode() {
  if (isPowerSaveSleep) return;
  isPowerSaveSleep = true;

  Serial.println("[SLEEP MODE] 🌙 Motorcycle stationary for 3 mins (0 vibration, 0 motion) -> Entering Smart Battery-Saver Mode (80MHz CPU, Live MQTT Online)...");
  sendAck("POWER_MODE", "SLEEP_STATIONARY_BATTERY_SAVER");

  if (modemReady) {
    // Pause high-current GPS/GNSS engine & active antenna while parked still (~45mA saved)
    sendAT("AT+CGNSPWR=0", 1000);
    sendAT("AT+SGPIO=0,4,1,0", 800);
    gnssEnabled = false;

    // Keep SIM7000G DTR LOW (awake) so Cellular MQTT (+SMSUB Engine Cut) & Online Heartbeat stay 100% instant!
    digitalWrite(PIN_MODEM_DTR, LOW);
  }

  // Scale ESP32 CPU down to 80 MHz while keeping Piezo Vibration ADC (GPIO 32) & 12V Sense (GPIO 34) active!
  setCpuFrequencyMhz(80);
}

void exitSmartSleepMode(const char* wakeReason) {
  lastMotionOrVibMs = millis();
  if (!isPowerSaveSleep) return;
  isPowerSaveSleep = false;

  // Restore full 240 MHz ESP32 CPU speed immediately (< 1ms)
  setCpuFrequencyMhz(240);

  Serial.println("[WAKE UP] ⚡ Waking from Smart Sleep — Reason: " + String(wakeReason));

  if (modemReady) {
    // Pull DTR LOW to wake SIM7000G UART immediately
    digitalWrite(PIN_MODEM_DTR, LOW);
    delay(40);
    sendAT("AT+CSCLK=0", 800);

    // Re-enable Active GPS Antenna & Multi-Constellation GNSS Engine for 3s live tracking
    sendAT("AT+SGPIO=0,4,1,1", 800);
    sendAT("AT+CGPIO=0,48,1,1", 800);
    if (sendAT("AT+CGNSPWR=1", 1200).indexOf("OK") >= 0) {
      gnssEnabled = true;
    }
  }

  sendAck("POWER_MODE", "AWAKE_" + String(wakeReason));
  lastTelemetryTime = 0; // Force immediate telemetry transmission
}

/**
 * High-speed 12V Motorcycle Battery Removal / Wire-Cut Monitor (GPIO 34):
 *   - Samples the 100k/22k divider every 250ms (even while in Smart Sleep Mode).
 *   - Once 12V battery is present (>= 8.5V), any removal or cable cut (< 5.0V)
 *     immediately wakes the board from Smart Sleep and triggers the full Tamper Alert
 *     while the LILYGO continues running on its onboard 18650 backup battery!
 */
void checkMain12vBatteryCut() {
  if (millis() - lastBatteryCheckMs < 250) return;
  lastBatteryCheckMs = millis();

  int rawADC = analogRead(PIN_BATTERY_ADC);
  // Pre-charge shared SAR ADC1 sample-and-hold cap back onto GPIO 32
  (void)analogRead(PIN_PIEZO_VIBRATION);

  float mainBattV = (rawADC / 4095.0f) * 3.3f * ((100.0f + 22.0f) / 22.0f);

  if (mainBattV >= 8.5f) {
    if (!main12vArmed) {
      main12vArmed = true;
      Serial.println("[12V SENSE] 🔋 Main 12V Motorcycle Battery detected (" + String(mainBattV, 2) + "V) -> Wire-Cut/Removal protection ARMED.");
    }
    if (mainPowerCutAlerted && mainBattV >= 9.2f) {
      mainPowerCutAlerted = false;
      if (isPowerSaveSleep) exitSmartSleepMode("12V_BATTERY_RECONNECTED");
      Serial.println("[12V SENSE] ✅ Main 12V Motorcycle Battery reconnected (" + String(mainBattV, 2) + "V).");
    }
  } else if (main12vArmed && mainBattV < 5.0f && !mainPowerCutAlerted) {
    mainPowerCutAlerted = true;
    lastMotionOrVibMs = millis();
    if (isPowerSaveSleep) {
      exitSmartSleepMode("12V_BATTERY_REMOVED");
    }
    Serial.println("[!] 🚨 12V MOTORCYCLE BATTERY REMOVED / WIRE CUT (GPIO 34 dropped to " + String(mainBattV, 2) + "V)! Running on 18650 backup cell...");
    triggerTamperAlert(5.0f, 500, "12V_BATTERY_REMOVED");
    placeTamperVoiceCall("12V_BATTERY_REMOVED");
  }
}

/**
 * Triggers 100% Offline Local Siren/Strobe + Direct MTN SIM Tamper SMS to +256748802162
 */
void triggerTamperAlert(float gForce, int piezoAdcDelta, const char* sourceName) {
  Serial.println("[!] TAMPER DETECTED (" + String(sourceName) + "): Shock " +
                 String(gForce, 2) + "G | Piezo ADC Delta: " + String(piezoAdcDelta) + "/4095!");

  // 1. Activate Offline Local Buzzer/Siren (GPIO 14) + Rapid Blue LED Warning Strobe
  triggerLocalAlarm();

  bool isBatCut = String(sourceName).indexOf("12V_BATTERY") >= 0;
  String evtType = isBatCut ? "BATTERY_DISCONNECTED" : "TAMPER_DETECTED";

  String tamperPayload = "{";
  tamperPayload += "\"device_imei\":\"" + deviceImei + "\",";
  tamperPayload += "\"hardware_imei\":\"" + hardwareImei + "\",";
  tamperPayload += "\"event_type\":\"" + evtType + "\",";
  tamperPayload += "\"sensor\":\"" + String(sourceName) + "\",";
  tamperPayload += "\"g_force\":" + String(gForce, 2) + ",";
  tamperPayload += "\"piezo_adc\":" + String(piezoAdcDelta) + ",";
  tamperPayload += "\"latitude\":" + String(gnss.latitude, 6) + ",";
  tamperPayload += "\"longitude\":" + String(gnss.longitude, 6) + ",";
  tamperPayload += "\"plate\":\"" + bikePlate + "\",";
  tamperPayload += "\"alert_phone\":\"" + alertPhoneLocal + "\",";
  tamperPayload += "\"timestamp\":" + String(millis());
  tamperPayload += "}";

  // 2. Emit over Serial immediately (<2ms)
  Serial.println("[MQTT TAMPER -> " + TOPIC_TAMPER + "]: " + tamperPayload);

  // 3. Send Instant 100% Offline Direct Tamper SMS FIRST (before Class-B GPRS MQTT TBF burst!)
  sendTamperSmsAlert(gForce, piezoAdcDelta, sourceName);

  // 4. Then publish over MTN Uganda Cellular MQTT for live map & server sync
  sim7000PublishMqtt(TOPIC_TAMPER, tamperPayload);

  // 5. Place Emergency Voice Call to registered owner phone
  placeTamperVoiceCall(sourceName);
}

/**
 * Handles Incoming Command from Cellular MQTT (+SMSUB) or USB Serial Gateway
 */
void onMqttCommandReceived(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.println("[MQTT RX <- " + String(topic) + "]: " + message);

  if (message.indexOf("OTA_UPDATE") >= 0) {
    String targetBoard = extractJsonField(message, "board");
    if (targetBoard.length() > 0 && targetBoard != String(BOARD_TYPE)) {
      Serial.println("[OTA] Ignoring firmware broadcast for board='" + targetBoard + "' (this MCU is '" + String(BOARD_TYPE) + "').");
      return;
    }
    String url = extractJsonField(message, "url");
    if (url.length() > 0) {
      pendingOtaUrl       = url;
      pendingOtaVersion   = extractJsonField(message, "version");
      pendingOtaMd5       = extractJsonField(message, "md5");
      pendingOtaForce     = (message.indexOf("\"force\":true") >= 0);
      pendingOtaExecution = true;
    }
    return;
  }

  // Automatic Customer Sign-Up / Profile Sync ("cmd":"OWNER_SYNC") -> Saved Permanently in NVS!
  if (message.indexOf("OWNER_SYNC") >= 0) {
    String syncPlate = extractJsonField(message, "plate");
    String syncPhone = extractJsonField(message, "owner_phone");
    String syncEmail = extractJsonField(message, "owner_email");

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

  // Factory / USB / Remote NVS Provisioning Commands (SET_IMEI, CLEAR_IMEI, SET_PHONE, SET_PLATE, SET_EMAIL)
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

  if (message.startsWith("GEOFENCE_SET:")) {
    applyGeofenceCommand(message.substring(13));
  } else if (message.indexOf("ENGINE_CUT") >= 0) {
    isImmobilized = true;
    digitalWrite(PIN_RELAY_IMMOBILIZER, HIGH);
    digitalWrite(PIN_BLUE_LED, HIGH);
    Serial.println("[ACTION] ⛔ ENGINE IMMOBILIZED! Relay activated | Blue LED: OFF.");
    sendAck("ENGINE_CUT", "EXECUTED");
  } else if (message.indexOf("ENGINE_RESTORE") >= 0) {
    isImmobilized = false;
    digitalWrite(PIN_RELAY_IMMOBILIZER, LOW);
    digitalWrite(PIN_BLUE_LED, LOW);
    Serial.println("[ACTION] ✅ ENGINE RESTORED! Relay deactivated | Blue LED: ON (Running).");
    sendAck("ENGINE_RESTORE", "EXECUTED");
  }
}

/**
 * Sends Acknowledgment back to Cloud Server (Cellular MQTT + USB Serial Gateway)
 */
void sendAck(String action, String status) {
  String ackPayload = "{";
  ackPayload += "\"device_imei\":\"" + deviceImei + "\",";
  ackPayload += "\"hardware_imei\":\"" + hardwareImei + "\",";
  ackPayload += "\"board_type\":\"" + String(BOARD_TYPE) + "\",";
  ackPayload += "\"fw_version\":\"" + String(FIRMWARE_VERSION) + "\",";
  ackPayload += "\"plate\":\"" + bikePlate + "\",";
  ackPayload += "\"command_action\":\"" + action + "\",";
  ackPayload += "\"status\":\"" + status + "\",";
  ackPayload += "\"ack_timestamp\":" + String(millis());
  ackPayload += "}";

  sim7000PublishMqtt(TOPIC_ACK, ackPayload);
  Serial.println("[MQTT ACK -> " + TOPIC_ACK + "]: " + ackPayload);
}

String extractJsonField(const String& json, const char* key) {
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

/**
 * Dual-Path Internet OTA Updater for LILYGO T-SIM7000G:
 *   1. Fast Wi-Fi HTTP Stream (if connected to Hotspot / Workshop Wi-Fi)
 *   2. Nationwide Cellular SIM7000G HTTP Client (AT+SHCONF / AT+SHREQ / AT+SHREAD)
 */
void executeLilygoInternetOta(const String& url, const String& targetVer, const String& expectedMd5) {
  Serial.println("==================================================================");
  Serial.println("[OTA] 🚀 LILYGO T-SIM7000G INTERNET FIRMWARE UPDATE TRIGGERED!");
  Serial.println("[OTA] Version : v" + targetVer + " (Current: v" + String(FIRMWARE_VERSION) + ")");
  Serial.println("[OTA] URL     : " + url);
  Serial.println("==================================================================");

  if (gnss.hasFix && gnss.speedKmh >= 2.0f) {
    Serial.println("[OTA SAFETY] ⛔ Motorcycle moving (" + String(gnss.speedKmh, 1) + " km/h)! Deferring OTA.");
    sendAck("OTA_DEFERRED_MOVING", "SPEED_" + String(gnss.speedKmh, 1));
    return;
  }

  if (!pendingOtaForce && targetVer.length() > 0 && targetVer == String(FIRMWARE_VERSION)) {
    sendAck("OTA_SKIPPED", "ALREADY_V" + String(FIRMWARE_VERSION));
    return;
  }

  sendAck("OTA_STARTED", "DOWNLOADING_V" + targetVer);

  bool ok = false;
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[OTA] Streaming .bin via Wi-Fi HTTPClient...");
    ok = runLilygoWifiHttpOta(url, expectedMd5);
  }
  if (!ok && modemReady) {
    Serial.println("[OTA] Streaming .bin via SIM7000G Cellular HTTP (AT+SHREQ / AT+SHREAD)...");
    ok = runSim7000CellularHttpOta(url, expectedMd5);
  }

  if (ok) {
    Serial.println("[OTA] ✅ LILYGO FLASH COMPLETE! Rebooting into v" + targetVer + "...");
    sendAck("OTA_SUCCESS", "INSTALLED_V" + targetVer);
    delay(800);
    ESP.restart();
  } else {
    Serial.println("[OTA] ❌ OTA Failed — preserving v" + String(FIRMWARE_VERSION) + " safely.");
    sendAck("OTA_FAILED", "ROLLED_BACK_V" + String(FIRMWARE_VERSION));
  }
}

bool runLilygoWifiHttpOta(const String& url, const String& expectedMd5) {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(25000);
  if (!http.begin(url)) return false;

  const char* hdrs[] = { "x-MD5" };
  http.collectHeaders(hdrs, 1);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  int totalLen = http.getSize();
  String md5 = expectedMd5.length() == 32 ? expectedMd5 : http.header("x-MD5");
  md5.trim();

  if (totalLen <= 0 || !Update.begin(totalLen)) {
    http.end();
    return false;
  }
  if (md5.length() == 32) Update.setMD5(md5.c_str());

  size_t written = Update.writeStream(*http.getStreamPtr());
  http.end();
  if (written != (size_t)totalLen) {
    Update.abort();
    return false;
  }
  return Update.end(true) && Update.isFinished();
}

bool runSim7000CellularHttpOta(const String& url, const String& expectedMd5) {
  // Parse origin ("http://host:port") and path ("/api/ota/download/lilygo_tsim7000g")
  int schemeEnd = url.indexOf("://");
  if (schemeEnd < 0) return false;
  int pathStart = url.indexOf('/', schemeEnd + 3);
  String origin = (pathStart > 0) ? url.substring(0, pathStart) : url;
  String reqPath = (pathStart > 0) ? url.substring(pathStart) : "/";

  // Pause MQTT client briefly so SIM7000G SHCONN HTTP engine gets dedicated socket
  sendAT("AT+SMDISC", 1000);
  mqttConnected = false;

  sendAT("AT+SHDISC", 800);
  sendAT("AT+SHCONF=\"URL\",\"" + origin + "\"", 1000);
  sendAT("AT+SHCONF=\"BODYLEN\",1024", 800);
  sendAT("AT+SHCONF=\"HEADERLEN\",350", 800);

  if (sendAT("AT+SHCONN", 12000).indexOf("OK") < 0) {
    Serial.println("[-] SIM7000G AT+SHCONN failed.");
    return false;
  }

  // Stream firmware in 2048-byte HTTP Range requests so SIM7000G RAM never overflows!
  const int CHUNK = 2048;
  uint8_t buf[CHUNK];
  int offset = 0;
  int totalBytes = -1;
  bool updateStarted = false;

  while (totalBytes < 0 || offset < totalBytes) {
    int endByte = offset + CHUNK - 1;
    sendAT("AT+SHCHEAD", 500);
    sendAT("AT+SHAHEAD=\"Range\",\"bytes=" + String(offset) + "-" + String(endByte) + "\"", 600);

    while (SerialModem.available()) SerialModem.read();
    SerialModem.println("AT+SHREQ=\"" + reqPath + "\",1");

    String shResp = "";
    unsigned long t0 = millis();
    while (millis() - t0 < 20000UL) {
      while (SerialModem.available()) shResp += (char)SerialModem.read();
      if (shResp.indexOf("+SHREQ:") >= 0 && shResp.indexOf('\n', shResp.indexOf("+SHREQ:")) >= 0) break;
      delay(15);
    }

    int srIdx = shResp.indexOf("+SHREQ:");
    if (srIdx < 0) {
      if (updateStarted) Update.abort();
      sendAT("AT+SHDISC", 800);
      return false;
    }

    String srLine = shResp.substring(srIdx + 7);
    srLine.trim();
    int status = getCsvField(srLine, 1).toInt();
    int chunkLen = getCsvField(srLine, 2).toInt();

    if ((status != 206 && status != 200) || chunkLen <= 0) {
      if (updateStarted) Update.abort();
      sendAT("AT+SHDISC", 800);
      return false;
    }

    if (!updateStarted) {
      // If server returned 200 full or 206 partial, initialize Update with UNKNOWN_SIZE if total unknown
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        sendAT("AT+SHDISC", 800);
        return false;
      }
      if (expectedMd5.length() == 32) Update.setMD5(expectedMd5.c_str());
      updateStarted = true;
    }

    while (SerialModem.available()) SerialModem.read();
    SerialModem.println("AT+SHREAD=0," + String(chunkLen));

    String hdr = "";
    t0 = millis();
    bool gotHdr = false;
    while (millis() - t0 < 8000UL) {
      if (SerialModem.available()) {
        char c = (char)SerialModem.read();
        hdr += c;
        int p = hdr.indexOf("+SHREAD:");
        if (p >= 0 && c == '\n' && hdr.indexOf('\n', p) >= 0) {
          gotHdr = true;
          break;
        }
      }
    }
    if (!gotHdr) {
      Update.abort();
      sendAT("AT+SHDISC", 800);
      return false;
    }

    int got = 0;
    t0 = millis();
    while (got < chunkLen && (millis() - t0 < 8000UL)) {
      if (SerialModem.available()) buf[got++] = (uint8_t)SerialModem.read();
    }
    if (got != chunkLen || Update.write(buf, got) != (size_t)got) {
      Update.abort();
      sendAT("AT+SHDISC", 800);
      return false;
    }

    offset += got;
    if (got < CHUNK) {
      totalBytes = offset; // Reached final chunk of .bin!
      break;
    }
  }

  sendAT("AT+SHDISC", 800);
  return Update.end(true) && Update.isFinished();
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
  String tail = alertPhoneLocal.length() >= 8
    ? alertPhoneLocal.substring(alertPhoneLocal.length() - 8)
    : alertPhoneLocal;
  return tail.length() > 0 && sender.indexOf(tail) >= 0;
}

void printIdentityCard() {
  String modeStr = imeiCustomLocked
    ? "CUSTOM NVS LOCKED"
    : ((hardwareImei != chipFallbackImei) ? "AUTO SIM7000G AT+GSN" : "AUTO ESP32 SILICON MAC");
  Serial.println("┌──────── BODA 360 PERMANENT BOARD IDENTITY (NVS) ────────┐");
  Serial.println("│ Active Device IMEI : " + deviceImei + " (" + modeStr + ")");
  Serial.println("│ Modem AT+GSN IMEI  : " + hardwareImei);
  Serial.println("│ ESP32 Chip Fallback: " + chipFallbackImei);
  Serial.println("│ Bound Bike Plate   : " + bikePlate);
  Serial.println("│ Owner Voice/SMS    : " + alertPhoneLocal + " (" + alertPhoneIntl + ")");
  Serial.println("│ Owner Account Email: " + (ownerEmail.length() ? ownerEmail : String("(awaiting customer sign-up)")));
  Serial.println("└─────────────────────────────────────────────────────────┘");
}
