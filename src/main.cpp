#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define E220_RX_PIN   21
#define E220_TX_PIN   22
#define E220_M0_PIN   2
#define E220_M1_PIN   19
#define UART_BAUD     9600

AsyncWebServer server(80);
HardwareSerial e220Serial(2);
String chatHistory[100];
int chatIndex = 0;

// E220 Config stored in EEPROM
struct {
  float freq;
  int txpower;
  int baud;
  char addr[8];
  char dest[8];
  int netid;
  int airrate;
  int pktsize;
  int rxtmo;
  int txwait;
  int subpkt;
  int rssi;
  int crc;
  int repeater;
  int parity;
  int txmode;
  int savetype;
} e220_config = {930.125, 21, 9600, "0x0000", "0xFFFF", 0, 4, 0, 1000, 0, 1, -100, 1, 0, 0, 0, 0};

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

void applyE220Config() {
  // Set to CONFIG mode (M0=1, M1=1)
  digitalWrite(E220_M0_PIN, HIGH);
  digitalWrite(E220_M1_PIN, HIGH);
  delay(100);
  
  // Build config command
  // E220 uses binary protocol or AT commands depending on firmware
  // For simplicity, send configuration bytes
  uint8_t config[6] = {
    0xC0,                           // Address high byte
    (uint8_t)(e220_config.netid),  // Address low byte  
    0x00 | (e220_config.airrate << 3) | (e220_config.pktsize << 6),  // SPED
    (uint8_t)((int)(e220_config.freq * 10) % 256),                  // CHAN (derived from freq)
    0x40 | (e220_config.crc << 4) | (e220_config.txmode << 7),     // OPMODE
    (e220_config.savetype == 1) ? 0xC0 : 0x80                        // SAVETYPE
  };
  
  e220Serial.write(0xC0);
  e220Serial.write(config[1]);
  e220Serial.write(config[2]);
  e220Serial.write(config[3]);
  e220Serial.write(config[4]);
  e220Serial.write(config[5]);
  e220Serial.flush();
  
  delay(100);
  
  // Return to NORMAL mode (M0=0, M1=0)
  setE220Mode(0);
  
  Serial.println("[E220] Config applied");
}

void setupE220() {
  pinMode(E220_M0_PIN, OUTPUT);
  pinMode(E220_M1_PIN, OUTPUT);
  e220Serial.begin(UART_BAUD, SERIAL_8N1, E220_RX_PIN, E220_TX_PIN);
  setE220Mode(0);
  Serial.println("[E220] Init");
}

void setupWiFi() {
  WiFi.mode(WIFI_AP);
  // Generate random 3-digit number (100-999) for unique SSID
  int randomNum = random(100, 1000);
  String ssidName = "E220-Chat-" + String(randomNum);
  WiFi.softAP(ssidName.c_str(), "password123");
  Serial.print("[WiFi] SSID: ");
  Serial.println(ssidName);
  Serial.print("[WiFi] IP: ");
  Serial.println(WiFi.softAPIP());
}

void setupFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Mount failed");
    return;
  }
  Serial.println("[FS] Ready");
  
  // Debug: list files
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while(file) {
    Serial.print("[FS] Found: ");
    Serial.println(file.name());
    file = root.openNextFile();
  }
}

void setupWebRoutes() {
  // Serve index.html (with gzip support)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[Web] Request for /");
    
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
      Serial.println("[Web] Serving compressed HTML");
      AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/index.html.gz", "text/html");
      response->addHeader("Content-Encoding", "gzip");
      response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
      response->addHeader("Pragma", "no-cache");
      response->addHeader("Expires", "0");
      request->send(response);
    } else if (LittleFS.exists("/index.html")) {
      Serial.println("[Web] Serving uncompressed HTML");
      AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/index.html", "text/html");
      response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
      response->addHeader("Pragma", "no-cache");
      response->addHeader("Expires", "0");
      request->send(response);
    } else {
      request->send(404, "text/plain", "index.html not found");
      Serial.println("[Web] HTML not found!");
    }
  });

  // Chat history API
  server.on("/api/chat", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"history\":[";
    for (int i = 0; i < chatIndex; i++) {
      json += "\"" + chatHistory[i] + "\"";
      if (i < chatIndex - 1) json += ",";
    }
    json += "]}";
    request->send(200, "application/json", json);
  });

  // Send message API
  server.on("/api/send", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    String json = String((char*)data, len);
    if (json.indexOf("message") != -1) {
      int start = json.indexOf(":") + 2;  // Skip past: {"message":"
      int end = json.lastIndexOf("\"");
      String msg = json.substring(start, end);
      
      e220Serial.println(msg);
      if (chatIndex < 100) {
        chatHistory[chatIndex] = "[TX] " + msg;
        chatIndex++;
      }
      
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      Serial.print("[TX] ");
      Serial.println(msg);
    } else {
      request->send(400, "application/json", "{\"error\":\"no message\"}");
    }
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
    config["netid"] = e220_config.netid;
    config["airrate"] = e220_config.airrate;
    config["pktsize"] = e220_config.pktsize;
    config["rxtmo"] = e220_config.rxtmo;
    config["txwait"] = e220_config.txwait;
    config["subpkt"] = e220_config.subpkt;
    config["rssi"] = e220_config.rssi;
    config["crc"] = e220_config.crc;
    config["repeater"] = e220_config.repeater;
    config["parity"] = e220_config.parity;
    config["txmode"] = e220_config.txmode;
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
    
    // Update config - all parameters
    if (doc.containsKey("freq")) e220_config.freq = doc["freq"];
    if (doc.containsKey("txpower")) e220_config.txpower = doc["txpower"];
    if (doc.containsKey("baud")) e220_config.baud = doc["baud"];
    if (doc.containsKey("addr")) strlcpy(e220_config.addr, doc["addr"], sizeof(e220_config.addr));
    if (doc.containsKey("dest")) strlcpy(e220_config.dest, doc["dest"], sizeof(e220_config.dest));
    if (doc.containsKey("netid")) e220_config.netid = doc["netid"];
    if (doc.containsKey("airrate")) e220_config.airrate = doc["airrate"];
    if (doc.containsKey("pktsize")) e220_config.pktsize = doc["pktsize"];
    if (doc.containsKey("rxtmo")) e220_config.rxtmo = doc["rxtmo"];
    if (doc.containsKey("txwait")) e220_config.txwait = doc["txwait"];
    if (doc.containsKey("subpkt")) e220_config.subpkt = doc["subpkt"];
    if (doc.containsKey("rssi")) e220_config.rssi = doc["rssi"];
    if (doc.containsKey("crc")) e220_config.crc = doc["crc"];
    if (doc.containsKey("repeater")) e220_config.repeater = doc["repeater"];
    if (doc.containsKey("parity")) e220_config.parity = doc["parity"];
    if (doc.containsKey("txmode")) e220_config.txmode = doc["txmode"];
    if (doc.containsKey("savetype")) e220_config.savetype = doc["savetype"];
    
    Serial.println("[CONFIG] Updated all parameters:");
    Serial.print("  freq="); Serial.print(e220_config.freq);
    Serial.print(" txpower="); Serial.print(e220_config.txpower);
    Serial.print(" baud="); Serial.println(e220_config.baud);
    Serial.print("  addr="); Serial.print(e220_config.addr);
    Serial.print(" dest="); Serial.print(e220_config.dest);
    Serial.print(" netid="); Serial.println(e220_config.netid);
    Serial.print("  airrate="); Serial.print(e220_config.airrate);
    Serial.print(" pktsize="); Serial.println(e220_config.pktsize);
    Serial.print("  rxtmo="); Serial.print(e220_config.rxtmo);
    Serial.print(" txwait="); Serial.print(e220_config.txwait);
    Serial.print(" subpkt="); Serial.println(e220_config.subpkt);
    Serial.print("  rssi="); Serial.print(e220_config.rssi);
    Serial.print(" crc="); Serial.print(e220_config.crc);
    Serial.print(" repeater="); Serial.println(e220_config.repeater);
    Serial.print("  parity="); Serial.print(e220_config.parity);
    Serial.print(" txmode="); Serial.print(e220_config.txmode);
    Serial.print(" savetype="); Serial.println(e220_config.savetype);
    
    // Apply config to E220 module if savetype is 1 (EEPROM)
    if (e220_config.savetype == 1) {
      applyE220Config();
    }
    
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.begin();
  Serial.println("[Web] Server on 192.168.4.1");
}

void handleE220Serial() {
  while (e220Serial.available()) {
    String line = e220Serial.readStringUntil('\n');
    line.trim();
    
    if (line.length() > 0 && chatIndex < 100) {
      chatHistory[chatIndex] = "[RX] " + line;
      chatIndex++;
      Serial.print("[RX] ");
      Serial.println(line);
    }
  }
}

void handleUSBSerial() {
  while (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    if (cmd.length() == 0) return;
    
    // Commands:
    // "send <message>" - send via E220
    // "config" - show current config
    // "history" - show chat history
    // "clear" - clear chat history
    
    if (cmd.startsWith("send ")) {
      String msg = cmd.substring(5);
      e220Serial.println(msg);
      if (chatIndex < 100) {
        chatHistory[chatIndex] = "[TX] " + msg;
        chatIndex++;
      }
      Serial.println("[OK] Sent");
    }
    else if (cmd == "config") {
      Serial.print("freq="); Serial.println(e220_config.freq);
      Serial.print("txpower="); Serial.println(e220_config.txpower);
      Serial.print("baud="); Serial.println(e220_config.baud);
      Serial.print("addr="); Serial.println(e220_config.addr);
      Serial.print("dest="); Serial.println(e220_config.dest);
      Serial.print("netid="); Serial.println(e220_config.netid);
      Serial.print("airrate="); Serial.println(e220_config.airrate);
      Serial.print("pktsize="); Serial.println(e220_config.pktsize);
      Serial.print("rxtmo="); Serial.println(e220_config.rxtmo);
      Serial.print("txwait="); Serial.println(e220_config.txwait);
      Serial.print("subpkt="); Serial.println(e220_config.subpkt);
      Serial.print("rssi="); Serial.println(e220_config.rssi);
      Serial.print("crc="); Serial.println(e220_config.crc);
      Serial.print("repeater="); Serial.println(e220_config.repeater);
      Serial.print("parity="); Serial.println(e220_config.parity);
      Serial.print("txmode="); Serial.println(e220_config.txmode);
      Serial.print("savetype="); Serial.println(e220_config.savetype);
    }
    else if (cmd == "history") {
      Serial.print("[HISTORY] ");
      Serial.print(chatIndex);
      Serial.println(" messages:");
      for (int i = 0; i < chatIndex; i++) {
        Serial.print(i);
        Serial.print(": ");
        Serial.println(chatHistory[i]);
      }
    }
    else if (cmd == "clear") {
      chatIndex = 0;
      Serial.println("[OK] History cleared");
    }
    else if (cmd == "help") {
      Serial.println("Commands:");
      Serial.println("  send <message>  - Send message via E220");
      Serial.println("  config          - Show current E220 config");
      Serial.println("  history         - Show chat history");
      Serial.println("  clear           - Clear chat history");
      Serial.println("  help            - Show this help");
    }
    else {
      Serial.println("[ERROR] Unknown command. Type 'help' for usage.");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n[BOOT] E220 Chat + Config");
  
  setupFS();
  setupE220();
  setupWiFi();
  setupWebRoutes();
  
  Serial.println("[BOOT] Ready!");
}

void loop() {
  handleE220Serial();
  handleUSBSerial();
  delay(10);
}
