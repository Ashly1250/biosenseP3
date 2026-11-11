#include "biosense_radio.h"

esp_err_t biosense_radio_init(void)
{
    const sx1262_hw_t hw = {
        .spi_host  = HELTEC_V3_LORA_SPI_HOST,
        .pin_sck   = HELTEC_V3_LORA_SCK,
        .pin_mosi  = HELTEC_V3_LORA_MOSI,
        .pin_miso  = HELTEC_V3_LORA_MISO,
        .pin_nss   = HELTEC_V3_LORA_NSS,
        .pin_rst   = HELTEC_V3_LORA_RST,
        .pin_busy  = HELTEC_V3_LORA_BUSY,
        .pin_dio1  = HELTEC_V3_LORA_DIO1,
        .tcxo_1v8  = true,
    };
    const sx1262_cfg_t cfg = {
        .freq_hz      = BIOSENSE_LORA_FREQ_HZ,
        .tx_power_dbm = BIOSENSE_LORA_TX_POWER_DBM,
        .sf           = BIOSENSE_LORA_SF,
        .bw           = BIOSENSE_LORA_BW,
        .cr           = BIOSENSE_LORA_CR,
        .preamble_len = BIOSENSE_LORA_PREAMBLE,
        .sync_word    = BIOSENSE_LORA_SYNC_WORD,
    };
    return sx1262_init(&hw, &cfg);
}
