#pragma once

#include <stdbool.h>

#include "app_cfg.h"

/*
 * Field provisioning for the presence sensor. Two independent entry points,
 * both operating on the shared app_cfg_t and persisting via app_cfg_save():
 *
 *  - Serial console CLI: a line-oriented shell on stdin (UART/USB-CDC) with
 *    show / set / save / erase / reboot / help commands. Always available so
 *    an installer can reconfigure a deployed device over USB.
 *
 *  - SoftAP setup portal: when the device is unprovisioned (or forced), it
 *    starts a Wi-Fi AP "AIoT-Presence-XXXX" and serves a captive setup form
 *    at http://192.168.4.1/ . Submitting the form saves config and reboots.
 */

/* Start the background serial-console task. Non-blocking. */
void provisioning_console_start(app_cfg_t *cfg);

/*
 * Run the SoftAP setup portal until the user submits a valid configuration.
 * Blocks the calling task. Returns true when new config was saved (caller
 * should reboot), false on timeout_ms elapsed with no submission
 * (0 = wait forever).
 */
bool provisioning_portal_run(app_cfg_t *cfg, unsigned timeout_ms);
