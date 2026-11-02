#!/usr/bin/env python3
"""Freeze selected source/config identities alongside each immutable device image."""

import hashlib, json, sys, tarfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1])
files = [
    "src/main/app_moq_concurrent.inc",
    "src/main/moq_representation.c",
    "src/main/moq_representation.h",
    "src/main/moq_codec_worker.c",
    "src/main/moq_codec_worker.h",
    "src/main/CMakeLists.txt",
    "src/main/moq_options.cmake",
    "src/components/transport/moqt_ngtcp2.inc",
    "src/components/transport/ngtcp2_sample.h",
    "src/components/transport/moqt_swma.h",
    "src/components/transport/moqt_feasibility.h",
    "src/components/transport/CMakeLists.txt",
    "src/components/transport/moq_options.cmake",
    "src/components/sources/source_async.inc",
]


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


config = {}
for line in (root / "build_moq_screw_cap24/CMakeCache.txt").read_text().splitlines():
    if (line.startswith("AIOT_MOQT_") and "PIN_HEADER" not in line) or line.startswith(
        ("PROJECT_VER:", "AIOT_REPLAY_JPEG:", "AIOT_EMBED_REPLAY_JPEG:")
    ):
        key, value = line.split("=", 1)
        config[key] = value
rtos = {}
for line in (root / "build_moq_screw_cap24/config/sdkconfig.h").read_text().splitlines():
    if line.startswith("#define CONFIG_"):
        parts = line.split(maxsplit=2)
        if parts[1] in (
            "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ",
            "CONFIG_FREERTOS_HZ",
            "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS",
            "CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER",
            "CONFIG_ESP_MAIN_TASK_AFFINITY",
        ):
            rtos[parts[1]] = parts[2]
with tarfile.open(out / "changed-sources.tar.gz", "w:gz") as archive:
    for f in files:
        archive.add(root / f, arcname=f)
(out / "provenance.json").write_text(
    json.dumps(
        dict(
            scope="selected sources/config, not complete dependency bundle",
            firmware_sha256=sha(out / "firmware.bin"),
            configuration=config,
            rtos=rtos,
            source_hashes={f: sha(root / f) for f in files},
        ),
        indent=2,
    )
    + "\n"
)
