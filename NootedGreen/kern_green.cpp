//  Copyright © 2023 ChefKiss Inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
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
    auto *devInfo = DeviceInfo::create();
    if (devInfo) {
        devInfo->processSwitchOff();
		
		

        this->iGPU = OSDynamicCast(IOPCIDevice, devInfo->videoBuiltin);
        PANIC_COND(!this->iGPU, "ngreen", "videoBuiltin is not IOPCIDevice");
		
		this->iGPU->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
		this->iGPU->setBusMasterEnable(true);
		this->iGPU->setMemoryEnable(true);
		

        static uint8_t builtin[] = {0x00};
		//static uint8_t builtin2[] = {0x02, 0x00, 0x5c, 0x8A};
		//static uint8_t builtin3[] = {0x5c, 0x8A,0x00,0x00};
		
		static uint8_t builtin2[] = {0x00, 0x00, 0x49, 0x9A};
		static uint8_t builtin3[] = {0x49, 0x9A,0x00,0x00};

		WIOKit::renameDevice(this->iGPU, "IGPU");
		WIOKit::awaitPublishing(this->iGPU);
		
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
		
		this->iGPU->setProperty("built-in", builtin, arrsize(builtin));
		this->iGPU->setProperty("AAPL,slot-name", const_cast<char *>("built-in"), 9);
		this->iGPU->setProperty("hda-gfx", const_cast<char *>("onboard-1"), 10);
		this->iGPU->setProperty("model", const_cast<char *>("Intel Iris Xe Graphics"), 23);
		
		
		auto *prop = OSDynamicCast(OSData, this->iGPU->getProperty("saved-config"));
		if (!prop) this->iGPU->setProperty("saved-config", sconf, 0xea);
			
		this->iGPU->setProperty("AAPL,ig-platform-id", builtin2, arrsize(builtin2));
		this->iGPU->setProperty("device-id", builtin3, arrsize(builtin3));


		//auto x = OSDynamicCast(OSData, this->iGPU->getProperty("AAPL,ig-platform-id"));
		//framebufferId = *(uint32_t*)x->getBytesNoCopy();
		
		//NETLOG("gen11", "framebufferId: = %x", framebufferId);
		//setRMMIOIfNecessary();

        this->deviceId = WIOKit::readPCIConfigValue(this->iGPU, WIOKit::kIOPCIConfigDeviceID);
        this->pciRevision = WIOKit::readPCIConfigValue(NGreen::callback->iGPU, WIOKit::kIOPCIConfigRevisionID);
		
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
		
		// Set framebuffer-unifiedmem = 1536 MB (0x60000000) for proper VRAM reporting
		static uint8_t unifiedMem[] = {0x00, 0x00, 0x00, 0x60}; // 0x60000000 = 1536 MB
		this->iGPU->setProperty("framebuffer-unifiedmem", unifiedMem, arrsize(unifiedMem));
		
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
	
	if (!checkKernelArgument("-nbdyldoff")) {
		dyldpatches.processPatcher(patcher);
	} else {
		DBGLOG("ngreen", "DYLD patches disabled by boot argument -nbdyldoff");
	}
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

void NGreen::setRMMIOIfNecessary() {
	if (UNLIKELY(!this->rmmio || !this->rmmio->getLength())) {
		this->rmmio = this->iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
		this->rmmioPtr = reinterpret_cast<volatile uint32_t *>(this->rmmio->getVirtualAddress());
	}
}

bool NGreen::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (kextIOAcceleratorFamily2.loadIndex == index) {
		SYSLOG("NGreen", "IOAccelF2: TEXT 0x%llx size 0x%lx", address, size);
		
		// V35: Use masked patterns for version-independent matching.
		// The original f1/f2 had hard-coded RIP-relative offsets and test immediates
		// that change between macOS builds.  Masked find/replace wildcards those bytes.
		
		// ── f1: je → jmp before mov r9d,[r15+OFFSET]; REX-prefixed instr ──
		// Original (pre-Sonoma): je +0x57; mov r9d,[r15+0x284]; lea rdi,[rip+...]
		// Anchor: 45 8b 8f = mov r9d,[r15+disp32]  (REX.RB, MOV, ModRM)
		// Only byte changed: je (0x74) → jmp (0xEB)
		static const uint8_t f1_f[]  = {0x74, 0x00, 0x45, 0x8b, 0x8f, 0x00, 0x00, 0x00, 0x00, 0x48};
		static const uint8_t f1_m[]  = {0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF};
		static const uint8_t f1_r[]  = {0xEB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
		static const uint8_t f1_rm[] = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
		const LookupPatchPlus p1 {&kextIOAcceleratorFamily2, f1_f, f1_m, f1_r, f1_rm, 1};
		bool f1ok = p1.apply(patcher, address, size);
		patcher.clearError();
		SYSLOG("NGreen", "IOAccelF2 f1 (masked): %s", f1ok ? "OK" : "FAILED");
		
		// ── f2: je → jmp after sub rsp,imm8; test edx,imm32 ──
		// Original: sub rsp,0x18; test edx,0xFF8073C0; je +0x33
		// Anchor: 48 83 ec = sub rsp,imm8  followed by  f7 c2 = test edx,imm32
		// Only byte changed: je (0x74) → jmp (0xEB)
		static const uint8_t f2_f[]  = {0x48, 0x83, 0xec, 0x00, 0xf7, 0xc2, 0x00, 0x00, 0x00, 0x00, 0x74, 0x00};
		static const uint8_t f2_m[]  = {0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00};
		static const uint8_t f2_r[]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEB, 0x00};
		static const uint8_t f2_rm[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00};
		const LookupPatchPlus p2 {&kextIOAcceleratorFamily2, f2_f, f2_m, f2_r, f2_rm, 1};
		bool f2ok = p2.apply(patcher, address, size);
		patcher.clearError();
		SYSLOG("NGreen", "IOAccelF2 f2 (masked): %s", f2ok ? "OK" : "FAILED");
		
		// ── Fallback: scan binary for anchors and log context ──
		if (!f1ok || !f2ok) {
			const uint8_t *bin = reinterpret_cast<const uint8_t *>(address);
			
			if (!f1ok) {
				int n = 0;
				// Look for mov r9d,[r15+disp32] preceded by je or jne
				for (size_t i = 2; i + 16 < size && n < 5; i++) {
					if ((bin[i] == 0x45 || bin[i] == 0x44) &&
					    bin[i+1] == 0x8b && bin[i+2] == 0x8f &&
					    (bin[i-2] == 0x74 || bin[i-2] == 0x75)) {
						SYSLOG("NGreen", "f1 SCAN +0x%lx: %02x%02x %02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x",
							i-2, bin[i-2], bin[i-1],
							bin[i], bin[i+1], bin[i+2], bin[i+3], bin[i+4], bin[i+5], bin[i+6],
							bin[i+7], bin[i+8], bin[i+9], bin[i+10]);
						n++;
					}
				}
				if (!n) SYSLOG("NGreen", "f1 SCAN: no je/jne before mov r?d,[r15+disp32] found");
			}
			
			if (!f2ok) {
				int n = 0;
				// Look for sub rsp,XX; test edx,imm32; je/jne
				for (size_t i = 0; i + 12 < size && n < 5; i++) {
					if (bin[i] == 0x48 && bin[i+1] == 0x83 && bin[i+2] == 0xec &&
					    bin[i+4] == 0xf7 && bin[i+5] == 0xc2 &&
					    (bin[i+10] == 0x74 || bin[i+10] == 0x75)) {
						SYSLOG("NGreen", "f2 SCAN +0x%lx: %02x%02x%02x%02x %02x%02x%02x%02x%02x%02x %02x%02x",
							i, bin[i], bin[i+1], bin[i+2], bin[i+3],
							bin[i+4], bin[i+5], bin[i+6], bin[i+7], bin[i+8], bin[i+9],
							bin[i+10], bin[i+11]);
						n++;
					}
				}
				if (!n) SYSLOG("NGreen", "f2 SCAN: no sub rsp; test edx; je/jne found");
			}
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
		/*KernelPatcher::RouteRequest request {"__ZN15AppleIntelPanel10setDisplayEP9IODisplay", wrapApplePanelSetDisplay,
	  orgApplePanelSetDisplay};
			if (patcher.routeMultiple(kextBacklight.loadIndex, &request, 1, address, size)) {
				const UInt8 find[] = {"F%uT%04x"};
				const UInt8 replace[] = {"F%uTxxxx"};
				const LookupPatchPlus patch {&kextBacklight, find, replace, 1};
				SYSLOG_COND(!patch.apply(patcher, address, size), "NGreen", "Failed to apply backlight patch");
			}*/
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

