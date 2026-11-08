#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>

constexpr signed INPUT = 0;
using portMUX_TYPE = signed;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) static_cast<void>(x)
#define portEXIT_CRITICAL(x) static_cast<void>(x)

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

void pinMode(signed pin , signed mode);
signed digitalRead(signed pin);
uint32_t millis();
