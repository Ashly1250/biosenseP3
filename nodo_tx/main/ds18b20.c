#include "ds18b20.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "ds18b20";
static gpio_num_t s_pin;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool ow_reset(void)
{
    bool presence;
    portENTER_CRITICAL(&s_mux);
    gpio_set_level(s_pin, 0);
    esp_rom_delay_us(480);
    gpio_set_level(s_pin, 1);
    esp_rom_delay_us(70);
    presence = (gpio_get_level(s_pin) == 0);
    portEXIT_CRITICAL(&s_mux);
    esp_rom_delay_us(410);
    return presence;
}

static void ow_write_bit(int bit)
{
    portENTER_CRITICAL(&s_mux);
    gpio_set_level(s_pin, 0);
    esp_rom_delay_us(bit ? 6 : 60);
    gpio_set_level(s_pin, 1);
    esp_rom_delay_us(bit ? 64 : 10);
    portEXIT_CRITICAL(&s_mux);
}

static int ow_read_bit(void)
{
    int bit;
    portENTER_CRITICAL(&s_mux);
    gpio_set_level(s_pin, 0);
    esp_rom_delay_us(3);
    gpio_set_level(s_pin, 1);
    esp_rom_delay_us(9);
    bit = gpio_get_level(s_pin);
    esp_rom_delay_us(55);
    portEXIT_CRITICAL(&s_mux);
    return bit;
}

static void ow_write_byte(uint8_t v)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(v & 1);
        v >>= 1;
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint8_t)(ow_read_bit() << i);
    }
    return v;
}

static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t crc = 0;
    while (n--) {
        uint8_t b = *d++;
        for (int i = 0; i < 8; i++) {
            uint8_t mix = (crc ^ b) & 1;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            b >>= 1;
        }
    }
    return crc;
}

esp_err_t ds18b20_init(gpio_num_t pin)
{
    s_pin = pin;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t e = gpio_config(&io);
    if (e == ESP_OK) gpio_set_level(pin, 1);
    return e;
}

esp_err_t ds18b20_read_temp_c(float *temp_c)
{
    if (!ow_reset()) {
        ESP_LOGW(TAG, "Sin presencia en el bus 1-Wire (revisar cableado/pull-up)");
        return ESP_ERR_NOT_FOUND;
    }
    ow_write_byte(0xCC);   /* SKIP ROM */
    ow_write_byte(0x44);   /* CONVERT T */
    vTaskDelay(pdMS_TO_TICKS(750));

    if (!ow_reset()) return ESP_ERR_NOT_FOUND;
    ow_write_byte(0xCC);
    ow_write_byte(0xBE);   /* READ SCRATCHPAD */

    uint8_t sp[9];
    for (int i = 0; i < 9; i++) sp[i] = ow_read_byte();

    if (crc8(sp, 8) != sp[8]) {
        ESP_LOGW(TAG, "CRC invalido en scratchpad");
        return ESP_ERR_INVALID_CRC;
    }
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    *temp_c = raw / 16.0f;
    return ESP_OK;
}
