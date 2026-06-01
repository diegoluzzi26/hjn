#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "rc522.h"
#include "driver/rc522_spi.h"
#include "rc522_picc.h"

// ─── Configurações — edite aqui ───────────────────────────────────────────────
#define WIFI_SSID        "TP-Link_A232"
#define WIFI_PASS        "62707558"
#define MQTT_BROKER_URI  "mqtt://broker.hivemq.com:1883"   // IP ou hostname do broker
#define MQTT_TOPIC       "RFID"                 // Tópico MQTT para publicar os eventos
// ──────────────────────────────────────────────────────────────────────────────

static const char *TAG = "rfid";

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static esp_mqtt_client_handle_t mqtt_client = NULL;

// ─── RC522 ────────────────────────────────────────────────────────────────────

static rc522_spi_config_t driver_config = {
    .host_id = SPI3_HOST,
    .bus_config = &(spi_bus_config_t){
        .miso_io_num = 19,
        .mosi_io_num = 23,
        .sclk_io_num = 18,
    },
    .dev_config = {
        .spics_io_num = 5,
    },
    .rst_io_num = -1,
};

static rc522_driver_handle_t driver;
static rc522_handle_t scanner;

typedef struct {
    uint8_t uid[10];
    uint8_t length;
    const char *name;
} authorized_uid_t;

static const authorized_uid_t authorized_uids[] = {
    { .uid = {0xF3, 0x54, 0xB3, 0x29}, .length = 4, .name = "Cartão 1" },
    { .uid = {0x01, 0x02, 0x03, 0x04}, .length = 4, .name = "Cartão 2" },
};

#define AUTHORIZED_COUNT (sizeof(authorized_uids) / sizeof(authorized_uids[0]))

static const authorized_uid_t *find_authorized(const rc522_picc_uid_t *uid)
{
    for (int i = 0; i < AUTHORIZED_COUNT; i++) {
        if (uid->length == authorized_uids[i].length &&
            memcmp(uid->value, authorized_uids[i].uid, uid->length) == 0) {
            return &authorized_uids[i];
        }
    }
    return NULL;
}

// ─── Wi-Fi ────────────────────────────────────────────────────────────────────

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi desconectado, reconectando...");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) data;
        ESP_LOGI(TAG, "IP obtido: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,   wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Aguardando conexão Wi-Fi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Wi-Fi conectado");
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) data;
    switch ((esp_mqtt_event_id_t) event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT conectado");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT desconectado");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT erro");
            break;
        default:
            break;
    }
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

static void mqtt_publish_access(const rc522_picc_uid_t *uid, const authorized_uid_t *auth)
{
    if (mqtt_client == NULL) return;

    // Monta UID em formato "AA:BB:CC:DD"
    char uid_str[32] = {0};
    for (int i = 0; i < uid->length; i++) {
        char byte_str[4];
        snprintf(byte_str, sizeof(byte_str), i == 0 ? "%02X" : ":%02X", uid->value[i]);
        strncat(uid_str, byte_str, sizeof(uid_str) - strlen(uid_str) - 1);
    }

    char payload[128];
    if (auth != NULL) {
        snprintf(payload, sizeof(payload),
                 "{\"uid\":\"%s\",\"status\":\"PERMITIDO\",\"nome\":\"%s\"}",
                 uid_str, auth->name);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"uid\":\"%s\",\"status\":\"NEGADO\"}",
                 uid_str);
    }

    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, payload, 0, 1, 0);
    ESP_LOGI(TAG, "MQTT publicado: %s", payload);
}

// ─── Evento RFID ─────────────────────────────────────────────────────────────

static void on_picc_state_changed(void *arg, esp_event_base_t base,
                                   int32_t event_id, void *data)
{
    rc522_picc_state_changed_event_t *event =
        (rc522_picc_state_changed_event_t *) data;
    rc522_picc_t *picc = event->picc;

    if (picc->state == RC522_PICC_STATE_ACTIVE) {
        printf("Cartão detectado! UID: ");
        for (int i = 0; i < picc->uid.length; i++) {
            printf("%02X ", picc->uid.value[i]);
        }
        printf("\n");

        const authorized_uid_t *auth = find_authorized(&picc->uid);
        if (auth != NULL) {
            printf("ACESSO PERMITIDO — %s\n", auth->name);
        } else {
            printf("ACESSO NEGADO\n");
        }

        mqtt_publish_access(&picc->uid, auth);
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    wifi_init();
    mqtt_init();

    rc522_spi_create(&driver_config, &driver);
    rc522_driver_install(driver);

    rc522_config_t scanner_config = {
        .driver = driver,
    };

    rc522_create(&scanner_config, &scanner);
    rc522_register_events(scanner, RC522_EVENT_PICC_STATE_CHANGED, on_picc_state_changed, NULL);
    rc522_start(scanner);
}
