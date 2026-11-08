#pragma once

#include <Arduino.h>

void initfeedback();
void cardfeedback(bool success);
void duplicatefeedback();
void testfeedback(Stream &out);
void reportfeedback(Stream &out);
