#pragma once
#include "esp_err.h"

typedef struct {
    float temp_c;     /* °C */
    float tds_ppm;    /* ppm */
    float ph;         /* sensor real PH-4502C */
    float od_mgl;     /* simulado, mg/L */
    float sst_mgl;    /* simulado, mg/L */
} sensor_data_t;

esp_err_t sensors_init(void);
esp_err_t sensors_read(sensor_data_t *out);   /* bloquea ~750 ms por el DS18B20 */

/* mV crudos en el pin del ADC del pH (para calibrar) */
float sensors_read_ph_adc_mv(void);
