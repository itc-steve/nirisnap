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

Target stack: **Niri** and **Noctalia v5**, plugin API **24** or later (tested offline with v5.2.1).
The Noctalia v4 QML plugin is removed; previous revisions remain in Git history.
The native binary, build, and direct Niri keybind remain independent of Noctalia.

You need:

- `niri` and `noctalia` on `PATH`
- `~/.local/bin` on your desktop session `PATH`
- Noctalia v5 supporting plugin API 24 (v5.0.0-beta.9 or later)

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
It does not edit Noctalia config/state or Niri configuration, reload the shell, or enable the plugin.

Packages it can install:

```text
base-devel cmake ninja pkgconf qt6-base layer-shell-qt grim
wl-clipboard tesseract tesseract-data-eng libnotify
```

Set `NIRISNAP_PREFIX` to use a prefix other than `~/.local`.

### 2. Discover and enable the v5 plugin

The installer links `plugin/nirisnap` under `$XDG_DATA_HOME/noctalia/plugins/nirisnap`
(default `~/.local/share/noctalia/plugins/nirisnap`). Installed does not mean enabled:

```bash
noctalia msg config-reload
noctalia msg plugins enable itc-steve/nirisnap
noctalia msg plugins list
```

For an existing binary install, skip the installer (it refuses overwrites) and add this checkout as a path source instead:

```bash
noctalia plugins lint ./plugin/nirisnap
noctalia msg plugins source add nirisnap-dev path "$PWD/plugin"
noctalia msg config-reload
noctalia msg plugins enable itc-steve/nirisnap
```

Use one discovery method, not both. No Quickshell or v4 packages are needed.

### 3. Add the bar widget

1. Open **Noctalia Settings → Bar → Widgets**.
2. Add **Nirisnap**, entry `itc-steve/nirisnap:capture`.

The capture panel is `itc-steve/nirisnap:panel`. Shared typed settings live under
**Settings → Plugins → Nirisnap → gear**. No separate settings script is needed.

Bar controls:

| Input | Action |
|---|---|
| Left click | Start configured default capture |
| Middle click | Open capture panel |
| Right click | Open plugin settings |

Panel actions: Region, Window, Full monitor, and Edit clipboard image.
Settings choose default capture (region/window/fullscreen), output (editor/copy/save/copy+save),
and theme-role icon color. V4 settings are not automatically imported; reselect them in v5.
Clipboard always opens the editor. Captures close the panel first, then launch after a 200 ms
unmap grace; slow compositor animations may need a longer grace. Other open shell surfaces
are not forcibly dismissed. No screenshot or clipboard contents are read by the plugin.

IPC from a terminal or a Niri bind (capture widget must be placed on the target output):

```bash
noctalia msg plugin itc-steve/nirisnap:capture focused toggle
noctalia msg plugin itc-steve/nirisnap:capture focused region
noctalia msg plugin itc-steve/nirisnap:capture focused window
noctalia msg plugin itc-steve/nirisnap:capture focused fullscreen
noctalia msg plugin itc-steve/nirisnap:capture focused clipboard
noctalia msg panel-toggle itc-steve/nirisnap:panel
```

The plugin launches fixed allowlisted argv through a detached delay helper, never `sh -c`.
Missing binary/launch acceptance failures show notifications; later capture/editor errors
remain the native binary's responsibility. No daemon or polling of captures is added.
Mosaic/solid redaction, pins, OCR, backdrops, and single-instance toggle stay in the native
editor; quick-output flags skip that editor. Noctalia's built-in screenshots do not replace
these extras.

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

Full local gate (requires Noctalia v5 for manifest/config lint and `lua` for offline
execution of the scripts' Lua-compatible subset):

```bash
make check
```

Plugin-only checks, without a running shell or captures:

```bash
make plugin-lint
python3 -m unittest discover -s tests -p 'test_plugin_contract.py' -v
```

Plugin API reference: [Noctalia v5 development](https://docs.noctalia.dev/noctalia/plugins/development/).
Offline tests stub host UI/process APIs; live rendering, focus, unmap timing, and capture flows
still need checking in a desktop session.

## Attribution

Nirisnap is adapted from [Tobi Lütke's Omasnap](https://github.com/tobi/omasnap) under the MIT License. Original copyright and license text remain in [`LICENSE`](LICENSE).

The workflow also draws inspiration from [Shottr](https://shottr.cc/), [Satty](https://github.com/Satty-org/Satty), and [Flameshot](https://github.com/flameshot-org/flameshot).

Bundled Neucha font is distributed under the SIL Open Font License in [`assets/OFL.txt`](assets/OFL.txt).
