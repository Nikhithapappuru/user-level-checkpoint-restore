#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_ROOT}"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "[demo] run this project on Linux"
  exit 1
fi

if [[ "${EUID}" -ne 0 ]]; then
  echo "[demo] run with sudo because ptrace-based checkpoint capture needs privileges"
  echo "[demo] example: sudo ./scripts/demo.sh"
  exit 1
fi

echo "[demo] building project"
make clean
make

echo "[demo] initializing runtime"
./bin/ulcr_manager init configs/demo.conf

echo "[demo] starting workload"
./bin/ulcr_manager start configs/demo.conf

echo "[demo] starting monitor in background"
nohup ./bin/ulcr_manager monitor configs/demo.conf > runtime/logs/monitor.stdout 2>&1 &
MONITOR_PID=$!
echo "${MONITOR_PID}" > runtime/monitor.pid

echo "[demo] waiting for first checkpoint"
sleep 20
./bin/ulcr_manager status configs/demo.conf

echo "[demo] simulating failure"
./bin/ulcr_manager fail configs/demo.conf

echo "[demo] waiting for auto-restore"
sleep 8
./bin/ulcr_manager status configs/demo.conf

echo "[demo] recent workload output"
tail -n 10 runtime/processes/counter-demo/process.log || true

echo "[demo] recent checkpoint manifest"
cat "runtime/checkpoints/counter-demo/active.meta" || true
ACTIVE_SLOT="$(awk -F= '/^active_slot=/{print $2}' runtime/checkpoints/counter-demo/active.meta 2>/dev/null || true)"
if [[ -n "${ACTIVE_SLOT}" ]]; then
  cat "runtime/checkpoints/counter-demo/slot_${ACTIVE_SLOT}/manifest.txt" || true
fi

echo "[demo] stop the monitor later with: kill \$(cat runtime/monitor.pid)"
