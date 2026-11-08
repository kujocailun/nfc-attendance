#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_I2CDevice.h>
#include "clock.h"
using namespace std;
#define int long long

const uint8_t rtc_sda = 16;
const uint8_t rtc_scl = 17;
TwoWire clock_bus(1);
RTC_DS3231 rtc;
Adafruit_I2CDevice clock_dev(0x68 , &clock_bus);
uint8_t clock_data[19];
uint8_t time_fields[5] , control_command[2];
char clock_stamp[32] = "RTC NOT CONNECTED";
char clock_cmd[2][15];
uint8_t clock_len[2];
bool clock_active[2] , clock_bad[2];
uint32_t clock_input_at[2];
bool clock_connected , clock_ok;
bool clock_bus_started;
uint32_t clock_polled;
DateTime clock_now(2000 , 1 , 1);
const char *clock_state = "NOT_INITIALIZED";

bool validdate(uint16_t y , uint8_t m , uint8_t d , uint8_t h , uint8_t n , uint8_t s)
{
    if (y < 2000 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31 ||
        h > 23 || n > 59 || s > 59)
    {
        return false;
    }
    return DateTime(y , m , d , h , n , s).isValid();
}

signed unbcd(uint8_t x)
{
    if ((x & 15) > 9 || (x >> 4) > 9)
    {
        return -1;
    }
    return (x >> 4) * 10 + (x & 15);
}

void readclock()
{
    clock_ok = false;
    uint8_t reg = 0;
    if (!clock_dev.write_then_read(&reg , 1 , clock_data , sizeof(clock_data)))
    {
        clock_state = "I2C_READ_FAILED";
        return;
    }
    if (clock_data[15] & 0x80)
    {
        clock_state = "NEEDS_TIME_SYNC";
        return;
    }
    if (clock_data[14] & 0x80)
    {
        clock_state = "BACKUP_OSCILLATOR_DISABLED";
        return;
    }
    int s = unbcd(clock_data[0]);
    int n = unbcd(clock_data[1]);
    int h = unbcd(clock_data[2] & 0x3F);
    if (clock_data[2] & 0x40)
    {
        int hour12 = unbcd(clock_data[2] & 0x1F);
        h = hour12 >= 1 && hour12 <= 12 ? hour12 % 12 + ((clock_data[2] & 0x20) ? 12 : 0) : -1;
    }
    int d = unbcd(clock_data[4]);
    int m = unbcd(clock_data[5] & 0x7F);
    int y = unbcd(clock_data[6]);
    if (s < 0 || n < 0 || h < 0 || d < 0 || m < 0 || y < 0 ||
        (clock_data[5] & 0x80) || !validdate(2000 + y , m , d , h , n , s))
    {
        clock_state = "INVALID_CALENDAR";
        return;
    }
    clock_now = DateTime(2000 + y , m , d , h , n , s);
    snprintf(clock_stamp , sizeof(clock_stamp) , "%04u-%02u-%02u %02u:%02u:%02u" ,
             static_cast<unsigned>(clock_now.year()) , static_cast<unsigned>(clock_now.month()) ,
             static_cast<unsigned>(clock_now.day()) , static_cast<unsigned>(clock_now.hour()) ,
             static_cast<unsigned>(clock_now.minute()) , static_cast<unsigned>(clock_now.second()));
    clock_state = "VALID";
    clock_ok = true;
}

void initclock()
{
    clock_connected = false;
    clock_ok = false;
    if (!clock_bus.begin(rtc_sda , rtc_scl , 100000))
    {
        clock_state = "I2C_BEGIN_FAILED";
        return;
    }
    clock_bus_started = true;
    clock_bus.setTimeOut(50);
    if (!rtc.begin(&clock_bus) || !clock_dev.begin())
    {
        clock_state = "NO_DS3231_ACK";
        return;
    }
    clock_connected = true;
    readclock();
    clock_polled = millis();
}

void diagnoseclock(Stream &out)
{
    initclock();
    if (!clock_bus_started)
    {
        reportclock(out);
        return;
    }
    out.printf("[RTC-BUS] sda_gpio=%u level=%u scl_gpio=%u level=%u\n" ,
               static_cast<unsigned>(rtc_sda) , static_cast<unsigned>(digitalRead(rtc_sda)) ,
               static_cast<unsigned>(rtc_scl) , static_cast<unsigned>(digitalRead(rtc_scl)));
    uint16_t found = 0;
    for (uint8_t address = 0x08 ; address < 0x78 ; address ++)
    {
        clock_bus.beginTransmission(address);
        uint8_t result = clock_bus.endTransmission();
        if (!result)
        {
            found ++;
            out.printf("[RTC-BUS] address=0x%02X ACK\n" , static_cast<unsigned>(address));
        }
    }
    out.printf("[RTC-BUS] devices=%u\n" , static_cast<unsigned>(found));
    reportclock(out);
}

void pollclock()
{
    uint32_t now = millis();
    if (clock_connected && now - clock_polled >= 500)
    {
        clock_polled = now;
        readclock();
    }
}

bool setclock(Stream &out , uint8_t port)
{
    if (clock_bad[port] || clock_len[port] != 14)
    {
        out.print("[RTC-SYNC] INVALID_COMMAND\n");
        return false;
    }
    uint16_t y = 0;
    for (int i = 0 ; i < 4 ; i ++)
    {
        y = y * 10 + clock_cmd[port][i] - '0';
    }
    for (int i = 0 ; i < 5 ; i ++)
    {
        time_fields[i] = (clock_cmd[port][4 + i * 2] - '0') * 10 + clock_cmd[port][5 + i * 2] - '0';
    }
    if (!validdate(y , time_fields[0] , time_fields[1] , time_fields[2] , time_fields[3] , time_fields[4]))
    {
        out.print("[RTC-SYNC] INVALID_DATE\n");
        return false;
    }
    if (!clock_connected)
    {
        out.print("[RTC-SYNC] RTC_NOT_CONNECTED\n");
        return false;
    }
    uint8_t reg = 0x0E;
    uint8_t ctrl;
    if (!clock_dev.write_then_read(&reg , 1 , &ctrl , 1))
    {
        out.print("[RTC-SYNC] I2C_READ_FAILED\n");
        return false;
    }
    control_command[0] = reg;
    control_command[1] = ctrl & 0x7F;
    if (!clock_dev.write(control_command , sizeof(control_command)))
    {
        out.print("[RTC-SYNC] I2C_WRITE_FAILED\n");
        return false;
    }
    DateTime target(y , time_fields[0] , time_fields[1] , time_fields[2] , time_fields[3] , time_fields[4]);
    rtc.adjust(target);
    readclock();
    if (!clock_ok || clock_now.unixtime() - target.unixtime() > 1)
    {
        out.print("[RTC-SYNC] VERIFY_FAILED\n");
        return false;
    }
    out.printf("[RTC-SYNC] time=%s timezone=UTC+08:00 [PASS]\n" , clock_stamp);
    return true;
}

bool clockinput(Stream &out , uint8_t port , signed c)
{
    if (port >= 2)
    {
        return false;
    }
    uint32_t now = millis();
    if (clock_active[port] && now - clock_input_at[port] > 5000)
    {
        clock_active[port] = false;
        out.print("[RTC-SYNC] COMMAND_TIMEOUT\n");
    }
    if (!clock_active[port])
    {
        if (c != 't' && c != 'T')
        {
            return false;
        }
        clock_active[port] = true;
        clock_bad[port] = false;
        clock_len[port] = 0;
        clock_input_at[port] = now;
        return true;
    }
    if (c == '\r')
    {
        return true;
    }
    if (c == '\n')
    {
        clock_active[port] = false;
        setclock(out , port);
        return true;
    }
    if (c < '0' || c > '9' || clock_len[port] >= 14)
    {
        clock_bad[port] = true;
        return true;
    }
    clock_cmd[port][clock_len[port] ++] = c;
    return true;
}

void reportclock(Stream &out)
{
    out.printf("rtc_state=%s [%s]\n" , clock_state , clock_ok ? "PASS" : "FAIL");
    out.printf("rtc_bus=1 address=0x68 sda=%u scl=%u timezone=UTC+08:00\n" ,
               static_cast<unsigned>(rtc_sda) , static_cast<unsigned>(rtc_scl));
    if (clock_ok)
    {
        out.printf("rtc_local=%s\n" , clock_stamp);
    }
    out.print("RTC sync command: TYYYYMMDDhhmmss followed by newline, China local time.\n");
}

bool clockvalid()
{
    return clock_ok;
}

const char *clocktext()
{
    return clock_ok ? clock_stamp : (clock_connected ? "CLOCK NEEDS SYNC" : "RTC OFFLINE");
}

bool clocksettext(const char *stamp , Stream &out)
{
    if (strlen(stamp) != 19 || stamp[4] != '-' || stamp[7] != '-' ||
        stamp[10] != ' ' || stamp[13] != ':' || stamp[16] != ':')
    {
        return false;
    }
    clock_bad[0] = false;
    clock_len[0] = 0;
    for (size_t i = 0 ; i < 19 ; i ++)
    {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16)
        {
            continue;
        }
        if (stamp[i] < '0' || stamp[i] > '9')
        {
            return false;
        }
        clock_cmd[0][clock_len[0] ++] = stamp[i];
    }
    return setclock(out , 0);
}
