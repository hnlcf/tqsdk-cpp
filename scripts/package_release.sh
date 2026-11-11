#!/usr/bin/env bash
# Build a release tree and Debian/RPM packages.
#
# Typical release host:
#   VCPKG_ROOT=/opt/vcpkg pnpm --dir web install --frozen-lockfile
#   VCPKG_ROOT=/opt/vcpkg ./scripts/package_release.sh
#
# Useful overrides:
#   TQSDK_PACKAGE_BUILD_DIR=build-package
#   TQSDK_PACKAGE_OUTPUT_DIR=dist
#   TQSDK_RPMBUILD=/usr/bin/rpmbuild
set -Eeuo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${TQSDK_PACKAGE_BUILD_DIR:-"$ROOT/build-package"}
OUT=${TQSDK_PACKAGE_OUTPUT_DIR:-"$ROOT/dist"}
FORMATS=${TQSDK_PACKAGE_FORMATS:-"deb,rpm,tgz"}
VERSION_OVERRIDE=""
SKIP_BUILD=0
SKIP_WEB=0
ARCH=$(uname -m)
DEB_ARCH=""
RPM_ARCH=""

usage() {
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --build-dir) BUILD=$2; shift 2 ;;
    --output-dir) OUT=$2; shift 2 ;;
    --version) VERSION_OVERRIDE=$2; shift 2 ;;
    --formats) FORMATS=$2; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --skip-web) SKIP_WEB=1; shift ;;
    --arch) ARCH=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$ARCH" in
x86_64|amd64) DEB_ARCH=amd64; RPM_ARCH=x86_64 ;;
aarch64|arm64) DEB_ARCH=arm64; RPM_ARCH=aarch64 ;;
*) DEB_ARCH=$ARCH; RPM_ARCH=$ARCH ;;
esac

mkdir -p "$OUT"
VERSION=$VERSION_OVERRIDE
if [[ -z "$VERSION" ]]; then
    VERSION=$(sed -n 's/.*kVersionString.*"\([^"]*\)".*/\1/p' \
        "$BUILD/generated/tqsdk/version.h" 2>/dev/null | head -1 || true)
fi
if [[ -z "$VERSION" ]]; then
    VERSION=$(sed -n 's/^project(tqsdk_cpp VERSION \([^ ]*\).*/\1/p' \
        "$ROOT/CMakeLists.txt" | head -1)
fi
[[ -n "$VERSION" ]] || {
    echo "cannot determine version; pass --version or configure/build first" >&2
    exit 2
}

if (( ! SKIP_BUILD )); then
    if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
        [[ -n "${VCPKG_ROOT:-}" ]] || {
            echo "fresh build needs VCPKG_ROOT (or preconfigure $BUILD)" >&2
            exit 2
        }
        cmake -S "$ROOT" -B "$BUILD" -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_COMPILER=clang++ \
            -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
            -DTQSDK_ENABLE_CTP=ON \
            -DTQSDK_BUILD_TESTS=OFF \
            -DTQSDK_BUILD_APPS=ON
    fi
    cmake --build "$BUILD" --target tqsdk-gw tqsdk-node -j "${TQSDK_PACKAGE_JOBS:-4}"
fi

if (( ! SKIP_WEB )); then
    if [[ ! -d "$ROOT/web/node_modules" ]]; then
        pnpm --dir "$ROOT/web" install --frozen-lockfile
    fi
    VITE_API_BASE="${VITE_API_BASE:-/api}" pnpm --dir "$ROOT/web" build
fi
[[ -f "$ROOT/web/dist/index.html" ]] || {
    echo "web/dist/index.html missing; run without --skip-web" >&2
    exit 2
}

STAGE="$OUT/stage/tqsdk-$VERSION"
rm -rf "$STAGE"
mkdir -p "$STAGE"
DESTDIR="$STAGE" cmake --install "$BUILD" --prefix /usr

# Static web terminal.
mkdir -p "$STAGE/usr/share/tqsdk/web"
cp -a "$ROOT/web/dist/." "$STAGE/usr/share/tqsdk/web/"

# Package-specific systemd units (relocate from /opt/tqsdk to /usr).
mkdir -p "$STAGE/usr/lib/systemd/system"
for unit in tqsdk-gw.service tqsdk-node.service tqsdk.target; do
    sed -e 's#/opt/tqsdk/bin/#/usr/bin/#g' \
        -e 's#/opt/tqsdk/docs/#/usr/share/doc/tqsdk/#g' \
        "$ROOT/deploy/systemd/$unit" >"$STAGE/usr/lib/systemd/system/$unit"
done

mkdir -p "$STAGE/etc/tqsdk"
install -m 0644 "$ROOT/config/gw.toml" "$STAGE/etc/tqsdk/gw.toml.example"
install -m 0644 "$ROOT/config/node.toml" "$STAGE/etc/tqsdk/node.toml.example"
install -m 0640 "$ROOT/deploy/systemd/tqsdk.env.example" "$STAGE/etc/tqsdk/tqsdk.env.example"

mkdir -p "$STAGE/usr/share/tqsdk/examples/caddy"
mkdir -p "$STAGE/usr/share/tqsdk/examples/nginx"
install -m 0644 "$ROOT/deploy/caddy/Caddyfile" "$STAGE/usr/share/tqsdk/examples/caddy/Caddyfile"
install -m 0644 "$ROOT/deploy/nginx/tqsdk.conf" "$STAGE/usr/share/tqsdk/examples/nginx/tqsdk.conf"

# Keep deployment docs available even if the CMake install list changes.
mkdir -p "$STAGE/usr/share/doc/tqsdk"
cp -a "$ROOT/docs/." "$STAGE/usr/share/doc/tqsdk/"

# A stable manifest is useful for release review.
(
    cd "$STAGE"
    find . -type f -o -type l | sort >.package-files
)
mv "$STAGE/.package-files" "$OUT/tqsdk-$VERSION.files.txt"

TGZ="$OUT/tqsdk-$VERSION-linux-$RPM_ARCH.tar.gz"
tar -C "$STAGE" -czf "$TGZ" .

IFS=',' read -r -a REQUESTED <<<"$FORMATS"
for format in "${REQUESTED[@]}"; do
    case "$format" in
    tgz)
        echo "built $TGZ"
        ;;
    deb)
        python3 "$ROOT/scripts/package_deb.py" \
            --stage "$STAGE" \
            --output "$OUT/tqsdk_${VERSION}_${DEB_ARCH}.deb" \
            --version "$VERSION" \
            --architecture "$DEB_ARCH" \
            --postinst "$ROOT/deploy/package/postinst.sh"
        ;;
    rpm)
        "$ROOT/scripts/package_rpm.sh" "$STAGE" "$OUT" "$VERSION" "$RPM_ARCH"
        ;;
    *)
        echo "unsupported package format: $format" >&2
        exit 2
        ;;
    esac
done

# Release checksum manifest.
: >"$OUT/SHA256SUMS"
for file in "$OUT"/tqsdk_*.deb "$OUT"/tqsdk-*.rpm "$OUT"/tqsdk-*.tar.gz; do
    [[ -f "$file" ]] || continue
    (cd "$OUT" && sha256sum "$(basename "$file")") >>"$OUT/SHA256SUMS"
done
echo "built $OUT/SHA256SUMS"

echo "release staging: $STAGE"
echo "release output:   $OUT"
