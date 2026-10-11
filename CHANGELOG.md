# Changelog

## [1.21.0] — 2026-10-11

### Breaking change: Noctalia v5 migration

> [!WARNING]
> The bundled Noctalia launcher now requires **Noctalia v5, plugin API 24 or later** (v5.0.0-beta.9+). The v4 QML plugin has been replaced with Luau and **will not work on Noctalia v4**. Existing v4 widget placement and settings are not automatically migrated.

- Upgrade Noctalia before enabling the new plugin. If you need the bundled v4 plugin, stay on [Nirisnap v0.3.0](https://github.com/itc-steve/nirisnap/releases/tag/v0.3.0) until you migrate.
- Follow the [v5 installation and discovery instructions](https://github.com/itc-steve/nirisnap/blob/v1.21.0/README.md#install-on-cachyos-niri--noctalia). Register the plugin through **one** discovery method, reload Noctalia, and enable `itc-steve/nirisnap`.
- Add `itc-steve/nirisnap:capture` to the bar and reselect your capture mode, output mode, and icon color in the plugin settings. Remove any obsolete v4 widget entry through the shell's settings.
- The native Nirisnap executable and direct Niri keybind remain independent of Noctalia. The installer does not edit Niri/Noctalia configuration or migrate settings, and still refuses to overwrite an existing installation; see the README's existing-install guidance for plugin discovery.

### Version alignment

The jump from **0.3.0 to 1.21.0 is intentional**: the native application version now records this maintenance pass against Omasnap's 1.21.0 release line. Upstream was reviewed through [`6771b19`](https://github.com/omacom/omasnap/commit/6771b19de8bcead2b17c16b49f1a8a97445dd8e3). This is **not full upstream feature parity** or a bulk merge. The separately versioned Noctalia plugin is `2.0.0` for its v5 migration.

### Fixed

- Capture monitor pixels before showing the overlay, preventing self-capture of its dim layer. A second invocation cancels pending monitor capture; Escape closes the visible overlay.
- Round cutout strip boundaries outward to native pixels, including fractional-scale captures, while preserving secure redaction, alpha, and undo/redo.
- Prioritize selected spotlight handles and allow existing selected layers to be moved/resized outside the image without jumping on first movement. New annotations still cannot start outside the image.
- Ignore stale Ctrl/Shift modifiers left over from the launch shortcut until a fresh key event.
- Preserve the previous tool and layer selection after eyedropper sampling.
- Keep the source image anchored during crop drags, then refit on release.
- Match backdrop preview margins, rounded corners, and shadows to exported images without changing native export pixels.
- Limit Tesseract to one OpenMP thread in its child process only, preserving OCR language validation and the parent environment.
- Support absolute `BUILD_DIR` paths in the smoke target.

### Verification and compatibility

- Expanded synthetic startup, interaction, cutout, redaction, OCR, crop, and backdrop regression coverage; smoke notifications no longer reach the live desktop.
- Run binary-dependent contract tests after compilation in CI rather than silently skipping them on a clean checkout.
- Preserve Niri JSON IPC, asynchronous native window capture with no monitor-crop fallback, layer-shell pins, custom cutout/Open-in-default-app behavior, immediate Escape, and private runtime recovery.
- No persistent recent-capture shelf, default autosave, Hyprland integration, or replacement pin behavior is enabled.
- Build, expanded Qt smoke, 18 Python tests, Noctalia plugin lint, installer preflight, and staged installation were verified. Maintainer confirmed live capture/cancellation, cutout/undo, and crop/backdrop/Open flows. Clang-tidy completed with advisory warnings; clazy was unavailable locally.

[1.21.0]: https://github.com/itc-steve/nirisnap/compare/v0.3.0...v1.21.0
