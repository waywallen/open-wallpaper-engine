# Open Wallpaper Engine
Made this for fun.

## Scene Renderer
Open source scene renderer, mostly for linux.  

- vulkan 1.1
- render graph for automatic pass dependencies

## Web Renderer
CEF-based host for web wallpapers.

## How to use

### Waywallen plugin
For Waywallen's Flatpak and AppImage builds, download the
`open-wallpaper-engine-*.zip` for your architecture from the
[Releases page](https://github.com/waywallen/open-wallpaper-engine/releases) and
install it from Waywallen's Plugins page.

If you built Waywallen yourself, [build and install the plugin](BUILD.md#plugin-bundle).
If you use a Linux distribution package, install its matching plugin package.

### Standalone viewer
Extract the prebuilt ZIP and run the commands below from the extracted
directory. The bundle contains the same binaries for standalone preview and
Waywallen integration. For source builds, see [BUILD.md](BUILD.md).

`owe-scene` loads a Wallpaper Engine `scene.pkg` directly. Pass the
Wallpaper Engine `assets/` directory and the scene file:

```bash
./bin/owe-scene \
    "/path/to/steamapps/common/wallpaper_engine/assets" \
    "/path/to/steamapps/workshop/content/431960/<id>/scene.pkg"
```

Flags (see `owe-scene --help`):

| Flag | Default | Description |
|------|---------|-------------|
| `-R, --resolution WxH` | `1280x720` | window size, e.g. `1920x1080` |
| `-f, --fps N`          | `15`       | render fps |
| `-M, --msaa N`         | `1`        | MSAA samples for the screen RT (1/2/4/8/16; `1` = off) |
| `-C, --cache-path DIR` | XDG cache  | shader cache directory |
| `-V, --valid-layer`    | off        | enable Vulkan validation layers |
| `-G, --graphviz`       | off        | dump render graph to `graph.dot` |


`owe-web` is the CEF-backed HTML/JS counterpart and uses Vulkan for presentation.
Keep the bundled CEF runtime alongside the executable:

```bash
./bin/weweb/owe-web \
    "/path/to/steamapps/workshop/content/431960/<id>" \
    --width 1920 --height 1080
```

## Feature list

- [x] Layer
	- [x] Image
	- [x] Composition / Fullscreen
	- [x] Text
- [x] Effect
    - [x] Basic
	- [x] Mouse position with delay
	- [x] Parallax
    - [x] Depth Parallax
	- [x] ColorBlendMode
	- [x] PBR light
	- [x] Global bloom
- [x] Camera
	- [x] Zoom
	- [x] Shake
	- [x] Path
- [x] Audio
	- [x] Loop
	- [x] Random
	- [x] Visualization
- [x] Particle System
	- [x] Renderers
	- [x] Emitters
	- [x] Initializers
	- [x] Operators
	- [x] Control Points
        - [x] Mouse Follow
	- [x] Children
    - [x] Audio Response
- [x] Puppet
    - [x] Mesh
    - [x] Skeleton
    - [x] Animation
	- [x] Attachments (MDAT)
	- [x] Mesh masks
	- [x] Interactive
    - [ ] Bone simulation
	- [ ] Shape Animations
- [X] 3D model
- [x] Scenescript  
- [x] User Properties

## License

[GPL-2.0-only](LICENSE)
