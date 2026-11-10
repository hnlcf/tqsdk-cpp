#!/usr/bin/env bash
# Build the checked-in WASM indicator plugins and copy their public assets.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

if ! command -v cargo >/dev/null || ! command -v rustup >/dev/null; then
    echo "cargo/rustup are required to build WASM plugins" >&2
    exit 2
fi

if (($# > 0)); then
    PLUGINS=("$@")
else
    PLUGINS=("$ROOT"/wasm/plugins/*)
fi

rustup target add wasm32-unknown-unknown >/dev/null

for PLUGIN in "${PLUGINS[@]}"; do
    [[ -f "$PLUGIN/Cargo.toml" ]] || continue
    NAME=$(basename "$PLUGIN")
    TARGET="$ROOT/web/public/plugins/$NAME"
    cargo build --manifest-path "$PLUGIN/Cargo.toml" --target wasm32-unknown-unknown --release

    CRATE=$(python3 -c '
import pathlib, sys, tomllib
manifest = tomllib.loads(pathlib.Path(sys.argv[1]).read_text())
print(manifest["package"]["name"].replace("-", "_"))
' "$PLUGIN/Cargo.toml")
    WASM="$PLUGIN/target/wasm32-unknown-unknown/release/$CRATE.wasm"
    mkdir -p "$TARGET"
    cp "$WASM" "$TARGET/$NAME.wasm"
    cp "$PLUGIN/manifest.json" "$TARGET/manifest.json"
    (cd "$TARGET" && sha256sum "$NAME.wasm" >sha256.txt)
    echo "built $TARGET"
done
