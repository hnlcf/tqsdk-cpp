#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
rm -f /tmp/tqsdk-gw.sock
export TQSDK_LOG_LEVEL=${TQSDK_LOG_LEVEL:-info}
./build/tqsdk-gw -c config/gw.toml > /tmp/gw.log 2>&1 &
GW_PID=$!
sleep 1
./build/tqsdk-node -c config/node.toml > /tmp/node.log 2>&1 &
NODE_PID=$!
sleep 2

echo "=== gw.log ==="; cat /tmp/gw.log
echo "=== node.log ==="; cat /tmp/node.log

echo "=== curl /healthz ==="; curl -s http://127.0.0.1:8080/healthz; echo
echo "=== curl /account ==="; curl -s http://127.0.0.1:8080/account; echo
echo "=== curl /quote ==="; curl -s http://127.0.0.1:8080/quote/SHFE.rb2610; echo
echo "=== POST /order (market buy open 2) ==="
curl -s -X POST http://127.0.0.1:8080/order \
  -H 'Content-Type: application/json' \
  -d '{"instrument_id":"SHFE.rb2610","direction":"BUY","offset":"OPEN","price_type":"ANY","volume":2}'; echo
sleep 1
echo "=== /orders ==="; curl -s http://127.0.0.1:8080/orders; echo
echo "=== /positions ==="; curl -s http://127.0.0.1:8080/positions; echo
echo "=== /account (after fill) ==="; curl -s http://127.0.0.1:8080/account; echo

kill $NODE_PID 2>/dev/null
kill $GW_PID 2>/dev/null
sleep 1
