#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define FILE_READ "r"
#define FILE_APPEND "a"
enum SeekMode
{
    SeekSet , SeekCur , SeekEnd
};
extern std::map<std::string , std::vector<uint8_t>> host_files;
extern std::set<std::string> host_dirs;
extern bool host_fail_open , host_short_write , host_short_read , host_fail_seek;
extern bool host_fail_commit , host_corrupt_readback , host_fail_mkdir;
extern signed host_handles , host_write_opens;

struct host_file
{
    std::string path;
    size_t at = 0 , initial_size = 0;
    bool writable = false , directory = false , created = false;

    ~host_file()
    {
        host_handles --;
    }
};

class File
{
public:
    std::shared_ptr<host_file> data;

    explicit operator bool() const
    {
        return data != nullptr;
    }

    bool isDirectory() const
    {
        return data && data->directory;
    }

    size_t size() const
    {
        if (!data || data->directory)
        {
            return 0;
        }
        return data->created && data->writable ? 0x12345678 : host_files.at(data->path).size();
    }

    size_t position() const
    {
        return data ? data->at : 0;
    }

    bool seek(uint32_t offset , SeekMode mode)
    {
        if (!data || data->directory || host_fail_seek)
        {
            return false;
        }
        size_t base = mode == SeekEnd ? host_files.at(data->path).size() : (mode == SeekCur ? data->at : 0);
        if (base + offset > host_files.at(data->path).size())
        {
            return false;
        }
        data->at = base + offset;
        return true;
    }

    size_t read(uint8_t *value , size_t len)
    {
        assert(data && !data->writable && !data->directory);
        auto &bytes = host_files.at(data->path);
        size_t count = std::min(len , bytes.size() - data->at);
        if (host_short_read && count)
        {
            count --;
        }
        std::memcpy(value , bytes.data() + data->at , count);
        data->at += count;
        if (host_corrupt_readback && count)
        {
            value[0] ^= 1;
        }
        return count;
    }

    signed read()
    {
        uint8_t c;
        return read(&c , 1) == 1 ? c : -1;
    }

    bool available() const
    {
        return data && !data->directory && data->at < host_files.at(data->path).size();
    }

    size_t readBytesUntil(char stop , char *value , size_t capacity)
    {
        size_t count = 0;
        while (available() && count < capacity)
        {
            signed c = read();
            if (c < 0 || c == stop)
            {
                break;
            }
            value[count ++] = static_cast<char>(c);
        }
        return count;
    }

    const char *name() const
    {
        return data ? data->path.c_str() : "";
    }

    File openNextFile()
    {
        File result;
        if (!data || !data->directory || host_fail_open)
        {
            return result;
        }
        std::string prefix = data->path == "/" ? "/" : data->path + "/";
        size_t index = 0;
        for (auto &entry : host_files)
        {
            if (entry.first.rfind(prefix , 0) != 0 || entry.first.find('/' , prefix.size()) != std::string::npos)
            {
                continue;
            }
            if (index ++ < data->at)
            {
                continue;
            }
            data->at ++;
            result.data = std::make_shared<host_file>();
            host_handles ++;
            result.data->path = entry.first;
            return result;
        }
        return result;
    }

    size_t write(const uint8_t *value , size_t len)
    {
        assert(data && data->writable && !data->directory);
        auto &bytes = host_files.at(data->path);
        size_t count = len - (host_short_write && len ? 1 : 0);
        bytes.insert(bytes.end() , value , value + count);
        data->at = bytes.size();
        return count;
    }

    void flush()
    {
        assert(data && data->writable);
        if (host_fail_commit)
        {
            host_files.at(data->path).resize(data->initial_size);
        }
    }

    void close()
    {
        data.reset();
    }
};

class SDFS
{
public:
    bool exists(const char *path) const
    {
        return host_files.count(path) || host_dirs.count(path);
    }

    bool mkdir(const char *path)
    {
        if (host_fail_mkdir || host_files.count(path))
        {
            return false;
        }
        host_dirs.insert(path);
        return true;
    }

    File open(const char *path , const char *mode)
    {
        File file;
        bool write = std::string(mode) == "a";
        bool directory = host_dirs.count(path) != 0;
        if (host_fail_open || (!write && !exists(path)))
        {
            return file;
        }
        bool fresh = !exists(path);
        if (write && !directory)
        {
            host_files[path];
            host_write_opens ++;
        }
        file.data = std::make_shared<host_file>();
        host_handles ++;
        file.data->path = path;
        file.data->writable = write;
        file.data->directory = directory;
        file.data->created = fresh;
        file.data->initial_size = directory ? 0 : host_files.at(path).size();
        file.data->at = write ? file.data->initial_size : 0;
        return file;
    }
};

extern SDFS SD;
