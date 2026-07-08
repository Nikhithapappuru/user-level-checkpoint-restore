#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}" || exit 1

OUT_DIR="runtime/benchmark"
RAW_CSV="${OUT_DIR}/cycles.csv"
MON_CSV="${OUT_DIR}/monitor_samples.csv"
SUMMARY="${OUT_DIR}/summary.txt"
CONFIG="configs/demo.conf"
MANAGER="./bin/ulcr_manager"

log_summary() {
  printf "%s\n" "$*" | tee -a "${SUMMARY}"
}

now_ns() {
  date +%s%N
}

elapsed_ms() {
  local start_ns="$1"
  local end_ns="$2"
  awk -v s="${start_ns}" -v e="${end_ns}" 'BEGIN { printf "%.3f", (e - s) / 1000000.0 }'
}

read_pid() {
  tr -d '\r\n' < runtime/processes/counter-demo/pid
}

wait_dead() {
  local pid="$1"
  local i
  for i in $(seq 1 50); do
    if [[ ! -e "/proc/${pid}" ]]; then
      return 0
    fi
    sleep 0.02
  done
  return 1
}

wait_alive() {
  local pid="$1"
  local i
  for i in $(seq 1 50); do
    if [[ -r "/proc/${pid}/status" ]]; then
      return 0
    fi
    sleep 0.02
  done
  return 1
}

rss_kb() {
  local pid="$1"
  awk '/^VmRSS:/ { print $2; found=1 } END { if (!found) print 0 }' "/proc/${pid}/status" 2>/dev/null
}

active_snapshot_size() {
  local slot
  slot="$(awk -F= '/^active_slot=/{print $2}' runtime/checkpoints/counter-demo/active.meta 2>/dev/null | tr -d '\r')"
  if [[ -n "${slot}" && -f "runtime/checkpoints/counter-demo/slot_${slot}/snapshot.bin" ]]; then
    stat -c %s "runtime/checkpoints/counter-demo/slot_${slot}/snapshot.bin"
  else
    echo 0
  fi
}

manifest_value() {
  local key="$1"
  awk -F= -v k="${key}" '$1 == k { print $2; exit }' runtime/checkpoints/counter-demo/slot_*/manifest.txt 2>/dev/null | head -n 1
}

safe_stop_runtime() {
  if [[ -f runtime/monitor.pid ]]; then
    local mpid
    mpid="$(tr -d '\r\n' < runtime/monitor.pid 2>/dev/null || true)"
    if [[ -n "${mpid}" ]]; then
      kill "${mpid}" 2>/dev/null || true
    fi
  fi
  if [[ -f runtime/processes/counter-demo/pid ]]; then
    "${MANAGER}" stop "${CONFIG}" >/dev/null 2>&1 || true
    local pid
    pid="$(read_pid 2>/dev/null || true)"
    if [[ -n "${pid}" ]]; then
      kill -9 "${pid}" 2>/dev/null || true
    fi
  fi
}

sample_monitor() {
  local monitor_pid="$1"
  local seconds="$2"
  local hz
  local ncpu
  local start_ns
  local end_ns
  local prev_proc
  local prev_total
  local i

  hz="$(getconf CLK_TCK)"
  ncpu="$(getconf _NPROCESSORS_ONLN)"
  prev_proc="$(awk '{ print $14 + $15 }' "/proc/${monitor_pid}/stat")"
  prev_total="$(awk '/^cpu / { for (i=2; i<=NF; i++) total += $i; print total }' /proc/stat)"
  start_ns="$(now_ns)"
  printf "sample,elapsed_ms,cpu_percent,rss_kb\n" > "${MON_CSV}"

  for i in $(seq 1 "${seconds}"); do
    sleep 1
    local proc_now
    local total_now
    local rss_now
    local elapsed
    proc_now="$(awk '{ print $14 + $15 }' "/proc/${monitor_pid}/stat" 2>/dev/null || echo 0)"
    total_now="$(awk '/^cpu / { for (j=2; j<=NF; j++) total += $j; print total }' /proc/stat)"
    rss_now="$(rss_kb "${monitor_pid}")"
    end_ns="$(now_ns)"
    elapsed="$(elapsed_ms "${start_ns}" "${end_ns}")"
    awk -v sample="${i}" -v elapsed="${elapsed}" -v pn="${proc_now}" -v pp="${prev_proc}" \
      -v tn="${total_now}" -v tp="${prev_total}" -v ncpu="${ncpu}" -v rss="${rss_now}" \
      'BEGIN {
        total_delta = tn - tp;
        proc_delta = pn - pp;
        cpu = total_delta > 0 ? (proc_delta / total_delta) * ncpu * 100.0 : 0.0;
        printf "%d,%.3f,%.6f,%d\n", sample, elapsed, cpu, rss;
      }' >> "${MON_CSV}"
    prev_proc="${proc_now}"
    prev_total="${total_now}"
  done
}

safe_stop_runtime
make clean >/dev/null
make >/tmp/ulcr_benchmark_make.log 2>&1 || {
  cat /tmp/ulcr_benchmark_make.log
  exit 1
}

mkdir -p "${OUT_DIR}"
: > "${RAW_CSV}"
: > "${MON_CSV}"
: > "${SUMMARY}"

"${MANAGER}" init "${CONFIG}" >/dev/null || exit 1
"${MANAGER}" start "${CONFIG}" >/dev/null || exit 1
sleep 1

printf "cycle,checkpoint_ms,restore_ms,checkpoint_ok,fail_ok,restore_ok,success,old_pid,new_pid,new_vm_rss_kb,snapshot_size_bytes\n" > "${RAW_CSV}"

success_count=0
max_consecutive=0
current_consecutive=0
max_rss=0

for cycle in $(seq 1 100); do
  old_pid="$(read_pid)"

  cp_start="$(now_ns)"
  "${MANAGER}" checkpoint "${CONFIG}" >/dev/null 2>&1
  checkpoint_rc=$?
  cp_end="$(now_ns)"
  checkpoint_ms="$(elapsed_ms "${cp_start}" "${cp_end}")"
  snapshot_bytes="$(active_snapshot_size)"

  fail_rc=1
  restore_rc=1
  restore_ms="0.000"
  new_pid=0
  vmrss=0

  if [[ "${checkpoint_rc}" -eq 0 ]]; then
    "${MANAGER}" fail "${CONFIG}" >/dev/null 2>&1
    fail_rc=$?
    wait_dead "${old_pid}" || true

    rt_start="$(now_ns)"
    "${MANAGER}" restore "${CONFIG}" >/dev/null 2>&1
    restore_rc=$?
    rt_end="$(now_ns)"
    restore_ms="$(elapsed_ms "${rt_start}" "${rt_end}")"

    if [[ "${restore_rc}" -eq 0 ]]; then
      new_pid="$(read_pid)"
      wait_alive "${new_pid}" || true
      sleep 0.05
      vmrss="$(rss_kb "${new_pid}")"
    fi
  fi

  if [[ "${checkpoint_rc}" -eq 0 && "${fail_rc}" -eq 0 && "${restore_rc}" -eq 0 && "${new_pid}" -gt 0 && "${vmrss}" -gt 0 ]]; then
    success=1
    success_count=$((success_count + 1))
    current_consecutive=$((current_consecutive + 1))
    if [[ "${current_consecutive}" -gt "${max_consecutive}" ]]; then
      max_consecutive="${current_consecutive}"
    fi
    if [[ "${vmrss}" -gt "${max_rss}" ]]; then
      max_rss="${vmrss}"
    fi
  else
    success=0
    current_consecutive=0
  fi

  printf "%d,%s,%s,%d,%d,%d,%d,%d,%d,%d,%d\n" \
    "${cycle}" "${checkpoint_ms}" "${restore_ms}" "${checkpoint_rc}" "${fail_rc}" "${restore_rc}" \
    "${success}" "${old_pid}" "${new_pid}" "${vmrss}" "${snapshot_bytes}" >> "${RAW_CSV}"

  if (( cycle % 10 == 0 )); then
    printf "completed_cycles=%d\n" "${cycle}" | tee -a "${SUMMARY}"
  fi

  if [[ "${success}" -ne 1 ]]; then
    break
  fi
done

cycles_run="$(tail -n +2 "${RAW_CSV}" | wc -l)"
success_rate="$(awk -F, 'NR > 1 { total++; ok += $7 } END { if (total) printf "%.2f", (ok / total) * 100.0; else print "0.00" }' "${RAW_CSV}")"

safe_stop_runtime
"${MANAGER}" init "${CONFIG}" >/dev/null || exit 1
"${MANAGER}" start "${CONFIG}" >/dev/null || exit 1
sleep 1
nohup "${MANAGER}" monitor "${CONFIG}" >/dev/null 2>&1 &
monitor_pid=$!
echo "${monitor_pid}" > runtime/monitor.pid
sample_monitor "${monitor_pid}" 60

monitor_cpu_avg="$(awk -F, 'NR > 1 { sum += $3; n++ } END { if (n) printf "%.6f", sum / n; else print "0.000000" }' "${MON_CSV}")"
monitor_cpu_min="$(awk -F, 'NR == 2 || (NR > 1 && $3 < min) { min = $3 } END { if (NR > 1) printf "%.6f", min; else print "0.000000" }' "${MON_CSV}")"
monitor_cpu_max="$(awk -F, 'NR > 1 && $3 > max { max = $3 } END { printf "%.6f", max }' "${MON_CSV}")"
manager_rss_avg="$(awk -F, 'NR > 1 { sum += $4; n++ } END { if (n) printf "%.2f", sum / n; else print "0.00" }' "${MON_CSV}")"
manager_rss_min="$(awk -F, 'NR == 2 || (NR > 1 && $4 < min) { min = $4 } END { if (NR > 1) printf "%d", min; else print "0" }' "${MON_CSV}")"
manager_rss_max="$(awk -F, 'NR > 1 && $4 > max { max = $4 } END { printf "%d", max }' "${MON_CSV}")"

log_summary "cycles_run=${cycles_run}"
log_summary "success_count=${success_count}"
log_summary "success_rate_percent=${success_rate}"
log_summary "max_consecutive_success=${max_consecutive}"
log_summary "checkpoint_ms_avg_min_max=$(awk -F, 'NR > 1 { sum += $2; if (n == 0 || $2 < min) min = $2; if ($2 > max) max = $2; n++ } END { printf \"%.3f %.3f %.3f\", sum / n, min, max }' "${RAW_CSV}")"
log_summary "restore_ms_avg_min_max=$(awk -F, 'NR > 1 { sum += $3; if (n == 0 || $3 < min) min = $3; if ($3 > max) max = $3; n++ } END { printf \"%.3f %.3f %.3f\", sum / n, min, max }' "${RAW_CSV}")"
log_summary "snapshot_bytes_avg_min_max=$(awk -F, 'NR > 1 { sum += $11; if (n == 0 || $11 < min) min = $11; if ($11 > max) max = $11; n++ } END { printf \"%.2f %d %d\", sum / n, min, max }' "${RAW_CSV}")"
log_summary "largest_restored_vmrss_kb=${max_rss}"
log_summary "monitor_cpu_percent_avg_min_max=${monitor_cpu_avg} ${monitor_cpu_min} ${monitor_cpu_max}"
log_summary "ulcr_manager_monitor_rss_kb_avg_min_max=${manager_rss_avg} ${manager_rss_min} ${manager_rss_max}"
log_summary "manifest_region_count=$(manifest_value region_count)"
log_summary "manifest_fd_count=$(manifest_value fd_count)"
log_summary "manifest_exe_path=$(manifest_value exe_path)"
log_summary "manifest_cwd=$(manifest_value cwd)"
log_summary "raw_cycles_csv=${RAW_CSV}"
log_summary "monitor_samples_csv=${MON_CSV}"

safe_stop_runtime
