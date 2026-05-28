#include <stdio.h>
#include <string.h>
#include "rc522.h"
#include "driver/rc522_spi.h"
#include "rc522_picc.h"

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

// Lista de UIDs autorizados — adicione ou remova entradas conforme necessário
static const authorized_uid_t authorized_uids[] = {
    { .uid = {0xF3, 0x54, 0xB3, 0x29}, .length = 4, .name = "Cartão 1" },
    { .uid = {0x01, 0x02, 0x03, 0x04}, .length = 4, .name = "Cartão 2" },
};

#define AUTHORIZED_COUNT (sizeof(authorized_uids) / sizeof(authorized_uids[0]))

static bool is_authorized(const rc522_picc_uid_t *uid)
{
    for (int i = 0; i < AUTHORIZED_COUNT; i++) {
        if (uid->length == authorized_uids[i].length &&
            memcmp(uid->value, authorized_uids[i].uid, uid->length) == 0) {
            return true;
        }
    }
    return false;
}

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

        if (is_authorized(&picc->uid)) {
            // Encontra o nome do cartão autorizado
            for (int i = 0; i < AUTHORIZED_COUNT; i++) {
                if (picc->uid.length == authorized_uids[i].length &&
                    memcmp(picc->uid.value, authorized_uids[i].uid, picc->uid.length) == 0) {
                    printf("ACESSO PERMITIDO — %s\n", authorized_uids[i].name);
                    break;
                }
            }
        } else {
            printf("ACESSO NEGADO\n");
        }
    }
}

void app_main(void)
{
    rc522_spi_create(&driver_config, &driver);
    rc522_driver_install(driver);

    rc522_config_t scanner_config = {
        .driver = driver,
    };

    rc522_create(&scanner_config, &scanner);
    rc522_register_events(scanner, RC522_EVENT_PICC_STATE_CHANGED, on_picc_state_changed, NULL);
    rc522_start(scanner);
}
