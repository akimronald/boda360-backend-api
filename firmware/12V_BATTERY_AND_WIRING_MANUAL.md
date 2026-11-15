# Boda 360 — 12V Motorcycle Battery & Complete Hardware Wiring Manual

**Target Controller:** LILYGO TTGO T-SIM7000G (ESP32 + Onboard SIM7000G Cellular/GPS + 18650 Backup Holder)  
**Firmware File:** `firmware/esp32_boda_node.ino`  
**Provisioned Bike / Emergency Number:** `UEV 412K` (`IMEI: 865421045981201`) | SMS: `+256748802162` | Voice Call: `0748802162`

---

## 1. CRITICAL SAFETY RULES (Read Before Connecting the 12V Battery!)

1. **NEVER connect `12V` directly to any ESP32 pin, `5V` pin, or `3.3V` pin.** Direct `12V` will permanently burn the LILYGO T-SIM7000G instantly.
2. **Pre-set Adjustable Buck Converters FIRST:** If you are using an adjustable buck converter (such as an `LM2596` module with a brass screw), connect it to the `12V` battery **by itself first** and turn the screw until a multimeter on `OUT+ / OUT-` reads **`5.0V`** *before* wiring `OUT+` to the LILYGO or Relay!
3. **Wire the `22 kΩ` Ground Resistor (`R2`) FIRST:** On `GPIO 34`, always connect the `22 kΩ` resistor between `GPIO 34` and `GND` **before** connecting the `100 kΩ` resistor (`R1`) to `+12V`. If `R2` is missing, `GPIO 34` will receive un-divided voltage!
4. **Check 18650 Backup Polarity:** Look closely at the `+` and `-` markings inside the black `18650` battery holder on the back of the LILYGO T-SIM7000G before inserting the cell (reverse polarity damages the charging IC). Flip the small onboard **BAT switch to `ON`**.

---

## 2. Parts Checklist for 12V Battery & Power-Cut Detection

| Qty | Component | Value / Spec | Role in System |
| :---: | :--- | :--- | :--- |
| **1** | **Motorcycle Battery** | `12V` Lead-Acid / AGM (`12.0V–14.5V`) | Main motorcycle power source |
| **1** | **Inline Blade Fuse** *(recommended)* | `2A` or `3A` automotive fuse | Protects wire near the `+12V` battery terminal |
| **1** | **DC-DC Step-Down (Buck) Converter** | `12V -> 5.0V` (`>= 2A`, e.g. `LM2596` or sealed `12V-to-5V` module) | Steps `12V` down to `5.0V` to run LILYGO + 2-Way Relay and charge the `18650` |
| **1** | **18650 Li-ion Rechargeable Cell** | `3.7V` (`2000–3500 mAh`) | Inserted in back of LILYGO — keeps ESP32 + SIM7000G alive when `12V` is cut! |
| **1** | **Resistor `R1`** | **`100 kΩ`** (1/4W or 1/2W) — *4-band: Brown-Black-Yellow-Gold* | Upper voltage-divider resistor from `+12V` to `GPIO 34` |
| **1** | **Resistor `R2`** | **`22 kΩ`** (1/4W or 1/2W) — *4-band: Red-Red-Orange-Gold* | Lower voltage-divider resistor from `GPIO 34` to `GND` |

### Voltage Divider Math Check (`R1 = 100 kΩ`, `R2 = 22 kΩ`)
$$V_{\text{GPIO34}} = V_{\text{Battery}} \times \frac{22}{100 + 22} = V_{\text{Battery}} \times 0.1803$$

- At **`12.6V`** (resting bike battery) -> `GPIO 34` receives **`2.27V`** (Safe!)
- At **`14.5V`** (engine running / alternator charging) -> `GPIO 34` receives **`2.61V`** (Safe — well below `3.3V` max!)
- At **`0.0V`** (battery unplugged or wire cut) -> `GPIO 34` drops to **`0.00V`** -> Triggers SMS + Owner Call in **0.6 seconds**!

---

## 3. 12V Battery Power & Cut-Sense Wiring Diagram

> **IMPORTANT:** Tap the `100 kΩ` resistor (`R1`) on the **`12V` input side BEFORE the Buck Converter**, so the ESP32 sees the drop immediately even before the buck converter's output capacitors discharge.

```text
                       [ 2A Fuse ]
12V Bike Battery (+) ──────═══──────┬───────────────────────► Buck Converter IN+
                                    │                         Buck Converter OUT+ (5.0V) ──┬──► LILYGO 5V Pin (or USB-C)
                                    │                                                      └──► 2-Way Relay VCC Pin
                               [ 100 kΩ ] (R1)
                                    │
                                    ├───────────────────────► LILYGO GPIO 34  (12V Cut-Sense Input)
                                    │
                                [ 22 kΩ ] (R2)
                                    │
12V Bike Battery (-) ───────────────┴───────────────────────► Buck Converter IN- / OUT-
                                                                     │
                                                                     ├──► LILYGO GND Pin
                                                                     └──► 2-Way Relay GND Pin
```

*(Note: On standard non-isolated `LM2596` / `MP1584` buck converters, `IN-` and `OUT-` are already connected together internally as common ground. Always verify continuity or tie all `GND` wires cleanly together.)*

---

## 4. Complete Master Pinout Table (All Sensors & 2-Way Relay)

Use this table to verify every wire connected to your **LILYGO T-SIM7000G**:

| Module / Component | Module Pin | LILYGO T-SIM7000G Pin | Notes |
| :--- | :--- | :--- | :--- |
| **12V Power Supply (Buck `5.0V`)** | `OUT+ (5.0V)` | **`5V`** *(or USB-C)* | Powers board & charges onboard `18650` battery |
| **12V Power Supply Ground** | `OUT- / GND` | **`GND`** | Common ground shared across battery, buck, ESP32 & relay |
| **12V Cut-Sense Divider** | Middle junction (`R1` + `R2`) | **`GPIO 34`** | `R1 (100kΩ)` to `+12V`; `R2 (22kΩ)` to `GND` |
| **Onboard 18650 Backup Sense** | *(Internal on board)* | **`GPIO 35`** | Built into LILYGO T-SIM7000G — **do not connect external wires to `GPIO 35`** |
| **2-Way Relay Module (Channel 1)** | `VCC` | **`5V`** *(from Buck `5.0V`)* | Also ensure `JD-VCC — VCC` jumper cap is installed on the relay board |
| **2-Way Relay Module (Channel 1)** | `GND` | **`GND`** | **Must be solid!** Without `GND`, the red LED switches silently with no click |
| **2-Way Relay Module (Channel 1)** | `IN1` | **`GPIO 23`** | Active-LOW (`IN2` left unconnected as spare) |
| **MPU-6050 Motion Sensor** | `VCC` | **`3.3V`** | Mutes Piezo vibration alerts while the motorcycle is being ridden |
| **MPU-6050 Motion Sensor** | `GND` | **`GND`** | Common ground |
| **MPU-6050 Motion Sensor** | `SDA` | **`GPIO 21`** | I2C Data |
| **MPU-6050 Motion Sensor** | `SCL` | **`GPIO 22`** | I2C Clock (`AD0` unconnected or `GND` for `0x68`) |
| **Piezo Vibration Sensor** | Signal (`S` / `+`) | **`GPIO 32`** | Analog shock/tamper sensor (with `1 MΩ` parallel bleed to `GND` if bare disc) |
| **Piezo Vibration Sensor** | `GND` (`-`) | **`GND`** | Common ground |
| **Local Alarm Buzzer / Siren** | Positive (`+` / `I/O`) | **`GPIO 14`** | Active buzzer (`HIGH` = 10s pulsing alarm on tamper / battery cut) |
| **Local Alarm Buzzer / Siren** | Negative (`-`) | **`GND`** | Common ground |

### 2-Way Relay Channel 1 — Motorcycle Ignition / CDI Wire Connection (3 Screw Terminals)
- **`COM1` (Middle Screw Terminal):** Connect to one side of the cut motorcycle Ignition Switch / CDI kill-wire.
- **`NC1` (Normally Closed Terminal):** Connect to the other side of the cut motorcycle Ignition Switch / CDI kill-wire.
- **`NO1` (Normally Open Terminal):** Leave **EMPTY**.
- *Why `COM1` + `NC1`?* During normal riding, the relay is OFF (`0 mA` drain, red `IN1` LED OFF) and the bike runs normally. When `ENGINE_CUT` (or SMS `STOP`) is sent, Channel 1 clicks ON (red `IN1` LED ON) and breaks the ignition circuit.

---

## 5. Step-by-Step Installation Order (Follow 1 -> 7 When You Get the Battery)

1. **Unplug USB and turn OFF power** while wiring resistors and power leads.
2. **Insert the charged `18650` battery** into the back of the LILYGO T-SIM7000G (double-check `+` and `-` markings!) and slide the small onboard battery switch to **`ON`**.
3. **Set Buck Converter to `5.0V`:** Connect your buck converter `IN+` and `IN-` to the `12V` battery *alone* first. Measure `OUT+` and `OUT-` with a multimeter and confirm it reads **`4.95V – 5.10V`**. Disconnect from the `12V` battery before continuing.
4. **Wire the `GPIO 34` Voltage Divider:**
   - Connect **`22 kΩ` (`R2`)** between **`GPIO 34`** and **`GND`**.
   - Connect **`100 kΩ` (`R1`)** between **`GPIO 34`** and the **`+12V` wire** (before `IN+` of the buck converter).
5. **Connect `5.0V` Power & Common `GND`:**
   - Connect Buck `OUT-` (`GND`) to LILYGO **`GND`** and 2-Way Relay **`GND`**.
   - Connect Buck `OUT+` (`5.0V`) to LILYGO **`5V`** and 2-Way Relay **`VCC`**.
6. **Connect the `12V` Motorcycle Battery (`-` black terminal first, then `+` red terminal):**
   - Wait **2 seconds**.
   - The firmware sees `>= 8.5V` (`~12.6V`) on `GPIO 34` and automatically arms the battery-cut guard:
     ```text
     [12V GUARD] 🔋 12V Motorcycle Battery detected (12.60V) — Battery Removal Tamper ARMED!
     ```
7. **Live Cut Test:**
   - Unclip the `+12V` lead from the motorcycle battery terminal (simulating a thief cutting or removing the battery).
   - The LILYGO stays powered seamlessly on its **18650 backup battery**.
   - Within **0.6 seconds**:
     1. Local alarm buzzer (`GPIO 14`) + Blue LED strobe triggers for 10 seconds.
     2. Direct MTN SMS (`[BODA POWER CUT UEV 412K]`) with live Google Maps link is sent to **`+256748802162`**.
     3. Critical `BATTERY_DISCONNECTED` alert is sent to the Customer Portal.
     4. SIM7000G dials **`0748802162`** directly to wake/alert the owner!
   - Reconnect the `+12V` lead -> after 2 seconds the system automatically clears the cut state and **re-arms** itself.

---

## 6. Serial Monitor & SMS Testing Reference (`115200` Baud)

### USB Serial Monitor Commands
| Command | What It Does |
| :--- | :--- |
| **`BAT_STATUS`** | Prints live `12V` voltage (`GPIO 34`), `18650` backup voltage (`GPIO 35`), and whether the guard is `ARMED` |
| **`TEST_BAT_CUT`** | Simulates a `12V` battery cut immediately (tests buzzer + SMS + owner phone call without a 12V battery) |
| **`TEST_RELAY`** | Clicks Relay Channel 1 (`IN1` -> `GPIO 23`) ON for 1.5 seconds and back OFF |
| **`RELAY_INVERT`** | Toggles Channel 1 trigger polarity between `ACTIVE-LOW` and `ACTIVE-HIGH` |
| **`ENGINE_CUT`** | Locks engine continuously (Relay CH1 ON, `COM1-NC1` open) |
| **`ENGINE_RESTORE`** | Restores engine (Relay CH1 OFF, `COM1-NC1` closed) |
| **`MPU_STATUS`** | Prints MPU-6050 attachment, `PARKED` vs `IN MOTION (MUTED)` state, dynamic G, and gyro rad/s |
| **`TEST_TAMPER`** | Simulates a Piezo shock tamper alert |
| **`TEST_SMS`** | Sends a test GPS location SMS to `+256748802162` |
| **`TEST_CALL`** | Places a test voice call to `0748802162` |

### Owner SMS Commands (Send from `0748802162` to the Tracker's SIM Number)
- **`STOP`** (or `LOCK` / `CUT`) -> Immobilizes the motorcycle via Relay Channel 1 and replies with Google Maps link.
- **`START`** (or `UNLOCK` / `RESTORE`) -> Restores the motorcycle engine and replies with Google Maps link.
- **`GPS`** (or `LOC` / `STATUS`) -> Replies with 3D GPS fix, `12V` battery voltage (or `CUT!`), speed, and Google Maps link.

---

## 7. Optional Desk Test (Before You Have the 12V Battery)
If you want to physically test wire-pull detection before buying the `12V` battery:
1. Connect **`22 kΩ`** between **`GPIO 34`** and **`GND`**.
2. Touch a jumper wire from LILYGO **`3.3V`** to **`GPIO 34`** for **2 seconds** -> Serial Monitor prints `Battery Removal Tamper ARMED!`.
3. Pull that `3.3V` jumper wire off `GPIO 34` -> within **0.6 seconds**, the full Battery-Cut Alarm, SMS, and Voice Call trigger!
*(Remove the direct `3.3V` jumper and insert the `100 kΩ` resistor before connecting a real `12V` battery.)*

