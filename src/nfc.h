#pragma once

#include <Arduino.h>

void initnfc();
void pollnfc();
void reportnfc(Stream &out);
bool nfcready();
uint32_t cardcount();
uint32_t cardtime();
const char *carduid();
