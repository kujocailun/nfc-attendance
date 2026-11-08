#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <cstring>
#include "display.h"
#include "nfc.h"
#include "clock.h"
#include "storage.h"
#include "attendance.h"
#include "network.h"
using namespace std;
#define int long long

const uint8_t sda_pin = 8;
const uint8_t scl_pin = 9;
Adafruit_SH1106G oled(128 , 64 , &Wire , -1 , 100000 , 100000);
uint8_t oled_addr , probe_3c , probe_3d;
bool oled_ok;
uint32_t painted_count = UINT32_MAX;
uint32_t painted_second = UINT32_MAX;
const char *oled_state = "NOT_INITIALIZED";

void initdisplay()
{
    oled_ok = false;
    if (!Wire.begin(sda_pin , scl_pin , 100000))
    {
        oled_state = "I2C_BEGIN_FAILED";
        return;
    }
    Wire.setTimeOut(50);
    Wire.beginTransmission(0x3C);
    probe_3c = Wire.endTransmission();
    Wire.beginTransmission(0x3D);
    probe_3d = Wire.endTransmission();
    if (!probe_3c)
    {
        oled_addr = 0x3C;
    }
    else if (!probe_3d)
    {
        oled_addr = 0x3D;
    }
    else
    {
        oled_state = "NO_OLED_ACK";
        return;
    }
    if (!oled.begin(oled_addr , false))
    {
        oled_state = "OLED_BEGIN_FAILED";
        return;
    }
    oled.setContrast(0x7F);
    oled.setTextColor(SH110X_WHITE);
    oled.setTextSize(1);
    oled.setTextWrap(false);
    oled_state = "INITIALIZED";
    oled_ok = true;
    polldisplay();
}

void polldisplay()
{
    if (!oled_ok)
    {
        return;
    }
    uint32_t count = cardcount();
    uint32_t now = millis();
    uint32_t second = now / 1000;
    if (count == painted_count && second == painted_second)
    {
        return;
    }
    painted_count = count;
    painted_second = second;
    oled.clearDisplay();
    char net_state_text[32] , net_ssid_text[24] , net_password_text[17];
    bool portal;
    networkview(net_state_text , net_ssid_text , net_password_text , portal);
    if (portal && now - attendancechanged() >= 5000)
    {
        oled.setCursor(0 , 0);
        oled.print("WI-FI SETUP (10 MIN)");
        oled.setCursor(0 , 12);
        oled.print(net_ssid_text);
        oled.setCursor(0 , 24);
        oled.print("PW: ");
        oled.print(net_password_text);
        oled.setCursor(0 , 36);
        oled.print("192.168.4.1");
        oled.setCursor(0 , 48);
        oled.print("2.4G HOTSPOT / WPA2");
        oled.display();
        return;
    }
    oled.drawRect(0 , 0 , 128 , 64 , SH110X_WHITE);
    oled.setCursor(4 , 4);
    oled.print("NFC TERMINAL");
    if (second & 1)
    {
        oled.fillCircle(120 , 7 , 2 , SH110X_WHITE);
    }
    oled.drawFastHLine(1 , 15 , 126 , SH110X_WHITE);
    oled.setCursor(4 , 18);
    if (!nfcready())
    {
        oled.print("PN532 OFFLINE");
    }
    else if (!storageready())
    {
        oled.print("SD OFFLINE");
    }
    else if (!attendancehealthy())
    {
        oled.print("REGISTRY ERROR");
    }
    else if (!clockvalid())
    {
        oled.print("CLOCK ERROR");
    }
    else if (now - attendancechanged() < 5000 || !strcmp(attendancestate() , "LOG_ERROR"))
    {
        const char *state = attendancestate();
        if (!strcmp(state , "SAVED"))
        {
            oled.print("CHECK-IN SAVED");
        }
        else if (!strcmp(state , "ALREADY_TODAY"))
        {
            oled.print("ALREADY CHECKED IN");
        }
        else if (!strcmp(state , "ENROLLED"))
        {
            oled.print("CARD ENROLLED");
        }
        else if (!strcmp(state , "UNKNOWN_CARD"))
        {
            oled.print("UNKNOWN CARD");
        }
        else if (!strcmp(state , "WRITE_FAILED") || !strcmp(state , "LOG_ERROR"))
        {
            oled.print("SAVE ERROR");
        }
        else
        {
            oled.print("READY - TAP A CARD");
        }
    }
    else
    {
        oled.print("READY - TAP A CARD");
    }
    oled.setCursor(4 , 29);
    if (now - attendancechanged() < 5000 && strcmp(attendancestate() , "READY"))
    {
        const char *name = attendancename();
        bool ascii = name[0] != 0;
        for (size_t i = 0 ; name[i] ; i ++)
        {
            ascii &= static_cast<uint8_t>(name[i]) < 128;
        }
        if (ascii)
        {
            for (size_t i = 0 ; name[i] && i < 20 ; i ++)
            {
                oled.write(static_cast<uint8_t>(name[i]));
            }
        }
        else
        {
            oled.print(count ? carduid() : "Card registered");
        }
    }
    else
    {
        oled.printf("Members: %u" , static_cast<unsigned>(attendancemembers()));
    }
    oled.setCursor(4 , 40);
    oled.print(clocktext());
    oled.setCursor(4 , 51);
    oled.printf("Today:%u %s" , static_cast<unsigned>(attendancetoday()) ,
                !strcmp(net_state_text , "ONLINE") ? "ONLINE" : "OFFLINE");
    oled.display();
}

void reportdisplay(Stream &out)
{
    out.printf("oled_state=%s [%s]\n" , oled_state , oled_ok ? "PASS" : "FAIL");
    out.printf("oled_driver=SH1106G size=128x64 i2c_sda=%u i2c_scl=%u address=0x%02X\n" ,
               static_cast<unsigned>(sda_pin) , static_cast<unsigned>(scl_pin) ,
               static_cast<unsigned>(oled_addr));
    out.printf("oled_probe_3c=%u oled_probe_3d=%u (0=ACK)\n" ,
               static_cast<unsigned>(probe_3c) , static_cast<unsigned>(probe_3d));
}

bool displayready()
{
    return oled_ok;
}
