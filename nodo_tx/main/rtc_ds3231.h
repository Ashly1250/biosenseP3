#pragma once

#include <stddef.h>
#include "esp_err.h"

esp_err_t rtc_ds3231_init(void);

void rtc_ds3231_get_timestamp(
    char *buffer,
    size_t len
);

/* Ajusta el DS3231 (hora tal como se quiere guardar, 24 h). */
esp_err_t rtc_ds3231_set(int year, int month, int day, int hour, int min, int sec);

/* Ajusta el DS3231 con la fecha/hora de compilacion (__DATE__ / __TIME__). */
esp_err_t rtc_ds3231_set_from_build(void);
