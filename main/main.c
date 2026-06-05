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
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "rc522_picc.h"

// ─── Configurações — edite aqui ───────────────────────────────────────────────
#define WIFI_SSID        "Angelika"
#define WIFI_PASS        "98472222"
#define MQTT_BROKER_URI  "mqtt://broker.hivemq.com:1883"   // IP ou hostname do broker
#define MQTT_TOPIC       "RFID"                 // Tópico MQTT para publicar os eventos

#define LED_VERMELHO_GPIO   22

#define LED_VERDE_GPIO      4

// ──────────────────────────────────────────────────────────────────────────────

static const char *TAG = "rfid";

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t mqtt_event_group;
#define MQTT_CONNECTED_BIT BIT0

static esp_mqtt_client_handle_t mqtt_client = NULL;// Handle do cliente MQTT

static QueueHandle_t led_queue = NULL;// Fila para comandos de LED

static SemaphoreHandle_t mqtt_mutex = NULL;// Mutex para proteger o acesso ao cliente MQTT

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
        .clock_speed_hz = 5000000,
    },
    .rst_io_num = -1,
};

static rc522_driver_handle_t driver;
static rc522_handle_t scanner;

typedef struct {
    uint8_t uid[10];
    uint8_t length;
    const char *name;
} authorized_uid_t;// Estrutura para armazenar UID autorizados

typedef enum {
    LED_ON_PERMITIDO,
    LED_ON_NEGADO,
} led_cmd_t;// Tipos de comando para o LED


static const authorized_uid_t authorized_uids[] = {
    { .uid = {0xF3, 0x54, 0xB3, 0x29}, .length = 4, .name = "Cartão branco" },
    { .uid = {0x01, 0x02, 0x03, 0x04}, .length = 4, .name = "Cartão azul" },
};// Lista de UID autorizados

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
}// Função para verificar se o UID é autorizado

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
}// Manipulador de eventos para Wi-Fi e IP

static void leds_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_VERDE_GPIO) | (1ULL << LED_VERMELHO_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_VERDE_GPIO, 0);
    gpio_set_level(LED_VERMELHO_GPIO, 0);
}// Configura os pinos dos LEDs como saída e os desliga

static void led_task(void *arg)
{
    led_cmd_t cmd;
    while (1) {
        xQueueReceive(led_queue, &cmd, portMAX_DELAY);
        if (cmd == LED_ON_PERMITIDO) {
            gpio_set_level(LED_VERDE_GPIO, 1);
            gpio_set_level(LED_VERMELHO_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(2000));// Mantém o LED verde aceso por 2 segundos
            gpio_set_level(LED_VERDE_GPIO, 0);
        } else {
            gpio_set_level(LED_VERDE_GPIO, 0);
            gpio_set_level(LED_VERMELHO_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(2000));// Mantém o LED vermelho aceso por 2 segundos
            gpio_set_level(LED_VERMELHO_GPIO, 0);
        }
    }
} // Tarefa para controlar os LEDs com base nos comandos recebidos pela fila
        
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
}// Inicializa o Wi-Fi, conecta à rede e aguarda até obter um IP

// ─── MQTT ─────────────────────────────────────────────────────────────────────

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) data;
    switch ((esp_mqtt_event_id_t) event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT conectado");
            xEventGroupSetBits(mqtt_event_group, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT desconectado");
            xEventGroupClearBits(mqtt_event_group, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT erro");
            break;
        default:
            break;
    }
}// Manipulador de eventos para o cliente MQTT

static void mqtt_init(void)
{
    mqtt_event_group = xEventGroupCreate();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    ESP_LOGI(TAG, "Aguardando conexão MQTT...");
    xEventGroupWaitBits(mqtt_event_group, MQTT_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "MQTT pronto");
}// Inicializa o cliente MQTT, registra o manipulador de eventos e aguarda conexão

static void mqtt_publish_access(const rc522_picc_uid_t *uid, const authorized_uid_t *auth)
{
    if (mqtt_client == NULL) return;
    if (!(xEventGroupGetBits(mqtt_event_group) & MQTT_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "MQTT não conectado, publicação ignorada");
        return;
    }

    // Monta UID em formato "AA:BB:CC:DD"
    char uid_str[32] = {0};
    for (int i = 0; i < uid->length; i++) {
        char byte_str[4];
        snprintf(byte_str, sizeof(byte_str), i == 0 ? "%02X" : ":%02X", uid->value[i]);
        strncat(uid_str, byte_str, sizeof(uid_str) - strlen(uid_str) - 1);
    }// Converte o UID para uma string legível

    char payload[128];
    if (auth != NULL) {
        snprintf(payload, sizeof(payload),
                 "{\"uid\":\"%s\",\"status\":\"PERMITIDO\",\"nome\":\"%s\"}",
                 uid_str, auth->name);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"uid\":\"%s\",\"status\":\"NEGADO\"}",
                 uid_str);
    }// Monta o payload JSON para publicar no MQTT

    //esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, payload, 0, 1, 0);
    //ESP_LOGI(TAG, "MQTT publicado: %s", payload);
    if (xSemaphoreTake(mqtt_mutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, payload, 0, 1, 0);
        ESP_LOGI(TAG, "MQTT publicado: %s", payload);
        xSemaphoreGive(mqtt_mutex);
    } else {
        ESP_LOGW(TAG, "Não foi possível publicar MQTT: mutex ocupado");
    }// Publica o evento de acesso no MQTT, protegendo o cliente com um mutex
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

        led_cmd_t cmd = (auth != NULL) ? LED_ON_PERMITIDO : LED_ON_NEGADO;
        xQueueSend(led_queue, &cmd, 0);

        mqtt_publish_access(&picc->uid, auth);
    }
}// Manipulador de eventos para mudanças no estado do PICC, verifica o UID, controla os LEDs e publica no MQTT

// ─── Main ─────────────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    leds_init();
    mqtt_mutex = xSemaphoreCreateMutex();
    assert(mqtt_mutex != NULL);// Cria mutex para proteger o cliente MQTT
    led_queue = xQueueCreate(5, sizeof(led_cmd_t));
    assert(led_queue != NULL);// Cria fila para 5 comandos de LED
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);

    wifi_init();
    mqtt_init();

    rc522_spi_create(&driver_config, &driver);
    rc522_driver_install(driver);

    rc522_config_t scanner_config = {
        .driver = driver,
    };// Configura o scanner RC522 com o driver SPI criado

    rc522_create(&scanner_config, &scanner);
    rc522_register_events(scanner, RC522_EVENT_PICC_STATE_CHANGED, on_picc_state_changed, NULL);
    rc522_start(scanner);
}
