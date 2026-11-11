#include "rtc_ds3231.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#define DS3231_ADDR 0x68

static const char *TAG = "RTC";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

static uint8_t dec_to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int bcd_to_dec(uint8_t b)
{
    return ((b >> 4) * 10) + (b & 0x0F);
}

esp_err_t rtc_ds3231_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = RTC_PIN_SDA,
        .scl_io_num = RTC_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(
        i2c_new_master_bus(
            &bus_cfg,
            &s_bus
        )
    );

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_ADDR,
        .scl_speed_hz = 100000,
    };

    ESP_ERROR_CHECK(
        i2c_master_bus_add_device(
            s_bus,
            &dev_cfg,
            &s_dev
        )
    );

    ESP_LOGI(
        TAG,
        "DS3231 inicializado"
    );

    return ESP_OK;
}

void rtc_ds3231_get_timestamp(
    char *buffer,
    size_t len
)
{
    uint8_t reg = 0x00;
    uint8_t data[7];

    esp_err_t err;

    err = i2c_master_transmit(
        s_dev,
        &reg,
        1,
        1000
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Error seleccionando registro: %s",
            esp_err_to_name(err)
        );

        snprintf(buffer, len, "RTC_ERROR");

        return;
    }

    err = i2c_master_receive(
        s_dev,
        data,
        sizeof(data),
        1000
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Error leyendo RTC: %s",
            esp_err_to_name(err)
        );

        snprintf(buffer, len, "RTC_ERROR");

        return;
    }

    int sec   = bcd_to_dec(data[0] & 0x7F);
    int min   = bcd_to_dec(data[1] & 0x7F);
    int hour  = bcd_to_dec(data[2] & 0x3F);
    int day   = bcd_to_dec(data[4] & 0x3F);
    int month = bcd_to_dec(data[5] & 0x1F);
    int year  = 2000 + bcd_to_dec(data[6]);

    ESP_LOGI(
        TAG,
        "%02d/%02d/%04d %02d:%02d:%02d",
        day,
        month,
        year,
        hour,
        min,
        sec
    );

    snprintf(
        buffer,
        len,
        "%04d-%02d-%02d %02d:%02d:%02d",
        year,
        month,
        day,
        hour,
        min,
        sec
    );
}

esp_err_t rtc_ds3231_set(int year, int month, int day, int hour, int min, int sec)
{
    /* dia de la semana (1=lunes..7=domingo), formula de Sakamoto */
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year - (month < 3);
    int dow = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;   /* 0=domingo */
    if (dow == 0) dow = 7;

    uint8_t buf[8] = {
        0x00,                         /* registro inicial */
        dec_to_bcd(sec), dec_to_bcd(min), dec_to_bcd(hour),
        dec_to_bcd(dow), dec_to_bcd(day), dec_to_bcd(month), dec_to_bcd(year - 2000)
    };
    esp_err_t e = i2c_master_transmit(s_dev, buf, sizeof(buf), 1000);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo ajustar el DS3231: %s", esp_err_to_name(e));
        return e;
    }
    /* limpia el bit OSF (0x0F bit7) para marcar que la hora ya es valida */
    uint8_t st[2] = { 0x0F, 0x00 };
    e = i2c_master_transmit(s_dev, st, sizeof(st), 1000);
    ESP_LOGI(TAG, "DS3231 ajustado a %04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, min, sec);
    return e;
}

esp_err_t rtc_ds3231_set_from_build(void)
{
    static const char *meses = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day, year, hh, mm, ss;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3) {
        return ESP_FAIL;
    }
    const char *p = strstr(meses, mon);
    int month = p ? (int)((p - meses) / 3) + 1 : 1;
    return rtc_ds3231_set(year, month, day, hh, mm, ss);
}
