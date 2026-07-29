#include <Arduino.h>

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <IRac.h>

// HX1838 OUT nối GPIO27
const uint16_t IR_RECEIVE_PIN = 27;

// Remote điều hòa có frame dài nên dùng buffer lớn
const uint16_t IR_BUFFER_SIZE = 2048;

// Đổi tên để không trùng macro TIMEOUT_MS của thư viện
const uint8_t IR_CAPTURE_TIMEOUT_MS = 80;

IRrecv irrecv(
  IR_RECEIVE_PIN,
  IR_BUFFER_SIZE,
  IR_CAPTURE_TIMEOUT_MS,
  true
);

decode_results results;

void printHexState(const decode_results &data) {
  if (data.bits == 0 || data.state == nullptr) {
    return;
  }

  // Số byte được làm tròn lên từ số bit
  uint16_t byteCount = (data.bits + 7) / 8;

  Serial.print("State bytes: ");

  for (uint16_t i = 0; i < byteCount; i++) {
    Serial.printf("0x%02X", data.state[i]);

    if (i < byteCount - 1) {
      Serial.print(", ");
    }
  }

  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  irrecv.enableIRIn();

  Serial.println();
  Serial.println("======================================");
  Serial.println("ESP32 Universal IR A/C Receiver");
  Serial.println("HX1838 OUT -> GPIO27");
  Serial.println("Dang cho tin hieu remote...");
  Serial.println("======================================");
}

void loop() {
  if (!irrecv.decode(&results)) {
    return;
  }

  Serial.println();
  Serial.println("========== IR RECEIVED ==========");

  // Thư viện tự động xác định protocol
  Serial.print("Protocol: ");
  Serial.println(typeToString(results.decode_type));

  Serial.print("Bits: ");
  Serial.println(results.bits);

  Serial.print("Code: ");
  Serial.println(resultToHexidecimal(&results));

  /*
   * Hàm này tự động dựa vào results.decode_type để chọn
   * bộ giải mã điều hòa tương ứng:
   * Daikin, Gree, Electra, Panasonic, Mitsubishi...
   */
  String acDescription = IRAcUtils::resultAcToString(&results);

  if (acDescription.length() > 0) {
    Serial.println();
    Serial.println("--- A/C state decoded automatically ---");
    Serial.println(acDescription);
  } else {
    Serial.println();
    Serial.println("--- Khong giai ma duoc trang thai chi tiet ---");
    Serial.println(
      "Protocol co the chi duoc nhan dien song chua ho tro "
      "giai ma day du cac truong A/C."
    );
  }

  // In dữ liệu từng byte nếu frame có state
  Serial.println();
  printHexState(results);

  // Luôn in raw/source code để dự phòng
  Serial.println();
  Serial.println("--- Raw data / Source code ---");
  Serial.println(resultToSourceCode(&results));

  Serial.println("=================================");
  Serial.println("Cho tin hieu tiep theo...");

  irrecv.resume();
}