# Complete Component-by-Component Wiring Manual for LILYGO TTGO T-SIM7000G

This manual gives you **step-by-step bench and motorcycle wiring instructions for every single component** connected to your **LILYGO TTGO T-SIM7000G** (`BOARD_TYPE = "lilygo_tsim7000g"`), matching [`firmware/esp32_boda_node/esp32_boda_node.ino`](esp32_boda_node/esp32_boda_node.ino).

---

## 0. Know Your LILYGO TTGO T-SIM7000G Board First

Before connecting any wires, hold your **LILYGO T-SIM7000G** board with the **USB-C port facing DOWN**:

```text
          +---------------------------------------------------+
          |  [MAIN U.FL]   [SIM7000G Metal Shield]  [GPS U.FL]|
          |  (Cell Ant.)                            (GPS Ant.)|
          |                                                   |
          |  [Nano-SIM Slot]          [MicroSD Slot]          |
          |                                                   |
          |   o RST                                  TX  o    |
          |   o 3V3                                  RX  o    |
          |   o GND                                 IO22 o <-- MPU6050 SCL
          |   o IO27 (INTERNAL MODEM TX - DO NOT USE)IO21 o <-- MPU6050 SDA
          |   o IO26 (INTERNAL MODEM RX - DO NOT USE)IO32 o <-- PIEZO SENSOR (+)
          |   o IO25 (INTERNAL MODEM DTR- DO NOT USE)IO33 o    |
          |   o IO34 <-- 12V BATTERY DIVIDER MID     IO35 o (INTERNAL 18650 ADC)
          |   o IO14 <-- ALARM BUZZER / SIREN (+)    IO23 o <-- RELAY MODULE (IN)
          |   o IO12 (INTERNAL BLUE LED)             IO4  o (INTERNAL MODEM PWR)
          |   o 5V   <-- 5V BUCK CONVERTER (OUT+)    GND  o <-- COMMON GROUND
          |                                                   |
          |       [PWR Switch: ON/OFF]    [USB-C Port]        |
          +---------------------------------------------------+
                  (Rear of Board: 18650 Battery Holder)
```

### ⚠️ Reserved Internal Pins (NEVER Connect External Wires Here)
These pins are already wired inside the LILYGO PCB layers:
- **`IO4` (`GPIO 4`):** Internal SIM7000G Power Key (`PWRKEY`)
- **`IO12` (`GPIO 12`):** Onboard Status Blue LED (`LOW` = ON, `HIGH` = OFF)
- **`IO25` (`GPIO 25`):** Internal SIM7000G `DTR` Wake Pin
- **`IO26` (`GPIO 26`):** Internal SIM7000G `TXD` &rarr; ESP32 `RX1`
- **`IO27` (`GPIO 27`):** Internal SIM7000G `RXD` &larr; ESP32 `TX1`
- **`IO35` (`GPIO 35`):** Internal 18650 Li-Ion Battery Voltage Divider

---

## Component Manual 1: Onboard Antennas, SIM Card & 18650 Backup Battery

### What You Need
1. **LTE/GSM Cellular Antenna** (flat black whip or PCB sticker antenna with tiny gold IPEX/U.FL snap plug)
2. **Active GPS Antenna** (square ceramic patch antenna with IPEX/U.FL snap plug)
3. **Nano-SIM Card** (MTN Uganda or Airtel Uganda, PIN code disabled, loaded with SMS + Data)
4. **1x 18650 Rechargeable Li-Ion Battery (`3.7V`, `2500mAh–3500mAh`)**

### Step-by-Step Installation
1. **Keep the power switch `OFF` and USB unplugged.**
2. **Snap on the Cellular Antenna:**
   - Locate the tiny round gold socket labeled **`MAIN`** near the SIM7000G metal shield.
   - Align the LTE/GSM antenna U.FL connector straight on top of **`MAIN`** and press down firmly until it clicks. *(Do NOT connect it to `AUX`.)*
3. **Snap on the Active GPS Antenna:**
   - Locate the socket labeled **`GPS`** on the opposite corner of the SIM7000G shield.
   - Align the square ceramic GPS antenna U.FL plug over **`GPS`** and press down until it clicks.
   - *Mounting rule:* The printed/ceramic face of the GPS patch must face **UP toward the sky** (under plastic fairing, never underneath metal).
4. **Insert the Nano-SIM Card:**
   - Slide the metal SIM tray cover slightly toward `OPEN` and lift it up (or push-in if spring-loaded).
   - Place the **Nano-SIM** with the **gold contacts facing DOWN** against the PCB and the **notched corner matching the silkscreen outline**.
   - Lock the metal tray cover back down.
5. **Insert the 18650 Li-Ion Battery:**
   - Flip the LILYGO board over to the black 18650 battery holder on the back.
   - **CRITICAL:** Look inside the plastic tray for the **`+`** and **`-`** symbols. Match the flat negative (`-`) end of the 18650 cell to `-` and the button positive (`+`) end to `+`. *(Inserting the 18650 backwards will immediately burn the charging chip!)*
6. **Turn On the Board Switch:**
   - Slide the small **`PWR`** switch next to the USB-C port to **`ON`**.

---

## Component Manual 2: 12V-to-5V DC-DC Buck Converter (Main Power Supply)

### Purpose
Steps down the motorcycle's `12V–14.4V` lead-acid battery to a regulated **`5.0V DC`** to power the LILYGO board and keep the onboard 18650 backup battery charged.

### What the Component Looks Like (LM2596 or Waterproof 12V-to-5V Buck Module)
- Has 4 pads/wires: **`IN+`**, **`IN-`**, **`OUT+`**, **`OUT-`** (and a brass tuning screw if using an adjustable LM2596).

### Wiring Schematic
```text
Motorcycle +12V Battery ──► [ 3A Blade Fuse ] ──► [ IN+       OUT+ ] ──(Red Wire)──► LILYGO 5V Pin
                                                  |  LM2596 Buck   |
Motorcycle Chassis GND  ────────────────────────► [ IN-       OUT- ] ─(Black Wire)─► LILYGO GND Pin
```

### Pin-by-Pin Connection Table
| From Buck Converter Pin | Recommended Wire Color | Connect To | Notes |
| :--- | :--- | :--- | :--- |
| **`IN+`** | Red (with `3A` Fuse) | **Motorcycle Battery `+12V`** | Protects wiring in case of short circuit |
| **`IN-`** | Black | **Motorcycle Battery `-` (Chassis `GND`)** | Input ground |
| **`OUT+`** | Red | **LILYGO `5V` Pin** (or `VIN`) | **Must be adjusted to `5.0V` BEFORE connecting LILYGO!** |
| **`OUT-`** | Black | **LILYGO `GND` Pin** | Common ground |

### Step-by-Step Wiring & Calibration
1. **Do NOT connect `OUT+` to the LILYGO yet.**
2. Connect **`IN+`** and **`IN-`** to your `12V` motorcycle battery (or 12V bench supply).
3. Place your multimeter probes on **`OUT+`** and **`OUT-`** (DC Volts mode).
4. If using an adjustable LM2596 module, turn the brass potentiometer screw counter-clockwise until the multimeter reads exactly **`5.00V DC`** (acceptable range: `4.95V – 5.10V`).
5. Disconnect 12V power, then solder/wire **`OUT+` to LILYGO `5V`** and **`OUT-` to LILYGO `GND`**.

---

## Component Manual 3: 12V Battery Voltage Sensor & Wire-Cut Detector (`GPIO 34`)

### Purpose
Allows the ESP32 (`3.3V` max ADC input) to safely measure the motorcycle's `12V–14.5V` battery voltage and **instantly detect if a thief cuts the 12V battery cable** (`battery_voltage` drops below `5.0V`, triggering the siren, SMS, and voice call while running on the 18650 cell).

### What You Need
- **1x `100kΩ` Resistor (`R1`)** — Color bands: *Brown, Black, Yellow, Gold* (or *Brown, Black, Black, Orange, Brown*)
- **1x `22kΩ` Resistor (`R2`)** — Color bands: *Red, Red, Orange, Gold*

### Wiring Schematic
```text
Motorcycle +12V (Before Buck) ───► [ R1 = 100kΩ Resistor ] ───┬───► LILYGO GPIO 34 (IO34)
                                                              │
LILYGO GND / Bike GND         ◄─── [ R2 = 22kΩ Resistor  ] ───┘
```

### Pin-by-Pin Connection Table
| Resistor Leg / Junction | Connect To | Voltage Level |
| :--- | :--- | :--- |
| **Leg 1 of `R1` (`100kΩ`)** | **Motorcycle `+12V`** (same line as Buck `IN+`) | `12.0V – 14.5V` |
| **Middle Junction (`R1` + `R2` twisted together)** | **LILYGO `IO34` (`GPIO 34`)** | **`2.16V – 2.61V`** (Safe for 3.3V ESP32) |
| **Leg 2 of `R2` (`22kΩ`)** | **LILYGO `GND`** | `0.0V` |

### Step-by-Step Assembly & Verification
1. Twist one leg of the **`100kΩ` resistor (`R1`)** together with one leg of the **`22kΩ` resistor (`R2`)** and solder them together. This middle junction is your **Sense Output**.
2. Solder a jumper wire from that **Middle Junction** to **LILYGO `IO34` (`GPIO 34`)**.
3. Connect the free leg of the **`22kΩ` resistor (`R2`)** to **LILYGO `GND`**.
4. Connect the free leg of the **`100kΩ` resistor (`R1`)** to the **Motorcycle `+12V`** wire.
5. **Multimeter Test:** With `12.6V` applied, measure DC voltage between **`IO34`** and **`GND`**. It must read **`~2.27V`** (never above `3.3V`).

---

## Component Manual 4: 5V Immobilizer Relay Module (`GPIO 23`)

### Purpose
Remotely cuts or restores the motorcycle's engine ignition when you click **Immobilize / Restore** in the Customer Portal or send SMS **`STOP` / `START`**.

### What the Component Looks Like (Single-Channel 5V Relay Module)
- **Low-Voltage Logic Header (3 pins):**
  - `VCC` (or `DC+` / `+`)
  - `GND` (or `DC-` / `-`)
  - `IN` (or `S` / `SIG`)
- **High-Voltage Blue Screw Terminal Block (3 screws):**
  - `NO` (Normally Open)
  - `COM` (Common — middle screw)
  - `NC` (Normally Closed)

### Wiring Schematic
```text
                         +-------------------------------+
  5V Buck OUT+ (5.0V) ──►| VCC                       NO  | (Leave Empty)
                         |                               |
  LILYGO GND          ──►| GND    5V RELAY MODULE    COM |◄── Motorcycle CDI Ignition Wire (End A)
                         |                               |
  LILYGO IO23 (GPIO23)──►| IN                        NC  |──► Motorcycle CDI Ignition Wire (End B)
                         +-------------------------------+
```

### Pin-by-Pin Connection Table
| Side | Relay Terminal | Connect To | Function |
| :--- | :--- | :--- | :--- |
| **Control Pins** | **`VCC` (`DC+`)** | **`5V` Rail** (shared with Buck `OUT+` / LILYGO `5V`) | Powers the 5V relay coil |
| **Control Pins** | **`GND` (`DC-`)** | **LILYGO `GND`** | Common ground |
| **Control Pins** | **`IN` (`SIG`)** | **LILYGO `IO23` (`GPIO 23`)** | `LOW (0V)` = Engine ON; `HIGH (3.3V)` = Engine CUT |
| **Screw Terminals** | **`COM` (Middle)** | **Cut CDI Ignition Wire — Side A** (from key switch/stator) | Common contact |
| **Screw Terminals** | **`NC` (Normally Closed)** | **Cut CDI Ignition Wire — Side B** (going into CDI box) | Closed by default so bike runs normally with zero battery drain |
| **Screw Terminals** | **`NO` (Normally Open)** | *Do Not Connect* | Unused |

### Step-by-Step Wiring & Verification
1. Wire Relay **`VCC`** to **`5V`**, Relay **`GND`** to **`GND`**, and Relay **`IN`** to **LILYGO `IO23` (`GPIO 23`)**.
2. *(Note for jumper-equipped relay boards)*: If your relay module has a `High/Low Level Trigger` jumper cap, place the jumper on **`HIGH` (`H`)** so the relay only energizes when `GPIO 23` outputs `HIGH` (`3.3V`).
3. Locate the motorcycle's **CDI Ignition Power wire** (or Kill-Switch feed wire under the seat/tank) and cut it into two ends.
4. Strip 6mm of insulation from both ends, insert one end into the **`COM`** screw terminal and the other end into the **`NC`** screw terminal, and tighten both screws firmly.
5. **Bench Test via Serial Monitor (`115200` baud):**
   - Type **`ENGINE_CUT`** &rarr; You will hear a sharp mechanical *CLICK*, the relay status LED turns ON, and `COM` to `NC` opens (engine immobilized).
   - Type **`ENGINE_RESTORE`** &rarr; The relay clicks OFF, and `COM` to `NC` closes (engine enabled).

---

## Component Manual 5: Piezoelectric Ceramic Vibration / Tamper Sensor (`GPIO 32`)

### Purpose
Detects physical shocks, hammer strikes, lock-picking, or someone tampering with the parked motorcycle, triggering the 10-second local siren, a direct SIM7000G tamper SMS with Google Maps coordinates, and a live GSM voice call to the owner.

### Option A: 3-Pin Ceramic Piezo Vibration Sensor Module (Recommended)
Most Arduino Piezo Vibration modules have 3 pins (`S`, `+`, `-`) or 2 pins (`S`, `GND`):

```text
  +------------------------------------+
  |  (Brass Piezo Disc)                |
  |        |                           |
  |  [ Piezo Sensor PCB ]              |
  |    S (or OUT)          GND (or -)  |
  +----|-----------------------|-------+
       |                       |
       ▼                       ▼
  LILYGO IO32 (GPIO 32)   LILYGO GND
```

| Sensor Module Pin | Connect To | Notes |
| :--- | :--- | :--- |
| **`S` (or `OUT` / `ADO`)** | **LILYGO `IO32` (`GPIO 32`)** | Analog piezoelectric output (`ADC1_CH4`) |
| **`GND` (or `-`)** | **LILYGO `GND`** | Common ground |
| **`VCC` (or `+`)** *(if present on 3-pin active board)* | **LILYGO `3V3`** | *(Passive piezo boards do not use `VCC`; only connect if your board has an op-amp IC)* |

### Option B: Bare 2-Wire Brass Piezo Disc (Without a Module PCB)
If you are using a raw round brass piezo element with just a **Red (`+`)** and **Black (`-`)** wire:
```text
Piezo Red Wire (+)   ──────────┬────────► LILYGO IO32 (GPIO 32)
                               │
                        [ 1MΩ Resistor ] (Brown-Black-Green-Gold)
                               │
Piezo Black Wire (-) ──────────┴────────► LILYGO GND
```
- **Why the `1MΩ` resistor?** A raw piezo crystal acts like a capacitor. Placing a **`1MΩ` bleed resistor** in parallel between **`IO32`** and **`GND`** keeps the resting DC baseline stable at `~475` ADC counts and prevents false triggers.

### Step-by-Step Mounting & Test
1. Wire the signal pin to **`IO32` (`GPIO 32`)** and ground to **`GND`**.
2. Tape or screw the flat brass face of the piezo disc tightly against the metal frame of the motorcycle under the seat.
3. **Bench Test:** Open the Arduino Serial Monitor (`115200` baud). Every 3 seconds you will see `PiezoPeak:` printed in the log. Tap the brass disc with your finger—when the tap exceeds the calibrated threshold, the tracker immediately triggers `TAMPER_DETECTED`!

---

## Component Manual 6: Local Alarm Buzzer / 12V Motorcycle Siren (`GPIO 14`)

### Purpose
Sounds an immediate **10-second audible alarm** on the motorcycle the moment physical tampering or a 12V battery wire cut is detected, even if there is zero cellular network coverage.

### Option A: 3.3V / 5V Active Piezo Buzzer (Direct Connection)
An **Active Buzzer** (black cylinder with a `+` sticker on top and two legs) has a built-in oscillator and sounds a loud tone as soon as DC voltage is applied:
```text
LILYGO IO14 (GPIO 14) ─────► (+) Active Buzzer (-) ─────► LILYGO GND
```
| Buzzer Pin | Connect To | Notes |
| :--- | :--- | :--- |
| **Long Leg / `+` Terminal** | **LILYGO `IO14` (`GPIO 14`)** | Driven `HIGH (3.3V)` for 10 seconds during tamper |
| **Short Leg / `-` Terminal** | **LILYGO `GND`** | Common ground |

### Option B: Loud 12V Motorcycle Horn / Siren (Using NPN Transistor or 2nd Relay)
Because `GPIO 14` outputs `3.3V` (max `20mA`), if you want to drive a high-power **12V Motorcycle Siren**, wire `GPIO 14` through a **`2N2222` / `TIP120` NPN transistor** (or a second 5V relay module):
```text
Motorcycle +12V ──────┬──────► (+) 12V Siren (-) ──────┐
                      │                                │
               [ 1N4007 Diode ] (Cathode stripe to +12V)│
                      │                                ▼
                      └──────────────────────────► Collector (C)
LILYGO IO14 (GPIO 14) ──► [ 1kΩ Resistor ] ──────► Base (B)      [2N2222 / TIP120 NPN]
LILYGO GND  ─────────────────────────────────────► Emitter (E)
```

### Bench Test
- In the Arduino Serial Monitor (`115200` baud), type **`TEST_TAMPER`** and press Enter.
- **`IO14` (`GPIO 14`)** will immediately go `HIGH` for 10 seconds while the onboard Blue LED (`GPIO 12`) rapidly strobes!

---

## Component Manual 7: Optional MPU-6050 6-Axis Accelerometer (`GPIO 21` & `GPIO 22`)

### Purpose
*(Optional add-on)* Works alongside the Piezo sensor on `GPIO 32` to measure 3D G-force impacts (`> 3.2G`) and motorcycle tilt/crash angles over the `I2C` bus. The firmware ([`esp32_boda_node.ino`](esp32_boda_node/esp32_boda_node.ino#L235-L243)) automatically detects whether an MPU-6050 is wired at startup.

### Wiring Schematic
```text
  +------------------------+
  |  MPU-6050 Module (GY-521)
  |  VCC   GND   SCL   SDA |
  +---|-----|-----|-----|--+
      |     |     |     |
      |     |     |     +────► LILYGO IO21 (GPIO 21 - I2C SDA)
      |     |     +──────────► LILYGO IO22 (GPIO 22 - I2C SCL)
      |     +────────────────► LILYGO GND
      +──────────────────────► LILYGO 3V3
```

### Pin-by-Pin Connection Table
| MPU-6050 Pin | Connect To LILYGO Pin | Function |
| :--- | :--- | :--- |
| **`VCC`** | **`3V3`** | 3.3V clean logic power |
| **`GND`** | **`GND`** | Common ground |
| **`SDA`** | **`IO21` (`GPIO 21`)** | I2C Serial Data |
| **`SCL`** | **`IO22` (`GPIO 22`)** | I2C Serial Clock |
| `XDA`, `XCL`, `AD0`, `INT` | *Leave Unconnected* | Default I2C address is `0x68` when `AD0` is unconnected |

---

## 8. Final Step-by-Step Assembly & Testing Order

To wire your LILYGO T-SIM7000G without mistakes, follow this exact sequence:

1. **Step 1 (Unpowered Prep):** Snap on the **`MAIN`** (Cellular) and **`GPS`** antennas, insert the **Nano-SIM card**, and insert the **18650 battery** (double-checking `+` and `-` polarity).
2. **Step 2 (Calibrate 5V Buck First):** Connect the 12V-to-5V buck converter to your 12V battery and verify with a multimeter that `OUT+` and `OUT-` read **`5.0V`** before connecting them to LILYGO **`5V`** and **`GND`**.
3. **Step 3 (Common Ground Rail):** Tie **all `GND` wires together** (Motorcycle `-`, Buck `OUT-`, LILYGO `GND`, Relay `GND`, Piezo `GND`, Buzzer `-`, and the bottom leg of the `22kΩ` voltage-divider resistor).
4. **Step 4 (Signal Pins):**
   - Connect the **12V Voltage Divider midpoint (`100kΩ` + `22kΩ`)** &rarr; **`IO34`**
   - Connect the **5V Relay Module `IN`** &rarr; **`IO23`** (and `VCC` &rarr; `5V`)
   - Connect the **Piezo Vibration Sensor `S` (`+`)** &rarr; **`IO32`**
   - Connect the **Active Buzzer `+`** &rarr; **`IO14`**
   - *(Optional)* Connect **MPU-6050 `SDA` &rarr; `IO21`, `SCL` &rarr; `IO22`, `VCC` &rarr; `3V3`**
5. **Step 5 (Interactive Verification in Serial Monitor at `115200` baud):**
   - Plug the LILYGO into your PC via USB-C and slide the onboard **`PWR` switch to `ON`**.
   - Type **`WHOAMI`** &rarr; Confirms SIM7000G booted and read its 15-digit hardware IMEI.
   - Type **`ENGINE_CUT`** and **`ENGINE_RESTORE`** &rarr; Verifies the immobilizer relay on `IO23` clicks ON and OFF.
   - Type **`TEST_TAMPER`** &rarr; Verifies the buzzer on `IO14` sounds for 10s, SIM7000G sends a GPS tamper SMS, and places a voice call.
