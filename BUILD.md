# Building open-wallpaper-engine

OWE is built with [lito](https://github.com/litocpp/lito).  

## Requirements

| Dependency | Version | Notes |
|------------|---------|-------|
| Clang | 22+ | C++20 modules toolchain |
| lito | 0.8.10+ | C++ builder |
| CMake | 4.0+ | used for CMake dependencies |
| Ninja | recent | CMake dependency builds |
| pkg-config | - | system dependency discovery |
| Vulkan SDK | 1.1+ | loader and headers |
| Wayland client, wayland-scanner, X11, XCB, XCB RandR, xkbcommon | - | Linux window backends |
| liblz4 | - | `.pkg` decompression |
| OpenAL Soft | - | Headers and `openal.pc` for builds; optional shared library at runtime for Scene spatial audio |

The complete release environment is declared in `environment.yml`.

## Build

The default workspace members build both standalone CLIs:

```bash
lito build --profile release
```

Build an individual binary when only one surface is needed:

```bash
lito build -p owe-cli-scene --profile release
lito build -p owe-cli-web --profile release
```

Release binaries are written under:

```text
build/release/bin/owe-cli-scene/owe-scene
build/release/bin/owe-cli-web/owe-web
```

On Linux, use `OWE_WINDOW_BACKEND=x11` or `OWE_WINDOW_BACKEND=wayland` to select
a window backend. `WP_HEADLESS=1` and `--screenshot` run the Scene CLI without
opening a window. To install both previews with the same executable paths as
the prebuilt ZIP and stage the Web CLI's CEF runtime, run:

```bash
lito install -p owe-cli-scene -p owe-cli-web --profile release --prefix "$PWD/build/preview"
```

Run the [preview examples](README.md#standalone-viewer) from `build/preview`.

## Plugin bundle

The release helper builds and installs `waywallen-bridge`, invokes lito for the OWE
plugin, and packages:

```bash
./scripts/build_waywallen_plugin.sh
```

The script writes `open-wallpaper-engine-<version>-linux-<arch>.zip` to `dist/`.

With the bridge already installed, build and install the local plugin with:

```bash
lito install -p owe-waywallen-plugin --prefix ../install --profile debug
```

The plugin package enables the `waywallen` feature on both CLI packages. The
daemon's `--ipc` argument selects the bridge without opening a preview window.

The bundle contains:

```text
plugin.toml
files.txt
main.lua
wallpaper_engine/
bin/owe-scene
bin/weweb/owe-web
bin/weweb/libcef.so
bin/weweb/locales/
```
