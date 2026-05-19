#!/usr/bin/env bash
set -euo pipefail

# Lightweight launcher for Binance websocket streaming.

EXCHANGE=${1:-binance}
SYMBOL=${2:-BTCUSDT}
shift 2 || true

if [ "$EXCHANGE" != "binance" ]; then
  echo "Only the Binance live feed is supported by the current build. Use ./build/binance_ws directly for live GPU testing." >&2
  exit 1
fi

exec ./build/binance_ws --symbol "$SYMBOL" --normalize "$@"
