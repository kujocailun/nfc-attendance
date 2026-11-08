#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "storage.h"
using namespace std;
#define int long long

const uint8_t sd_sck = 18 , sd_miso = 15 , sd_mosi = 14 , sd_cs = 21;
const uint32_t sd_frequency = 400000;
const size_t probe_size = 1024;
const char probe_path[] = "/attendance_sdcheck_v2.bin";
const char probe_magic[] = "student NFC storage probe v2\n";
SPIClass sd_bus(HSPI);
uint8_t storage_buffer[128];
uint8_t sd_idle[4] , sd_r7[4];
bool storage_ok;
uint8_t storage_type;
uint64_t storage_capacity , storage_total , storage_used;
const char *storage_state = "NOT_INITIALIZED";
const char *storage_probe = "NOT_RUN";

uint8_t probebyte(size_t index)
{
    if (index < sizeof(probe_magic) - 1)
    {
        return static_cast<uint8_t>(probe_magic[index]);
    }
    return static_cast<uint8_t>((index * 37 + 0x5A) & 255);
}

bool verifyprobe()
{
    if (!SD.exists(probe_path))
    {
        storage_probe = "NOT_CREATED";
        return false;
    }
    File file = SD.open(probe_path , FILE_READ);
    if (!file)
    {
        storage_probe = "READ_OPEN_FAILED";
        return false;
    }
    if (file.isDirectory() || file.size() != probe_size)
    {
        file.close();
        storage_probe = "SIZE_OR_TYPE_MISMATCH";
        return false;
    }
    for (size_t offset = 0 ; offset < probe_size ; offset += sizeof(storage_buffer))
    {
        if (file.read(storage_buffer , sizeof(storage_buffer)) != sizeof(storage_buffer))
        {
            file.close();
            storage_probe = "READ_SHORT";
            return false;
        }
        for (size_t i = 0 ; i < sizeof(storage_buffer) ; i ++)
        {
            if (storage_buffer[i] != probebyte(offset + i))
            {
                file.close();
                storage_probe = "CONTENT_MISMATCH";
                return false;
            }
        }
    }
    file.close();
    storage_probe = "PASS";
    return true;
}

void initstorage()
{
    storage_ok = false;
    storage_type = CARD_NONE;
    storage_capacity = storage_total = storage_used = 0;
    storage_probe = "NOT_RUN";
    SD.end();
    sd_bus.end();
    digitalWrite(sd_cs , HIGH);
    pinMode(sd_cs , OUTPUT);
    sd_bus.begin(sd_sck , sd_miso , sd_mosi , sd_cs);
    if (!SD.begin(sd_cs , sd_bus , sd_frequency , "/sd" , 5 , false))
    {
        storage_state = "MOUNT_FAILED";
        return;
    }
    storage_type = SD.cardType();
    if (storage_type == CARD_NONE)
    {
        SD.end();
        storage_state = "NO_CARD";
        return;
    }
    storage_capacity = SD.cardSize();
    storage_total = SD.totalBytes();
    storage_used = SD.usedBytes();
    storage_state = "MOUNTED";
    storage_ok = true;
    verifyprobe();
}

void reportstorage(Stream &out)
{
    out.printf("sd_state=%s [%s]\n" , storage_state , storage_ok ? "PASS" : "FAIL");
    out.printf("sd_spi=HSPI sck=18 miso=15 mosi=14 cs=21 supply=5V frequency_hz=%lu auto_format=OFF\n" ,
               static_cast<unsigned long>(sd_frequency));
    out.printf("sd_type=%u capacity_bytes=%llu filesystem_bytes=%llu used_bytes_snapshot=%llu\n" ,
               static_cast<unsigned>(storage_type) , static_cast<unsigned long long>(storage_capacity) ,
               static_cast<unsigned long long>(storage_total) , static_cast<unsigned long long>(storage_used));
    out.printf("sd_probe=%s expected_bytes=%u path=%s\n" , storage_probe ,
               static_cast<unsigned>(probe_size) , probe_path);
    out.print("Storage diagnostics; attendance status is reported separately.\n");
}

void teststorage(Stream &out)
{
    if (!storage_ok)
    {
        out.print("[SD-TEST] unavailable: card/filesystem not mounted.\n");
        return;
    }
    if (SD.exists(probe_path))
    {
        bool ok = verifyprobe();
        out.printf("[SD-TEST] existing file verify=%s detail=%s; file was not modified.\n" ,
                   ok ? "PASS" : "FAIL" , storage_probe);
        return;
    }
    File file = SD.open(probe_path , FILE_APPEND);
    if (!file)
    {
        storage_probe = "WRITE_OPEN_FAILED";
        out.print("[SD-TEST] WRITE_OPEN_FAILED\n");
        return;
    }
    if (file.isDirectory())
    {
        file.close();
        storage_probe = "CREATE_CONFLICT";
        out.print("[SD-TEST] CREATE_CONFLICT\n");
        return;
    }
    if (!file.seek(0 , SeekEnd))
    {
        file.close();
        storage_probe = "SEEK_FAILED";
        out.print("[SD-TEST] SEEK_FAILED\n");
        return;
    }
    if (file.position() != 0)
    {
        file.close();
        storage_probe = "CREATE_CONFLICT";
        out.print("[SD-TEST] CREATE_CONFLICT\n");
        return;
    }
    for (size_t offset = 0 ; offset < probe_size ; offset += sizeof(storage_buffer))
    {
        for (size_t i = 0 ; i < sizeof(storage_buffer) ; i ++)
        {
            storage_buffer[i] = probebyte(offset + i);
        }
        if (file.write(storage_buffer , sizeof(storage_buffer)) != sizeof(storage_buffer))
        {
            file.close();
            storage_probe = "WRITE_SHORT";
            out.print("[SD-TEST] WRITE_SHORT; incomplete probe file retained.\n");
            return;
        }
    }
    file.flush();
    file.close();
    bool ok = verifyprobe();
    out.printf("[SD-TEST] created_bytes=%u close_reopen_verify=%s detail=%s\n" ,
               static_cast<unsigned>(probe_size) , ok ? "PASS" : "FAIL" , storage_probe);
    out.print("[SD-TEST] all probe handles closed; ready for a power-off persistence test.\n");
}

uint8_t sdrawcommand(uint8_t command , uint32_t argument , uint8_t crc)
{
    digitalWrite(sd_cs , HIGH);
    sd_bus.transfer(0xFF);
    digitalWrite(sd_cs , LOW);
    sd_bus.transfer(0xFF);
    sd_bus.transfer(0x40 | command);
    for (signed shift = 24 ; shift >= 0 ; shift -= 8)
    {
        sd_bus.transfer(static_cast<uint8_t>(argument >> shift));
    }
    sd_bus.transfer(crc);
    uint8_t response = 0xFF;
    for (int i = 0 ; i < 16 ; i ++)
    {
        response = sd_bus.transfer(0xFF);
        if (!(response & 0x80))
        {
            break;
        }
    }
    return response;
}

void diagnosestorage(Stream &out)
{
    if (storage_ok)
    {
        out.print("[SD-DIAG] skipped: filesystem is mounted.\n");
        return;
    }
    SD.end();
    sd_bus.end();
    digitalWrite(sd_cs , HIGH);
    pinMode(sd_cs , OUTPUT);
    sd_bus.begin(sd_sck , sd_miso , sd_mosi , sd_cs);
    sd_bus.beginTransaction(SPISettings(100000 , MSBFIRST , SPI_MODE0));
    for (int i = 0 ; i < 20 ; i ++)
    {
        sd_bus.transfer(0xFF);
    }
    for (int i = 0 ; i < 4 ; i ++)
    {
        sd_idle[i] = sd_bus.transfer(0xFF);
        sd_r7[i] = 0xFF;
    }
    uint8_t idle_response = sdrawcommand(0 , 0 , 0x95);
    uint8_t interface_response = 0xFF;
    if (idle_response == 1)
    {
        interface_response = sdrawcommand(8 , 0x1AA , 0x87);
        if (interface_response <= 1)
        {
            for (int i = 0 ; i < 4 ; i ++)
            {
                sd_r7[i] = sd_bus.transfer(0xFF);
            }
        }
    }
    digitalWrite(sd_cs , HIGH);
    sd_bus.transfer(0xFF);
    sd_bus.endTransaction();
    out.printf("[SD-DIAG] spi_hz=100000 idle_miso=%02X-%02X-%02X-%02X CMD0_R1=0x%02X CMD8_R1=0x%02X R7=%02X-%02X-%02X-%02X\n" ,
               static_cast<unsigned>(sd_idle[0]) , static_cast<unsigned>(sd_idle[1]) ,
               static_cast<unsigned>(sd_idle[2]) , static_cast<unsigned>(sd_idle[3]) ,
               static_cast<unsigned>(idle_response) , static_cast<unsigned>(interface_response) ,
               static_cast<unsigned>(sd_r7[0]) , static_cast<unsigned>(sd_r7[1]) ,
               static_cast<unsigned>(sd_r7[2]) , static_cast<unsigned>(sd_r7[3]));
    out.print("[SD-DIAG] expected CMD0_R1=0x01; SDv2 CMD8_R1=0x01 and R7=00-00-01-AA. No sector write command sent.\n");
    initstorage();
    reportstorage(out);
}

bool storageready()
{
    return storage_ok;
}
