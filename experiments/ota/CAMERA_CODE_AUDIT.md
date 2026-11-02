# Camera clarity source audit — 2026-09-22

## Conclusion

The inspected preview path does not decode a small AI tensor and enlarge it for transmission. It copies the sensor JPEG, fragments those bytes, and writes reassembled bytes directly to disk. An independently reconstructed received frame matches the actual `latest.jpg` SHA-256. No blur-producing transformation was found in the active preview transport/display path.

This is not proof that the optics are faulty. Sensor register application, capture-time data integrity and physical lens condition remain unverified. In particular, receiver-side hashing does not compare against a hash measured at the sensor framebuffer.

## Running configuration and scope

OTA polls identify deployed ELF `0ccf545c4c5d36c9cc335bc6d21c63b2e9f6cf2eaa5035655b405e484b73b8e9` (`camera-uxga-q6-v2`). UXGA/JPEG quality 6, automatic exposure, legacy FOMO. The fixed-exposure diagnostic was previously restored and the build option is OFF. MoQ, ESPDet and batching experiment options are OFF. Audit follows `app_main → app_mobility_main`, not the dormant product-mode reassembly path. No firmware or service restart was performed for this audit.

## Paths inspected

| Stage | Source | Finding |
|---|---|---|
| Board pins and camera init | `src/components/sources/source_manager.c` | UXGA allocation, sensor JPEG, PSRAM framebuffers. PWDN incorrectly set to GPIO38 for S3-EYE; see fix below. |
| Sensor mode | `src/third_party/esp_camera/sensors/ov2640.c`, `sensors/private_include/ov2640_settings.h`, `driver/sensor.c` | UXGA selects full 1600×1200 window and UXGA sensor mode. Old 1024-square `set_res_raw` branch is excluded by UXGA flag. No application-level upscale. |
| Sensor configuration ordering | same files, `driver/esp_camera.c` | `esp_camera_init` sets resolution/format/quality, then app setters run, then app repeats UXGA setup. No explicit JPEG QS overwrite found in the repeated UXGA/JPEG tables. Exact applied hardware register values are not recorded. |
| DMA/frame ownership | `src/third_party/esp_camera/driver/cam_hal.c`, `source_manager.c` | Driver checks JPEG markers; source copies framebuffer before returning it. Profiling-only driver changes are inactive. Frame over 262144 bytes is rejected, not resized. Real-camera gate prevents fallback 1×1 test JPEG transmission in normal mobility path. |
| AI fork | `source_async.inc`, `ai_inference.c`, `ai_inference_tflm.cc` | AI works on separate copied JPEG buffers. UXGA decodes at 1/8 to 200×150 then maps to model input. Its reduced RGB/tensor is not the live stream JPEG. |
| App fragmentation | `src/main/app_mobility.inc`, `app_main.c` | Preview is passed directly to `app_stream_frame_at`; whole-frame length is uint32_t/size_t on this active path. Dormant product code has uint16_t lengths but is not called in this build. |
| Transport ownership | `src/components/transport/mqtt_quic_transport.c` | Each fragment is copied into owned heap storage before asynchronous TX; no retained pointer to the next overwritten source frame. No codec here. |
| Receiver | `host_receiver/picoquic/picohttp/demoserver.c` | Checks connection, frame timestamp, consecutive fragment sequence, capacity, SOI/EOI. Writes bytes with atomic rename; no image resampling. |
| Web | `backend/dashboard.py`, `backend/focus.html` | `/frame.jpg` and `/stream` serve original bytes. CSS fits image to preview area. `/focus` adds optional browser-only unsharp masking to a separate canvas; original file and AI input are not modified. |

## Definite configuration defect, corrected in source only

`CAMERA_PIN_PWDN` was 38, copied from a generic S3-WROOM mapping. S3-EYE's official mapping is -1. On the official v2.2 schematic, GPIO38 is microSD CMD, while CAM_PWDN is pulled low and CAM_RESET pulled high on the board.

Changed the S3-EYE default to -1, allowing other S3 boards to override it. This is a real board-configuration defect, but **not a demonstrated cause of smooth blur**: GPIO38 is not connected to this board's camera PWDN. Existing firmware still streams. Do not label this as an image-quality fix.

References:
- https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/Camera/CameraWebServer/camera_pins.h
- https://dl.espressif.com/dl/schematics/SCH_ESP32-S3-EYE-MB_20211201_V2.2.pdf (page 4, visually inspected)

## Validation

A bounded 16 MiB tail of the existing feed was processed without changing capture or network configuration:

- 89 complete JPEG frames, all 1600×1200.
- All 89 decoded successfully using Pillow with truncated-image acceptance disabled.
- Frame sizes 109078–110009 bytes; no sequence/key/capacity rejection in this sample.
- Snapshot SHA-256 `b5f8a359037c80098bd67fba17ea0e7dfc3b915e58c6bbe46823490242f6e6e1` matched independent reconstruction from received datagrams.
- This sample is not a global packet-loss or frame-loss estimate; fully missing frames are not counted.
- ESP-IDF v5.2.2 build passed with the PWDN source correction; fixed-exposure diagnostic OFF. Built image was not published.
- Existing C reassembly test extended with 100000-byte exact byte equality, buffer overflow rejection and next-frame recovery. Loss/connection/frame boundary cases and new cases pass under AddressSanitizer + UBSan.

Artifacts: `results/camera-code-audit/receiver-validation.json`, `reference.jpg`, `build.log`.

## Gaps and ancillary findings

1. Most application sensor setters ignore return codes. `set_window` also ignores the final `set_pixformat` result. Successful startup does not certify every sensor setting was applied. Add error accounting and register readback before attributing blur to hardware.
2. JPEG dimensions confirm output raster, not optical resolution. A camera-side hash paired with receiver hash is still needed to exclude corruption before receiver logging. Sensor mode/window readback is needed to verify live hardware configuration independently of source intent.
3. Legacy feed emits bytes >= 0x80 without JSON Unicode escaping. UTF-8 readers fail on image records; existing `ReceiverStatus` intentionally reads Latin-1. Audit used Latin-1 and parsed all sampled records. This does not modify `latest.jpg`, but external analyses must respect the legacy encoding.
4. Normal profile submits AI only after preview admission succeeds. This can reduce AI service rate when larger JPEGs are rejected; it does not explain optical blur.
5. Frontend `read_image()` checks SOI only, despite its 'complete JPEG' comment. The receiver supplies EOI gating, and every sampled frame decoded; this is a validation limitation, not evidence of current corruption.

Next discriminating measurement is sensor setting return codes + live register readback + source-frame checksum, then a same-scene comparison with stock sensor defaults. Lens/optics remain one candidate, not a conclusion. Generative reconstruction and extra sharpening cannot validate source image fidelity.
