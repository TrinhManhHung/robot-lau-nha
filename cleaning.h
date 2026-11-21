#pragma once
#include <Arduino.h>

// Bo phan ve sinh: 1 relay (PIN_CLEANING_RELAY) dong/cat nguon 12V cho quat hut + 2 choi quet.
// Relay tu bat khi robot dang lam viec (AUTO / MANUAL) va tat khi dung (IDLE),
// nen moi lenh START / STOP / SET_MODE / RETURN_TO_DOCK tu app deu tu dieu khien duoc relay.

void cleaningInit();

// Goi lien tuc trong loop(), sau navUpdate()
void cleaningUpdate();

// Tat ngay lap tuc (bo qua tre / thoi gian toi thieu) - dung khi khan cap
void cleaningForceOff();

bool cleaningIsOn();
