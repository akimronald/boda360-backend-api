# Dual-Board Internet OTA & SIM800L + NEO-6M Hardware Guide

Boda 360 supports **two ESP32 hardware architectures** simultaneously, with separate Internet Over-The-Air (OTA) firmware channels so an update meant for one microcontroller never overwrites the other:

| Board Key (`BOARD_TYPE`) | Microcontroller & Modules | Sketch Path | Voice Call Capability |
| :--- | :--- | :--- | :--- |
| `esp32_wroom` | **ESP32-WROOM-32 + SIM800L + u-blox NEO-6M GPS** | `firmware/esp32_wroom_sim800l_neo6m/esp32_wroom_sim800l_neo6m.ino` | **Direct 2G GSM Voice Call (`ATD0748802162;`)** — rings owner phone directly with zero internet! |
| `lilygo_tsim7000g` | **ESP32 LILYGO TTGO T-SIM7000G** | `firmware/esp32_boda_node/esp32_boda_node.ino` | SIM7000G `ATD` + Cloud Voice Gateway fallback |

---

## 1. Wiring Diagram: ESP32-WROOM-32 + SIM800L + NEO-6M GPS

### CRITICAL Power Rule for SIM800L (`3.7V – 4.2V @ 2A Peak`)
- **NEVER connect `SIM800L VCC` to `5V`** (destroys the SIM800L chip).
- **NEVER connect `SIM800L VCC` to the ESP32 `3V3` pin** (GSM voice calls and SMS bursts draw up to **2.0A**, causing immediate brownout resets).
- Power `SIM800L VCC` from **either**:
  1. A dedicated **LM2596 DC-DC Buck Converter** stepped down from the motorcycle's 12V battery to **`4.0V`**, **OR**
  2. Directly from a **3.7V 18650 Li-Ion cell** (`3.7V–4.2V`), with a **`1000µF` electrolytic + `100nF` ceramic capacitor** soldered right across `SIM800L VCC` and `GND`.
- Tie **all `GND` pins together** (Motorcycle 12V GND, Buck/18650 GND, SIM800L GND, NEO-6M GND, and ESP32 GND).

### Complete Pin Mapping (`esp32_wroom_sim800l_neo6m.ino`)

| Module / Peripheral | Module Pin | ESP32-WROOM Pin | Notes |
| :--- | :--- | :--- | :--- |
| **SIM800L GSM/Voice** | `VCC` | *4.0V Buck / 18650+* | `3.7V–4.2V` (>= 2A peak) + `1000µF` cap |
| **SIM800L GSM/Voice** | `GND` | `GND` | Common ground |
| **SIM800L GSM/Voice** | `TXD` | `GPIO 26` (`RX1`) | Direct connection |
| **SIM800L GSM/Voice** | `RXD` | `GPIO 27` (`TX1`) | Via `10kΩ` series resistor (or `10kΩ` to RXD + `20kΩ` to GND divider for 2.8V logic) |
| **SIM800L GSM/Voice** | `RST` | `GPIO 4` | Hardware auto-reset recovery |
| **u-blox NEO-6M GPS** | `VCC` | `3V3` or `VIN (5V)` | Most GY-NEO6MV2 boards have an onboard 3.3V LDO (`3.3V–5V` safe) |
| **u-blox NEO-6M GPS** | `GND` | `GND` | Common ground |
| **u-blox NEO-6M GPS** | `TX` | `GPIO 16` (`RX2`) | 9600 baud NMEA `$GPRMC` / `$GPGGA` |
| **u-blox NEO-6M GPS** | `RX` | `GPIO 17` (`TX2`) | 9600 baud |
| **Immobilizer Relay** | `IN` | `GPIO 23` | `HIGH` = Energize Relay (Cut CDI Ignition) |
| **Local Alarm Buzzer** | `+` | `GPIO 14` | `HIGH` = 10s Siren Strobe on Tamper |
| **Piezo Shock Sensor** | `+ (Signal)` | `GPIO 32` (`ADC1`) | Place `1MΩ` resistor in parallel from `GPIO 32` to `GND` |
| **Optional MPU-6050** | `SDA / SCL` | `GPIO 21 / GPIO 22` | Optional 6-axis I2C shock sensor (`0x68`) |
| **12V Battery Sense** | Divider Mid | `GPIO 34` (`ADC1`) | `R1 = 100kΩ` to Bike `+12V`, `R2 = 22kΩ` to `GND` (detects wire cut!) |
| **Backup Li-Ion Sense** | Divider Mid | `GPIO 35` (`ADC1`) | `R1 = 100kΩ` to `18650+`, `R2 = 100kΩ` to `GND` |

---

## 2. How to Upload Code Over The Internet (OTA Workflow)

### Step A — One-Time USB Flash (Installs the Internet OTA Bootloader Partition)
You only plug each ESP32 into your computer **once** via USB:
1. Open Arduino IDE and select:
   - **Tools &rarr; Board:** `ESP32 Dev Module`
   - **Tools &rarr; Partition Scheme:** `Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)` *(Mandatory so flash holds `app0` + `app1` OTA slots!)*
2. Flash:
   - `firmware/esp32_wroom_sim800l_neo6m/esp32_wroom_sim800l_neo6m.ino` onto your **ESP32-WROOM + SIM800L + NEO-6M** boards.
   - `firmware/esp32_boda_node/esp32_boda_node.ino` onto your **ESP32 LILYGO T-SIM7000G** boards.

### Step B — Updating Any Bike Remotely Over The Internet (Zero USB Cable!)
Whenever you edit the code in the future:
1. Bump `FIRMWARE_VERSION` in the sketch (e.g., from `"2.1.0"` to `"2.2.0"`).
2. In Arduino IDE, click **Sketch &rarr; Export Compiled Binary**.
   - Arduino creates a `.bin` file inside `build/esp32.esp32.esp32/` next to your `.ino` file.
3. Open **Manufacturer Portal** (`http://localhost:3000/manufacturer`) &rarr; click **Internet OTA Firmware**:
   - Choose the **Target Microcontroller** (`ESP32-WROOM` or `ESP32 LILYGO`).
   - Enter the new version (e.g. `2.2.0`) and select the exported `.bin` file.
   - *(For nationwide Cellular SIM updates outside your local Wi-Fi)*: Expose port `3000` with Cloudflare Tunnel (`cloudflared tunnel --url http://localhost:3000`) or deploy the Node backend on Railway/Render, and paste the public `http://...` URL in **Optional Public Internet URL** *(note: SIM800L's built-in 2G HTTP engine requires plain `http://`, whereas Wi-Fi OTA supports both `http://` and `https://`)*.
   - Click **Stage Firmware Binary on OTA Server**.
4. Click **Push OTA to All ESP32-WROOM Bikes**, **Push OTA to All LILYGO Bikes**, or **Flash Target IMEI**:
   - The server publishes `{"cmd":"OTA_UPDATE","board":"...","version":"2.2.0","md5":"...","url":"..."}` over `broker.hivemq.com:1883`.
   - Matching bikes check that **speed &lt; 2 km/h** (stationary safety lock), stream the `.bin` over Wi-Fi or Cellular GPRS, verify the **MD5 checksum**, report `OTA_SUCCESS` live to your Manufacturer console, and reboot into the new version!

---

## 3. Direct GSM Voice Call & Offline SMS Commands (`esp32_wroom`)
Even when the SIM card has **zero mobile data**, you can control and test the `ESP32-WROOM + SIM800L + NEO-6M` tracker from owner phone `+256748802162`:
- **Physical Tamper / 12V Battery Cut:** Sounds 10s local siren, texts a Google Maps NEO-6M GPS link, and **places a live 2G GSM voice call (`ATD0748802162;`)** directly to your phone!
- **SMS `CALLME`** &rarr; Tracker immediately dials your phone back to test SIM800L voice audio.
- **SMS `STOP` / `CUT`** &rarr; Cuts motorcycle ignition relay (`GPIO 23`) and replies with Google Maps coordinates.
- **SMS `START` / `RESTORE`** &rarr; Restores motorcycle ignition relay.
- **SMS `GPS` / `STATUS`** &rarr; Replies with 3D NEO-6M satellite lock, speed, firmware version, and Google Maps link.
- **SMS `OTA:http://your-server/api/ota/download/esp32_wroom`** &rarr; Triggers an Internet OTA firmware upgrade via SMS!

---

## 4. How Every Board Gets Its Own Unique IMEI & Syncs Customer Email/Phone/Plate

Both sketches use ESP32's permanent **NVS (`<Preferences.h>`, namespace `"boda360"`)** so **you compile ONE `.bin` file for 100+ boards** and NVS is **never erased** during Internet OTA updates:

1. **Automatic Unique IMEI Priority Chain (`loadIdentityFromNvs()`):**
   - **Priority 1 — Custom Locked NVS IMEI:** If you assign a custom batch IMEI via USB Serial or MQTT (`SET_IMEI:863892050119300`), it is locked permanently in NVS (`CLEAR_IMEI` reverts to automatic).
   - **Priority 2 — Modem Factory `AT+GSN` IMEI:** Automatically reads the globally unique 15-digit IMEI burned into each SIM800L or SIM7000G module and caches it in NVS (`auto_imei`).
   - **Priority 3 — ESP32 Silicon MAC Fallback:** Derives a unique 15-digit ID (`86360xxxxxxxxxx`) from `ESP.getEfuseMac()` if the modem is not wired yet.
   - Type **`WHOAMI`** in the Arduino Serial Monitor (`115200` baud) at any time to view the board's active IMEI, bound plate, owner phone, and owner email.

2. **Automatic Customer Sign-Up & Multi-Bike Sync (`OWNER_SYNC`):**
   - When a customer signs up on `http://localhost:3000/customer` (or adds/edits a bike) with their **Email, Phone Number, Bike Plate, and that board's 15-digit IMEI**, the backend automatically publishes `{"cmd":"OWNER_SYNC","imei":"...","plate":"...","owner_phone":"...","owner_email":"..."}` over `broker.hivemq.com:1883` + USB Serial.
   - If the ESP32 was powered off during sign-up, the server automatically pushes `OWNER_SYNC` the instant the board powers on and sends `BOOT_READY`.
   - The ESP32 saves the customer's **phone number, email, and plate** into NVS permanently—switching all future SIM800L/SIM7000G **Tamper Voice Calls**, **Offline GPS SMS alerts**, and **authorized SMS control (`STOP`/`START`)** to that customer's phone!


