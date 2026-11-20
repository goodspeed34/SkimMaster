#!/usr/bin/env bash
# Preserve the command output and expose failures through the Checks API.
set -u
set -o pipefail

log=$(mktemp)
trap 'rm -f "$log"' EXIT
"$@" 2>&1 | tee "$log"
status=$?
if (( status != 0 )); then
  excerpt=$(tail -n 90 "$log")
  excerpt=${excerpt: -12000}
  excerpt=${excerpt//%/%25}
  excerpt=${excerpt//$'\r'/%0D}
  excerpt=${excerpt//$'\n'/%0A}
  printf '::error title=Windows CI command failed::%s exited with %s%%0A%s\n' \
    "$*" "$status" "$excerpt"
fi
exit "$status"
