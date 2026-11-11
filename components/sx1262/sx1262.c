#include "sx1262.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

static const char *TAG = "sx1262";

/* Opcodes */
#define OP_SET_STANDBY        0x80
#define OP_SET_TX             0x83
#define OP_SET_RX             0x82
#define OP_SET_PKT_TYPE       0x8A
#define OP_SET_RF_FREQ        0x86
#define OP_SET_PA_CONFIG      0x95
#define OP_SET_TX_PARAMS      0x8E
#define OP_SET_BUF_BASE       0x8F
#define OP_SET_MOD_PARAMS     0x8B
#define OP_SET_PKT_PARAMS     0x8C
#define OP_SET_DIO_IRQ        0x08
#define OP_GET_IRQ            0x12
#define OP_CLR_IRQ            0x02
#define OP_SET_DIO2_RFSW      0x9D
#define OP_SET_DIO3_TCXO      0x97
#define OP_SET_REG_MODE       0x96
#define OP_CALIBRATE          0x89
#define OP_CALIBRATE_IMAGE    0x98
#define OP_WRITE_REG          0x0D
#define OP_WRITE_BUF          0x0E
#define OP_READ_BUF           0x1E
#define OP_GET_RX_BUF_STATUS  0x13
#define OP_GET_PKT_STATUS     0x14

/* IRQ */
#define IRQ_TX_DONE    0x0001
#define IRQ_RX_DONE    0x0002
#define IRQ_HEADER_ERR 0x0020
#define IRQ_CRC_ERR    0x0040
#define IRQ_TIMEOUT    0x0200
#define IRQ_USED  (IRQ_TX_DONE | IRQ_RX_DONE | IRQ_HEADER_ERR | IRQ_CRC_ERR | IRQ_TIMEOUT)

#define CHECK(x) do { esp_err_t _e = (x); if (_e != ESP_OK) return _e; } while (0)

static sx1262_hw_t s_hw;
static sx1262_cfg_t s_cfg;
static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_irq_sem;

/* ---------------------------------------------------------------- bajo nivel */

static esp_err_t wait_busy(uint32_t timeout_ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (gpio_get_level(s_hw.pin_busy)) {
        if (esp_timer_get_time() > end) {
            ESP_LOGE(TAG, "BUSY no bajo (revisar conexion/alimentacion)");
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(10);
    }
    return ESP_OK;
}

static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    CHECK(wait_busy(200));
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t cmd_write(uint8_t op, const uint8_t *data, size_t n)
{
    uint8_t buf[260] __attribute__((aligned(4)));
    buf[0] = op;
    if (n) memcpy(buf + 1, data, n);
    return xfer(buf, NULL, n + 1);
}

/* Comandos de lectura: [op][status][datos...] */
static esp_err_t cmd_read(uint8_t op, uint8_t *out, size_t n)
{
    uint8_t tx[8] __attribute__((aligned(4))) = {0};
    uint8_t rx[8] __attribute__((aligned(4)));
    tx[0] = op;
    CHECK(xfer(tx, rx, n + 2));
    memcpy(out, rx + 2, n);
    return ESP_OK;
}

static esp_err_t write_reg(uint16_t addr, const uint8_t *data, size_t n)
{
    uint8_t buf[8];
    buf[0] = addr >> 8;
    buf[1] = addr & 0xFF;
    memcpy(buf + 2, data, n);
    return cmd_write(OP_WRITE_REG, buf, n + 2);
}

static esp_err_t write_buffer(const uint8_t *data, size_t len)
{
    uint8_t buf[258] __attribute__((aligned(4)));
    buf[0] = 0x00; /* offset */
    memcpy(buf + 1, data, len);
    return cmd_write(OP_WRITE_BUF, buf, len + 1);
}

static esp_err_t read_buffer(uint8_t offset, uint8_t *out, size_t len)
{
    uint8_t tx[260] __attribute__((aligned(4))) = {0};
    uint8_t rx[260] __attribute__((aligned(4)));
    tx[0] = OP_READ_BUF;
    tx[1] = offset;
    CHECK(xfer(tx, rx, len + 3));
    memcpy(out, rx + 3, len);
    return ESP_OK;
}

static esp_err_t get_irq(uint16_t *irq)
{
    uint8_t d[2];
    CHECK(cmd_read(OP_GET_IRQ, d, 2));
    *irq = ((uint16_t)d[0] << 8) | d[1];
    return ESP_OK;
}

static esp_err_t clear_irq(uint16_t mask)
{
    uint8_t d[2] = { mask >> 8, mask & 0xFF };
    return cmd_write(OP_CLR_IRQ, d, 2);
}

static esp_err_t set_packet_params(uint8_t payload_len)
{
    uint8_t p[6] = {
        s_cfg.preamble_len >> 8, s_cfg.preamble_len & 0xFF,
        0x00,         /* cabecera explicita */
        payload_len,
        0x01,         /* CRC activado */
        0x00          /* IQ normal */
    };
    return cmd_write(OP_SET_PKT_PARAMS, p, 6);
}

static void IRAM_ATTR dio1_isr(void *arg)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_irq_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

/* Espera una IRQ del radio. timeout_ms = 0 -> consulta una sola vez. */
static esp_err_t wait_irq(uint32_t timeout_ms, uint16_t *irq)
{
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (1) {
        if (!gpio_get_level(s_hw.pin_dio1)) {
            int64_t rem_ms = (end - esp_timer_get_time()) / 1000;
            if (rem_ms <= 0) return ESP_ERR_TIMEOUT;
            if (rem_ms > 100) rem_ms = 100;      /* despierta cada 100 ms por seguridad */
            TickType_t ticks = pdMS_TO_TICKS(rem_ms);
            if (ticks == 0) ticks = 1;
            xSemaphoreTake(s_irq_sem, ticks);
        }
        uint16_t st = 0;
        CHECK(get_irq(&st));
        if (st & IRQ_USED) {
            *irq = st;
            return ESP_OK;
        }
        if (esp_timer_get_time() >= end) return ESP_ERR_TIMEOUT;
    }
}

static void calibrate_image(uint32_t f)
{
    uint8_t p[2];
    if (f >= 902000000UL)      { p[0] = 0xE1; p[1] = 0xE9; }
    else if (f >= 863000000UL) { p[0] = 0xD7; p[1] = 0xDB; }
    else if (f >= 779000000UL) { p[0] = 0xC1; p[1] = 0xC5; }
    else if (f >= 470000000UL) { p[0] = 0x75; p[1] = 0x81; }
    else                       { p[0] = 0x6B; p[1] = 0x6F; }
    cmd_write(OP_CALIBRATE_IMAGE, p, 2);
}

/* ---------------------------------------------------------------- API */

esp_err_t sx1262_init(const sx1262_hw_t *hw, const sx1262_cfg_t *cfg)
{
    s_hw = *hw;
    s_cfg = *cfg;
    s_irq_sem = xSemaphoreCreateBinary();

    /* GPIO */
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << s_hw.pin_rst,
        .mode = GPIO_MODE_OUTPUT,
    };
    CHECK(gpio_config(&out));
    gpio_set_level(s_hw.pin_rst, 1);

    gpio_config_t in = {
        .pin_bit_mask = 1ULL << s_hw.pin_busy,
        .mode = GPIO_MODE_INPUT,
    };
    CHECK(gpio_config(&in));

    gpio_config_t dio = {
        .pin_bit_mask = 1ULL << s_hw.pin_dio1,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    CHECK(gpio_config(&dio));
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    CHECK(gpio_isr_handler_add(s_hw.pin_dio1, dio1_isr, NULL));

    /* SPI */
    spi_bus_config_t bus = {
        .mosi_io_num = s_hw.pin_mosi,
        .miso_io_num = s_hw.pin_miso,
        .sclk_io_num = s_hw.pin_sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 260,
    };
    CHECK(spi_bus_initialize(s_hw.spi_host, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 8 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = s_hw.pin_nss,
        .queue_size = 1,
    };
    CHECK(spi_bus_add_device(s_hw.spi_host, &dev, &s_spi));

    /* Reset por hardware */
    gpio_set_level(s_hw.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(s_hw.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    CHECK(wait_busy(500));

    /* Configuracion */
    uint8_t b[8];
    b[0] = 0x00; /* STDBY_RC */
    CHECK(cmd_write(OP_SET_STANDBY, b, 1));

    if (s_hw.tcxo_1v8) {
        /* 1.8 V, tiempo de arranque 5 ms (320 * 15.625 us = 0x000140) */
        b[0] = 0x02; b[1] = 0x00; b[2] = 0x01; b[3] = 0x40;
        CHECK(cmd_write(OP_SET_DIO3_TCXO, b, 4));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    b[0] = 0x7F; /* calibrar todo */
    CHECK(cmd_write(OP_CALIBRATE, b, 1));
    vTaskDelay(pdMS_TO_TICKS(20));
    CHECK(wait_busy(500));

    b[0] = 0x01; CHECK(cmd_write(OP_SET_DIO2_RFSW, b, 1));  /* DIO2 controla el switch RF */
    b[0] = 0x01; CHECK(cmd_write(OP_SET_REG_MODE, b, 1));   /* DC-DC */
    b[0] = 0x01; CHECK(cmd_write(OP_SET_PKT_TYPE, b, 1));   /* LoRa */

    calibrate_image(s_cfg.freq_hz);
    uint32_t frf = (uint32_t)(((uint64_t)s_cfg.freq_hz << 25) / 32000000ULL);
    b[0] = frf >> 24; b[1] = frf >> 16; b[2] = frf >> 8; b[3] = frf;
    CHECK(cmd_write(OP_SET_RF_FREQ, b, 4));

    /* PA para SX1262 */
    b[0] = 0x04; b[1] = 0x07; b[2] = 0x00; b[3] = 0x01;
    CHECK(cmd_write(OP_SET_PA_CONFIG, b, 4));
    int8_t pw = s_cfg.tx_power_dbm;
    if (pw > 22) pw = 22;
    if (pw < -9) pw = -9;
    b[0] = (uint8_t)pw; b[1] = 0x04; /* rampa 200 us */
    CHECK(cmd_write(OP_SET_TX_PARAMS, b, 2));

    b[0] = 0x00; b[1] = 0x00;
    CHECK(cmd_write(OP_SET_BUF_BASE, b, 2));

    uint8_t ldro = (s_cfg.sf >= 11 && s_cfg.bw == SX1262_BW_125KHZ) ? 1 : 0;
    b[0] = s_cfg.sf; b[1] = s_cfg.bw; b[2] = s_cfg.cr; b[3] = ldro;
    CHECK(cmd_write(OP_SET_MOD_PARAMS, b, 4));

    CHECK(set_packet_params(255));

    b[0] = s_cfg.sync_word >> 8; b[1] = s_cfg.sync_word & 0xFF;
    CHECK(write_reg(0x0740, b, 2));

    b[0] = 0x96; /* ganancia RX reforzada */
    CHECK(write_reg(0x08AC, b, 1));

    /* IRQs enrutadas a DIO1 */
    b[0] = IRQ_USED >> 8; b[1] = IRQ_USED & 0xFF;
    b[2] = b[0];          b[3] = b[1];
    b[4] = 0; b[5] = 0; b[6] = 0; b[7] = 0;
    CHECK(cmd_write(OP_SET_DIO_IRQ, b, 8));
    CHECK(clear_irq(0xFFFF));

    ESP_LOGI(TAG, "SX1262 listo: %lu Hz, SF%u, CR4/%u, %d dBm",
             (unsigned long)s_cfg.freq_hz, s_cfg.sf, s_cfg.cr + 4, pw);
    return ESP_OK;
}

esp_err_t sx1262_send(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (len == 0 || len > 255) return ESP_ERR_INVALID_SIZE;

    CHECK(set_packet_params((uint8_t)len));
    CHECK(clear_irq(0xFFFF));
    CHECK(write_buffer(data, len));

    uint8_t t[3] = {0, 0, 0};            /* sin timeout interno */
    CHECK(cmd_write(OP_SET_TX, t, 3));

    uint16_t irq = 0;
    esp_err_t e = wait_irq(timeout_ms, &irq);
    clear_irq(0xFFFF);
    if (e != ESP_OK) return e;
    return (irq & IRQ_TX_DONE) ? ESP_OK : ESP_FAIL;
}

esp_err_t sx1262_start_rx(void)
{
    CHECK(set_packet_params(255));
    CHECK(clear_irq(0xFFFF));
    uint8_t t[3] = {0xFF, 0xFF, 0xFF};   /* recepcion continua */
    return cmd_write(OP_SET_RX, t, 3);
}

esp_err_t sx1262_receive(uint8_t *buf, size_t max_len, size_t *out_len,
                         int16_t *rssi_dbm, float *snr_db, uint32_t timeout_ms)
{
    uint16_t irq = 0;
    CHECK(wait_irq(timeout_ms, &irq));
    CHECK(clear_irq(0xFFFF));

    if (irq & (IRQ_CRC_ERR | IRQ_HEADER_ERR)) return ESP_ERR_INVALID_CRC;
    if (!(irq & IRQ_RX_DONE)) return ESP_ERR_INVALID_RESPONSE;

    uint8_t bs[2], ps[3];
    CHECK(cmd_read(OP_GET_RX_BUF_STATUS, bs, 2));
    CHECK(cmd_read(OP_GET_PKT_STATUS, ps, 3));

    size_t len = bs[0];
    if (len > max_len) len = max_len;
    CHECK(read_buffer(bs[1], buf, len));

    *out_len = len;
    if (rssi_dbm) *rssi_dbm = (int16_t)(-(int)ps[0] / 2);
    if (snr_db)   *snr_db = (float)((int8_t)ps[1]) / 4.0f;
    return ESP_OK;
}
