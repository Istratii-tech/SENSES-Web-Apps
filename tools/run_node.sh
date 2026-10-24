#!/bin/sh
# Runs a Node-targeted build with a time limit (macOS has no timeout(1)).
# Usage: run_node.sh SECONDS SCRIPT.js [ARGS...]
limit=$1; shift
node "$@" &
pid=$!
# The watchdog must not hold our stdout open, or a pipe reading this
# script's output would wait for the sleep to finish.
(sleep "$limit" && kill "$pid") >/dev/null 2>&1 &
watchdog=$!
wait "$pid"; status=$?
pkill -P "$watchdog" sleep 2>/dev/null
kill "$watchdog" 2>/dev/null
[ "$status" -eq 143 ] && echo "run_node: killed after ${limit}s" >&2
exit $status
