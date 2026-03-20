#!/usr/bin/env bash
# setup_cpu.sh — Lock CPU frequency for reproducible benchmarks (Linux only).
# Usage:
#   sudo ./scripts/setup_cpu.sh          # set performance governor, disable turbo
#   sudo ./scripts/setup_cpu.sh --reset  # restore ondemand governor, re-enable turbo
#   sudo ./scripts/setup_cpu.sh --status # show current governor and turbo state

set -euo pipefail

if [ "$(uname)" != "Linux" ]; then
  echo "This script only works on Linux."
  exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "Run with sudo."
  exit 1
fi

show_status() {
  echo "CPU governors:"
  cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "  (not available)"
  if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    val=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)
    [ "$val" = "1" ] && echo "Turbo: disabled" || echo "Turbo: enabled"
  fi
}

if [ "${1:-}" = "--status" ]; then
  show_status
  exit 0
fi

if [ "${1:-}" = "--reset" ]; then
  for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo ondemand > "$f" 2>/dev/null || true
  done
  if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo
  fi
  echo "Reset: ondemand governor, turbo re-enabled."
  show_status
  exit 0
fi

# default: lock to performance, disable turbo
for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  echo performance > "$f"
done
if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
  echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
fi
echo "Set: performance governor, turbo disabled."
show_status
