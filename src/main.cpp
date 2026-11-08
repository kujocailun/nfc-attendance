#include <Arduino.h>
#include <HWCDC.h>
#include <esp_system.h>
#include <soc/esp32s3/spiram.h>
#include "nfc.h"
#include "display.h"
#include "clock.h"
#include "feedback.h"
#include "button.h"
#include "storage.h"
#include "attendance.h"
#include "network.h"
using namespace std;
#define int long long

const uint32_t flash_expected = 16U * 1024 * 1024;
const uint32_t psram_expected = 8U * 1024 * 1024;
const size_t test_size = 4096;
volatile uint8_t *a;
bool ram_ok;
uint32_t last_report;
uint32_t handled_count;

bool checkram()
{
    if (!psramFound() || esp_spiram_get_size() != psram_expected)
    {
        return false;
    }
    a = static_cast<uint8_t *>(ps_malloc(test_size));
    if (!a)
    {
        return false;
    }
    bool ok = true;
    for (int k = 0 ; k < 2 ; k ++)
    {
        for (size_t i = 0 ; i < test_size ; i ++)
        {
            a[i] = static_cast<uint8_t>((i * 37 + k * 91) & 255);
        }
        for (size_t i = 0 ; i < test_size ; i ++)
        {
            if (a[i] != static_cast<uint8_t>((i * 37 + k * 91) & 255))
            {
                ok = false;
            }
        }
    }
    free(const_cast<uint8_t *>(a));
    a = nullptr;
    return ok;
}

void report(Stream &out)
{
    out.print("\n[NFC-ATTENDANCE] stage=10-remote-management version=0.10.0\n");
    out.printf("chip=%s cores=%u\n" , ESP.getChipModel() ,
               static_cast<unsigned>(ESP.getChipCores()));
    out.printf("sdk=%s reset_reason=%u\n" , ESP.getSdkVersion() ,
               static_cast<unsigned>(esp_reset_reason()));
    out.printf("flash_bytes=%lu expected=%lu [%s]\n" ,
               static_cast<unsigned long>(ESP.getFlashChipSize()) ,
               static_cast<unsigned long>(flash_expected) ,
               ESP.getFlashChipSize() == flash_expected ? "PASS" : "FAIL");
    out.printf("psram_physical_bytes=%lu expected=%lu [%s]\n" ,
               static_cast<unsigned long>(psramFound() ? esp_spiram_get_size() : 0) ,
               static_cast<unsigned long>(psram_expected) ,
               psramFound() && esp_spiram_get_size() == psram_expected ? "PASS" : "FAIL");
    out.printf("psram_sample_bytes=%u patterns=2 [%s]\n" ,
               static_cast<unsigned>(test_size) , ram_ok ? "PASS" : "FAIL");
    out.printf("free_heap_bytes=%lu psram_heap_bytes=%lu free_psram_bytes=%lu uptime_ms=%lu\n" ,
               static_cast<unsigned long>(ESP.getFreeHeap()) ,
               static_cast<unsigned long>(ESP.getPsramSize()) ,
               static_cast<unsigned long>(ESP.getFreePsram()) ,
               static_cast<unsigned long>(millis()));
    reportnfc(out);
    reportdisplay(out);
    reportclock(out);
    reportfeedback(out);
    reportbutton(out);
    reportstorage(out);
    reportattendance(out);
    reportnetwork(out);
    out.print("Send r to report, i to retry PN532, b to probe RTC, f to test lights/sound, d to retry SD, s to test SD, g for SD diagnostics.\n");
}

void readcmd(Stream &out , uint8_t port)
{
    for (int i = 0 ; i < 32 && out.available() ; i ++)
    {
        int c = out.read();
        if (attendanceinput(out , port , static_cast<signed>(c)))
        {
            continue;
        }
        if (clockinput(out , port , static_cast<signed>(c)))
        {
            continue;
        }
        if (c == 'r' || c == 'R')
        {
            report(out);
        }
        else if ((c == 'i' || c == 'I') && ram_ok)
        {
            initnfc();
            report(out);
        }
        else if ((c == 'b' || c == 'B') && ram_ok)
        {
            diagnoseclock(out);
        }
        else if (c == 'f' || c == 'F')
        {
            testfeedback(out);
        }
        else if ((c == 'd' || c == 'D') && ram_ok)
        {
            initstorage();
            initattendance();
            reportstorage(out);
            reportattendance(out);
        }
        else if (c == 's' || c == 'S')
        {
            teststorage(out);
        }
        else if (c == 'g' || c == 'G')
        {
            diagnosestorage(out);
        }
        else if (c == 'l' || c == 'L')
        {
            listmembers(out);
        }
    }
}

void sol()
{
    readcmd(Serial , 0);
    readcmd(USBSerial , 1);
    pollclock();
    pollattendance();
    pollnfc();
    if (cardcount() != handled_count)
    {
        handled_count = cardcount();
        checkin(carduid() , Serial);
        if (USBSerial)
        {
            reportattendanceevent(USBSerial);
        }
    }
    polldisplay();
    pollbutton(Serial);
    pollnetwork();
    uint32_t now = millis();
    if (now - last_report >= 5000)
    {
        last_report = now;
        report(Serial);
        if (USBSerial)
        {
            report(USBSerial);
        }
    }
    delay(10);
}

void setup()
{
    initfeedback();
    initbutton();
    Serial.begin(115200);
    USBSerial.begin(115200);
    USBSerial.setTxTimeoutMs(20);
    delay(300);
    ram_ok = checkram();
    if (ram_ok && ESP.getFlashChipSize() == flash_expected)
    {
        initnfc();
        initclock();
        initdisplay();
        initstorage();
        initattendance();
        initnetwork();
    }
    report(Serial);
    if (USBSerial)
    {
        report(USBSerial);
    }
    last_report = millis();
}

void loop()
{
    sol();
}
