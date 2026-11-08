#pragma once

#include <cstdint>
#include <cstddef>

constexpr signed LOW = 0;
constexpr signed OUTPUT = 1;
using portMUX_TYPE = signed;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) static_cast<void>(x)
#define portEXIT_CRITICAL(x) static_cast<void>(x)

class Stream
{
public:
    void print(const char *)
    {
    }

    template <typename... Args>
    signed printf(const char * , Args...)
    {
        return 0;
    }
};

void digitalWrite(signed pin , signed value);
void pinMode(signed pin , signed mode);
