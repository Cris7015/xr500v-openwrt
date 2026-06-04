#!/bin/bash
# Captures the UART boot log for a given interval (default 90s)
# Usage: ./capture-uart.sh [seconds] [output_file]

DURATION="${1:-90}"
OUTFILE="${2:-/tmp/xr500v_boot_$(date +%Y%m%d_%H%M%S).log}"
DEV="/dev/ttyUSB0"

# Check device
if [[ ! -e "$DEV" ]]; then
    echo "ERROR: $DEV does not exist — check usbipd attach from Windows" >&2
    exit 1
fi

# Permissions
sudo chmod 666 "$DEV" 2>/dev/null

# Configure raw port
stty -F "$DEV" 115200 cs8 -cstopb -parenb -ixon -ixoff raw -echo -echoe -echok -echoctl -echoke -onlcr 2>/dev/null

echo "[+] Capturing $DEV for ${DURATION}s → $OUTFILE"
echo "[+] (Power cycle the router NOW if you want to capture the full boot)"

timeout "$DURATION" cat "$DEV" > "$OUTFILE" 2>&1 &
CAT_PID=$!

# Show progress
for i in $(seq 1 $((DURATION/5))); do
    sleep 5
    SIZE=$(wc -c < "$OUTFILE" 2>/dev/null || echo 0)
    echo "  [${i}*5s] bytes=$SIZE"
done

wait $CAT_PID 2>/dev/null

echo
echo "=== BOOT LOG (filtrado a printable) ==="
tr -cd '[:print:]\n\r\t' < "$OUTFILE" | tr '\r' '\n' | tr -s '\n' | tail -60
echo
echo "=== Saved to: $OUTFILE ==="
echo "=== Total bytes: $(wc -c < "$OUTFILE") ==="
