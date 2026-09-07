#!/bin/sh
# Host check of the paragraph model and paginator over the sample books.
set -e
cd "$(dirname "$0")/../.."
mkdir -p .pio/host
c++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -I src -I lib/stb \
    src/textmodel.cpp src/layout.cpp test/host/font_stub.cpp test/host/test_layout.cpp -o .pio/host/test_layout
.pio/host/test_layout "$@"
