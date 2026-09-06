#!/bin/sh
# One-time setup after cloning: submodules, the vendor symlink the OpenTrailPaper
# fork expects, and the PlatformIO packages for every build in the repo.
set -eu
cd "$(dirname "$0")/.."
git submodule update --init --depth 1 vendor/T5S3-4.7-e-paper-PRO
git submodule update --init apps/opentrailpaper
# OpenTrailPaper's platformio.ini looks for the LilyGO board support under its
# own vendor/ (gitignored there); point it at the copy this repo already has.
mkdir -p apps/opentrailpaper/vendor
[ -e apps/opentrailpaper/vendor/T5S3-4.7-e-paper-PRO ] || \
  ln -s ../../../vendor/T5S3-4.7-e-paper-PRO apps/opentrailpaper/vendor/T5S3-4.7-e-paper-PRO
command -v pio >/dev/null || { echo "PlatformIO missing: uv tool install --python 3.13 platformio"; exit 1; }
(cd launcher && pio pkg install)
(cd apps/opentrailpaper && pio pkg install -e t5s3-launcher)
echo "bootstrap done"
