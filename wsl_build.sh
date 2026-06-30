#!/bin/bash
set -e
PROJ="/mnt/d/Projects/SDE Projects/AdaptTree"
cd "$PROJ"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -Wno-dev 2>/dev/null
cmake --build build -j$(nproc)
