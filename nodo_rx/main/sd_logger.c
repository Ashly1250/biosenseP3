#include "sd_logger.h"
#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sd";
static sdmmc_card_t *s_card;
static bool s_ready;

bool sd_logger_ready(void) { return s_ready; }

static esp_err_t ensure_header(void)
{
    struct stat st;
    if (stat(SD_FILE_PATH, &st) == 0 && st.st_size > 0) {
        ESP_LOGI(TAG, "Archivo %s listo (%ld bytes)", SD_FILE_PATH, (long)st.st_size);
        return ESP_OK;
    }
    FILE *f = fopen(SD_FILE_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "No se pudo crear %s", SD_FILE_PATH);
        return ESP_FAIL;
    }
    fprintf(f, "%s\n", CSV_HEADER);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    ESP_LOGI(TAG, "Archivo %s creado con encabezados", SD_FILE_PATH);
    return ESP_OK;
}

esp_err_t sd_logger_init(void)
{
    /* Vext ON */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << VEXT_CTRL_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(VEXT_CTRL_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    spi_bus_config_t bus = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t e = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(e));
        return e;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = SD_SPI_HOST;

    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false,   /* nunca formatear sin permiso */
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    e = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo montar la SD (%s). Verifica cableado y que este en FAT32.",
                 esp_err_to_name(e));
        return e;
    }
    sdmmc_card_print_info(stdout, s_card);

    e = ensure_header();
    s_ready = (e == ESP_OK);
    return e;
}

esp_err_t sd_logger_append(const char *trama,
                           int rssi_dbm,
                           float snr_db)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(SD_FILE_PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "Fallo al abrir %s para escribir", SD_FILE_PATH);
        return ESP_FAIL;
    }
    int n = fprintf(f, "%s,%d,%.2f\n", trama, rssi_dbm, snr_db);
    fflush(f);
    fsync(fileno(f));          /* fuerza escritura fisica a la tarjeta */
    fclose(f);
    return (n > 0) ? ESP_OK : ESP_FAIL;
}
