#!/bin/bash
set -eo pipefail

docker build --build-arg GIMP_VERSION="${GIMP_VERSION:-2}" -t switchwave-builder .

docker run --rm --name devkitpro-switchwave \
    -v "$(pwd)":/mnt/ \
    switchwave-builder \
    bash -c "
        set -e
        git config --global --add safe.directory '*'
        cd /mnt/
        make clean
        make configure-ffmpeg
        make build-ffmpeg -j\$(nproc)
        make configure-uam
        make build-uam
        make configure-mpv
        make build-mpv
        make dist -j\$(nproc)
    "
