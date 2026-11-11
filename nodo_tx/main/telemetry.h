#pragma once
#include <stddef.h>
#include "sensors.h"

#include <stdint.h>

/* Formato: "SEQ,FECHA_HORA,TEMP,TDS,PH,OD,SST" (2 decimales). Devuelve longitud o <0 si no cabe. */
int telemetry_format_csv(
    uint32_t seq,
    const char *timestamp,
    const sensor_data_t *d,
    char *out,
    size_t out_size
);