#ifndef NFC_ATTENDANCE_BUTTON_H
#define NFC_ATTENDANCE_BUTTON_H

#include <Arduino.h>

void initbutton();
void pollbutton(Stream &out);
void reportbutton(Stream &out);
bool buttonpressed();

#endif
