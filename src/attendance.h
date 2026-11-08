#pragma once

#include <Arduino.h>

void initattendance();
void pollattendance();
void checkin(const char *uid , Stream &out);
void reportattendance(Stream &out);
void reportattendanceevent(Stream &out);
void listmembers(Stream &out);
bool attendanceinput(Stream &out , uint8_t port , signed c);
bool attendancehealthy();
const char *attendancestate();
const char *attendancename();
uint32_t attendancechanged();
uint16_t attendancemembers();
uint16_t attendancetoday();
bool attendancemember(uint16_t index , char *uid , char *name);
bool attendancebind(const char *uid , const char *name , Stream &out);
bool attendanceday(const char *date);
bool attendancerecord(char *line , char *stamp , char *uid , char *name);
