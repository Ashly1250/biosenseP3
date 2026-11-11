#include "sd_store.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sd";

static sdmmc_card_t *s_card;
static bool     s_ready;
static uint32_t s_next_seq = 1;

/* Archivo de datos: una linea por lectura, siempre al final, nunca se reescribe.
   Puntero de pendientes = posicion (byte) de la primera lectura que el RX aun no confirmo. */
static long     s_end_off;        /* tamano del archivo = donde termina la ultima lectura */
static long     s_pend_off;       /* inicio de la lectura pendiente mas antigua */
static uint32_t s_pend_seq;       /* su SEQ */
static uint32_t s_last_seq;       /* ultima lectura escrita */
static long     s_last_len;       /* y su longitud en bytes (con salto de linea) */
static uint32_t s_peek_seq;
static long     s_peek_len;
static int64_t  s_last_save_us;

bool     sd_store_ready(void)     { return s_ready; }
uint32_t sd_store_next_seq(void)  { return s_next_seq; }
bool     sd_store_has_pending(void) { return s_ready && s_pend_off < s_end_off; }

uint32_t sd_store_pending_count(void)
{
    if (!sd_store_has_pending()) return 0;
    return (s_last_seq >= s_pend_seq) ? (s_last_seq - s_pend_seq + 1) : 0;
}

static void save_ack_state(bool force)
{
    int64_t now = esp_timer_get_time();
    if (!force && (now - s_last_save_us) < 10LL * 1000000LL) return;   /* maximo 1 vez cada 10 s */
    s_last_save_us = now;
    FILE *f = fopen(SD_ACK_FILE, "w");
    if (!f) { ESP_LOGW(TAG, "No se pudo guardar %s", SD_ACK_FILE); return; }
    fprintf(f, "%lu %ld\n", (unsigned long)s_pend_seq, s_pend_off);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

/* Lee la linea que empieza en 'off'. Devuelve true y su SEQ si es una lectura valida. */
static bool read_line_at(long off, char *line, size_t size, uint32_t *seq, long *len)
{
    FILE *f = fopen(SD_DATA_FILE, "r");
    if (!f) return false;
    bool ok = false;
    if (fseek(f, off, SEEK_SET) == 0 && fgets(line, (int)size, f)) {
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] == '\n') {
            char *end = NULL;
            unsigned long s = strtoul(line, &end, 10);
            if (end && *end == ',' && end != line) {
                if (seq) *seq = (uint32_t)s;
                if (len) *len = (long)l;
                while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
                ok = true;
            }
        }
    }
    fclose(f);
    return ok;
}

static void recover_state(void)
{
    struct stat st;
    if (stat(SD_DATA_FILE, &st) != 0) return;
    s_end_off = (long)st.st_size;

    /* ultimo SEQ escrito: se lee el final del archivo */
    FILE *f = fopen(SD_DATA_FILE, "r");
    if (f) {
        char tail[256];
        long start = s_end_off > (long)sizeof(tail) - 1 ? s_end_off - ((long)sizeof(tail) - 1) : 0;
        fseek(f, start, SEEK_SET);
        size_t n = fread(tail, 1, sizeof(tail) - 1, f);
        tail[n] = '\0';
        fclose(f);
        /* busca el inicio de la ultima linea completa */
        char *last_nl = strrchr(tail, '\n');
        if (last_nl) {
            *last_nl = '\0';
            char *prev = strrchr(tail, '\n');
            char *ln = prev ? prev + 1 : (start == 0 ? tail : NULL);
            if (ln) {
                char *end = NULL;
                unsigned long s = strtoul(ln, &end, 10);
                if (end && *end == ',' && end != ln) {
                    s_last_seq = (uint32_t)s;
                    s_next_seq = s_last_seq + 1;
                    s_last_len = (long)strlen(ln) + 1;
                }
            }
        }
    }

    /* puntero de pendientes guardado la ultima vez */
    unsigned long ps = 0; long po = 0;
    f = fopen(SD_ACK_FILE, "r");
    bool ok = false;
    if (f) {
        ok = (fscanf(f, "%lu %ld", &ps, &po) == 2);
        fclose(f);
    }
    if (ok && po >= 0 && po <= s_end_off) {
        char line[160]; uint32_t sq; long ln;
        if (po == s_end_off) {                          /* no habia nada pendiente */
            s_pend_off = po; s_pend_seq = s_next_seq;
            return;
        }
        if (read_line_at(po, line, sizeof(line), &sq, &ln) && sq == (uint32_t)ps) {
            s_pend_off = po; s_pend_seq = sq;
            ESP_LOGI(TAG, "Recuperado: hay %lu lecturas sin confirmar por el RX",
                     (unsigned long)(s_last_seq >= sq ? s_last_seq - sq + 1 : 0));
            return;
        }
    }
    /* sin estado valido: se asume todo confirmado (mejor perder un pendiente que inundar el canal) */
    ESP_LOGW(TAG, "Sin estado de ACK valido; se asume todo confirmado");
    s_pend_off = s_end_off; s_pend_seq = s_next_seq;
}

esp_err_t sd_store_init(void)
{
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
        ESP_LOGE(TAG, "No se pudo montar la SD (%s). Revisa cableado y que este en FAT32.",
                 esp_err_to_name(e));
        return e;
    }
    sdmmc_card_print_info(stdout, s_card);

    struct stat st;
    if (stat(SD_DATA_FILE, &st) != 0 || st.st_size == 0) {
        FILE *f = fopen(SD_DATA_FILE, "w");
        if (!f) { ESP_LOGE(TAG, "No se pudo crear %s", SD_DATA_FILE); return ESP_FAIL; }
        fprintf(f, "%s\n", SD_CSV_HEADER);
        fflush(f); fsync(fileno(f)); fclose(f);
        stat(SD_DATA_FILE, &st);
        s_end_off = (long)st.st_size;
        s_pend_off = s_end_off;
        s_pend_seq = 1;
        s_next_seq = 1;
        ESP_LOGI(TAG, "Archivo %s creado", SD_DATA_FILE);
    } else {
        recover_state();
        ESP_LOGI(TAG, "Archivo %s listo (%ld bytes), siguiente SEQ = %lu",
                 SD_DATA_FILE, s_end_off, (unsigned long)s_next_seq);
    }
    s_ready = true;
    return ESP_OK;
}

esp_err_t sd_store_append(uint32_t seq, const char *line)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(SD_DATA_FILE, "a");
    if (!f) { ESP_LOGE(TAG, "Fallo al abrir %s", SD_DATA_FILE); return ESP_FAIL; }
    int n = fprintf(f, "%s\n", line);
    fflush(f);
    fsync(fileno(f));            /* fuerza escritura fisica: sobrevive a un corte de energia */
    fclose(f);
    if (n <= 0) return ESP_FAIL;

    bool was_empty = (s_pend_off >= s_end_off);
    long start = s_end_off;
    s_end_off += n;
    s_last_seq = seq;
    s_last_len = n;
    s_next_seq = seq + 1;
    if (was_empty) { s_pend_off = start; s_pend_seq = seq; }
    return ESP_OK;
}

esp_err_t sd_store_peek(char *line, size_t size, uint32_t *seq)
{
    if (!sd_store_has_pending()) return ESP_ERR_NOT_FOUND;
    uint32_t s = 0; long l = 0;
    if (!read_line_at(s_pend_off, line, size, &s, &l)) {
        ESP_LOGW(TAG, "Linea pendiente ilegible; se salta el resto");
        s_pend_off = s_end_off; s_pend_seq = s_next_seq;
        save_ack_state(true);
        return ESP_FAIL;
    }
    s_peek_seq = s; s_peek_len = l;
    if (seq) *seq = s;
    return ESP_OK;
}

void sd_store_mark_acked(uint32_t seq)
{
    if (!sd_store_has_pending() || seq != s_pend_seq) return;

    long len = 0;
    if (seq == s_last_seq)      len = s_last_len;
    else if (seq == s_peek_seq) len = s_peek_len;
    if (len <= 0) return;

    s_pend_off += len;
    s_pend_seq = seq + 1;
    save_ack_state(!sd_store_has_pending());    /* al quedar al dia, se guarda de inmediato */
}
