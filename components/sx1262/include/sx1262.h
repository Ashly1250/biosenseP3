#pragma once
/*
 * Driver minimo del transceptor LoRa SX1262 para ESP-IDF
 * (probado en concepto para Heltec WiFi LoRa 32 V3: ESP32-S3 + SX1262).
 * Solo modulacion LoRa, paquetes con cabecera explicita y CRC.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#define SX1262_BW_125KHZ 0x04
#define SX1262_BW_250KHZ 0x05
#define SX1262_BW_500KHZ 0x06

typedef struct {
    spi_host_device_t spi_host;
    gpio_num_t pin_sck, pin_mosi, pin_miso, pin_nss;
    gpio_num_t pin_rst, pin_busy, pin_dio1;
    bool tcxo_1v8;          /* true en Heltec V3 (TCXO alimentado por DIO3 a 1.8 V) */
} sx1262_hw_t;

typedef struct {
    uint32_t freq_hz;       /* p.ej. 915000000 */
    int8_t   tx_power_dbm;  /* -9 .. 22 */
    uint8_t  sf;            /* 5 .. 12 */
    uint8_t  bw;            /* SX1262_BW_xxx */
    uint8_t  cr;            /* 1..4 => 4/5 .. 4/8 */
    uint16_t preamble_len;
    uint16_t sync_word;     /* 0x1424 privada, 0x3444 publica (LoRaWAN) */
} sx1262_cfg_t;

esp_err_t sx1262_init(const sx1262_hw_t *hw, const sx1262_cfg_t *cfg);

/* Envia un paquete y espera TX_DONE. */
esp_err_t sx1262_send(const uint8_t *data, size_t len, uint32_t timeout_ms);

/* Pone el radio en recepcion continua. */
esp_err_t sx1262_start_rx(void);

/* Espera un paquete. Devuelve ESP_OK, ESP_ERR_TIMEOUT o ESP_ERR_INVALID_CRC. */
esp_err_t sx1262_receive(uint8_t *buf, size_t max_len, size_t *out_len,
                         int16_t *rssi_dbm, float *snr_db, uint32_t timeout_ms);
