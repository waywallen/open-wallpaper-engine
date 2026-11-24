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
| Wayland client, wayland-scanner, X11, XCB, XCB RandR, xkbcommon | - | Linux vui-host window backends |
| liblz4 | - | `.pkg` decompression |
| OpenAL Soft | - | Scene spatial audio; |

The complete release environment is declared in `environment.yml`.

## Build

The default workspace package, `owe-cli`, builds the Scene and Web binaries:

```bash
lito build --profile release
```

Build an individual binary when only one surface is needed:

```bash
lito build -p owe-cli --target owe-scene --profile release
lito build -p owe-cli --target owe-web --profile release
```

Release binaries are written under:

```text
build/release/bin/owe-cli/owe-scene
build/release/bin/owe-cli/owe-web
```

With no mode option, these binaries open a native preview using vui-host. Use
`OWE_WINDOW_BACKEND=x11` or `OWE_WINDOW_BACKEND=wayland` to select a Linux
window backend. `WP_HEADLESS=1` and `--screenshot` run the Scene CLI without
opening a window. Web preview uses the Vulkan presenter.

## Plugin bundle

The release helper builds and installs `waywallen-bridge`, invokes lito for the OWE
plugin, and packages:

```bash
./scripts/build_waywallen_plugin.sh
```

With the bridge already installed, build and install the local plugin with:

```bash
lito install -p owe-waywallen-plugin -p owe-cli \
  --features waywallen --prefix ../install --profile debug
```

The plugin prepends `--waywallen` to select the bridge instead of opening a
preview window. The option requires the `waywallen` build feature. The daemon
must support fixed renderer `args` in the plugin manifest.

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
