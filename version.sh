#!/bin/bash
VER=${AIRBRIDGE_VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo "unknown")}
if [ -n "$SOURCE_DATE_EPOCH" ]; then
    DATE=$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M)
else
    DATE=$(date +%Y-%m-%dT%H:%M)
fi
echo "-DAIRBRIDGE_VERSION=\\\"$VER\\\" -DAIRBRIDGE_BUILD_DATE=\\\"$DATE\\\""
