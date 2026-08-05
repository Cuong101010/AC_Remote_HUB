/*
  AC Controller Firmware v0.3 - MQTT Instant Real-Time Control + OLED FSM
  Board: ESP32 Dev Module

  Chức năng nổi bật v0.3:
  1) MQTT Instant Push: Nhận lệnh điều khiển tức thì (<50ms latency) từ Broker thông qua PubSubClient.
  2) Tự động kết nối lại MQTT Broker khi mất mạng.
  3) Phát tín hiệu IR NATIVE hoặc RAW cực nhanh ngay khi bấm nút trên Web.
  4) Đọc & Publish dữ liệu cảm biến DHT11 (Nhiệt độ, Độ ẩm) theo chu kỳ 5 giây lên MQTT & HTTP Backend.
  5) Màn hình OLED SSD1306 hiển thị trạng thái MQTT, Wi-Fi, Mã Ghép Nối.
  6) Giữ đầy đủ fallback HTTP Rest API cho ghép nối và đăng ký thiết bị.

  Phần cứng:
  - HX1838 IR Receiver -> GPIO27
  - LED IR Transmitter -> GPIO4
  - OLED SSD1306 I2C   -> SDA: GPIO21, SCL: GPIO22 (Address 0x3C)
  - DHT11 Sensor       -> GPIO14

  Thư viện cần cài đặt trên Arduino IDE / PlatformIO:
  - PubSubClient by Nick O'Leary
  - WiFiManager by tzapu
  - ArduinoJson (v6 hoặc v7)
  - IRremoteESP8266
  - U8g2 by oliver
  - DHT sensor library by Adafruit
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <DHT.h>

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRac.h>
#include <IRutils.h>

// ============================================================
// CẤU HÌNH THÔNG SỐ HỆ THỐNG
// ============================================================

static const char *FW_VERSION = "0.3.0-MQTT";

// API Server HTTP & MQTT Broker
static const char *API_BASE_URL        = "https://ac-remote-hub-hf34.vercel.app/api/v1";
static const char *DEVICE_BOOTSTRAP_KEY= "CHANGE_ME_BOOTSTRAP_KEY";
static const char *CONFIG_AP_PASSWORD  = ""; // Chuỗi rỗng = Open AP

// Cấu hình MQTT Broker (Public HiveMQ Broker hoặc broker riêng)
static const char *MQTT_BROKER = "broker.hivemq.com";
static const uint16_t MQTT_PORT = 1883;

// ============================================================
// PHẦN CỨNG
// ============================================================

static const uint16_t IR_RX_PIN   = 27;
static const uint16_t IR_TX_PIN   = 4;
static const uint8_t  OLED_SDA_PIN = 21;
static const uint8_t  OLED_SCL_PIN = 22;
static const uint8_t  OLED_I2C_ADDRESS = 0x3C;
static const uint8_t  DHT_PIN      = 14;
static const uint8_t  DHT_TYPE     = DHT11;

static const uint16_t IR_CAPTURE_BUFFER_SIZE = 2048;
static const uint8_t  IR_CAPTURE_TIMEOUT_MS   = 80;
static const uint16_t MAX_RAW_TIMINGS_UPLOAD  = 700;

// ============================================================
// CHU KỲ
// ============================================================

static const unsigned long COMMAND_POLL_INTERVAL_MS      = 3000;
static const unsigned long HEARTBEAT_INTERVAL_MS         = 15000;
static const unsigned long MQTT_RECONNECT_INTERVAL_MS    = 5000;
static const unsigned long REGISTER_RETRY_INTERVAL_MS     = 10000;
static const unsigned long DEFAULT_LEARNING_TIMEOUT_MS   = 45000;
static const unsigned long BOOT_SCREEN_DURATION_MS       = 2000;
static const unsigned long EVENT_SCREEN_DURATION_MS      = 2500;
static const unsigned long DISPLAY_STATUS_REFRESH_MS     = 1000;
static const unsigned long CLOUD_ONLINE_WINDOW_MS        = 15000;
static const unsigned long WIFI_INITIAL_CONNECT_WINDOW_MS= 10000;
static const unsigned long DHT_READ_INTERVAL_MS         = 5000;

// ============================================================
// ĐỐI TƯỢNG TOÀN CỤC
// ============================================================

Preferences preferences;
DHT dht(DHT_PIN, DHT_TYPE);
WiFiManager wifiManager;
WiFiClient espMqttClient;
PubSubClient mqttClient(espMqttClient);

U8X8_SSD1306_128X64_NONAME_HW_I2C oled(U8X8_PIN_NONE);
bool oledReady = false;
bool wifiManagerStarted = false;
bool wifiPortalStarted = false;
bool wifiWasConnected = false;
unsigned long wifiInitialConnectStartedAt = 0;
String wifiConfigApName;

IRrecv irReceiver(IR_RX_PIN, IR_CAPTURE_BUFFER_SIZE, IR_CAPTURE_TIMEOUT_MS, true);
IRsend irSender(IR_TX_PIN);
IRac universalAc(IR_TX_PIN);
decode_results irResults;

// ============================================================
// TRẠNG THÁI
// ============================================================

String deviceId;
String deviceToken;
String pairingCode;
String lastCommandId;

float dhtTemperature = NAN;
float dhtHumidity    = NAN;
unsigned long lastDhtReadAt = 0;
unsigned long lastCommandPollAt = 0;
unsigned long lastHeartbeatAt = 0;
unsigned long lastRegisterAttemptAt = 0;
unsigned long wifiDisconnectedSince = 0;
unsigned long lastMqttReconnectAttemptAt = 0;

bool devicePaired = false;
bool cloudConnected = false;
unsigned long lastCloudSuccessAt = 0;

enum class DisplayState : uint8_t {
  STATE_BOOT,
  STATE_WIFI_CONNECTING,
  STATE_PAIRING_CODE,
  STATE_IDLE_MASCOT,
  STATE_EVENT_CMD,
  STATE_EVENT_LEARN,
  STATE_EVENT_LEARN_OK
};

struct CommandScreenData {
  bool power = false;
  int temperature = 26;
  String mode = "AUTO";
  String commandType = "SET_AC_STATE";
  bool resultKnown = false;
  bool resultOk = false;
};

DisplayState displayState = DisplayState::STATE_BOOT;
unsigned long displayStateEnteredAt = 0;
unsigned long lastDisplayRefreshAt = 0;
bool displayDirty = true;
CommandScreenData commandScreen;
String learnedProtocolScreen;
uint16_t learnedBitsScreen = 0;

struct LearningSession {
  bool active = false;
  String commandId;
  String profileId;
  String expectedAction;
  unsigned long startedAt = 0;
  unsigned long timeoutMs = DEFAULT_LEARNING_TIMEOUT_MS;
};

LearningSession learning;
stdAc::state_t previousAcState;
bool hasPreviousAcState = false;
String previousProfileId;
decode_type_t previousProtocol = decode_type_t::UNKNOWN;

// Khai báo hàm
void renderDisplay();
void setDisplayState(const DisplayState nextState, const bool renderNow = true);
void goToBaseDisplayState();
void processCommand(JsonObject command, const char* source = "MQTT Instant ⚡");
void acknowledgeCommand(const String &commandId, const String &status, const String &message);
void publishMqttSensorData();
void publishMqttHeartbeat();

// ============================================================
// OLED DISPLAY RENDER
// ============================================================

String fit16(const String &text) {
  if (text.length() <= 16) return text;
  return text.substring(0, 16);
}

void drawRow(const uint8_t row, const String &text) {
  if (!oledReady || row > 7) return;
  const String line = fit16(text);
  oled.drawString(0, row, "                ");
  oled.drawString(0, row, line.c_str());
}

void setDisplayState(const DisplayState nextState, const bool renderNow) {
  displayState = nextState;
  displayStateEnteredAt = millis();
  displayDirty = true;
  if (renderNow) renderDisplay();
}

void markCloudSuccess() {
  lastCloudSuccessAt = millis();
  cloudConnected = true;
  displayDirty = true;
}

void savePairedState(const bool paired) {
  if (devicePaired == paired) return;
  devicePaired = paired;
  preferences.putBool("paired", paired);
  displayDirty = true;
  if (paired && WiFi.status() == WL_CONNECTED) {
    goToBaseDisplayState();
  }
}

void renderPairingScreen() {
  drawRow(0, mqttClient.connected() ? " MQTT: ONLINE  " : " MQTT: OFF/POLL");
  drawRow(1, "----------------");
  const String code = pairingCode.isEmpty() ? "------" : pairingCode;
  oled.draw2x2String(2, 3, code.c_str());
  drawRow(7, "  AC REMOTE HUB ");
}

void renderCommandScreen() {
  drawRow(0, "  [ MQTT CMD ]  ");
  drawRow(1, "----------------");
  drawRow(2, String("PWR : ") + (commandScreen.power ? "ON" : "OFF"));
  drawRow(3, "TEMP: " + String(commandScreen.temperature) + " C");
  drawRow(4, "MODE: " + commandScreen.mode);
  if (!commandScreen.resultKnown) {
    drawRow(6, "IR  : SENDING...");
  } else {
    drawRow(6, String("IR  : ") + (commandScreen.resultOk ? "OK" : "ERROR"));
  }
}

void renderDisplay() {
  if (!oledReady) return;
  switch (displayState) {
    case DisplayState::STATE_BOOT:
      drawRow(1, " AC REMOTE HUB ");
      drawRow(3, "  v0.3.0 MQTT  ");
      drawRow(6, "  [BOOTING...]");
      break;
    case DisplayState::STATE_WIFI_CONNECTING:
      drawRow(0, " WiFi: CONNECT ");
      drawRow(1, "----------------");
      drawRow(2, "AP:");
      drawRow(3, " " + fit16(wifiConfigApName));
      drawRow(5, "IP: 192.168.4.1");
      break;
    case DisplayState::STATE_PAIRING_CODE:
    case DisplayState::STATE_IDLE_MASCOT:
      renderPairingScreen();
      break;
    case DisplayState::STATE_EVENT_CMD:
      renderCommandScreen();
      break;
    case DisplayState::STATE_EVENT_LEARN:
      drawRow(0, "  [ LEARN IR ]  ");
      drawRow(1, "----------------");
      drawRow(2, "PRESS REMOTE...");
      break;
    case DisplayState::STATE_EVENT_LEARN_OK:
      drawRow(0, "  [ LEARN OK ]  ");
      drawRow(1, "----------------");
      drawRow(2, "PROTO:");
      drawRow(3, " " + fit16(learnedProtocolScreen));
      break;
  }
  displayDirty = false;
}

void goToBaseDisplayState() {
  if (WiFi.status() != WL_CONNECTED) {
    setDisplayState(DisplayState::STATE_WIFI_CONNECTING);
    return;
  }
  setDisplayState(DisplayState::STATE_PAIRING_CODE);
}

void processDisplayFsm(const unsigned long now) {
  if (displayState == DisplayState::STATE_EVENT_CMD && now - displayStateEnteredAt >= EVENT_SCREEN_DURATION_MS) {
    goToBaseDisplayState();
  }
  if (displayDirty || now - lastDisplayRefreshAt >= DISPLAY_STATUS_REFRESH_MS) {
    renderDisplay();
    lastDisplayRefreshAt = now;
  }
}

// ============================================================
// MQTT CALLBACK & RECONNECT
// ============================================================

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  Serial.printf("\n⚡ [MQTT INSTANT PUSH] Nhan lenh qua Topic: %s (do dai: %d bytes)\n", topic, length);
  
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[MQTT Error] JSON message khong hop le: %s\n", err.c_str());
    return;
  }

  JsonObject command = doc.as<JsonObject>();
  if (doc["command"].is<JsonObject>()) {
    command = doc["command"].as<JsonObject>();
  }

  if (!command.isNull() && command.size() > 0) {
    processCommand(command);
  }
}

void maintainMqttConnection() {
  if (WiFi.status() != WL_CONNECTED || deviceId.isEmpty()) return;

  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }

  const unsigned long now = millis();
  if (now - lastMqttReconnectAttemptAt >= MQTT_RECONNECT_INTERVAL_MS) {
    lastMqttReconnectAttemptAt = now;
    Serial.printf("[MQTT] Dang ket noi toi Broker: %s:%d...\n", MQTT_BROKER, MQTT_PORT);

    String clientId = "ESP32_ACRemote_" + deviceId;
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("✅ [MQTT CONNECTED] Ket noi MQTT thanh cong!");

      String cmdTopic = "acremote/devices/" + deviceId + "/commands";
      mqttClient.subscribe(cmdTopic.c_str(), 1);
      Serial.printf("[MQTT Subscribed] Topic: %s\n", cmdTopic.c_str());

      markCloudSuccess();
      displayDirty = true;
    } else {
      Serial.printf("❌ [MQTT Failed] state=%d. Thu lai sau 5s...\n", mqttClient.state());
    }
  }
}

// ============================================================
// HTTP FALLBACK & DEVICE REGISTRATION
// ============================================================

String apiUrl(const String &path) {
  String base(API_BASE_URL);
  base.replace("//api", "/api");
  while (base.endsWith("/")) base.remove(base.length() - 1);
  return path.startsWith("/") ? base + path : base + "/" + path;
}

struct HttpResponse {
  int code = -1;
  String body;
  String error;
};

HttpResponse executeHttpJson(const String &method, const String &url, const String &payload, const bool auth) {
  HttpResponse response;
  HTTPClient http;
  WiFiClient plainClient;
  WiFiClientSecure secureClient;

  bool begun = url.startsWith("https://") ? (secureClient.setInsecure(), http.begin(secureClient, url)) : http.begin(plainClient, url);
  if (!begun) { response.error = "HTTP begin failed"; return response; }

  http.setConnectTimeout(5000);
  http.setTimeout(10000);
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Id", deviceId);
  if (auth && !deviceToken.isEmpty()) http.addHeader("Authorization", "Bearer " + deviceToken);

  response.code = (method == "GET") ? http.GET() : http.POST(payload);
  if (response.code > 0) response.body = http.getString();
  else response.error = http.errorToString(response.code);

  if (response.code >= 200 && response.code < 300) markCloudSuccess();
  http.end();
  return response;
}

bool registerDevice() {
  DynamicJsonDocument doc(768);
  doc["deviceId"] = deviceId;
  doc["firmwareVersion"] = FW_VERSION;
  doc["mac"] = WiFi.macAddress();
  doc["mqttBroker"] = MQTT_BROKER;

  String payload;
  serializeJson(doc, payload);
  HttpResponse res = executeHttpJson("POST", apiUrl("/devices/register"), payload, false);

  if (res.code != 200 && res.code != 201) return false;

  DynamicJsonDocument resp(1536);
  if (deserializeJson(resp, res.body)) return false;

  deviceToken = resp["deviceToken"] | resp["data"]["deviceToken"] | "";
  pairingCode = resp["pairingCode"] | resp["data"]["pairingCode"] | "";
  if (deviceToken.isEmpty()) return false;

  preferences.putString("token", deviceToken);
  preferences.putString("pairCode", pairingCode);
  goToBaseDisplayState();
  return true;
}

void ensureDeviceRegistered() {
  if (!deviceToken.isEmpty()) return;
  if (millis() - lastRegisterAttemptAt < REGISTER_RETRY_INTERVAL_MS) return;
  lastRegisterAttemptAt = millis();
  registerDevice();
}

// ============================================================
// DHT11 & SENSOR DATA
// ============================================================

void readDhtSensor() {
  const float t = dht.readTemperature();
  const float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    dhtTemperature = t;
    dhtHumidity    = h;
    Serial.printf("[DHT11 Sensor] Nhiet do: %.1f C | Do am: %.1f %%\n", t, h);
  }
}

void publishMqttSensorData() {
  if (isnan(dhtTemperature) || isnan(dhtHumidity)) return;

  DynamicJsonDocument doc(256);
  doc["deviceId"]    = deviceId;
  doc["temperature"] = serialized(String(dhtTemperature, 1));
  doc["humidity"]    = serialized(String(dhtHumidity, 1));
  doc["timestamp"]   = millis();

  String payload;
  serializeJson(doc, payload);

  // Publish len MQTT Topic
  if (mqttClient.connected()) {
    String topic = "acremote/devices/" + deviceId + "/sensor";
    mqttClient.publish(topic.c_str(), payload.c_str());
  }

  // Gửi backup qua HTTP
  if (WiFi.status() == WL_CONNECTED && !deviceToken.isEmpty()) {
    executeHttpJson("POST", apiUrl("/devices/" + deviceId + "/sensor"), payload, true);
  }
}

void sendHeartbeat() {
  DynamicJsonDocument doc(512);
  doc["deviceId"] = deviceId;
  doc["firmwareVersion"] = FW_VERSION;
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["uptimeMs"] = millis();
  doc["mqttConnected"] = mqttClient.connected();
  if (!isnan(dhtTemperature)) doc["temperature"] = serialized(String(dhtTemperature, 1));
  if (!isnan(dhtHumidity))    doc["humidity"]    = serialized(String(dhtHumidity, 1));

  String payload;
  serializeJson(doc, payload);

  if (mqttClient.connected()) {
    String topic = "acremote/devices/" + deviceId + "/heartbeat";
    mqttClient.publish(topic.c_str(), payload.c_str());
  }

  if (WiFi.status() == WL_CONNECTED && !deviceToken.isEmpty()) {
    executeHttpJson("POST", apiUrl("/devices/" + deviceId + "/heartbeat"), payload, true);
  }
}

// ============================================================
// IR COMMAND TRANSMISSION
// ============================================================

bool sendNativeAcState(JsonObject command, String &errorMessage) {
  const String protocolStr = command["protocol"] | "";
  const decode_type_t protocol = strToDecodeType(protocolStr.c_str());
  if (!IRac::isProtocolSupported(protocol)) {
    errorMessage = "Protocol is not supported: " + protocolStr;
    return false;
  }

  stdAc::state_t state;
  IRac::initState(&state);
  state.protocol = protocol;
  state.power    = command["power"] | true;
  state.degrees  = command["temperature"] | 26.0;
  state.mode     = IRac::strToOpmode(command["mode"] | "cool");
  state.fanspeed = IRac::strToFanspeed(command["fan"] | "auto");

  universalAc.sendAc(state, &previousAcState);
  previousAcState = state;
  return true;
}

bool sendRawSignal(JsonObject command, String &errorMessage) {
  JsonArray rawUs = command["rawUs"].as<JsonArray>();
  if (rawUs.isNull() || rawUs.size() == 0) {
    errorMessage = "rawUs empty";
    return false;
  }
  const uint16_t khz = command["frequencyKhz"] | 38;
  const uint16_t len = rawUs.size();
  uint16_t *buffer = new (std::nothrow) uint16_t[len];
  if (!buffer) { errorMessage = "No memory for rawUs"; return false; }

  for (uint16_t i = 0; i < len; i++) buffer[i] = rawUs[i].as<uint16_t>();
  irSender.sendRaw(buffer, len, khz);
  delete[] buffer;
  return true;
}

void acknowledgeCommand(const String &commandId, const String &status, const String &message) {
  if (commandId.isEmpty()) return;

  DynamicJsonDocument doc(512);
  doc["id"] = commandId;
  doc["deviceId"] = deviceId;
  doc["status"] = status;
  doc["message"] = message;
  doc["deviceTimeMs"] = millis();

  String payload;
  serializeJson(doc, payload);

  if (mqttClient.connected()) {
    String topic = "acremote/devices/" + deviceId + "/ack";
    mqttClient.publish(topic.c_str(), payload.c_str());
  }

  if (WiFi.status() == WL_CONNECTED && !deviceToken.isEmpty()) {
    executeHttpJson("POST", apiUrl("/devices/" + deviceId + "/commands/" + commandId + "/ack"), payload, true);
  }
}

void processCommand(JsonObject command, const char* source) {
  const String commandId = command["id"] | "";
  const String type      = command["type"] | "";

  if (commandId.isEmpty() || type.isEmpty()) return;
  if (commandId == lastCommandId) return; // Bo qua lenh trùng
  lastCommandId = commandId;

  Serial.println("\n========================================");
  Serial.printf("➡️ NHẬN LỆNH QUA KÊNH: %s\n", source);
  Serial.printf("   Command ID : %s\n", commandId.c_str());
  Serial.printf("   Type       : %s\n", type.c_str());
  Serial.println("========================================");

  if (type == "SET_AC_STATE") {
    commandScreen.commandType = type;
    commandScreen.power = command["power"] | false;
    commandScreen.temperature = static_cast<int>(command["temperature"] | 26.0);
    commandScreen.mode = String(command["mode"] | "COOL");
    setDisplayState(DisplayState::STATE_EVENT_CMD);

    String errorMsg;
    const bool ok = sendNativeAcState(command, errorMsg);
    commandScreen.resultKnown = true;
    commandScreen.resultOk = ok;
    displayDirty = true;

    acknowledgeCommand(commandId, ok ? "completed" : "failed", ok ? String(source) + " - Native AC IR Sent OK" : errorMsg);
    return;
  }

  if (type == "SEND_RAW") {
    commandScreen.commandType = type;
    commandScreen.power = true;
    commandScreen.mode = "RAW";
    setDisplayState(DisplayState::STATE_EVENT_CMD);

    String errorMsg;
    const bool ok = sendRawSignal(command, errorMsg);
    commandScreen.resultKnown = true;
    commandScreen.resultOk = ok;
    displayDirty = true;

    acknowledgeCommand(commandId, ok ? "completed" : "failed", ok ? String(source) + " - Raw IR Signal Sent OK" : errorMsg);
    return;
  }
}

void pollNextCommand() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (deviceToken.isEmpty()) {
    ensureDeviceRegistered();
    return;
  }
  String path = "/devices/" + deviceId + "/commands/next";
  if (!lastCommandId.isEmpty()) path += "?after=" + lastCommandId;

  HttpResponse res = executeHttpJson("GET", apiUrl(path), "", true);
  if (res.code == 200) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, res.body)) {
      JsonObject cmd = doc["command"].is<JsonObject>() ? doc["command"].as<JsonObject>() : doc.as<JsonObject>();
      if (!cmd.isNull() && cmd.size() > 0) processCommand(cmd, "HTTP Polling 🔄");
    }
  }
}

// ============================================================
// SETUP & LOOP
// ============================================================

String buildDeviceId() {
  const uint64_t chipId = ESP.getEfuseMac();
  char idBuffer[32];
  snprintf(idBuffer, sizeof(idBuffer), "ACIR-%04X%08X", static_cast<uint16_t>(chipId >> 32), static_cast<uint32_t>(chipId));
  return String(idBuffer);
}

void setup() {
  Serial.begin(115200);
  deviceId = buildDeviceId();

  preferences.begin("ac-controller", false);
  deviceToken  = preferences.getString("token", "");
  pairingCode  = preferences.getString("pairCode", "");
  devicePaired = preferences.getBool("paired", false);

  Serial.println("\n========================================");
  Serial.println("AC CONTROLLER FIRMWARE v0.3 (MQTT PUSH)");
  Serial.print("Device ID: "); Serial.println(deviceId);
  Serial.println("========================================");

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.beginTransmission(OLED_I2C_ADDRESS);
  if (Wire.endTransmission() == 0) {
    oled.begin();
    oled.setFont(u8x8_font_chroma48medium8_r);
    oled.clear();
    oledReady = true;
  }

  dht.begin();
  irSender.begin();
  irReceiver.enableIRIn();
  IRac::initState(&previousAcState);

  // Setup MQTT
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(4096); // Buffer lớn chứa lệnh RAW IR

  setDisplayState(DisplayState::STATE_BOOT);

  // WiFi Non-blocking setup
  wifiConfigApName = "AC-Remote-" + deviceId.substring(deviceId.length() - 6);
  WiFi.mode(WIFI_STA);
  WiFi.begin();
}

void loop() {
  const unsigned long now = millis();

  if (!wifiManagerStarted && now - displayStateEnteredAt >= BOOT_SCREEN_DURATION_MS) {
    wifiManagerStarted = true;
    setDisplayState(DisplayState::STATE_WIFI_CONNECTING);
  }

  if (WiFi.status() == WL_CONNECTED) {
    ensureDeviceRegistered();
    maintainMqttConnection();

    // Polling HTTP dự phòng nhận lệnh từ Server (nếu MQTT trên Cloud bị nghẽn port)
    if (now - lastCommandPollAt >= COMMAND_POLL_INTERVAL_MS) {
      lastCommandPollAt = now;
      pollNextCommand();
    }

    if (now - lastHeartbeatAt >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeatAt = now;
      sendHeartbeat();
    }

    if (now - lastDhtReadAt >= DHT_READ_INTERVAL_MS) {
      lastDhtReadAt = now;
      readDhtSensor();
      publishMqttSensorData();
    }
  }

  processDisplayFsm(now);
  yield();
}
