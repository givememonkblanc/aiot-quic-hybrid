"""Add a read-only probe to the isolated experiment SDK, never the system SDK."""
from pathlib import Path
base=Path(__file__).resolve().parent
p=base.parent/'vlm_split/vendor/esp-idf-v5.3.6/components/freertos/FreeRTOS-Kernel/tasks.c'
s=p.read_text()
probe=(base/'runtime_counter.inc').read_text()
if 'AIOT_BATCH_RUNTIME_SNAPSHOT' not in s:
    p.write_text(s+probe)
elif not s.endswith(probe):
    raise RuntimeError('Existing isolated SDK runtime probe differs; review before replacing')
