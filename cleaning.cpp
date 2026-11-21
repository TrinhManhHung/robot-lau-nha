#include "cleaning.h"
#include "config.h"
#include "types.h"
#include "navigation.h"

namespace {

bool relayOn = false;
uint32_t lastSwitchMs = 0;
// Thoi diem robot bat dau can ve sinh (0 = khong can)
uint32_t wantSinceMs = 0;

const int LEVEL_ON  = CLEANING_RELAY_ACTIVE_LEVEL;
const int LEVEL_OFF = CLEANING_RELAY_ACTIVE_LEVEL == HIGH ? LOW : HIGH;

void writeRelay(bool on) {
  if (on == relayOn) return;
  relayOn = on;
  lastSwitchMs = millis();
  digitalWrite(PIN_CLEANING_RELAY, on ? LEVEL_ON : LEVEL_OFF);
  Serial.printf("[CLEAN] Quat hut + choi: %s\n", on ? "BAT" : "TAT");
}

bool shouldClean() {
  switch (navGetMode()) {
    case MODE_AUTO:   return true;
    case MODE_MANUAL: return CLEANING_IN_MANUAL;
    default:          return false;
  }
}

}  // namespace

void cleaningInit() {
  // Ghi muc TAT truoc khi chuyen sang OUTPUT -> relay khong bi dong thoang qua luc khoi dong
  digitalWrite(PIN_CLEANING_RELAY, LEVEL_OFF);
  pinMode(PIN_CLEANING_RELAY, OUTPUT);
  digitalWrite(PIN_CLEANING_RELAY, LEVEL_OFF);
  relayOn = false;
  // Cho phep bat ngay lan dau, khong phai doi CLEANING_MIN_OFF_MS
  lastSwitchMs = millis() - CLEANING_MIN_OFF_MS;
}

void cleaningUpdate() {
  uint32_t now = millis();

  if (!shouldClean()) {
    wantSinceMs = 0;
    writeRelay(false);  // Tat luon khong cho: robot dung thi choi/quat dung theo
    return;
  }

  if (wantSinceMs == 0) wantSinceMs = now ? now : 1;
  if (relayOn) return;

  // Bat tre mot chut sau banh xe: tranh 2 dong khoi dong lon cung luc lam sut ap -> ESP32 reset
  if (now - wantSinceMs < CLEANING_ON_DELAY_MS) return;
  // Vua tat xong khong bat lai ngay (bao ve tiep diem relay khi bam START/STOP lien tuc)
  if (now - lastSwitchMs < CLEANING_MIN_OFF_MS) return;

  writeRelay(true);
}

void cleaningForceOff() {
  wantSinceMs = 0;
  writeRelay(false);
}

bool cleaningIsOn() {
  return relayOn;
}
