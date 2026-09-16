#!/bin/sh
#
# Open a shell in the ESP8266 toolchain container with the project mounted and
# the SDK environment already exported.
#
# Usage:
#   docker/run.sh                  # interactive shell
#   docker/run.sh make -j flash    # run one command and exit
#
# Set ESPPORT to pick a different serial device:
#   ESPPORT=/dev/ttyUSB1 docker/run.sh
#
# The container runs as the calling user so that build/ and sdkconfig stay
# owned by you on the host instead of by root.
#
set -eu

ESPPORT="${ESPPORT:-/dev/ttyUSB0}"
IMAGE="${IMAGE:-greenhouse-esp8266-idf}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

set -- ${1:+"$@"}
if [ "$#" -eq 0 ]; then
    set -- /bin/bash
fi

# Serial access needs the device to exist and the container user to be in a
# group that can open it.
DEVICE_ARGS=""
if [ -e "$ESPPORT" ]; then
    DEVICE_ARGS="--device=$ESPPORT --group-add $(stat -c %g "$ESPPORT")"
else
    echo "warning: $ESPPORT not present, starting without a serial device" >&2
fi

# Only ask for a TTY when we actually have one, so the script also works from
# scripts and CI.
TTY_ARGS="-i"
if [ -t 0 ] && [ -t 1 ]; then
    TTY_ARGS="-it"
fi

exec docker run --rm $TTY_ARGS \
    --user "$(id -u):$(id -g)" \
    $DEVICE_ARGS \
    -e "ESPPORT=$ESPPORT" \
    -e HOME=/tmp \
    -v "$PROJECT_DIR:/project" \
    -w /project \
    "$IMAGE" \
    /bin/bash -c '. "$IDF_PATH/export.sh" >/dev/null 2>&1; exec "$@"' -- "$@"
