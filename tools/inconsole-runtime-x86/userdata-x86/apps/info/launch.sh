#!/bin/sh
OUT="${INCONSOLE_DATA_ROOT:-./userdata-x86}/system/logs/system-info.log"
{
  echo "== System info $(date) =="
  echo "CPU: $(grep -m1 -E 'model name|Hardware' /proc/cpuinfo | sed 's/.*: *//')"
  echo "RAM: $(grep -m1 '^MemTotal:' /proc/meminfo | awk '{print int($2/1024) " MiB"}')"
  echo "Free data: $(df -m "${INCONSOLE_DATA_ROOT:-./userdata-x86}" | awk 'NR==2 {print $4 " MiB"}')"
} >> "$OUT"
