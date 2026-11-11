#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "biosense_radio.h"
#include "sx1262.h"
#include "config.h"
#include "sensors.h"
#include "telemetry.h"
#include "ph_sensor.h"
#include "rtc_ds3231.h"
#include "sd_store.h"
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"

static const char *TAG = "TX";

#if PH_CAL_MODE
/* ---------------- MODO CALIBRACION DEL pH (sin LoRa) ---------------- */
static void ph_cal_task(void *arg)
{
    while (1) {
        float adc_mv = sensors_read_ph_adc_mv();
        ESP_LOGI(TAG, "pH ADC: %.0f mV | Po del modulo: %.0f mV | pH estimado: %.2f",
                 adc_mv, ph_module_mv(adc_mv), ph_from_adc_mv(adc_mv, 25.0f));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_LOGW(TAG, "MODO CALIBRACION pH: anota 'Po del modulo' en cada buffer. LoRa desactivado.");
    ESP_ERROR_CHECK(sensors_init());
    xTaskCreate(ph_cal_task, "ph_cal", 4096, NULL, 5, NULL);
}

#else
/* ---------------- OPERACION NORMAL ---------------- */

/* Envia una trama por LoRa y (si ENABLE_ACK_REPLAY) espera el ACK "A,<seq>" del RX.
   Devuelve true si el RX confirmo que la recibio y la entrego a la Raspberry. */
static bool send_frame(const char *frame, size_t n, uint32_t seq)
{
    esp_err_t e = sx1262_send((const uint8_t *)frame, n, TX_TIMEOUT_MS);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Error de envio: %s", esp_err_to_name(e));
        return false;
    }
#if ENABLE_ACK_REPLAY
    sx1262_start_rx();
    int64_t t0 = esp_timer_get_time();
    while (1) {
        int64_t used = (esp_timer_get_time() - t0) / 1000;
        if (used >= ACK_WAIT_MS) break;
        uint8_t b[24];
        size_t len = 0;
        e = sx1262_receive(b, sizeof(b) - 1, &len, NULL, NULL, (uint32_t)(ACK_WAIT_MS - used));
        if (e == ESP_ERR_TIMEOUT) break;
        if (e != ESP_OK) continue;
        b[len] = '\0';
        if (len > 2 && b[0] == 'A' && b[1] == ',' && strtoul((char *)b + 2, NULL, 10) == seq) {
            return true;
        }
    }
    return false;
#else
    (void)seq;
    return true;      /* sin ACK: se asume entregada (comportamiento anterior) */
#endif
}

static void tx_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    char frame[BIOSENSE_LORA_MAX_PAYLOAD];

    while (1) {
        sensor_data_t d;
        sensors_read(&d);

        char timestamp[32];
        rtc_ds3231_get_timestamp(timestamp, sizeof(timestamp));

        /* 1) Siempre se guarda primero en la SD (asi nada se pierde aunque falle el radio) */
        uint32_t seq = sd_store_next_seq();
        int n = telemetry_format_csv(seq, timestamp, &d, frame, sizeof(frame));
        if (n > 0) {
            if (sd_store_ready() && sd_store_append(seq, frame) != ESP_OK) {
                ESP_LOGW(TAG, "No se pudo guardar en la SD");
            }

            /* 2) Envio en vivo */
            ESP_LOGI(TAG, "Trama: %s", frame);
            bool ok = send_frame(frame, (size_t)n, seq);
            ESP_LOGI(TAG, "SEQ %lu %s | pendientes en SD: %lu",
                     (unsigned long)seq,
                     ok ? "confirmada por el RX" : "SIN confirmacion",
                     (unsigned long)sd_store_pending_count());
            if (ok) sd_store_mark_acked(seq);

#if ENABLE_ACK_REPLAY
            /* 3) Si el enlace funciona, se reenvian las lecturas atrasadas (las mas viejas primero) */
            if (ok) {
                for (int i = 0; i < REPLAY_PER_CYCLE && sd_store_has_pending(); i++) {
                    char old[BIOSENSE_LORA_MAX_PAYLOAD];
                    uint32_t oseq = 0;
                    if (sd_store_peek(old, sizeof(old), &oseq) != ESP_OK) break;
                    if (oseq == seq) {            /* es la de hace un momento: ya esta confirmada */
                        sd_store_mark_acked(oseq);
                        continue;
                    }
                    if (!send_frame(old, strlen(old), oseq)) break;   /* se cayo de nuevo: se intenta luego */
                    ESP_LOGI(TAG, "Reenviada SEQ %lu", (unsigned long)oseq);
                    sd_store_mark_acked(oseq);
                }
            }
#endif
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TX_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "BIOSENSE - Nodo transmisor (SD + ACK/reenvio)");
    ESP_ERROR_CHECK(sensors_init());
    ESP_ERROR_CHECK(biosense_radio_init());
    ESP_ERROR_CHECK(rtc_ds3231_init());
#if RTC_SET_FROM_BUILD
    rtc_ds3231_set_from_build();
    ESP_LOGW(TAG, "RTC ajustado con la hora de compilacion: pon RTC_SET_FROM_BUILD en 0 y vuelve a flashear");
#endif
    if (sd_store_init() != ESP_OK) {
        ESP_LOGW(TAG, "Sin SD: se transmite igual, pero sin respaldo ni reenvio");
    }
    xTaskCreate(tx_task, "tx_task", 6144, NULL, 5, NULL);
}
#endif
