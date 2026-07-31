/*
  AC Controller Firmware v0.2 - OLED FSM
  Board: ESP32 Dev Module

  Chức năng:
  1) WiFiManager: nếu chưa có Wi-Fi hoặc chuyển địa điểm, ESP32 mở AP cấu hình.
  2) Tạo Device ID từ eFuse MAC.
  3) Đăng ký thiết bị với backend, nhận device token + pairing code.
  4) Gửi heartbeat và polling lệnh từ backend.
  5) START_LEARNING: nhận remote gốc, tự nhận protocol, giải mã common A/C state,
     gửi protocol/state/raw timing lên backend và gắn với profileId.
  6) SET_AC_STATE: nhận trạng thái từ web và phát IR bằng IRac cho protocol hỗ trợ.
  7) SEND_RAW: phát raw timing cho profile/lệnh chưa có native encoder.

  Phần cứng mặc định:
  - HX1838 OUT -> GPIO27
  - LED IR qua 2N2222 -> GPIO4

  Thư viện cần cài:
  - WiFiManager by tzapu
  - ArduinoJson
  - IRremoteESP8266
  - U8g2 by oliver

  LƯU Ý:
  - Thay API_BASE_URL và DEVICE_BOOTSTRAP_KEY trước khi dùng.
  - HTTPS hiện dùng setInsecure() để thử nghiệm. Bản sản phẩm phải dùng CA certificate.
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
#include <new>

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRac.h>
#include <IRutils.h>

// ============================================================
// CẤU HÌNH PHẢI SỬA
// ============================================================

static const char *FW_VERSION = "0.2.0";

// Ví dụ local backend: http://192.168.1.10:3000/api/v1
// Ví dụ cloud backend: https://api.tenmiencuaban.vn/api/v1
static const char *API_BASE_URL = "https://accontrollerremote.pythonanywhere.com/api/v1";

// Khóa bootstrap chỉ dùng để backend cho phép một thiết bị mới đăng ký.
// Bản sản phẩm nên dùng khóa riêng theo từng thiết bị, không dùng chung toàn bộ thiết bị.
static const char *DEVICE_BOOTSTRAP_KEY = "CHANGE_ME_BOOTSTRAP_KEY";

// AP WiFiManager phát mạng mở (không cần mật khẩu).
// Captive portal DNS sẽ tự redirect browser về trang cấu hình.
static const char *CONFIG_AP_PASSWORD = ""; // Chuỗi rỗng = Open AP

// ============================================================
// CẤU HÌNH PHẦN CỨNG
// ============================================================

static const uint16_t IR_RX_PIN = 27;
static const uint16_t IR_TX_PIN = 4;

// OLED 0.96 inch SSD1306 I2C 128x64.
// GPIO25/26 khong xung dot voi IR RX/TX trong firmware nay.
static const uint8_t OLED_SDA_PIN = 21;
static const uint8_t OLED_SCL_PIN = 22;
static const uint8_t OLED_I2C_ADDRESS = 0x3C;

// Remote điều hòa có frame dài.
static const uint16_t IR_CAPTURE_BUFFER_SIZE = 2048;
static const uint8_t IR_CAPTURE_TIMEOUT_MS = 80;

// Giới hạn số timing raw gửi lên server trong một event.
// Frame Electra 104-bit của bạn có 211 timing nên nằm trong giới hạn này.
static const uint16_t MAX_RAW_TIMINGS_UPLOAD = 700;

// ============================================================
// CHU KỲ HỆ THỐNG
// ============================================================

static const unsigned long COMMAND_POLL_INTERVAL_MS = 2000;
static const unsigned long HEARTBEAT_INTERVAL_MS = 30000;
static const unsigned long REGISTER_RETRY_INTERVAL_MS = 10000;
static const unsigned long WIFI_RESTART_AFTER_MS = 60000;
static const unsigned long DEFAULT_LEARNING_TIMEOUT_MS = 45000;
static const unsigned long BOOT_SCREEN_DURATION_MS = 2000;
static const unsigned long EVENT_SCREEN_DURATION_MS = 3000;
static const unsigned long DISPLAY_STATUS_REFRESH_MS = 1000;
static const unsigned long CLOUD_ONLINE_WINDOW_MS = 15000;
static const unsigned long WIFI_INITIAL_CONNECT_WINDOW_MS = 10000;

// ============================================================
// ĐỐI TƯỢNG TOÀN CỤC
// ============================================================

Preferences preferences;
WiFiManager wifiManager;

// U8x8 text mode: nhe hon full-framebuffer, phu hop firmware IR lon.
U8X8_SSD1306_128X64_NONAME_HW_I2C oled(U8X8_PIN_NONE);
bool oledReady = false;
bool wifiManagerStarted = false;
bool wifiPortalStarted = false;
bool wifiWasConnected = false;
unsigned long wifiInitialConnectStartedAt = 0;
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

// ============================================================
// TRẠNG THÁI THIẾT BỊ
// ============================================================

String deviceId;
String deviceToken;
String pairingCode;
String lastCommandId;

unsigned long lastCommandPollAt = 0;
unsigned long lastHeartbeatAt = 0;
unsigned long lastRegisterAttemptAt = 0;
unsigned long wifiDisconnectedSince = 0;

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
    lastCloudSuccessAt != 0 &&
    now - lastCloudSuccessAt <= CLOUD_ONLINE_WINDOW_MS;

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

bool extractPairedFlag(const JsonVariantConst root, bool &found) {
  found = false;

  const JsonVariantConst directPaired = root["paired"];
  if (directPaired.is<bool>()) {
    found = true;
    return directPaired.as<bool>();
  }

  const JsonVariantConst directIsPaired = root["isPaired"];
  if (directIsPaired.is<bool>()) {
    found = true;
    return directIsPaired.as<bool>();
  }

  const JsonVariantConst deviceNode = root["device"];
  if (!deviceNode.isNull()) {
    const JsonVariantConst nested = deviceNode["paired"];
    if (nested.is<bool>()) {
      found = true;
      return nested.as<bool>();
    }
  }

  const JsonVariantConst dataNode = root["data"];
  if (!dataNode.isNull()) {
    const JsonVariantConst nested = dataNode["paired"];
    if (nested.is<bool>()) {
      found = true;
      return nested.as<bool>();
    }
  }

  return false;
}

bool applyPairingStateFromJson(const String &jsonBody) {
  if (jsonBody.isEmpty()) {
    return false;
  }

  DynamicJsonDocument doc(1536);
  if (deserializeJson(doc, jsonBody)) {
    return false;
  }

  bool found = false;
  const bool paired = extractPairedFlag(doc.as<JsonVariantConst>(), found);
  if (found) {
    savePairedState(paired);
  }

  return found;
}

void drawStatusRow() {
  char line[17];
  const int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(
      line,
      sizeof(line),
      "W:%ddBm C:%s",
      rssi,
      cloudConnected ? "ON" : "OFF"
    );
  } else {
    snprintf(line, sizeof(line), "W:OFF C:OFF");
  }

  drawRow(0, line);
  drawRow(1, "----------------");
}

void renderBootScreen() {
  drawRow(0, "================");
  drawRow(2, " AC REMOTE HUB");
  drawRow(3, "     v0.2.0");
  drawRow(5, "================");
  drawRow(7, "Khoi dong...");
}

void renderWiFiConnectingScreen() {
  drawRow(0, "WiFi:DANG KET NOI");
  drawRow(1, "----------------");
  drawRow(2, " [CONNECTING...]");
  drawRow(4, wifiConfigApName);
  drawRow(6, "Mo 192.168.4.1");
  drawRow(7, "de cau hinh WiFi");
}

void renderPairingScreen() {
  drawRow(0, String("WiFi:OK C:") + (cloudConnected ? "ON" : "OFF"));
  drawRow(1, "----------------");
  drawRow(2, " MA GHEP NOI WEB");

  const String code = pairingCode.isEmpty() ? "------" : pairingCode;
  oled.draw2x2String(2, 3, code.c_str());

  drawRow(6, "Nhap ma tren Web");
  drawRow(7, "de kich hoat");
}

void renderIdleScreen() {
  drawStatusRow();
  oled.draw2x2String(3, 3, "(^_^)");
  drawRow(6, " READY/STANDBY");
}

void renderCommandScreen() {
  drawRow(0, "NHAN LENH WEB");
  drawRow(1, "----------------");
  drawRow(2, String("TRANG THAI:") + (commandScreen.power ? "BAT" : "TAT"));
  drawRow(3, "NHIET DO:" + String(commandScreen.temperature) + " C");
  drawRow(4, "CHE DO:" + commandScreen.mode);

  if (!commandScreen.resultKnown) {
    drawRow(6, "PHAT IR:DANG GUI");
  } else {
    drawRow(6, String("PHAT IR:") + (commandScreen.resultOk ? "OK" : "LOI"));
  }
}

void renderLearningScreen() {
  drawRow(0, "DANG HOC REMOTE");
  drawRow(1, "----------------");
  drawRow(2, "Chia remote vao");
  drawRow(3, "mat doc va BAM");
  drawRow(4, "NUT CAN HOC!");

  unsigned long remainingMs = 0;
  const unsigned long elapsed = millis() - learning.startedAt;
  if (learning.timeoutMs > elapsed) {
    remainingMs = learning.timeoutMs - elapsed;
  }
  const unsigned long remainingSec = (remainingMs + 999UL) / 1000UL;
  drawRow(6, "Dem nguoc:" + String(remainingSec) + "s");
  drawRow(7, fit16(learning.expectedAction));
}

void renderLearningOkScreen() {
  drawRow(0, "HOC IR THANH CONG");
  drawRow(1, "----------------");
  drawRow(3, "Protocol:");
  drawRow(4, learnedProtocolScreen);
  drawRow(5, "Bits:" + String(learnedBitsScreen));
  drawRow(7, "Da luu len Cloud");
}

void renderDisplay() {
  // [BỎ OLED TẠM THỜI] Chuyển thông tin trạng thái & ID ra Serial Monitor
  static DisplayState lastStatePrinted = static_cast<DisplayState>(255);
  static bool lastCloudPrinted = false;
  static uint8_t lastWifiPrinted = 255;
  static String lastCommandTypePrinted = "";
  static bool lastCommandPowerPrinted = false;
  static int lastCommandTempPrinted = 0;
  static String lastCommandModePrinted = "";
  static bool lastCommandKnownPrinted = false;
  static bool lastCommandOkPrinted = false;
  static String lastLearnedProtocolPrinted = "";
  static uint16_t lastLearnedBitsPrinted = 0;

  const uint8_t currentWifi = WiFi.status();

  bool needPrint = (displayState != lastStatePrinted) ||
                   (cloudConnected != lastCloudPrinted) ||
                   (currentWifi != lastWifiPrinted);

  if (displayState == DisplayState::STATE_EVENT_CMD) {
    if (commandScreen.commandType != lastCommandTypePrinted ||
        commandScreen.power != lastCommandPowerPrinted ||
        commandScreen.temperature != lastCommandTempPrinted ||
        commandScreen.mode != lastCommandModePrinted ||
        commandScreen.resultKnown != lastCommandKnownPrinted ||
        commandScreen.resultOk != lastCommandOkPrinted) {
      needPrint = true;
    }
  } else if (displayState == DisplayState::STATE_EVENT_LEARN_OK) {
    if (learnedProtocolScreen != lastLearnedProtocolPrinted ||
        learnedBitsScreen != lastLearnedBitsPrinted) {
      needPrint = true;
    }
  }

  if (needPrint) {
    lastStatePrinted = displayState;
    lastCloudPrinted = cloudConnected;
    lastWifiPrinted = currentWifi;
    lastCommandTypePrinted = commandScreen.commandType;
    lastCommandPowerPrinted = commandScreen.power;
    lastCommandTempPrinted = commandScreen.temperature;
    lastCommandModePrinted = commandScreen.mode;
    lastCommandKnownPrinted = commandScreen.resultKnown;
    lastCommandOkPrinted = commandScreen.resultOk;
    lastLearnedProtocolPrinted = learnedProtocolScreen;
    lastLearnedBitsPrinted = learnedBitsScreen;

    Serial.println();
    Serial.printf(">>> [STATUS MONITOR] Device ID: %s | Pairing Code: %s | State: ",
                  deviceId.c_str(),
                  pairingCode.isEmpty() ? "------" : pairingCode.c_str());
    switch (displayState) {
      case DisplayState::STATE_BOOT:
        Serial.println("STATE_BOOT (Khoi dong system)");
        break;
      case DisplayState::STATE_WIFI_CONNECTING:
        Serial.printf("STATE_WIFI_CONNECTING (AP: %s | URL: 192.168.4.1)\n", wifiConfigApName.c_str());
        break;
      case DisplayState::STATE_PAIRING_CODE:
        Serial.printf("STATE_PAIRING_CODE (Ma ghep noi web: %s)\n", pairingCode.isEmpty() ? "------" : pairingCode.c_str());
        break;
      case DisplayState::STATE_IDLE_MASCOT:
        Serial.printf("STATE_IDLE_STANDBY (WiFi: %s | Cloud: %s)\n",
                      WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED",
                      cloudConnected ? "ONLINE" : "OFFLINE");
        break;
      case DisplayState::STATE_EVENT_CMD:
        Serial.printf("STATE_EVENT_CMD (Type: %s | Power: %s | Temp: %d C | Mode: %s | Result: %s)\n",
                      commandScreen.commandType.c_str(),
                      commandScreen.power ? "BAT" : "TAT",
                      commandScreen.temperature,
                      commandScreen.mode.c_str(),
                      !commandScreen.resultKnown ? "SENDING..." : (commandScreen.resultOk ? "OK" : "ERROR"));
        break;
      case DisplayState::STATE_EVENT_LEARN:
        Serial.printf("STATE_EVENT_LEARN (Action: %s | Dang cho Remote goc...)\n", learning.expectedAction.c_str());
        break;
      case DisplayState::STATE_EVENT_LEARN_OK:
        Serial.printf("STATE_EVENT_LEARN_OK (Protocol: %s | Bits: %d)\n", learnedProtocolScreen.c_str(), learnedBitsScreen);
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

  if (!devicePaired) {
    setDisplayState(DisplayState::STATE_PAIRING_CODE);
    return;
  }

  setDisplayState(DisplayState::STATE_IDLE_MASCOT);
}

void showCommandEvent(JsonObject command, const String &type) {
  commandScreen.commandType = type;
  commandScreen.power = command["power"] | false;
  commandScreen.temperature = static_cast<int>(command["temperature"] | 26.0);
  commandScreen.mode = String(command["mode"] | (type == "SEND_RAW" ? "RAW" : "AUTO"));
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
        if (!deviceToken.isEmpty() && !devicePaired) {
          setDisplayState(DisplayState::STATE_PAIRING_CODE, false);
        } else if (devicePaired) {
          setDisplayState(DisplayState::STATE_IDLE_MASCOT, false);
        }
      }
      break;

    case DisplayState::STATE_PAIRING_CODE:
      if (WiFi.status() != WL_CONNECTED) {
        setDisplayState(DisplayState::STATE_WIFI_CONNECTING, false);
      } else if (devicePaired) {
        setDisplayState(DisplayState::STATE_IDLE_MASCOT, false);
      }
      break;

    case DisplayState::STATE_IDLE_MASCOT:
      if (WiFi.status() != WL_CONNECTED) {
        setDisplayState(DisplayState::STATE_WIFI_CONNECTING, false);
      } else if (!devicePaired) {
        setDisplayState(DisplayState::STATE_PAIRING_CODE, false);
      }
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

  unsigned long refreshInterval = DISPLAY_STATUS_REFRESH_MS;
  if (displayState == DisplayState::STATE_EVENT_LEARN) {
    refreshInterval = 250;
  }

  if (displayDirty || now - lastDisplayRefreshAt >= refreshInterval) {
    renderDisplay();
  }
}

// ============================================================
// HTTP
// ============================================================

struct HttpResponse {
  int code = -1;
  String body;
  String error;
};

String apiUrl(const String &path) {
  String base(API_BASE_URL);
  while (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }

  if (path.startsWith("/")) {
    return base + path;
  }

  return base + "/" + path;
}

HttpResponse executeHttpJson(
  const String &method,
  const String &url,
  const String &payload,
  const bool useDeviceToken,
  const bool useBootstrapKey = false
) {
  HttpResponse response;
  HTTPClient http;

  // Hai client phai song den het request. Code cu khai bao secureClient
  // ben trong khoi if, nen doi tuong bi huy truoc http.GET()/POST().
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  bool begun = false;

  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    begun = http.begin(secureClient, url);
  } else {
    begun = http.begin(plainClient, url);
  }

  if (!begun) {
    response.error = "HTTP begin failed";
    return response;
  }

  http.setConnectTimeout(5000);
  http.setTimeout(10000);
  http.setReuse(false);
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Id", deviceId);

  if (useDeviceToken && !deviceToken.isEmpty()) {
    http.addHeader("Authorization", "Bearer " + deviceToken);
  }

  if (useBootstrapKey) {
    http.addHeader("X-Device-Bootstrap-Key", DEVICE_BOOTSTRAP_KEY);
  }

  if (method == "GET") {
    response.code = http.GET();
  } else if (method == "POST") {
    response.code = http.POST(payload);
  } else {
    response.error = "Unsupported HTTP method";
    http.end();
    return response;
  }

  if (response.code > 0) {
    response.body = http.getString();
  } else {
    response.error = http.errorToString(response.code);
  }

  if (response.code >= 200 && response.code < 300) {
    markCloudSuccess();
  }

  http.end();
  return response;
}

HttpResponse httpGetJson(const String &path, const bool auth = true) {
  return executeHttpJson("GET", apiUrl(path), "", auth, false);
}

HttpResponse httpPostJson(
  const String &path,
  const String &payload,
  const bool auth = true,
  const bool bootstrap = false
) {
  return executeHttpJson("POST", apiUrl(path), payload, auth, bootstrap);
}

// ============================================================
// DEVICE ID + NVS
// ============================================================

String buildDeviceId() {
  const uint64_t chipId = ESP.getEfuseMac();
  char idBuffer[32];

  // Lấy 48 bit MAC để tạo ID ổn định theo chip.
  snprintf(
    idBuffer,
    sizeof(idBuffer),
    "ACIR-%04X%08X",
    static_cast<uint16_t>(chipId >> 32),
    static_cast<uint32_t>(chipId)
  );

  return String(idBuffer);
}

void loadPersistentState() {
  preferences.begin("ac-controller", false);

  deviceToken = preferences.getString("token", "");
  pairingCode = preferences.getString("pairCode", "");
  lastCommandId = preferences.getString("lastCmd", "");
  devicePaired = preferences.getBool("paired", false);
}

void saveDeviceToken(const String &token) {
  deviceToken = token;
  preferences.putString("token", token);
}

void clearDeviceToken() {
  deviceToken = "";
  pairingCode = "";
  preferences.remove("token");
  preferences.remove("pairCode");
  preferences.putBool("paired", false);
  devicePaired = false;
  cloudConnected = false;
  displayDirty = true;
}

// ============================================================
// WIFI MANAGER - NON-BLOCKING
// ============================================================

void beginWiFiManagerNonBlocking() {
  if (wifiManagerStarted) {
    return;
  }

  wifiManagerStarted = true;
  wifiConfigApName = "AC-Remote-" + deviceId.substring(deviceId.length() - 6);

  wifiManager.setAPCallback([](WiFiManager *manager) {
    wifiPortalStarted = true;
    Serial.println();
    Serial.println("========== WIFI SETUP MODE ==========");
    Serial.print("AP: ");
    Serial.println(manager->getConfigPortalSSID());
    Serial.print("IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.println("Mo 192.168.4.1 de cau hinh Wi-Fi.");
    Serial.println("=====================================");
    displayDirty = true;
  });

  wifiManager.setConnectTimeout(20);
  wifiManager.setConfigPortalTimeout(0);
  wifiManager.setConfigPortalBlocking(false);

  // Ket noi credential da luu theo co che bat dong bo cua WiFi core.
  // Khong goi autoConnect() tai day vi buoc thu ket noi ban dau cua
  // WiFiManager co the cho dong den connect timeout.
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  wifiInitialConnectStartedAt = millis();
  Serial.println("Dang ket noi Wi-Fi da luu...");
}

void processWiFiManager() {
  if (!wifiManagerStarted) {
    return;
  }

  if (
    WiFi.status() != WL_CONNECTED &&
    !wifiPortalStarted &&
    millis() - wifiInitialConnectStartedAt >= WIFI_INITIAL_CONNECT_WINDOW_MS
  ) {
    Serial.println("Khong ket noi duoc Wi-Fi da luu. Mo Config Portal...");
    wifiPortalStarted = true;
    wifiManager.startConfigPortal(
      wifiConfigApName.c_str(),
      strlen(CONFIG_AP_PASSWORD) > 0 ? CONFIG_AP_PASSWORD : nullptr
    );
    displayDirty = true;
  }

  if (wifiPortalStarted) {
    wifiManager.process();
  }
}

void maintainWiFi() {
  if (!wifiManagerStarted) {
    return;
  }

  const bool connected = WiFi.status() == WL_CONNECTED;

  if (connected) {
    wifiDisconnectedSince = 0;

    if (!wifiWasConnected) {
      wifiWasConnected = true;
      wifiPortalStarted = false;
      Serial.println("Da ket noi Wi-Fi.");
      Serial.print("SSID: ");
      Serial.println(WiFi.SSID());
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("RSSI: ");
      Serial.println(WiFi.RSSI());

      lastHeartbeatAt = millis() - HEARTBEAT_INTERVAL_MS;
      lastCommandPollAt = millis();
      displayDirty = true;
    }
    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    cloudConnected = false;
    displayDirty = true;
  }

  if (wifiDisconnectedSince == 0) {
    wifiDisconnectedSince = millis();
    Serial.println("Mat ket noi Wi-Fi, dang thu reconnect...");
  }

  static unsigned long lastReconnectAt = 0;
  if (millis() - lastReconnectAt >= 5000) {
    lastReconnectAt = millis();
    WiFi.reconnect();
  }

  if (
    !wifiPortalStarted &&
    millis() - wifiDisconnectedSince >= WIFI_RESTART_AFTER_MS
  ) {
    Serial.println("Khong reconnect duoc. Restart de mo lai WiFiManager...");
    ESP.restart();
  }
}

// ============================================================
// BACKEND: REGISTER + HEARTBEAT
// ============================================================

bool registerDevice() {
  DynamicJsonDocument requestDoc(768);
  requestDoc["deviceId"] = deviceId;
  requestDoc["firmwareVersion"] = FW_VERSION;
  requestDoc["chipModel"] = ESP.getChipModel();
  requestDoc["chipRevision"] = ESP.getChipRevision();
  requestDoc["mac"] = WiFi.macAddress();
  requestDoc["irRxPin"] = IR_RX_PIN;
  requestDoc["irTxPin"] = IR_TX_PIN;

  String payload;
  serializeJson(requestDoc, payload);

  const HttpResponse response = httpPostJson(
    "/devices/register",
    payload,
    false,
    true
  );

  if (response.code != 200 && response.code != 201) {
    Serial.printf(
      "Dang ky thiet bi that bai. HTTP=%d, error=%s, body=%s\n",
      response.code,
      response.error.c_str(),
      response.body.c_str()
    );
    return false;
  }

  DynamicJsonDocument responseDoc(1536);
  const DeserializationError jsonError = deserializeJson(
    responseDoc,
    response.body
  );

  if (jsonError) {
    Serial.print("JSON register khong hop le: ");
    Serial.println(jsonError.c_str());
    return false;
  }

  String token = responseDoc["deviceToken"] | "";
  String code = responseDoc["pairingCode"] | "";

  if (token.isEmpty() && responseDoc["data"].is<JsonObject>()) {
    token = responseDoc["data"]["deviceToken"] | "";
  }
  if (code.isEmpty() && responseDoc["data"].is<JsonObject>()) {
    code = responseDoc["data"]["pairingCode"] | "";
  }

  if (token.isEmpty()) {
    Serial.println("Backend khong tra deviceToken.");
    return false;
  }

  saveDeviceToken(token);
  pairingCode = code;
  preferences.putString("pairCode", pairingCode);

  bool pairedFound = false;
  const bool paired = extractPairedFlag(
    responseDoc.as<JsonVariantConst>(),
    pairedFound
  );
  savePairedState(pairedFound ? paired : false);
  goToBaseDisplayState();

  Serial.println();
  Serial.println("========== DEVICE REGISTERED ==========");
  Serial.print("Device ID: ");
  Serial.println(deviceId);
  Serial.print("Pairing code: ");
  Serial.println(pairingCode);
  Serial.println("Nhap Device ID / pairing code tren web de gan vao tai khoan.");
  Serial.println("=======================================");

  return true;
}

void ensureDeviceRegistered() {
  if (!deviceToken.isEmpty()) {
    return;
  }

  if (
    lastRegisterAttemptAt != 0 &&
    millis() - lastRegisterAttemptAt < REGISTER_RETRY_INTERVAL_MS
  ) {
    return;
  }

  lastRegisterAttemptAt = millis();
  registerDevice();
}

void sendHeartbeat() {
  DynamicJsonDocument doc(1024);
  doc["firmwareVersion"] = FW_VERSION;
  doc["ip"] = WiFi.localIP().toString();
  doc["ssid"] = WiFi.SSID();
  doc["rssi"] = WiFi.RSSI();
  doc["uptimeMs"] = millis();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["learning"] = learning.active;
  doc["activeLearningProfileId"] = learning.profileId;

  String payload;
  serializeJson(doc, payload);

  const HttpResponse response = httpPostJson(
    "/devices/" + deviceId + "/heartbeat",
    payload,
    true
  );

  if (response.code == 401 || response.code == 403) {
    Serial.println("Device token bi tu choi. Xoa token va dang ky lai.");
    clearDeviceToken();
    return;
  }

  if (response.code < 200 || response.code >= 300) {
    Serial.printf("Heartbeat loi HTTP=%d, body=%s\n", response.code, response.body.c_str());
    return;
  }

  // Uu tien field paired/isPaired tu backend. Neu backend cu khong tra
  // field nay, heartbeat 2xx duoc xem la da pair (backend hien tai chi
  // chap nhan heartbeat hop le sau khi device duoc gan vao user).
  const bool foundPairedFlag = applyPairingStateFromJson(response.body);
  if (!foundPairedFlag && !devicePaired) {
    savePairedState(true);
  }
}

// ============================================================
// COMMAND ACK
// ============================================================

void acknowledgeCommand(
  const String &commandId,
  const String &status,
  const String &message
) {
  if (commandId.isEmpty()) {
    return;
  }

  DynamicJsonDocument doc(768);
  doc["status"] = status;
  doc["message"] = message;
  doc["deviceTimeMs"] = millis();

  String payload;
  serializeJson(doc, payload);

  const HttpResponse response = httpPostJson(
    "/devices/" + deviceId + "/commands/" + commandId + "/ack",
    payload,
    true
  );

  if (response.code >= 200 && response.code < 300) {
    lastCommandId = commandId;
    preferences.putString("lastCmd", lastCommandId);
  } else {
    Serial.printf("ACK command loi HTTP=%d\n", response.code);
  }
}

// ============================================================
// IR LEARNING
// ============================================================

void startLearning(
  const String &commandId,
  const String &profileId,
  const String &expectedAction,
  const unsigned long timeoutMs
) {
  learning.active = true;
  learning.commandId = commandId;
  learning.profileId = profileId;
  learning.expectedAction = expectedAction;
  learning.startedAt = millis();
  learning.timeoutMs = timeoutMs;

  // Bật ngắt IR receiver và xả sạch bất kỳ tín hiệu rác/cũ nào còn đệm từ trước
  irReceiver.enableIRIn();
  while (irReceiver.decode(&irResults)) {
    irReceiver.resume();
  }
  irReceiver.resume();
  showLearningEvent();

  Serial.println();
  Serial.println("========== IR LEARNING ==========");
  Serial.print("Profile ID: ");
  Serial.println(profileId);
  Serial.print("Expected action: ");
  Serial.println(expectedAction);
  Serial.println("Huong remote goc vao HX1838 va bam nut theo huong dan tren web.");
  Serial.println("=================================");
}

void cancelLearning(const String &reason) {
  if (!learning.active) {
    return;
  }

  irReceiver.disableIRIn();
  Serial.print("Dung IR learning: ");
  Serial.println(reason);

  learning = LearningSession();
  goToBaseDisplayState();
}

String stateBytesToHex(const decode_results &results) {
  if (results.state == nullptr || results.bits == 0) {
    return "";
  }

  const uint16_t byteCount = (results.bits + 7) / 8;
  String output;
  output.reserve(byteCount * 2);

  for (uint16_t i = 0; i < byteCount; i++) {
    char byteBuffer[3];
    snprintf(byteBuffer, sizeof(byteBuffer), "%02X", results.state[i]);
    output += byteBuffer;
  }

  return output;
}

void addCommonAcStateToJson(
  JsonObject target,
  const stdAc::state_t &state
) {
  target["protocol"] = typeToString(state.protocol);
  target["model"] = state.model;
  target["power"] = state.power;
  target["mode"] = IRac::opmodeToString(state.mode);
  target["temperature"] = state.degrees;
  target["celsius"] = state.celsius;
  target["fan"] = IRac::fanspeedToString(state.fanspeed);
  target["swingV"] = IRac::swingvToString(state.swingv);
  target["swingH"] = IRac::swinghToString(state.swingh);
  target["quiet"] = state.quiet;
  target["turbo"] = state.turbo;
  target["econo"] = state.econo;
  target["light"] = state.light;
  target["filter"] = state.filter;
  target["clean"] = state.clean;
  target["beep"] = state.beep;
  target["sleep"] = state.sleep;
}

bool uploadLearnedSignal(const decode_results &results) {
  const String protocolName = typeToString(results.decode_type);
  const bool nativeSendSupported = IRac::isProtocolSupported(
    results.decode_type
  );

  stdAc::state_t commonState;
  IRac::initState(&commonState);

  const bool commonDecoded = IRAcUtils::decodeToState(
    &results,
    &commonState,
    nullptr
  );

  // 32 KB phù hợp với frame điều hòa phổ biến + raw timing.
  DynamicJsonDocument doc(32768);
  doc["event"] = "IR_SIGNAL_LEARNED";
  doc["deviceId"] = deviceId;
  doc["commandId"] = learning.commandId;
  doc["profileId"] = learning.profileId;
  doc["expectedAction"] = learning.expectedAction;
  doc["protocol"] = protocolName;
  doc["protocolId"] = static_cast<int>(results.decode_type);
  doc["bits"] = results.bits;
  doc["code"] = resultToHexidecimal(&results);
  doc["address"] = results.address;
  doc["commandCode"] = results.command;
  doc["repeatCount"] = results.repeat ? 1 : 0;
  doc["stateHex"] = stateBytesToHex(results);
  doc["description"] = IRAcUtils::resultAcToString(&results);
  doc["nativeSendSupported"] = nativeSendSupported;
  doc["commonDecoded"] = commonDecoded;
  doc["controlType"] =
    (commonDecoded && nativeSendSupported) ? "NATIVE" : "RAW";

  if (commonDecoded) {
    JsonObject common = doc.createNestedObject("commonState");
    addCommonAcStateToJson(common, commonState);
  }

  JsonArray raw = doc.createNestedArray("rawUs");
  const uint16_t availableRawCount =
    results.rawlen > 0 ? results.rawlen - 1 : 0;
  const uint16_t rawCount =
    availableRawCount < MAX_RAW_TIMINGS_UPLOAD
      ? availableRawCount
      : MAX_RAW_TIMINGS_UPLOAD;

  // rawbuf[0] là khoảng gap trước frame, không phải dữ liệu phát lại.
  for (uint16_t i = 1; i <= rawCount; i++) {
    raw.add(static_cast<uint32_t>(results.rawbuf[i]) * kRawTick);
  }

  doc["rawCount"] = rawCount;
  doc["rawTruncated"] = (results.rawlen > rawCount + 1);
  doc["captureBufferSize"] = IR_CAPTURE_BUFFER_SIZE;

  String payload;
  payload.reserve(12000);
  serializeJson(doc, payload);

  const HttpResponse response = httpPostJson(
    "/devices/" + deviceId +
      "/profiles/" + learning.profileId +
      "/learned-signals",
    payload,
    true
  );

  if (response.code < 200 || response.code >= 300) {
    Serial.printf(
      "Upload IR learned that bai HTTP=%d, body=%s\n",
      response.code,
      response.body.c_str()
    );
    return false;
  }

  // Cache tối thiểu hồ sơ gần nhất trên ESP32.
  preferences.putString("profile", learning.profileId);
  preferences.putString("protocol", protocolName);
  preferences.putInt("model", commonDecoded ? commonState.model : -1);
  preferences.putString(
    "ctrlType",
    (commonDecoded && nativeSendSupported) ? "NATIVE" : "RAW"
  );

  Serial.println("Da gui ket qua hoc IR len server.");
  Serial.print("Protocol: ");
  Serial.println(protocolName);
  Serial.print("Control type: ");
  Serial.println(
    (commonDecoded && nativeSendSupported) ? "NATIVE" : "RAW"
  );

  return true;
}

void processIrReceiver() {
  if (!irReceiver.decode(&irResults)) {
    return;
  }

  // Lọc tín hiệu nhiễu: Lệnh IR điều hòa/thiết bị thực tế luôn có tối thiểu 15 timings.
  if (irResults.rawlen < 15 || irResults.overflow) {
    irReceiver.resume();
    return;
  }

  // Chế độ 1: Đang trong phiên học lệnh từ Web
  if (learning.active) {
    Serial.println();
    Serial.println("Da nhan tin hieu remote goc (Learning Mode).");
    Serial.print("Protocol: ");
    Serial.println(typeToString(irResults.decode_type));
    Serial.print("Bits: ");
    Serial.println(irResults.bits);
    Serial.println(resultToHumanReadableBasic(&irResults));

    const bool uploaded = uploadLearnedSignal(irResults);

    if (uploaded) {
      const String completedCommandId = learning.commandId;
      const String learnedProtocol = typeToString(irResults.decode_type);
      const uint16_t learnedBits = irResults.bits;
      learning = LearningSession();
      showLearningSuccess(learnedProtocol, learnedBits);
      acknowledgeCommand(
        completedCommandId,
        "completed",
        "IR signal learned and uploaded"
      );
    } else {
      Serial.println("Upload loi. Van tiep tuc cho remote trong thoi gian con lai.");
      irReceiver.resume();
    }
    return;
  }

  // Chế độ 2: Lắng nghe thụ động (Passive IR Sniffing) khi remote nhựa bên ngoài bấm
  Serial.println("[IR SNIFFED] Remote ngoai bam:");
  Serial.printf("  Protocol: %s | Code: 0x%s | Addr: 0x%X | Cmd: 0x%X | Bits: %d\n",
                typeToString(irResults.decode_type).c_str(),
                resultToHexidecimal(&irResults).c_str(),
                irResults.address, irResults.command, irResults.bits);

  DynamicJsonDocument doc(512);
  doc["event"] = "IR_SNIFFED";
  doc["protocol"] = typeToString(irResults.decode_type);
  doc["code"] = resultToHexidecimal(&irResults);
  doc["address"] = irResults.address;
  doc["commandCode"] = irResults.command;
  doc["bits"] = irResults.bits;
  doc["repeatCount"] = irResults.repeat ? 1 : 0;

  String payload;
  serializeJson(doc, payload);
  httpPostJson("/devices/" + deviceId + "/events", payload, true);

  irReceiver.resume();
}

void processLearningTimeout() {
  if (!learning.active) {
    return;
  }

  if (millis() - learning.startedAt < learning.timeoutMs) {
    return;
  }

  irReceiver.disableIRIn();
  const String timedOutCommandId = learning.commandId;
  const String profileId = learning.profileId;
  learning = LearningSession();
  goToBaseDisplayState();

  DynamicJsonDocument doc(512);
  doc["event"] = "IR_LEARNING_TIMEOUT";
  doc["profileId"] = profileId;

  String payload;
  serializeJson(doc, payload);

  httpPostJson(
    "/devices/" + deviceId + "/events",
    payload,
    true
  );

  acknowledgeCommand(
    timedOutCommandId,
    "failed",
    "IR learning timeout"
  );

  Serial.println("IR learning timeout.");
}

// ============================================================
// PHÁT IR NATIVE / RAW
// ============================================================

bool sendNativeAcState(JsonObject command, String &errorMessage) {
  const String protocolText = command["protocol"] | "";
  const decode_type_t protocol = strToDecodeType(protocolText.c_str());

  if (protocol == decode_type_t::UNKNOWN) {
    errorMessage = "Unknown protocol string: " + protocolText;
    return false;
  }

  if (!IRac::isProtocolSupported(protocol)) {
    errorMessage = "Protocol is not supported by IRac sender";
    return false;
  }

  stdAc::state_t desired;
  IRac::initState(&desired);

  desired.protocol = protocol;
  desired.model = command["model"] | -1;
  desired.power = command["power"] | false;
  desired.celsius = command["celsius"] | true;
  desired.degrees = command["temperature"] | 26.0;

  const String modeText = command["mode"] | "auto";
  const String fanText = command["fan"] | "auto";
  const String swingVText = command["swingV"] | "off";
  const String swingHText = command["swingH"] | "off";

  desired.mode = IRac::strToOpmode(
    modeText.c_str(),
    stdAc::opmode_t::kAuto
  );
  desired.fanspeed = IRac::strToFanspeed(
    fanText.c_str(),
    stdAc::fanspeed_t::kAuto
  );
  desired.swingv = IRac::strToSwingV(
    swingVText.c_str(),
    stdAc::swingv_t::kOff
  );
  desired.swingh = IRac::strToSwingH(
    swingHText.c_str(),
    stdAc::swingh_t::kOff
  );

  desired.quiet = command["quiet"] | false;
  desired.turbo = command["turbo"] | false;
  desired.econo = command["econo"] | false;
  desired.light = command["light"] | false;
  desired.filter = command["filter"] | false;
  desired.clean = command["clean"] | false;
  desired.beep = command["beep"] | false;
  desired.sleep = command["sleep"] | -1;
  desired.clock = command["clock"] | -1;

  const String profileId = command["profileId"] | "";

  const bool canUsePrevious =
    hasPreviousAcState &&
    profileId == previousProfileId &&
    protocol == previousProtocol;

  // Tắt mắt thu IR trước khi phát để tránh hiện tượng tự nhại lại (self-echo)
  irReceiver.disableIRIn();

  const bool sent = universalAc.sendAc(
    desired,
    canUsePrevious ? &previousAcState : nullptr
  );

  // Đợi 50ms cho chùm sóng phát tan hết rồi mới mở lại mắt thu
  delay(50);
  if (!learning.active) {
    irReceiver.enableIRIn();
    irReceiver.resume();
  }

  if (!sent) {
    errorMessage = "IRac::sendAc returned false";
    return false;
  }

  previousAcState = universalAc.getState();
  hasPreviousAcState = true;
  previousProfileId = profileId;
  previousProtocol = protocol;

  return true;
}

bool sendRawSignal(JsonObject command, String &errorMessage) {
  const String protocolStr = command["protocol"] | "";
  const String codeStr = command["code"] | "";
  const uint16_t bits = command["bits"] | 0;
  const uint16_t repeatCount = command["repeatCount"] | 0;
  const uint32_t addressVal = command["address"] | 0;
  const uint32_t commandVal = command["commandCode"] | 0;

  decode_type_t protocol = strToDecodeType(protocolStr.c_str());
  uint64_t codeVal = 0;

  if (!codeStr.isEmpty()) {
    if (codeStr.startsWith("0x") || codeStr.startsWith("0X")) {
      codeVal = strtoull(codeStr.c_str() + 2, NULL, 16);
    } else {
      codeVal = strtoull(codeStr.c_str(), NULL, 10);
    }
  }

  // Tắt mắt thu IR trước khi phát để tránh hiện tượng tự nhại lại (self-echo)
  irReceiver.disableIRIn();

  bool sentSuccess = false;
  if (protocol != decode_type_t::UNKNOWN && bits > 0) {
    uint64_t transmitCode = codeVal;
    if (protocol == decode_type_t::NEC && (addressVal > 0 || commandVal > 0)) {
      transmitCode = irSender.encodeNEC(addressVal, commandVal);
    }
    const uint16_t effectiveRepeat = (repeatCount > 0) ? repeatCount : 1;
    if (transmitCode > 0) {
      sentSuccess = irSender.send(protocol, transmitCode, bits, effectiveRepeat);
      if (sentSuccess) {
        Serial.printf("[IR TRANSMIT] Sent via Protocol=%s, Code=0x%llX (Addr=0x%X, Cmd=0x%X), Bits=%d, Repeat=%d\n",
                      protocolStr.c_str(), transmitCode, addressVal, commandVal, bits, effectiveRepeat);
      }
    }
  }

  if (!sentSuccess) {
    JsonArray rawArray = command["rawUs"].as<JsonArray>();
    if (!rawArray.isNull() && rawArray.size() > 0 && rawArray.size() <= 1200) {
      const uint16_t frequencyKhz = command["frequencyKhz"] | 38;
      const size_t count = rawArray.size();
      uint16_t *rawData = new (std::nothrow) uint16_t[count];
      if (rawData != nullptr) {
        size_t index = 0;
        for (JsonVariant value : rawArray) {
          const uint32_t timing = value.as<uint32_t>();
          rawData[index++] = static_cast<uint16_t>(timing > 65535UL ? 65535UL : timing);
        }
        irSender.sendRaw(rawData, count, frequencyKhz);
        delete[] rawData;
        sentSuccess = true;
        Serial.printf("[IR TRANSMIT] Sent via RAW us (count=%d)\n", count);
      } else {
        errorMessage = "Not enough heap for raw data";
      }
    } else {
      errorMessage = "rawUs is empty or invalid";
    }
  }

  // Đợi 50ms cho sóng phát tan hết rồi mới kích hoạt lại mắt thu
  delay(50);
  if (!learning.active) {
    irReceiver.enableIRIn();
    irReceiver.resume();
  }

  if (!sentSuccess) {
    if (errorMessage.isEmpty()) {
      errorMessage = "Could not send IR signal";
    }
    return false;
  }

  return true;
}

// ============================================================
// XỬ LÝ COMMAND TỪ SERVER
// ============================================================

void processCommand(JsonObject command) {
  const String commandId = command["id"] | "";
  const String type = command["type"] | "";

  if (commandId.isEmpty() || type.isEmpty()) {
    Serial.println("Command thieu id/type.");
    return;
  }

  if (commandId == lastCommandId) {
    Serial.println("Command trung lap, bo qua.");
    return;
  }

  Serial.println();
  Serial.print("Nhan command: ");
  Serial.print(type);
  Serial.print(" / ");
  Serial.println(commandId);

  if (type == "START_LEARNING") {
    const String profileId = command["profileId"] | "";
    const String expectedAction = command["expectedAction"] | "UNKNOWN";
    const unsigned long timeoutSeconds = command["timeoutSeconds"] | 45;

    if (profileId.isEmpty()) {
      acknowledgeCommand(commandId, "failed", "Missing profileId");
      return;
    }

    startLearning(
      commandId,
      profileId,
      expectedAction,
      timeoutSeconds * 1000UL
    );

    // Command được nhận, nhưng chỉ completed sau khi nhận và upload IR.
    acknowledgeCommand(commandId, "accepted", "Waiting for original remote");
    return;
  }

  if (type == "CANCEL_LEARNING") {
    cancelLearning("Cancelled by server");
    acknowledgeCommand(commandId, "completed", "Learning cancelled");
    return;
  }

  if (type == "SET_AC_STATE") {
    showCommandEvent(command, type);
    String errorMessage;
    const bool ok = sendNativeAcState(command, errorMessage);
    updateCommandEventResult(ok);

    acknowledgeCommand(
      commandId,
      ok ? "completed" : "failed",
      ok ? "Native A/C state sent" : errorMessage
    );
    return;
  }

  if (type == "SEND_RAW") {
    showCommandEvent(command, type);
    String errorMessage;
    const bool ok = sendRawSignal(command, errorMessage);
    updateCommandEventResult(ok);

    acknowledgeCommand(
      commandId,
      ok ? "completed" : "failed",
      ok ? "Raw IR signal sent" : errorMessage
    );
    return;
  }

  if (type == "RESET_WIFI") {
    acknowledgeCommand(commandId, "completed", "Wi-Fi settings cleared");
    wifiManager.resetSettings();
    ESP.restart();
    return;
  }

  if (type == "FACTORY_RESET") {
    acknowledgeCommand(commandId, "completed", "Factory reset requested");
    wifiManager.resetSettings();
    preferences.clear();
    ESP.restart();
    return;
  }

  if (type == "PING") {
    acknowledgeCommand(commandId, "completed", "PONG");
    return;
  }

  acknowledgeCommand(commandId, "failed", "Unsupported command type");
}

void pollNextCommand() {
  String path = "/devices/" + deviceId + "/commands/next";

  if (!lastCommandId.isEmpty()) {
    path += "?after=" + lastCommandId;
  }

  const HttpResponse response = httpGetJson(path, true);

  if (response.code == 204) {
    if (!devicePaired) {
      savePairedState(true);
    }
    return;
  }

  if (response.code == 401 || response.code == 403) {
    Serial.println("Polling bi tu choi token. Dang ky lai thiet bi.");
    clearDeviceToken();
    return;
  }

  if (response.code != 200) {
    Serial.printf("Poll command loi HTTP=%d\n", response.code);
    return;
  }

  DynamicJsonDocument doc(24576);
  const DeserializationError error = deserializeJson(doc, response.body);

  if (error) {
    Serial.print("JSON command loi: ");
    Serial.println(error.c_str());
    return;
  }

  JsonObject command = doc.as<JsonObject>();

  if (doc["command"].is<JsonObject>()) {
    command = doc["command"].as<JsonObject>();
  }

  if (command.isNull() || command.size() == 0) {
    return;
  }

  processCommand(command);
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);

  deviceId = buildDeviceId();
  loadPersistentState();

  Serial.println();
  Serial.println("========================================");
  Serial.println("AC CONTROLLER FIRMWARE + OLED FSM");
  Serial.print("Firmware: ");
  Serial.println(FW_VERSION);
  Serial.print("Device ID: ");
  Serial.println(deviceId);
  Serial.print("Pairing Code: ");
  Serial.println(pairingCode.isEmpty() ? "(Not generated yet)" : pairingCode);
  Serial.print("Paired Status: ");
  Serial.println(devicePaired ? "PAIRED" : "NOT PAIRED");
  Serial.println("========================================");

  // [BỎ OLED TẠM THỜI] Tắt khởi tạo phần cứng OLED, in ra Serial Monitor
  oledReady = false;
  Serial.println("[OLED] Tam thoi tat OLED, chuyen tat ca trang thai va ID sang Serial Monitor.");

  IRac::initState(&previousAcState);
  irSender.begin();
  irReceiver.enableIRIn();
  irReceiver.setUnknownThreshold(12);

  displayStateEnteredAt = millis();
  setDisplayState(DisplayState::STATE_BOOT);

  lastCommandPollAt = millis();
  lastHeartbeatAt = millis();
}

void loop() {
  const unsigned long now = millis();

  // BOOT dung dung 2000 ms nhung khong khoa CPU.
  if (
    !wifiManagerStarted &&
    now - displayStateEnteredAt >= BOOT_SCREEN_DURATION_MS
  ) {
    setDisplayState(DisplayState::STATE_WIFI_CONNECTING);
    beginWiFiManagerNonBlocking();
  }

  processWiFiManager();
  maintainWiFi();
  processIrReceiver();
  processLearningTimeout();
  processDisplayFsm(now);

  if (WiFi.status() != WL_CONNECTED) {
    yield();
    return;
  }

  ensureDeviceRegistered();

  if (deviceToken.isEmpty()) {
    yield();
    return;
  }

  if (now - lastCommandPollAt >= COMMAND_POLL_INTERVAL_MS) {
    lastCommandPollAt = now;
    pollNextCommand();
  }

  if (now - lastHeartbeatAt >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatAt = now;
    sendHeartbeat();
  }

  yield();
}
