#include <cassert>
#include <cstdio>
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "../../src/storage.cpp"
using namespace std;

map<string , vector<uint8_t>> host_files;
bool fail_mount , fail_read_open , fail_write_open , short_read , short_write , host_directory;
bool stale_new_size , fail_seek , create_race;
uint8_t host_card_type = CARD_SDHC;
signed host_handles , host_write_opens;
SDFS SD;
Stream output;
vector<uint8_t> saved;
vector<uint8_t> host_rx , host_tx;
size_t host_rx_at;
bool host_cs = true;

void digitalWrite(signed pin , signed value)
{
    assert(pin == 21 && (value == HIGH || value == LOW));
    host_cs = value;
}

void pinMode(signed pin , signed mode)
{
    assert(pin == 21 && mode == OUTPUT);
}

void reset()
{
    assert(host_handles == 0);
    host_files.clear();
    host_files["/keep.txt"] = {1 , 2 , 3 , 4};
    fail_mount = fail_read_open = fail_write_open = short_read = short_write = host_directory = false;
    stale_new_size = fail_seek = create_race = false;
    host_card_type = CARD_SDHC;
    host_write_opens = 0;
    output.text.clear();
    host_rx.clear();
    host_tx.clear();
    host_rx_at = 0;
}

void sol()
{
    reset();
    initstorage();
    assert(storage_ok && string(storage_probe) == "NOT_CREATED" && host_write_opens == 0);
    assert(storage_capacity == 32000000000ull);
    teststorage(output);
    assert(string(storage_probe) == "PASS" && host_handles == 0);
    assert(output.text.find("close_reopen_verify=PASS") != string::npos);
    assert(host_files.at("/keep.txt") == vector<uint8_t>({1 , 2 , 3 , 4}));
    saved = host_files.at(probe_path);
    assert(saved.size() == 1024);
    string magic = "student NFC storage probe v2\n";
    for (size_t i = 0 ; i < saved.size() ; i ++)
    {
        uint8_t expected = i < magic.size() ? static_cast<uint8_t>(magic[i]) : static_cast<uint8_t>((i * 37 + 90) % 256);
        assert(saved[i] == expected);
    }
    initstorage();
    assert(string(storage_probe) == "PASS" && host_write_opens == 1);
    teststorage(output);
    assert(host_files.at(probe_path) == saved && host_write_opens == 1);
    host_files.at(probe_path)[700] ^= 1;
    saved = host_files.at(probe_path);
    teststorage(output);
    assert(string(storage_probe) == "CONTENT_MISMATCH" && host_write_opens == 1);
    assert(host_files.at(probe_path) == saved && host_handles == 0);

    reset();
    stale_new_size = true;
    initstorage();
    teststorage(output);
    assert(string(storage_probe) == "PASS" && host_files.at(probe_path).size() == 1024);
    assert(host_handles == 0 && host_write_opens == 1);

    reset();
    fail_seek = true;
    initstorage();
    teststorage(output);
    assert(string(storage_probe) == "SEEK_FAILED" && host_files.at(probe_path).empty());
    assert(host_handles == 0);

    reset();
    create_race = true;
    initstorage();
    teststorage(output);
    assert(string(storage_probe) == "CREATE_CONFLICT" && host_handles == 0);
    assert(host_files.at(probe_path) == vector<uint8_t>({7 , 8 , 9}));

    reset();
    host_files[probe_path] = {9 , 8 , 7};
    initstorage();
    teststorage(output);
    assert(string(storage_probe) == "SIZE_OR_TYPE_MISMATCH" && host_write_opens == 0);
    assert(host_files.at(probe_path) == vector<uint8_t>({9 , 8 , 7}));

    reset();
    fail_mount = true;
    initstorage();
    teststorage(output);
    assert(!storage_ok && string(storage_state) == "MOUNT_FAILED" && host_write_opens == 0);
    reset();
    host_card_type = CARD_NONE;
    initstorage();
    assert(!storage_ok && !SD.mounted && string(storage_state) == "NO_CARD");

    reset();
    initstorage();
    fail_write_open = true;
    teststorage(output);
    assert(string(storage_probe) == "WRITE_OPEN_FAILED" && host_handles == 0);
    reset();
    initstorage();
    short_write = true;
    teststorage(output);
    assert(string(storage_probe) == "WRITE_SHORT" && host_handles == 0);
    short_write = false;
    saved = host_files.at(probe_path);
    teststorage(output);
    assert(string(storage_probe) == "SIZE_OR_TYPE_MISMATCH" && host_write_opens == 1);
    assert(host_files.at(probe_path) == saved);

    reset();
    initstorage();
    teststorage(output);
    fail_read_open = true;
    assert(!verifyprobe() && string(storage_probe) == "READ_OPEN_FAILED");
    fail_read_open = false;
    short_read = true;
    assert(!verifyprobe() && string(storage_probe) == "READ_SHORT" && host_handles == 0);
    short_read = false;
    host_directory = true;
    assert(!verifyprobe() && string(storage_probe) == "SIZE_OR_TYPE_MISMATCH" && host_handles == 0);
    reportstorage(output);
    assert(output.text.find("auto_format=OFF") != string::npos);

    reset();
    initstorage();
    diagnosestorage(output);
    assert(host_tx.empty() && output.text.find("skipped") != string::npos);
    fail_mount = true;
    initstorage();
    host_rx.assign(47 , 0xFF);
    host_rx[32] = 1;
    host_rx[41] = 1;
    host_rx[42] = 0;
    host_rx[43] = 0;
    host_rx[44] = 1;
    host_rx[45] = 0xAA;
    diagnosestorage(output);
    assert(host_rx_at == 47 && host_cs && !sd_bus.transaction);
    assert(host_tx[26] == 0x40 && host_tx[31] == 0x95);
    assert(host_tx[35] == 0x48 && host_tx[38] == 1 && host_tx[39] == 0xAA && host_tx[40] == 0x87);
    assert(output.text.find("CMD0_R1=0x01 CMD8_R1=0x01 R7=00-00-01-AA") != string::npos);

    reset();
    fail_mount = true;
    initstorage();
    host_rx.assign(49 , 0xFF);
    diagnosestorage(output);
    assert(host_rx_at == 49 && host_cs && !sd_bus.transaction);
    assert(output.text.find("CMD0_R1=0xFF CMD8_R1=0xFF") != string::npos);
    assert(host_write_opens == 0);
    puts("PASS: separate SPI pins, no auto-format, close/reopen verification, existing-file preservation, stale new-file size, seek failure, create race, corrupt/short I/O, card/mount/open errors, handle closure, mounted diagnostic guard, read-only CMD0/CMD8 packets, bounded no-response diagnostic.");
}

signed main ()
{
    sol();
    return 0;
}
