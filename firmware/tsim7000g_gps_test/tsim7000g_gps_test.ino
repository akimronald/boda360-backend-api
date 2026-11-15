/**
 * ============================================================================
 * LILYGO T-SIM7000G - STANDALONE GPS / GNSS HARDWARE FLASH TEST
 * Project: ISBAT University Boda-Boda Real-Time Tracking & Immobilization
 * Board:   LILYGO TTGO T-SIM7000G (ESP32 + Integrated SIMCom SIM7000G)
 *
 * Hardware Pin Mapping (Internal to LILYGO T-SIM7000G):
 *   - SIM7000G UART RX (ESP32 <- Modem TX): GPIO 26
 *   - SIM7000G UART TX (ESP32 -> Modem RX): GPIO 27
 *   - SIM7000G PWRKEY:                      GPIO 4
 *   - SIM7000G DTR (Sleep/Wake):            GPIO 25 (LOW = Awake)
 *   - Onboard Status Blue LED:              GPIO 12 (LOW = ON, HIGH = OFF)
 *   - Onboard 18650 Battery ADC:            GPIO 35
 *
 * IMPORTANT HARDWARE CHECKLIST:
 *   1. Snap the GPS antenna onto the "GPS" IPEX/U.FL connector (NOT "LTE").
 *   2. Place the GPS ceramic patch antenna facing up near a window or outdoors.
 *   3. Slide the onboard T-SIM7000G power switch to ON (18650 battery or USB-C
 *      with sufficient current helps stabilize modem + GNSS peaks).
 * ============================================================================
 */

#include <Arduino.h>

// LILYGO T-SIM7000G Hardware Pins
#define PIN_MODEM_RX   26
#define PIN_MODEM_TX   27
#define PIN_MODEM_PWR  4
#define PIN_MODEM_DTR  25
#define PIN_STATUS_LED 12   // Active LOW on T-SIM7000G (LOW = ON, HIGH = OFF)
#define PIN_BAT_ADC    35   // Onboard 18650 Li-ion voltage divider

HardwareSerial SerialAT(1);

uint32_t activeBaud = 115200;
unsigned long lastPollMs = 0;
const unsigned long GPS_POLL_INTERVAL_MS = 2000;
bool ledBlinkState = false;
bool gpsReady = false;

/**
 * Sends an AT command and returns the full response string.
 */
String sendATCommand(const String& cmd, uint32_t timeoutMs = 1500, bool echo = false) {
  while (SerialAT.available()) {
    SerialAT.read();
  }
  SerialAT.println(cmd);

  String resp = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) {
      char c = (char)SerialAT.read();
      resp += c;
    }
    if (resp.indexOf("\r\nOK\r\n") >= 0 || resp.indexOf("\r\nERROR\r\n") >= 0) {
      break;
    }
    delay(5);
  }

  if (echo) {
    Serial.println(">> " + cmd);
    Serial.println(resp);
  }
  return resp;
}

/**
 * Extracts a 0-based comma-separated field while preserving empty fields (,,).
 */
String getCsvField(const String& csv, int targetIndex) {
  int currentIndex = 0;
  int startPos = 0;
  int len = csv.length();

  for (int i = 0; i <= len; i++) {
    if (i == len || csv.charAt(i) == ',') {
      if (currentIndex == targetIndex) {
        String field = csv.substring(startPos, i);
        field.trim();
        return field;
      }
      currentIndex++;
      startPos = i + 1;
    }
  }
  return "";
}

/**
 * Pulses GPIO 4 (PWRKEY) to turn ON the SIM7000G modem.
 */
void modemPowerPulse() {
  Serial.println("[MODEM] Pulsing PWRKEY (GPIO 4) for 1.1s...");
  pinMode(PIN_MODEM_PWR, OUTPUT);
  digitalWrite(PIN_MODEM_PWR, LOW);
  delay(100);
  digitalWrite(PIN_MODEM_PWR, HIGH);
  delay(1100);
  digitalWrite(PIN_MODEM_PWR, LOW);
  delay(3500); // Allow SIM7000G RTOS to boot
}

/**
 * Probes UART at 115200 and 9600 baud; powers on SIM7000G only if not yet awake.
 */
bool initModemUART() {
  pinMode(PIN_MODEM_DTR, OUTPUT);
  digitalWrite(PIN_MODEM_DTR, LOW); // Keep SIM7000G awake

  const uint32_t bauds[] = {115200, 9600};

  // Step 1: Check if SIM7000G is already powered on (avoids toggling OFF on soft reset)
  for (uint32_t b : bauds) {
    SerialAT.begin(b, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
    delay(100);
    for (int i = 0; i < 3; i++) {
      String r = sendATCommand("AT", 400);
      if (r.indexOf("OK") >= 0) {
        activeBaud = b;
        Serial.println("[+] SIM7000G already awake at " + String(activeBaud) + " baud.");
        return true;
      }
    }
  }

  // Step 2: Pulse PWRKEY and poll until modem responds
  modemPowerPulse();

  for (int attempt = 1; attempt <= 2; attempt++) {
    for (uint32_t b : bauds) {
      SerialAT.begin(b, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
      delay(100);
      Serial.println("[MODEM] Probing UART at " + String(b) + " baud (attempt " + String(attempt) + ")...");
      for (int i = 0; i < 8; i++) {
        String r = sendATCommand("AT", 500);
        if (r.indexOf("OK") >= 0) {
          activeBaud = b;
          Serial.println("[+] SIM7000G responded OK at " + String(activeBaud) + " baud!");
          return true;
        }
        delay(300);
      }
    }
    if (attempt == 1) {
      modemPowerPulse();
    }
  }

  return false;
}

/**
 * Enables active GPS antenna rail and SIM7000G GNSS engine.
 */
bool enableGNSS() {
  sendATCommand("ATE0", 500); // Disable command echo for clean parsing

  String model = sendATCommand("AT+CGMM", 1000);
  model.replace("OK", "");
  model.trim();
  Serial.println("[INFO] Modem Model : " + (model.length() ? model : "Unknown"));

  String imei = sendATCommand("AT+GSN", 1000);
  imei.replace("OK", "");
  imei.trim();
  Serial.println("[INFO] Modem IMEI  : " + (imei.length() ? imei : "Unknown"));

  String cpin = sendATCommand("AT+CPIN?", 1000);
  cpin.replace("OK", "");
  cpin.trim();
  Serial.println("[INFO] SIM Status  : " + (cpin.length() ? cpin : "No SIM / Not Ready"));

  // Configure 2G GSM/EDGE mode (AT+CNMP=13) for Uganda carriers (MTN / Airtel)
  // Factory SIM7000G defaults to CNMP=38 (LTE Cat-M1/NB-IoT only)
  sendATCommand("AT+CNMP=13", 1500);
  String cops = sendATCommand("AT+COPS?", 1200);
  cops.replace("OK", "");
  cops.trim();
  String csq = sendATCommand("AT+CSQ", 800);
  csq.replace("OK", "");
  csq.trim();
  Serial.println("[INFO] Carrier/CSQ : " + cops + " | " + csq);

  // Enable active GPS antenna power rail on LILYGO T-SIM7000G
  // Both SGPIO (older firmware) and CGPIO (newer firmware) are sent safely
  Serial.println("[GPS] Enabling active GPS antenna power rail...");
  String sgpio = sendATCommand("AT+SGPIO=0,4,1,1", 1500);
  String cgpio = sendATCommand("AT+CGPIO=0,48,1,1", 1500);
  if (sgpio.indexOf("OK") >= 0 || cgpio.indexOf("OK") >= 0) {
    Serial.println("[+] GPS antenna power rail enabled.");
  } else {
    Serial.println("[!] Note: GPIO antenna rail command returned non-OK (continuing).");
  }

  // Power on internal GNSS engine
  Serial.println("[GPS] Powering ON SIM7000G GNSS engine (AT+CGNSPWR=1)...");
  String pwr = sendATCommand("AT+CGNSPWR=1", 2000);
  if (pwr.indexOf("OK") < 0) {
    Serial.println("[-] Failed to enable GNSS engine! Response: " + pwr);
    return false;
  }

  // Configure multi-constellation GNSS (GPS + GLONASS + BeiDou + Galileo)
  sendATCommand("AT+CGNSMOD=1,1,1,1", 1500);

  String checkPwr = sendATCommand("AT+CGNSPWR?", 1000);
  checkPwr.replace("OK", "");
  checkPwr.trim();
  Serial.println("[+] GNSS Power State: " + checkPwr);
  return true;
}

/**
 * Reads onboard 18650 Li-ion battery voltage on GPIO 35.
 */
float readOnboardBatteryV() {
  int raw = analogRead(PIN_BAT_ADC);
  // T-SIM7000G uses a 100k/100k (x2) divider to GPIO 35
  return (raw / 4095.0f) * 3.3f * 2.0f * 1.1f;
}

/**
 * Queries AT+CGNSINF and prints formatted GPS status & coordinates.
 */
void pollGPS() {
  String raw = sendATCommand("AT+CGNSINF", 1500);
  int idx = raw.indexOf("+CGNSINF:");
  if (idx < 0) {
    Serial.println("[GPS WARN] No +CGNSINF response from SIM7000G (check power/battery switch).");
    return;
  }

  int lineEnd = raw.indexOf('\n', idx);
  String line = (lineEnd > idx) ? raw.substring(idx + 9, lineEnd) : raw.substring(idx + 9);
  line.trim();

  String runStatus  = getCsvField(line, 0);
  String fixStatus  = getCsvField(line, 1);
  String utcTime    = getCsvField(line, 2);
  String lat        = getCsvField(line, 3);
  String lng        = getCsvField(line, 4);
  String alt        = getCsvField(line, 5);
  String speedKmh   = getCsvField(line, 6);
  String courseDeg  = getCsvField(line, 7);
  String hdop       = getCsvField(line, 10);
  String satsView   = getCsvField(line, 14);
  String satsUsed   = getCsvField(line, 15);
  String cn0Max     = getCsvField(line, 18);

  bool hasFix = (fixStatus == "1" && lat.length() > 0 && lng.length() > 0);

  if (hasFix) {
    digitalWrite(PIN_STATUS_LED, LOW); // Solid ON when 3D Fix Locked
  } else {
    ledBlinkState = !ledBlinkState;
    digitalWrite(PIN_STATUS_LED, ledBlinkState ? LOW : HIGH); // Blink while searching
  }

  float vbat = readOnboardBatteryV();

  Serial.println("-----------------------------------------------------------------");
  Serial.print("[GPS] Engine: ");
  Serial.print(runStatus == "1" ? "ON" : "OFF");
  Serial.print(" | Status: ");
  Serial.print(hasFix ? "LOCKED (3D FIX)" : "SEARCHING SKY...");
  Serial.print(" | Sats (Used/View): ");
  Serial.print(satsUsed.length() ? satsUsed : "0");
  Serial.print("/");
  Serial.print(satsView.length() ? satsView : "0");
  Serial.print(" | Peak SNR: ");
  Serial.print(cn0Max.length() ? cn0Max : "0");
  Serial.print(" dB-Hz | VBAT: ");
  Serial.print(vbat, 2);
  Serial.println("V");

  if (hasFix) {
    Serial.println("      Lat: " + lat + "  |  Lng: " + lng + "  |  Alt: " + alt + " m");
    Serial.println("      Speed: " + speedKmh + " km/h  |  Heading: " + courseDeg + " deg  |  HDOP: " + hdop + "  |  UTC: " + utcTime);
    Serial.println("      Map: https://www.google.com/maps?q=" + lat + "," + lng);
  } else {
    Serial.println("      UTC: " + (utcTime.length() ? utcTime : "syncing...") + "  |  Raw: +CGNSINF: " + line);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, HIGH); // LED OFF initially
  pinMode(PIN_BAT_ADC, INPUT);

  Serial.println();
  Serial.println("=================================================================");
  Serial.println("  LILYGO T-SIM7000G - GPS / GNSS HARDWARE DIAGNOSTIC TEST");
  Serial.println("  ISBAT University Boda-Boda Telematics Node");
  Serial.println("=================================================================");

  if (!initModemUART()) {
    Serial.println("[-] ERROR: SIM7000G did not respond on GPIO 26/27!");
    Serial.println("    -> Ensure the physical T-SIM7000G power switch is slid to ON.");
    Serial.println("    -> Ensure sufficient USB-C power or an 18650 cell is inserted.");
    gpsReady = false;
  } else {
    gpsReady = enableGNSS();
  }

  Serial.println("[TIP] Type any AT command (e.g. AT+CGNSINF, AT+CSQ) or 'RESTART_GPS' in Serial Monitor.");
}

void loop() {
  // Interactive USB Serial -> SIM7000G AT Command Bridge
  if (Serial.available()) {
    String userCmd = Serial.readStringUntil('\n');
    userCmd.trim();
    if (userCmd.equalsIgnoreCase("RESTART_GPS")) {
      Serial.println("[CMD] Re-initializing modem & GNSS...");
      if (initModemUART()) {
        gpsReady = enableGNSS();
      }
    } else if (userCmd.length() > 0) {
      sendATCommand(userCmd, 2000, true);
    }
  }

  // Periodic GPS Poll
  if (millis() - lastPollMs >= GPS_POLL_INTERVAL_MS) {
    lastPollMs = millis();
    if (gpsReady) {
      pollGPS();
    } else {
      Serial.println("[WAIT] Modem/GNSS not initialized. Check power switch & send 'RESTART_GPS'.");
    }
  }
}

