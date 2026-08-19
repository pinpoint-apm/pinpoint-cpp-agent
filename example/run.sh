#!/usr/bin/env bash
# Runs the distributed tracing demo locally: server (:8081) + proxy (:8080).
#
#   PINPOINT_CPP_COLLECTOR_HOST=my-collector ./run.sh [bin-dir]
#
# bin-dir defaults to the `default` preset's output. Generate traffic with:
#   curl http://localhost:8080/api/members
# Stop with Ctrl-C; both servers are stopped together.
set -euo pipefail

BIN_DIR="${1:-$(dirname "$0")/../build/default/example}"

for app in distributed_server distributed_proxy; do
    if [[ ! -x "$BIN_DIR/$app" ]]; then
        echo "error: $BIN_DIR/$app not found — build the examples first:" >&2
        echo "  cmake --preset default" >&2
        echo "  cmake --build build/default --target distributed_proxy distributed_server" >&2
        exit 1
    fi
done

# Stop both servers on exit, Ctrl-C, or termination. Signals need their own
# trap: an untrapped fatal signal ends bash without running the EXIT trap.
trap 'kill $(jobs -p) 2>/dev/null' EXIT INT TERM

"$BIN_DIR/distributed_server" &
"$BIN_DIR/distributed_proxy" &
wait
