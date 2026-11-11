#pragma once
/* ---------- SD por SPI (bus SPI3, independiente del LoRa que usa SPI2) ---------- */
#define SD_SPI_HOST      SPI3_HOST
#define SD_PIN_CS        26
#define SD_PIN_SCK       47
#define SD_PIN_MOSI      48
#define SD_PIN_MISO      45
#define SD_FREQ_KHZ      4000      /* 4 MHz: estable en protoboard */

/* Pin Vext de la Heltec V3 (LOW = alimenta el pin Vext 3.3V) */
#define VEXT_CTRL_PIN    36

#define SD_MOUNT_POINT   "/sdcard"
/* Si por algun motivo tu SD da problemas con .csv, cambia a "/sdcard/datos.txt" */
#define SD_FILE_PATH     SD_MOUNT_POINT "/datos.txt"
#define CSV_HEADER       "FECHA_HORA,TEMP_C,TDS_PPM,PH,OD_MGL,SST_MGL,RSSI_DBM,SNR_DB"

#define RX_POLL_TIMEOUT_MS  1000
