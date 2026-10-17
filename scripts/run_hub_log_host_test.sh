#!/usr/bin/env bash
# Host test for L3.5 (ROADMAP section L): the Hub app's log store.
#
# pocs/hub_shell/hub_log.c is the app side of the TweakLog plumbing - the ring
# every engine/probe line lands in once the app installs hub_log_hook_sink()
# through tweak_log_set_hook(), plus the on-demand export the host can read
# back. It is plain C with one mutex, so the whole contract is testable here:
# ordering, wraparound, truncation, newline handling, export, and the
# two-threads-appending-at-once case (the hook runs on engine threads).
#
# The test compiles the file the app compiles - same source, no stub copy - so a
# ring bug cannot hide behind a hand-written double.
#
#   bash scripts/run_hub_log_host_test.sh
set -euo pipefail

cd "$(dirname "$0")/.."

CC="${CC:-cc}"
OUT="${TMPDIR:-/tmp}/hub_log_host_test"

# shellcheck disable=SC2086
"$CC" -std=gnu99 -Wall -Wextra -Werror -pthread \
    -Ipocs/hub_shell \
    -o "$OUT" \
    tests/hub_log_host_test.c pocs/hub_shell/hub_log.c

exec "$OUT"
