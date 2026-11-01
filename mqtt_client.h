#pragma once
#include <Arduino.h>

// Ket noi MQTT toi backend: nhan lenh dieu khien, gui telemetry.
// Goi sau wifiInit(). onCommand duoc goi moi khi ap dung xong 1 lenh tu backend.
void mqttInit(void (*onCommand)());

// Goi lien tuc trong loop()
void mqttUpdate();

bool mqttConnected();
