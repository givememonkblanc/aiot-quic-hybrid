# Capture 10Hz and pipeline profiling

Measured findings and limitations: [RESULTS.md](RESULTS.md).

`trial.py` profiles the deployed FOMO firmware through runtime CLI settings.
`build_pipeline.sh` builds a separate, instrumented ESPDet firmware for camera,
fragmentation, QUIC crypto/packetization, socket and partial Wi-Fi completion scopes.
`flash_pipeline.sh` temporarily flashes that build, runs `pipeline_trial.py`, then
restores the verified full-flash backup via an EXIT trap. These scripts actively
change device state; do not run concurrently with another serial client. No NVS
save is performed. A hard interruption still requires manual full-flash restoration.

Offline analysis:

```sh
python experiments/capture10/analyze.py experiments/capture10/results/live-20260921T004340Z
python experiments/capture10/analyze.py experiments/capture10/results/pipeline-20260921T005217Z
python experiments/capture10/analyze_pipeline.py experiments/capture10/results/pipeline-20260921T005217Z
```

`analyze.py` requires Pillow to validate complete received JPEGs. Its capture field
is a replay-generation counter in the fixed-trace stages, not a sensor FPS measure.
`analyze_pipeline.py` reports per-frame sensor timing only in live-camera stages.

Probe host checks (crypto scope accounting and bounded callback matching):

```sh
mkdir -p /tmp/aiot-pipe-test
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Iexperiments/capture10/test_stubs experiments/capture10/test_pipeline.c \
  -o /tmp/aiot-pipe-test/test
/tmp/aiot-pipe-test/test
```

The profiling flag defaults off. Probe overhead and sparse Wi-Fi callback matching
are explicit limitations; do not interpret unmatched callbacks as loss or combine
nested CPU/wall measurements by simple addition. Raw logs, images, backups and
generated results remain ignored; the checked-in report contains aggregate data.
