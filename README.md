# NootedGreen

Lilu plugin for Intel iGPU acceleration on macOS — Haswell through Raptor Lake, via Tiger Lake driver spoofing.

## What it does

Patches Apple's Tiger Lake (Gen12) graphics drivers to work with newer Intel iGPUs. Handles device-id spoofing, MMIO addressing, ForceWake, GPU topology, display controller init, combo PHY calibration, and GT workarounds.

## Status

**Work in progress.** Framebuffer controller starts, combo PHY calibration patched, accelerator ring initialises, host-based scheduler (type 5) active with RCS ring running on RPL. Display pipeline working (eDP trained, cursor visible). WindowServer connects successfully — 12 user clients created (2x IGAccel2DContext, 2x IOAccelDisplayPipeUserClient2, 8x IGAccelSurface). DYLD patches hook `_cs_validate_page` early (before DeviceInfo) to ensure CoreDisplay is patched before WindowServer starts. Metal rendering enabled by default; V50 patches `gpu_bundle_find_trusted()` in libsystem_sandbox.dylib to redirect GPU bundle search from `/Library/GPUBundles` → `/Library/Extensions/` where the TGL driver actually lives (Apple never shipped a TGL Mac). Alternative: manually `sudo cp -R /Library/Extensions/AppleIntelTGLGraphicsMTLDriver.bundle /Library/GPUBundles/`. ICL Metal driver device-ID bypass uses mask-based matching for build portability. V52 adds CPUID-based cross-platform detection (`isRealTGL`) — RPL-specific patches (topology overrides, GuC stub, BCS reset, ForceWake override) are automatically skipped on genuine Tiger Lake hardware. GPU command submission under active development.

## Requirements

- [Lilu](https://github.com/acidanthera/Lilu) 1.7.2+
- macOS Sonoma 14.x (Gen11/Gen12 targets) or macOS 10.12–14.x (legacy NootedBlue targets)
- Supported Intel iGPU (see **Compatibility** below)
- Discrete GPU disabled via SSDT (recommended) or `disable-gpu` DeviceProperty on its PCI path

## Boot args

```
-v keepsyms=1 debug=0x100 -liludbg liludump=60 -NGreenDebug -disablegfxfirmware
```

| Arg | Purpose |
|---|---|
| `-NGreenDebug` | Enable NootedGreen debug logging |
| `-disablegfxfirmware` | Disable GuC/HuC firmware loading — required on RPL/ADL because scheduler selection (`ngreenSched`) happens after the HW-branch `processKext`, so the driver attempts firmware init before NootedGreen can override the scheduler type. Not needed on real TGL (GuC loads natively). |
| `ngreenSched=N` | Select GPU scheduler type: `3` = GuC firmware, `4` = IGScheduler4, `5` = host preemptive (default: `3` on real TGL, `5` on RPL/ADL) |
| `ngreen-dmc=skip` | Skip DMC firmware |
| `-allow3d` | Force 3D acceleration |
| `-ngreenNoMetal` | Disable Metal rendering — stub out CoreDisplay Metal paths to prevent NULL MTLDevice crashes (display-only debug mode) |
| `-ngreenAllowMetal` | Legacy flag (backward compat) — forces Metal ON, equivalent to not setting `-ngreenNoMetal` |
| `-nbdyldoff` | **Disable ALL DYLD patches** (CoreDisplay, OpenGL, Metal, SkyLight) — debug only |
| `IGLogLevel=8` | Maximum Intel GPU driver logging |
| `-liludbg` | Enable Lilu debug logging |
| `liludump=60` | Dump Lilu logs after 60 seconds |

## GPU Scheduler

The Intel TGL graphics driver supports three scheduler types, selectable at boot:

| Type | Name | Description |
|------|------|-------------|
| 3 | **GuC firmware** | Default Apple scheduler — loads GuC binary firmware. Requires matching firmware blobs. |
| 4 | **IGScheduler4** | Intermediate scheduler. |
| 5 | **Host preemptive** | Host-based scheduler — no firmware required. Ring command streamer managed by the driver. **Recommended for unsupported hardware.** |

**Selection priority:**

1. Boot argument `ngreenSched=N` (highest priority)
2. `SchedulerType` key in Info.plist (NootedGreen personality)
3. Default: `3` (GuC firmware) on real TGL, `5` (host preemptive) on RPL/ADL

Type 5 bypasses `IGScheduler::initFirmware()` entirely, avoiding GuC/HuC binary loading which fails on spoofed devices. The RCS ring is initialized directly by the host driver.

## Cross-Platform Detection (V52)

NootedGreen uses inline CPUID (`EAX=1`) at load time to read the real CPU model — no config.plist or device-ID spoofing can fake this. The result sets `isRealTGL`:

| CPU | Model | `isRealTGL` |
|-----|-------|-------------|
| Tiger Lake-U (i7-1165G7, etc.) | `0x8C` | `true` |
| Tiger Lake-H (i7-11800H, etc.) | `0x8D` | `true` |
| Raptor Lake-P (i7-13700H, etc.) | `0xBA` | `false` |
| Raptor Lake-S | `0xBF` | `false` |
| Raptor Lake-HX | `0xB7` | `false` |
| Alder Lake-P | `0x9A` | `false` |
| Alder Lake-S | `0x97` | `false` |

When `isRealTGL = false` (RPL/ADL), the following RPL-specific patches are applied:

- **Topology overrides** — L3 bank count, max EU count, subslice count hardcoded for 96EU RPL config
- **BCS engine bypass** — skip blitter engine init in `hwDevStart` (RPL BCS is dead under TGL driver)
- **GuC binary stub** — `loadGuCBinary` returns 1 instead of loading firmware (wrong microarch)
- **MultiForceWakeSelect=1** — redirect ForceWake to hooked `SafeForceWakeMultithreaded` (RPL ACK=0 on native path)
- **BCS engine reset** — stop+clear dead BCS ring after `start()` (V51)

When `isRealTGL = true`, all of the above are skipped — the driver uses Apple's native topology, GuC firmware, ForceWake, and BCS engine as-is.

### Required GPU driver bundles

For GPU acceleration, the following userspace driver bundles must be installed in `/Library/Extensions/`:

| Bundle | Purpose |
|--------|---------|
| `AppleIntelTGLGraphicsMTLDriver.bundle` | Metal driver |
| `AppleIntelTGLGraphicsGLDriver.bundle` | OpenGL driver |
| `AppleIntelTGLGraphicsVADriver.bundle` | Video Acceleration driver |
| `AppleIntelTGLGraphicsVAME.bundle` | VA Media Engine |
| `AppleIntelGraphicsShared.bundle` | Shared graphics library |

These bundles are loaded by name (via `MetalPluginName`, `IOGLBundleName`, etc.), not by `CFBundleIdentifier`. They are not shipped with NootedGreen and must be sourced separately.

## Compatibility

NootedBlue legacy support (fully preserved):

- **Haswell** (10.12+)
- **Broadwell / Braswell** (10.14+)
- **Gemini Lake** (10.14+)

NootedGreen (Gen11/Gen12 — TGL driver spoofing):

- **Tiger Lake** (macOS Sonoma 14.x) — cross-platform safe (V52: RPL-specific patches auto-skipped via CPUID)
- **Raptor Lake-P** (macOS Sonoma 14.x) — work in progress (primary dev platform: i7-13700H)
- **Ice Lake** — untested, may work
- **Rocket Lake** — untested, may work
- **Alder Lake** — untested, may work

## Building

Open `NootedGreen.xcodeproj` and select the **NootedGreen** scheme to build the Gen11/Gen12 plugin, or one of the original NootedBlue schemes for legacy hardware. Build with Xcode.

## Authors

- **Stefano Giammori** ([@sgiammori](https://github.com/sgiammori)) — reverse engineering, driver development, hardware testing
- **Claude Sonnet** (Anthropic) — AI pair-programming, code generation, debug analysis
- **Claude Opus 4.6** via GitHub Copilot — AI pair-programming, code generation, debug analysis

## License

[Thou Shalt Not Profit License 1.0](LICENSE)
