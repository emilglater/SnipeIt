#!/bin/bash
# Download the mediaMTX RTSP server binary that streaming_server launches.
#
# The binary is ~39 MB, so it is gitignored rather than committed. This script
# is the record of which build the pipeline expects. Run it once after cloning.
#
# Usage: ./script/fetch_mediamtx.sh

set -e
cd "$(dirname "$0")/.."   # the binary lives at pi-streaming/, next to config/

VERSION="v1.9.3"
ARCHIVE="mediamtx_${VERSION}_linux_arm64v8.tar.gz"
URL="https://github.com/bluenviron/mediamtx/releases/download/${VERSION}/${ARCHIVE}"

if [ -x ./mediamtx ]; then
    echo "[fetch] mediamtx already present: $(./mediamtx --version 2>/dev/null || echo unknown)"
    echo "[fetch] Delete it and re-run to force a fresh download."
    exit 0
fi

echo "[fetch] Downloading ${ARCHIVE}..."
wget -q --show-progress "$URL"

echo "[fetch] Extracting the binary only (the tarball's own config is not used;"
echo "        the pipeline reads config/mediamtx.yml instead)..."
tar -xzf "$ARCHIVE" mediamtx
chmod +x mediamtx
rm -f "$ARCHIVE"

echo "[fetch] Done: ./mediamtx ($(du -h mediamtx | cut -f1))"
