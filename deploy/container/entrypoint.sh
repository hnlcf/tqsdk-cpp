#!/bin/sh
set -eu

ROLE=${TQSDK_ROLE:-node}
NODE_CONFIG=${TQSDK_NODE_CONFIG:-/etc/tqsdk/node-sim.toml}
GW_CONFIG=${TQSDK_GW_CONFIG:-/etc/tqsdk/gw-sim.toml}
NODE_PID=
GW_PID=
NGINX_PID=

cleanup() {
    trap - TERM INT EXIT
    for pid in "$NGINX_PID" "$NODE_PID" "$GW_PID"; do
        [ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}

trap cleanup TERM INT EXIT
mkdir -p /var/lib/tqsdk /var/log/tqsdk /run/nginx

start_node() {
    tqsdk-node -c "$NODE_CONFIG" &
    NODE_PID=$!
    echo "tqsdk: node pid=$NODE_PID config=$NODE_CONFIG"
}

start_gw() {
    tqsdk-gw -c "$GW_CONFIG" &
    GW_PID=$!
    echo "tqsdk: gw pid=$GW_PID config=$GW_CONFIG"
}

start_nginx() {
    nginx -c /etc/nginx/nginx.conf -g 'daemon off;' &
    NGINX_PID=$!
    echo "tqsdk: nginx pid=$NGINX_PID"
}

case "$ROLE" in
node)
    start_node
    start_nginx
    wait "$NGINX_PID"
    ;;
gw)
    start_gw
    wait "$GW_PID"
    ;;
all)
    start_gw
    start_node
    start_nginx
    wait "$NGINX_PID"
    ;;
*)
    echo "unknown TQSDK_ROLE=$ROLE (expected node, gw, all)" >&2
    exit 2
    ;;
esac
