#!/bin/bash
URL="https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip"
echo "Checking connectivity..."
curl -sSL --max-time 30 "$URL" -o /tmp/gtest.zip && sha256sum /tmp/gtest.zip || echo "Download failed"
