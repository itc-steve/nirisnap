# Nirisnap agent guide

Nirisnap is a native Wayland screenshot and annotation overlay for Niri, Noctalia Shell, and CachyOS/Arch.

## Principles

- Speed first: capture, annotate, output.
- Niri only: stable `niri msg -j` IPC plus Niri compositor screenshot actions.
- No compatibility layer for other compositors.
- Keep one native executable; Noctalia is a thin launcher.
- Keep private screenshots private: fail closed, use user-only runtime paths, and preserve secure redaction.
- Prefer existing Qt/stdlib/platform code. No daemon, portal abstraction, or QML editor rewrite.

## Layout

| Path | Purpose |
|---|---|
| `src/main.cpp` | CLI, single-instance mode dispatch, layer-shell setup |
| `src/capture.cpp/.hpp` | Niri discovery, `grim`, rendering, OCR, clipboard, notifications |
| `src/surface-capture.cpp` | Niri-native window screenshot action |
| `src/editor.cpp/.hpp` | Annotation editor and capture selection |
| `src/pin.cpp/.hpp` | Always-visible pinned capture surfaces |
| `tests/` | Offscreen Qt smoke and stdlib contract checks |
| `plugin/nirisnap/` | Noctalia 4.x plugin |
| `packaging/` | CachyOS installer and Ctrl+Shift+1 Niri keybind snippet |

## Commands

```bash
make build
make smoke
make check
python3 -m unittest discover -s tests -p 'test_*.py' -v
./packaging/install-local.sh --check
```

Run behavior checks after each change. Do not rerun an unchanged successful check.

## Hard constraints

- Use JSON IPC. Human `niri msg` output is unstable.
- Niri logical output geometry is already transformed; do not apply another rotation.
- `screenshot-window --id --path` writes PNG asynchronously after `niri msg` exits. Success requires a valid file, not exit code 0.
- Niri always replaces clipboard with raw native window image. Document it; never fake restoration.
- Never crop monitor pixels as fallback for failed native window capture.
- Never pass external values through `sh -c`.
- Noctalia plugin arguments are fixed allowlisted arrays.
- Installer must not edit live Niri configuration or Noctalia `settings.json`.
- Preserve `LICENSE` and `assets/OFL.txt`.

## Release

Version lives in `CMakeLists.txt`. Before tagging `vX.Y.Z`, run `make check`, installer/unit checks, and inspect staged CMake installation. CI creates an Arch Linux artifact for tags containing the binary, desktop file, licenses, Noctalia plugin, and optional Niri keybind snippet.
