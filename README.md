# ESP32 E220 LoRa Web Chat

A dual-device LoRa messaging system using ESP32 + Ebyte E220 900MHz LoRa modules with web UI and serial interface.

**Status:** Working — both devices can send/receive messages via LoRa and WiFi.

## Features

✅ **LoRa Messaging**
- Two devices communicate via Ebyte E220-900T22S LoRa modules
- Default: 930.125 MHz, 2.4 kbps air rate, 200-byte subpackets
- Configurable: frequency, air rate, TX power, addresses, encryption

✅ **Dual Interface**
- **Web Chat:** Browser interface at `http://192.168.4.1` on each device's WiFi AP
- **Serial Terminal:** USB serial at 115200 baud — just type to send, `/commands` for admin

✅ **Web Configuration Tab**
- Real-time E220 register adjustment (REG0-REG3, crypto keys)
- WOR cycle, LBT, RSSI settings
- Persistent Flash storage (survives power cycle)

✅ **Hardware**
- ESP32-DevKit (default, any ESP32 works)
- 2× Ebyte E220-900T22S 30dBm LoRa modules (or E220-900T30S for higher power)
- UART2 (GPIO16/17) wired to E220 TX/RX
- AUX pin (GPIO4) for status polling

## Hardware Wiring

| E220 Pin | ESP32 Pin | Purpose |
|----------|-----------|---------|
| RX | GPIO17 (RX2) | UART2 RX from module |
| TX | GPIO16 (TX2) | UART2 TX to module |
| M0 | GPIO2 | Mode control (low = normal TX/RX) |
| M1 | GPIO19 | Mode control (low = normal TX/RX) |
| AUX | GPIO4 | Status output (high = ready, low = busy) |
| VCC | 3.3V | Power (add 10µF cap nearby) |
| GND | GND | Ground |

**Important:** Add 10µF 16V electrolytic capacitor between VCC–GND near the E220 module. E220 draws ~500mA at TX peak, capacitor provides current spike buffering.

## Installation

### 1. Clone & Setup

```bash
git clone https://github.com/dmahony/esp32-e220-web.git
cd esp32-e220-web
pip install platformio
```

### 2. Build & Flash

```bash
# Flash filesystem (HTML/CSS/JS to LittleFS)
pio run -t uploadfs --upload-port /dev/ttyUSB0

# Flash firmware
pio run -t upload --upload-port /dev/ttyUSB0

# Repeat for second device on /dev/ttyUSB1
pio run -t uploadfs --upload-port /dev/ttyUSB1
pio run -t upload --upload-port /dev/ttyUSB1
```

### 3. Connect & Test

Each device creates a WiFi AP: `E220-Chat-XXX` (password: `password123`)

**Via Web:**
1. Connect phone/laptop to `E220-Chat-XXX`
2. Open browser → `http://192.168.4.1`
3. Type in chat → sends via LoRa to the other device

**Via Serial:**
```bash
screen /dev/ttyUSB0 115200
# Just type a message and press Enter to send
# Type /help for slash commands
```

## Configuration

### Web UI (Default Tabs)

1. **CHAT** — Send/receive messages, see debug console
2. **CONFIG** — Adjust E220 registers, restart ESP32
3. **WiFi** — View SSID/IP, connected devices

### E220 Registers (CONFIG Tab)

All settings map directly to E220 hardware registers per datasheet (Section 6):

| Setting | Register | Notes |
|---------|----------|-------|
| Frequency | REG2 | 850.125–930.125 MHz (900MHz band, 1 MHz steps) |
| TX Power | REG1[1:0] | 30/27/24/21 dBm (hardware-dependent) |
| Air Rate | REG0[2:0] | 2.4/4.8/9.6/19.2/38.4/62.5 kbps (0-7 code) |
| Baud Rate | REG0[7:5] | Serial speed (1200–115200) |
| Parity | REG0[4:3] | 8N1/8O1/8E1 |
| TX Mode | REG3[6] | Transparent (default) or Fixed-point addressing |
| Subpacket Size | REG1[7:6] | 200/128/64/32 bytes |
| WOR Cycle | REG3[2:0] | Wake-on-radio: 500–4000 ms |
| LBT | REG3[4] | Listen-before-talk collision avoidance |
| RSSI Settings | REG1[5], REG3[7] | Ambient noise reporting (disabled by default to avoid binary noise bytes) |
| Encryption | REG6-7 | 16-bit XOR key (both modules must match) |
| Save Type | — | RAM (temp) or Flash (permanent, survives reboot) |

**Default Config:**
- Frequency: 930.125 MHz
- Air Rate: 2.4 kbps (max range ~3 km line-of-sight)
- TX Power: 21 dBm
- Baud: 9600
- Both modules: address 0x0000, destination 0xFFFF (broadcast)
- Mode: Transparent (any module can TX to any module)

### Serial Commands

Type directly (no `send` prefix needed):

```
hello world              # Sends via LoRa
/config                  # Show current E220 settings
/read                    # Read module registers (debug)
/history                 # Show chat history
/clear                   # Clear history
/help                    # Show this help
```

## Architecture

### Firmware (`src/main.cpp`)

- **E220 Protocol:** Proper binary register read/write per datasheet (CMD+START+LEN+data+CRC16)
- **Web Server:** AsyncWebServer + LittleFS for static files
- **JSON Config:** ArduinoJson for `/api/config` endpoint
- **Serial I/O:** Dual UART handling (USB debug + E220 LoRa module)
- **String Pre-allocation:** Prevents heap fragmentation on large messages

### Web UI (`data/index.html`)

- Responsive design, blue accent, WCAG AA accessible
- Tabs: CHAT (debug console) | CONFIG (register settings) | WiFi (status)
- `/api/chat` polling for messages
- `/api/config` POST/GET for E220 settings
- `/api/reboot` to restart ESP32

## Known Limitations

1. **AP-only WiFi:** Each ESP32 creates its own hotspot (`E220-Chat-XXX`). Station mode (connecting to your router) not yet implemented.
2. **No encryption in flight:** LoRa TX is unencrypted (can add E220 XOR crypto in CONFIG tab, but both devices must match).
3. **Message length:** Limited by E220 subpacket size (default 200 bytes per packet, max ~240 bytes raw).
4. **RSSI:** Disabled by default to avoid binary noise bytes in text. Can enable per-message RSSI in CONFIG if needed (requires text filtering).
5. **No pairing:** Any two E220s on the same freq/air-rate will hear each other.

## Troubleshooting

**Devices not communicating?**
- Check both are on same **frequency** (930.125 MHz default)
- Check both are on same **air rate** (2.4 kbps default)
- Check E220 wiring: TX2 → RXD, RX2 → TXD, GND, AUX→GPIO4
- Check AUX pin is pulled high (E220 indicates ready to RX/TX)
- Enable `/read` command to see actual module registers

**Web page won't load?**
- Make sure you're connected to the ESP32's WiFi (`E220-Chat-XXX`)
- Try `http://192.168.4.1` (not `localhost`)
- Check serial monitor for boot errors
- Reset ESP32 via Restart button in CONFIG tab

**Serial garbage?**
- Make sure baud rate matches (115200 for ESP32→USB, E220 baud rate configurable)
- Check if RSSI bytes are enabled (causes `�bP` garbage in text)

## Development

### File Structure

```
esp32-e220-web/
├── src/
│   └── main.cpp                 # Firmware (AsyncWeb, E220 protocol, serial I/O)
├── data/
│   └── index.html               # Web UI (tabs, CSS, JS, API calls)
├── platformio.ini               # PlatformIO config
└── README.md                    # This file
```

### Building Locally

```bash
pio run                  # Compile only
pio run -t upload        # Flash to first connected ESP32
pio run -t uploadfs      # Flash filesystem (HTML/CSS)
pio run --environment release  # (if you add release config)
```

## References

- **E220 Datasheet:** Ebyte E220-900TxDS User Manual (Section 6: Register map)
- **ESP32 Docs:** https://docs.espressif.com/projects/esp-idf/
- **AsyncWebServer:** https://github.com/me-no-dev/ESPAsyncWebServer
- **ArduinoJson:** https://github.com/bblanchon/ArduinoJson

## Hardware Notes

**E220-900T22S vs E220-900T30S:**
- T22S: 22 dBm max (~7 km range)
- T30S: 30 dBm max (~10 km range)

Both are API-compatible; just adjust TX Power in CONFIG tab.

**Power Budget:**
- ESP32: ~80 mA average (WiFi AP active)
- E220: ~500 mA peak TX, ~10 mA RX idle
- Total: use 2A+ USB supply or 4S LiPo battery

**Antenna:**
- E220 ships with 5 dBi dipole antenna
- Upgrade to 9 dBi for longer range
- Match 50Ω impedance

## License

MIT

## Commits

- **0ec5d21** — Fix crash on large RX messages, pre-allocate strings, RSSI stripping (current)
- **dfbace5** — Fixed E220 register protocol per datasheet, added Restart button
- **bb3d678** — Serial type-to-send, slash commands

---

**Last updated:** 2026-03-23  
**Tested on:** 2× ESP32-DevKit + E220-900T22S @ 930.125 MHz, 2.4 kbps
