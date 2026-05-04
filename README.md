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
- **GPU reset storm fully tamed (V157):** `resetGraphicsEngine` circuit-breaker (V153+V154) confirmed working — V153 fires from call #1 (coerces `ret=1025→0` when RCS ring head == tail), V154 opens at call #4 (skips original reset entirely after 3 consecutive quiescent coercions). Ring CTL is restored to `0x7001` (RING_ENABLE set) on every fake-success path via V155 saved-CTL restore.
- **Execlist cleanup in progress (V158):** Root cause of remaining `userspace watchdog timeout` panics is identified — `EXEC=0x40018098` (execlist FIFO stalled, bit 30 set) and `CSB=0x1001` (15 unread context-status entries) persist post-fake-success, causing Apple's driver to keep scheduling resets. V158 addresses this by writing four null descriptors to `RING_ELSP` (cancelling pending EL0/EL1 contexts) and advancing the CSB read pointer to match the write pointer on every V153/V154 return-0 path.
- **V80L root cause identified:** The `userspace watchdog timeout` KP (T+120s, 2 induced WS crashes) was traced to the V80L plane-linearization block inside `v71EmrEnforcer`. It was writing `PLANE_CTL` / `PLANE_STRIDE` / `PLANE_SURF` every 50ms while WindowServer was actively compositing those same registers — WS detected a render failure, crash-looped, and watchdogd fired. V80L is now limited to the first 3 enforcer ticks (≤150ms after `start()`), which preserves the visible brief display flash at early boot without fighting WS after it takes over the compositor. Use `-ngreenv80l` for continuous mode (testing only).

### Recent Progress

- **V80L fix:** `userspace watchdog timeout` KP root cause confirmed and fixed. V80L (plane-linearization writes in `v71EmrEnforcer`) limited to first 3 ticks only. Brief display flash preserved; WS crash-loop eliminated. `-ngreenv80l` enables continuous mode for testing.
- **V158:** Cancel pending execlist contexts + drain CSB after every fake-success `resetGraphicsEngine` return. Writes 4×0 to `RING_ELSP` (two null 64-bit context descriptors = cancel EL0 + EL1), then reads `RING_CONTEXT_STATUS_PTR` and writes back with read_ptr set to write_ptr. Logs `EXEC` value after the null writes to confirm whether execlist FIFO cleared. Applied on both V153 and V154 return-0 paths. Build confirmed clean. Boot test pending.
- **V157:** Removed `rcsCtl == 0` guard from V153 quiescent check — hardware restores CTL to `0x7001` before V153 reads it, so the condition was always false. Only `rcsHead == rcsTail && err == 0` is needed. Boot log confirmed: V153 now fires from call #1.
- **V156:** Fixed `RING_ENABLE` bit (ORed `| 1` at all CTL write sites). Dropped `bCtl == 0` from V154 check. Stabilized ring-enabled state across all fake-success paths.
- **V155:** Snapshot of RCS CTL (with `RING_ENABLE` forced) taken before the original `resetGraphicsEngine` call; restored on V153 and V154 return-0 paths to prevent the driver disabling the ring.
- **V154:** RPL-only circuit-breaker — after 3 consecutive quiescent `ret=1025` coercions by V153, skip calling the original `resetGraphicsEngine` entirely. Prevents the health monitor (V60M, 2 s tick × 60 = 120 s budget) from burning the full watchdog window on redundant resets.
- **V153:** Coerce `resetGraphicsEngine ret=1025 → 0` when RCS ring is quiescent (`rcsHead == rcsTail`, `ERROR_GEN6 == 0`) on non-real TGL. Increments consecutive-quiescent counter for V154 arming.
- **V152:** BCS ring drain — force `RING_TAIL ← RING_HEAD` before BCS reset to stop repeated "ring not empty" resets on RPL where the BCS engine cannot execute under the TGL driver.
- **DYLD compact baseline:** Assertion bypass remains baseline on Ventura+/Sonoma. Stage-3 safety stubs are conditional (non-real TGL default), with full-MTL override available via `-ngreenfullmtl`.
- **DisplayPipe defaults updated:** Native `DisplayPipeSupported` is default. `-ngreendp0` is explicit fallback.
- **V77 default policy updated:** kill delay default is full monitor window (no automatic client termination unless explicitly requested).
- **V96 connector state update:** `getOnlineInfo` forces online + changed for stronger hotplug/state propagation.
- **V110:** V59 delayed checks + V74 EMR enforcer run unconditionally on non-real TGL; V60 monitor remains opt-in via `-ngreenexp`.
- **V75:** **CRITICAL FIX** — Removed post-`awaitPublishing` property override race condition. Display output now stable and corruption-free.
- **V74:** Permanent EMR enforcer — 50ms timer runs indefinitely, keeping ERROR_GEN6 masked.
- **V111C:** For non-real TGL, original Blit3D initializer is no longer auto-selected in full-MTL mode; it is opt-in only via `-ngreenV69AllowOriginal`.

### GPU Reset Storm — Root Cause Analysis (RPL / macOS 14.7.1)

On RPL under the TGL driver spoof, Apple's `resetGraphicsEngine` returns `1025` (ETIMEDOUT) on every call because the TGL reset sequence issues commands the RPL hardware cannot execute. This triggers a `userspace watchdog timeout` panic as follows:

1. The V60M health monitor fires every ~2 s for up to 120 s (60 ticks).
2. Each tick calls `resetGraphicsEngine` — before V153/V154 this always returned `1025` and reset the watchdog counter.
3. After 120 s of no successful reset, the kernel watchdog fires.

V153/V154 break the storm by detecting a quiescent RCS ring and returning `0` (success) without touching the hardware. V154 then short-circuits all subsequent calls for the remainder of the boot session. V155/V156/V157 ensure the RCS ring stays enabled (`CTL=0x7001`) across all fake-success returns.

The remaining issue post-V157 is that `RING_EXECLIST_STATUS` (`EXEC`) stays at `0x40018098` (bit 30 = execlist queue stalled) and `RING_CONTEXT_STATUS_PTR` shows write_ptr ahead of read_ptr across all V60M ticks — meaning Apple's driver still sees pending context work and continues scheduling resets even though V154 returns instantly. V158 targets this by nulling the execlist submission port and draining the CSB on every fake-success return.

Key registers (all offsets from `RENDER_RING_BASE = 0x2000`):

| Register | Offset | Role in fix |
|----------|--------|-------------|
| `RING_ELSP` | `+0x230` | Write 4×0 to cancel pending EL0+EL1 context descriptors |
| `RING_EXECLIST_STATUS` | `+0x234` | Bit 30 = execlist FIFO stalled; cleared by ELSP null writes |
| `RING_CONTEXT_STATUS_PTR` | `+0x3a0` | Bits[15:8]=write_ptr, [7:0]=read_ptr; advance read→write to drain CSB |
| `RING_CTL` | `+0x3c` | Bit 0 = RING_ENABLE; V155 saves and restores this on every fake-success |
| `ERROR_GEN6` | `0x40A0` | Must be 0 for V153 quiescent condition |

### Current Status

System boots to login on RPL macOS 14.7.1 (`23H222`). The GPU reset storm is tamed: V153+V154 prevent the health monitor from burning the watchdog budget on redundant reset calls. CTL is stable at `0x7001`. The `userspace watchdog timeout` KP root cause has been identified and fixed: V80L plane-linearization writes in `v71EmrEnforcer` were fighting WindowServer over `PLANE_CTL`/`PLANE_STRIDE`/`PLANE_SURF` every 50ms — this has been corrected (first 3 ticks only). Active investigation continues on execlist/CSB cleanup (V158). V88 visual fill is opt-in only (`-ngreenv88`) so default boots preserve normal Apple UI layout.

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
-v keepsyms=1 debug=0x100 IGLogLevel=8 -NGreenDebug -liludbg liludump=200 ngreen-dmc=skip -allow3d -disablegfxfirmware -ngreenexp
```

| Arg | Purpose |
|---|---|
| `-NGreenDebug` | Enable NootedGreen debug logging |
| `-disablegfxfirmware` | Disable GuC/HuC firmware loading — required on RPL/ADL because scheduler selection (`ngreenSched`) happens after the HW-branch `processKext`, so the driver attempts firmware init before NootedGreen can override the scheduler type. Not needed on real TGL (GuC loads natively). |
| `-nbwegcoex` / `nbwegcoex=1` | Enable WEG coexistence mode. |
| `ngreenSched=N` | Select GPU scheduler type: `3` = GuC firmware, `4` = IGScheduler4, `5` = host preemptive (default: `3` on real TGL, `5` on RPL/ADL) |
| `ngreen-dmc=skip|tgl|adlp` | DMC policy: skip CSR load, or force TGL/ADL-P DMC path for diagnostics. |
| `-allow3d` | Force 3D acceleration |
| `-nbdyldoff` | **Disable ALL DYLD patches** (CoreDisplay, OpenGL, Metal, SkyLight) — debug only |
| `-ngreenexp` / `ngreenexp=1` | Enable experimental runtime monitor/timer paths (V60 monitor and extra diagnostics) |
| `-ngreenv60` / `ngreenv60=1` | Force-enable V60 monitor. |
| `-ngreenv60off` / `ngreenv60off=1` | Force-disable V60 monitor. |
| `-ngreenv60rw` / `ngreenv60rw=1` | Enable aggressive V60 MMIO write path. |
| `-ngreendp0` / `ngreendp0=1` | Force fallback mode: set `DisplayPipeSupported=0` in accelerator capabilities |
| `-ngreendp1` / `ngreendp1=1` | Explicitly keep native `DisplayPipeSupported` path (default behavior) |
| `ngreenV77DelayKill=N` | Delay V77 display-pipe client termination by `N` monitor iterations (`0..60`, default `60` = effectively disabled) |
| `-ngreenv88` / `ngreenv88=1` | Enable V88 scanout fill + plane toggle diagnostics (draws test bars/colors; off by default) |
| `-ngreenv93` / `ngreenv93=1` | Enable V93 plane guard diagnostics (disabled by default). |
| `-ngreenfullmtl` / `ngreenfullmtl=1` | Force full CoreDisplay Metal path on Ventura+/Sonoma by skipping Stage-3 NULL safety stubs (GetMTLTexture/GetMTLCommandQueue/RunFullDisplayPipe guard). This **does not** auto-enable Apple's original Blit3D initializer. |
| `-ngreenfullmtldyld` / `ngreenfullmtldyld=1` | DYLD-side full-MTL override only. |
| `ngreenV142=0|1|2|3` / `-ngreenV142hardunsupported` / `-ngreenV142ok` / `-ngreenV142pass` / `-ngreenV142orig` | Select spoof-path `submitBlit` behavior on non-real TGL. `0`=return unsupported, `1`=bypass return 0 (**default/recommended**), `2`=bypass return 1, `3`=call Apple original (high-risk diagnostic). V186 applies this mode early before task/context mutation to reduce `IGAccelTask::release` lifetime crashes. |
| `-ngreenbcsirq` | Enable BCS bit in tier-1 interrupt want mask on spoof path (advanced diagnostic). |
| `ngreenV120=0|1|2` / `-ngreenV120ok` / `-ngreenV120fail` / `-ngreenV120pass` | Fallback return mode used when submitBlit sees invalid/null task on spoof path. |
| `ngreenV130=0|1|2|3` / `-ngreenV130fail` / `-ngreenV130pass` / `-ngreenV130orig` / `-ngreenV130hybrid` | `barrierSubmission` spoof policy on non-real TGL. |
| `ngreenV130warmup=N` | Hybrid warmup window for `ngreenV130=3` (`0..200`, default 12 calls). |
| `-ngreenV130forceorig` | Allow unsafe original barrier path when mode requests it. |
| `-ngreenV69AllowOriginal` | Opt in to Apple's original Blit3D initialize on non-real TGL when safety preconditions are met. **High risk / diagnostic only**; can panic on unsupported setups. |
| `-ngreenV69SkipOriginal` | Hard-disable Apple's original Blit3D initialize on non-real TGL, even if `-ngreenV69AllowOriginal` is present. |
| `-ngreenV69ForceOriginalUnsafe` | Override safety rejection and force original Blit3D init on spoofed RPL/ADL (crash-debug only). |
| `-ngreenv80l` / `ngreenv80l=1` | Run V80L plane-linearization writes continuously (every 50ms) instead of the default first-3-ticks-only behavior. **Testing only** — causes WS crash-loop + watchdog KP on normal boots. |
| `-ngreenforceprops` / `ngreenforceprops=1` | Enable legacy forced IGPU property injection (`AAPL,ig-platform-id`, `model`, `saved-config`, etc.). Disabled by default in compatibility-first mode. |
| `-ngreenV188htfind` | Enable narrow DYLD hash-find guard variant (V188) for AccessComplete crash diagnostics. |
| `netdbg=<ip:port>` | Enable network debug logger destination for kernel-side NETDBG output. |
| `IGLogLevel=8` | Maximum Intel GPU driver logging |
| `-liludbg` | Enable Lilu debug logging |
| `liludump=N` | Dump Lilu logs after `N` seconds (example: 125 or 200). |

Recommended debug order for `ngreenV142` on spoofed RPL/ADL:

1. `ngreenV142=1` (stable bypass baseline)
2. `ngreenV142=2` (alternate bypass semantics)
3. `ngreenV142=3` only for controlled repro (Apple original path)

Sonoma 14.7.1 note: when WindowServer crashes with `EXC_BAD_ACCESS` at `0x80` in
`CoreDisplay::DisplaySurface::AccessComplete()` / `std::__hash_table::find`, NootedGreen
now applies a non-real-TGL-only DYLD guard (V188) that stubs the crashing
`MTLRenderPipelineState` hash-table `find` specialization so CoreDisplay can take its
fallback path. Real TGL behavior is unchanged.

`-ngreenV188htfind` enables the optional narrow V188 hash-find debug path. Keep it off for normal testing.

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

=> they need permissions fix (also Hookcase) so before move to /L/E do in some random folder, check below
=> IOPCIPrimarymatch must be set in both *TGLGraphics* kexts in /Library/Extensiona
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
| **Raptor Lake-P** | ~70% | V80L | Primary dev platform (i7-13700H). System boots to login on macOS 14.7.1 (`23H222`). GPU reset storm tamed: V153/V154 circuit-breaker confirmed working. `userspace watchdog timeout` KP root cause identified and fixed: V80L plane-linearization in `v71EmrEnforcer` was fighting WindowServer over plane registers every 50ms — now limited to first 3 ticks. Brief display flash at boot preserved. Active work: V158 execlist/CSB drain. |
| **Alder Lake** | ~35% | — | Same Gen12 arch as RPL, should behave similarly. Untested. |
| **Rocket Lake** | ~25% | — | Gen12 LP but different display engine. Untested. |
| **Ice Lake** | ~50% | V52 | Dedicated ICL path exists (ICL FB + ICL HW kextInfos, ICL-specific object offsets in `getGPUInfoICL`, SKU gate×3, platform remap, PAVP hook, DYLD ICL Metal device-ID bypass). Topology hardcoded to ICL GT2 LP (1×8×8=64EU). IRQ init disabled (V37 boot hang). ICL path only activates when TGL kexts are absent. Untested on real ICL hardware. |

## Building

Open `NootedGreen.xcodeproj` and select the **NootedGreen** scheme to build the Gen11/Gen12 plugin, or one of the original NootedBlue schemes for legacy hardware. Build with Xcode.

## Authors

- **Stefano Giammori** ([@sgiammori](https://github.com/sgiammori)) — reverse engineering, driver development, hardware testing
- Thanks to [@shl628](https://github.com/lshbluesky) and [@jalavoui](https://github.com/macintelk), developers of NootedBlue
- **GitHub Copilot** (Claude Sonnet 4.6) — AI pair-programming, code generation, debug analysis

## License

[Thou Shalt Not Profit License 1.0](LICENSE)
