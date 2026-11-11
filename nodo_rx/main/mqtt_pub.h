#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t mqtt_pub_start(void);                 /* conecta al broker de la Raspberry (reintenta solo) */
bool      mqtt_pub_connected(void);
bool      mqtt_pub_publish(const char *payload); /* true si quedo en cola/enviado */
