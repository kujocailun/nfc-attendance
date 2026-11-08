#pragma once

#include <Arduino.h>

void initclock();
void pollclock();
void reportclock(Stream &out);
void diagnoseclock(Stream &out);
bool clockinput(Stream &out , uint8_t port , signed c);
bool clockvalid();
const char *clocktext();
bool clocksettext(const char *stamp , Stream &out);
