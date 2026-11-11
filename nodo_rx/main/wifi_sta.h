#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Inicia el WiFi como estacion. No bloquea: conecta y reconecta en segundo plano. */
esp_err_t wifi_sta_start(void);
bool      wifi_sta_connected(void);
