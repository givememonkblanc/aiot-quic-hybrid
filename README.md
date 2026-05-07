# AIoT QUIC Hybrid Transport

A network-aware, edge-AI telemetry transport architecture for MCU-based IoT devices using QUIC and MQTT over QUIC. This project demonstrates how to decouple high-priority control/telemetry traffic from loss-tolerant edge AI payloads (like image/inference results) using a Hybrid Stream/Datagram approach over a single QUIC connection.

## 🚀 Features

- **MQTT over QUIC**: Uses QUIC Streams for reliable MQTT control traffic (connect, subscribe, critical publish).
- **Hybrid Transport Scheduler (`dl_qads`)**: Dynamically decides routing policies (`STREAM`, `DATAGRAM`, `DEFER`, `DROP`) based on real-time network metrics like RTT, queue depth, and loss.
- **Edge AI / TinyML Integration**: Performs on-device image capture and TensorFlow Lite Micro (TFLM) inferences, sending results asynchronously via unreliable but fast QUIC Datagrams to prevent head-of-line blocking.
- **Graceful Degradation**: Under severe stress (latency, packet loss, bandwidth limit), the system protects critical control streams while intentionally shedding low-priority AI/image datagrams.

## 📁 Repository Structure

```
├── firmware/                        # Pre-compiled partitions/SDK configs
├── host_receiver/                   # Host-side QUIC testing/logging server (picoquic)
├── experiments/                     # Evaluation scripts, logs, and telemetry analyzers
├── data/                            # Collected datasets & telemetry captures
├── src/
│   ├── main/                        # App orchestration, mode selection, QUIC-MQTT bindings
│   ├── components/
│   │   ├── ai_inference/            # TFLM and esp-nn wrapper logic
│   │   ├── common/                  # Packet and telemetry metric structures
│   │   ├── dl_qads/                 # The core transport policy/scheduler
│   │   ├── esp_camera/              # ESP32 camera driver
│   │   ├── esp-http3/               # HTTP/3 QUIC library (wolfssl/ngtcp2)
│   │   ├── sources/                 # Camera and sensor payload generator
│   │   ├── transport/               # Hybrid transport enqueuer & QUIC stream/datagram TX task
│   │   └── wifi_link/               # Wi-Fi connection manager
│   └── managed_components/          # ESP-IDF registry managed components
```

## 🧠 System Architecture

1. **Source Generation**: `source_manager` continuously captures camera frames or sensors.
2. **On-Device Inference**: `ai_inference_run()` parses JPEG chunks and returns TinyML metadata.
3. **Policy Engine (`dl_qads`)**: App evaluates the network state. Highly critical MQTT traffic is bound to QUIC Streams. AI/Image payload decisions fall back to QUIC Datagrams when the network is congested, prioritizing data freshness.
4. **Execution Layer**: `mqtt_quic_transport_send_hybrid` pushes payloads to a TX queue. The background network worker transmits them using `quic_client_write_safe` (Stream) or `quic_client_write_datagram_safe` (Datagram).

## 🛠️ Configuration & Setup

1. Configure your network and receiver host settings in `src/main/app_config.h`:
   ```c
   #define APP_WIFI_SSID "YOUR_SSID"
   #define APP_WIFI_PASSWORD "YOUR_PASSWORD"
   #define APP_PEER_HOST "YOUR_QUIC_SERVER_IP"
   ```

2. Select a transport preset in `app_config.h`:
   ```c
   // Choose the operational mode:
   #define APP_TRANSPORT_MODE APP_TRANSPORT_MODE_HYBRID
   // Other options: APP_TRANSPORT_MODE_STREAM_ONLY, APP_TRANSPORT_MODE_DATAGRAM_ONLY
   ```

3. **Build and Flash** via ESP-IDF v5.x:
   ```bash
   idf.py build
   idf.py flash monitor
   ```

## 📊 Evaluation & Experiments

The `experiments/` directory includes python analyzers and bash scripts to parse serial captures and evaluate:
- **Telemetry Freshness (Latency)**
- **Round-Trip Time (RTT)**
- **Queue Saturation & Drop Rates**
- **Throughput differences** between Stream-only, Datagram-only, and Hybrid configurations.

See `experiments/telemetry_analyzer/README.md` for instructions on parsing and plotting test logs.

## 📝 License

This project integrates several open-source libraries:
- **coreMQTT** (MIT)
- **picoquic** / **picotls** (MIT)
- **wolfSSL** (GPLv2)
- **ngtcp2** (MIT)
- **TensorFlow Lite Micro** (Apache 2.0)

Please refer to individual component licenses in the `src/components/` and `host_receiver/` directories for complete details.
