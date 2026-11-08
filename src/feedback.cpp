#include <Arduino.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include "feedback.h"
using namespace std;
#define int long long

const gpio_num_t buzzer_pin = GPIO_NUM_4;
const gpio_num_t green_pin = GPIO_NUM_5;
const gpio_num_t red_pin = GPIO_NUM_6;
esp_timer_handle_t feedback_timer;
portMUX_TYPE feedback_lock = portMUX_INITIALIZER_UNLOCKED;
bool feedback_ok;
uint8_t feedback_mode;
int64_t feedback_start;

void feedbackoff()
{
    gpio_set_level(buzzer_pin , 0);
    gpio_set_level(green_pin , 0);
    gpio_set_level(red_pin , 0);
}

void feedbacktick(void *)
{
    portENTER_CRITICAL(&feedback_lock);
    int64_t elapsed = (esp_timer_get_time() - feedback_start) / 1000;
    bool buzzer = false , green = false , red = false;
    if (feedback_mode == 1)
    {
        green = elapsed < 800;
        buzzer = elapsed < 100;
        if (elapsed >= 800)
        {
            feedback_mode = 0;
        }
    }
    else if (feedback_mode == 2)
    {
        red = elapsed < 800;
        buzzer = elapsed < 100 || (elapsed >= 220 && elapsed < 320);
        if (elapsed >= 800)
        {
            feedback_mode = 0;
        }
    }
    else if (feedback_mode == 3)
    {
        green = elapsed < 650;
        red = elapsed >= 800 && elapsed < 1450;
        buzzer = elapsed >= 1600 && elapsed < 1700;
        if (elapsed >= 1800)
        {
            feedback_mode = 0;
        }
    }
    else if (feedback_mode == 4)
    {
        green = elapsed < 150;
        if (elapsed >= 150)
        {
            feedback_mode = 0;
        }
    }
    gpio_set_level(buzzer_pin , buzzer);
    gpio_set_level(green_pin , green);
    gpio_set_level(red_pin , red);
    portEXIT_CRITICAL(&feedback_lock);
}

void initfeedback()
{
    digitalWrite(buzzer_pin , LOW);
    digitalWrite(green_pin , LOW);
    digitalWrite(red_pin , LOW);
    pinMode(buzzer_pin , OUTPUT);
    pinMode(green_pin , OUTPUT);
    pinMode(red_pin , OUTPUT);
    feedbackoff();
    esp_timer_create_args_t args = {};
    args.callback = feedbacktick;
    args.name = "nfc_feedback";
    if (esp_timer_create(&args , &feedback_timer) != ESP_OK)
    {
        return;
    }
    if (esp_timer_start_periodic(feedback_timer , 5000) != ESP_OK)
    {
        esp_timer_delete(feedback_timer);
        feedback_timer = nullptr;
        return;
    }
    feedback_ok = true;
}

void startfeedback(uint8_t mode)
{
    if (!feedback_ok)
    {
        return;
    }
    portENTER_CRITICAL(&feedback_lock);
    feedbackoff();
    feedback_start = esp_timer_get_time();
    feedback_mode = mode;
    portEXIT_CRITICAL(&feedback_lock);
}

void cardfeedback(bool success)
{
    startfeedback(success ? 1 : 2);
}

void duplicatefeedback()
{
    startfeedback(4);
}

void testfeedback(Stream &out)
{
    if (!feedback_ok)
    {
        out.print("[FEEDBACK-TEST] timer unavailable\n");
        return;
    }
    startfeedback(3);
    out.print("[FEEDBACK-TEST] scheduled: green, red, short beep, then all off.\n");
}

void reportfeedback(Stream &out)
{
    out.printf("feedback_timer=%s buzzer_gpio=4 green_gpio=5 red_gpio=6 active=HIGH\n" ,
               feedback_ok ? "READY" : "FAILED");
    out.print("Feedback: saved/enrolled=green+one beep, already today=short green without beep, rejected=red+two beeps.\n");
}
