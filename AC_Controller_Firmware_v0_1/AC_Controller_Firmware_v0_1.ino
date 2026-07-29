/*
  AC Controller Firmware v0.1
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
#include <DNSServer.h>
#include <WebServer.h>
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

static const char *FW_VERSION = "0.1.0";

// Ví dụ local backend: http://192.168.1.10:3000/api/v1
// Ví dụ cloud backend: https://api.tenmiencuaban.vn/api/v1
static const char *API_BASE_URL = "http://192.168.0.104:3000/api/v1";

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

// ============================================================
// ĐỐI TƯỢNG TOÀN CỤC
// ============================================================

Preferences preferences;
WiFiManager wifiManager;

// Captive Portal (dùng khi WiFiManager đang chạy AP Setup)
DNSServer    captiveDns;
WebServer    captivePortal(80);
bool         captivePortalActive = false;

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

  bool begun = false;

  if (url.startsWith("https://")) {
    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    begun = http.begin(secureClient, url);
  } else {
    begun = http.begin(url);
  }

  if (!begun) {
    response.error = "HTTP begin failed";
    return response;
  }

  http.setConnectTimeout(5000);
  http.setTimeout(10000);
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
}

// ============================================================
// WIFI MANAGER
// ============================================================


// ============================================================
// CAPTIVE PORTAL (DNS redirect cho AP mở)
// Mọi truy vấn DNS khi ESP32 đang ở chế độ AP đều trả về
// IP của AP (192.168.4.1) để điện thoại / laptop tự mở trình duyệt.
// ============================================================

void startCaptivePortalDns() {
  // Khởi động DNS server - mọi domain đều trả về IP AP
  captiveDns.start(53, "*", WiFi.softAPIP());

  // Trang web cấu hình đơn giản – redirect về captive portal WiFiManager
  captivePortal.onNotFound([]() {
    captivePortal.sendHeader("Location", "http://192.168.4.1", true);
    captivePortal.send(302, "text/plain", "");
  });
  captivePortal.begin();
  captivePortalActive = true;
  Serial.println("Captive Portal DNS da khoi dong.");
}

void stopCaptivePortalDns() {
  if (captivePortalActive) {
    captiveDns.stop();
    captivePortal.stop();
    captivePortalActive = false;
  }
}

void processCaptivePortal() {
  if (!captivePortalActive) return;
  captiveDns.processNextRequest();
  captivePortal.handleClient();
}

bool connectWiFiWithManager() {
  String apName = "AC-Remote-" + deviceId.substring(deviceId.length() - 6);

  // Đặt callback trước khi autoConnect
  wifiManager.setAPCallback([](WiFiManager *manager) {
    Serial.println();
    Serial.println("========== WIFI SETUP MODE ==========");
    Serial.print("AP (Mo, khong mat khau): ");
    Serial.println(manager->getConfigPortalSSID());
    Serial.print("IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.println("Ket noi vao mang AC-Remote-XXXXXX");
    Serial.println("Mo trinh duyet bat ky de cau hinh Wi-Fi");
    Serial.println("=====================================");
    startCaptivePortalDns();
  });

  wifiManager.setConnectTimeout(20);
  wifiManager.setConfigPortalTimeout(180);

  // Tạo luồng xử lý DNS/HTTP trong khi WiFiManager chạy portal
  // WiFiManager gọi yield() bên trong, nên ta process trong callback
  // Thực tế: WiFiManager tự loop, ta gọi process ở đây trước/sau
  processCaptivePortal();

  Serial.println();
  Serial.println("Dang ket noi Wi-Fi da luu...");

  // CONFIG_AP_PASSWORD rỗng = AP mở (không mật khẩu)
  const bool connected = wifiManager.autoConnect(
    apName.c_str(),
    strlen(CONFIG_AP_PASSWORD) > 0 ? CONFIG_AP_PASSWORD : nullptr
  );

  stopCaptivePortalDns();

  if (!connected) {
    Serial.println("WiFiManager timeout/thoat portal. Khoi dong lai...");
    return false;
  }

  Serial.println("Da ket noi Wi-Fi.");
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");
  Serial.println(WiFi.RSSI());

  return true;
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiDisconnectedSince = 0;
    return;
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

  // Khi mang Wi-Fi cu khong con ton tai (mang thiet bi sang noi khac),
  // restart de WiFiManager mo lai captive portal.
  if (millis() - wifiDisconnectedSince >= WIFI_RESTART_AFTER_MS) {
    Serial.println("Khong reconnect duoc. Restart de mo WiFiManager portal...");
    delay(500);
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

  const String token = responseDoc["deviceToken"] | "";
  const String code = responseDoc["pairingCode"] | "";

  if (token.isEmpty()) {
    Serial.println("Backend khong tra deviceToken.");
    return false;
  }

  saveDeviceToken(token);
  pairingCode = code;
  preferences.putString("pairCode", pairingCode);

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

  if (millis() - lastRegisterAttemptAt < REGISTER_RETRY_INTERVAL_MS) {
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
    Serial.printf("Heartbeat loi HTTP=%d\n", response.code);
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

  // Bật ngắt IR receiver khi bắt đầu học lệnh.
  irReceiver.enableIRIn();
  irReceiver.resume();

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

  // Nếu không ở chế độ học, bỏ capture để tránh remote ngoài ý muốn tạo profile.
  if (!learning.active) {
    irReceiver.resume();
    return;
  }

  Serial.println();
  Serial.println("Da nhan tin hieu remote goc.");
  Serial.print("Protocol: ");
  Serial.println(typeToString(irResults.decode_type));
  Serial.print("Bits: ");
  Serial.println(irResults.bits);
  Serial.println(resultToHumanReadableBasic(&irResults));

  const bool uploaded = uploadLearnedSignal(irResults);

  if (uploaded) {
    irReceiver.disableIRIn();
    const String completedCommandId = learning.commandId;
    learning = LearningSession();
    acknowledgeCommand(
      completedCommandId,
      "completed",
      "IR signal learned and uploaded"
    );
  } else {
    // Giữ learning active để người dùng có thể bấm lại.
    Serial.println("Upload loi. Van tiep tuc cho remote trong thoi gian con lai.");
  }

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

  const bool wasLearning = learning.active;
  if (wasLearning) {
    irReceiver.disableIRIn();
  }
  delay(20);

  const bool sent = universalAc.sendAc(
    desired,
    canUsePrevious ? &previousAcState : nullptr
  );

  delay(20);
  if (wasLearning) {
    irReceiver.enableIRIn();
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
  JsonArray rawArray = command["rawUs"].as<JsonArray>();

  if (rawArray.isNull() || rawArray.size() == 0) {
    errorMessage = "rawUs is empty";
    return false;
  }

  if (rawArray.size() > 1200) {
    errorMessage = "rawUs is too long";
    return false;
  }

  const uint16_t frequencyKhz = command["frequencyKhz"] | 38;
  const size_t count = rawArray.size();

  uint16_t *rawData = new (std::nothrow) uint16_t[count];
  if (rawData == nullptr) {
    errorMessage = "Not enough heap for raw data";
    return false;
  }

  size_t index = 0;
  for (JsonVariant value : rawArray) {
    const uint32_t timing = value.as<uint32_t>();
    rawData[index++] = static_cast<uint16_t>(
      timing > 65535UL ? 65535UL : timing
    );
  }

  const bool wasLearning = learning.active;
  if (wasLearning) {
    irReceiver.disableIRIn();
  }
  delay(20);
  irSender.sendRaw(rawData, count, frequencyKhz);
  delay(20);
  if (wasLearning) {
    irReceiver.enableIRIn();
  }

  delete[] rawData;
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
    String errorMessage;
    const bool ok = sendNativeAcState(command, errorMessage);

    acknowledgeCommand(
      commandId,
      ok ? "completed" : "failed",
      ok ? "Native A/C state sent" : errorMessage
    );
    return;
  }

  if (type == "SEND_RAW") {
    String errorMessage;
    const bool ok = sendRawSignal(command, errorMessage);

    acknowledgeCommand(
      commandId,
      ok ? "completed" : "failed",
      ok ? "Raw IR signal sent" : errorMessage
    );
    return;
  }

  if (type == "RESET_WIFI") {
    acknowledgeCommand(commandId, "completed", "Wi-Fi settings cleared");
    delay(300);
    wifiManager.resetSettings();
    ESP.restart();
    return;
  }

  if (type == "FACTORY_RESET") {
    acknowledgeCommand(commandId, "completed", "Factory reset requested");
    delay(300);
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

  DynamicJsonDocument doc(2048);
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
  delay(800);

  deviceId = buildDeviceId();
  loadPersistentState();

  Serial.println();
  Serial.println("========================================");
  Serial.println("AC CONTROLLER FIRMWARE");
  Serial.print("Firmware: ");
  Serial.println(FW_VERSION);
  Serial.print("Device ID: ");
  Serial.println(deviceId);
  Serial.println("========================================");

  IRac::initState(&previousAcState);

  irSender.begin();
  irReceiver.enableIRIn();
  irReceiver.disableIRIn();
  irReceiver.setUnknownThreshold(12);

  if (!connectWiFiWithManager()) {
    delay(1000);
    ESP.restart();
  }

  if (deviceToken.isEmpty()) {
    registerDevice();
  } else {
    Serial.println("Da co device token trong NVS.");
    Serial.print("Pairing code da luu: ");
    Serial.println(pairingCode);
  }

  lastCommandPollAt = millis();
  lastHeartbeatAt = millis() - HEARTBEAT_INTERVAL_MS;
}

void loop() {
  processCaptivePortal(); // Xử lý DNS/HTTP captive nếu đang ở chế độ AP
  maintainWiFi();
  processIrReceiver();
  processLearningTimeout();

  if (WiFi.status() != WL_CONNECTED) {
    delay(5);
    return;
  }

  ensureDeviceRegistered();

  if (deviceToken.isEmpty()) {
    delay(5);
    return;
  }

  const unsigned long now = millis();

  if (now - lastCommandPollAt >= COMMAND_POLL_INTERVAL_MS) {
    lastCommandPollAt = now;
    pollNextCommand();
  }

  if (now - lastHeartbeatAt >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatAt = now;
    sendHeartbeat();
  }

  delay(5);
}
