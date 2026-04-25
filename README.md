# NootedGreen

Lilu plugin for Intel iGPU acceleration on macOS — Haswell through Raptor Lake, via Tiger Lake driver spoofing.

## What it does

Patches Apple's Tiger Lake (Gen12) graphics drivers to work with newer Intel iGPUs. Handles device-id spoofing, MMIO addressing, ForceWake, GPU topology, display controller init, combo PHY calibration, and GT workarounds.

## Status

**Work in progress.** Framebuffer controller starts, combo PHY calibration is patched, accelerator ring initialises, and host-based scheduler (type 5) runs on RPL with sustained RCS activity. Login is reachable on current test setups, but stability is still under active tuning for full-Metal paths. DYLD patches hook `_cs_validate_page` early (before DeviceInfo) so CoreDisplay is patched before WindowServer starts. Metal remains enabled by default. V50 still patches `gpu_bundle_find_trusted()` in libsystem_sandbox.dylib to redirect GPU bundle search from `/Library/GPUBundles` to `/Library/Extensions/` where the TGL driver bundle is installed. ICL Metal driver device-ID bypass uses mask-based matching for build portability. V52 adds CPUID-based cross-platform detection (`isRealTGL`) so RPL-only patches are skipped on genuine Tiger Lake hardware.

### Important (Current Test State)

- **CRITICAL FIX (V75):** Removed post-`awaitPublishing` property override that was causing cursor corruption (TV static). Display now stable.
- Early IGPU identity detection is confirmed working: Lilu reads `AAPL,ig-platform-id` (`9A490000`) via OpenCore DeviceProperties during `DeviceInfo` scan.
- Platform-ID and device-ID injection now handled cleanly by config.plist only — NootedGreen no longer interferes mid-initialization.
- System boots to login screen reliably with no visual corruption or kernel panics.
- CoreDisplay DYLD path keeps assertion bypass always-on for Ventura+/Sonoma. Stage-3 safety stubs (RunFullDisplayPipe NULL guard, GetMTLTexture NULL stub, GetMTLCommandQueue NULL stub) are now applied only on non-real TGL unless full-MTL mode is forced.
- Blit3D init policy on spoofed paths is now conservative by default: Apple's original `IGHardwareBlit3DContext::initialize()` is **disabled unless explicitly opted in** with `-ngreenV69AllowOriginal`.
- `-ngreenV69AllowOriginal` is **diagnostic only**. It can still panic at `IGHardwareBlit3DContext::initialize + 0x4c` (`SecurityAgent`/IOAccel submit path) on unsupported setups.
- `DisplayPipeSupported` native path is now the default. Use `-ngreendp0` only for forced fallback testing.
- V77 display-pipe client termination is disabled by default (delay defaults to full monitor window).
- **V88 scanout fill remains opt-in** (`-ngreenv88`). Default boots do not paint diagnostic bars over normal UI layout.

### Recent Progress

- **DYLD compact baseline:** Assertion bypass remains baseline on Ventura+/Sonoma. Stage-3 safety stubs are conditional (non-real TGL default), with full-MTL override available via `-ngreenfullmtl`.
- **DisplayPipe defaults updated:** Native `DisplayPipeSupported` is default. `-ngreendp0` is explicit fallback.
- **V77 default policy updated:** kill delay default is full monitor window (no automatic client termination unless explicitly requested).
- **V96 connector state update:** `getOnlineInfo` forces online + changed for stronger hotplug/state propagation.
- **V98/V98T/V90L4:** Conservative eDP training and lane policy diagnostics for spoofed paths.
- **V110:** V59 delayed checks + V74 EMR enforcer run unconditionally on non-real TGL; V60 monitor remains opt-in via `-ngreenexp`.
- **V75:** **CRITICAL FIX** — Removed post-`awaitPublishing` property override race condition. Display output now stable and corruption-free.
- **V74:** Permanent EMR enforcer — 50ms timer runs indefinitely, keeping ERROR_GEN6 masked.
- **V73:** Fixed double-dereference crash in blit3D initialize hook.
- **V72:** EMR write interception on all MMIO write paths.
- **V111C:** For non-real TGL, original Blit3D initializer is no longer auto-selected in full-MTL mode; it is opt-in only via `-ngreenV69AllowOriginal`.

### Current Status

System can boot to login on current RPL test baselines. Current baseline is compact DYLD + native display-pipe defaults with minimal boot-arg requirements and conservative Blit3D init defaults. V88 visual fill is opt-in only (`-ngreenv88`) so default boots preserve normal Apple UI layout.

## Requirements

- [Lilu](https://github.com/acidanthera/Lilu) 1.7.2+
- macOS Sonoma 14.x (Gen11/Gen12 targets) or macOS 10.12–14.x (legacy NootedBlue targets)
- Supported Intel iGPU (see **Compatibility** below)
- Discrete GPU disabled via SSDT (recommended) or `disable-gpu` DeviceProperty on its PCI path

For current RPL test configuration, OpenCore `DeviceProperties` IGPU injection is required:

- `PciRoot(0x0)/Pci(0x2,0x0)`
- `AAPL,ig-platform-id` = `AABJmg==` (0x9A490000 in little-endian)
- `device-id` = `SZoAAA==` (0x9A490000 in little-endian)
- `force-online` = `AQAAAA==` (enable force-online WEG patch)
- `complete-modeset` = `AQAAAA==` (enable complete-modeset WEG patch)
- `rps-control` = `AQAAAA==` (enable rps-control WEG patch)
- `built-in` = `AA==`
- `AAPL,slot-name` = `built-in`
- `hda-gfx` = `onboard-1`

These properties are essential for correct platform identification and WEG coexistence mode on RPL with TGL driver spoof.

## Boot args

```
-v keepsyms=1 debug=0x100 IGLogLevel=8 -NGreenDebug -liludbg liludump=200 ngreen-dmc=skip -allow3d -disablegfxfirmware -ngreenexp (as optional to try full MTL path : -ngreenfullmtl)
```

| Arg | Purpose |
|---|---|
| `-NGreenDebug` | Enable NootedGreen debug logging |
| `-disablegfxfirmware` | Disable GuC/HuC firmware loading — required on RPL/ADL because scheduler selection (`ngreenSched`) happens after the HW-branch `processKext`, so the driver attempts firmware init before NootedGreen can override the scheduler type. Not needed on real TGL (GuC loads natively). |
| `ngreenSched=N` | Select GPU scheduler type: `3` = GuC firmware, `4` = IGScheduler4, `5` = host preemptive (default: `3` on real TGL, `5` on RPL/ADL) |
| `ngreen-dmc=skip` | Skip DMC firmware |
| `-allow3d` | Force 3D acceleration |
| `-nbdyldoff` | **Disable ALL DYLD patches** (CoreDisplay, OpenGL, Metal, SkyLight) — debug only |
| `-ngreenexp` / `ngreenexp=1` | Enable experimental runtime monitor/timer paths (V60 monitor and extra diagnostics) |
| `-ngreendp0` / `ngreendp0=1` | Force fallback mode: set `DisplayPipeSupported=0` in accelerator capabilities |
| `-ngreendp1` / `ngreendp1=1` | Explicitly keep native `DisplayPipeSupported` path (default behavior) |
| `ngreenV77DelayKill=N` | Delay V77 display-pipe client termination by `N` monitor iterations (`0..60`, default `60` = effectively disabled) |
| `-ngreenv88` / `ngreenv88=1` | Enable V88 scanout fill + plane toggle diagnostics (draws test bars/colors; off by default) |
| `-ngreenfullmtl` / `ngreenfullmtl=1` | Force full CoreDisplay Metal path on Ventura+/Sonoma by skipping Stage-3 NULL safety stubs (GetMTLTexture/GetMTLCommandQueue/RunFullDisplayPipe guard). This **does not** auto-enable Apple's original Blit3D initializer. |
| `-ngreenRefProbeF2` / `ngreenRefProbeF2=1` | Enable reference f2 osinfo patch probe on non-real TGL (diagnostic only) |
| `-ngreenV69AllowOriginal` | Opt in to Apple's original Blit3D initialize on non-real TGL when safety preconditions are met. **High risk / diagnostic only**; can panic on unsupported setups. |
| `-ngreenV69SkipOriginal` | Hard-disable Apple's original Blit3D initialize on non-real TGL, even if `-ngreenV69AllowOriginal` is present. |
| `ngreenLanes=1|2|4` | Override lane count used by non-real TGL computeLaneCount bypass (default 2 lanes) |
| `-ngreenforceprops` / `ngreenforceprops=1` | Enable legacy forced IGPU property injection (`AAPL,ig-platform-id`, `model`, `saved-config`, etc.). Disabled by default in compatibility-first mode. |
| `IGLogLevel=8` | Maximum Intel GPU driver logging |
| `-liludbg` | Enable Lilu debug logging |
| `liludump=60` | Dump Lilu logs after 60 seconds |

## Hookcase

Hookcase (change `AppleInteePortHal` and `AppleIntelPortHal` implementation):

```cpp
// Register selection by platform:
//   ICL  (AppleIntelFramebufferController path): 0xC4030 (ICL_SHOTPLUG_CTL_DDI)
//   TGL:                                         0x44470
//   ADL-P / RPL-P:                               0x1638a0
uint32_t registerValue = callback->readReg32(0x1638a0); // ADL-P/RPL-P
```

## Useful logs or kp log to debug:

need your logs or kernel panic log.. without them I cannot do anything...
so you must boot with - for example an empty nblue - inside the system and get previous lilulog and logs related THAT boot.

```bash
log show --style syslog --predicate 'processID == 0' --last 15m --info --debug > /tmp/x.log
grep "[IGFB]" /tmp/x.log > /tmp/fb.log
```

Example Lilu log path:

```text
/private/var/log/Lilu_1.7.2_23.6.txt
/Libtary/Logs/DiagnosticReports/..
```

Additional developer note:

```text
[N.B. library is in continue developement]

Developer from all over the world, are you ready?  Still needs some patches to my fully working RPL-P laptop but... it's open to developer now.
No more IOPCIPrimaryMatch, we work on IOResource so just put your kexts (+ bundle) in /Library/Extensions folder or use /System/Library/Extensions kext

https://github.com/sgiammori/NootedGreen

IOResources solving now is done like this for TGL kexts or IcL ketxts : first look at LE kexts and if any kexts is found than fallback to find in SLE kexts : * framebuffer for fb * + * graphics for gpu * + * bundle for metal *

they (also Bookcase) need permissions fix so before move to /L/E do in some random folder (maybe better use our Lilu plugin forked for IOResources eventually problems...
```

## Workflow for kexts (fb+Graphics+Hookcase in LE)

```
- sudo chmod -R 755 Apple*
- sudo chown -R root:wheel Apple*
- move the files to /L/E
- delete /Library/KernelCollections/AuxiliaryKernelExtensions.kc
- redo sudo chown -R root:wheel /Library/Extensions/Apple*
- sudo kextcache -i /
(if System asks you permissions, just allow and restart before to test)

(maybe necessary this below also)

sudo kmutil load -p /Library/Extensions/AppleIntelTGLGraphics.kext 2>&1
sudo kextcache -i /

- and - important - also set IOPCIPrimaryMatch in kexts to probe your device-id
```

## Compatibility-First Defaults

Recent changes switch NootedGreen to safer defaults for cross-machine portability:

- Legacy hardcoded IGPU property seeding is now **opt-in**, not default.
- Experimental watchdog/monitor logic is still **opt-in** via `-ngreenexp`.
- Native display-pipe path is now default; forced fallback mode is opt-in via `-ngreendp0`.
- Coexistence paths avoid forcing DVMT/framebuffer processing when the DVMT module is not enabled.

This reduces machine-specific assumptions in default boots and keeps aggressive behavior available only when explicitly requested for debugging.

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

### Driver path resolution and fallback order

NootedGreen keeps a strict load-path policy for Gen11/Gen12 bring-up:

- **Primary (preferred): TGL from `/Library/Extensions`**
	- `AppleIntelTGLGraphicsFramebuffer.kext`
	- `AppleIntelTGLGraphics.kext`
- **Fallback: ICL from `/System/Library/Extensions`**
	- `AppleIntelICLLPGraphicsFramebuffer.kext`
	- `AppleIntelICLGraphics.kext`

Runtime guards enforce this behavior:

- If TGL framebuffer loads, ICL framebuffer processing is skipped.
- If TGL accelerator loads, ICL accelerator processing is skipped.

For Metal bundle discovery, DYLD patching still prioritizes `/Library/Extensions` first (via `gpu_bundle_find_trusted` path rewrite), with `/System/Library/Extensions` retained as fallback.

## Compatibility

NootedBlue legacy support (fully preserved):

- **Haswell** (10.12+)
- **Broadwell / Braswell** (10.14+)
- **Gemini Lake** (10.14+)

NootedGreen (Gen11/Gen12 — TGL driver spoofing):

| Platform | Status | Est. | Notes |
|----------|--------|------|-------|
| **Tiger Lake** | ~90% | V52 | RPL-specific patches auto-skipped via CPUID. GuC, topology, ForceWake, BCS all use native Apple paths. Remaining risk: SKU bypass hook + DYLD patches still in the path. No real TGL hardware tested yet. |
| **Raptor Lake-P** | ~55% | V74+ | Primary dev platform (i7-13700H). Early IGPU identity selection now works with OpenCore DeviceProperties injection (`AAPL,ig-platform-id=9A490000` observed by Lilu). System is stable (no kernel panic), but display output is still visually corrupted/unstable after driver takeover. |
| **Alder Lake** | ~35% | — | Same Gen12 arch as RPL, should behave similarly. Untested. |
| **Rocket Lake** | ~25% | — | Gen12 LP but different display engine. Untested. |
| **Ice Lake** | ~50% | V52 | Dedicated ICL path exists (ICL FB + ICL HW kextInfos, ICL-specific object offsets in `getGPUInfoICL`, SKU gate×3, platform remap, PAVP hook, DYLD ICL Metal device-ID bypass). Topology hardcoded to ICL GT2 LP (1×8×8=64EU). IRQ init disabled (V37 boot hang). ICL path only activates when TGL kexts are absent. Untested on real ICL hardware. |

## Building

Open `NootedGreen.xcodeproj` and select the **NootedGreen** scheme to build the Gen11/Gen12 plugin, or one of the original NootedBlue schemes for legacy hardware. Build with Xcode.

## Authors

- **Stefano Giammori** ([@sgiammori](https://github.com/sgiammori)) — reverse engineering, driver development, hardware testing
- Thanks to [@shl628](https://github.com/shl628) and [@jalavoui](https://github.com/jalavoui), developers of NootedBlue
- **Claude Sonnet** (Anthropic) — AI pair-programming, code generation, debug analysis
- **Claude Opus 4.6** via GitHub Copilot — AI pair-programming, code generation, debug analysis

## License

[Thou Shalt Not Profit License 1.0](LICENSE)
