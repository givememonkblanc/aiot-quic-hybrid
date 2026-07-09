# Vendored third-party components

These are **external libraries vendored (copied) into the repo**, kept separate
from the project's own components in `src/components/`. They are registered as
ESP-IDF components via `COMPONENT_DIRS` in the top-level `CMakeLists.txt`.

They are vendored on purpose — for reproducibility of the QUIC-datagram behavior
the research measures, and because the QUIC/TLS stack is coupled and partly
customized. **Do not treat this as first-party code**; update from upstream
rather than editing in place (except the documented wolfSSL patch).

| Component | Upstream | Version | Notes |
|-----------|----------|---------|-------|
| `wolfssl` | https://github.com/wolfSSL/wolfssl | 5.8.2~1 | **Locally patched**: `include/user_settings.h` enables `WOLFSSL_QUIC` (required by ngtcp2). Not the pristine registry build — do **not** swap for the managed `wolfssl/wolfssl` component or QUIC support is lost. |
| `ngtcp2` | https://github.com/ngtcp2/ngtcp2 | vendored source + IDF CMake wrapper | Hand-wrapped as an ESP-IDF component (no `idf_component.yml`); not a registry component. Requires wolfSSL built with QUIC. |
| `core_mqtt` | AWS FreeRTOS coreMQTT | vendored source + IDF CMake wrapper | Hand-wrapped (no `idf_component.yml`); config in `src/main/core_mqtt_config.h`. |
| `esp-http3` | https://github.com/78/esp-http3 | 1.3.3 | QUIC HTTP/3 layer (single maintainer). `REQUIRES` ngtcp2 + wolfssl as sibling components. |
| `esp_camera` | https://github.com/espressif/esp32-camera | pristine, Kconfig-driven | The one pristine registry component here (OV2640 driver). Configured via `sdkconfig` (`CONFIG_OV2640_SUPPORT`, `CONFIG_CAMERA_*`). Could be migrated to a managed dependency (`espressif/esp32-camera`) later if desired. |

## Why not managed_components?

The project already uses `src/managed_components/` for registry deps
(esp-tflite-micro, esp-nn, esp_jpeg). The libraries here are **not** cleanly
managed:

- **wolfssl** carries a local `WOLFSSL_QUIC` patch — the registry version would
  disable QUIC and break ngtcp2.
- **ngtcp2 / core_mqtt** are hand-rolled IDF wrappers, not registry components.
- **esp-http3** depends on the two above being present as siblings.

Only `esp_camera` is a pristine, migratable registry component; it is kept here
for now so the whole external stack lives in one place.
