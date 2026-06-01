idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver freertos rc522 nvs_flash esp_wifi esp_event esp_netif log esp-mqtt)