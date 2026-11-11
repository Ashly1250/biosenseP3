#pragma once
#include "esp_err.h"
#include "driver/gpio.h"

/* Un solo DS18B20 en el bus (usa SKIP ROM). */
esp_err_t ds18b20_init(gpio_num_t pin);
esp_err_t ds18b20_read_temp_c(float *temp_c);   /* bloquea ~750 ms */
