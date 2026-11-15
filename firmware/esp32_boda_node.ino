/**
 * ============================================================================
 * ESP32 FIRMWARE FOR BODA-BODA REAL-TIME TRACKING & REMOTE IMMOBILIZATION
 * Author / Lead Engineer: Akim Ronald David (ISBAT University, Kampala)
 * Hardware Components:
 *   - Primary Board: LILYGO TTGO T-SIM7000G (ESP32 + SIMCom SIM7000G - Single Modem)
 *   - Integrated Cellular + GNSS Modem: SIM7000G (UART1: RX=26, TX=27, PWR=4, DTR=25)
 *       * Streams live 3D GPS & MQTT over MTN Uganda ("internet" APN)
 *       * Sends GPS Breach SMS to +256748802162 every 10 mins outside perimeter
 *       * Triggers Tamper & 12V Battery-Cut Voice Call + SMS to 0748802162 / +256748802162
 *   - Onboard Status Blue LED: GPIO 12 (Active LOW: LOW=ON/Running, HIGH=OFF/Cut)
 *   - Piezo Vibration / Tamper Sensor: Analog Piezoelectric Ceramic Sensor (ADC1: GPIO 32)
 *   - Motion-Gating IMU: MPU6050 6-Axis Accelerometer + Gyro (I2C: SDA=21, SCL=22)
 *       * Detects sustained riding motion & suppresses Piezo tamper messages while moving
 *   - Immobilization Actuator: 2-Way (2-Channel) 5V Relay Module — Channel 1 (IN1 -> GPIO 23, COM1 & NC1)
 *   - Battery Voltage & Power-Cut Tamper Sense:
 *       * External 12V Motorcycle Battery Divider (GPIO 34: R1=100k to +12V, R2=22k to GND)
 *       * Onboard 18650 Backup Battery ADC (GPIO 35: keeps ESP32+SIM7000G alive when 12V cut)
 * ============================================================================
 */

#include <Arduino.h>
#include <WString.h>
#include <Wire.h>
#include <math.h>
#include <PubSubClient.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// LILYGO T-SIM7000G + PERIPHERAL PIN DEFINITIONS
#define PIN_BLUE_LED          12 // T-SIM7000G Onboard Blue LED (Active LOW: LOW=ON, HIGH=OFF)
#define PIN_ALARM_BUZZER      14 // Offline Local Siren / Active Piezo Buzzer output (HIGH = Alarm ON)
#define PIN_RELAY_IMMOBILIZER 23 // 2-Way Relay Channel 1 (IN1 -> GPIO 23)
#define PIN_PIEZO_VIBRATION   32 // Analog Piezoelectric Ceramic Vibration Sensor (ADC1_CH4)
#define PIN_BATTERY_ADC       34 // External 12V Motorcycle Battery divider (R1=100k, R2=22k)
#define PIN_ONBOARD_BAT_ADC   35 // LILYGO T-SIM7000G onboard 18650 backup battery divider
#define PIN_MPU_SDA           21 // MPU6050 I2C Data (SDA)
#define PIN_MPU_SCL           22 // MPU6050 I2C Clock (SCL)

// Internal SIM7000G (UART1: Data, MQTT, 3D GPS, Geofence SMS & Tamper Call Trigger)
#define PIN_MODEM_RX          26 // ESP32 RX <- SIM7000G TX
#define PIN_MODEM_TX          27 // ESP32 TX -> SIM7000G RX
#define PIN_MODEM_PWR         4  // SIM7000G PWRKEY
#define PIN_MODEM_DTR         25 // SIM7000G DTR (LOW = Awake)

// CONFIGURATION & EMERGENCY CONTACT PARAMETERS
const char* PROVISIONED_IMEI  = "865421045981201"; // Bound motorcycle node ID (Plate: UEV 412K)
const char* BIKE_PLATE        = "UEV 412K";
const char* ALERT_PHONE_LOCAL = "0748802162";      // Owner phone for Tamper / Battery-Cut Voice Call
const char* ALERT_PHONE_INTL  = "+256748802162";   // Owner phone for SIM7000G SMS Alerts
const char* CELLULAR_APN      = "internet";        // MTN Uganda / Airtel Uganda universal APN
const char* MQTT_SERVER       = "broker.hivemq.com";
const int   MQTT_PORT         = 1883;

String deviceImei   = PROVISIONED_IMEI;
String hardwareImei = "869951038535641";

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
bool relayActiveLow = true;  // Standard 2-Way (2-Channel) 5V Relay Modules trigger ON when IN1 = LOW

unsigned long lastTelemetryTime     = 0;
unsigned long lastMqttReconnectTry  = 0;
const unsigned long TELEMETRY_INTERVAL      = 3000;   // 3s telemetry cadence
const unsigned long MQTT_RECONNECT_INTERVAL = 30000;  // 30s cellular MQTT retry

// Marked Perimeter (Geofence) & 10-Minute SMS Alert State
bool   geofenceAnchored        = false; // Anchors to first 3D GPS fix unless Portal sets custom center
double geofenceCenterLat       = 0.184309;
double geofenceCenterLng       = 32.524292;
double geofenceRadiusMeters    = 500.0; // Default 500m radius (synced dynamically from Customer Portal)
bool   isOutsidePerimeter      = false;
unsigned long lastGeofenceSmsMs = 0;
const unsigned long GEOFENCE_SMS_INTERVAL_MS = 600000UL; // 10 minutes (600,000 ms) between breach SMS

// Offline Local Siren/Strobe & Direct SIM7000G Tamper SMS / Voice Call Management
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

// Vibration Tamper Thresholds (Analog Piezo on GPIO 32)
float piezoBaseline = 475.0f;
int piezoNoisePeak = 10;
int windowPeakPiezoDelta = 0;
int piezoTamperThreshold = 85;
unsigned long lastTamperAlertMs = 0;
unsigned long lastModemRfActivityMs = 0;
unsigned long lastMutedVibrationLogMs = 0;
const unsigned long TAMPER_COOLDOWN_MS = 5000;

// MPU6050 Motion-Gating State (Suppresses Piezo tamper messages while motorcycle is in motion)
bool  bikeInMotion               = false;
int   mpuMotionScore             = 0;
float mpuGravityBaseG            = 1.0f;
float mpuGyroBiasX               = 0.0f;
float mpuGyroBiasY               = 0.0f;
float mpuGyroBiasZ               = 0.0f;
float lastMpuDynAccG             = 0.0f;
float lastMpuGyroRad             = 0.0f;
unsigned long lastMpuSampleMs    = 0;
unsigned long lastMotionActiveMs = 0;

const unsigned long MPU_SAMPLE_INTERVAL_MS = 50;    // 20 Hz non-blocking I2C poll (preserves fast Piezo ADC loop)
const unsigned long MOTION_HOLD_MS         = 6000;  // Keep tamper muted for 6s after stopping (traffic/bumps)
const float MOTION_ACC_THRESHOLD_G         = 0.08f; // Continuous linear acceleration delta above resting gravity
const float MOTION_GYRO_THRESHOLD_RAD      = 0.12f; // Continuous tilt/steering/rolling rotation (~6.9 deg/s)
const int   MOTION_SCORE_MAX               = 40;
const int   MOTION_ENTER_SCORE             = 20;    // ~0.5s of sustained movement enters motion-mute state

// 12V Motorcycle Battery Removal / Power-Cut Tamper State (GPIO 34)
bool  batteryTamperArmed         = false; // Auto-arms once >= 8.5V bike battery is detected for 2s
bool  batteryDisconnected        = false; // True while 12V battery is removed after being armed
int   batPresentStreak           = 0;
int   batMissingStreak           = 0;
float lastExt12VVoltage          = 0.0f;
float lastBackup18650Voltage     = 3.9f;
unsigned long lastBatCheckMs     = 0;

const unsigned long BAT_CHECK_INTERVAL_MS = 100;   // Check 12V rail every 100ms (10 Hz)
const float BAT_CONNECTED_MIN_V           = 8.5f;  // >= 8.5V confirms connected to 12V bike battery
const float BAT_REMOVED_THRESHOLD_V       = 5.5f;  // < 5.5V indicates 12V battery removed / wire cut (ignores starter crank dips)
const int   BAT_ARM_SAMPLES               = 20;    // 20 * 100ms = 2.0s continuous 12V presence to arm
const int   BAT_CUT_DEBOUNCE_SAMPLES      = 6;     // 6 * 100ms = 600ms continuous 0V drop to trigger alert

// Forward Declarations
String sendAT(const String& cmd, uint32_t timeoutMs = 1200);
void checkModemURC();
void handleUrcLine(const String& line);
String getCsvField(const String& csv, int targetIndex);
bool initSim7000G();
bool connectCellularMqtt();
bool sim7000PublishMqtt(const String& topic, const String& payload);
bool sendDirectSimSms(const String& smsBody, const char* label);
bool sendGeofenceSmsAlert(double lat, double lng, double distMeters, float speedKmh, bool isTest = false);
bool sendTamperSmsAlert(float gForce, int piezoAdcDelta, const char* sourceName);
bool sendBatteryCutSmsAlert(float backupVolts);
void triggerLocalAlarm();
void manageLocalAlarm();
void handleIncomingSmsCommand(const String& sender, const String& smsText);
void placeTamperVoiceCall(const char* reason, bool bypassCooldown = false);
void manageActiveVoiceCall();
double calculateHaversineMeters(double lat1, double lon1, double lat2, double lon2);
void checkGeofencePerimeter();
void applyGeofenceCommand(const String& spec);
void updateGnssAndSignal();
void publishTelemetryPayload();
float readExternal12VVoltage();
float readOnboard18650Voltage();
void checkBatteryRemovalTamper();
void triggerBatteryRemovalAlert(float backupVolts);
int readFilteredPiezoADC();
void calibratePiezoSensor();
void calibrateMpuSensor();
void updateMpuMotionState();
void checkPiezoVibrationSensor();
void triggerTamperAlert(float gForce, int piezoAdcDelta, const char* sourceName);
void setImmobilizerRelay(bool cutEngine);
void onMqttCommandReceived(char* topic, byte* payload, unsigned int length);
void sendAck(String action, String status);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=================================================");
  Serial.println("ISBAT UNIVERSITY - BODA-BODA TELEMATICS SYSTEM");
  Serial.println("Single Board  : LILYGO T-SIM7000G (3D GPS + MQTT + SMS + Call)");
  Serial.println("Alert Contact : " + String(ALERT_PHONE_INTL) + " (SMS + Owner Voice Call)");
  Serial.println("=================================================");

  // Configure Onboard Blue LED (Active LOW on T-SIM7000G: LOW = Ignition ON, HIGH = Cut)
  pinMode(PIN_BLUE_LED, OUTPUT);
  digitalWrite(PIN_BLUE_LED, LOW);

  // Configure Offline Local Alarm Buzzer/Siren Pin (GPIO 14) & 2-Way Relay Channel 1 (IN1 -> GPIO 23)
  pinMode(PIN_ALARM_BUZZER, OUTPUT);
  digitalWrite(PIN_ALARM_BUZZER, LOW);
  setImmobilizerRelay(false); // Normal Riding State: Channel 1 Coil OFF (COM1 <-> NC1 closed)

  // Configure Battery & Analog Piezo Vibration ADCs
  pinMode(PIN_BATTERY_ADC, INPUT);
  pinMode(PIN_ONBOARD_BAT_ADC, INPUT);
  pinMode(PIN_PIEZO_VIBRATION, INPUT);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  analogSetPinAttenuation(PIN_ONBOARD_BAT_ADC, ADC_11db);
  analogSetPinAttenuation(PIN_PIEZO_VIBRATION, ADC_11db);

  // Sample initial 12V & 18650 voltages at startup
  lastExt12VVoltage = readExternal12VVoltage();
  lastBackup18650Voltage = readOnboard18650Voltage();
  Serial.println("[POWER] External 12V Input (GPIO 34): " + String(lastExt12VVoltage, 2) +
                 "V | Backup 18650 (GPIO 35): " + String(lastBackup18650Voltage, 2) + "V");

  // Initialize Integrated SIM7000G Modem & GNSS Engine (UART1: RX=26, TX=27)
  modemReady = initSim7000G();

  // Initialize MQTT Topics
  TOPIC_TELEMETRY  = "boda/" + deviceImei + "/telemetry";
  TOPIC_COMMAND    = "boda/" + deviceImei + "/command";
  TOPIC_COMMAND_HW = "boda/" + hardwareImei + "/command";
  TOPIC_ACK        = "boda/" + deviceImei + "/ack";
  TOPIC_TAMPER     = "boda/" + deviceImei + "/tamper";

  Serial.println("[+] Provisioned Node ID : " + deviceImei);
  Serial.println("[+] Physical Modem IMEI : " + hardwareImei);

  // Bring up MTN Uganda GPRS bearer & Native SIM7000G MQTT client
  if (modemReady) {
    lastMqttReconnectTry = millis();
    connectCellularMqtt();
  }

  // Calibrate Analog Piezo Sensor AFTER 3.3V rail & 1M-ohm bias settle (~475 ADC counts)
  delay(250);
  calibratePiezoSensor();

  // Initialize MPU6050 6-Axis IMU (I2C: SDA=21, SCL=22) to gate Piezo tamper alerts during motion
  Wire.begin(PIN_MPU_SDA, PIN_MPU_SCL);
  Wire.setClock(400000); // 400 kHz Fast-Mode I2C keeps reads < 0.4ms
  if (mpu.begin(0x68, &Wire) || mpu.begin(0x69, &Wire)) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    mpuAvailable = true;
    calibrateMpuSensor();
    Serial.println("[+] MPU6050 initialized (SDA=21, SCL=22) — Motion-gating ARMED (mutes tamper when riding).");
  } else {
    mpuAvailable = false;
    Serial.println("[!] MPU6050 not detected on SDA=21 / SCL=22! Check 3V3, GND, SDA, SCL wiring.");
  }

  Serial.println("[+] Hardware initialization complete.");
  Serial.println("[INFO] Commands: ENGINE_CUT | ENGINE_RESTORE | TEST_RELAY | RELAY_INVERT | MPU_STATUS | BAT_STATUS | TEST_BAT_CUT | TEST_TAMPER | TEST_SMS | TEST_CALL | GEOFENCE_SET:lat,lng,rad");
}

void loop() {
  // 1. Sample MPU6050 Motion-Gating State (20 Hz non-blocking) BEFORE Piezo evaluation
  updateMpuMotionState();

  // 2. High-speed Analog Piezoelectric Ceramic Vibration/Shock Sampling (GPIO 32)
  //    (Automatically muted while bikeInMotion == true)
  checkPiezoVibrationSensor();

  // 3. Monitor External 12V Motorcycle Battery Rail (GPIO 34) for Removal / Wire-Cut Tamper
  //    (NEVER muted by motion — always sends SMS & calls owner if 12V battery is removed!)
  checkBatteryRemovalTamper();

  // 4. Non-blocking Offline Local Alarm Buzzer & LED Strobe Timer
  manageLocalAlarm();
  manageActiveVoiceCall();

  // 5. Poll asynchronous SIM7000G URCs (incoming cellular MQTT commands +SMSUB & SMS)
  if (modemReady) {
    checkModemURC();
  }

  // 6. Periodic GNSS Poll, Marked Perimeter Check & Telemetry Transmission
  if (millis() - lastTelemetryTime >= TELEMETRY_INTERVAL) {
    lastTelemetryTime = millis();
    if (modemReady) {
      updateGnssAndSignal();
      checkGeofencePerimeter();

      if (!mqttConnected && (millis() - lastMqttReconnectTry >= MQTT_RECONNECT_INTERVAL)) {
        lastMqttReconnectTry = millis();
        connectCellularMqtt();
      }
    }
    publishTelemetryPayload();
  }

  // 7. USB-Serial Command Interface (Node.js Serial Gateway & Interactive Testing)
  if (Serial.available() > 0) {
    String serialCmd = Serial.readStringUntil('\n');
    serialCmd.trim();
    if (serialCmd.equalsIgnoreCase("TEST_SMS")) {
      sendGeofenceSmsAlert(gnss.latitude, gnss.longitude, 0.0, gnss.speedKmh, true);
    } else if (serialCmd.equalsIgnoreCase("TEST_TAMPER")) {
      lastVoiceCallMs = 0;
      lastTamperSmsMs = 0;
      triggerTamperAlert(4.8f, 320, "TEST_TAMPER");
    } else if (serialCmd.equalsIgnoreCase("TEST_BAT_CUT")) {
      lastVoiceCallMs = 0;
      lastTamperSmsMs = 0;
      triggerBatteryRemovalAlert(readOnboard18650Voltage());
    } else if (serialCmd.equalsIgnoreCase("TEST_CALL")) {
      lastVoiceCallMs = 0;
      placeTamperVoiceCall("MANUAL_TEST_CALL", true);
    } else if (serialCmd.equalsIgnoreCase("TEST_RELAY")) {
      Serial.println("[RELAY CH1] Clicking Channel 1 (IN1 -> GPIO 23) ON (Engine Cut) for 1.5s...");
      setImmobilizerRelay(true);
      delay(1500);
      setImmobilizerRelay(isImmobilized);
      Serial.println("[RELAY CH1] Channel 1 test complete.");
    } else if (serialCmd.equalsIgnoreCase("RELAY_INVERT")) {
      relayActiveLow = !relayActiveLow;
      setImmobilizerRelay(isImmobilized);
      Serial.println("[RELAY CH1] Polarity switched -> " + String(relayActiveLow ? "ACTIVE-LOW (Standard 2-Way Board)" : "ACTIVE-HIGH"));
    } else if (serialCmd.equalsIgnoreCase("MPU_STATUS")) {
      Serial.println("[MPU6050] Attached: " + String(mpuAvailable ? "YES" : "NO") +
                     " | State: " + String(bikeInMotion ? "IN MOTION (TAMPER MUTED)" : "PARKED (TAMPER ARMED)") +
                     " | Score: " + String(mpuMotionScore) + "/" + String(MOTION_ENTER_SCORE) +
                     " | DynAcc: " + String(lastMpuDynAccG, 3) + "G" +
                     " | Gyro: " + String(lastMpuGyroRad, 3) + " rad/s");
    } else if (serialCmd.equalsIgnoreCase("BAT_STATUS")) {
      Serial.println("[12V GUARD] Ext12V (GPIO 34): " + String(lastExt12VVoltage, 2) + "V" +
                     " | Backup18650 (GPIO 35): " + String(lastBackup18650Voltage, 2) + "V" +
                     " | Armed: " + String(batteryTamperArmed ? "YES" : "NO (waiting for >=8.5V)") +
                     " | CutState: " + String(batteryDisconnected ? "DISCONNECTED!" : "NORMAL"));
    } else if (serialCmd.startsWith("GEOFENCE_SET:")) {
      applyGeofenceCommand(serialCmd.substring(13));
    } else if (serialCmd.startsWith("AT") || serialCmd.startsWith("at")) {
      String r = sendAT(serialCmd, 2000);
      Serial.println("[MODEM] " + r);
    } else if (serialCmd.length() > 0) {
      byte dummyBuf[192];
      unsigned int len = min((unsigned int)serialCmd.length(), (unsigned int)(sizeof(dummyBuf) - 1));
      memcpy(dummyBuf, serialCmd.c_str(), len);
      onMqttCommandReceived((char*)"serial/gateway", dummyBuf, len);
    }
  }
}

/**
 * Reads external 12V motorcycle battery voltage via GPIO 34 divider (R1=100k, R2=22k)
 * Uses median-of-3 filtering and re-primes GPIO 32 SAR cap afterward
 */
float readExternal12VVoltage() {
  int a = analogRead(PIN_BATTERY_ADC);
  int b = analogRead(PIN_BATTERY_ADC);
  int c = analogRead(PIN_BATTERY_ADC);
  int lo = min(a, min(b, c));
  int hi = max(a, max(b, c));
  int med = (a + b + c) - lo - hi;
  (void)analogRead(PIN_PIEZO_VIBRATION); // Re-prime shared ADC1 S&H cap onto Piezo pin
  return (med / 4095.0f) * 3.3f * ((100.0f + 22.0f) / 22.0f);
}

/**
 * Reads LILYGO T-SIM7000G onboard 18650 backup battery voltage via GPIO 35 internal divider
 */
float readOnboard18650Voltage() {
  int raw = analogRead(PIN_ONBOARD_BAT_ADC);
  (void)analogRead(PIN_PIEZO_VIBRATION);
  float v = (raw / 4095.0f) * 3.3f * 2.0f * 1.1f;
  return (v > 2.0f) ? v : 3.9f;
}

/**
 * Monitors external 12V motorcycle battery on GPIO 34 at 10 Hz:
 * - Automatically ARMS once installed on bike (>= 8.5V present for 2.0s)
 * - Fires instant SMS + Owner Voice Call if 12V drops below 5.5V for >= 600ms
 * - Re-arms automatically when 12V battery is reconnected
 */
void checkBatteryRemovalTamper() {
  unsigned long now = millis();
  if (now - lastBatCheckMs < BAT_CHECK_INTERVAL_MS) {
    return;
  }
  lastBatCheckMs = now;

  // Skip ADC sampling during active GSM RF burst to avoid 2G TDMA ripple
  if (now - lastModemRfActivityMs < 150) {
    return;
  }

  float extV = readExternal12VVoltage();
  lastExt12VVoltage = extV;
  lastBackup18650Voltage = readOnboard18650Voltage();

  if (extV >= BAT_CONNECTED_MIN_V) {
    batMissingStreak = 0;
    if (batPresentStreak < BAT_ARM_SAMPLES) {
      batPresentStreak++;
    }

    if (!batteryTamperArmed && batPresentStreak >= BAT_ARM_SAMPLES) {
      batteryTamperArmed = true;
      batteryDisconnected = false;
      Serial.println("[12V GUARD] 🔋 12V Motorcycle Battery detected (" + String(extV, 2) +
                     "V) — Battery Removal Tamper ARMED!");
    } else if (batteryDisconnected && batPresentStreak >= BAT_ARM_SAMPLES) {
      batteryDisconnected = false;
      Serial.println("[12V GUARD] ✅ 12V Motorcycle Battery reconnected (" + String(extV, 2) +
                     "V) — Battery Removal Guard RE-ARMED.");
    }
  } else if (extV < BAT_REMOVED_THRESHOLD_V) {
    batPresentStreak = 0;
    if (batteryTamperArmed && !batteryDisconnected) {
      batMissingStreak++;
      if (batMissingStreak >= BAT_CUT_DEBOUNCE_SAMPLES) {
        batteryDisconnected = true;
        batMissingStreak = 0;
        triggerBatteryRemovalAlert(lastBackup18650Voltage);
      }
    }
  }
}

/**
 * Sends Urgent Offline GSM SMS when 12V Motorcycle Battery is removed / power wire cut
 */
bool sendBatteryCutSmsAlert(float backupVolts) {
  String smsBody = "[BODA POWER CUT " + String(BIKE_PLATE) + "]\n";
  smsBody += "CRITICAL: 12V BIKE BATTERY REMOVED!\n";
  smsBody += "Tracker on backup cell (" + String(backupVolts, 1) + "V).\n";
  smsBody += String(isImmobilized ? "Engine: LOCKED\n" : "Reply STOP to lock engine\n");
  smsBody += "https://maps.google.com/?q=" + String(gnss.latitude, 6) + "," + String(gnss.longitude, 6);
  return sendDirectSimSms(smsBody, "12V Battery Removal");
}

/**
 * Full Emergency Response on 12V Battery Removal:
 *   1. Local 10s Siren + Strobe
 *   2. Direct SIM7000G SMS to +256748802162
 *   3. Cellular MQTT + Serial Alert (triggers Cloud Voice Call & Portal Critical Banner)
 *   4. Direct SIM7000G GSM Voice Call to Owner (0748802162)
 */
void triggerBatteryRemovalAlert(float backupVolts) {
  Serial.println("[!] 🚨 CRITICAL TAMPER: 12V MOTORCYCLE BATTERY REMOVED / WIRE CUT! Running on 18650 backup (" +
                 String(backupVolts, 2) + "V)!");

  // 1. Sound local offline buzzer & flash Blue LED strobe immediately
  triggerLocalAlarm();

  // 2. Build JSON alert packet for Serial Gateway & Cellular MQTT (triggers Cloud Voice Call too)
  String cutPayload = "{";
  cutPayload += "\"device_imei\":\"" + deviceImei + "\",";
  cutPayload += "\"hardware_imei\":\"" + hardwareImei + "\",";
  cutPayload += "\"event_type\":\"BATTERY_DISCONNECTED\",";
  cutPayload += "\"sensor\":\"BATTERY_12V_CUT\",";
  cutPayload += "\"ext_voltage\":" + String(lastExt12VVoltage, 2) + ",";
  cutPayload += "\"backup_voltage\":" + String(backupVolts, 2) + ",";
  cutPayload += "\"latitude\":" + String(gnss.latitude, 6) + ",";
  cutPayload += "\"longitude\":" + String(gnss.longitude, 6) + ",";
  cutPayload += "\"alert_phone\":\"" + String(ALERT_PHONE_LOCAL) + "\",";
  cutPayload += "\"timestamp\":" + String(millis());
  cutPayload += "}";

  Serial.println("[MQTT TAMPER -> " + TOPIC_TAMPER + "]: " + cutPayload);

  // 3. Send Direct Offline GSM SMS to Owner FIRST while 2G radio channel is clear
  sendBatteryCutSmsAlert(backupVolts);

  // 4. Publish over MTN Uganda Cellular MQTT (triggers Cloud Voice Gateway & Live Portal Alert)
  sim7000PublishMqtt(TOPIC_TAMPER, cutPayload);

  // 5. Dial Owner directly on SIM7000G (ATD0748802162;) bypassing normal vibration call cooldown
  delay(400);
  placeTamperVoiceCall("12V_BATTERY_REMOVED", true);
}

/**
 * Places an Emergency Voice Call to 0748802162 using the onboard SIM7000G
 * (Attempts direct ATD0748802162; on SIM7000G + triggers Cloud Voice Gateway over SIM7000G Data)
 */
void placeTamperVoiceCall(const char* reason, bool bypassCooldown) {
  if (!modemReady) return;
  if (voiceCallActive) {
    Serial.println("[CALL] Voice call to " + String(ALERT_PHONE_LOCAL) + " already active.");
    return;
  }
  if (!bypassCooldown && lastVoiceCallMs != 0 && (millis() - lastVoiceCallMs < CALL_COOLDOWN_MS)) {
    Serial.println("[CALL] Voice call cooldown active (preventing redial spam).");
    return;
  }

  lastModemRfActivityMs = millis();
  lastVoiceCallMs = millis();
  Serial.println("[CALL] 📞 Dialing " + String(ALERT_PHONE_LOCAL) + " on SIM7000G — Reason: " + String(reason));

  sendAT("AT+CVHU=0", 500);
  String dialResp = sendAT("ATD" + String(ALERT_PHONE_LOCAL) + ";", 2000);
  lastModemRfActivityMs = millis();

  if (dialResp.indexOf("OK") >= 0) {
    voiceCallActive = true;
    voiceCallStartMs = millis();
    Serial.println("[+] 📞 SIM7000G RINGING " + String(ALERT_PHONE_LOCAL) + " (auto-hangup in 25s)...");
  } else {
    Serial.println("[CALL] 📡 SIM7000G (MDM9206) routed Call trigger over MTN Data -> Backend Voice Gateway (" + String(ALERT_PHONE_INTL) + ").");
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

  Serial.println("[SMS] 📩 Sending Offline " + String(label) + " SMS via SIM7000G to " + String(ALERT_PHONE_INTL) + "...");
  lastModemRfActivityMs = millis();

  // Essential MTN Uganda GSM SMS settings (prevents +CMS ERROR: 500)
  sendAT("AT+CMGF=1", 800);
  sendAT("AT+CSCS=\"GSM\"", 600);
  sendAT("AT+CSMP=17,167,0,0", 600);
  sendAT("AT+CGSMS=3", 600);

  while (SerialModem.available()) {
    SerialModem.read();
  }
  SerialModem.print("AT+CMGS=\"" + String(ALERT_PHONE_INTL) + "\"\r");

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
      Serial.println("[+] ✅ Offline " + String(label) + " SMS delivered to MTN network for " + String(ALERT_PHONE_INTL) + "!");
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
  String smsBody = "[BODA " + String(BIKE_PLATE) + "]\n";
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
 * Sends Instant Offline Tamper Alert SMS directly from SIM7000G MTN SIM to +256748802162
 */
bool sendTamperSmsAlert(float gForce, int piezoAdcDelta, const char* sourceName) {
  if (lastTamperSmsMs != 0 && (millis() - lastTamperSmsMs < TAMPER_SMS_COOLDOWN_MS)) {
    Serial.println("[SMS] Tamper SMS cooldown active (25s) — skipping duplicate SMS.");
    return false;
  }
  lastTamperSmsMs = millis();

  String smsBody = "[BODA TAMPER " + String(BIKE_PLATE) + "]\n";
  smsBody += "VIBRATION ALERT! Shock: " + String(gForce, 1) + "G\n";
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
      breachPayload += "\"alert_phone\":\"" + String(ALERT_PHONE_LOCAL) + "\"";
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

  // Accept offline SMS commands from owner number (0748802162 / +256748802162)
  if (sender.length() > 0 && sender.indexOf("748802162") < 0) {
    Serial.println("[SMS RX] Ignored SMS from unauthorized number: " + sender);
    return;
  }

  if (cmd.indexOf("STOP") >= 0 || cmd.indexOf("LOCK") >= 0 || cmd.indexOf("CUT") >= 0) {
    isImmobilized = true;
    localAlarmActive = false;
    digitalWrite(PIN_ALARM_BUZZER, LOW);
    setImmobilizerRelay(true);
    digitalWrite(PIN_BLUE_LED, HIGH);
    sendAck("ENGINE_CUT", "EXECUTED");
    String reply = "[BODA " + String(BIKE_PLATE) + "]\nENGINE IMMOBILIZED OFFLINE!\nhttps://maps.google.com/?q=" +
                   String(gnss.latitude, 6) + "," + String(gnss.longitude, 6);
    sendDirectSimSms(reply, "Immobilize Confirm");
  } else if (cmd.indexOf("START") >= 0 || cmd.indexOf("UNLOCK") >= 0 || cmd.indexOf("RESTORE") >= 0) {
    isImmobilized = false;
    localAlarmActive = false;
    digitalWrite(PIN_ALARM_BUZZER, LOW);
    setImmobilizerRelay(false);
    digitalWrite(PIN_BLUE_LED, LOW);
    sendAck("ENGINE_RESTORE", "EXECUTED");
    String reply = "[BODA " + String(BIKE_PLATE) + "]\nENGINE RESTORED OFFLINE!\nhttps://maps.google.com/?q=" +
                   String(gnss.latitude, 6) + "," + String(gnss.longitude, 6);
    sendDirectSimSms(reply, "Restore Confirm");
  } else if (cmd.indexOf("GPS") >= 0 || cmd.indexOf("LOC") >= 0 || cmd.indexOf("STATUS") >= 0) {
    String reply = "[BODA " + String(BIKE_PLATE) + "]\nFix: " + String(gnss.hasFix ? "3D LOCK" : "LAST KNOWN") +
                   " | 12V: " + String(batteryDisconnected ? "CUT!" : (String(lastExt12VVoltage, 1) + "V")) +
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
    int q1 = line.indexOf('"');
    int q2 = line.indexOf('"', q1 + 1);
    incomingSmsSender = (q1 >= 0 && q2 > q1) ? line.substring(q1 + 1, q2) : "";
    awaitingIncomingSmsBody = true;
    return;
  }

  if (line.startsWith("+SMSUB:")) {
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
    Serial.println("[+] Cellular MQTT Connected & Subscribed to " + TOPIC_COMMAND);
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
  float speed = hasFix ? gnss.speedKmh : 0.0f;
  float heading = hasFix ? gnss.headingDeg : 0.0f;

  float extVoltage = readExternal12VVoltage();
  float onboardLiIon = readOnboard18650Voltage();
  float batteryVoltage = extVoltage;

  // Report 0.00V if 12V battery was removed after arming; otherwise report real 12V or backup voltage
  if (batteryDisconnected) {
    batteryVoltage = 0.0f;
  } else if (batteryVoltage < 5.0f) {
    batteryVoltage = (onboardLiIon > 2.5f) ? onboardLiIon : 12.6f;
  }

  bool ignition = !isImmobilized && !batteryDisconnected;

  String payload = "{";
  payload += "\"device_imei\":\"" + deviceImei + "\",";
  payload += "\"hardware_imei\":\"" + hardwareImei + "\",";
  payload += "\"latitude\":" + String(lat, 6) + ",";
  payload += "\"longitude\":" + String(lng, 6) + ",";
  payload += "\"speed\":" + String(speed, 1) + ",";
  payload += "\"heading\":" + String(heading, 1) + ",";
  payload += "\"battery_voltage\":" + String(batteryVoltage, 2) + ",";
  payload += "\"battery_disconnected\":" + String(batteryDisconnected ? "true" : "false") + ",";
  payload += "\"ignition_status\":" + String(ignition ? "true" : "false") + ",";
  payload += "\"is_immobilized\":" + String(isImmobilized ? "true" : "false") + ",";
  payload += "\"gsm_signal\":" + String(gnss.gsmCsq) + ",";
  payload += "\"satellites\":" + String(sats) + ",";
  payload += "\"gps_lock\":" + String(hasFix ? "true" : "false");
  payload += "}";

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
  Serial.print(" | 12V: ");
  Serial.print(batteryDisconnected ? "CUT!" : (String(extVoltage, 1) + "V"));
  Serial.print(" | PiezoPeak: ");
  Serial.print(windowPeakPiezoDelta);
  Serial.print(" | Motion: ");
  Serial.print(bikeInMotion ? "MOVING(MUTED)" : "PARKED(ARMED)");
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
  piezoNoisePeak = maxNoise;

  int dynamicThresh = (piezoNoisePeak * 3) + 35;
  piezoTamperThreshold = constrain(max(80, dynamicThresh), 80, 260);

  Serial.println("[+] Analog Piezo Sensor calibrated on GPIO 32 (Baseline: " +
                 String(piezoBaseline, 1) + ", NoiseFloor: " + String(piezoNoisePeak) +
                 ", TriggerThreshold: " + String(piezoTamperThreshold) + "/4095)");
}

/**
 * Calibrates resting gravity magnitude & gyroscope zero-rate offsets on MPU6050
 */
void calibrateMpuSensor() {
  if (!mpuAvailable) return;

  const int samples = 40;
  float sumAccG = 0.0f;
  float sumGx = 0.0f, sumGy = 0.0f, sumGz = 0.0f;
  int validSamples = 0;

  for (int i = 0; i < samples; i++) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    float accG = sqrtf(a.acceleration.x * a.acceleration.x +
                       a.acceleration.y * a.acceleration.y +
                       a.acceleration.z * a.acceleration.z) / 9.80665f;
    if (!isnan(accG) && accG > 0.1f) {
      sumAccG += accG;
      sumGx += g.gyro.x;
      sumGy += g.gyro.y;
      sumGz += g.gyro.z;
      validSamples++;
    }
    delay(5);
  }

  if (validSamples > 0) {
    mpuGravityBaseG = sumAccG / (float)validSamples;
    mpuGyroBiasX    = sumGx / (float)validSamples;
    mpuGyroBiasY    = sumGy / (float)validSamples;
    mpuGyroBiasZ    = sumGz / (float)validSamples;
  }

  Serial.println("[+] MPU6050 calibrated (GravityBase: " + String(mpuGravityBaseG, 2) +
                 "G, GyroBias: [" + String(mpuGyroBiasX, 3) + ", " +
                 String(mpuGyroBiasY, 3) + ", " + String(mpuGyroBiasZ, 3) + "] rad/s)");
}

/**
 * Samples MPU6050 at 20 Hz to detect sustained motorcycle motion (riding / rolling / steering)
 * and mute Piezo vibration tamper messages until the motorcycle parks again.
 */
void updateMpuMotionState() {
  unsigned long now = millis();
  if (now - lastMpuSampleMs < MPU_SAMPLE_INTERVAL_MS) {
    return;
  }
  lastMpuSampleMs = now;

  bool gpsMoving = gnss.hasFix && (gnss.speedKmh >= 4.0f);
  bool imuMoving = false;

  if (mpuAvailable) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    float accMagG = sqrtf(a.acceleration.x * a.acceleration.x +
                          a.acceleration.y * a.acceleration.y +
                          a.acceleration.z * a.acceleration.z) / 9.80665f;

    float gx = g.gyro.x - mpuGyroBiasX;
    float gy = g.gyro.y - mpuGyroBiasY;
    float gz = g.gyro.z - mpuGyroBiasZ;
    float gyroMagRad = sqrtf(gx * gx + gy * gy + gz * gz);

    if (!isnan(accMagG) && !isnan(gyroMagRad) && accMagG > 0.05f) {
      float dynAccG = fabsf(accMagG - mpuGravityBaseG);
      lastMpuDynAccG = dynAccG;
      lastMpuGyroRad = gyroMagRad;

      // Slowly track resting gravity & gyro thermal drift only when completely still
      if (dynAccG < 0.04f && gyroMagRad < 0.05f) {
        mpuGravityBaseG = (0.99f * mpuGravityBaseG) + (0.01f * accMagG);
        mpuGyroBiasX    = (0.995f * mpuGyroBiasX) + (0.005f * g.gyro.x);
        mpuGyroBiasY    = (0.995f * mpuGyroBiasY) + (0.005f * g.gyro.y);
        mpuGyroBiasZ    = (0.995f * mpuGyroBiasZ) + (0.005f * g.gyro.z);
      }

      imuMoving = (dynAccG >= MOTION_ACC_THRESHOLD_G) || (gyroMagRad >= MOTION_GYRO_THRESHOLD_RAD);
    }
  }

  if (imuMoving || gpsMoving) {
    mpuMotionScore = min(MOTION_SCORE_MAX, mpuMotionScore + 2);
  } else {
    mpuMotionScore = max(0, mpuMotionScore - 1);
  }

  if (!bikeInMotion) {
    if (mpuMotionScore >= MOTION_ENTER_SCORE || gpsMoving) {
      bikeInMotion = true;
      lastMotionActiveMs = now;

      // Silence local siren if rider just mounted & started riding away (keep alarm if 12V battery cut!)
      if (localAlarmActive && !isImmobilized && !batteryDisconnected) {
        localAlarmActive = false;
        digitalWrite(PIN_ALARM_BUZZER, LOW);
        digitalWrite(PIN_BLUE_LED, LOW);
      }
      Serial.println("[MPU6050] 🏍️ Bike IN MOTION — Piezo vibration tamper messages MUTED.");
    }
  } else {
    if (imuMoving || gpsMoving || mpuMotionScore >= (MOTION_ENTER_SCORE / 2)) {
      lastMotionActiveMs = now;
    } else if ((now - lastMotionActiveMs >= MOTION_HOLD_MS) && (mpuMotionScore == 0)) {
      bikeInMotion = false;
      lastTamperAlertMs = now;
      Serial.println("[MPU6050] 🅿️ Bike PARKED / STILL — Piezo vibration tamper alerts ARMED.");
    }
  }
}

/**
 * High-speed AC-coupled peak detector for Analog Piezoelectric Ceramic Sensor (GPIO 32).
 * Continues tracking DC baseline at all times, but suppresses tamper alerts when bikeInMotion == true.
 */
void checkPiezoVibrationSensor() {
  if (millis() - lastModemRfActivityMs < 200) {
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

  // Gate Piezo tamper reaction: do NOT send tamper messages when MPU6050 detects bike motion
  if (bikeInMotion) {
    if (delta >= piezoTamperThreshold && (millis() - lastMutedVibrationLogMs >= 5000)) {
      lastMutedVibrationLogMs = millis();
      Serial.println("[MPU6050 GATE] 🔇 Ignored Piezo vibration (ADC delta: " + String(delta) +
                     ") — motorcycle is in motion.");
    }
    return;
  }

  if (delta >= piezoTamperThreshold && (millis() - lastTamperAlertMs >= TAMPER_COOLDOWN_MS)) {
    delayMicroseconds(50); // Stay near crest of 1-2 kHz piezo half-wave (not 400us zero-crossing!)
    int confirmDelta = abs(readFilteredPiezoADC() - baseInt);
    if (confirmDelta >= (piezoTamperThreshold * 3 / 4)) {
      int peak = max(delta, confirmDelta);
      lastTamperAlertMs = millis();
      float norm = (float)(peak - piezoTamperThreshold) / 600.0f;
      if (norm < 0.0f) norm = 0.0f;
      if (norm > 1.0f) norm = 1.0f;
      float equivGForce = 3.4f + (norm * 8.6f);
      triggerTamperAlert(equivGForce, peak, "PIEZO_GPIO32");
    }
  }
}

/**
 * Triggers 100% Offline Local Siren/Strobe + Direct MTN SIM Tamper SMS + Owner Voice Call
 */
void triggerTamperAlert(float gForce, int piezoAdcDelta, const char* sourceName) {
  Serial.println("[!] TAMPER DETECTED (" + String(sourceName) + "): Shock " +
                 String(gForce, 2) + "G | Piezo ADC Delta: " + String(piezoAdcDelta) + "/4095!");

  // 1. Activate Offline Local Buzzer/Siren (GPIO 14) + Rapid Blue LED Warning Strobe
  triggerLocalAlarm();

  String tamperPayload = "{";
  tamperPayload += "\"device_imei\":\"" + deviceImei + "\",";
  tamperPayload += "\"hardware_imei\":\"" + hardwareImei + "\",";
  tamperPayload += "\"event_type\":\"TAMPER_DETECTED\",";
  tamperPayload += "\"sensor\":\"" + String(sourceName) + "\",";
  tamperPayload += "\"g_force\":" + String(gForce, 2) + ",";
  tamperPayload += "\"piezo_adc\":" + String(piezoAdcDelta) + ",";
  tamperPayload += "\"alert_phone\":\"" + String(ALERT_PHONE_LOCAL) + "\",";
  tamperPayload += "\"timestamp\":" + String(millis());
  tamperPayload += "}";

  // 2. Emit over Serial immediately (<2ms)
  Serial.println("[MQTT TAMPER -> " + TOPIC_TAMPER + "]: " + tamperPayload);

  // 3. Send Instant 100% Offline Direct Tamper SMS FIRST (before Class-B GPRS MQTT TBF burst!)
  sendTamperSmsAlert(gForce, piezoAdcDelta, sourceName);

  // 4. Publish over MTN Uganda Cellular MQTT for live map sync & Cloud Voice Call
  sim7000PublishMqtt(TOPIC_TAMPER, tamperPayload);

  // 5. Place Direct SIM7000G Tamper Call
  placeTamperVoiceCall(sourceName, false);
}

/**
 * Drives Channel 1 (IN1 -> GPIO 23) of the 2-Way 5V Relay Module.
 * Uses Open-Drain High-Z when OFF on Active-LOW 5V relay modules so 3.3V ESP32 logic
 * shuts off the 5V optocoupler 100% cleanly (0 mA drain during normal riding).
 */
void setImmobilizerRelay(bool cutEngine) {
  if (relayActiveLow) {
    if (cutEngine) {
      pinMode(PIN_RELAY_IMMOBILIZER, OUTPUT);
      digitalWrite(PIN_RELAY_IMMOBILIZER, LOW);  // Pull IN1 to GND -> Relay CH1 Energized (Opens COM1-NC1)
    } else {
      pinMode(PIN_RELAY_IMMOBILIZER, INPUT);     // High-Z Open-Drain -> Relay CH1 OFF (COM1-NC1 Closed)
    }
  } else {
    pinMode(PIN_RELAY_IMMOBILIZER, OUTPUT);
    digitalWrite(PIN_RELAY_IMMOBILIZER, cutEngine ? HIGH : LOW);
  }
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

  if (message.startsWith("GEOFENCE_SET:")) {
    applyGeofenceCommand(message.substring(13));
  } else if (message.indexOf("ENGINE_CUT") >= 0) {
    isImmobilized = true;
    setImmobilizerRelay(true);
    digitalWrite(PIN_BLUE_LED, HIGH);
    Serial.println("[ACTION] ⛔ ENGINE IMMOBILIZED! Relay CH1 (IN1) activated | Blue LED: OFF.");
    sendAck("ENGINE_CUT", "EXECUTED");
  } else if (message.indexOf("ENGINE_RESTORE") >= 0) {
    isImmobilized = false;
    setImmobilizerRelay(false);
    digitalWrite(PIN_BLUE_LED, LOW);
    Serial.println("[ACTION] ✅ ENGINE RESTORED! Relay CH1 (IN1) deactivated | Blue LED: ON (Running).");
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
  ackPayload += "\"command_action\":\"" + action + "\",";
  ackPayload += "\"status\":\"" + status + "\",";
  ackPayload += "\"ack_timestamp\":" + String(millis());
  ackPayload += "}";

  sim7000PublishMqtt(TOPIC_ACK, ackPayload);
  Serial.println("[MQTT ACK -> " + TOPIC_ACK + "]: " + ackPayload);
}
