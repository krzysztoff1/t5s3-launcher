#!/bin/sh
# Apps carry verbatim copies of the two files the launcher owns. Fail loudly if
# any copy has drifted: a mismatched partition table means an image flashed at
# the wrong offset, and a mismatched API header means an app that does not hand
# back to the launcher the way the launcher expects.
set -eu
cd "$(dirname "$0")/.."
status=0
check() {
  if [ -e "$2" ]; then
    if cmp -s "$1" "$2"; then
      echo "ok       $2"
    else
      echo "DRIFTED  $2  (differs from $1)"; status=1
    fi
  else
    echo "missing  $2"
  fi
}
check partitions.csv           apps/opentrailpaper/partitions_launcher.csv
check launcher_api/launcher_api.h apps/opentrailpaper/src/launcher_api.h
check partitions.csv           apps/_template/partitions_launcher.csv
check launcher_api/launcher_api.h apps/_template/src/launcher_api.h
exit $status
