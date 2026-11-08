#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>

constexpr signed HIGH = 1 , LOW = 0 , OUTPUT = 1;

class Stream
{
public:
    std::string text;

    void print(const char *value)
    {
        text += value;
    }

    template <typename... Args>
    signed printf(const char *format , Args... args)
    {
        char line[512];
        signed size = std::snprintf(line , sizeof(line) , format , args...);
        text += line;
        return size;
    }
};

void digitalWrite(signed pin , signed value);
void pinMode(signed pin , signed mode);
