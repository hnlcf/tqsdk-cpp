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
FORMATS=${TQSDK_PACKAGE_FORMATS:-"deb,rpm,tgz,container"}
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
# rpmbuild %install runs from a temporary directory; make every path passed to
# stages/specs absolute so a relative TQSDK_PACKAGE_OUTPUT_DIR works in CI.
OUT=$(cd "$OUT" && pwd)
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
    cmake --build "$BUILD" --target tqsdk-gw tqsdk-node tqsdk_backtest -j "${TQSDK_PACKAGE_JOBS:-4}"
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

# The full staging tree is used by the container image. Package-specific
# runtime/app/libs/dev trees are split below.

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

# Split the install tree into installable components.
RUNTIME_STAGE="$OUT/stage/tqsdk-runtime-$VERSION"
APP_STAGE="$OUT/stage/tqsdk-app-$VERSION"
LIBS_STAGE="$OUT/stage/tqsdk-libs-$VERSION"
DEV_STAGE="$OUT/stage/tqsdk-dev-$VERSION"
rm -rf "$RUNTIME_STAGE" "$APP_STAGE" "$LIBS_STAGE" "$DEV_STAGE"
cp -a "$STAGE" "$RUNTIME_STAGE"
rm -rf "$RUNTIME_STAGE/usr/include" "$RUNTIME_STAGE/usr/lib/cmake"
cp -a "$RUNTIME_STAGE" "$APP_STAGE"
find "$APP_STAGE/usr/lib" -maxdepth 1 \( -name 'libtqsdk*' -o -name 'libthost*' \) -delete 2>/dev/null || true

mkdir -p "$LIBS_STAGE/usr/lib"
find "$STAGE/usr/lib" -maxdepth 1 \( -name 'libtqsdk*' -o -name 'libthost*' \) -exec cp -a -t "$LIBS_STAGE/usr/lib" {} +

mkdir -p "$DEV_STAGE/usr/include" "$DEV_STAGE/usr/lib"
cp -a "$STAGE/usr/include/." "$DEV_STAGE/usr/include/"
if [[ -d "$STAGE/usr/lib/cmake" ]]; then
    cp -a "$STAGE/usr/lib/cmake" "$DEV_STAGE/usr/lib/"
fi

TGZ="$OUT/tqsdk-$VERSION-linux-$RPM_ARCH.tar.gz"
LIBS_TGZ="$OUT/tqsdk-libs-$VERSION-linux-$RPM_ARCH.tar.gz"
DEV_TGZ="$OUT/tqsdk-dev-$VERSION-linux-$RPM_ARCH.tar.gz"
tar -C "$RUNTIME_STAGE" -czf "$TGZ" .
tar -C "$LIBS_STAGE" -czf "$LIBS_TGZ" .
tar -C "$DEV_STAGE" -czf "$DEV_TGZ" .

IFS=',' read -r -a REQUESTED <<<"$FORMATS"
for format in "${REQUESTED[@]}"; do
    case "$format" in
    tgz)
        echo "built $TGZ"
        echo "built $LIBS_TGZ"
        echo "built $DEV_TGZ"
        ;;
    deb)
        python3 "$ROOT/scripts/package_deb.py" \
            --stage "$APP_STAGE" \
            --output "$OUT/tqsdk_${VERSION}_${DEB_ARCH}.deb" \
            --name tqsdk \
            --version "$VERSION" \
            --architecture "$DEB_ARCH" \
            --depends "tqsdk-libs (= ${VERSION}-1), libc6, libstdc++6, libgcc-s1, passwd" \
            --postinst "$ROOT/deploy/package/postinst.sh"
        python3 "$ROOT/scripts/package_deb.py" \
            --stage "$LIBS_STAGE" \
            --output "$OUT/tqsdk-libs_${VERSION}_${DEB_ARCH}.deb" \
            --name tqsdk-libs \
            --version "$VERSION" \
            --architecture "$DEB_ARCH" \
            --depends "libc6, libstdc++6, libgcc-s1" \
            --description "TQSDK shared runtime libraries" \
            --description-body " Shared core/backtest libraries and CTP runtime." \
            --allow-no-node
        python3 "$ROOT/scripts/package_deb.py" \
            --stage "$DEV_STAGE" \
            --output "$OUT/tqsdk-dev_${VERSION}_${DEB_ARCH}.deb" \
            --name tqsdk-dev \
            --version "$VERSION" \
            --architecture "$DEB_ARCH" \
            --depends "tqsdk-libs (= ${VERSION}-1)" \
            --description "TQSDK C++ development headers and CMake exports" \
            --description-body " Headers, CTP SDK headers and CMake package exports." \
            --allow-no-node
        ;;
    rpm)
        "$ROOT/scripts/package_rpm.sh" "$APP_STAGE" "$OUT" "$VERSION" "$RPM_ARCH" tqsdk.spec.in
        "$ROOT/scripts/package_rpm.sh" "$LIBS_STAGE" "$OUT" "$VERSION" "$RPM_ARCH" tqsdk-libs.spec.in
        "$ROOT/scripts/package_rpm.sh" "$DEV_STAGE" "$OUT" "$VERSION" "$RPM_ARCH" tqsdk-devel.spec.in
        ;;
    container)
        ENGINE=${TQSDK_CONTAINER_ENGINE:-}
        if [[ -z "$ENGINE" ]]; then
            if command -v podman >/dev/null 2>&1; then ENGINE=podman; else ENGINE=docker; fi
        fi
        "$ROOT/scripts/build_container.sh" \
            --engine "$ENGINE" \
            --version "$VERSION" \
            --release-dir "dist/stage/tqsdk-$VERSION" \
            --archive "$OUT/tqsdk-image-$VERSION-$ARCH.tar.gz"
        ;;
    *)
        echo "unsupported package format: $format" >&2
        exit 2
        ;;
    esac
done

# Optional Python wheel (uv is the project's Python environment manager).
if command -v uv >/dev/null 2>&1 && [[ -f "$ROOT/pyproject.toml" ]]; then
    uv build --wheel --out-dir "$OUT" >/dev/null
    echo "built Python wheel(s) in $OUT"
fi

# Release checksum manifest.
: >"$OUT/SHA256SUMS"
for file in "$OUT"/tqsdk*.deb "$OUT"/tqsdk-*.rpm "$OUT"/tqsdk-*.tar.gz "$OUT"/*.whl; do
    [[ -f "$file" ]] || continue
    (cd "$OUT" && sha256sum "$(basename "$file")") >>"$OUT/SHA256SUMS"
done
echo "built $OUT/SHA256SUMS"

echo "release staging: $STAGE"
echo "release output:   $OUT"
