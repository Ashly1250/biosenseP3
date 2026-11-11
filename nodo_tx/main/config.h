#pragma once

/* ---------- Pines de sensores (Heltec V3) ---------- */
#define PIN_ONE_WIRE   4
#define PIN_TDS        2
#define PIN_PH         5
#define PIN_OD         6
#define PIN_TSS        7
/* RTC DS3231 */
#define RTC_PIN_SDA    19
#define RTC_PIN_SCL    20


/* ---------- pH ---------- */
   #define PH_DIVIDER_RATIO  1.0f
   #define PH_MODE           1
   #define PH_LIN_M          (-5.70f)
   #define PH_LIN_B          21.34f

/* ---------- ADC ---------- */
#define ADC_SAMPLES         16

#define SIM_FULLSCALE_MV    3100.0f

/* ---------- Sensores simulados ---------- */
#define OD_MAX_MGL          20.0f
#define SST_MAX_MGL         1000.0f

/* ---------- Calibracion pH ---------- */
#define PH_CAL_PH_A         7.00f
#define PH_CAL_MV_A         2500.0f

#define PH_CAL_PH_B         4.00f
#define PH_CAL_MV_B         3030.0f

#define PH_CAL_TEMP_C       25.0f

#define PH_CAL_MODE         0

#define PH_ERROR_VALUE      (-1.0f)

#define TX_PERIOD_MS        5000
#define TX_TIMEOUT_MS       3000

#define TEMP_ERROR_VALUE    (-127.0f)

/* ---------- SD (modulo microSD por SPI, bus SPI3; el LoRa usa SPI2) ---------- */
#define SD_SPI_HOST         SPI3_HOST
#define SD_PIN_CS           26
#define SD_PIN_SCK          47     /* CLK  */
#define SD_PIN_MOSI         48
#define SD_PIN_MISO         45
#define SD_FREQ_KHZ         4000
#define SD_MOUNT_POINT      "/sdcard"
#define SD_DATA_FILE        SD_MOUNT_POINT "/datos.txt"   /* todas las lecturas (nunca se borra) */
#define SD_ACK_FILE         SD_MOUNT_POINT "/ack.txt"     /* hasta donde ya confirmo el RX */
#define SD_CSV_HEADER       "SEQ,FECHA_HORA,TEMP_C,TDS_PPM,PH,OD_MGL,SST_MGL"

/* ---------- Persistencia por LoRa (ACK + reenvio desde la SD) ---------- */
#define ENABLE_ACK_REPLAY   1      /* 1 = espera ACK del RX y reenvia lo pendiente; 0 = como antes */
#define ACK_WAIT_MS         600    /* cuanto espera el ACK despues de cada envio */
#define REPLAY_PER_CYCLE    4      /* cuantas lecturas atrasadas reenvia por ciclo */

/* ---------- RTC ---------- */
/* 1 = al arrancar, ajusta el DS3231 con la hora a la que se COMPILO (hora de tu PC).
   Ponlo en 1, compila y flashea UNA vez; luego vuelve a 0 y flashea otra vez. */
#define RTC_SET_FROM_BUILD  0