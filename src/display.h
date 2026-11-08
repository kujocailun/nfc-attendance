#pragma once

#include <Arduino.h>

void initdisplay();
void polldisplay();
void reportdisplay(Stream &out);
bool displayready();
