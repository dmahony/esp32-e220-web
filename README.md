# E220 LoRa Terminal - ESP32 Web & Serial Control

Professional web and serial terminal for the E220 LoRa module on ESP32.

## Features

- **Web Interface**: Modern, responsive UI for chat and E220 configuration
- **USB Serial Control**: Send messages and manage settings from terminal/scripts
- **Dual Interface**: Web and serial share the same message history and config
- **E220 Configuration**: 18 parameters for frequency, power, baud rate, networking, etc.
- **Real-time Chat**: Messages sync between web browser and serial connections
- **EEPROM Persistence**: Save configuration directly to E220 module memory

## Hardware Requirements

- **ESP32 Development Board** (or any ESP32 variant)
- **E220-400T30D LoRa Module** (or compatible E220 model)
- **USB Cable** for programming and serial communication

## Wiring

| E220 Pin | ESP32 Pin | Purpose |
|----------|-----------|---------|
| RX       | GPIO 21   | UART2 RX |
| TX       | GPIO 22   | UART2 TX |
| M0       | GPIO 2    | Mode control |
| M1       | GPIO 19   | Mode control |
| VCC      | 3.3V      | Power |
| GND      | GND       | Ground |

## Quick Start

### 1. Install PlatformIO

```bash
# Via pip
pip install platformio

# Or download: https://platformio.org
```

### 2. Clone and Setup

```bash
git clone https://github.com/dmahory/esp32-e220-web.git
cd esp32-e220-web
```

### 3. Build and Upload

```bash
# Connect ESP32 via USB
pio run -t upload          # Upload firmware
pio run -t uploadfs        # Upload web interface
```

### 4. Access

**Web Interface**: Open browser to `http://192.168.4.1`
- Default WiFi: SSID = "E220-Chat", Password = "password123"

**Serial Control**: 
```bash
stty -F /dev/ttyUSB0 115200 raw -echo
cat /dev/ttyUSB0 &
echo "help" > /dev/ttyUSB0
```

## Web Interface

### Chat Tab
- Send/receive messages via E220 module
- Messages appear in real-time
- Full message history available

### Config Tab
**Basic Settings**
- Frequency (433-930 MHz)
- TX Power (0-30 dBm)
- Baud Rate (1200-115200)
- Node & Destination Addresses (hex)
- Network ID & Channel

**Advanced Settings**
- Air Data Rate
- Packet & Subpacket Size
- RX Timeout, TX Wait
- CRC, Repeater, Parity
- TX Mode, Save Type

## Serial Commands

```bash
send <message>          # Send message via E220
config                  # Show current config
history                 # Show chat history
clear                   # Clear chat history
help                    # Show commands
```

### Example

```bash
$ echo "send Hello from serial" > /dev/ttyUSB0
[OK] Sent

$ echo "config" > /dev/ttyUSB0
freq=930.125
txpower=0
baud=9600
addr=0x0000
dest=0xFFFF
...
```

## Configuration

### Default Settings

```
Frequency:    930.125 MHz
TX Power:     0 dBm (Min)
Baud Rate:    9600
Node Address: 0x0000
Dest Address: 0xFFFF
Network ID:   0
Channel:      0
Air Rate:     38.4 kbps
Packet Size:  240 bytes
CRC:          Enabled
```

### Change Settings

1. **Via Web**: Click Config tab, adjust values, click "Save Configuration"
2. **Via Serial**: Commands coming soon (currently read-only from serial)

### Save to E220 EEPROM

Click **"⚡ Quick Save"** to write configuration to E220 module memory.
- Ensures settings persist after power cycle
- Requires "Save Type" = "Save to EEPROM"

## Technical Details

### Hardware
- **MCU**: ESP32 (240MHz, WiFi, 320KB RAM)
- **Radio**: E220 LoRa module (400-510 MHz)
- **Storage**: LittleFS (SPIFFS) for web files
- **Communication**: UART2 for E220, USB for serial monitor

### Software Stack
- **Firmware**: Arduino/ESP32 Core
- **Web**: HTML5, CSS3, JavaScript (Fetch API)
- **Server**: ESPAsyncWebServer
- **Compression**: GZIP (web assets)
- **Build**: PlatformIO

### File System
- `src/main.cpp`: Firmware (369 lines)
- `data/index.html`: Web UI (777 lines, compressed to ~6.7KB)
- `platformio.ini`: Build configuration

### Performance
- Flash Usage: ~84% (1.1MB / 1.3MB)
- RAM Usage: ~16% (51KB / 327KB)
- Message History: 100 messages max
- Web Load Time: <1 second

## Troubleshooting

### Can't Connect to WiFi
- Check WiFi SSID is "E220-Chat"
- Password is "password123"
- Ensure ESP32 antenna is secure
- Try restarting ESP32

### Messages Not Syncing
- Check E220 module has power
- Verify M0/M1 pins are correctly wired
- Check serial connection with `cat /dev/ttyUSB0`

### Config Not Saving
- Click "⚡ Quick Save" (not just "Save Configuration")
- Watch serial output for `[E220] Config applied` confirmation
- Check E220 module responds to AT commands

### Web Page Not Loading
- Hard refresh browser: Ctrl+Shift+R (Windows/Linux) or Cmd+Shift+R (Mac)
- Clear browser cache
- Check ESP32 is in AP mode: `192.168.4.1` (not `192.168.1.1`)

## Development

### Build System
Uses PlatformIO with ESP32 Arduino framework.

```bash
# Build without upload
pio run

# Clean build
pio run --target clean

# Serial monitor
pio device monitor --baud 115200

# Full rebuild
pio run -t clean && pio run -t uploadfs && pio run -t upload
```

### Modify Web UI
Edit `data/index.html`, then:
```bash
rm data/index.html.gz    # Force rebuild of compressed version
pio run -t uploadfs
```

### Modify Firmware
Edit `src/main.cpp`, then:
```bash
pio run -t upload
```

## Environment Variables

None required. WiFi SSID/password hardcoded:
- SSID: `E220-Chat`
- Password: `password123`

To change, modify in `src/main.cpp` (line 62):
```cpp
WiFi.softAP("E220-Chat", "password123");
```

## Known Limitations

- Max 100 messages in history
- No encryption on WiFi (change password in code)
- Config changes from serial not yet supported (read-only)
- E220 must be in transparent mode
- Single client connection to web interface (not multiplayer)

## Future Enhancements

- [ ] Serial config command support
- [ ] WiFi WPA2 security
- [ ] MQTT integration
- [ ] Persistent config to SPIFFS
- [ ] Multiple simultaneous connections
- [ ] Message timestamps
- [ ] File transfer capability

## License

MIT License - See LICENSE file

## Support

For issues, questions, or contributions:
- GitHub Issues: https://github.com/dmahory/esp32-e220-web/issues
- Check troubleshooting section above first

## Credits

Built with:
- PlatformIO
- ESP32 Arduino Core
- ESPAsyncWebServer
- ArduinoJson
- Modern design principles (Impeccable)

---

**Last Updated**: March 2026  
**Status**: Production Ready
