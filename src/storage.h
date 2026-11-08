#pragma once

#include <Arduino.h>

void initstorage();
void reportstorage(Stream &out);
void teststorage(Stream &out);
void diagnosestorage(Stream &out);
bool storageready();
