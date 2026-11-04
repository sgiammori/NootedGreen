//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#include "kern_green.hpp"
#include "kern_gen11.hpp"
#include "kern_genx.hpp"
#include "kern_model.hpp"
#include "DYLDPatches.hpp"
#include "HDMI.hpp"
#include "kern_patcherplus.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>


static const char *pathIOAcceleratorFamily2= "/System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/MacOS/IOAcceleratorFamily2";
static const char *pathAGDP = "/System/Library/Extensions/AppleGraphicsControl.kext/Contents/PlugIns/"
							  "AppleGraphicsDevicePolicy.kext/Contents/MacOS/AppleGraphicsDevicePolicy";
static const char *pathBacklight = "/System/Library/Extensions/AppleBacklight.kext/Contents/MacOS/AppleBacklight";
static const char *pathMCCSControl = "/System/Library/Extensions/AppleMCCSControl.kext/Contents/MacOS/AppleMCCSControl";
static const char *pathIOGraphics= "/System/Library/Extensions/IOGraphicsFamily.kext/IOGraphicsFamily";

static KernelPatcher::KextInfo kextAGDP {"com.apple.driver.AppleGraphicsDevicePolicy", &pathAGDP, 1, {true}, {},
	KernelPatcher::KextInfo::Unloaded};
static KernelPatcher::KextInfo kextBacklight {"com.apple.driver.AppleBacklight", &pathBacklight, 1, {true}, {},
	KernelPatcher::KextInfo::Unloaded};
static KernelPatcher::KextInfo kextMCCSControl {"com.apple.driver.AppleMCCSControl", &pathMCCSControl, 1, {true}, {},
	KernelPatcher::KextInfo::Unloaded};
static KernelPatcher::KextInfo kextIOGraphics { "com.apple.iokit.IOGraphicsFamily", &pathIOGraphics, 1, {true}, {},
	KernelPatcher::KextInfo::Unloaded };
static KernelPatcher::KextInfo kextIOAcceleratorFamily2 { "com.apple.iokit.IOAcceleratorFamily2", &pathIOAcceleratorFamily2, 1, {true}, {},
	KernelPatcher::KextInfo::Unloaded };

NGreen *NGreen::callback = nullptr;

static Genx genx;
static Gen11 gen11;
static DYLDPatches dyldpatches;
static HDMI agfxhda;

static uint8_t builtin2[] = {0x00, 0x00, 0x49, 0x9A};
static uint8_t builtin3[] = {0x49, 0x9A, 0x00, 0x00};

static bool shouldForceRPLBringupPlatform(IORegistryEntry *entry) {
	auto *prop = OSDynamicCast(OSData, entry ? entry->getProperty("AAPL,ig-platform-id") : nullptr);
	if (!prop || prop->getLength() < sizeof(uint32_t)) {
		return true;
	}

	uint32_t platformId = *reinterpret_cast<const uint32_t *>(prop->getBytesNoCopy());
	// 0x8A5C0002 is undefined for the currently used TGL framebuffer path on this setup.
	return platformId == 0x8A5C0002U;
}

static bool isLegacyIGPUPropSeedingEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenforceprops", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenforceprops");
}

static bool seedIGPUPropertiesOnEntry(IORegistryEntry *entry) {
	if (!entry) {
		return false;
	}

	bool changed = false;

	// Default fallback platform-id for RPL/TGL spoof bring-up. Only inject when missing.
	if (!entry->getProperty("AAPL,ig-platform-id")) {
		entry->setProperty("AAPL,ig-platform-id", builtin2, arrsize(builtin2));
		changed = true;
	}

	if (!entry->getProperty("device-id")) {
		entry->setProperty("device-id", builtin3, arrsize(builtin3));
		changed = true;
	}

	if (!entry->getProperty("built-in")) {
		static uint8_t builtin[] = {0x00};
		entry->setProperty("built-in", builtin, arrsize(builtin));
		changed = true;
	}

	if (!entry->getProperty("AAPL,slot-name")) {
		entry->setProperty("AAPL,slot-name", const_cast<char *>("built-in"), 9);
		changed = true;
	}

	if (!entry->getProperty("hda-gfx")) {
		entry->setProperty("hda-gfx", const_cast<char *>("onboard-1"), 10);
		changed = true;
	}

	if (!entry->getProperty("model")) {
		entry->setProperty("model", const_cast<char *>("Intel Iris Xe Graphics"), 23);
		changed = true;
	}

	if (!entry->getProperty("framebuffer-unifiedmem")) {
		static uint8_t unifiedMem[] = {0x00, 0x00, 0x00, 0x60}; // 1536 MB
		entry->setProperty("framebuffer-unifiedmem", unifiedMem, arrsize(unifiedMem));
		changed = true;
	}

	if (!entry->getProperty("saved-config")) {
		static uint8_t sconf[] = {};
		entry->setProperty("saved-config", sconf, 0xea);
		changed = true;
	}

	return changed;
}

static void seedIGPUPropertiesEarly() {
	// Try common ACPI namespace variants used by laptop firmware before DeviceInfo scans.
	const char *paths[] = {
		"IOService:/AppleACPIPlatformExpert/PC00@0/IGPU@2",
		"IOService:/AppleACPIPlatformExpert/PC00@0/GFX0@2",
		"IOService:/AppleACPIPlatformExpert/PCI0@0/IGPU@2",
		"IOService:/AppleACPIPlatformExpert/PCI0@0/GFX0@2"
	};

	bool found = false;
	bool changed = false;
	for (auto path : paths) {
		auto *entry = IORegistryEntry::fromPath(path, gIOServicePlane);
		if (!entry) {
			continue;
		}
		found = true;
		if (seedIGPUPropertiesOnEntry(entry)) {
			changed = true;
		}
		entry->release();
	}

	if (found) {
		SYSLOG("ngreen", "Early IGPU pre-seed via IOService path: changed=%d", changed);
	} else {
		SYSLOG("ngreen", "Early IGPU pre-seed skipped: no IGPU path resolved before DeviceInfo");
	}
}

void NGreen::init() {
    callback = this;
	
	lilu.onKextLoadForce(&kextAGDP);
	/*lilu.onKextLoadForce(&kextBacklight);
	lilu.onKextLoadForce(&kextMCCSControl);
	lilu.onKextLoadForce(&kextIOGraphics);*/
	lilu.onKextLoadForce(&kextIOAcceleratorFamily2);
	
	genx.init();
	gen11.init();
	//agfxhda.init();
	dyldpatches.init();
	
    lilu.onPatcherLoadForce(
        [](void *user, KernelPatcher &patcher) { static_cast<NGreen *>(user)->processPatcher(patcher); }, this);
    lilu.onKextLoadForce(
        nullptr, 0,
        [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            static_cast<NGreen *>(user)->processKext(patcher, index, address, size);
        },
        this);
	
}


void NGreen::processPatcher(KernelPatcher &patcher) {
	// Hook _cs_validate_page FIRST — before DeviceInfo which blocks
	// for >60s polling PEGP (NVIDIA dGPU) disable via processSwitchOff.
	// Without this, WindowServer starts before the hook is established
	// and CoreDisplay loads unpatched from the shared cache.
	if (!checkKernelArgument("-nbdyldoff")) {
		dyldpatches.processPatcher(patcher);
	} else {
		DBGLOG("ngreen", "DYLD patches disabled by boot argument -nbdyldoff");
	}

	// Compatibility-first default: do not force-inject IGPU properties unless explicitly requested.
	if (isLegacyIGPUPropSeedingEnabled()) {
		seedIGPUPropertiesEarly();
	} else {
		SYSLOG("ngreen", "compat mode: legacy IGPU property seeding disabled (use -ngreenforceprops to enable)");
	}

    auto *devInfo = DeviceInfo::create();
    if (devInfo) {
        devInfo->processSwitchOff();
		
		

        this->iGPU = OSDynamicCast(IOPCIDevice, devInfo->videoBuiltin);
        PANIC_COND(!this->iGPU, "ngreen", "videoBuiltin is not IOPCIDevice");
		
		// necessary without : igpu stall hang on boot
		this->iGPU->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
		this->iGPU->setBusMasterEnable(true);
		this->iGPU->setMemoryEnable(true);
		

		static uint8_t builtin[] = {0x00};

		WIOKit::renameDevice(this->iGPU, "IGPU");
		WIOKit::awaitPublishing(this->iGPU);

		if (isLegacyIGPUPropSeedingEnabled()) {
			seedIGPUPropertiesOnEntry(this->iGPU);
		}
		
		static uint8_t sconf[] = {};
		
		static uint8_t panel[] = {0x01, 0x00, 0x00, 0x00};
		/*static uint8_t panel1[] = {0x19, 0x01, 0x00, 0x00};
		static uint8_t panel2[] = {0x3c, 0x00, 0x00, 0x00};
		static uint8_t panel3[] = {0x11, 0x00, 0x00, 0x00};
		static uint8_t panel4[] = {0xfa, 0x00, 0x00, 0x00};

		this->iGPU->setProperty("AAPL00,PanelPowerUp", panel, arrsize(panel));
		this->iGPU->setProperty("AAPL00,PanelPowerOn", panel1, arrsize(panel1));
		this->iGPU->setProperty("AAPL00,PanelPowerDown", panel2, arrsize(panel2));
		this->iGPU->setProperty("AAPL00,PanelPowerOff", panel3, arrsize(panel3));
		this->iGPU->setProperty("AAPL00,PanelCycleDelay", panel4, arrsize(panel4));*/
		

		//this->iGPU->setProperty("@0,display-dither-support", panel, arrsize(panel));
		
		if (isLegacyIGPUPropSeedingEnabled()) {
			this->iGPU->setProperty("built-in", builtin, arrsize(builtin));
			this->iGPU->setProperty("AAPL,slot-name", const_cast<char *>("built-in"), 9);
			this->iGPU->setProperty("hda-gfx", const_cast<char *>("onboard-1"), 10);
			this->iGPU->setProperty("model", const_cast<char *>("Intel Iris Xe Graphics"), 23);

			auto *prop = OSDynamicCast(OSData, this->iGPU->getProperty("saved-config"));
			if (!prop) this->iGPU->setProperty("saved-config", sconf, 0xea);
		}
			
		//auto x = OSDynamicCast(OSData, this->iGPU->getProperty("AAPL,ig-platform-id"));
		//framebufferId = *(uint32_t*)x->getBytesNoCopy();
		
		//NETLOG("gen11", "framebufferId: = %x", framebufferId);
		//setRMMIOIfNecessary();

        this->deviceId = WIOKit::readPCIConfigValue(this->iGPU, WIOKit::kIOPCIConfigDeviceID);
        this->pciRevision = WIOKit::readPCIConfigValue(NGreen::callback->iGPU, WIOKit::kIOPCIConfigRevisionID);

        // V52: Detect real TGL vs spoofed RPL/ADL by CPU model
        // TGL-U: model 0x8C, TGL-H: model 0x8D
        // RPL-P: model 0xBA, RPL-S: model 0xBF, RPL-HX: model 0xB7
        // ADL-P: model 0x9A, ADL-S: model 0x97
        {
            uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
            asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
            uint32_t family = (eax >> 8) & 0xF;
            uint32_t model = (eax >> 4) & 0xF;
            uint32_t extModel = (eax >> 16) & 0xF;
            uint32_t stepping = eax & 0xF;
            if (family == 0x6) model |= (extModel << 4);
            this->cpuModel = model;
            this->isRealTGL = (model == 0x8C || model == 0x8D);
            SYSLOG("ngreen", "V52: CPU family=0x%x model=0x%x stepping=%u isRealTGL=%d",
                   family, model, stepping, this->isRealTGL);
        }
		
		auto gms = WIOKit::readPCIConfigValue(devInfo->videoBuiltin, WIOKit::kIOPCIConfigGraphicsControl, 0, 16) >> 8;
		
		if (gms < 0x10) {
			stolen_size = gms * 32;
		} else if (gms == 0x20 || gms == 0x30 || gms == 0x40) {
			stolen_size = gms * 32;
		} else if (gms >= 0xF0 && gms <= 0xFE) {
			stolen_size = ((gms & 0x0F) + 1) * 4;
		} else {
			SYSLOG( "ngreen", "PANIC stolen_size=0 check DVMT in bios");
		}
		if (stolen_size<128) stolen_size=128;
		stolen_size *= (1024 * 1024);
		SYSLOG("ngreen", "stolen_size 0x%x",stolen_size);
		
		if (isLegacyIGPUPropSeedingEnabled()) {
			static uint8_t unifiedMem[] = {0x00, 0x00, 0x00, 0x60};
			this->iGPU->setProperty("framebuffer-unifiedmem", unifiedMem, arrsize(unifiedMem));
		}
		
		KernelPatcher::routeVirtual(this->iGPU, WIOKit::PCIConfigOffset::ConfigRead16, configRead16, &orgConfigRead16);
		KernelPatcher::routeVirtual(this->iGPU, WIOKit::PCIConfigOffset::ConfigRead32, configRead32, &orgConfigRead32);

        DeviceInfo::deleter(devInfo);
		

		
    } else {
        SYSLOG("ngreen", "Failed to create DeviceInfo");
    }
	
	/*KernelPatcher::RouteRequest request {"__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass", wrapSafeMetaCast,
		this->orgSafeMetaCast};
	PANIC_COND(!patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1), "ngreen",
		"Failed to route kernel symbols");*/
}

OSMetaClassBase *NGreen::wrapSafeMetaCast(const OSMetaClassBase *anObject, const OSMetaClass *toMeta) {
	auto ret = FunctionCast(wrapSafeMetaCast, callback->orgSafeMetaCast)(anObject, toMeta);
	if (UNLIKELY(!ret)) {
		for (const auto &ent : callback->metaClassMap) {
			if (LIKELY(ent[0] == toMeta)) {
				return FunctionCast(wrapSafeMetaCast, callback->orgSafeMetaCast)(anObject, ent[1]);
			} else if (UNLIKELY(ent[1] == toMeta)) {
				return FunctionCast(wrapSafeMetaCast, callback->orgSafeMetaCast)(anObject, ent[0]);
			}
		}
	}
	return ret;
}

bool NGreen::wrapIGAccelDeviceStart(void *that) {
	auto ret = FunctionCast(wrapIGAccelDeviceStart, callback->orgIGAccelDeviceStart)(that);
	if (!callback->isRealTGL && !ret) {
		SYSLOG("NGreen", "IOAccelF2: forcing IGAccelDevice::deviceStart success on spoofed TGL path");
		return true;
	}
	DBGLOG("NGreen", "IOAccelF2: IGAccelDevice::deviceStart returned %d", ret);
	return ret;
}

void NGreen::setRMMIOIfNecessary() {
	if (UNLIKELY(!this->rmmio || !this->rmmio->getLength())) {
		this->rmmio = this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
		this->rmmioPtr = reinterpret_cast<volatile uint32_t *>(this->rmmio->getVirtualAddress());
	}
}

void NGreen::setApertureIfNecessary() {
	if (UNLIKELY(!this->aperture || !this->aperture->getLength())) {
		this->aperture = this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
		if (this->aperture) {
			this->aperturePtr = reinterpret_cast<volatile uint32_t *>(this->aperture->getVirtualAddress());
			this->apertureLen = this->aperture->getLength();
			SYSLOG("ngreen", "V201: BAR2 aperture mapped, len=0x%llx", this->apertureLen);
		} else {
			SYSLOG("ngreen", "V201: BAR2 aperture map FAILED");
		}
	}
}

// V221: waitForStamp hook — delays CoreDisplay's stamp-3 wait until IntelAccelerator::start()
// has returned (and thus the GFX interrupt handler is installed). Without this, stamp 3 is
// submitted during FB init while the GFX kext hasn't started yet; the interrupt fires but sits
// unserviced for 5s, causing CoreDisplay_CreateDisplayForCGXDisplayDevice to assert.
static mach_vm_address_t orgWaitForStamp = 0;
static IOReturn wrapWaitForStamp(void *that, int32_t channel, unsigned int stamp, unsigned int *outStamp) {
	IOReturn ret = FunctionCast(wrapWaitForStamp, orgWaitForStamp)(that, channel, stamp, outStamp);
	if (ret != kIOReturnSuccess && !Gen11::gGfxAccelStartDone) {
		// Stamp timed out before GFX start() completed. The GPU DID execute the ring
		// (HEAD==TAIL, IPEHR=MI_REPORT_HEAD) but the interrupt handler isn't installed
		// yet so gpu_stamp was never bumped. Returning success here lets CoreDisplay
		// proceed, which unblocks IOKit matching so GFX kext can start. GFX's interrupt
		// handler will then process the pending GT_INTR_DW0 backlog when it installs.
		// Blocking here causes a deadlock (CoreDisplay stuck → GFX never matches).
		SYSLOG("ngreen", "V221: stamp(%d,%u) timed out before GFX start — faking success to unblock CoreDisplay",
			   channel, stamp);
		if (outStamp) *outStamp = stamp;
		return kIOReturnSuccess;
	}
	return ret;
}

// V500: IOAccelLegacySurface::set_id_mode(uint32_t id, uint32_t mode) active fix.
// Hardware rejects mode bits covered by 0xff8073c0 with kIOReturnUnsupported.
// On non-TGL (RPL-P spoofed as TGL), strip those bits before the call so the
// GPU task gets a valid scheduling mode instead of silently failing.
static mach_vm_address_t orgSetIdMode = 0;
static IOReturn wrapSetIdMode(void *that, uint32_t id, uint32_t mode) {
    uint32_t patchedMode = mode;
    if (NGreen::callback && !NGreen::callback->getIsRealTGL())
        patchedMode = mode & ~0xff8073c0u;  // strip TGL-only preemption/mode bits

    IOReturn ret = FunctionCast(wrapSetIdMode, orgSetIdMode)(that, id, patchedMode);

    static int v500Count = 0;
    if (v500Count < 32) {
        ++v500Count;
        if (patchedMode != mode)
            SYSLOG("ngreen", "V500[%d]: set_id_mode stripped 0x%x→0x%x id=0x%x ret=0x%x",
                   v500Count, mode, patchedMode, id, ret);
        else if (ret != kIOReturnSuccess)
            SYSLOG("ngreen", "V500[%d]: set_id_mode FAILED ret=0x%x id=0x%x mode=0x%x",
                   v500Count, ret, id, mode);
    }
    return ret;
}

bool NGreen::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (kextIOAcceleratorFamily2.loadIndex == index) {
		SYSLOG("NGreen", "IOAccelF2: TEXT 0x%llx size 0x%lx", address, size);
		
		// ── V40: f2 FIXED based on V39 diagnostics ──
		// V39 revealed: Sonoma uses push r12; push rbx; push rax (41 54 53 50)
		// as function prologue instead of sub rsp,imm8 (48 83 ec).
		// Two matching test edx,imm32; je found at +0x38b0 and +0x102e6.
		// Patching both (count=2) to bypass capability checks.
		static const uint8_t f2_f[]  = {0x41, 0x54, 0x53, 0x50, 0xf7, 0xc2, 0x00, 0x00, 0x00, 0x00, 0x74, 0x00};
		static const uint8_t f2_m[]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00};
		static const uint8_t f2_r[]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEB, 0x00};
		static const uint8_t f2_rm[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00};
		const LookupPatchPlus p2 {&kextIOAcceleratorFamily2, f2_f, f2_m, f2_r, f2_rm, 2};
		
		// necessary : without it hang on boot
		bool f2ok = p2.apply(patcher, address, size);
		patcher.clearError();
		SYSLOG("NGreen", "IOAccelF2 f2 (fixed): %s", f2ok ? "OK" : "FAILED");
		
		// ── V41: f1 FIXED based on V40 diagnostics ──
		// V40 revealed: Sonoma has jne (0x75) at -16 from mov r9d,[r15+0x284],
		// NOT je (0x74) at -2 as the original pattern assumed.
		// Code structure: cmp eax,ebx; jne +0x39; <device-specific setup>; mov r9d,[r15+0x284]
		// This is a device-ID capability check: vtable call returns ID, compared with expected.
		// NOP the jne (75 XX → 90 90) so our device 0x9A49 always falls through.
		// Pattern: jne XX; mov rax,[r15+disp32]; mov r8,[rax+disp32]; mov r9d,[r15+disp32]
		static const uint8_t f1_f[]  = {0x75, 0x00, 0x49, 0x8b, 0x87, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x80, 0x00, 0x00, 0x00, 0x00, 0x45, 0x8b, 0x8f};
		static const uint8_t f1_m[]  = {0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
		static const uint8_t f1_r[]  = {0x90, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
		static const uint8_t f1_rm[] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
		const LookupPatchPlus p1 {&kextIOAcceleratorFamily2, f1_f, f1_m, f1_r, f1_rm, 1};
		// V206: disabled IOAccelF2 f1 patch + IGAccelDevice::deviceStart hook + V181 lock
		// resolves to match Visual Ehrmanntraut's working config. Restore if regression.
		/*bool f1ok = p1.apply(patcher, address, size);
		patcher.clearError();
		SYSLOG("NGreen", "IOAccelF2 f1 (fixed): %s", f1ok ? "OK" : "FAILED");

		RouteRequestPlus routes[] = {
			{"__ZN13IGAccelDevice11deviceStartEv", wrapIGAccelDeviceStart, this->orgIGAccelDeviceStart},
		};
		if (RouteRequestPlus::routeAll(patcher, index, routes, address, size)) {
			SYSLOG("NGreen", "IOAccelF2: hooked IGAccelDevice::deviceStart");
		} else {
			patcher.clearError();
			SYSLOG("NGreen", "IOAccelF2: IGAccelDevice::deviceStart symbol not found");
		}

		// V181: resolve lockForCPUAccess / unlockForCPUAccess for Gen11 blit3d scratch init
		if (Gen11::callback) {
			SolveRequestPlus ioaf2Solve[] = {
				{"__ZN16IOAccelSysMemory16lockForCPUAccessEP4taskj", Gen11::callback->oIOAF2_lockForCPUAccess},
				{"__ZN16IOAccelSysMemory18unlockForCPUAccessEP4task", Gen11::callback->oIOAF2_unlockForCPUAccess},
			};
			if (SolveRequestPlus::solveAll(patcher, index, ioaf2Solve, address, size)) {
				SYSLOG("ngreen", "V181: IOAF2 lock/unlock resolved lock=%p unlock=%p",
				       reinterpret_cast<void *>(Gen11::callback->oIOAF2_lockForCPUAccess),
				       reinterpret_cast<void *>(Gen11::callback->oIOAF2_unlockForCPUAccess));
			} else {
				patcher.clearError();
				SYSLOG("ngreen", "V181: IOAF2 lock/unlock resolve failed");
			}
		}*/

		// V221: Hook IOAccelEventMachine2::waitForStamp to delay CoreDisplay's stamp-3 wait
		// until IntelAccelerator::start() has returned and the GFX IRQ handler is installed.
		RouteRequestPlus wfsRequest[] = {
			{"__ZN20IOAccelEventMachine212waitForStampEijPj", wrapWaitForStamp, orgWaitForStamp},
		};
		if (RouteRequestPlus::routeAll(patcher, index, wfsRequest, address, size)) {
			SYSLOG("ngreen", "V221: hooked IOAccelEventMachine2::waitForStamp");
		} else {
			patcher.clearError();
			SYSLOG("ngreen", "V221: waitForStamp hook FAILED — symbol not found");
		}

		// V500: IOAccelLegacySurface::set_id_mode — diagnostic logger.
		RouteRequestPlus simRequest[] = {
			{"__ZN20IOAccelLegacySurface11set_id_modeEjj", wrapSetIdMode, orgSetIdMode},
		};
		if (RouteRequestPlus::routeAll(patcher, index, simRequest, address, size)) {
			SYSLOG("ngreen", "V500: hooked IOAccelLegacySurface::set_id_mode");
		} else {
			patcher.clearError();
			SYSLOG("ngreen", "V500: set_id_mode hook FAILED — symbol not found");
		}

	}  else if (kextIOGraphics.loadIndex == index) {
		/*
		KernelPatcher::RouteRequest requests[] = {
				{"__ZN13IOFramebuffer25extValidateDetailedTimingEP8OSObjectPvP25IOExternalMethodArguments", wrapValidateDetailedTiming},
			};
			patcher.routeMultiple(index, requests, address, size);
			patcher.clearError();*/
		
	}  else if (kextAGDP.loadIndex == index) {
		const LookupPatchPlus patch {&kextAGDP, kAGDPBoardIDKeyOriginal, kAGDPBoardIDKeyPatched, 1};
		SYSLOG_COND(!patch.apply(patcher, address, size), "NGreen", "Failed to apply AGDP board-id patch");

		/*if (getKernelVersion() == KernelVersion::Ventura) {
			const LookupPatchPlus patch {&kextAGDP, kAGDPFBCountCheckVenturaOriginal, kAGDPFBCountCheckVenturaPatched,
				1};
			SYSLOG_COND(!patch.apply(patcher, address, size), "NGreen", "Failed to apply AGDP fb count check patch");
		} else {
			const LookupPatchPlus patch {&kextAGDP, kAGDPFBCountCheckOriginal, kAGDPFBCountCheckPatched, 1};
			SYSLOG_COND(!patch.apply(patcher, address, size), "NGreen", "Failed to apply AGDP fb count check patch");
		}*/
	}  else if (kextBacklight.loadIndex == index) {
		// V204b: re-enable AppleIntelPanel::setDisplay route + backlight string patch.
		// Friend's working version has these enabled. Sets up panel data for backlight ramp.
		KernelPatcher::RouteRequest request {"__ZN15AppleIntelPanel10setDisplayEP9IODisplay", wrapApplePanelSetDisplay,
			orgApplePanelSetDisplay};
		if (patcher.routeMultiple(kextBacklight.loadIndex, &request, 1, address, size)) {
			const UInt8 find[] = {"F%uT%04x"};
			const UInt8 replace[] = {"F%uTxxxx"};
			const LookupPatchPlus patch {&kextBacklight, find, replace, 1};
			SYSLOG_COND(!patch.apply(patcher, address, size), "NGreen", "Failed to apply backlight patch");
		}
} else if (kextMCCSControl.loadIndex == index) {
		/*KernelPatcher::RouteRequest requests[] = {
				{"__ZN25AppleMCCSControlGibraltar5probeEP9IOServicePi", wrapFunctionReturnZero},
				{"__ZN21AppleMCCSControlCello5probeEP9IOServicePi", wrapFunctionReturnZero},
			};
			patcher.routeMultiple(index, requests, address, size);
			patcher.clearError();*/
} else if (genx.processKext(patcher, index, address, size)) {
	DBGLOG("ngreen", "Processed Generation x configuration");
} else if (gen11.processKext(patcher, index, address, size)) {
        DBGLOG("ngreen", "Processed Generation 11 configuration");
    } /*else if (agfxhda.processKext(patcher, index, address, size)) {
		DBGLOG("ngreen", "Processed AppleGFXHDA");
	}*/
    return true;
}



uint16_t NGreen::configRead16(IORegistryEntry *service, uint32_t space, uint8_t offset) {
	if (callback && callback->orgConfigRead16) {
		auto result = callback->orgConfigRead16(service, space, offset);
		// XE-GMS: DVMT pre-allocation in 4 MB steps. Many ADL/RPL laptops (e.g. Lenovo IdeaPad Slim 5 14IAH8,
		// i5-12450H) reserve 60 MB of DSM, so GGC (config 0x50) has GMS = 0xFE (the 0xF0..0xFE = 4..60 MB encoding).
		// The TGL 16.0.32 drivers only know the 32 MB steps: the FB (FBMemMgr_Init, (GGC << 17) & 0xFE000000) gets
		// 4032 MB and puts the cursor memory past the 256 MB aperture -> IODeviceMemory::withSubRange returns NULL ->
		// "Can't create cursor memory" -> AppleIntelBaseController::start() fails -> "Could not register with IOFB",
		// and the accelerator (IGMemoryManager::initSegments, (GGC & 0xFF00) << 17) gets ~8 GB of stolen memory ->
		// IGAccelTask::initAllocators fails -> page fault in IOAccelTask::release (CR2 0xc78) at boot.
		// Show the drivers the real DSM rounded down to 32 MB (60 -> 32, GMS 0x01): everything below is real stolen
		// memory, the rest stays unused. NootedGreen's own GGC read in processPatcher happens before this hook.
		// No change when GMS is already in 32 MB steps.
		if (offset == 0x50 && ((space >> 24) & 0xFu) == 0 && service != nullptr) {
			auto name = service->getName();
			uint32_t gms = (result >> 8) & 0xFFu;
			if (name && name[0] == 'I' && name[1] == 'G' && name[2] == 'P' && name[3] == 'U' && gms >= 0xF0 && gms <= 0xFE) {
				uint32_t mb = (gms - 0xEFu) * 4u, units = mb / 32u;
				static volatile uint32_t logged = 0;
				uint16_t fixed = static_cast<uint16_t>((result & 0x00FFu) | (units << 8));
				if (!logged) {
					logged = 1;
					SYSLOG("ngreen", "XE-GMS: GGC 0x%04x -> 0x%04x (DSM %u MB, %u MB shown to the drivers)", result, units ? fixed : result, mb, units * 32u);
				}
				if (units) return fixed;
			}
		}
		if (offset == WIOKit::kIOPCIConfigDeviceID && service != nullptr) {
			auto name = service->getName();
			if (name && name[0] == 'I' && name[1] == 'G' && name[2] == 'P' && name[3] == 'U') {
				uint32_t device;
				if (WIOKit::getOSDataValue(service, "device-id", device) && device != result) {
					return device;
				}
			}
		}

		return result;
	}

	return 0;
}

uint32_t NGreen::configRead32(IORegistryEntry *service, uint32_t space, uint8_t offset) {
	if (callback && callback->orgConfigRead32) {
		auto result = callback->orgConfigRead32(service, space, offset);
		// According to lvs unaligned reads may happen
		if ((offset == WIOKit::kIOPCIConfigDeviceID || offset == WIOKit::kIOPCIConfigVendorID) && service != nullptr) {
			auto name = service->getName();
			if (name && name[0] == 'I' && name[1] == 'G' && name[2] == 'P' && name[3] == 'U') {
				uint32_t device;
				if (WIOKit::getOSDataValue(service, "device-id", device) && device != (result & 0xFFFF)) {
					device = (result & 0xFFFF) | (device << 16);
					return device;
				}
			}
		}

		return result;
	}

	return 0;
}

size_t NGreen::wrapFunctionReturnZero() { return 0; }

struct ApplePanelData {
	const char *deviceName;
	UInt8 deviceData[36];
};

static ApplePanelData appleBacklightData[] = {
	{"F14Txxxx", {0x00, 0x11, 0x00, 0x00, 0x00, 0x34, 0x00, 0x52, 0x00, 0x73, 0x00, 0x94, 0x00, 0xBE, 0x00, 0xFA, 0x01,
					 0x36, 0x01, 0x72, 0x01, 0xC5, 0x02, 0x2F, 0x02, 0xB9, 0x03, 0x60, 0x04, 0x1A, 0x05, 0x0A, 0x06,
					 0x0E, 0x07, 0x10}},
	{"F15Txxxx", {0x00, 0x11, 0x00, 0x00, 0x00, 0x36, 0x00, 0x54, 0x00, 0x7D, 0x00, 0xB2, 0x00, 0xF5, 0x01, 0x49, 0x01,
					 0xB1, 0x02, 0x2B, 0x02, 0xB8, 0x03, 0x59, 0x04, 0x13, 0x04, 0xEC, 0x05, 0xF3, 0x07, 0x34, 0x08,
					 0xAF, 0x0A, 0xD9}},
	{"F16Txxxx", {0x00, 0x11, 0x00, 0x00, 0x00, 0x18, 0x00, 0x27, 0x00, 0x3A, 0x00, 0x52, 0x00, 0x71, 0x00, 0x96, 0x00,
					 0xC4, 0x00, 0xFC, 0x01, 0x40, 0x01, 0x93, 0x01, 0xF6, 0x02, 0x6E, 0x02, 0xFE, 0x03, 0xAA, 0x04,
					 0x78, 0x05, 0x6C}},
	{"F17Txxxx", {0x00, 0x11, 0x00, 0x00, 0x00, 0x1F, 0x00, 0x34, 0x00, 0x4F, 0x00, 0x71, 0x00, 0x9B, 0x00, 0xCF, 0x01,
					 0x0E, 0x01, 0x5D, 0x01, 0xBB, 0x02, 0x2F, 0x02, 0xB9, 0x03, 0x60, 0x04, 0x29, 0x05, 0x1E, 0x06,
					 0x44, 0x07, 0xA1}},
	{"F18Txxxx", {0x00, 0x11, 0x00, 0x00, 0x00, 0x53, 0x00, 0x8C, 0x00, 0xD5, 0x01, 0x31, 0x01, 0xA2, 0x02, 0x2E, 0x02,
					 0xD8, 0x03, 0xAE, 0x04, 0xAC, 0x05, 0xE5, 0x07, 0x59, 0x09, 0x1C, 0x0B, 0x3B, 0x0D, 0xD0, 0x10,
					 0xEA, 0x14, 0x99}},
	{"F19Txxxx", {0x00, 0x11, 0x00, 0x00, 0x02, 0x8F, 0x03, 0x53, 0x04, 0x5A, 0x05, 0xA1, 0x07, 0xAE, 0x0A, 0x3D, 0x0E,
					 0x14, 0x13, 0x74, 0x1A, 0x5E, 0x24, 0x18, 0x31, 0xA9, 0x44, 0x59, 0x5E, 0x76, 0x83, 0x11, 0xB6,
					 0xC7, 0xFF, 0x7B}},
	{"F24Txxxx", {0x00, 0x11, 0x00, 0x01, 0x00, 0x34, 0x00, 0x52, 0x00, 0x73, 0x00, 0x94, 0x00, 0xBE, 0x00, 0xFA, 0x01,
					 0x36, 0x01, 0x72, 0x01, 0xC5, 0x02, 0x2F, 0x02, 0xB9, 0x03, 0x60, 0x04, 0x1A, 0x05, 0x0A, 0x06,
					 0x0E, 0x07, 0x10}},
};

bool NGreen::wrapApplePanelSetDisplay(IOService *that, IODisplay *display) {
	static bool once = false;
	if (!once) {
		once = true;
		auto *panels = OSDynamicCast(OSDictionary, that->getProperty("ApplePanels"));
		if (panels) {
			auto *rawPanels = panels->copyCollection();
			panels = OSDynamicCast(OSDictionary, rawPanels);

			if (panels) {
				for (auto &entry : appleBacklightData) {
					auto pd = OSData::withBytes(entry.deviceData, sizeof(entry.deviceData));
					if (pd) {
						panels->setObject(entry.deviceName, pd);
						//! No release required by current AppleBacklight implementation.
					} else {
					}
				}
				that->setProperty("ApplePanels", panels);
			}

			OSSafeReleaseNULL(rawPanels);
		} else {
		}
	}

	bool ret = FunctionCast(wrapApplePanelSetDisplay, callback->orgApplePanelSetDisplay)(that, display);
	return ret;
}

