# AIoT Presence Sensor — Product Guide

This is the field guide for the presence-sensor product built on the
MQTT-over-QUIC hybrid transport. The research/experiment scaffolding that
produced the paper's measurements still lives in the tree but is off by
default — see [Build modes](#build-modes) and `docs/EXPERIMENTS.md`.

## What the product does

An ESP32-S3 + OV2640 device runs on-device TinyML inference on camera frames,
debounces the result into a `present` / `absent` state, and reports over a
single QUIC connection using the transport that each traffic class needs:

| Payload | Topic / track | QUIC service |
|---|---|---|
| State-change event (enter/exit) | `aiot/presence/event` | **stream** — reliable, ordered (MQTT PUBLISH) |
| Heartbeat (state, score, RSSI, heap) | telemetry track | **datagram** + delivery timeout |
| Snapshot JPEG on enter | image track | **datagram** chunks, loss-tolerant |
| Commands (e.g. OTA) | `aiot/<device_id>/cmd` | **stream** (MQTT SUBSCRIBE) |

MQTT control (CONNECT/SUBSCRIBE/PUBLISH) is always carried on a QUIC stream:
control on datagrams never yields a CONNACK. Only loss-tolerant telemetry and
image data use datagrams.

## Auto-registration & discovery

On every (re)connect the device announces itself the way MQTT appliances do,
so a backend can pick it up with zero manual entry:

- **Availability (Last Will):** `aiot/<device_id>/status` = `online` on connect,
  and `offline` published by the broker via the MQTT Last Will if the device
  drops. (retained)
- **Registration:** a retained self-description on `aiot/<device_id>/register`
  — device id, type, model, firmware version, heartbeat period, and the full
  topic map — so a backend can auto-register the device.
- **State:** retained `present`/`absent` on `aiot/<device_id>/state`, updated on
  every transition, so a late-joining subscriber sees the current state.
- **Home Assistant MQTT Discovery** (default on, `APP_HA_DISCOVERY_ENABLE`): a
  retained config on `homeassistant/binary_sensor/<device_id>/config`
  (`device_class: occupancy`) so Home Assistant / any compatible platform
  auto-creates the sensor entity with availability wired up.

These require a broker that honours retain and Last Will (e.g. EMQX, Mosquitto,
HiveMQ over MQTT-over-QUIC); the bundled picoquic test server logs the traffic
but does not implement retain/LWT semantics.

## Build modes

Selected by `APP_PRODUCT_MODE` in `src/main/app_config.h`:

- `APP_PRODUCT_MODE 1` (default) — the product: NVS-backed config, field
  provisioning, presence state machine, session supervisor, OTA.
- `APP_PRODUCT_MODE 0` — the bench/experiment build that keeps the
  transport-mode matrix and scenario timeline on the hot path for the paper.

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

## Provisioning (no secrets in the image)

A factory image ships with `REPLACE_ME` placeholders, so it boots straight into
setup. Configuration lives in NVS (namespace `aiotcfg`) and is set two ways:

### 1. SoftAP setup portal (installer, no tools)
When the device is unprovisioned — or the setup button (`APP_SETUP_BUTTON_GPIO`,
default GPIO0/BOOT) is held at boot — it starts an open AP
`AIoT-Presence-XXXX`. Connect a phone, open <http://192.168.4.1/>, fill in
Wi-Fi + server, and submit; the device saves and reboots.

### 2. Serial console (over USB)
A line shell is always running on the USB serial port:

```
show                       print current config (password masked)
set wifi.ssid MyNetwork
set wifi.pass secret
set server.host 192.168.0.10
set server.port 14567
set presence.enter 140     detection thresholds / timing
save                       persist to flash
erase                      factory reset
reboot
```

Factory reset = `erase` on the console, or hold the setup button at boot to
re-enter the portal.

## OTA firmware update

OTA needs an OTA-capable partition table. The default `partitions.csv` is a
single `factory` app (no OTA); a ready 2-slot 4MB layout is in
`partitions_ota.csv` (two 1.9375MB slots; the product image is ~1.84MB, so
~100KB / 5% headroom per slot — verified building with `sdkconfig.prod`).

Enable it (one-time, wipes NVS so the device re-provisions):
```bash
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.prod" reconfigure
idf.py erase-flash flash
```
`sdkconfig.prod` also turns on rollback (`BOOTLOADER_APP_ROLLBACK_ENABLE`) and
the mbedTLS CA bundle for HTTPS. On the first boot of a new image
`ota_confirm_boot()` marks it valid; a boot that never reaches `app_main` rolls
back automatically.

Trigger an update by publishing to the device's command topic:
```
topic:   aiot/<device_id>/cmd
payload: ota https://updates.example.com/presence/firmware.bin
```
The device downloads with `esp_https_ota` and reboots into the new slot. (This
requires a broker/server that delivers PUBLISH to the subscribed cmd topic.)

## Security

Production builds should enable, in order of importance:

1. **QUIC server authentication.** By default the client does **not** verify the
   server (`WOLFSSL_VERIFY_NONE`) and warns on every connect. Drop your server's
   issuing CA at `src/components/transport/server_ca.pem` (see the `.example`);
   the build embeds it and switches to `WOLFSSL_VERIFY_PEER` automatically.
2. **TLS for OTA** — `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` (in `sdkconfig.prod`).
3. **Secure Boot V2 + Flash Encryption** — templates in `sdkconfig.prod`, left
   commented because they burn one-way eFuses. Validate on a sacrificial board
   first; see the Espressif security docs.
4. **NVS encryption** for the stored Wi-Fi password (`CONFIG_NVS_ENCRYPTION`,
   needs flash encryption + an `nvs_keys` partition).

Never commit real keys/certs — `.gitignore` already excludes `server_ca.pem`,
`secure_boot_signing_key.pem`, and `nvs_keys.bin`.

## Resilience

- Wi-Fi drops: the session supervisor detects the dead QUIC connection, nudges
  `esp_wifi_connect()`, and re-establishes QUIC+MQTT with capped exponential
  backoff (`APP_SESSION_RETRY_MIN_MS` → `APP_SESSION_RETRY_MAX_MS`).
- The main task is watchdog-fed throughout bring-up and the loop.
