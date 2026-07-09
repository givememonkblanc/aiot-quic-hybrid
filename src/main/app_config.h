#pragma once

/*
 * Compile-time defaults. In the product build these are only fallbacks: the
 * device is provisioned in the field over the serial console or the SoftAP
 * setup portal, and the live values live in NVS (see components/app_cfg).
 * "REPLACE_ME" counts as unset, so a factory image ships with no secrets and
 * boots straight into the setup portal. For a bench/experiment build you may
 * hard-code real values here (do NOT commit real credentials).
 */
#define APP_WIFI_SSID "REPLACE_ME"
#define APP_WIFI_PASSWORD "REPLACE_ME"
#define APP_PEER_HOST "REPLACE_ME"
#define APP_MQTT_PORT 14567
#define APP_WIFI_TIMEOUT_MS 15000
#define APP_DEVICE_ID "esp32s3"

/*
 * Zero-config server endpoint. The customer never types a server address:
 * the product ships pointing at a fixed cloud/broker baked in here at build
 * time, so field setup only asks for Wi-Fi. Set this to your endpoint for a
 * product image (hostname or IP). APP_PEER_HOST above still overrides it for a
 * bench build. "REPLACE_ME" means "not built for a fixed server yet".
 */
#ifndef APP_DEFAULT_SERVER_HOST
#define APP_DEFAULT_SERVER_HOST "REPLACE_ME"
#endif
#ifndef APP_DEFAULT_SERVER_PORT
#define APP_DEFAULT_SERVER_PORT APP_MQTT_PORT
#endif

/*
 * Product master switch. When enabled (default) the firmware boots as the
 * presence-sensor product: NVS-backed config, field provisioning, presence
 * state machine, session supervisor, and OTA. Set to 0 for the bench build
 * that keeps the transport-mode / scenario experiment scaffolding on the
 * hot path (used for the paper's measurements).
 */
#ifndef APP_PRODUCT_MODE
#define APP_PRODUCT_MODE 1
#endif

/* Presence eventing topics / tracks. */
#define APP_TOPIC_PRESENCE_EVENT "aiot/presence/event"
#define APP_TOPIC_PRESENCE_HB    "aiot/presence/hb"
#define APP_TOPIC_CMD_SUFFIX     "/cmd"   /* subscribe aiot/<device_id>/cmd */

/*
 * Auto-registration & discovery. On connect the device announces itself the
 * way MQTT appliances do: a retained self-description on a per-device register
 * topic, an online/offline availability topic backed by an MQTT Last Will, a
 * retained per-device state topic, and (optionally) a Home Assistant MQTT
 * Discovery config so any compatible backend auto-adds the sensor.
 * Per-device topics: aiot/<device_id>/{status,state,register,cmd}
 */
#define APP_TOPIC_STATUS_SUFFIX   "/status"    /* online / offline (LWT) */
#define APP_TOPIC_STATE_SUFFIX    "/state"     /* present / absent (retained) */
#define APP_TOPIC_REGISTER_SUFFIX "/register"  /* retained JSON self-description */
#define APP_STATUS_ONLINE  "online"
#define APP_STATUS_OFFLINE "offline"

#define APP_DEVICE_TYPE  "presence_sensor"
#define APP_DEVICE_MODEL "esp32s3-ov2640"
#define APP_DEVICE_MANUFACTURER "solfac"

#ifndef APP_HA_DISCOVERY_ENABLE
#define APP_HA_DISCOVERY_ENABLE 1
#endif
#ifndef APP_HA_DISCOVERY_PREFIX
#define APP_HA_DISCOVERY_PREFIX "homeassistant"
#endif

/* Snapshot JPEG chunking for the "snapshot on enter" feature. */
#ifndef APP_SNAPSHOT_CHUNK_SIZE
#define APP_SNAPSHOT_CHUNK_SIZE 1000
#endif

/*
 * Live camera streaming: send each captured JPEG frame on the IMAGE track
 * (chunked QUIC datagrams) so the backend can serve an MJPEG live view.
 * Rate-limited to one frame per APP_STREAM_INTERVAL_MS to bound bandwidth.
 */
#ifndef APP_STREAM_ENABLE
#define APP_STREAM_ENABLE 1
#endif
#ifndef APP_STREAM_INTERVAL_MS
#define APP_STREAM_INTERVAL_MS 350   /* ~3 fps */
#endif

/* Presence heartbeat cadence default (overridable via NVS heartbeat.ms). */
#ifndef APP_PRESENCE_HEARTBEAT_MS
#define APP_PRESENCE_HEARTBEAT_MS 5000
#endif

/*
 * Session supervisor: after the QUIC+MQTT session drops, wait this long
 * before the first re-establish attempt, doubling up to the cap.
 */
#ifndef APP_SESSION_RETRY_MIN_MS
#define APP_SESSION_RETRY_MIN_MS 1000
#endif
#ifndef APP_SESSION_RETRY_MAX_MS
#define APP_SESSION_RETRY_MAX_MS 30000
#endif

/*
 * Optional GPIO held LOW at boot to force the setup portal even when the
 * device is already provisioned (installer factory-reset button). -1 = off.
 * On most ESP32-S3 devkits GPIO0 is the BOOT button.
 */
#ifndef APP_SETUP_BUTTON_GPIO
#define APP_SETUP_BUTTON_GPIO 0
#endif

/*
 * Onboard status RGB LED (WS2812) so the screenless device shows what it is
 * doing: boot / setup portal / connecting / online / occupied / error. -1
 * disables it. GPIO48 is the WS2812 on the Freenove ESP32-S3 CAM board; set
 * to your board's addressable-LED pin.
 */
#ifndef APP_STATUS_LED_GPIO
#define APP_STATUS_LED_GPIO 48
#endif

#define APP_AI_STREAM_ID 0
#define APP_SENSOR_STREAM_ID 4
#define APP_IMAGE_STREAM_ID 8

#define APP_EXPERIMENT_PRESET_HYBRID_BASELINE 0
#define APP_EXPERIMENT_PRESET_RELIABILITY_BIASED 1
#define APP_EXPERIMENT_PRESET_THROUGHPUT_BIASED 2
#define APP_EXPERIMENT_PRESET_STREAM_FIRST_HYBRID 3

#define APP_TRANSPORT_MODE_STREAM_ONLY 1
#define APP_TRANSPORT_MODE_DATAGRAM_ONLY 2
#define APP_TRANSPORT_MODE_HYBRID 3

#ifndef APP_TRANSPORT_MODE
#define APP_TRANSPORT_MODE APP_TRANSPORT_MODE_HYBRID
#endif

#ifndef APP_EXPERIMENT_PRESET
#define APP_EXPERIMENT_PRESET APP_EXPERIMENT_PRESET_STREAM_FIRST_HYBRID
#endif

#if APP_EXPERIMENT_PRESET == APP_EXPERIMENT_PRESET_RELIABILITY_BIASED
#define APP_CONTROL_PERIOD_MS 60
#define APP_IMAGE_DATAGRAM_BUDGET 900
#define APP_QADS_POLICY_MODE 1
#elif APP_EXPERIMENT_PRESET == APP_EXPERIMENT_PRESET_THROUGHPUT_BIASED
#define APP_CONTROL_PERIOD_MS 40
#define APP_IMAGE_DATAGRAM_BUDGET 1300
#define APP_QADS_POLICY_MODE 2
#elif APP_EXPERIMENT_PRESET == APP_EXPERIMENT_PRESET_STREAM_FIRST_HYBRID
#define APP_CONTROL_PERIOD_MS 50
#define APP_IMAGE_DATAGRAM_BUDGET 1200
#define APP_QADS_POLICY_MODE 3
#else
#define APP_CONTROL_PERIOD_MS 50
#define APP_IMAGE_DATAGRAM_BUDGET 1200
#define APP_QADS_POLICY_MODE 0
#endif

#define APP_MAX_PAYLOAD_SIZE 1024

/*
 * Delivery timeout per deadline_class (MOQT delivery-timeout analog).
 * A queued unreliable message older than its class timeout is dropped
 * instead of transmitted. 0 = no expiry.
 */
#ifndef APP_DEADLINE_CLASS0_MS
#define APP_DEADLINE_CLASS0_MS 0
#endif

#ifndef APP_DEADLINE_CLASS1_MS
#define APP_DEADLINE_CLASS1_MS 500
#endif

#ifndef APP_DEADLINE_IMAGE_MS
#define APP_DEADLINE_IMAGE_MS 200
#endif

#ifndef APP_NET_TX_STACK_SAMPLE_INTERVAL
#define APP_NET_TX_STACK_SAMPLE_INTERVAL 10
#endif

#ifndef APP_STRESS_MODE_ENABLE
#define APP_STRESS_MODE_ENABLE 0
#endif

#ifndef APP_STRESS_DATAGRAM_BURST_COUNT
#define APP_STRESS_DATAGRAM_BURST_COUNT 12
#endif

#ifndef APP_STRESS_DATAGRAM_PAYLOAD_SIZE
#define APP_STRESS_DATAGRAM_PAYLOAD_SIZE 512
#endif

#ifndef APP_STRESS_INTER_BURST_DELAY_MS
#define APP_STRESS_INTER_BURST_DELAY_MS 0
#endif

/*
 * Scenario mode: phased load timeline within one capture so a single rep
 * exercises congestion entry and recovery (normal -> image burst -> normal).
 * During the burst phase real camera JPEG frames are transmitted in
 * APP_IMAGE_CHUNK_SIZE chunks on the IMAGE track every
 * APP_IMAGE_TX_PERIOD_MS (datagrams with APP_DEADLINE_IMAGE_MS expiry;
 * chunked MQTT publishes in stream-only mode).
 */
#ifndef APP_SCENARIO_MODE_ENABLE
#define APP_SCENARIO_MODE_ENABLE 0
#endif

#ifndef APP_SCENARIO_PHASE_SECS
#define APP_SCENARIO_PHASE_SECS 30
#endif

#ifndef APP_IMAGE_TX_PERIOD_MS
#define APP_IMAGE_TX_PERIOD_MS 500
#endif

#ifndef APP_IMAGE_CHUNK_SIZE
#define APP_IMAGE_CHUNK_SIZE 1000
#endif
