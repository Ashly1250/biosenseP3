#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Monta la SD y recupera el estado (siguiente SEQ y hasta donde confirmo el RX). */
esp_err_t sd_store_init(void);
bool      sd_store_ready(void);

/* Siguiente numero de secuencia (continua tras reiniciar si la SD tiene datos). */
uint32_t  sd_store_next_seq(void);

/* Guarda UNA lectura: 'line' es exactamente la trama CSV "SEQ,FECHA,..." (sin salto de linea). */
esp_err_t sd_store_append(uint32_t seq, const char *line);

/* ---- Reenvio de pendientes ---- */
bool      sd_store_has_pending(void);
/* Lee (sin avanzar) la lectura pendiente mas antigua. 'line' queda sin salto de linea. */
esp_err_t sd_store_peek(char *line, size_t size, uint32_t *seq);
/* El RX confirmo 'seq': si era la pendiente mas antigua, avanza el puntero. */
void      sd_store_mark_acked(uint32_t seq);
/* Cuantas lecturas faltan por confirmar (aprox.) */
uint32_t  sd_store_pending_count(void);
