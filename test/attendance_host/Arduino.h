#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>

inline char host_print_line[512];

class Stream
{
public:
    std::string text;

    void print(const char *value)
    {
        text += value;
    }

    size_t write(const uint8_t *value , size_t len)
    {
        text.append(reinterpret_cast<const char *>(value) , len);
        return len;
    }

    template <typename... Args>
    signed printf(const char *format , Args... args)
    {
        signed len = std::snprintf(host_print_line , sizeof(host_print_line) , format , args...);
        text += host_print_line;
        return len;
    }
};

uint32_t millis();
