#include "sensors.h"
#include "config.h"
#include "ds18b20.h"
#include "ph_sensor.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "sensors";

enum { CH_TDS, CH_PH, CH_OD, CH_TSS, CH_COUNT };

typedef struct {
    int gpio;
    adc_channel_t ch;
    adc_cali_handle_t cali;   /* NULL si no hay calibracion disponible */
} adc_input_t;

static adc_oneshot_unit_handle_t s_adc;
static adc_input_t s_in[CH_COUNT] = {
    [CH_TDS] = { .gpio = PIN_TDS },
    [CH_PH]  = { .gpio = PIN_PH  },
    [CH_OD]  = { .gpio = PIN_OD  },
    [CH_TSS] = { .gpio = PIN_TSS },
};

esp_err_t sensors_init(void)
{
    ESP_ERROR_CHECK(ds18b20_init(PIN_ONE_WIRE));

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));

    for (int i = 0; i < CH_COUNT; i++) {
        adc_unit_t unit;
        ESP_ERROR_CHECK(adc_oneshot_io_to_channel(s_in[i].gpio, &unit, &s_in[i].ch));

        adc_oneshot_chan_cfg_t ccfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_in[i].ch, &ccfg));

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cal = {
            .unit_id = ADC_UNIT_1,
            .chan = s_in[i].ch,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cal, &s_in[i].cali) != ESP_OK) {
            s_in[i].cali = NULL;
            ESP_LOGW(TAG, "GPIO%d sin calibracion ADC, se usara conversion lineal", s_in[i].gpio);
        }
#endif
    }
    ESP_LOGI(TAG, "Sensores inicializados");
    return ESP_OK;
}

/* Promedia ADC_SAMPLES lecturas y devuelve milivoltios. */
static float read_mv(int idx)
{
    int sum = 0, raw = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        if (adc_oneshot_read(s_adc, s_in[idx].ch, &raw) == ESP_OK) sum += raw;
        esp_rom_delay_us(200);
    }
    int avg = sum / ADC_SAMPLES;
    int mv = 0;
    if (s_in[idx].cali && adc_cali_raw_to_voltage(s_in[idx].cali, avg, &mv) == ESP_OK) {
        return (float)mv;
    }
    return avg * 3100.0f / 4095.0f;
}

static float sim_fraction(int idx)
{
    float f = read_mv(idx) / SIM_FULLSCALE_MV;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

float sensors_read_ph_adc_mv(void)
{
    return read_mv(CH_PH);
}

esp_err_t sensors_read(sensor_data_t *out)
{
    /* Temperatura */
    float t = 0;
    if (ds18b20_read_temp_c(&t) == ESP_OK) {
        out->temp_c = t;
    } else {
        out->temp_c = TEMP_ERROR_VALUE;
        t = 25.0f;                 /* para compensar TDS y pH sin datos de temperatura */
    }

    /* TDS (formula DFRobot con compensacion por temperatura) */
    float v = read_mv(CH_TDS) / 1000.0f;
    float comp = 1.0f + 0.02f * (t - 25.0f);
    float vc = v / comp;
    out->tds_ppm = (133.42f * vc * vc * vc - 255.86f * vc * vc + 857.39f * vc) * 0.5f;
    if (out->tds_ppm < 0) out->tds_ppm = 0;

    /* pH: sensor real, calibracion de dos puntos + compensacion por temperatura */
    out->ph = ph_from_adc_mv(read_mv(CH_PH), t);

    /* Simulados con potenciometro */
    out->od_mgl  = sim_fraction(CH_OD)  * OD_MAX_MGL;
    out->sst_mgl = sim_fraction(CH_TSS) * SST_MAX_MGL;
    return ESP_OK;
}
