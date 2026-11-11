#!/usr/bin/env bash
# Build an RPM from an already-prepared staging tree.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
STAGE=${1:?staging directory required}
OUTPUT=${2:?output directory required}
VERSION=${3:?version required}
ARCH=${4:-x86_64}
RPMBUILD=${TQSDK_RPMBUILD:-$(command -v rpmbuild || true)}

if [[ -z "$RPMBUILD" ]]; then
    echo "rpmbuild not found; install rpm-build and set TQSDK_RPMBUILD if needed" >&2
    exit 2
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/SPECS" "$TMP/SOURCES" "$TMP/BUILD" "$TMP/BUILDROOT" "$TMP/RPMS" "$TMP/SRPMS"
sed -e "s#@STAGE@#$STAGE#g" \
    -e "s#@VERSION@#$VERSION#g" \
    -e "s#@ARCH@#$ARCH#g" \
    "$ROOT/deploy/package/tqsdk.spec.in" >"$TMP/SPECS/tqsdk.spec"

mkdir -p "$OUTPUT"
"$RPMBUILD" -bb "$TMP/SPECS/tqsdk.spec" \
    --define "_topdir $TMP" \
    --define "_arch $ARCH" \
    >/tmp/tqsdk_rpmbuild.log 2>&1
cp "$TMP/RPMS/$ARCH"/*.rpm "$OUTPUT"/
echo "$OUTPUT/$(basename "$(find "$TMP/RPMS/$ARCH" -name '*.rpm' -print -quit)")"
