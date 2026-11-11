#pragma once
/*
 * Parametros LoRa COMPARTIDOS por TX y RX (deben ser identicos en ambos nodos)
 * y pines del SX1262 de la Heltec WiFi LoRa 32 V3.
 */
#include "esp_err.h"
#include "sx1262.h"

/* ---- Parametros LoRa (equivalentes a tu codigo Arduino) ---- */
#define BIOSENSE_LORA_FREQ_HZ       915000000UL
#define BIOSENSE_LORA_SF            7
#define BIOSENSE_LORA_BW            SX1262_BW_125KHZ
#define BIOSENSE_LORA_CR            1            /* 4/5 */
#define BIOSENSE_LORA_PREAMBLE      8
#define BIOSENSE_LORA_TX_POWER_DBM  14
#define BIOSENSE_LORA_SYNC_WORD     0x1424       /* red privada */
#define BIOSENSE_LORA_MAX_PAYLOAD   128

/* ---- Pines del SX1262 en Heltec V3 ---- */
#define HELTEC_V3_LORA_SPI_HOST  SPI2_HOST
#define HELTEC_V3_LORA_NSS       8
#define HELTEC_V3_LORA_SCK       9
#define HELTEC_V3_LORA_MOSI      10
#define HELTEC_V3_LORA_MISO      11
#define HELTEC_V3_LORA_RST       12
#define HELTEC_V3_LORA_BUSY      13
#define HELTEC_V3_LORA_DIO1      14

esp_err_t biosense_radio_init(void);
