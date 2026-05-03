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

# Some hosts (notably AWS EC2 Graviton) don't expose cpufreq at all —
# scaling_governor either doesn't exist or is read-only. Detect that
# and skip rather than failing under set -e.
HAS_CPUFREQ=0
if compgen -G '/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor' > /dev/null; then
  HAS_CPUFREQ=1
fi

if [ "${1:-}" = "--reset" ]; then
  if [ "$HAS_CPUFREQ" = "1" ]; then
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
      echo ondemand > "$f" 2>/dev/null || true
    done
  else
    echo "cpufreq not exposed on this host — nothing to reset."
  fi
  if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo
  fi
  echo "Reset complete."
  show_status
  exit 0
fi

# default: lock to performance, disable turbo
if [ "$HAS_CPUFREQ" = "1" ]; then
  for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$f" 2>/dev/null || true
  done
  echo "Set: performance governor."
else
  echo "cpufreq not exposed on this host (typical on AWS EC2 Graviton);"
  echo "  skipping governor setting. CPU frequency is hypervisor-managed."
fi
if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
  echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
  echo "Turbo disabled."
fi
show_status
