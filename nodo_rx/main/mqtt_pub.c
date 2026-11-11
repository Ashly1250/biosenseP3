#include "mqtt_pub.h"
#include <string.h>
#include "mqtt_client.h"
#include "esp_log.h"
#include "secrets.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;

static void on_mqtt(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "Conectado al broker " MQTT_URI);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "Desconectado del broker (se reintenta solo)");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "Error MQTT (revisa IP, usuario y clave del broker)");
        break;
    default:
        break;
    }
}

bool mqtt_pub_connected(void) { return s_connected; }

esp_err_t mqtt_pub_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_URI,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) return ESP_FAIL;
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt, NULL));
    return esp_mqtt_client_start(s_client);
}

bool mqtt_pub_publish(const char *payload)
{
    if (!s_client) return false;
    /* enqueue guarda el mensaje en una cola interna si el broker no esta disponible
       (por unos minutos); la copia completa siempre queda en la SD. */
    int id = esp_mqtt_client_enqueue(s_client, MQTT_TOPIC, payload, 0, 1, 0, true);
    return id >= 0;
}
