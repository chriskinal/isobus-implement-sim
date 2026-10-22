#!/usr/bin/env bash
# Phase 0 check (docs/PLAN.md section 8): the stack's seeder example, built unchanged, claims an
# address on a virtual CAN interface.
#
#   sudo modprobe vcan && sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
#   tests/phase0_vcan.sh [build/seeder_example/SeederExample] [vcan0]
set -uo pipefail
bin=${1:-build/seeder_example/SeederExample}
iface=${2:-vcan0}
log=$(mktemp)
candump -L "$iface" > "$log" &
dump=$!
sleep 0.3
# The example runs until SIGINT; five seconds is enough for the claim and a few maintenance messages.
# The example loads BasePool.iop from the working directory, so run it from its own.
(cd "$(dirname "$bin")" && timeout -s INT 5 "./$(basename "$bin")" --interface "$iface" > /dev/null 2>&1)
sleep 0.3
kill "$dump" 2> /dev/null
wait "$dump" 2> /dev/null
# An address claim is PGN 0xEE00 to the global address: CAN id 18EEFFxx, xx the claimed address.
claim=$(grep -oE '18EEFF[0-9A-F]{2}#[0-9A-F]{16}' "$log" | head -1)
if [ -n "$claim" ]; then
  echo "ok    address claim on $iface: $claim ($(wc -l < "$log") frames in 5 s)"
  rm -f "$log"
  exit 0
fi
echo "FAIL  no address claim on $iface; frames seen:"
cat "$log"
rm -f "$log"
exit 1
