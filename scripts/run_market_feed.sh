#!/usr/bin/env bash
set -euo pipefail

# Lightweight launcher for market_feed that sources per-user credentials
# Credentials file (shell format) is expected at: $HOME/.config/fluxion/credentials

CONFIG="$HOME/.config/fluxion/credentials"
if [ -f "$CONFIG" ]; then
  # protect file permissions
  chmod 600 "$CONFIG" || true
  # shellcheck disable=SC1090
  set -a
  source "$CONFIG"
  set +a
else
  echo "Warning: credentials file $CONFIG not found. You can export ALPACA_API_KEY/ALPACA_SECRET_KEY or create the file." >&2
fi

EXCHANGE=${1:-alpaca}
SYMBOL=${2:-AAPL}
shift 2 || true

exec ./build/market_feed --exchange "$EXCHANGE" --symbol "$SYMBOL" "$@"
