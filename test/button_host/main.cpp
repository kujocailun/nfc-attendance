#include <cassert>
#include <cstdio>
#include <Arduino.h>
#include <esp_timer.h>
#include "../../src/button.cpp"
using namespace std;

struct host_timer
{
    void (*callback)(void *);
    uint64_t interval;
};

host_timer timer_data;
Stream output;
uint32_t host_now;
bool host_level , fail_create , fail_start , timer_deleted;

void pinMode(signed pin , signed mode)
{
    assert(pin == 7 && mode == INPUT);
}

signed digitalRead(signed pin)
{
    assert(pin == 7);
    return host_level;
}

uint32_t millis()
{
    return host_now;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args , esp_timer_handle_t *timer)
{
    if (fail_create)
    {
        return -1;
    }
    timer_data.callback = args->callback;
    *timer = &timer_data;
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer , uint64_t interval)
{
    timer->interval = interval;
    return fail_start ? -1 : ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t)
{
    timer_deleted = true;
    return ESP_OK;
}

void reset(bool level , uint32_t now = 0)
{
    host_now = now;
    host_level = level;
    button_ok = false;
    button_total = button_dropped = 0;
    button_head = button_size = 0;
    button_timer = nullptr;
    fail_create = fail_start = timer_deleted = false;
    output.text.clear();
}

void advance(uint32_t duration)
{
    assert(duration % 5 == 0);
    for (uint32_t i = 0 ; i < duration ; i += 5)
    {
        host_now += 5;
        timer_data.callback(nullptr);
    }
}

void sol()
{
    reset(false);
    initbutton();
    assert(button_ok && timer_data.interval == 5000);
    advance(100);
    assert(button_total == 0);
    host_level = true;
    advance(10);
    host_level = false;
    advance(10);
    host_level = true;
    advance(20);
    host_level = false;
    advance(50);
    assert(button_total == 0);

    host_level = true;
    advance(100);
    host_level = false;
    advance(100);
    assert(button_total == 2 && button_size == 2);
    pollbutton(output);
    assert(output.text.find("event=1 gpio=7 edge=RISING level=HIGH") != string::npos);
    assert(output.text.find("event=2 gpio=7 edge=FALLING level=LOW") != string::npos);
    assert(button_size == 0 && button_dropped == 0);
    host_level = true;
    advance(3000);
    assert(button_total == 3);

    reset(true);
    initbutton();
    advance(1000);
    assert(button_total == 0 && button_initial);
    host_level = false;
    advance(40);
    assert(button_total == 1 && !button_stable);

    reset(false , UINT32_MAX - 19);
    initbutton();
    host_level = true;
    advance(40);
    assert(button_total == 1 && button_stable);

    reset(false);
    initbutton();
    for (int i = 0 ; i < 20 ; i ++)
    {
        host_level = !host_level;
        advance(40);
    }
    assert(button_total == 20 && button_size == 16 && button_dropped == 4);
    pollbutton(output);
    assert(button_size == 0);
    host_level = true;
    advance(40);
    pollbutton(output);
    assert(output.text.find("event=21 gpio=7") != string::npos);
    reportbutton(output);
    assert(output.text.find("edges=21 dropped=4") != string::npos);

    reset(false);
    fail_create = true;
    initbutton();
    assert(!button_ok);
    pollbutton(output);
    assert(output.text.empty());
    reportbutton(output);
    assert(output.text.find("timer=FAILED") != string::npos);
    reset(false);
    fail_start = true;
    initbutton();
    assert(!button_ok && timer_deleted && button_timer == nullptr);
    puts("PASS: bounce rejection, queued short tap while loop blocked, no repeat while held, initial held state, timer wrap, overflow reporting, timer failure.");
}

signed main ()
{
    sol();
    return 0;
}
