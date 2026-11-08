#include <Arduino.h>
#include <esp_timer.h>
#include "button.h"
using namespace std;
#define int long long

const uint8_t button_pin = 7;
const uint32_t debounce_ms = 30;
const uint8_t button_capacity = 16;
struct button_event
{
    uint32_t number , at;
    bool level;
};

button_event button_events[button_capacity];
esp_timer_handle_t button_timer;
portMUX_TYPE button_lock = portMUX_INITIALIZER_UNLOCKED;
bool button_ok;
bool button_raw , button_stable , button_initial;
uint32_t button_changed_at , button_total , button_dropped;
uint8_t button_head , button_size;

void buttontick(void *)
{
    bool raw = digitalRead(button_pin);
    uint32_t now = millis();
    portENTER_CRITICAL(&button_lock);
    if (raw != button_raw)
    {
        button_raw = raw;
        button_changed_at = now;
    }
    if (button_raw != button_stable && now - button_changed_at >= debounce_ms)
    {
        button_stable = button_raw;
        button_total ++;
        if (button_size < button_capacity)
        {
            uint8_t tail = (button_head + button_size) % button_capacity;
            button_events[tail] = {button_total , now , button_stable};
            button_size ++;
        }
        else
        {
            button_dropped ++;
        }
    }
    portEXIT_CRITICAL(&button_lock);
}

void initbutton()
{
    pinMode(button_pin , INPUT);
    button_raw = digitalRead(button_pin);
    button_initial = button_stable = button_raw;
    button_changed_at = millis();
    esp_timer_create_args_t args = {};
    args.callback = buttontick;
    args.name = "nfc_button";
    if (esp_timer_create(&args , &button_timer) != ESP_OK)
    {
        return;
    }
    if (esp_timer_start_periodic(button_timer , 5000) != ESP_OK)
    {
        esp_timer_delete(button_timer);
        button_timer = nullptr;
        return;
    }
    button_ok = true;
}

void pollbutton(Stream &out)
{
    if (!button_ok)
    {
        return;
    }
    for (int i = 0 ; i < button_capacity ; i ++)
    {
        button_event event = {};
        portENTER_CRITICAL(&button_lock);
        bool available = button_size != 0;
        if (available)
        {
            event = button_events[button_head];
            button_head = (button_head + 1) % button_capacity;
            button_size --;
        }
        portEXIT_CRITICAL(&button_lock);
        if (!available)
        {
            break;
        }
        out.printf("[BUTTON] event=%lu gpio=7 edge=%s level=%s at_ms=%lu\n" ,
                   static_cast<unsigned long>(event.number) ,
                   event.level ? "RISING" : "FALLING" , event.level ? "HIGH" : "LOW" ,
                   static_cast<unsigned long>(event.at));
    }
}

void reportbutton(Stream &out)
{
    portENTER_CRITICAL(&button_lock);
    bool level = button_stable;
    uint32_t total = button_total , dropped = button_dropped;
    portEXIT_CRITICAL(&button_lock);
    out.printf("button_gpio=7 timer=%s initial_level=%s current_level=%s debounce_ms=%lu scan_ms=5 edges=%lu dropped=%lu\n" ,
               button_ok ? "READY" : "FAILED" , button_initial ? "HIGH" : "LOW" ,
               level ? "HIGH" : "LOW" , static_cast<unsigned long>(debounce_ms) ,
               static_cast<unsigned long>(total) , static_cast<unsigned long>(dropped));
    out.print("Button polarity not yet confirmed; edges are diagnostic only.\n");
}

bool buttonpressed()
{
    portENTER_CRITICAL(&button_lock);
    bool pressed = button_ok && !button_stable;
    portEXIT_CRITICAL(&button_lock);
    return pressed;
}
