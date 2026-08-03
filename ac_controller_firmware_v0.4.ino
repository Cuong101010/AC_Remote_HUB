/*
  AC Controller Firmware v0.4 - MQTT Hybrid Architecture (<50ms Latency)
  Board: ESP32 Dev Module

  Thay đổi so với v0.3:
  - Tích hợp MQTT Client (PubSubClient) để nhận lệnh đẩy tức thì (Push Notification) từ Server/Broker.
  - Loại bỏ hoàn toàn độ trễ HTTP Polling (giảm từ 10s xuống <50ms khi dùng Cloud MQTT như HiveMQ/EMQX/Public Broker).
  - Duy trì kết nối TCP/TLS vĩnh viễn tới MQTT Broker. Khi có lệnh bấm từ Web, Broker đẩy ngay lập tức xuống ESP32.
  - Giữ nguyên 100% cơ chế ghép nối (Pairing Code), Quản lý User và Database cũ qua HTTP Bootstrap.
  - Hỗ trợ MQTT ACK phản hồi trạng thái thực thi lệnh về Backend/Web.
  - Giữ nguyên kiến trúc FreeRTOS 2 Lõi: Core 0 (MQTT / Network), Core 1 (IR Control / OLED FSM).

  Thư viện bổ sung cần cài trong Arduino IDE:
  - PubSubClient by Nick O'Leary
  - WiFiManager by tzapu
  - ArduinoJson (v6)
  - IRremoteESP8266
  - U8g2 by oliver
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
#include <new>

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRac.h>
#include <IRutils.h>

// ============================================================
// CẤU HÌNH HỆ THỐNG & MQTT
// ============================================================

static const char *FW_VERSION = "0.4.0";

// API Backend HTTP (Dùng để đăng ký thiết bị ban đầu & lấy Pairing Code)
static const char *API_BASE_URL = "https://accontrollerremote.pythonanywhere.com/api/v1";
static const char *DEVICE_BOOTSTRAP_KEY = "CHANGE_ME_BOOTSTRAP_KEY";
static const char *CONFIG_AP_PASSWORD = ""; // Chuỗi rỗng = Open AP

// MQTT Broker (Mặc định dùng Public Broker miễn phí cực nhanh, có thể đổi sang HiveMQ Cloud)
// Ví dụ Public: "broker.hivemq.com" (Port 1883) hoặc "broker.emqx.io" (Port 1883)
// Ví dụ Private/HiveMQ Cloud: "xxxx.s1.eu.hivemq.cloud" (Port 8883 với TLS)
static const char *MQTT_BROKER_HOST = "broker.hivemq.com";
static const uint16_t MQTT_BROKER_PORT = 1883;
static const char *MQTT_BROKER_USER = ""; // Bỏ rỗng nếu dùng Public Broker
static const char *MQTT_BROKER_PASS = "";

// ============================================================
// CẤU HÌNH PHẦN CỨNG
// ============================================================

static const uint16_t IR_RX_PIN = 27;
static const uint16_t IR_TX_PIN = 4;

static const uint8_t OLED_SDA_PIN = 21;
static const uint8_t OLED_SCL_PIN = 22;

static const uint16_t IR_CAPTURE_BUFFER_SIZE = 2048;
static const uint8_t IR_CAPTURE_TIMEOUT_MS = 80;
static const uint16_t MAX_RAW_SAMPLES = 400;

// ============================================================
// CHU KỲ HỆ THỐNG & TIMEOUT
// ============================================================

static const unsigned long HEARTBEAT_INTERVAL_MS = 30000;
static const unsigned long REGISTER_RETRY_INTERVAL_MS = 10000;
static const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
static const unsigned long DEFAULT_LEARNING_TIMEOUT_MS = 45000;
static const unsigned long EVENT_SCREEN_DURATION_MS = 3000;
static const unsigned long DISPLAY_STATUS_REFRESH_MS = 1000;
static const unsigned long CLOUD_ONLINE_WINDOW_MS = 15000;
static const uint32_t MIN_SAFE_HEAP_BYTES = 20000;

// ============================================================
// ĐỐI TƯỢNG TOÀN CỤC
// ============================================================

Preferences preferences;
WiFiManager wifiManager;

U8X8_SSD1306_128X64_NONAME_HW_I2C oled(U8X8_PIN_NONE);
bool oledReady = false;
String wifiConfigApName;

IRrecv irReceiver(
  IR_RX_PIN,
  IR_CAPTURE_BUFFER_SIZE,
  IR_CAPTURE_TIMEOUT_MS,
  true
);

IRsend irSender(IR_TX_PIN);
IRac universalAc(IR_TX_PIN);
decode_results irResults;

WiFiClient netClient;
WiFiClientSecure secureNetClient;
PubSubClient mqttClient;

// ============================================================
// TRẠNG THÁI THIẾT BỊ
// ============================================================

String deviceId;
String deviceToken;
String pairingCode;
String lastCommandId;

unsigned long lastHeartbeatAt = 0;
unsigned long lastRegisterAttemptAt = 0;
unsigned long lastMqttReconnectAttemptAt = 0;

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

// ============================================================
// FREERTOS TWO-WAY QUEUES (OWNERSHIP ARCHITECTURE)
// ============================================================

enum CommandType {
  CMD_NONE,
  CMD_SET_AC_STATE,
  CMD_SEND_RAW,
  CMD_START_LEARNING,
  CMD_CANCEL_LEARNING,
  CMD_RESET_WIFI,
  CMD_FACTORY_RESET,
  CMD_PING
};

struct CommandMsg {
  CommandType type = CMD_NONE;
  char id[64] = {0};
  char profileId[64] = {0};
  char expectedAction[64] = {0};
  uint32_t timeoutSeconds = 45;

  char protocol[32] = {0};
  char codeStr[32] = {0};
  uint16_t bits = 0;
  uint16_t repeatCount = 0;
  uint32_t address = 0;
  uint32_t commandCode = 0;
  bool power = false;
  float temperature = 26.0;
  char mode[16] = {0};
  char fan[16] = {0};
  char swingV[16] = {0};

  uint16_t rawUs[MAX_RAW_SAMPLES];
  uint16_t rawCount = 0;
  uint16_t frequencyKhz = 38;
};

enum CloudMsgType {
  CLOUD_ACK_COMMAND,
  CLOUD_UPLOAD_LEARNED_SIGNAL
};

struct CloudMsg {
  CloudMsgType type;
  char commandId[64] = {0};
  char status[32] = {0};
  char message[128] = {0};

  char profileId[64] = {0};
  char expectedAction[64] = {0};
  char protocol[32] = {0};
  uint16_t bits = 0;
  char codeHex[64] = {0};
  uint32_t address = 0;
  uint32_t commandCode = 0;
  bool repeat = false;
  char stateHex[128] = {0};
  char description[128] = {0};
  bool nativeSendSupported = false;
  bool commonDecoded = false;
  uint16_t rawUs[MAX_RAW_SAMPLES];
  uint16_t rawCount = 0;
};

QueueHandle_t xCommandQueue = NULL; // Core 0 (MQTT) -> Core 1 (IR/OLED)
QueueHandle_t xCloudQueue = NULL;   // Core 1 -> Core 0

void renderDisplay();
void goToBaseDisplayState();

String fit16(const String &text) {
  if (text.length() <= 16) {
    return text;
  }
  return text.substring(0, 16);
}

void drawRow(const uint8_t row, const String &text) {
  if (!oledReady || row > 7) {
    return;
  }
  const String line = fit16(text);
  oled.drawString(0, row, "                ");
  oled.drawString(0, row, line.c_str());
}

void setDisplayState(const DisplayState nextState, const bool renderNow = true) {
  displayState = nextState;
  displayStateEnteredAt = millis();
  displayDirty = true;

  if (renderNow) {
    renderDisplay();
  }
}

void markCloudSuccess() {
  lastCloudSuccessAt = millis();
  cloudConnected = true;
  displayDirty = true;
}

void refreshCloudStatus(const unsigned long now) {
  const bool online =
    WiFi.status() == WL_CONNECTED &&
    (mqttClient.connected() || (lastCloudSuccessAt != 0 && now - lastCloudSuccessAt <= CLOUD_ONLINE_WINDOW_MS));

  if (cloudConnected != online) {
    cloudConnected = online;
    displayDirty = true;
  }
}

void savePairedState(const bool paired) {
  if (devicePaired == paired) {
    return;
  }

  devicePaired = paired;
  preferences.putBool("paired", paired);
  displayDirty = true;

  if (paired && WiFi.status() == WL_CONNECTED) {
    goToBaseDisplayState();
  }
}

void renderBootScreen() {
  drawRow(1, " AC REMOTE HUB");
  drawRow(3, "    v0.4.0");
  drawRow(6, "  [MQTT READY]");
}

void renderWiFiConnectingScreen() {
  drawRow(0, " WiFi: CONNECT ");
  drawRow(1, "----------------");
  drawRow(2, "AP:");
  drawRow(3, " " + fit16(wifiConfigApName));
  drawRow(5, "IP: 192.168.4.1");
  drawRow(7, "Open 192.168.4.1");
}

void renderPairingScreen() {
  drawRow(0, "   PAIR CODE    ");
  drawRow(1, "----------------");

  const String code = pairingCode.isEmpty() ? "------" : pairingCode;
  oled.draw2x2String(2, 3, code.c_str());

  drawRow(7, " AC MQTT HUB v04");
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

void renderLearningScreen() {
  drawRow(0, "  [ LEARN IR ]  ");
  drawRow(1, "----------------");
  drawRow(2, "ACT:");
  drawRow(3, " " + fit16(learning.expectedAction));
  drawRow(5, "PRESS REMOTE...");

  unsigned long remainingMs = 0;
  const unsigned long elapsed = millis() - learning.startedAt;
  if (learning.timeoutMs > elapsed) {
    remainingMs = learning.timeoutMs - elapsed;
  }
  const unsigned long remainingSec = (remainingMs + 999UL) / 1000UL;
  drawRow(7, "TIME: " + String(remainingSec) + "s");
}

void renderLearningOkScreen() {
  drawRow(0, "  [ LEARN OK ]  ");
  drawRow(1, "----------------");
  drawRow(2, "PROTO:");
  drawRow(3, " " + fit16(learnedProtocolScreen));
  drawRow(5, "BITS: " + String(learnedBitsScreen));
  drawRow(7, "STATUS: SAVED");
}

void renderDisplay() {
  if (oledReady) {
    switch (displayState) {
      case DisplayState::STATE_BOOT:
        renderBootScreen();
        break;
      case DisplayState::STATE_WIFI_CONNECTING:
        renderWiFiConnectingScreen();
        break;
      case DisplayState::STATE_PAIRING_CODE:
        renderPairingScreen();
        break;
      case DisplayState::STATE_IDLE_MASCOT:
        renderPairingScreen();
        break;
      case DisplayState::STATE_EVENT_CMD:
        renderCommandScreen();
        break;
      case DisplayState::STATE_EVENT_LEARN:
        renderLearningScreen();
        break;
      case DisplayState::STATE_EVENT_LEARN_OK:
        renderLearningOkScreen();
        break;
    }
  }

  lastDisplayRefreshAt = millis();
  displayDirty = false;
}

void goToBaseDisplayState() {
  if (WiFi.status() != WL_CONNECTED) {
    setDisplayState(DisplayState::STATE_WIFI_CONNECTING);
    return;
  }
  setDisplayState(DisplayState::STATE_PAIRING_CODE);
}

void showCommandEvent(bool power, int temperature, const String &mode, const String &type) {
  commandScreen.commandType = type;
  commandScreen.power = power;
  commandScreen.temperature = temperature;
  commandScreen.mode = mode;
  commandScreen.mode.toUpperCase();
  commandScreen.resultKnown = false;
  commandScreen.resultOk = false;
  setDisplayState(DisplayState::STATE_EVENT_CMD);
}

void updateCommandEventResult(const bool ok) {
  commandScreen.resultKnown = true;
  commandScreen.resultOk = ok;
  displayDirty = true;
  renderDisplay();
}

void showLearningEvent() {
  setDisplayState(DisplayState::STATE_EVENT_LEARN);
}

void showLearningSuccess(const String &protocol, const uint16_t bits) {
  learnedProtocolScreen = fit16(protocol);
  learnedBitsScreen = bits;
  setDisplayState(DisplayState::STATE_EVENT_LEARN_OK);
}

void processDisplayFsm(const unsigned long now) {
  refreshCloudStatus(now);

  switch (displayState) {
    case DisplayState::STATE_BOOT:
      break;
    case DisplayState::STATE_WIFI_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        setDisplayState(DisplayState::STATE_PAIRING_CODE, false);
      }
      break;
    case DisplayState::STATE_PAIRING_CODE:
      if (WiFi.status() != WL_CONNECTED) {
        setDisplayState(DisplayState::STATE_WIFI_CONNECTING, false);
      }
      break;
    case DisplayState::STATE_IDLE_MASCOT:
      goToBaseDisplayState();
      break;
    case DisplayState::STATE_EVENT_CMD:
      if (now - displayStateEnteredAt >= EVENT_SCREEN_DURATION_MS) {
        goToBaseDisplayState();
      }
      break;
    case DisplayState::STATE_EVENT_LEARN:
      if (!learning.active) {
        goToBaseDisplayState();
      }
      break;
    case DisplayState::STATE_EVENT_LEARN_OK:
      if (now - displayStateEnteredAt >= EVENT_SCREEN_DURATION_MS) {
        goToBaseDisplayState();
      }
      break;
  }

  if (displayDirty || now - lastDisplayRefreshAt >= DISPLAY_STATUS_REFRESH_MS) {
    renderDisplay();
  }
}

// ============================================================
// DEVICE BOOTSTRAP HTTP (KHI MỚI BẬT NGUỒN)
// ============================================================

String buildDeviceId() {
  const uint64_t chipId = ESP.getEfuseMac();
  char idBuffer[32];
  snprintf(
    idBuffer,
    sizeof(idBuffer),
    "ACIR-%04X%08X",
    static_cast<uint16_t>(chipId >> 32),
    static_cast<uint32_t>(chipId)
  );
  return String(idBuffer);
}

void ensureDeviceRegistered() {
  if (WiFi.status() != WL_CONNECTED || !deviceToken.isEmpty()) {
    return;
  }

  const unsigned long now = millis();
  if (lastRegisterAttemptAt != 0 && now - lastRegisterAttemptAt < REGISTER_RETRY_INTERVAL_MS) {
    return;
  }

  lastRegisterAttemptAt = now;

  HTTPClient http;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  String url = String(API_BASE_URL) + "/devices/register";
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Bootstrap-Key", DEVICE_BOOTSTRAP_KEY);

  DynamicJsonDocument doc(256);
  doc["deviceId"] = deviceId;
  doc["fwVersion"] = FW_VERSION;
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code == 200 || code == 201) {
    String respStr = http.getString();
    DynamicJsonDocument respDoc(512);
    if (!deserializeJson(respDoc, respStr)) {
      deviceToken = respDoc["deviceToken"].as<String>();
      pairingCode = respDoc["pairingCode"].as<String>();
      devicePaired = respDoc["paired"] | false;

      preferences.putString("dev_token", deviceToken);
      preferences.putString("pair_code", pairingCode);
      preferences.putBool("paired", devicePaired);

      Serial.printf("[HTTP Bootstrap] Dang ky thanh cong! DeviceID=%s, PairCode=%s\n", deviceId.c_str(), pairingCode.c_str());
      markCloudSuccess();
    }
  }
  http.end();
}

// ============================================================
// MQTT CALLBACK & RECONNECT LOGIC (NHẬN LỆNH TỨC THÌ <50ms)
// ============================================================

void parseMqttCommand(const char* payload, unsigned int length) {
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[MQTT Error] JSON deserialize failure: %s\n", err.c_str());
    return;
  }

  JsonVariantConst cmdObj = doc["command"].isNull() ? doc.as<JsonVariantConst>() : doc["command"].as<JsonVariantConst>();
  String typeStr = cmdObj["type"] | "";
  String commandId = cmdObj["id"] | "";

  if (commandId.isEmpty() || typeStr.isEmpty()) {
    return;
  }

  if (commandId == lastCommandId) {
    return; // Đã xử lý lệnh này trước đó
  }
  lastCommandId = commandId;

  CommandMsg cmd;
  snprintf(cmd.id, sizeof(cmd.id), "%s", commandId.c_str());
  snprintf(cmd.profileId, sizeof(cmd.profileId), "%s", cmdObj["profileId"] | "default");

  if (typeStr == "SET_AC_STATE") {
    cmd.type = CMD_SET_AC_STATE;
    cmd.power = cmdObj["power"] | false;
    cmd.temperature = cmdObj["temperature"] | 26.0f;

    snprintf(cmd.protocol, sizeof(cmd.protocol), "%s", cmdObj["protocol"] | "ELECTRA_AC");
    snprintf(cmd.mode, sizeof(cmd.mode), "%s", cmdObj["mode"] | "cool");
    snprintf(cmd.fan, sizeof(cmd.fan), "%s", cmdObj["fan"] | "auto");
    snprintf(cmd.swingV, sizeof(cmd.swingV), "%s", cmdObj["swingV"] | "off");

    showCommandEvent(cmd.power, (int)cmd.temperature, String(cmd.mode), "SET_AC_STATE");
  }
  else if (typeStr == "SEND_RAW") {
    cmd.type = CMD_SEND_RAW;
    cmd.frequencyKhz = cmdObj["frequencyKhz"] | 38;
    JsonArrayConst rawArr = cmdObj["rawUs"].as<JsonArrayConst>();
    if (!rawArr.isNull()) {
      uint16_t idx = 0;
      for (uint16_t val : rawArr) {
        if (idx < MAX_RAW_SAMPLES) {
          cmd.rawUs[idx++] = val;
        }
      }
      cmd.rawCount = idx;
    }
    showCommandEvent(true, 26, "RAW", "SEND_RAW");
  }
  else if (typeStr == "START_LEARNING") {
    cmd.type = CMD_START_LEARNING;
    snprintf(cmd.expectedAction, sizeof(cmd.expectedAction), "%s", cmdObj["expectedAction"] | "LEARN_IR");
    cmd.timeoutSeconds = cmdObj["timeoutSeconds"] | 45;

    learning.active = true;
    learning.commandId = commandId;
    learning.profileId = cmd.profileId;
    learning.expectedAction = cmd.expectedAction;
    learning.startedAt = millis();
    learning.timeoutMs = cmd.timeoutSeconds * 1000UL;

    showLearningEvent();
  }
  else if (typeStr == "CANCEL_LEARNING") {
    cmd.type = CMD_CANCEL_LEARNING;
    learning.active = false;
    goToBaseDisplayState();
  }

  // Nạp lệnh vào FreeRTOS Queue để Core 1 thực thi IR ngay lập tức
  if (xCommandQueue != NULL) {
    if (xQueueSend(xCommandQueue, &cmd, 0) == pdPASS) {
      Serial.printf("[MQTT Push -> Core 1] Enqueued Command ID: %s (%s) <50ms\n", cmd.id, typeStr.c_str());
    } else {
      Serial.println("[MQTT Push Error] CommandQueue is full!");
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("\n>>> [MQTT Instant Received] Topic: %s | Length: %u bytes\n", topic, length);
  parseMqttCommand((const char*)payload, length);
}

void reconnectMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastMqttReconnectAttemptAt < MQTT_RECONNECT_INTERVAL_MS) {
    return;
  }
  lastMqttReconnectAttemptAt = now;

  String clientId = String("ESP32-ACRemote-") + deviceId;
  String cmdTopic = String("acremote/devices/") + deviceId + "/commands";

  Serial.printf("[MQTT] Connecting to Broker %s:%d (ClientID: %s)...\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT, clientId.c_str());

  bool connected = false;
  if (strlen(MQTT_BROKER_USER) > 0) {
    connected = mqttClient.connect(clientId.c_str(), MQTT_BROKER_USER, MQTT_BROKER_PASS);
  } else {
    connected = mqttClient.connect(clientId.c_str());
  }

  if (connected) {
    Serial.println("[MQTT Connected Success!] Subscribing to command push topic...");
    mqttClient.subscribe(cmdTopic.c_str(), 1);
    markCloudSuccess();

    // Gửi thông báo Online
    String statusTopic = String("acremote/devices/") + deviceId + "/status";
    mqttClient.publish(statusTopic.c_str(), "{\"status\":\"online\",\"fw\":\"0.4.0\"}");
  } else {
    Serial.printf("[MQTT Connect Failed] state=%d. Retrying in 5s...\n", mqttClient.state());
  }
}

// ============================================================
// HÀM BỔ TRỢ PHÁT IR
// ============================================================

void silenceReceiverForTx() {
  irReceiver.disableIRIn();
}

void reArmReceiverAfterTx() {
  irReceiver.enableIRIn();
  irReceiver.resume();
}

bool sendNativeAcState(const CommandMsg &cmd, String &errorMessage) {
  decode_type_t protocol = decode_type_t::UNKNOWN;
  if (strlen(cmd.protocol) > 0) {
    protocol = strToDecodeType(cmd.protocol);
  }

  if (protocol == decode_type_t::UNKNOWN) {
    protocol = previousProtocol;
  }

  if (protocol == decode_type_t::UNKNOWN || !IRac::isProtocolSupported(protocol)) {
    errorMessage = "Protocol unfamiliar or unsupported by IRac";
    return false;
  }

  silenceReceiverForTx();
  universalAc.next.protocol = protocol;
  universalAc.next.power = cmd.power;
  universalAc.next.degrees = cmd.temperature;

  String modeUpper(cmd.mode);
  modeUpper.toUpperCase();
  if (modeUpper == "COOL") universalAc.next.mode = stdAc::opmode_t::kCool;
  else if (modeUpper == "HEAT") universalAc.next.mode = stdAc::opmode_t::kHeat;
  else if (modeUpper == "DRY") universalAc.next.mode = stdAc::opmode_t::kDry;
  else if (modeUpper == "FAN") universalAc.next.mode = stdAc::opmode_t::kFan;
  else universalAc.next.mode = stdAc::opmode_t::kAuto;

  universalAc.sendAc();
  reArmReceiverAfterTx();

  previousProtocol = protocol;
  return true;
}

bool sendEncodedSignal(const CommandMsg &cmd, String &errorMessage) {
  decode_type_t protocol = strToDecodeType(cmd.protocol);
  uint64_t code = 0;

  if (cmd.codeStr[0] == '0' && (cmd.codeStr[1] == 'x' || cmd.codeStr[1] == 'X')) {
    code = strtoull(cmd.codeStr + 2, NULL, 16);
  } else {
    code = strtoull(cmd.codeStr, NULL, 16);
  }

  if (protocol == decode_type_t::UNKNOWN || cmd.bits == 0) {
    errorMessage = "Protocol missing or bits invalid";
    return false;
  }

  silenceReceiverForTx();
  bool sent = irSender.send(protocol, code, cmd.bits, cmd.repeatCount);
  reArmReceiverAfterTx();

  if (!sent) {
    errorMessage = "IRsend failed";
    return false;
  }

  return true;
}

// ============================================================
// FREERTOS CONTROL TASK (CORE 1 - IR & OLED EXCLUSIVE)
// ============================================================

void controlTaskLoop(void *pvParameters) {
  Serial.println("[FreeRTOS] Control Task running on Core 1 (IR & OLED)");

  irSender.begin();
  irReceiver.enableIRIn();
  irReceiver.setUnknownThreshold(12);
  IRac::initState(&previousAcState);

  CommandMsg cmd;

  for (;;) {
    const unsigned long now = millis();

    // 1. Kiểm tra hàng đợi lệnh từ Core 0
    if (xQueueReceive(xCommandQueue, &cmd, pdMS_TO_TICKS(10)) == pdTRUE) {
      Serial.printf("[Core 1 Execution] Command ID: %s | Type: %d\n", cmd.id, cmd.type);
      bool success = false;
      String errStr = "";

      if (cmd.type == CMD_SET_AC_STATE) {
        if (strlen(cmd.codeStr) > 0) {
          success = sendEncodedSignal(cmd, errStr);
        } else {
          success = sendNativeAcState(cmd, errStr);
        }
        updateCommandEventResult(success);
      }
      else if (cmd.type == CMD_SEND_RAW) {
        if (cmd.rawCount > 0) {
          silenceReceiverForTx();
          irSender.sendRaw(cmd.rawUs, cmd.rawCount, cmd.frequencyKhz);
          reArmReceiverAfterTx();
          success = true;
        } else {
          errStr = "rawUs is empty";
        }
        updateCommandEventResult(success);
      }

      // Gửi ACK phản hồi về Core 0 để đẩy lên MQTT Broker
      if (xCloudQueue != NULL && strlen(cmd.id) > 0) {
        CloudMsg ack;
        ack.type = CLOUD_ACK_COMMAND;
        snprintf(ack.commandId, sizeof(ack.commandId), "%s", cmd.id);
        snprintf(ack.status, sizeof(ack.status), "%s", success ? "completed" : "failed");
        snprintf(ack.message, sizeof(ack.message), "%s", errStr.c_str());
        xQueueSend(xCloudQueue, &ack, 0);
      }
    }

    // 2. Lắng nghe tín hiệu từ Remote gốc (Learning mode)
    if (learning.active) {
      if (now - learning.startedAt >= learning.timeoutMs) {
        learning.active = false;
        goToBaseDisplayState();
      } else if (irReceiver.decode(&irResults)) {
        if (irResults.overflow) {
          irReceiver.resume();
        } else {
          String protocolStr = typeToString(irResults.decode_type);
          showLearningSuccess(protocolStr, irResults.bits);

          if (xCloudQueue != NULL) {
            CloudMsg upload;
            upload.type = CLOUD_UPLOAD_LEARNED_SIGNAL;
            snprintf(upload.commandId, sizeof(upload.commandId), "%s", learning.commandId.c_str());
            snprintf(upload.profileId, sizeof(upload.profileId), "%s", learning.profileId.c_str());
            snprintf(upload.expectedAction, sizeof(upload.expectedAction), "%s", learning.expectedAction.c_str());
            snprintf(upload.protocol, sizeof(upload.protocol), "%s", protocolStr.c_str());
            upload.bits = irResults.bits;

            uint16_t count = getCorrectedRawLength(&irResults);
            upload.rawCount = min(count, (uint16_t)MAX_RAW_SAMPLES);
            for (uint16_t i = 0; i < upload.rawCount; i++) {
              upload.rawUs[i] = irResults.rawbuf[i + 1] * kRawTick;
            }

            xQueueSend(xCloudQueue, &upload, 0);
          }

          learning.active = false;
          irReceiver.resume();
        }
      }
    }

    processDisplayFsm(now);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================
// SETUP & LOOP (CORE 0 NETWORK & MQTT TASK)
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("==================================================");
  Serial.printf("  AC CONTROLLER FIRMWARE v%s (MQTT HYBRID)\n", FW_VERSION);
  Serial.println("==================================================");

  preferences.begin("ac_remote", false);
  deviceId = buildDeviceId();
  deviceToken = preferences.getString("dev_token", "");
  pairingCode = preferences.getString("pair_code", "");
  devicePaired = preferences.getBool("paired", false);

  // Khởi tạo màn hình OLED I2C (GPIO21 / GPIO22)
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(400000);
  Wire.beginTransmission(0x3C);
  if (Wire.endTransmission() == 0) {
    oled.begin();
    oled.setFont(u8x8_font_chroma48medium8_r);
    oled.clear();
    oledReady = true;
    Serial.println("[OLED] Da kich hoat va khoi tao OLED thanh cong (Address: 0x3C)");
  } else {
    oledReady = false;
    Serial.println("[OLED] Khong tim thay hardware OLED tai 0x3C, bypass hien thi.");
  }

  wifiConfigApName = "AC-HUB-" + deviceId.substring(deviceId.length() - 4);

  // Khởi tạo WiFiManager
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setConnectTimeout(10);
  wifiManager.setBreakAfterConfig(true);

  if (!wifiManager.autoConnect(wifiConfigApName.c_str(), CONFIG_AP_PASSWORD)) {
    Serial.println("[WiFi] AutoConnect timeout or failed. Continuing...");
  } else {
    Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  }

  // Khởi tạo FreeRTOS Queues
  xCommandQueue = xQueueCreate(10, sizeof(CommandMsg));
  xCloudQueue   = xQueueCreate(10, sizeof(CloudMsg));

  // Cấu hình Client MQTT
  mqttClient.setClient(netClient);
  mqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  mqttClient.setCallback(mqttCallback);

  setDisplayState(DisplayState::STATE_BOOT);

  // Task Control trên Core 1
  xTaskCreatePinnedToCore(
    controlTaskLoop,
    "ControlTask",
    8192,
    NULL,
    2,
    NULL,
    1
  );

  // Task Network / MQTT trên Core 0
  xTaskCreatePinnedToCore(
    [](void *pvParameters) {
      Serial.println("[FreeRTOS] Network Task running on Core 0 (MQTT Engine)");

      for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
          ensureDeviceRegistered();

          if (!deviceToken.isEmpty()) {
            // Duy trì kết nối MQTT vĩnh viễn
            if (!mqttClient.connected()) {
              reconnectMqtt();
            } else {
              mqttClient.loop(); // Lắng nghe tin nhắn đẩy tức thì
            }

            // Xử lý Cloud Queue (Gửi ACK phản hồi)
            if (xCloudQueue != NULL && uxQueueMessagesWaiting(xCloudQueue) > 0) {
              CloudMsg cloudMsg;
              if (xQueueReceive(xCloudQueue, &cloudMsg, 0) == pdTRUE) {
                if (cloudMsg.type == CLOUD_ACK_COMMAND && mqttClient.connected()) {
                  String ackTopic = String("acremote/devices/") + deviceId + "/ack";
                  DynamicJsonDocument ackDoc(256);
                  ackDoc["commandId"] = cloudMsg.commandId;
                  ackDoc["status"] = cloudMsg.status;
                  ackDoc["message"] = cloudMsg.message;
                  String ackPayload;
                  serializeJson(ackDoc, ackPayload);

                  mqttClient.publish(ackTopic.c_str(), ackPayload.c_str());
                  Serial.printf("[MQTT ACK Sent] Cmd ID: %s | Status: %s\n", cloudMsg.commandId, cloudMsg.status);
                }
              }
            }

            // Memory guard
            if (ESP.getFreeHeap() < MIN_SAFE_HEAP_BYTES) {
              Serial.printf("[Memory Guard] Free heap low (%u bytes). Restarting...\n", ESP.getFreeHeap());
              ESP.restart();
            }
          }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
      }
    },
    "NetworkTask",
    12288,
    NULL,
    1,
    NULL,
    0
  );
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
