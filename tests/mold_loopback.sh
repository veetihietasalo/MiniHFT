#!/usr/bin/env bash
# End to end over real multicast: gen_itch -> mold_send -> kernel -> mold_recv -> book builder.
# Registered with ctest on POSIX builds (Multicast.MoldUdp64Loopback).
#
# Usage: mold_loopback.sh <directory with the programs> <base port>
#
# Three scenarios, each on its own ports:
#   clean       one feed: everything arrives, the books are consistent       (mold_recv exits 0)
#   a-drops     feed A drops every 3rd packet, no B feed: gaps are reported  (mold_recv exits 1)
#   b-fills     the same drops with a B feed: B fills every gap              (mold_recv exits 0)
# The sender is paced so that a slow (sanitizer) build doesn't drop datagrams of its own. Needs a
# multicast-capable interface: packets go out through the default route with IP_MULTICAST_LOOP on.

set -u
bin="$1"
base="$2"
work="$(mktemp -d)"
trap 'kill "$recv_pid" 2>/dev/null; rm -rf "$work"' EXIT
recv_pid=""
cd "$work" || exit 2
"$bin/gen_itch" --orders=20000 --out=feed.itch > /dev/null || exit 2

failures=0

# scenario NAME EXPECTED_EXIT PORT RECV_ARGS SEND_ARGS: runs one, checks the exit code.
scenario() {
    local name=$1 expected=$2 port=$3 recv_args=$4 send_args=$5
    # shellcheck disable=SC2086 # the argument strings split on purpose
    "$bin/mold_recv" --port="$port" --idle-ms=3000 $recv_args > "recv-$name.txt" 2>&1 &
    recv_pid=$!
    # Wait for the receiver to have joined: it prints its first line after IP_ADD_MEMBERSHIP.
    for _ in $(seq 100); do
        grep -q "mold_recv" "recv-$name.txt" 2>/dev/null && break
        kill -0 "$recv_pid" 2>/dev/null || break
        sleep 0.05
    done
    # shellcheck disable=SC2086
    "$bin/mold_send" feed.itch --port="$port" --pps=20000 $send_args > "send-$name.txt" 2>&1
    wait "$recv_pid"
    local status=$?
    recv_pid=""
    if [[ $status == "$expected" ]]; then
        echo "ok    $name: mold_recv exited $status"
    else
        echo "FAIL  $name: mold_recv exited $status, expected $expected"
        failures=$((failures + 1))
    fi
    sed 's/^/        /' "send-$name.txt" "recv-$name.txt"
}

scenario clean 0 "$base" "" ""
scenario a-drops 1 $((base + 1)) "" "--drop-a=3"
grep -q "lost      0 messages" recv-a-drops.txt && { echo "FAIL  a-drops: no loss reported"; failures=$((failures + 1)); }
scenario b-fills 0 $((base + 2)) "--group-b=239.192.0.2 --port-b=$((base + 12))" \
    "--drop-a=3 --group-b=239.192.0.2 --port-b=$((base + 12))"
grep -q "0 duplicates dropped" recv-b-fills.txt && { echo "FAIL  b-fills: the B feed delivered nothing"; failures=$((failures + 1)); }

exit $((failures > 0))
