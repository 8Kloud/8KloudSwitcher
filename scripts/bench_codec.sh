#!/usr/bin/env bash
# Measures OMT/VMX codec CPU cost on this host: encode (kloud-testgen process)
# and decode (kloud-latmeter process) in cores, plus delivered fps/latency.
# Usage: bench_codec.sh [WxH] [seconds] [builddir]
set -euo pipefail

SIZE="${1:-7680x4320}"
SECS="${2:-30}"
BUILD="${3:-build}"
CSV="$(mktemp /tmp/kloudbench-XXXX.csv)"
CLK_TCK="$(getconf CLK_TCK)"

"$BUILD/kloud-testgen" --size "$SIZE" --name "KloudBench" --quiet &
TG=$!
sleep 3
"$BUILD/kloud-latmeter" --source "KloudBench" --csv "$CSV" --quiet &
LM=$!

cleanup() { kill "$TG" "$LM" 2>/dev/null || true; wait 2>/dev/null || true; }
trap cleanup EXIT

cpu_secs() { awk '{print ($14+$15)/'"$CLK_TCK"'}' "/proc/$1/stat"; }

sleep 5  # warmup
T0=$(date +%s.%N); C0_TG=$(cpu_secs "$TG"); C0_LM=$(cpu_secs "$LM")
sleep "$SECS"
T1=$(date +%s.%N); C1_TG=$(cpu_secs "$TG"); C1_LM=$(cpu_secs "$LM")

echo "== SpeedHQ bench: $SIZE, ${SECS}s window =="
awk -v t0="$T0" -v t1="$T1" -v a0="$C0_TG" -v a1="$C1_TG" \
    -v b0="$C0_LM" -v b1="$C1_LM" 'BEGIN {
  dt = t1 - t0
  printf "  testgen  (pattern stamp + VMX encode): %5.2f cores\n", (a1-a0)/dt
  printf "  latmeter (VMX decode + strip read):    %5.2f cores\n", (b1-b0)/dt
}'
echo "  last latmeter samples (fps / gaps / latency):"
tail -n 3 "$CSV" | sed 's/^/    /'
echo "  csv: $CSV"
