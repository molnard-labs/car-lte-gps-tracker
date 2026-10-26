#pragma once
#include <stdint.h>
#define RTC_DATA_ATTR
#define ESP_OK 0
#define GPIO_MODE_INPUT_OUTPUT 3
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_INTR_DISABLE 0
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
int gpio_set_level(int gpio, int value);
int gpio_get_level(int gpio);
int gpio_config(const gpio_config_t *config);
int gpio_hold_en(int gpio);
int gpio_hold_dis(int gpio);
void gpio_deep_sleep_hold_en(void);
