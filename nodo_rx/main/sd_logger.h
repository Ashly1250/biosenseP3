#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t sd_logger_init(void);     /* alimenta Vext, monta la SD y crea el CSV con encabezado */
bool      sd_logger_ready(void);

esp_err_t sd_logger_append(const char *trama,
                           int rssi_dbm,
                           float snr_db);
