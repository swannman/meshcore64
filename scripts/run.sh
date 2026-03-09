#!/bin/bash
# Launch tcpser + VICE for testing meshcore64
#
# Prerequisites:
#   brew install vice
#   brew install tcpser (or build from source)
#
# Usage: ./scripts/run.sh [host:port]

set -e

HOST="${1:-192.168.2.145:5000}"
TCPSER_PORT=25232
PRG="build/meshcore64.prg"
D64="build/meshcore64.d64"

if [ ! -f "$PRG" ]; then
    echo "Build first: make"
    exit 1
fi

# Create blank .d64 disk image if it doesn't exist (for config persistence)
if [ ! -f "$D64" ]; then
    echo "Creating disk image $D64..."
    c1541 -format "meshcore,mc" d64 "$D64"
fi

# Copy PRG onto the disk image (autostart needs it on device 8)
echo "Writing PRG to disk image..."
c1541 -attach "$D64" -delete meshcore64 -write "$PRG" meshcore64 2>/dev/null || true

# Kill any existing tcpser
pkill tcpser 2>/dev/null || true
sleep 1

# Start tcpser in background
echo "Starting tcpser on port $TCPSER_PORT..."
tcpser -v $TCPSER_PORT -s 9600 -l 4 &
TCPSER_PID=$!
sleep 1

echo "Starting VICE with SwiftLink..."
x64sc \
    -acia1 \
    -myaciadev 0 \
    -rsdev1 "127.0.0.1:$TCPSER_PORT" \
    -rsdev1baud 9600 \
    +rsdev1ip232 \
    -8 "$D64" \
    -autostart "$PRG"

# Cleanup
kill $TCPSER_PID 2>/dev/null || true
echo "Done."
