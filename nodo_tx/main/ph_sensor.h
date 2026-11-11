#pragma once
#include <stdbool.h>

float ph_module_mv(float adc_mv);
float ph_from_adc_mv(float adc_mv, float temp_c);