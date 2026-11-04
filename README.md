# HDR+ Grade (Dusklight mod)

A Special K-style SDR-to-HDR grade for Dusklight. It applies a perceptual and color boost, saturation and gamut expansion. Optionally, it restores lighting that the GameCube clipped, plus glow, using the engine's HDR side buffers.

It needs the **[Dusklight HDR fork](https://github.com/KevinX8/dusklight)**: official Dusklight has no HDR output yet, and this mod does nothing without it.

## Install

1. Install the HDR engine from the **[fork release](https://github.com/KevinX8/dusklight/releases/tag/v2.0.3-hdr.1)** (see the [fork README](https://github.com/KevinX8/dusklight#readme) for platform notes).
2. Download `hdr_mod.dusk` from the **[latest release](https://github.com/KevinX8/dusklight-hdr-mod/releases/latest)**. One file covers every platform.
3. Copy it into your mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`
   - Android / iOS: the `mods` folder in Dusklight's data directory.
4. Set **Graphics > HDR Output** to HDR10 or scRGB and restart. Your display or OS HDR mode must be on.

The grade's settings (paper white, peak, boost, saturation, gamut, unclip lighting, glow) are in the mod's **HDR+ Grade** panel in the Mods tab.

## Platforms

| Platform | HDR modes |
|---|---|
| Linux (x86_64, aarch64) | HDR10, scRGB |
| Windows (x86_64, arm64) | HDR10, scRGB |
| Android (arm64) | HDR10, scRGB |
| macOS (arm64, x86_64), iOS | scRGB (EDR) |

Tested on Linux (Legion Go 2). Windows, Android and iOS are in testing; macOS is untested.

## Engine requirement

This mod depends on engine changes that are not upstream:

- [KevinX8/dusklight `hdr`](https://github.com/KevinX8/dusklight/tree/hdr): HDR output setting, GfxService 1.5 (HDR frame, scene texture, gain/glow side buffers).
- [KevinX8/aurora `hdr`](https://github.com/KevinX8/aurora/tree/hdr): HDR swapchain on Vulkan/D3D12/Metal, float present path, lighting gain/glow buffers.

The `hdr-fork` branch of the dusklight fork points the submodules at these forks, so it clones with `--recursive`. CI builds against that branch.

## Build

```sh
cmake -B build -G Ninja                                   # fetches the pinned fork
cmake -B build -DDUSKLIGHT_DIR=/path/to/dusklight        # or use a local hdr checkout
cmake --build build                                       # -> build/mods/hdr_mod.dusk
```

CI builds every platform and merges them into one `hdr_mod.dusk`; pushing a tag publishes it as a release.
