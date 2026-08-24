#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="$PROJECT_DIR/environment.yml"
ENV_NAME="${OWE_CONDA_ENV:-owe-plugin}"
ENV_PREFIX="${OWE_CONDA_PREFIX:-$PROJECT_DIR/build/conda-envs/$ENV_NAME}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
WAYWALLEN_REF="${WAYWALLEN_REF:-main}"
WAYWALLEN_SRC="${WAYWALLEN_SRC:-$PROJECT_DIR/build/waywallen-src}"
BRIDGE_INSTALL_DIR="${BRIDGE_INSTALL_DIR:-$PROJECT_DIR/build/waywallen-bridge-install}"
BUNDLE_DIR="$PROJECT_DIR/build/waywallen-plugin-bundle"
SYMBOLS_DIR="$PROJECT_DIR/build/waywallen-plugin-symbols"
DIST_DIR="${DIST_DIR:-$PROJECT_DIR/dist}"
PLUGIN_ID="org.waywallen.open-wallpaper-engine"

info() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
fail() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

case "$BUILD_TYPE" in
    Debug) LITO_PROFILE="${LITO_PROFILE:-debug}" ;;
    Release) LITO_PROFILE="${LITO_PROFILE:-release}" ;;
    RelWithDebInfo) LITO_PROFILE="${LITO_PROFILE:-relwithdebinfo}" ;;
    *) fail "unsupported BUILD_TYPE for lito profile: $BUILD_TYPE" ;;
esac

host_arch="$(uname -m)"
case "$host_arch" in
    x86_64)
        CONDA_TARGET="linux-64"
        CONDA_GBM_ARCH="x86_64"
        ;;
    aarch64|arm64)
        CONDA_TARGET="linux-aarch64"
        CONDA_GBM_ARCH="aarch64"
        ;;
    *) fail "unsupported host architecture for conda target packages: $host_arch" ;;
esac
ENV_MERGED_FILE="$PROJECT_DIR/build/environment-${CONDA_TARGET}.yml"
CONDA_TARGET_PACKAGES=(
    "clang_${CONDA_TARGET}=22"
    "clangxx_${CONDA_TARGET}=22"
    "sysroot_${CONDA_TARGET}=2.28"
    "mesa-libgbm-devel-conda-${CONDA_GBM_ARCH}=23.1.4"
)

command -v conda >/dev/null || fail "conda not found"
command -v curl >/dev/null || fail "curl not found"
command -v git >/dev/null || fail "git not found"
command -v python3 >/dev/null || fail "python3 not found"
[[ -f "$ENV_FILE" ]] || fail "missing $ENV_FILE"

if [[ -n "${LITO_BIN:-}" ]] && "$LITO_BIN" --help >/dev/null 2>&1; then
    info "Using lito: $LITO_BIN"
else
    info "Installing lito"
    curl -fsSL https://raw.githubusercontent.com/litocpp/lito/main/install.sh | bash
    LITO_BIN="$HOME/.local/bin/lito"
    [[ -x "$LITO_BIN" ]] || fail "lito installer did not create $LITO_BIN"
fi

export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$PROJECT_DIR/build/.cache}"
export CONDARC="${CONDARC:-$PROJECT_DIR/build/condarc}"
if [[ ! -f "$CONDARC" ]]; then
    mkdir -p "$(dirname "$CONDARC")"
    {
        printf 'channels:\n'
        printf '  - conda-forge\n'
        printf '  - nodefaults\n'
        printf 'channel_priority: strict\n'
        printf 'default_channels: []\n'
        printf 'auto_activate_base: false\n'
    } > "$CONDARC"
fi

case "$BRIDGE_INSTALL_DIR" in
    "$PROJECT_DIR"/build/*) ;;
    *) fail "BRIDGE_INSTALL_DIR must stay under $PROJECT_DIR/build" ;;
esac
info "Writing merged conda env: $ENV_MERGED_FILE"
mkdir -p "$(dirname "$ENV_MERGED_FILE")"
python3 - "$ENV_FILE" "$ENV_MERGED_FILE" "${CONDA_TARGET_PACKAGES[@]}" <<'PY'
import sys
from pathlib import Path

src = Path(sys.argv[1])
dst = Path(sys.argv[2])
extras = sys.argv[3:]

lines = src.read_text().splitlines()
existing = set()
in_dependencies = False
for raw in lines:
    line = raw.split("#", 1)[0].rstrip()
    if line.strip() == "dependencies:":
        in_dependencies = True
        continue
    if not in_dependencies:
        continue
    stripped = line.strip()
    if stripped.startswith("- "):
        existing.add(stripped[2:].strip())

extras = [pkg for pkg in extras if pkg not in existing]
if extras:
    insert_at = len(lines)
    for idx, raw in enumerate(lines):
        if raw.strip() == "- llvm-tools=22":
            insert_at = idx + 1
            break

    additions = [f"  - {pkg}" for pkg in extras]
    if insert_at < len(lines) and lines[insert_at].strip():
        additions.append("")
    lines[insert_at:insert_at] = additions

dst.write_text("\n".join(lines) + "\n")
PY

info "Preparing conda env: $ENV_PREFIX"
set +u
# shellcheck disable=SC1091
source "$(conda info --base)/etc/profile.d/conda.sh"
set -u

if [[ -d "$ENV_PREFIX/conda-meta" ]]; then
    conda env update -p "$ENV_PREFIX" -f "$ENV_MERGED_FILE" --prune
else
    conda env create -p "$ENV_PREFIX" -f "$ENV_MERGED_FILE"
fi

set +u
conda activate "$ENV_PREFIX"
set -u

command -v llvm-objcopy >/dev/null || fail "llvm-objcopy not found"

if [[ ! -d "$WAYWALLEN_SRC/.git" ]]; then
    if [[ -e "$WAYWALLEN_SRC" ]]; then
        fail "$WAYWALLEN_SRC exists but is not a git checkout"
    fi
    info "Fetching waywallen"
    git clone https://github.com/waywallen/waywallen.git "$WAYWALLEN_SRC"
fi

info "Selecting waywallen bridge ref: $WAYWALLEN_REF"
if git -C "$WAYWALLEN_SRC" fetch --quiet origin "$WAYWALLEN_REF" 2>/dev/null; then
    git -C "$WAYWALLEN_SRC" -c advice.detachedHead=false checkout --force FETCH_HEAD
else
    git -C "$WAYWALLEN_SRC" fetch --quiet origin
    git -C "$WAYWALLEN_SRC" -c advice.detachedHead=false checkout --force "$WAYWALLEN_REF"
fi

info "Preparing shared waywallen build dependencies"
bash "$WAYWALLEN_SRC/scripts/build_ffmpeg.sh"
bash "$WAYWALLEN_SRC/scripts/copy_syslibs.sh"

info "Cleaning install prefixes"
rm -rf "$BRIDGE_INSTALL_DIR" "$BUNDLE_DIR"
mkdir -p "$BRIDGE_INSTALL_DIR" "$BUNDLE_DIR" "$DIST_DIR"

info "Building waywallen bridge"
"$LITO_BIN" -C "$WAYWALLEN_SRC" install -p waywallen-bridge \
    --no-config \
    --profile "$LITO_PROFILE" \
    --prefix "$BRIDGE_INSTALL_DIR" \
    --config "tools.pkg-config.search-path=[\"$CONDA_PREFIX/lib/pkgconfig\"]"

info "Building open-wallpaper-engine"
OWE_WAYWALLEN_PLUGIN_BUNDLE_LAYOUT=ON "$LITO_BIN" install -p owe-waywallen-plugin \
    --no-config \
    --profile "$LITO_PROFILE" \
    --prefix "$BUNDLE_DIR" \
    --config "tools.pkg-config.search-path=[\"$BRIDGE_INSTALL_DIR/lib/pkgconfig\",\"$CONDA_PREFIX/lib/pkgconfig\"]"

info "Packaging plugin bundle"
rm -f "$DIST_DIR"/*.zip
plugin_version="$(sed -n 's/^version = "\([^"]*\)"$/\1/p' "$BUNDLE_DIR/plugin.toml")"
[[ -n "$plugin_version" ]] || fail "plugin version not found in $BUNDLE_DIR/plugin.toml"
system_name="$(uname -s | tr '[:upper:]' '[:lower:]')"
package_path="$DIST_DIR/$PLUGIN_ID-$plugin_version-$system_name-$host_arch.zip"

info "Packaging renderer debug symbols"
rm -rf "$SYMBOLS_DIR"
scene_bin="bin/waywallen-wescene-renderer"
web_bin="lib/weweb/waywallen-weweb-renderer"
for binary in "$scene_bin" "$web_bin"; do
    [[ -f "$BUNDLE_DIR/$binary" ]] || fail "missing renderer binary: $BUNDLE_DIR/$binary"
    mkdir -p "$SYMBOLS_DIR/$(dirname "$binary")"
    llvm-objcopy --only-keep-debug "$BUNDLE_DIR/$binary" "$SYMBOLS_DIR/$binary.debug"
    llvm-objcopy --strip-unneeded "$BUNDLE_DIR/$binary"
    llvm-objcopy --add-gnu-debuglink="$SYMBOLS_DIR/$binary.debug" "$BUNDLE_DIR/$binary"
done
symbols_path="$DIST_DIR/debug-symbols-$system_name-$host_arch.zip"
cmake -E chdir "$BUNDLE_DIR" cmake -E tar cf "$package_path" --format=zip .
cmake -E chdir "$SYMBOLS_DIR" cmake -E tar cf "$symbols_path" --format=zip .

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    output_path="$package_path"
    symbols_output_path="$symbols_path"
    if [[ "$output_path" == "$PROJECT_DIR/"* ]]; then
        output_path="${output_path#$PROJECT_DIR/}"
    fi
    if [[ "$symbols_output_path" == "$PROJECT_DIR/"* ]]; then
        symbols_output_path="${symbols_output_path#$PROJECT_DIR/}"
    fi
    printf 'zip=%s\n' "$output_path" >> "$GITHUB_OUTPUT"
    printf 'symbols=%s\n' "$symbols_output_path" >> "$GITHUB_OUTPUT"
fi

printf '%s\n' "$package_path"
printf '%s\n' "$symbols_path"
