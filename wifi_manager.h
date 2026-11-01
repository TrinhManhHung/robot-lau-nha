#pragma once
#include <Arduino.h>

void wifiInit();

// Goi lien tuc trong loop(). Khong chan: mat ket noi thi tu thu lai.
void wifiUpdate();

bool wifiConnected();
