#include "wifi_manager.h"
#include <WiFi.h>
#include "config.h"
#include "secrets.h"

namespace {

bool wasConnected = false;
uint32_t lastBeginMs = 0;

}  // namespace

void wifiInit() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastBeginMs = millis();
  Serial.printf("[WIFI] Dang ket noi \"%s\"...\n", WIFI_SSID);
}

void wifiUpdate() {
  bool connected = WiFi.status() == WL_CONNECTED;
  uint32_t now = millis();

  if (connected && !wasConnected) {
    Serial.printf("[WIFI] Da ket noi, IP %s, RSSI %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (!connected && wasConnected) {
    Serial.println("[WIFI] Mat ket noi");
    lastBeginMs = now;
  }
  wasConnected = connected;

  // Tu dong ket noi lai cua ESP32 doi khi bi ket -> thu lai thu cong
  if (!connected && now - lastBeginMs >= WIFI_RETRY_MS) {
    lastBeginMs = now;
    Serial.println("[WIFI] Thu ket noi lai...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}
