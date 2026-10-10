# Nirisnap

Native Wayland screenshot capture and annotation for **Niri**, **Noctalia Shell**, and **CachyOS/Arch**.

## Features

- Freeform region, Niri-native window, and full-monitor capture.
- Movable and resizable vector annotations: arrows, lines, freehand, highlighter, rectangles, numbered markers, text, and spotlight/loupe.
- Secure solid or randomized non-spatial mosaic redaction.
- Per-layer colors, undo/redo, OCR region capture, gradient backdrops, and rendered shadows.
- External recrop handles and native-pixel export on fractional-scale monitors.
- Copy, save, or copy-and-save output; direct quick-output CLI modes.
- Open an existing file or Wayland clipboard image in the editor.
- Pin finished captures as draggable always-visible layer surfaces.
- Private crash-recovery snapshots under `$XDG_RUNTIME_DIR/nirisnap` with a secure `/tmp/nirisnap-$UID` fallback.
- Single-instance overlay toggle: press once to open, again to dismiss.
- Noctalia bar widget, capture panel, settings, and IPC methods.

## How capture works

Nirisnap reads focused-output, workspace, and window metadata from Niri's stable JSON IPC. `grim` captures focused-output pixels before Nirisnap maps its layer-shell overlay.

`--capture-window` uses `niri msg pick-window`, then asks Niri to render that exact window with `screenshot-window --id`. It never substitutes a desktop crop when native window capture fails.

> **Window-picker cancellation:** press `Escape` to cancel Niri's picker. Niri 26.04 exposes no IPC action to cancel an active picker, so invoking Nirisnap again cannot dismiss the crosshair by itself; the original process stays attached until you press `Escape` or click.

> **Niri 26.04 clipboard behavior:** Niri always places its raw native window screenshot on the clipboard before Nirisnap opens the editor. Niri exposes no flag to disable this. Copying from Nirisnap later replaces it with the annotated image; cancelling after window selection leaves the raw window image on the clipboard.

## Install on CachyOS Niri + Noctalia

Target stack is the default CachyOS Niri desktop: **Niri**, **Noctalia Shell 4.x** via `qs -c noctalia-shell`, and **noctalia-qs**.

You need:

- `niri` and `qs` on `PATH`
- `~/.local/bin` on `PATH` (CachyOS user sessions usually have this)
- Noctalia Shell 4.4.3 or later (CachyOS `noctalia-shell`)

### 1. Build and install Nirisnap

```bash
git clone https://github.com/itc-steve/nirisnap.git
cd nirisnap
./packaging/install-local.sh --check   # dry run; no writes
./packaging/install-local.sh
```

The installer runs as your desktop user. It asks `sudo` only for packages.
It builds Nirisnap, runs the offscreen smoke suite, then installs under `~/.local`.
It links the Noctalia plugin and copies a Niri keybind snippet.
It will not replace an existing Nirisnap install or plugin path.
It does not edit Noctalia `settings.json` or Niri configuration.

Packages it can install:

```text
base-devel cmake ninja pkgconf qt6-base layer-shell-qt grim
wl-clipboard tesseract tesseract-data-eng libnotify
```

Set `NIRISNAP_PREFIX` to use a prefix other than `~/.local`.

### 2. Confirm `qs` starts

```bash
qs --version
```

If that prints a version, restart Noctalia:

```bash
qs kill -c noctalia-shell
qs -d -c noctalia-shell
```

If `qs` dies with `undefined symbol` … `Qt_6_PRIVATE_API`, the CachyOS `noctalia-qs` package is older than your Qt. Rebuild it. See [If qs fails after a Qt upgrade](#if-qs-fails-after-a-qt-upgrade).

### 3. Add the bar widget

1. Open **Noctalia Settings → Bar → Widgets**.
2. Add **Nirisnap**.

The plugin matches CachyOS Noctalia 4.x (`minNoctaliaVersion` 4.4.3).

Bar controls:

| Input | Action |
|---|---|
| Left click | Start configured default capture |
| Middle click | Open capture panel |
| Right click | Open plugin settings |

Panel actions: Region, Window, Full monitor, and Edit clipboard image.
Settings choose default capture, output behavior, and icon color.

IPC from a terminal or a Niri bind:

```bash
qs -c noctalia-shell ipc call plugin:nirisnap toggle
qs -c noctalia-shell ipc call plugin:nirisnap region
qs -c noctalia-shell ipc call plugin:nirisnap window
qs -c noctalia-shell ipc call plugin:nirisnap fullscreen
qs -c noctalia-shell ipc call plugin:nirisnap clipboard
```

### 4. Bind Ctrl+Shift+1 (required)

The installer does **not** edit Niri config. You must replace CachyOS's stock screenshot bind yourself.

CachyOS Niri puts this in `~/.config/niri/cfg/keybinds.kdl`:

```kdl
    CTRL+Shift+1                        { screenshot; }
```

Replace that line with:

```kdl
    CTRL+Shift+1 repeat=false hotkey-overlay-title="Screenshot: Nirisnap" { spawn "nirisnap" "--capture-region"; }
```

Leave `CTRL+Shift+2` and `CTRL+Shift+3` as Niri screen and window shots unless you want those on Nirisnap too.

Then validate:

```bash
niri validate
```

Niri hot-reloads a valid config. Press `Ctrl+Shift+1` to open the region overlay.

If the key does nothing, Niri cannot find `nirisnap` on `PATH`. Put `~/.local/bin` on `PATH`, or spawn the full path `~/.local/bin/nirisnap`.

Do not also `include` the copied snippet while the same key remains in `keybinds.kdl`. That is a duplicate bind. The file at `~/.local/share/nirisnap/nirisnap-keybinds.kdl` is a reference copy of the replacement line.

### If `qs` fails after a Qt upgrade

`noctalia-qs` links Qt **private** APIs. A `qt6-base` / `qt6-declarative` upgrade can break `/usr/bin/qs` until CachyOS rebuilds the package.

Symptom:

```text
qs: symbol lookup error: qs: undefined symbol: … version Qt_6_PRIVATE_API
```

`sudo pacman -S noctalia-qs` reinstalls the same broken binary. Do not do that until the repo version is newer than your Qt.

Rebuild the tagged `noctalia-qs` that CachyOS ships (today: `v0.0.12`) against the installed Qt:

```bash
sudo pacman -S --needed cli11 wayland-protocols vulkan-headers cmake ninja pkgconf \
  qt6-base qt6-declarative qt6-wayland qt6-svg qt6-shadertools jemalloc

git clone --depth 1 --branch v0.0.12 --recurse-submodules \
  https://github.com/noctalia-dev/noctalia-qs.git
cd noctalia-qs

cmake -GNinja -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DINSTALL_QML_PREFIX=lib/qt6/qml \
  -DDISTRIBUTOR="CachyOS local Qt rebuild" \
  -DUSE_JEMALLOC=ON \
  -DCRASH_HANDLER=OFF

cmake --build build --parallel
sudo cmake --install build
qs --version
qs kill -c noctalia-shell
qs -d -c noctalia-shell
```

This overwrites `/usr/bin/qs`. A later `pacman -S noctalia-qs` from an unfixed repo will restore the broken binary. Rebuild again, or wait for CachyOS to publish a package built against the new Qt.

## CLI

Default region capture:

```bash
nirisnap
nirisnap --capture-region
```

Native window picker and full focused monitor:

```bash
nirisnap --capture-window
nirisnap --capture-fullscreen
```

Positional capture names remain available:

```bash
nirisnap region
nirisnap windows
nirisnap fullscreen
nirisnap smart
```

Quick output skips the annotation editor:

```bash
nirisnap --capture-region --copy
nirisnap --capture-window --save
nirisnap --capture-fullscreen --copy --save
```

Edit a file or clipboard image:

```bash
nirisnap ~/Pictures/Screenshots/capture.png
nirisnap --file /path/to/capture.png
nirisnap --clipboard
```

Environment overrides:

```bash
NIRISNAP_SCREENSHOT_DIR="$HOME/Pictures/Captures" nirisnap
NIRISNAP_OCR_LANGS="eng+deu" nirisnap
```

OCR language values use three-letter Tesseract codes joined by `+`. Install matching Tesseract language packages first.

Exit codes:

| Code | Meaning |
|---|---|
| `0` | Success, including dismissing an open overlay |
| `1` | Capture, image, or single-instance failure |
| `2` | CLI usage error |

## Controls

### Capture selection

| Input | Action |
|---|---|
| Drag | Select region |
| `Space` | Toggle region/floating-window selection when Niri reports window geometry |
| `Super` + Arrow | Move among selectable floating windows |
| `Enter` | Capture highlighted window |
| `Ctrl+A` | Select full focused monitor |
| `Esc` | Dismiss |

Use `--capture-window` or Noctalia's **Window** action for Niri's native tiled-window picker.

### Annotation editor

| Input | Action |
|---|---|
| `V` | Select/move/resize; wheel scales selected layer |
| `A` | Arrow |
| `S` | Spotlight/loupe; press again to cycle shape |
| `L` | Straight line |
| `F` | Freehand stroke |
| `I` | Eyedropper from color popover |
| `C` | Numbered marker |
| `R` | Rectangle |
| `D` | Redact; press again to toggle mosaic/solid |
| `T` | Text |
| `X` / Cutout toolbar button | Remove inner strip and join remaining sides |
| `O` | OCR selected region to clipboard |
| `B` | Cycle backdrop |
| `1`–`6` | Set annotation color |
| Wheel | Scale selection, spotlight magnification, or active tool size |
| `Shift` + drag | Square shapes or 45° line/arrow snapping |
| Double-click text | Reopen text editor |
| `Delete` | Delete selected layer |
| `Ctrl+Z` | Undo |
| `Ctrl+Shift+Z`, `Ctrl+Y` | Redo |
| `Ctrl+C` | Copy PNG |
| `Ctrl+S` | Save PNG |
| Open in default image app toolbar button (after Save) | Save current PNG and open system-associated image app |
| `Enter` | Copy and save |
| `P` | Pin capture and close editor |
| `Esc` | Close overlay immediately, including text, popovers, and active selection/drawing |
| Right click | Return to Select or cancel drawing |

**Cutout:** select a capture, then click the Cutout toolbar icon (two retained sides, crossed-out strip, and inward join arrows) or press `X`. Drag **vertically** between the top and bottom edges of a full-width horizontal strip; drag **horizontally** between the left and right edges of a full-height vertical strip. The larger drag direction chooses the axis (ties choose horizontal). The red preview marks the entire strip to remove. Release to delete it and join the remaining sides; right-click cancels the drag. Keep some image on both sides: empty, edge-only, and whole-image cuts are rejected. No automatic whitespace detection. Press `V` to return to existing outer crop handles.

Cutout snaps strip boundaries to native pixels without downscaling. It flattens existing annotations and secure redactions into the joined image, **excluding the decorative backdrop**; the backdrop remains separately selectable. Status confirms flattening. `Ctrl+Z` restores the original source and editable layers; `Ctrl+Shift+Z` or `Ctrl+Y` reapplies the cut. New annotations remain editable.

**Open in default image app:** click the image-with-outward-arrow button after Save. Nirisnap saves the current full-resolution crop, annotations, secure redactions, and backdrop to the normal screenshot directory, then starts `xdg-open` with that durable PNG before closing the overlay. Clipboard stays unchanged. If the launcher cannot start, the overlay stays open and reports the saved path. The PNG survives closing Nirisnap either way. The system PNG association chooses the app (for example Pinta); Nirisnap neither hardcodes an app nor changes associations. Launcher startup cannot confirm that the external app ultimately opened the file. External apps do not inherit Nirisnap's layer-shell setting.

### Pinned captures

| Input | Action |
|---|---|
| Drag image | Move pin |
| Wheel | Resize while preserving aspect ratio |
| Edit button | Reopen full-resolution PNG in Nirisnap |
| Link button | Copy source path |
| Copy button, `Ctrl+C` | Copy full-resolution PNG |
| Drag handle | Drag PNG into a file-capable target |
| Close button, `Esc`, middle click | Close pin |

Pins live on every workspace through layer-shell; no Niri window rule is required.

## Development

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
QT_QPA_PLATFORM=offscreen ./build/nirisnap-smoke /tmp/nirisnap-smoke
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

Full local gate:

```bash
make check
```

## Attribution

Nirisnap is adapted from [Tobi Lütke's Omasnap](https://github.com/tobi/omasnap) under the MIT License. Original copyright and license text remain in [`LICENSE`](LICENSE).

The workflow also draws inspiration from [Shottr](https://shottr.cc/), [Satty](https://github.com/Satty-org/Satty), and [Flameshot](https://github.com/flameshot-org/flameshot).

Bundled Neucha font is distributed under the SIL Open Font License in [`assets/OFL.txt`](assets/OFL.txt).
