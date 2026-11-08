#include <Arduino.h>
#include <HWCDC.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include "nfc.h"
using namespace std;
#define int long long

const uint8_t sck_pin = 12;
const uint8_t miso_pin = 13;
const uint8_t mosi_pin = 11;
const uint8_t ss_pin = 10;
Adafruit_PN532 nfc(ss_pin , &SPI);
uint8_t uid[32] , old_uid[32];
uint8_t uid_len , old_len;
uint32_t firmware , last_seen , card_count;
uint32_t last_event;
char uid_text[21];
bool nfc_ok;
const char *nfc_state = "NOT_INITIALIZED";

void initnfc()
{
    nfc_ok = false;
    firmware = 0;
    old_len = 0;
    SPI.begin(sck_pin , miso_pin , mosi_pin , ss_pin);
    if (!nfc.begin())
    {
        nfc_state = "SPI_BEGIN_FAILED";
        return;
    }
    firmware = nfc.getFirmwareVersion();
    if ((firmware >> 24) != 0x32)
    {
        nfc_state = "NO_PN532_RESPONSE";
        return;
    }
    if (!nfc.SAMConfig())
    {
        nfc_state = "SAM_CONFIG_FAILED";
        return;
    }
    if (!nfc.setPassiveActivationRetries(0x00))
    {
        nfc_state = "RETRY_CONFIG_FAILED";
        return;
    }
    nfc_state = "READY";
    nfc_ok = true;
}

void reportnfc(Stream &out)
{
    out.printf("pn532_state=%s [%s]\n" , nfc_state , nfc_ok ? "PASS" : "FAIL");
    out.printf("pn532_spi sck=%u miso=%u mosi=%u ss=%u switch1=OFF switch2=ON\n" ,
               static_cast<unsigned>(sck_pin) , static_cast<unsigned>(miso_pin) ,
               static_cast<unsigned>(mosi_pin) , static_cast<unsigned>(ss_pin));
    if (firmware)
    {
        out.printf("pn532_ic=0x%02X firmware=%u.%u support=0x%02X\n" ,
                   static_cast<unsigned>((firmware >> 24) & 255) ,
                   static_cast<unsigned>((firmware >> 16) & 255) ,
                   static_cast<unsigned>((firmware >> 8) & 255) ,
                   static_cast<unsigned>(firmware & 255));
    }
    out.printf("card_events=%lu\n" ,
               static_cast<unsigned long>(card_count));
    if (nfc_ok)
    {
        out.print("Present one ISO14443A test card; remove for 1 second before tapping again.\n");
    }
    else
    {
        out.print("Power off before checking wires and switches; then reconnect COM USB.\n");
    }
}

void reportcard(Stream &out)
{
    out.printf("[CARD] event=%lu uid_bytes=%u uid=" ,
               static_cast<unsigned long>(card_count) ,
               static_cast<unsigned>(uid_len));
    for (int i = 0 ; i < uid_len ; i ++)
    {
        if (i)
        {
            out.print("-");
        }
        out.printf("%02X" , static_cast<unsigned>(uid[i]));
    }
    out.print("\n");
}

void pollnfc()
{
    if (!nfc_ok)
    {
        return;
    }
    uid_len = 0;
    bool found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A , uid , &uid_len , 300);
    if (!found || (uid_len != 4 && uid_len != 7))
    {
        return;
    }
    uint32_t now = millis();
    bool same = uid_len == old_len && memcmp(uid , old_uid , uid_len) == 0;
    bool fresh = !same || now - last_seen >= 1000;
    last_seen = now;
    if (!fresh)
    {
        return;
    }
    memcpy(old_uid , uid , uid_len);
    old_len = uid_len;
    card_count ++;
    last_event = now;
    for (int i = 0 ; i < uid_len ; i ++)
    {
        snprintf(uid_text + i * 3 , sizeof(uid_text) - i * 3 ,
                 i + 1 < uid_len ? "%02X-" : "%02X" , static_cast<unsigned>(uid[i]));
    }
    reportcard(Serial);
    if (USBSerial)
    {
        reportcard(USBSerial);
    }
}

bool nfcready()
{
    return nfc_ok;
}

uint32_t cardcount()
{
    return card_count;
}

uint32_t cardtime()
{
    return last_event;
}

const char *carduid()
{
    return uid_text;
}
