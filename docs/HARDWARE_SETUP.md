# Hardware Setup Guide

This guide provides detailed information about the ESP32-C5 hardware requirements, pin configurations, and physical setup for the Unified Gateway (Zigbee2MQTT + Bluetooth + ESPHome API).

## Table of Contents

- [ESP32-C5 Overview](#esp32-c5-overview)
- [Development Board](#development-board)
- [Pin Configuration](#pin-configuration)
- [Power Requirements](#power-requirements)
- [Antenna Configuration](#antenna-configuration)
- [USB Connection](#usb-connection)
- [Optional External Hardware](#optional-external-hardware)

## ESP32-C5 Overview

### SoC Specifications

The ESP32-C5 is a highly integrated RISC-V based SoC with wireless connectivity designed for IoT applications.

| Specification | Details |
|--------------|---------|
| **CPU** | RISC-V 32-bit single-core @ 240 MHz |
| **RAM** | 384 KB SRAM + 8 MB PSRAM |
| **Flash** | Supports up to 16 MB external flash (8 MB minimum recommended) |
| **WiFi** | IEEE 802.11ax (WiFi 6), 2.4 GHz and 5 GHz dual-band |
| **Zigbee** | IEEE 802.15.4 @ 2.4 GHz (Zigbee 3.0 coordinator) |
| **Peripherals** | GPIO, UART, SPI, I2C, I2S, PWM, ADC, DAC |
| **Operating Voltage** | 3.0V to 3.6V |
| **Operating Temperature** | -40°C to +85°C |

### Key Features

- **Tri-Radio Wireless**: WiFi 6 (dual-band), Zigbee 3.0, and Bluetooth 5.0 LE operate simultaneously
- **WiFi Dual-Band**: 2.4 GHz and 5 GHz support (5 GHz preferred for tri-radio operation)
- **Hardware Co-existence**: Built-in arbitration mechanism for WiFi/BT/Zigbee on 2.4 GHz
- **Low Power**: Multiple power modes for battery-operated applications
- **Secure Boot**: Hardware security features including secure boot and flash encryption
- **Rich Peripherals**: Extensive GPIO and peripheral support

## Development Board

### ESP32-C5-DevKitC-1

The recommended development board for this project is the ESP32-C5-DevKitC-1.

**Board Features:**
- USB-C connector for power and programming
- On-board USB-to-UART bridge (CP2102N)
- Reset and Boot buttons
- Power LED indicator
- User programmable RGB LED (GPIO8)
- Dual antenna connectors (PCB antenna + external U.FL)
- All GPIO pins broken out to header pins
- 3.3V LDO regulator (up to 500mA)

### Board Layout

```
                USB-C Connector
                      |
    [RST]  [BOOT]    [ESP32-C5]    [RGB LED]
      |      |          |              |
    |-----------------------------------------|
    | GND  3V3 | GPIO Pins Header  | 5V  GND |
    |-----------------------------------------|
                      |
              External Antenna (U.FL)
```

### Pin Headers

The DevKitC-1 breaks out all GPIO pins on 0.1" headers:

**Left Side (Top to Bottom):**
- 3V3, GND
- GPIO0 - GPIO10
- 5V, GND

**Right Side (Top to Bottom):**
- GPIO11 - GPIO21
- RXD0, TXD0
- GND

## Pin Configuration

### Default Pin Usage

This gateway firmware uses the following pins by default:

| Pin | Function | Notes |
|-----|----------|-------|
| GPIO0 | Boot Mode | Low during boot enters download mode |
| GPIO8 | Status LED (RGB) | Optional status indicator |
| GPIO19 | UART TX | Debug console output |
| GPIO20 | UART RX | Debug console input |
| USB D+/D- | USB-UART | Used for flashing and monitoring |

### Zigbee Radio

The Zigbee radio uses internal hardware and does not require external pins. It operates on 2.4 GHz using the integrated IEEE 802.15.4 transceiver.

### WiFi Radio

WiFi uses internal hardware with dual-band support:
- **2.4 GHz**: Shared with Zigbee and Bluetooth through hardware co-existence
- **5 GHz**: Preferred for tri-radio operation to minimize 2.4 GHz interference

### Bluetooth Radio 🔵

Bluetooth LE 5.0 radio uses internal hardware:
- Operates on 2.4 GHz with Adaptive Frequency Hopping (AFH)
- Shares 2.4 GHz band with WiFi and Zigbee through hardware arbitration
- No external pins required
- Supports passive scanning and active GATT connections

### Available GPIO

All other GPIO pins (GPIO1-7, GPIO9-18, GPIO21) are available for:
- External sensors
- Status LEDs
- Buttons
- I2C devices
- SPI devices
- UART peripherals

**GPIO Limitations:**
- Some GPIOs are strapping pins (GPIO0, GPIO2, GPIO8, GPIO9)
- Check datasheet for specific pin capabilities
- PSRAM uses 6 pins (GPIO33-38) - not available on headers

## Power Requirements

### Power Input

The ESP32-C5 gateway can be powered in several ways:

1. **USB-C Port** (Recommended)
   - Input: 5V DC via USB-C
   - Current: Minimum 500mA, 1A recommended
   - Regulated to 3.3V by on-board LDO

2. **3V3 Pin**
   - Direct 3.3V input to 3V3 header pin
   - **Warning**: Bypasses LDO regulator
   - Current: Minimum 500mA, 1A recommended
   - Must be clean, regulated 3.3V

3. **5V Pin**
   - 5V input to 5V header pin
   - Regulated to 3.3V by on-board LDO
   - Current: Minimum 500mA, 1A recommended

### Current Consumption

Typical current consumption during operation:

| Mode | Current (mA) | Notes |
|------|--------------|-------|
| **Active (WiFi + Zigbee)** | 150-250 mA | Two radios active |
| **Active (WiFi + Zigbee + BT)** | 200-300 mA | Tri-radio operation 🔵 |
| **WiFi TX (Peak)** | 300-400 mA | WiFi transmitting |
| **Zigbee TX (Peak)** | 150-200 mA | Zigbee transmitting |
| **BLE Scan (Active)** | 40-60 mA | Bluetooth scanning 🔵 |
| **BLE GATT (Active)** | 60-80 mA | Bluetooth connections 🔵 |
| **WiFi RX** | 80-120 mA | WiFi receiving |
| **Idle (Radio Sleep)** | 30-50 mA | Radios in sleep mode |
| **Deep Sleep** | 10-50 µA | Not used in gateway mode |

**Power Supply Requirements:**
- Provide clean, stable power with low ripple
- Use adequate capacitance near ESP32-C5 (10µF + 0.1µF)
- For reliable operation, use power supply rated for 500mA minimum

## Antenna Configuration

The ESP32-C5 DevKitC-1 supports two antenna options:

### 1. PCB Antenna (Default)

- Integrated PCB antenna on board
- Omnidirectional radiation pattern
- Typical gain: 2-3 dBi
- Effective range: 10-30 meters indoors
- **Pros**: No additional components needed
- **Cons**: Limited range, affected by nearby metal

### 2. External Antenna (U.FL Connector)

- U.FL/IPEX connector for external antenna
- Supports 2.4 GHz antennas
- Typical gain: 3-5 dBi (depending on antenna)
- Effective range: 30-100+ meters
- **Pros**: Better range and signal quality
- **Cons**: Requires external antenna

### Antenna Selection

To switch between antennas:
- **Hardware**: Check if your board has an RF switch resistor
- **Default**: Most boards default to PCB antenna
- **To use external antenna**: May need to move 0Ω resistor (check board schematic)

**Antenna Recommendations:**
- Use PCB antenna for development and testing
- Use external antenna for production or extended range
- Ensure proper antenna placement (away from metal, enclosures)
- Never operate without an antenna (can damage RF circuit)

### Antenna Placement Tips

For optimal performance:
- Keep antenna clear of metal objects
- Mount vertically for omnidirectional coverage
- Avoid placing near power supplies or switching circuits
- Test signal strength with RSSI monitoring

## USB Connection

### USB-C Cable Requirements

Use a quality USB-C cable with:
- Data lines connected (not power-only cable)
- USB 2.0 specification minimum
- Maximum length 1.5-2 meters for reliable communication
- Shielded cable recommended to reduce EMI

### USB-to-UART Bridge

The DevKitC-1 includes a CP2102N USB-to-UART bridge:
- **Baud Rate**: 115200 (default), supports up to 2 Mbps
- **Driver**: Usually auto-detected on modern OS
- **Manual Driver**: Available from Silicon Labs website

#### Driver Installation

**Windows:**
```
1. Download CP210x driver from Silicon Labs
2. Install driver package
3. Connect board - should appear as COMx port
```

**macOS:**
```
# Modern macOS includes driver
# Check device appears:
ls /dev/cu.usbserial-*
```

**Linux:**
```
# Driver built into kernel
# Check device appears:
ls /dev/ttyUSB*

# Add user to dialout group for permissions:
sudo usermod -a -G dialout $USER
# Log out and back in
```

### USB Permissions (Linux)

Create udev rule for non-root access:

```bash
# Create udev rule
sudo nano /etc/udev/rules.d/99-esp32.rules

# Add this line:
SUBSYSTEMS=="usb", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60", MODE:="0666"

# Reload rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

## Optional External Hardware

### Status LED

Connect external LED to GPIO8 for visual status indication:

```
ESP32-C5 GPIO8 --> [330Ω Resistor] --> LED Anode --> LED Cathode --> GND
```

Firmware indicates:
- Solid Green: Connected and operational
- Flashing Yellow: Connecting to WiFi/MQTT
- Flashing Blue: Zigbee device pairing mode
- Red: Error state

### Reset Button

External reset button can be added:
```
Reset Pin (EN) --> [Push Button] --> GND
```

### Boot Button

External boot button (enters download mode when held during reset):
```
GPIO0 --> [Push Button] --> GND
```

## Mounting and Enclosure

### PCB Mounting

The DevKitC-1 has mounting holes for M3 screws:
- 4x M3 mounting holes at corners
- Use plastic or nylon standoffs to avoid shorts
- Minimum 5mm clearance under board

### Enclosure Considerations

When choosing or designing an enclosure:
- **Ventilation**: Provide airflow for heat dissipation
- **Antenna**: Use plastic enclosure or antenna cutout
- **Antenna Placement**: Keep antenna near enclosure edge
- **Status LED**: Add light pipe if using external LED
- **USB Access**: Provide access to USB-C port for updates
- **Dimensions**: DevKitC-1 is approximately 25mm x 55mm

**Materials:**
- Plastic (ABS, PLA): Best for RF transparency
- Avoid metal enclosures (blocks RF signals)
- If metal enclosure needed, use antenna cutout or external antenna

## Testing Hardware Setup

### Power-On Test

1. Connect USB-C cable to PC
2. Power LED should illuminate
3. Check device appears in system:
   - Windows: Device Manager → Ports (COM & LPT)
   - macOS: `ls /dev/cu.usbserial-*`
   - Linux: `ls /dev/ttyUSB*`

### Serial Console Test

```bash
# Linux/macOS
screen /dev/ttyUSB0 115200

# Or use ESP-IDF monitor
idf.py monitor

# Windows
putty.exe -serial COM3 -sercfg 115200,8,n,1,N
```

Press reset button - should see boot messages.

### LED Test (Optional)

If RGB LED is installed (GPIO8), it may flash during boot.

## Troubleshooting

### Board Not Detected

**Symptom**: USB device not appearing in system

**Solutions**:
1. Try different USB cable (must support data)
2. Try different USB port
3. Install/update CP210x driver
4. Check USB cable length (<2m)
5. Try different computer to isolate issue

### Cannot Flash Device

**Symptom**: Flash fails with "Timed out waiting for packet header"

**Solutions**:
1. Hold BOOT button while connecting USB
2. Press BOOT, then press and release RESET, then release BOOT
3. Check USB cable quality
4. Try lower baud rate: `idf.py flash -b 115200`
5. Verify correct USB port selected

### Frequent Resets/Crashes

**Symptom**: Device keeps resetting randomly

**Solutions**:
1. Check power supply - use 1A minimum
2. Try powered USB hub if using USB power
3. Check for loose connections
4. Verify USB cable quality
5. Check for shorts on board
6. Ensure antenna is connected (do not operate without antenna)

### Poor RF Performance

**Symptom**: Low WiFi/Zigbee range or connection issues

**Solutions**:
1. Check antenna connection
2. Try external antenna
3. Keep antenna clear of metal objects
4. Adjust Zigbee channel to avoid WiFi interference
5. Check WiFi RSSI with `wifi_manager_get_rssi()`
6. Position gateway centrally in coverage area

## Hardware Datasheet and Resources

- [ESP32-C5 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c5_datasheet_en.pdf)
- [ESP32-C5 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-c5_technical_reference_manual_en.pdf)
- [ESP32-C5-DevKitC-1 Schematic](https://dl.espressif.com/dl/schematics/SCH_ESP32-C5-DEVKITC-1_V1_0_20231017.pdf)
- [CP210x USB Driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)

## Tri-Radio Coexistence Considerations 🔵

When operating all three radios (WiFi + Zigbee + Bluetooth):

### Frequency Planning
- **WiFi**: Use 5 GHz (STRONGLY RECOMMENDED)
- **Zigbee**: Channel 15, 20, or 25 (avoids WiFi overlap)
- **Bluetooth**: AFH automatically avoids interference

### Power Requirements
- Use 1A+ power supply for tri-radio operation
- Peak current can reach 400mA with all radios transmitting

### Antenna Considerations
- All three radios share the same 2.4 GHz antenna
- 5 GHz WiFi uses separate antenna (dual-band boards)
- External antenna recommended for better range with tri-radio

### Performance Notes
- WiFi 5 GHz frees up 2.4 GHz spectrum
- Hardware arbitration prevents conflicts
- Expect ~10-15% reduction in throughput vs single-radio

## Next Steps

After hardware setup is complete:
1. Proceed to [Installation Guide](INSTALLATION.md) to set up the software environment
2. Follow [Configuration Guide](CONFIGURATION.md) to configure WiFi, MQTT, Zigbee, and Bluetooth settings
3. See [Bluetooth Gateway Guide](BLUETOOTH_GATEWAY.md) for BLE device setup 🔵
4. Review [Coexistence Guide](COEXISTENCE.md) for WiFi/BT/Zigbee optimization 🔵
5. See [Usage Guide](USAGE.md) for connecting devices and integration with Home Assistant
