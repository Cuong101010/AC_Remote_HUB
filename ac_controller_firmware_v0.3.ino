/*
  AC Controller Firmware v0.3 - OLED FSM (toi uu do tre lenh + don code)
  Board: ESP32 Dev Module

  Thay doi so voi v0.2:
  - Sua loi lech hang so buffer raw (comment ghi 700 nhung mang thuc te 350) ->
    gop lai thanh MOT hang so MAX_RAW_SAMPLES=400 dung xuyen suot.
  - Bo vong JSON serialize/deserialize thua khi thuc thi SET_AC_STATE/SEND_RAW:
    sendNativeAcState() va sendEncodedSignal() doc thang tu CommandMsg da parse
    san o pollNextCommand(), khong bọc lai qua StaticJsonDocument nua.
  - Xoa nhanh code chet (cap phat heap dong cho rawUs qua JSON) trong duong
    SEND_RAW - rawUs tu server luon di qua CommandMsg.rawUs, khong con di qua
    nhanh nay.
  - Poll lenh: giam COMMAND_POLL_INTERVAL_MS tu 300ms -> 150ms, VA them co che
    "greedy poll": ngay sau khi nhan 1 lenh, poll lai NGAY (co gioi han bang
    CommandQueue con cho trong) thay vi doi het chu ky, giup nhieu lenh lien
    tiep tu web toi thiet bi gan nhu tuc thi thay vi xep hang tung 150-300ms.
  - Poll dung timeout rieng ngan hon (1.2s connect / 1.5s tong) qua
    httpGetJsonFast() de mot request treo mang khong khoa ca vong lap
    Network Task; heartbeat/register van dung timeout dai nhu cu.
  - Network Task: giam vTaskDelay 50ms -> 20ms de tang do phan giai polling.
  - Them heap-health guard: neu free heap roi duoi nguong an toan, chu dong
    restart thay vi co gang chay tiep va co the crash giua mot request.
  - Gop code disable/enable IR receiver quanh moi lan phat song vao 2 ham
    dung chung: silenceReceiverForTx() / reArmReceiverAfterTx().

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

static const char *FW_VERSION = "0.3.0";

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

// Giới hạn số timing raw (learning-upload lẫn SEND_RAW từ server) dùng CHUNG một
// hằng số duy nhất để tránh lệch giữa comment và kích thước mảng thực tế.
// Frame Electra 104-bit có 211 timing nên 400 là đủ dư cho hầu hết remote A/C.
static const uint16_t MAX_RAW_SAMPLES = 400;

// ============================================================
// CHU KỲ HỆ THỐNG
// ============================================================

// Chu kỳ poll tối thiểu giữa hai lần gọi /commands/next khi hàng đợi đang RỖNG.
// Ngay khi một lệnh được xử lý xong, Network Task sẽ poll lại NGAY (xem
// vòng lặp bên dưới) thay vì đợi đủ khoảng này — giá trị này chỉ giới hạn
// tần suất polling lúc idle để đỡ tải backend, không phải độ trễ lệnh thực tế.
static const unsigned long COMMAND_POLL_INTERVAL_MS = 150;
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

QueueHandle_t xCommandQueue = NULL; // Core 0 -> Core 1
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
  drawRow(1, " AC REMOTE HUB");
  drawRow(3, "    v0.2.0");
  drawRow(6, "  [BOOTING...]");
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

  drawRow(7, "  AC REMOTE HUB ");
}

void renderIdleScreen() {
  renderPairingScreen();
}

void renderCommandScreen() {
  drawRow(0, "  [ WEB CMD ]   ");
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

  // Render nội dung ra màn hình OLED SSD1306 nếu phần cứng khả dụng
  if (oledReady) {
    if (displayState != lastStatePrinted) {
      oled.clear();
    }
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
        renderIdleScreen();
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

  // Luôn đặt màn hình hiển thị Mã Ghép Nối (Pair Code) làm màn hình chờ rảnh mặc định
  setDisplayState(DisplayState::STATE_PAIRING_CODE);
}

// Nhận trực tiếp giá trị đã parse sẵn (không cần bọc lại qua JsonObject) để
// tránh một vòng serialize/deserialize thừa mỗi khi có lệnh mới từ web.
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

void initHttpClient() {
  // Safe empty initializer
}

static WiFiClientSecure g_secureClient;
static HTTPClient g_httpClient;
static bool g_httpInited = false;

HttpResponse executeHttpJson(
  const String &method,
  const String &url,
  const String &payload,
  const bool useDeviceToken,
  const bool useBootstrapKey = false,
  const uint16_t timeoutMsOverride = 0,
  const uint16_t connectTimeoutMsOverride = 0
) {
  HttpResponse response;
  const unsigned long startMs = millis();

  if (!g_httpInited) {
    g_secureClient.setInsecure();
    g_secureClient.setTimeout(3);
    g_httpClient.setReuse(true);
    g_httpInited = true;
  }

  // Cho phép mỗi lời gọi tự chọn timeout: request polling (chạy mỗi 150ms)
  // dùng timeout ngắn để một lần treo mạng không khoá cả vòng lặp Network
  // Task; request ít quan trọng về độ trễ (heartbeat, register) vẫn dùng
  // timeout dài hơn để chịu được kết nối chậm mà không bị false-fail.
  g_httpClient.setTimeout(timeoutMsOverride > 0 ? timeoutMsOverride : 3000);
  g_httpClient.setConnectTimeout(connectTimeoutMsOverride > 0 ? connectTimeoutMsOverride : 2500);

  WiFiClient plainClient;
  bool begun = false;

  if (url.startsWith("https://")) {
    begun = g_httpClient.begin(g_secureClient, url);
  } else {
    plainClient.setTimeout(3);
    begun = g_httpClient.begin(plainClient, url);
  }

  if (!begun) {
    response.error = "HTTP begin failed";
    g_httpClient.end();
    return response;
  }

  g_httpClient.addHeader("Accept", "application/json");
  g_httpClient.addHeader("Content-Type", "application/json");
  g_httpClient.addHeader("X-Device-Id", deviceId);

  if (useDeviceToken && !deviceToken.isEmpty()) {
    g_httpClient.addHeader("Authorization", "Bearer " + deviceToken);
  }

  if (useBootstrapKey) {
    g_httpClient.addHeader("X-Device-Bootstrap-Key", DEVICE_BOOTSTRAP_KEY);
  }

  if (method == "GET") {
    response.code = g_httpClient.GET();
  } else if (method == "POST") {
    response.code = g_httpClient.POST(payload);
  } else {
    response.error = "Unsupported HTTP method";
    g_httpClient.end();
    return response;
  }

  const unsigned long durationMs = millis() - startMs;

  if (response.code > 0) {
    response.body = g_httpClient.getString();
    if (response.code != 204 && response.code != 200) {
      Serial.printf("[HTTP] %s %s -> Code=%d (%lu ms)\n", method.c_str(), url.c_str(), response.code, durationMs);
    }
  } else {
    response.error = g_httpClient.errorToString(response.code);
    g_httpClient.end(); // Reset socket nếu bị ngắt kết nối mạng
  }

  if (response.code >= 200 && response.code < 300) {
    markCloudSuccess();
  }

  return response;
}

HttpResponse httpGetJson(const String &path, const bool auth = true) {
  return executeHttpJson("GET", apiUrl(path), "", auth, false);
}

// Dùng riêng cho poll lệnh: timeout ngắn (1.2s connect / 1.5s tổng) để một
// request treo không giữ Network Task lâu, giữ độ trễ lệnh ổn định.
HttpResponse httpGetJsonFast(const String &path, const bool auth = true) {
  return executeHttpJson("GET", apiUrl(path), "", auth, false, 1500, 1200);
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
  StaticJsonDocument<512> doc;
  doc["firmwareVersion"] = FW_VERSION;
  doc["ip"] = WiFi.localIP().toString();
  doc["ssid"] = WiFi.SSID();
  doc["rssi"] = WiFi.RSSI();
  doc["uptimeMs"] = millis();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["learning"] = learning.active;
  doc["activeLearningProfileId"] = learning.profileId;

  char payload[512];
  serializeJson(doc, payload, sizeof(payload));

  char path[128];
  snprintf(path, sizeof(path), "/devices/%s/heartbeat", deviceId.c_str());

  const HttpResponse response = httpPostJson(
    path,
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

  CloudMsg msg;
  msg.type = CLOUD_ACK_COMMAND;
  strncpy(msg.commandId, commandId.c_str(), sizeof(msg.commandId) - 1);
  strncpy(msg.status, status.c_str(), sizeof(msg.status) - 1);
  strncpy(msg.message, message.c_str(), sizeof(msg.message) - 1);

  if (xCloudQueue != NULL) {
    xQueueSend(xCloudQueue, &msg, 0);
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
  CloudMsg msg;
  msg.type = CLOUD_UPLOAD_LEARNED_SIGNAL;
  strncpy(msg.commandId, learning.commandId.c_str(), sizeof(msg.commandId) - 1);
  strncpy(msg.profileId, learning.profileId.c_str(), sizeof(msg.profileId) - 1);
  strncpy(msg.expectedAction, learning.expectedAction.c_str(), sizeof(msg.expectedAction) - 1);

  const String protocolName = typeToString(results.decode_type);
  strncpy(msg.protocol, protocolName.c_str(), sizeof(msg.protocol) - 1);
  msg.bits = results.bits;
  strncpy(msg.codeHex, resultToHexidecimal(&results).c_str(), sizeof(msg.codeHex) - 1);
  msg.address = results.address;
  msg.commandCode = results.command;
  msg.repeat = results.repeat;
  strncpy(msg.stateHex, stateBytesToHex(results).c_str(), sizeof(msg.stateHex) - 1);
  strncpy(msg.description, IRAcUtils::resultAcToString(&results).c_str(), sizeof(msg.description) - 1);

  msg.nativeSendSupported = IRac::isProtocolSupported(results.decode_type);
  stdAc::state_t commonState;
  IRac::initState(&commonState);
  msg.commonDecoded = IRAcUtils::decodeToState(&results, &commonState, nullptr);

  const uint16_t availableRawCount = results.rawlen > 0 ? results.rawlen - 1 : 0;
  const uint16_t rawCount = availableRawCount < MAX_RAW_SAMPLES ? availableRawCount : MAX_RAW_SAMPLES;
  msg.rawCount = rawCount;

  for (uint16_t i = 1; i <= rawCount; i++) {
    msg.rawUs[i - 1] = static_cast<uint32_t>(results.rawbuf[i]) * kRawTick;
  }

  const bool nativeSendSupported = msg.nativeSendSupported;
  const bool commonDecoded = msg.commonDecoded;

  if (xCloudQueue != NULL) {
    xQueueSend(xCloudQueue, &msg, 0);
  }

  // Cache tối thiểu hồ sơ gần nhất trên ESP32.
  preferences.putString("profile", learning.profileId);
  preferences.putString("protocol", protocolName);
  preferences.putInt("model", commonDecoded ? commonState.model : -1);
  preferences.putString(
    "ctrlType",
    (commonDecoded && nativeSendSupported) ? "NATIVE" : "RAW"
  );

  Serial.println("Da day tin hoc IR vao CloudQueue de gui len server.");
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
      irReceiver.resume();
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

// Tắt/mở lại mắt thu IR quanh một lần phát sóng — dùng chung cho cả đường
// native (IRac), encode (protocol/bits/code) và raw us, tránh lặp code.
void silenceReceiverForTx() {
  irReceiver.disableIRIn();
  yield();
}

void reArmReceiverAfterTx() {
  yield();
  delay(50); // Đợi sóng phát tan hết trước khi mở lại mắt thu, tránh tự nhại lại (self-echo)
  if (!learning.active) {
    irReceiver.enableIRIn();
    irReceiver.resume();
  }
}

// Nhận thẳng CommandMsg (đã parse 1 lần ở pollNextCommand) thay vì bọc lại
// qua StaticJsonDocument rồi đọc ra — bớt một vòng serialize/deserialize
// ArduinoJson không cần thiết trên mỗi lệnh SET_AC_STATE (giảm CPU + heap churn).
bool sendNativeAcState(const CommandMsg &cmd, String &errorMessage) {
  const decode_type_t protocol = strToDecodeType(cmd.protocol);

  if (protocol == decode_type_t::UNKNOWN) {
    errorMessage = String("Unknown protocol string: ") + cmd.protocol;
    return false;
  }

  if (!IRac::isProtocolSupported(protocol)) {
    errorMessage = "Protocol is not supported by IRac sender";
    return false;
  }

  stdAc::state_t desired;
  IRac::initState(&desired);

  desired.protocol = protocol;
  desired.power = cmd.power;
  desired.celsius = true;
  desired.degrees = cmd.temperature;

  desired.mode = IRac::strToOpmode(cmd.mode, stdAc::opmode_t::kAuto);
  desired.fanspeed = IRac::strToFanspeed(cmd.fan, stdAc::fanspeed_t::kAuto);
  desired.swingv = IRac::strToSwingV(cmd.swingV, stdAc::swingv_t::kOff);
  desired.swingh = stdAc::swingh_t::kOff;

  const String profileId = cmd.profileId;

  const bool canUsePrevious =
    hasPreviousAcState &&
    profileId == previousProfileId &&
    protocol == previousProtocol;

  silenceReceiverForTx();
  const bool sent = universalAc.sendAc(
    desired,
    canUsePrevious ? &previousAcState : nullptr
  );
  reArmReceiverAfterTx();

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

// Phát lệnh IR khi server gửi protocol/bits/code nhưng KHÔNG có mảng rawUs kèm
// theo (trường hợp có rawUs được xử lý thẳng ở processCommandMsg, không đi qua
// hàm này — nên không cần nhánh JsonArray + cấp phát heap động nữa).
bool sendEncodedSignal(const CommandMsg &cmd, String &errorMessage) {
  const decode_type_t protocol = strToDecodeType(cmd.protocol);
  uint64_t codeVal = 0;

  if (strlen(cmd.codeStr) > 0) {
    if (cmd.codeStr[0] == '0' && (cmd.codeStr[1] == 'x' || cmd.codeStr[1] == 'X')) {
      codeVal = strtoull(cmd.codeStr + 2, NULL, 16);
    } else {
      codeVal = strtoull(cmd.codeStr, NULL, 10);
    }
  }

  if (protocol == decode_type_t::UNKNOWN || cmd.bits == 0) {
    errorMessage = "Missing rawUs and no valid protocol/bits/code to encode";
    return false;
  }

  uint64_t transmitCode = codeVal;
  if (protocol == decode_type_t::NEC && (cmd.address > 0 || cmd.commandCode > 0)) {
    transmitCode = irSender.encodeNEC(cmd.address, cmd.commandCode);
  }

  if (transmitCode == 0) {
    errorMessage = "Could not resolve a code to transmit";
    return false;
  }

  const uint16_t effectiveRepeat = (cmd.repeatCount > 0) ? cmd.repeatCount : 1;

  silenceReceiverForTx();
  const bool sentSuccess = irSender.send(protocol, transmitCode, cmd.bits, effectiveRepeat);
  reArmReceiverAfterTx();

  if (sentSuccess) {
    Serial.printf("[IR TRANSMIT] Sent via Protocol=%s, Code=0x%llX (Addr=0x%X, Cmd=0x%X), Bits=%d, Repeat=%d\n",
                  cmd.protocol, transmitCode, cmd.address, cmd.commandCode, cmd.bits, effectiveRepeat);
  } else {
    errorMessage = "Could not send IR signal";
  }

  return sentSuccess;
}

// ============================================================
// XỬ LÝ COMMAND TỪ SERVER (CHẠY TRÊN CORE 1)
// ============================================================

void processCommandMsg(const CommandMsg &cmd) {
  if (strlen(cmd.id) == 0) {
    return;
  }

  static String s_lastExecutedCmdId = "";
  if (String(cmd.id) == s_lastExecutedCmdId) {
    Serial.printf("[Core 1] Command ID=%s trung lap, bo qua.\n", cmd.id);
    return;
  }
  s_lastExecutedCmdId = cmd.id;

  Serial.println();
  Serial.printf("[Core 1] Thuc thi Command: ID=%s, Type=%d\n", cmd.id, cmd.type);

  if (cmd.type == CMD_START_LEARNING) {
    if (strlen(cmd.profileId) == 0) {
      acknowledgeCommand(cmd.id, "failed", "Missing profileId");
      return;
    }

    startLearning(
      cmd.id,
      cmd.profileId,
      cmd.expectedAction,
      cmd.timeoutSeconds * 1000UL
    );

    acknowledgeCommand(cmd.id, "accepted", "Waiting for original remote");
    return;
  }

  if (cmd.type == CMD_CANCEL_LEARNING) {
    cancelLearning("Cancelled by server");
    acknowledgeCommand(cmd.id, "completed", "Learning cancelled");
    return;
  }

  if (cmd.type == CMD_SET_AC_STATE) {
    showCommandEvent(cmd.power, static_cast<int>(cmd.temperature), String(cmd.mode), "SET_AC_STATE");

    String errorMessage;
    const bool ok = sendNativeAcState(cmd, errorMessage);
    updateCommandEventResult(ok);

    acknowledgeCommand(
      cmd.id,
      ok ? "completed" : "failed",
      ok ? "Native A/C state sent" : errorMessage
    );
    return;
  }

  if (cmd.type == CMD_SEND_RAW) {
    showCommandEvent(false, 26, cmd.rawCount > 0 ? "RAW" : String(cmd.protocol), "SEND_RAW");

    String errorMessage;
    bool ok = false;

    if (cmd.rawCount > 0) {
      silenceReceiverForTx();
      irSender.sendRaw(cmd.rawUs, cmd.rawCount, cmd.frequencyKhz);
      reArmReceiverAfterTx();
      ok = true;
      Serial.printf("[IR TRANSMIT] Sent via RAW us (count=%d, freq=%d kHz)\n", cmd.rawCount, cmd.frequencyKhz);
    } else {
      ok = sendEncodedSignal(cmd, errorMessage);
    }

    updateCommandEventResult(ok);

    acknowledgeCommand(
      cmd.id,
      ok ? "completed" : "failed",
      ok ? "Raw IR signal sent" : errorMessage
    );
    return;
  }

  if (cmd.type == CMD_RESET_WIFI) {
    acknowledgeCommand(cmd.id, "completed", "Wi-Fi settings cleared");
    wifiManager.resetSettings();
    ESP.restart();
    return;
  }

  if (cmd.type == CMD_FACTORY_RESET) {
    acknowledgeCommand(cmd.id, "completed", "Factory reset requested");
    wifiManager.resetSettings();
    preferences.clear();
    ESP.restart();
    return;
  }

  if (cmd.type == CMD_PING) {
    acknowledgeCommand(cmd.id, "completed", "PONG");
    return;
  }

  acknowledgeCommand(cmd.id, "failed", "Unsupported command type");
}

bool pollNextCommand() {
  char path[128];
  if (!lastCommandId.isEmpty()) {
    snprintf(path, sizeof(path), "/devices/%s/commands/next?after=%s", deviceId.c_str(), lastCommandId.c_str());
  } else {
    snprintf(path, sizeof(path), "/devices/%s/commands/next", deviceId.c_str());
  }

  const HttpResponse response = httpGetJsonFast(path, true);

  if (response.code == 204) {
    if (!devicePaired) {
      savePairedState(true);
    }
    return false;
  }

  if (response.code == 401 || response.code == 403) {
    Serial.println("Polling bi tu choi token. Dang ky lai thiet bi.");
    clearDeviceToken();
    return false;
  }

  if (response.code != 200) {
    return false;
  }

  Serial.printf("[Poll HTTP 200] Body: %s\n", response.body.c_str());

  DynamicJsonDocument doc(12288);
  const DeserializationError error = deserializeJson(doc, response.body);

  if (error) {
    Serial.printf("[Poll Error] Lỗi parse JSON: %s\n", error.c_str());
    return false;
  }

  JsonObject command = doc.as<JsonObject>();
  if (doc["command"].is<JsonObject>()) {
    command = doc["command"].as<JsonObject>();
  }

  if (command.isNull() || command.size() == 0) {
    return false;
  }

  const String commandId = command["id"] | "";
  const String type = command["type"] | "";

  if (commandId.isEmpty() || type.isEmpty()) {
    return false;
  }

  if (commandId == lastCommandId) {
    Serial.printf("[Poll] Command ID=%s trung voi lastCommandId (%s), bo qua.\n", commandId.c_str(), lastCommandId.c_str());
    return false;
  }

  // Ghi nhận ngay lập tức ID lệnh vừa nhận để lần poll sau (sau 2s) truyền after=lastCommandId
  lastCommandId = commandId;
  preferences.putString("lastCmd", lastCommandId);

  Serial.printf("[Core 0] Nhan Command tu Cloud: ID=%s, Type=%s -> Day vao Queue\n", commandId.c_str(), type.c_str());

  CommandMsg cmdMsg;
  strncpy(cmdMsg.id, commandId.c_str(), sizeof(cmdMsg.id) - 1);
  strncpy(cmdMsg.profileId, command["profileId"] | "", sizeof(cmdMsg.profileId) - 1);
  strncpy(cmdMsg.expectedAction, command["expectedAction"] | "UNKNOWN", sizeof(cmdMsg.expectedAction) - 1);
  cmdMsg.timeoutSeconds = command["timeoutSeconds"] | 45;

  if (type == "START_LEARNING") cmdMsg.type = CMD_START_LEARNING;
  else if (type == "CANCEL_LEARNING") cmdMsg.type = CMD_CANCEL_LEARNING;
  else if (type == "SET_AC_STATE") cmdMsg.type = CMD_SET_AC_STATE;
  else if (type == "SEND_RAW") cmdMsg.type = CMD_SEND_RAW;
  else if (type == "RESET_WIFI") cmdMsg.type = CMD_RESET_WIFI;
  else if (type == "FACTORY_RESET") cmdMsg.type = CMD_FACTORY_RESET;
  else if (type == "PING") cmdMsg.type = CMD_PING;

  strncpy(cmdMsg.protocol, command["protocol"] | "", sizeof(cmdMsg.protocol) - 1);
  strncpy(cmdMsg.codeStr, command["code"] | "", sizeof(cmdMsg.codeStr) - 1);
  cmdMsg.bits = command["bits"] | 0;
  cmdMsg.repeatCount = command["repeatCount"] | 0;
  cmdMsg.address = command["address"] | 0;
  cmdMsg.commandCode = command["commandCode"] | 0;
  cmdMsg.power = command["power"] | false;
  cmdMsg.temperature = command["temperature"] | 26.0;
  strncpy(cmdMsg.mode, command["mode"] | "auto", sizeof(cmdMsg.mode) - 1);
  strncpy(cmdMsg.fan, command["fan"] | "auto", sizeof(cmdMsg.fan) - 1);
  strncpy(cmdMsg.swingV, command["swingV"] | "off", sizeof(cmdMsg.swingV) - 1);

  cmdMsg.frequencyKhz = command["frequencyKhz"] | 38;
  JsonArray rawArr = command["rawUs"].as<JsonArray>();
  if (!rawArr.isNull()) {
    cmdMsg.rawCount = 0;
    for (uint16_t val : rawArr) {
      if (cmdMsg.rawCount < MAX_RAW_SAMPLES) {
        cmdMsg.rawUs[cmdMsg.rawCount++] = val;
      }
    }
  }

  if (xCommandQueue != NULL) {
    xQueueSend(xCommandQueue, &cmdMsg, 0);
  }

  return true;
}

void processCloudQueue() {
  CloudMsg msg;
  while (xCloudQueue != NULL && xQueueReceive(xCloudQueue, &msg, 0) == pdTRUE) {
    if (msg.type == CLOUD_ACK_COMMAND) {
      StaticJsonDocument<384> doc;
      doc["status"] = msg.status;
      doc["message"] = msg.message;
      doc["deviceTimeMs"] = millis();

      char payload[384];
      serializeJson(doc, payload, sizeof(payload));

      char path[128];
      snprintf(path, sizeof(path), "/devices/%s/commands/%s/ack", deviceId.c_str(), msg.commandId);

      const HttpResponse response = httpPostJson(path, payload, true);
      if (response.code >= 200 && response.code < 300) {
        lastCommandId = String(msg.commandId);
        preferences.putString("lastCmd", lastCommandId);
      }
    } else if (msg.type == CLOUD_UPLOAD_LEARNED_SIGNAL) {
      DynamicJsonDocument doc(4096);
      doc["event"] = "IR_SIGNAL_LEARNED";
      doc["deviceId"] = deviceId;
      doc["commandId"] = msg.commandId;
      doc["profileId"] = msg.profileId;
      doc["expectedAction"] = msg.expectedAction;
      doc["protocol"] = msg.protocol;
      doc["bits"] = msg.bits;
      doc["code"] = msg.codeHex;
      doc["address"] = msg.address;
      doc["commandCode"] = msg.commandCode;
      doc["repeatCount"] = msg.repeat ? 1 : 0;
      doc["stateHex"] = msg.stateHex;
      doc["description"] = msg.description;
      doc["nativeSendSupported"] = msg.nativeSendSupported;
      doc["commonDecoded"] = msg.commonDecoded;
      doc["controlType"] = (msg.commonDecoded && msg.nativeSendSupported) ? "NATIVE" : "RAW";

      JsonArray raw = doc.createNestedArray("rawUs");
      for (uint16_t i = 0; i < msg.rawCount; i++) {
        raw.add(msg.rawUs[i]);
      }

      String payload;
      payload.reserve(3072);
      serializeJson(doc, payload);

      char path[128];
      snprintf(path, sizeof(path), "/devices/%s/profiles/%s/learned-signals", deviceId.c_str(), msg.profileId);
      httpPostJson(path, payload, true);
    }
  }
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

  // Khởi tạo HTTP client duy nhất một lần
  initHttpClient();

  // Khởi tạo Queues 2 chiều cho Core 0 (Network) và Core 1 (IR/OLED)
  xCommandQueue = xQueueCreate(10, sizeof(CommandMsg));
  xCloudQueue = xQueueCreate(10, sizeof(CloudMsg));

  // Khởi tạo màn hình OLED SSD1306 qua I2C (SDA=21, SCL=22, Address=0x3C)
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.beginTransmission(OLED_I2C_ADDRESS);
  if (Wire.endTransmission() == 0) {
    oled.begin();
    oled.setFont(u8x8_font_chroma48medium8_r);
    oled.clear();
    oledReady = true;
    Serial.println("[OLED] Da kich hoat va khoi tao OLED thanh cong (SDA: 21, SCL: 22, Address: 0x3C)");
  } else {
    oledReady = false;
    Serial.println("[OLED] Khong tim thay hardware OLED tai 0x3C, duy tri hien thi tren Serial Monitor.");
  }

  IRac::initState(&previousAcState);
  irSender.begin();
  irReceiver.enableIRIn();
  irReceiver.setUnknownThreshold(12);

  displayStateEnteredAt = millis();
  setDisplayState(DisplayState::STATE_BOOT);

  lastCommandPollAt = millis();
  lastHeartbeatAt = millis();

  // Khởi tạo Network Task ĐỘC QUYỀN HTTP trên CORE 0
  xTaskCreatePinnedToCore(
    [](void *pvParameters) {
      Serial.println("[FreeRTOS] Network Task khoi chay thanh cong tren Core 0 (Chuu quyen HTTP)");
      // Bảo vệ chống rò rỉ heap lâu dài: nếu bộ nhớ trống rơi xuống mức nguy
      // hiểm, tự khởi động lại thay vì tiếp tục chạy và có nguy cơ crash giữa
      // một request (giữ hệ thống "chạy ổn định" 24/7).
      static const uint32_t MIN_SAFE_HEAP_BYTES = 20000;

      for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
          ensureDeviceRegistered();
          if (!deviceToken.isEmpty()) {
            const unsigned long now = millis();

            // 1. Poll lệnh từ server. Nếu có lệnh mới, poll lại NGAY LẬP TỨC
            //    (không đợi hết COMMAND_POLL_INTERVAL_MS) để rút ngắn tối đa
            //    thời gian phản hồi khi có nhiều lệnh liên tiếp từ web.
            if (now - lastCommandPollAt >= COMMAND_POLL_INTERVAL_MS) {
              lastCommandPollAt = now;
              // Chỉ drain liên tục khi CommandQueue còn chỗ trống — nếu Core 1
              // (thực thi IR) chưa kịp xử lý kịp, dừng lại và để lần poll kế
              // tiếp (150ms sau) tiếp tục, tránh làm tràn queue và rớt lệnh.
              while (
                xCommandQueue != NULL &&
                uxQueueSpacesAvailable(xCommandQueue) > 1 &&
                pollNextCommand()
              ) {
                lastCommandPollAt = millis();
              }
            }

            // 2. Gui phan hoi ACK va upload IR signal tu CloudQueue
            processCloudQueue();

            // 3. Gui heartbeat
            if (now - lastHeartbeatAt >= HEARTBEAT_INTERVAL_MS) {
              lastHeartbeatAt = now;
              sendHeartbeat();

              if (ESP.getFreeHeap() < MIN_SAFE_HEAP_BYTES) {
                Serial.printf("[Network Task] Free heap qua thap (%u bytes). Restart de phong ngua crash.\n", ESP.getFreeHeap());
                ESP.restart();
              }
            }
          }
        }
        // 20ms thay vi 50ms: tang do phan giai cua vong lap polling/ack ma
        // van de CPU ranh cho Core 0 idle task va WiFi stack.
        vTaskDelay(pdMS_TO_TICKS(20));
      }
    },
    "NetworkTask",
    16384, // Tăng Stack lên 16KB cho HTTPS (mbedTLS) + JSON Parsing
    NULL,
    1,
    NULL,
    0
  );
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

  // Nhận command từ Core 0 va thuc thi tren Core 1
  CommandMsg cmd;
  if (xCommandQueue != NULL && xQueueReceive(xCommandQueue, &cmd, 0) == pdTRUE) {
    processCommandMsg(cmd);
  }

  processIrReceiver();
  processLearningTimeout();
  processDisplayFsm(now);

  yield();
}
