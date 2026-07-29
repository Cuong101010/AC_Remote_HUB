#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

const uint16_t IR_LED_PIN = 4;
const uint8_t BUTTON_PIN = 18;

IRsend irsend(IR_LED_PIN);

uint8_t stateOn[13] = {
  0xC3, 0x77, 0xE0, 0x00, 0x60, 0x00, 0x20, 0x00, 0x00, 0x20, 0x00, 0x05, 0xBF
};

bool lastButtonState = HIGH;

void setup() {
  Serial.begin(115200);

  irsend.begin();

  // Không nhấn: HIGH
  // Nhấn: LOW
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.println();
  Serial.println("Nhan nut de phat lenh bat dieu hoa.");
}

void loop() {
  bool currentButtonState = digitalRead(BUTTON_PIN);

  // Chỉ phát tại thời điểm nút chuyển từ thả sang nhấn
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    delay(30);  // Chống dội phím

    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("Dang phat lenh ON...");

      irsend.sendElectraAC(
        stateOn,
        sizeof(stateOn)
      );

      Serial.println("Da phat lenh ON.");

      // Chờ người dùng thả nút để không phát liên tục
      while (digitalRead(BUTTON_PIN) == LOW) {
        delay(10);
      }
    }
  }

  lastButtonState = currentButtonState;

  delay(5);
}