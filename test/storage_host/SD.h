#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <SPI.h>

#define FILE_READ "r"
#define FILE_APPEND "a"
constexpr uint8_t CARD_NONE = 0 , CARD_SDHC = 3;
extern std::map<std::string , std::vector<uint8_t>> host_files;
extern bool fail_mount , fail_read_open , fail_write_open , short_read , short_write , host_directory;
extern bool stale_new_size , fail_seek , create_race;
extern uint8_t host_card_type;
extern signed host_handles , host_write_opens;
enum SeekMode
{
    SeekSet , SeekCur , SeekEnd
};

class File
{
public:
    std::vector<uint8_t> *data = nullptr;
    size_t at = 0;
    bool writable = false;
    bool created = false;

    File() = default;

    File(std::vector<uint8_t> &value , bool write , bool fresh = false) : data(&value) , writable(write) , created(fresh)
    {
        at = write ? value.size() : 0;
        host_handles ++;
    }

    explicit operator bool() const
    {
        return data != nullptr;
    }

    bool isDirectory() const
    {
        return host_directory;
    }

    size_t size() const
    {
        if (writable && created && stale_new_size)
        {
            return 0x12345678;
        }
        return data->size();
    }

    bool seek(uint32_t offset , SeekMode mode)
    {
        assert(data && offset == 0 && mode == SeekEnd);
        if (fail_seek)
        {
            return false;
        }
        at = data->size();
        return true;
    }

    size_t position() const
    {
        return at;
    }

    size_t read(uint8_t *buffer , size_t size)
    {
        assert(data && !writable);
        size_t count = std::min(size , data->size() - at);
        if (short_read && count)
        {
            count --;
        }
        std::memcpy(buffer , data->data() + at , count);
        at += count;
        return count;
    }

    size_t write(const uint8_t *buffer , size_t size)
    {
        assert(data && writable);
        size_t count = size - (short_write && size ? 1 : 0);
        data->insert(data->end() , buffer , buffer + count);
        at += count;
        return count;
    }

    void flush()
    {
        assert(data && writable);
    }

    void close()
    {
        if (data)
        {
            host_handles --;
            data = nullptr;
        }
    }
};

class SDFS
{
public:
    bool mounted = false;

    bool begin(uint8_t cs , SPIClass &bus , uint32_t frequency , const char *mount , uint8_t max_files , bool format)
    {
        assert(cs == 21 && bus.bus == HSPI && bus.started && frequency == 400000);
        assert(std::string(mount) == "/sd" && max_files == 5 && !format);
        mounted = !fail_mount;
        return mounted;
    }

    void end()
    {
        assert(host_handles == 0);
        mounted = false;
    }

    uint8_t cardType() const
    {
        return host_card_type;
    }

    uint64_t cardSize() const
    {
        return 32000000000ull;
    }

    uint64_t totalBytes() const
    {
        return 31900000000ull;
    }

    uint64_t usedBytes() const
    {
        return 4096;
    }

    bool exists(const char *path) const
    {
        assert(mounted);
        return host_files.count(path) != 0;
    }

    File open(const char *path , const char *mode)
    {
        assert(mounted);
        if (std::string(mode) == "r")
        {
            if (fail_read_open || !exists(path))
            {
                return File();
            }
            return File(host_files.at(path) , false);
        }
        assert(std::string(mode) == "a");
        host_write_opens ++;
        if (fail_write_open)
        {
            return File();
        }
        bool fresh = !exists(path);
        if (fresh && create_race)
        {
            host_files[path] = {7 , 8 , 9};
        }
        return File(host_files[path] , true , fresh);
    }
};

extern SDFS SD;
