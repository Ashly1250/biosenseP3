#pragma once
/* Datos de tu red y del broker MQTT de la Raspberry.
   NO subas este archivo a GitHub con la clave real. */

#define WIFI_SSID   "BioSense"          /* solo redes de 2.4 GHz */
#define WIFI_PASS   "larry726"

/* IP de la Raspberry en esa misma red (en la Raspberry: hostname -I) */
#define MQTT_URI    "mqtt://10.91.36.95:1883"
#define MQTT_USER   "biosense"
#define MQTT_PASS   "larry726"    /* la misma que pongas en mosquitto_passwd */
#define MQTT_TOPIC  "biosense/rx/data"
