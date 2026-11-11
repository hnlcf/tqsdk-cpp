#!/usr/bin/env bash
# Static deployment checks that do not start production services.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
STATUS=0

if command -v systemd-analyze >/dev/null 2>&1; then
    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT
    sed 's#/opt/tqsdk/bin/tqsdk-gw#/bin/true#' \
        "$ROOT/deploy/systemd/tqsdk-gw.service" >"$TMP/tqsdk-gw.service"
    sed 's#/opt/tqsdk/bin/tqsdk-node#/bin/true#' \
        "$ROOT/deploy/systemd/tqsdk-node.service" >"$TMP/tqsdk-node.service"
    cp "$ROOT/deploy/systemd/tqsdk.target" "$TMP/"
    if systemd-analyze verify \
        "$TMP/tqsdk-gw.service" "$TMP/tqsdk-node.service" "$TMP/tqsdk.target"; then
        echo "PASS systemd units"
    else
        echo "FAIL systemd units" >&2
        STATUS=1
    fi
else
    echo "SKIP systemd-analyze not installed"
fi

if command -v caddy >/dev/null 2>&1; then
    if caddy validate --config "$ROOT/deploy/caddy/Caddyfile" --adapter caddyfile; then
        echo "PASS caddy config"
    else
        echo "FAIL caddy config" >&2
        STATUS=1
    fi
else
    echo "SKIP caddy not installed"
fi

if command -v nginx >/dev/null 2>&1; then
    echo "INFO nginx config requires a domain/certificate; run nginx -t after installing"
else
    echo "SKIP nginx not installed"
fi

for file in \
    "$ROOT/deploy/nginx/tqsdk.conf" \
    "$ROOT/deploy/caddy/Caddyfile"; do
    rg -q 'Upgrade|reverse_proxy' "$file" || {
        echo "FAIL missing WebSocket/proxy directive in $file" >&2
        STATUS=1
    }
done
echo "deploy checks rc=$STATUS"
exit "$STATUS"
