#!/usr/bin/env bash
# Boot Zenbite headlessly in QEMU and wait for the "ZENBITE READY" banner on
# the serial port. Used by CI; usable locally too.
set -euo pipefail

IMG="${1:-zenbite.img}"
SETUP="${2:-zenbite_setup.img}"
INST1="${3:-zenbite_install1.img}"
INST2="${4:-zenbite_install2.img}"
TARGET="${5:-zenbite_target.img}"
TIMEOUT="${TIMEOUT:-30}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG=$(mktemp)

# Spawn QEMU with stdio serial, no display. We use the x86_64 binary so the
# kernel's CPUID 64-bit detection fires.
"$QEMU" \
    -fda "$IMG" -fdb "$SETUP" \
    -hda "$INST1" -hdb "$INST2" -hdc "$TARGET" -boot a \
    -display none -serial file:"$LOG" \
    -no-reboot -no-shutdown &
QPID=$!

trap 'kill $QPID 2>/dev/null || true; rm -f "$LOG"' EXIT

# Poll the log for the readiness banner.
for _ in $(seq 1 "$TIMEOUT"); do
    if grep -q "ZENBITE READY" "$LOG" 2>/dev/null; then
        echo "boot OK"
        cat "$LOG"
        exit 0
    fi
    sleep 1
done

echo "boot timed out after ${TIMEOUT}s" >&2
cat "$LOG" >&2
exit 1
