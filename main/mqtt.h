#pragma once
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"

#define WIFI_SSID "TP-Link_A232"
#define WIFI_PASS "62707558"

#define MQTT_URI    "mqtt://broker.hivemq.com:1883"
#define MQTT_USER   ""
#define MQTT_PASS   ""

#define MQTT_TOPIC  "RFID"
#define LED_VERMELHO_GPIO   22
#define LED_VERDE_GPIO      23
#define BT_VERMELHO_GPIO    19
#define BT_VERDE_GPIO       21

extern esp_mqtt_client_handle_t mqtt_client;

void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
void escreverMqtt(const char *Mensagem);
