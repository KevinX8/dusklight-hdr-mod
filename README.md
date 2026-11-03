# HDR+ Grade (Dusklight mod)

A Special K-style SDR-to-HDR grade for Dusklight. It applies a perceptual and color boost, saturation and gamut expansion. Optionally, it restores lighting that the GameCube clipped, plus glow, using the engine's HDR side buffers.

**Linux only for now.** It needs HDR output, which requires the Vulkan backend and an HDR display. Enable **HDR Output** in Graphics settings.

## Engine requirement

This mod depends on engine changes that are not upstream yet:

- [KevinX8/dusklight `hdr`](https://github.com/KevinX8/dusklight/tree/hdr): HDR output setting, GfxService 1.5 (HDR frame, scene texture, gain/glow side buffers).
- [KevinX8/aurora `hdr`](https://github.com/KevinX8/aurora/tree/hdr): HDR swapchain, float present path, lighting gain/glow buffers.
- [KevinX8/borealis `hdr`](https://github.com/KevinX8/borealis/tree/hdr): version override for the HDR build.

The `hdr-fork` branch of the dusklight fork adds one commit that points the submodules at these forks. That makes it clone with `--recursive`. CI builds against that branch.

## Build

```sh
cmake -B build -G Ninja                                   # fetches the pinned fork
cmake -B build -DDUSKLIGHT_DIR=/path/to/dusklight        # or use a local hdr checkout
cmake --build build                                       # -> build/mods/hdr_mod.dusk
```
