#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#define E220_RX_PIN   21
#define E220_TX_PIN   22
#define E220_M0_PIN   2
#define E220_M1_PIN   19
#define E220_AUX_PIN  4
#define UART_BAUD_CONFIG  9600    // E220 config mode always uses 9600
#define UART_BAUD_NORMAL  9600    // Normal mode baud rate (match config mode for simplicity)

AsyncWebServer server(80);
Preferences preferences;

struct {
  char ssid[64];
  char password[64];
  char ap_ssid[64];
  char ap_password[64];
} wifi_config = {"", "", "", "password123"};

HardwareSerial e220Serial(2);
String chatHistory[100];
int chatIndex = 0;

// Debug log ring buffer - captures Serial output for web debug tab
#define DEBUG_LOG_SIZE 4096
char debugLogBuf[DEBUG_LOG_SIZE];
int debugLogHead = 0;
int debugLogTail = 0;
int debugLogReadPos = 0;  // tracks what web client has already read

void debugLogWrite(const char *str) {
  while (*str) {
    debugLogBuf[debugLogHead] = *str++;
    debugLogHead = (debugLogHead + 1) % DEBUG_LOG_SIZE;
    if (debugLogHead == debugLogTail) {
      debugLogTail = (debugLogTail + 1) % DEBUG_LOG_SIZE;  // overwrite oldest
      if (debugLogReadPos == debugLogTail)
        debugLogReadPos = (debugLogReadPos + 1) % DEBUG_LOG_SIZE;
    }
  }
}

// Custom Print class that tees to Serial AND debug buffer
class DebugPrint : public Print {
public:
  size_t write(uint8_t c) override {
    char buf[2] = {(char)c, 0};
    debugLogWrite(buf);
    return Serial.write(c);
  }
  size_t write(const uint8_t *buffer, size_t size) override {
    for (size_t i = 0; i < size; i++) {
      char buf[2] = {(char)buffer[i], 0};
      debugLogWrite(buf);
    }
    return Serial.write(buffer, size);
  }
};

DebugPrint dbg;

// TX queue: messages queued from web handler, sent from loop()
// This avoids blocking the async_tcp task and triggering the watchdog
String txQueue = "";
bool txPending = false;

// E220 Config - matches actual E220 registers (00h-07h)
// See E220-xxxTxxx_UserManual_EN.pdf Section 6.2-6.3
struct {
  float freq;        // Derived from REG2 channel: 850.125 + CH (900MHz band)
  int txpower;       // REG1[1:0]: 30=0x00, 27=0x01, 24=0x02, 21=0x03 (for 30dBm models)
  int baud;          // REG0[7:5]: 1200-115200
  char addr[8];      // ADDH(00h) + ADDL(01h): module address "0xHHLL"
  char dest[8];      // Destination for fixed-point TX (not a register, used in TX prefix)
  int airrate;       // REG0[2:0]: 0-2=2.4k, 3=4.8k, 4=9.6k, 5=19.2k, 6=38.4k, 7=62.5k
  int subpkt;        // REG1[7:6]: 0=200B, 1=128B, 2=64B, 3=32B
  int parity;        // REG0[4:3]: 0=8N1, 1=8O1, 2=8E1
  int txmode;        // REG3[6]: 0=transparent, 1=fixed-point
  int rssi_noise;    // REG1[5]: 0=disabled, 1=enable ambient noise RSSI
  int rssi_byte;     // REG3[7]: 0=disabled, 1=append RSSI byte to RX data
  int lbt;           // REG3[4]: 0=disabled, 1=listen-before-talk
  int wor_cycle;     // REG3[2:0]: period = (1+WOR)*500ms, 0=500ms..7=4000ms
  int crypt_h;       // REG 06h: encryption key high byte (write-only)
  int crypt_l;       // REG 07h: encryption key low byte (write-only)
  int savetype;      // 0=C2 (RAM only), 1=C0 (save to flash)
} e220_config = {930.125, 21, 9600, "0x0000", "0xFFFF", 2, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0};

void setE220Mode(uint8_t mode) {
  if (mode == 1) {
    digitalWrite(E220_M0_PIN, HIGH);
    digitalWrite(E220_M1_PIN, LOW);
  } else {
    digitalWrite(E220_M0_PIN, LOW);
    digitalWrite(E220_M1_PIN, LOW);
  }
  delay(50);
}

// AUX Pin Monitoring (GPIO 4)
// HIGH = Idle/Ready, LOW = Busy (TX/RX/Processing)
// Per E220 Manual Section 5.2: typical delay 3ms when idle
bool isE220Ready() {
  return digitalRead(E220_AUX_PIN) == HIGH;
}

// Wait for E220 to become ready with timeout protection
// timeout_ms: max milliseconds to wait (default 5000ms = 5 seconds)
// returns: true if ready, false if timeout
bool waitE220Ready(uint32_t timeout_ms = 5000) {
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    if (isE220Ready()) {
      return true;
    }
    delay(10);  // Poll every 10ms to avoid hogging CPU
  }
  dbg.printf("[E220] Timeout waiting for AUX ready after %u ms\n", timeout_ms);
  return false;
}

// Wait for E220 to become BUSY (AUX goes LOW)
// Used to detect when module has accepted data for transmission
// timeout_ms: max milliseconds to wait (default 1000ms)
// returns: true if busy detected, false if timeout
bool waitE220Busy(uint32_t timeout_ms = 1000) {
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    if (!isE220Ready()) {
      return true;
    }
    delay(5);
  }
  dbg.printf("[E220] Timeout waiting for AUX busy after %u ms\n", timeout_ms);
  return false;
}

// Change UART baud rate (switches ESP32 serial port only, doesn't change E220 module)
void setE220UARTBaud(int baud) {
  dbg.printf("[E220] Changing UART baud from %d to %d\n", UART_BAUD_CONFIG, baud);
  e220Serial.end();
  delay(50);
  e220Serial.begin(baud, SERIAL_8N1, E220_RX_PIN, E220_TX_PIN);
  delay(50);
}

uint8_t baudToReg(int baud) {
  switch(baud) {
    case 1200:   return 0;
    case 2400:   return 1;
    case 4800:   return 2;
    case 9600:   return 3;
    case 19200:  return 4;
    case 38400:  return 5;
    case 57600:  return 6;
    case 115200: return 7;
    default:     return 3; // 9600
  }
}

uint8_t txpowerToReg(int dbm) {
  switch(dbm) {
    case 30: return 0;
    case 27: return 1;
    case 24: return 2;
    case 21: return 3;
    default: return 3; // 21 dBm
  }
}

void readE220Config() {
  // Ensure serial is at 9600 for config mode (manual: config mode ONLY supports 9600 8N1)
  e220Serial.end();
  delay(50);
  e220Serial.begin(9600, SERIAL_8N1, E220_RX_PIN, E220_TX_PIN);
  delay(50);
  
  // Enter CONFIG mode (M0=1, M1=1) per manual Section 5.1.3
  digitalWrite(E220_M0_PIN, HIGH);
  digitalWrite(E220_M1_PIN, HIGH);
  
  // Per manual Section 5.2.4: mode switch takes 9-11ms, AUX goes LOW during switch
  // Wait for AUX to go LOW first (indicates switch has started)
  delay(15);  // Ensure mode switch has begun
  
  // Now wait for AUX to go HIGH (module ready for config commands)
  if (!waitE220Ready(2000)) {
    dbg.println("[E220] Config mode failed - AUX timeout");
    setE220Mode(0);
    return;
  }
  
  // Extra settling delay after AUX goes HIGH
  delay(50);
  
  // Flush any stale data
  while(e220Serial.available()) e220Serial.read();
  
  // Read registers 0x00-0x07: CMD(0xC1) + START(0x00) + LEN(0x08)
  // Manual Section 6.1: Read command = C1 + start_addr + length
  // Response = C1 + start_addr + length + data bytes
  uint8_t readCmd[3] = {0xC1, 0x00, 0x06};
  dbg.printf("[E220] Sending read cmd: %02X %02X %02X\n", readCmd[0], readCmd[1], readCmd[2]);
  e220Serial.write(readCmd, 3);
  e220Serial.flush();  // Wait for TX to complete
  
  // Wait for response: 3 header bytes + 6 data bytes = 9 bytes total
  uint32_t timeout = millis() + 1000;
  while (e220Serial.available() < 9 && millis() < timeout) {
    delay(10);
  }
  
  int avail = e220Serial.available();
  dbg.printf("[E220] Got %d bytes in response\n", avail);
  
  // Response: 0xC1 + START + LEN + ADDH + ADDL + REG0 + REG1 + REG2 + REG3
  if (avail >= 9) {
    uint8_t hdr = e220Serial.read(); // 0xC1
    uint8_t start = e220Serial.read(); // 0x00
    uint8_t len = e220Serial.read(); // 0x06
    uint8_t addh = e220Serial.read();
    uint8_t addl = e220Serial.read();
    uint8_t reg0 = e220Serial.read();
    uint8_t reg1 = e220Serial.read();
    uint8_t reg2 = e220Serial.read(); // channel
    uint8_t reg3 = e220Serial.read();
    
    // Update e220_config struct from register values so web UI stays in sync
    snprintf(e220_config.addr, sizeof(e220_config.addr), "0x%02X%02X", addh, addl);
    e220_config.freq = 850.125 + reg2;
    e220_config.airrate = reg0 & 0x07;
    e220_config.parity = (reg0 >> 3) & 0x03;
    
    // Reverse baud from register bits
    static const int baudTable[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    e220_config.baud = baudTable[(reg0 >> 5) & 0x07];
    
    e220_config.subpkt = (reg1 >> 6) & 0x03;
    e220_config.rssi_noise = (reg1 >> 5) & 0x01;
    // Reverse TX power from register bits
    static const int powerTable30[] = {30, 27, 24, 21};
    e220_config.txpower = powerTable30[reg1 & 0x03];
    
    e220_config.rssi_byte = (reg3 >> 7) & 0x01;
    e220_config.txmode = (reg3 >> 6) & 0x01;
    e220_config.lbt = (reg3 >> 4) & 0x01;
    e220_config.wor_cycle = reg3 & 0x07;
    
    dbg.println("[E220] Read config from module:");
    dbg.printf("  HDR=0x%02X START=0x%02X LEN=0x%02X\n", hdr, start, len);
    dbg.printf("  ADDH=0x%02X ADDL=0x%02X\n", addh, addl);
    dbg.printf("  REG0=0x%02X REG1=0x%02X REG2=0x%02X REG3=0x%02X\n", reg0, reg1, reg2, reg3);
    dbg.printf("  Channel=%d -> Freq=%.3f MHz\n", reg2, e220_config.freq);
    dbg.printf("  TX Power=%d dBm\n", e220_config.txpower);
    dbg.printf("  Air Rate=%d (bits=%d)\n", e220_config.airrate, reg0 & 0x07);
    dbg.printf("  Baud=%d\n", e220_config.baud);
    dbg.printf("  TX Mode=%s\n", e220_config.txmode ? "Fixed" : "Transparent");
  } else {
    dbg.printf("[E220] Read failed, got %d bytes\n", avail);
    while(e220Serial.available()) {
      dbg.printf("  byte: 0x%02X\n", e220Serial.read());
    }
    dbg.println("[E220] Check: M0->GPIO2, M1->GPIO19, AUX->GPIO4, TX->GPIO22, RX->GPIO21");
    dbg.printf("[E220] AUX pin state: %s\n", digitalRead(E220_AUX_PIN) ? "HIGH" : "LOW");
  }
  
  // Return to NORMAL mode
  setE220Mode(0);
  delay(50);
}

void applyE220Config() {
  // Ensure serial is at 9600 for config mode (manual: config mode ONLY supports 9600 8N1)
  e220Serial.end();
  delay(50);
  e220Serial.begin(9600, SERIAL_8N1, E220_RX_PIN, E220_TX_PIN);
  delay(50);
  
  // Enter CONFIG mode (M0=1, M1=1) - serial port MUST be 9600 8N1 in this mode
  digitalWrite(E220_M0_PIN, HIGH);
  digitalWrite(E220_M1_PIN, HIGH);
  
  // Per manual Section 5.2.4: mode switch takes 9-11ms
  delay(15);
  
  // Wait for AUX to go HIGH per manual Section 5.2
  if (!waitE220Ready(2000)) {
    dbg.println("[E220] Config mode failed - AUX timeout");
    setE220Mode(0);  // Return to normal mode
    return;
  }
  
  // Extra settling delay
  delay(50);
  
  // Flush any stale data
  while(e220Serial.available()) e220Serial.read();
  
  // Parse address from hex string "0xHHLL" -> ADDH, ADDL
  uint16_t addr = (uint16_t)strtol(e220_config.addr, NULL, 16);
  uint8_t addh = (addr >> 8) & 0xFF;
  uint8_t addl = addr & 0xFF;
  
  // REG0 (02h): [7:5] UART baud, [4:3] parity, [2:0] air data rate
  uint8_t reg0 = (baudToReg(e220_config.baud) << 5) | 
                 ((e220_config.parity & 0x03) << 3) | 
                 (e220_config.airrate & 0x07);
  
  // REG1 (03h): [7:6] subpacket, [5] RSSI ambient noise, [4:3] reserved, [2] soft switch, [1:0] TX power
  uint8_t reg1 = ((e220_config.subpkt & 0x03) << 6) | 
                 ((e220_config.rssi_noise & 0x01) << 5) |
                 (txpowerToReg(e220_config.txpower) & 0x03);
  
  // REG2 (04h): Channel number
  // E220-900T30D: freq = 850.125 + CH (MHz), CH = 0-80
  uint8_t reg2 = (uint8_t)(e220_config.freq - 850.125);
  if (reg2 > 80) reg2 = 80;
  
  // REG3 (05h): [7] RSSI byte, [6] TX method, [5] reserved, [4] LBT, [3] reserved, [2:0] WOR cycle
  uint8_t reg3 = ((e220_config.rssi_byte & 0x01) << 7) |
                 ((e220_config.txmode & 0x01) << 6) |
                 ((e220_config.lbt & 0x01) << 4) |
                 (e220_config.wor_cycle & 0x07);
  
  // CRYPT (06h-07h): Encryption key
  uint8_t crypt_h = (uint8_t)(e220_config.crypt_h & 0xFF);
  uint8_t crypt_l = (uint8_t)(e220_config.crypt_l & 0xFF);
  
  // Command: 0xC0 (save to flash) or 0xC2 (temp/RAM only)
  uint8_t cmd = (e220_config.savetype == 1) ? 0xC0 : 0xC2;
  
  // Write all 8 registers: CMD + START(0x00) + LEN(0x08) + ADDH + ADDL + REG0-REG3 + CRYPT_H + CRYPT_L
  uint8_t packet[11] = {cmd, 0x00, 0x08, addh, addl, reg0, reg1, reg2, reg3, crypt_h, crypt_l};
  
  dbg.println("[E220] Writing config:");
  dbg.printf("  CMD=0x%02X (%s)\n", cmd, cmd == 0xC0 ? "SAVE TO FLASH" : "RAM ONLY");
  dbg.printf("  ADDH=0x%02X ADDL=0x%02X (addr=%s)\n", addh, addl, e220_config.addr);
  dbg.printf("  REG0=0x%02X: baud=%d parity=%d airrate=%d\n", reg0, e220_config.baud, e220_config.parity, e220_config.airrate);
  dbg.printf("  REG1=0x%02X: subpkt=%d rssi_noise=%d txpower=%d dBm\n", reg1, e220_config.subpkt, e220_config.rssi_noise, e220_config.txpower);
  dbg.printf("  REG2=0x%02X: channel=%d freq=%.3f MHz\n", reg2, reg2, 850.125 + reg2);
  dbg.printf("  REG3=0x%02X: rssi_byte=%d txmode=%s lbt=%d wor=%d\n", reg3, e220_config.rssi_byte, e220_config.txmode ? "FIXED" : "TRANSPARENT", e220_config.lbt, e220_config.wor_cycle);
  dbg.printf("  CRYPT=0x%02X%02X\n", crypt_h, crypt_l);
  
  dbg.printf("[E220] Sending %d bytes: ", 11);
  for (int i = 0; i < 11; i++) dbg.printf("%02X ", packet[i]);
  dbg.println();
  
  e220Serial.write(packet, 11);
  e220Serial.flush();
  
  // Wait for AUX to indicate processing, then ready
  delay(100);
  waitE220Ready(2000);
  delay(200);
  
  // Read response: E220 echoes C1 + start + len + data
  uint32_t respTimeout = millis() + 1000;
  while (e220Serial.available() < 3 && millis() < respTimeout) {
    delay(10);
  }
  
  int avail = e220Serial.available();
  if (avail > 0) {
    dbg.printf("[E220] Response (%d bytes):", avail);
    uint8_t first = 0;
    while(e220Serial.available()) {
      uint8_t b = e220Serial.read();
      if (!first) first = b;
      dbg.printf(" 0x%02X", b);
    }
    dbg.println();
    if (first == 0xC1) {
      dbg.println("[E220] Config write SUCCESS (C1 acknowledged)");
    } else if (first == 0xFF) {
      dbg.println("[E220] Config write FAILED (FF FF FF = format error!)");
    }
  } else {
    dbg.println("[E220] WARNING: No response from module!");
  }
  
  delay(100);
  
  // Return to NORMAL mode (M0=0, M1=0)
  // Per manual Section 5.2.4 note 3: switching FROM config mode causes the module
  // to reset user parameters. AUX goes LOW during this reset. MUST wait for it.
  dbg.println("[E220] Switching back to normal mode (module will reset params)...");
  digitalWrite(E220_M0_PIN, LOW);
  digitalWrite(E220_M1_PIN, LOW);
  
  // Wait for mode switch to begin (9-11ms per manual)
  delay(50);
  
  // Wait for AUX HIGH - module has finished resetting with new params
  if (!waitE220Ready(5000)) {
    dbg.println("[E220] WARNING: Module not ready after config apply! May need power cycle.");
  }
  
  // Extra settling time after param reset
  delay(200);
  
  dbg.println("[E220] Config applied, back to normal mode");
  
  // Read back to verify
  delay(200);
  readE220Config();
}

void setupE220() {
  pinMode(E220_M0_PIN, OUTPUT);
  pinMode(E220_M1_PIN, OUTPUT);
  pinMode(E220_AUX_PIN, INPUT);  // AUX is a status input (HIGH=ready, LOW=busy)
  // Start with config baud (9600) - will switch to normal baud after config
  e220Serial.begin(UART_BAUD_CONFIG, SERIAL_8N1, E220_RX_PIN, E220_TX_PIN);
  setE220Mode(0);
  delay(100);  // Wait for module startup (T1 = ~16ms per manual)
  if (waitE220Ready(1000)) {
    dbg.println("[E220] Init - AUX ready");
  } else {
    dbg.println("[E220] Init - WARNING: AUX not ready (module may not be responding)");
  }
}

void setupWiFi() {
  preferences.begin("wifi", false);

  // Load saved STA credentials (check if key exists first to avoid NVS errors)
  String savedSSID = preferences.isKey("sta_ssid") ? preferences.getString("sta_ssid", "") : "";
  String savedPass = preferences.isKey("sta_pass") ? preferences.getString("sta_pass", "") : "";
  strlcpy(wifi_config.ssid, savedSSID.c_str(), sizeof(wifi_config.ssid));
  strlcpy(wifi_config.password, savedPass.c_str(), sizeof(wifi_config.password));

  // Load saved AP settings with defaults
  // Generate random AP name only once (first boot), then persist it
  String apSSID, apPass;
  if (preferences.isKey("ap_ssid")) {
    apSSID = preferences.getString("ap_ssid");
    apPass = preferences.getString("ap_pass", "password123");
  } else {
    // First boot: generate random AP name and save it
    int randomNum = random(100, 1000);
    apSSID = "E220-Chat-" + String(randomNum);
    apPass = "password123";
    preferences.putString("ap_ssid", apSSID);
    preferences.putString("ap_pass", apPass);
  }
  strlcpy(wifi_config.ap_ssid, apSSID.c_str(), sizeof(wifi_config.ap_ssid));
  strlcpy(wifi_config.ap_password, apPass.c_str(), sizeof(wifi_config.ap_password));

  // Start AP+STA mode
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(wifi_config.ap_ssid, wifi_config.ap_password);
  dbg.print("[WiFi] AP SSID: ");
  dbg.println(wifi_config.ap_ssid);
  dbg.print("[WiFi] AP IP: ");
  dbg.println(WiFi.softAPIP());

  // If saved STA credentials exist, attempt connection
  if (strlen(wifi_config.ssid) > 0) {
    dbg.printf("[WiFi] Connecting to '%s'...\n", wifi_config.ssid);
    WiFi.begin(wifi_config.ssid, wifi_config.password);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(250);
      dbg.print(".");
    }
    dbg.println();
    if (WiFi.status() == WL_CONNECTED) {
      dbg.print("[WiFi] STA connected, IP: ");
      dbg.println(WiFi.localIP());
    } else {
      dbg.println("[WiFi] STA connection failed, AP-only mode");
      WiFi.mode(WIFI_AP);
      WiFi.softAP(wifi_config.ap_ssid, wifi_config.ap_password);
    }
  } else {
    dbg.println("[WiFi] No saved STA credentials, AP-only mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(wifi_config.ap_ssid, wifi_config.ap_password);
  }
}

void setupFS() {
  if (!LittleFS.begin(true)) {
    dbg.println("[FS] Mount failed");
    return;
  }
  dbg.println("[FS] Ready");
  
  // Debug: list files
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while(file) {
    dbg.print("[FS] Found: ");
    dbg.println(file.name());
    file = root.openNextFile();
  }
}

void setupWebRoutes() {
  // Serve index.html (with gzip support)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    dbg.println("[Web] Request for /");
    
    // Check if client accepts gzip
    bool acceptGzip = false;
    if (request->hasHeader("Accept-Encoding")) {
      String encoding = request->header("Accept-Encoding");
      if (encoding.indexOf("gzip") != -1) {
        acceptGzip = true;
      }
    }
    
    // Try to serve compressed version first
    if (acceptGzip && LittleFS.exists("/index.html.gz")) {
      dbg.println("[Web] Serving compressed HTML");
      AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/index.html.gz", "text/html");
      response->addHeader("Content-Encoding", "gzip");
      response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
      response->addHeader("Pragma", "no-cache");
      response->addHeader("Expires", "0");
      request->send(response);
    } else if (LittleFS.exists("/index.html")) {
      dbg.println("[Web] Serving uncompressed HTML");
      AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/index.html", "text/html");
      response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
      response->addHeader("Pragma", "no-cache");
      response->addHeader("Expires", "0");
      request->send(response);
    } else {
      request->send(404, "text/plain", "index.html not found");
      dbg.println("[Web] HTML not found!");
    }
  });

  // Chat history API - uses ArduinoJson for proper escaping
  server.on("/api/chat", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(8192);
    JsonArray history = doc.createNestedArray("history");
    for (int i = 0; i < chatIndex; i++) {
      history.add(chatHistory[i]);
    }
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // Send message API
  // Body is reassembled then queued for transmission in loop() to avoid
  // blocking the async_tcp task (which triggers watchdog on large messages)
  static String sendBodyBuffer;
  server.on("/api/send", HTTP_POST,
  [](AsyncWebServerRequest *request) {
    if (sendBodyBuffer.length() == 0) {
      request->send(400, "application/json", "{\"error\":\"empty body\"}");
      return;
    }
    
    DynamicJsonDocument doc(sendBodyBuffer.length() + 128);
    DeserializationError error = deserializeJson(doc, sendBodyBuffer);
    sendBodyBuffer = "";
    
    if (error || !doc.containsKey("message")) {
      request->send(400, "application/json", "{\"error\":\"no message\"}");
      return;
    }
    
    String msg = doc["message"].as<String>();
    
    if (txPending) {
      request->send(429, "application/json", "{\"error\":\"TX busy, wait for previous message\"}");
      return;
    }
    
    // Queue for loop() - no blocking here
    txQueue = msg;
    txPending = true;
    
    if (chatIndex < 100) {
      chatHistory[chatIndex] = "[TX] " + msg;
      chatIndex++;
    }
    
    request->send(200, "application/json", "{\"status\":\"ok\"}");
    dbg.printf("[TX] Queued (%d bytes)\n", msg.length());
  }, NULL,
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (index == 0) {
      sendBodyBuffer = "";
      sendBodyBuffer.reserve(total);
    }
    sendBodyBuffer += String((char*)data, len);
  });

  // Get config API
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(512);
    JsonObject config = doc.createNestedObject("config");
    config["freq"] = e220_config.freq;
    config["txpower"] = e220_config.txpower;
    config["baud"] = e220_config.baud;
    config["addr"] = e220_config.addr;
    config["dest"] = e220_config.dest;
    config["airrate"] = e220_config.airrate;
    config["subpkt"] = e220_config.subpkt;
    config["parity"] = e220_config.parity;
    config["txmode"] = e220_config.txmode;
    config["rssi_noise"] = e220_config.rssi_noise;
    config["rssi_byte"] = e220_config.rssi_byte;
    config["lbt"] = e220_config.lbt;
    config["wor_cycle"] = e220_config.wor_cycle;
    config["crypt_h"] = e220_config.crypt_h;
    config["crypt_l"] = e220_config.crypt_l;
    config["savetype"] = e220_config.savetype;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // Save config API
  server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, (const char*)data, len);
    
    if (error) {
      request->send(400, "application/json", "{\"error\":\"JSON parse error\"}");
      return;
    }
    
    // Update config - matches actual E220 registers
    if (doc.containsKey("freq")) e220_config.freq = doc["freq"];
    if (doc.containsKey("txpower")) e220_config.txpower = doc["txpower"];
    if (doc.containsKey("baud")) e220_config.baud = doc["baud"];
    if (doc.containsKey("addr")) strlcpy(e220_config.addr, doc["addr"], sizeof(e220_config.addr));
    if (doc.containsKey("dest")) strlcpy(e220_config.dest, doc["dest"], sizeof(e220_config.dest));
    if (doc.containsKey("airrate")) e220_config.airrate = doc["airrate"];
    if (doc.containsKey("subpkt")) e220_config.subpkt = doc["subpkt"];
    if (doc.containsKey("parity")) e220_config.parity = doc["parity"];
    if (doc.containsKey("txmode")) e220_config.txmode = doc["txmode"];
    if (doc.containsKey("rssi_noise")) e220_config.rssi_noise = doc["rssi_noise"];
    if (doc.containsKey("rssi_byte")) e220_config.rssi_byte = doc["rssi_byte"];
    if (doc.containsKey("lbt")) e220_config.lbt = doc["lbt"];
    if (doc.containsKey("wor_cycle")) e220_config.wor_cycle = doc["wor_cycle"];
    if (doc.containsKey("crypt_h")) e220_config.crypt_h = doc["crypt_h"];
    if (doc.containsKey("crypt_l")) e220_config.crypt_l = doc["crypt_l"];
    if (doc.containsKey("savetype")) e220_config.savetype = doc["savetype"];
    
    dbg.println("[CONFIG] Updated parameters:");
    dbg.printf("  freq=%.3f txpower=%d baud=%d\n", e220_config.freq, e220_config.txpower, e220_config.baud);
    dbg.printf("  addr=%s dest=%s\n", e220_config.addr, e220_config.dest);
    dbg.printf("  airrate=%d subpkt=%d parity=%d txmode=%d\n", e220_config.airrate, e220_config.subpkt, e220_config.parity, e220_config.txmode);
    dbg.printf("  rssi_noise=%d rssi_byte=%d lbt=%d wor=%d\n", e220_config.rssi_noise, e220_config.rssi_byte, e220_config.lbt, e220_config.wor_cycle);
    dbg.printf("  crypt=0x%02X%02X savetype=%d\n", e220_config.crypt_h, e220_config.crypt_l, e220_config.savetype);
    
    // Always apply config to the E220 module
    applyE220Config();
    
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // Debug log API - returns new serial output since last poll
  server.on("/api/debug", HTTP_GET, [](AsyncWebServerRequest *request) {
    String out;
    out.reserve(512);
    while (debugLogReadPos != debugLogHead) {
      char c = debugLogBuf[debugLogReadPos];
      out += c;
      debugLogReadPos = (debugLogReadPos + 1) % DEBUG_LOG_SIZE;
      if (out.length() > 2048) break;  // cap per response
    }
    request->send(200, "text/plain", out);
  });

  // Debug log clear
  server.on("/api/debug/clear", HTTP_POST, [](AsyncWebServerRequest *request) {
    debugLogReadPos = debugLogHead;
    request->send(200, "text/plain", "ok");
  });

  // Reboot API
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"status\":\"rebooting\"}");
    dbg.println("[SYS] Reboot requested via web");
    delay(500);
    ESP.restart();
  });

  // WiFi status API
  server.on("/api/wifi/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(512);
    wifi_mode_t mode = WiFi.getMode();
    doc["mode"] = (mode == WIFI_AP) ? "AP" : (mode == WIFI_STA) ? "STA" : "AP_STA";
    doc["ap_ssid"] = String(wifi_config.ap_ssid);
    doc["ap_ip"] = WiFi.softAPIP().toString();
    doc["sta_connected"] = (WiFi.status() == WL_CONNECTED);
    if (WiFi.status() == WL_CONNECTED) {
      doc["sta_ssid"] = WiFi.SSID();
      doc["sta_ip"] = WiFi.localIP().toString();
      doc["sta_rssi"] = WiFi.RSSI();
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // WiFi scan API
  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
    int n = WiFi.scanNetworks();
    DynamicJsonDocument doc(2048);
    JsonArray networks = doc.createNestedArray("networks");
    for (int i = 0; i < n && i < 20; i++) {
      JsonObject net = networks.createNestedObject();
      net["ssid"] = WiFi.SSID(i);
      net["rssi"] = WiFi.RSSI(i);
      net["encryption"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "Open" : "Encrypted";
      net["channel"] = WiFi.channel(i);
    }
    WiFi.scanDelete();
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // WiFi connect API
  server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, (const char*)data, len)) {
      request->send(400, "application/json", "{\"error\":\"JSON parse error\"}");
      return;
    }
    const char* ssid = doc["ssid"] | "";
    const char* pass = doc["password"] | "";
    if (strlen(ssid) == 0) {
      request->send(400, "application/json", "{\"error\":\"SSID required\"}");
      return;
    }
    // Save credentials
    preferences.putString("sta_ssid", ssid);
    preferences.putString("sta_pass", pass);
    strlcpy(wifi_config.ssid, ssid, sizeof(wifi_config.ssid));
    strlcpy(wifi_config.password, pass, sizeof(wifi_config.password));

    // Attempt connection
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(wifi_config.ap_ssid, wifi_config.ap_password);
    WiFi.begin(ssid, pass);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
      dbg.printf("[WiFi] Connected to %s, IP: %s\n", ssid, WiFi.localIP().toString().c_str());
      request->send(200, "application/json", "{\"status\":\"connected\",\"ip\":\"" + WiFi.localIP().toString() + "\"}");
    } else {
      dbg.printf("[WiFi] Failed to connect to %s\n", ssid);
      request->send(200, "application/json", "{\"status\":\"failed\"}");
    }
  });

  // WiFi disconnect API
  server.on("/api/wifi/disconnect", HTTP_POST, [](AsyncWebServerRequest *request) {
    WiFi.disconnect(true);
    preferences.remove("sta_ssid");
    preferences.remove("sta_pass");
    wifi_config.ssid[0] = '\0';
    wifi_config.password[0] = '\0';
    WiFi.mode(WIFI_AP);
    WiFi.softAP(wifi_config.ap_ssid, wifi_config.ap_password);
    dbg.println("[WiFi] Disconnected STA, cleared credentials");
    request->send(200, "application/json", "{\"status\":\"disconnected\"}");
  });

  // WiFi AP settings API
  server.on("/api/wifi/ap", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, (const char*)data, len)) {
      request->send(400, "application/json", "{\"error\":\"JSON parse error\"}");
      return;
    }
    const char* ssid = doc["ssid"] | "";
    const char* pass = doc["password"] | "";
    if (strlen(ssid) > 0) {
      preferences.putString("ap_ssid", ssid);
      strlcpy(wifi_config.ap_ssid, ssid, sizeof(wifi_config.ap_ssid));
    }
    if (strlen(pass) >= 8) {
      preferences.putString("ap_pass", pass);
      strlcpy(wifi_config.ap_password, pass, sizeof(wifi_config.ap_password));
    }
    dbg.printf("[WiFi] AP settings saved: SSID=%s (reboot required)\n", wifi_config.ap_ssid);
    request->send(200, "application/json", "{\"status\":\"saved\",\"note\":\"Reboot to apply\"}");
  });

  server.begin();
  dbg.println("[Web] Server on 192.168.4.1");
}

// RX buffer for reassembling large incoming messages
static uint8_t rxBuf[256];
static int rxLen = 0;
static unsigned long lastRxTime = 0;
// If no new data for this many ms, flush whatever we have as a complete message
#define RX_FLUSH_TIMEOUT 2000
static int lastRssi = 0;  // last RSSI value in dBm

// Process a complete received packet (strip RSSI byte if enabled)
void processRxPacket() {
  if (rxLen == 0) return;
  
  int msgLen = rxLen;
  int rssiRaw = -1;
  
  // If RSSI byte is enabled, last byte is RSSI value
  if (e220_config.rssi_byte && msgLen > 1) {
    rssiRaw = rxBuf[msgLen - 1];
    msgLen--;  // strip RSSI byte from message
    lastRssi = -(256 - rssiRaw);  // convert to dBm (E220: RSSI = -(256-val) dBm)
    dbg.printf("[RSSI] raw=0x%02X -> %d dBm\n", rssiRaw, lastRssi);
  } else if (e220_config.rssi_byte && msgLen == 1) {
    // Single byte with RSSI enabled = just RSSI, no message content
    rssiRaw = rxBuf[0];
    lastRssi = -(256 - rssiRaw);
    dbg.printf("[RSSI] raw=0x%02X -> %d dBm (standalone)\n", rssiRaw, lastRssi);
    rxLen = 0;
    return;
  }
  
  // Build message string, keeping all bytes except control chars (0x00-0x1F except tab)
  // This preserves UTF-8 multi-byte sequences (high bytes 0x80-0xFF)
  String msg = "";
  for (int i = 0; i < msgLen; i++) {
    uint8_t b = rxBuf[i];
    if (b >= 0x20 || b == '\t') {  // printable ASCII, UTF-8 high bytes (0x80+), and tab
      msg += (char)b;
    }
  }
  msg.trim();
  
  if (msg.length() > 0 && chatIndex < 100) {
    String display = "[RX] " + msg;
    if (e220_config.rssi_byte && rssiRaw >= 0) {
      display += " [RSSI:" + String(lastRssi) + "dBm]";
    }
    chatHistory[chatIndex] = display;
    chatIndex++;
    dbg.printf("[RX] (%d bytes) %s", msg.length(), msg.c_str());
    if (rssiRaw >= 0) {
      dbg.printf(" [RSSI:%d dBm]", lastRssi);
    }
    dbg.println();
  }
  
  rxLen = 0;
}

void handleE220Serial() {
  while (e220Serial.available()) {
    uint8_t b = e220Serial.read();
    lastRxTime = millis();
    
    if ((char)b == '\n') {
      // Newline = end of message
      processRxPacket();
    } else {
      if (rxLen < (int)sizeof(rxBuf) - 1) {
        rxBuf[rxLen++] = b;
      }
    }
  }
  
  // Flush partial buffer after timeout (in case sender didn't send newline)
  if (rxLen > 0 && (millis() - lastRxTime) > RX_FLUSH_TIMEOUT) {
    processRxPacket();
  }
}

void handleUSBSerial() {
  while (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    
    if (input.length() == 0) return;
    
    // Slash commands for config/admin, everything else is a message
    if (input == "/config") {
      dbg.println("[CONFIG] Current settings:");
      dbg.printf("  freq=%.3f MHz (CH=%d)\n", e220_config.freq, (int)(e220_config.freq - 850.125));
      dbg.printf("  txpower=%d dBm\n", e220_config.txpower);
      dbg.printf("  baud=%d\n", e220_config.baud);
      dbg.printf("  addr=%s  dest=%s\n", e220_config.addr, e220_config.dest);
      dbg.printf("  airrate=%d  subpkt=%d  parity=%d\n", e220_config.airrate, e220_config.subpkt, e220_config.parity);
      dbg.printf("  txmode=%s\n", e220_config.txmode ? "FIXED" : "TRANSPARENT");
      dbg.printf("  rssi_noise=%d  rssi_byte=%d\n", e220_config.rssi_noise, e220_config.rssi_byte);
      dbg.printf("  lbt=%d  wor_cycle=%d\n", e220_config.lbt, e220_config.wor_cycle);
      dbg.printf("  crypt=0x%02X%02X  savetype=%d\n", e220_config.crypt_h, e220_config.crypt_l, e220_config.savetype);
    }
    else if (input == "/read") {
      dbg.println("[E220] Reading module registers...");
      readE220Config();
    }
    else if (input == "/history") {
      dbg.printf("[HISTORY] %d messages:\n", chatIndex);
      for (int i = 0; i < chatIndex; i++) {
        dbg.printf("  %d: %s\n", i, chatHistory[i].c_str());
      }
    }
    else if (input == "/clear") {
      chatIndex = 0;
      dbg.println("[OK] History cleared");
    }
    else if (input == "/factory") {
      dbg.println("[E220] Restoring factory defaults (900MHz: addr=0, ch=80, 9600 8N1, air 2.4k, 21dBm)...");
      e220_config.freq = 930.125;
      e220_config.txpower = 21;
      e220_config.baud = 9600;
      strlcpy(e220_config.addr, "0x0000", sizeof(e220_config.addr));
      strlcpy(e220_config.dest, "0xFFFF", sizeof(e220_config.dest));
      e220_config.airrate = 2;
      e220_config.subpkt = 0;
      e220_config.parity = 0;
      e220_config.txmode = 0;
      e220_config.rssi_noise = 0;
      e220_config.rssi_byte = 0;
      e220_config.lbt = 0;
      e220_config.wor_cycle = 3;
      e220_config.crypt_h = 0;
      e220_config.crypt_l = 0;
      e220_config.savetype = 1;
      applyE220Config();
      delay(500);
      readE220Config();
    }
    else if (input == "/help") {
      dbg.println("Type anything to send it via E220.");
      dbg.println("Slash commands:");
      dbg.println("  /config   - Show E220 config");
      dbg.println("  /read     - Read module registers");
      dbg.println("  /factory  - Restore factory defaults");
      dbg.println("  /history  - Show chat history");
      dbg.println("  /clear    - Clear history");
      dbg.println("  /help     - This help");
    }
    else {
      // Everything else is a message - send it (chunked for large messages)
      const int CHUNK_SIZE = 190;
      int inputLen = input.length();
      
      if (inputLen <= CHUNK_SIZE) {
        e220Serial.print(input);
        e220Serial.print('\n');
      } else {
        for (int i = 0; i < inputLen; i += CHUNK_SIZE) {
          int end = min(i + CHUNK_SIZE, inputLen);
          String chunk = input.substring(i, end);
          e220Serial.print(chunk);
          e220Serial.flush();
          waitE220Ready(3000);
          delay(50);
        }
        e220Serial.print('\n');
      }
      e220Serial.flush();
      
      if (chatIndex < 100) {
        chatHistory[chatIndex] = "[TX] " + input;
        chatIndex++;
      }
      dbg.printf("[TX] (%d bytes) %s\n", inputLen, input.c_str());
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  dbg.println("\n\n[BOOT] E220 Chat + Config");
  
  setupFS();
  setupE220();
  setupWiFi();
  setupWebRoutes();
  
  // Read current E220 config on boot
  dbg.println("[BOOT] Reading E220 module config...");
  readE220Config();
  
  dbg.println("[BOOT] Ready!");
}

// Drain TX queue from loop() context where blocking is safe
void handleTxQueue() {
  if (!txPending) return;
  
  const int CHUNK_SIZE = 190;
  int msgLen = txQueue.length();
  
  dbg.printf("[TX] Sending (%d bytes)...\n", msgLen);
  
  if (msgLen <= CHUNK_SIZE) {
    e220Serial.print(txQueue);
    e220Serial.print('\n');
  } else {
    for (int i = 0; i < msgLen; i += CHUNK_SIZE) {
      int chunkEnd = min(i + CHUNK_SIZE, msgLen);
      String chunk = txQueue.substring(i, chunkEnd);
      e220Serial.print(chunk);
      e220Serial.flush();
      waitE220Ready(3000);
      delay(50);
    }
    e220Serial.print('\n');
  }
  e220Serial.flush();
  
  dbg.printf("[TX] Sent %d bytes\n", msgLen);
  txQueue = "";
  txPending = false;
}

void loop() {
  handleE220Serial();
  handleUSBSerial();
  handleTxQueue();
  delay(10);
}
