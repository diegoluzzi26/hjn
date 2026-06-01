#include "mqtt.h"

static const char *TAG = "LED_MQTT";
esp_mqtt_client_handle_t mqtt_client;

void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data){

    esp_mqtt_event_handle_t event = event_data;

    switch (event_id){
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Conectado ao broker!");
            esp_mqtt_client_subscribe(event->client, MQTT_TOPIC, 0);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Mensagem recebida: %.*s", event->data_len, event->data);

            if(strncmp(event->data, "ligar led", event->data_len) == 0){
                gpio_set_level(LED_VERMELHO_GPIO, 1);
                ESP_LOGI(TAG, "LED LIGADO");
            }else if(strncmp(event->data, "desligar led", event->data_len) == 0){
                gpio_set_level(LED_VERMELHO_GPIO, 0);
                ESP_LOGI(TAG, "LED DESLIGADO");
            }
            break;

        default:
            break;
    }
}

void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data){
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START){
        esp_wifi_connect();
    }else if(base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP){
        ESP_LOGI(TAG, "Wifi conectado");
    }
}

void escreverMqtt(const char *Mensagem){
    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, Mensagem, 0, 0, 0);
    ESP_LOGI(TAG, "Mensagem enviada: %s", Mensagem);
}