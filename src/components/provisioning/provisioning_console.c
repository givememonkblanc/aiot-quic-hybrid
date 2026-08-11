#include "include/provisioning.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "prov_cli";

static const char *PROV_HELP =
    "\n== AIoT Presence Sensor console ==\n"
    "  show                 print current configuration\n"
    "  set <key> <value>    set a config field (see keys below)\n"
    "  save                 persist configuration to flash\n"
    "  erase                factory reset (wipe stored config)\n"
    "  reboot               restart the device\n"
    "  help                 this text\n"
    "keys: wifi.ssid wifi.pass server.host server.port device.id\n"
    "      presence.enter presence.exit presence.frames presence.timeout_ms\n"
    "      heartbeat.ms snapshot.on_enter snapshot.periodic_ms\n"
    "example: set wifi.ssid MyNetwork  /  set server.host 192.168.0.10  /  save  /  reboot\n";

static void prov_handle_line(app_cfg_t *cfg, char *line)
{
    /* tokenize: cmd [key] [value...] */
    char *cmd = strtok(line, " \t");
    if (cmd == NULL) {
        return;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        printf("%s", PROV_HELP);
    } else if (strcmp(cmd, "show") == 0) {
        app_cfg_print(cfg);
    } else if (strcmp(cmd, "save") == 0) {
        printf(app_cfg_save(cfg) == ESP_OK ? "[OK] saved\n" : "[ERR] save failed\n");
    } else if (strcmp(cmd, "erase") == 0) {
        printf(app_cfg_erase() == ESP_OK ? "[OK] erased; reboot to enter setup\n"
                                         : "[ERR] erase failed\n");
    } else if (strcmp(cmd, "reboot") == 0) {
        printf("[OK] rebooting...\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (strcmp(cmd, "set") == 0) {
        const char *key = strtok(NULL, " \t");
        const char *value = strtok(NULL, "");  /* rest of line (allow spaces) */
        if (key == NULL || value == NULL) {
            printf("[ERR] usage: set <key> <value>\n");
            return;
        }
        /* trim leading spaces from value */
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        esp_err_t err = app_cfg_set(cfg, key, value);
        if (err == ESP_OK) {
            printf("[OK] %s set (run 'save' to persist)\n", key);
        } else {
            printf("[ERR] invalid key or value: %s\n", key);
        }
    } else {
        printf("[ERR] unknown command '%s' (try 'help')\n", cmd);
    }
}

static void prov_console_task(void *arg)
{
    app_cfg_t *cfg = (app_cfg_t *)arg;
    char line[160];
    size_t pos = 0;

    printf("%s> ", PROV_HELP);
    fflush(stdout);

    for (;;) {
        int c = getchar();
        if (c == EOF || c < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                prov_handle_line(cfg, line);
                pos = 0;
                printf("> ");
                fflush(stdout);
            }
            continue;
        }
        if (pos < sizeof(line) - 1) {
            line[pos++] = (char)c;
        }
    }
}

void provisioning_console_start(app_cfg_t *cfg)
{
    if (xTaskCreate(prov_console_task, "prov_cli", 4096, cfg, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start console task");
    }
}
