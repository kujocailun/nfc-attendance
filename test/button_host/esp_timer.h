#pragma once

#include <cstdint>

using esp_err_t = signed;
constexpr esp_err_t ESP_OK = 0;
struct host_timer;
using esp_timer_handle_t = host_timer *;

struct esp_timer_create_args_t
{
    void (*callback)(void *);
    const char *name;
};

esp_err_t esp_timer_create(const esp_timer_create_args_t *args , esp_timer_handle_t *timer);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer , uint64_t interval);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
