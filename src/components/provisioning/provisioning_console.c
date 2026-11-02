#include "video_admission.h"
#include "include/provisioning.h"
#include "wifi_link.h"
#include "mobility_scheduler.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"

#if AIOT_QUIC_BATCH_EXPERIMENT
extern bool quic_batch_set(unsigned kib);
#endif

static const char *TAG = "prov_cli";

static const char *PROV_HELP =
    "\n== AIoT Presence Sensor console ==\n"
    "  show                 print current configuration\n"
    "  mode 0|1|2           research: reconnect / migration / adaptive\n"
    "  video <fps> <KiB>    research input target 1..30, queue budget 64..6144\n"
    "  handoff              switch to configured Wi-Fi without reboot\n"
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
    } else if (strcmp(cmd, "mode") == 0) {
        const char *mode = strtok(NULL, " \t");
        if (mode && mode[1] == '\0' && mode[0] >= '0' && mode[0] <= '2') {
            mobility_mode_set(mode[0] - '0');
            printf("[OK] mobility.mode=%d\n", mobility_mode_get());
        } else printf("[ERR] mode must be 0, 1 or 2\n");
    } else if (strcmp(cmd, "policy") == 0) {
        unsigned policy; char extra;
        const char *args = strtok(NULL, "");
        if (args && sscanf(args, "%u %c", &policy, &extra) == 1 && video_policy_set(policy))
            printf("[OK] video.policy=%u\n", policy);
        else printf("[ERR] policy 0=current 1=amr_latest 2=amr_rssi 3=amr_quality\n");
    } else if (strcmp(cmd, "profile") == 0) {
        unsigned p; char extra;
        const char *args = strtok(NULL, "");
        if (args && sscanf(args, "%u %c", &p, &extra) == 1 && mobility_profile_set(p))
            printf("[OK] profile=%u\n", p);
        else printf("[ERR] profile 0=legacy 1=A 2=B 3=C 4=D 5=E 6=queue_drop 7=preview+evidence 8=ungated_preview+evidence\n");
#if AIOT_QUIC_BATCH_EXPERIMENT
    } else if (strcmp(cmd, "batch") == 0) {
        unsigned kib; char extra; const char *args = strtok(NULL, "");
        bool ok = args && sscanf(args, "%u %c", &kib, &extra) == 1 && quic_batch_set(kib);
        printf("[%s] batch_kib=%u (0=frame)\n", ok ? "OK" : "ERR", ok ? kib : 0);
#endif
    } else if (strcmp(cmd, "probe") == 0) {
        unsigned hz; char extra; const char *args = strtok(NULL, "");
        if (args && sscanf(args, "%u %c", &hz, &extra) == 1 && mobility_probe_set(hz)) printf("[OK] probe_hz=%u\n", hz);
        else printf("[ERR] probe 0=normal or 1..10 image attempts/s\n");
    } else if (strcmp(cmd, "aiprio") == 0 || strcmp(cmd, "replay") == 0) {
        unsigned value; char extra; const char *args = strtok(NULL, "");
        bool ok = args && sscanf(args, "%u %c", &value, &extra) == 1;
        if (ok) ok = !strcmp(cmd,"aiprio") ? mobility_ai_priority_set(value) : mobility_replay_set(value);
        printf("[%s] %s=%u\n",ok ? "OK" : "ERR",cmd,ok ? value : 0);
    } else if (strcmp(cmd, "video") == 0) {
        unsigned fps, kib; char extra;
        const char *args = strtok(NULL, "");
        if (args && sscanf(args, "%u %u %c", &fps, &kib, &extra) == 2 && mobility_video_config(fps, kib))
            printf("[OK] video target_fps=%u budget_kib=%u\n", fps, kib);
        else printf("[ERR] video <1..30 fps> <64..6144 KiB>\n");
    } else if (strcmp(cmd, "handoff") == 0) {
        esp_err_t err = wifi_link_handoff(cfg->wifi_ssid, cfg->wifi_password);
        printf("[%s] handoff %s\n", err == ESP_OK ? "OK" : "ERR", esp_err_to_name(err));
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
