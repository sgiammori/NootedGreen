//! Copyright © 2022-2023 ChefKiss Inc. Licensed under the Thou Shalt Not Profit License version 1.5.
//! See LICENSE for details.

#include "DYLDPatches.hpp"
#include "kern_green.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <IOKit/IODeviceTreeSupport.h>

DYLDPatches *DYLDPatches::callback = nullptr;

void DYLDPatches::init() { callback = this; }

void DYLDPatches::processPatcher(KernelPatcher &patcher) {

    auto *entry = IORegistryEntry::fromPath("/", gIODTPlane);
    if (entry) {
        DBGLOG("DYLD", "Setting hwgva-id to iMacPro1,1");
        entry->setProperty("hwgva-id", const_cast<char *>(kHwGvaId), arrsize(kHwGvaId));
        OSSafeReleaseNULL(entry);
    }

    KernelPatcher::RouteRequest request {"_cs_validate_page", wrapCsValidatePage, this->orgCsValidatePage};

    SYSLOG_COND(!patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1), "DYLD",
        "Failed to route kernel symbols");
}

void DYLDPatches::wrapCsValidatePage(vnode *vp, memory_object_t pager, memory_object_offset_t page_offset,
    const void *data, int *validated_p, int *tainted_p, int *nx_p) {
    FunctionCast(wrapCsValidatePage, callback->orgCsValidatePage)(vp, pager, page_offset, data, validated_p, tainted_p,
        nx_p);

    char path[PATH_MAX];
    int pathlen = PATH_MAX;
    if (vn_getpath(vp, path, &pathlen) != 0) { return; }

    if (!UserPatcher::matchSharedCachePath(path)) {
        if (LIKELY(strncmp(path, kCoreLSKDMSEPath, arrsize(kCoreLSKDMSEPath))) ||
            LIKELY(strncmp(path, kCoreLSKDPath, arrsize(kCoreLSKDPath)))) {
            return;
        }
        const DYLDPatch patch = {kCoreLSKDOriginal, kCoreLSKDPatched, "CoreLSKD streaming CPUID to Haswell"};
        patch.apply(const_cast<void *>(data), PAGE_SIZE);
        return;
    }

    if (UNLIKELY(KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE, kVideoToolboxDRMModelOriginal,
            arrsize(kVideoToolboxDRMModelOriginal), BaseDeviceInfo::get().modelIdentifier, 20))) {
        DBGLOG("DYLD", "Applied 'VideoToolbox DRM model check' patch");
    }

    const DYLDPatch patches[] = {
        {kAGVABoardIdOriginal, kAGVABoardIdPatched, "iMacPro1,1 spoof (AppleGVA)"},
		{kHEVCEncBoardIdOriginal, kHEVCEncBoardIdPatched, "iMacPro1,1 spoof (AppleGVAHEVCEncoder)"},
    };
    DYLDPatch::applyAll(patches, const_cast<void *>(data), PAGE_SIZE);
	
	// ── V50: GPU bundle search path redirect ──
	// Metal calls gpu_bundle_find_trusted() in libsystem_sandbox.dylib to locate
	// GPU plugin bundles. This function searches exactly two directories:
	//   1. /Library/GPUBundles      (checked first)
	//   2. /System/Library/Extensions  (checked second)
	// using format "%s/%s.bundle" to construct paths.
	//
	// Apple never made a Mac with Tiger Lake — no TGL Metal driver exists in either
	// of those directories. The TGL driver is at /Library/Extensions/ (user-installed).
	//
	// Fix: patch the first search path in libsystem_sandbox's __cstring:
	//   "/Library/GPUBundles\0"  (20 bytes) → "/Library/Extensions\0" (20 bytes)
	// This makes gpu_bundle_find_trusted search /Library/Extensions/ first,
	// where the TGL driver bundle actually exists.
	// /System/Library/Extensions stays as the fallback for system GPU bundles.
	//
	// Alternative: manually copy the TGL bundle:
	//   sudo mkdir -p /Library/GPUBundles
	//   sudo cp -R /Library/Extensions/AppleIntelTGLGraphicsMTLDriver.bundle /Library/GPUBundles/
	static const uint8_t gpuPathFind[] = {
		0x2F, 0x4C, 0x69, 0x62, 0x72, 0x61, 0x72, 0x79, // /Library
		0x2F, 0x47, 0x50, 0x55, 0x42, 0x75, 0x6E, 0x64, // /GPUBund
		0x6C, 0x65, 0x73, 0x00,                           // les\0
	};
	static const uint8_t gpuPathRepl[] = {
		0x2F, 0x4C, 0x69, 0x62, 0x72, 0x61, 0x72, 0x79, // /Library
		0x2F, 0x45, 0x78, 0x74, 0x65, 0x6E, 0x73, 0x69, // /Extensi
		0x6F, 0x6E, 0x73, 0x00,                           // ons\0
	};
	if (UNLIKELY(KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE,
			gpuPathFind, arrsize(gpuPathFind), gpuPathRepl, arrsize(gpuPathRepl)))) {
		SYSLOG("DYLD", "V50: Patched gpu_bundle_find_trusted: /Library/GPUBundles -> /Library/Extensions");
	}
	
	// V50: ICL Metal driver device-ID bypass (mask-based, build-portable).
	// The ICL driver (in shared cache) checks device_id:vendor_id against
	// 0x8A5C8086/0x8A5D8086, then calls a hw-cap fallback check.
	// Patch: change jne to jmp so the hw-cap check always succeeds.
	// This is a fallback — if the TGL driver loads, this won't be needed.
	static const uint8_t f2find[] = {
		0x81, 0xFF, 0x86, 0x80, 0x5C, 0x8A,  // cmp edi, 0x8A5C8086
		0x74, 0x00,                            // je +XX (wildcard offset)
		0x81, 0xFF, 0x86, 0x80, 0x5D, 0x8A,  // cmp edi, 0x8A5D8086
		0x74, 0x00,                            // je +XX (wildcard offset)
		0xE8, 0x00, 0x00, 0x00, 0x00,         // call +XXXX (wildcard offset)
		0x84, 0xC0,                            // test al, al
		0x75, 0x00,                            // jne +XX → change to EB (jmp)
	};
	static const uint8_t f2mask[] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // cmp exact
		0xFF, 0x00,                            // je opcode exact, offset wildcard
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // cmp exact
		0xFF, 0x00,                            // je opcode exact, offset wildcard
		0xFF, 0x00, 0x00, 0x00, 0x00,         // call opcode exact, offset wildcard
		0xFF, 0xFF,                            // test exact
		0xFF, 0x00,                            // jne opcode exact, offset wildcard
	};
	static const uint8_t f2repl[] = {
		0x81, 0xFF, 0x86, 0x80, 0x5C, 0x8A,  // unchanged
		0x74, 0x00,                            // unchanged
		0x81, 0xFF, 0x86, 0x80, 0x5D, 0x8A,  // unchanged
		0x74, 0x00,                            // unchanged
		0xE8, 0x00, 0x00, 0x00, 0x00,         // unchanged
		0x84, 0xC0,                            // unchanged
		0xEB, 0x00,                            // jne→jmp (0x75→0xEB)
	};
	static const uint8_t f2rmask[] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // don't touch
		0x00, 0x00,                            // don't touch
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // don't touch
		0x00, 0x00,                            // don't touch
		0x00, 0x00, 0x00, 0x00, 0x00,         // don't touch
		0x00, 0x00,                            // don't touch
		0xFF, 0x00,                            // CHANGE byte 23 only (0x75→0xEB)
	};
	if (UNLIKELY(KernelPatcher::findAndReplaceWithMask(const_cast<void *>(data), PAGE_SIZE,
			f2find, f2mask, f2repl, f2rmask, 1, 0))) {
		SYSLOG("DYLD", "V50: Applied ICL Metal device-ID bypass (f2, mask-based)");
	}
	
	//disp
	static const uint8_t f3[] = {0x0F, 0x85, 0xE1, 0x03, 0x00, 0x00, 0x48, 0x8D, 0x3C, 0xDD, 0x00, 0x00, 0x00, 0x00, 0xE8, 0xA4, 0x1F, 0x0B, 0x00, 0x48, 0x8B, 0x3D, 0xE1, 0x5D, 0xCB, 0x41, 0x48, 0x89, 0x05, 0xDA, 0x5D, 0xCB, 0x41, 0x48, 0x85, 0xFF, 0x74, 0x05};
	static const uint8_t r3[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x8D, 0x3C, 0xDD, 0x00, 0x00, 0x00, 0x00, 0xE8, 0xA4, 0x1F, 0x0B, 0x00, 0x48, 0x8B, 0x3D, 0xE1, 0x5D, 0xCB, 0x41, 0x48, 0x89, 0x05, 0xDA, 0x5D, 0xCB, 0x41, 0x48, 0x85, 0xFF, 0x74, 0x05};
	
	static const uint8_t f3b[] = {0x75, 0x0A, 0xE8, 0x5C, 0x1B, 0x0B, 0x00, 0xE9, 0xD0, 0xF6, 0xFF, 0xFF, 0x83, 0xBD, 0xC0, 0xFE, 0xFF, 0xFF, 0x00, 0x48, 0x8D, 0x05, 0x20, 0x93, 0x0C, 0x00};
	static const uint8_t r3b[] = {0x90, 0x90, 0xE8, 0x5C, 0x1B, 0x0B, 0x00, 0xE9, 0xD0, 0xF6, 0xFF, 0xFF, 0x83, 0xBD, 0xC0, 0xFE, 0xFF, 0xFF, 0x00, 0x48, 0x8D, 0x05, 0x20, 0x93, 0x0C, 0x00};
	
	//CoreDisplay assertion bypass - Sonoma 14.7.1
	//CoreDisplay_CreateDisplayForCGXDisplayDevice: NOP jne to __assert_rtn
	static const uint8_t f3b_sonoma[] = {0x75, 0x0A, 0xE8, 0x79, 0x03, 0x0B, 0x00, 0xE9, 0xE6, 0xF6, 0xFF, 0xFF, 0x83, 0xBD, 0xC0, 0xFE, 0xFF, 0xFF, 0x00, 0x48, 0x8D, 0x05, 0xF2, 0x76, 0x0C, 0x00};
	static const uint8_t r3b_sonoma[] = {0x90, 0x90, 0xE8, 0x79, 0x03, 0x0B, 0x00, 0xE9, 0xE6, 0xF6, 0xFF, 0xFF, 0x83, 0xBD, 0xC0, 0xFE, 0xFF, 0xFF, 0x00, 0x48, 0x8D, 0x05, 0xF2, 0x76, 0x0C, 0x00};
	
	//CoreDisplay::DisplayPipe::RunFullDisplayPipe - return immediately (Sonoma 14.7.1)
	//Without Metal acceleration, RunFullDisplayPipe crashes at multiple points
	//(NULL MetalDevice, ud2 assertions, heap corruption from uninitialized Metal objects).
	//Replace function prologue with ret to skip the entire Metal rendering path.
	//Pattern: push rbp; mov rbp,rsp; push r15-r12; push rbx; sub rsp,0x578; mov [rbp-0x478],rsi; mov r14,rdi
	static const uint8_t f_skipfdp_sonoma[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x78, 0x05, 0x00, 0x00, 0x48, 0x89, 0xb5, 0x88, 0xfb, 0xff, 0xff};
	static const uint8_t r_skipfdp_sonoma[] = {0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
	
	//CoreDisplay::Display::Present - return immediately (Sonoma 14.7.1)
	//Present calls AccessComplete which calls GetMTLTexture, asserting on NULL Metal device.
	//This is a separate code path from RunFullDisplayPipe (display scanout vs compositing).
	//Pattern: push rbp; mov rbp,rsp; push r15-r12; push rbx; sub rsp,0xa8; mov r14,rcx; mov ebx,edx; mov r12,rsi; mov r15,rdi
	static const uint8_t f_skippresent_sonoma[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xa8, 0x00, 0x00, 0x00, 0x49, 0x89, 0xce, 0x89, 0xd3, 0x49, 0x89, 0xf4, 0x49, 0x89, 0xff};
	static const uint8_t r_skippresent_sonoma[] = {0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
	
	//CoreDisplay::DisplaySurface::GetMTLTexture - return NULL (Sonoma 14.7.1)
	//Root cause: without Metal GPU, GetMTLTexture asserts on NULL MetalDevice.
	//Called from AccessComplete (via Display::Present, DisplaySurface_Free, CDSurface::finish, etc.).
	//Patching individual callers is whack-a-mole; patch GetMTLTexture itself to return NULL gracefully.
	//xor eax,eax (31 c0); ret (c3) + NOPs
	//Pattern: push rbp; mov rbp,rsp; push r15-r12; push rbx; sub rsp,0x168; mov r14,rsi; mov r15,rdi
	static const uint8_t f_getmtltex_sonoma[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x68, 0x01, 0x00, 0x00, 0x49, 0x89, 0xf6, 0x49, 0x89, 0xff};
	static const uint8_t r_getmtltex_sonoma[] = {0x31, 0xC0, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
	
	//CoreDisplay::DisplaySurface::AccessComplete - return immediately (Sonoma 14.7.1)
	//AccessComplete calls multiple Metal functions (GetMTLTexture, GetMTLCommandQueue, etc.)
	//on a NULL MetalDevice. Patching individual Metal functions is whack-a-mole.
	//Patch AccessComplete prologue to return early and skip all Metal GPU synchronization.
	//Pattern: push rbp; mov rbp,rsp; push r15-r12; push rbx; sub rsp,0x1d8;
	//         mov rax,[rip+0x3f8faac1]; mov rax,[rax]; mov [rbp-0x30],rax
	static const uint8_t f_skipac_sonoma[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xd8, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x05, 0xc1, 0xaa, 0x8f, 0x3f, 0x48, 0x8b, 0x00, 0x48, 0x89, 0x45, 0xd0};
	static const uint8_t r_skipac_sonoma[] = {0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

	//CoreDisplay::DisplayPipe::RunFullDisplayPipe - null-guard virtual call (Sonoma 14.7.1)
	//Stage-3 crash shows rdi == 0 at sequence:
	//  mov rdi, [r14+0x888]
	//  mov rax, [rdi]
	//  call qword ptr [rax+0x28]
	//Replace with test/jump so the call is skipped when rdi is NULL.
	static const uint8_t f_runfdp_guard_sonoma[] = {0x49, 0x8b, 0xbe, 0x88, 0x08, 0x00, 0x00, 0x48, 0x8b, 0x07, 0xff, 0x50, 0x28};
	static const uint8_t r_runfdp_guard_sonoma[] = {0x48, 0x85, 0xff, 0x74, 0x06, 0x90, 0x90, 0x48, 0x8b, 0x07, 0xff, 0x50, 0x28};
	
	
	//skyl
	static const uint8_t f4[] = {0x0F, 0x84, 0x9E, 0x00, 0x00, 0x00, 0xE8, 0x75, 0x3D, 0xEC, 0xFF, 0x4C, 0x8B, 0x3D, 0x58, 0xCC, 0x91, 0x3E, 0x4C, 0x89, 0xFF, 0xBE, 0x10, 0x00, 0x00, 0x00, 0xE8, 0x09, 0xEA, 0x1C, 0x00, 0x84, 0xC0, 0x0F, 0x84, 0x51, 0xFA, 0xFF, 0xFF};
	static const uint8_t r4[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xE8, 0x75, 0x3D, 0xEC, 0xFF, 0x4C, 0x8B, 0x3D, 0x58, 0xCC, 0x91, 0x3E, 0x4C, 0x89, 0xFF, 0xBE, 0x10, 0x00, 0x00, 0x00, 0xE8, 0x09, 0xEA, 0x1C, 0x00, 0x84, 0xC0, 0x0F, 0x84, 0x51, 0xFA, 0xFF, 0xFF};
	
	
	if (getKernelVersion() >= KernelVersion::Ventura) {
		// V97: Metal is OFF by default on the RPL+TGL spoof path.
		// WindowServer crash reports show NULL deref in
		// CoreDisplay::DisplayPipe::RunFullDisplayPipe when Metal display-pipe is active.
		// Keep CoreDisplay safety stubs enabled by default; opt into Metal explicitly.
        static bool noMetalChecked = false;
        static bool noMetal = false;
        static bool fullMTLChecked = false;
        static int fullMTLStage = 0;
		static bool skylBypass = false;
        if (!noMetalChecked) {
			// Safe default
			noMetal = true;

			// Optional explicit scalar boot-arg support: ngreenNoMetal=0|1
			int noMetalArg = 0;
			if (PE_parse_boot_argn("ngreenNoMetal", &noMetalArg, sizeof(noMetalArg))) {
				noMetal = (noMetalArg != 0);
			}

			// Legacy flag still supported
			if (checkKernelArgument("-ngreenNoMetal"))
				noMetal = true;

			// Explicit opt-in to Metal for experiments
            bool legacyAllow = checkKernelArgument("-ngreenAllowMetal");
            if (legacyAllow) noMetal = false;

			// Experimental staged bring-up of full Metal path.
			// Stages are only used when Metal is ON:
			//   0 = keep all safety stubs
			//   1 = re-enable AccessComplete only
			//   2 = re-enable AccessComplete + GetMTLTexture
			//   3 = re-enable AccessComplete + GetMTLTexture + RunFullDisplayPipe
			int fullMTLArg = 0;
			if (PE_parse_boot_argn("ngreenFullMTLStage", &fullMTLArg, sizeof(fullMTLArg))) {
				if (fullMTLArg < 0) fullMTLArg = 0;
				if (fullMTLArg > 3) fullMTLArg = 3;
				fullMTLStage = fullMTLArg;
			}
			if (checkKernelArgument("-ngreenFullMTLTest") && fullMTLStage == 0)
				fullMTLStage = 1;

			// Optional SkyLight branch bypass for stage-3 black-screen diagnostics.
			// Enable with: -ngreenSkylBypass
			skylBypass = checkKernelArgument("-ngreenSkylBypass");
			fullMTLChecked = true;
            noMetalChecked = true;
            SYSLOG("DYLD", "V50: Metal=%s (-ngreenNoMetal=%d)", noMetal ? "OFF" : "ON", noMetal);
			SYSLOG("DYLD", "V101: FullMTL stage=%d (%s)", fullMTLStage,
				fullMTLStage == 0 ? "all safety stubs active" :
				fullMTLStage == 1 ? "AccessComplete enabled" :
				fullMTLStage == 2 ? "AccessComplete+GetMTLTexture enabled" :
				"RunFullDisplayPipe+GetMTLTexture+AccessComplete enabled");
			SYSLOG("DYLD", "V102: SkyLight bypass=%s", skylBypass ? "ON" : "OFF");
        }
        
		if (!noMetal) {
			// V101: staged Boot C hardening for full-MTL bring-up.
			const DYLDPatch stage0Patches[] = {
				{f3b_sonoma, r3b_sonoma, "CoreDisplay assertion bypass (Sonoma)"},
				{f_skipfdp_sonoma, r_skipfdp_sonoma, "RunFullDisplayPipe skip entire function (Sonoma, Metal ON)"},
				{f_getmtltex_sonoma, r_getmtltex_sonoma, "GetMTLTexture return NULL (Sonoma, Metal ON)"},
				{f_skipac_sonoma, r_skipac_sonoma, "AccessComplete skip (Sonoma, Metal ON)"},
			};
			const DYLDPatch stage1Patches[] = {
				{f3b_sonoma, r3b_sonoma, "CoreDisplay assertion bypass (Sonoma)"},
				{f_skipfdp_sonoma, r_skipfdp_sonoma, "RunFullDisplayPipe skip entire function (Sonoma, Metal ON)"},
				{f_getmtltex_sonoma, r_getmtltex_sonoma, "GetMTLTexture return NULL (Sonoma, Metal ON)"},
			};
			const DYLDPatch stage2Patches[] = {
				{f3b_sonoma, r3b_sonoma, "CoreDisplay assertion bypass (Sonoma)"},
				{f_skipfdp_sonoma, r_skipfdp_sonoma, "RunFullDisplayPipe skip entire function (Sonoma, Metal ON)"},
			};
			const DYLDPatch stage3Patches[] = {
				{f3b_sonoma, r3b_sonoma, "CoreDisplay assertion bypass (Sonoma)"},
				{f_runfdp_guard_sonoma, r_runfdp_guard_sonoma, "RunFullDisplayPipe NULL vcall guard (Sonoma, Metal ON)"},
			};
			const DYLDPatch stage3SkylPatches[] = {
				{f3b_sonoma, r3b_sonoma, "CoreDisplay assertion bypass (Sonoma)"},
				{f_runfdp_guard_sonoma, r_runfdp_guard_sonoma, "RunFullDisplayPipe NULL vcall guard (Sonoma, Metal ON)"},
				{f4, r4, "SkyLight conditional bypass (Sonoma, Metal ON)"},
			};

			const DYLDPatch *patches = stage0Patches;
			size_t patchCount = arrsize(stage0Patches);
			if (fullMTLStage == 1) {
				patches = stage1Patches;
				patchCount = arrsize(stage1Patches);
			} else if (fullMTLStage == 2) {
				patches = stage2Patches;
				patchCount = arrsize(stage2Patches);
			} else if (fullMTLStage >= 3) {
				if (skylBypass) {
					patches = stage3SkylPatches;
					patchCount = arrsize(stage3SkylPatches);
				} else {
					patches = stage3Patches;
					patchCount = arrsize(stage3Patches);
				}
			}

			DYLDPatch::applyAll(patches, patchCount, const_cast<void *>(data), PAGE_SIZE);
        } else {
            // Metal OFF: stub out CoreDisplay Metal paths to prevent NULL MTLDevice crashes
            const DYLDPatch patches[] = {
                {f3b_sonoma, r3b_sonoma, "CoreDisplay assertion bypass (Sonoma)"},
                {f_skipfdp_sonoma, r_skipfdp_sonoma, "RunFullDisplayPipe skip entire function (Sonoma)"},
                // V98: removed Display::Present stub — allow hardware scanout path
                {f_getmtltex_sonoma, r_getmtltex_sonoma, "GetMTLTexture return NULL (Sonoma)"},
                {f_skipac_sonoma, r_skipac_sonoma, "AccessComplete skip (Sonoma)"},
            };
            DYLDPatch::applyAll(patches, const_cast<void *>(data), PAGE_SIZE);
        }
    }

	
}
