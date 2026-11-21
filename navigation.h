#pragma once
#include "types.h"

void navInit();

// Goi lien tuc trong loop(), sau sensorsUpdate()
void navUpdate();

void navStartAuto();
void navStop();
// timeoutMs > 0: lenh tu het hieu luc (dung banh) neu khong duoc goi lai trong khoang nay.
// Dung cho dieu khien tu xa kieu nhan giu. 0 = giu lenh cho den khi co lenh khac.
void navManual(ManualCmd cmd, uint32_t timeoutMs = 0);

RobotMode navGetMode();
NavState navGetState();
const char* navStateName(NavState s);
const char* navModeName(RobotMode m);
