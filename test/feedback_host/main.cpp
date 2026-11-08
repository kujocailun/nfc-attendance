#include <cassert>
#include <cstdio>
#include <Arduino.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include "../../src/feedback.cpp"
using namespace std;

struct host_timer
{
    void (*callback)(void *);
    uint64_t interval;
};

host_timer timer_data;
int64_t host_now;
signed levels[7];
bool fail_create , fail_start , timer_deleted;

void digitalWrite(signed pin , signed value)
{
    levels[pin] = value;
}

void pinMode(signed , signed)
{
}

signed gpio_set_level(gpio_num_t pin , unsigned value)
{
    levels[pin] = value;
    return 0;
}

int64_t esp_timer_get_time()
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

void advance(uint32_t milliseconds)
{
    host_now += milliseconds * 1000ll;
    timer_data.callback(nullptr);
}

void expect(bool buzzer , bool green , bool red)
{
    assert(levels[4] == buzzer);
    assert(levels[5] == green);
    assert(levels[6] == red);
}

void sol()
{
    initfeedback();
    assert(feedback_ok && timer_data.interval == 5000);
    expect(false , false , false);
    advance(500);
    expect(false , false , false);

    cardfeedback(true);
    advance(5);
    expect(true , true , false);
    advance(95);
    expect(false , true , false);
    advance(700);
    expect(false , false , false);

    duplicatefeedback();
    advance(5);
    expect(false , true , false);
    advance(145);
    expect(false , false , false);

    cardfeedback(false);
    advance(5);
    expect(true , false , true);
    advance(95);
    expect(false , false , true);
    advance(120);
    expect(true , false , true);
    advance(100);
    expect(false , false , true);
    advance(480);
    expect(false , false , false);

    startfeedback(3);
    advance(5);
    expect(false , true , false);
    advance(645);
    expect(false , false , false);
    advance(150);
    expect(false , false , true);
    advance(650);
    expect(false , false , false);
    advance(150);
    expect(true , false , false);
    advance(100);
    expect(false , false , false);
    advance(100);
    assert(feedback_mode == 0);

    cardfeedback(true);
    advance(5);
    cardfeedback(false);
    expect(false , false , false);
    advance(5);
    expect(true , false , true);
    advance(795);
    expect(false , false , false);

    feedback_ok = false;
    fail_create = true;
    initfeedback();
    assert(!feedback_ok);
    cardfeedback(true);
    expect(false , false , false);
    fail_create = false;
    fail_start = true;
    initfeedback();
    assert(!feedback_ok && timer_deleted && feedback_timer == nullptr);
    expect(false , false , false);
    puts("PASS: idle outputs, independent pulse timing, two-beep pattern, self-test sequence, event replacement, timer failure.");
}

signed main ()
{
    sol();
    return 0;
}
