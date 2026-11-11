#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "biosense_radio.h"
#include "sx1262.h"
#include "config.h"
#include "sd_logger.h"
#include "wifi_sta.h"
#include "mqtt_pub.h"

/* 1 = guarda respaldo en la SD del RX (ya no hace falta: la SD esta en el TX), 0 = sin SD. */
#ifndef ENABLE_SD_BACKUP
#define ENABLE_SD_BACKUP 0
#endif

/* 1 = responde "A,<seq>" al TX cuando entrego la lectura a la Raspberry (necesario para la persistencia) */
#ifndef ENABLE_ACK
#define ENABLE_ACK 1
#endif

static const char *TAG = "RX";

static bool trama_valida(const char *s)
{
    int comas = 0;
    for (; *s; s++) if (*s == ',') comas++;
    return comas == 6;      /* SEQ,FECHA_HORA,TEMP,TDS,PH,OD,SST */
}

#if ENABLE_ACK
/* Responde al TX con "A,<seq>" y vuelve a escuchar. */
static void send_ack(uint32_t seq)
{
    char ack[24];
    int n = snprintf(ack, sizeof(ack), "A,%lu", (unsigned long)seq);
    vTaskDelay(pdMS_TO_TICKS(30));          /* el TX necesita unos ms para pasar de TX a RX */
    esp_err_t e = sx1262_send((const uint8_t *)ack, (size_t)n, 1000);
    if (e != ESP_OK) ESP_LOGW(TAG, "No se pudo enviar el ACK: %s", esp_err_to_name(e));
    sx1262_start_rx();
}
#endif

static void rx_task(void *arg)
{
    uint8_t buf[BIOSENSE_LORA_MAX_PAYLOAD + 1];
    char payload[BIOSENSE_LORA_MAX_PAYLOAD + 32];
    

    ESP_ERROR_CHECK(sx1262_start_rx());
    ESP_LOGI(TAG, "Esperando lecturas de sensores por LoRa...");

    while (1) {
        size_t len = 0;
        int16_t rssi = 0;
        float snr = 0;
        esp_err_t e = sx1262_receive(buf, sizeof(buf) - 1, &len, &rssi, &snr, RX_POLL_TIMEOUT_MS);

        if (e == ESP_ERR_TIMEOUT) continue;
        if (e == ESP_ERR_INVALID_CRC) {
            ESP_LOGW(TAG, "Paquete con CRC/cabecera invalida, descartado");
            continue;
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "Error de recepcion: %s", esp_err_to_name(e));
            sx1262_start_rx();
            continue;
        }

        buf[len] = '\0';
        char *trama = (char *)buf;
        while (len > 0 && (trama[len - 1] == '\r' || trama[len - 1] == '\n')) trama[--len] = '\0';

        if (!trama_valida(trama)) {
            ESP_LOGW(TAG, "Trama con formato inesperado, no se envia: %s", trama);
            continue;
        }

        /* Payload MQTT: SEQ,FECHA_HORA,TEMP,TDS,PH,OD,SST,RSSI,SNR */
        snprintf(payload, sizeof(payload), "%s,%d,%.2f", trama, rssi, snr);
        uint32_t seq = (uint32_t)strtoul(trama, NULL, 10);
        (void)seq;

        /* Solo se publica (y se confirma al TX) si hay conexion con la Raspberry.
           Si no la hay, el TX no recibe ACK y reenviara esa lectura mas tarde desde su SD. */
        bool ok = mqtt_pub_connected() && mqtt_pub_publish(payload);
#if ENABLE_ACK
        if (ok) send_ack(seq);
#endif
        
        ESP_LOGI(TAG,
         "LoRa -> %s | RSSI %d dBm | WiFi:%s MQTT:%s | publicado:%s",
         trama,
         rssi,
         wifi_sta_connected() ? "ok" : "no",
         mqtt_pub_connected() ? "ok" : "no",
         ok ? "si" : "NO");

#if ENABLE_SD_BACKUP
        if (sd_logger_ready()) {
            if (sd_logger_append(trama, rssi, snr) != ESP_OK) {
                ESP_LOGW(TAG, "No se pudo guardar en la SD");
            }
        }
#endif
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "BIOSENSE - Nodo receptor LoRa -> WiFi/MQTT -> Raspberry");

    ESP_ERROR_CHECK(biosense_radio_init());

#if ENABLE_SD_BACKUP
    if (sd_logger_init() != ESP_OK) {
        ESP_LOGW(TAG, "Sin SD: se sigue enviando por WiFi sin respaldo local");
    }
#endif

    ESP_ERROR_CHECK(wifi_sta_start());
    ESP_ERROR_CHECK(mqtt_pub_start());

    xTaskCreate(rx_task, "rx_task", 8192, NULL, 5, NULL);
}
