#pragma once

enum gpio_num_t
{
    GPIO_NUM_4 = 4 ,
    GPIO_NUM_5 = 5 ,
    GPIO_NUM_6 = 6
};

signed gpio_set_level(gpio_num_t pin , unsigned level);
