#pragma once

#include <Arduino.h>

void initnetwork();
void pollnetwork();
void reportnetwork(Stream &out);
void networkview(char *state , char *ssid , char *password , bool &portal);
