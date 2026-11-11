#!/usr/bin/env bash
# Build and optionally smoke-test the deployment image from a release staging tree.
set -Eeuo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ENGINE=${TQSDK_CONTAINER_ENGINE:-}
IMAGE=${TQSDK_IMAGE:-tqsdk:0.1.0}
BASE_IMAGE=${TQSDK_BASE_IMAGE:-registry.fedoraproject.org/fedora:44}
VERSION=${TQSDK_VERSION:-0.1.0}
RELEASE_DIR=${TQSDK_RELEASE_DIR:-dist/stage/tqsdk-$VERSION}
SMOKE=0
PORT=${TQSDK_SMOKE_PORT:-18088}

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; }
while [[ $# -gt 0 ]]; do
    case "$1" in
    --engine) ENGINE=$2; shift 2 ;;
    --image) IMAGE=$2; shift 2 ;;
    --base-image) BASE_IMAGE=$2; shift 2 ;;
    --version) VERSION=$2; shift 2 ;;
    --release-dir) RELEASE_DIR=$2; shift 2 ;;
    --smoke-test) SMOKE=1; shift ;;
    --port) PORT=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$ENGINE" ]]; then
    if command -v podman >/dev/null 2>&1; then ENGINE=podman; else ENGINE=docker; fi
fi
[[ -f "$ROOT/$RELEASE_DIR/usr/bin/tqsdk-node" ]] || {
    echo "release tree missing: $ROOT/$RELEASE_DIR" >&2
    echo "run ./scripts/package_release.sh --formats tgz first" >&2
    exit 2
}
[[ -f "$ROOT/web/dist/index.html" ]] || {
    echo "web/dist missing; run the package release first" >&2
    exit 2
}

cd "$ROOT"
FORMAT_ARGS=()
if [[ "$ENGINE" == podman ]]; then
    FORMAT_ARGS=(--format docker)
fi
"$ENGINE" build "${FORMAT_ARGS[@]}" -f deploy/container/Dockerfile -t "$IMAGE" \
    --build-arg "BASE_IMAGE=$BASE_IMAGE" \
    --build-arg "TQSDK_VERSION=$VERSION" \
    --build-arg "RELEASE_DIR=$RELEASE_DIR" \
    .

if (( SMOKE )); then
    NAME="tqsdk-smoke-$$"
    cleanup() { "$ENGINE" rm -f "$NAME" >/dev/null 2>&1 || true; }
    trap cleanup EXIT
    "$ENGINE" run -d --name "$NAME" -p "$PORT:8080" "$IMAGE"
    for _ in $(seq 1 40); do
        if curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; then break; fi
        sleep 0.5
    done
    echo "healthz: $(curl -fsS "http://127.0.0.1:$PORT/healthz")"
    echo "index:   $(curl -fsS "http://127.0.0.1:$PORT/" | head -c 80)"
    echo "accounts: $(curl -fsS "http://127.0.0.1:$PORT/api/accounts")"
    echo "smoke container: $NAME (removed)"
fi
echo "image: $IMAGE"
