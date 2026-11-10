#!/bin/bash
# Load-tests a running Vortex server with ApacheBench (ab).
#
# Usage: scripts/benchmark.sh [url] [requests] [concurrency] [extra ab flags...]
#   scripts/benchmark.sh                                    # 50k requests, 1000 concurrent, new connection each
#   scripts/benchmark.sh http://127.0.0.1:8080/ 50000 1000 -k   # same, with keep-alive
set -euo pipefail

URL=${1:-http://127.0.0.1:8080/}
REQUESTS=${2:-50000}
CONCURRENCY=${3:-1000}
shift $(( $# < 3 ? $# : 3 ))
EXTRA_FLAGS=("$@")

if ! command -v ab &> /dev/null; then
    echo "ApacheBench (ab) not found. Install it with: sudo apt-get install apache2-utils" >&2
    exit 1
fi

if ! curl -fsS -o /dev/null "$URL"; then
    echo "No 200 response from $URL. Start the server first, e.g. ./build/vortex_server" >&2
    exit 1
fi

# ab needs one descriptor per concurrent connection.
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

echo "Warming up (5,000 requests)..."
ab -q -n 5000 -c 100 "${EXTRA_FLAGS[@]}" "$URL" > /dev/null

echo "Running: ab -n $REQUESTS -c $CONCURRENCY ${EXTRA_FLAGS[*]} $URL"
ab -n "$REQUESTS" -c "$CONCURRENCY" "${EXTRA_FLAGS[@]}" "$URL"
