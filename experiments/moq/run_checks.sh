#!/usr/bin/env bash
set -euo pipefail
TASK_BASE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "$TASK_BASE/vendor/venv/bin/python" "$TASK_BASE/run_checks.py" "$@"
